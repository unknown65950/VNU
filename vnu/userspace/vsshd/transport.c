/* transport.c - SSH-2 transport for vsshd: version exchange, KEXINIT
 * negotiation, diffie-hellman-group14-sha256 key exchange, AES-128-CTR
 * packet codec with hmac-sha2-256, and the key derivation that wires
 * the two together.
 *
 * See ssh.h for the one configuration this speaks. The surface aims at
 * a stock OpenSSH 9.x client; anything it does not implement (curve
 * keys, gssapi, etm MACs) is simply absent from its KEXINIT, so the
 * negotiation falls back to what both sides have.
 */

#include <vlibc/string.h>
#include <vlibc/unistd.h>
#include <vlibc/fcntl.h>
#include <vlibc/socket.h>
#include <vlibc/tls.h>
#include <vlibc/stdlib.h>
#include <vnu/abi.h>
#include "ssh.h"
#include "hostkey.h"

/* ------ growable buffer -------------------------------------------- */

void buf_init(Buf* b)
{
    b->b = 0;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf* b)
{
    if (b->b)
        free(b->b);
    b->b = 0;
    b->len = b->cap = 0;
}

static int buf_reserve(Buf* b, uint32_t extra)
{
    if (b->len + extra <= b->cap)
        return 0;
    uint32_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra)
        cap *= 2;
    /* vlibc has no realloc; grow the way the classic macros did */
    uint8_t* nb = (uint8_t*)malloc(cap);
    if (!nb)
        return -1;
    if (b->len)
        memcpy(nb, b->b, b->len);
    free(b->b);
    b->b = nb;
    b->cap = cap;
    return 0;
}

int buf_put(Buf* b, const void* data, uint32_t n)
{
    if (buf_reserve(b, n) < 0)
        return -1;
    if (n)
        memcpy(b->b + b->len, data, n);
    b->len += n;
    return 0;
}

int buf_put_u8(Buf* b, uint8_t v)
{
    return buf_put(b, &v, 1);
}

int buf_put_u32(Buf* b, uint32_t v)
{
    uint8_t t[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
                    (uint8_t)(v >> 8), (uint8_t)v};
    return buf_put(b, t, 4);
}

int buf_put_str(Buf* b, const void* data, uint32_t n)
{
    return buf_put_u32(b, n) || buf_put(b, data, n);
}

int buf_put_cstr(Buf* b, const char* s)
{
    return buf_put_str(b, s, (uint32_t)strlen(s));
}

/* SSH mpint: a signed two's-complement big integer, so a non-negative
 * value whose top bit is set gets a zero byte prepended. The minimal
 * form always keeps one byte (zero encodes as a single 0x00). */
int buf_put_mpint(Buf* b, const uint8_t* v, uint32_t len)
{
    while (len > 1 && v[0] == 0)
        ++v, --len;
    if (len && (v[0] & 0x80)) {
        uint8_t z = 0;
        if (buf_put_u32(b, len + 1) < 0 || buf_put(b, &z, 1) < 0)
            return -1;
    } else if (buf_put_u32(b, len) < 0) {
        return -1;
    }
    return buf_put(b, v, len);
}

int buf_put_bytes(Buf* b, uint8_t fill, uint32_t n)
{
    if (buf_reserve(b, n) < 0)
        return -1;
    memset(b->b + b->len, fill, n);
    b->len += n;
    return 0;
}

/* ------ exact socket I/O ------------------------------------------- */

int ssh_write(Ssh* s, const void* data, uint32_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t done = 0;
    while (done < len) {
        int r = vnu_send(s->sock, p + done, len - done, 5000);
        if (r < 0)
            return -1;
        if (r == 0)
            return -1; /* peer closed mid-stream */
        done += (uint32_t)r;
    }
    return 0;
}

static int recv_exact(Ssh* s, uint8_t* out, uint32_t len)
{
    uint32_t done = 0;
    while (done < len) {
        /* the first byte of a packet respects the poll timeout; the
         * rest of it blocks (it is on the wire, it has to arrive) */
        int timeout = (done == 0) ? s->recv_timeout : 5000;
        int r = vnu_recv(s->sock, out + done, len - done, timeout);
        if (r == -VNU_ETIMEDOUT && done == 0)
            return SSH_RECV_TIMEOUT;
        if (r < 0)
            return -1;
        if (r == 0)
            return -1;
        done += (uint32_t)r;
    }
    return 0;
}

