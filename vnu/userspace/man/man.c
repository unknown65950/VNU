/*
 * man — VNU manual pager.
 *
 * Displays the reference manual page for a command. Unlike a classic
 * man(1) this binary does not read files off disk: the pages live in a
 * database compiled into this very ELF (the VFS is RAM-backed and the
 * per-node buffer is shared with embedded binaries, so a real
 * /usr/share/man tree would blow the node table). That keeps `man`
 * self-contained and working from anywhere, for any user.
 *
 * On a terminal the page is paged: the arrow keys scroll it a line at a
 * time, other keys scroll a screen or jump to an end, `h` lists them all
 * and `q` goes back to the shell. A white status bar on the last row says
 * which page and line you are on. When stdout is not a terminal (a
 * redirect, a test) the page is simply printed, so `man ls > file` and
 * the test harness keep working.
 *
 * Usage:
 *     man              list every documented command
 *     man -l           same as above
 *     man NAME [NAME]  print the page(s) for NAME
 *     man -h           this help
 *
 * POLICY — READ THIS IF YOU ADD A COMMAND:
 * Every user-visible command in the system (vash builtins, coreutils
 * applets and standalone binaries like vedit) MUST have a page in the
 * pages[] table at the bottom of this file before the feature is
 * considered done. The rule lives in AGENTS.md at the repository root;
 * new commands get rejected in review without a `man <cmd>` page.
 */
#include <vlibc/unistd.h>
#include <vlibc/string.h>
#include <vlibc/stdio.h>
#include <vlibc/stdlib.h>
#include <vlibc/keys.h>
#include <vlibc/term.h>

struct Page {
    const char* name;
    const char* desc;   /* one-line summary, used by the listing */
    const char* text;   /* the full manual page */
};

static void w(const char* s)
{
    if (s)
        write(1, s, strlen(s));
}

static void we(const char* s)
{
    if (s)
        write(2, s, strlen(s));
}

/* --- manual pages --------------------------------------------------- */

#define MAN_PAGE "NAME\n\
    man - display reference manual pages\n\
\n\
SYNOPSIS\n\
    man [NAME...]\n\
    man [-l | --list]\n\
    man [-h | --help]\n\
\n\
DESCRIPTION\n\
    Displays the manual page for each NAME on standard output.\n\
    With no arguments (or with -l) it lists every documented\n\
    command on the system with a one-line summary, which doubles\n\
    as a quick way to discover what is installed.\n\
\n\
    When the output is a terminal the page is shown in a pager\n\
    instead: a white status bar on the last row reads\n\
        Manual page vcc(1) line 12 (press h for help or q to quit)\n\
    and names the page and the first line you are looking at. When\n\
    the output is redirected the page is simply printed instead, so\n\
    'man ls > /tmp/ls.txt' leaves you a file to grep.\n\
\n\
KEYS\n\
    Up / Down      scroll one line (k and j do the same)\n\
    space, f, Enter  scroll down one screen (also Ctrl+F, Ctrl+D)\n\
    b, u           scroll up one screen (also Ctrl+B)\n\
    Home, g        jump to the first line of the page\n\
    End, G         jump to the last line of the page\n\
    h              list the keys (any other key goes back)\n\
    q              quit and return to the shell (Esc, Ctrl+C too)\n\
\n\
EXAMPLES\n\
    man ls          page for the ls command\n\
    man useradd id  two pages at once\n\
    man             full listing of documented commands\n\
    man vcc > /tmp/v.txt  the page as a plain file, no pager\n\
\n\
SEE ALSO\n\
    Every command in the system has a page; try man vash,\n\
    man vedit, man coreutils. AGENTS.md at the repo root rules\n\
    that new commands must come with a page.\n"

#define VASH_PAGE "NAME\n\
    vash - VNU shell\n\
\n\
SYNOPSIS\n\
    vash\n\
\n\
DESCRIPTION\n\
    The interactive login shell. Prints a prompt of the form\n\
    user@name:cwd$, where name comes from /etc/hostname, and reads\n\
    commands line by line. Words are\n\
    split on whitespace; there is no quoting or piping yet.\n\
\n\
    Redirection is supported with > file, >> file (append) and\n\
    < file; the operators may be attached or space-separated.\n\
\n\
    On a cold boot vash runs the login sequence (VNU login: /\n\
    Password:) against /etc/passwd. After an external command\n\
    the kernel respawns it and it silently re-adopts the session\n\
    it saved to /tmp/.session, so a 'logout' (exit) returns to\n\
    the login prompt.\n\
\n\
    Editor keys: Up/Down history, Ctrl+C cancels a line, Shift\n\
    is used to type symbol characters.\n\
\n\
    Also installed as /bin/sh, /sbin/init and as the GUI's\n\
    'term' app - see man sh, man init, man term.\n\
\n\
OPTIONS\n\
    none.\n"

#define CD_PAGE "NAME\n\
    cd - change the working directory\n\
\n\
SYNOPSIS\n\
    cd [DIR]\n\
\n\
DESCRIPTION\n\
    Makes DIR the current working directory. With no argument\n\
    it returns to the invoking user's home directory. '.' and\n\
    '..' are resolved by the VFS, so cd .. works like on a real\n\
    Unix. Built-in to vash (run inside the shell).\n"

#define EXPORT_PAGE "NAME\n\
    export - print or set the PATH search path\n\
\n\
SYNOPSIS\n\
    export\n\
    export PATH=<dirs>\n\
\n\
DESCRIPTION\n\
    With no argument, prints the current entry search path used\n\
    to locate external commands. With an argument of the form\n\
    PATH=dir1:dir2:... it replaces the search path in the current\n\
    session. Only PATH is supported. Shell builtin.\n"

#define UNSET_PAGE "NAME\n\
    unset - empty (unset) the command search path\n\
\n\
SYNOPSIS\n\
    unset PATH\n\
\n\
DESCRIPTION\n\
    Clears the session PATH. External commands then resolve\n\
    directly to /bin anyway. Only PATH may be unset. Shell builtin.\n"

#define EXIT_PAGE "NAME\n\
    exit - end the shell session\n\
\n\
SYNOPSIS\n\
    exit [STATUS]\n\
\n\
DESCRIPTION\n\
    Terminates the current shell. In a login shell this is a\n\
    logout: the saved session is dropped and the machine returns\n\
    to the 'VNU login:' prompt. STATUS, an optional exit code,\n\
    is returned to the kernel. Shell builtin.\n"

#define HELP_PAGE "NAME\n\
    help - list the shell builtins\n\
\n\
SYNOPSIS\n\
    help\n\
\n\
DESCRIPTION\n\
    Prints the list of vash built-in commands, the current PATH\n\
    and a reminder of the editor keys. For full documentation of\n\
    any command use man (see man man). Shell builtin.\n"

#define TYPE_PAGE "NAME\n\
    type - say how a name would be interpreted\n\
\n\
SYNOPSIS\n\
    type NAME\n\
\n\
DESCRIPTION\n\
    Reports whether NAME is a shell builtin ('NAME is a shell\n\
    builtin') or resolves to an external program path. Identical\n\
    in behaviour to which. Shell builtin.\n"

#define WHICH_PAGE "NAME\n\
    which - report where a command lives\n\
\n\
SYNOPSIS\n\
    which NAME\n\
\n\
DESCRIPTION\n\
    Returns the path (or 'is a shell builtin') that would be\n\
    used for NAME. Identical in behaviour to type.\n"

#define ID_PAGE "NAME\n\
    id - print the current identity\n\
\n\
SYNOPSIS\n\
    id\n\
\n\
DESCRIPTION\n\
    Prints the effective user and group as uid and gid with\n\
    names, e.g. uid=1000(guest) gid=100(users). Standalone\n\
    binary in /bin.\n"

#define WHOAMI_PAGE "NAME\n\
    whoami - print the current user name\n\
\n\
SYNOPSIS\n\
    whoami\n\
\n\
DESCRIPTION\n\
    Prints the login name of the current user. Standalone\n\
    binary in /bin.\n"

#define GROUPS_PAGE "NAME\n\
    groups - print the current group name\n\
\n\
SYNOPSIS\n\
    groups\n\
\n\
DESCRIPTION\n\
    Prints the group of the current session. Standalone\n\
    binary in /bin.\n"

