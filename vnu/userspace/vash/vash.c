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
#include <vlibc/sys/stat.h>

/* read_line is defined near the bottom (after load_hist); login/session
 * helpers above it need the prototype. */
static int read_line(char* line, int max);

#define HIST 16
#define LMAX 128
#define PATH_MAX 256

static char path_var[PATH_MAX] = "/bin";
static char history[HIST][LMAX];
static int hist_count = 0;

/* --- multiuser session state ---
 * vash is execve()d over by every external command and respawned fresh
 * by the kernel afterwards, so the login identity is re-read from
 * /tmp/.session on each respawn (uid/gid themselves survive the exec
 * since the kernel keeps them on the Process; the file carries the
 * username and home directory across the shell's death). At a plain
 * boot there is no session file yet, and the login prompt creates one. */
#define PASSWD_F   "/etc/passwd"
#define GROUP_F    "/etc/group"
#define SESSION_F  "/tmp/.session"
#define MAX_USERS  8

static char cur_user[32] = "root";
static unsigned long cur_uid = 0;
static unsigned long cur_gid = 0;
static char home_dir[PATH_MAX] = "/root";

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

static void wnumu(unsigned long v)
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

/* djb2 hash of a password -> 8 hex chars. This is what /etc/passwd
 * stores in field 2 (see the kernel-seeded accounts and useradd). */
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

struct PwEntry {
    char name[32];
    char hash[17];
    unsigned long uid;
    unsigned long gid;
    char home[64];
};

/* Read /etc/passwd (`name:hash:uid:gid:gecos:home:shell`) into `out`.
 * Returns the number of entries parsed (capped at MAX_USERS). */