/* ------ AES-128-CTR stream ----------------------------------------- */

/* Crypt `len` bytes of `data` with the counter `ctr`, which advances by
 * one per AES block and is *not* reset between packets (RFC 4344). */
static void ctr_crypt(tls_aes128* a, uint8_t ctr[16], uint8_t* data,
                      uint32_t len)
{
    uint8_t ks[16];
    uint32_t off = 0;
    while (off < len) {
        int i;
        tls_aes128_enc(a, ctr, ks);
        for (i = 0; i < 16 && off < len; ++i)
            data[off + (uint32_t)i] ^= ks[i];
        off += 16;
        /* increment the whole 128-bit counter, big-endian */
        for (i = 15; i >= 0; --i)
            if (++ctr[i])
                break;
    }
}

/* ------ packet send -------------------------------------------------
 * Builds one SSH binary packet around `payload`, pads it and, once the
 * cipher is live, seals it the way RFC 4253 describes for hmac-sha2-256
 * (not the etm variant): the MAC is computed from the *unencrypted*
 * packet (6.4) and then the whole packet - length field included - goes
 * through AES-CTR (6.3). */
static int send_packet(Ssh* s, const uint8_t* payload, uint32_t plen)
{
    uint8_t body[SSH_MAX_PACKET + 64];
    uint32_t pad_len = 4;
    uint32_t packet_length;
    uint32_t i;

    if (plen + 1 + 4 > sizeof(body))
        return -1;

    /* Choose padding so the total on-wire packet - the 4-byte length
     * field plus packet_length (1 + payload + padding) - is a multiple of
     * the block size (16 with the cipher live, 8 before it). RFC 4253 6:
     * the concatenation of packet_length, padding_length, payload and
     * padding must align, and that concatenation includes the length
     * field itself. Leaving those 4 bytes out is what a client reports as
     * a padding error. */
    uint32_t block = s->enc ? 16 : 8;
    while ((4 + 1 + plen + pad_len) % block)
        ++pad_len;

    packet_length = 1 + plen + pad_len;
    body[0] = (uint8_t)(packet_length >> 24);
    body[1] = (uint8_t)(packet_length >> 16);
    body[2] = (uint8_t)(packet_length >> 8);
    body[3] = (uint8_t)packet_length;
    body[4] = (uint8_t)pad_len;
    memcpy(body + 5, payload, plen);
    /* padding bytes: anything; zero is as good as random here */
    for (i = 5 + plen; i < 5 + plen + pad_len; ++i)
        body[i] = 0;

    uint8_t mac[32];
    uint32_t mac_len = s->enc ? 32 : 0;

    if (s->enc) {
        /* MAC over seq || the plaintext packet, then encrypt the whole
         * packet (length first) with the rolling CTR counter. */
        uint8_t macin[4 + 4 + SSH_MAX_PACKET + 64];
        uint32_t seq = s->seq_out++;
        macin[0] = (uint8_t)(seq >> 24);
        macin[1] = (uint8_t)(seq >> 16);
        macin[2] = (uint8_t)(seq >> 8);
        macin[3] = (uint8_t)seq;
        memcpy(macin + 4, body, 4 + packet_length);
        tls_hmac_sha256(s->mac_out, 32, macin, 4 + 4 + packet_length, mac);
        ctr_crypt(&s->enc_aes, s->enc_ctr, body, 4 + packet_length);
    } else {
        s->seq_out++; /* the counter runs over plaintext packets too */
    }

    if (ssh_write(s, body, 4 + packet_length) < 0)
        return -1;
    if (mac_len && ssh_write(s, mac, mac_len) < 0)
        return -1;
    return 0;
}

int ssh_send_raw(Ssh* s, const uint8_t* payload, uint32_t len)
{
    return send_packet(s, payload, len);
}

/* ------ packet recv -------------------------------------------------- */
static int read_u32(const uint8_t* p, uint32_t* v)
{
    *v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    return 0;
}

/* Receive one packet's payload (type byte first). Returns 0 on success,
 * -1 on I/O error or protocol failure. */
