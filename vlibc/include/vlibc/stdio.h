#ifdef __cplusplus
extern "C" {
#endif

#pragma once

#include <vlibc/unistd.h>
#include <stdarg.h>

#define EOF (-1)

/* Only when nothing has defined it already: vlibc's headers are used
 * alongside the host's when a library is built for both sides of the
 * tree, and a second definition of NULL is a warning in every compiler
 * that notices, in whatever language the user happens to read. */
#ifndef NULL
#define NULL ((void*)0)
#endif

int putchar(int c);
int puts(const char* str);
int printf(const char* format, ...);
int vprintf(const char* format, va_list args);


#ifdef __cplusplus
}
#endif
