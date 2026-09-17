/* ls — list directory contents (separate binary). */
#include "cu.h"

int main(int argc, char** argv)
{
    char cwd[128];
    const char* path = "/";
    if (argc > 1 && argv[1] && argv[1][0]) {
        path = argv[1];
        if (path[0] == '.' && path[1] == 0) {
            if (getcwd(cwd, sizeof(cwd)))
                path = cwd;
            else
                path = "/";
        }
    } else {
        if (getcwd(cwd, sizeof(cwd)))
            path = cwd;
        else
            path = "/";
    }
    DIR* d = opendir(path);
    if (!d) {
        we("ls: cannot open ");
        we(path);
        we("\n");
        return 1;
    }
    struct dirent* e;
    while ((e = readdir(d)) != 0) {
        w(e->d_name);
        w(e->d_type == 4 ? "/\n" : "\n");
    }
    closedir(d);
    return 0;
}