#pragma once

/* uint32_t, for the structs below. Freestanding clang and the vlibc
 * build both have it; the kernel's own headers include it the same way
 * (see vnu/pmm.h). */
#include <stdint.h>

#define VNU_ABI_VERSION 1

#define VNU_SYS_read      0
#define VNU_SYS_write     1
#define VNU_SYS_open      2
#define VNU_SYS_close     3
#define VNU_SYS_exit      4
#define VNU_SYS_lseek     5
#define VNU_SYS_stat      6
#define VNU_SYS_fstat     7
#define VNU_SYS_brk       8
#define VNU_SYS_getpid    9
#define VNU_SYS_chdir    10
#define VNU_SYS_getcwd   11
#define VNU_SYS_fork     12
#define VNU_SYS_execve   13
#define VNU_SYS_getuid   14
#define VNU_SYS_getgid   15
#define VNU_SYS_mkdir    16
#define VNU_SYS_rmdir    17
#define VNU_SYS_unlink   18
#define VNU_SYS_getdents 19
#define VNU_SYS_waitpid  20
#define VNU_SYS_pipe     21
#define VNU_SYS_dup      22
#define VNU_SYS_dup2     23
#define VNU_SYS_kill     24
#define VNU_SYS_uname    25
#define VNU_SYS_getppid  26
#define VNU_SYS_isatty   27
#define VNU_SYS_spawn    28
#define VNU_SYS_blkcount 29
#define VNU_SYS_install  30
#define VNU_SYS_setuid   31
#define VNU_SYS_setgid   32
#define VNU_SYS_chmod    33
#define VNU_SYS_chown    34
#define VNU_SYS_reboot   35
#define VNU_SYS_time     36
#define VNU_SYS_uptime   37
#define VNU_SYS_ping     38
#define VNU_SYS_netinfo  39
#define VNU_SYS_resolve  40
#define VNU_SYS_socket   41
#define VNU_SYS_connect  42
#define VNU_SYS_send     43
#define VNU_SYS_recv     44
#define VNU_SYS_netclose 45
#define VNU_SYS_bind     46
#define VNU_SYS_listen   47
#define VNU_SYS_accept   48
#define VNU_SYS_audio_open    49
#define VNU_SYS_audio_set_fmt 50
#define VNU_SYS_audio_write   51
#define VNU_SYS_audio_drain   52
#define VNU_SYS_audio_close   53
#define VNU_SYS_audio_pending 54
#define VNU_SYS_audio_pause   55
#define VNU_SYS_audio_reset   56
#define VNU_SYS_dnd_declare  57
#define VNU_SYS_sleep    58
#define VNU_SYS_tty_size 59
#define VNU_SYS_wallpaper 60
#define VNU_SYS_gfx_surface 61
#define VNU_SYS_gfx_setmode 62
#define VNU_SYS_gfx_getmode 63
#define VNU_SYS_gfx_getinfo 64
#define VNU_SYS_gfx_palette 65
#define VNU_SYS_rt_sigaction  66
#define VNU_SYS_rt_sigprocmask 67
#define VNU_SYS_rt_sigreturn 68
#define VNU_SYS_alarm        69
#define VNU_SYS_sigraise     70
#define VNU_SYS_sigpending   71
#define VNU_SYS_gfx_canvas   72
#define VNU_SYS_times        73
#define VNU_SYS_getrusage    74
#define VNU_SYS_quota        75

/* Out-parameter of VNU_SYS_times. All four fields are in units of the
 * scheduler's 100 Hz tick (CLK_TCK 100); the syscall's return value is
 * the tick count since boot. The kernel runs every process at privilege
 * 0 (one address space, per-process page tables) and samples no
 * privilege level, so user and system time cannot be separated the way
 * Linux separates them - every tick a process runs is charged to
 * utime, and stime/cstime are always 0 (the same reason /proc/stat
 * reports all busy jiffies in its "system" slot). */
struct vnu_tms {
    int32_t utime;  /* ticks this process ran (all CPU time; see above) */
    int32_t stime;  /* always 0: no user/system split */
    int32_t cutime; /* utime of waited-for children, accumulated at exit */
    int32_t cstime; /* always 0 */
};

