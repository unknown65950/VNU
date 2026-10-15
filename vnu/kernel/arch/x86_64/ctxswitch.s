BITS 64
section .text

global vnu_swtch
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

global vnu_swtch_pd
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

global vnu_proc_preempt
vnu_proc_preempt:
    ret

global vnu_proc_resume_preempted
vnu_proc_resume_preempted:
    ret

global vnu_timer_isr
vnu_timer_isr:
    iretq

global vnu_timer_init
vnu_timer_init:
    ret
