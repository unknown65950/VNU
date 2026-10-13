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
enum class DevKind : uint8_t { None = 0, Null, Zero, Full, Random, Tty, Audio, Framebuffer, Block, Clipboard };

struct File {
    bool used = false;
    uint32_t position = 0;
    uint32_t flags = 0;
    int node = -1; /* index into node table, -1 = stdio or an on-disk file */
    /* Which process opened it. The table is global to the kernel, so
     * without this nothing can tell a program's descriptors from anyone
     * else's and a process that stops running cannot take its own with
     * it: a /dev/fb0 descriptor left behind holds the display in a
     * graphics mode with the console's pass suppressed for good, and a
     * full table of them fails every later open with ENOSPC. See
     * close_all_of(). */
    uint32_t owner = 0; /* pid */
    /* A descriptor for a file on a mounted disk volume (mount > 0, an
     * index into the VFS mount table offset by one, so a zeroed File is
     * an ordinary in-memory file): the cluster of the directory holding
     * it, its name there, and its size. All three are ignored for every
     * other kind of descriptor, including the directory descriptors a
     * volume's directories are read through, where `dir_clu` alone
     * matters and `fname` is empty. */
    int mount = 0;
    uint32_t dir_clu = 0;
    uint32_t fsize = 0;
    char fname[64] = {};
};

/* Directory entry for getdents */
struct Dirent {
    uint32_t ino;
    uint16_t reclen;
    uint8_t type; /* 4=dir DT_DIR, 8=reg DT_REG, 2=DT_CHR */
    char name[56];
} __attribute__((packed));

void init();
/* Second half of the VFS bring-up, for after the ATA driver has probed
 * the disks: register the block devices under /dev and auto-mount any
 * data volume at /mnt. A no-op when no disk is attached. */
void init_disks();
int open(const char* path, uint32_t flags);
int close(int fd);
/* Close every descriptor belonging to `pid`, as that process is no
 * longer running and cannot do it: called when it exits or a signal
 * takes it. One of them may be the last descriptor on /dev/fb0, so this
 * goes through close() rather than clearing the table, and a redirection
 * left on fd 0/1/2 is put back to the console. */
void close_all_of(uint32_t pid);
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
int move_file(const char* from, const char* to); /* file, copy+unlink across dirs */
int read_path(const char* path, char* buf, uint32_t count);
/* Replace a file's whole content by path (kernel-side `> file`):
 * 0, or a negative errno. Used for /etc/hostname. */
int set_path(const char* path, const char* data, uint32_t len);
void list(char* buf, uint32_t count);
int getdents(int fd, void* buf, uint32_t count);
int chdir(const char* path);
int getcwd(char* buf, uint32_t size);
/* Multiuser: change permission bits (owner or root only) and owner /
 * group id (root only). `uid`/`gid` of -1 leave that field unchanged. */
int chmod(const char* path, uint32_t mode);
int chown(const char* path, uint32_t uid, uint32_t gid);

/* Per-user storage quota, backing VNU_SYS_quota and the `quota`
 * command. A user's usage is the sum of the sizes of the regular files
 * it owns in the VFS; `limit` is a hard ceiling on that sum, 0 meaning
 * unlimited. write() charges a growing write to the file's owner and
 * refuses the whole write with -VNU_EDQUOT when it would cross the
 * limit. `quota_set` returns 0 or a negative errno (the small per-user
 * table can be full); the caller enforces "root only". */
uint32_t quota_used(uint32_t uid);
uint32_t quota_limit(uint32_t uid);
int quota_set(uint32_t uid, uint32_t limit);
uint32_t quota_default();
void quota_set_default(uint32_t limit);

/* DevKind::None for anything that isn't an open fd on a /dev
 * character device. */
DevKind fd_dev_kind(int fd);

/* True once fd 0/1/2 has been pointed at a real file (by the shell's
 * dup2-based redirection). The syscall layer checks this before
 * sending stdio traffic to the console: without it, write(1) would
 * always reach the screen and `> file` could never work. */
bool fd_is_redirected(int fd);

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
