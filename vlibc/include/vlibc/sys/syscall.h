#pragma once
#include <vnu/abi.h>

#define SYS_read VNU_SYS_read
#define SYS_write VNU_SYS_write
#define SYS_open VNU_SYS_open
#define SYS_close VNU_SYS_close
#define SYS_exit VNU_SYS_exit
#define SYS_lseek VNU_SYS_lseek
#define SYS_stat VNU_SYS_stat
#define SYS_fstat VNU_SYS_fstat
#define SYS_brk VNU_SYS_brk
#define SYS_getpid VNU_SYS_getpid
#define SYS_chdir VNU_SYS_chdir
#define SYS_getcwd VNU_SYS_getcwd
#define SYS_fork VNU_SYS_fork
#define SYS_execve VNU_SYS_execve
#define SYS_getuid VNU_SYS_getuid
#define SYS_getgid VNU_SYS_getgid
#define SYS_mkdir VNU_SYS_mkdir
#define SYS_rmdir VNU_SYS_rmdir
#define SYS_unlink VNU_SYS_unlink
#define SYS_getdents VNU_SYS_getdents
#define SYS_waitpid VNU_SYS_waitpid
#define SYS_pipe VNU_SYS_pipe
#define SYS_dup VNU_SYS_dup
#define SYS_dup2 VNU_SYS_dup2
#define SYS_kill VNU_SYS_kill
#define SYS_uname VNU_SYS_uname
#define SYS_getppid VNU_SYS_getppid
#define SYS_isatty VNU_SYS_isatty
#define SYS_spawn VNU_SYS_spawn
#define SYS_reboot VNU_SYS_reboot
#define SYS_time VNU_SYS_time
#define SYS_uptime VNU_SYS_uptime

#ifdef __cplusplus
extern "C" {
#endif
long syscall(long number, ...);
#ifdef __cplusplus
}
#endif