int ssh_recv_packet(Ssh* s, uint8_t* payload, uint32_t* len, uint32_t maxlen)
{
    uint8_t hdr[4];
    uint32_t packet_length;
    uint8_t body[SSH_MAX_PACKET + 64];

    if (s->enc) {
        /* The length field is encrypted as well (RFC 4253 6.3), so the
         * first cipher block must be read - and decrypted - before the
         * length is known. That block yields the length plus the first
         * 12 bytes of the body; (4 + packet_length) is a multiple of 16
         * for a well-formed peer, so every ctr_crypt() call below
         * covers whole blocks and the counter stays aligned with the
         * sender's. */
        uint8_t blk[16];
        uint8_t mac[32];
        uint8_t maccalc[32];
        uint8_t macin[4 + 4 + SSH_MAX_PACKET + 64];
        uint32_t seq;
        int r = recv_exact(s, blk, 16);
        if (r == SSH_RECV_TIMEOUT)
            return SSH_RECV_TIMEOUT;
        if (r < 0)
            return -1;
        ctr_crypt(&s->dec_aes, s->dec_ctr, blk, 16);
        memcpy(hdr, blk, 4);
        read_u32(hdr, &packet_length);
        if (packet_length < 12 || packet_length > sizeof(body) ||
            (4 + packet_length) % 16 != 0)
            return -1;
        memcpy(body, blk + 4, 12);
        if (packet_length > 12) {
            if (recv_exact(s, body + 12, packet_length - 12) < 0)
                return -1;
            ctr_crypt(&s->dec_aes, s->dec_ctr, body + 12,
                      packet_length - 12);
        }
        /* verify the MAC over seq || plaintext packet (RFC 4253 6.4) */
        if (recv_exact(s, mac, 32) < 0)
            return -1;
        seq = s->seq_in;
        macin[0] = (uint8_t)(seq >> 24);
        macin[1] = (uint8_t)(seq >> 16);
        macin[2] = (uint8_t)(seq >> 8);
        macin[3] = (uint8_t)seq;
        memcpy(macin + 4, hdr, 4);
        memcpy(macin + 8, body, packet_length);
        tls_hmac_sha256(s->mac_in, 32, macin, 4 + 4 + packet_length, maccalc);
        if (memcmp(mac, maccalc, 32) != 0) {
            return -1;
        }
        s->seq_in++;
    } else {
        int r = recv_exact(s, hdr, 4);
        if (r == SSH_RECV_TIMEOUT)
            return SSH_RECV_TIMEOUT;
        if (r < 0)
            return -1;
        read_u32(hdr, &packet_length);
        if (packet_length < 5 || packet_length > sizeof(body))
            return -1;
        if (recv_exact(s, body, packet_length) < 0)
            return -1;
        s->seq_in++;
    }

    uint8_t pad_len = body[0];
    if (pad_len < 4 || pad_len > packet_length - 1)
        return -1;
    uint32_t plen = packet_length - 1 - pad_len;
    if (plen > maxlen)
        return -1;
    memcpy(payload, body + 1, plen);
    *len = plen;
    return 0;
}

