#include <vnu/net.h>
#include <vnu/pci.h>
#include <vnu/paging.h>
#include <vnu/pmm.h>
#include <vnu/abi.h>
#include <vnu/vfs.h>
#include <stdint.h>

extern "C" void* memcpy(void* dst, const void* src, unsigned long count);
extern "C" void* memset(void* dst, int val, unsigned long count);
extern "C" int memcmp(const void* a, const void* b, unsigned long count);

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

/* IPv4 ID counter for outgoing non-ICMP datagrams. */
uint16_t g_ip_id = 0x4000;

/* Pending DNS query state (one in flight, same model as the ping). */
bool g_dns_active = false;
uint32_t g_dns_server = 0;
uint16_t g_dns_port = 0xCD00;
uint16_t g_dns_txid = 0;
bool g_dns_done = false;
long g_dns_rc = -VNU_ENOENT;
uint32_t g_dns_ip = 0;

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

/* --- DNS over UDP --- */

/* Encodes `name` ("foo.example") into the length-prefixed qname labels
 * each <=63 bytes, returns the total label bytes (final 0 included) or
 * -1 for an empty/oversized/odd name. */
int dns_encode_name(const char* name, uint8_t* out, int out_cap)
{
    if (!name || !name[0] || out_cap < 2)
        return -1;
    int n = 0;
    const char* p = name;
    for (;;) {
        const char* dot = p;
        int lab = 0;
        while (*dot && *dot != '.') {
            ++dot;
            ++lab;
        }
        if (lab == 0 || lab > 63)
            return -1;
        if (n + 1 + lab + 1 > out_cap)
            return -1;
        out[n++] = static_cast<uint8_t>(lab);
        for (int i = 0; i < lab; ++i)
            out[n++] = static_cast<uint8_t>(p[i]);
        if (*dot == '.') {
            p = dot + 1;
            if (!*p)
                break; /* trailing dot, fine */
            continue;
        }
        break;
    }
    out[n++] = 0;
    return n;
}

/* Assembles a DNS A query (RD set, QD=1) in the shared TX buffer and
 * sends it; the UDP checksum covers the IPv4 pseudo-header. */
void send_dns_query(const uint8_t* dst_mac, uint32_t server_ip,
                    const uint8_t* qname, int qlen)
{
    uint16_t dlen = static_cast<uint16_t>(12 + qlen + 4);
    eth_header(0x0800, dst_mac);

    g_tx_buf[14] = 0x45;
    g_tx_buf[15] = 0;
    put_be16(g_tx_buf + 16, static_cast<uint16_t>(20 + 8 + dlen));
    put_be16(g_tx_buf + 18, g_ip_id++);
    put_be16(g_tx_buf + 20, 0);
    g_tx_buf[22] = 64;
    g_tx_buf[23] = 17; /* UDP */
    put_be16(g_tx_buf + 24, 0);
    put_be32(g_tx_buf + 26, g_ip);
    put_be32(g_tx_buf + 30, server_ip);

    uint8_t* udp = g_tx_buf + 34;
    put_be16(udp + 0, g_dns_port);
    put_be16(udp + 2, 53);
    put_be16(udp + 4, static_cast<uint16_t>(8 + dlen));
    put_be16(udp + 6, 0); /* filled below */

    uint8_t* dns = g_tx_buf + 42;
    memset(const_cast<uint8_t*>(dns), 0, dlen);
    put_be16(dns + 0, g_dns_txid);
    put_be16(dns + 2, 0x0100); /* RD */
    put_be16(dns + 4, 1);      /* QDCOUNT; others stay 0 */
    memcpy(dns + 12, qname, static_cast<unsigned long>(qlen));
    put_be16(dns + 12 + qlen, 1); /* QTYPE: A */
    put_be16(dns + 14 + qlen, 1); /* QCLASS: IN */

    /* UDP checksum over pseudo-header + datagram (IPv4 allows 0, but a
     * real one survives slirp's and normal routers' validation). */
    uint8_t csbuf[12 + 300];
    put_be32(csbuf + 0, g_ip);
    put_be32(csbuf + 4, server_ip);
    csbuf[8] = 0;
    csbuf[9] = 17;
    put_be16(csbuf + 10, static_cast<uint16_t>(8 + dlen));
    memcpy(csbuf + 12, udp, 8);
    memcpy(csbuf + 20, dns, static_cast<unsigned long>(dlen));
    put_be16(udp + 6, checksum(csbuf, 20 + dlen));
    put_be16(g_tx_buf + 24, checksum(g_tx_buf + 14, 20));

    tx_send(g_tx_buf, static_cast<uint16_t>(14 + 20 + 8 + dlen));
}