/* Out-parameter of VNU_SYS_getrusage (ebx = who: 0 for RUSAGE_SELF,
 * -1 for RUSAGE_CHILDREN; ecx = pointer). Layout mirrors the i386
 * struct rusage: two timevals followed by fourteen longs. VNU measures
 * CPU time (charged to ru_utime for the same no-split reason as
 * vnu_tms) and the two context-switch counters; everything else is
 * reported as 0 rather than guessed, documented per field. */
struct vnu_timeval {
    int32_t tv_sec;
    int32_t tv_usec;
};

struct vnu_rusage {
    struct vnu_timeval ru_utime; /* CPU time (all of it; stime stays 0) */
    struct vnu_timeval ru_stime; /* always 0 */
    int32_t ru_maxrss;  /* 0: no resident-set accounting */
    int32_t ru_ixrss;   /* 0 */
    int32_t ru_idrss;   /* 0 */
    int32_t ru_isrss;   /* 0 */
    int32_t ru_minflt;  /* 0: the kernel allocates eagerly, so minor
                         * faults do not exist */
    int32_t ru_majflt;  /* 0 */
    int32_t ru_nswap;   /* 0 */
    int32_t ru_inblock; /* 0: no block-I/O accounting */
    int32_t ru_oublock; /* 0 */
    int32_t ru_msgsnd;  /* 0: no SysV IPC */
    int32_t ru_msgrcv;  /* 0 */
    int32_t ru_nsignals;/* 0: not counted */
    int32_t ru_nvcsw;   /* voluntary switches: cooperative yields (a
                         * process blocking on a wait, a pipe, a read
                         * or a sleep gives the CPU up itself) */
    int32_t ru_nivcsw;  /* involuntary switches: timer preemptions */
};

/* VNU_SYS_quota: how much room a user's files take and how much they
 * may take. The unit is a byte of a regular file's content: `used' is
 * the sum of the sizes of every regular file in the VFS owned by `uid',
 * and `limit' is a hard ceiling on that sum, 0 meaning no limit. The
 * write() path charges a growing write to the file's owner and refuses
 * the whole write with -VNU_EDQUOT when it would cross the limit (a
 * quota is a hard limit, not a warning).
 *
 * The kernel keeps a small table of per-user limits, so a user with no
 * entry of their own is held to a default; GET/SET act on `uid', and
 * the GET_DEFAULT/SET_DEFAULT commands read and write that fallback.
 * A program may read any user's numbers; changing them is root's.
 *
 * In-parameter and result of one syscall (ebx selects the command,
 * ecx points here): plain uint32 fields, the same struct on both sides
 * of the boundary. GET_DEFAULT/SET_DEFAULT ignore `uid' and `used'. */
struct vnu_quota {
    uint32_t uid;   /* in:  the user GET/SET is about */
    uint32_t used;  /* out: bytes that user's files hold (GET) */
    uint32_t limit; /* in/out: hard limit in bytes, 0 = unlimited */
};
#define VNU_QUOTA_GET         0
#define VNU_QUOTA_SET         1 /* uid + limit in; root only */
#define VNU_QUOTA_GET_DEFAULT 2
#define VNU_QUOTA_SET_DEFAULT 3 /* limit in; root only */

/* Which display driver is in use, as vnu_gfx_info::driver. */
#define VNU_GFX_DRIVER_VGA        0 /* Bochs VBE, 8bpp indexed */
#define VNU_GFX_DRIVER_VIRTIO_GPU 1 /* virtio-gpu, 32bpp B8G8R8X8 scanout */

/* Out-parameter of VNU_SYS_gfx_getinfo: what the display is, in one
 * read. Plain uint32 fields, so it is the same struct on both sides of
 * the boundary with no packing to get wrong. */
struct vnu_gfx_info {
    uint32_t width;  /* pixels across the mode in use */
    uint32_t height;
    uint32_t bpp;    /* bits per pixel a program draws in: 8 or 32 */
    uint32_t driver; /* VNU_GFX_DRIVER_* */
};

