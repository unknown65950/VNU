/* conn.c - RFC 4254 connection layer for vsshd. One connection carries
 * exactly one "session" channel running the machine's own /bin/vash
 * against the authenticated account (see auth.c).
 *
 * There is no pty anywhere in the picture: /bin/vash echoes and edits
 * the lines it reads itself (it is a line editor, not a disguising
 * terminal), so the SSH session is two byte pipes against one process.
 * The pipe into the shell carries typed input; the shell's stdout and
 * stderr go to a file, because without select()/poll() the daemon
 * cannot know whether the socket or the pipe has data next - the file
 * can be stat()ed instead, so the event loop polls the socket with a
 * short timeout and the file with stat()+read between packets.
 */

#include <vlibc/string.h>
#include <vlibc/fcntl.h>
#include <vlibc/unistd.h>
#include <vlibc/signal.h>
#include <vlibc/stdlib.h>
#include <vlibc/sys/stat.h>
#include <vlibc/sys/wait.h>
#include <vnu/abi.h>
#include "ssh.h"

/* the shell's stdout/stderr land here; one connection per process, so
 * the name needs no per-connection disambiguation */
#define OUT_PATH "/tmp/.ssh_out"

struct Sess {
    Ssh* ssh;
    uint32_t rcpt;       /* the client's channel number (recipient) */
    uint32_t ours;       /* our channel number; we always assign 0 */
    uint32_t win_out;    /* how much the client lets us send them */
    uint32_t win_in;     /* our advertised window (replenished in place) */
    int pin_wr;          /* write end of the shell's stdin pipe */
    int child;           /* pid of /bin/vash */
    int exit_status;     /* the shell's exit code, to report to the client */
    int shell_up;        /* a shell/exec request started the child */
    const struct SshUser* user;
    uint32_t drained;    /* bytes of OUT_PATH already sent to the client */
    int child_done;
    int eof_sent;
    int close_sent;
};

static uint32_t get_u32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_u32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int get_str(const uint8_t* p, uint32_t plen, uint32_t* off,
                   const char** out, uint32_t* olen)
{
    if (*off + 4 > plen)
        return -1;
    uint32_t l = get_u32(p + *off);
    *off += 4;
    if (*off + l > plen)
        return -1;
    *out = (const char*)p + *off;
    *olen = l;
    *off += l;
    return 0;
}

/* --- building strings without snprintf ------------------------------ */
static int ncat(char* b, int n, int cap, const char* s)
{
    while (s && *s && n < cap - 1)
        b[n++] = *s++;
    return n;
}

static int ucat(char* b, int n, int cap, unsigned long v)
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

/* /tmp/.session in the format vash's adopt_session() reads:
 * "name uid gid home\n" (see vash.c save_session()). */
static void write_session(const struct SshUser* u)
{
    char b[192];
    int n = 0;
    n = ncat(b, n, sizeof(b), u->name);
    b[n++] = ' ';
    n = ucat(b, n, sizeof(b), u->uid);
    b[n++] = ' ';
    n = ucat(b, n, sizeof(b), u->gid);
    b[n++] = ' ';
    n = ncat(b, n, sizeof(b), u->home);
    b[n++] = '\n';
    b[n] = 0;
    int fd = open("/tmp/.session", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        write(fd, b, (unsigned long)n);
        close(fd);
    }
}

/* Fork the authenticated user's shell. `cmd` (when non-empty) runs as
 * `vash -c cmd`; otherwise the interactive shell. Returns 0 on success,
 * leaving the child in se->child and the pipe write end in se->pin_wr. */
static int start_shell(struct Sess* se, const char* cmd)
{
    int pin[2];
    if (pipe(pin) < 0) {
        return -1;
    }
    int pid = fork();
    if (pid < 0) {
        close(pin[0]);
        close(pin[1]);
        return -1;
    }
    if (pid == 0) {
        /* the shell's view of the world: stdin from the pipe, stdout
         * and stderr to the drain file, the session identity on disk */
        close(pin[1]);
        dup2(pin[0], 0);
        close(pin[0]);
        int ofd = open(OUT_PATH, O_WRONLY | O_CREAT | O_TRUNC);
        if (ofd >= 0) {
            dup2(ofd, 1);
            dup2(ofd, 2);
            if (ofd > 2)
                close(ofd);
        }
        write_session(se->user);
        char* argv[4];
        if (cmd && cmd[0]) {
            argv[0] = "/bin/vash";
            argv[1] = "-c";
            argv[2] = (char*)cmd;
            argv[3] = 0;
        } else {
            argv[0] = "/bin/vash";
            argv[1] = 0;
        }
        execve("/bin/vash", argv, 0);
        exit(127); /* exec failed: the shell never got to run */
    }
    close(pin[0]);
    se->pin_wr = pin[1];
    se->child = pid;
    se->child_done = 0;
    se->shell_up = 1; /* gates output draining and the exit-status reporting */
    return 0;
}

