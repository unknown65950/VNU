#ifdef __cplusplus
extern "C" {
#endif

#pragma once

#include <vlibc/stdio.h>

unsigned long strlen(const char* str);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, unsigned long n);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, unsigned long n);
void* memcpy(void* dest, const void* src, unsigned long n);
void* memset(void* s, int c, unsigned long n);


#ifdef __cplusplus
}
#endif
