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

/* How many slots VNU_SYS_gfx_palette answers, and how many bytes a
 * program has to offer for it: the 16 entries of the desktop's own
 * image palette as the same B8G8R8X8 words the frame itself holds
 * (0xXXRRGGBB, alpha in the top byte). */
#define VNU_GFX_PALETTE_SLOTS 16
#define VNU_GFX_PALETTE_BYTES 64 /* SLOTS * sizeof(uint32_t) */

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
#define VNU_ENOSYS 38
#define VNU_ENOTEMPTY 39
#define VNU_EPIPE 32

#define VNU_ESRCH 3
