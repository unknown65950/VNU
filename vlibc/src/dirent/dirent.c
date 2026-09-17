#include <vlibc/dirent.h>
#include <vlibc/unistd.h>
#include <vlibc/sys/syscall.h>
#include <vlibc/stdlib.h>
#include <vlibc/string.h>

/* Kernel Dirent layout must match vnu::vfs::Dirent */
struct kdirent {
    unsigned int ino;
    unsigned short reclen;
    unsigned char type;
    char name[56];
} __attribute__((packed));

struct DIR {
    int fd;
    char buf[512];
    int pos;
    int end;
    struct dirent out;
};

DIR* opendir(const char* name) {
    int fd = open(name, 0);
    if (fd < 0) return 0;
    DIR* d = (DIR*)malloc(sizeof(DIR));
    if (!d) { close(fd); return 0; }
    memset(d, 0, sizeof(DIR));
    d->fd = fd;
    return d;
}

struct dirent* readdir(DIR* dir) {
    if (!dir) return 0;
    if (dir->pos >= dir->end) {
        long n = syscall(SYS_getdents, dir->fd, dir->buf, sizeof(dir->buf));
        if (n <= 0) return 0;
        dir->pos = 0;
        dir->end = (int)n;
    }
    struct kdirent* k = (struct kdirent*)(dir->buf + dir->pos);
    dir->pos += k->reclen;
    dir->out.d_ino = k->ino;
    dir->out.d_reclen = k->reclen;
    dir->out.d_type = k->type;
    strncpy(dir->out.d_name, k->name, sizeof(dir->out.d_name) - 1);
    return &dir->out;
}

int closedir(DIR* dir) {
    if (!dir) return -1;
    int r = close(dir->fd);
    free(dir);
    return r;
}