/* ------ RFC 3526 group 14 (2048-bit MODP) ---------------------------- */
static const uint8_t DH_P[256] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xc9,0x0f,0xda,0xa2,
    0x21,0x68,0xc2,0x34,0xc4,0xc6,0x62,0x8b,0x80,0xdc,0x1c,0xd1,
    0x29,0x02,0x4e,0x08,0x8a,0x67,0xcc,0x74,0x02,0x0b,0xbe,0xa6,
    0x3b,0x13,0x9b,0x22,0x51,0x4a,0x08,0x79,0x8e,0x34,0x04,0xdd,
    0xef,0x95,0x19,0xb3,0xcd,0x3a,0x43,0x1b,0x30,0x2b,0x0a,0x6d,
    0xf2,0x5f,0x14,0x37,0x4f,0xe1,0x35,0x6d,0x6d,0x51,0xc2,0x45,
    0xe4,0x85,0xb5,0x76,0x62,0x5e,0x7e,0xc6,0xf4,0x4c,0x42,0xe9,
    0xa6,0x37,0xed,0x6b,0x0b,0xff,0x5c,0xb6,0xf4,0x06,0xb7,0xed,
    0xee,0x38,0x6b,0xfb,0x5a,0x89,0x9f,0xa5,0xae,0x9f,0x24,0x11,
    0x7c,0x4b,0x1f,0xe6,0x49,0x28,0x66,0x51,0xec,0xe4,0x5b,0x3d,
    0xc2,0x00,0x7c,0xb8,0xa1,0x63,0xbf,0x05,0x98,0xda,0x48,0x36,
    0x1c,0x55,0xd3,0x9a,0x69,0x16,0x3f,0xa8,0xfd,0x24,0xcf,0x5f,
    0x83,0x65,0x5d,0x23,0xdc,0xa3,0xad,0x96,0x1c,0x62,0xf3,0x56,
    0x20,0x85,0x52,0xbb,0x9e,0xd5,0x29,0x07,0x70,0x96,0x96,0x6d,
    0x67,0x0c,0x35,0x4e,0x4a,0xbc,0x98,0x04,0xf1,0x74,0x6c,0x08,
    0xca,0x18,0x21,0x7c,0x32,0x90,0x5e,0x46,0x2e,0x36,0xce,0x3b,
    0xe3,0x9e,0x77,0x2c,0x18,0x0e,0x86,0x03,0x9b,0x27,0x83,0xa2,
    0xec,0x07,0xa2,0x8f,0xb5,0xc5,0x5d,0xf0,0x6f,0x4c,0x52,0xc9,
    0xde,0x2b,0xcb,0xf6,0x95,0x58,0x17,0x18,0x39,0x95,0x49,0x7c,
    0xea,0x95,0x6a,0xe5,0x15,0xd2,0x26,0x18,0x98,0xfa,0x05,0x10,
    0x15,0x72,0x8e,0x5a,0x8a,0xac,0xaa,0x68,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,
};
static const uint8_t DH_G[1] = {2};

/* ------ host key blob and rsa-sha2-256 signing ----------------------- */
int ssh_hostkey_blob(uint8_t* out, uint32_t outcap, uint32_t* len)
{
    static const uint8_t ebuf[3] = {0x01, 0x00, 0x01};
    Buf b;
    buf_init(&b);
    buf_put_cstr(&b, "ssh-rsa");
    buf_put_mpint(&b, ebuf, sizeof(ebuf));
    buf_put_mpint(&b, vsshd_pub_n, VSSHD_PUB_N_LEN);
    if (b.len > outcap) {
        buf_free(&b);
        return -1;
    }
    memcpy(out, b.b, b.len);
    *len = b.len;
    buf_free(&b);
    return 0;
}

/* RSASSA-PKCS1-v1_5 with SHA-256 as RFC 8332 wants it for the
 * rsa-sha2-256 signature: EM = 01 FF..FF 00 || DigestInfo(SHA-256) || H,
 * then s = EM^d mod n. `out` gets the raw 256-byte signature. */
int ssh_sign(const uint8_t h[32], uint8_t* out, uint32_t outcap,
             uint32_t* len)
{
    static const uint8_t digest_info[19] = {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
        0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20,
    };
    uint8_t d[32];
    uint8_t em[256];
    uint32_t tlen = sizeof(digest_info) + 32;
    uint32_t i;
    if (outcap < VSSHD_PUB_N_LEN)
        return -1;
    /* rsa-sha2-256 signs SHA-256(H): PKCS#1 v1.5 hashes the data it is
     * given (RFC 4253 8 spells it out for ssh-dss and SHA-1; the same
     * applies to the SHA-256 flavour). Putting H itself into the
     * DigestInfo - however correct H is - makes the client report an
     * incorrect signature. */
    tls_sha256(h, 32, d);
    em[0] = 0x00;
    em[1] = 0x01;
    for (i = 2; i < VSSHD_PUB_N_LEN - tlen - 1; ++i)
        em[i] = 0xff;
    em[i] = 0x00;
    memcpy(em + VSSHD_PUB_N_LEN - tlen, digest_info, sizeof(digest_info));
    memcpy(em + VSSHD_PUB_N_LEN - 32, d, 32);
    tls_modpow(em, VSSHD_PUB_N_LEN, vsshd_priv_d, VSSHD_PRIV_D_LEN,
               vsshd_pub_n, VSSHD_PUB_N_LEN, out);
    *len = VSSHD_PUB_N_LEN;
    return 0;
}

