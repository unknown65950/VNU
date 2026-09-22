#include <vnu/net.h>
#include <vnu/pci.h>
#include <vnu/paging.h>
#include <vnu/pmm.h>
#include <vnu/abi.h>
#include <stdint.h>

extern "C" void* memcpy(void* dst, const void* src, unsigned long count);
extern "C" void* memset(void* dst, int val, unsigned long count);

namespace {

/* --- e1000 register map (82540EM / QEMU's default NIC) --- */

constexpr uint16_t REG_CTRL = 0x0000;
constexpr uint16_t REG_STATUS = 0x0008;
constexpr uint16_t REG_EERD = 0x0014;
constexpr uint16_t REG_RCTL = 0x0100;
constexpr uint16_t REG_TCTL = 0x0400;
constexpr uint16_t REG_TIPG = 0x0410;
constexpr uint16_t REG_RDBAL = 0x2800;
constexpr uint16_t REG_RDBAH = 0x2804;
constexpr uint16_t REG_RDLEN = 0x2808;
constexpr uint16_t REG_RDH = 0x2810;
constexpr uint16_t REG_RDT = 0x2818;
constexpr uint16_t REG_TDBAL = 0x3800;
constexpr uint16_t REG_TDBAH = 0x3804;
constexpr uint16_t REG_TDLEN = 0x3808;
constexpr uint16_t REG_TDH = 0x3810;
constexpr uint16_t REG_TDT = 0x3818;

constexpr uint32_t CTRL_RST = 1u << 26;
constexpr uint32_t CTRL_SLU = 1u << 6; /* software sets link up */

constexpr uint32_t RCTL_EN = 1u << 1;
constexpr uint32_t RCTL_SBP = 1u << 2;
constexpr uint32_t RCTL_UPE = 1u << 3;
constexpr uint32_t RCTL_MPE = 1u << 4;
constexpr uint32_t RCTL_BAM = 1u << 15;
constexpr uint32_t RCTL_SECRC = 1u << 26;

constexpr uint32_t TCTL_EN = 1u << 1;
constexpr uint32_t TCTL_PSP = 1u << 3;
constexpr uint32_t TCTL_CT = 0x10u << 4;   /* collision threshold */
constexpr uint32_t TCTL_COLD = 0x40u << 12; /* collision distance */

constexpr uint8_t DESC_CMD_EOP = 1u << 0;
constexpr uint8_t DESC_CMD_RS = 1u << 3;
constexpr uint8_t DESC_CMD_IFCS = 1u << 4;
constexpr uint8_t DESC_STATUS_DD = 1u << 0;

constexpr unsigned RX_N = 32;
constexpr unsigned TX_N = 16;
constexpr unsigned RX_BUFSIZE = 2048;

struct RxDesc {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct TxDesc {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

volatile uint8_t* g_mmio = nullptr;
bool g_up = false;

uint8_t g_mac[6];
uint32_t g_ip = vnu::net::OUR_IP;

/* Backing frames for the descriptor rings and one shared TX buffer
 * (only ever one packet in flight, so a single buffer suffices). */
RxDesc* g_rx_ring;
uint32_t g_rx_ring_phys;
TxDesc* g_tx_ring;
uint32_t g_tx_ring_phys;
uint32_t* g_rx_bufs;
uint8_t* g_tx_buf;
uint32_t g_tx_buf_phys;
uint32_t g_rx_tail = 0;

/* ARP cache: one entry (the peer we last talked to). */
uint32_t g_arp_ip = 0;    /* resolved peer, big-endian */
uint8_t g_arp_mac[6];
uint32_t g_arp_resolve = 0; /* address of an in-flight request (0 = none) */

/* Pending ICMP echo state. */
uint32_t g_ping_ip = 0;
uint16_t g_ping_id = 0x2B7E;
uint16_t g_ping_seq = 0;
bool g_ping_done = false;

/* --- port I/O helpers (PIT clock) --- */

void outb(uint16_t port, uint8_t val)
{
    asm volatile("outb %0,%1" : : "a"(val), "Nd"(port));
}

uint8_t inb(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

/* PIT channel 2 (otherwise unused) in square-wave mode with reload 0
 * (64856 ticks per period ≈ 54.9254 ms) as a polling millisecond clock. */
volatile uint32_t g_pit_wraps = 0;
volatile uint32_t g_pit_prev = 0;

void pit_init_ms()
{
    outb(0x43, 0xB6); /* ch2, LSB then MSB, mode 3, binary */
    outb(0x42, 0x00);
    outb(0x42, 0x00);
    g_pit_prev = 0;
    g_pit_wraps = 0;
}

uint32_t pit_read_count()
{
    outb(0x43, 0x80); /* latch counter 2 */
    uint8_t l = inb(0x42);
    uint8_t h = inb(0x42);
    return static_cast<uint32_t>(l) | (static_cast<uint32_t>(h) << 8);
}

/* --- MMIO register access --- */

volatile uint32_t* reg(uint16_t off)
{
    return reinterpret_cast<volatile uint32_t*>(g_mmio + off);
}

void wr(uint16_t off, uint32_t val)
{
    *reg(off) = val;
}

uint32_t rd(uint16_t off)
{
    return *reg(off);
}

/* --- network byte-order helpers --- */

void put_be16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

void put_be32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

uint32_t get_be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint16_t checksum(const uint8_t* d, unsigned len)
{
    uint32_t sum = 0;
    unsigned i = 0;
    for (; i + 1 < len; i += 2)
        sum += (static_cast<uint32_t>(d[i]) << 8) | d[i + 1];
    if (i < len)
        sum += static_cast<uint32_t>(d[i]) << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

/* --- MAC via the EEPROM interface --- */

void read_mac()
{
    for (int w = 0; w < 3; ++w) {
        wr(REG_EERD, (static_cast<uint32_t>(w) << 8) | 1u);
        uint16_t data = 0xFFFF;
        for (unsigned spin = 0; spin < 10000; ++spin) {
            uint32_t v = rd(REG_EERD);
            if (v & 0x10u) { /* EERD read-done */
                data = static_cast<uint16_t>(v >> 16);
                break;
            }
        }
        g_mac[w * 2] = static_cast<uint8_t>(data & 0xFF);
        g_mac[w * 2 + 1] = static_cast<uint8_t>(data >> 8);
    }
}

/* --- TX: one shared buffer, one descriptor, TDT as producer index --- */

bool tx_send(const uint8_t* data, uint16_t len)
{
    if (len == 0 || len > RX_BUFSIZE || !g_up)
        return false;

    unsigned spin = 0;
    for (;;) {
        uint32_t tdt = rd(REG_TDT);
        uint32_t tdh = rd(REG_TDH);
        uint32_t used = (tdt + TX_N - tdh) % TX_N;
        if (used < TX_N - 1)
            break;
        if (++spin > 100000)
            return false; /* ring full; drop rather than hang */
    }

    memcpy(g_tx_buf, data, len);
    uint32_t tdt = rd(REG_TDT);
    TxDesc& d = g_tx_ring[tdt % TX_N];
    d.addr_lo = g_tx_buf_phys;
    d.addr_hi = 0;
    d.length = len;
    d.cso = 0;
    d.cmd = DESC_CMD_EOP | DESC_CMD_RS | DESC_CMD_IFCS;
    d.status = 0;
    d.css = 0;
    d.special = 0;
    wr(REG_TDT, (tdt + 1u) % TX_N);
    return true;
}

/* --- packet builders (all frames assembled in the shared TX buffer) --- */

void eth_header(uint16_t ethertype, const uint8_t* dst)
{
    memset(const_cast<uint8_t*>(g_tx_buf), 0, 60);
    memcpy(g_tx_buf, dst, 6);
    memcpy(g_tx_buf + 6, g_mac, 6);
    put_be16(g_tx_buf + 12, ethertype);
}

void send_arp_request(uint32_t target)
{
    const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    eth_header(0x0806, bcast);
    put_be16(g_tx_buf + 14, 1);       /* htype: Ethernet */
    put_be16(g_tx_buf + 16, 0x0800);  /* ptype: IPv4 */
    g_tx_buf[18] = 6;
    g_tx_buf[19] = 4;
    put_be16(g_tx_buf + 20, 1);       /* op: request */
    memcpy(g_tx_buf + 22, g_mac, 6);
    put_be32(g_tx_buf + 28, g_ip);
    memset(g_tx_buf + 32, 0, 6);
    put_be32(g_tx_buf + 38, target);
    tx_send(g_tx_buf, 42);
}

void send_arp_reply(const uint8_t* dst_mac, uint32_t dst_ip)
{
    eth_header(0x0806, dst_mac);
    put_be16(g_tx_buf + 14, 1);
    put_be16(g_tx_buf + 16, 0x0800);
    g_tx_buf[18] = 6;
    g_tx_buf[19] = 4;
    put_be16(g_tx_buf + 20, 2);       /* op: reply */
    memcpy(g_tx_buf + 22, g_mac, 6);
    put_be32(g_tx_buf + 28, g_ip);
    memcpy(g_tx_buf + 32, dst_mac, 6);
    put_be32(g_tx_buf + 38, dst_ip);
    tx_send(g_tx_buf, 42);
}

/* Builds an ICMP echo with `icmp_type` (8 = request, 0 = reply) and 56
 * bytes of payload, and sends it. Returns the total frame length. */
uint16_t send_icmp(const uint8_t* dst_mac, uint32_t dst_ip, uint8_t icmp_type,
                   uint16_t id, uint16_t seq, const uint8_t* payload)
{
    eth_header(0x0800, dst_mac);

    g_tx_buf[14] = 0x45; /* IPv4, IHL 5 */
    g_tx_buf[15] = 0;
    put_be16(g_tx_buf + 16, 20 + 8 + 56); /* total IP length */
    put_be16(g_tx_buf + 18, static_cast<uint16_t>(id));
    put_be16(g_tx_buf + 20, 0); /* flags/fragment offset */
    g_tx_buf[22] = 64;          /* TTL */
    g_tx_buf[23] = 1;           /* ICMP */
    put_be16(g_tx_buf + 24, 0); /* checksum filled below */
    put_be32(g_tx_buf + 26, g_ip);
    put_be32(g_tx_buf + 30, dst_ip);

    uint8_t* icmp = g_tx_buf + 34;
    icmp[0] = icmp_type;
    icmp[1] = 0;
    put_be16(icmp + 2, 0); /* checksum filled below */
    put_be16(icmp + 4, id);
    put_be16(icmp + 6, seq);
    memcpy(icmp + 8, payload, 56);

    put_be16(icmp + 2, checksum(icmp, 8 + 56));
    put_be16(g_tx_buf + 24, checksum(g_tx_buf + 14, 20));

    uint16_t len = static_cast<uint16_t>(14 + 20 + 8 + 56);
    tx_send(g_tx_buf, len);
    return len;
}

/* --- RX --- */

void handle_frame(const uint8_t* f, uint16_t len)
{
    if (len < 16)
        return;
    uint16_t ethertype = (static_cast<uint16_t>(f[12]) << 8) | f[13];

    if (ethertype == 0x0806) { /* ARP */
        if (len < 42)
            return;
        uint16_t op = (static_cast<uint16_t>(f[20]) << 8) | f[21];
        uint32_t tip = get_be32(f + 38);
        if (tip != g_ip)
            return;
        if (op == 1) /* request for us: answer it */
            send_arp_reply(f + 6, get_be32(f + 28));
        else if (op == 2 && get_be32(f + 28) == g_arp_resolve) {
            memcpy(g_arp_mac, f + 6, 6);
            g_arp_ip = g_arp_resolve;
        }
        return;
    }

    if (ethertype != 0x0800) /* IPv4 */
        return;
    if (len < 34)
        return;
    if ((f[14] >> 4) != 4)
        return;
    uint16_t iplen = (static_cast<uint16_t>(f[16]) << 8) | f[17];
    if (iplen < 20 || static_cast<uint16_t>(14 + iplen) > len)
        return;
    if (f[23] != 1) /* ICMP */
        return;
    if (iplen < 28)
        return;

    uint32_t src_ip = get_be32(f + 26);
    uint32_t dst_ip = get_be32(f + 30);
    if (dst_ip != g_ip)
        return;

    const uint8_t* icmp = f + 34;
    uint8_t icmp_type = icmp[0];
    uint16_t id = (static_cast<uint16_t>(icmp[4]) << 8) | icmp[5];
    uint16_t seq = (static_cast<uint16_t>(icmp[6]) << 8) | icmp[7];

    if (icmp_type == 0) { /* echo reply: does it answer our ping? */
        if (src_ip == g_ping_ip && g_ping_id == id && g_ping_seq == seq)
            g_ping_done = true;
        return;
    }
    if (icmp_type == 8) {
        /* echo request: answer it (echo back id/seq and payload) */
        const uint8_t* payload = icmp + 8;
        uint16_t paylen = static_cast<uint16_t>(iplen - 20 - 8);
        eth_header(0x0800, f + 6);
        g_tx_buf[14] = 0x45;
        g_tx_buf[15] = 0;
        put_be16(g_tx_buf + 16, static_cast<uint16_t>(20 + 8 + paylen));
        put_be16(g_tx_buf + 18, static_cast<uint16_t>(id));
        put_be16(g_tx_buf + 20, 0);
        g_tx_buf[22] = 64;
        g_tx_buf[23] = 1;
        put_be16(g_tx_buf + 24, 0);
        put_be32(g_tx_buf + 26, g_ip);
        put_be32(g_tx_buf + 30, src_ip);
        uint8_t* r = g_tx_buf + 34;
        r[0] = 0;
        r[1] = 0;
        put_be16(r + 2, 0);
        put_be16(r + 4, id);
        put_be16(r + 6, seq);
        if (paylen > 56)
            paylen = 56;
        memcpy(r + 8, payload, paylen);
        put_be16(r + 2, checksum(r, 8 + paylen));
        put_be16(g_tx_buf + 24, checksum(g_tx_buf + 14, 20));
        tx_send(g_tx_buf, static_cast<uint16_t>(14 + 20 + 8 + paylen));
    }
}

void rx_drain()
{
    for (unsigned i = 0; i < RX_N; ++i) {
        uint32_t n = (g_rx_tail + 1u) % RX_N;
        RxDesc& d = g_rx_ring[n];
        if (!(d.status & DESC_STATUS_DD))
            return;
        handle_frame(reinterpret_cast<const uint8_t*>(g_rx_bufs[n]), d.length);
        d.status = 0;
        d.length = 0;
        g_rx_tail = n;
        wr(REG_RDT, g_rx_tail);
    }
}

} // namespace

namespace vnu::net {

int init()
{
    pci::Address card{};
    if (!pci::find(0x8086, 0x100E, card))
        return -VNU_EIO;

    uint32_t bar0 = pci::read_dword(card, 0x10);
    if ((bar0 & 1u) || bar0 == 0)
        return -VNU_EIO; /* need a memory BAR */
    uint32_t phys = bar0 & ~0xFu;
    uint32_t size = pci::bar_size(card, 0);

    /* Enable the device on the bus: decode its memory BAR and grant DMA
     * (bus master), or the card can never receive DMA'd frames. */
    uint32_t cmd = pci::read_dword(card, 0x04);
    pci::write_dword(card, 0x04, cmd | (1u << 1) /* mem space */ | (1u << 2) /* bus master */);

    g_mmio = reinterpret_cast<volatile uint8_t*>(paging::map_device_region(phys, size));
    if (!g_mmio)
        return -VNU_EIO;

    pit_init_ms();

    /* Soft reset, then force the link up (QEMU's link is up by default). */
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    for (unsigned spin = 0; spin < 100000 && (rd(REG_CTRL) & CTRL_RST); ++spin) {
    }
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SLU);

    read_mac();

    g_rx_ring_phys = pmm::alloc_contig(1);
    g_tx_ring_phys = pmm::alloc_contig(1);
    g_tx_buf_phys = pmm::alloc_frame();
    g_rx_bufs = reinterpret_cast<uint32_t*>(pmm::alloc_contig((RX_N * sizeof(uint32_t) + 4095) / 4096));
    g_tx_buf = reinterpret_cast<uint8_t*>(g_tx_buf_phys);
    g_rx_ring = reinterpret_cast<RxDesc*>(g_rx_ring_phys);
    g_tx_ring = reinterpret_cast<TxDesc*>(g_tx_ring_phys);
    if (!g_rx_ring_phys || !g_tx_ring_phys || !g_tx_buf_phys || !g_rx_bufs) {
        wr(REG_CTRL, 0);
        return -VNU_EIO;
    }

    for (unsigned i = 0; i < RX_N; ++i) {
        g_rx_bufs[i] = pmm::alloc_frame();
        if (!g_rx_bufs[i])
            return -VNU_EIO;
        g_rx_ring[i].addr_lo = g_rx_bufs[i];
        g_rx_ring[i].addr_hi = 0;
        g_rx_ring[i].status = 0;
        g_rx_ring[i].length = 0;
    }

    wr(REG_RCTL, 0);
    wr(REG_RDBAL, g_rx_ring_phys);
    wr(REG_RDBAH, 0);
    wr(REG_RDLEN, RX_N * sizeof(RxDesc));
    wr(REG_RDH, 0);
    wr(REG_RDT, RX_N - 1u);
    g_rx_tail = RX_N - 1u;
    wr(REG_RCTL, RCTL_EN | RCTL_SBP | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);

    wr(REG_TDLEN, TX_N * sizeof(TxDesc));
    wr(REG_TDBAL, g_tx_ring_phys);
    wr(REG_TDBAH, 0);
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    wr(REG_TIPG, 0x0060200A);
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);

    /* QEMU arms a ~1 s flush-queue timer when RX is enabled and discards
     * any frame that arrives while it is pending; wait it out once. */
    uint32_t t0 = uptime_ms();
    while (uptime_ms() - t0 < 1100u) {
    }

    g_up = true;
    return 0;
}

uint32_t uptime_ms()
{
    uint32_t c = pit_read_count();
    if (c > g_pit_prev)
        g_pit_wraps += 1;
    g_pit_prev = c;
    return g_pit_wraps * 55u + (65535u - c) / 1193u;
}

long ping(uint32_t ip, uint32_t timeout_ms)
{
    if (!g_up)
        return -VNU_EIO;
    if (ip == OUR_IP || ip == 0)
        return 0; /* pinging ourselves: nothing leaves the machine */
    if (timeout_ms < 10)
        timeout_ms = 10;
    if (timeout_ms > 2000)
        timeout_ms = 2000;

    if (ip != g_arp_ip) {
        g_arp_resolve = ip;
        g_arp_ip = 0;
        send_arp_request(ip);
        uint32_t t0 = uptime_ms();
        while (uptime_ms() - t0 < timeout_ms) {
            rx_drain();
            if (g_arp_ip == ip)
                break;
        }
        g_arp_resolve = 0;
        if (g_arp_ip != ip)
            return -VNU_EHOSTUNREACH;
    }

    const uint8_t payload[56] = {0};
    ++g_ping_seq;
    g_ping_ip = ip;
    g_ping_done = false;
    send_icmp(g_arp_mac, ip, 8, g_ping_id, g_ping_seq, payload);

    uint32_t t0 = uptime_ms();
    while (uptime_ms() - t0 < timeout_ms) {
        rx_drain();
        if (g_ping_done)
            return static_cast<long>(uptime_ms() - t0);
    }
    return -VNU_ETIMEDOUT;
}

void get_info(Info* out)
{
    if (!out)
        return;
    out->up = g_up ? 1 : 0;
    out->ip = g_ip;
    out->mask = OUR_MASK;
    out->gw = GW_IP;
    for (int i = 0; i < 6; ++i)
        out->mac[i] = g_mac[i];
}

} // namespace vnu::net