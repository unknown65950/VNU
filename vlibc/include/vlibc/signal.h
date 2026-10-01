#pragma once
#include <vlibc/sys/syscall.h>
#include <vlibc/errno.h>
#ifdef __cplusplus
extern "C" {
#endif

/* --- signals ----------------------------------------------------------
 *
 * The POSIX numbers, for the signals VNU has an answer for. A program
 * written against a real system means the same thing here; the ones
 * POSIX also has and VNU does not (SIGSTOP, SIGCONT, the job-control
 * trio) are deliberately left undefined rather than defined and
 * ignored, so a program that uses them does not compile by accident. */
#define SIGHUP  1  /* the console went away */
#define SIGINT  2  /* ^C at the console */
#define SIGQUIT 3
#define SIGILL  4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGFPE  8
#define SIGKILL 9  /* cannot be caught, blocked or ignored */
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13 /* reserved: nothing raises it yet, a write with
                  * no reader fails with EPIPE */
#define SIGALRM 14 /* alarm() ran out */
#define SIGTERM 15
#define SIGCHLD 17 /* reserved: a child that changes state is not a
                  * signal yet, waitpid() is how you hear about it */
#define SIGWINCH 28 /* the window's pixel canvas changed size: a gfx
                   * app catches this, asks vgfx_canvas() again and
                   * redraws at the size it was given */

#define NSIG 32 /* the signals are 1..NSIG-1; 0 is never a signal */

/* One word of signals, bit n being signal n, so sigemptyset() is 0 and
 * SIGINT is 1 << SIGINT. */
typedef unsigned int sigset_t;

typedef void (*__sighandler_t)(int);

#define SIG_DFL ((__sighandler_t)0)  /* do the default thing: terminate */
#define SIG_IGN ((__sighandler_t)1)  /* do nothing at all */
#define SIG_ERR ((__sighandler_t)-1) /* sigaction() could not do it */

/* What a handler does about the signal it is running, besides running:
 * SA_RESETHAND puts the default action back after one delivery, and
 * SA_NODEFER leaves the signal itself unblocked while its handler runs
 * (by default the signal that is being handled is blocked, so a ^C
 * inside a ^C handler waits for the handler to be done).
 *
 * The restorer is not one of these flags and not one of your fields to
 * fill: sigaction() below sets sa_restorer for you, because the kernel
 * has to be told where the `return` at the end of your handler lands.
 * Leave the field out of initialisers (or zero it) and it is filled. */
#define SA_RESETHAND 0x80000000
#define SA_NODEFER 0x40000000

/* The same four words as `struct vnu_sigaction` in <vnu/abi.h>, which is
 * what crosses the syscall boundary. */
struct sigaction {
    __sighandler_t sa_handler;
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

/* Install (or, with `act` NULL, just report) what happens to `signo`.
 * `act`'s mask is blocked in addition to the process's own mask for as
 * long as the handler runs. SIGKILL is refused. Returns 0, or -errno. */
int sigaction(int signo, const struct sigaction* act, struct sigaction* old);

/* sigsignal() from the older interface: the handler is installed with no
 * extra mask and resets to the default after one delivery. `handler` is
 * a plain function pointer, not a cast to int. */
__sighandler_t signal(int signo, __sighandler_t handler);

/* SIG_BLOCK adds `set` to the signals blocked outside a handler,
 * SIG_UNBLOCK takes them out, SIG_SETMASK makes the blocked set exactly
 * `set`. SIGKILL is never in the result. `old` (optional) receives the
 * set that was in force. The mask survives execve, so a program that
 * starts a command with signals blocked can rely on that. */
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
int sigprocmask(int how, const sigset_t* set, sigset_t* old);

/* The signals raised at this process and not yet delivered. */
int sigpending(sigset_t* set);

/* Raise `signo` in the calling process. */
int raise(int signo);

/* Have SIGALRM raised in `seconds` (0 cancels the alarm) and return the
 * seconds left on the previous one, 0 if there was none. The alarm is
 * measured on the same clock as usleep() and is cancelled by execve(). */
unsigned int alarm(unsigned int seconds);

#ifdef __cplusplus
}
#endif
