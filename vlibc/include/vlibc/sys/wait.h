#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int waitpid(int pid, int* status, int options);
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
