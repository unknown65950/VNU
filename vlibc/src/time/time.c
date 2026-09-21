#include <vlibc/time.h>

time_t time(time_t* out)
{
    time_t t = (time_t)syscall(SYS_time);
    if (out)
        *out = t;
    return t;
}