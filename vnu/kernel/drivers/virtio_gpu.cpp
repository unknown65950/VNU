// virtio_gpu.cpp — virtio-gpu display driver (virtio 1.0 over PCI).
//
// QEMU's `-device virtio-gpu-pci` (or the VGA-compatible `virtio-vga`,
// which can drive the sole display) has no scanout framebuffer the way
// VBE does: the guest creates a "resource" backed by its own physical
// memory and the host copies it into the display on demand. The desktop
// keeps rendering into the 8-bpp VGA backbuffer as usual; vga_gfx::
// present() expands it to true-colour B8G8R8X8 into this driver's
// scanout buffer, then present() runs a full-frame TRANSFER_TO_HOST_2D
// + RESOURCE_FLUSH so the host shows it.
//
// Transport: modern (VIRTIO_F_VERSION_1) only. Every register — common
// config, notification and ISR status — sits in a memory BAR behind the
// virtio PCI capabilities, which paging::map_device_region() hands over
// as a plain pointer. Command completion is polled on the control
// queue's used ring, so no IRQ/MSI-X wiring is needed.

#include <vnu/virtio_gpu.h>
#include <vnu/pci.h>
#include <vnu/paging.h>
#include <vnu/pmm.h>
#include <vnu/vga_gfx.h>
#include <vnu/tty.h>
#include <stdint.h>

extern "C" void* memset(void* dst, int val, unsigned long count);
extern "C" void vnu_debug_putc(char c); /* serial (COM1) — mirrors the boot log */

