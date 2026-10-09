/* auth.c - RFC 4252 userauth for vsshd: the "none" and "password"
 * methods. A password is checked exactly the way the machine's own
 * console login checks it: a djb2 hash compared against /etc/passwd
 * field 2 (see vash.c's pw_hash for the reference algorithm).
 *
 * Only "password" is ever offered, and only "ssh-connection" is an
 * acceptable service, so the loop below is the whole conversation:
 * service request, then userauth requests until success or give-up.
 */

#include <vlibc/string.h>
#include <vlibc/fcntl.h>
#include <vlibc/unistd.h>
#include <vlibc/stdlib.h>
#include <vnu/abi.h>
#include "ssh.h"

#define PASSWD_F "/etc/passwd"
#define MAX_USERS 16

/* djb2 hash of a password -> 8 lowercase hex chars; identical to what
 * vash.c and useradd store in /etc/passwd. */
static void pw_hash(const char* s, char out[17])
{
    unsigned long h = 5381;
    const char* p = s;
    while (*p) {
        h = ((h << 5) + h) + (unsigned char)*p;
        h &= 0xFFFFFFFFUL;
        ++p;
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 7; i >= 0; --i) {
        out[i] = hex[h & 0xF];
        h >>= 4;
    }
    out[8] = 0;
}

/* Find `name` in /etc/passwd (`name:hash:uid:gid:gecos:home:shell`).
 * Fills *u (name, hash, uid, gid, home) on success and returns 1. */
static int passwd_lookup(const char* name, struct SshUser* u)
{
    int fd = open(PASSWD_F, O_RDONLY);
    if (fd < 0)
        return 0;
    char buf[2048];
    long got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got <= 0)
        return 0;
    buf[got] = 0;

    char* line = buf;
    while (line && *line) {
        char* nl = line;
        while (*nl && *nl != '\n')
            ++nl;
        if (*nl)
            *nl = 0;
        if (*line) {
            char* f[7] = {0, 0, 0, 0, 0, 0, 0};
            int fi = 0;
            f[fi++] = line;
            char* p = line;
            while (*p) {
                if (*p == ':') {
                    *p = 0;
                    if (fi < 7)
                        f[fi++] = p + 1;
                }
                ++p;
            }
            if (fi >= 6 && f[0][0] && strcmp(f[0], name) == 0) {
                strncpy(u->name, f[0], sizeof(u->name) - 1);
                u->name[sizeof(u->name) - 1] = 0;
                strncpy(u->hash, f[1], sizeof(u->hash) - 1);
                u->hash[sizeof(u->hash) - 1] = 0;
                u->uid = (unsigned long)atoi(f[2]);
                u->gid = (unsigned long)atoi(f[3]);
                strncpy(u->home, f[5], sizeof(u->home) - 1);
                u->home[sizeof(u->home) - 1] = 0;
                return 1;
            }
        }
        line = nl + 1;
    }
    return 0;
}

/* Pull one SSH string field from a packet at *off (updating it). */
static int get_str(const uint8_t* p, uint32_t plen, uint32_t* off,
                   const char** out, uint32_t* olen)
{
    if (*off + 4 > plen)
        return -1;
    uint32_t l = ((uint32_t)p[*off] << 24) | ((uint32_t)p[*off + 1] << 16) |
                 ((uint32_t)p[*off + 2] << 8) | (uint32_t)p[*off + 3];
    *off += 4;
    if (*off + l > plen)
        return -1;
    *out = (const char*)p + *off;
    *olen = l;
    *off += l;
    return 0;
}

static int send_failure(Ssh* s)
{
    uint8_t b[32];
    b[0] = SSH_MSG_USERAUTH_FAILURE;
    b[1] = 0; b[2] = 0; b[3] = 0; b[4] = 8;          /* "password" */
    b[5] = 'p'; b[6] = 'a'; b[7] = 's'; b[8] = 's';
    b[9] = 'w'; b[10] = 'o'; b[11] = 'r'; b[12] = 'd';
    b[13] = 0;                                       /* partial, false */
    return ssh_send_raw(s, b, 14);
}

