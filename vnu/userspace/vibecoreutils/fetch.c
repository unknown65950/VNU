/* fetch — fetch a URL over HTTP or HTTPS, print the response body.
 *
 *   fetch [-o FILE] URL
 *
 * Schemes http:// and https:// (TLS 1.2 through vlibc's TLS client,
 * with SNI from the host name). A dotted-quad host is used as-is;
 * anything else resolves through /etc/hosts first, then DNS
 * (nameservers from /etc/resolv.conf, default 1.1.1.1). Up to five
 * redirects are followed. The request is HTTP/1.0 with
 * Connection: close, so the body runs to EOF and no chunked decoding
 * is needed. The body goes to standard output (or FILE with -o);
 * anything else, including the status line, goes to standard error.
 */
#include "cu.h"
#include <vlibc/socket.h>
#include <vlibc/netdb.h>
#include <vlibc/tls.h>
#include <vlibc/time.h>

#define URL_CAP 256
#define HOST_CAP 128
#define PATH_CAP 192
#define REQ_CAP 512
#define XFER_CAP 2048
#define HDR_CAP 4096
#define MAX_REDIRECTS 5
#define TIMEOUT_MS 15000

static int g_sock = -1;
static tls_conn* g_tls = 0;

/* Demo-grade xorshift32 (as in tlsdemo): the handshake needs
 * random-looking bytes, not cryptographic randomness. Seeded from the
 * clock so two runs differ. */
static unsigned long g_prng = 0x2545f491ul;

static void prng_seed(unsigned long v)
{
    g_prng = v ^ 0x9e3779b9ul;
    if (!g_prng)
        g_prng = 0x1234abcdul;
}

static int t_rng(void* ctx, void* buf, unsigned int len)
{
    unsigned char* p = (unsigned char*)buf;
    unsigned int i;
    (void)ctx;
    for (i = 0; i < len; ++i) {
        g_prng ^= g_prng << 13;
        g_prng ^= g_prng >> 17;
        g_prng ^= g_prng << 5;
        p[i] = (unsigned char)(g_prng >> 24);
    }
    return 0;
}

static int t_send(void* ctx, const void* data, unsigned int len,
                  unsigned int to)
{
    (void)ctx;
    return vnu_send(g_sock, data, (uint32_t)len, (uint32_t)to);
}

static int t_recv(void* ctx, void* buf, unsigned int len, unsigned int to)
{
    (void)ctx;
    return vnu_recv(g_sock, buf, (uint32_t)len, (uint32_t)to);
}

/* Transport shims: plain TCP, or TLS over it once the handshake ran. */
static int xsend(const void* buf, unsigned len)
{
    if (g_tls)
        return tls_write(g_tls, buf, (uint32_t)len, TIMEOUT_MS);
    return vnu_send(g_sock, buf, (uint32_t)len, TIMEOUT_MS);
}

static int xrecv(void* buf, unsigned len)
{
    if (g_tls)
        return tls_read(g_tls, buf, (uint32_t)len, TIMEOUT_MS);
    return vnu_recv(g_sock, buf, (uint32_t)len, TIMEOUT_MS);
}

static void close_conn(void)
{
    if (g_tls) {
        tls_close(g_tls);
        g_tls = 0;
    }
    if (g_sock >= 0) {
        vnu_netclose(g_sock);
        g_sock = -1;
    }
}

static int digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Parse "a.b.c.d" into a big-endian uint32. Returns 0 on success. */
static int parse_ip(const char* s, unsigned long* out)
{
    unsigned long oct[4];
    int n = 0;
    unsigned long v = 0;
    int have = 0;
    const char* p;
    for (p = s;; ++p) {
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
            oct[3] = v;
            *out = (oct[0] << 24) | (oct[1] << 16) | (oct[2] << 8) | oct[3];
            return 0;
        } else {
            return -1;
        }
    }
}

static int starts_with(const char* s, const char* pre)
{
    while (*pre) {
        if (*s != *pre)
            return 0;
        ++s;
        ++pre;
    }
    return 1;
}

/* Split URL into host, port, path and scheme. Returns 0 on success,
 * -1 on a malformed URL. Path always starts with '/'; the fragment
 * (anything from '#') is dropped, it is never sent. */
