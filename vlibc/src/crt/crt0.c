// The entry point for programs (x86_64, Linux).
//
// Important: _start must NOT be an ordinary C function. The compiler
// puts a prologue in it (push %rbp; mov %rsp,%rbp; sub $N,%rsp for
// locals), so by the time our code runs %rsp no longer points at what
// the kernel put there when the process started. That was exactly the
// bug once: argc was read out of _start's own stack frame instead of
// the kernel's data - garbage at best, a segfault at worst.
//
// The correct scheme (as in musl/glibc): a naked asm stub grabs the
// "raw" %rsp BEFORE any prologue and passes it as an ordinary argument
// to a small C function.
#include <stddef.h>
#include <vlibc/unistd.h>

// The user-facing main
extern int main(int argc, char** argv, char** envp);

// At process start on x86_64 Linux the stack looks like this:
//   rsp -> [argc]
//          [argv[0]]
//          ...
//          [argv[argc-1]]
//          [NULL]
//          [envp[0]]
//          ...
//          [NULL]
//          [auxv...]
void _start_c(long* stack) {
    int argc = (int)stack[0];
    char** argv = (char**)&stack[1];
    char** envp = argv + argc + 1;

    int result = main(argc, argv, envp);

    exit(result);
}

__asm__(
    ".global _start\n"
    "_start:\n"
    "   xor %ebp, %ebp\n"        // clear rbp - end of the frame chain for debuggers
    "   mov %rsp, %rdi\n"        // rdi = pointer to argc (the first argument of _start_c)
    "   and $-16, %rsp\n"        // align the stack to 16 bytes as the SysV ABI wants
    "   call _start_c\n"
    "   hlt\n"                   // must never be reached (_start_c calls exit)
);
