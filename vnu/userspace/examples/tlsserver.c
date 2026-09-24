/* tlsserver — a tiny TLS 1.2 server demo for VNU.
 *
 * Binds `PORT` (default 7779), listens, and serves one client at a
 * time: performs the vlibc TLS handshake (cipher suite
 * TLS_RSA_WITH_AES_128_GCM_SHA256) through the tls_stream callbacks,
 * then echoes every byte it receives straight back until the peer
 * closes. The certificate + RSA key are the fixed demo keypair in
 * tlsserver_key.h (self-signed, DEMO ONLY — the private key is public).
 *
 *   tlsserver [PORT]
 *
 * Test vectors:
 *   - inside the guest: `tlsserver 7779` + `tlsdemo 127.0.0.1 7779`
 *     (the kernel now loopbacks 127.0.0.1, so the vlibc client talks to
 *     the vlibc server with no NIC involved);
 *   - from the host: run.sh forwards host loopback port 17779 to the
 *     guest's 7779, so `printf 'ping\n' | openssl s_client -quiet \
 *     -tls1_2 -cipher AES128-GCM-SHA256 -connect 127.0.0.1:17779`
 *     drives it over the real NIC (slirp hostfwd).
 */
#include <vlibc/tls.h>
#include <vlibc/socket.h>
#include <vlibc/string.h>
#include <vlibc/unistd.h>
#include <stdint.h>
#include "tlsserver_key.h"

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

/* Deterministic xorshift32 PRNG (demo only — same note as tlsdemo). */
static uint32_t g_prng = 0x9e3779b9u;
static void prng_seed(unsigned long v)
{
    g_prng = (uint32_t)v ^ 0x2545f491u;
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

/* The embedded demo keypair: certificate + raw RSA-2048 integers. */
static const tls_keypair g_kp = {
    tlsserver_cert_der, tlsserver_cert_der_len,
    tlsserver_key_n, tlsserver_key_n_len,
    tlsserver_key_e, tlsserver_key_e_len,
    tlsserver_key_d, tlsserver_key_d_len,
};

/* Sanity-check the embedded keypair against its own certificate and do
 * one RSA encrypt/decrypt round trip before we trust it with handshakes. */

static int same_bytes(const uint8_t* a, const uint8_t* b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; ++i)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int key_self_check(void)
{
    uint8_t mod[TLS_RSA_MAX_MODULUS], exp[16];
    uint32_t mlen = (uint32_t)sizeof mod;
    uint32_t elen = (uint32_t)sizeof exp;
    uint8_t msg[48], enc[TLS_RSA_MAX_MODULUS], back[TLS_RSA_MAX_MODULUS];
    int i;

    if (tls_x509_rsa_pubkey(tlsserver_cert_der, tlsserver_cert_der_len,
                            mod, &mlen, exp, &elen) != 0)
        return -1;
    if (mlen != tlsserver_key_n_len || elen != tlsserver_key_e_len)
        return -1;
    if (!same_bytes(mod, tlsserver_key_n, mlen) ||
        !same_bytes(exp, tlsserver_key_e, elen))
        return -1;

    msg[0] = 0x03;
    msg[1] = 0x03;
    for (i = 2; i < 48; ++i)
        msg[i] = (uint8_t)(i * 7u + 1u);
    if (tls_rsa_encrypt_pkcs1(tlsserver_key_n, tlsserver_key_n_len,
                              msg, 48, enc) != 0)
        return -1;
    if (tls_rsa_decrypt_pkcs1(tlsserver_key_n, tlsserver_key_n_len,
                              tlsserver_key_d, tlsserver_key_d_len,
                              enc, back) != 48)
        return -1;
    if (!same_bytes(msg, back, 48))
        return -1;
    return 0;
}

int main(int argc, char** argv)
{
    long port = 7779;
    long tie = 0; /* monotonic per-connection PRNG re-seed */
    tls_stream s;

    if (argc >= 2) {
        port = parse_port(argv[1]);
        if (port < 0) {
            we("tlsserver: bad port (1..65535)\n");
            return 2;
        }
    }

    if (key_self_check() != 0) {
        we("tlsserver: embedded keypair does not match its certificate\n");
        return 1;
    }

    int sk = vnu_socket(VNU_AF_INET, VNU_SOCK_STREAM);
    if (sk < 0) {
        we("tlsserver: socket failed\n");
        return 1;
    }
    if (vnu_bind(sk, (uint16_t)port) < 0) {
        we("tlsserver: bind failed (port in use?)\n");
        return 1;
    }
    if (vnu_listen(sk, 4) < 0) {
        we("tlsserver: listen failed\n");
        return 1;
    }

    w("tlsserver: listening on 0.0.0.0:");
    put_uint((unsigned long)port);
    w("\n");

    memset(&s, 0, sizeof s);
    s.send_fn = t_send;
    s.recv_fn = t_recv;
    s.rng_fn = t_rng;

    for (;;) {
        uint32_t rip = 0;
        uint16_t rport = 0;
        long c = vnu_accept(sk, &rip, &rport, 20000);
        tls_conn* t = NULL;
        if (c < 0) {
            we("tlsserver: accept failed (no connection for 20s)\n");
            break;
        }
        g_sock = (int)c;

        w("tlsserver: accepted ");
        print_ip((unsigned long)rip);
        w(":");
        put_uint((unsigned long)rport);
        w("\n");

        prng_seed((unsigned long)port + ++tie);
        s.ctx = &g_sock;
        t = tls_accept(&s, &g_kp, 15000);
        if (!t) {
            we("tlsserver: TLS handshake failed: ");
            we(tls_last_error());
            we("\n");
            vnu_netclose((int)c);
            w("tlsserver: closed\n");
            continue;
        }
        w("tlsserver: TLS handshake OK (AES128-GCM-SHA256)\n");

        for (;;) {
            char buf[512];
            long n = tls_read(t, buf, (unsigned)sizeof(buf), 5000);
            if (n < 0) {
                we("tlsserver: tls_read error\n");
                break;
            }
            if (n == 0)
                break; /* close_notify or EOF */
            if (tls_write(t, buf, (uint32_t)n, 5000) != n) {
                we("tlsserver: tls_write error\n");
                break;
            }
        }

        tls_close(t);
        vnu_netclose((int)c);
        w("tlsserver: closed\n");
    }

    vnu_netclose(sk);
    return 0;
}