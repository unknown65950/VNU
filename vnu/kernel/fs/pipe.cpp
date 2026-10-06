#include <vnu/pipe.h>
#include <vnu/abi.h>
#include <vnu/process.h>   /* current_pid(): whose table a descriptor is in */

namespace {

constexpr int MAX_PIPES = 8;
constexpr int MAX_FD = 64;
constexpr int MAX_TABLES = 16; /* one per process, and then some */
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

/* One fd table per process, not one for the machine.
 *
 * The table used to be global, and a global table cannot mean what an fd
 * number has to mean: "the descriptor *this* process has at 0". Every
 * step of wiring a pipe rests on the other end's descriptors being left
 * alone by this end's — a shell closes its copy of an end right after it
 * forks the process that needs it, and with a shared table that close
 * took the other process's end down with it, so the following dup2()
 * found no pipe on the number and fell back to the terminal. That is
 * how a pipeline ended up with its first stage writing into a pipe and
 * its second stage reading from the console, and how a process
 * substitution lost its output before it could move it onto stdout.
 *
 * The reader/writer counts were wrong for the same reason: they counted
 * slots, and a slot belonged to whoever reached it first, so one
 * process's close could report the far end dead while another process
 * still held it.
 *
 * A table is stamped with the pid of the process it belongs to and is
 * created on first use, released by close_all_of() when that process
 * exits. fork_copy() gives the child a copy of its parent's table with
 * the counts raised, which is what makes a descriptor inherited across
 * fork() a reference of its own rather than a second name for the
 * parent's. */
struct Table {
    bool used;
    uint32_t pid;
    Fd e[MAX_FD];
};

Pipe pipes[MAX_PIPES];
Table tables[MAX_TABLES];

int alloc_in(const Table& t)
{
    /* Past stdio, like every other descriptor: a windowed app's canvas
     * is shared memory, not a pipe. */
    for (int i = 3; i < MAX_FD; ++i)
        if (!t.e[i].used)
            return i;
    return -1;
}

Table* find(uint32_t pid)
{
    for (int i = 0; i < MAX_TABLES; ++i)
        if (tables[i].used && tables[i].pid == pid)
            return &tables[i];
    return nullptr;
}

/* Find it, or make it: this is the first thing a process that has never
 * touched a pipe does with one. */
Table* table_for(uint32_t pid)
{
    if (Table* t = find(pid))
        return t;
    for (int i = 0; i < MAX_TABLES; ++i) {
        if (!tables[i].used) {
            tables[i] = {};
            tables[i].used = true;
            tables[i].pid = pid;
            return &tables[i];
        }
    }
    return nullptr;
}

Table* mine()
{
    return find(vnu::proc::current_pid());
}

/* Give every reference a table holds back to its pipe. */
void release(Table& t)
{
    for (int i = 0; i < MAX_FD; ++i) {
        if (!t.e[i].used)
            continue;
        Pipe& p = pipes[t.e[i].pipe_id];
        if (t.e[i].write_end)
            --p.writers;
        else
            --p.readers;
        t.e[i] = {};
        if (p.readers <= 0 && p.writers <= 0)
            p.used = false;
    }
}

} // namespace

