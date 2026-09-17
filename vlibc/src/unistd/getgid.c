#include <vlibc/unistd.h>

unsigned long getgid(void) {
    return (unsigned long)syscall(SYS_getgid);
}
