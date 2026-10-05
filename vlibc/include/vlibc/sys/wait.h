#pragma once
#include <vlibc/sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The options argument of waitpid(). Only the one the kernel acts on is
 * defined: sys_waitpid() reads it as "return 0 instead of blocking when
 * no zombie of mine is ready" (process.cpp), so a nonzero value means
 * exactly WNOHANG and nothing else. WUNTRACED and WCONTINUED are left
 * undefined rather than defined-and-ignored — a program that passed
 * them would get non-blocking behaviour it had not asked for, and a
 * name in a header is a promise about what the call does. */
#define WNOHANG 1

int waitpid(pid_t pid, int* status, int options);
int wait(int* status);

/* The bits of a status word, spelled the way POSIX spells them: what a
 * program exited with, or which signal stopped or killed it. */
#define WIFEXITED(s)   (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) ((((s) & 0x7f) != 0) && (((s) & 0x7f) != 0x7f))
#define WTERMSIG(s)    ((s) & 0x7f)
#define WIFSTOPPED(s)  (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)    ((s) >> 8)
#define WIFCONTINUED(s) ((s) == 0xffff)
#ifdef __cplusplus
}
#endif
