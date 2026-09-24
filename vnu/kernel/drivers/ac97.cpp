/*
 * ac97.cpp — AC'97 audio controller driver (Intel 82801AA / ICH, the
 * device QEMU provides with `-soundhw ac97`: PCI 8086:2415) and the
 * backend behind the vnu::audio syscalls (audio_open/set_fmt/write/
 * drain/pending/pause/reset/close, VNU_SYS_audio_open..56).
 *
 * The controller exposes two 16-bit-mixed I/O spaces: the NAM (native
 * audio mixer — the codec's 16-bit registers: volumes, sample rate) at
 * BAR0 and the NABM (native audio bus master — the PCM-out DMA engine)
 * at BAR1. QEMU maps both BARs as I/O ports, so every access goes
 * through in/out instructions (no MMIO).
 *
 * Playback is bus-mastering DMA driven by a buffer-descriptor list
 * (BDL) in physical memory. The ring is NDESC equal chunks played
 * round-robin: each descriptor points at its own chunk with a length
 * of CHUNK/2 (the descriptor length field counts 16-bit words, and the
 * PICB position register does too), the driver fills chunk g_next,
 * commits it by writing PO_LVI and advances; the DMA plays chunks
 * blindly up to LVI and halts at (SR.DCH) when it catches LVI. Writing
 * LVI again re-arms the engine exactly at the chunk behind the one it
 * just drained, so a ring that is always filled from behind never
 * needs a full restart. (This is the same contract Linux's intel8x0
 * driver relies on.)
 *
 * The hardware only plays 16-bit stereo at the codec's sample rate, so
 * audio_set_fmt() programs the codec rate register and write_data()
 * converts 8-bit/mono PCM to 16-bit stereo in software while feeding
 * the ring.
 *
 * There is no IRQ plumbing in the kernel, so the driver polls: every
 * call re-reads PO_SR/PO_CIV/PICB to track how much the DMA consumed.
 * write_data() is deliberately NON-blocking — it copies what fits and
 * returns the byte count — so a windowed task (the play app) never
 * stalls the cooperative desktop scheduler. The ring holds ~1.5 s of
 * audio at 48 kHz, which comfortably bridges the GUI's 1 s heartbeat.
 * drain() is the only blocking call (it spins until SR.DCH) and is
 * meant for console-style producers; the GUI player uses pending().
 */
#include <vnu/audio.h>
#include <vnu/pci.h>
#include <vnu/pmm.h>
#include <vnu/abi.h>
#include <stdint.h>

extern "C" void vnu_debug_putc(char c);

