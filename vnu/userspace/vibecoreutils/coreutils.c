/*
 * VNU freestanding multi-call coreutils — alternative to host vibecoreutils.
 * Behavior selected by argv[0] basename (busybox style).
 */
#include <vlibc/unistd.h>
#include <vlibc/stdio.h>
#include <vlibc/string.h>
#include <vlibc/stdlib.h>
#include <vlibc/fcntl.h>
#include <vlibc/dirent.h>
#include <vlibc/sys/stat.h>
#include <vlibc/sys/utsname.h>
#include <vlibc/sys/wait.h>
#include <vlibc/keys.h>

static const char* base(const char* p)
{
    const char* s = p;
    for (; *p; ++p)
        if (*p == '/')
            s = p + 1;
    return s;
}

static void w(const char* s) { write(1, s, strlen(s)); }
static void we(const char* s) { write(2, s, strlen(s)); }

static int cmd_echo(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        w(argv[i]);
        if (i + 1 < argc)
            w(" ");
    }
    w("\n");
    return 0;
}

static int cmd_true(int a, char** v) { (void)a; (void)v; return 0; }
static int cmd_false(int a, char** v) { (void)a; (void)v; return 1; }

static int cmd_pwd(int a, char** v)
{
    (void)a; (void)v;
    char buf[128];
    if (!getcwd(buf, sizeof(buf))) {
        we("pwd: fail\n");
        return 1;
    }
    w(buf);
    w("\n");
    return 0;
}

static int cmd_cat(int argc, char** argv)
{
    char buf[128];
    if (argc < 2) {
        for (;;) {
            long n = read(0, buf, sizeof(buf));
            if (n <= 0)
                break;
            write(1, buf, (unsigned long)n);
        }
        return 0;
    }
    for (int i = 1; i < argc; ++i) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            we("cat: cannot open ");
            we(argv[i]);
            we("\n");
            continue;
        }
        for (;;) {
            long n = read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            write(1, buf, (unsigned long)n);
        }
        close(fd);
    }
    return 0;
}

static int cmd_ls(int argc, char** argv)
{
    char cwd[128];
    const char* path = "/";
    if (argc > 1 && argv[1] && argv[1][0]) {
        path = argv[1];
        if (path[0] == '.' && path[1] == 0) {
            if (getcwd(cwd, sizeof(cwd)))
                path = cwd;
            else
                path = "/";
        }
    } else {
        if (getcwd(cwd, sizeof(cwd)))
            path = cwd;
        else
            path = "/";
    }
    DIR* d = opendir(path);
    if (!d) {
        we("ls: cannot open ");
        we(path);
        we("\n");
        return 1;
    }
    struct dirent* e;
    while ((e = readdir(d)) != 0) {
        w(e->d_name);
        w(e->d_type == 4 ? "/\n" : "\n");
    }
    closedir(d);
    return 0;
}

static int cmd_mkdir(int argc, char** argv)
{
    if (argc < 2) {
        we("mkdir: missing operand\n");
        return 1;
    }
    for (int i = 1; i < argc; ++i)
        if (mkdir(argv[i], 0755) < 0) {
            we("mkdir: fail ");
            we(argv[i]);
            we("\n");
        }
    return 0;
}

static int cmd_rm(int argc, char** argv)
{
    if (argc < 2) {
        we("rm: missing operand\n");
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        if (unlink(argv[i]) < 0 && rmdir(argv[i]) < 0) {
            we("rm: fail ");
            we(argv[i]);
            we("\n");
        }
    }
    return 0;
}

static int cmd_touch(int argc, char** argv)
{
    if (argc < 2)
        return 1;
    for (int i = 1; i < argc; ++i) {
        int fd = open(argv[i], O_CREAT);
        if (fd >= 0)
            close(fd);
    }
    return 0;
}

static void uname_help(void)
{
    w("Usage: uname [OPTION]...\n");
    w("Print certain system information. With no OPTION, same as -s.\n\n");
    w("  -a, --all                 print all information\n");
    w("  -s, --kernel-name         print the kernel name\n");
    w("  -n, --nodename            print the network node hostname\n");
    w("  -r, --kernel-release      print the kernel release\n");
    w("  -v, --kernel-version      print the kernel version\n");
    w("  -m, --machine             print the machine hardware name\n");
    w("  -p, --processor           print the processor type\n");
    w("  -i, --hardware-platform   print the hardware platform\n");
    w("  -o, --operating-system    print the operating system\n");
    w("      --help                display this help and exit\n");
    w("      --version             output version information and exit\n");
}

static void uname_field(const char* s, int* first)
{
    if (!*first)
        w(" ");
    w(s);
    *first = 0;
}