namespace {

constexpr uint16_t PCI_VENDOR_VIRTIO = 0x1AF4;
/* Virtio-gpu family device IDs. QEMU's virtio-gpu-pci is transitional
 * (0x1050) and modern-only (0x1054); virtio-vga, a VGA-compatible
 * virtio-gpu usable as the sole display device, is 0x1040 / 0x1041. */
constexpr uint16_t GPU_DEVICE_IDS[] = {0x1040, 0x1041, 0x1050, 0x1054};

/* Device status flags (virtio 1.0). */
constexpr uint32_t STATUS_ACKNOWLEDGE = 1u << 0;
constexpr uint32_t STATUS_DRIVER = 1u << 1;
constexpr uint32_t STATUS_DRIVER_OK = 1u << 2;
constexpr uint32_t STATUS_FEATURES_OK = 1u << 3;

/* Common config register offsets (virtio 1.0 / virtio-pci, as laid out by
 * QEMU's standard-headers/linux/virtio_pci.h: the msix/num_queues/status
 * block packs into 16-bit/8-bit fields, so the queue registers sit two
 * bytes earlier than the drafts' padded layout). */
constexpr uint32_t COMMON_DEVICE_FEATURE_SELECT = 0x00;
constexpr uint32_t COMMON_DEVICE_FEATURE = 0x04;
constexpr uint32_t COMMON_DRIVER_FEATURE_SELECT = 0x08;
constexpr uint32_t COMMON_DRIVER_FEATURE = 0x0C;
constexpr uint32_t COMMON_MSIX_CONFIG = 0x10;
constexpr uint32_t COMMON_NUM_QUEUES = 0x12;
constexpr uint32_t COMMON_DEVICE_STATUS = 0x14;
constexpr uint32_t COMMON_QUEUE_SELECT = 0x16;
constexpr uint32_t COMMON_QUEUE_SIZE = 0x18;
constexpr uint32_t COMMON_QUEUE_MSIX = 0x1A;
constexpr uint32_t COMMON_QUEUE_ENABLE = 0x1C;
constexpr uint32_t COMMON_QUEUE_NOTIFY_OFF = 0x1E;
constexpr uint32_t COMMON_QUEUE_DESC_LO = 0x20;
constexpr uint32_t COMMON_QUEUE_DESC_HI = 0x24;
constexpr uint32_t COMMON_QUEUE_AVAIL_LO = 0x28;
constexpr uint32_t COMMON_QUEUE_AVAIL_HI = 0x2C;
constexpr uint32_t COMMON_QUEUE_USED_LO = 0x30;
constexpr uint32_t COMMON_QUEUE_USED_HI = 0x34;

/* virtio-pci capability types (found in the PCI capability list). */
constexpr uint8_t CAP_COMMON_CFG = 1;
constexpr uint8_t CAP_NOTIFY_CFG = 2;
constexpr uint8_t CAP_ISR_CFG = 3;
constexpr uint8_t CAP_DEVICE_CFG = 4;

/* The one feature we insist on: it is what switches the transitional
 * device into the modern (capability-based) mode this driver drives. */
constexpr uint32_t FEATURE_VERSION_1 = 1u << 0; /* bit 32 of features */

/* Virtio-gpu control commands (virtio spec 5.7). */
constexpr uint32_t GPU_CMD_GET_DISPLAY_INFO = 0x0100;
constexpr uint32_t GPU_CMD_RESOURCE_CREATE_2D = 0x0101;
constexpr uint32_t GPU_CMD_SET_SCANOUT = 0x0103;
constexpr uint32_t GPU_CMD_RESOURCE_FLUSH = 0x0104;
constexpr uint32_t GPU_CMD_TRANSFER_TO_HOST_2D = 0x0105;
constexpr uint32_t GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106;

constexpr uint32_t GPU_RESP_OK_NODATA = 0x1100;
constexpr uint32_t GPU_RESP_OK_DISPLAY_INFO = 0x1101;

/* 32-bpp, byte layout B,G,R,X (a pixel is little-endian 0x00RRGGBB).
 * QEMU renders this natively with no format conversion. */
constexpr uint32_t GPU_FORMAT_B8G8R8X8 = 2;

constexpr uint32_t RESOURCE_ID = 1;
constexpr uint16_t MAX_QUEUE_SIZE = 256;
constexpr uint32_t SCRATCH_SIZE = 4096;
constexpr uint32_t NO_VECTOR = 0xFFFF;

/* --- virtio-pci capability bookkeeping --- */

struct CapLoc {
    int bar = -1;
    uint32_t offset = 0;
    uint32_t mult = 0;
};

vnu::pci::Address g_card{};
CapLoc g_cap[6];
uint32_t g_bar_virt[6] = {};

/* MMIO windows handed out by paging::map_device_region(). */
volatile uint8_t* g_common = nullptr;
volatile uint8_t* g_notify = nullptr;

/* --- control virtqueue (split ring, polled) --- */

struct VqDesc {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct VqAvail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[MAX_QUEUE_SIZE];
} __attribute__((packed));

struct VqUsedElem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct VqUsed {
    uint16_t flags;
    uint16_t idx;
    VqUsedElem ring[MAX_QUEUE_SIZE];
} __attribute__((packed));

struct Queue {
    volatile VqDesc* desc;
    VqAvail* avail;
    volatile VqUsed* used;
    uint16_t size;
    uint16_t next_desc;
    uint32_t notify_off;
} g_q{};

/* Virtio-gpu command payloads (all little-endian; the kernel is x86). */

struct GpuHdr {
    uint32_t type;
    uint32_t flags;
    uint32_t fence_lo;
    uint32_t fence_hi;
    uint32_t ctx_id;
    uint32_t ring_idx;
} __attribute__((packed)); /* 24 bytes */

struct GpuRect {
    uint32_t x, y, w, h;
} __attribute__((packed)); /* 16 bytes */

struct GpuDisplay {
    uint32_t x, y, w, h, enabled;
} __attribute__((packed)); /* 20 bytes */

struct GpuRespDisplayInfo {
    GpuHdr hdr;
    uint32_t pmodes;
    uint32_t dmodes;
    GpuDisplay displays[16];
} __attribute__((packed));

struct GpuCreate2D {
    GpuHdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)); /* 40 bytes */

struct BackingEntry {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct GpuAttachBacking {
    GpuHdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    BackingEntry entries[1];
} __attribute__((packed)); /* 48 bytes */

struct GpuSetScanout {
    GpuHdr hdr;
    GpuRect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed)); /* 48 bytes */