#define USERADD_PAGE "NAME\n\
    useradd - add a new user account\n\
\n\
SYNOPSIS\n\
    useradd NAME\n\
\n\
DESCRIPTION\n\
    Root-only. Adds a user called NAME (lowercase letters,\n\
    digits, '_' and '-' only) to /etc/passwd, prompting for a\n\
    password twice. The new user gets uid >= 1000, primary group\n\
    users (100), a home directory /home/NAME, owned by them with\n\
    mode 0700.\n\
\n\
SEE ALSO\n\
    man passwd, man su\n"

#define PASSWD_PAGE "NAME\n\
    passwd - change a user's password\n\
\n\
SYNOPSIS\n\
    passwd [USER]\n\
\n\
DESCRIPTION\n\
    Root-only. Replaces the stored hash of USER (default: the\n\
    current user) in /etc/passwd after asking for the new\n\
    password twice. Standalone binary in /bin.\n"

#define SU_PAGE "NAME\n\
    su - switch the session to another user\n\
\n\
SYNOPSIS\n\
    su [USER]\n\
\n\
DESCRIPTION\n\
    Requires USER's password and switches the session identity\n\
    and working directory to that user. Changing to anything\n\
    other than your own privilege level needs root. On success\n\
    it starts a fresh /bin/vash as the target user.\n\
    Standalone binary in /bin.\n"

#define COREUTILS_PAGE "NAME\n\
    coreutils - the standard command set (index)\n\
\n\
SYNOPSIS\n\
    (no such binary - see DESCRIPTION)\n\
\n\
DESCRIPTION\n\
    The basic operating-system tools used to be one busybox-style\n\
    multi-call binary selected by argv[0]. They are now each a\n\
    separate, self-contained executable in /bin:\n\
\n\
        echo true false pwd cat ls mkdir rm touch uname clear\n\
        wc head tail grep sort cp mv basename dirname seq df ping\n\
\n\
    There is no 'coreutils' program on the image any more; this\n\
    page is kept as an index of the set. Run 'man <name>' for a\n\
    single tool, or bare 'man' for every documented command.\n\
\n\
    The separate account tools id, whoami, groups, useradd,\n\
    passwd and su are standalone /bin binaries too. The disk\n\
    installer is not one of them: it is 'vnu install' (see man vnu).\n"

#define ECHO_PAGE "NAME\n\
    echo - write its arguments to standard output\n\
\n\
SYNOPSIS\n\
    echo [WORD...]\n\
\n\
DESCRIPTION\n\
    Writes each WORD in order, separated by a single space, and\n\
    finishes with a newline. No option handling (backslashes,\n\
    -n and -e are copied literally).\n"

#define TRUE_PAGE "NAME\n\
    true - do nothing, successfully\n\
\n\
SYNOPSIS\n\
    true\n\
\n\
DESCRIPTION\n\
    Ignores all arguments and exits with status 0. Useful in\n\
    scripts as the body of an 'always succeeds' command.\n"

#define FALSE_PAGE "NAME\n\
    false - do nothing, unsuccessfully\n\
\n\
SYNOPSIS\n\
    false\n\
\n\
DESCRIPTION\n\
    Ignores all arguments and exits with status 1. The\n\
    traditional 'never succeeds' command.\n"

#define PWD_PAGE "NAME\n\
    pwd - print the working directory\n\
\n\
SYNOPSIS\n\
    pwd\n\
\n\
DESCRIPTION\n\
    Writes the absolute path of the current working directory\n\
    to standard output.\n"

#define CAT_PAGE "NAME\n\
    cat - concatenate files and print\n\
\n\
SYNOPSIS\n\
    cat [FILE...]\n\
\n\
DESCRIPTION\n\
    Reads each FILE in turn and writes it to standard output.\n\
    With no arguments, copies standard input to standard output.\n\
    Sizes are bounded by the VFS file buffer (20 KiB).\n"

#define LS_PAGE "NAME\n\
    ls - list directory contents\n\
\n\
SYNOPSIS\n\
    ls [DIR]\n\
\n\
DESCRIPTION\n\
    Prints the names of the entries in DIR, one per line, with\n\
    a trailing '/' on directories. The default is the current\n\
    directory; '.' is accepted as an explicit current directory.\n\
    Sorting is by VFS node order, not alphabetical.\n"

#define MKDIR_PAGE "NAME\n\
    mkdir - create a directory\n\
\n\
SYNOPSIS\n\
    mkdir DIR...\n\
\n\
DESCRIPTION\n\
    Creates each DIR (mode 0755, owned by the caller).\n\
    Intermediate directories are not created automatically.\n"

#define RM_PAGE "NAME\n\
    rm - remove files or directories\n\
\n\
SYNOPSIS\n\
    rm FILE...\n\
\n\
DESCRIPTION\n\
    Removes each argument: it tries unlink first and falls back\n\
    to rmdir, so both files and directories are acceptable\n\
    operands. There is no -r, -f or confirmation; nothing is\n\
    recursive.\n"

#define TOUCH_PAGE "NAME\n\
    touch - create an empty file\n\
\n\
SYNOPSIS\n\
    touch FILE...\n\
\n\
DESCRIPTION\n\
    Creates each FILE if it does not exist (as an empty,\n\
    root-or-caller-owned regular file) and ignores existing\n\
    files. Unlike real touch it never updates timestamps.\n"

#define UNAME_PAGE "NAME\n\
    uname - print system information\n\
\n\
SYNOPSIS\n\
    uname [OPTION]...\n\
\n\
DESCRIPTION\n\
    With no option prints the kernel name (same as -s).\n\
\n\
    -a, --all            print everything\n\
    -s, --kernel-name    kernel name\n\
    -n, --nodename       host name (from /etc/hostname; see\n\
                         'man hostname')\n\
    -r, --kernel-release kernel release\n\
    -v, --kernel-version kernel version\n\
    -m, --machine        machine hardware name\n\
    -p, --processor      processor type\n\
    -i, --hardware-platform\n\
    -o, --operating-system\n\
        --help           show help and exit\n\
        --version        show version and exit\n"

#define CLEAR_PAGE "NAME\n\
    clear - clear the terminal screen\n\
\n\
SYNOPSIS\n\
    clear\n\
\n\
DESCRIPTION\n\
    Writes enough newlines to push prior output off the top of\n\
    the 25-row text console. No ANSI escape sequences are used.\n"

#define WC_PAGE "NAME\n\
    wc - count lines, words and bytes\n\
\n\
SYNOPSIS\n\
    wc [FILE...]\n\
\n\
DESCRIPTION\n\
    For each FILE (or standard input, with no arguments) prints\n\
    the number of lines, words and bytes, then the name. A word\n\
    is any run of non-blank characters.\n"

#define HEAD_PAGE "NAME\n\
    head - print the first lines of a file\n\
\n\
SYNOPSIS\n\
    head [-N] [FILE]\n\
\n\
DESCRIPTION\n\
    Prints the first N lines (default 10). The whole file is\n\
    read into a fixed buffer first, so only the first 128 lines\n\
    (or 100 columns each) of any input are ever seen - a\n\
    documented consequence of using a static buffer.\n"

#define TAIL_PAGE "NAME\n\
    tail - print the last lines of a file\n\
\n\
SYNOPSIS\n\
    tail [-N] [FILE]\n\
\n\
DESCRIPTION\n\
    Prints the last N lines (default 10), subject to the same\n\
    fixed-buffer 128-line limit as head (see man head).\n"

#define GREP_PAGE "NAME\n\
    grep - print lines matching a pattern\n\
\n\
SYNOPSIS\n\
    grep PATTERN [FILE]\n\
\n\
DESCRIPTION\n\
    Prints every line of FILE (or standard input) that contains\n\
    PATTERN as a plain substring. There is no regular-expresion\n\
    support, no options, and the same 128-line input cap as\n\
    head. Exit status is 0 if a match was found, 1 otherwise.\n"

#define SORT_PAGE "NAME\n\
    sort - sort lines of a file\n\
\n\
SYNOPSIS\n\
    sort [FILE]\n\
\n\
DESCRIPTION\n\
    Sorts the lines of FILE (or standard input) in ascending\n\
    bytewise (strcmp) order using insertion sort, then prints\n\
    them. Subject to the 128-line fixed-buffer limit like head.\n\
    No options.\n"

