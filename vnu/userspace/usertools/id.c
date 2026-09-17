/* id — print the effective user and group names with their ids */
#include "ue.h"

int main(void)
{
    unsigned long uid = getuid();
    unsigned long gid = getgid();
    struct ue_pw e;
    ue_out("uid=");
    ue_num(uid);
    ue_out("(");
    if (ue_pw_by_uid(uid, &e))
        ue_out(e.name);
    else
        ue_num(uid);
    ue_out(") gid=");
    ue_num(gid);
    ue_out("(");
    const char* gn = ue_group_name(gid);
    if (gn)
        ue_out(gn);
    else
        ue_num(gid);
    ue_out(")\n");
    return 0;
}