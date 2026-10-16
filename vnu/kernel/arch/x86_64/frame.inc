; frame.inc - the x86_64 trap-frame build/teardown macros.
;
; In long mode the CPU pushes the COMPLETE five-word frame on every
; interrupt and trap - RIP, CS, RFLAGS, RSP, SS - even when the
; privilege level does not change. (Intel describes SS:RSP as part of
; the IA-32e interrupt frame, and QEMU's do_interrupt64 pushes all five
; unconditionally; the port is built against that.) There is therefore
; nothing to synthesise on the way in: the entry stubs only save the
; general registers below the hardware frame, and the return path pops
; them and irets straight out of the hardware frame.
;
; On entry rsp points at the hardware frame, which is H here:
;     [H]=RIP  [H+8]=CS  [H+16]=RFLAGS  [H+24]=RSP  [H+32]=SS
; VNU_FRAME_ENTER leaves the frame base (the struct pointer) in rsp and
; clobbers rax, whose value is already saved in the frame by then. The
; eight bytes at struct offset 0..120 are the sixteen general registers
; (the eight the 32-bit ABI names, then r8-r15, which compat code cannot
; name but the kernel's interrupt windows can hold); from offset 128 on
; the struct overlaps the hardware frame verbatim, so the saved RSP and
; SS the iretq needs are already correct there and are never rewritten.

%define VNU_TF_EAX  56
%define VNU_TF_EIP  128
%define VNU_TF_CS   136
%define VNU_TF_EFL  144
%define VNU_TF_RSP  152
%define VNU_TF_SS   160

; The sixteen general registers, leaving rsp at the frame base (offset
; 0), where vnu::arch::TrapFrame begins.
%macro VNU_FRAME_ENTER 0
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rax
    push rcx
    push rdx
    push rbx
    lea rax, [rsp+96]       ; saved-ESP slot: the frame's own EIP word (H)
    push rax
    push rbp
    push rsi
    push rdi
%endmacro

%macro VNU_FRAME_LEAVE 0
    pop rdi
    pop rsi
    pop rbp
    add rsp, 8              ; the saved-ESP slot is not a register
    pop rbx
    pop rdx
    pop rcx
    pop rax
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    iretq                   ; rsp now points at the hardware frame
%endmacro
