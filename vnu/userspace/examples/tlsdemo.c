/* tlsdemo — TLS 1.2 client demo for VNU.
 *
 * Connects to a TLS server (e.g. host `openssl s_server
 * -tls1_2 -cipher AES128-GCM-SHA256 -accept 14433`) via the kernel's
 * TCP socket syscalls, runs the vlibc TLS handshake
 * (TLS_RSA_WITH_AES_128_GCM_SHA256) through the tls_stream callbacks,
 * sends one GET request and prints the response. From inside QEMU the
 * host is reachable at 10.0.2.2 (slirp user networking).
 */
#include <vlibc/tls.h>
#include <vlibc/socket.h>
#include <vlibc/stdio.h>
#include <vlibc/stdlib.h>
#include <vlibc/string.h>
#include <vlibc/unistd.h>
#include <stdint.h>

static int g_sock = -1;

static int t_send(void* ctx, const void* data, unsigned int len, unsigned int to)
{
    (void)ctx;
    return vnu_send(g_sock, data, (uint32_t)len, (uint32_t)to);
}

static int t_recv(void* ctx, void* buf, unsigned int len, unsigned int to)
{
    (void)ctx;
    return vnu_recv(g_sock, buf, (uint32_t)len, (uint32_t)to);
}

/* Deterministic xorshift32 PRNG (demo only: the handshake needs *some*
 * random-looking bytes for the client random and the RSA pre-master).
 * Seeded at startup so every connection differs. */
static uint32_t g_prng = 0x2545f491u;
static void prng_seed(unsigned long v)
{
    g_prng = (uint32_t)v ^ 0x9e3779b9u;
    if (!g_prng)
        g_prng = 0x1234abcd;
}
static int t_rng(void* ctx, void* buf, unsigned int len)
{
    uint8_t* p = (uint8_t*)buf;
    unsigned i;
    (void)ctx;
    for (i = 0; i < len; ++i) {
        g_prng ^= g_prng << 13;
        g_prng ^= g_prng >> 17;
        g_prng ^= g_prng << 5;
        p[i] = (uint8_t)(g_prng >> 24);
    }
    return 0;
}

static long parse_ip(const char* s, unsigned long* out)
{
    unsigned long oct[4];
    int n = 0;
    unsigned long v = 0;
    int have = 0;
    const char* p;
    for (p = s;; ++p) {
        if (*p >= '0' && *p <= '9') {
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
            oct[3] = v;
            *out = (oct[0] << 24) | (oct[1] << 16) | (oct[2] << 8) | oct[3];
            return 0;
        } else {
            return -1;
        }
    }
}

int main(int argc, char** argv)
{
    unsigned long ip;
    long port;
    long tmo;
    const char* sni = "localhost";
    tls_stream s;
    tls_conn* c;
    long rc;

    if (argc < 3 || parse_ip(argv[1], &ip) != 0) {
        puts("usage: tlsdemo IP PORT [TIME_MS] [SNI]");
        return 1;
    }
    port = atol(argv[2]);
    tmo = argc >= 4 ? atol(argv[3]) : 15000;
    if (argc >= 5)
        sni = argv[4];
    prng_seed((unsigned long)port);

    long sk = vnu_socket(VNU_AF_INET, VNU_SOCK_STREAM);
    if (sk < 0) {
        printf("socket: %ld\n", sk);
        return 1;
    }
    g_sock = (int)sk;
    rc = vnu_connect(g_sock, (uint32_t)ip, (uint16_t)port, (uint32_t)tmo);
    if (rc < 0) {
        printf("connect: %ld\n", rc);
        vnu_netclose(g_sock);
        return 1;
    }
    puts("connected, starting TLS handshake");

    memset(&s, 0, sizeof s);
    s.ctx = &g_sock;
    s.send_fn = t_send;
    s.recv_fn = t_recv;
    s.rng_fn = t_rng;

    c = tls_connect(&s, sni, (uint32_t)tmo);
    if (!c) {
        printf("TLS handshake failed: %s\n", tls_last_error());
        vnu_netclose(g_sock);
        return 1;
    }
    puts("TLS handshake OK (AES128-GCM-SHA256)");

    {
        static const char req[] =
            "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        long n = tls_write(c, req, (unsigned)(sizeof(req) - 1), (uint32_t)tmo);
        if (n != (long)(sizeof(req) - 1)) {
            printf("tls_write short: %ld\n", n);
            tls_close(c);
            vnu_netclose(g_sock);
            return 1;
        }
    }

    {
        char buf[512];
        long got = 0;
        for (;;) {
            long n = tls_read(c, buf, (unsigned)sizeof(buf), (uint32_t)tmo);
            if (n < 0) {
                printf("tls_read: %s\n", tls_last_error());
                break;
            }
            if (n == 0)
                break;
            for (long i = 0; i < n; ++i)
                putchar((int)buf[i]);
            got += n;
        }
        if (got == 0)
            puts("[no data]");
    }

    tls_close(c);
    vnu_netclose(g_sock);
    return 0;
}