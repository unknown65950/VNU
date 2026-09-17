#include <vlibc/sys/syscall.h>

#if defined(__i386__) && defined(VLIBC_TARGET_VNU)
long syscall(long number, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, number);
    long a1 = __builtin_va_arg(ap, long);
    long a2 = __builtin_va_arg(ap, long);
    long a3 = __builtin_va_arg(ap, long);
    long a4 = __builtin_va_arg(ap, long);
    long a5 = __builtin_va_arg(ap, long);
    __builtin_va_end(ap);
    long ret;
    __asm__ volatile("int $0x80"
        : "=a"(ret)
        : "a"(number), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5)
        : "memory", "cc");
    return ret;
}
#elif defined(__x86_64__) && defined(VLIBC_TARGET_LINUX)
__attribute__((naked)) long syscall(long number, ...) {
    __asm__ volatile("movq %rdi,%rax\nmovq %rsi,%rdi\nmovq %rdx,%rsi\nmovq %rcx,%rdx\nmovq %r8,%r10\nmovq %r9,%r8\nmovq 8(%rsp),%r9\nsyscall\nret\n");
}
#else
#error "Select VLIBC_TARGET_VNU on i386 or VLIBC_TARGET_LINUX on x86_64"
#endif
