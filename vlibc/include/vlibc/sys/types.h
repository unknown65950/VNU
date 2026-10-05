#pragma once
/* sys/types.h — the POSIX integer typedefs, in the header POSIX puts
 * them in.
 *
 * A program writing portable process code declares `pid_t`, and nothing
 * in vlibc defined it, so fork() and waitpid() could not be spelled the
 * way POSIX spells them: the prototype said `int pid` and a `pid_t`
 * variable in the caller was a type that did not exist. It is an int
 * because vlibc's fork() and waitpid() return what syscall() returned,
 * cast to int (vlibc/src/unistd/fork.c, waitpid.c) — so the typedef
 * names the ABI the kernel already has rather than inventing one.
 *
 * The rest of POSIX's list is absent on purpose. Nothing in vlibc takes
 * or returns an off_t, a mode_t, an ssize_t or a dev_t, so declaring
 * them would be a promise with no implementation behind it. When a
 * syscall needs one, it comes in with the syscall. */
#ifdef __cplusplus
extern "C" {
#endif

typedef int pid_t;

#ifdef __cplusplus
}
#endif