static int cmd_uname(int argc, char** argv)
{
    struct utsname u;
    if (uname(&u) < 0)
        return 1;
    if (argc == 1) {
        w(u.sysname);
        w("\n");
        return 0;
    }

    int all = 0, first = 1;
    int show_s = 0, show_n = 0, show_r = 0, show_v = 0, show_m = 0;
    int show_p = 0, show_i = 0, show_o = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "--help") == 0) {
            uname_help();
            return 0;
        }
        if (strcmp(a, "--version") == 0) {
            w("uname (VNU coreutils) 0.3\n");
            return 0;
        }
        if (strcmp(a, "--all") == 0) { all = 1; continue; }
        if (strcmp(a, "--kernel-name") == 0) { show_s = 1; continue; }
        if (strcmp(a, "--nodename") == 0) { show_n = 1; continue; }
        if (strcmp(a, "--kernel-release") == 0) { show_r = 1; continue; }
        if (strcmp(a, "--kernel-version") == 0) { show_v = 1; continue; }
        if (strcmp(a, "--machine") == 0) { show_m = 1; continue; }
        if (strcmp(a, "--processor") == 0) { show_p = 1; continue; }
        if (strcmp(a, "--hardware-platform") == 0) { show_i = 1; continue; }
        if (strcmp(a, "--operating-system") == 0) { show_o = 1; continue; }
        if (a[0] == '-' && a[1] != '-') {
            for (int j = 1; a[j]; ++j) {
                switch (a[j]) {
                case 'a': all = 1; break;
                case 's': show_s = 1; break;
                case 'n': show_n = 1; break;
                case 'r': show_r = 1; break;
                case 'v': show_v = 1; break;
                case 'm': show_m = 1; break;
                case 'p': show_p = 1; break;
                case 'i': show_i = 1; break;
                case 'o': show_o = 1; break;
                default:
                    we("uname: invalid option\n");
                    return 1;
                }
            }
        } else {
            we("uname: unexpected argument\n");
            return 1;
        }
    }
    if (all)
        show_s = show_n = show_r = show_v = show_m = show_p = show_i = show_o = 1;
    if (!show_s && !show_n && !show_r && !show_v && !show_m && !show_p && !show_i && !show_o)
        show_s = 1;
    if (show_s) uname_field(u.sysname, &first);
    if (show_n) uname_field(u.nodename, &first);
    if (show_r) uname_field(u.release, &first);
    if (show_v) uname_field(u.version, &first);
    if (show_m) uname_field(u.machine, &first);
    if (show_p) uname_field(u.processor, &first);
    if (show_i) uname_field(u.hardware_platform, &first);
    if (show_o) uname_field(u.operating_system, &first);
    w("\n");
    return 0;
}

static int cmd_clear(int a, char** v)
{
    (void)a;
    (void)v;
    for (int i = 0; i < 40; ++i)
        w("\n");
    return 0;
}

