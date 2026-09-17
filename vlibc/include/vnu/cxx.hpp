#pragma once
/* Minimal C++ helpers for freestanding VNU — NOT a full standard library. */
#include <stddef.h>
#include <stdint.h>

namespace vnu {
inline void* operator new(size_t n) {
    extern void* malloc(unsigned long);
    return malloc(n);
}
inline void operator delete(void* p) noexcept {
    extern void free(void*);
    free(p);
}
inline void operator delete(void* p, size_t) noexcept { operator delete(p); }
}
