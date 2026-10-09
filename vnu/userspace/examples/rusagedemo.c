/* rusagedemo - what times(2) and getrusage(2) actually report, run in
 * the guest as the regression test for the two syscalls (VNU_SYS_times
 * 73, VNU_SYS_getrusage 74).
 *
 * A child is forked, burns a few clock ticks of CPU in a busy loop and
 * exits; the parent waits for it and prints its own figures plus the
 * child's, which the kernel folded into the parent at the child's
 * exit. The fork is the point of the demo: POSIX children start with
 * zero CPU time, and that time re-appears in the parent's tms_cutime
 * and in getrusage(RUSAGE_CHILDREN) once the child is reaped.
 *
 * VNU executes everything at privilege 0 (one address space) and does
 * not sample a privilege level, so user and system time are not
 * separated the way Linux separates them: every tick a process runs is
 * charged to utime, and stime/cstime are always 0. The demo prints
 * those zeros rather than hiding them (see <vnu/abi.h>).
 */
#include <vlibc/stdio.h>
#include <vlibc/sys/resource.h>
#include <vlibc/sys/times.h>
#include <vlibc/sys/wait.h>
#include <vlibc/unistd.h>

int main(void)
{
    int pid = fork();
    if (pid < 0) {
        printf("rusagedemo: fork() failed\n");
        return 1;
    }

    if (pid == 0) {
        /* Child: a fresh fork has zero CPU time, so spin until three
         * ticks (30 ms of the 100 Hz scheduler clock) have been
         * charged to this process, then report and go away. The tick
         * keeps landing here because the parent is blocked in waitpid
         * and there is nobody else to run. */
        struct tms t;
        times(&t);
        while (t.tms_utime < 3)
            times(&t);
        printf("rusagedemo: child burn: utime=%d ticks (exit status 7)\n",
               (int)t.tms_utime);
        exit(7);
    }

    int status = -1;
    int got = waitpid(pid, &status, 0);
    if (got != pid || status != 7) {
        printf("rusagedemo: waitpid -> pid=%d status=%d\n", got, status);
        return 1;
    }

    struct tms me;
    times(&me);
    printf("rusagedemo: times after waitpid: utime=%d stime=%d cutime=%d cstime=%d\n",
           (int)me.tms_utime, (int)me.tms_stime,
           (int)me.tms_cutime, (int)me.tms_cstime);

    struct rusage self;
    if (getrusage(RUSAGE_SELF, &self) == 0) {
        printf("rusagedemo: getrusage self: utime=%ds %dus stime=%ds %dus nvcsw=%d nivcsw=%d\n",
               (int)self.ru_utime.tv_sec, (int)self.ru_utime.tv_usec,
               (int)self.ru_stime.tv_sec, (int)self.ru_stime.tv_usec,
               (int)self.ru_nvcsw, (int)self.ru_nivcsw);
    }

    struct rusage kids;
    if (getrusage(RUSAGE_CHILDREN, &kids) == 0) {
        printf("rusagedemo: getrusage children: utime=%ds %dus stime=%ds %dus\n",
               (int)kids.ru_utime.tv_sec, (int)kids.ru_utime.tv_usec,
               (int)kids.ru_stime.tv_sec, (int)kids.ru_stime.tv_usec);
    }
    return 0;
}