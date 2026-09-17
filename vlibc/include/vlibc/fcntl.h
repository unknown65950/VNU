#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/* POSIX open() flags. These must match vnu::posix::OpenFlags in
 * kernel/include/vnu/posix.h — the kernel reads them straight off the
 * syscall argument. Previously callers had to spell out the raw
 * numbers (0x40 for O_CREAT and so on), which is easy to get wrong. */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400

#ifdef __cplusplus
}
#endif
