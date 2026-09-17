#include <vlibc/sys/syscall.h>

int setgid(unsigned long gid) {
    return (int)syscall(VNU_SYS_setgid, gid);
}