#define CP_PAGE "NAME\n\
    cp - copy a file\n\
\n\
SYNOPSIS\n\
    cp SRC DST\n\
\n\
DESCRIPTION\n\
    Copies the contents of SRC to DST, creating or truncating\n\
    DST. Permissions and ownership of DST follow the umask\n\
    defaults of whoever creates it, not SRC's.\n"

#define MV_PAGE "NAME\n\
    mv - move (rename) a file\n\
\n\
SYNOPSIS\n\
    mv SRC DST\n\
\n\
DESCRIPTION\n\
    Moves SRC to DST. There is no rename() in the VFS yet, so\n\
    this is implemented as copy-then-unlink. Directories cannot\n\
    be moved.\n"

#define BASENAME_PAGE "NAME\n\
    basename - strip the directory part of a path\n\
\n\
SYNOPSIS\n\
    basename PATH\n\
\n\
DESCRIPTION\n\
    Prints PATH with any leading directory components removed,\n\
    e.g. basename /bin/ls prints ls.\n"

#define DIRNAME_PAGE "NAME\n\
    dirname - strip the final component of a path\n\
\n\
SYNOPSIS\n\
    dirname PATH\n\
\n\
DESCRIPTION\n\
    Prints PATH with its last component removed, giving the\n\
    directory part. Trailing slashes are stripped first, and a\n\
    path with no directory component yields '.'. A root path\n\
    yields '/'. Examples: dirname /bin/ls -> /bin, dirname ls\n\
    -> .\n"

#define SEQ_PAGE "NAME\n\
    seq - print a sequence of numbers\n\
\n\
SYNOPSIS\n\
    seq LAST\n\
    seq FIRST LAST\n\
    seq FIRST STEP LAST\n\
\n\
DESCRIPTION\n\
    Prints integers from FIRST (default 1) up to LAST, counting\n\
    by STEP (default 1), one per line. A negative STEP counts\n\
    downwards. Unrepresentable values are printed as 0.\n"

#define DF_PAGE "NAME\n\
    df - report file system space usage\n\
\n\
SYNOPSIS\n\
    df [OPTION] [FILE...]\n\
\n\
DESCRIPTION\n\
    Shows the amount of space used and available on each mounted\n\
    file system. VNU's only real file system is the in-memory VFS\n\
    (mounted on /); /dev and /proc are pseudo file systems with\n\
    no accounted space, so they print zeros. FILE operands restrict\n\
    the report to the file systems containing those files.\n\
\n\
    Sizes are shown in 1 KiB blocks unless a human-readable option\n\
    is given.\n\
\n\
OPTIONS\n\
    -a            include pseudo file systems (all are shown anyway)\n\
    -h            human-readable sizes (powers of 1024)\n\
    -H            human-readable sizes (powers of 1000)\n\
    -k            show sizes in 1 KiB blocks (the default)\n\
    -l            limit to local file systems (all are local)\n\
    -P            POSIX output format (1024-blocks, single spaces)\n\
    -T            also print the file system type\n\
    -t TYPE       only include file systems of type TYPE\n\
    -x TYPE       exclude file systems of type TYPE\n\
    -v            ignored (macOS compatibility)\n\
        --help    display this help and exit\n\
        --version output version information and exit\n\
\n\
EXAMPLES\n\
    df\n\
    df -h /proc/version\n\
    df -T -t vfs\n"

#define VCC_PAGE "NAME\n\
    vcc - the VNU C compiler\n\
\n\
SYNOPSIS\n\
    vcc [-c] [-o out] [-I dir] file.c [file.o] [file.a]\n\
\n\
DESCRIPTION\n\
    Compiles and links C programs for VNU: a preprocessor, a compiler\n\
    for a strict i386 C subset into ELF relocatable objects, and a\n\
    linker that links against the standard library (crt0.o plus\n\
    libvlibc.a, read from /lib). The whole compiler runs inside the OS\n\
    and writes its output to the VFS, so a simple\n\
        vcc hello.c -o a.out\n\
    produces a VNU executable.\n\
\n\
    Flags are appended in any order. With -c only the relocatable\n\
    object is written (default file a.o); without it vcc compiles the\n\
    single .c file and links it with crt0.o and the whole libvlibc.a\n\
    into a full VNU ELF (default a.out). Object files may be linked\n\
    individually through a lib.a archive.\n\
\n\
    Input sources are read from the VFS; no files larger than memory\n\
    are expected. vcc is itself compiled with vcc (it is self-hosting).\n\
\n\
OPTIONS\n\
    -c            compile only, write the object file\n\
    -o out        output file (default a.o with -c, else a.out)\n\
    -I dir        add dir to the include search path\n\
        --help    display this help and exit\n\
        --version output version information and exit\n\
\n\
EXAMPLES\n\
    vcc -c hello.c -o hello.o\n\
    vcc main.c libfoo.a -o app          link app from main.c and the\n\
                                        archive\n\
\n\
FILES\n\
    /lib/crt0.o     startup object\n\
    /lib/libvlibc.a standard C library archive\n\
\n\
EXIT STATUS\n\
    0 on success, 1 on any compile, link or I/O error.\n"

#define VNU_PAGE "NAME\n\
    vnu - report what system you are running on\n\
\n\
SYNOPSIS\n\
    vnu [COMMAND]...\n\
\n\
DESCRIPTION\n\
    Prints a summary of the running system: its versions, how long it\n\
    has been up, its host name, memory use, the shell and file system\n\
    the session runs in, the display, and how much space each part of\n\
    the system image takes.\n\
