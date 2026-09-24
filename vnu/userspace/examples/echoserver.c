/* echoserver — a tiny TCP echo server for VNU.
 *
 * Binds `PORT` (default 7778), listens, and serves one client at a
 * time: every byte received is bounced straight back until the peer
 * closes. Prints progress lines on stdout.
 *
 *   echoserver [PORT]
 *
 * From inside QEMU, run.sh forwards host loopback port 17778 to the
 * guest's 7778 (slirp hostfwd), so a host-side
 * `nc 127.0.0.1 17778` (or `exec 3<>/dev/tcp/127.0.0.1/17778`) drives
 * this tool over the real NIC. The guest's own 10.0.2.15 is NOT
 * reachable from the host on a plain slirp user network.
 */
#include <vlibc/socket.h>
#include <vlibc/string.h>
#include <vlibc/unistd.h>
#include <stdint.h>

static void w(const char* s) { write(1, s, strlen(s)); }
static void we(const char* s) { write(2, s, strlen(s)); }

static void put_uint(unsigned long v)
{
    char digits[12];
    int n = 0;
    if (v == 0) {
        w("0");
        return;
    }
    while (v > 0 && n < 12) {
        digits[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int i = n - 1; i >= 0; --i) {
        char c = digits[i];
        write(1, &c, 1);
    }
}

static void print_ip(unsigned long ip)
{
    put_uint((ip >> 24) & 0xFF);
    w(".");
    put_uint((ip >> 16) & 0xFF);
    w(".");
    put_uint((ip >> 8) & 0xFF);
    w(".");
    put_uint(ip & 0xFF);
}

static long parse_port(const char* s)
{
    long v = 0;
    if (!*s)
        return -1;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9')
            return -1;
        v = v * 10 + (long)(*s - '0');
        if (v > 65535)
            return -1;
    }
    return v ? v : -1;
}

int main(int argc, char** argv)
{
    long port = 7778;
    if (argc >= 2) {
        port = parse_port(argv[1]);
        if (port < 0) {
            we("echoserver: bad port (1..65535)\n");
            return 2;
        }
    }

    int s = vnu_socket(VNU_AF_INET, VNU_SOCK_STREAM);
    if (s < 0) {
        we("echoserver: socket failed\n");
        return 1;
    }
    if (vnu_bind(s, (uint16_t)port) < 0) {
        we("echoserver: bind failed (port in use?)\n");
        return 1;
    }
    if (vnu_listen(s, 4) < 0) {
        we("echoserver: listen failed\n");
        return 1;
    }

    w("echoserver: listening on 0.0.0.0:");
    put_uint((unsigned long)port);
    w("\n");

    for (;;) {
        uint32_t rip = 0;
        uint16_t rport = 0;
        long c = vnu_accept(s, &rip, &rport, 20000);
        if (c < 0) {
            we("echoserver: accept failed (no connection for 20s)\n");
            break;
        }
        w("echoserver: accepted ");
        print_ip((unsigned long)rip);
        w(":");
        put_uint((unsigned long)rport);
        w("\n");

        for (;;) {
            char buf[512];
            long n = vnu_recv((int)c, buf, sizeof(buf), 5000);
            if (n < 0) {
                we("echoserver: recv error, closing client\n");
                break;
            }
            if (n == 0)
                break; /* peer closed */
            if (vnu_send((int)c, buf, (uint32_t)n, 5000) != n) {
                we("echoserver: send error, closing client\n");
                break;
            }
        }
        vnu_netclose((int)c);
        w("echoserver: closed\n");
    }

    vnu_netclose(s);
    return 0;
}