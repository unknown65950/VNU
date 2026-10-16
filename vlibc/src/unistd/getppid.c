#include <vlibc/unistd.h>

pid_t getppid(void) {
    return (pid_t)syscall(SYS_getppid);
}
