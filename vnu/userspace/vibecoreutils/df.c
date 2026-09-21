/* df — report file system space usage (separate binary).
 *
 * Space comes from /proc/dfstat, which the kernel regenerates on every
 * open() and which has one line per mounted file system:
 *     source fstype mountpoint total_bytes used_bytes
 * Only the in-memory VFS (source "vfs", mounted on /) has real data;
 * /dev and /proc are pseudo file systems with nothing to account.
 */
#include "cu.h"

#define MAX_MOUNTS 8
#define MAX_OPTS 8

struct Mount {
    char src[32];
    char fstype[32];
    char mnt[32];
    unsigned long total;
    unsigned long used;
};

static int read_file(const char* path, char* buf, int cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = 0;
    for (;;) {
        long r = read(fd, buf + n, (unsigned long)(cap - n - 1));
        if (r <= 0)
            break;
        n += (int)r;
    }
    close(fd);
    buf[n] = 0;
    return n;
}

static int parse_mounts(struct Mount* out, int maxn)
{
    char buf[512];
    if (read_file("/proc/dfstat", buf, sizeof(buf)) < 0)
        return 0;
    int n = 0;
    char* p = buf;
    while (*p && n < maxn) {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            ++p;
        if (!*p)
            break;
        char* tok[5];
        int tn = 0;
        while (*p && *p != '\n' && tn < 5) {
            tok[tn++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n')
                ++p;
            if (*p)
                *p++ = 0;
        }
        if (tn < 5)
            break;
        struct Mount* m = &out[n];
        strncpy(m->src, tok[0], sizeof(m->src) - 1);
        strncpy(m->fstype, tok[1], sizeof(m->fstype) - 1);
        strncpy(m->mnt, tok[2], sizeof(m->mnt) - 1);
        unsigned long total = 0, used = 0;
        for (const char* t = tok[3]; *t; ++t)
            total = total * 10 + (unsigned long)(*t - '0');
        for (const char* t = tok[4]; *t; ++t)
            used = used * 10 + (unsigned long)(*t - '0');
        m->total = total;
        m->used = used;
        ++n;
    }
    return n;
}

static int longest_prefix_match(const struct Mount* mounts, int n, const char* path)
{
    int best = -1;
    int bestlen = -1;
    for (int i = 0; i < n; ++i) {
        int len = (int)strlen(mounts[i].mnt);
        if (len <= bestlen)
            continue;
        if (strncmp(path, mounts[i].mnt, (unsigned long)len) == 0 &&
            (path[len] == 0 || path[len] == '/'))
            best = i;
    }
    return best;
}

/* Decimal width / right-aligned output, no snprintf in vlibc. */
static int num_len(unsigned long v)
{
    int n = 1;
    while (v >= 10) {
        v /= 10;
        ++n;
    }
    return n;
}

static void num_field(unsigned long v, int width)
{
    char digits[20];
    int dc = 0;
    do {
        digits[dc++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && dc < 20);
    int sp = width - dc;
    while (sp-- > 0)
        w(" ");
    while (dc-- > 0)
        putchar(digits[dc]);
}

/* Human-readable size: powers of 1024 for -h, powers of 1000 for -H.
 * Emits "1K"/"7.0M"/"1023M" style strings into out (needs 12 bytes).
 * GNU rounds up, we just floor — close enough for this OS. */
static void human(unsigned long bytes, int si, char* out)
{
    unsigned long div = si ? 1000u : 1024u;
    const char* suf = "KMGTPE";

    if (bytes < div) {
        strncpy(out, "0", 12);
        return;
    }

    unsigned long unit = div;
    int level = 0;
    while (level < 5 && bytes / div >= unit) {
        unit *= div;
        ++level;
    }

    unsigned long whole = bytes / unit;
    unsigned long frac = (bytes % unit) * 10u / unit;

    if (whole >= 10 || frac == 0) {
        char digits[4];
        int n = 0;
        do {
            digits[n++] = (char)('0' + (whole % 10));
            whole /= 10;
        } while (whole && n < 4);
        int i = 0;
        while (n--)
            out[i++] = digits[n];
        out[i++] = suf[level];
        out[i] = 0;
    } else {
        out[0] = (char)('0' + whole);
        out[1] = '.';
        out[2] = (char)('0' + frac);
        out[3] = suf[level];
        out[4] = 0;
    }
}

static void usage(void)
{
    w("usage: df [OPTION] [FILE...]\n");
    w("Show the space usage of each mounted file system.\n\n");
    w("  -a            include pseudo file systems (all are shown anyway)\n");
    w("  -h            human-readable sizes (powers of 1024)\n");
    w("  -H            human-readable sizes (powers of 1000)\n");
    w("  -k            show sizes in 1 KiB blocks (the default)\n");
    w("  -l            limit to local file systems (all are local)\n");
    w("  -P            POSIX output format\n");
    w("  -T            print file system type\n");
    w("  -t TYPE       limit listing to file systems of type TYPE\n");
    w("  -x TYPE       limit listing to file systems not of type TYPE\n");
    w("  -v            ignored (compatibility)\n");
    w("      --help    display this help and exit\n");
    w("      --version output version information and exit\n");
}

int main(int argc, char** argv)
{
    int hflag = 0;
    int Hflag = 0;
    int Pflag = 0;
    int Tflag = 0;
    char inc[MAX_OPTS][32];
    int incn = 0;
    char exc[MAX_OPTS][32];
    int excn = 0;
    char* files[MAX_OPTS];
    int filesn = 0;

    for (int a = 1; a < argc; ++a) {
        const char* arg = argv[a];
        if (arg[0] != '-' || arg[1] == 0) {
            if (filesn < MAX_OPTS)
                files[filesn++] = argv[a];
            continue;
        }
        if (arg[1] == '-') {
            if (strcmp(arg, "--help") == 0) {
                usage();
                return 0;
            }
            if (strcmp(arg, "--version") == 0) {
                w("df (VNU coreutils) 0.5\n");
                return 0;
            }
            we("df: unsupported option ");
            we(arg);
            we("\n");
            usage();
            return 1;
        }
        for (const char* c = arg + 1; *c; ++c) {
            switch (*c) {
            case 'a':
            case 'l':
            case 'k':
            case 'v':
                break;
            case 'h':
                hflag = 1;
                Hflag = 0;
                break;
            case 'H':
                Hflag = 1;
                hflag = 0;
                break;
            case 'P':
                Pflag = 1;
                break;
            case 'T':
                Tflag = 1;
                break;
            case 't':
            case 'x': {
                const char* ty = c[1] ? c + 1 : (a + 1 < argc ? argv[++a] : 0);
                if (!ty) {
                    we("df: option requires an argument -- ");
                    w("?\n");
                    return 1;
                }
                if (*c == 't') {
                    if (incn < MAX_OPTS)
                        strncpy(inc[incn++], ty, 31);
                } else if (excn < MAX_OPTS) {
                    strncpy(exc[excn++], ty, 31);
                }
                goto nextarg;
            }
            default:
                we("df: invalid option -- ");
                w("?\n");
                usage();
                return 1;
            }
        }
    nextarg:;
    }

    struct Mount mounts[MAX_MOUNTS];
    int n = parse_mounts(mounts, MAX_MOUNTS);
    if (n <= 0) {
        we("df: cannot read /proc/dfstat\n");
        return 1;
    }

    /* FILE operands: keep only the mounts holding each file. */
    int want[MAX_MOUNTS];
    int wantn = 0;
    for (int i = 0; i < filesn; ++i) {
        char abs[64];
        const char* f = files[i];
        if (f[0] == '/') {
            strncpy(abs, f, sizeof(abs) - 1);
            abs[sizeof(abs) - 1] = 0;
        } else {
            char cwd[64];
            int l = 0;
            if (getcwd(cwd, sizeof(cwd)))
                l = (int)strlen(cwd);
            int n2 = 0;
            if (l > 0) {
                strncpy(abs, cwd, sizeof(abs) - 1);
                n2 = l;
            } else {
                abs[n2++] = '/';
            }
            if (n2 > 0 && abs[n2 - 1] != '/' && n2 < (int)sizeof(abs) - 1)
                abs[n2++] = '/';
            for (int k = 0; f[k] && n2 < (int)sizeof(abs) - 1; ++k)
                abs[n2++] = f[k];
            abs[n2] = 0;
        }
        struct stat st;
        if (stat(abs, &st) < 0) {
            we("df: cannot access ");
            we(f);
            we(": no such file or directory\n");
            continue;
        }
        int m = longest_prefix_match(mounts, n, abs);
        if (m < 0) {
            we("df: cannot determine file system of ");
            we(f);
            we("\n");
            continue;
        }
        int already = 0;
        for (int j = 0; j < wantn; ++j)
            if (want[j] == m)
                already = 1;
        if (!already)
            want[wantn++] = m;
    }
    if (filesn > 0 && wantn == 0)
        return 1;

    /* Column widths (numeric columns right-aligned, GNU style). */
    int fs_w = (int)strlen("Filesystem");
    int size_w = (int)strlen("1K-blocks");
    int used_w = (int)strlen("Used");
    int avail_w = (int)strlen("Available");
    for (int i = 0; i < n; ++i) {
        if ((int)strlen(mounts[i].src) > fs_w)
            fs_w = (int)strlen(mounts[i].src);
        unsigned long avail = mounts[i].total - mounts[i].used;
        char t1[12], t2[12], t3[12];
        human(mounts[i].total, 0, t1);
        human(mounts[i].used, 0, t2);
        human(avail, 0, t3);
        int l = (hflag || Hflag) ? (int)strlen(t1) : num_len(mounts[i].total / 1024);
        if (l > size_w)
            size_w = l;
        l = (hflag || Hflag) ? (int)strlen(t2) : num_len(mounts[i].used / 1024);
        if (l > used_w)
            used_w = l;
        l = (hflag || Hflag) ? (int)strlen(t3) : num_len(avail / 1024);
        if (l > avail_w)
            avail_w = l;
    }
    int ty_w = (int)strlen("Type");
    if (Tflag)
        for (int i = 0; i < n; ++i)
            if ((int)strlen(mounts[i].fstype) > ty_w)
                ty_w = (int)strlen(mounts[i].fstype);

    if (Pflag) {
        w("Filesystem");
        if (Tflag)
            w(" Type");
        w(" 1024-blocks Used Available Capacity Mounted on\n");
    } else {
        w("Filesystem");
        for (int i = (int)strlen("Filesystem"); i < fs_w; ++i)
            w(" ");
        if (Tflag) {
            w(" ");
            w("Type");
            for (int i = (int)strlen("Type"); i < ty_w; ++i)
                w(" ");
        }
        w("  ");
        w("1K-blocks");
        for (int i = (int)strlen("1K-blocks"); i < size_w; ++i)
            w(" ");
        w(" ");
        w("Used");
        for (int i = (int)strlen("Used"); i < used_w; ++i)
            w(" ");
        w(" ");
        w("Available");
        for (int i = (int)strlen("Available"); i < avail_w; ++i)
            w(" ");
        w(" Use% Mounted on\n");
    }

    for (int i = 0; i < n; ++i) {
        if (filesn > 0) {
            int keep = 0;
            for (int j = 0; j < wantn; ++j)
                if (want[j] == i)
                    keep = 1;
            if (!keep)
                continue;
        }
        int skip = 0;
        for (int j = 0; j < incn; ++j)
            if (strcmp(mounts[i].fstype, inc[j]) != 0)
                skip = 1;
        for (int j = 0; j < excn; ++j)
            if (strcmp(mounts[i].fstype, exc[j]) == 0)
                skip = 1;
        if (skip)
            continue;

        struct Mount* m = &mounts[i];
        unsigned long avail = m->total - m->used;
        unsigned long pct = m->total ? (m->used * 100u) / m->total : 0u;

        if (Pflag) {
            w(m->src);
            if (Tflag) {
                w(" ");
                w(m->fstype);
            }
            w(" ");
            num_field(m->total / 1024, 0);
            w(" ");
            num_field(m->used / 1024, 0);
            w(" ");
            num_field(avail / 1024, 0);
            w(" ");
            num_field(pct, 0);
            w("% ");
            w(m->mnt);
            w("\n");
            continue;
        }

        w(m->src);
        for (int i2 = (int)strlen(m->src); i2 < fs_w; ++i2)
            w(" ");
        if (Tflag) {
            w(" ");
            w(m->fstype);
            for (int i2 = (int)strlen(m->fstype); i2 < ty_w; ++i2)
                w(" ");
        }
        w("  ");

        char t1[12], t2[12], t3[12];
        int l1, l2, l3;
        if (hflag || Hflag) {
            human(m->total, Hflag, t1);
            human(m->used, Hflag, t2);
            human(avail, Hflag, t3);
            l1 = (int)strlen(t1);
            l2 = (int)strlen(t2);
            l3 = (int)strlen(t3);
        } else {
            unsigned long kb_t = m->total / 1024;
            unsigned long kb_u = m->used / 1024;
            unsigned long kb_a = avail / 1024;
            num_field(kb_t, size_w);
            num_field(kb_u, used_w);
            num_field(kb_a, avail_w);
            l1 = l2 = l3 = 0;
        }
        if (hflag || Hflag) {
            w(t1);
            for (int i2 = l1; i2 < size_w; ++i2)
                w(" ");
            w(t2);
            for (int i2 = l2; i2 < used_w; ++i2)
                w(" ");
            w(t3);
            for (int i2 = l3; i2 < avail_w; ++i2)
                w(" ");
        }
        w(" ");
        int pw = (int)strlen("999%"); /* "Use%" + max 3 digits + % */
        int pl = num_len(pct) + 1;
        if (pl > pw)
            pw = pl;
        num_field(pct, pw - 1);
        w("% ");
        w(m->mnt);
        w("\n");
    }
    return 0;
}