/* Bounds a gfx app may ask its canvas to be. The low end is the window's
 * own minimum client area (gui.cpp); the high end is the mode, applied
 * by the kernel when a request is granted. */
#define VNU_GFX_CANVAS_MIN_W 160
#define VNU_GFX_CANVAS_MIN_H 112

/* In-parameter and result of VNU_SYS_gfx_canvas: one struct for "let me
 * have this size" and "here is the size you actually got", so the whole
 * negotiation is a single read-modify-write and a program learns the
 * granted size without a second syscall.
 *
 * want_w/want_h are the size the app would like its canvas to be, in
 * pixels; 0 in either means "leave that axis alone". The kernel clamps
 * to the window's minimum, the mode and what the page pool can back, so
 * a request is a request, not a command - and the size that comes back
 * is the one to draw at, which may be smaller than asked for.
 *
 * width/height/bpp are the granted canvas, always written. The pages
 * are shared with the compositor at the same VA gfx_surface() returns,
 * so nothing moves when the size changes: the app clears and redraws
 * the new rectangle at the same address. */
struct vnu_gfx_canvas {
    uint32_t want_w;  /* in:  desired width, 0 = keep */
    uint32_t want_h;  /* in:  desired height, 0 = keep */
    uint32_t width;   /* out: granted width in pixels */
    uint32_t height;  /* out: granted height in pixels */
    uint32_t bpp;     /* out: bits per pixel of the canvas now */
    uint32_t granted; /* out: 1 = want_w/want_h met, 0 = clamped */
};

/* /dev/fb0: where a program draws on the display itself.
 *
 * There is no syscall for this. The device maps the display's frame into
 * the caller's address space when it is opened, and a read answers with
 * this struct: open("/dev/fb0"), read(fd, &fb, sizeof(fb)), draw into
 * fb.va, write(fd, 0, 0) to put it on the screen. The address is fixed
 * so a program compiled ahead of the run can name it.
 *
 * Above the 32 MiB identity map on purpose: that region is the kernel
 * image, .bss and the frame pool, and a program's private mapping must
 * not land on any of it. The window is VNU_FB_MAX_PAGES pages, which
 * covers the top mode (1280x1024 at 32bpp is 5 MiB) with room to spare.
 */
#define VNU_FB_VA 0x02000000u
#define VNU_FB_MAX_PAGES 2048u

/* One read() on /dev/fb0. Plain uint32 fields, so it is the same struct
 * on both sides of the boundary with no packing to get wrong.
 *
 * A pixel is one byte at 8 bpp (a palette index, the desktop's own DAC)
 * and a B8G8R8X8 word at 32 bpp - blue, green, red, then a byte the
 * compositor reads as coverage, 0xFF for opaque. va is VNU_FB_VA mapped;
 * size is stride * height, the whole frame. */
struct vnu_fb_info {
    uint32_t va;     /* where the frame is in this process */
    uint32_t width;
    uint32_t height;
    uint32_t bpp;    /* 8 or 32 */
    uint32_t stride; /* bytes per row: width * bpp / 8 */
    uint32_t size;   /* bytes in the whole frame */
};

/* How many slots VNU_SYS_gfx_palette answers, and how many bytes a
 * program has to offer for it: the 256 entries of the display palette
 * as the same B8G8R8X8 words the frame itself holds (0xXXRRGGBB,
 * alpha in the top byte). The first 16 are the system's own colours -
 * the ones the desktop and the console draw in, and the ones a
 * program's VGFX_* constants mean - and the 240 after them belong to
 * whatever picture is on screen (the wallpaper's), so a program can
 * index the exact colour a pixel of it holds. */
#define VNU_GFX_PALETTE_SLOTS 256
#define VNU_GFX_PALETTE_BYTES 1024 /* SLOTS * sizeof(uint32_t) */

/* --- signals -----------------------------------------------------------
 * A signal is a number, a handler is an address, and a set of signals
 * is one 32-bit word with a bit per signal (bit n is signal n, so
 * signal 0 is the empty set and is never a signal at all). */