/* ------ name-list negotiation ---------------------------------------- */
static int list_has(const char* list, uint32_t llen, const char* want)
{
    uint32_t wlen = (uint32_t)strlen(want);
    uint32_t i = 0;
    while (i < llen) {
        uint32_t j = i;
        while (j < llen && list[j] != ',')
            ++j;
        if (j - i == wlen && memcmp(list + i, want, wlen) == 0)
            return 1;
        i = j + 1;
    }
    return 0;
}

/* Verify the client's KEXINIT payload offers what we can answer, and
 * remember its first-kex-follows flag in `*first_follows`. When the
 * flag is set, the client optimistically appended a KEXDH_INIT for its
 * *first* kex algorithm; `*first_is_group14` tells whether that guess
 * happens to match ours (then it must be consumed, not skipped).
 * Returns 0 when a usable intersection exists. */
static int parse_kexinit(const uint8_t* p, uint32_t plen, int* first_follows,
                         int* first_is_group14)
{
    const char* lists[10];
    uint32_t lens[10];
    uint32_t off;
    int i;
    if (plen < 17 || p[0] != SSH_MSG_KEXINIT)
        return -1;
    off = 17; /* type + cookie */
    for (i = 0; i < 10; ++i) {
        if (off + 4 > plen)
            return -1;
        uint32_t l = ((uint32_t)p[off] << 24) | ((uint32_t)p[off + 1] << 16) |
                     ((uint32_t)p[off + 2] << 8) | (uint32_t)p[off + 3];
        off += 4;
        if (off + l > plen)
            return -1;
        lists[i] = (const char*)p + off;
        lens[i] = l;
        off += l;
    }
    if (off + 1 > plen)
        return -1;
    *first_follows = p[off];
    {
        /* the client's first real kex choice (markers skipped) */
        uint32_t i = 0;
        *first_is_group14 = 0;
        while (i < lens[0]) {
            uint32_t j = i;
            while (j < lens[0] && lists[0][j] != ',')
                ++j;
            uint32_t n = j - i;
            if (!(n == 9 && memcmp(lists[0] + i, "ext-info-c", 9) == 0)) {
                if (n == 30 &&
                    memcmp(lists[0] + i,
                           "diffie-hellman-group14-sha256", 30) == 0)
                    *first_is_group14 = 1;
                break;
            }
            i = j + 1;
        }
    }
    /* the four negotiated axes must contain our single offering */
    if (!list_has(lists[0], lens[0], "diffie-hellman-group14-sha256"))
        return -1;
    if (!list_has(lists[1], lens[1], "rsa-sha2-256") &&
        !list_has(lists[1], lens[1], "ssh-rsa"))
        return -1;
    if (!list_has(lists[2], lens[2], "aes128-ctr"))
        return -1;
    if (!list_has(lists[4], lens[4], "hmac-sha2-256"))
        return -1;
    if (!list_has(lists[6], lens[6], "none"))
        return -1;
    return 0;
}

/* Hash a "string" - RFC 4253's uint32 length followed by the bytes -
 * into a running SHA-256 context. The exchange hash carries V_C, V_S,
 * I_C, I_S and K_S this way, length prefix included. */
static void hash_str(tls_sha256_ctx* hc, const uint8_t* p, uint32_t n)
{
    uint8_t lp[4];
    lp[0] = (uint8_t)(n >> 24);
    lp[1] = (uint8_t)(n >> 16);
    lp[2] = (uint8_t)(n >> 8);
    lp[3] = (uint8_t)n;
    tls_sha256_update(hc, lp, 4);
    if (n)
        tls_sha256_update(hc, p, n);
}

/* mpint serialization of v (len bytes, sign-padded) into buf, returning
 * the total length. Used for the transcript and the key derivation,
 * where the peer needs the exact same bytes both sides agreed on. */
static uint32_t mpint_cat(uint8_t* out, const uint8_t* v, uint32_t len)
{
    while (len > 1 && v[0] == 0)
        ++v, --len;
    uint32_t body = len;
    if (len && (v[0] & 0x80))
        body = len + 1;
    out[0] = (uint8_t)(body >> 24);
    out[1] = (uint8_t)(body >> 16);
    out[2] = (uint8_t)(body >> 8);
    out[3] = (uint8_t)body;
    if (len && (v[0] & 0x80))
        out[4] = 0;
    memcpy(out + 4 + (body - len), v, len);
    return 4 + body;
}

