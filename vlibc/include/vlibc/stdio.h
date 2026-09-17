#ifdef __cplusplus
extern "C" {
#endif

#pragma once

#include <vlibc/unistd.h>
#include <stdarg.h>

#define EOF (-1)
#define NULL ((void*)0)

int putchar(int c);
int puts(const char* str);
int printf(const char* format, ...);
int vprintf(const char* format, va_list args);


#ifdef __cplusplus
}
#endif
