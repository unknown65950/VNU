/*
 * install.h — the disk installer, reached as `vnu install`.
 *
 * With arguments:  vnu install DRIVE HOSTNAME [SIZE_MIB]
 *   non-interactive; DRIVE is the disk index (0..N-1, see /proc/disks),
 *   HOSTNAME the name the machine should boot with (written to
 *   /etc/hostname and recorded on the disk), SIZE_MIB the partition size
 *   in MiB (omitted = the whole usable disk).
 *
 * With no arguments: a full-screen interactive wizard with blue ANSI
 * screens (the console's CSI/SGR subset) that lets you pick the target
 * disk, choose the partition size, type the host name, review the
 * choices, confirm the destructive write and — on success — reboot.
 *
 * The write itself is the kernel VNU_SYS_install syscall (30); it
 * stamps MBR + GRUB core image, records the host name in the config
 * sector before the partition and creates a FAT16 partition at LBA 2048
 * holding /boot/kernel.elf so the disk boots standalone.
 *
 * A header, not a .c, because `vnu` is one binary and vcc links one
 * source file per program (see AGENTS.md): shared code goes into a
 * header as static inline.
 */
#ifndef VNU_INSTALL_H
#define VNU_INSTALL_H

#include "../usertools/ue.h"

#define ANSI_RESET "\x1b[0m"
#define ANSI_CLEAR "\x1b[2J"
#define ANSI_TITLE "\x1b[1;37;44m" /* bold white on blue        */
#define ANSI_BODY  "\x1b[37;44m"   /* white on blue             */
#define ANSI_LABEL "\x1b[33;44m"   /* yellow(brown) on blue     */
#define ANSI_PICK  "\x1b[1;36;44m" /* bold cyan on blue         */

#define UE_DISKS_F "/proc/disks"

#define HOST_MAX 64 /* 63 characters plus the NUL */

struct disk_info {
    int idx;
    char model[41];
    unsigned long size_mib;
};

/* Read the /proc/disks table (`diskN \t MODEL \t SIZE_MIB MiB` per line,
 * produced by the kernel). Returns the number of entries or -1 if the
 * file could not be read. */
static inline int read_disks(struct disk_info* out, int max)
{
    int fd = open(UE_DISKS_F, O_RDONLY);
    if (fd < 0)
        return -1;
    char buf[2048];
    long got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got <= 0)
        return -1;
    buf[got] = 0;

    int n = 0;
    char* line = buf;
    while (line && *line && n < max) {
        char* nl = line;
        while (*nl && *nl != '\n')
            ++nl;
        if (*nl)
            *nl = 0;
        char* f[3] = {line, 0, 0};
        int fi = 0;
        for (char* p = line; *p && fi + 1 < 3; ++p)
            if (*p == '\t') {
                *p = 0;
                f[++fi] = p + 1;
            }
        if (fi >= 2) {
            struct disk_info* d = &out[n];
            unsigned long idx = (unsigned long)atoi(f[0] + 4);
            d->idx = (int)idx;
            strncpy(d->model, f[1], sizeof(d->model) - 1);
            d->model[sizeof(d->model) - 1] = 0;
            d->size_mib = (unsigned long)atoi(f[2]);
            ++n;
        }
        if (!*nl)
            break; /* no trailing newline: last line */
        line = nl + 1;
    }
    return n;
}

/* Read one line, echoing the characters the user types (the tty itself
 * does not echo). Returns the length, or -1 on Ctrl+C/EOF. */
static inline int read_field(char* out, int max)
{
    int n = 0;
    out[0] = 0;
    for (;;) {
        char raw = 0;
        if (read(0, &raw, 1) <= 0)
            return -1;
        unsigned char ch = (unsigned char)raw;
        if (ch == VNU_KEY_INTR) {
            ue_out("^C\n");
            return -1;
        }
        if (ch == '\n') {
            ue_out("\n");
            out[n] = 0;
            return n;
        }
        if ((ch == '\b' || ch == 0x7F) && n > 0) {
            --n;
            ue_out("\b \b");
        } else if (ch >= 32 && ch < 127 && n < max - 1) {
            out[n++] = (char)ch;
            out[n] = 0;
            write(1, &ch, 1);
        }
    }
}

static inline int yes_no(int* confirmed)
{
    char buf[8];
    if (read_field(buf, sizeof(buf)) < 0)
        return -1;
    *confirmed = (buf[0] == 'y' || buf[0] == 'Y');
    return 0;
}

static inline void banner(const char* title)
{
    ue_out(ANSI_CLEAR);
    ue_out(ANSI_TITLE);
    ue_out("VNU Setup");
    ue_out(ANSI_RESET);
    ue_out(ANSI_BODY);
    ue_out("\n\n");
    ue_out(title);
    ue_out("\n\n");
}

static inline void screen_reset(void)
{
    ue_out(ANSI_RESET);
    ue_out(ANSI_CLEAR);
}

