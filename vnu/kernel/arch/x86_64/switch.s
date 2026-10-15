BITS 64
section .text

global vnu_enter_user
vnu_enter_user:
    mov rsp, rsi
    mov rdi, rsp
    jmp rdi

global vnu_return_to_console
vnu_return_to_console:
    ret
