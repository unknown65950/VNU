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

// Probes the PCI bus for an Intel 82540EM e1000 (QEMU's default NIC),
// maps its MMIO BAR, sets up DMA descriptor rings and brings the link
// up. Call once during kernel init. Returns 0 or -VNU_EIO.
int init();

// Monotonic milliseconds since init() started a PIT-based clock.
uint32_t uptime_ms();

// Round-trip time to `ip` (big-endian uint32) in milliseconds, or a
// negative errno: -VNU_EIO (no NIC), -VNU_EHOSTUNREACH (no ARP reply),
// -VNU_ETIMEDOUT (no ICMP reply in time). Ping to OUR_IP returns 0.
long ping(uint32_t ip, uint32_t timeout_ms);

void get_info(Info* out);

} // namespace vnu::net