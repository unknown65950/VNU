#include <vlibc/sys/utsname.h>
#include <vlibc/sys/syscall.h>
int uname(struct utsname* buf) {
    return (int)syscall(SYS_uname, buf);
}