\n\
    Every value is read from the running system (uname(2), gfx_getinfo(2)\n\
    for the display, the /proc files listed below, and the size\n\
    accounting in the kernel, so the output describes the machine you\n\
    are on, not a built-in table.\n\
    Nothing is written to disk and no other process is disturbed, so\n\
    vnu is safe to run at any time, including from scripts.\n\
\n\
    With no COMMAND the usage summary is printed.\n\
\n\
COMMANDS\n\
    fetch     overview of the running system: OS and kernel version,\n\
              uptime, host name, memory, shell, file system, display\n\
              with its depth and driver, compiler version and build\n\
              date\n\
    version   version block: OS, kernel, compiler, ABI version and\n\
              build date\n\
    size      image size per component (kernel, userspace, libraries,\n\
              fonts, resources) with the number of objects in each\n\
    install   install VNU onto a disk, see 'vnu install' below\n\
\n\
    The remaining options are:\n\
\n\
        --help    display this help and exit\n\
        --version output version information and exit\n\
\n\
VNU INSTALL\n\
    With no argument, install opens a full-screen wizard in the style of\n\
    the classic Windows Setup / FreeBSD sysinstall / Slackware setup\n\
    screens: pick the target disk (from /proc/disks), choose the\n\
    partition size, type the host name for the machine, review the\n\
    choices, confirm the destructive write and reboot. Blue screens use\n\
    the console's ANSI color support.\n\
\n\
    With arguments it runs without asking anything:\n\
\n\
        vnu install DRIVE HOSTNAME [SIZE_MIB]\n\
\n\
    DRIVE is the disk index (0..N-1, the same numbering as /proc/disks),\n\
    HOSTNAME the name the machine should boot with (see 'man\n\
    hostname'; it is written to /etc/hostname and recorded on the disk)\n\
    and SIZE_MIB the partition size in MiB, omitted for the whole usable\n\
    disk. Requires root.\n\
\n\
    The write itself is done by the kernel (syscall VNU_SYS_install,\n\
    30): it stamps the MBR + GRUB core image, records the host name in\n\
    the config sector before the partition, and creates a FAT16\n\
    partition at LBA 2048 holding /boot/kernel.elf and a GRUB config, so\n\
    the disk boots on its own with no CD. This is what a distro\n\
    'sysinst' does.\n\
\n\
EXAMPLES\n\
    vnu\n\
    vnu fetch      the Graphics line: driver, WxH and depth\n\
    vnu version\n\
    vnu size\n\
    vnu install           the wizard\n\
    vnu install 0 mybox   disk 0, named mybox, whole disk\n\
\n\
FILES\n\
    /proc/version  OS, kernel and build versions\n\
    /proc/meminfo  total and free memory\n\
    /proc/boot     root file system, install state, session shell\n\
    /proc/images   per-component image sizes\n\
    /etc/hostname  the name of this machine\n\
\n\
EXIT STATUS\n\
    0 on success, 1 when the requested information cannot be read, or\n\
    when an install fails (in which case nothing was written to the\n\
    disk).\n"

#define PING_PAGE "NAME\n\
    ping - send ICMP echo requests (network test)\n\
\n\
SYNOPSIS\n\
    ping TARGET [COUNT]\n\
\n\
DESCRIPTION\n\
    Sends an IPv4 ICMP echo request to TARGET and reports the\n\
    round-trip time the kernel measured. Used to check that the NIC is\n\
    up: on QEMU's default user network the host router answers at\n\
    10.0.2.2, so 'ping 10.0.2.2' is a quick connectivity smoke test.\n\
\n\
    TARGET is a dotted-quad address or a host name. Names are resolved\n\
    through /etc/hosts first, then via DNS: A queries to each\n\
    'nameserver' line of /etc/resolv.conf (default server 1.1.1.1),\n\
    tried in order. COUNT (default 4) picks how many requests to send\n\
    before printing a summary; each request waits at most 300 ms in the\n\
    kernel (ARP resolution for a new target, via the gateway off\n\
    subnet).\n\
\n\
FILES\n\
    /etc/hosts\n\
    /etc/resolv.conf\n\
\n\
EXIT STATUS\n\
    0 if at least one reply arrived, 1 otherwise; 2 on bad usage or an\n\
    unknown host.\n\
\n\
EXAMPLES\n\
    ping 10.0.2.2\n\
    ping gateway 1\n\
    ping 10.0.2.2 1\n"

#define HOSTNAME_PAGE "NAME\n\
    hostname - the name of this machine\n\
\n\
SYNOPSIS\n\
    cat /etc/hostname\n\
\n\
DESCRIPTION\n\
    /etc/hostname holds the node name of the machine: one line, no\n\
    spaces, as on any other Unix. It is the single source of truth, so\n\
    everything that has to know who the machine is reads it from\n\
    here instead of keeping a copy:\n\
\n\
      - the vash prompt, which prints user@name:cwd$ where it used to\n\
        print a hard-coded root@vnu:...\n\
      - uname(2), so 'uname -n' and 'uname -a' agree with the prompt\n\
      - the installer, which asks for the name and records it on the\n\
        disk it writes (vnu install)\n\
\n\
    A name is 1 to 63 characters of letters, digits, '-', '_' and\n\
    '.', and must start and end with a letter or digit. 'vnu fetch'\n\
    reports the current one as its Host line.\n\
\n\
    The file is seeded at boot with the built-in default 'vnu'. An\n\
    installed disk carries the name it was given, so booting that disk\n\
    puts the name here instead. To rename a running system, write the\n\
    file as root ('echo mybox > /etc/hostname') or call sethostname()\n\
    from a program; the shell picks the new name up at the next\n\
    prompt. Keep it short: it is part of every prompt.\n\
\n\
    See 'man vnu' for the installer and 'man uname' for -n.\n\
\n\
FILES\n\
    /etc/hostname\n\
"

#define HOSTS_PAGE "NAME\n\
    hosts - static host name to IP mapping\n\
\n\
DESCRIPTION\n\
    /etc/hosts maps names to IPv4 addresses before any DNS traffic is\n\
    sent, so entries work offline and always win over DNS answers.\n\
\n\
    Each non-comment line lists an IP followed by one or more aliases:\n\
\n\
        10.0.2.2    gateway\n\
        10.0.2.15   vnu\n\
\n\
    Lines starting with '#' are ignored; fields are separated by\n\
    spaces or tabs. Lookups are case-insensitive. The file is seeded\n\
    at boot and is a plain editable file (root: 0644).\n\
\n\
FILES\n\
    /etc/hosts\n"

#define RESOLV_PAGE "NAME\n\
    resolv.conf - DNS server configuration\n\
\n\
DESCRIPTION\n\
    /etc/resolv.conf lists the DNS servers the kernel queries when a\n\
    name is not found in /etc/hosts. Each 'nameserver a.b.c.d' line\n\
    adds a server; they are tried in order until one answers. Comment\n\
    lines start with '#' or ';'.\n\
\n\
    The default file contains 'nameserver 1.1.1.1'. If the file is\n\
    missing or lists no servers, 1.1.1.1 is used. Under QEMU's slirp\n\
    user network the guest-side resolver is also reachable at\n\
    10.0.2.3, and any external server (1.1.1.1, 8.8.8.8, ...) is\n\
    forwarded through the host's NAT.\n\
\n\
FILES\n\
    /etc/resolv.conf\n"

#define VEDIT_PAGE "NAME\n\
    vedit - full-screen text editor\n\
\n\
SYNOPSIS\n\
    vedit\n\
\n\
DESCRIPTION\n\
    A text editor in the style of MS-DOS EDIT, text-mode only.\n\
    22x78 editing buffer; the buffer starts as a single blank\n\
    line and is not loaded from disk at startup.\n\
\n\
KEYS\n\
    Arrows / Home / End / Del / Backspace / Enter  edit text\n\
    Esc    enter the command line at the bottom\n\
\n\
    In command mode:\n\
      s [file]  save to a file\n\
      o [file]  load a file\n\
      q         quit (asks if modified; q! forces)\n\
      h         help\n\
      Enter / Esc  return to editing\n"

#define VC_PAGE "NAME\n\
    vibecommander - two-panel file manager (Norton Commander style)\n\
\n\
SYNOPSIS\n\
    vibecommander [DIRECTORY]\n\
\n\
DESCRIPTION\n\
    A text-mode file manager in the style of Norton Commander and\n\
    Midnight Commander. Two panels browse two directories side by\n\
    side; Tab picks which panel is active. Enter opens the entry\n\
    under the cursor (a directory changes the panel there, a\n\
    regular file opens in the built-in viewer), Left leaves to the\n\
    parent directory. A letter typed jumps to the first entry that\n\
    starts with it.\n\
\n\
    The function keys run the file operations, as on a Commander:\n\
\n\
      F1 help   F3 view   F4 edit   F5 copy\n\
      F6 move   F7 mkdir  F8 delete F10 quit\n\
\n\
    F4 edits in vedit, running it as a child process; the manager\n\
    returns when the editor exits. F5 and F6 copy and move the\n\
    selected file into the other panel's directory. F7 asks for a\n\
    name and creates the directory, F8 deletes the entry after a\n\
    yes/no confirmation.\n\
\n\
KEYS\n\
    Tab             switch the active panel\n\
    Enter / Right   open the entry under the cursor\n\
    Left            leave to the parent directory\n\
    Up / Down       move the cursor\n\
    Home / End      jump to the first / last entry\n\
    Space           page the list down\n\
    a..z 0..9 . _ - jump to the first entry starting with it\n\
    F1 F3 F4 F5 F6 F7 F8 F10   the operations above\n\
    Esc             quit\n\
\n\
    A screen layout from bottom to top: the function-key bar on the\n\
    last row, the status line with the selected entry's size above\n\
    it, the two lists, and the panel titles with their paths.\n"

#define CALC_PAGE "NAME\n\
    calc - graphical calculator\n\
\n\
SYNOPSIS\n\
    calc\n\
\n\
DESCRIPTION\n\
    A graphical keypad calculator for the VNU desktop. Arithmetic\n\
    follows immediate execution without operator precedence, so\n\
    2 + 3 * 4 is 20, not 14.\n\
\n\
KEYS\n\
    digits 0-9, . + - * / % and = operators, Backspace to clear\n\
    the last digit, Esc closes the window. Launched from the\n\
    desktop or as /apps/calc/bin; not reachable from the shell.\n"

#define STICKY_PAGE "NAME\n\
    sticky - a sticky note for the desktop\n\
\n\
SYNOPSIS\n\
    sticky\n\
\n\
DESCRIPTION\n\
    A window holding one note. Type into it and it is kept: the text is\n\
    written to /root/.sticky-note within a second of the last change, and\n\
    again when the window is closed, so opening the note again - or\n\
    restarting the whole desktop - gives back what was last written\n\
    rather than an empty page.\n\
\n\
    The status line names the file the note lives in and says whether it\n\
    has been written, so a note that is still only in the window cannot\n\
    be mistaken for one that is on disk.\n\
\n\
    The editor is the small one a note needs: printable keys, Enter,\n\
    Backspace, Delete, the arrows, Home and End. A row that reaches the\n\
    right margin does not wrap, so a long line is scrolled sideways to\n\
    keep the caret on screen, and a note longer than the window is\n\
    scrolled vertically to keep the row being edited in view.\n\
\n\
KEYS\n\
    printable keys  type into the note\n\
    Enter            start a new line\n\
    Backspace        delete backwards, joining lines at the start\n\
    Delete           delete forwards\n\
    Arrows, Home, End  move the caret\n\
    a click          put the caret where it landed\n\
    Esc              save if needed and close the window\n\
\n\
    The two buttons under the note save it at once and empty it.\n\
\n\
FILES\n\
    /root/.sticky-note  the note itself, rewritten as it changes\n\
\n\
    Like the rest of the RAM filesystem that file is gone at reboot,\n\
    where the desktop comes back with an empty note.\n\
"

#define FILES_PAGE "NAME\n\
    files - graphical file manager\n\
\n\
SYNOPSIS\n\
    files\n\
\n\
DESCRIPTION\n\
    A click-through directory browser over the in-memory VFS\n\
    (/, /bin, /apps, /dev, /proc, /home, /tmp, ...), in two views\n\
    toggled with v.\n\
\n\
    List view: each row shows a coloured type tile, the entry name\n\
    and, for files, the size: yellow tiles for directories, green\n\
    for files, magenta for the parent entry. The selected row is a\n\
    full-width light-blue bar.\n\
\n\
    Icon view: a grid of flat 32px icons — a yellow folder for\n\
    directories (magenta for the parent), a white document for\n\
    files, a picture-embossed document for images — with the name\n\
    underneath each and the selection as a light-blue cell highlight.\n\
\n\
KEYS\n\
    j/k      move down/up (list view)\n\
    h/l      move left/right between icons (icon view)\n\
    v        toggle between the list and icon views\n\
    Enter    open the selected directory, or a .png/.jpg/.jpeg\n\
             picture in picview (Esc hands the window back here)\n\
    Esc      close the window\n\
    Mouse: click to select, release on the same entry to open.\n\
\n\
    Drag-and-drop: press a file entry and drag the pointer away\n\
    (the document cursor follows) and release to drop it:\n\
\n\
      on another gfx window      that window gets the file (picview\n\
                                 opens it)\n\
      on an app icon             that app opens the file\n\
      on the bare desktop        the file moves to /root/desktop\n\
      back onto the files window the drag is cancelled\n\
\n\
    Esc while dragging cancels it too.\n\
    A GUI app (see man calc for how GUI apps are launched).\n"

#define PREFS_PAGE "NAME\n\
    prefs - graphical system preferences\n\
\n\
SYNOPSIS\n\
    prefs\n\
\n\
DESCRIPTION\n\
    A preferences window split into a pane selector (left)\n\
    and a content area (right):\n\
\n\
      About     OS identification (utsname, /proc/version)\n\
      Memory    live totals from /proc/meminfo and /proc/uptime\n\
      Mounts    mounted filesystems from /proc/mounts\n\
      CPU       /proc/cpuinfo snippet\n\
      Wallpaper the pictures in /etc/vnu/pics and the shipped one\n\
\n\
KEYS\n\
    j/k switch pane, Esc closes. Click a pane to select it.\n\
\n\
    In the Wallpaper pane the Up/Down arrows move the cursor over the\n\
    pictures (clicking one also) and Enter applies the selected one:\n\
    the kernel decodes it and the desktop redraws at once. A picture\n\
    it refuses leaves the desktop as it was and says why on the status\n\
    line. See man wallpaper for what may be used, and note that the\n\
    choice is forgotten at reboot.\n\
\n\
    F12 belongs to the desktop rather than to this window, so it works\n\
    with every pane and with no window at all: it steps the display\n\
    through 640x480, 800x600, 1024x768 and 1280x1024 while the desktop\n\
    keeps running, moving windows back inside the new screen. The mode\n\
    is recorded in /etc/vnuconfig/gfx.conf, so the next gui and the\n\
    next boot come up in it, and /proc/gfx reports the one in use.\n\
    Like everything else in the RAM filesystem, that file is gone at\n\
    reboot, so a booted ISO starts over from the built-in 1024x768.\n\
    A GUI app.\n"

#define WALLPAPER_PAGE "NAME\n\
    wallpaper - show or set the desktop background\n\
\n\
SYNOPSIS\n\
    wallpaper [option]... [file]\n\
 \n\
DESCRIPTION\n\
    The desktop background is a plain file, /etc/vnu/wallpaper, that\n\
    the kernel decodes and stretches behind the windows at startup.\n\
    This command reports which picture is in use, or replaces it.\n\
\n\
    With no argument, print the bare name of the background the\n\
    desktop is drawing, or `none` while it is still the procedural\n\
    sky. With a file argument, hand the path to the kernel\n\
    (VNU_SYS_wallpaper): the kernel reads the file, decodes it with\n\
    the same px.h decoder picview uses, quantises it to the 16\n\
    palette indices the DAC holds, makes it the background and\n\
    rewrites /etc/vnu/wallpaper with it, so the choice survives a\n\
    restart of the GUI. A candidate that fails to decode is refused\n\
    and changes nothing -- neither the file nor the screen.\n\
\n\
    BMP and PNG pictures of at most 512x384 pixels and 64 KiB are\n\
    accepted. JPEG is not: its IDCT is the one piece of float code\n\
    px.h has, and the kernel is built without a FPU. Such a file is\n\
    listed by --list and refused when chosen.\n\
\n\
    The picture lives in the RAM filesystem and is therefore\n\
    forgotten at reboot, where the kernel seeds the shipped desktop\n\
    back into place. That seed is /etc/vnu/wallpaper.default, which\n\
    this command never overwrites -- it is always there to fall back\n\
    on.\n\
\n\
    The same pictures can be picked from the desktop: open the\n\
    preferences window (prefs) and use its Wallpaper pane.\n\
\n\
OPTIONS\n\
    -l, --list     list the pictures that can be used, one per line,\n\
                   marking the JPEGs the kernel cannot decode, then\n\
                   the shipped desktop\n\
        --help     display this help and exit\n\
        --version  output version information and exit\n\
\n\
EXIT STATUS\n\
    0 on success, 1 on a missing file, an unusable picture, an unknown\n\
    option or an unreadable /proc/gfx.\n\
\n\
EXAMPLES\n\
    wallpaper -l\n\
        list the candidates\n\
    wallpaper /etc/vnu/pics/sunset.png\n\
        put the sunset on the desktop\n\
    wallpaper /etc/vnu/wallpaper.default\n\
        go back to the desktop VNU ships with\n\
\n\
FILES\n\
    /etc/vnu/wallpaper           the background in use\n\
    /etc/vnu/wallpaper.default   the shipped background\n\
    /etc/vnu/pics                the demo picture pack\n\
    /proc/gfx                    reports the background in use\n\
\n\
SYSCALLS\n\
    wallpaper   decode and adopt a background picture\n\
"

#define PICVIEW_PAGE "NAME\n\
    picview - graphical image viewer\n\
\n\
SYNOPSIS\n\
picview [file]\n\
 \n\
 DESCRIPTION\n\
    A tiny image viewer for the VNU desktop. Without an argument it\n\
    walks the /etc/vnu/pics directory; with a file argument (as the files\n\
    manager supplies when you open a .png/.jpg/.jpeg) it decodes and\n\
    shows just that file. Either way, BMP, PNG and JPEG are decoded\n\
    with the embedded px.h decoder and rendered into the window's\n\
    client area in the display's own colours. A display attached to a\n\
    virtio-gpu shows 24 bits per pixel and the picture is drawn as it\n\
    is; a VBE display has 16 colours behind a DAC, so the same picture\n\
    is quantised to the nearest of them (dithered unless `d` says not\n\
    to), which is the only thing about it that loses anything. The\n\
    program asks the display which case it is in rather than deciding.\n\
    A file dropped onto the window (drag it from the file manager)\n\
    switches it to that file, like a file argument would.\n\
\n\
KEYS\n\
    [ ]   previous / next picture (gallery mode only)\n\
    z     toggle zoom (fit window / 1:1)\n\
    d     toggle ordered dithering (a 16-colour display only; on a\n\
          24-bit one the picture is already finer than 16 steps)\n\
    Esc   close the window (opened from the file manager: return to it)\n\
\n\
FILES\n\
    /etc/vnu/pics   directory of embedded demo pictures\n\
    /apps/picview/bin   the program itself\n"

#define CLOCK_PAGE "NAME\n\
    clock - analog clock, stopwatch and countdown timer\n\
\n\
SYNOPSIS\n\
    clock\n\
\n\
DESCRIPTION\n\
    A graphical clock for the VNU desktop, rewritten from scratch (in\n\
    C) after the classic clock-tui: an analog face over the `time`\n\
    syscall (RTC, one-second resolution) with a digital read-out, a\n\
    stopwatch and a countdown timer. The desktop advances it once per\n\
    second through the GUI heartbeat; Esc closes the window.\n\
\n\
KEYS\n\
    1 2 3      switch mode: Clock / Stopwatch / Timer\n\
    Space      start or pause the stopwatch, start or pause the timer\n\
    r          reset the stopwatch or the timer\n\
    l          record a lap (stopwatch mode)\n\
    Esc        close the window\n\
\n\
MOUSE\n\
    Click the mode tabs to switch modes and the on-screen buttons to\n\
    start, pause, lap, reset or tweak the timer duration.\n\
\n\
SYSCALLS\n\
    time      seconds since midnight from the RTC (0..86399)\n\
    uptime    whole seconds since boot (RTC-delta)\n\
\n\
FILES\n\
    /apps/clock/bin   the program itself\n"

#define PLAY_PAGE "NAME\n\
    play - WAV sound player for the desktop\n\
\n\
SYNOPSIS\n\
    play\n\
\n\
DESCRIPTION\n\
    A graphical sound player for the VNU desktop: lists the WAV\n\
    clips in /etc/vnu/sounds, decodes the selected one with the embedded\n\
    libwav parser and streams the PCM through the kernel's AC'97\n\
    audio driver (audio_* syscalls). It is launched from the play\n\
    tile on the desktop; the window is 480x340 and lives on the\n\
    cooperative GUI scheduler, so audio_write is non-blocking and\n\
    the ring is topped up on every heartbeat.\n\
\n\
    The transport buttons and the space bar start (play), halt\n\
    (pause), resume and stop playback; the progress bar tracks the\n\
    play position via audio_pending() and the track's time is shown\n\
    next to it. Pause keeps the position; resume restarts from there,\n\
    stop rewinds to the beginning. The hardware plays 16-bit stereo,\n\
    so 8-bit and/or mono clips are converted by the kernel driver.\n\
\n\
KEYS\n\
    Up / Down   select a clip\n\
    Space       play / pause / resume\n\
    s           stop\n\
    Esc         close the window\n\
\n\
FILES\n\
    /etc/vnu/sounds     directory of embedded demo WAV clips\n\
    /dev/dsp            the sound card as a byte stream (16-bit stereo PCM;\n\
                        writes go straight to the AC'97 DMA ring)\n\
    /apps/play/bin      the program itself\n\
