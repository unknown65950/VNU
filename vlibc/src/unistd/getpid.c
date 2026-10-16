#include <vlibc/unistd.h>

pid_t getpid(void) {
    return (pid_t)syscall(SYS_getpid);
}
