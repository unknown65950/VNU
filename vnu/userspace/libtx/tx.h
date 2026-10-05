#ifndef VLIBC_TX_H
#define VLIBC_TX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tx_font;
typedef struct tx_font tx_font;

struct tx_glyph {
    int w;
    int h;
    int xoff;
    int yoff;
    int advance;
    uint8_t* bitmap; /* 8-bit alpha */
};

tx_font* tx_load(const uint8_t* ttf_data, uint32_t ttf_size, float height);
void tx_free(tx_font* f);
void tx_metrics(tx_font* f, const char* text, int* w, int* h, int* ascent, int* descent);
void tx_draw(tx_font* f, uint32_t* dst, int dst_w, int dst_h, int x, int y, const char* text, uint32_t color);
int tx_glyph(tx_font* f, int codepoint, struct tx_glyph* g);

#ifdef __cplusplus
}
#endif

#endif
