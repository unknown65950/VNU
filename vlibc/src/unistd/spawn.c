#include <vlibc/unistd.h>
#include <vlibc/sys/syscall.h>

/* Launch `path` as a new process running in parallel with the caller
 * (cooperative scheduler) and return its pid, or a negative error.
 * Unlike fork+execve this does not clone the caller; it exists so the
 * init system can start services without replacing itself. */
int spawn(const char* path) {
    return (int)syscall(SYS_spawn, path);
}
