#pragma once
#include <vlibc/sys/syscall.h>
#ifdef __cplusplus
extern "C" {
#endif

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

long write(int fd, const void* buffer, unsigned long count);
long read(int fd, void* buffer, unsigned long count);
long lseek(int fd, long offset, int whence);
int open(const char* path, int flags, ...);
int close(int fd);
int fork(void);
int execve(const char* path, char* const argv[], char* const envp[]);
void exit(int status);
unsigned long getpid(void);
unsigned long getuid(void);
unsigned long getgid(void);
/* Change effective/real user or group id. Root may set any value; a
 * non-root process may only keep its own ids (see kernel VFS checks). */
int setuid(unsigned long uid);
int setgid(unsigned long gid);
/* Change ownership of a file; pass (unsigned long)-1 to leave a field
 * untouched. Only root may change ownership. */
int chown(const char* path, unsigned long uid, unsigned long gid);
int chdir(const char* path);
char* getcwd(char* buffer, unsigned long size);
int pipe(int fds[2]);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int kill(int pid, int sig);
unsigned long getppid(void);
/* 1 if fd refers to a terminal (stdio, /dev/tty, /dev/console), else 0. */
int isatty(int fd);
/* Sleep for usec/seconds. Blocks on the PIT clock (see SYS_sleep);
 * returns 0 on success (after the full interval). Milliseconds are
 * exposed by usleep() so sub-second intervals stay accurate. */
int usleep(unsigned long usec);
unsigned int sleep(unsigned int seconds);
/* Start `path` as a new process in parallel with the caller (cooperative
 * scheduler) and return its pid, or a negative error. Does not replace
 * the caller. */
int spawn(const char* path);

#ifdef __cplusplus
}
#endif
