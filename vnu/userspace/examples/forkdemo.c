/* forkdemo - what fork(2) gives a child, and what it does not.
 *
 * The child is a second process in its own address space: it runs while
 * the parent runs (not after it), it has its own pid, and it resumes at
 * the instruction after fork() with a return value of 0. What it shares
 * with the parent is the *outside* of memory - the console, the
 * filesystem - and what it does not share is memory: `marker` below is
 * one variable in one address space that the child writes to, and the
 * parent never sees the write.
 *
 * Every line is a claim the kernel has to make true, so this doubles as
 * the regression test for proc::sys_fork:
 *   - both processes run, interleaved, and the parent's line comes out
 *     while the child is still alive (the old fork could not: its child
 *     only ran when the parent exited);
 *   - getpid()/getppid() agree on who is who;
 *   - the child's write to a global is invisible in the parent;
 *   - waitpid() reaps the child and reports its exit status.
 */
#include <vlibc/stdio.h>
#include <vlibc/string.h>
#include <vlibc/sys/wait.h>
#include <vlibc/unistd.h>

/* .data, so it lives in a private page after the fork. */
static int marker = 1;

int main(void)
{
    printf("forkdemo: parent pid=%d\n", (int)getpid());

    int pid = fork();
    if (pid < 0) {
        printf("forkdemo: fork() failed\n");
        return 1;
    }

    if (pid == 0) {
        /* Child. Sleep first so the parent is provably still running. */
        usleep(300000);
        marker = 2;
        printf("forkdemo: child pid=%d getpid=%d getppid=%d\n", pid,
               (int)getpid(), (int)getppid());
        printf("forkdemo: child marker=%d (its own copy)\n", marker);
        exit(7);
    }

    printf("forkdemo: parent forked pid=%d, marker=%d\n", pid, marker);
    /* Still here, and running, while the child has not even woken up. */
    usleep(900000);
    printf("forkdemo: parent marker=%d, child alive? %s\n", marker,
           (kill(pid, 0) == 0) ? "yes" : "no (reaped or gone)");

    int status = -1;
    int got = waitpid(pid, &status, 0);
    printf("forkdemo: parent marker=%d after the child wrote 2\n", marker);
    printf("forkdemo: waitpid -> pid=%d status=%d\n", got, status);
    return got == pid && status == 7 ? 0 : 1;
}
