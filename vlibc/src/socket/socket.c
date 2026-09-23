#include <vlibc/socket.h>
#include <vlibc/sys/syscall.h>
#include <vnu/abi.h>

int vnu_socket(int family, int type)
{
    return (int)syscall(VNU_SYS_socket, family, type);
}

int vnu_connect(int sock, uint32_t ip_be, uint16_t port, uint32_t timeout_ms)
{
    return (int)syscall(VNU_SYS_connect, sock, (long)ip_be, (long)port, (long)timeout_ms);
}

int vnu_send(int sock, const void* buf, uint32_t len, uint32_t timeout_ms)
{
    return (int)syscall(VNU_SYS_send, sock, (long)buf, (long)len, (long)timeout_ms);
}

int vnu_recv(int sock, void* buf, uint32_t len, uint32_t timeout_ms)
{
    return (int)syscall(VNU_SYS_recv, sock, (long)buf, (long)len, (long)timeout_ms);
}

int vnu_netclose(int sock)
{
    return (int)syscall(VNU_SYS_netclose, sock);
}