namespace {

/* --- PCI identity (QEMU's Intel 82801AA AC'97 link) --- */

constexpr uint16_t PCI_VENDOR_AC97 = 0x8086;
constexpr uint16_t PCI_DEVICE_AC97 = 0x2415;

/* --- NABM (bus-master) registers, byte offsets in BAR1's I/O space --- */

constexpr uint16_t NB_PO_BDBAR = 0x10; /* dword, rw — BDL physical address */
constexpr uint16_t NB_PO_CIV   = 0x14; /* byte,  ro — current descriptor */
constexpr uint16_t NB_PO_LVI   = 0x15; /* byte,  rw — last valid descriptor */
constexpr uint16_t NB_PO_SR    = 0x16; /* byte,  rw(1c) — status */
constexpr uint16_t NB_PO_PICB  = 0x18; /* word,  ro — bytes left in buffer */
constexpr uint16_t NB_PO_PIV   = 0x1a; /* byte,  ro — prefetched descriptor */
constexpr uint16_t NB_PO_CR    = 0x1b; /* byte,  rw — command */

constexpr uint8_t SR_DCH  = 1u << 0; /* DMA halted */
constexpr uint8_t SR_CELV = 1u << 1; /* CIV == LVI */
constexpr uint8_t SR_LVBCI = 1u << 2; /* last valid buffer completed */
constexpr uint8_t SR_BCIS = 1u << 3; /* buffer completed */
constexpr uint8_t SR_FIFOE = 1u << 4; /* FIFO error */

constexpr uint8_t CR_RPBM = 1u << 0; /* run/pause (STARTBM) */
constexpr uint8_t CR_RR   = 1u << 1; /* reset bus-master registers */
constexpr uint8_t CR_LVBIE = 1u << 2;
constexpr uint8_t CR_FEIE = 1u << 3;
constexpr uint8_t CR_IOCE = 1u << 4;

/* --- NAM (mixer) 16-bit registers, byte offsets in BAR0's I/O space --- */

constexpr uint16_t NM_MASTER_VOL = 0x02; /* bit 15 = mute, 6-bit volume */
constexpr uint16_t NM_PCM_VOL    = 0x18; /* PCM out volume, bit 15 = mute */
constexpr uint16_t NM_PCM_RATE   = 0x32; /* PCM front DAC sample rate */

/* --- DMA ring geometry ---
 * 8 equal chunks, one BDL entry each, length field in words.
 * 288000 bytes = ~1.5 s of 16-bit stereo at 48 kHz. */

constexpr unsigned NDESC = 8;
constexpr uint32_t CHUNK = 36000;                /* bytes per chunk */
constexpr uint32_t RING_BYTES = NDESC * CHUNK;   /* 288000 */
constexpr uint32_t RING_FRAMES = (RING_BYTES + 4095u) / 4096u; /* 71 */

struct BdlEntry {
    uint32_t addr;   /* physical buffer address */
    uint16_t len;    /* buffer length in 16-bit words */
    uint16_t flags;  /* IOC/BUP — unused */
} __attribute__((packed));

/* 64-step sine table (unit circle), computed at compile time with a
 * Taylor expansion — no libm in the freestanding kernel. */
struct SineTable {
    int16_t v[64];
    constexpr SineTable() : v{}
    {
        for (int i = 0; i < 64; ++i) {
            const double x = 6.283185307179586 * i / 64.0;
            double s = x - x * x * x / 6.0 + x * x * x * x * x / 120.0 -
                       x * x * x * x * x * x * x / 5040.0 +
                       x * x * x * x * x * x * x * x * x / 362880.0;
            /* the truncated Taylor series can overshoot 1.0 by a hair
             * near the peaks — clamp so the int16 cast never overflows */
            double y = s * 32767.0;
            if (y > 32767.0)
                y = 32767.0;
            if (y < -32768.0)
                y = -32768.0;
            v[i] = (int16_t)y;
        }
    }
};
constexpr SineTable k_sine;

uint16_t g_nam_port = 0;
uint16_t g_nabm_port = 0;

/* --- port I/O --- */

inline uint8_t inb(uint16_t p)
{
    uint8_t v;
    asm volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
inline void outb(uint16_t p, uint8_t v)
{
    asm volatile("outb %0,%1" : : "a"(v), "Nd"(p));
}
inline uint16_t inw(uint16_t p)
{
    uint16_t v;
    asm volatile("inw %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
inline void outw(uint16_t p, uint16_t v)
{
    asm volatile("outw %0,%1" : : "a"(v), "Nd"(p));
}
inline uint32_t inl(uint16_t p)
{
    uint32_t v;
    asm volatile("inl %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
inline void outl(uint16_t p, uint32_t v)
{
    asm volatile("outl %0,%1" : : "a"(v), "Nd"(p));
}

inline uint8_t nab_b(uint16_t off) { return inb(g_nabm_port + off); }
inline void nab_wb(uint16_t off, uint8_t v) { outb(g_nabm_port + off, v); }
inline uint16_t nab_w(uint16_t off) { return inw(g_nabm_port + off); }
inline uint32_t nab_l(uint16_t off) { return inl(g_nabm_port + off); }
inline void nab_wl(uint16_t off, uint32_t v) { outl(g_nabm_port + off, v); }
inline void nam_ww(uint16_t off, uint16_t v) { outw(g_nam_port + off, v); }

/* --- session state --- */

bool g_up = false;     /* hardware probed and armed */
bool g_open = false;   /* audio_open()/audio_close() session */
bool g_fmt_set = false;
uint32_t g_rate = 0;
uint32_t g_ch = 1;
uint32_t g_bits = 16;
bool g_started = false; /* engine armed (CR.STARTBM written) */
bool g_paused = false;  /* software pause active */
uint8_t g_sr = SR_DCH;  /* cached status register */
uint8_t g_civ = 0;      /* cached current-descriptor register */
int g_lvi = -1;         /* last committed descriptor (-1 = none yet) */
unsigned g_next = 0;    /* descriptor currently being filled */
uint32_t g_fill = 0;    /* bytes written into g_next so far */

/* DMA objects; the pmm pool is identity-mapped, so physical == virtual
 * and the hardware and the code address the same bytes. */
BdlEntry* g_bdl = nullptr;
uint32_t g_bdl_phys = 0;
uint8_t* g_ring = nullptr;
uint32_t g_ring_phys = 0;

void puts_debug(const char* s)
{
    while (*s)
        vnu_debug_putc(*s++);
}

void poll_hw()
{
    g_sr = nab_b(NB_PO_SR);
    g_civ = nab_b(NB_PO_CIV);
}

/* Number of descriptors the DMA is still holding on to: the window
 * [CIV .. LVI]. Always <= NDESC; callers treat NDESC as "ring full". */
int in_flight()
{
    if (g_lvi < 0)
        return 0;
    int d = (g_lvi - (int)g_civ) % (int)NDESC;
    if (d < 0)
        d += (int)NDESC;
    return d + 1;
}

/* Marks descriptor `idx` as playable and advances LVI. Writing LVI has
 * two effects in the ICH/QEMU model: while the DMA runs it just extends
 * the playable window; when the DMA halted at the previous LVI, it
 * re-arms the engine at CIV = PIV, which is exactly the chunk that
 * follows the one that just drained. The first commit starts playback. */
void commit_buffer(unsigned idx)
{
    g_lvi = (int)idx;
    nab_wb(NB_PO_LVI, (uint8_t)idx);
    if (!g_started) {
        nab_wl(NB_PO_BDBAR, g_bdl_phys);
        nab_wb(NB_PO_CR, CR_RPBM);
        g_started = true;
        g_paused = false;
    }
}

} // namespace

namespace vnu::audio {

int open_device()
{
    if (!g_up)
        return -VNU_ENODEV;
    if (g_open)
        return -VNU_EBUSY;
    g_open = true;
    g_fmt_set = false;
    g_started = false;
    g_paused = false;
    g_sr = SR_DCH;
    g_civ = 0;
    g_lvi = -1;
    g_next = 0;
    g_fill = 0;
    return 0;
}

int set_format(uint32_t rate, uint32_t channels, uint32_t bits)
{
    if (!g_up || !g_open)
        return -VNU_EIO;
    if (channels < 1 || channels > 2 || (bits != 8 && bits != 16))
        return -VNU_EINVAL;
    if (rate < 8000 || rate > 48000)
        return -VNU_EINVAL;
    nam_ww(NM_PCM_RATE, (uint16_t)rate);
    g_rate = rate;
    g_ch = channels;
    g_bits = bits;
    g_fmt_set = true;
    return 0;
}

long write_data(const void* buf, uint32_t len)
{
    if (!g_up || !g_open)
        return -VNU_EIO;
    if (!g_fmt_set || g_paused)
        return 0;
    if (!buf || len == 0)
        return 0;

    const uint8_t* src = static_cast<const uint8_t*>(buf);
    const uint32_t in_sz = g_ch * (g_bits / 8); /* input bytes per sample */
    const bool is8 = (g_bits == 8);
    const bool mono = (g_ch == 1);
    uint32_t done = 0; /* input bytes consumed */

    while (done < len) {
        const uint32_t avail = (len - done) / in_sz; /* whole samples left */
        if (avail == 0)
            break;
        poll_hw();
        const int ifl = in_flight();
        /* Signed room: (NDESC-1-ifl) goes to -1 when the window covers
         * every descriptor, which cancels the (CHUNK-g_fill) term so a
         * full ring reports zero room instead of a negative one. */
        int32_t room = (int32_t)((int)NDESC - 1 - ifl) * (int32_t)CHUNK +
                       (int32_t)(CHUNK - g_fill);
        if (room < 0)
            room = 0;
        uint32_t can = (uint32_t)room / 4u; /* 16-bit stereo frames that fit */
        if (can > avail)
            can = avail;
        if (can == 0)
            break;

        uint8_t* dst = g_ring + (uint32_t)g_next * CHUNK + g_fill;
        const uint8_t* p = src + done;
        for (uint32_t i = 0; i < can; ++i) {
            int16_t l, r;
            if (is8) {
                int16_t v = (int16_t)(((int16_t)p[i * in_sz] - 128) << 8);
                l = v;
                r = mono ? v
                         : (int16_t)(((int16_t)p[i * in_sz + 1] - 128) << 8);
            } else {
                l = (int16_t)(p[i * in_sz] | ((uint16_t)p[i * in_sz + 1] << 8));
                r = mono ? l
                         : (int16_t)(p[i * in_sz + 2] |
                                     ((uint16_t)p[i * in_sz + 3] << 8));
            }
            dst[4 * i] = (uint8_t)l;
            dst[4 * i + 1] = (uint8_t)((uint16_t)l >> 8);
            dst[4 * i + 2] = (uint8_t)r;
            dst[4 * i + 3] = (uint8_t)((uint16_t)r >> 8);
        }
        g_fill += can * 4;
        done += can * in_sz;

        if (g_fill == CHUNK) {
            g_fill = 0;
            const unsigned idx = g_next;
            g_next = (g_next + 1) % NDESC;
            commit_buffer(idx);
        }
    }
    return (long)done;
}

int drain()
{
    if (!g_up || !g_open)
        return -VNU_EIO;

    /* Commit a partially-filled tail, zero-padding so stale ring bytes
     * can't garble the ending. */
    if (g_fill > 0) {
        uint8_t* b = g_ring + (uint32_t)g_next * CHUNK;
        for (uint32_t i = g_fill; i < CHUNK; ++i)
            b[i] = 0;
        g_fill = 0;
        const unsigned idx = g_next;
        g_next = (g_next + 1) % NDESC;
        commit_buffer(idx);
    }

    if (g_lvi < 0)
        return 0; /* nothing was ever fed */

    /* Wait for the DMA to consume everything (SR.DCH). Bounded spin —
     * no PIT timer (the e1000 driver owns the PIT IRQ). */
    uint32_t spins = 0;
    for (;;) {
        poll_hw();
        if (g_sr & SR_DCH)
            return 0;
        if (++spins > 100000000u)
            return -VNU_EIO;
    }
}

long pending()
{
    if (!g_up || !g_open)
        return -VNU_EIO;
    if (g_lvi < 0)
        return (long)g_fill; /* nothing committed yet */

    poll_hw();
    int ifl;
    if (g_paused) {
        /* The DMA was halted by the software pause (not by natural
         * exhaustion): the in-flight window is still queued and must
         * be counted. */
        ifl = in_flight();
    } else if (g_sr & SR_DCH) {
        ifl = 0; /* engine caught up: everything consumed */
    } else {
        ifl = in_flight();
    }
    if (ifl == 0)
        return (long)g_fill;

    const uint32_t remaining =
        (uint32_t)ifl * CHUNK -
        (CHUNK - (uint32_t)nab_w(NB_PO_PICB) * 2u) + g_fill;
    return (long)remaining;
}

int pause_device()
{
    if (!g_up || !g_open)
        return -VNU_EIO;
    if (g_started && !g_paused) {
        nab_wb(NB_PO_CR, CR_IOCE); /* clear STARTBM → engine halts */
        poll_hw();
        g_paused = true;
    }
    return 0;
}

int reset_device()
{
    if (!g_up || !g_open)
        return -VNU_EIO;
    if (g_started) {
        nab_wb(NB_PO_CR, 0);
        uint32_t spins = 0;
        while (!(nab_b(NB_PO_SR) & SR_DCH)) {
            if (++spins > 100000000u)
                break;
        }
        nab_wb(NB_PO_CR, CR_RR); /* reset bus-master registers */
    }
    g_civ = 0;
    g_lvi = -1;
    g_next = 0;
    g_fill = 0;
    g_started = false;
    g_paused = false;
    return 0;
}

int close_device()
{
    if (!g_up)
        return -VNU_EIO;
    if (!g_open)
        return -VNU_EINVAL;
    reset_device();
    g_open = false;
    g_fmt_set = false;
    return 0;
}

int init()
{
    vnu::pci::Address a{};
    if (!vnu::pci::find(PCI_VENDOR_AC97, PCI_DEVICE_AC97, a)) {
        puts_debug("ac97: no controller\n");
        return 0;
    }

    /* Enable I/O space + bus mastering (bits 0 and 2 of the command
     * register). */
    const uint32_t cmd = vnu::pci::read_dword(a, 0x04);
    vnu::pci::write_dword(a, 0x04, cmd | 0x5u);

    g_nam_port = (uint16_t)(vnu::pci::read_dword(a, 0x10) & 0xfffcu);
    g_nabm_port = (uint16_t)(vnu::pci::read_dword(a, 0x14) & 0xfffcu);
    if (!g_nam_port || !g_nabm_port) {
        puts_debug("ac97: bad BARs\n");
        return -VNU_ENODEV;
    }

    g_bdl_phys = vnu::pmm::alloc_contig(1);
    g_ring_phys = vnu::pmm::alloc_contig(RING_FRAMES);
    if (!g_bdl_phys || !g_ring_phys) {
        puts_debug("ac97: no DMA memory\n");
        return -VNU_ENOMEM;
    }
    g_bdl = reinterpret_cast<BdlEntry*>(g_bdl_phys);
    g_ring = reinterpret_cast<uint8_t*>(g_ring_phys);

    /* QEMU's STAC9700 powers up muted (master/PCM volume registers carry
     * their mute bit), so unmute at 0 dB attenuation before anything. */
    nam_ww(NM_MASTER_VOL, 0x0000);
    nam_ww(NM_PCM_VOL, 0x0000);

    /* One descriptor per chunk; the length field counts 16-bit words
     * (CHUNK bytes → CHUNK/2), exactly like Linux's intel8x0 setup. */
    for (unsigned i = 0; i < NDESC; ++i) {
        g_bdl[i].addr = g_ring_phys + i * CHUNK;
        g_bdl[i].len = (uint16_t)(CHUNK / 2u);
        g_bdl[i].flags = 0;
    }

    g_sr = SR_DCH;
    g_up = true;
    puts_debug("ac97: 8086:2415 ring=288 KiB\n");

    /* Boot self-test: play 0.25 s of 440 Hz and wait for the DMA to
     * consume it — proves the codec rate, the BDL and the ring plumbing
     * end to end. */
    const int rc = [&] {
        if (open_device() != 0)
            return -1;
        if (set_format(22050, 1, 16) != 0)
            return -1;
        const unsigned n = 5512; /* samples for 0.25 s @ 22050 Hz */
        const unsigned fade = 110; /* 5 ms clicks-free fades */
        static int16_t tone[5512];
        for (unsigned i = 0; i < n; ++i) {
            const uint32_t ph = (i * 64u) / 50u; /* 440 Hz cycle in 64 steps */
            int32_t v = k_sine.v[ph % 64u];
            int32_t env = 1024;
            if (i < fade)
                env = (int32_t)i * 1024 / (int32_t)fade;
            else if (n - i < fade)
                env = (int32_t)(n - i) * 1024 / (int32_t)fade;
            tone[i] = (int16_t)((v * env) >> 10);
        }
        const long fed = write_data(tone, n * 2u);
        if (fed != (long)n * 2u)
            return -2;
        if (drain() != 0)
            return -3;
        return close_device();
    }();
    if (rc == 0)
        puts_debug("ac97: selftest OK\n");
    else
        puts_debug("ac97: selftest FAIL\n");
    return 0;
}

} // namespace vnu::audio