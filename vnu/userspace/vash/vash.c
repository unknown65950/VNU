/*
 * vash — VNU shell (bash-like subset)
 * Builtins: cd, export, unset, exit, help, type, which
 * External commands resolved via PATH (default /bin).
 */
#include <vlibc/unistd.h>
#include <vlibc/stdio.h>
#include <vlibc/string.h>
#include <vlibc/stdlib.h>
#include <vlibc/fcntl.h>
#include <vlibc/keys.h>

#define HIST 16
#define LMAX 128
#define PATH_MAX 256

static char path_var[PATH_MAX] = "/bin";
static char history[HIST][LMAX];
static int hist_count = 0;

static void w(const char* s)
{
    if (s)
        write(1, s, strlen(s));
}

static void we(const char* s)
{
    if (s)
        write(2, s, strlen(s));
}

static void wnum(int v)
{
    char b[16];
    int i = 0;
    if (v < 0) {
        write(1, "-", 1);
        v = -v;
    }
    if (v == 0)
        b[i++] = '0';
    while (v > 0) {
        b[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (i > 0)
        write(1, &b[--i], 1);
}

static int starts_with(const char* s, const char* p)
{
    while (*p) {
        if (*s++ != *p++)
            return 0;
    }
    return 1;
}

/* Join dir + "/" + cmd into out (if dir is "/" just "/" + cmd). */
static void join_path(char* out, int outn, const char* dir, const char* cmd)
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
    for (int j = 0; cmd[j] && i < outn - 1; ++j)
        out[i++] = cmd[j];
    out[i] = 0;
}

/* Resolve cmd via PATH or absolute path. Returns 1 if found path in out. */
static int resolve_cmd(const char* cmd, char* out, int outn)
{
    if (!cmd || !cmd[0])
        return 0;
    if (cmd[0] == '/') {
        strncpy(out, cmd, outn - 1);
        out[outn - 1] = 0;
        return 1;
    }
    char dirs[PATH_MAX];
    strncpy(dirs, path_var, PATH_MAX - 1);
    dirs[PATH_MAX - 1] = 0;
    char* save = dirs;
    while (*save) {
        char* colon = save;
        while (*colon && *colon != ':')
            ++colon;
        char saved = *colon;
        *colon = 0;
        if (save[0]) {
            join_path(out, outn, save, cmd);
            int fd = open(out, 0);
            if (fd >= 0) {
                close(fd);
                return 1;
            }
        }
        if (!saved)
            break;
        save = colon + 1;
    }
    /* last resort: still form /bin/cmd for execve embed lookup */
    join_path(out, outn, "/bin", cmd);
    return 1;
}

static int is_builtin(const char* cmd)
{
    return strcmp(cmd, "cd") == 0 || strcmp(cmd, "export") == 0 ||
           strcmp(cmd, "unset") == 0 || strcmp(cmd, "exit") == 0 ||
           strcmp(cmd, "help") == 0 || strcmp(cmd, "type") == 0 ||
           strcmp(cmd, "which") == 0 || strcmp(cmd, "install") == 0;
}

static int run_builtin(int ac, char** av)
{
    const char* cmd = av[0];
    if (strcmp(cmd, "exit") == 0)
        exit(ac > 1 ? atoi(av[1]) : 0);
    if (strcmp(cmd, "install") == 0) {
        long n = syscall(VNU_SYS_blkcount);
        if (n <= 0) {
            we("install: no disks detected\n");
            return 1;
        }
        int drive = ac > 1 ? atoi(av[1]) : 0;
        if (drive < 0 || drive >= (int)n) {
            w("install: drive out of range (available: 0..");
            wnum((int)n - 1);
            w(")\n");
            return 1;
        }
        w("install: target disk ");
        wnum(drive);
        w(" of ");
        wnum((int)n);
        w(" — writing VNU...\n");
        long rc = syscall(VNU_SYS_install, drive);
        if (rc == 0) {
            w("install: complete. Reboot from this disk to use it.\n");
            return 0;
        }
        we("install: failed (error ");
        wnum((int)rc);
        we(")\n");
        return 1;
    }
    if (strcmp(cmd, "help") == 0) {
        w("vash builtins: cd export unset exit help type which install\n");
        w("PATH=");
        w(path_var);
        w("\nExternal: /bin/* via PATH (ls echo cat ...)\n");
        w("Up/Down history, Ctrl+C cancel, Shift for symbols\n");
        return 0;
    }
    if (strcmp(cmd, "cd") == 0) {
        const char* d = ac > 1 ? av[1] : "/";
        if (chdir(d) < 0) {
            we("cd: no such directory\n");
            return 1;
        }
        return 0;
    }
    if (strcmp(cmd, "export") == 0) {
        if (ac < 2) {
            w("PATH=");
            w(path_var);
            w("\n");
            return 0;
        }
        if (starts_with(av[1], "PATH=")) {
            strncpy(path_var, av[1] + 5, PATH_MAX - 1);
            path_var[PATH_MAX - 1] = 0;
            return 0;
        }
        we("export: only PATH supported\n");
        return 1;
    }
    if (strcmp(cmd, "unset") == 0) {
        if (ac > 1 && strcmp(av[1], "PATH") == 0) {
            path_var[0] = 0;
            return 0;
        }
        return 1;
    }
    if (strcmp(cmd, "type") == 0 || strcmp(cmd, "which") == 0) {
        if (ac < 2) {
            we("usage: which <cmd>\n");
            return 1;
        }
        if (is_builtin(av[1])) {
            w(av[1]);
            w(" is a shell builtin\n");
            return 0;
        }
        char resolved[PATH_MAX];
        resolve_cmd(av[1], resolved, sizeof(resolved));
        w(resolved);
        w("\n");
        return 0;
    }
    return 127;
}

/* Strips redirection operators out of av[] and applies them with
 * dup2(), leaving av[] holding just the command and its real
 * arguments. Supports "> file", ">> file", "< file", both spaced and
 * attached (">file"). Returns 0 on success, -1 if a file couldn't be
 * opened (message already printed).
 *
 * Note the redirection outlives this call deliberately: run_external()
 * execve()s straight over this process, so the dup2'd descriptors are
 * exactly what the new program inherits. The kernel resets fd 0/1/2
 * when the next shell session starts (see kernel/kernel/kernel.cpp),
 * since its fd table is global. */
static int apply_redirections(int* ac, char** av)
{
    int out = 0;
    for (int i = 0; i < *ac; ++i) {
        const char* tok = av[i];
        int mode = 0; /* 1 = >, 2 = >>, 3 = < */
        const char* target = 0;

        if (tok[0] == '>' && tok[1] == '>') {
            mode = 2;
            target = tok[2] ? tok + 2 : 0;
        } else if (tok[0] == '>') {
            mode = 1;
            target = tok[1] ? tok + 1 : 0;
        } else if (tok[0] == '<') {
            mode = 3;
            target = tok[1] ? tok + 1 : 0;
        }

        if (!mode) {
            av[out++] = av[i];
            continue;
        }
        if (!target) {
            if (i + 1 >= *ac) {
                we("vash: syntax error near redirection\n");
                return -1;
            }
            target = av[++i];
        }

        int fd;
        if (mode == 3) {
            fd = open(target, O_RDONLY);
        } else if (mode == 2) {
            fd = open(target, O_WRONLY | O_CREAT | O_APPEND);
        } else {
            fd = open(target, O_WRONLY | O_CREAT | O_TRUNC);
        }
        if (fd < 0) {
            we("vash: cannot open ");
            we(target);
            we("\n");
            return -1;
        }
        dup2(fd, mode == 3 ? 0 : 1);
        close(fd);
    }
    av[out] = 0;
    *ac = out;
    return 0;
}

static int run_external(int ac, char** av)
{
    char resolved[PATH_MAX];
    resolve_cmd(av[0], resolved, sizeof(resolved));
    /* Rebuild argv with resolved path as argv[0] for multi-call */
    char* xav[16];
    xav[0] = resolved;
    for (int i = 1; i < ac && i < 15; ++i)
        xav[i] = av[i];
    xav[ac < 15 ? ac : 15] = 0;
    execve(resolved, xav, 0);
    we("vash: command not found: ");
    we(av[0]);
    we("\n");
    return 127;
}

#define HISTFILE "/tmp/.vash_history"

/* vash does not survive running an external command: execve() replaces
 * it, and when the command exits the kernel starts a fresh vash (the
 * console respawn loop, or — for a VibeGraphics terminal window —
 * vnu::wintask::exit_respawns_root). So in-memory history would be
 * wiped after literally every command. Keeping it in a file instead
 * means it persists across those restarts, which is what makes Up/Down
 * actually useful. */
static void load_hist(void)
{
    int fd = open(HISTFILE, O_RDONLY);
    if (fd < 0)
        return;
    char buf[HIST * LMAX];
    long got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got <= 0)
        return;
    buf[got] = 0;

    hist_count = 0;
    int start = 0;
    for (long i = 0; i <= got; ++i) {
        if (buf[i] == '\n' || buf[i] == 0) {
            int len = (int)(i - start);
            if (len > 0 && len < LMAX) {
                buf[i] = 0;
                if (hist_count < HIST) {
                    strcpy(history[hist_count++], buf + start);
                } else {
                    for (int k = 1; k < HIST; ++k)
                        strcpy(history[k - 1], history[k]);
                    strcpy(history[HIST - 1], buf + start);
                }
            }
            start = (int)i + 1;
        }
    }
}

