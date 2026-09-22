/* ping — send ICMP echo requests and report round-trip times.
 *
 * Requires the kernel NIC (QEMU's default e1000). IPs are given as
 * dotted quads; there is no DNS yet, so names are not accepted.
 *
 *   ping IP [COUNT]
 *
 * COUNT pings with a kernel-side timeout of 300 ms each (IPv4 only).
 */
#include "cu.h"
#include <vnu/abi.h>

static int digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Parse "a.b.c.d" into a big-endian uint32 (10.0.2.2 -> 0x0A000202). */
static long parse_ip(const char* s, unsigned long* out)
{
    unsigned long oct[4];
    int n = 0;
    unsigned long v = 0;
    int have = 0;

    for (const char* p = s;; ++p) {
        if (digit(*p)) {
            v = v * 10 + (unsigned long)(*p - '0');
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

static void print_ip(unsigned long ip)
{
    put_uint(ip >> 24);
    w(".");
    put_uint((ip >> 16) & 0xFF);
    w(".");
    put_uint((ip >> 8) & 0xFF);
    w(".");
    put_uint(ip & 0xFF);
}

int main(int argc, char** argv)
{
    unsigned long count = 4;
    unsigned long ip = 0;

    if (argc < 2) {
        we("usage: ping IP [COUNT]\n");
        return 2;
    }
    if (parse_ip(argv[1], &ip) != 0) {
        we("ping: bad address; names are not supported yet (no DNS)\n");
        return 2;
    }
    if (argc >= 3) {
        count = 0;
        for (const char* p = argv[2]; *p; ++p) {
            if (!digit(*p) || count > 1000) {
                we("ping: bad count\n");
                return 2;
            }
            count = count * 10 + (unsigned long)(*p - '0');
        }
        if (count > 1000)
            count = 1000;
    }

    w("PING ");
    print_ip(ip);
    w(" (");
    print_ip(ip);
    w("): 56 data bytes\n");

    unsigned long sent = 0, answered = 0;
    for (unsigned long i = 0; i < count; ++i) {
        long rtt = syscall(VNU_SYS_ping, (unsigned long)ip, 300ul);
        ++sent;
        if (rtt >= 0) {
            ++answered;
            print_ip(ip);
            w(": icmp_seq=");
            put_uint(i);
            w(" time=");
            put_uint((unsigned long)rtt);
            w(" ms\n");
        } else {
            print_ip(ip);
            w(": ");
            if (rtt == -VNU_EIO)
                w("no NIC\n");
            else if (rtt == -VNU_EHOSTUNREACH)
                w("no ARP reply (host unreachable)\n");
            else if (rtt == -VNU_ETIMEDOUT)
                w("no ICMP reply\n");
            else
                w("error\n");
        }
    }

    w("--- ");
    print_ip(ip);
    w(" ping statistics ---\n");
    put_uint(sent);
    w(" packets transmitted, ");
    put_uint(answered);
    w(" received, ");
    put_uint(sent - answered);
    w(" lost\n");

    return answered > 0 ? 0 : 1;
}