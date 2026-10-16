BITS 64
; ctxswitch.s - the x86_64 cooperative coroutine switch and the timer
; preemption pair. Mirrors arch/i386/ctxswitch.s, widened to the six
; callee-saved 64-bit registers and the System V argument registers.
;
; The interrupt side differs from i386 in one way that is easy to miss:
; a long-mode trap pushes the full five-word frame (including RSP and
; SS) even without a privilege change, so the entry only has to save
; the general registers below it and the return irets straight out of
; it. VNU_FRAME_ENTER/VNU_FRAME_LEAVE (frame.inc) do exactly that. See
; that file for the layout.
section .text

%include "frame.inc"

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
extern vnu_compat_enter
extern vnu_pending_user_esp
extern vnu_pending_user_esp_flag

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

; Landed on by the first switch into a brand-new windowed task: the
; switch frame this returns through (see build_first_frame) puts the
; program's own argc/argv stack in vnu_wintask_pending_stack and its
; entry point in vnu_wintask_pending_entry. The landing itself is the
; same iretq into compat mode the classic path uses.
vnu_wintask_trampoline:
    mov edi, [vnu_wintask_pending_entry]
    mov esi, [vnu_wintask_pending_stack]
    jmp vnu_compat_enter

; First-run trampoline for coro-managed classic processes (mirrors
; vnu_wintask_trampoline): the initial switch frame on the process's
; user stack has this as its "return address"; the scheduler primes
; vnu_proc_pending_entry/stack (ELF entry + freshly built argc/argv
; stack) and hands over to the program. sti here (and nowhere in
; the scheduler itself) is what turns cooperative round-robin into
; preemptive round-robin: from this point the process's own code runs
; with interrupts enabled, so the PIT's IRQ0 can seize the CPU from it —
; the two most common resume points (the iretq that enters user mode
; and the one the syscall return path runs) are the only places in the
; whole coro-process path where IF is ever set.
vnu_proc_trampoline:
    mov edi, [vnu_proc_pending_entry]
    mov esi, [vnu_proc_pending_stack]
    sti
    jmp vnu_compat_enter

; --- Preemptive round-robin: the PIT timer (IRQ0) -------------------
;
; vnu_timer_isr saves the general registers below the trap frame
; (VNU_FRAME_ENTER), ACKs the PIC and defers to
; vnu_timer_tick(frame_rsp). The frame pointer is the whole saved stack
; pointer: the process model keeps it as a uintptr_t, and every stack
; this kernel runs on lives in the low identity-mapped window, so the
; value round-trips through the 32-bit user address space unchanged.
vnu_timer_isr:
    cli
    push rax
    mov al, 0x20
    out 0x20, al            ; EOI -> master PIC
    pop rax

    VNU_FRAME_ENTER

    mov rdi, rsp            ; frame_rsp
    call vnu_timer_tick

    ; A signal waiting for this process builds the handler's frame on
    ; the same stack and arms the flag; return through that instead of
    ; the interrupted instruction.
    cmp byte [vnu_pending_user_esp_flag], 0
    je .do_iret
    mov byte [vnu_pending_user_esp_flag], 0
    mov rsp, [vnu_pending_user_esp]
    iretq
.do_iret:
    VNU_FRAME_LEAVE

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

    ; run_slice() may have built a handler's frame on this one while the
    ; process was parked; honour it before the interrupted context.
    cmp byte [vnu_pending_user_esp_flag], 0
    je .do_iret
    mov byte [vnu_pending_user_esp_flag], 0
    mov rsp, [vnu_pending_user_esp]
    iretq
.do_iret:
    VNU_FRAME_LEAVE

; --- Plain hardware IRQ stubs ---------------------------------------
;
; The core installs these for the vectors it does not otherwise handle.
; Nothing here calls C, so no register is clobbered: the entry saves
; the general registers under the hardware frame and the return puts
; them back and irets out of it.
global vnu_ignore_irq
vnu_ignore_irq:
    VNU_FRAME_ENTER
    VNU_FRAME_LEAVE

global vnu_irq_master_ack
vnu_irq_master_ack:
    push rax
    mov al, 0x20
    out 0x20, al            ; EOI -> master PIC
    pop rax
    VNU_FRAME_ENTER
    VNU_FRAME_LEAVE

global vnu_irq_slave_ack
vnu_irq_slave_ack:
    push rax
    mov al, 0x20
    out 0xA0, al            ; EOI -> slave PIC first
    out 0x20, al            ; then EOI -> master PIC
    pop rax
    VNU_FRAME_ENTER
    VNU_FRAME_LEAVE

section .bss
align 4
vnu_wintask_pending_entry: resd 1
vnu_wintask_pending_stack: resd 1
vnu_proc_pending_entry: resd 1
vnu_proc_pending_stack: resd 1
