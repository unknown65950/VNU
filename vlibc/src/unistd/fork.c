#include <vlibc/unistd.h>

pid_t fork(void) {
    return (pid_t)syscall(SYS_fork);
}