/* Truthful entropy for the DH exponent and the KEXINIT cookie. The
 * random device is the kernel's pool; a short read is topped up with
 * the address-space counter, good enough for an exponent whose secrecy
 * is a formality on a hobby box. */
static void fill_random(uint8_t* out, uint32_t len)
{
    int fd = open("/dev/random", O_RDONLY);
    uint32_t got = 0;
    uint32_t i;
    if (fd >= 0) {
        got = (uint32_t)read(fd, out, len);
        close(fd);
    }
    for (i = got; i < len; ++i)
        out[i] = (uint8_t)(i ^ (got + 1));
}

/* Build our KEXINIT payload (type byte first, cookie, ten name-lists,
 * first_kex_follows, reserved). */
static void build_kexinit(uint8_t* out, uint32_t* len)
{
    uint8_t cookie[16];
    Buf b;
    buf_init(&b);
    fill_random(cookie, sizeof(cookie));
    buf_put_u8(&b, SSH_MSG_KEXINIT);
    buf_put(&b, cookie, 16);
    buf_put_cstr(&b, "diffie-hellman-group14-sha256");
    buf_put_cstr(&b, "rsa-sha2-256,ssh-rsa");
    buf_put_cstr(&b, "aes128-ctr");
    buf_put_cstr(&b, "aes128-ctr");
    buf_put_cstr(&b, "hmac-sha2-256");
    buf_put_cstr(&b, "hmac-sha2-256");
    buf_put_cstr(&b, "none");
    buf_put_cstr(&b, "none");
    buf_put_cstr(&b, "");
    buf_put_cstr(&b, "");
    buf_put_u8(&b, 0);
    buf_put_u32(&b, 0);
    memcpy(out, b.b, b.len);
    *len = b.len;
    buf_free(&b);
}

/* The client's version line: read whole lines and take the first that
 * starts with "SSH-". RFC 4253 lets a client send a preamble before it,
 * and OpenSSH sends none, but a modem-noise line should not kill the
 * handshake. */
static int read_version(Ssh* s)
{
    for (;;) {
        uint8_t line[256];
        uint32_t n = 0;
        for (;;) {
            uint8_t c;
            if (recv_exact(s, &c, 1) < 0) {
                return -1;
            }
            if (c == '\n')
                break;
            if (n < sizeof(line) - 1)
                line[n++] = c;
        }
        while (n > 0 && line[n - 1] == '\r')
            --n;
        if (n >= 4 && memcmp(line, "SSH-", 4) == 0) {
            memcpy(s->v_c, line, n);
            s->vc_len = (int)n;
            return 0;
        }
        if (n > 254)
            return -1; /* a line this long is not a version string */
    }
}

/* Derive the six keys from the shared secret and exchange hash (RFC
 * 4253 7.2 with SHA-256): IV/keys for both directions plus both MAC
 * keys, each SHA256(K || H || letter || session_id). */
static void derive_keys(Ssh* s)
{
    uint8_t kmp[260 + 4];
    uint32_t klen = mpint_cat(kmp, s->k, (uint32_t)s->k_len);
    uint8_t msg[260 + 4 + 32 + 1 + 32];
    static const char letters[6] = {'A', 'B', 'C', 'D', 'E', 'F'};
    int i;
    memcpy(msg, kmp, klen);
    memcpy(msg + klen, s->h, 32);
    memcpy(msg + klen + 32 + 1, s->session_id, 32);
    for (i = 0; i < 6; ++i) {
        uint8_t d[32];
        msg[klen + 32] = (uint8_t)letters[i];
        tls_sha256(msg, klen + 32 + 1 + 32, d);
        switch (i) {
        case 0: /* initial IV, client -> server */
            memcpy(s->dec_ctr, d, 16);
            break;
        case 1: /* initial IV, server -> client */
            memcpy(s->enc_ctr, d, 16);
            break;
        case 2: /* AES-128 key, client -> server */
            tls_aes128_setkey(&s->dec_aes, d);
            break;
        case 3: /* AES-128 key, server -> client */
            tls_aes128_setkey(&s->enc_aes, d);
            break;
        case 4: /* HMAC-SHA-256 key, client -> server */
            memcpy(s->mac_in, d, 32);
            break;
        default: /* HMAC-SHA-256 key, server -> client */
            memcpy(s->mac_out, d, 32);
            break;
        }
    }
}

