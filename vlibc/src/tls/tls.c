/*
 * tls.c — TLS 1.2 client handshake and record layer for VNU.
 *
 * Cipher suite: TLS_RSA_WITH_AES_128_GCM_SHA256 (0x009c), TLS 1.2 only.
 * Transport goes through the tls_stream callbacks (send/recv/rng), so
 * this code is POSIX-portable and runs unchanged on a host OS — that is
 * how it is tested (qemu guest <-> host harness <-> openssl s_server).
 *
 * References: RFC 5246 (handshake, PRF, master secret, record layer),
 * RFC 5288 (AES-GCM wire format: 12-byte nonce = write_IV[4] ||
 * nonce_explicit[8] carried in each record; AAD = seq || type ||
 * version || plaintext length). Each direction restarts its record
 * sequence at 0 when the cipher state changes at ChangeCipherSpec
 * (verified against OpenSSL's record layer): on write we put the
 * 64-bit sequence number in the explicit-nonce field, on read we take
 * the peer's explicit bytes from the wire (OpenSSL fills them with a
 * random per-direction counter, not the sequence number).
 * Certificate verification is limited to extracting the leaf
 * RSA public key and matching SNI where possible in tls_x509.c.
 */
#include <vlibc/tls.h>
#include <vlibc/string.h>
#include <vlibc/stdlib.h>

#define TLS_VERSION 0x0303u
#define MAX_PLAIN   (1u << 14)                /* 16384 */
#define AES_EXPLICIT 8u                       /* nonce_explicit length */
#define MAX_RECORD_RX (MAX_PLAIN + 256u + 16u)

#define CT_CCS       0x14u
#define CT_ALERT     0x15u
#define CT_HANDSHAKE 0x16u
#define CT_APP       0x17u

#define HS_CLIENT_HELLO         1
#define HS_SERVER_HELLO         2
#define HS_CERTIFICATE         11
#define HS_SERVER_KEY_EXCHANGE 12
#define HS_CERT_REQUEST        13
#define HS_SERVER_HELLO_DONE   14
#define HS_CLIENT_KEY_EXCHANGE 16
#define HS_FINISHED            20

#define ALERT_CLOSE_NOTIFY  0
#define ALERT_HANDSHAKE_FAIL 40
#define ALERT_ILLEGAL_PARAM  47
#define ALERT_DECODE_ERROR   50
#define ALERT_BAD_RECORD_MAC 20

#define SUITE 0x009cu   /* TLS_RSA_WITH_AES_128_GCM_SHA256 */

struct tls_conn {
    tls_stream s;
    char  err[96];

    uint8_t* hs;             /* transcript (wire form, all messages) */
    size_t   hs_len, hs_cap;
    uint8_t* rx;             /* received handshake bytes not yet parsed */
    size_t   rx_len, rx_cap;

    uint8_t crandom[32], srandom[32];
    uint8_t master[48];
    uint8_t ckey[16], skey[16], civ[4], siv[4];
    uint64_t seq_w, seq_r;

    uint8_t* inbuf;          /* decrypted plaintext awaiting tls_read */
    size_t   in_len, in_off, in_cap;

    uint8_t  peer_mod[TLS_RSA_MAX_MODULUS];
    uint32_t peer_modlen;

    int stage;
    int closed;
    int write_enc;       /* 0 until our ChangeCipherSpec: plaintext records */
    int read_enc;        /* 0 until the server's ChangeCipherSpec */
};

enum {
    ST_INIT = 0,
    ST_AWAIT_SHD,       /* ClientHello sent */
    ST_FLIGHT_DONE,     /* ServerHelloDone consumed */
    ST_ESTABLISHED
};

static char g_last_error[96] = "no error";

static void seterr(tls_conn* c, const char* m)
{
    size_t n = 0, i;
    while (m[n] && n + 1 < sizeof g_last_error)
        ++n;
    for (i = 0; i < n; ++i) {
        g_last_error[i] = m[i];
        if (c) c->err[i] = m[i];
    }
    g_last_error[n] = 0;
    if (c) c->err[n] = 0;
}

