#include <vlibc/sys/stat.h>
#include <vlibc/sys/syscall.h>
int mkdir(const char* path, int mode) {
    (void)mode;
    return (int)syscall(SYS_mkdir, path);
}
int rmdir(const char* path) { return (int)syscall(SYS_rmdir, path); }
int unlink(const char* path) { return (int)syscall(SYS_unlink, path); }