static inline void print_disks(struct disk_info* d, int n)
{
    for (int i = 0; i < n; ++i) {
        ue_out(ANSI_PICK);
        ue_out("  ");
        ue_num((unsigned long)d[i].idx);
        ue_out("\t");
        ue_out(ANSI_BODY);
        if (d[i].model[0])
            ue_out(d[i].model);
        else
            ue_out("<unknown model>");
        ue_out("  [");
        ue_num(d[i].size_mib);
        ue_out(" MiB]\n");
    }
    ue_out("\n");
}

/* A host name is 1..63 characters of letters, digits, '-', '_' and '.',
 * starting and ending with a letter or digit. The kernel applies the
 * same rule to whatever it records on a disk (vnu/kernel/host/host.cpp),
 * so the same name can be typed here or stored in /etc/hostname. */
static inline int host_name_ok(const char* s)
{
    int n = 0;
    for (; s[n]; ++n) {
        if (n >= HOST_MAX - 1)
            return 0;
        char c = s[n];
        int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9');
        if (!alnum && c != '-' && c != '_' && c != '.')
            return 0;
        if (n == 0 && !alnum)
            return 0;
    }
    if (n == 0)
        return 0;
    char last = s[n - 1];
    return (last >= 'a' && last <= 'z') || (last >= 'A' && last <= 'Z') ||
           (last >= '0' && last <= '9');
}

/* The name this system currently answers to, for the wizard's default. */
static inline void current_host(char* out, int max)
{
    if (max < 2)
        return;
    out[0] = 0;
    if (gethostname(out, (unsigned long)max) != 0)
        strcpy(out, "vnu");
}

/* The running system is the one being installed — the kernel copies
 * itself onto the disk — so it takes the name as well: /etc/hostname is
 * rewritten and the next shell prompt already says it. */
static inline void adopt_host(const char* hostname)
{
    if (sethostname(hostname) == 0) {
        ue_out("vnu install: this machine is now called ");
        ue_out(hostname);
        ue_out(" (/etc/hostname)\n");
        return;
    }
    ue_err("vnu install: warning: /etc/hostname could not be updated\n");
}

/* Full-screen interactive installer: pick disk, pick partition size,
 * type the host name, review, confirm the destructive write, install,
 * offer a reboot. */
