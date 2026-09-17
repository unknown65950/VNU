#pragma once
#ifdef __cplusplus
extern "C" {
#endif

struct dirent {
    unsigned long d_ino;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[256];
};

typedef struct DIR DIR;
DIR* opendir(const char* name);
struct dirent* readdir(DIR* dir);
int closedir(DIR* dir);

#ifdef __cplusplus
}
#endif