static int parse_url(const char* url, char* host, int* port, int* secure,
                     char* path)
{
    const char* p;
    int i;
    unsigned long portv;
    if (starts_with(url, "https://")) {
        *secure = 1;
        *port = 443;
        p = url + 8;
    } else if (starts_with(url, "http://")) {
        *secure = 0;
        *port = 80;
        p = url + 7;
    } else {
        return -1;
    }
    i = 0;
    while (*p && *p != ':' && *p != '/' && i < HOST_CAP - 1)
        host[i++] = *p++;
    host[i] = 0;
    if (i == 0)
        return -1;
    if (*p == ':') {
        ++p;
        portv = 0;
        if (!digit(*p))
            return -1;
        while (digit(*p)) {
            portv = portv * 10 + (unsigned long)(*p - '0');
            if (portv > 65535)
                return -1;
            ++p;
        }
        *port = (int)portv;
    }
    i = 0;
    if (*p != '/') {
        path[i++] = '/';
    } else {
        while (*p && *p != '#' && i < PATH_CAP - 1)
            path[i++] = *p++;
    }
    path[i] = 0;
    return 0;
}

/* Resolve a host: dotted quads directly, names through gethostbyname
 * (/etc/hosts, then DNS). Sets *is_ip for the SNI decision. Returns 0
 * with *ip_be set, else -1. */
static int resolve_host(const char* host, unsigned long* ip_be, int* is_ip)
{
    uint32_t ip;
    if (parse_ip(host, ip_be) == 0) {
        *is_ip = 1;
        return 0;
    }
    *is_ip = 0;
    if (gethostbyname(host, &ip) == 0) {
        *ip_be = (unsigned long)ip;
        return 0;
    }
    return -1;
}

static int send_all(const void* buf, unsigned len)
{
    const char* p = (const char*)buf;
    while (len > 0) {
        int n = xsend(p, len);
        if (n <= 0)
            return -1;
        p += n;
        len -= (unsigned)n;
    }
    return 0;
}

/* Read until the header terminator "\r\n\r\n" is in buf. Returns the
 * offset just past it with the total bytes read in *total_out, or -1
 * on EOF, error or an overlong header. */
static int read_headers(char* buf, int* total_out)
{
    int n = 0;
    int i;
    for (;;) {
        int r;
        if (n >= HDR_CAP)
            return -1;
        r = xrecv(buf + n, (unsigned)(HDR_CAP - n));
        if (r <= 0)
            return -1;
        n += r;
        for (i = 0; i + 4 <= n; ++i) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                *total_out = n;
                return i + 4;
            }
        }
    }
}

/* The response status: the number after "HTTP/x.y ". -1 when the
 * status line does not parse. */
static int parse_status(const char* hdr)
{
    const char* p = hdr;
    int code;
    while (*p && *p != ' ')
        ++p;
    if (*p != ' ')
        return -1;
    ++p;
    if (!digit(p[0]) || !digit(p[1]) || !digit(p[2]))
        return -1;
    code = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
    return code;
}

static int lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* Value of the Location: header, if any. Returns 1 with the value in
 * out (no trailing CR), else 0. */
static int find_location(const char* hdr, int hdr_len, char* out, int cap)
{
    int i = 0;
    int k;
    /* Skip the status line. */
    while (i < hdr_len && hdr[i] != '\n')
        ++i;
    if (i < hdr_len)
        ++i;
    while (i < hdr_len) {
        int eol = i;
        int v;
        while (eol < hdr_len && hdr[eol] != '\n')
            ++eol;
        /* Empty line: end of headers (the terminator itself). */
        if ((eol == i) || (eol == i + 1 && hdr[i] == '\r'))
            break;
        if (lower(hdr[i]) == 'l' && eol - i > 9 &&
            lower(hdr[i + 1]) == 'o' && lower(hdr[i + 2]) == 'c' &&
            lower(hdr[i + 3]) == 'a' && lower(hdr[i + 4]) == 't' &&
            lower(hdr[i + 5]) == 'i' && lower(hdr[i + 6]) == 'o' &&
            lower(hdr[i + 7]) == 'n' && hdr[i + 8] == ':') {
            v = i + 9;
            while (v < eol && (hdr[v] == ' ' || hdr[v] == '\t'))
                ++v;
            k = 0;
            while (v < eol && hdr[v] != '\r' && k < cap - 1)
                out[k++] = hdr[v++];
            out[k] = 0;
            return 1;
        }
        i = eol + 1;
    }
    return 0;
}

/* Stream the body to fd: first the bytes already read past the
 * headers, then until EOF. Returns 0 at a clean EOF, -1 on error. */
