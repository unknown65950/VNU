/* passwd — change a user's password in /etc/passwd */
#include "ue.h"

int main(int ac, char** av)
{
    if (getuid() != 0) {
        ue_err("passwd: only root can change passwords\n");
        return 1;
    }
    struct ue_pw self;
    const char* target = ac > 1 ? av[1] : (ue_pw_by_uid(getuid(), &self)
                                               ? self.name : "root");
    struct ue_pw all[UE_MAX_USERS];
    int n = ue_read_passwd(all);
    int idx = -1;
    for (int i = 0; i < n; ++i)
        if (strcmp(all[i].name, target) == 0) {
            idx = i;
            break;
        }
    if (idx < 0) {
        ue_err("passwd: no such user\n");
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
        ue_err("passwd: passwords do not match\n");
        return 1;
    }
    ue_pw_hash(p1, all[idx].hash);

    int fd = open(UE_PASSWD_F, O_WRONLY | O_TRUNC);
    if (fd < 0) {
        ue_err("passwd: cannot write /etc/passwd\n");
        return 1;
    }
    for (int i = 0; i < n; ++i) {
        char line[256];
        int li = ue_format_pwline(&all[i], line, sizeof(line));
        write(fd, line, (unsigned long)li);
    }
    close(fd);
    ue_out("passwd: password updated for ");
    ue_out(target);
    ue_out("\n");
    return 0;
}