\n\
SYSCALLS\n\
    audio_open, audio_set_fmt, audio_write, audio_pending, audio_pause,\n\
    audio_reset, audio_close   the AC'97 playback channel\n"

#define HELLO_PAGE "NAME\n\
    hello - demonstration program\n\
\n\
SYNOPSIS\n\
    hello\n\
\n\
DESCRIPTION\n\
    Prints 'Hello from VNU userspace!' followed by its argument\n\
    vector, showing how the kernel passes argc/argv to an\n\
    embedded userspace binary. Mostly useful as a test.\n"

#define FORKDEMO_PAGE "NAME\n\
    forkdemo - what fork(2) gives a child, and what it does not\n\
\n\
SYNOPSIS\n\
    forkdemo\n\
\n\
DESCRIPTION\n\
    Forks once and reports what the two processes can see of each\n\
    other. The child is a second process in its own address space:\n\
    it runs while the parent runs, has its own pid, and resumes at\n\
    the instruction after fork() with a return value of 0. Memory is\n\
    not shared - the child writes to a variable in .data and the\n\
    parent never sees it. The filesystem, the console and every other\n\
    process are shared, and the parent reaps the child with\n\
    waitpid().\n\
\n\
    Useful as a smoke test for the process table and for fork()\n\
    itself: every line printed is a claim about what the kernel\n\
    guarantees.\n\
