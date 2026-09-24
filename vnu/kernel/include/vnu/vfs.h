#pragma once
#include <vnu/posix.h>
#include <stdint.h>
#include <stddef.h>

namespace vnu::vfs {

/* Character devices under /dev. Most are handled entirely inside the
 * VFS (read/write below); Tty is the exception — it needs the same
 * console/keyboard routing the syscall layer already does for fd
 * 0/1/2 (including the windowed-task redirection added for
 * VibeGraphics), so the syscall layer queries fd_dev_kind() and
 * handles that case itself rather than the VFS duplicating it. */
enum class DevKind : uint8_t { None = 0, Null, Zero, Full, Random, Tty, Audio };

struct File {
    bool used;
    uint32_t position;
    uint32_t flags;
    int node; /* index into node table, -1 = stdio */
};

/* Directory entry for getdents */
struct Dirent {
    uint32_t ino;
    uint16_t reclen;
    uint8_t type; /* 4=dir DT_DIR, 8=reg DT_REG, 2=DT_CHR */
    char name[56];
} __attribute__((packed));

void init();
int open(const char* path, uint32_t flags);
int close(int fd);
int read(int fd, void* buf, uint32_t count);
int write(int fd, const void* buf, uint32_t count);
int lseek(int fd, int32_t offset, int whence);
int stat(const char* path, vnu::posix::Stat* st);
int fstat(int fd, vnu::posix::Stat* st);
int touch(const char* path);
int mkdir(const char* path);
int rmdir(const char* path);
int unlink(const char* path);
int remove(const char* path); /* file or empty dir */
int read_path(const char* path, char* buf, uint32_t count);
void list(char* buf, uint32_t count);
int getdents(int fd, void* buf, uint32_t count);
int chdir(const char* path);
int getcwd(char* buf, uint32_t size);
/* Multiuser: change permission bits (owner or root only) and owner /
 * group id (root only). `uid`/`gid` of -1 leave that field unchanged. */
int chmod(const char* path, uint32_t mode);
int chown(const char* path, uint32_t uid, uint32_t gid);

/* DevKind::None for anything that isn't an open fd on a /dev
 * character device. */
DevKind fd_dev_kind(int fd);

/* True once fd 0/1/2 has been pointed at a real file (by the shell's
 * dup2-based redirection). The syscall layer checks this before
 * sending stdio traffic to the console: without it, write(1) would
 * always reach the screen and `> file` could never work. */
bool fd_is_redirected(int fd);

/* True if `fd` is currently open as a real VFS file (not the default
 * stdio meaning). The syscall gfx-surface intercept must not steal a
 * descriptor that a windowed task has genuinely opened: vash's history
 * file lands on fd 3, and blind interception would flip that task's
 * window into gfx mode and paint garbage. */
bool fd_is_open(int fd);

/* Put fd 0/1/2 back to their default console meaning. Called when a
 * fresh shell session starts, since the VFS fd table is global to the
 * kernel — otherwise one command's redirection would silently persist
 * into every later one. */
void reset_stdio();

/* Duplicate an open VFS fd into the lowest free slot (dup) or into a
 * specific slot, closing whatever was there (dup2). The copy shares
 * the original's node but gets its own file position, which is enough
 * for the shell redirection cases these exist for. */
int dup_fd(int fd);
int dup2_fd(int oldfd, int newfd);

/* Whole seconds since boot (RTC delta). This kernel has no timer
 * interrupt, so the RTC is the only clock available; resolution is one
 * second. Backs the cooperative sleep() syscall. */
uint32_t uptime_seconds();

/* The RTC wall-clock time as whole seconds since local midnight
 * (0..86399). Exposed to userspace as the `time` syscall; the GUI
 * taskbar analog clock and its per-second heartbeat tick read it
 * directly. */
uint32_t time_seconds();

} // namespace vnu::vfs
