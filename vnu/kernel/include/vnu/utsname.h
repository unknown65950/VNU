#pragma once
struct vnu_utsname {
    char sysname[64];
    char nodename[64];
    char release[64];
    char version[64];
    char machine[64];
    /* Extra POSIX uname(1) fields (processor/hardware_platform not in
     * the base struct utsname(2), but uname(1) -p/-i/-o expose them
     * on most systems, so cmd_uname's -a matches real behavior). */
    char processor[64];
    char hardware_platform[64];
    char operating_system[64];
};
