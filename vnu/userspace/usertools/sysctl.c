/*
 * sysctl — read and write kernel parameters.
 *
 * Every parameter is a small file under /proc/sys, and the name a
 * program uses is the path with '.' (or '/') where the file has '/':
 * `kernel.hostname' is /proc/sys/kernel/hostname. This is the procps
 * tool of the same name, narrowed to what VNU's /proc/sys actually
 * carries (my man sysctl): read a key, `-w' one, `-a' the lot, `-p' a
 * configuration file.
 *
 * The two writable keys are /proc/sys/kernel/hostname and
 * .domainname; the write itself is handled by the kernel (the write
 * path in vfs.cpp), so this program only has to hand over the bytes.
 */
#include <vlibc/stdio.h>
#include <vlibc/string.h>
#include <vlibc/unistd.h>
#include <vlibc/fcntl.h>
#include <vlibc/dirent.h>
#include "../libregex/regex.h"

#define SYS_ROOT "/proc/sys"
#define KEY_CAP  192
#define VAL_CAP  4096

static int flag_all;      /* -a: every key */
static int flag_binary;   /* -b: value without its newline */
static int flag_ignore;   /* -e: do not complain about unknown keys */
static int flag_names;    /* -N: print only the names */
static int flag_values;   /* -n: print only the values */
static int flag_quiet;    /* -q: no `key = value' echo when writing */
static int flag_write;    /* -w: the operands are assignments */
static const char* flag_pattern; /* -r: filter -a by this ERE */

static void err(const char* a, const char* b, const char* c)
{
    write(2, a, strlen(a));
    if (b)
        write(2, b, strlen(b));
    if (c)
        write(2, c, strlen(c));
    write(2, "\n", 1);
}

/* key -> /proc/sys file path. Returns 0 when the key is too long. */
static int key_path(const char* key, char* out, unsigned long cap)
{
    unsigned long i = 0;
    const char* p;
    for (p = SYS_ROOT; *p; ++p) {
        if (i + 1 >= cap)
            return 0;
        out[i++] = *p;
    }
    if (i + 1 >= cap)
        return 0;
    out[i++] = '/';
    for (p = key; *p; ++p) {
        if (i + 1 >= cap)
            return 0;
        out[i++] = (*p == '.' || *p == '/') ? '/' : *p;
    }
    out[i] = 0;
    return 1;
}

/* Read a whole parameter file, trimming the trailing newline. */
static int value_read(const char* path, char* out, unsigned long cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    long r = read(fd, out, cap - 1);
    close(fd);
    if (r < 0)
        return 0;
    unsigned long len = (unsigned long)r;
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        --len;
    out[len] = 0;
    return 1;
}

static int value_write(const char* path, const char* val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return 0;
    unsigned long n = strlen(val);
    long w = write(fd, val, n);
    close(fd);
    /* The kernel takes the bytes up to the first write's newline (and
     * there is none here), so one whole-value write is all there is. */
    return w == (long)n;
}

static void show(const char* key, const char* val)
{
    if (flag_names) {
        printf("%s\n", key);
    } else if (flag_values) {
        printf("%s%s", val, flag_binary ? "" : "\n");
    } else {
        printf("%s = %s\n", key, val);
    }
}

static int do_read(const char* key)
{
    char path[KEY_CAP];
    char val[VAL_CAP];
    if (!key_path(key, path, sizeof(path))) {
        err("sysctl: ", key, ": name too long");
        return 1;
    }
    if (!value_read(path, val, sizeof(val))) {
        if (!flag_ignore)
            err("sysctl: cannot read ", path, ": no such parameter");
        return 1;
    }
    show(key, val);
    return 0;
}

