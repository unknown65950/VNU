#include <vlibc/sys/wait.h>
#include <vlibc/sys/syscall.h>
int waitpid(int pid, int* status, int options) {
    return (int)syscall(SYS_waitpid, pid, status, options);
}
int wait(int* status) { return waitpid(-1, status, 0); }
