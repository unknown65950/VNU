/* ssh.h - SSH-2 transport types and packet helpers for vsshd.
 *
 * The daemon speaks one configuration end to end: RFC 4253 transport
 * (kex diffie-hellman-group14-sha256, host key rsa-sha2-256 over an
 * embedded RSA-2048 key, cipher aes128-ctr, MAC hmac-sha2-256, no
 * compression), RFC 4252 userauth (password method, djb2 check against
 * /etc/passwd) and RFC 4254 connection (one "session" channel running
 * the machine's own /bin/vash).
 *
 * Everything here is bare C over vlibc (no libc beyond it), the same
 * constraint the rest of userspace works under.
 */
#ifndef VSSHD_SSH_H
#define VSSHD_SSH_H

#include <stdint.h>
#include <vlibc/tls.h>

/* vlibc has no memcmp (its string.h is strings only); the byte compares
 * the SSH protocol needs - MACs, name lists, version lines - are short,
 * so one shared plain loop serves every file below. */
static int memcmp(const void* a, const void* b, unsigned long n)
{
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    while (n--) {
        if (x[0] != y[0])
            return (int)x[0] - (int)y[0];
        ++x;
        ++y;
    }
    return 0;
}

/* --- packet message numbers (RFC 4250-4254) ------------------------ */
#define SSH_MSG_DISCONNECT 1
#define SSH_MSG_IGNORE 2
#define SSH_MSG_UNIMPLEMENTED 3
#define SSH_MSG_DEBUG 4
#define SSH_MSG_SERVICE_REQUEST 5
#define SSH_MSG_SERVICE_ACCEPT 6
#define SSH_MSG_EXT_INFO 7
#define SSH_MSG_KEXINIT 20
#define SSH_MSG_NEWKEYS 21
#define SSH_MSG_KEXDH_INIT 30
#define SSH_MSG_KEXDH_REPLY 31
#define SSH_MSG_USERAUTH_REQUEST 50
#define SSH_MSG_USERAUTH_FAILURE 51
#define SSH_MSG_USERAUTH_SUCCESS 52
#define SSH_MSG_GLOBAL_REQUEST 80
#define SSH_MSG_REQUEST_SUCCESS 81
#define SSH_MSG_REQUEST_FAILURE 82
#define SSH_MSG_CHANNEL_OPEN 90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91
#define SSH_MSG_CHANNEL_OPEN_FAILURE 92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST 93
#define SSH_MSG_CHANNEL_DATA 94
#define SSH_MSG_CHANNEL_EXTENDED_DATA 95
#define SSH_MSG_CHANNEL_EOF 96
#define SSH_MSG_CHANNEL_CLOSE 97
#define SSH_MSG_CHANNEL_REQUEST 98
#define SSH_MSG_CHANNEL_SUCCESS 99
#define SSH_MSG_CHANNEL_FAILURE 100

/* Our advertised limits. max_packet caps one SSH packet (and with it a
 * channel DATA chunk) at 8 KiB - small enough that the packet codec's
 * working buffers stay deep under the 64 KiB user stack, large enough
 * that a remote shell feels unthrottled; window sizes are honest but
 * generous. */
#define SSH_MAX_PACKET 8192
#define SSH_WINDOW_IN 131072
#define SSH_WINDOW_OUT 262144

/* ssh_recv_packet() soft-timeout: the first byte of a packet did not
 * arrive within Ssh::recv_timeout ms (not an error; poll again). */
#define SSH_RECV_TIMEOUT (-2)

/* The embedded RSA-2048 host key lives in hostkey.h (static arrays:
 * big-endian n and d; e is the fixed 65537). */

/* --- growable byte buffer (user-facing inputs are small) ----------- */
typedef struct {
    uint8_t* b;
    uint32_t len;
    uint32_t cap;
} Buf;

void buf_init(Buf* b);
void buf_free(Buf* b);
int buf_put(Buf* b, const void* data, uint32_t n);
int buf_put_u8(Buf* b, uint8_t v);
int buf_put_u32(Buf* b, uint32_t v);
int buf_put_str(Buf* b, const void* data, uint32_t n); /* uint32 len + bytes */
int buf_put_cstr(Buf* b, const char* s);
int buf_put_mpint(Buf* b, const uint8_t* v, uint32_t len); /* two's-complement mpint */
int buf_put_bytes(Buf* b, uint8_t fill, uint32_t n);

/* --- connection state ---------------------------------------------- */
typedef struct Ssh {
    int sock;               /* the TCP socket */
    int enc;                /* NEWKEYS done? */
    int recv_timeout;       /* ms for the first byte of a packet */

    /* packet-level cipher state */
    tls_aes128 enc_aes;     /* server -> client (keys "D","B","F") */
    tls_aes128 dec_aes;     /* client -> server (keys "C","A","E") */
    uint8_t enc_ctr[16];    /* rolling AES-CTR counter, server side */
    uint8_t dec_ctr[16];    /* rolling AES-CTR counter, client side */
    uint8_t mac_out[32];    /* hmac-sha2-256 key, server -> client */
    uint8_t mac_in[32];     /* hmac-sha2-256 key, client -> server */
    uint32_t seq_out;       /* packet sequence numbers (0 at NEWKEYS) */
    uint32_t seq_in;

    /* transcript pieces (hashed in order) */
    uint8_t v_c[256]; int vc_len;   /* client version line, no CRLF */
    uint8_t v_s[256]; int vs_len;   /* server version line, no CRLF */
    uint8_t i_c[2048]; int ic_len;  /* client KEXINIT payload */
    uint8_t i_s[2048]; int is_len;  /* server KEXINIT payload */
    uint8_t k_s[512]; int ks_len;   /* host key blob */
    uint8_t e[260]; int e_len;      /* client's DH public, mpint body */
    uint8_t f[260]; int f_len;      /* our DH public, mpint body */
    uint8_t k[260]; int k_len;      /* shared secret, mpint body */

    uint8_t h[32];                  /* exchange hash */
    uint8_t session_id[32];
    int have_sid;
} Ssh;

/* transport.c */
int ssh_handshake(Ssh* s);          /* version, kex, keys; 0 on success */
int ssh_send_raw(Ssh* s, const uint8_t* payload, uint32_t len);
int ssh_recv_packet(Ssh* s, uint8_t* payload, uint32_t* len,
                    uint32_t maxlen);       /* blocking; -1 on error/EOF */
int ssh_write(Ssh* s, const void* data, uint32_t len); /* exact socket write */

/* hostkey.h */
int ssh_hostkey_blob(uint8_t* out, uint32_t outcap, uint32_t* len);
int ssh_sign(const uint8_t h[32], uint8_t* out, uint32_t outcap,
             uint32_t* len); /* rsa-sha2-256: PKCS#1 v1.5 over SHA-256(H) */

/* --- the account vsshd authenticated -------------------------------- */
struct SshUser {
    char name[32];
    char hash[17];          /* the djb2 hash from /etc/passwd field 2 */
    unsigned long uid;
    unsigned long gid;
    char home[64];
};

/* auth.c */
int ssh_auth(Ssh* s, struct SshUser* user);   /* service + userauth; 0 on success */

/* conn.c */
int ssh_connection(Ssh* s, const struct SshUser* user); /* one session channel */

#endif /* VSSHD_SSH_H */