static int stream_body(int fd, const char* first, int first_len)
{
    static char chunk[XFER_CAP];
    int n;
    if (first_len > 0) {
        if (write(fd, first, (unsigned long)first_len) != (long)first_len)
            return -1;
    }
    for (;;) {
        n = xrecv(chunk, XFER_CAP);
        if (n == 0)
            return 0;
        if (n < 0)
            return -1;
        if (write(fd, chunk, (unsigned long)n) != (long)n)
            return -1;
    }
}

/* Build "GET path HTTP/1.0" with Host and Connection: close. The port
 * rides along in Host: only off the scheme default. */
static int build_request(char* out, const char* host, int port, int secure,
                         const char* path)
{
    int n = 0;
    const char* p;
    int defport = secure ? 443 : 80;
    int i;
    unsigned long pv = (unsigned long)port;
    char digs[6];
    int dl = 0;
    /* "GET " */
    out[n++] = 'G';
    out[n++] = 'E';
    out[n++] = 'T';
    out[n++] = ' ';
    for (p = path; *p; ++p) {
        if (n >= REQ_CAP - 1)
            return -1;
        out[n++] = *p;
    }
    p = " HTTP/1.0\r\nHost: ";
    while (*p) {
        if (n >= REQ_CAP - 1)
            return -1;
        out[n++] = *p++;
    }
    for (p = host; *p; ++p) {
        if (n >= REQ_CAP - 1)
            return -1;
        out[n++] = *p;
    }
    if (port != defport) {
        if (pv == 0)
            return -1;
        do {
            digs[dl++] = (char)('0' + pv % 10);
            pv /= 10;
        } while (pv && dl < 6);
        if (n >= REQ_CAP - 1)
            return -1;
        out[n++] = ':';
        for (i = dl - 1; i >= 0; --i) {
            if (n >= REQ_CAP - 1)
                return -1;
            out[n++] = digs[i];
        }
    }
    p = "\r\nConnection: close\r\n\r\n";
    while (*p) {
        if (n >= REQ_CAP - 1)
            return -1;
        out[n++] = *p++;
    }
    out[n] = 0;
    return n;
}

/* Turn a redirect Location into the next URL: absolute http(s) URLs
 * as-is, absolute paths against the current origin, anything else
 * against the current directory. Returns 0 on success. */
static int next_url(const char* loc, const char* scheme, const char* host,
                    int port, int secure, const char* path, char* out)
{
    int n = 0;
    const char* p;
    int i;
    if (starts_with(loc, "http://") || starts_with(loc, "https://")) {
        for (p = loc; *p && n < URL_CAP - 1; ++p)
            out[n++] = *p;
        out[n] = 0;
        return 0;
    }
    for (p = scheme; *p && n < URL_CAP - 1; ++p)
        out[n++] = *p;
    for (p = host; *p && n < URL_CAP - 1; ++p)
        out[n++] = *p;
    if ((secure && port != 443) || (!secure && port != 80)) {
        /* Port digits without snprintf. */
        unsigned long pv = (unsigned long)port;
        char digs[6];
        int dl = 0;
        if (n >= URL_CAP - 1)
            return -1;
        out[n++] = ':';
        do {
            digs[dl++] = (char)('0' + pv % 10);
            pv /= 10;
        } while (pv && dl < 6);
        for (i = dl - 1; i >= 0; --i) {
            if (n >= URL_CAP - 1)
                return -1;
            out[n++] = digs[i];
        }
    }
    if (loc[0] == '/') {
        p = loc;
    } else {
        /* Relative to the current directory. */
        int dir = 0;
        i = 0;
        while (path[i]) {
            if (path[i] == '/')
                dir = i;
            ++i;
        }
        if (n >= URL_CAP - 1)
            return -1;
        out[n++] = '/';
        for (i = 1; i <= dir && n < URL_CAP - 1; ++i)
            out[n++] = path[i];
        p = loc;
    }
    while (*p && *p != '#' && n < URL_CAP - 1)
        out[n++] = *p++;
    out[n] = 0;
    return 0;
}

static void usage(void)
{
    we("usage: fetch [-o FILE] URL\n");
}