static void save_hist(void)
{
    int fd = open(HISTFILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return;
    for (int i = 0; i < hist_count; ++i) {
        write(fd, history[i], strlen(history[i]));
        write(fd, "\n", 1);
    }
    close(fd);
}

static void push_hist(const char* line)
{
    if (!line[0])
        return;
    if (hist_count > 0 && strcmp(history[hist_count - 1], line) == 0)
        return;
    if (hist_count < HIST) {
        strcpy(history[hist_count++], line);
    } else {
        for (int i = 1; i < HIST; ++i)
            strcpy(history[i - 1], history[i]);
        strcpy(history[HIST - 1], line);
    }
    save_hist();
}

static void erase_n(int n)
{
    for (int i = 0; i < n; ++i)
        w("\b \b");
}

/* Redraws everything from cursor position `from` to the end of the
 * line, blanks the one character position the shortened line just
 * vacated, then parks the terminal cursor back at `cur`. Needed
 * because this terminal has no escape-sequence support at all: the
 * only cursor control available is emitting characters and
 * backspaces, so any mid-line edit has to be repainted by hand. */
static void repaint_tail(const char* line, int n, int from, int cur)
{
    if (n > from)
        write(1, line + from, (unsigned long)(n - from));
    w(" "); /* blank the stale trailing character */
    /* Cursor now sits one past the blank; walk it back to `cur`. */
    for (int i = n + 1; i > cur; --i)
        w("\b");
}

static int read_line(char* line, int max)
{
    int n = 0;    /* length of the line */
    int cur = 0;  /* cursor position within it, 0..n */
    int hist_idx = hist_count;
    char draft[LMAX];
    draft[0] = 0;
    line[0] = 0;

    for (;;) {
        char raw = 0;
        if (read(0, &raw, 1) <= 0)
            return -1;
        unsigned char ch = (unsigned char)raw;

        if (ch == VNU_KEY_INTR) {
            w("^C\n");
            line[0] = 0;
            return 0;
        }
        if (ch == '\n') {
            w("\n");
            line[n] = 0;
            return n;
        }

        /* --- history: replace the whole line --- */
        if (ch == VNU_KEY_UP || ch == VNU_KEY_DOWN) {
            if (hist_count == 0)
                continue;
            if (ch == VNU_KEY_UP) {
                if (hist_idx == hist_count)
                    strcpy(draft, line);
                if (hist_idx > 0)
                    --hist_idx;
            } else {
                if (hist_idx < hist_count)
                    ++hist_idx;
            }
            /* Move to end of the old line before erasing it, so the
               backspaces land in the right place. */
            for (; cur < n; ++cur)
                write(1, line + cur, 1);
            erase_n(n);
            if (ch == VNU_KEY_DOWN && hist_idx >= hist_count)
                strcpy(line, draft);
            else
                strcpy(line, history[hist_idx]);
            n = (int)strlen(line);
            cur = n;
            if (n)
                write(1, line, (unsigned long)n);
            continue;
        }

        /* --- cursor movement --- */
        if (ch == VNU_KEY_LEFT) {
            if (cur > 0) {
                --cur;
                w("\b");
            }
            continue;
        }
        if (ch == VNU_KEY_RIGHT) {
            if (cur < n) {
                write(1, line + cur, 1);
                ++cur;
            }
            continue;
        }
        if (ch == VNU_KEY_HOME) {
            while (cur > 0) {
                --cur;
                w("\b");
            }
            continue;
        }
        if (ch == VNU_KEY_END) {
            while (cur < n) {
                write(1, line + cur, 1);
                ++cur;
            }
            continue;
        }

        /* --- editing --- */
        if (ch == '\b') {
            if (cur > 0) {
                for (int i = cur - 1; i < n - 1; ++i)
                    line[i] = line[i + 1];
                --n;
                --cur;
                line[n] = 0;
                w("\b");
                repaint_tail(line, n, cur, cur);
            }
            continue;
        }
        if (ch == VNU_KEY_DEL) {
            if (cur < n) {
                for (int i = cur; i < n - 1; ++i)
                    line[i] = line[i + 1];
                --n;
                line[n] = 0;
                repaint_tail(line, n, cur, cur);
            }
            continue;
        }

        /* --- insert a printable character at the cursor --- */
        if (ch >= 32 && ch < 127 && n < max - 1) {
            for (int i = n; i > cur; --i)
                line[i] = line[i - 1];
            line[cur] = (char)ch;
            ++n;
            line[n] = 0;
            write(1, &ch, 1);
            ++cur;
            if (cur < n) {
                write(1, line + cur, (unsigned long)(n - cur));
                for (int i = n; i > cur; --i)
                    w("\b");
            }
        }
    }
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    /* 0x9000: warm-start flag (shared physical mem, no paging). */
    volatile unsigned* warm = (volatile unsigned*)0x9000;
    if (!*warm) {
        w("VNU vash — PATH=");
        w(path_var);
        w("\ntype 'help'\n");
        *warm = 1;
    }
    load_hist();

    char line[LMAX];
    for (;;) {
        w("vash$ ");
        if (read_line(line, LMAX) < 0)
            break;
        if (!line[0])
            continue;
        push_hist(line);

        char* av[16];
        int ac = 0;
        char* p = line;
        while (*p && ac < 15) {
            while (*p == ' ' || *p == '\t')
                ++p;
            if (!*p)
                break;
            av[ac++] = p;
            while (*p && *p != ' ' && *p != '\t')
                ++p;
            if (*p)
                *p++ = 0;
        }
        av[ac] = 0;
        if (!ac)
            continue;

        if (apply_redirections(&ac, av) < 0)
            continue;
        if (!ac)
            continue;

        if (is_builtin(av[0]))
            run_builtin(ac, av);
        else
            run_external(ac, av);
        /* After execve success we never return; on failure shell continues.
           After successful external, kernel respawns init — whole process
           is replaced. So external always ends session; that's OK for now. */
    }
    return 0;
}
