#pragma once
#include <stdint.h>

namespace vnu::net {

/* Addresses on QEMU's slirp user network (the default -netdev user on a
 * plain `qemu-system-i386`). IPs are big-endian uint32s, i.e. value
 * 0x0A00020F is the dotted-quad "10.0.2.15". */
constexpr uint32_t OUR_IP = 0x0A00020Fu;   /* 10.0.2.15  (guest) */
constexpr uint32_t OUR_MASK = 0xFFFFFF00u; /* 255.255.255.0 */
constexpr uint32_t GW_IP = 0x0A000202u;    /* 10.0.2.2 */
constexpr uint32_t BROADCAST_IP = 0x0A0002FFu;

struct Info {
    uint8_t mac[6];
    uint32_t ip;    /* big-endian */
    uint32_t mask;  /* big-endian */
    uint32_t gw;    /* big-endian */
    uint8_t up;     /* 1 when the NIC is initialized */
};

/* What has crossed the wire since boot, for /proc/net/dev.
 *
 * Counted at the driver, where every frame is accounted exactly once:
 * a receive when the ring hands one over, a transmit when the TX tail
 * is handed to the NIC. Bytes are frame lengths off the wire, headers
 * and FCS included, and they include the ARP and ICMP traffic the stack
 * produces on its own — this is the interface's traffic, not any one
 * program's. A monitor reads it twice and divides, which is why these
 * are monotonic counters and not a rate. */
struct Stats {
    uint32_t rx_bytes;
    uint32_t rx_packets;
    uint32_t tx_bytes;
    uint32_t tx_packets;
};

// Probes the PCI bus for an Intel 82540EM e1000 (QEMU's default NIC),
// maps its MMIO BAR, sets up DMA descriptor rings and brings the link
// up. Call once during kernel init. Returns 0 or -VNU_EIO.
int init();

// Monotonic milliseconds since init() started a PIT-based clock.
uint32_t uptime_ms();

// Round-trip time to `ip` (big-endian uint32) in milliseconds, or a
// negative errno: -VNU_EIO (no NIC), -VNU_EHOSTUNREACH (no ARP reply,
// via the gateway for off-subnet targets), -VNU_ETIMEDOUT (no ICMP
// reply in time). Ping to OUR_IP returns 0.
long ping(uint32_t ip, uint32_t timeout_ms);

// DNS A-record lookup of `name` against `server` (big-endian IPv4).
// Returns 0 with *out big-endian on success, else -VNU_ENOENT,
// -VNU_EIO, -VNU_EHOSTUNREACH or -VNU_ETIMEDOUT.
long dns_query(uint32_t server, const char* name, uint32_t* out,
               uint32_t timeout_ms);

// Resolve `name`: /etc/hosts mapping first, then DNS A queries to the
// servers listed in /etc/resolv.conf (default 1.1.1.1). Returns 0 with
// *out big-endian, or a negative errno (same set as dns_query).
long resolve_host(const char* name, uint32_t* out);

void get_info(Info* out);

/* Fills `out` with the interface counters. Zeroes it and returns when
 * there is no NIC, so a reader never sees stale numbers from a machine
 * whose driver did not come up. */
void get_stats(Stats* out);

} // namespace vnu::net
