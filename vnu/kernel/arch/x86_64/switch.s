BITS 64
; switch.s - the x86_64 one-way user entry and its console continuation.
; See arch/i386/switch.s: vnu_enter_user hands control to a program on
; its own stack and stashes the (return address, frame, stack) that
; vnu_return_to_console winds back to when the program ends.
;
; The userspace is 32-bit on every port, so the hand-off here is an
; iretq into the compat code segment (GDT selector 0x18, see boot/
; gdt.cpp) rather than a jmp into a 64-bit CS, which cannot decode a
; 32-bit binary. Long mode's iretq always pops five words even with no
; privilege change, so the entry builds a full RSP/SS frame just below
; the program's stack top; the iretq consumes it and leaves RSP exactly
; at that top, with argc/argv still on top as crt0 expects.
section .text
global vnu_enter_user
global vnu_compat_enter
global vnu_return_to_console
global vnu_saved_eip
global vnu_saved_esp
global vnu_saved_ebp

; void vnu_compat_enter(uint32_t entry, uint32_t user_esp)
;   rdi = entry, rsi = user_esp (System V)
; Lands in 32-bit compatibility mode at CPL0, on the program's own
; stack, with interrupts left as the caller left them - the i386 paths
; differ here (the coro trampolines run the program with IF set, the
; one-way console entry with it cleared) and this mirrors them, since
; only the caller's RFLAGS are used.
vnu_compat_enter:
    pushfq
    pop rax                 ; caller's RFLAGS (IF as the caller set it)
    mov rsp, rsi            ; the program's own stack; the iret frame lands
    push qword 0x20         ; SS: 32-bit compatibility data
    push rsi                ; RSP: the stack top itself
    push rax                ; RFLAGS
    push qword 0x18         ; CS: 32-bit compatibility code
    push rdi                ; RIP: the program's entry point
    iretq

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

    cli
    jmp vnu_compat_enter        ; rdi/rsi still hold entry/esp

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
align 4
vnu_saved_eip: resd 1
vnu_saved_esp: resd 1
vnu_saved_ebp: resd 1