static void put_uint(unsigned long v)
{
    char digits[12];
    int n = 0;
    if (v == 0) {
        w("0");
        return;
    }
    while (v > 0 && n < 12) {
        digits[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    char out[13];
    for (int i = 0; i < n; ++i)
        out[i] = digits[n - 1 - i];
    out[n] = 0;
    w(out);
}

/* wc: count lines, words, bytes. No args or "-" reads stdin. */
static int cmd_wc(int argc, char** argv)
{
    char buf[256];
    int any_file = 0;
    for (int a = 1; a < argc || (a == 1 && argc <= 1); ++a) {
        int fd;
        const char* label = 0;
        if (argc <= 1) {
            fd = 0;
            a = argc; /* stop after this single stdin pass */
        } else {
            fd = open(argv[a], O_RDONLY);
            label = argv[a];
            if (fd < 0) {
                we("wc: cannot open ");
                we(argv[a]);
                we("\n");
                continue;
            }
            any_file = 1;
        }
        unsigned long lines = 0, words = 0, bytes = 0;
        int in_word = 0;
        for (;;) {
            long n = read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            for (long i = 0; i < n; ++i) {
                char c = buf[i];
                ++bytes;
                if (c == '\n')
                    ++lines;
                if (c == ' ' || c == '\t' || c == '\n') {
                    in_word = 0;
                } else if (!in_word) {
                    in_word = 1;
                    ++words;
                }
            }
        }
        if (fd != 0)
            close(fd);
        put_uint(lines);
        w(" ");
        put_uint(words);
        w(" ");
        put_uint(bytes);
        if (label) {
            w(" ");
            w(label);
        }
        w("\n");
        if (argc <= 1)
            break;
    }
    (void)any_file;
    return 0;
}

/* head/tail share a line-oriented reader that copies at most
 * MAX_LINES lines (each up to LINE_CAP-1 bytes) into a fixed buffer —
 * enough for this OS's small text files, no dynamic allocation. Kept
 * deliberately small: this lives in coreutils's own .bss, which has to
 * fit inside the small private app-image region every process gets
 * (see kernel/proc/process.cpp) alongside the actual code. */
#define MAX_LINES 128
#define LINE_CAP 100

static int read_lines(int fd, char lines[MAX_LINES][LINE_CAP], int* out_count)
{
    int count = 0;
    int col = 0;
    char buf[256];
    for (;;) {
        /* Stop once the buffer is full rather than reading on and
         * discarding: /dev/zero and /dev/urandom never return EOF, so
         * without this, head/tail/grep/sort would spin forever on
         * them. Means these tools only ever see the first MAX_LINES
         * lines of a file, which is the documented trade-off of the
         * fixed-size buffer. */
        if (count >= MAX_LINES)
            break;
        long n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        for (long i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\n') {
                if (count < MAX_LINES) {
                    lines[count][col < LINE_CAP - 1 ? col : LINE_CAP - 1] = 0;
                    ++count;
                }
                col = 0;
            } else if (count < MAX_LINES && col < LINE_CAP - 1) {
                lines[count][col++] = c;
                lines[count][col] = 0;
            }
        }
    }
    if (col > 0 && count < MAX_LINES) {
        lines[count][col < LINE_CAP - 1 ? col : LINE_CAP - 1] = 0;
        ++count;
    }
    *out_count = count;
    return 0;
}

static int open_or_stdin(int argc, char** argv, int arg_index, int* is_stdin)
{
    if (argc <= arg_index) {
        *is_stdin = 1;
        return 0;
    }
    *is_stdin = 0;
    int fd = open(argv[arg_index], O_RDONLY);
    if (fd < 0) {
        we("cannot open ");
        we(argv[arg_index]);
        we("\n");
    }
    return fd;
}

static int cmd_head(int argc, char** argv)
{
    int n = 10;
    int file_arg = 1;
    if (argc > 2 && argv[1][0] == '-') {
        n = atoi(argv[1] + 1);
        if (n <= 0)
            n = 10;
        file_arg = 2;
    }
    int is_stdin;
    int fd = open_or_stdin(argc, argv, file_arg, &is_stdin);
    if (fd < 0)
        return 1;
    static char lines[MAX_LINES][LINE_CAP];
    int count = 0;
    read_lines(fd, lines, &count);
    if (!is_stdin)
        close(fd);
    if (n > count)
        n = count;
    for (int i = 0; i < n; ++i) {
        w(lines[i]);
        w("\n");
    }
    return 0;
}

static int cmd_tail(int argc, char** argv)
{
    int n = 10;
    int file_arg = 1;
    if (argc > 2 && argv[1][0] == '-') {
        n = atoi(argv[1] + 1);
        if (n <= 0)
            n = 10;
        file_arg = 2;
    }
    int is_stdin;
    int fd = open_or_stdin(argc, argv, file_arg, &is_stdin);
    if (fd < 0)
        return 1;
    static char lines[MAX_LINES][LINE_CAP];
    int count = 0;
    read_lines(fd, lines, &count);
    if (!is_stdin)
        close(fd);
    int start = count - n;
    if (start < 0)
        start = 0;
    for (int i = start; i < count; ++i) {
        w(lines[i]);
        w("\n");
    }
    return 0;
}

static int contains(const char* hay, const char* needle)
{
    if (!*needle)
        return 1;
    for (int i = 0; hay[i]; ++i) {
        int j = 0;
        while (hay[i + j] && needle[j] && hay[i + j] == needle[j])
            ++j;
        if (!needle[j])
            return 1;
    }
    return 0;
}

/* grep: plain substring match (no regex). */
static int cmd_grep(int argc, char** argv)
{
    if (argc < 2) {
        we("grep: missing pattern\n");
        return 1;
    }
    const char* pattern = argv[1];
    int is_stdin;
    int fd = open_or_stdin(argc, argv, 2, &is_stdin);
    if (fd < 0)
        return 1;
    static char lines[MAX_LINES][LINE_CAP];
    int count = 0;
    read_lines(fd, lines, &count);
    if (!is_stdin)
        close(fd);
    int found = 1;
    for (int i = 0; i < count; ++i) {
        if (contains(lines[i], pattern)) {
            w(lines[i]);
            w("\n");
            found = 0;
        }
    }
    return found;
}

/* sort: whole-file line sort, ascending, simple insertion sort — fine
 * for this OS's small files (MAX_LINES cap, same as head/tail/grep). */
static int cmd_sort(int argc, char** argv)
{
    int is_stdin;
    int fd = open_or_stdin(argc, argv, 1, &is_stdin);
    if (fd < 0)
        return 1;
    static char lines[MAX_LINES][LINE_CAP];
    int count = 0;
    read_lines(fd, lines, &count);
    if (!is_stdin)
        close(fd);
    for (int i = 1; i < count; ++i) {
        char tmp[LINE_CAP];
        strcpy(tmp, lines[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(lines[j], tmp) > 0) {
            strcpy(lines[j + 1], lines[j]);
            --j;
        }
        strcpy(lines[j + 1], tmp);
    }
    for (int i = 0; i < count; ++i) {
        w(lines[i]);
        w("\n");
    }
    return 0;
}

static int copy_file(const char* src, const char* dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0) {
        we("cannot open ");
        we(src);
        we("\n");
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        we("cannot create ");
        we(dst);
        we("\n");
        close(in);
        return -1;
    }
    char buf[256];
    for (;;) {
        long n = read(in, buf, sizeof(buf));
        if (n <= 0)
            break;
        write(out, buf, (unsigned long)n);
    }
    close(in);
    close(out);
    return 0;
}

static int cmd_cp(int argc, char** argv)
{
    if (argc < 3) {
        we("cp: usage: cp SRC DST\n");
        return 1;
    }
    return copy_file(argv[1], argv[2]) == 0 ? 0 : 1;
}

static int cmd_mv(int argc, char** argv)
{
    if (argc < 3) {
        we("mv: usage: mv SRC DST\n");
        return 1;
    }
    /* No rename() in this VFS yet — copy then remove the original. */
    if (copy_file(argv[1], argv[2]) != 0)
        return 1;
    unlink(argv[1]);
    return 0;
}

static int cmd_basename(int argc, char** argv)
{
    if (argc < 2) {
        we("basename: missing operand\n");
        return 1;
    }
    w(base(argv[1]));
    w("\n");
    return 0;
}

/* dirname: everything before the last non-trailing '/'. Trailing
 * slashes are stripped first (so "a/b/" behaves like "a/b"), then any
 * further trailing slashes on the result are also stripped (so
 * "//a" -> "/", not "//"). No path left after that means the operand
 * had no directory part, i.e. ".". */
static int cmd_dirname(int argc, char** argv)
{
    if (argc < 2) {
        we("dirname: missing operand\n");
        return 1;
    }
    char buf[256];
    strncpy(buf, argv[1], sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    int n = (int)strlen(buf);
    while (n > 1 && buf[n - 1] == '/')
        buf[--n] = 0;
    while (n > 0 && buf[n - 1] != '/')
        --n;
    if (n == 0) {
        w(".\n");
    } else {
        while (n > 1 && buf[n - 1] == '/')
            --n;
        buf[n] = 0;
        w(buf);
        w("\n");
    }
    return 0;
}

/* seq: seq N | seq FIRST LAST | seq FIRST STEP LAST */
static int cmd_seq(int argc, char** argv)
{
    long first = 1, step = 1, last = 0;
    if (argc == 2) {
        last = atol(argv[1]);
    } else if (argc == 3) {
        first = atol(argv[1]);
        last = atol(argv[2]);
    } else if (argc >= 4) {
        first = atol(argv[1]);
        step = atol(argv[2]);
        last = atol(argv[3]);
    } else {
        we("seq: usage: seq [FIRST [STEP]] LAST\n");
        return 1;
    }
    if (step == 0)
        step = 1;
    if (step > 0) {
        for (long v = first; v <= last; v += step) {
            put_uint((unsigned long)v);
            w("\n");
        }
    } else {
        for (long v = first; v >= last; v += step) {
            put_uint((unsigned long)v);
            w("\n");
        }
    }
    return 0;
}

/* Minimal shell (vash mode) */
typedef int (*cmd_fn)(int, char**);

static struct {
    const char* name;
    cmd_fn fn;
} table[] = {
    {"echo", cmd_echo},
    {"true", cmd_true},
    {"false", cmd_false},
    {"pwd", cmd_pwd},
    {"cat", cmd_cat},
    {"ls", cmd_ls},
    {"mkdir", cmd_mkdir},
    {"rm", cmd_rm},
    {"touch", cmd_touch},
    {"uname", cmd_uname},
    {"clear", cmd_clear},
    {"wc", cmd_wc},
    {"head", cmd_head},
    {"tail", cmd_tail},
    {"grep", cmd_grep},
    {"sort", cmd_sort},
    {"cp", cmd_cp},
    {"mv", cmd_mv},
    {"basename", cmd_basename},
    {"dirname", cmd_dirname},
    {"seq", cmd_seq},
    {0, 0},
};

int main(int argc, char** argv)
{
    const char* name = (argc > 0 && argv[0]) ? base(argv[0]) : "coreutils";
    for (int i = 0; table[i].name; ++i)
        if (strcmp(name, table[i].name) == 0)
            return table[i].fn(argc, argv);
    w("coreutils: unknown applet ");
    w(name);
    w("\n");
    return 1;
}
