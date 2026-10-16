#ifdef __cplusplus
extern "C" {
#endif

#pragma once

#include <stddef.h>
#include <vlibc/unistd.h>
#include <vlibc/stdio.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void* malloc(unsigned long size);
void free(void* ptr);
int atoi(const char* str);
long atol(const char* str);
/* The value of an environment variable, or NULL if it is not set. The
 * string belongs to the environment: POSIX lets a program read it and
 * forbids it to change it, so it must not be freed and must not be
 * written to. `environ` itself is declared in <unistd.h>. */
char* getenv(const char* name);


#ifdef __cplusplus
}
#endif
