/*
 * ue.h — shared helpers for the standalone account/install tools
 * (id, whoami, groups, useradd, passwd, su, install).
 *
 * Every VNU userspace program is one self-contained .c (vcc links one
 * file per binary), so all helpers here are static inline. The tools
 * read the same /etc/passwd + /etc/group records and the same djb2
 * password hash as the vash login prompt.
 */
#ifndef UE_H
#define UE_H

#include <vlibc/unistd.h>
#include <vlibc/stdio.h>
#include <vlibc/stdlib.h>
#include <vlibc/string.h>
#include <vlibc/fcntl.h>
#include <vlibc/keys.h>
#include <vlibc/sys/stat.h>
#include <vlibc/sys/syscall.h>
#include <vnu/abi.h>

#define UE_PASSWD_F   "/etc/passwd"
#define UE_GROUP_F    "/etc/group"
#define UE_SESSION_F  "/tmp/.session"
#define UE_MAX_USERS  8

struct ue_pw {
    char name[32];
    char hash[17];
    unsigned long uid;
    unsigned long gid;
    char home[64];
};

static inline void ue_out(const char* s)
{
    if (s)
        write(1, s, strlen(s));
}

static inline void ue_err(const char* s)
{
    if (s)
        write(2, s, strlen(s));
}

static inline void ue_num(unsigned long v)
{
    char b[24];
    int i = 0;
    if (v == 0)
        b[i++] = '0';
    while (v > 0) {
        b[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (i > 0)
        write(1, &b[--i], 1);
}

/* --- building strings (no snprintf in vlibc) --- */
static inline int ue_nappend(char* b, int n, int cap, const char* s)
{
    while (s && *s && n < cap - 1)
        b[n++] = *s++;
    return n;
}

static inline int ue_uappend(char* b, int n, int cap, unsigned long v)
{
    char t[24];
    int i = 0;
    if (v == 0)
        t[i++] = '0';
    while (v) {
        t[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (i)
        if (n < cap - 1)
            b[n++] = t[--i];
    return n;
}

/* djb2 hash of a password -> 8 hex chars. This is what /etc/passwd
 * stores in field 2 (see the kernel-seeded accounts and useradd). */
static inline void ue_pw_hash(const char* s, char out[17])
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

/* Read /etc/passwd (`name:hash:uid:gid:gecos:home:shell`) into `out`.
 * Returns the number of entries parsed (capped at UE_MAX_USERS). */
static inline int ue_read_passwd(struct ue_pw* out)
{
    int fd = open(UE_PASSWD_F, O_RDONLY);
    if (fd < 0)
        return 0;
    char buf[2048];
    long got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got <= 0)
        return 0;
    buf[got] = 0;
    int n = 0;
    char* line = buf;
    while (line && *line && n < UE_MAX_USERS) {
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
            if (fi >= 6 && f[0][0]) {
                struct ue_pw* e = &out[n];
                strncpy(e->name, f[0], sizeof(e->name) - 1);
                e->name[sizeof(e->name) - 1] = 0;
                strncpy(e->hash, f[1], sizeof(e->hash) - 1);
                e->hash[sizeof(e->hash) - 1] = 0;
                e->uid = (unsigned long)atoi(f[2]);
                e->gid = (unsigned long)atoi(f[3]);
                strncpy(e->home, f[5], sizeof(e->home) - 1);
                e->home[sizeof(e->home) - 1] = 0;
                ++n;
            }
        }
        line = nl + 1;
    }
    return n;
}

static inline int ue_pw_lookup(const char* name, struct ue_pw* out)
{
    struct ue_pw all[UE_MAX_USERS];
    int n = ue_read_passwd(all);
    for (int i = 0; i < n; ++i)
        if (strcmp(all[i].name, name) == 0) {
            *out = all[i];
            return 1;
        }
    return 0;
}

/* First passwd record whose uid matches, if any. */
static inline int ue_pw_by_uid(unsigned long uid, struct ue_pw* out)
{
    struct ue_pw all[UE_MAX_USERS];
    int n = ue_read_passwd(all);
    for (int i = 0; i < n; ++i)
        if (all[i].uid == uid) {
            *out = all[i];
            return 1;
        }
    return 0;
}

/* Group name for a gid from /etc/group, or NULL if none. */
static inline const char* ue_group_name(unsigned long gid)
{
    static char gname[32];
    int fd = open(UE_GROUP_F, O_RDONLY);
    if (fd < 0)
        return 0;
    char gb[512];
    long gn = read(fd, gb, sizeof(gb) - 1);
    close(fd);
    if (gn <= 0)
        return 0;
    gb[gn] = 0;
    char* line = gb;
    while (line && *line) {
        char* nl = line;
        while (*nl && *nl != '\n')
            ++nl;
        if (*nl)
            *nl = 0;
        char* f[3] = {line, 0, 0};
        int fi = 0;
        char* p = line;
        while (*p && fi < 2) {
            if (*p == ':') {
                *p = 0;
                f[++fi] = p + 1;
            }
            ++p;
        }
        if (fi >= 2 && (unsigned long)atoi(f[2]) == gid) {
            strncpy(gname, f[0], sizeof(gname) - 1);
            gname[sizeof(gname) - 1] = 0;
            return gname;
        }
        line = nl + 1;
    }
    return 0;
}

/* Format one /etc/passwd entry back into `line` (canonical order). */
static inline int ue_format_pwline(const struct ue_pw* e, char* line, int cap)
{
    int n = 0;
    n = ue_nappend(line, n, cap, e->name);
    line[n++] = ':';
    n = ue_nappend(line, n, cap, e->hash);
    line[n++] = ':';
    n = ue_uappend(line, n, cap, e->uid);
    line[n++] = ':';
    n = ue_uappend(line, n, cap, e->gid);
    line[n++] = ':';
    n = ue_nappend(line, n, cap, e->name);
    line[n++] = ':';
    n = ue_nappend(line, n, cap, e->home);
    line[n++] = ':';
    n = ue_nappend(line, n, cap, "/bin/vash");
    line[n++] = '\n';
    line[n] = 0;
    return n;
}

/* Read a line without echoing (for passwords). Ctrl+C aborts (-1). */
static inline int ue_read_secret(char* out, int max)
{
    int n = 0;
    for (;;) {
        char raw = 0;
        if (read(0, &raw, 1) <= 0)
            return -1;
        unsigned char ch = (unsigned char)raw;
        if (ch == '\n') {
            ue_out("\n");
            out[n] = 0;
            return n;
        }
        if (ch == VNU_KEY_INTR) {
            ue_out("^C\n");
            return -1;
        }
        if (ch == '\b' && n > 0) {
            --n;
            out[n] = 0;
        } else if (ch >= 32 && ch < 127 && n < max - 1) {
            out[n++] = (char)ch;
            out[n] = 0;
        }
    }
}

/* Join dir + "/" + name into out (if dir is "/" just "/" + name). */
static inline void ue_join_path(char* out, int outn, const char* dir,
                                const char* name)
{
    int i = 0;
    if (dir[0] == '/' && dir[1] == 0) {
        if (i < outn - 1)
            out[i++] = '/';
    } else {
        for (int j = 0; dir[j] && i < outn - 2; ++j)
            out[i++] = dir[j];
        if (i > 0 && out[i - 1] != '/' && i < outn - 1)
            out[i++] = '/';
    }
    for (int j = 0; name[j] && i < outn - 1; ++j)
        out[i++] = name[j];
    out[i] = 0;
}

/* Record "name uid gid home" in /tmp/.session so the next respawned
 * vash re-adopts this identity (mirror of vash's save_session). */
static inline void ue_save_session(const char* name, unsigned long uid,
                                   unsigned long gid, const char* home)
{
    char b[192];
    int n = 0;
    n = ue_nappend(b, n, sizeof(b), name);
    b[n++] = ' ';
    n = ue_uappend(b, n, sizeof(b), uid);
    b[n++] = ' ';
    n = ue_uappend(b, n, sizeof(b), gid);
    b[n++] = ' ';
    n = ue_nappend(b, n, sizeof(b), home);
    b[n++] = '\n';
    b[n] = 0;
    int fd = open(UE_SESSION_F, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return;
    write(fd, b, (unsigned long)n);
    close(fd);
}

#endif /* UE_H */