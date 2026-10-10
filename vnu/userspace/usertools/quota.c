/* quota — show and set per-user storage quotas */
#include "ue.h"

/* The "unlimited" spelling both the read and the help use, so `limit=0'
 * always means the same thing on either side. */
static void print_limit(unsigned long limit)
{
    if (limit == 0)
        ue_out("unlimited\n");
    else {
        ue_num(limit);
        ue_out("\n");
    }
}

/* Read one user's numbers and print them as `user=NAME used=N limit=M'. */
static void show_user(unsigned long uid)
{
    struct vnu_quota q;
    q.uid = (unsigned long)uid;
    q.used = 0;
    q.limit = 0;
    long r = syscall(VNU_SYS_quota, VNU_QUOTA_GET, (unsigned long)&q);
    if (r < 0) {
        ue_err("quota: cannot read the quota\n");
        return;
    }
    struct ue_pw e;
    char line[96];
    if (ue_pw_by_uid(uid, &e))
        snprintf(line, sizeof(line), "quota: user=%s used=%lu limit=", e.name,
                 (unsigned long)q.used);
    else
        snprintf(line, sizeof(line), "quota: user=%lu used=%lu limit=", uid,
                 (unsigned long)q.used);
    ue_out(line);
    print_limit(q.limit);
}

static void show_default(void)
{
    struct vnu_quota q;
    q.uid = 0;
    q.used = 0;
    q.limit = 0;
    long r = syscall(VNU_SYS_quota, VNU_QUOTA_GET_DEFAULT, (unsigned long)&q);
    if (r < 0) {
        ue_err("quota: cannot read the default limit\n");
        return;
    }
    ue_out("quota: default limit=");
    print_limit(q.limit);
}

/* A decimal byte count, or -1 when the text is not one or overflows a
 * 32-bit field (which is all a quota can hold). */
static long parse_bytes(const char* s)
{
    if (!s || !*s)
        return -1;
    unsigned long v = 0;
    for (const char* p = s; *p; ++p) {
        if (*p < '0' || *p > '9')
            return -1;
        if (v > (0xFFFFFFFFUL - (unsigned long)(*p - '0')) / 10)
            return -1;
        v = v * 10 + (unsigned long)(*p - '0');
    }
    return (long)v;
}

static void usage(void)
{
    ue_err("usage: quota [OPTION...] [USER]\n");
    ue_err("  -s LIMIT   set USER's hard limit to LIMIT bytes (root only)\n");
    ue_err("  -d LIMIT   set the default limit for users without one (root only)\n");
    ue_err("  -D         show the default limit\n");
    ue_err("  -h         show this help and exit\n");
    ue_err("  -V         show the version and exit\n");
    ue_err("With no -s, print USER's (default: the current user's) usage and limit.\n");
    ue_err("A limit of 0 is unlimited; a write that would cross a limit fails.\n");
}

int main(int argc, char** argv)
{
    long set_limit = 0;
    long def_limit = 0;
    int have_set = 0, have_def = 0, show_def = 0;
    const char* user = 0;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage();
            return 0;
        }
        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("quota (VNU) 0.1\n");
            return 0;
        }
        if (strcmp(a, "-s") == 0 || strcmp(a, "--set") == 0) {
            if (i + 1 >= argc) {
                ue_err("quota: -s needs a byte count\n");
                return 2;
            }
            set_limit = parse_bytes(argv[++i]);
            if (set_limit < 0) {
                ue_err("quota: bad byte count\n");
                return 2;
            }
            have_set = 1;
            continue;
        }
        if (strcmp(a, "-d") == 0 || strcmp(a, "--default") == 0) {
            if (i + 1 >= argc) {
                ue_err("quota: -d needs a byte count\n");
                return 2;
            }
            def_limit = parse_bytes(argv[++i]);
            if (def_limit < 0) {
                ue_err("quota: bad byte count\n");
                return 2;
            }
            have_def = 1;
            continue;
        }
        if (strcmp(a, "-D") == 0 || strcmp(a, "--show-default") == 0) {
            show_def = 1;
            continue;
        }
        if (a[0] == '-' && a[1]) {
            ue_err("quota: unknown option ");
            ue_err(a);
            ue_err("\n");
            return 2;
        }
        if (user) {
            ue_err("quota: too many arguments\n");
            return 2;
        }
        user = a;
    }

    if (show_def) {
        show_default();
        return 0;
    }

    /* Resolve the operand: a name from /etc/passwd, or a bare uid. With
     * no operand the command is about the caller. */
    unsigned long uid = getuid();
    if (user) {
        int digits = user[0] != 0;
        for (const char* p = user; *p; ++p)
            if (*p < '0' || *p > '9')
                digits = 0;
        if (digits) {
            long v = parse_bytes(user);
            if (v < 0) {
                ue_err("quota: bad user id\n");
                return 2;
            }
            uid = (unsigned long)v;
        } else {
            struct ue_pw e;
            if (!ue_pw_lookup(user, &e)) {
                ue_err("quota: no such user: ");
                ue_err(user);
                ue_err("\n");
                return 1;
            }
            uid = e.uid;
        }
    }

    if (have_set) {
        struct vnu_quota q;
        q.uid = (unsigned long)uid;
        q.used = 0;
        q.limit = (unsigned long)set_limit;
        long r = syscall(VNU_SYS_quota, VNU_QUOTA_SET, (unsigned long)&q);
        if (r < 0) {
            if (r == -(long)VNU_EPERM)
                ue_err("quota: only root may set a limit\n");
            else
                ue_err("quota: cannot set the limit\n");
            return 1;
        }
        show_user(uid);
        return 0;
    }

    if (have_def) {
        struct vnu_quota q;
        q.uid = 0;
        q.used = 0;
        q.limit = (unsigned long)def_limit;
        long r = syscall(VNU_SYS_quota, VNU_QUOTA_SET_DEFAULT, (unsigned long)&q);
        if (r < 0) {
            if (r == -(long)VNU_EPERM)
                ue_err("quota: only root may set the default limit\n");
            else
                ue_err("quota: cannot set the default limit\n");
            return 1;
        }
        show_default();
        return 0;
    }

    show_user(uid);
    return 0;
}
