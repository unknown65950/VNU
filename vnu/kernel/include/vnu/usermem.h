#pragma once
#ifndef VNU_USERMEM_H
#define VNU_USERMEM_H

/* usermem.h — reading values a program left in its own memory.
 *
 * The userspace is 32-bit on every port (arch/README.md): native i386 on
 * one, long mode's compatibility mode on x86_64. Whatever the kernel's
 * own pointers are, a pointer a program *stored* is four bytes (ABI.md:
 * "Architecture: i386, little-endian, 32-bit pointers"). The kernel must
 * therefore never walk an argv/envp-style array with its own pointer
 * type: on a port where that type is wider, one subscript fetches an
 * element together with the next one, and the address assembled from
 * both is one the program never made - on x86_64 the top half is another
 * low-4-GiB pointer, which no canonical address has, so the first use of
 * it faults as #GP(0). A #GP carries no cr2, so such a crash reports
 * cr2 = 0 and says nothing about the address that caused it.
 *
 * Strings need none of this: they are read a byte at a time, and a byte
 * does not care how wide the pointer that led to it was. Only arrays of
 * pointers need the helper below. It assembles the four bytes in the
 * ABI's little-endian order, so it makes no alignment assumption, and
 * hands the value back zero-extended to a native pointer.
 */

#include <stddef.h>
#include <stdint.h>

namespace vnu {

/* The `index'-th entry of a NULL-terminated pointer array in a program's
 * memory - an argv or envp passed to execve - as a native pointer, or
 * null when the four bytes there are zero (the list ended).
 *
 * `index' must already be known to be in bounds: this is the read, not
 * the range check. Callers keep their bound before the read so that a
 * list stopping exactly at the limit is never asked for the entry past
 * its end. */
inline const char* user_ptr_at(const void* array, size_t index)
{
    const unsigned char* p =
        static_cast<const unsigned char*>(array) + index * 4;
    const uint32_t raw = static_cast<uint32_t>(p[0]) |
                         (static_cast<uint32_t>(p[1]) << 8) |
                         (static_cast<uint32_t>(p[2]) << 16) |
                         (static_cast<uint32_t>(p[3]) << 24);
    return reinterpret_cast<const char*>(static_cast<uintptr_t>(raw));
}

} // namespace vnu

#endif /* VNU_USERMEM_H */