int main(int argc, char** argv)
{
    static char url[URL_CAP];
    static char host[HOST_CAP];
    static char path[PATH_CAP];
    static char req[REQ_CAP];
    static char hdr[HDR_CAP + 1];
    static char loc[URL_CAP];
    static char next[URL_CAP];
    const char* outfile = 0;
    const char* urlarg = 0;
    int port;
    int secure;
    int is_ip;
    int out_fd = 1;
    int hops;
    int a;
    unsigned long ip;
    long sk;
    long rc;
    /* per-hop state, declared up here for the small compiler */
    int rlen;
    int hlen;
    int total;
    int status;
    tls_stream ts;

    for (a = 1; a < argc; ++a) {
        if (argv[a][0] == '-' && argv[a][1] == 'o' && argv[a][2] == 0) {
            if (++a >= argc) {
                usage();
                return 1;
            }
            outfile = argv[a];
        } else if (argv[a][0] == '-' && argv[a][1] == '-' &&
                   argv[a][2] == 'h') {
            usage();
            return 0;
        } else if (argv[a][0] == '-' && argv[a][1] == 0) {
            usage();
            return 1;
        } else if (!urlarg) {
            urlarg = argv[a];
        } else {
            usage();
            return 1;
        }
    }
    if (!urlarg) {
        usage();
        return 1;
    }
    {
        int i = 0;
        while (urlarg[i] && i < URL_CAP - 1) {
            url[i] = urlarg[i];
            ++i;
        }
        url[i] = 0;
        if (urlarg[i]) {
            we("fetch: URL too long\n");
            return 1;
        }
    }
    if (outfile) {
        out_fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out_fd < 0) {
            we("fetch: cannot write ");
            we(outfile);
            we("\n");
            return 1;
        }
    }
    prng_seed((unsigned long)time(0));

    hops = 0;
    for (;;) {
        if (parse_url(url, host, &port, &secure, path) != 0) {
            we("fetch: bad URL (want http:// or https://)\n");
            close_conn();
            return 1;
        }
        if (resolve_host(host, &ip, &is_ip) != 0) {
            we("fetch: cannot resolve ");
            we(host);
            we("\n");
            close_conn();
            return 1;
        }
        sk = vnu_socket(VNU_AF_INET, VNU_SOCK_STREAM);
        if (sk < 0) {
            we("fetch: no socket\n");
            close_conn();
            return 1;
        }
        g_sock = (int)sk;
        rc = vnu_connect(g_sock, (uint32_t)ip, (uint16_t)port, TIMEOUT_MS);
        if (rc < 0) {
            we("fetch: cannot connect\n");
            close_conn();
            return 1;
        }
        if (secure) {
            memset(&ts, 0, sizeof ts);
            ts.ctx = 0;
            ts.send_fn = t_send;
            ts.recv_fn = t_recv;
            ts.rng_fn = t_rng;
            /* No SNI for a numeric address: it names no DNS name. */
            g_tls = tls_connect(&ts, is_ip ? 0 : host, 20000);
            if (!g_tls) {
                we("fetch: TLS handshake failed: ");
                we(tls_last_error());
                we("\n");
                close_conn();
                return 1;
            }
        }
        rlen = build_request(req, host, port, secure, path);
        if (rlen < 0) {
            we("fetch: request too long\n");
            close_conn();
            return 1;
        }
        if (send_all(req, (unsigned)rlen) != 0) {
            we("fetch: send failed\n");
            close_conn();
            return 1;
        }
        hlen = read_headers(hdr, &total);
        if (hlen < 0) {
            we("fetch: bad or truncated response\n");
            close_conn();
            return 1;
        }
        /* NUL-terminate after the last byte read, not at the end of the
         * header: bytes between hlen and total are the start of the
         * body and are streamed out below, so terminating at hlen
         * would swallow the first body byte. */
        hdr[total] = 0;
        status = parse_status(hdr);
        if ((status == 301 || status == 302 || status == 303 ||
             status == 307 || status == 308) &&
            hops < MAX_REDIRECTS &&
            find_location(hdr, hlen, loc, URL_CAP)) {
            if (next_url(loc, secure ? "https://" : "http://", host, port,
                         secure, path, next) != 0) {
                we("fetch: bad redirect\n");
                close_conn();
                return 1;
            }
            {
                int i = 0;
                while (next[i] && i < URL_CAP - 1) {
                    url[i] = next[i];
                    ++i;
                }
                url[i] = 0;
            }
            ++hops;
            close_conn();
            continue;
        }
        if (status < 200 || status > 299) {
            we("fetch: HTTP status ");
            {
                char code[4];
                int v = status < 0 ? 0 : status;
                code[0] = (char)('0' + (v / 100) % 10);
                code[1] = (char)('0' + (v / 10) % 10);
                code[2] = (char)('0' + v % 10);
                code[3] = 0;
                we(code);
            }
            we("\n");
        }
        /* Body bytes already past the headers come first; the rest
         * streams until the server closes. */
        if (stream_body(out_fd, hdr + hlen, total - hlen) != 0) {
            we("fetch: body truncated\n");
            close_conn();
            return 1;
        }
        close_conn();
        return 0;
    }
}
