/*
 * tls.h — TLS 1.2 client library for VNU (self-contained POSIX flavour).
 *
 * Zero kernel/VNU dependencies: the handshake and record layer talk to
 * the network only through the two transport callbacks in tls_stream,
 * so the same code runs unchanged on any host OS (that is how the
 * library is validated: qemu guest <-> host test harness <-> openssl).
 *
 * Cipher suite implemented: TLS_RSA_WITH_AES_128_GCM_SHA256 (0x009c).
 * TLS 1.2 only; SNI + RSA key exchange; no client certificates; no
 * renegotiation; close_notify handled. Certificate chain verification
 * is TODO — the leaf RSA key is parsed and used, hostname is sent as
 * SNI and matched against the certificate SAN/CN when available.
 */
#ifndef VLIBC_TLS_H
#define VLIBC_TLS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Transport abstraction                                               */
/* ------------------------------------------------------------------ */

typedef struct tls_stream {
    void* ctx;
    /* Send/recv bytes. Return bytes transferred, 0 on EOF, -errno on
     * error. Both must honour `timeout_ms` (<=0: wait forever). */
    int (*send_fn)(void* ctx, const void* data, uint32_t len, uint32_t timeout_ms);
    int (*recv_fn)(void* ctx, void* buf, uint32_t len, uint32_t timeout_ms);
    /* Randomness source. Write `len` unpredictable bytes into buf.
     * Return 0 on success, -errno on failure (handshake aborts). May be
     * NULL when the caller pre-seeds tls_conn via tls_seed() instead. */
    int (*rng_fn)(void* ctx, void* buf, uint32_t len);
} tls_stream;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

typedef struct tls_conn tls_conn;

/* Full TLS 1.2 client handshake over `stream`. `host` (may be NULL) is
 * fed into SNI and, when possible, matched against the server cert.
 * Returns a connection or NULL (see tls_last_error()).
 * timeout_ms bounds the whole handshake. */
tls_conn* tls_connect(tls_stream* stream, const char* host, uint32_t timeout_ms);

/* Write plaintext (blocks until everything is written or the timeout
 * expires). Returns bytes written or -errno. */
int tls_write(tls_conn* c, const void* buf, uint32_t len, uint32_t timeout_ms);

/* Read plaintext. Returns bytes read, 0 at close_notify/EOF, -errno. */
int tls_read(tls_conn* c, void* buf, uint32_t len, uint32_t timeout_ms);

/* Shut the TLS connection down (close_notify) and free resources. */
int tls_close(tls_conn* c);

/* Human-readable message for the last failure (static buffer). */
const char* tls_last_error(void);

/* ------------------------------------------------------------------ */
/* Cryptography primitives (public so the library is unit-testable)    */
/* ------------------------------------------------------------------ */

#define TLS_SHA256_LEN 32
#define TLS_AES_BLOCK  16
#define TLS_GCM_TAG    16

/* SHA-256 (FIPS 180-4). out must hold 32 bytes. */
void tls_sha256(const uint8_t* m, size_t len, uint8_t out[TLS_SHA256_LEN]);

/* HMAC-SHA-256 (RFC 2104). out must hold 32 bytes. */
void tls_hmac_sha256(const uint8_t* key, size_t keylen,
                     const uint8_t* msg, size_t msglen,
                     uint8_t out[TLS_SHA256_LEN]);

/* TLS 1.2 PRF (RFC 5246 $5): P_SHA256(secret, seed) truncated to
 * out_len bytes. `seed` is the concatenated label||seed material. */
void tls_prf(const uint8_t* secret, size_t secretlen,
             const uint8_t* seed, size_t seedlen,
             uint8_t* out, size_t out_len);

/* SHA-256 incremental — needed for the handshake transcript. */
typedef struct tls_sha256_ctx {
    uint32_t h[8];
    uint64_t bits;
    uint8_t  buf[64];
    uint32_t buflen;
} tls_sha256_ctx;

void tls_sha256_init(tls_sha256_ctx* ctx);
void tls_sha256_update(tls_sha256_ctx* ctx, const uint8_t* m, size_t len);
void tls_sha256_final(tls_sha256_ctx* ctx, uint8_t out[TLS_SHA256_LEN]);

/* AES-128: expanded key = 11 round keys. */
typedef struct tls_aes128 {
    uint8_t rk[11][16];
} tls_aes128;

void tls_aes128_setkey(tls_aes128* a, const uint8_t key[TLS_AES_BLOCK]);
void tls_aes128_enc(const tls_aes128* a, const uint8_t in[TLS_AES_BLOCK],
                    uint8_t out[TLS_AES_BLOCK]);
void tls_aes128_dec(const tls_aes128* a, const uint8_t in[TLS_AES_BLOCK],
                    uint8_t out[TLS_AES_BLOCK]);

/* AES-128-GCM (RFC 5116 / NIST SP800-38D). Seal appends the 16-byte
 * tag to `ct` (ct must hold ptlen + TLS_GCM_TAG). Open verifies the
 * appended tag; returns 0 on success, -1 on authentication failure. */
void tls_gcm_seal(const uint8_t key[TLS_AES_BLOCK],
                  const uint8_t nonce[12],
                  const uint8_t* aad, size_t aadlen,
                  const uint8_t* pt, size_t ptlen,
                  uint8_t* ct);                    /* ct = ptlen + 16 */
int  tls_gcm_open(const uint8_t key[TLS_AES_BLOCK],
                  const uint8_t nonce[12],
                  const uint8_t* aad, size_t aadlen,
                  const uint8_t* ct, size_t ctlen,   /* includes tag */
                  uint8_t* pt, uint8_t tag[TLS_GCM_TAG]);

/* RSA public-key ops (PKCS#1 v1.5), plain byte-strings big-endian. */
#define TLS_RSA_MAX_MODULUS 512 /* bytes = 4096 bits */

/* RSAEP padding+encrypt: message `m` (len <= modlen-11) becomes a
 * modulus-sized block. Assumes e == 65537 (the only one we accept). */
int tls_rsa_encrypt_pkcs1(const uint8_t* n, uint32_t nlen,
                          const uint8_t* m, uint32_t mlen,
                          uint8_t* out);            /* out: nlen bytes */
/* Generic modpow: out = base^exp mod mod (all big-endian byte strings,
 * mod up to TLS_RSA_MAX_MODULUS bytes). */
void tls_modpow(const uint8_t* base, uint32_t base_len,
                const uint8_t* exp, uint32_t exp_len,
                const uint8_t* mod, uint32_t mod_len,
                uint8_t* out);                      /* out: mod_len bytes */

/* X.509: pull the RSA public key out of a DER Certificate (or cert
 * chain element). Returns 0 and fills mod/exp on success. mod_len and
 * exp_len are in/out: on entry the buffer capacities, on success the
 * actual byte lengths. */
int tls_x509_rsa_pubkey(const uint8_t* der, size_t len,
                        uint8_t* mod_out, uint32_t* mod_len,
                        uint8_t* exp_out, uint32_t* exp_len);

#ifdef __cplusplus
}
#endif

#endif /* VLIBC_TLS_H */