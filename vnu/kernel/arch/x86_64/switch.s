BITS 64
; switch.s - the x86_64 one-way user entry and its console continuation.
; See arch/i386/switch.s: vnu_enter_user hands control to a program on
; its own stack and stashes the (return address, frame, stack) that
; vnu_return_to_console winds back to when the program ends.
section .text
global vnu_enter_user
global vnu_return_to_console
global vnu_saved_eip
global vnu_saved_esp
global vnu_saved_ebp

extern vnu_saved_eip
extern vnu_saved_esp
extern vnu_saved_ebp

; void vnu_enter_user(uint32_t entry, uint32_t user_esp)
;   rdi = entry, rsi = user_esp (System V)
vnu_enter_user:
    push rbp
    mov rbp, rsp
    push rbx
    push rsi
    push rdi

    mov rax, [rbp+8]            ; caller's return address
    mov [vnu_saved_eip], rax
    mov [vnu_saved_ebp], rbp
    mov [vnu_saved_esp], rsp

    mov rax, rdi                ; entry point
    mov rcx, rsi                ; user stack
    cli
    mov rsp, rcx
    jmp rax

vnu_return_to_console:
    cli
    mov rsp, [vnu_saved_esp]
    mov rbp, [vnu_saved_ebp]
    pop rdi
    pop rsi
    pop rbx
    pop rbp
    sti
    ret

section .bss
align 8
vnu_saved_eip: resq 1
vnu_saved_esp: resq 1
vnu_saved_ebp: resq 1