#define VNU_NSIG 32

/* The POSIX numbers, for the signals VNU has an answer for, so a
 * program written against a real system means the same thing here.
 * The signals POSIX also has and VNU does not (SIGSTOP, SIGCONT, the
 * job-control trio) are deliberately left undefined rather than
 * defined and ignored. */
#define VNU_SIGHUP  1  /* the console went away */
#define VNU_SIGINT  2  /* ^C at the console */
#define VNU_SIGQUIT 3
#define VNU_SIGILL  4
#define VNU_SIGTRAP 5
#define VNU_SIGABRT 6
#define VNU_SIGFPE  8
#define VNU_SIGKILL 9  /* cannot be caught, blocked or ignored */
#define VNU_SIGUSR1 10
#define VNU_SIGSEGV 11
#define VNU_SIGUSR2 12
#define VNU_SIGPIPE 13 /* reserved: defined so a program that names it
                           * compiles, but nothing raises it yet - a
                           * write with no reader fails with EPIPE */
#define VNU_SIGALRM 14 /* alarm() ran out */
#define VNU_SIGTERM 15
#define VNU_SIGCHLD 17 /* a child changed state */
#define VNU_SIGWINCH 28 /* the window's pixel canvas changed size: a
                           * gfx app catches this, asks gfx_canvas()
                           * again and redraws at the new size */

/* The two handler values that are not addresses. */
#define VNU_SIG_DFL 0u /* do the default thing: terminate */
#define VNU_SIG_IGN 1u /* do nothing at all */

/* What the kernel does with a signal it delivers. SA_RESTORER is not
 * optional: it is where the handler's own `return` lands, and the
 * kernel has no other way to be told the handler is done. */
#define VNU_SA_ONSTACK   0x08000000u /* not supported: ignored */
#define VNU_SA_RESTART   0x10000000u /* not supported: ignored */
#define VNU_SA_NODEFER   0x40000000u /* do not block the signal itself */
#define VNU_SA_RESETHAND 0x80000000u /* back to default after one delivery */
#define VNU_SA_SIGINFO   0x00000004u /* not supported: ignored */
#define VNU_SA_RESTORER  0x04000000u /* the restorer field is valid; vlibc's
                                       * sigaction() always sets it */

/* VNU_SYS_rt_sigaction: what a program asks for. `handler` is the
 * address of a void (*)(int), or one of the two values above;
 * `mask` is added to the process's blocked set for as long as the
 * handler runs; `restorer` is the address the handler returns to. */
struct vnu_sigaction {
    uint32_t handler;
    uint32_t mask;
    uint32_t flags;
    uint32_t restorer;
};

/* VNU_SYS_rt_sigprocmask: `how` is one of these. */
#define VNU_SIG_BLOCK   0u
#define VNU_SIG_UNBLOCK 1u
#define VNU_SIG_SETMASK 2u

#define VNU_EPERM 1
#define VNU_ENOENT 2
#define VNU_ESRCH 3
#define VNU_EIO 5
#define VNU_EINTR 4
#define VNU_ENOEXEC 8
#define VNU_EBADF 9
#define VNU_ECHILD 10
#define VNU_EAGAIN 11
#define VNU_ENOMEM 12
#define VNU_EACCES 13
#define VNU_EFAULT 14
#define VNU_EBUSY 16
#define VNU_EEXIST 17
#define VNU_ENODEV 19
#define VNU_ENOTDIR 20
#define VNU_EISDIR 21
#define VNU_EINVAL 22
#define VNU_EMFILE 24
#define VNU_ENOSPC 28
#define VNU_ERANGE 34
#define VNU_EADDRINUSE 98
#define VNU_ECONNRESET 104
#define VNU_ENOTCONN 107
#define VNU_ETIMEDOUT 110
#define VNU_ECONNREFUSED 111
#define VNU_EHOSTUNREACH 113
#define VNU_EDQUOT 122
#define VNU_ENOSYS 38
#define VNU_ENOTEMPTY 39
#define VNU_EPIPE 32

#define VNU_ESRCH 3
