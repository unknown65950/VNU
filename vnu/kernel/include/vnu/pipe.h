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

} // namespace vnu::pipe
