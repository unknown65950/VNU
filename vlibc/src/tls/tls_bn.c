/*
 * tls_bn.c — fixed-size big integers for the RSA public-key ops used by
 * the TLS 1.2 client (RFC 8017: RSAEP through modpow with e = 65537).
 *
 * Numbers live as uint32_t words, little-endian limb order. Capacity is
 * TLS_RSA_MAX_MODULUS bytes (128 limbs by default), enough for 4096-bit
 * keys. Reduction uses simple bitwise binary division: slow but tiny and
 * obviously correct — fine for a 17-squaring handshake.
 */

#include <vlibc/tls.h>
#include <vlibc/string.h>

#define BN_LIMBS ((TLS_RSA_MAX_MODULUS + 3u) / 4u) /* 128 */

/* zero `n` limbs */
static void bn_zero(uint32_t* a, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; ++i)
        a[i] = 0;
}

/* parse big-endian bytes into `n` limbs (little-endian limb order) */
static void bn_from_bytes(uint32_t* a, unsigned n, const uint8_t* b, uint32_t blen)
{
    unsigned i;
    bn_zero(a, n);
    for (i = 0; i < blen; ++i) {
        unsigned pos = (blen - 1u - i) * 8u; /* bit offset from LSB */
        unsigned limb = pos >> 5;
        unsigned shift = pos & 31u;
        if (limb < n)
            a[limb] |= (uint32_t)b[i] << shift;
    }
}

/* write `na` words into n little-endian-aligned bytes, big-endian output */
static void bn_to_bytes(const uint32_t* a, unsigned na, uint8_t* out, uint32_t n)
{
    unsigned i;
    memset(out, 0, n);
    for (i = 0; i < n; ++i) {
        unsigned pos = (n - 1u - i) * 8u; /* bit offset from LSB */
        unsigned limb = pos >> 5;
        unsigned shift = pos & 31u;
        if (limb < na)
            out[i] = (uint8_t)(a[limb] >> shift);
    }
}

/* highest used limb + 1 */
static unsigned bn_used(const uint32_t* a, unsigned n)
{
    while (n && a[n - 1] == 0)
        --n;
    return n;
}

/* returns 1 if a > b, 0 if equal, -1 if a < b (n limbs each) */
static int bn_cmp(const uint32_t* a, const uint32_t* b, unsigned n)
{
    while (n) {
        unsigned i = n - 1;
        if (a[i] != b[i])
            return a[i] > b[i] ? 1 : -1;
        --n;
    }
    return 0;
}

/* a -= b (n limbs, caller ensures a >= b) */
static void bn_sub(uint32_t* a, const uint32_t* b, unsigned n)
{
    unsigned i;
    uint32_t borrow = 0;
    for (i = 0; i < n; ++i) {
        uint64_t ai = a[i];
        uint64_t sub = (uint64_t)b[i] + borrow;
        a[i] = (uint32_t)(ai - sub);
        borrow = ai < sub ? 1u : 0u;
    }
}

/* bit index of the highest set bit + 1; 0 if x is zero */
static unsigned bn_bitlen(const uint32_t* a, unsigned n)
{
    unsigned top = bn_used(a, n);
    if (top == 0)
        return 0;
    unsigned bits = top * 32u;
    uint32_t v = a[top - 1];
    while ((v & 0x80000000u) == 0) {
        v <<= 1;
        --bits;
    }
    return bits;
}

static uint32_t bn_getbit(const uint32_t* a, unsigned bit)
{
    return (a[bit >> 5] >> (bit & 31u)) & 1u;
}

