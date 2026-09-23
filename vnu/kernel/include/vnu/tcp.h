#pragma once
#include <stdint.h>

/* vnu::tcp — a small client-side TCP/IPv4 stack for the kernel.
 * Lives beside the e1000 driver (vnu/kernel/drivers/) and feeds off its
 * netcore bridge. Only client connections: socket/connect/send/recv/close.
 * All operations block (busy-wait on the RX path) exactly like ping/DNS,
 * with retransmission and window handling inside the block.
 *
 * Socket handles are small integers from a fixed 8-slot table; they are
 * NOT VFS file descriptors. IPs are big-endian, ports host byte order. */

namespace vnu::tcp {

/* Called from the driver's RX path for every IPv4/TCP frame addressed to
 * us. `frame` points at the Ethernet header, `frame_len` is the L2 length
 * and `ip_len` the IPv4 total-length field (20 <= ip_len <= frame_len-14). */
void input_packet(uint32_t src_ip, const uint8_t* frame, uint16_t frame_len,
                  uint16_t ip_len);

/* socket(): validate family/type and grab a free slot. */
int socket_open(int family, int type);

/* connect(): resolve ARP, run the handshake, block until ESTABLISHED. */
long socket_connect(int sock, uint32_t ip_be, uint16_t port,
                    uint32_t timeout_ms);

/* send(): buffer all bytes, push them onto the wire, return len. */
long socket_send(int sock, const void* buf, uint32_t len, uint32_t timeout_ms);

/* recv(): return up to len in-order bytes, 0 on FIN, -errno on failure. */
long socket_recv(int sock, void* buf, uint32_t len, uint32_t timeout_ms);

/* close(): FIN if established, else drop. Frees the slot. */
long socket_close(int sock);

} // namespace vnu::tcp