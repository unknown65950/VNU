/* su — switch the session to another user and start a fresh shell */
#include "ue.h"

int main(int ac, char** av)
{
    const char* target = ac > 1 ? av[1] : "root";
    struct ue_pw e;
    if (!ue_pw_lookup(target, &e)) {
        ue_err("su: unknown user\n");
        return 1;
    }
    ue_out("Password: ");
    char pass[96];
    if (ue_read_secret(pass, sizeof(pass)) < 0)
        return 1;
    char hash[17];
    ue_pw_hash(pass, hash);
    if (strcmp(hash, e.hash) != 0) {
        ue_err("su: authentication failure\n");
        return 1;
    }
    /* Only an identity change the kernel will allow may proceed: root may
     * become anyone, anyone may become themselves. Refuse everything
     * else BEFORE touching the session file, or a denied switch would
     * clobber /tmp/.session (it is owned by the current user) and the
     * next respawned shell would adopt the wrong identity. */
    unsigned long cur_uid = (unsigned long)getuid();
    int permit = (cur_uid == 0) || (e.uid == cur_uid && e.gid == (unsigned long)getgid());
    if (!permit) {
        ue_err("su: cannot change identity (need root)\n");
        return 1;
    }
    if (cur_uid == 0)
        ue_save_session(e.name, e.uid, e.gid, e.home);
    if (setgid(e.gid) < 0 || setuid(e.uid) < 0) {
        ue_err("su: cannot change identity (need root)\n");
        return 1;
    }
    chdir(e.home);
    ue_out("su: switched to ");
    ue_out(e.name);
    ue_out("\n");
    char* argv[] = {"/bin/vash", 0};
    execve("/bin/vash", argv, 0);
    ue_err("su: cannot start shell\n");
    return 1;
}