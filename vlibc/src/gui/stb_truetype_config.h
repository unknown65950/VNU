#ifndef STB_TRUETYPE_CONFIG_H
#define STB_TRUETYPE_CONFIG_H
#include <vlibc/stdlib.h>
#include <vlibc/string.h>
#include <stddef.h>
#define STBTT_malloc(x,u) malloc(x)
#define STBTT_free(x,u) free(x)
#define STBTT_strlen(x) strlen(x)
#define STBTT_memcpy memcpy
#define STBTT_memset memset
#endif