static inline int install_wizard(void)
{
    struct disk_info disks[8];
    int nd = read_disks(disks, 8);
    if (nd <= 0) {
        ue_out(ANSI_CLEAR);
        ue_out(ANSI_BODY);
        ue_out("VNU Setup: no disks detected.\n");
        ue_out(ANSI_RESET);
        return 1;
    }

    /* --- pick the target disk --- */
    int drive = -1;
    for (;;) {
        banner("Select the disk to install VNU onto");
        ue_out("The disk will be overwritten.\n\n");
        print_disks(disks, nd);
        ue_out("Enter the disk number, or type 'q' to cancel: ");
        char buf[16];
        if (read_field(buf, sizeof(buf)) < 0) {
            screen_reset();
            return 1;
        }
        if (buf[0] == 'q' || buf[0] == 'Q') {
            screen_reset();
            return 1;
        }
        int v = atoi(buf);
        int ok = 0;
        for (int i = 0; i < nd; ++i)
            if (disks[i].idx == v) {
                drive = v;
                ok = 1;
                break;
            }
        if (ok)
            break;
        ue_out("\nInvalid choice.\n");
        ue_out("Press Enter to try again...");
        char junk[8];
        read_field(junk, sizeof(junk));
    }

    /* --- pick the partition size --- */
    unsigned long size_mib = 0;
    for (;;) {
        banner("Choose the partition size");
        ue_out("Partition size in MiB. Leave empty to use the whole disk\n");
        ue_out("(up to what the FAT16 filesystem allows).\n\n");
        ue_out("Size in MiB: ");
        char buf[16];
        if (read_field(buf, sizeof(buf)) < 0) {
            screen_reset();
            return 1;
        }
        if (buf[0] == 0)
            break; /* whole disk */
        int v = atoi(buf);
        if (v > 0) {
            size_mib = (unsigned long)v;
            break;
        }
        ue_out("Invalid size.\n");
        ue_out("Press Enter to try again...");
        char junk[8];
        read_field(junk, sizeof(junk));
    }

    /* --- the host name --- */
    char hostname[HOST_MAX];
    char deflt[HOST_MAX];
    current_host(deflt, sizeof(deflt));
    for (;;) {
        banner("Name this machine");
        ue_out("The name is stored in /etc/hostname and shows up in the\n");
        ue_out("shell prompt, in uname and on the installed disk.\n");
        ue_out("Letters, digits, '-', '_' and '.', up to 63 characters.\n\n");
        ue_out("Host name [");
        ue_out(deflt);
        ue_out("]: ");
        if (read_field(hostname, sizeof(hostname)) < 0) {
            screen_reset();
            return 1;
        }
        if (hostname[0] == 0) {
            strcpy(hostname, deflt);
            break;
        }
        if (host_name_ok(hostname))
            break;
        ue_out("\nNot a valid host name.\n");
        ue_out("Press Enter to try again...");
        char junk[8];
        read_field(junk, sizeof(junk));
    }

    /* --- review --- */
    banner("Review your choices");
    ue_out("Target:  disk ");
    ue_num((unsigned long)drive);
    ue_out("\n");
    for (int i = 0; i < nd; ++i)
        if (disks[i].idx == drive) {
            if (disks[i].model[0]) {
                ue_out("Model:   ");
                ue_out(disks[i].model);
                ue_out("\n");
            }
            ue_out("Size:    ");
            ue_num(disks[i].size_mib);
            ue_out(" MiB\n");
            break;
        }
    ue_out("Partition: ");
    if (size_mib == 0)
        ue_out("whole disk (maximum allowed by FAT16)\n");
    else {
        ue_num(size_mib);
        ue_out(" MiB\n");
    }
    ue_out("Host:    ");
    ue_out(hostname);
    ue_out("\n\nThis will erase the whole disk. ");
    ue_out(ANSI_LABEL);
    ue_out("Continue? [y/N]: ");
    int go = 0;
    if (yes_no(&go) < 0) {
        screen_reset();
        return 1;
    }
    if (!go) {
        screen_reset();
        return 1;
    }

    /* --- write --- */
    ue_out(ANSI_RESET);
    ue_out(ANSI_CLEAR);
    ue_out(ANSI_BODY);
    ue_out("\nInstalling to disk ");
    ue_num((unsigned long)drive);
    ue_out(" ... this may take a while.\n");
    long rc = syscall(VNU_SYS_install, (unsigned long)drive, size_mib,
                      hostname);
    if (rc != 0) {
        ue_out("\nInstall failed (error ");
        ue_num((unsigned long)-rc);
        ue_out("). The disk was left unchanged.\n");
        if (rc == -VNU_EPERM)
            ue_out("You must be root to install.\n");
        screen_reset();
        return 1;
    }

    ue_out(ANSI_PICK);
    ue_out("Install complete.\n");
    ue_out(ANSI_BODY);
    adopt_host(hostname);
    ue_out("\nReboot now to boot from the new disk? [y/N]: ");
    int rb = 0;
    if (yes_no(&rb) < 0) {
        screen_reset();
        return 1;
    }
    if (rb) {
        ue_out("Rebooting...\n");
        syscall(VNU_SYS_reboot);
        return 1; /* a real reboot never returns */
    }
    ue_out("The disk is ready; reboot when you want to use it.\n");
    screen_reset();
    return 0;
}

static inline int install_non_interactive(int drive, const char* hostname,
                                          unsigned long size_mib)
{
    if (!host_name_ok(hostname)) {
        ue_err("vnu install: '");
        ue_err(hostname);
        ue_err("' is not a valid host name\n");
        return 1;
    }
    long n = syscall(VNU_SYS_blkcount);
    if (n <= 0) {
        ue_err("vnu install: no disks detected\n");
        return 1;
    }
    if (drive < 0 || drive >= (int)n) {
        ue_err("vnu install: drive out of range (available: 0..");
        ue_num((unsigned long)n - 1);
        ue_err(")\n");
        return 1;
    }
    long rc = syscall(VNU_SYS_install, (unsigned long)drive, size_mib,
                      hostname);
    if (rc == 0) {
        ue_out("vnu install: complete. Reboot from this disk to use it.\n");
        adopt_host(hostname);
        return 0;
    }
    ue_err("vnu install: failed (error ");
    ue_num((unsigned long)-rc);
    ue_err(")");
    if (rc == -VNU_EPERM)
        ue_err(": permission denied (run as root)");
    ue_err("\n");
    return 1;
}

/* `argc`/`argv` are the ones of `vnu install`, so av[0] is "install". */
static inline int vnu_install(int argc, char** argv)
{
    (void)argv;
    if (argc < 2)
        return install_wizard();

    if (argc < 3) {
        ue_err("vnu install: a host name is required: "
               "vnu install DRIVE HOSTNAME [SIZE_MIB]\n");
        ue_err("vnu install: with no arguments the wizard asks for both\n");
        return 1;
    }
    int drive = atoi(argv[1]);
    const char* hostname = argv[2];
    unsigned long size_mib = 0;
    if (argc > 3) {
        int v = atoi(argv[3]);
        if (v <= 0) {
            ue_err("vnu install: partition size must be a positive number "
                   "of MiB (omit it for the whole disk)\n");
            return 1;
        }
        size_mib = (unsigned long)v;
    }
    return install_non_interactive(drive, hostname, size_mib);
}

#endif // VNU_INSTALL_H
