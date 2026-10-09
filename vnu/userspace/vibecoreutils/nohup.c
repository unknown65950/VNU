/* nohup — run a command immune to SIGHUP (separate binary).
 *
 * Usage: nohup COMMAND [ARG...]
 *
 * With the console behind it, standard output is a terminal, so the
 * command's output would be lost the moment the session goes away -
 * exactly the case nohup exists for. This follows GNU nohup:
 *
 *   - standard input, when it is a terminal, is redirected from
 *     /dev/null so the command never blocks on a keyboard that is no
 *     longer there;
 *   - standard output, when it is a terminal, is appended to nohup.out
 *     in the current directory (created with mode 0600 if needed);
 *   - standard error, when it is a terminal, is sent wherever standard
 *     output now goes;
 *   - SIGHUP is set to be ignored. POSIX keeps SIG_IGN across execve,
 *     so the command itself starts with SIGHUP already ignored.
 *
 * The command then replaces nohup with execve(), so the caller's shell
 * sees the command's own exit status. Two GNU details are left out
 * because VNU has no use for them: the $HOME/nohup.out fallback when the
 * current directory cannot be written, and the PATH search - a command
 * with no slash in it is looked up in /bin, the only place programs
 * live.
 */
#include "cu.h"
#include <vlibc/signal.h>

static char* append(char* p, const char* s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

/* Does the path contain a directory part? */
static int has_slash(const char* p)
{
    for (; *p; ++p)
        if (*p == '/')
            return 1;
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        we("usage: nohup COMMAND [ARG...]\n");
        return 125;
    }
    if (argc > 64) {
        we("nohup: too many arguments\n");
        return 125;
    }

    int in_tty = isatty(0);
    int out_tty = isatty(1);
    int err_tty = isatty(2);

    /* Build the one diagnostic coreutils prints, from the parts that
     * actually apply ("ignoring input and appending output to ..."). */
    char note[128];
    char* np = note;
    int have = 0;
    if (in_tty) {
        np = append(np, "ignoring input");
        have = 1;
    }
    if (out_tty) {
        if (have)
            np = append(np, " and ");
        np = append(np, "appending output to 'nohup.out'");
        have = 1;
    } else if (isatty(2)) {
        if (have)
            np = append(np, " and ");
        np = append(np, "redirecting stderr to stdout");
        have = 1;
    }
    *np = 0;

    /* Print it to the original standard error, before any redirection:
     * once stderr follows the output file the person behind the terminal
     * would never see it. */
    if (have) {
        we("nohup: ");
        we(note);
        we("\n");
    }

    if (isatty(1)) {
        int fd = open("nohup.out", O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd < 0) {
            we("nohup: cannot create 'nohup.out'\n");
            return 125;
        }
        dup2(fd, 1);
        if (fd != 1)
            close(fd);
    }
    if (isatty(2))
        dup2(1, 2);
    if (isatty(0)) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) {
            dup2(fd, 0);
            if (fd != 0)
                close(fd);
        }
    }

    signal(SIGHUP, SIG_IGN);

    /* execve() receives the command and its arguments unchanged; argv[0]
     * is the name as typed. */
    char* av[65];
    for (int i = 1; i < argc; ++i)
        av[i - 1] = argv[i];
    av[argc - 1] = 0;

    char path[128];
    char* end;
    unsigned long cmdlen = strlen(av[0]);
    if (cmdlen + 6 > sizeof(path)) {
        we("nohup: command name too long\n");
        return 127;
    }
    if (av[0][0] && av[0][0] != '/' && !has_slash(av[0])) {
        end = append(path, "/bin/");
        end = append(end, av[0]);
    } else {
        end = append(path, av[0]);
    }
    *end = 0;

    execve(path, av, 0);

    /* execve() returned, so the command was not found or not runnable.
     * By now standard error may point at nohup.out, which is where GNU
     * nohup leaves this message too. */
    we("nohup: cannot run '");
    we(argv[1]);
    we("'\n");
    return 127;
}
