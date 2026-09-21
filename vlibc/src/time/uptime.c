#include <vlibc/time.h>

unsigned long uptime(void)
{
    return (unsigned long)syscall(SYS_uptime);
}