/* initialD - VNU's userspace init, PID 1.
 *
 * The kernel starts /sbin/init as the first scheduler-managed process.
 * initialD runs the boot scripts in /etc/initialD, lexical order,
 * then keeps a login shell on the console, respawning it whenever it
 * exits. It also reaps whatever the kernel reparents to it, so a daemon
 * that outlives its parent never leaks a process slot.
 *
 * It never execs over itself and it never exits: PID 1 stays PID 1,
 * which is what makes orphan adoption sound (see reparent_orphans() in
 * kernel/proc/process.cpp). A boot script that fails does not fail the
 * boot - the failure is logged and the next script runs.
 */
#include <vlibc/unistd.h>
#include <vlibc/string.h>
#include <vlibc/stdlib.h>
#include <vlibc/dirent.h>
#include <vlibc/signal.h>
#include <vlibc/sys/wait.h>
#include <vlibc/errno.h>

#define INIT_DIR "/etc/initialD"
#define CONSOLE_SHELL "/bin/vash"

/* Lexical boot order over this many scripts; names past the caps are
 * skipped, never truncated into something else. */
#define MAX_SCRIPTS 32
#define NAME_CAP 64
#define PATH_CAP 128

static void w(const char* s)
{
    write(1, s, strlen(s));
}

static void w2(const char* a, const char* b)
{
    w(a);
    w(b);
}

/* Decimal, no snprintf in a PID 1 that wants no heap before it must. */
static void put_int(int v)
{
    char buf[12];
    int n = 0;
    unsigned u;
    if (v < 0) {
        w("-");
        u = (unsigned)(-(v + 1)) + 1u;
    } else {
        u = (unsigned)v;
    }
    if (u == 0) {
        w("0");
        return;
    }
    while (u > 0 && n < 11) {
        buf[n++] = (char)('0' + (u % 10));
        u /= 10;
    }
    while (n > 0)
        write(1, &buf[--n], 1);
}

/* Fork and exec `path` with `argv`; the child never returns. */
static int spawn_child(const char* path, char* const argv[])
{
    static char* const envp[] = {"PATH=/bin", 0};
    int pid = fork();
    if (pid < 0) {
        w2("initialD: cannot fork for ", path);
        w("\n");
        return -1;
    }
    if (pid == 0) {
        execve(path, argv, envp);
        w2("initialD: cannot exec ", path);
        w("\n");
        exit(127);
    }
    return pid;
}

/* Wait for one child (pid) or any (-1); EINTR restarts, anything else
 * comes back to the caller. vlibc has no errno variable: a failing
 * call answers its negated error number. */
static int wait_child(int pid, int* status)
{
    for (;;) {
        int r = waitpid(pid, status, 0);
        if (r == -EINTR)
            continue;
        return r;
    }
}

static int ends_init(const char* name)
{
    int n = strlen(name);
    return n > 5 && strcmp(name + n - 5, ".init") == 0;
}

/* The boot scripts, lexical order. readdir() hands them back in node
 * order, which is build order and not the contract, so they are
 * insertion-sorted here. Dotfiles and over-long names are skipped. */
static int collect_scripts(char names[MAX_SCRIPTS][NAME_CAP])
{
    int count = 0;
    DIR* d = opendir(INIT_DIR);
    if (!d) {
        w2("initialD: cannot list ", INIT_DIR);
        w(", skipping boot scripts\n");
        return 0;
    }
    struct dirent* e;
    while ((e = readdir(d)) != 0) {
        if (e->d_name[0] == '.' || !ends_init(e->d_name))
            continue;
        if (strlen(e->d_name) >= NAME_CAP || count >= MAX_SCRIPTS)
            continue;
        int i = count;
        while (i > 0 && strcmp(names[i - 1], e->d_name) > 0) {
            strcpy(names[i], names[i - 1]);
            --i;
        }
        strcpy(names[i], e->d_name);
        ++count;
    }
    closedir(d);
    return count;
}

/* Run a boot script through the shell's script-file mode (vash path).
 * The script is NOT exec'd directly: that would go through the kernel's
 * #! parse, which mishandles the argumentless interpreter line these
 * scripts carry (see the shebang-noarg case), and hand vash no script
 * at all - an interactive login where a boot step should be. The #!
 * line stays as documentation of the runner. */
static int run_script(const char* path)
{
    char* argv[3];
    argv[0] = CONSOLE_SHELL;
    argv[1] = (char*)path;
    argv[2] = 0;
    w2("initialD: running ", path);
    w("\n");
    int pid = spawn_child(CONSOLE_SHELL, argv);
    if (pid < 0)
        return -1;
    int status = 0;
    int r = wait_child(pid, &status);
    if (r < 0) {
        w2("initialD: lost ", path);
        w(" while waiting\n");
        return -1;
    }
    if (status != 0) {
        w2(path, " exited ");
        put_int(status);
        w("\n");
        return 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    /* PID 1 does not die by keyboard: ^C at the console belongs to
     * whatever is reading it, and a TERM to init is a shutdown order
     * nobody here carries out (restarting is the desktop system menu's
     * Reboot item). */
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    chdir("/");

    w("initialD: VNU userspace init\n");

    char names[MAX_SCRIPTS][NAME_CAP];
    int n = collect_scripts(names);
    for (int i = 0; i < n; ++i) {
        char path[PATH_CAP];
        int k = 0;
        const char* p = INIT_DIR "/";
        while (*p && k < PATH_CAP - 1)
            path[k++] = *p++;
        p = names[i];
        while (*p && k < PATH_CAP - 1)
            path[k++] = *p++;
        path[k] = 0;
        run_script(path);
    }

    /* The console session, forever. The shell does its own login the
     * first time (no session file yet) and re-adopts the session after
     * that; `exit' drops the session, so the respawn is a login again.
     * waitpid(-1) also reaps adopted orphans between shell runs. */
    for (;;) {
        char* shargv[2];
        shargv[0] = CONSOLE_SHELL;
        shargv[1] = 0;
        int shell = spawn_child(CONSOLE_SHELL, shargv);
        if (shell < 0) {
            sleep(5);
            continue;
        }
        int status = 0;
        for (;;) {
            int r = wait_child(-1, &status);
            if (r == -ECHILD) {
                sleep(1);
                break;
            }
            if (r < 0)
                continue;
            if (r == shell)
                break;
            w("initialD: reaped orphan pid ");
            put_int(r);
            w(" status ");
            put_int(status);
            w("\n");
        }
        /* Turnover is routine - the shell execs over itself for every
         * external command, so each one ends here - and stays silent
         * the way the kernel respawn this loop replaces was. Only a
         * shell that cannot start (missing binary: 127) is reported,
         * and waits, or it would respawn in a hot loop. */
        if (status == 127) {
            w("initialD: console shell failed to start, retrying\n");
            sleep(5);
        }
    }
}
