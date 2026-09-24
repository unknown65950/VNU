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
EXAMPLES\n\
    man ls          page for the ls command\n\
    man useradd id  two pages at once\n\
    man             full listing of documented commands\n\
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
    user@vnu:cwd$ and reads commands line by line. Words are\n\
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

#define INSTALL_PAGE "NAME\n\
    install - install the VNU system image onto a disk\n\
\n\
SYNOPSIS\n\
    install [DRIVE [SIZE_MIB]]\n\
\n\
DESCRIPTION\n\
    With no arguments, install launches a full-screen interactive wizard\n\
    in the style of the classic Windows Setup / FreeBSD sysinstall /\n\
    Slackware setup screens. It lets you pick the target disk (from\n\
    /proc/disks), choose the partition size in MiB, review the chosen\n\
    parameters, confirm the destructive write and finally reboot. Blue\n\
    screens use the console's ANSI color support.\n\
\n\
    With arguments, install runs non-interactively: DRIVE is the disk\n\
    index (0..N-1, see VNU_SYS_blkcount or /proc/disks) and SIZE_MIB is\n\
    the partition size in MiB (0 or omitted = the whole usable disk).\n\
\n\
    The write itself is performed by the kernel (syscall VNU_SYS_install,\n\
    30): it stamps the MBR + GRUB core image and creates a FAT16\n\
    partition at LBA 2048 holding /boot/kernel.elf and a GRUB config so\n\
    the disk boots standalone. This is what a distro 'sysinst' would be.\n\
\n\
    Requires root. After a successful write, reboot from the target\n\
    disk (with no arguments the wizard offers to reboot for you).\n\
\n\
    Standalone binary /bin/install.\n"

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
    The separate account/install tools id, whoami, groups,\n\
    useradd, passwd, su and install are standalone /bin binaries\n\
    too — see also the VNU manual's list.\n"

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
    -n, --nodename       network hostname\n\
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

#define FILES_PAGE "NAME\n\
    files - graphical file manager\n\
\n\
SYNOPSIS\n\
    files\n\
\n\
DESCRIPTION\n\
    A click-through directory browser over the in-memory VFS\n\
    (/, /bin, /apps, /dev, /proc, /home, /tmp, ...). Each row\n\
    shows a coloured type tile, the entry name and, for files, the\n\
    size: yellow tiles for directories, green for files, magenta\n\
    for the parent entry. The selected row is a full-width\n\
    light-blue bar.\n\
\n\
KEYS\n\
    j/k      move up and down\n\
    Enter    open the selected directory\n\
    Esc      close the window\n\
    Mouse: click to select, release on the same row to open.\n\
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
      About   OS identification (utsname, /proc/version)\n\
      Memory  live totals from /proc/meminfo and /proc/uptime\n\
      Mounts  mounted filesystems from /proc/mounts\n\
      CPU     /proc/cpuinfo snippet\n\
\n\
KEYS\n\
    j/k switch pane, Esc closes. Click a pane to select it.\n\
    A GUI app.\n"

#define PICVIEW_PAGE "NAME\n\
    picview - graphical image viewer\n\
\n\
SYNOPSIS\n\
    picview\n\
\n\
DESCRIPTION\n\
    A tiny image viewer for the VNU desktop. Walks the /pics\n\
    directory, decodes each BMP, PNG or JPEG with the embedded\n\
    px.h decoder and renders it, colour-quantised to the 16\n\
    index Catppuccin palette, into the window's client area.\n\
\n\
KEYS\n\
    [ ]   previous / next picture\n\
    z     toggle zoom (fit window / 1:1)\n\
    d     toggle ordered dithering\n\
    Esc   close the window\n\
\n\
FILES\n\
    /pics   directory of embedded demo pictures\n\
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
\n\
SEE ALSO\n\
    ping(1), vprobe, man hosts\n"

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
    {"install",  "write the VNU system image to a disk",
     INSTALL_PAGE},
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
    {"calc",     "graphical calculator",
     CALC_PAGE},
    {"files",    "graphical file manager",
     FILES_PAGE},
    {"prefs",    "graphical system preferences",
     PREFS_PAGE},
    {"picview",  "graphical image viewer",
     PICVIEW_PAGE},
    {"clock",    "analog clock, stopwatch and timer",
     CLOCK_PAGE},
    {"hello",    "demonstration program",
     HELLO_PAGE},
    {"ttytest",  "POSIX compatibility smoke test",
     TTYTEST_PAGE},
    {"tlsdemo",  "TLS 1.2 client demo",
     TLSDEMO_PAGE},
    {"echoserver", "TCP echo server demo",
     ECHOSERVER_PAGE},
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

static int show_page(const char* name)
{
    for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i)
        if (strcmp(pages[i].name, name) == 0) {
            w(pages[i].text);
            w("\n");
            return 0;
        }
    we("man: no manual page for ");
    we(name);
    we("\n");
    return 1;
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
    for (int i = 1; i < argc; ++i)
        if (show_page(argv[i]) != 0)
            rc = 1;
    return rc;
}