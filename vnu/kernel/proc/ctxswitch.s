BITS 32
section .text

global vnu_swtch
global vnu_swtch_pd
global vnu_wintask_trampoline
global vnu_wintask_pending_entry
global vnu_wintask_pending_stack
global vnu_wintask_new_pd
global vnu_wintask_old_pd
global vnu_proc_switch
global vnu_proc_trampoline
global vnu_proc_pending_entry
global vnu_proc_pending_stack
global vnu_proc_new_pd
global vnu_proc_old_pd
global vnu_timer_isr
global vnu_proc_preempt
global vnu_proc_resume_preempted

extern vnu_timer_tick

; void vnu_swtch(uint32_t* old_esp_out, uint32_t new_esp)
;
; Classic cooperative "coroutine" context switch (the same pattern
; teaching kernels like xv6 use for kernel threads): only the four
; callee-saved GP registers plus ESP/EIP need to survive a switch,
; because a switch only ever happens at an explicit call site (a
; blocking stdin read, or task exit) — not an asynchronous interrupt —
; so by the ordinary x86 cdecl calling convention the caller has
; already accepted that eax/ecx/edx may not survive a call.
vnu_swtch:
    mov eax, [esp+4]      ; old_esp_out
    mov edx, [esp+8]      ; new_esp

    push ebp
    push ebx
    push esi
    push edi

    mov [eax], esp         ; *old_esp_out = esp (this task's suspended stack)
    mov esp, edx            ; switch onto the other task's stack

    pop edi
    pop esi
    pop ebx
    pop ebp
    ret                     ; resumes wherever *that* stack's return address points

; void vnu_swtch_pd(uint32_t* old_esp_out, uint32_t new_esp)
;
; Like vnu_swtch, but ALSO swaps the page directory (CR3). The next page
; directory comes from the vnu_wintask_new_pd global (which each caller
; sets first) and the previously active one is stored to
; vnu_wintask_old_pd. The CR3 load is deliberately placed between the
; stack swap and the epilogue pops: pushes go to the OLD stack in the OLD
; directory, pops come from the NEW stack in the NEW directory. This is
; what makes switching between the GUI (whose stack lives in the
; console-process's private mapping at 0x600000) and per-window tasks
; (private 0x900000 stack) safe.
vnu_swtch_pd:
    mov eax, [esp+4]      ; old_esp_out
    mov edx, [esp+8]      ; new_esp

    push ebp
    push ebx
    push esi
    push edi

    mov [eax], esp         ; *old_esp_out = esp
    mov esp, edx           ; switch stacks first (write done above)

    mov eax, cr3
    mov [vnu_wintask_old_pd], eax
    mov eax, [vnu_wintask_new_pd]
    mov cr3, eax

    pop edi
    pop esi
    pop ebx
    pop ebp
    ret                     ; resumes wherever *that* stack's return address points

; Landed on via the `ret` above the very first time a brand-new task is
; switched into: its initial stack is crafted (see wintask.cpp) so the
; "return address" slot left by a never-really-happened prior call
; points here instead. Mirrors what proc/switch.s's vnu_enter_user does
; for the classic (non-windowed) process path — jump straight to the
; ELF entry point on its own argc/argv stack — just reached via `ret`
; instead of `call` since we got here through vnu_swtch.
vnu_wintask_trampoline:
    mov eax, [vnu_wintask_pending_entry]
    mov esp, [vnu_wintask_pending_stack]
    jmp eax

; void vnu_proc_switch(uint32_t* old_esp_out, uint32_t new_esp)
;
; Scheduler coroutine switch for the classic (non-windowed) process
; path — exactly the same switch as vnu_swtch_pd (saves callee-saved
; regs + ESP, loads the new stack, swaps CR3) but with its own page-
; directory globals, vnu_proc_new_pd/vnu_proc_old_pd, so the classic-
; process scheduler and the GUI/wintask manager never clobber each
; other's new/old pd slots (they may interleave: a coro-managed vash
; can hand control to the GUI, which lives in the same process table).
; The process being switched INTO its own private address space is
; parked when it blocks, and re-entered later from the scheduler.
vnu_proc_switch:
    mov eax, [esp+4]      ; old_esp_out
    mov edx, [esp+8]      ; new_esp

    push ebp
    push ebx
    push esi
    push edi

    mov [eax], esp        ; *old_esp_out = esp

    mov esp, edx          ; switch stacks first (write done above)
    mov eax, cr3
    mov [vnu_proc_old_pd], eax
    mov eax, [vnu_proc_new_pd]
    mov cr3, eax

    pop edi
    pop esi
    pop ebx
    pop ebp
    ret                   ; resumes wherever *that* stack's return addr points

; First-run trampoline for coro-managed classic processes (mirrors
; vnu_wintask_trampoline): the initial switch frame on the process's
; user stack has this as its "return address"; the scheduler primes
; vnu_proc_pending_entry/stack (ELF entry + freshly built argc/argv
; stack) and jumps straight into the program. sti here (and nowhere in
; the scheduler itself) is what turns cooperative round-robin into
; preemptive round-robin: from this point the process's own code runs
; with interrupts enabled, so the PIT's IRQ0 can seize the CPU from it —
; the two most common resume points (an `iretd` and this jmp) are the
; only places in the whole coro-process path where IF is ever set.
vnu_proc_trampoline:
    mov eax, [vnu_proc_pending_entry]
    mov esp, [vnu_proc_pending_stack]
    sti
    jmp eax

; --- Preemptive round-robin: the PIT timer (IRQ0) ---------------------
;
; IRQ0 fires every ~10 ms while a coro process executes its own code.
; vnu_timer_isr saves the process's ENTIRE CPU state (pushad plus the
; interrupt gate's hardware frame), asks vnu_timer_tick() whether anyone
; else is runnable, and if so parks the process — vnu_proc_preempt
; stores that complete frame pointer in its coro_esp and hops onto the
; scheduler coroutine, whose own stack (g_sched_esp) is resumed exactly
; like a cooperative yield would. run_slice() later resurrects the
; preempted process with vnu_proc_resume_preempted, which reloads the
; full frame and irets back into the middle of whatever instruction was
; interrupted. Contrast with vnu_proc_switch, which only preserves the
; four callee-saved registers — fine for a yield at an explicit call
; site, insufficient for an interrupt in arbitrary code.
;
; The frame layout this walks: the hardware interrupt entry pushed
; [eip][cs][eflags]; pushad added [edi][esi][ebp][esp][ebx][edx][ecx][eax]
;   low address -> edi esi ebp esp ebx edx ecx eax | eip cs eflags
; So popad leaves ESP exactly at the hardware frame and `iretd` returns
; into the interrupted instruction with eflags (incl. IF=1) restored.

; vnu_timer_isr — ACKs the PIC first (an un-EOI'd IRQ0 latches the 8259
; in-service bit and kills all further master-side interrupts), then
; defers to vnu_timer_tick(frame_esp). On "no preemption" (current
; process is the scheduler itself, or nobody else is runnable) it simply
; returns; on a preemption vnu_timer_tick never comes back.
vnu_timer_isr:
    cli
    push eax
    mov al, 0x20
    out 0x20, al            ; EOI -> master PIC
    pop eax
    pushad
    push esp
    call vnu_timer_tick
    add esp, 4
    popad
    iretd

; void vnu_proc_preempt(uint32_t sched_esp)
;
; The preempt-side switch, called only from the timer ISR's C handler
; (current stack is the interrupted process's own, deep in the ISR/tick
; call chain). The saved full frame pointer is NOT esp here — it was
; already recorded into the process's coro_esp by vnu_timer_tick() with
; the value passed straight out of the pushad before any of this call
; machinery had a chance to move the stack — so this routine must not
; re-save it (vnu_proc_switch's symmetric store would happily stash the
; wrong, deep ESP and the later popad+iret would resume from garbage).
; It just slides onto the scheduler's parked [edi][esi][ebx][ebp][ret]
; frame that run_slice left on g_sched_esp, exactly like a cooperative
; yield would, and swaps CR3 to the scheduler's directory.
vnu_proc_preempt:
    mov edx, [esp+4]      ; new_esp = g_sched_esp
    mov esp, edx          ; onto the scheduler coroutine's stack
    mov eax, cr3
    mov [vnu_proc_old_pd], eax
    mov eax, [vnu_proc_new_pd]
    mov cr3, eax
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret                   ; resumes run_slice right after a switch-in

; void vnu_proc_resume_preempted(uint32_t* old_esp_out, uint32_t new_esp)
;
; The scheduler-side resume for a process parked by the timer: same
; [edi][esi][ebx][ebp][ret] park of g_sched_esp (so the process can
; preempt/yield back into run_slice later), CR3 swap, then popad+iretd
; straight into the interrupted instruction instead of the callee-saved
; pops + ret of vnu_proc_switch.
vnu_proc_resume_preempted:
    push ebp
    push ebx
    push esi
    push edi
    mov eax, [esp+20]     ; old_esp_out = &g_sched_esp
    mov edx, [esp+24]     ; new_esp = p.coro_esp (full interrupt frame)
    mov [eax], esp        ; park the scheduler coroutine here
    mov esp, edx          ; onto the process's saved frame
    mov eax, cr3
    mov [vnu_proc_old_pd], eax
    mov eax, [vnu_proc_new_pd]
    mov cr3, eax
    popad
    iretd

section .bss
align 4
vnu_wintask_pending_entry: resd 1
vnu_wintask_pending_stack: resd 1
vnu_wintask_new_pd: resd 1
vnu_wintask_old_pd: resd 1
vnu_proc_pending_entry: resd 1
vnu_proc_pending_stack: resd 1
vnu_proc_new_pd: resd 1
vnu_proc_old_pd: resd 1
