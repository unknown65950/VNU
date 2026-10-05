#pragma once
/* stddef.h — the types that belong to no other header.
 *
 * vlibc spells every length `unsigned long` in its own prototypes, which
 * is the same width as size_t on the only target it has and is a
 * legitimate freestanding choice. What it was missing is the *name*: a
 * program ported from an ordinary libc writes `size_t` and has to
 * compile unchanged. This header gives it that name, and it is the same
 * type under it — a program may mix both spellings freely.
 *
 * It sits at the top of vlibc/include rather than under vlibc/ because
 * `-I vlibc/include` comes before the system directories, so this is
 * what `#include <stddef.h>` resolves to in every VNU build: the vlibc
 * sources, the userspace programs, and the headers that already reach
 * for it (vlibc/tls.h, vnu/cxx.hpp, vnu/abi.h) all end up with this one
 * size_t, and nothing has to agree with anything. Were it namespaced
 * instead, every header below would have to include it and any file that
 * included both this and the host's stddef.h would get two conflicting
 * definitions of size_t — which is an error, not a warning, because the
 * two are different types of the same width. */

typedef unsigned long size_t;
typedef long ptrdiff_t;
#ifndef __cplusplus
/* A builtin in C++, where redeclaring it is an error rather than a
 * duplicate. Every other libc guards it this way round. */
typedef int wchar_t;
#endif

/* Only when nothing has defined it already, for the reason stdio.h
 * gives: vlibc's headers are used alongside a host's when a library is
 * built for both sides of the tree, and a second definition of NULL is
 * a warning in every compiler that notices. */
#ifndef NULL
#define NULL ((void*)0)
#endif

#define offsetof(type, member) ((size_t)&(((type*)0)->member))
