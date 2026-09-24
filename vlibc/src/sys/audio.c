#include <vlibc/audio.h>
#include <vlibc/sys/syscall.h>
#include <vnu/abi.h>

int audio_open(void)
{
    return (int)syscall(VNU_SYS_audio_open, 0L, 0L, 0L);
}

int audio_set_fmt(uint32_t rate, uint32_t channels, uint32_t bits)
{
    return (int)syscall(VNU_SYS_audio_set_fmt, (long)rate, (long)channels,
                        (long)bits);
}

long audio_write(const void* buf, uint32_t len)
{
    return syscall(VNU_SYS_audio_write, (long)buf, (long)len, 0L);
}

int audio_drain(void)
{
    return (int)syscall(VNU_SYS_audio_drain, 0L, 0L, 0L);
}

long audio_pending(void)
{
    return syscall(VNU_SYS_audio_pending, 0L, 0L, 0L);
}

int audio_pause(void)
{
    return (int)syscall(VNU_SYS_audio_pause, 0L, 0L, 0L);
}

int audio_reset(void)
{
    return (int)syscall(VNU_SYS_audio_reset, 0L, 0L, 0L);
}

int audio_close(void)
{
    return (int)syscall(VNU_SYS_audio_close, 0L, 0L, 0L);
}
