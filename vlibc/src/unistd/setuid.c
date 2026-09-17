#include <vlibc/sys/syscall.h>

int setuid(unsigned long uid) {
    return (int)syscall(VNU_SYS_setuid, uid);
}