/* `key=value': split at the first '=', write the left. */
static int do_write(const char* key, const char* val)
{
    char path[KEY_CAP];
    if (!key_path(key, path, sizeof(path))) {
        err("sysctl: unknown key \"", key, "\"");
        return 1;
    }
    if (!value_write(path, val)) {
        err("sysctl: cannot write ", path, "");
        return 1;
    }
    if (!flag_quiet)
        printf("%s = %s\n", key, val);
    return 0;
}

/* Walk /proc/sys, one `key = value' line per parameter. The key is the
 * path below /proc/sys with every '/' turned into '.', so the recursion
 * builds the whole name as it descends. */
static void walk(const char* dirpath)
{
    DIR* dir = opendir(dirpath);
    if (!dir)
        return;
    struct dirent* e;
    while ((e = readdir(dir)) != 0) {
        const char* name = e->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        char child[KEY_CAP];
        unsigned long i = 0;
        const char* p = dirpath;
        for (; *p && i + 1 < sizeof(child); ++p)
            child[i++] = *p;
        if (i + 1 < sizeof(child))
            child[i++] = '/';
        for (p = name; *p && i + 1 < sizeof(child); ++p)
            child[i++] = *p;
        child[i] = 0;

        if (e->d_type == 4) {
            walk(child);
        } else {
            char key[KEY_CAP];
            i = 0;
            for (p = child + sizeof(SYS_ROOT); *p && i + 1 < sizeof(key); ++p)
                key[i++] = (*p == '/') ? '.' : *p;
            key[i] = 0;
            if (flag_pattern && !re_match(flag_pattern, key))
                continue;
            char val[VAL_CAP];
            if (value_read(child, val, sizeof(val)))
                show(key, val);
        }
    }
    closedir(dir);
}

static void copy_n(char* d, const char* s, unsigned long n)
{
    unsigned long i = 0;
    for (; s[i] && i + 1 < n; ++i)
        d[i] = s[i];
    d[i] = 0;
}

/* One `key = value' line per line, `#' comments and blanks ignored. */
static int load_file(const char* file)
{
    int fd = open(file, O_RDONLY);
    if (fd < 0) {
        err("sysctl: cannot open ", file, "");
        return 1;
    }
    static char buf[16384];
    long r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r < 0)
        return 1;
    buf[r] = 0;

    int rc = 0;
    char* line = buf;
    while (line && *line) {
        char* nl = line;
        while (*nl && *nl != '\n')
            ++nl;
        char had_nl = *nl;
        *nl = 0;
        char* p = line;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p && *p != '#') {
            char* eq = p;
            while (*eq && *eq != '=')
                ++eq;
            if (*eq == '=') {
                char* ke = eq;
                while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t'))
                    --ke;
                char* ve = eq + 1;
                while (*ve == ' ' || *ve == '\t')
                    ++ve;
                char* vend = ve;
                while (*vend)
                    ++vend;
                while (vend > ve && (vend[-1] == ' ' || vend[-1] == '\t' ||
                                     vend[-1] == '\r'))
                    --vend;
                char ksaved = *ke, vsaved = *vend;
                *ke = 0;
                *vend = 0;
                char key[KEY_CAP], val[VAL_CAP];
                copy_n(key, p, sizeof(key));
                copy_n(val, ve, sizeof(val));
                *ke = ksaved;
                *vend = vsaved;
                if (do_write(key, val))
                    rc = 1;
            }
        }
        if (!had_nl)
            break;
        line = nl + 1;
    }
    return rc;
}

static void usage(void)
{
    printf("usage: sysctl [options] [variable[=value]] ...\n");
    printf("  -a, --all            display all variables\n");
    printf("  -b, --binary         print the value without a newline\n");
    printf("  -e, --ignore         ignore unknown keys\n");
    printf("  -N, --names          print only the variable names\n");
    printf("  -n, --values         print only the values\n");
    printf("  -q, --quiet          do not echo when writing\n");
    printf("  -w, --write          write the given values\n");
    printf("  -p, --load[=FILE]    load values from FILE (default %s)\n",
           "/etc/sysctl.conf");
    printf("  -r, --pattern ERE    with -a, only keys matching ERE\n");
    printf("  -h, --help           this text\n");
    printf("  -V, --version        version\n");
}

