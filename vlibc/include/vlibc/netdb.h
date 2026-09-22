#pragma once
#include <stdint.h>

// Resolve a host name to a big-endian IPv4 address (the byte order the
// kernel `ping`/`resolve` ABI uses; 10.0.2.2 = 0x0A000202).
//
// Returns 0 on success with *ip_be filled, or a negative VNU errno:
// -VNU_ENOENT (unknown name), -VNU_EIO (no NIC), -VNU_EHOSTUNREACH,
// -VNU_ETIMEDOUT. The lookup checks /etc/hosts first, then DNS through
// the nameservers in /etc/resolv.conf (default server 1.1.1.1).
int gethostbyname(const char* name, uint32_t* ip_be);