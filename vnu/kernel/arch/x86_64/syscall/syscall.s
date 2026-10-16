BITS 64
; syscall.s - the x86_64 int $0x80 syscall entry.
;
; The userspace is 32-bit (compat mode), so the syscall ABI is the
; i386 one and this stub presents the i386 trap frame: the eight
; pushad-style registers in the slots vnu::arch::TrapFrame names, then
; the long-mode words the CPU pushed under them (see frame.inc and the
; struct's comment). It calls the shared dispatcher (arch/common/
; syscall_dispatch.cpp), writes the result back into the frame's eax
; slot, and then either irets straight back or - on a pending jump or
; signal frame - hands over to the new image or the handler's own
; frame, the same way arch/i386/syscall/syscall.s does.
section .text
global vnu_syscall_entry
extern vnu_syscall_dispatch
extern vnu_pending_user_esp
extern vnu_pending_user_esp_flag
extern vnu_pending_user_eip
extern vnu_pending_jump_flag
extern vnu_compat_enter

%include "frame.inc"

vnu_syscall_entry:
    ; The gate is an interrupt gate, so IF is already clear; this cli
    ; mirrors the i386 stub's rule that the handler runs with
    ; preemption off (the PIT must not preempt a half-written VFS
    ; state), whatever the gate type is.
    cli
    VNU_FRAME_ENTER

    mov rdi, rsp            ; TrapFrame*
    call vnu_syscall_dispatch
    mov [rsp + VNU_TF_EAX], rax  ; frame.eax - the syscall's return value

    cmp byte [vnu_pending_jump_flag], 0
    je .normal_return
    ; execve, or a windowed task landing: abandon this frame and enter
    ; the new image the way a first entry does - an iretq into compat
    ; mode, on the program's own stack.
    mov byte [vnu_pending_jump_flag], 0
    mov edi, [vnu_pending_user_eip]
    mov esi, [vnu_pending_user_esp]
    jmp vnu_compat_enter

.normal_return:
    ; A signal handler's frame has been armed on the program's own
    ; stack (see request_stack_switch): return through it instead of
    ; the frame just built.
    cmp byte [vnu_pending_user_esp_flag], 0
    je .do_iret
    mov byte [vnu_pending_user_esp_flag], 0
    mov rsp, [vnu_pending_user_esp]
    iretq
.do_iret:
    VNU_FRAME_LEAVE
