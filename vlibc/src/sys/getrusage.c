#include <vlibc/sys/resource.h>
#include <vlibc/sys/syscall.h>
int getrusage(int who, struct rusage* usage) {
    return (int)syscall(VNU_SYS_getrusage, (long)who, (long)usage);
}