static int read_passwd(struct PwEntry* out)
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
    int n = 0;
    char* line = buf;
    while (line && *line && n < MAX_USERS) {
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
                struct PwEntry* e = &out[n];
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

static int pw_lookup(const char* name, struct PwEntry* out)
{
    struct PwEntry all[MAX_USERS];
    int n = read_passwd(all);
    for (int i = 0; i < n; ++i)
        if (strcmp(all[i].name, name) == 0) {
            *out = all[i];
            return 1;
        }
    return 0;
}

/* Group name for a gid from /etc/group, or NULL if none. */
static const char* group_name(unsigned long gid)
{
    static char gname[32];
    int fd = open(GROUP_F, O_RDONLY);
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

/* --- building strings (no snprintf in vlibc) --- */
static int nappend(char* b, int n, int cap, const char* s)
{
    while (s && *s && n < cap - 1)
        b[n++] = *s++;
    return n;
}

static int uappend(char* b, int n, int cap, unsigned long v)
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

/* Read a line without echoing (for passwords). Ctrl+C aborts (-1). */
static int read_secret(char* out, int max)
{
    int n = 0;
    for (;;) {
        char raw = 0;
        if (read(0, &raw, 1) <= 0)
            return -1;
        unsigned char ch = (unsigned char)raw;
        if (ch == '\n') {
            w("\n");
            out[n] = 0;
            return n;
        }
        if (ch == VNU_KEY_INTR) {
            w("^C\n");
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

static void save_session(void)
{
    char b[192];
    int n = 0;
    n = nappend(b, n, sizeof(b), cur_user);
    b[n++] = ' ';
    n = uappend(b, n, sizeof(b), cur_uid);
    b[n++] = ' ';
    n = uappend(b, n, sizeof(b), cur_gid);
    b[n++] = ' ';
    n = nappend(b, n, sizeof(b), home_dir);
    b[n++] = '\n';
    b[n] = 0;
    int fd = open(SESSION_F, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return;
    write(fd, b, (unsigned long)n);
    close(fd);
}

static void drop_session(void)
{
    unlink(SESSION_F);
}

static int access_session_exists(void)
{
    int fd = open(SESSION_F, O_RDONLY);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static int next_word(char** pp, char* out, int max)
{
    char* p = *pp;
    while (*p == ' ')
        ++p;
    if (!*p)
        return 0;
    int i = 0;
    while (*p && *p != ' ' && *p != '\n' && *p != '\r') {
        if (i < max - 1)
            out[i++] = *p;
        ++p;
    }
    out[i] = 0;
    *pp = p;
    return 1;
}

/* Re-establish the user identity recorded by a previous login. The
 * respawned shell comes up as root (the kernel spawns init as uid 0),
 * so dropping to the session's user/gid is permitted. */
static void adopt_session(void)
{
    int fd = open(SESSION_F, O_RDONLY);
    if (fd < 0)
        return;
    char buf[192];
    long got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got <= 0)
        return;
    buf[got] = 0;
    char name[32], tmp[24], home[PATH_MAX];
    char* p = buf;
    if (!next_word(&p, name, sizeof(name)))
        return;
    if (!next_word(&p, tmp, sizeof(tmp)))
        return;
    unsigned long uid = (unsigned long)atoi(tmp);
    if (!next_word(&p, tmp, sizeof(tmp)))
        return;
    unsigned long gid = (unsigned long)atoi(tmp);
    if (!next_word(&p, home, sizeof(home)))
        return;
    if (setgid(gid) < 0 || setuid(uid) < 0)
        return;
    strncpy(cur_user, name, sizeof(cur_user) - 1);
    cur_user[sizeof(cur_user) - 1] = 0;
    cur_uid = uid;
    cur_gid = gid;
    strncpy(home_dir, home, sizeof(home_dir) - 1);
    home_dir[sizeof(home_dir) - 1] = 0;
    chdir(home_dir);
}

/* The login prompt. Runs as root (fresh boot respawn) so setuid can
 * drop to the logged-in account. Returns 0 on success, -1 on EOF. */
static int do_login(void)
{
    char name[32], pass[96];
    for (;;) {
        w("VNU login: ");
        if (read_line(name, sizeof(name)) < 0)
            return -1;
        if (!name[0])
            continue;
        struct PwEntry e;
        if (!pw_lookup(name, &e)) {
            w("Login incorrect\n");
            continue;
        }
        w("Password: ");
        if (read_secret(pass, sizeof(pass)) < 0)
            return -1;
        char hash[17];
        pw_hash(pass, hash);
        if (strcmp(hash, e.hash) != 0) {
            w("Login incorrect\n");
            continue;
        }
        strncpy(cur_user, e.name, sizeof(cur_user) - 1);
        cur_user[sizeof(cur_user) - 1] = 0;
        cur_uid = e.uid;
        cur_gid = e.gid;
        strncpy(home_dir, e.home, sizeof(home_dir) - 1);
        home_dir[sizeof(home_dir) - 1] = 0;
        /* setgid first: once setuid drops root, setgid to a foreign
         * gid would be rejected. */
        setgid(e.gid);
        setuid(e.uid);
        chdir(e.home);
        save_session();
        return 0;
    }
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
           strcmp(cmd, "which") == 0 || strcmp(cmd, "install") == 0 ||
           strcmp(cmd, "id") == 0 || strcmp(cmd, "whoami") == 0 ||
           strcmp(cmd, "groups") == 0 || strcmp(cmd, "useradd") == 0 ||
           strcmp(cmd, "passwd") == 0 || strcmp(cmd, "su") == 0;
}

/* Format one /etc/passwd entry back into `line` (canonical order). */
static int format_pwline(const struct PwEntry* e, char* line, int cap)
{
    int n = 0;
    n = nappend(line, n, cap, e->name);
    line[n++] = ':';
    n = nappend(line, n, cap, e->hash);
    line[n++] = ':';
    n = uappend(line, n, cap, e->uid);
    line[n++] = ':';
    n = uappend(line, n, cap, e->gid);
    line[n++] = ':';
    n = nappend(line, n, cap, e->name);
    line[n++] = ':';
    n = nappend(line, n, cap, e->home);
    line[n++] = ':';
    n = nappend(line, n, cap, "/bin/vash");
    line[n++] = '\n';
    line[n] = 0;
    return n;
}

static int cmd_useradd(int ac, char** av)
{
    if (getuid() != 0) {
        we("useradd: only root may add users\n");
        return 1;
    }
    if (ac != 2) {
        we("usage: useradd <name>\n");
        return 1;
    }
    const char* name = av[1];
    if (!name[0] || strlen(name) > 24) {
        we("useradd: bad name\n");
        return 1;
    }
    for (const char* p = name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-')) {
            we("useradd: name must be [a-z0-9_-]\n");
            return 1;
        }
    struct PwEntry all[MAX_USERS];
    int n = read_passwd(all);
    for (int i = 0; i < n; ++i)
        if (strcmp(all[i].name, name) == 0) {
            we("useradd: user already exists\n");
            return 1;
        }

    char p1[96], p2[96];
    w("New password: ");
    if (read_secret(p1, sizeof(p1)) < 0)
        return 1;
    w("Retype password: ");
    if (read_secret(p2, sizeof(p2)) < 0)
        return 1;
    if (strcmp(p1, p2) != 0) {
        we("useradd: passwords do not match\n");
        return 1;
    }

    unsigned long next_uid = 1000;
    for (int i = 0; i < n; ++i)
        if (all[i].uid >= next_uid)
            next_uid = all[i].uid + 1;

    struct PwEntry e;
    memset(&e, 0, sizeof(e));
    strncpy(e.name, name, sizeof(e.name) - 1);
    pw_hash(p1, e.hash);
    e.uid = next_uid;
    e.gid = 100; /* users */
    join_path(e.home, sizeof(e.home), "/home", name);

    int fd = open(PASSWD_F, O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0) {
        we("useradd: cannot write /etc/passwd\n");
        return 1;
    }
    char line[256];
    int li = format_pwline(&e, line, sizeof(line));
    long rc = write(fd, line, (unsigned long)li);
    close(fd);
    if (rc <= 0) {
        we("useradd: write failed\n");
        return 1;
    }

    /* Home directory: create, then hand it to the new owner with a
     * private 0700, exactly like a real useradd. */
    if (mkdir(e.home, 0755) < 0) {
        we("useradd: warning: home dir exists\n");
    }
    chown(e.home, e.uid, e.gid);
    chmod(e.home, 0700);

    w("user ");
    w(name);
    w(" added (uid ");
    wnumu(next_uid);
    w(")\n");
    return 0;
}

static int cmd_passwd(int ac, char** av)
{
    if (getuid() != 0) {
        we("passwd: only root can change passwords\n");
        return 1;
    }
    const char* target = ac > 1 ? av[1] : cur_user;
    struct PwEntry all[MAX_USERS];
    int n = read_passwd(all);
    int idx = -1;
    for (int i = 0; i < n; ++i)
        if (strcmp(all[i].name, target) == 0) {
            idx = i;
            break;
        }
    if (idx < 0) {
        we("passwd: no such user\n");
        return 1;
    }

    char p1[96], p2[96];
    w("New password: ");
    if (read_secret(p1, sizeof(p1)) < 0)
        return 1;
    w("Retype password: ");
    if (read_secret(p2, sizeof(p2)) < 0)
        return 1;
    if (strcmp(p1, p2) != 0) {
        we("passwd: passwords do not match\n");
        return 1;
    }
    pw_hash(p1, all[idx].hash);

    int fd = open(PASSWD_F, O_WRONLY | O_TRUNC);
    if (fd < 0) {
        we("passwd: cannot write /etc/passwd\n");
        return 1;
    }
    for (int i = 0; i < n; ++i) {
        char line[256];
        int li = format_pwline(&all[i], line, sizeof(line));
        write(fd, line, (unsigned long)li);
    }
    close(fd);
    w("passwd: password updated for ");
    w(target);
    w("\n");
    return 0;
}

static int cmd_su(int ac, char** av)
{
    const char* target = ac > 1 ? av[1] : "root";
    struct PwEntry e;
    if (!pw_lookup(target, &e)) {
        we("su: unknown user\n");
        return 1;
    }
    w("Password: ");
    char pass[96];
    if (read_secret(pass, sizeof(pass)) < 0)
        return 1;
    char hash[17];
    pw_hash(pass, hash);
    if (strcmp(hash, e.hash) != 0) {
        we("su: authentication failure\n");
        return 1;
    }
    if (setgid(e.gid) < 0 || setuid(e.uid) < 0) {
        we("su: cannot change identity (need root)\n");
        return 1;
    }
    strncpy(cur_user, e.name, sizeof(cur_user) - 1);
    cur_user[sizeof(cur_user) - 1] = 0;
    cur_uid = e.uid;
    cur_gid = e.gid;
    strncpy(home_dir, e.home, sizeof(home_dir) - 1);
    home_dir[sizeof(home_dir) - 1] = 0;
    chdir(e.home);
    save_session();
    w("su: switched to ");
    w(cur_user);
    w("\n");
    return 0;
}

static int run_builtin(int ac, char** av)
{
    const char* cmd = av[0];
    if (strcmp(cmd, "exit") == 0) {
        /* exiting the login shell is a logout: drop the session so the
         * next respawn shows the login prompt again. */
        drop_session();
        exit(ac > 1 ? atoi(av[1]) : 0);
    }
    if (strcmp(cmd, "id") == 0) {
        w("uid=");
        wnumu(cur_uid);
        w("(");
        w(cur_user);
        w(") gid=");
        wnumu(cur_gid);
        w("(");
        const char* gn = group_name(cur_gid);
        if (gn)
            w(gn);
        else
            wnumu(cur_gid);
        w(")\n");
        return 0;
    }
    if (strcmp(cmd, "whoami") == 0) {
        w(cur_user);
        w("\n");
        return 0;
    }
    if (strcmp(cmd, "groups") == 0) {
        const char* gn = group_name(cur_gid);
        if (gn)
            w(gn);
        else
            wnumu(cur_gid);
        w("\n");
        return 0;
    }
    if (strcmp(cmd, "useradd") == 0)
        return cmd_useradd(ac, av);
    if (strcmp(cmd, "passwd") == 0)
        return cmd_passwd(ac, av);
    if (strcmp(cmd, "su") == 0)
        return cmd_su(ac, av);
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
        w("               id whoami groups useradd passwd su\n");
        w("PATH=");
        w(path_var);
        w("\nExternal: /bin/* via PATH (ls echo cat ...)\n");
        w("Documentation: 'man <command>' (e.g. man ls, man useradd)\n");
        w("Up/Down history, Ctrl+C cancel, Shift for symbols\n");
        return 0;
    }
    if (strcmp(cmd, "cd") == 0) {
        const char* d = ac > 1 ? av[1] : home_dir;
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
    /* First time in a boot: login. Later respawns (after an external
     * command execve()d over us) re-adopt the /tmp/.session identity. */
    if (access_session_exists())
        adopt_session();
    else if (do_login() < 0)
        return 0;
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
        w(cur_user);
        w("@vnu:");
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)))
            w(strcmp(cwd, home_dir) == 0 ? "~" : cwd);
        w("$ ");
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
