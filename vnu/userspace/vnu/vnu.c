/*
 * vnu — what system are you on? (separate binary)
 *
 *   vnu fetch    one-screen overview of the running system
 *   vnu version  version block (OS, kernel, compiler, ABI, build date)
 *   vnu size     how much of what the system image is made of
 *   vnu install  install the system onto a disk (wizard, or
 *                vnu install DRIVE HOSTNAME [SIZE_MIB])
 *
 * Every number printed here is read from the running system: uname(2),
 * /proc/version, /proc/meminfo, /proc/gfx, /proc/boot, /proc/dfstat
 * and /proc/images. Nothing is hard-coded, so a rebuilt kernel or a
 * different machine reports itself, not this file.
 *
 * vlibc is deliberately tiny (no snprintf, no strchr), so the handful
 * of string/number helpers below live here; the command is one binary
 * with no library of its own.
 */
#include <vlibc/unistd.h>
#include <vlibc/string.h>
#include <vlibc/stdlib.h>
#include <vlibc/fcntl.h>
#include <vlibc/time.h>
#include <vlibc/sys/utsname.h>
#include <vlibc/sys/syscall.h>
#include <vnu/abi.h>

/* vcc's own version number, shared with the compiler binary. */
#include "../vcc/vcc_version.h"

/* The disk installer, reached as `vnu install`: one binary, so the
 * wizard lives in a header (see AGENTS.md). */
#include "install.h"

#define PROC_CAP 4096

static void w(const char* s) { write(1, s, strlen(s)); }
static void we(const char* s) { write(2, s, strlen(s)); }

/* ---- tiny helpers vlibc does not provide -------------------------- */

static char* find_ch(const char* s, char c)
{
    for (; *s; ++s) {
        if (*s == c)
            return (char*)s;
    }
    return 0;
}

/* Copy at most cap-1 bytes of `src` (stopping at its end) and
 * NUL-terminate. Blanks at either end are dropped: values cut out of
 * /proc text ("0.7 (Canyon) i386" sliced at the paren) are full of them. */
static void set_str(char* dst, int cap, const char* src)
{
    if (cap < 1)
        return;
    while (src && (*src == ' ' || *src == '\t'))
        ++src;
    int n = 0;
    while (src && src[n] && n < cap - 1)
        ++n;
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t'))
        --n;
    for (int i = 0; i < n; ++i)
        dst[i] = src[i];
    dst[n] = 0;
}

