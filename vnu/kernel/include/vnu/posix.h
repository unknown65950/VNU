#pragma once
#include <stdint.h>
#include <stddef.h>
namespace vnu::posix {
using fd_t=int32_t; using off_t=int32_t;
constexpr fd_t FD_STDIN=0, FD_STDOUT=1, FD_STDERR=2;
enum OpenFlags:uint32_t { O_RDONLY=0, O_WRONLY=1, O_RDWR=2, O_CREAT=0x40, O_TRUNC=0x200, O_APPEND=0x400 };
/* Standard POSIX st_mode file-type bits, so userspace S_ISDIR/S_ISREG/
 * S_ISCHR work against what stat() reports (previously only directory
 * and regular were ever produced). */
constexpr uint32_t S_IFMT=0170000, S_IFCHR=0020000, S_IFDIR=0040000, S_IFREG=0100000;
/* Layout must match vlibc's `struct stat` (they're the same ABI struct).
 * The low 9 bits of st_mode are the rwx bits reported by the kernel. */
struct Stat { uint32_t mode,size; uint32_t type; uint32_t uid; uint32_t gid; };
}