static int send_data(Ssh* s, uint32_t rcpt, const uint8_t* data, uint32_t n)
{
    uint8_t pkt[9 + SSH_MAX_PACKET];
    if (n > SSH_MAX_PACKET)
        n = SSH_MAX_PACKET;
    pkt[0] = SSH_MSG_CHANNEL_DATA;
    put_u32(pkt + 1, rcpt);
    put_u32(pkt + 5, n);
    memcpy(pkt + 9, data, n);
    return ssh_send_raw(s, pkt, 9 + n);
}

/* Move whatever the shell has written since the last poll to the
 * client, respecting the client's advertised window. */
static void drain_output(struct Sess* se)
{
    struct stat st;
    if (!se->shell_up)
        return;
    if (stat(OUT_PATH, &st) < 0)
        return;
    if (st.st_size <= se->drained)
        return;
    uint32_t avail = st.st_size - se->drained;
    if (avail == 0 || se->win_out == 0)
        return;
    if (avail > SSH_MAX_PACKET)
        avail = SSH_MAX_PACKET;
    if (avail > se->win_out)
        avail = se->win_out;

    uint8_t buf[SSH_MAX_PACKET + 16];
    int fd = open(OUT_PATH, O_RDONLY);
    if (fd < 0)
        return;
    long got = read(fd, buf + 9, sizeof(buf) - 9);
    close(fd);
    if (got < 0)
        return;
    uint32_t start = se->drained;
    if ((uint32_t)got <= start)
        return;
    uint32_t take = (uint32_t)got - start;
    if (take > avail)
        take = avail;
    if (take == 0)
        return;
    if (send_data(se->ssh, se->rcpt, buf + 9 + start, take) < 0)
        return;
    se->drained = start + take;
    se->win_out -= take;
}

static int send_eof_decl(Ssh* s, uint32_t rcpt)
{
    uint8_t pkt[5];
    pkt[0] = SSH_MSG_CHANNEL_EOF;
    put_u32(pkt + 1, rcpt);
    return ssh_send_raw(s, pkt, 5);
}

static int send_close(Ssh* s, uint32_t rcpt)
{
    uint8_t pkt[5];
    pkt[0] = SSH_MSG_CHANNEL_CLOSE;
    put_u32(pkt + 1, rcpt);
    return ssh_send_raw(s, pkt, 5);
}

static int send_exit_status(Ssh* s, uint32_t rcpt, uint32_t status)
{
    uint8_t pkt[5 + 4 + 13 + 4];
    uint32_t n = 0;
    pkt[n++] = SSH_MSG_CHANNEL_REQUEST;
    put_u32(pkt + n, rcpt);
    n += 4;
    /* RFC 4254 6.1: request type string, then want reply, then the
     * type-specific data. */
    pkt[n++] = 0; pkt[n++] = 0; pkt[n++] = 0; pkt[n++] = 11;
    memcpy(pkt + n, "exit-status", 11);
    n += 11;
    pkt[n++] = 0; /* want_reply: false */
    put_u32(pkt + n, status);
    n += 4;
    return ssh_send_raw(s, pkt, n);
}

