#pragma once
#include <stdint.h>

namespace vnu::pipe {

void init();
/* Create pipe, write two fds into fds[0]=read, fds[1]=write. Returns 0 or -errno. */
int create(int fds[2]);
/* Called from vfs-like fd ops when fd is a pipe end. */
bool is_pipe_fd(int fd);
int read_fd(int fd, void* buf, uint32_t n);
int write_fd(int fd, const void* buf, uint32_t n);
int close_fd(int fd);
int dup_fd(int oldfd);
int dup2_fd(int oldfd, int newfd);
/* Release every descriptor this pid still holds: a descriptor table that
 * counts readers and writers cannot afford to have them counted for a
 * process that no longer exists, or the other end waits for an EOF that
 * will never come. The counterpart of vfs::close_all_of(). */
void close_all_of(uint32_t pid);
/* Hand the child a copy of the calling process's descriptors, with the
 * reader/writer counts raised to match: an inherited pipe end has to be
 * the child's own reference, or the parent's close() of it would take
 * the child's away - one table shared by two processes cannot tell one
 * holder's descriptor from another's. Called from fork(). */
void fork_copy(uint32_t child_pid);

} // namespace vnu::pipe
