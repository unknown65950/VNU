/* groups — print the current user's primary group name */
#include "ue.h"

int main(void)
{
    unsigned long gid = getgid();
    const char* gn = ue_group_name(gid);
    if (gn)
        ue_out(gn);
    else
        ue_num(gid);
    ue_out("\n");
    return 0;
}