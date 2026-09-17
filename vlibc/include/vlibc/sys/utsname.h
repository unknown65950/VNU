#pragma once
#ifdef __cplusplus
extern "C" {
#endif
struct utsname {
    char sysname[64];
    char nodename[64];
    char release[64];
    char version[64];
    char machine[64];
    char processor[64];
    char hardware_platform[64];
    char operating_system[64];
};
int uname(struct utsname* buf);
#ifdef __cplusplus
}
#endif
