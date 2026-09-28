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

/* How many slots VNU_SYS_gfx_palette answers, and how many bytes a
 * program has to offer for it: the 16 entries of the desktop's own
 * image palette as the same B8G8R8X8 words the frame itself holds
 * (0xXXRRGGBB, alpha in the top byte). */
#define VNU_GFX_PALETTE_SLOTS 16
#define VNU_GFX_PALETTE_BYTES 64 /* SLOTS * sizeof(uint32_t) */

#define VNU_EPERM 1
#define VNU_ENOENT 2
#define VNU_ESRCH 3
#define VNU_EIO 5
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
