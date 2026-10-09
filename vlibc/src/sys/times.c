#include <vlibc/sys/syscall.h>
#include <vlibc/sys/times.h>
clock_t times(struct tms* buf) {
    return (clock_t)syscall(VNU_SYS_times, (long)buf);
}