\n\
EXIT STATUS\n\
    0 if waitpid() returned the child's pid and its exit status 7,\n\
    1 if fork() failed or the child was not reaped as expected.\n\
\n\
SEE ALSO\n\
    ttytest(1), man(1)\n"

#define TTYTEST_PAGE "NAME\n\
    ttytest - POSIX compatibility smoke test\n\
\n\
SYNOPSIS\n\
    ttytest\n\
\n\
DESCRIPTION\n\
    Exercises isatty(), getpid(), getppid() and stat()'s file-\n\
    type bits against /dev and /proc and reports the results.\n\
    A quick way to sanity-check the terminal and VFS plumbing.\n"

#define TLSDEMO_PAGE "NAME\n\
    tlsdemo - TLS 1.2 client demo\n\
\n\
SYNOPSIS\n\
    tlsdemo IP PORT [TIME_MS] [SNI]\n\
\n\
DESCRIPTION\n\
    Connects to a TLS 1.2 server over the kernel's TCP socket\n\
    API and runs the vlibc TLS handshake (cipher suite\n\
    TLS_RSA_WITH_AES_128_GCM_SHA256) through the tls_stream\n\
    callbacks, then sends one HTTP GET and prints the response.\n\
    IP is a dotted quad; the default timeout is 15000 ms and the\n\
    default SNI name is 'localhost'. From inside QEMU the host is\n\
    reachable at 10.0.2.2 (slirp user networking).\n\
\n\
EXAMPLES\n\
    tlsdemo 10.0.2.2 14433\n\
    tlsdemo 127.0.0.1 7779\n\
\n\
SEE ALSO\n\
    ping(1), tlsserver, echoserver, man hosts\n"

#define ECHOSERVER_PAGE "NAME\n\
    echoserver - TCP echo server demo\n\
\n\
SYNOPSIS\n\
    echoserver [PORT]\n\
