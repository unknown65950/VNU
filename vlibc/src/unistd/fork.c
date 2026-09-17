#include <vlibc/unistd.h>

int fork(void) {
    return (int)syscall(SYS_fork);
}
