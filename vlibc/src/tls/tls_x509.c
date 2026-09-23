/*
 * tls_x509.c — minimal DER (X.509) parser: pulls the RSA public key out
 * of a PEM-less DER Certificate so the TLS handshake can nonce-wrap the
 * pre-master secret. Self-contained C99, bounds-checked everywhere.
 */

#include <vlibc/tls.h>
#include <vlibc/string.h>

/* rsaEncryption OID = 1.2.840.113549.1.1.1 (DER content, no tag/len) */
static const uint8_t OID_RSA_ENCRYPTION[9] = {
    0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01,
};

struct der {
    uint8_t tag;
    const uint8_t* p; /* value */
    size_t len;
    size_t hlen;      /* tag + length bytes */
};

/* Parse one TLV at `off`, advancing it past the element. */
static int der_next(const uint8_t* b, size_t blen, size_t* off, struct der* t)
{
    size_t i, nb;
    uint8_t l1;
    size_t l;

    if (*off >= blen)
        return -1;
    t->tag = b[*off];
    if ((t->tag & 0x1fu) == 0x1fu)
        return -1; /* multi-byte tags unsupported */
    i = *off + 1;
    if (i >= blen)
        return -1;
    l1 = b[i++];
    if (l1 & 0x80u) {
        nb = (size_t)(l1 & 0x7fu);
        if (nb == 0 || nb > 4 || i + nb > blen)
            return -1;
        l = 0;
        while (nb--) {
            l = (l << 8) | b[i++];
        }
    } else {
        l = l1;
    }
    if (l > blen - i)
        return -1;
    t->p = b + i;
    t->len = l;
    t->hlen = i - *off;
    *off = i + l;
    return 0;
}

/* Check the value of an OID TLV against a known OID. */
static int oid_eq(const struct der* d, const uint8_t* oid, size_t oidlen)
{
    size_t i;
    if (d->tag != 0x06 || d->len != oidlen)
        return 0;
    for (i = 0; i < oidlen; ++i)
        if (d->p[i] != oid[i])
            return 0;
    return 1;
}

/* Copy a DER INTEGER value into out as big-endian bytes, stripping a
 * leading 0x00 sign byte and (defensively) fixing a 0x80 high bit. */
static int int_bytes(const struct der* d, uint8_t* out, uint32_t* outlen)
{
    const uint8_t* v = d->p;
    size_t n = d->len;

    if (d->tag != 0x02 || n == 0)
        return -1;
    if (v[0] & 0x80u) /* negative: value has a sign byte, skip it */
        v++, n--;
    while (n > 1 && v[0] == 0x00)
        v++, n--; /* strip leading zeros */
    if (n == 0 || n > *outlen)
        return -1;
    memcpy(out, v, n);
    *outlen = (uint32_t)n;
    return 0;
}

int tls_x509_rsa_pubkey(const uint8_t* der, size_t len,
                        uint8_t* mod_out, uint32_t* mod_len,
                        uint8_t* exp_out, uint32_t* exp_len)
{
    struct der cert, tbs, child, alg, oid, bitstr, key, ki;
    size_t off = 0, off2;

    /* Certificate ::= SEQUENCE { TBSCertificate, signatureAlgorithm, ... } */
    if (der_next(der, len, &off, &cert) != 0 || cert.tag != 0x30)
        return -1;

    /* TBSCertificate ::= SEQUENCE { ..., subjectPublicKeyInfo, ... } */
    off2 = 0;
    if (der_next(cert.p, cert.len, &off2, &tbs) != 0 || tbs.tag != 0x30)
        return -1;

    /* Iterate TBSCertificate children, hunting for SubjectPublicKeyInfo. */
    off2 = 0;
    while (der_next(tbs.p, tbs.len, &off2, &child) == 0) {
        size_t ao;

        if (child.tag != 0x30)
            continue; /* [0]/INTEGER/validity/... */

        /* SubjectPublicKeyInfo ::= SEQUENCE {
         *     AlgorithmIdentifier ::= SEQUENCE { OID rsaEncryption, ... },
         *     BIT STRING } */
        ao = 0;
        if (der_next(child.p, child.len, &ao, &alg) != 0 || alg.tag != 0x30)
            continue;
        {
            size_t aoa = 0;
            if (der_next(alg.p, alg.len, &aoa, &oid) != 0)
                continue;
            if (!oid_eq(&oid, OID_RSA_ENCRYPTION, sizeof(OID_RSA_ENCRYPTION)))
                continue;
        }

        /* The BIT STRING right after the AlgorithmIdentifier. */
        if (der_next(child.p, child.len, &ao, &bitstr) != 0 || bitstr.tag != 0x03)
            return -1;
        if (bitstr.len < 1 || bitstr.p[0] != 0x00)
            return -1; /* unused-bits must be 0 for an RSA key */

        /* RSAPublicKey ::= SEQUENCE { INTEGER modulus, INTEGER exponent } */
        {
            size_t ko = 1; /* skip the unused-bits byte */
            if (der_next(bitstr.p, bitstr.len, &ko, &key) != 0 || key.tag != 0x30)
                return -1;
            ko = 0;
            if (der_next(key.p, key.len, &ko, &ki) != 0 || ki.tag != 0x02)
                return -1;
            if (int_bytes(&ki, mod_out, mod_len) != 0)
                return -1;
            if (der_next(key.p, key.len, &ko, &ki) != 0 || ki.tag != 0x02)
                return -1;
            if (int_bytes(&ki, exp_out, exp_len) != 0)
                return -1;
        }
        return 0;
    }
    return -1;
}