static void handle_open(struct Sess* se, const uint8_t* p, uint32_t plen)
{
    uint32_t off = 1;
    const char* ctype;
    uint32_t clen;
    if (get_str(p, plen, &off, &ctype, &clen) < 0)
        return;
    uint32_t rcpt = get_u32(p + off);
    off += 4;
    if (off + 8 > plen)
        return;
    /* initial window and max packet are the client's limits on us */
    se->win_out = get_u32(p + off);
    if (se->win_out == 0)
        se->win_out = SSH_WINDOW_OUT;

    if (se->shell_up || clen != 7 || memcmp(ctype, "session", 7) != 0) {
        /* answer any other channel type with a refusal */
        uint8_t full[13 + 19 + 4];
        full[0] = SSH_MSG_CHANNEL_OPEN_FAILURE;
        put_u32(full + 1, rcpt);
        put_u32(full + 5, 1); /* SSH_OPEN_ADMINISTRATIVELY_PROHIBITED */
        put_u32(full + 9, 19);
        memcpy(full + 13, "unsupported channel", 19);
        ssh_send_raw(se->ssh, full, 13 + 19 + 4);
        return;
    }

    se->rcpt = rcpt;
    se->ours = 0;
    se->win_in = SSH_WINDOW_IN;
    se->pin_wr = -1;
    se->child = -1;

    uint8_t reply[5 + 4 + 4 + 4 + 4];
    reply[0] = SSH_MSG_CHANNEL_OPEN_CONFIRMATION;
    put_u32(reply + 1, se->rcpt);
    put_u32(reply + 5, se->ours);
    put_u32(reply + 9, se->win_in);
    put_u32(reply + 13, SSH_MAX_PACKET);
    ssh_send_raw(se->ssh, reply, 17);
}

static void handle_request(struct Sess* se, const uint8_t* p, uint32_t plen)
{
    uint32_t off = 1;
    if (off + 4 > plen) {
        return;
    }
    uint32_t rcpt = get_u32(p + off);
    off += 4;
    if (rcpt != se->ours) {
        return;
    }
    /* RFC 4254 6.1: recipient, then the request type string, then the
     * want-reply boolean - the string comes before the flag. */
    const char* rtype;
    uint32_t rlen;
    if (get_str(p, plen, &off, &rtype, &rlen) < 0) {
        return;
    }
    if (off >= plen) {
        return;
    }
    int want = p[off++];

    int ok = 0;
    if (rlen == 5 && memcmp(rtype, "shell", 5) == 0) {
        if (!se->shell_up && start_shell(se, 0) == 0)
            ok = 1;
    } else if (rlen == 4 && memcmp(rtype, "exec", 4) == 0) {
        const char* cmd;
        uint32_t cmdlen;
        if (get_str(p, plen, &off, &cmd, &cmdlen) == 0 &&
            cmdlen < 4000 && !se->shell_up) {
            /* the command lives inside the packet buffer and is not
             * nul-terminated there: copy it so the shell's argv is a
             * clean C string */
            char cmdbuf[4096];
            uint32_t i;
            for (i = 0; i < cmdlen; ++i)
                cmdbuf[i] = cmd[i];
            cmdbuf[cmdlen] = 0;
            if (start_shell(se, cmdbuf) == 0)
                ok = 1;
        }
    } else if (rlen == 3 && memcmp(rtype, "env", 3) == 0) {
        /* stray env vars are harmless: the shell is freshly spawned
         * and exports only what the session asks for */
        ok = 1;
    } else if (rlen == 7 && memcmp(rtype, "pty-req", 7) == 0) {
        /* accepted, and ignored: /bin/vash edits its own lines, so no
         * terminal emulation is needed on this side */
        ok = 1;
    } else if (rlen == 6 && memcmp(rtype, "signal", 6) == 0) {
        ok = 1;
    } else if (rlen == 13 && memcmp(rtype, "window-change", 13) == 0) {
        ok = 1;
    } else if (rlen == 9 && memcmp(rtype, "subsystem", 9) == 0) {
        ok = 0; /* no subsystems on a machine this small */
    }

    if (want) {
        uint8_t b[5];
        b[0] = ok ? SSH_MSG_CHANNEL_SUCCESS : SSH_MSG_CHANNEL_FAILURE;
        put_u32(b + 1, rcpt);
        ssh_send_raw(se->ssh, b, 5);
    }
}

static void handle_data(struct Sess* se, const uint8_t* p, uint32_t plen)
{
    uint32_t off = 1;
    if (off + 4 > plen)
        return;
    uint32_t rcpt = get_u32(p + off);
    off += 4;
    if (rcpt != se->ours)
        return;
    const char* d;
    uint32_t dlen;
    if (get_str(p, plen, &off, &d, &dlen) < 0)
        return;
    if (dlen > se->win_in)
        dlen = se->win_in;

    if (se->pin_wr >= 0) {
        uint32_t done = 0;
        while (done < dlen) {
            long r = write(se->pin_wr, d + done, dlen - done);
            if (r <= 0)
                break;
            done += (uint32_t)r;
        }
    }

    /* replenish the window we advertised, so a long session never
     * stalls waiting for the client's response to a window update */
    se->win_in -= dlen;
    {
        uint8_t b[9];
        b[0] = SSH_MSG_CHANNEL_WINDOW_ADJUST;
        put_u32(b + 1, rcpt);
        put_u32(b + 5, dlen);
        ssh_send_raw(se->ssh, b, 9);
    }
    se->win_in += dlen;
}

