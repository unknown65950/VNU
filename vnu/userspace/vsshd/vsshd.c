/* vsshd - the VNU SSH daemon.
 *
 * Binds port 22 and wires the three protocol layers together: the
 * SSH-2 transport (transport.c), password userauth against the
 * machine's own /etc/passwd (auth.c), and one session channel running
 * /bin/vash (conn.c).
 *
 * Started and stopped by `vnu set outsideaccess on|off`, which flips
 * /etc/vnu/config/vsshd.conf and hands this daemon its pid file at
 * /tmp/vsshd.pid. The daemon watches that switch file itself (SIGTERM
 * is only the fast path: see the accept loop) and stops within a second
 * of `off`. Each accepted connection forks: the parent keeps listening,
 * the child runs the session for that client and exits when the channel
 * closes.
 */

#include <vlibc/string.h>
#include <vlibc/fcntl.h>
#include <vlibc/unistd.h>
#include <vlibc/signal.h>
#include <vlibc/stdlib.h>
#include <vlibc/socket.h>
#include <vlibc/sys/stat.h>
#include <vlibc/sys/wait.h>
#include <vnu/abi.h>
#include "ssh.h"

#define VSSHD_PORT 22
#define PID_F "/tmp/vsshd.pid"
#define CONF_F "/etc/vnu/config/vsshd.conf"

/* the listener, file-scope so the SIGTERM handler can close it: VNU
 * sockets live in the kernel's global table and are NOT closed when a
 * process exits, so a daemon that just unlinked its pid file would
 * leave port 22 listening forever with nobody to accept into it */
static int g_ls = -1;

static void we(const char* s)
{
    if (s)
        write(2, s, strlen(s));
}

/* Reap finished connection processes. */
static void on_sigchld(int sig)
{
    (void)sig;
    int st;
    while (waitpid(-1, &st, WNOHANG) > 0)
        ;
}

/* Stop the daemon for good: drop the pid file, close the listener and
 * exit. Shared by the SIGTERM handler and the config poll in the accept
 * loop (see there). */
static void shutdown_now(void)
{
    unlink(PID_F);
    if (g_ls >= 0)
        vnu_netclose(g_ls); /* see the note next to g_ls */
    exit(0);
}

static void on_term(int sig)
{
    (void)sig;
    shutdown_now();
}

/* The switch file is the source of truth: `vnu set outsideaccess on|off`
 * rewrites /etc/vnu/config/vsshd.conf, the daemon refuses to start
 * unless that file reads "outsideaccess on", and the accept loop re-reads
 * it so the daemon stops on its own once it flips to off. */
static int enabled(void)
{
    int fd = open(CONF_F, O_RDONLY);
    char cfg[64];
    long n = fd >= 0 ? read(fd, cfg, sizeof(cfg) - 1) : -1;
    if (fd >= 0)
        close(fd);
    if (n <= 0)
        return 0;
    cfg[n] = 0;
    return memcmp(cfg, "outsideaccess on", 16) == 0;
}

static void write_pid(void)
{
    char b[16];
    int n = 0;
    unsigned long pid = (unsigned long)getpid();
    char t[12];
    int i = 0;
    if (pid == 0)
        t[i++] = '0';
    while (pid) {
        t[i++] = (char)('0' + pid % 10);
        pid /= 10;
    }
    while (i)
        if (n < (int)sizeof(b) - 1)
            b[n++] = t[--i];
    b[n++] = '\n';
    int fd = open(PID_F, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        write(fd, b, (unsigned long)n);
        close(fd);
    }
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    if (!enabled()) {
        we("vsshd: not enabled (vnu set outsideaccess on)\n");
        return 1;
    }

    struct sigaction sa;
    sa.sa_handler = on_sigchld;
    sa.sa_mask = 0;
    sa.sa_flags = 0;
    sa.sa_restorer = 0;
    sigaction(SIGCHLD, &sa, 0);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, 0);

    int ls = vnu_socket(VNU_AF_INET, VNU_SOCK_STREAM);
    if (ls < 0) {
        we("vsshd: socket failed\n");
        return 1;
    }
    g_ls = ls;
    if (vnu_bind(ls, VSSHD_PORT) < 0) {
        we("vsshd: bind failed (port 22 in use?)\n");
        vnu_netclose(ls);
        return 1;
    }
    if (vnu_listen(ls, 4) < 0) {
        we("vsshd: listen failed\n");
        vnu_netclose(ls);
        return 1;
    }
    write_pid();

    for (;;) {
        /* Signals do not reliably reach a process parked inside accept()
         * (the kernel only runs handlers at syscall boundaries), so on
         * top of the SIGTERM fast path the daemon watches its own switch
         * file: this check runs on every pass, and the one-second accept
         * timeout bounds how long a shut-down stays unnoticed. */
        if (!enabled())
            shutdown_now();
        uint32_t rip = 0;
        uint16_t rport = 0;
        int c = vnu_accept(ls, &rip, &rport, 1000);
        if (c < 0)
            continue; /* nothing arrived; accept again */

        /* A session that finished while we were parked in accept() is a
         * zombie nobody has collected yet - SIGCHLD does not interrupt
         * the wait - and MAX_PROCS is small enough that a handful of
         * corpses makes the next fork fail. Reap before claiming a
         * slot: a connection that cannot spawn its session is a
         * connection dead on arrival. */
        {
            int st;
            while (waitpid(-1, &st, WNOHANG) > 0)
                ;
        }

        int pid = fork();
        if (pid < 0) {
            vnu_netclose(c);
            continue;
        }
        if (pid == 0) {
            /* the session process owns the connection for good. VNU socket
             * handles are global and have no per-process refcount on fork,
             * so the child must NOT close the listener here (it is the
             * parent's to keep using) - it closes only the connection. */
            Ssh s;
            struct SshUser u;
            memset(&s, 0, sizeof(s));
            memset(&u, 0, sizeof(u));
            s.sock = c;
            s.recv_timeout = 5000;
            if (ssh_handshake(&s) == 0 && ssh_auth(&s, &u) == 0)
                ssh_connection(&s, &u);
            vnu_netclose(c);
            exit(0);
        }
        /* The parent must not close c: the handle is global, so closing it
         * here would free the socket the child is about to use. The child
         * closes it when the session ends. */
    }
}