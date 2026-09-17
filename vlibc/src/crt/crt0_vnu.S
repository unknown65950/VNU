/* VNU i386 userspace entry. Stack at entry (when processes exist):
 *   esp -> [argc]
 *          [argv[0]] ... [NULL]
 *          [envp[0]] ... [NULL]
 * Until the kernel provides that layout, _start still calls main(0, NULL, NULL)
 * after zeroing ebp and aligning the stack.
 */
    .text
    .global _start
    .extern _start_c
_start:
    xorl    %ebp, %ebp
    movl    %esp, %eax          /* raw stack pointer */
    andl    $-16, %esp          /* SysV i386 16-byte align before call */
    subl    $12, %esp           /* keep 16-byte align after 4-byte push */
    pushl   %eax                /* arg0: long *stack */
    call    _start_c
    /* _start_c must not return; if it does, hang */
1:  hlt
    jmp     1b

    .section .note.GNU-stack, "", @progbits