static void handle_window_adjust(struct Sess* se, const uint8_t* p, uint32_t plen)
{
    uint32_t off = 1;
    if (off + 8 > plen)
        return;
    uint32_t rcpt = get_u32(p + off);
    off += 4;
    if (rcpt != se->ours)
        return;
    se->win_out += get_u32(p + off);
}

/* The RFC 4254 conversation for one authenticated connection. Runs
 * until the channel is closed or the connection dies. */
int ssh_connection(Ssh* s, const struct SshUser* user)
{
    struct Sess se;
    memset(&se, 0, sizeof(se));
    se.ssh = s;
    se.user = user;
    se.pin_wr = -1;
    se.child = -1;
    s->recv_timeout = 150; /* poll the shell's output file each try */

    for (;;) {
        uint8_t pkt[SSH_MAX_PACKET];
        uint32_t plen;
        int r = ssh_recv_packet(s, pkt, &plen, sizeof(pkt));
        if (r == SSH_RECV_TIMEOUT) {
            drain_output(&se);
        } else if (r < 0) {
            goto leave; /* EOF or a dead connection */
        } else if (plen < 1) {
            goto leave;
        } else {
            switch (pkt[0]) {
            case SSH_MSG_DISCONNECT:
                goto leave;
            case SSH_MSG_IGNORE:
            case SSH_MSG_DEBUG:
            case SSH_MSG_UNIMPLEMENTED:
                break;
            case SSH_MSG_CHANNEL_OPEN:
                handle_open(&se, pkt, plen);
                break;
            case SSH_MSG_CHANNEL_REQUEST:
                handle_request(&se, pkt, plen);
                break;
            case SSH_MSG_CHANNEL_DATA:
                handle_data(&se, pkt, plen);
                break;
            case SSH_MSG_CHANNEL_WINDOW_ADJUST:
                handle_window_adjust(&se, pkt, plen);
                break;
            case SSH_MSG_CHANNEL_EOF:
                if (se.pin_wr >= 0) {
                    close(se.pin_wr); /* the shell sees stdin end */
                    se.pin_wr = -1;
                }
                break;
            case SSH_MSG_CHANNEL_CLOSE:
                /* the client is done; take the session down with it */
                if (se.child > 0)
                    kill(se.child, SIGTERM);
                send_close(s, se.rcpt);
                goto leave;
            case SSH_MSG_GLOBAL_REQUEST: {
                /* reply to keepalives etc.; a failure is the honest
                 * answer. GLOBAL_REQUEST replies carry no recipient. */
                uint32_t o = 1;
                const char* rn;
                uint32_t rnl;
                if (get_str(pkt, plen, &o, &rn, &rnl) == 0 && o < plen) {
                    if (pkt[o]) { /* want_reply */
                        uint8_t b[1];
                        b[0] = SSH_MSG_REQUEST_FAILURE;
                        ssh_send_raw(s, b, 1);
                    }
                }
                break;
            }
            default:
                goto leave; /* an unknown message: a broken client */
            }
            if (se.shell_up)
                drain_output(&se);
        }

        /* the shell's fate, once per loop turn */
        if (se.child > 0) {
            int status = 0;
            int got = waitpid(se.child, &status, WNOHANG);
            if (got == se.child) {
                se.child = -1;
                se.child_done = 1;
                se.exit_status = status & 0xFF;
            }
        }
        if (se.child_done && !se.eof_sent) {
            drain_output(&se); /* the shell's last words */
            send_exit_status(s, se.rcpt, (uint32_t)se.exit_status);
            send_eof_decl(s, se.rcpt);
            se.eof_sent = 1;
        }
        if (se.child_done && se.eof_sent && !se.close_sent) {
            drain_output(&se); /* anything arriving between EOF and now */
            send_close(s, se.rcpt);
            se.close_sent = 1;
            goto leave;
        }
    }

leave:
    if (se.child > 0)
        kill(se.child, SIGTERM);
    if (se.pin_wr >= 0)
        close(se.pin_wr);
    return 0;
}