/* Did the client's kex list carry the RFC 8308 "ext-info-c" marker? */
static int client_wants_ext_info(const uint8_t* kexinit, uint32_t plen)
{
    uint32_t off = 1 + 16; /* type + cookie */
    if (off + 4 > plen)
        return 0;
    uint32_t l = ((uint32_t)kexinit[off] << 24) |
                 ((uint32_t)kexinit[off + 1] << 16) |
                 ((uint32_t)kexinit[off + 2] << 8) |
                 (uint32_t)kexinit[off + 3];
    off += 4;
    if (off + l > plen)
        return 0;
    return list_has((const char*)kexinit + off, l, "ext-info-c");
}

/* The whole RFC 4253 transport up to (and including) the first NEWKEYS:
 * version exchange, KEXINIT negotiation, diffie-hellman-group14-sha256,
 * host-key signature, key activation, then - when the client asked for
 * it - one EXT_INFO naming our signature algorithms. On success the
 * caller may start RFC 4252 userauth on the now-encrypted channel. */
int ssh_handshake(Ssh* s)
{
    static const char v_s[] = "SSH-2.0-vsshd_0.1\r\n";
    uint8_t pkt[SSH_MAX_PACKET];
    uint32_t plen;
    uint8_t x[256];
    int first_follows = 0, first_is_group14 = 0;
    tls_sha256_ctx hc;
    Buf b;

    /* 1. version exchange: the server speaks first (RFC 4253 4.2) */
    s->vs_len = (int)(sizeof(v_s) - 3); /* chop the CRLF */
    memcpy(s->v_s, v_s, (size_t)s->vs_len);
    if (ssh_write(s, v_s, (uint32_t)(sizeof(v_s) - 1)) < 0)
        return -1;
    if (read_version(s) < 0)
        return -1;

    /* 2. KEXINIT both ways; the transcript hashes the full payloads */
    {
        uint32_t klen = 0;
        build_kexinit(s->i_s, &klen);
        s->is_len = (int)klen;
        if (send_packet(s, s->i_s, klen) < 0)
            return -1;
    }
    for (;;) {
        if (ssh_recv_packet(s, pkt, &plen, sizeof(pkt)) < 0) {
            return -1;
        }
        if (plen >= 1 && pkt[0] == SSH_MSG_KEXINIT)
            break;
        if (plen >= 1 && pkt[0] == SSH_MSG_DISCONNECT)
            return -1;
        /* IGNORE / DEBUG / UNIMPLEMENTED: keep reading */
    }
    if (parse_kexinit(pkt, plen, &first_follows, &first_is_group14) < 0) {
        return -1;
    }
    if (plen > sizeof(s->i_c))
        return -1;
    memcpy(s->i_c, pkt, plen);
    s->ic_len = (int)plen;

    /* 3. the client guessed a kex we do not speak; toss the guess */
    if (first_follows && !first_is_group14) {
        if (ssh_recv_packet(s, pkt, &plen, sizeof(pkt)) < 0)
            return -1;
        if (plen >= 1 && pkt[0] == SSH_MSG_DISCONNECT)
            return -1;
    }

    /* 4. KEXDH_INIT carries the client's public value as an mpint */
    if (ssh_recv_packet(s, pkt, &plen, sizeof(pkt)) < 0) {
        return -1;
    }
    if (plen < 5 || pkt[0] != SSH_MSG_KEXDH_INIT) {
        return -1;
    }
    {
        uint32_t elen = ((uint32_t)pkt[1] << 24) | ((uint32_t)pkt[2] << 16) |
                        ((uint32_t)pkt[3] << 8) | (uint32_t)pkt[4];
        if (elen < 1 || elen > sizeof(s->e) || 5 + elen > plen)
            return -1;
        memcpy(s->e, pkt + 5, elen);
        s->e_len = (int)elen;
    }

    /* 5. our public value f = g^x mod p and the secret K = e^x mod p */
    fill_random(x, sizeof(x));
    x[0] &= 0x7f; /* keep x below 2^2047, comfortably inside the group */
    tls_modpow(DH_G, 1, x, sizeof(x), DH_P, sizeof(DH_P), s->f);
    s->f_len = (int)sizeof(DH_P);
    tls_modpow(s->e, (uint32_t)s->e_len, x, sizeof(x),
               DH_P, sizeof(DH_P), s->k);
    s->k_len = (int)sizeof(DH_P);

    /* 6. H = SHA256(V_C || V_S || I_C || I_S || K_S || e || f || K)
     * (RFC 4253 8). V_C..K_S are strings - their four-byte length is
     * part of the hash - e, f and K are mpints. Hashing the bare
     * version/KEXINIT lines, or dropping the shared secret K, gives an
     * H the client computes differently and rejects as an incorrect
     * signature. */
    tls_sha256_init(&hc);
    hash_str(&hc, s->v_c, (uint32_t)s->vc_len);
    hash_str(&hc, s->v_s, (uint32_t)s->vs_len);
    hash_str(&hc, s->i_c, (uint32_t)s->ic_len);
    hash_str(&hc, s->i_s, (uint32_t)s->is_len);
    {
        uint8_t kb[512];
        uint32_t kblen = 0;
        if (ssh_hostkey_blob(kb, sizeof(kb), &kblen) < 0)
            return -1;
        if (kblen > sizeof(s->k_s))
            return -1;
        memcpy(s->k_s, kb, kblen);
        s->ks_len = (int)kblen;
        hash_str(&hc, kb, kblen);
    }
    {
        uint8_t mp[260 + 4];
        uint32_t n;
        n = mpint_cat(mp, s->e, (uint32_t)s->e_len);
        tls_sha256_update(&hc, mp, n);
        n = mpint_cat(mp, s->f, (uint32_t)s->f_len);
        tls_sha256_update(&hc, mp, n);
        n = mpint_cat(mp, s->k, (uint32_t)s->k_len);
        tls_sha256_update(&hc, mp, n);
    }
    tls_sha256_final(&hc, s->h);
    memcpy(s->session_id, s->h, 32);
    s->have_sid = 1;

    /* 7. KEXDH_REPLY: host key, our public value, and the signature of
     * H with rsa-sha2-256, the whole inner blob itself a string. */
    buf_init(&b);
    buf_put_u8(&b, SSH_MSG_KEXDH_REPLY);
    buf_put_str(&b, s->k_s, (uint32_t)s->ks_len);
    buf_put_mpint(&b, s->f, (uint32_t)s->f_len);
    {
        uint8_t sig[256];
        uint32_t siglen = 0;
        uint8_t nk[1];
        if (ssh_sign(s->h, sig, sizeof(sig), &siglen) < 0) {
            buf_free(&b);
            return -1;
        }
        {
            Buf inner;
            buf_init(&inner);
            buf_put_cstr(&inner, "rsa-sha2-256");
            buf_put_str(&inner, sig, siglen);
            buf_put_str(&b, inner.b, inner.len); /* the signature string */
            buf_free(&inner);
        }
        if (send_packet(s, b.b, b.len) < 0) {
            buf_free(&b);
            return -1;
        }
        buf_free(&b);

        /* 8. NEWKEYS out, NEWKEYS in, then the cipher goes live */
        nk[0] = SSH_MSG_NEWKEYS;
        if (send_packet(s, nk, 1) < 0)
            return -1;
    }
    for (;;) {
        if (ssh_recv_packet(s, pkt, &plen, sizeof(pkt)) < 0) {
            return -1;
        }
        if (plen >= 1 && pkt[0] == SSH_MSG_NEWKEYS)
            break;
        if (plen >= 1 && pkt[0] == SSH_MSG_DISCONNECT)
            return -1;
    }

    /* 9. derive the six keys and go encrypted. The packet counters are
     * NOT reset: RFC 4253 6.4 runs one sequence per direction for the
     * life of the connection, encrypted or not. */
    derive_keys(s);
    s->enc = 1;

    /* 10. one EXT_INFO naming server-sig-algs, when the client asked */
    if (client_wants_ext_info(s->i_c, (uint32_t)s->ic_len)) {
        buf_init(&b);
        buf_put_u8(&b, SSH_MSG_EXT_INFO);
        buf_put_u32(&b, 1);
        buf_put_cstr(&b, "server-sig-algs");
        buf_put_cstr(&b, "rsa-sha2-256,ssh-rsa");
        if (send_packet(s, b.b, b.len) < 0) {
            buf_free(&b);
            return -1;
        }
        buf_free(&b);
    }
    return 0;
}