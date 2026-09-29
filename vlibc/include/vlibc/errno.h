#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/* The error numbers the system calls answer with.
 *
 * vlibc has no global `errno`: a failing call returns its error number
 * negated (`-VNU_EINVAL`, 22, for a bad argument), and that is what a
 * caller compares against. The numbers are the ones the kernel uses in
 * <vnu/abi.h>, which are the ordinary POSIX/Linux ones, so a program
 * that knows a name can use the name. */

#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4   /* a signal arrived; the call did no work */
#define EIO 5
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define ENOSPC 28
#define EROFS 30
#define EPIPE 32
#define ERANGE 34
#define ENOSYS 38
#define ENOTEMPTY 39
#define ETIMEDOUT 110
#define ECONNREFUSED 111

#ifdef __cplusplus
}
#endif