static void seterr_global(const char* m)
{
    size_t i, n = 0;
    while (m[n] && n + 1 < sizeof g_last_error)
        ++n;
    for (i = 0; i < n; ++i)
        g_last_error[i] = m[i];
    g_last_error[n] = 0;
}

/* Map a plaintext alert description to a diagnostic error. */
static void alert_err(tls_conn* c, uint8_t desc)
{
    switch (desc) {
    case ALERT_BAD_RECORD_MAC: seterr(c, "peer: bad record mac"); break;
    case ALERT_HANDSHAKE_FAIL: seterr(c, "peer: handshake failure"); break;
    case ALERT_ILLEGAL_PARAM:  seterr(c, "peer: illegal parameter"); break;
    case ALERT_DECODE_ERROR:   seterr(c, "peer: decode error"); break;
    case ALERT_CLOSE_NOTIFY:   seterr(c, "peer closed"); break;
    default:                   seterr(c, "peer: fatal alert"); break;
    }
}

const char* tls_last_error(void)
{
    return g_last_error;
}

/* ------------------------------------------------------------------ */
/* primitives                                                          */
/* ------------------------------------------------------------------ */

static unsigned be16(const uint8_t* p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint32_t be24(const uint8_t* p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
static void put16(uint8_t* p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put24(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}

/* constant-time equality */
static int ct_eq(const uint8_t* a, const uint8_t* b, size_t n)
{
    uint8_t d = 0;
    while (n--) d |= (uint8_t)(a[n] ^ b[n]);
    return d == 0;
}

/* vlibc lacks calloc/realloc/memmove: provide tiny stand-ins. */
static void* xcalloc(size_t n, size_t sz)
{
    void* p = malloc(n * sz);
    if (p)
        memset(p, 0, n * sz);
    return p;
}

/* grow `*base` so it can hold `need` bytes; preserve existing content. */
static int grow(void** base, size_t* cap, size_t need, tls_conn* c)
{
    uint8_t* nb;
    size_t ncap = *cap ? *cap : 4096u;
    while (ncap < need)
        ncap *= 2;
    nb = (uint8_t*)malloc(ncap);
    if (!nb) { seterr(c, "out of memory"); return -1; }
    if (*base) {
        memcpy(nb, *base, *cap);
        free(*base);
    }
    *base = nb;
    *cap = ncap;
    return 0;
}

/* shift `buf` left by `off` bytes (dest first; ascending byte copy). */
static void shift_left(uint8_t* buf, size_t off, size_t len)
{
    size_t i;
    for (i = 0; i + off < len; ++i)
        buf[i] = buf[i + off];
}

static int send_all(tls_conn* c, const uint8_t* p, size_t n, uint32_t to)
{
    while (n > 0) {
        int r = c->s.send_fn(c->s.ctx, p, (uint32_t)n, to);
        if (r <= 0) { seterr(c, "transport error"); return -1; }
        p += (size_t)r; n -= (size_t)r;
    }
    return 0;
}

static int recv_all(tls_conn* c, uint8_t* p, size_t n, uint32_t to)
{
    while (n > 0) {
        int r = c->s.recv_fn(c->s.ctx, p, (uint32_t)n, to);
        if (r <= 0) { seterr(c, "transport error"); return -1; }
        p += (size_t)r; n -= (size_t)r;
    }
    return 0;
}

static int want_rng(tls_conn* c, uint8_t* p, size_t n)
{
    if (!c->s.rng_fn) { seterr(c, "no randomness source"); return -1; }
    if (c->s.rng_fn(c->s.ctx, p, (uint32_t)n) != 0) {
        seterr(c, "no randomness source");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* buffering                                                           */
/* ------------------------------------------------------------------ */

static int transcript_add(tls_conn* c, const uint8_t* p, size_t n)
{
    if (c->hs_len + n > c->hs_cap && grow((void**)&c->hs, &c->hs_cap,
                                          c->hs_len + n, c) != 0)
        return -1;
    memcpy(c->hs + c->hs_len, p, n);
    c->hs_len += n;
    return 0;
}

static int rx_add(tls_conn* c, const uint8_t* p, size_t n)
{
    if (c->rx_len + n > c->rx_cap && grow((void**)&c->rx, &c->rx_cap,
                                          c->rx_len + n, c) != 0)
        return -1;
    memcpy(c->rx + c->rx_len, p, n);
    c->rx_len += n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* record layer                                                       */
/* ------------------------------------------------------------------ */

static int read_record(tls_conn* c, uint32_t to, uint8_t* out, size_t outcap,
                       uint8_t* rtype, size_t* rlen)
{
    uint8_t hdr[5];
    unsigned n;
    if (recv_all(c, hdr, 5, to) != 0)
        return -1;
    if (be16(hdr + 1) != TLS_VERSION) {
        seterr(c, "version mismatch");
        return -1;
    }
    n = be16(hdr + 3);
    if (n > outcap) {
        seterr(c, "peer violation (record too large)");
        return -1;
    }
    if (n && recv_all(c, out, n, to) != 0)
        return -1;
    *rtype = hdr[0];
    *rlen = n;
    return 0;
}

static void gcm_nonce(const uint8_t iv[4], uint64_t seq, uint8_t nonce[12])
{
    unsigned i;
    memcpy(nonce, iv, 4);
    for (i = 0; i < 8; ++i)
        nonce[4 + i] = (uint8_t)(seq >> (8u * (7u - i)));
}

/* Nonce for a peer record: fixed IV || the 8 explicit bytes carried on
 * the wire (the peer chooses them; OpenSSL uses a random counter, so
 * this cannot be rebuilt from our local sequence number). */
static void gcm_nonce_wire(const uint8_t iv[4], const uint8_t rec[8],
                           uint8_t nonce[12])
{
    memcpy(nonce, iv, 4);
    memcpy(nonce + 4, rec, 8);
}

static void gcm_aad(uint64_t seq, uint8_t type, uint16_t ptlen, uint8_t aad[13])
{
    unsigned i;
    for (i = 0; i < 8; ++i)
        aad[i] = (uint8_t)(seq >> (8u * (7u - i)));
    aad[8] = type;
    aad[9] = (uint8_t)(TLS_VERSION >> 8);
    aad[10] = (uint8_t)TLS_VERSION;
    put16(aad + 11, ptlen);
}

/* Send one record. Before ChangeCipherSpec records are sent in the
 * clear; afterwards they use the GCM scheme (RFC 5288): the 8-byte
 * write sequence number doubles as the explicit nonce.
 * seq_w advances. */
static int tls_send_record(tls_conn* c, uint8_t type, const uint8_t* pt,
                           size_t ptlen, uint32_t to)
{
    uint8_t buf[MAX_PLAIN + 5 + 8 + TLS_GCM_TAG];
    uint64_t seq = c->seq_w;

    if (ptlen > MAX_PLAIN) { seterr(c, "peer violation (bad length)"); return -1; }

    buf[0] = type;
    put16(buf + 1, TLS_VERSION);

    if (c->write_enc) {
        uint8_t nonce[12], aad[13], ct[MAX_PLAIN + TLS_GCM_TAG];
        size_t i;
        gcm_nonce(c->civ, seq, nonce);
        gcm_aad(seq, type, (uint16_t)ptlen, aad);
        tls_gcm_seal(c->ckey, nonce, aad, sizeof aad, pt, ptlen, ct);
        put16(buf + 3, (unsigned)(AES_EXPLICIT + ptlen + TLS_GCM_TAG));
        for (i = 0; i < 8; ++i)
            buf[5 + i] = (uint8_t)(seq >> (8u * (7u - i)));
        memcpy(buf + 5 + AES_EXPLICIT, ct, ptlen + TLS_GCM_TAG);
        if (send_all(c, buf, 5 + AES_EXPLICIT + ptlen + TLS_GCM_TAG, to) != 0)
            return -1;
    } else {
        put16(buf + 3, (unsigned)ptlen);
        if (ptlen)
            memcpy(buf + 5, pt, ptlen);
        if (send_all(c, buf, 5 + ptlen, to) != 0)
            return -1;
    }
    ++c->seq_w;
    return 0;
}

/* ------------------------------------------------------------------ */
/* handshake messages                                                  */
/* ------------------------------------------------------------------ */

static int hs_send(tls_conn* c, uint8_t type, const uint8_t* body,
                   size_t bodylen, uint32_t to)
{
    uint8_t* wire;
    size_t n = 4 + bodylen;

    if (bodylen > 0xffffffu) { seterr(c, "handshake failure"); return -1; }
    wire = (uint8_t*)malloc(n);
    if (!wire) { seterr(c, "out of memory"); return -1; }
    wire[0] = type;
    put24(wire + 1, (uint32_t)bodylen);
    if (bodylen)
        memcpy(wire + 4, body, bodylen);

    if (transcript_add(c, wire, n) != 0
        || tls_send_record(c, CT_HANDSHAKE, wire, n, to) != 0) {
        free(wire);
        return -1;
    }
    free(wire);
    return 0;
}

static int build_client_hello(tls_conn* c, const char* host,
                              uint8_t* out, size_t cap)
{
    size_t hostlen = host ? strlen(host) : 0;
    /* server_name extension body: 2+1+2+hostlen (+4 type/len) */
    size_t sni_body = 9 + hostlen;
    /* signature_algorithms: 4 type/len + 2 listlen + 4 pairs*2 */
    size_t sig_body = 14;
    size_t n = 43 + sni_body + sig_body;
    size_t o;

    if (hostlen > 0xffff || n > cap)
        return -1;

    o = 0;
    put16(out + o, TLS_VERSION); o += 2;
    memcpy(out + o, c->crandom, 32); o += 32;
    out[o++] = 0;                    /* session id */
    put16(out + o, 2); o += 2;       /* suites length */
    put16(out + o, SUITE); o += 2;
    out[o++] = 1; out[o++] = 0;      /* compression: none */
    put16(out + o, (unsigned)(sni_body + sig_body)); o += 2;

    put16(out + o, 0); o += 2;       /* extension type: server_name */
    put16(out + o, (unsigned)(5 + hostlen)); o += 2;  /* ext payload len */
    put16(out + o, (unsigned)(3 + hostlen)); o += 2;  /* list length */
    out[o++] = 0;                    /* name type host_name */
    put16(out + o, (unsigned)hostlen); o += 2;
    if (hostlen)
        memcpy(out + o, host, hostlen);
    o += hostlen;

    put16(out + o, 13); o += 2;      /* extension type: signature_algs */
    put16(out + o, 10); o += 2;      /* ext payload len */
    put16(out + o, 8); o += 2;       /* list length (4 pairs) */
    out[o++] = 4; out[o++] = 1;      /* rsa_pkcs1_sha256 */
    out[o++] = 5; out[o++] = 1;      /* rsa_pkcs1_sha384 */
    out[o++] = 6; out[o++] = 1;      /* rsa_pkcs1_sha512 */
    out[o++] = 2; out[o++] = 1;      /* rsa_pkcs1_sha1   */

    return (int)n;
}

static int parse_server_hello(tls_conn* c, const uint8_t* b, size_t n)
{
    unsigned sidlen;
    if (n < 2 + 32 + 1) { seterr(c, "handshake failure"); return -1; }
    if (be16(b) != TLS_VERSION) { seterr(c, "version mismatch"); return -1; }
    memcpy(c->srandom, b + 2, 32);
    b += 34; n -= 34;
    sidlen = b[0]; b += 1; n -= 1;
    if (sidlen > n) { seterr(c, "handshake failure"); return -1; }
    b += sidlen; n -= sidlen;
    if (n < 2 + 1) { seterr(c, "handshake failure"); return -1; }
    if (be16(b) != SUITE) { seterr(c, "unsupported cipher suite"); return -1; }
    b += 2;
    if (b[0] != 0) { seterr(c, "unsupported compression"); return -1; }
    ++b; --n;
    return 0;   /* extensions (if any) follow; skipped */
}

static int parse_certificate(tls_conn* c, const uint8_t* b, size_t n)
{
    const uint8_t* pend = b + n;
    uint32_t chainlen;
    int got = 0;

    if (n < 3) { seterr(c, "server certificate unusable"); return -1; }
    chainlen = be24(b); b += 3;
    if (chainlen != (uint32_t)(pend - b)) {
        seterr(c, "server certificate unusable");
        return -1;
    }
    while ((uint32_t)(pend - b) >= 3) {
        uint32_t clen = be24(b);
        b += 3;
        if ((uint32_t)(pend - b) < clen) {
            seterr(c, "server certificate unusable");
            return -1;
        }
if (!got) {
                uint8_t mod[TLS_RSA_MAX_MODULUS], exp[8];
                uint32_t mlen = (uint32_t)sizeof mod;
                uint32_t elen = (uint32_t)sizeof exp;
                if (tls_x509_rsa_pubkey(b, clen, mod, &mlen, exp, &elen) == 0 &&
                    mlen <= sizeof c->peer_mod) {
                memcpy(c->peer_mod, mod, mlen);
                c->peer_modlen = mlen;
                got = 1;
            }
        }
        b += clen;
    }
    if (!got) {
        seterr(c, "server certificate unusable");
        return -1;
    }
    return 0;
}

/* Parse as many complete server handshake messages as rx holds. */
static int hs_drain(tls_conn* c)
{
    uint8_t* p = c->rx;
    size_t   left = c->rx_len;

    while (left >= 4) {
        uint32_t len = be24(p + 1);
        if (left < 4 + len)
            break;                       /* more data coming */
        switch (p[0]) {
        case HS_SERVER_HELLO:
            if (parse_server_hello(c, p + 4, len) != 0) return -1;
            break;
        case HS_CERTIFICATE:
            if (parse_certificate(c, p + 4, len) != 0) return -1;
            break;
        case HS_CERT_REQUEST:
            break;                       /* we send no client cert */
        case HS_SERVER_KEY_EXCHANGE:
            seterr(c, "unexpected server key exchange");
            return -1;
        case HS_SERVER_HELLO_DONE:
            c->stage = ST_FLIGHT_DONE;
            break;
        default:
            seterr(c, "handshake failure");
            return -1;
        }
        p += 4 + len;
        left -= 4 + len;
    }
    if (left)
        shift_left(c->rx, c->rx_len - left, c->rx_len);
    c->rx_len = left;
    return 0;
}

/* ------------------------------------------------------------------ */
/* key schedule                                                        */
/* ------------------------------------------------------------------ */

static void derive_keys(tls_conn* c, const uint8_t* pms)
{
    uint8_t seed[77];
    uint8_t kb[40];

    /* master = PRF(pre-master-secret, "master secret", c||s) */
    memcpy(seed, "master secret", 13);
    memcpy(seed + 13, c->crandom, 32);
    memcpy(seed + 45, c->srandom, 32);
    tls_prf(pms, 48, seed, 77, c->master, 48);

    /* key_block = PRF(master, "key expansion", s||c) */
    memcpy(seed, "key expansion", 13);
    memcpy(seed + 13, c->srandom, 32);
    memcpy(seed + 45, c->crandom, 32);
    tls_prf(c->master, 48, seed, 77, kb, sizeof kb);

    memcpy(c->ckey, kb, 16);
    memcpy(c->skey, kb + 16, 16);
    memcpy(c->civ, kb + 32, 4);
    memcpy(c->siv, kb + 36, 4);
}

/* ------------------------------------------------------------------ */
/* handshake                                                           */
/* ------------------------------------------------------------------ */

tls_conn* tls_connect(tls_stream* stream, const char* host, uint32_t to)
{
    tls_conn* c;
    uint8_t  hello[256];
    int      helolen;
    uint8_t  pms[48], enc[256];
    uint8_t  cke[258];
    int      rc = -1;

    if (!stream || !stream->send_fn || !stream->recv_fn) {
        seterr_global("invalid stream");
        return NULL;
    }

    c = (tls_conn*)xcalloc(sizeof *c, 1);
    if (!c) { seterr_global("out of memory"); return NULL; }
    memcpy(&c->s, stream, sizeof c->s);

    if (want_rng(c, c->crandom, 32) != 0)
        goto out;

    /* --- ClientHello --- */
    helolen = build_client_hello(c, host, hello, sizeof hello);
    if (helolen < 0) { seterr(c, "handshake failure"); goto out; }
    {
        /* the record payload is the full handshake message: header + body */
        uint8_t hmsg[256 + 4];
        hmsg[0] = HS_CLIENT_HELLO;
        put24(hmsg + 1, (uint32_t)helolen);
        memcpy(hmsg + 4, hello, (size_t)helolen);
        if (transcript_add(c, hmsg, (size_t)helolen + 4) != 0) goto out;
        if (tls_send_record(c, CT_HANDSHAKE, hmsg, (size_t)helolen + 4, to) != 0)
            goto out;
    }
    c->stage = ST_AWAIT_SHD;

    /* --- server flight --- */
    for (;;) {
        uint8_t rec[MAX_RECORD_RX];
        uint8_t rtype;
        size_t  rlen;
        if (read_record(c, to, rec, sizeof rec, &rtype, &rlen) != 0)
            goto out;
        ++c->seq_r;          /* server records: SH, Cert, SHD, ... */
        if (rtype == CT_ALERT && rlen == 2) {
            alert_err(c, rec[1]);
            goto out;
        }
        if (rtype != CT_HANDSHAKE) {
            seterr(c, "unexpected record during handshake");
            goto out;
        }
        if (transcript_add(c, rec, rlen) != 0) goto out;
        if (rx_add(c, rec, rlen) != 0) goto out;
        if (hs_drain(c) != 0) goto out;
        if (c->stage == ST_FLIGHT_DONE)
            break;
    }

    if (c->peer_modlen != 256) {
        seterr(c, "server certificate unusable (expected RSA 2048)");
        goto out;
    }

    /* --- ClientKeyExchange: RSA-encrypt the pre-master secret --- */
    pms[0] = (uint8_t)(TLS_VERSION >> 8);
    pms[1] = (uint8_t)TLS_VERSION;
    if (want_rng(c, pms + 2, 46) != 0) goto out;
    if (tls_rsa_encrypt_pkcs1(c->peer_mod, 256, pms, 48, enc) != 0) {
        seterr(c, "RSA encryption failed");
        goto out;
    }
    put16(cke, 256);
    memcpy(cke + 2, enc, 256);
    if (hs_send(c, HS_CLIENT_KEY_EXCHANGE, cke, 258, to) != 0)
        goto out;

    derive_keys(c, pms);

    /* --- ChangeCipherSpec (plaintext) --- */
    {
        uint8_t ccs[6] = { CT_CCS, (uint8_t)(TLS_VERSION >> 8),
                           (uint8_t)TLS_VERSION, 0, 1, 1 };
        if (send_all(c, ccs, sizeof ccs, to) != 0)
            goto out;
        /* New write epoch: the CCS was the last plaintext record, so the
         * client Finished below must be sealed with sequence number 0
         * (OpenSSL restarts its read counter at 0 for the same reason). */
        c->seq_w = 0;
    }

    /* From here on (client Finished + app data) records are encrypted. */
    c->write_enc = 1;

    /* --- client Finished (first encrypted record) --- */
    {
        uint8_t  fin[16];
        uint8_t  hv[32], sd[47];
        tls_sha256_ctx m;
        tls_sha256_init(&m);
        tls_sha256_update(&m, c->hs, c->hs_len);
        tls_sha256_final(&m, hv);
        memcpy(sd, "client finished", 15);
        memcpy(sd + 15, hv, 32);
        tls_prf(c->master, 48, sd, 47, fin + 4, 12);
        fin[0] = HS_FINISHED;
        put24(fin + 1, 12);
        if (transcript_add(c, fin, sizeof fin) != 0) goto out;
        if (tls_send_record(c, CT_HANDSHAKE, fin, sizeof fin, to) != 0)
            goto out;
    }

    /* --- server CCS + encrypted Finished --- */
    for (;;) {
        uint8_t rec[MAX_RECORD_RX + 64];
        uint8_t rtype;
        size_t  rlen;
        if (read_record(c, to, rec, sizeof rec, &rtype, &rlen) != 0)
            goto out;
        if (rtype == CT_CCS) {
            if (rlen != 1 || rec[0] != 1) {
                seterr(c, "handshake failure");
                goto out;
            }
            /* New read epoch: the server's Finished below is the first
             * record protected by the new cipher state, so its AAD
             * sequence number is 0. */
            c->seq_r = 0;
            c->read_enc = 1;
            continue;
        }
        if (rtype == CT_ALERT && rlen == 2 && !c->read_enc) {
            alert_err(c, rec[1]);
            goto out;
        }
        if (rtype != CT_HANDSHAKE) {
            seterr(c, "handshake failure");
            goto out;
        }
        if (!c->read_enc) {
            /* Plaintext handshake record before the server's CCS (e.g. a
             * NewSessionTicket when the peer sent one anyway): consume
             * it and keep it in the transcript for the server Finished. */
            if (transcript_add(c, rec, rlen) != 0)
                goto out;
            continue;
        }
        /* decrypt the Finished record */
        {
            uint8_t aad[13], nonce[12], pt[32];
            uint64_t seq = c->seq_r;
            size_t  ptlen;
            if (rlen < AES_EXPLICIT + TLS_GCM_TAG) {
                seterr(c, "peer violation (record too short)");
                goto out;
            }
            ptlen = rlen - AES_EXPLICIT - TLS_GCM_TAG;
            gcm_nonce_wire(c->siv, rec, nonce);
            gcm_aad(seq, CT_HANDSHAKE, (uint16_t)ptlen, aad);
            if (tls_gcm_open(c->skey, nonce, aad, sizeof aad,
                             rec + AES_EXPLICIT, ptlen + TLS_GCM_TAG,
                             pt, rec + AES_EXPLICIT + ptlen) != 0) {
                seterr(c, "record authentication failed");
                goto out;
            }
            ++c->seq_r;

            if (ptlen != 16 || pt[0] != HS_FINISHED) {
                seterr(c, "handshake failure");
                goto out;
            }
            {
                uint8_t hv[32], fin[12], sd[47];
                tls_sha256_ctx m;
                tls_sha256_init(&m);
                tls_sha256_update(&m, c->hs, c->hs_len);
                tls_sha256_final(&m, hv);
                memcpy(sd, "server finished", 15);
                memcpy(sd + 15, hv, 32);
                tls_prf(c->master, 48, sd, 47, fin, 12);
                if (!ct_eq(fin, pt + 4, 12)) {
                    seterr(c, "record authentication failed");
                    goto out;
                }
            }
        }
        break;
    }

    c->stage = ST_ESTABLISHED;
    rc = 0;

out:
    if (rc != 0) {
        free(c->hs);
        free(c->rx);
        free(c->inbuf);
        free(c);
        return NULL;
    }
    return c;
}

/* ------------------------------------------------------------------ */
/* application data                                                    */
/* ------------------------------------------------------------------ */

/* Decrypt one protected record (type CT_APP or CT_ALERT) into inbuf. */
static int decrypt_record(tls_conn* c, uint8_t type, uint8_t* rec, size_t rlen)
{
    uint8_t aad[13], nonce[12], tag[TLS_GCM_TAG];
    uint64_t seq = c->seq_r;
    size_t ptlen;

    if (rlen < AES_EXPLICIT + TLS_GCM_TAG) {
        seterr(c, "peer violation (record too short)");
        return -1;
    }
    ptlen = rlen - AES_EXPLICIT - TLS_GCM_TAG;
    if (ptlen > MAX_PLAIN) {
        seterr(c, "peer violation (bad length)");
        return -1;
    }
    if (c->in_len + ptlen > c->in_cap &&
        grow((void**)&c->inbuf, &c->in_cap, c->in_len + ptlen, c) != 0)
        return -1;

    gcm_nonce_wire(c->siv, rec, nonce);
    gcm_aad(seq, type, (uint16_t)ptlen, aad);
    if (tls_gcm_open(c->skey, nonce, aad, sizeof aad,
                     rec + AES_EXPLICIT, ptlen + TLS_GCM_TAG,
                     c->inbuf + c->in_len, tag) != 0) {
        seterr(c, "record authentication failed");
        return -1;
    }
    ++c->seq_r;
    c->in_len += ptlen;
    return 0;
}

int tls_write(tls_conn* c, const void* buf, uint32_t len, uint32_t to)
{
    const uint8_t* p = (const uint8_t*)buf;

    if (!c || c->stage != ST_ESTABLISHED) {
        seterr_global(c ? "not connected" : "invalid handle");
        return 0;
    }
    while (len > 0) {
        uint32_t chunk = len > MAX_PLAIN ? MAX_PLAIN : len;
        if (tls_send_record(c, CT_APP, p, chunk, to) != 0)
            return 0;
        p += chunk;
        len -= chunk;
    }
    return (int)(p - (const uint8_t*)buf);
}

int tls_read(tls_conn* c, void* buf, uint32_t len, uint32_t to)
{
    uint8_t* out = (uint8_t*)buf;

    if (!c || c->stage != ST_ESTABLISHED) {
        seterr_global(c ? "not connected" : "invalid handle");
        return -1;
    }

    for (;;) {
        size_t avail = c->in_len - c->in_off;
        if (avail > 0) {
            size_t n = avail > len ? len : avail;
            memcpy(out, c->inbuf + c->in_off, n);
            c->in_off += n;
            if (c->in_off == c->in_len) {
                c->in_len = c->in_off = 0;
            }
            return (int)n;
        }
        if (c->closed)
            return 0;

        {
            uint8_t rec[MAX_RECORD_RX + 8];
            uint8_t rtype;
            size_t  rlen;
            if (read_record(c, to, rec, sizeof rec, &rtype, &rlen) != 0)
                return -1;
            switch (rtype) {
            case CT_APP:
                if (decrypt_record(c, CT_APP, rec, rlen) != 0)
                    return -1;
                break;
            case CT_ALERT:
                if (decrypt_record(c, CT_ALERT, rec, rlen) != 0)
                    return -1;
                /* the alert is the first 2 plaintext bytes */
                if (c->in_len - c->in_off >= 2) {
                    int desc = c->inbuf[c->in_off + 1];
                    c->in_off += 2;
                    if (desc == ALERT_CLOSE_NOTIFY) {
                        c->closed = 1;
                        if (c->in_off == c->in_len)
                            c->in_len = c->in_off = 0;
                    } else {
                        seterr(c, "received fatal alert");
                        return 0;
                    }
                }
                break;
            case CT_HANDSHAKE:
                seterr(c, "unsolicited handshake (renegotiation?)");
                return -1;
            default:
                seterr(c, "peer violation (bad record type)");
                return -1;
            }
        }
    }
}

int tls_close(tls_conn* c)
{
    if (!c)
        return 0;
    if (c->stage == ST_ESTABLISHED && !c->closed) {
        /* send close_notify as an encrypted alert record */
        uint8_t alert[2] = { 1, ALERT_CLOSE_NOTIFY };
        tls_send_record(c, CT_ALERT, alert, sizeof alert, 2000);
        c->closed = 1;
    }
    free(c->hs);
    free(c->rx);
    free(c->inbuf);
    free(c);
    return 0;
}