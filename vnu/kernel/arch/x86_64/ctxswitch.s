BITS 64
; ctxswitch.s - the x86_64 cooperative coroutine switch and the timer
; preemption pair. Mirrors arch/i386/ctxswitch.s, widened to the six
; callee-saved 64-bit registers and the System V argument registers.
;
; STATUS: bring-up skeleton (see arch/README.md). The cooperative
; switch (vnu_swtch / vnu_swtch_pd) and the first-run trampolines are
; the real long-mode shapes; the preemption pair and the timer ISR are
; present so the port links and can be finished incrementally, but the
; ring3 signal hand-off on the return path is not implemented yet.
section .text

global vnu_swtch
global vnu_swtch_pd
global vnu_wintask_trampoline
global vnu_wintask_pending_entry
global vnu_wintask_pending_stack
global vnu_proc_trampoline
global vnu_proc_pending_entry
global vnu_proc_pending_stack
global vnu_timer_isr
global vnu_proc_preempt
global vnu_proc_resume_preempted

extern vnu_timer_tick

; void vnu_swtch(uint64_t* old_rsp_out, uint64_t new_rsp)
;   rdi = old_rsp_out, rsi = new_rsp
vnu_swtch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp
    mov rsp, rsi

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; void vnu_swtch_pd(uint64_t* old_rsp_out, uint64_t new_rsp, uint64_t new_pd)
vnu_swtch_pd:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp
    mov rsp, rsi
    mov cr3, rdx

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; Landed on by the first switch into a brand-new windowed task.
vnu_wintask_trampoline:
    mov eax, [vnu_wintask_pending_entry]
    mov esp, [vnu_wintask_pending_stack]
    jmp rax

; First-run trampoline for coro-managed classic processes.
vnu_proc_trampoline:
    mov eax, [vnu_proc_pending_entry]
    mov esp, [vnu_proc_pending_stack]
    sti
    jmp rax

; --- Preemptive round-robin: the PIT timer (IRQ0) -------------------
;
; vnu_timer_isr saves the general registers, ACKs the PIC and defers to
; vnu_timer_tick(frame_rsp). The frame pointer is truncated to 32 bits
; on the way in because vnu_timer_tick() and the whole process model are
; still 32-bit (the low-memory window); see arch/README.md.
vnu_timer_isr:
    cli
    push rax
    mov al, 0x20
    out 0x20, al            ; EOI -> master PIC
    pop rax

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

    mov edi, esp            ; frame_rsp (low 32 bits)
    call vnu_timer_tick

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
    iretq

; void vnu_proc_preempt(uint64_t sched_rsp, uint64_t new_pd)
;   rdi = sched_rsp, rsi = new_pd
vnu_proc_preempt:
    mov rsp, rdi
    mov cr3, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; void vnu_proc_resume_preempted(uint64_t* old_rsp_out, uint64_t new_rsp,
;                                uint64_t new_pd)
;   rdi = old_rsp_out, rsi = new_rsp (full process frame), rdx = new_pd
vnu_proc_resume_preempted:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp
    mov rsp, rsi
    mov cr3, rdx

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
    iretq

section .bss
align 8
vnu_wintask_pending_entry: resd 1
vnu_wintask_pending_stack: resd 1
vnu_proc_pending_entry: resd 1
vnu_proc_pending_stack: resd 1