struct GpuTransfer2D {
    GpuHdr hdr;
    GpuRect r;
    uint32_t offset_lo;
    uint32_t offset_hi;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)); /* 56 bytes */

struct GpuFlush {
    GpuHdr hdr;
    GpuRect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)); /* 48 bytes */

uint8_t* g_cmd_buf = nullptr;
uint8_t* g_resp_buf = nullptr;
uint8_t* g_scanout = nullptr;
bool g_active = false;

/* --- low-level helpers --- */

inline uint32_t mmio32_r(volatile uint8_t* p)
{
    return *reinterpret_cast<volatile uint32_t*>(p);
}

inline void mmio32_w(volatile uint8_t* p, uint32_t v)
{
    *reinterpret_cast<volatile uint32_t*>(p) = v;
}

/* QEMU splits a 32-bit access to an unaligned (sub-4-byte) register into
 * per-field accesses, so the narrow common-config fields (device status,
 * queue select/size/msix/enable/notify_off) MUST be touched with their own
 * width — a 32-bit store to queue_msix, for instance, would also write the
 * adjacent queue_enable register with the upper half of the value. */
inline uint16_t mmio16_r(volatile uint8_t* p)
{
    return *reinterpret_cast<volatile uint16_t*>(p);
}

inline void mmio16_w(volatile uint8_t* p, uint16_t v)
{
    *reinterpret_cast<volatile uint16_t*>(p) = v;
}

inline uint8_t mmio8_r(volatile uint8_t* p)
{
    return *p;
}

inline void mmio8_w(volatile uint8_t* p, uint8_t v)
{
    *p = v;
}

inline void wmb()
{
    asm volatile("" ::: "memory");
}

/* Byte-granular PCI config reads (capability fields are not necessarily
 * 4-byte aligned). */
uint8_t cfg_byte(uint16_t reg)
{
    uint32_t d = vnu::pci::read_dword(g_card, static_cast<uint16_t>(reg & ~3u));
    return static_cast<uint8_t>(d >> ((reg & 3u) * 8));
}

uint32_t cfg_dword(uint16_t reg)
{
    uint32_t lo = vnu::pci::read_dword(g_card, static_cast<uint16_t>(reg & ~3u));
    if ((reg & 3u) == 0)
        return lo;
    uint32_t hi = vnu::pci::read_dword(g_card, static_cast<uint16_t>((reg + 3u) & ~3u));
    uint32_t shift = static_cast<uint32_t>((reg & 3u) * 8);
    return (lo >> shift) | (hi << (32u - shift));
}

void set_status(uint32_t bits)
{
    uint8_t s = mmio8_r(g_common + COMMON_DEVICE_STATUS);
    mmio8_w(g_common + COMMON_DEVICE_STATUS, static_cast<uint8_t>(s | bits));
}

/* --- boot log helpers: mirrored to the text console and to serial so
 * the driver's verdict shows up in `qemu -serial stdio`, the same way
 * the network self-tests log. --- */

void out(char c)
{
    vnu::tty::putc(c);
    vnu_debug_putc(c);
}

void outstr(const char* s)
{
    for (; *s; ++s)
        out(*s);
}

