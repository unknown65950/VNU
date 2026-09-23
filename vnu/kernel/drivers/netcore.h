#pragma once
#include <stdint.h>

/* netcore — internal bridge between the e1000 driver and the TCP module.
 * The driver owns the NIC, the shared TX buffer, ARP and the RX path; TCP
 * builds frames straight into the driver's TX buffer and calls rx_drain()
 * from its own blocking loops (same model as ping/DNS). Not for userspace. */

namespace vnu::netcore {

bool net_up();                                   /* NIC initialized */
uint32_t our_ip();                               /* host address, big-endian */

/* Begins a frame: zeroes the TX buffer, writes dst MAC / our MAC /
 * ethertype in the Ethernet header and hands out a pointer to the IPv4
 * payload area (offset 14). */
void eth_header(uint16_t ethertype, const uint8_t* dst);
uint8_t* tx_room();                              /* returns g_tx_buf + 14 */

/* Submits a frame of `frame_len` bytes already assembled in the shared TX
 * buffer (waits for the TX ring to free a descriptor). */
bool tx_send_current(uint16_t frame_len);

int  arp_ensure(uint32_t ip, uint8_t mac[6], uint32_t timeout_ms);
void rx_drain();
uint32_t now_ms();

} // namespace vnu::netcore