/* DNS name walker: follows label/length bytes; a 0xC0 compression
 * pointer consumes 2 bytes and ends the name. Advances `off`. */
void dns_skip_name(const uint8_t* d, uint32_t n, uint32_t* off)
{
    uint32_t o = *off;
    while (o < n) {
        uint8_t b = d[o];
        if (b == 0) {
            o += 1;
            break;
        }
        if ((b & 0xC0u) == 0xC0) {
            o += 2;
            break;
        }
        if (b > 63) {
            o = n;
            break;
        }
        o += static_cast<uint32_t>(b) + 1;
    }
    *off = o;
}

/* Parses the answer section of a DNS response and picks the first A
 * record; sets g_dns_rc (0 or -VNU_ENOENT) and g_dns_ip. */
void dns_parse(const uint8_t* d, uint32_t n)
{
    g_dns_rc = -VNU_ENOENT;
    if (n < 12)
        return;
    uint16_t id = (static_cast<uint16_t>(d[0]) << 8) | d[1];
    if (id != g_dns_txid)
        return;
    uint16_t flags = (static_cast<uint16_t>(d[2]) << 8) | d[3];
    if (!(flags & 0x8000u)) /* not a response */
        return;
    uint16_t qd = (static_cast<uint16_t>(d[4]) << 8) | d[5];
    uint16_t an = (static_cast<uint16_t>(d[6]) << 8) | d[7];
    uint32_t off = 12;
    for (unsigned q = 0; q < qd; ++q) {
        dns_skip_name(d, n, &off);
        off += 4; /* qtype + qclass */
    }
    for (unsigned a = 0; a < an; ++a) {
        if (off >= n)
            return;
        dns_skip_name(d, n, &off);
        if (off + 10 > n)
            return;
        uint16_t type = (static_cast<uint16_t>(d[off]) << 8) | d[off + 1];
        uint16_t cls = (static_cast<uint16_t>(d[off + 2]) << 8) | d[off + 3];
        uint16_t rdlen = (static_cast<uint16_t>(d[off + 8]) << 8) | d[off + 9];
        if (type == 1 && cls == 1 && rdlen >= 4 && off + 10 + 4 <= n) {
            g_dns_ip = get_be32(d + off + 10);
            g_dns_rc = 0;
            return;
        }
        off += 10 + rdlen;
        if (off > n)
            return;
    }
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
    uint8_t proto = f[23];
    if (proto != 1 && proto != 17) /* ICMP or UDP */
        return;
    if (iplen < 28)
        return;

    uint32_t src_ip = get_be32(f + 26);
    uint32_t dst_ip = get_be32(f + 30);
    if (dst_ip != g_ip)
        return;

    if (proto == 17) {
        /* A DNS answer to the in-flight query? Match on server IP, our
         * source port and (inside the payload) the transaction id. */
        if (!g_dns_active)
            return;
        uint16_t dport = (static_cast<uint16_t>(f[36]) << 8) | f[37];
        if (src_ip == g_dns_server && dport == g_dns_port) {
            uint16_t udplen = (static_cast<uint16_t>(f[38]) << 8) | f[39];
            uint32_t dlen = udplen >= 8 ? udplen - 8 : 0;
            if (dlen > static_cast<uint32_t>(len - 42))
                dlen = static_cast<uint32_t>(len - 42);
            if (dlen > 512)
                dlen = 512;
            dns_parse(f + 42, dlen);
            g_dns_done = true;
        }
        return;
    }

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

bool on_subnet(uint32_t ip)
{
    return (ip & vnu::net::OUR_MASK) == (g_ip & vnu::net::OUR_MASK);
}

/* Resolve `peer` to a MAC address: on the local subnet the peer itself
 * (ARP), otherwise the gateway — frames then carry the gateway MAC with
 * the peer IP, which QEMU's slirp NAT forwards. Reuses the single-entry
 * ARP cache. Returns 0, -VNU_EIO or -VNU_EHOSTUNREACH. */
int arp_ensure(uint32_t peer, uint8_t mac[6], uint32_t timeout_ms)
{
    if (!g_up)
        return -VNU_EIO;
    uint32_t target = on_subnet(peer) ? peer : vnu::net::GW_IP;
    if (g_arp_ip == target) {
        memcpy(mac, g_arp_mac, 6);
        return 0;
    }
    g_arp_resolve = target;
    g_arp_ip = 0;
    send_arp_request(target);
    uint32_t t0 = vnu::net::uptime_ms();
    while (vnu::net::uptime_ms() - t0 < timeout_ms) {
        rx_drain();
        if (g_arp_ip == target)
            break;
    }
    g_arp_resolve = 0;
    if (g_arp_ip != target)
        return -VNU_EHOSTUNREACH;
    memcpy(mac, g_arp_mac, 6);
    return 0;
}

/* Case-insensitive string compare. */
bool streq_ci(const char* a, const char* b)
{
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z')
            x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = static_cast<char>(y + 32);
        if (x != y)
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

/* "a.b.c.d" -> big-endian uint32, or -1 on a malformed quad. */
long parse_ipv4(const char* s, uint32_t* out)
{
    uint32_t oct[4];
    int n = 0;
    uint32_t v = 0;
    int have = 0;
    for (const char* p = s;; ++p) {
        if (*p >= '0' && *p <= '9') {
            v = v * 10 + static_cast<uint32_t>(*p - '0');
            have = 1;
        } else if (*p == '.' && have && n < 3) {
            if (v > 255)
                return -1;
            oct[n++] = v;
            v = 0;
            have = 0;
        } else if (*p == '\0' && have && n == 3) {
            if (v > 255)
                return -1;
            oct[n++] = v;
            break;
        } else {
            return -1;
        }
    }
    *out = (oct[0] << 24) | (oct[1] << 16) | (oct[2] << 8) | oct[3];
    return 0;
}

/* /etc/hosts lookup: lines of `ip alias [alias...]`; `#` starts a
 * comment. Returns 0 with *out set, or -VNU_ENOENT. */
long hosts_lookup(const char* name, uint32_t* out)
{
    char buf[512];
    int n = vnu::vfs::read_path("/etc/hosts", buf, sizeof(buf) - 1);
    if (n <= 0)
        return -VNU_ENOENT;
    buf[n] = 0;

    const char* p = buf;
    while (*p) {
        const char* eol = p;
        while (*eol && *eol != '\n')
            ++eol;
        const char* t = p;
        while (t < eol && (*t == ' ' || *t == '\t'))
            ++t;
        if (t < eol && *t != '#') {
            const char* e = t;
            while (e < eol && *e != ' ' && *e != '\t')
                ++e;
            char tok[64];
            int tl = (e - t) < 63 ? static_cast<int>(e - t) : 63;
            memcpy(tok, t, static_cast<unsigned long>(tl));
            tok[tl] = 0;
            uint32_t ip;
            if (parse_ipv4(tok, &ip) == 0) {
                const char* a = e;
                while (a < eol) {
                    while (a < eol && (*a == ' ' || *a == '\t'))
                        ++a;
                    if (a >= eol || *a == '#')
                        break;
                    const char* ae = a;
                    while (ae < eol && *ae != ' ' && *ae != '\t')
                        ++ae;
                    char ali[64];
                    int al = (ae - a) < 63 ? static_cast<int>(ae - a) : 63;
                    memcpy(ali, a, static_cast<unsigned long>(al));
                    ali[al] = 0;
                    if (streq_ci(ali, name)) {
                        *out = ip;
                        return 0;
                    }
                    a = ae;
                }
            }
        }
        p = eol;
        if (*p == '\n')
            ++p;
    }
    return -VNU_ENOENT;
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

    uint8_t dst_mac[6];
    long rc = arp_ensure(ip, dst_mac, timeout_ms);
    if (rc < 0)
        return rc; /* -VNU_EHOSTUNREACH: no ARP reply */

    const uint8_t payload[56] = {0};
    ++g_ping_seq;
    g_ping_ip = ip;
    g_ping_done = false;
    send_icmp(dst_mac, ip, 8, g_ping_id, g_ping_seq, payload);

    uint32_t t0 = uptime_ms();
    while (uptime_ms() - t0 < timeout_ms) {
        rx_drain();
        if (g_ping_done)
            return static_cast<long>(uptime_ms() - t0);
    }
    return -VNU_ETIMEDOUT;
}

/* DNS A-record lookup for `name` against `server`. ARPs for the server
 * (via the gateway when off-subnet), sends the query and waits for an
 * answer. Returns 0 (with *out big-endian), -VNU_EIO, -VNU_ENOENT,
 * -VNU_EHOSTUNREACH or -VNU_ETIMEDOUT. */
long dns_query(uint32_t server, const char* name, uint32_t* out,
               uint32_t timeout_ms)
{
    if (!g_up)
        return -VNU_EIO;
    if (!out || !name || !name[0])
        return -VNU_ENOENT;
    if (timeout_ms < 10)
        timeout_ms = 10;
    if (timeout_ms > 2000)
        timeout_ms = 2000;

    uint8_t qname[256];
    int qlen = dns_encode_name(name, qname, sizeof(qname));
    if (qlen < 0)
        return -VNU_ENOENT;

    uint8_t dst_mac[6];
    long rc = arp_ensure(server, dst_mac, timeout_ms);
    if (rc < 0)
        return rc;

    g_dns_active = true;
    g_dns_server = server;
    ++g_dns_txid;
    g_dns_done = false;
    g_dns_rc = -VNU_ENOENT;
    g_dns_ip = 0;

    send_dns_query(dst_mac, server, qname, qlen);

    uint32_t t0 = uptime_ms();
    while (uptime_ms() - t0 < timeout_ms) {
        rx_drain();
        if (g_dns_done)
            break;
    }
    g_dns_active = false;
    if (!g_dns_done)
        return -VNU_ETIMEDOUT;
    if (g_dns_rc == 0)
        *out = g_dns_ip;
    return g_dns_rc;
}

/* Resolve `name` to a big-endian IPv4: /etc/hosts first, then DNS via
 * the nameservers listed in /etc/resolv.conf (default 1.1.1.1 when the
 * file lists none or is missing), tried in order until one answers. */
long resolve_host(const char* name, uint32_t* out)
{
    if (!out || !name || !name[0])
        return -VNU_ENOENT;

    if (hosts_lookup(name, out) == 0)
        return 0;

    uint32_t servers[4];
    int nsv = 0;
    char buf[512];
    int n = vnu::vfs::read_path("/etc/resolv.conf", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        const char* p = buf;
        while (*p && nsv < 4) {
            const char* eol = p;
            while (*eol && *eol != '\n')
                ++eol;
            const char* t = p;
            while (t < eol && (*t == ' ' || *t == '\t'))
                ++t;
            if (t < eol && *t != ';' && *t != '#' &&
                (eol - t) >= 11 && memcmp(t, "nameserver", 10) == 0) {
                const char* a = t + 10;
                while (a < eol && (*a == ' ' || *a == '\t'))
                    ++a;
                const char* ae = a;
                while (ae < eol && *ae != ' ' && *ae != '\t')
                    ++ae;
                char tok[32];
                int tl = (ae - a) < 31 ? static_cast<int>(ae - a) : 31;
                memcpy(tok, a, static_cast<unsigned long>(tl));
                tok[tl] = 0;
                uint32_t ip;
                if (tl > 0 && parse_ipv4(tok, &ip) == 0)
                    servers[nsv++] = ip;
            }
            p = eol;
            if (*p == '\n')
                ++p;
        }
    }
    if (nsv == 0)
        servers[nsv++] = 0x01010101u; /* 1.1.1.1 */

    long last = -VNU_ENOENT;
    for (int i = 0; i < nsv; ++i) {
        long rc = dns_query(servers[i], name, out, 2000);
        if (rc == 0)
            return 0;
        last = rc;
    }
    return last;
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