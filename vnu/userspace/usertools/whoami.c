/* whoami — print the login name of the current user */
#include "ue.h"

int main(void)
{
    struct ue_pw e;
    unsigned long uid = getuid();
    if (ue_pw_by_uid(uid, &e))
        ue_out(e.name);
    else
        ue_num(uid);
    ue_out("\n");
    return 0;
}