\n\
DESCRIPTION\n\
    Binds PORT (default 7778), enters listen mode and serves one\n\
    client at a time: every byte received on the connection is sent\n\
    straight back until the peer closes. Exercises the kernel's\n\
    bind/listen/accept server path end-to-end over the real NIC.\n\
\n\
    run.sh forwards host loopback port 17778 to the guest's 7778\n\
    (slirp hostfwd), so from the host shell 'nc 127.0.0.1 17778' (or\n\
    bash's /dev/tcp) can drive it; in the guest, run 'echoserver\n\
    7778' in the foreground first. The guest's own 10.0.2.15 is not\n\
    reachable from the host on a plain slirp user network. accept()\n\
    waits at most 20 s; recv()/send() 5 s per call.\n\
\n\
EXIT STATUS\n\
    0 after the listener is closed, 1 on socket/bind/listen failure,\n\
    2 on a bad PORT argument.\n\
\n\
EXAMPLES\n\
    echoserver\n\
    echoserver 9999\n\
\n\
SEE ALSO\n\
    ping(1), tlsdemo, tlsserver, echoserver, man hosts\n"

#define TLSSERVER_PAGE "NAME\n\
    tlsserver - TLS 1.2 echo server demo\n\
\n\
SYNOPSIS\n\
    tlsserver [PORT]\n\
\n\
DESCRIPTION\n\
    Binds PORT (default 7779), listens and serves one client at a\n\
    time: runs the vlibc TLS handshake (cipher suite\n\
    TLS_RSA_WITH_AES_128_GCM_SHA256) through the tls_stream\n\
    callbacks and echoes every byte received straight back until the\n\
    peer closes. The certificate and RSA private key are the fixed\n\
    demo keypair in tlsserver_key.h: self-signed and public, tests\n\
    only, never for real traffic.\n\
\n\
    The kernel loopbacks 127.0.0.1, so inside the guest\n\
    'tlsserver 7779' + 'tlsdemo 127.0.0.1 7779' runs the vlibc\n\
    client against the vlibc server with no NIC involved. From the\n\
    host, run.sh forwards guest port 7779 to host loopback 17779, so\n\
    'printf ping\\n | openssl s_client -quiet -tls1_2 -cipher\n\
    AES128-GCM-SHA256 -connect 127.0.0.1:17779' drives it over the\n\
    real NIC (slirp hostfwd). accept() waits at most 20 s, the\n\
    handshake 15 s and recv()/send() 5 s per call.\n\
\n\
EXIT STATUS\n\
    0 after the listener is closed; 1 on socket/bind/listen or\n\
    keypair failure; 2 on a bad PORT argument.\n\
\n\
EXAMPLES\n\
    tlsserver\n\
    tlsserver 9999\n\
\n\
SEE ALSO\n\
    ping(1), tlsdemo, echoserver, man hosts\n"

#define SH_PAGE "NAME\n\
    sh - the standard shell (synonym for vash)\n\
\n\
SYNOPSIS\n\
    sh\n\
\n\
DESCRIPTION\n\
    /bin/sh is the same binary as vash; the VFS and the embedded\n\
    program table only know it as a login shell path. See man vash.\n"

#define INIT_PAGE "NAME\n\
    init - the first process (synonym for vash)\n\
\n\
SYNOPSIS\n\
    init\n\
\n\
DESCRIPTION\n\
    /sbin/init is the same binary as vash, the only process the\n\
    kernel starts at boot. See man vash.\n"

#define TERM_PAGE "NAME\n\
    term - the desktop terminal app (synonym for vash)\n\
\n\
SYNOPSIS\n\
    term\n\
\n\
DESCRIPTION\n\
    The GUI launcher ships vash under the name 'term'; a login\n\
    happens inside the window just like on the console. See\n\
    man vash.\n"

/* --- the database ------------------------------------------------ */

static const char* const aliases[] = { "a", "all", "list", "-l", "--list", NULL };

static const struct Page pages[] = {
    {"man",      "display reference manual pages",
     MAN_PAGE},
    {"vcc",      "the VNU C compiler",
     VCC_PAGE},
    {"vnu",     "report what system you are running on",
     VNU_PAGE},
    {"vash",     "VNU login shell",
     VASH_PAGE},
    {"sh",       "the standard shell (synonym for vash)",
     SH_PAGE},
    {"init",     "the first process at boot (synonym for vash)",
     INIT_PAGE},
    {"term",     "desktop terminal app (synonym for vash)",
     TERM_PAGE},
    {"cd",       "change the working directory",
     CD_PAGE},
    {"export",   "print or set the PATH search path",
     EXPORT_PAGE},
    {"unset",    "empty (unset) the command search path",
     UNSET_PAGE},
    {"exit",     "end the shell session",
     EXIT_PAGE},
    {"help",     "list the shell builtins",
     HELP_PAGE},
    {"type",     "say how a name would be interpreted",
     TYPE_PAGE},
    {"which",    "report where a command lives",
     WHICH_PAGE},
    {"hostname", "the name of this machine (/etc/hostname)",
     HOSTNAME_PAGE},
    {"id",       "print the current identity",
     ID_PAGE},
    {"whoami",   "print the current user name",
     WHOAMI_PAGE},
    {"groups",   "print the current group name",
     GROUPS_PAGE},
    {"useradd",  "add a new user account",
     USERADD_PAGE},
    {"passwd",   "change a user's password",
     PASSWD_PAGE},
    {"su",       "switch the session to another user",
     SU_PAGE},
    {"coreutils", "the standard command set (index)",
     COREUTILS_PAGE},
    {"echo",     "write its arguments to standard output",
     ECHO_PAGE},
    {"true",     "do nothing, successfully",
     TRUE_PAGE},
    {"false",    "do nothing, unsuccessfully",
     FALSE_PAGE},
    {"pwd",      "print the working directory",
     PWD_PAGE},
    {"cat",      "concatenate files and print",
     CAT_PAGE},
    {"ls",       "list directory contents",
     LS_PAGE},
    {"mkdir",    "create a directory",
     MKDIR_PAGE},
    {"rm",       "remove files or directories",
     RM_PAGE},
    {"touch",    "create an empty file",
     TOUCH_PAGE},
    {"uname",    "print system information",
     UNAME_PAGE},
    {"clear",    "clear the terminal screen",
     CLEAR_PAGE},
    {"wc",       "count lines, words and bytes",
     WC_PAGE},
    {"head",     "print the first lines of a file",
     HEAD_PAGE},
    {"tail",     "print the last lines of a file",
     TAIL_PAGE},
    {"grep",     "print lines matching a pattern",
     GREP_PAGE},
    {"sort",     "sort lines of a file",
     SORT_PAGE},
    {"cp",       "copy a file",
     CP_PAGE},
    {"mv",       "move (rename) a file",
     MV_PAGE},
    {"basename", "strip the directory part of a path",
     BASENAME_PAGE},
    {"dirname",  "strip the final component of a path",
     DIRNAME_PAGE},
    {"seq",      "print a sequence of numbers",
     SEQ_PAGE},
{"df",       "report file system space usage",
     DF_PAGE},
    {"ping",     "send ICMP echo requests (network test)",
      PING_PAGE},
    {"hosts",    "static host name to IP mapping",
      HOSTS_PAGE},
    {"resolv.conf", "DNS server configuration",
      RESOLV_PAGE},
    {"vedit",    "full-screen text editor",
     VEDIT_PAGE},
    {"vibecommander", "two-panel file manager",
     VC_PAGE},
    {"calc",     "graphical calculator",
     CALC_PAGE},
    {"files",    "graphical file manager",
     FILES_PAGE},
    {"prefs",    "graphical system preferences",
     PREFS_PAGE},
    {"picview",  "graphical image viewer",
     PICVIEW_PAGE},
    {"wallpaper", "show or set the desktop background",
     WALLPAPER_PAGE},
    {"clock",    "analog clock, stopwatch and timer",
     CLOCK_PAGE},
    {"play",     "WAV sound player for the desktop",
     PLAY_PAGE},
    {"sticky",   "a sticky note for the desktop",
     STICKY_PAGE},
    {"hello",    "demonstration program",
     HELLO_PAGE},
    {"ttytest",  "POSIX compatibility smoke test",
     TTYTEST_PAGE},
    {"forkdemo", "fork(2) demonstration program",
     FORKDEMO_PAGE},
    {"tlsdemo",  "TLS 1.2 client demo",
     TLSDEMO_PAGE},
    {"echoserver", "TCP echo server demo",
     ECHOSERVER_PAGE},
    {"tlsserver",  "TLS 1.2 echo server demo",
     TLSSERVER_PAGE},
};

static int is_alias(const char* a)
{
    for (int i = 0; aliases[i]; ++i)
        if (strcmp(a, aliases[i]) == 0)
            return 1;
    return 0;
}

static void usage(void)
{
    w("usage: man [NAME...]\n");
    w("       man -l | --list      list all documented commands\n");
    w("       man -h | --help      this help\n");
    w("\nOn a terminal a page is paged: Up/Down scroll, h lists the\n");
    w("keys, q quits. Redirected output is printed as is.\n");
}

static void list_all(void)
{
    w("VNU manual pages:\n\n");
    for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        w("    ");
        w(pages[i].name);
        w("  -  ");
        w(pages[i].desc);
        w("\n");
    }
    w("\nType 'man NAME' for the page of NAME.\n");
}

static const struct Page* find_page(const char* name)
{
    for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i)
        if (strcmp(pages[i].name, name) == 0)
            return &pages[i];
    return 0;
}

/* --- the pager --------------------------------------------------------
 *
 * The page text is a string literal inside this ELF, so the document is
 * indexed in place instead of copied: a display line is a (pointer,
 * length) pair into that text, cut at every '\n' and wrapped at the
 * screen width, so nothing is ever truncated.
 */

struct Line {
    const char* p;
    unsigned len;
    unsigned page; /* which requested page the line came from */
};

static const char* g_names[32]; /* page names, in the order given */
static unsigned g_npages;
static struct Line* g_lines;
static unsigned g_nlines, g_capacity;
static unsigned g_top; /* first display line on screen */
static unsigned g_rows = TNU_ROWS, g_cols = TNU_COLS;
static char g_pad[TNU_COLS + 8]; /* spaces, to fill the status bar */