void putdec(unsigned long v)
{
    if (v == 0) {
        out('0');
        return;
    }
    char buf[12];
    int n = 0;
    while (v) {
        buf[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    }
    while (n)
        out(buf[--n]);
}

void puthex(unsigned long v, int digits)
{
    for (int i = digits - 1; i >= 0; --i) {
        unsigned nib = static_cast<unsigned>((v >> (i * 4)) & 0xF);
        out(static_cast<char>(nib < 10 ? '0' + nib : 'a' + nib - 10));
    }
}

void cap_dump()
{
    for (int t = 1; t <= 5; ++t) {
        if (g_cap[t].bar < 0)
            continue;
        out(' ');
        puthex(static_cast<unsigned long>(t), 1);
        out('/');
        puthex(static_cast<unsigned long>(g_cap[t].bar), 1);
        out('@');
        puthex(static_cast<unsigned long>(g_cap[t].offset), 5);
        if (t == CAP_NOTIFY_CFG) {
            out('*');
            puthex(static_cast<unsigned long>(g_cap[t].mult), 3);
        }
    }
    out('\n');
}

/* --- virtio-pci setup --- */

bool parse_caps()
{
    uint16_t cap = cfg_byte(0x34); /* capabilities pointer */
    while (cap) {
        if (cfg_byte(static_cast<uint16_t>(cap)) == 0x09) { /* vendor-specific */
            uint8_t len = cfg_byte(static_cast<uint16_t>(cap + 2));
            uint8_t type = cfg_byte(static_cast<uint16_t>(cap + 3));
            uint8_t bar = cfg_byte(static_cast<uint16_t>(cap + 4));
            /* QEMU sets cap_len = 0x10 (8 hdr + 8 offset/length) for
             * common/isr/device and 0x14 (multiplier) for notify; the
             * virtio spec only guarantees >= 0x10. */
            if (type >= CAP_COMMON_CFG && type <= CAP_DEVICE_CFG && bar < 6 && len >= 0x10) {
                g_cap[type].bar = bar;
                g_cap[type].offset = cfg_dword(static_cast<uint16_t>(cap + 8));
                if (type == CAP_NOTIFY_CFG && len >= 0x14)
                    g_cap[type].mult = cfg_dword(static_cast<uint16_t>(cap + 16));
            }
        }
        cap = cfg_byte(static_cast<uint16_t>(cap + 1));
    }
    return g_cap[CAP_COMMON_CFG].bar >= 0 && g_cap[CAP_NOTIFY_CFG].bar >= 0;
}

/* Identity-map a PCI BAR into the kernel's device-MMIO window so its
 * registers can be dereferenced as plain pointers. */
volatile uint8_t* map_bar(int idx)
{
    if (g_bar_virt[idx])
        return reinterpret_cast<volatile uint8_t*>(g_bar_virt[idx]);
    uint32_t bar = vnu::pci::read_dword(g_card, static_cast<uint16_t>(0x10 + 4 * idx));
    if ((bar & 1u) || !(bar & ~0xFu))
        return nullptr;
    uint32_t phys = bar & ~0xFu;
    uint32_t size = vnu::pci::bar_size(g_card, idx);
    uint32_t virt = vnu::paging::map_device_region(phys, size);
    if (!virt)
        return nullptr;
    g_bar_virt[idx] = virt;
    return reinterpret_cast<volatile uint8_t*>(virt);
}

bool negotiate_features()
{
    mmio32_w(g_common + COMMON_DEVICE_FEATURE_SELECT, 0);
    (void)mmio32_r(g_common + COMMON_DEVICE_FEATURE); /* lo half, unused */
    mmio32_w(g_common + COMMON_DEVICE_FEATURE_SELECT, 1);
    uint32_t hi = mmio32_r(g_common + COMMON_DEVICE_FEATURE);
    if (!(hi & FEATURE_VERSION_1))
        return false;
    mmio32_w(g_common + COMMON_DRIVER_FEATURE_SELECT, 0);
    mmio32_w(g_common + COMMON_DRIVER_FEATURE, 0);
    mmio32_w(g_common + COMMON_DRIVER_FEATURE_SELECT, 1);
    mmio32_w(g_common + COMMON_DRIVER_FEATURE, FEATURE_VERSION_1);
    return true;
}

bool setup_queue()
{
    mmio16_w(g_common + COMMON_QUEUE_SELECT, 0);
    uint32_t reported = mmio16_r(g_common + COMMON_QUEUE_SIZE);
    /* Q_SIZE is a read-write register: after a device reset it reads 0
     * (QEMU zeroes vring.num) and the driver must write the size it wants
     * — the spec says nothing about the device showing a default. Use the
     * device's value when one is offered, otherwise fall back to our own. */
    uint16_t qsize = reported ? (reported < MAX_QUEUE_SIZE
                                     ? static_cast<uint16_t>(reported)
                                     : MAX_QUEUE_SIZE)
                              : MAX_QUEUE_SIZE;

    uint32_t desc_pages = (static_cast<uint32_t>(qsize) * 16u + 4095u) / 4096u;
    uint32_t avail_pages = (4u + 2u * qsize + 4095u) / 4096u;
    uint32_t used_pages = (4u + 8u * qsize + 4095u) / 4096u;
    uint32_t desc_phys = vnu::pmm::alloc_contig(desc_pages);
    uint32_t avail_phys = vnu::pmm::alloc_contig(avail_pages);
    uint32_t used_phys = vnu::pmm::alloc_contig(used_pages);
    if (!desc_phys || !avail_phys || !used_phys)
        return false;

    /* Identity-mapped by paging, so physical == virtual. pmm zeroes the
     * frames, which is the correct idle state for all three rings. */
    g_q.desc = reinterpret_cast<VqDesc*>(desc_phys);
    g_q.avail = reinterpret_cast<VqAvail*>(avail_phys);
    g_q.used = reinterpret_cast<VqUsed*>(used_phys);
    g_q.size = qsize;
    g_q.next_desc = 0;

    mmio16_w(g_common + COMMON_QUEUE_SIZE, static_cast<uint16_t>(qsize));
    mmio32_w(g_common + COMMON_QUEUE_DESC_LO, desc_phys);
    mmio32_w(g_common + COMMON_QUEUE_DESC_HI, 0);
    mmio32_w(g_common + COMMON_QUEUE_AVAIL_LO, avail_phys);
    mmio32_w(g_common + COMMON_QUEUE_AVAIL_HI, 0);
    mmio32_w(g_common + COMMON_QUEUE_USED_LO, used_phys);
    mmio32_w(g_common + COMMON_QUEUE_USED_HI, 0);
    mmio16_w(g_common + COMMON_QUEUE_MSIX, NO_VECTOR);
    mmio16_w(g_common + COMMON_QUEUE_ENABLE, 1);
    if (mmio16_r(g_common + COMMON_QUEUE_ENABLE) != 1)
        return false;

    g_q.notify_off = mmio16_r(g_common + COMMON_QUEUE_NOTIFY_OFF);
    return true;
}

/* Build one element (a single readable command descriptor followed by a
 * writable response descriptor), kick the queue and poll the used ring
 * until the device reports completion. `expect` is the virtio-gpu
 * response type the command must answer with. */
bool submit_and_wait(uint32_t cmd_len, uint32_t expect)
{
    uint32_t cmd_phys = reinterpret_cast<uint32_t>(g_cmd_buf);
    uint32_t resp_phys = reinterpret_cast<uint32_t>(g_resp_buf);

    uint16_t head = g_q.next_desc;
    if (head + 2 > g_q.size)
        head = 0;
    uint16_t d1 = static_cast<uint16_t>((head + 1) % g_q.size);

    volatile VqDesc* d0 = &g_q.desc[head];
    d0->addr_lo = cmd_phys;
    d0->addr_hi = 0;
    d0->len = cmd_len;
    d0->flags = 1u; /* NEXT: chained to the response descriptor */
    d0->next = d1;
    volatile VqDesc* d1p = &g_q.desc[d1];
    d1p->addr_lo = resp_phys;
    d1p->addr_hi = 0;
    d1p->len = SCRATCH_SIZE;
    d1p->flags = 2u; /* WRITE: the device fills the response here */
    d1p->next = 0;

    uint16_t avail_idx = g_q.avail->idx;
    g_q.avail->ring[avail_idx % g_q.size] = head;
    wmb();
    g_q.avail->idx = static_cast<uint16_t>(avail_idx + 1);
    wmb();

    /* Kick: notification address = notify base + notify_off * mult. */
    volatile uint8_t* notify_addr = g_notify + g_q.notify_off * g_cap[CAP_NOTIFY_CFG].mult;
    mmio32_w(notify_addr, 0);

    uint16_t last = g_q.used->idx;
    for (unsigned spin = 0; spin < 20000000u && g_q.used->idx == last; ++spin)
        asm volatile("pause" ::: "memory");
    if (g_q.used->idx == last)
        return false;

    uint32_t result = reinterpret_cast<const GpuHdr*>(g_resp_buf)->type;
    g_q.next_desc = static_cast<uint16_t>((head + 2) % g_q.size);
    return result == expect;
}

/* All command builders share g_cmd_buf, so only one may be in flight;
 * that holds by construction (commands are submitted and awaited one
 * at a time from a single call site). */
void cmd_start(uint32_t type, uint32_t size)
{
    memset(g_cmd_buf, 0, size);
    reinterpret_cast<GpuHdr*>(g_cmd_buf)->type = type;
}

bool gpu_get_display_info()
{
    cmd_start(GPU_CMD_GET_DISPLAY_INFO, sizeof(GpuHdr));
    return submit_and_wait(sizeof(GpuHdr), GPU_RESP_OK_DISPLAY_INFO);
}

bool gpu_create_2d()
{
    cmd_start(GPU_CMD_RESOURCE_CREATE_2D, sizeof(GpuCreate2D));
    auto* c = reinterpret_cast<GpuCreate2D*>(g_cmd_buf);
    c->resource_id = RESOURCE_ID;
    c->format = GPU_FORMAT_B8G8R8X8;
    c->width = static_cast<uint32_t>(vnu::vgfx::WIDTH);
    c->height = static_cast<uint32_t>(vnu::vgfx::HEIGHT);
    return submit_and_wait(sizeof(GpuCreate2D), GPU_RESP_OK_NODATA);
}

bool gpu_attach_backing()
{
    cmd_start(GPU_CMD_RESOURCE_ATTACH_BACKING, sizeof(GpuAttachBacking));
    auto* c = reinterpret_cast<GpuAttachBacking*>(g_cmd_buf);
    c->resource_id = RESOURCE_ID;
    c->nr_entries = 1;
    c->entries[0].addr_lo = reinterpret_cast<uint32_t>(g_scanout);
    c->entries[0].addr_hi = 0;
    c->entries[0].length = static_cast<uint32_t>(vnu::vgfx::WIDTH * vnu::vgfx::HEIGHT * 4);
    return submit_and_wait(sizeof(GpuAttachBacking), GPU_RESP_OK_NODATA);
}

bool gpu_set_scanout()
{
    cmd_start(GPU_CMD_SET_SCANOUT, sizeof(GpuSetScanout));
    auto* c = reinterpret_cast<GpuSetScanout*>(g_cmd_buf);
    c->r.w = static_cast<uint32_t>(vnu::vgfx::WIDTH);
    c->r.h = static_cast<uint32_t>(vnu::vgfx::HEIGHT);
    c->scanout_id = 0;
    c->resource_id = RESOURCE_ID;
    return submit_and_wait(sizeof(GpuSetScanout), GPU_RESP_OK_NODATA);
}

} // namespace

