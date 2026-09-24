#ifndef VLIBC_AUDIO_H
#define VLIBC_AUDIO_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Playback API on top of the kernel's audio_* syscalls (49..56, see
 * vnu/abi/ABI.md). Single-device AC'97: one open session at a time.
 *
 * audio_write is NON-blocking: it returns the number of input bytes it
 * could queue (0 when the DMA ring is full) and never stalls the caller.
 * Callers that must not block (GUI apps) top the ring up periodically
 * and use audio_pending() to track progress; console-style producers
 * may queue everything and finish with audio_drain(). */
int audio_open(void);
int audio_set_fmt(uint32_t rate, uint32_t channels, uint32_t bits);
long audio_write(const void* buf, uint32_t len);
int audio_drain(void);
long audio_pending(void);
int audio_pause(void);
int audio_reset(void);
int audio_close(void);
#ifdef __cplusplus
}
#endif
#endif
