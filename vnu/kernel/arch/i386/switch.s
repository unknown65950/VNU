BITS 32
section .text
global vnu_enter_user
global vnu_return_to_console
global vnu_saved_eip
global vnu_saved_esp
global vnu_saved_ebp

vnu_enter_user:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi

    mov eax, [ebp+4]
    mov [vnu_saved_eip], eax
    mov [vnu_saved_ebp], ebp
    mov [vnu_saved_esp], esp

    mov eax, [ebp+8]
    mov ecx, [ebp+12]
    cli
    mov esp, ecx
    ; argc at [esp]; crt0 reads it — no extra push
    jmp eax

vnu_return_to_console:
    cli
    mov esp, [vnu_saved_esp]
    mov ebp, [vnu_saved_ebp]
    pop edi
    pop esi
    pop ebx
    pop ebp
    sti
    ret

section .bss
align 4
vnu_saved_eip: resd 1
vnu_saved_esp: resd 1
vnu_saved_ebp: resd 1
