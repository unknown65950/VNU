#ifndef VNU_LIBWAV_H
#define VNU_LIBWAV_H
/*
 * wav — self-contained RIFF/WAVE PCM parser (see wav.c for details).
 *
 * Drop-in POSIX library: include this header, compile wav.c alongside
 * your program, parse any .wav image sitting in memory.
 */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Extracted PCM parameters and a pointer to the raw payload inside the
 * source buffer (the library never copies the audio data). */
typedef struct wav_info {
    uint32_t sample_rate;    /* Hz, 8000..48000 */
    uint16_t channels;       /* 1 or 2 */
    uint16_t bits_per_sample; /* 8 or 16 */
    const unsigned char* data; /* pointer into the source buffer */
    uint32_t data_len;        /* PCM payload bytes */
} wav_info;

/* See wav.c for the return codes (0 = OK, negative = error). */
int wav_parse(const void* buf, size_t len, wav_info* out);

#ifdef __cplusplus
}
#endif
#endif