int main(int argc, char** argv)
{
    int rc = 0;
    int i;
    for (i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (a[0] == '-' && a[1] == '-' && a[2]) {
            if (strcmp(a, "--all") == 0)
                flag_all = 1;
            else if (strcmp(a, "--binary") == 0)
                flag_binary = 1;
            else if (strcmp(a, "--ignore") == 0)
                flag_ignore = 1;
            else if (strcmp(a, "--names") == 0)
                flag_names = 1;
            else if (strcmp(a, "--values") == 0)
                flag_values = 1;
            else if (strcmp(a, "--quiet") == 0)
                flag_quiet = 1;
            else if (strcmp(a, "--write") == 0)
                flag_write = 1;
            else if (strcmp(a, "--help") == 0) {
                usage();
                return 0;
            } else if (strcmp(a, "--version") == 0) {
                printf("sysctl (VNU) 0.1\n");
                return 0;
            } else if (strcmp(a, "--load") == 0) {
                const char* f = (i + 1 < argc && argv[i + 1][0] != '-')
                                    ? argv[++i]
                                    : "/etc/sysctl.conf";
                if (load_file(f))
                    rc = 1;
            } else if (strncmp(a, "--load=", 7) == 0) {
                if (load_file(a + 7))
                    rc = 1;
            } else if (strcmp(a, "--pattern") == 0) {
                if (i + 1 < argc)
                    flag_pattern = argv[++i];
            } else if (strncmp(a, "--pattern=", 10) == 0) {
                flag_pattern = a + 10;
            } else {
                err("sysctl: unknown option ", a, "");
                return 1;
            }
            continue;
        }
        if (a[0] == '-' && a[1]) {
            int j;
            for (j = 1; a[j]; ++j) {
                char c = a[j];
                if (c == 'a')
                    flag_all = 1;
                else if (c == 'b')
                    flag_binary = 1;
                else if (c == 'e')
                    flag_ignore = 1;
                else if (c == 'N')
                    flag_names = 1;
                else if (c == 'n')
                    flag_values = 1;
                else if (c == 'q')
                    flag_quiet = 1;
                else if (c == 'w')
                    flag_write = 1;
                else if (c == 'h') {
                    usage();
                    return 0;
                } else if (c == 'V') {
                    printf("sysctl (VNU) 0.1\n");
                    return 0;
                } else if (c == 'p') {
                    const char* f;
                    if (a[j + 1])
                        f = a + j + 1;
                    else if (i + 1 < argc)
                        f = argv[++i];
                    else
                        f = "/etc/sysctl.conf";
                    if (load_file(f))
                        rc = 1;
                    break;
                } else if (c == 'r') {
                    if (a[j + 1])
                        flag_pattern = a + j + 1;
                    else if (i + 1 < argc)
                        flag_pattern = argv[++i];
                    break;
                } else {
                    char ob[2];
                    ob[0] = c;
                    ob[1] = 0;
                    err("sysctl: unknown option -", ob, 0);
                    return 1;
                }
            }
            continue;
        }

        /* An operand: read it, or write it when it assigns a value.
         * `-w' says the operands are assignments, so a bare name with it
         * is the error procps makes it. */
        const char* eq = a;
        while (*eq && *eq != '=')
            ++eq;
        if (*eq == '=') {
            char key[KEY_CAP];
            copy_n(key, a, (unsigned long)(eq - a) + 1);
            if (do_write(key, eq + 1))
                rc = 1;
        } else if (flag_write) {
            err("sysctl: ", a, " must be of the form name=value");
            rc = 1;
        } else {
            if (do_read(a))
                rc = 1;
        }
    }

    if (flag_all)
        walk(SYS_ROOT);
    return rc;
}