/* prod = a * b; `prod` must hold alim+blim limbs */
static void bn_mul(uint32_t* prod,
                   const uint32_t* a, unsigned alim,
                   const uint32_t* b, unsigned blim)
{
    unsigned i, j;
    bn_zero(prod, alim + blim + 1);
    for (i = 0; i < alim; ++i) {
        uint64_t carry = 0;
        if (a[i] == 0)
            continue;
        for (j = 0; j < blim; ++j) {
            uint64_t cur = (uint64_t)a[i] * (uint64_t)b[j] +
                           (uint64_t)prod[i + j] + carry;
            prod[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        {
            uint32_t k = i + blim;
            while (carry) {
                uint64_t cur = (uint64_t)prod[k] + carry;
                prod[k] = (uint32_t)cur;
                carry = cur >> 32;
                ++k;
            }
        }
    }
}

/* rem = x mod m; x has xlimbs (may exceed m), rem holds L+1 limbs */
static void bn_mod(uint32_t rem[BN_LIMBS + 1], const uint32_t* x, unsigned xlimbs,
                   const uint32_t* m, unsigned mlimbs)
{
    unsigned bits = bn_bitlen(x, xlimbs);
    unsigned i;
    bn_zero(rem, BN_LIMBS + 1);
    if (bn_used(x, xlimbs) <= mlimbs && bn_cmp(x, m, mlimbs) < 0) {
        unsigned used = bn_used(x, xlimbs);
        memcpy(rem, x, (used < mlimbs ? used : mlimbs) * sizeof(uint32_t));
        return;
    }
    for (i = bits; i > 0; --i) {
        unsigned bit = i - 1;
        unsigned j;
        uint32_t carry = bn_getbit(x, bit);
        /* rem = rem << 1 | carry */
        for (j = 0; j <= mlimbs; ++j) {
            uint32_t nc = rem[j] >> 31;
            rem[j] = (rem[j] << 1) | carry;
            carry = nc;
        }
        /* if rem >= m: rem -= m (compare over mlimbs+1, m padded) */
        if (bn_cmp(rem, m, mlimbs + 1) >= 0) {
            bn_sub(rem, m, mlimbs + 1);
        }
    }
}

void tls_modpow(const uint8_t* base, uint32_t base_len,
                const uint8_t* exp, uint32_t exp_len,
                const uint8_t* mod, uint32_t mod_len,
                uint8_t* out)
{
    uint32_t b[BN_LIMBS];
    uint32_t m[BN_LIMBS + 1];
    uint32_t e[BN_LIMBS];
    uint32_t r[BN_LIMBS];
    uint32_t prod[2 * BN_LIMBS + 2];
    uint32_t rem[BN_LIMBS + 1];
    unsigned mlimbs, elimb, elen, bi;

    if (mod_len > TLS_RSA_MAX_MODULUS)
        mod_len = TLS_RSA_MAX_MODULUS;

    bn_from_bytes(m, BN_LIMBS, mod, mod_len);
    mlimbs = bn_used(m, BN_LIMBS);
    if (mlimbs == 0) {
        memset(out, 0, mod_len);
        return;
    }
    bn_from_bytes(e, BN_LIMBS, exp, exp_len);
    elimb = bn_used(e, BN_LIMBS);
    elen = bn_bitlen(e, BN_LIMBS);

    bn_from_bytes(b, BN_LIMBS, base, base_len);
    /* b %= m */
    if (bn_cmp(b, m, mlimbs) >= 0)
        bn_mod(rem, b, BN_LIMBS, m, mlimbs), memcpy(b, rem, BN_LIMBS * 4u);
    /* r = 1 */
    bn_zero(r, BN_LIMBS);
    r[0] = 1;

    for (bi = elen; bi > 0; --bi) {
        /* r = r*r mod m */
        bn_mul(prod, r, BN_LIMBS, r, BN_LIMBS);
        bn_mod(rem, prod, 2 * BN_LIMBS, m, mlimbs);
        memcpy(r, rem, BN_LIMBS * 4u);
        if (bn_getbit(e, bi - 1)) {
            /* r = r*b mod m */
            bn_mul(prod, r, BN_LIMBS, b, BN_LIMBS);
            bn_mod(rem, prod, 2 * BN_LIMBS, m, mlimbs);
            memcpy(r, rem, BN_LIMBS * 4u);
        }
    }
    (void)elimb;
    bn_to_bytes(r, BN_LIMBS, out, mod_len);
}

int tls_rsa_encrypt_pkcs1(const uint8_t* n, uint32_t nlen,
                          const uint8_t* m, uint32_t mlen,
                          uint8_t* out)
{
    uint8_t block[TLS_RSA_MAX_MODULUS];
    uint8_t e[3] = {0x01, 0x00, 0x01};
    uint32_t pslen, i;

    if (nlen > TLS_RSA_MAX_MODULUS || nlen < 12)
        return -1;
    if (mlen > nlen - 11u)
        return -1;

    /* PKCS#1 v1.5 type 2: 00 02 | PS (>=8 nonzero) | 00 | M */
    pslen = nlen - mlen - 3u;
    block[0] = 0x00;
    block[1] = 0x02;
    {
        /* Deterministic-but-valid padding: fill PS with a fixed non-zero
         * pattern. Characters only for demos/tests; a real PRNG would go
         * here. (Padding is decrypted by the server — never replayed.) */
        for (i = 0; i < pslen; ++i)
            block[2 + i] = (uint8_t)(0x01u + ((i * 37u + 11u) & 0xFEu));
    }
    block[2 + pslen] = 0x00;
    memcpy(block + 3u + pslen, m, mlen);

    tls_modpow(block, nlen, e, 3, n, nlen, out);
    return 0;
}