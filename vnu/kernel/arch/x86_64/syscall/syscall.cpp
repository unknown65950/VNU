#include <vnu/arch/context.h>
#include <stdint.h>

/* arch/x86_64/syscall/syscall.cpp - the x86_64 half of the syscall
 * dispatcher.
 *
 * STATUS: bring-up skeleton. arch/i386/syscall/syscall.cpp is the real
 * dispatcher (one case per vnu/abi.h call, reading the trap frame's
 * fields); porting it here is mostly mechanical field renaming, but it
 * only becomes meaningful once the port boots and the userspace ABI for
 * x86_64 is settled. Until then this returns -ENOSYS for every call so
 * the image links and a boot can be brought up incrementally.
 * See arch/README.md. */

using vnu::arch::TrapFrame;

extern "C" int64_t vnu_syscall_dispatch(TrapFrame* tf)
{
    (void)tf;
    return -38; /* -ENOSYS */
}
