#pragma once
#include <stdint.h>

/* Minimal POSIX-flavoured socket API on top of the kernel's TCP syscalls
 * (41..45). The kernel stack is client-only: socket/connect/send/recv/close.
 * IP addresses are big-endian uint32s (10.0.2.2 = 0x0A000202); ports are
 * host byte order. Socket handles are small integers, not VFS fds.
 *
 * Returns 0 or a negative VNU errno. */

#define VNU_AF_INET 2
#define VNU_SOCK_STREAM 1

int vnu_socket(int family, int type);
int vnu_connect(int sock, uint32_t ip_be, uint16_t port, uint32_t timeout_ms);
int vnu_send(int sock, const void* buf, uint32_t len, uint32_t timeout_ms);
int vnu_recv(int sock, void* buf, uint32_t len, uint32_t timeout_ms);
int vnu_netclose(int sock);