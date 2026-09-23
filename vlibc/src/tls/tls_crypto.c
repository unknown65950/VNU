/*
 * tls_crypto.c — hash/MAC/AES/GCM primitives for the VNU TLS 1.2 client.
 *
 * Self-contained, freestanding C99. Only <vlibc/tls.h> and the string
 * helpers (memcpy/memset) are used, so this file compiles unchanged on
 * any host toolchain for test purposes.
 */

#include <vlibc/tls.h>
#include <vlibc/string.h>

/* ------------------------------------------------------------------ */
/* SHA-256 (FIPS 180-4)                                                */
/* ------------------------------------------------------------------ */

static const uint32_t SHA_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static uint32_t be32_load(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void be32_store(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void sha256_block(tls_sha256_ctx* c, const uint8_t* p)
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h, t1, t2;
    unsigned i;

    for (i = 0; i < 16; ++i)
        w[i] = be32_load(p + 4 * i);
    for (i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];

    for (i = 0; i < 64; ++i) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        t1 = h + S1 + ch + SHA_K[i] + w[i];
        t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void tls_sha256_init(tls_sha256_ctx* ctx)
{
    ctx->h[0] = 0x6a09e667u; ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u; ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu; ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu; ctx->h[7] = 0x5be0cd19u;
    ctx->bits = 0;
    ctx->buflen = 0;
}

void tls_sha256_update(tls_sha256_ctx* ctx, const uint8_t* m, size_t len)
{
    ctx->bits += (uint64_t)len * 8u;
    while (len) {
        size_t take = sizeof(ctx->buf) - (size_t)ctx->buflen;
        if (take > len)
            take = len;
        memcpy(ctx->buf + ctx->buflen, m, take);
        ctx->buflen += (uint32_t)take;
        m += take;
        len -= take;
        if (ctx->buflen == sizeof(ctx->buf)) {
            sha256_block(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
}

void tls_sha256_final(tls_sha256_ctx* ctx, uint8_t out[TLS_SHA256_LEN])
{
    uint64_t bits = ctx->bits;
    uint8_t pad[64] = {0};
    uint8_t bl[8];
    size_t zero_count;
    unsigned i;

    /* Append 0x80 then zeros until the buffer length == 56 (mod 64). */
    pad[0] = 0x80;
    zero_count = (56u - ((uint32_t)ctx->buflen + 1u)) & 63u;
    tls_sha256_update(ctx, pad, zero_count + 1u);

    for (i = 0; i < 8; ++i)
        bl[7 - i] = (uint8_t)(bits >> (8u * i));
    tls_sha256_update(ctx, bl, 8);

    for (i = 0; i < 8; ++i)
        be32_store(out + 4 * i, ctx->h[i]);
}

void tls_sha256(const uint8_t* m, size_t len, uint8_t out[TLS_SHA256_LEN])
{
    tls_sha256_ctx c;
    tls_sha256_init(&c);
    tls_sha256_update(&c, m, len);
    tls_sha256_final(&c, out);
}

/* ------------------------------------------------------------------ */
/* HMAC-SHA-256 (RFC 2104)                                             */
/* ------------------------------------------------------------------ */

void tls_hmac_sha256(const uint8_t* key, size_t keylen,
                     const uint8_t* msg, size_t msglen,
                     uint8_t out[TLS_SHA256_LEN])
{
    uint8_t k[64] = {0};
    uint8_t ipad[64], opad[64];
    tls_sha256_ctx c;
    size_t i;

    if (keylen > 64) {
        tls_sha256(key, keylen, k);
    } else if (keylen) {
        memcpy(k, key, keylen);
    }
    for (i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36u;
        opad[i] = k[i] ^ 0x5cu;
    }

    tls_sha256_init(&c);
    tls_sha256_update(&c, ipad, 64);
    tls_sha256_update(&c, msg, msglen);
    tls_sha256_final(&c, out); /* inner = H(K^ipad || m) */

    tls_sha256_init(&c);
    tls_sha256_update(&c, opad, 64);
    tls_sha256_update(&c, out, TLS_SHA256_LEN);
    tls_sha256_final(&c, out);
}

/* ------------------------------------------------------------------ */
/* TLS 1.2 PRF (P_SHA256, RFC 5246 $5)                                 */
/* ------------------------------------------------------------------ */

void tls_prf(const uint8_t* secret, size_t secretlen,
             const uint8_t* seed, size_t seedlen,
             uint8_t* out, size_t out_len)
{
    uint8_t a[32];
    uint8_t a_next[32];
    uint8_t buf[32 + 128];
    size_t off = 0;

    tls_hmac_sha256(secret, secretlen, seed, seedlen, a);
    while (off < out_len) {
        size_t n = out_len - off;
        if (n > 32)
            n = 32;
        /* buf = A || seed */
        memcpy(buf, a, 32);
        if (seedlen > 128)
            return; /* cannot happen with our labels; guard anyway */
        memcpy(buf + 32, seed, seedlen);
        tls_hmac_sha256(secret, secretlen, buf, 32 + seedlen, out + off);
        tls_hmac_sha256(secret, secretlen, a, 32, a_next);
        memcpy(a, a_next, 32);
        off += n;
    }
}

/* ------------------------------------------------------------------ */
/* AES-128 (FIPS 197)                                                  */
/* ------------------------------------------------------------------ */

static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
    0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
    0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
    0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
    0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
    0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
    0xb0, 0x54, 0xbb, 0x16,
};

static const uint8_t INV_SBOX[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e,
    0x81, 0xf3, 0xd7, 0xfb, 0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87,
    0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb, 0x54, 0x7b, 0x94, 0x32,
    0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49,
    0x6d, 0x8b, 0xd1, 0x25, 0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16,
    0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92, 0x6c, 0x70, 0x48, 0x50,
    0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05,
    0xb8, 0xb3, 0x45, 0x06, 0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02,
    0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b, 0x3a, 0x91, 0x11, 0x41,
    0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8,
    0x1c, 0x75, 0xdf, 0x6e, 0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89,
    0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b, 0xfc, 0x56, 0x3e, 0x4b,
    0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59,
    0x27, 0x80, 0xec, 0x5f, 0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d,
    0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef, 0xa0, 0xe0, 0x3b, 0x4d,
    0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63,
    0x55, 0x21, 0x0c, 0x7d,
};

static const uint8_t RCON[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36,
};

static uint32_t xtime(uint32_t v)
{
    return ((v << 1) ^ ((((v >> 7) & 1) != 0) ? 0x1bu : 0u));
}

static void aes_addroundkey(uint8_t s[16], const uint8_t rk[16])
{
    unsigned i;
    for (i = 0; i < 16; ++i)
        s[i] ^= rk[i];
}

static void aes_subbytes(uint8_t s[16], const uint8_t* box)
{
    unsigned i;
    for (i = 0; i < 16; ++i)
        s[i] = box[s[i]];
}

static void aes_shiftrows(uint8_t s[16])
{
    uint8_t t;
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    t = s[2]; s[2] = s[10]; s[10] = t; /* row 2 rotates by 2: pairwise swap */
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}

static void aes_invshiftrows(uint8_t s[16])
{
    uint8_t t;
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
}

static uint8_t gm2(uint8_t b)
{
    return (uint8_t)xtime((uint32_t)b);
}

static uint8_t gm3(uint8_t b)
{
    return (uint8_t)(gm2(b) ^ b);
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    unsigned i;
    for (i = 0; i < 8; ++i) {
        if (b & 1)
            p ^= a;
        a = gm2(a);
        b >>= 1;
    }
    return p;
}

static void aes_mixcolumns(uint8_t s[16])
{
    unsigned c;
    for (c = 0; c < 4; ++c) {
        uint8_t* x = s + 4 * c;
        uint8_t a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
        x[0] = gm2(a0) ^ gm3(a1) ^ a2 ^ a3;
        x[1] = a0 ^ gm2(a1) ^ gm3(a2) ^ a3;
        x[2] = a0 ^ a1 ^ gm2(a2) ^ gm3(a3);
        x[3] = gm3(a0) ^ a1 ^ a2 ^ gm2(a3);
    }
}

static void aes_invmixcolumns(uint8_t s[16])
{
    unsigned c;
    for (c = 0; c < 4; ++c) {
        uint8_t* x = s + 4 * c;
        uint8_t a0 = x[0], a1 = x[1], a2 = x[2], a3 = x[3];
        x[0] = (uint8_t)(gmul(a0, 0x0eu) ^ gmul(a1, 0x0bu) ^ gmul(a2, 0x0du) ^ gmul(a3, 0x09u));
        x[1] = (uint8_t)(gmul(a0, 0x09u) ^ gmul(a1, 0x0eu) ^ gmul(a2, 0x0bu) ^ gmul(a3, 0x0du));
        x[2] = (uint8_t)(gmul(a0, 0x0du) ^ gmul(a1, 0x09u) ^ gmul(a2, 0x0eu) ^ gmul(a3, 0x0bu));
        x[3] = (uint8_t)(gmul(a0, 0x0bu) ^ gmul(a1, 0x0du) ^ gmul(a2, 0x09u) ^ gmul(a3, 0x0eu));
    }
}

void tls_aes128_setkey(tls_aes128* a, const uint8_t key[TLS_AES_BLOCK])
{
    unsigned i, k;

    memcpy(a->rk[0], key, 16);
    for (i = 1; i < 11; ++i) {
        a->rk[i][0] = SBOX[a->rk[i - 1][13]] ^ RCON[i] ^ a->rk[i - 1][0];
        a->rk[i][1] = SBOX[a->rk[i - 1][14]] ^ a->rk[i - 1][1];
        a->rk[i][2] = SBOX[a->rk[i - 1][15]] ^ a->rk[i - 1][2];
        a->rk[i][3] = SBOX[a->rk[i - 1][12]] ^ a->rk[i - 1][3];
        for (k = 0; k < 4; ++k) {
            a->rk[i][4 + k] = a->rk[i][k] ^ a->rk[i - 1][4 + k];
            a->rk[i][8 + k] = a->rk[i][4 + k] ^ a->rk[i - 1][8 + k];
            a->rk[i][12 + k] = a->rk[i][8 + k] ^ a->rk[i - 1][12 + k];
        }
    }
}

void tls_aes128_enc(const tls_aes128* a, const uint8_t in[TLS_AES_BLOCK],
                    uint8_t out[TLS_AES_BLOCK])
{
    uint8_t s[16];
    unsigned round, i;

    for (i = 0; i < 16; ++i)
        s[i] = in[i];
    aes_addroundkey(s, a->rk[0]);
    for (round = 1; round < 10; ++round) {
        aes_subbytes(s, SBOX);
        aes_shiftrows(s);
        aes_mixcolumns(s);
        aes_addroundkey(s, a->rk[round]);
    }
    aes_subbytes(s, SBOX);
    aes_shiftrows(s);
    aes_addroundkey(s, a->rk[10]);

    for (i = 0; i < 16; ++i)
        out[i] = s[i];
}

void tls_aes128_dec(const tls_aes128* a, const uint8_t in[TLS_AES_BLOCK],
                    uint8_t out[TLS_AES_BLOCK])
{
    uint8_t s[16];
    unsigned round, i;

    for (i = 0; i < 16; ++i)
        s[i] = in[i];
    aes_addroundkey(s, a->rk[10]);
    for (round = 9; round >= 1; --round) {
        aes_invshiftrows(s);
        aes_subbytes(s, INV_SBOX);
        aes_addroundkey(s, a->rk[round]);
        aes_invmixcolumns(s);
    }
    aes_invshiftrows(s);
    aes_subbytes(s, INV_SBOX);
    aes_addroundkey(s, a->rk[0]);

    for (i = 0; i < 16; ++i)
        out[i] = s[i];
}

/* ------------------------------------------------------------------ */
/* AES-128-GCM (NIST SP 800-38D)                                       */
/* ------------------------------------------------------------------ */

static void gcm_inc32(uint8_t ctr[16])
{
    unsigned i;
    for (i = 0; i < 4; ++i) {
        unsigned idx = 15 - i;
        if (++ctr[idx])
            break;
    }
}

/* Reverse the order of the 128 bits of a block (own inverse). GHASH maps a
 * string to a field element by taking the least significant bit of byte 15
 * as the coefficient of x^127 (bit-reverse each byte, then reverse bytes),
 * so absorb works on gcm_rev128()'d blocks. */
static void gcm_rev128(uint8_t out[16], const uint8_t in[16])
{
    unsigned i;
    for (i = 0; i < 16; ++i) {
        uint8_t v = in[15 - i];
        v = (uint8_t)(((v & 0x0fu) << 4) | ((v & 0xf0u) >> 4));
        v = (uint8_t)(((v & 0x33u) << 2) | ((v & 0xccu) >> 2));
        v = (uint8_t)(((v & 0x55u) << 1) | ((v & 0xaau) >> 1));
        out[i] = v;
    }
}

/* z ^= x (both 16 bytes) */
static void gf_xor(uint8_t z[16], const uint8_t x[16])
{
    unsigned i;
    for (i = 0; i < 16; ++i)
        z[i] ^= x[i];
}

/* acc = (acc XOR block) * H over GF(2^128).
 * Operates in the field representation where bit i (LSB-first) is the
 * coefficient of x^i: blocks are gcm_rev128()'d on the way in and the
 * accumulator is reversed back before it is used as a string.
 * V holds H * x^i; multiplying by x is a left shift with reduction. */
static void ghash_absorb(uint8_t acc[16], const uint8_t block[16],
                         const uint8_t H[16])
{
    uint8_t x[16], bf[16], v[16], z[16] = {0};
    int bit;

    gcm_rev128(bf, block); /* string -> field element */
    for (bit = 0; bit < 16; ++bit)
        x[bit] = acc[bit] ^ bf[bit];
    memcpy(v, H, 16);

    for (bit = 0; bit < 128; ++bit) {
        /* bit i of the field integer (coefficient x^i) is byte 15 -
         * (i / 8), bit (i % 8), since byte 0 is the most significant. */
        uint8_t xb = (uint8_t)((x[15 - (bit >> 3)] >> (bit & 7)) & 1u);
        unsigned j;
        uint8_t carry;
        if (xb)
            gf_xor(z, v);
        /* v *= x: left-shift the 128-bit value, reduce x^128 -> x^7+x^2+x+1 */
        carry = v[0] >> 7;
        for (j = 0; j < 15; ++j)
            v[j] = (uint8_t)((v[j] << 1) | (v[j + 1] >> 7));
        v[15] = (uint8_t)(v[15] << 1);
        if (carry)
            v[15] ^= 0x87u; /* x^128 => x^7+x^2+x+1, into the low byte */
    }
    memcpy(acc, z, 16);
}

/* GCTR: out[i] = in[i] XOR AES(K, counter); counter advances per block */
static void gcm_gctr(const tls_aes128* a, uint8_t ctr[16],
                     const uint8_t* in, uint8_t* out, size_t len)
{
    unsigned i;
    size_t off = 0;
    while (off < len) {
        uint8_t e[16];
        size_t n = len - off;
        tls_aes128_enc(a, ctr, e);
        if (n > 16)
            n = 16;
        for (i = 0; i < n; ++i)
            out[off + i] = in[off + i] ^ e[i];
        off += n;
        gcm_inc32(ctr);
    }
}

static void gcm_hash_final(const uint8_t* aad, size_t aadlen,
                           const uint8_t* ct, size_t ctlen,
                           const uint8_t H[16], uint8_t S[16])
{
    unsigned i;
    size_t off = 0;
    uint8_t lenblk[16] = {0};
    uint8_t Hf[16], Sr[16];

    gcm_rev128(Hf, H); /* string -> field element for the multiplications */

    while (off < aadlen) {
        uint8_t blk[16];
        size_t n = aadlen - off;
        if (n > 16)
            n = 16;
        memcpy(blk, aad + off, n);
        for (i = n; i < 16; ++i)
            blk[i] = 0;
        ghash_absorb(S, blk, Hf);
        off += n;
    }

    off = 0;
    while (off < ctlen) {
        uint8_t blk[16];
        size_t n = ctlen - off;
        if (n > 16)
            n = 16;
        memcpy(blk, ct + off, n);
        for (i = n; i < 16; ++i)
            blk[i] = 0;
        ghash_absorb(S, blk, Hf);
        off += n;
    }

    for (i = 0; i < 8; ++i) {
        lenblk[i] = (uint8_t)(((uint64_t)aadlen * 8u) >> (8u * (7 - i)));
        lenblk[8 + i] = (uint8_t)(((uint64_t)ctlen * 8u) >> (8u * (7 - i)));
    }
    ghash_absorb(S, lenblk, Hf);

    /* S is in field representation; give the caller the string form. */
    gcm_rev128(Sr, S); /* separate buffer: not in-place safe */
    memcpy(S, Sr, 16);
}

void tls_gcm_seal(const uint8_t key[TLS_AES_BLOCK],
                  const uint8_t nonce[12],
                  const uint8_t* aad, size_t aadlen,
                  const uint8_t* pt, size_t ptlen,
                  uint8_t* ct)
{
    tls_aes128 a;
    uint8_t H[16] = {0};
    uint8_t J0[16], Jc[16];
    uint8_t S[16] = {0};
    uint8_t T[16];

    tls_aes128_setkey(&a, key);
    tls_aes128_enc(&a, H, H);

    memcpy(J0, nonce, 12);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    /* Data blocks use inc32(J0) as the first counter; J0 stays for the tag. */
    memcpy(Jc, J0, 16);
    gcm_inc32(Jc);
    gcm_gctr(&a, Jc, pt, ct, ptlen);
    gcm_hash_final(aad, aadlen, ct, ptlen, H, S);

    /* T = E(K, J0) XOR S */
    gcm_gctr(&a, J0, S, T, 16);
    memcpy(ct + ptlen, T, 16);
}

int tls_gcm_open(const uint8_t key[TLS_AES_BLOCK],
                 const uint8_t nonce[12],
                 const uint8_t* aad, size_t aadlen,
                 const uint8_t* ct, size_t ctlen,
                 uint8_t* pt, uint8_t tag[TLS_GCM_TAG])
{
    tls_aes128 a;
    uint8_t H[16] = {0};
    uint8_t J0[16], Jc[16];
    uint8_t S[16] = {0};
    uint8_t T[16];
    size_t ptlen = ctlen - TLS_GCM_TAG;
    unsigned i, bad = 0;

    if (ctlen < TLS_GCM_TAG)
        return -1;

    tls_aes128_setkey(&a, key);
    tls_aes128_enc(&a, H, H);

    memcpy(J0, nonce, 12);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    memcpy(Jc, J0, 16);
    gcm_inc32(Jc);
    gcm_gctr(&a, Jc, ct, pt, ptlen);
    gcm_hash_final(aad, aadlen, ct, ptlen, H, S);
    gcm_gctr(&a, J0, S, T, 16);

    if (tag)
        memcpy(tag, T, 16);
    for (i = 0; i < TLS_GCM_TAG; ++i)
        bad |= (unsigned)(T[i] ^ ct[ptlen + i]);
    if (bad) {
        /* Never leak the keystream to the caller on a failed open. */
        memset(pt, 0, ptlen);
        return -1;
    }
    return 0;
}