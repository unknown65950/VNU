/* useradd — add a new user account to /etc/passwd */
#include "ue.h"

int main(int ac, char** av)
{
    if (getuid() != 0) {
        ue_err("useradd: only root may add users\n");
        return 1;
    }
    if (ac != 2) {
        ue_err("usage: useradd <name>\n");
        return 1;
    }
    const char* name = av[1];
    if (!name[0] || strlen(name) > 24) {
        ue_err("useradd: bad name\n");
        return 1;
    }
    for (const char* p = name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-')) {
            ue_err("useradd: name must be [a-z0-9_-]\n");
            return 1;
        }
    struct ue_pw all[UE_MAX_USERS];
    int n = ue_read_passwd(all);
    for (int i = 0; i < n; ++i)
        if (strcmp(all[i].name, name) == 0) {
            ue_err("useradd: user already exists\n");
            return 1;
        }

    char p1[96], p2[96];
    ue_out("New password: ");
    if (ue_read_secret(p1, sizeof(p1)) < 0)
        return 1;
    ue_out("Retype password: ");
    if (ue_read_secret(p2, sizeof(p2)) < 0)
        return 1;
    if (strcmp(p1, p2) != 0) {
        ue_err("useradd: passwords do not match\n");
        return 1;
    }

    unsigned long next_uid = 1000;
    for (int i = 0; i < n; ++i)
        if (all[i].uid >= next_uid)
            next_uid = all[i].uid + 1;

    struct ue_pw e;
    memset(&e, 0, sizeof(e));
    strncpy(e.name, name, sizeof(e.name) - 1);
    ue_pw_hash(p1, e.hash);
    e.uid = next_uid;
    e.gid = 100; /* users */
    ue_join_path(e.home, sizeof(e.home), "/home", name);

    int fd = open(UE_PASSWD_F, O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0) {
        ue_err("useradd: cannot write /etc/passwd\n");
        return 1;
    }
    char line[256];
    int li = ue_format_pwline(&e, line, sizeof(line));
    long rc = write(fd, line, (unsigned long)li);
    close(fd);
    if (rc <= 0) {
        ue_err("useradd: write failed\n");
        return 1;
    }

    /* Home directory: create, then hand it to the new owner with a
     * private 0700, exactly like a real useradd. */
    if (mkdir(e.home, 0755) < 0)
        ue_err("useradd: warning: home dir exists\n");
    chown(e.home, e.uid, e.gid);
    chmod(e.home, 0700);

    ue_out("user ");
    ue_out(name);
    ue_out(" added (uid ");
    ue_num(next_uid);
    ue_out(")\n");
    return 0;
}