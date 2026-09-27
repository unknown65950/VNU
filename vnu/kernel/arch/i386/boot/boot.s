; boot.s — the Multiboot2 header and the VNU kernel entry point.
;
; GRUB (or any other Multiboot2-compatible loader) finds the header
; below, switches the CPU into 32-bit protected mode, loads our kernel
; at 1 MiB and jumps to _start.

section .multiboot
align 8
multiboot_header:
    dd 0xE85250D6                                    ; magic (multiboot2)
    dd 0                                              ; architecture: 0 = i386 protected mode
    dd multiboot_header_end - multiboot_header        ; header length
    dd -(0xE85250D6 + 0 + (multiboot_header_end - multiboot_header)) ; checksum

    ; end tag (required)
    dw 0
    dw 0
    dd 8
multiboot_header_end:

section .bss
align 16
stack_bottom:
    resb 16384                  ; 16 KiB kernel stack
stack_top:

section .text
global _start
extern kernel_main
extern __bss_start
extern __bss_end
_start:
    ; GRUB passes in:
    ;   eax = 0x36d76289 (multiboot2 magic)
    ;   ebx = pointer to the multiboot info struct
    ;
    ; .bss relocated to 0x00A00000 (see linker.ld) and is not loaded from
    ; the file, so zero it explicitly. rep stosb clobbers eax/edi/ecx and
    ; the flags, so stash the Multiboot2 registers first; no stack is
    ; needed yet.
    mov esi, eax
    mov edx, ebx
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb
    mov eax, esi
    mov ebx, edx

    mov esp, stack_top
    push ebx
    push eax
    call kernel_main

    cli
.hang:
    hlt
    jmp .hang