static void put_uint(unsigned long v)
{
    char d[12];
    int n = 0;
    if (!v) {
        w("0");
        return;
    }
    while (v && n < 12) {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    char o[13];
    for (int i = 0; i < n; ++i)
        o[i] = d[n - 1 - i];
    o[n] = 0;
    w(o);
}

/* "1234567" -> "1.2 MiB" (1024-based, one decimal, KiB below 1 MiB). */
static void put_size(unsigned long bytes)
{
    if (bytes < 1024ul) {
        put_uint(bytes);
        w(" B");
        return;
    }
    if (bytes < 1024ul * 1024ul) {
        put_uint(bytes / 1024ul);
        w(".");
        put_uint((bytes % 1024ul) * 10 / 1024ul);
        w(" KiB");
        return;
    }
    put_uint(bytes / (1024ul * 1024ul));
    w(".");
    put_uint((bytes % (1024ul * 1024ul)) * 10 / (1024ul * 1024ul));
    w(" MiB");
}

/* "3725" -> "1 h 2 min", "905" -> "15 min", "42" -> "42 s". */
static void put_uptime(unsigned long secs)
{
    if (secs >= 3600ul) {
        put_uint(secs / 3600ul);
        w(" h ");
        put_uint((secs % 3600ul) / 60ul);
        w(" min");
    } else if (secs >= 60ul) {
        put_uint(secs / 60ul);
        w(" min");
    } else {
        put_uint(secs);
        w(" s");
    }
}

/* ---- /proc reading ------------------------------------------------ */

/* Read a whole /proc file into `buf`. Returns its length, or -1. The
 * kernel regenerates these files on every read, so no cache. */
static int read_proc(const char* path, char* buf, int cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = 0;
    while (n < cap - 1) {
        int r = read(fd, buf + n, cap - 1 - n);
        if (r <= 0)
            break;
        n += r;
    }
    close(fd);
    buf[n] = 0;
    return n;
}

/* Copy the value of `key` out of a /proc-style text file. The key must
 * start a line and be followed by ':', a tab or a space, so "MemTotal:"
 * and "driver\tvirtio-gpu" both parse. Returns 0 on success. */
static int field(const char* text, const char* key, char* out, int cap)
{
    int klen = (int)strlen(key);
    const char* p = text;
    while (*p) {
        const char* eol = p;
        while (*eol && *eol != '\n')
            ++eol;
        int len = (int)(eol - p);
        if (len > klen && strncmp(p, key, (unsigned long)klen) == 0 &&
            (p[klen] == ':' || p[klen] == '\t' || p[klen] == ' ')) {
            const char* v = p + klen;
            while (v < eol && (*v == ':' || *v == '\t' || *v == ' '))
                ++v;
            int n = 0;
            while (v < eol && n < cap - 1)
                out[n++] = *v++;
            while (n > 0 && out[n - 1] == ' ')
                --n;
            out[n] = 0;
            return 0;
        }
        p = *eol ? eol + 1 : eol;
    }
    return -1;
}

/* First unsigned number in the value of `key`. /proc/meminfo counts
 * kB, everything else bytes. Returns 0 when the key is absent. */
static unsigned long field_num(const char* text, const char* key, int kb)
{
    char val[32];
    if (field(text, key, val, sizeof(val)) < 0)
        return 0;
    unsigned long v = 0;
    for (int i = 0; val[i] >= '0' && val[i] <= '9'; ++i)
        v = v * 10 + (unsigned long)(val[i] - '0');
    return kb ? v * 1024ul : v;
}

/* ---- output shape ------------------------------------------------- */

static void label(const char* name)
{
    w(name);
    for (int i = (int)strlen(name); i < 9; ++i)
        w(" ");
}

static void row(const char* name, const char* value)
{
    label(name);
    w(":  ");
    w(value);
    w("\n");
}

static void row_live(const char* name, const char* value)
{
    label(name);
    w(":  ");
    w(value);
}

/* ---- /proc/version, parsed once ---------------------------------- */

struct VerInfo {
    char os[32];
    char codename[32];
    char kernel[32];
    char abi[16];
    char built[32];
};

/* /proc/version is one `key value` fact per line, e.g.
 *   version 0.7 (Canyon) i386
 *   kernel 0.7.0
 *   abi 1
 *   built 2026-09-26 11:02:03
 * Reading it (rather than re-declaring the numbers here) is what keeps
 * `vnu` and the kernel from ever disagreeing. */
static void read_version(struct VerInfo* vi)
{
    char proc[PROC_CAP];
    char v[64];

    set_str(vi->os, sizeof(vi->os), "?");
    set_str(vi->codename, sizeof(vi->codename), "");
    set_str(vi->kernel, sizeof(vi->kernel), "?");
    set_str(vi->abi, sizeof(vi->abi), "?");
    set_str(vi->built, sizeof(vi->built), "?");

    if (read_proc("/proc/version", proc, sizeof(proc)) <= 0)
        return;

    if (field(proc, "version", v, sizeof(v)) == 0) {
        char* open = find_ch(v, '(');
        char* close = open ? find_ch(open, ')') : 0;
        if (open && close) {
            /* "0.7 (Canyon) i386" -> os "0.7", codename "Canyon" */
            int n = (int)(open - v);
            if (n > 0) {
                char* tmp = v;
                tmp[n] = 0;
                set_str(vi->os, sizeof(vi->os), v);
            }
            int cn = (int)(close - open - 1);
            if (cn > 0) {
                char* tmp = open + 1;
                tmp[cn] = 0;
                set_str(vi->codename, sizeof(vi->codename), open + 1);
            }
        } else {
            set_str(vi->os, sizeof(vi->os), v);
        }
    }
    if (field(proc, "kernel", v, sizeof(v)) == 0)
        set_str(vi->kernel, sizeof(vi->kernel), v);
    if (field(proc, "abi", v, sizeof(v)) == 0)
        set_str(vi->abi, sizeof(vi->abi), v);
    if (field(proc, "built", v, sizeof(v)) == 0)
        set_str(vi->built, sizeof(vi->built), v);
}

/* ---- subcommands -------------------------------------------------- */

static int cmd_fetch(void)
{
    struct utsname u;
    int have_uname = uname(&u) == 0;
    struct VerInfo vi;
    read_version(&vi);

    w("VNU system information\n\n");

    row_live("OS", have_uname ? u.sysname : "VNU");
    w(" ");
    w(vi.os);
    if (vi.codename[0]) {
        w(" (");
        w(vi.codename);
        w(")");
    }
    w("\n");

    w("\n");
    row("Kernel", have_uname ? u.release : vi.kernel);

    char host[64];
    if (gethostname(host, sizeof(host)) == 0)
        row("Host", host);

    label("Uptime");
    w(":  ");
    put_uptime(uptime());
    w("\n");

    char proc[PROC_CAP];
    if (read_proc("/proc/meminfo", proc, sizeof(proc)) > 0) {
        unsigned long total = field_num(proc, "MemTotal", 1);
        unsigned long pool = field_num(proc, "PoolTotal", 1);
        unsigned long used = field_num(proc, "MemUsed", 1);
        label("Memory");
        w(":  ");
        put_size(total);
        w(pool && pool != total ? " RAM, " : " total, ");
        put_size(used);
        w(" used");
        if (pool && pool != total) {
            w(" of ");
            put_size(pool);
            w(" pool");
        }
        w("\n");
    }

    /* Session facts: the shell running this command, the root
     * filesystem, and whether an installed copy exists on a disk. */
    if (read_proc("/proc/boot", proc, sizeof(proc)) > 0) {
        char shell[32];
        char rootfs[32];
        char inst[32];
        set_str(shell, sizeof(shell), "?");
        set_str(rootfs, sizeof(rootfs), "?");
        set_str(inst, sizeof(inst), "none");
        field(proc, "shell", shell, sizeof(shell));
        field(proc, "rootfs", rootfs, sizeof(rootfs));
        field(proc, "installed", inst, sizeof(inst));
        row("Shell", shell);
        label("FS");
        w(":  ");
        w(rootfs);
        w(" (");
        if (strcmp(inst, "none") == 0) {
            w("live, no installed system");
        } else {
            w("installed on ");
            w(inst);
        }
        w(")\n");
    }

    /* The display is asked for, not parsed: gfx_getinfo(2) answers with
     * the mode in use, its depth and the driver that owns it, in one
     * call, so this line keeps working when the depth stops being 8. */
    {
        struct vnu_gfx_info gfx;
        if (syscall(SYS_gfx_getinfo, (unsigned long)&gfx) == 0) {
            label("Graphics");
            w(":  ");
            w(gfx.driver == VNU_GFX_DRIVER_VIRTIO_GPU ? "virtio-gpu"
                                                      : "vga");
            w(" ");
            put_uint(gfx.width);
            w("x");
            put_uint(gfx.height);
            w(" ");
            put_uint(gfx.bpp);
            w("bpp\n");
        }
    }

    row("VCC", VCC_VERSION);
    row("Build", vi.built);
    return 0;
}

static int cmd_version(void)
{
    struct utsname u;
    int have_uname = uname(&u) == 0;
    struct VerInfo vi;
    read_version(&vi);

    w("VNU version information\n\n");
    row_live("OS", have_uname ? u.sysname : "VNU");
    w(" ");
    w(vi.os);
    w("\n");
    row("Kernel", vi.kernel);
    row("Compiler", VCC_VERSION);
    row("ABI", vi.abi);
    row("Build", vi.built);
    if (have_uname) {
        row("Arch", u.machine);
        row("Node", u.nodename);
    }
    return 0;
}

static int cmd_size(void)
{
    char proc[PROC_CAP];
    if (read_proc("/proc/images", proc, sizeof(proc)) <= 0) {
        we("vnu: cannot read /proc/images\n");
        return 1;
    }

    w("VNU image size\n\n");
    const char* p = proc;
    while (*p) {
        const char* eol = p;
        while (*eol && *eol != '\n')
            ++eol;

        /* `name\tbytes\tfiles`, split on the tabs. */
        const char* t1 = p;
        while (t1 < eol && *t1 != '\t')
            ++t1;
        if (t1 < eol) {
            const char* t2 = t1 + 1;
            while (t2 < eol && *t2 != '\t')
                ++t2;

            char name[32];
            int n = 0;
            while (p + n < t1 && n < (int)sizeof(name) - 1) {
                name[n] = p[n];
                ++n;
            }
            name[n] = 0;

            unsigned long bytes = 0, files = 0;
            for (const char* q = t1 + 1; q < t2; ++q) {
                if (*q < '0' || *q > '9')
                    break;
                bytes = bytes * 10 + (unsigned long)(*q - '0');
            }
            for (const char* q = t2 + 1; q < eol; ++q) {
                if (*q < '0' || *q > '9')
                    break;
                files = files * 10 + (unsigned long)(*q - '0');
            }

            label(name);
            w(":  ");
            put_size(bytes);
            w("  (");
            put_uint(files);
            w(files == 1 ? " object)\n" : " objects)\n");
        }
        p = *eol ? eol + 1 : eol;
    }
    return 0;
}

static void usage(void)
{
    w("Usage: vnu [COMMAND]...\n");
    w("Report what system you are running on.\n\n");
    w("  fetch     overview of the running system (default)\n");
    w("  version   version block: OS, kernel, compiler, ABI, build date\n");
    w("  size      how much of what the system image is made of\n");
    w("  install   install VNU onto a disk: no arguments opens the\n");
    w("            wizard, or DRIVE HOSTNAME [SIZE_MIB] does it\n");
    w("            without asking\n");
    w("  --help    display this help and exit\n");
    w("  --version output version information and exit\n");
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage();
        return 0;
    }
    const char* cmd = argv[1];
    if (strcmp(cmd, "fetch") == 0)
        return cmd_fetch();
    if (strcmp(cmd, "version") == 0)
        return cmd_version();
    if (strcmp(cmd, "size") == 0)
        return cmd_size();
    if (strcmp(cmd, "install") == 0)
        return vnu_install(argc - 1, argv + 1);
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage();
        return 0;
    }
    if (strcmp(cmd, "--version") == 0) {
        /* The node's version is the OS release, not the compiler's. */
        struct VerInfo vi;
        read_version(&vi);
        w("vnu (VNU) ");
        if (vi.os[0])
            w(vi.os);
        else
            w(VCC_VERSION);
        w("\n");
        return 0;
    }
    we("vnu: unknown command: ");
    we(cmd);
    we("\n");
    usage();
    return 1;
}
