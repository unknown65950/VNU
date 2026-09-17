#include <vlibc/sys/syscall.h>

int chown(const char* path, unsigned long uid, unsigned long gid) {
    return (int)syscall(VNU_SYS_chown, path, uid, gid);
}