namespace vnu::virtio_gpu {

bool init()
{
    if (g_active)
        return true;

    bool found = false;
    for (unsigned i = 0; i < sizeof(GPU_DEVICE_IDS) / sizeof(GPU_DEVICE_IDS[0]); ++i) {
        if (pci::find(PCI_VENDOR_VIRTIO, GPU_DEVICE_IDS[i], g_card)) {
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    uint16_t dev_id = pci::device(g_card);
    uint16_t ven_id = pci::vendor(g_card);
    outstr("virtio-gpu: found ");
    puthex(static_cast<unsigned long>(ven_id), 4);
    out(':');
    puthex(static_cast<unsigned long>(dev_id), 4);
    out('\n');

    /* Enable the device on the bus: decode the memory BAR and grant bus
     * mastering (needed for the device to DMA our rings and scanout). */
    uint32_t pci_cmd = pci::read_dword(g_card, 0x04);
    pci::write_dword(g_card, 0x04, pci_cmd | (1u << 1) | (1u << 2));

    auto fail = [&](const char* why) {
        outstr("virtio-gpu: init failed near: ");
        outstr(why);
        out('\n');
        return false;
    };

    if (!parse_caps())
        return fail("parse_caps");
    outstr("virtio-gpu: caps:");
    cap_dump();
    for (int t = CAP_COMMON_CFG; t <= CAP_DEVICE_CFG; ++t) {
        if (g_cap[t].bar < 0)
            continue;
        if (!map_bar(g_cap[t].bar))
            return fail("map_bar");
    }
    g_common = reinterpret_cast<volatile uint8_t*>(g_bar_virt[g_cap[CAP_COMMON_CFG].bar]) +
               g_cap[CAP_COMMON_CFG].offset;
    g_notify = reinterpret_cast<volatile uint8_t*>(g_bar_virt[g_cap[CAP_NOTIFY_CFG].bar]) +
               g_cap[CAP_NOTIFY_CFG].offset;

    /* Official driver start: write 0 to the device status register, which
     * resets the device. QEMU's virtio-vga option ROM pokes the legacy
     * transport during BIOS, which can leave DEVICE_NEEDS_RESET set and
     * the vring sizes zeroed; a fresh reset restores the device. */
    mmio8_w(g_common + COMMON_DEVICE_STATUS, 0);

    set_status(STATUS_ACKNOWLEDGE | STATUS_DRIVER);
    if (!negotiate_features())
        return fail("features");
    set_status(STATUS_FEATURES_OK);
    if (!(mmio8_r(g_common + COMMON_DEVICE_STATUS) & STATUS_FEATURES_OK))
        return fail("features_ok");
    if (!setup_queue())
        return fail("queue");
    set_status(STATUS_DRIVER_OK);

    g_cmd_buf = reinterpret_cast<uint8_t*>(pmm::alloc_frame());
    g_resp_buf = reinterpret_cast<uint8_t*>(pmm::alloc_frame());
    if (!g_cmd_buf || !g_resp_buf)
        return fail("scratch");

    /* 1024x768x4 = 3 MiB of contiguous guest memory the host maps as
     * the scanout resource's backing store. */
    uint32_t fb_pages = (static_cast<uint32_t>(vgfx::WIDTH * vgfx::HEIGHT * 4) + 4095u) / 4096u;
    g_scanout = reinterpret_cast<uint8_t*>(pmm::alloc_contig(fb_pages));
    if (!g_scanout)
        return fail("framebuffer");

    if (!gpu_get_display_info())
        return fail("get_display_info");
    if (!gpu_create_2d())
        return fail("create_2d");
    if (!gpu_attach_backing())
        return fail("attach_backing");
    if (!gpu_set_scanout())
        return fail("set_scanout");

    g_active = true;
    outstr("virtio-gpu: ");
    putdec(static_cast<unsigned long>(vgfx::WIDTH));
    out('x');
    putdec(static_cast<unsigned long>(vgfx::HEIGHT));
    outstr(" scanout armed\n");
    return true;
}

bool active()
{
    return g_active;
}

uint8_t* framebuffer()
{
    return g_scanout;
}

void present()
{
    if (!g_active)
        return;

    cmd_start(GPU_CMD_TRANSFER_TO_HOST_2D, sizeof(GpuTransfer2D));
    auto* t = reinterpret_cast<GpuTransfer2D*>(g_cmd_buf);
    t->r.w = static_cast<uint32_t>(vgfx::WIDTH);
    t->r.h = static_cast<uint32_t>(vgfx::HEIGHT);
    t->resource_id = RESOURCE_ID;
    if (!submit_and_wait(sizeof(GpuTransfer2D), GPU_RESP_OK_NODATA))
        return;

    cmd_start(GPU_CMD_RESOURCE_FLUSH, sizeof(GpuFlush));
    auto* f = reinterpret_cast<GpuFlush*>(g_cmd_buf);
    f->r.w = static_cast<uint32_t>(vgfx::WIDTH);
    f->r.h = static_cast<uint32_t>(vgfx::HEIGHT);
    f->resource_id = RESOURCE_ID;
    (void)submit_and_wait(sizeof(GpuFlush), GPU_RESP_OK_NODATA);
}

} // namespace vnu::virtio_gpu