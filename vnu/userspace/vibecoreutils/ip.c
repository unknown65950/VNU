/* ip — show the network interface and its addressing.
 *
 *   ip [OBJECT]
 *
 * OBJECT is addr (default), link or route, spelled out or abbreviated
 * the way iproute2 does (a, l, r). The kernel holds one interface, the
 * e1000 the QEMU machine comes with, known here as eth0; its address,
 * netmask and default gateway are the fixed values of QEMU's slirp user
 * network, so this command reads configuration, it does not set it.
 *
 * The numbers come from the netinfo syscall (MAC, addresses, link
 * status); the interface counters live in /proc/net/dev.
 */
#include "cu.h"
#include <vnu/abi.h>
#include <vlibc/sys/syscall.h>

static void print_ip(unsigned long ip)
{
    put_uint(ip >> 24);
    w(".");
    put_uint((ip >> 16) & 0xFF);
    w(".");
    put_uint((ip >> 8) & 0xFF);
    w(".");
    put_uint(ip & 0xFF);
}

static void print_mac(const unsigned char* mac)
{
    static const char dig[] = "0123456789abcdef";
    char b[4];
    int i;
    for (i = 0; i < 6; ++i) {
        if (i)
            w(":");
        b[0] = dig[(mac[i] >> 4) & 0xF];
        b[1] = dig[mac[i] & 0xF];
        b[2] = 0;
        w(b);
    }
}

/* Counts the set bits of a netmask (255.255.255.0 -> 24). Displays what
 * the mask says; it is contiguous on this interface, as masks are. */
static int prefix_len(unsigned long mask)
{
    int n = 0;
    int i;
    for (i = 0; i < 32; ++i) {
        if (mask & (1ul << i))
            ++n;
    }
    return n;
}

static void show_addr(const struct vnu_netinfo* in)
{
    unsigned long ip = in->ip;
    unsigned long mask = in->mask;
    unsigned long brd = ip | (~mask & 0xFFFFFFFFul);
    w("1: eth0: <");
    if (in->up)
        w("BROADCAST,MULTICAST,UP,LOWER_UP");
    else
        w("BROADCAST,MULTICAST");
    w(">\n");
    w("    link/ether ");
    print_mac(in->mac);
    w("\n");
    if (in->up) {
        w("    inet ");
        print_ip(ip);
        w("/");
        put_uint((unsigned long)prefix_len(mask));
        w(" brd ");
        print_ip(brd);
        w(" scope global eth0\n");
    }
}

static void show_link(const struct vnu_netinfo* in)
{
    w("1: eth0: <BROADCAST,MULTICAST");
    if (in->up)
        w(",UP,LOWER_UP");
    w(">\n");
    w("    link/ether ");
    print_mac(in->mac);
    w("\n");
    if (!in->up)
        w("    state DOWN\n");
}

static void show_route(const struct vnu_netinfo* in)
{
    unsigned long net = in->ip & in->mask;
    if (!in->up)
        return;
    w("default via ");
    print_ip(in->gw);
    w(" dev eth0\n");
    print_ip(net);
    w("/");
    put_uint((unsigned long)prefix_len(in->mask));
    w(" dev eth0 scope link src ");
    print_ip(in->ip);
    w("\n");
}

static void usage(void)
{
    we("usage: ip [OBJECT]\n");
    we("OBJECT := { addr | link | route }\n");
}

int main(int argc, char** argv)
{
    struct vnu_netinfo info;
    const char* obj = "addr";
    long rc;

    if (argc > 2) {
        usage();
        return 1;
    }
    if (argc == 2) {
        const char* a = argv[1];
        if (a[0] == '-' && a[1] == 'h' && a[2] == 0) {
            usage();
            return 0;
        }
        obj = a;
    }

    rc = syscall(VNU_SYS_netinfo, (unsigned long)&info);
    if (rc < 0) {
        we("ip: no network interface\n");
        return 1;
    }

    if ((obj[0] == 'a' && obj[1] == 0) || !strcmp(obj, "addr") ||
        !strcmp(obj, "address"))
        show_addr(&info);
    else if ((obj[0] == 'l' && obj[1] == 0) || !strcmp(obj, "link"))
        show_link(&info);
    else if ((obj[0] == 'r' && obj[1] == 0) || !strcmp(obj, "route"))
        show_route(&info);
    else {
        we("ip: unknown object \"");
        we(obj);
        we("\"\n");
        usage();
        return 1;
    }
    return 0;
}
