#include <vlibc/netdb.h>
#include <vlibc/sys/syscall.h>
#include <vnu/abi.h>

int gethostbyname(const char* name, uint32_t* ip_be)
{
    return (int)syscall(VNU_SYS_resolve, (long)name, (long)ip_be);
}