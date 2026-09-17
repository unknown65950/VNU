#ifdef __cplusplus
extern "C" {
#endif

#pragma once

#include <vlibc/unistd.h>
#include <vlibc/stdio.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void* malloc(unsigned long size);
void free(void* ptr);
int atoi(const char* str);
long atol(const char* str);


#ifdef __cplusplus
}
#endif
