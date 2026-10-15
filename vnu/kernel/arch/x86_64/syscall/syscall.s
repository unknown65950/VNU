BITS 64
; syscall.s - the x86_64 int $0x80 syscall entry. Mirrors
; arch/i386/syscall/syscall.s: save the general registers into a
; vnu::arch::TrapFrame, call the dispatcher, write its result back into
; the frame's rax slot, then either take the execve jump or iretq.
;
; STATUS: bring-up skeleton. The dispatcher in arch/x86_64/syscall/
; syscall.cpp is not ported yet, so nothing that needs a real syscall
; works; see arch/README.md.
section .text
global vnu_syscall_entry
extern vnu_syscall_dispatch
extern vnu_pending_user_esp
extern vnu_pending_user_esp_flag
extern vnu_pending_user_eip
extern vnu_pending_jump_flag

vnu_syscall_entry:
    cli
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp                 ; TrapFrame*
    call vnu_syscall_dispatch
    mov [rsp + 112], rax         ; frame.rax (15th push from the top)

    cmp byte [vnu_pending_jump_flag], 0
    je .normal_return
    mov byte [vnu_pending_jump_flag], 0
    mov eax, [vnu_pending_user_eip]
    mov ecx, [vnu_pending_user_esp]
    mov rsp, rcx
    sti
    jmp rax

.normal_return:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    cmp byte [vnu_pending_user_esp_flag], 0
    je .do_iret
    mov rsp, [vnu_pending_user_esp]
    mov byte [vnu_pending_user_esp_flag], 0
.do_iret:
    iretq
