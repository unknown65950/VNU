BITS 32
section .text
global vnu_syscall_entry
extern vnu_syscall_dispatch
extern vnu_pending_user_esp
extern vnu_pending_user_esp_flag
extern vnu_pending_user_eip
extern vnu_pending_jump_flag

vnu_syscall_entry:
    pushad
    mov eax, esp
    push eax
    call vnu_syscall_dispatch
    add esp, 4
    mov [esp + 28], eax       ; saved EAX in pushad

    ; execve / process jump: abandon interrupt frame, jmp to new image
    cmp byte [vnu_pending_jump_flag], 0
    je .normal_return
    mov byte [vnu_pending_jump_flag], 0
    mov eax, [vnu_pending_user_eip]
    mov ecx, [vnu_pending_user_esp]
    ; discard pushad frame + iret frame (8*4 + 3*4 = 44)
    add esp, 44
    mov esp, ecx
    jmp eax

.normal_return:
    popad
    ; optional ESP adjust only if still on same image (rare)
    cmp byte [vnu_pending_user_esp_flag], 0
    je .do_iret
    mov esp, [vnu_pending_user_esp]
    mov byte [vnu_pending_user_esp_flag], 0
.do_iret:
    iretd