/* The three fixed pieces of the status line, kept as strings so the
 * padding arithmetic below cannot drift from what is printed. */
static const char* const S_PRE = "Manual page ";
static const char* const S_MID = "(1) line ";
static const char* const S_POST = " (press h for help or q to quit)";

static void seq_goto(unsigned row, unsigned col)
{
    char q[24];
    int n = tnu_goto(q, sizeof q, row, col);
    if (n > 0)
        write(1, q, (unsigned long)n);
}

static void seq_sgr(const char* params)
{
    char q[16];
    int n = tnu_sgr(q, sizeof q, params);
    if (n > 0)
        write(1, q, (unsigned long)n);
}

static void seq_erase_eol(void)
{
    char q[8];
    int n = tnu_erase_eol(q, sizeof q);
    if (n > 0)
        write(1, q, (unsigned long)n);
}

static unsigned ndigits(unsigned v)
{
    unsigned n = 1;
    while (v >= 10) {
        v /= 10;
        ++n;
    }
    return n;
}

static int doc_reserve(unsigned extra)
{
    unsigned need = g_nlines + extra;
    if (need <= g_capacity)
        return 0;
    unsigned cap = g_capacity ? g_capacity : 256;
    while (cap < need)
        cap *= 2;
    struct Line* nl = (struct Line*)malloc(sizeof(struct Line) * cap);
    if (!nl)
        return -1;
    if (g_lines) {
        memcpy(nl, g_lines, sizeof(struct Line) * g_nlines);
        free(g_lines);
    }
    g_lines = nl;
    g_capacity = cap;
    return 0;
}

/* Cut `text` into display lines and append them to the document. */
static int doc_add(const char* text, unsigned page)
{
    const char* cur = text;
    for (;;) {
        const char* nl = cur;
        while (*nl != 0 && *nl != '\n')
            ++nl;
        unsigned len = (unsigned)(nl - cur);
        unsigned off = 0;
        do {
            unsigned take = len - off;
            if (take > g_cols)
                take = g_cols;
            if (doc_reserve(1) != 0)
                return -1;
            g_lines[g_nlines].p = cur + off;
            g_lines[g_nlines].len = take;
            g_lines[g_nlines].page = page;
            ++g_nlines;
            off += take;
        } while (off < len);
        if (*nl == 0)
            break;
        cur = nl + 1;
    }
    return 0;
}

static void draw_page(unsigned view)
{
    for (unsigned r = 0; r < view; ++r) {
        seq_goto(r + 1, 1);
        unsigned i = g_top + r;
        if (i < g_nlines) {
            write(1, g_lines[i].p, g_lines[i].len);
            seq_erase_eol();
        } else {
            /* Past the end of the document: leave the row blank. */
            seq_sgr("0");
            seq_erase_eol();
        }
    }
}

static const char* const HELP[] = {
    "man - how to move around a page",
    "",
    "  Up, k            scroll up one line",
    "  Down, j          scroll down one line",
    "  space, f, Enter  scroll down one screen",
    "  b, u             scroll up one screen",
    "  Home, g          first line of the page",
    "  End, G           last line of the page",
    "  h                this help",
    "  q                quit, back to the shell",
    "",
    "The arrow keys move one line; the rest are shortcuts. q and h also",
    "work on this screen - any other key returns to the page.",
    0
};

static void draw_help(unsigned view)
{
    unsigned r = 0;
    for (; r < view && HELP[r] != 0; ++r) {
        seq_goto(r + 1, 1);
        write(1, HELP[r], strlen(HELP[r]));
        seq_erase_eol();
    }
    for (; r < view; ++r) {
        seq_goto(r + 1, 1);
        seq_sgr("0");
        seq_erase_eol();
    }
}

/* The white bar on the last row: which page, which line, what to press.
 * The cursor is parked on the last cell, since the console has no
 * hide-cursor sequence. */
static void draw_status(void)
{
    unsigned first = g_top + 1;
    unsigned name = 0;
    if (g_nlines > 0 && g_top < g_nlines && g_names[0] != 0)
        name = g_lines[g_top].page;
    const char* nm = g_names[name] ? g_names[name] : "?";

    seq_goto(g_rows, 1);
    seq_sgr("0;30;47");
    printf("%s%s%s%u%s", S_PRE, nm, S_MID, first, S_POST);
    unsigned used = (unsigned)(strlen(S_PRE) + strlen(nm) + strlen(S_MID) +
                               ndigits(first) + strlen(S_POST));
    unsigned fill = used + 1 < g_cols ? g_cols - used - 1 : 0;
    if (fill > sizeof g_pad)
        fill = sizeof g_pad;
    if (fill > 0)
        write(1, g_pad, (unsigned long)fill);
    seq_goto(g_rows, g_cols);
    seq_sgr("0");
}

static void draw(int help)
{
    unsigned view = g_rows - 1; /* the last row belongs to the status bar */
    seq_sgr("0");
    if (help)
        draw_help(view);
    else
        draw_page(view);
    draw_status();
}

static void run_pager(void)
{
    int help = 0;

    tnu_size(&g_rows, &g_cols);
    if (g_rows < 8) /* room for a status bar plus a little text */
        g_rows = 8;
    memset(g_pad, ' ', sizeof g_pad);
    /* Scrolling never goes past the end: the last screen of the document
     * sits at the bottom of the window, like less does. */
    unsigned view = g_rows - 1;
    unsigned max_top = g_nlines > view ? g_nlines - view : 0;

    draw(help);
    for (;;) {
        char raw = 0;
        if (read(0, &raw, 1) <= 0)
            break;
        unsigned ch = (unsigned char)raw;

        if (ch == VNU_KEY_INTR || ch == VNU_KEY_ESC || ch == 'Q')
            break;

        if (help) {
            if (ch == 'q' || ch == 'Q' || ch == 'h' || ch == 'H')
                break;
            help = 0; /* any other key: back to the page */
            draw(help);
            continue;
        }
        if (ch == 'q')
            break;
        if (ch == 'h' || ch == 'H') {
            help = 1;
            draw(help);
            continue;
        }

        if (ch == VNU_KEY_UP || ch == 'k') {
            if (g_top > 0)
                --g_top;
        } else if (ch == VNU_KEY_DOWN || ch == 'j') {
            if (g_top < max_top)
                ++g_top;
        } else if (ch == ' ' || ch == '\n' || ch == 'f' || ch == 6 /* ^F */ ||
                   ch == 4 /* ^D */) {
            g_top += view;
            if (g_top > max_top)
                g_top = max_top;
        } else if (ch == 'b' || ch == 'u' || ch == 2 /* ^B */) {
            g_top = g_top > view ? g_top - view : 0;
        } else if (ch == VNU_KEY_HOME || ch == 'g') {
            g_top = 0;
        } else if (ch == VNU_KEY_END || ch == 'G') {
            g_top = max_top;
        } else {
            continue; /* an unbound key changes nothing */
        }
        draw(help);
    }
    /* Leave the shell a clean prompt: the newline scrolls the bar off the
     * bottom and the cursor lands where vash continues. */
    seq_sgr("0");
    write(1, "\n", 1);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        list_all();
        return 0;
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }
    if (is_alias(argv[1])) {
        list_all();
        return 0;
    }
    int rc = 0;
    /* On a terminal the pages are shown one screen at a time; otherwise
     * they are printed straight out, so a redirect keeps working. */
    int interactive = isatty(1) == 1;
    for (int i = 1; i < argc; ++i) {
        const struct Page* page = find_page(argv[i]);
        if (page == 0) {
            we("man: no manual page for ");
            we(argv[i]);
            we("\n");
            rc = 1;
            continue;
        }
        if (interactive) {
            /* One document holds every page asked for, so the status bar
             * can name the one on screen; the array of names is the cap. */
            if (g_npages >= sizeof(g_names) / sizeof(g_names[0])) {
                we("man: too many pages requested\n");
                return 1;
            }
            g_names[g_npages++] = page->name;
            if (doc_add(page->text, g_npages - 1) != 0) {
                we("man: out of memory building the page\n");
                return 1;
            }
        } else {
            w(page->text);
            w("\n");
        }
    }
    if (interactive && g_npages > 0)
        run_pager();
    return rc;
}
