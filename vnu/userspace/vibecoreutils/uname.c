/* uname — print system information (separate binary). */
#include "cu.h"

static void uname_help(void)
{
    w("Usage: uname [OPTION]...\n");
    w("Print certain system information. With no OPTION, same as -s.\n\n");
    w("  -a, --all                 print all information\n");
    w("  -s, --kernel-name         print the kernel name\n");
    w("  -n, --nodename            print the network node hostname\n");
    w("  -r, --kernel-release      print the kernel release\n");
    w("  -v, --kernel-version      print the kernel version\n");
    w("  -m, --machine             print the machine hardware name\n");
    w("  -p, --processor           print the processor type\n");
    w("  -i, --hardware-platform   print the hardware platform\n");
    w("  -o, --operating-system    print the operating system\n");
    w("      --help                display this help and exit\n");
    w("      --version             output version information and exit\n");
}

static void uname_field(const char* s, int* first)
{
    if (!*first)
        w(" ");
    w(s);
    *first = 0;
}

int main(int argc, char** argv)
{
    struct utsname u;
    if (uname(&u) < 0)
        return 1;
    if (argc == 1) {
        w(u.sysname);
        w("\n");
        return 0;
    }

    int all = 0, first = 1;
    int show_s = 0, show_n = 0, show_r = 0, show_v = 0, show_m = 0;
    int show_p = 0, show_i = 0, show_o = 0;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "--help") == 0) {
            uname_help();
            return 0;
        }
        if (strcmp(a, "--version") == 0) {
            w("uname (VNU coreutils) 0.3\n");
            return 0;
        }
        if (strcmp(a, "--all") == 0) { all = 1; continue; }
        if (strcmp(a, "--kernel-name") == 0) { show_s = 1; continue; }
        if (strcmp(a, "--nodename") == 0) { show_n = 1; continue; }
        if (strcmp(a, "--kernel-release") == 0) { show_r = 1; continue; }
        if (strcmp(a, "--kernel-version") == 0) { show_v = 1; continue; }
        if (strcmp(a, "--machine") == 0) { show_m = 1; continue; }
        if (strcmp(a, "--processor") == 0) { show_p = 1; continue; }
        if (strcmp(a, "--hardware-platform") == 0) { show_i = 1; continue; }
        if (strcmp(a, "--operating-system") == 0) { show_o = 1; continue; }
        if (a[0] == '-' && a[1] != '-') {
            for (int j = 1; a[j]; ++j) {
                switch (a[j]) {
                case 'a': all = 1; break;
                case 's': show_s = 1; break;
                case 'n': show_n = 1; break;
                case 'r': show_r = 1; break;
                case 'v': show_v = 1; break;
                case 'm': show_m = 1; break;
                case 'p': show_p = 1; break;
                case 'i': show_i = 1; break;
                case 'o': show_o = 1; break;
                default:
                    we("uname: invalid option\n");
                    return 1;
                }
            }
        } else {
            we("uname: unexpected argument\n");
            return 1;
        }
    }
    if (all)
        show_s = show_n = show_r = show_v = show_m = show_p = show_i = show_o = 1;
    if (!show_s && !show_n && !show_r && !show_v && !show_m && !show_p && !show_i && !show_o)
        show_s = 1;
    if (show_s) uname_field(u.sysname, &first);
    if (show_n) uname_field(u.nodename, &first);
    if (show_r) uname_field(u.release, &first);
    if (show_v) uname_field(u.version, &first);
    if (show_m) uname_field(u.machine, &first);
    if (show_p) uname_field(u.processor, &first);
    if (show_i) uname_field(u.hardware_platform, &first);
    if (show_o) uname_field(u.operating_system, &first);
    w("\n");
    return 0;
}