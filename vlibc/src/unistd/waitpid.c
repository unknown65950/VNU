#include <vlibc/sys/wait.h>
#include <vlibc/sys/syscall.h>
pid_t waitpid(pid_t pid, int* status, int options) {
    return (pid_t)syscall(SYS_waitpid, pid, status, options);
}
int wait(int* status) { return waitpid(-1, status, 0); }