namespace vnu::pipe {

void init()
{
    for (int i = 0; i < MAX_PIPES; ++i)
        pipes[i] = {};
    for (int i = 0; i < MAX_TABLES; ++i)
        tables[i] = {};
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
    Table* t = table_for(vnu::proc::current_pid());
    if (!t)
        return -VNU_ENOSPC;
    /* The read end is marked present before the write end is looked for,
     * or the allocator hands out the same number twice: one descriptor
     * with the write entry standing on top of the read one. Every pipe
     * would then be a single fd — the two numbers a caller keeps as
     * `keep' and `give' identical — so the moment the shell closed its
     * copy of one end it destroyed the other, and the subsequent dup2()
     * found no pipe to put on the standard descriptor at all. */
    int rfd = alloc_in(*t);
    if (rfd < 0)
        return -VNU_ENOSPC;
    t->e[rfd] = {true, true, pi, false};
    int wfd = alloc_in(*t);
    if (wfd < 0) {
        t->e[rfd] = {};
        return -VNU_ENOSPC;
    }
    t->e[wfd] = {true, true, pi, true};
    pipes[pi] = {};
    pipes[pi].used = true;
    pipes[pi].readers = 1;
    pipes[pi].writers = 1;
    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

bool is_pipe_fd(int fd)
{
    Table* t = mine();
    return t && fd >= 0 && fd < MAX_FD && t->e[fd].used && t->e[fd].is_pipe;
}

int read_fd(int fd, void* buf, uint32_t n)
{
    Table* t = mine();
    if (!t || !is_pipe_fd(fd) || t->e[fd].write_end)
        return -VNU_EBADF;
    Pipe& p = pipes[t->e[fd].pipe_id];
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
    Table* t = mine();
    if (!t || !is_pipe_fd(fd) || !t->e[fd].write_end)
        return -VNU_EBADF;
    Pipe& p = pipes[t->e[fd].pipe_id];
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
    Table* t = mine();
    if (!t || !is_pipe_fd(fd))
        return -VNU_EBADF;
    Pipe& p = pipes[t->e[fd].pipe_id];
    if (t->e[fd].write_end)
        --p.writers;
    else
        --p.readers;
    t->e[fd] = {};
    if (p.readers <= 0 && p.writers <= 0)
        p.used = false;
    return 0;
}

int dup_fd(int oldfd)
{
    Table* t = mine();
    if (!t || !is_pipe_fd(oldfd))
        return -VNU_EBADF;
    int n = alloc_in(*t);
    if (n < 0)
        return -VNU_ENOSPC;
    t->e[n] = t->e[oldfd];
    Pipe& p = pipes[t->e[n].pipe_id];
    if (t->e[n].write_end)
        ++p.writers;
    else
        ++p.readers;
    return n;
}

int dup2_fd(int oldfd, int newfd)
{
    Table* t = mine();
    if (!t || !is_pipe_fd(oldfd))
        return -VNU_EBADF;
    /* newfd 0, 1 and 2 are the whole point of dup2(): putting a pipe on a
     * standard descriptor is how a shell wires a pipeline and a process
     * substitution into a command's stdio. The allocator starts past
     * stdio so that a pipe is never *handed out* as 0/1/2; that rule
     * used to be carried over here as well, and it answered every one of
     * those calls with -EBADF, leaving the command on the terminal while
     * the pipe dangled with nobody at either end. */
    if (newfd < 0 || newfd >= MAX_FD)
        return -VNU_EBADF;
    if (oldfd == newfd)
        return newfd;
    if (t->e[newfd].used && t->e[newfd].is_pipe)
        close_fd(newfd); /* POSIX: the target is closed first */
    t->e[newfd] = t->e[oldfd];
    Pipe& p = pipes[t->e[newfd].pipe_id];
    if (t->e[newfd].write_end)
        ++p.writers;
    else
        ++p.readers;
    return newfd;
}

void close_all_of(uint32_t pid)
{
    /* Unlike vfs::close_all_of(), this one takes 0/1/2 as well: a pipe on
     * a standard descriptor is not the console, and once the pipe half is
     * cleared the slot falls back to whatever the VFS half says, which
     * for stdio is the console. What has to happen now is that every
     * reader and writer count the dying process was holding drops — a
     * count left standing is a pipe that can never report end-of-file to
     * whoever is waiting on the other end of it. */
    Table* t = find(pid);
    if (!t)
        return;
    release(*t);
    *t = {};
}

void fork_copy(uint32_t child_pid)
{
    Table* src = mine();
    if (!src)          /* no pipes to inherit */
        return;
    Table* dst = table_for(child_pid);
    if (!dst)          /* out of tables: the child is born without any */
        return;
    if (dst->used)     /* a pid handed out twice: the old holder is gone */
        release(*dst);
    for (int i = 0; i < MAX_FD; ++i)
        dst->e[i] = src->e[i];
    for (int i = 0; i < MAX_FD; ++i) {
        if (!dst->e[i].used)
            continue;
        Pipe& p = pipes[dst->e[i].pipe_id];
        if (dst->e[i].write_end)
            ++p.writers;
        else
            ++p.readers;
    }
}

} // namespace vnu::pipe
