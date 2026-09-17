#pragma once
#ifdef __cplusplus
extern "C" {
#endif
struct stat {
    unsigned int st_mode;
    unsigned int st_size;
    unsigned int st_type;
};

/* POSIX file-type bits and classification macros. The kernel now
 * reports these properly for directories, regular files and /dev
 * character devices (see kernel/include/vnu/posix.h). */
#define S_IFMT   0170000
#define S_IFCHR  0020000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)

int stat(const char* path, struct stat* st);
int fstat(int fd, struct stat* st);
int mkdir(const char* path, int mode);
int rmdir(const char* path);
int unlink(const char* path);
#ifdef __cplusplus
}
#endif