int ssh_auth(Ssh* s, struct SshUser* user)
{
    uint8_t pkt[SSH_MAX_PACKET];
    uint32_t plen;
    int attempts = 0;

    for (;;) {
        if (ssh_recv_packet(s, pkt, &plen, sizeof(pkt)) < 0) {
            return -1;
        }
        if (plen < 1)
            return -1;
        uint8_t type = pkt[0];
        if (type == SSH_MSG_DISCONNECT)
            return -1;
        if (type == SSH_MSG_IGNORE || type == SSH_MSG_DEBUG ||
            type == SSH_MSG_UNIMPLEMENTED)
            continue;

        if (type == SSH_MSG_SERVICE_REQUEST) {
            uint32_t off = 1;
            const char* svc;
            uint32_t svclen;
            if (get_str(pkt, plen, &off, &svc, &svclen) < 0)
                return -1;
            if (svclen == 12 && memcmp(svc, "ssh-userauth", 12) == 0) {
                uint8_t b[1 + 4 + 12];
                b[0] = SSH_MSG_SERVICE_ACCEPT;
                b[1] = (uint8_t)(svclen >> 24);
                b[2] = (uint8_t)(svclen >> 16);
                b[3] = (uint8_t)(svclen >> 8);
                b[4] = (uint8_t)svclen;
                memcpy(b + 5, svc, svclen);
                if (ssh_send_raw(s, b, 1 + 4 + svclen) < 0)
                    return -1;
                continue;
            }
            return -1; /* a service we do not speak */
        }

        if (type != SSH_MSG_USERAUTH_REQUEST)
            return -1;

        /* SSH_MSG_USERAUTH_REQUEST: user, service, method, method data */
        uint32_t off = 1;
        const char* uname, *service, *method;
        uint32_t unamelen, servicelen, methodlen;
        if (get_str(pkt, plen, &off, &uname, &unamelen) < 0)
            return -1;
        if (get_str(pkt, plen, &off, &service, &servicelen) < 0)
            return -1;
        if (get_str(pkt, plen, &off, &method, &methodlen) < 0)
            return -1;
        if (servicelen != 14 ||
            memcmp(service, "ssh-connection", 14) != 0) {
            if (send_failure(s) < 0)
                return -1;
            continue;
        }
        if (methodlen == 4 && memcmp(method, "none", 4) == 0) {
            /* probe: we only ever answer "password" */
            if (send_failure(s) < 0)
                return -1;
            continue;
        }
        if (methodlen != 8 || memcmp(method, "password", 8) != 0) {
            if (send_failure(s) < 0)
                return -1;
            continue;
        }
        if (off >= plen)
            return -1;
        int change = pkt[off++];
        if (change != 0) {
            /* password-change request: not supported */
            if (send_failure(s) < 0)
                return -1;
            continue;
        }
        const char* pass;
        uint32_t passlen;
        if (get_str(pkt, plen, &off, &pass, &passlen) < 0)
            return -1;

        if (++attempts > 6)
            return -1; /* too many tries; drop the connection */

        char name[32];
        if (unamelen >= sizeof(name))
            return -1;
        memcpy(name, uname, unamelen);
        name[unamelen] = 0;

        struct SshUser u;
        memset(&u, 0, sizeof(u));
        char pbuf[128];
        if (passlen >= sizeof(pbuf))
            return -1;
        memcpy(pbuf, pass, passlen);
        pbuf[passlen] = 0;

        if (!passwd_lookup(name, &u)) {
            if (send_failure(s) < 0)
                return -1;
            continue;
        }
        char hash[17];
        pw_hash(pbuf, hash);
        if (strcmp(hash, u.hash) != 0) {
            if (send_failure(s) < 0)
                return -1;
            continue;
        }

        /* success: hand the account to the caller */
        *user = u;
        {
            uint8_t ok = SSH_MSG_USERAUTH_SUCCESS;
            return ssh_send_raw(s, &ok, 1);
        }
    }
}