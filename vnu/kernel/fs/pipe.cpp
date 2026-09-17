#include <vnu/pipe.h>
#include <vnu/abi.h>

namespace {

constexpr int MAX_PIPES = 8;
constexpr int MAX_FD = 64;
constexpr int BUF = 256;

struct Pipe {
    bool used;
    char data[BUF];
    uint32_t r, w, len;
    int readers, writers;
};

struct Fd {
    bool used;
    bool is_pipe;
    int pipe_id;
    bool write_end;
};

Pipe pipes[MAX_PIPES];
Fd table[MAX_FD];

int alloc_fd()
{
    /* fd 3 is the windowed-task gfx surface; keep it out of the real
     * wrapper. */
    for (int i = 4; i < MAX_FD; ++i)
        if (!table[i].used)
            return i;
    return -1;
}

} // namespace

namespace vnu::pipe {

void init()
{
    for (int i = 0; i < MAX_PIPES; ++i)
        pipes[i] = {};
    for (int i = 0; i < MAX_FD; ++i)
        table[i] = {};
}

int create(int fds[2])
{
    int pi = -1;
    for (int i = 0; i < MAX_PIPES; ++i)
        if (!pipes[i].used) {
            pi = i;
            break;
        }
    if (pi < 0)
        return -VNU_ENOSPC;
    int rfd = alloc_fd();
    int wfd = alloc_fd();
    if (rfd < 0 || wfd < 0)
        return -VNU_ENOSPC;
    pipes[pi] = {};
    pipes[pi].used = true;
    pipes[pi].readers = 1;
    pipes[pi].writers = 1;
    table[rfd] = {true, true, pi, false};
    table[wfd] = {true, true, pi, true};
    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

bool is_pipe_fd(int fd)
{
    return fd >= 0 && fd < MAX_FD && table[fd].used && table[fd].is_pipe;
}

int read_fd(int fd, void* buf, uint32_t n)
{
    if (!is_pipe_fd(fd) || table[fd].write_end)
        return -VNU_EBADF;
    Pipe& p = pipes[table[fd].pipe_id];
    auto* out = static_cast<char*>(buf);
    uint32_t got = 0;
    while (got < n && p.len > 0) {
        out[got++] = p.data[p.r];
        p.r = (p.r + 1) % BUF;
        --p.len;
    }
    if (got == 0 && p.writers == 0)
        return 0; /* EOF */
    if (got == 0)
        return -VNU_EAGAIN; /* would block — no scheduler sleep yet */
    return static_cast<int>(got);
}

int write_fd(int fd, const void* buf, uint32_t n)
{
    if (!is_pipe_fd(fd) || !table[fd].write_end)
        return -VNU_EBADF;
    Pipe& p = pipes[table[fd].pipe_id];
    if (p.readers == 0)
        return -VNU_EPIPE;
    const auto* in = static_cast<const char*>(buf);
    uint32_t put = 0;
    while (put < n && p.len < BUF) {
        p.data[p.w] = in[put++];
        p.w = (p.w + 1) % BUF;
        ++p.len;
    }
    return static_cast<int>(put);
}

int close_fd(int fd)
{
    if (!is_pipe_fd(fd))
        return -VNU_EBADF;
    Pipe& p = pipes[table[fd].pipe_id];
    if (table[fd].write_end)
        --p.writers;
    else
        --p.readers;
    table[fd] = {};
    if (p.readers <= 0 && p.writers <= 0)
        p.used = false;
    return 0;
}

int dup_fd(int oldfd)
{
    if (!is_pipe_fd(oldfd))
        return -VNU_EBADF;
    int n = alloc_fd();
    if (n < 0)
        return -VNU_ENOSPC;
    table[n] = table[oldfd];
    Pipe& p = pipes[table[n].pipe_id];
    if (table[n].write_end)
        ++p.writers;
    else
        ++p.readers;
    return n;
}

int dup2_fd(int oldfd, int newfd)
{
    if (!is_pipe_fd(oldfd))
        return -VNU_EBADF;
    if (newfd < 3 || newfd >= MAX_FD)
        return -VNU_EBADF;
    if (oldfd == newfd)
        return newfd;
    if (table[newfd].used && table[newfd].is_pipe)
        close_fd(newfd);
    table[newfd] = table[oldfd];
    Pipe& p = pipes[table[newfd].pipe_id];
    if (table[newfd].write_end)
        ++p.writers;
    else
        ++p.readers;
    return newfd;
}

} // namespace vnu::pipe
