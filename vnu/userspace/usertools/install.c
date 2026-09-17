/* install — write the bootable VNU image onto an ATA disk */
#include "ue.h"

int main(int ac, char** av)
{
    long n = syscall(VNU_SYS_blkcount);
    if (n <= 0) {
        ue_err("install: no disks detected\n");
        return 1;
    }
    int drive = ac > 1 ? atoi(av[1]) : 0;
    if (drive < 0 || drive >= (int)n) {
        ue_out("install: drive out of range (available: 0..");
        ue_num((unsigned long)n - 1);
        ue_out(")\n");
        return 1;
    }
    ue_out("install: target disk ");
    ue_num((unsigned long)drive);
    ue_out(" of ");
    ue_num((unsigned long)n);
    ue_out(" — writing VNU...\n");
    long rc = syscall(VNU_SYS_install, (unsigned long)drive);
    if (rc == 0) {
        ue_out("install: complete. Reboot from this disk to use it.\n");
        return 0;
    }
    ue_err("install: failed (error ");
    ue_num((unsigned long)rc);
    ue_err(")\n");
    return 1;
}