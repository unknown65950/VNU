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
; stack) and jumps straight into the program.
vnu_proc_trampoline:
    mov eax, [vnu_proc_pending_entry]
    mov esp, [vnu_proc_pending_stack]
    jmp eax

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
