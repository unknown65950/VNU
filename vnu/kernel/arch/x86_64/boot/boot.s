; boot.s - the x86_64 Multiboot2 header and the VNU kernel entry point.
;
; A Multiboot2 loader drops us in 32-bit protected mode. The kernel is
; 64-bit, so this stub builds a minimal identity map (2 MiB pages over
; the first 4 GiB) and walks the CPU into long mode before calling
; kernel_main. vnu::paging::init() later replaces the identity map with
; the real per-process model; vnu::arch::disable_paging() is a no-op on
; x86_64 because long mode cannot run with paging off.

section .multiboot
align 8
multiboot_header:
    dd 0xE85250D6
    dd 0                                              ; architecture: 0 = i386 (loaded in 32-bit PM)
    dd multiboot_header_end - multiboot_header
    dd -(0xE85250D6 + 0 + (multiboot_header_end - multiboot_header))
    dw 0
    dw 0
    dd 8
multiboot_header_end:

section .bss
align 16
stack_bottom:
    resb 32768
stack_top:
align 4096
pml4: resb 4096
pdpt: resb 4096
pd0:  resb 4096
pd1:  resb 4096
pd2:  resb 4096
pd3:  resb 4096
align 8
boot_magic: resd 1
boot_info:  resd 1

section .rodata
align 16
gdt64:
    dq 0x0000000000000000              ; null
    dq 0x00AF9A000000FFFF              ; 0x08: 64-bit code (L=1, D=0)
    dq 0x00CF92000000FFFF              ; 0x10: data
    dq 0x00CF9A000000FFFF              ; 0x18: 32-bit code (L=0, D=1)
    dq 0x00CF92000000FFFF              ; 0x20: 32-bit data
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64

section .text
bits 32
global _start
extern kernel_main
extern __bss_start
extern __bss_end

_start:
    ; GRUB: eax = 0x36d76289, ebx = multiboot info pointer.
    mov esi, eax
    mov edx, ebx

    ; Zero .bss (it is placed high, at 0xA00000, and not loaded).
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb

    mov [boot_magic], esi
    mov [boot_info], edx

    ; --- identity map the first 4 GiB with 2 MiB pages -------------
    ; pdpt[i] = pd_i | present|rw
    mov eax, pd0
    or eax, 0x3
    mov [pdpt + 0], eax
    mov eax, pd1
    or eax, 0x3
    mov [pdpt + 8], eax
    mov eax, pd2
    or eax, 0x3
    mov [pdpt + 16], eax
    mov eax, pd3
    or eax, 0x3
    mov [pdpt + 24], eax

    ; pml4[0] = pdpt | present|rw
    mov eax, pdpt
    or eax, 0x3
    mov [pml4], eax

    ; Fill each PD: 512 entries of 2 MiB. Entry = base + i*0x200000 | 0x83.
    mov edi, pd0
    call fill_pd
    mov edi, pd1
    call fill_pd
    mov edi, pd2
    call fill_pd
    mov edi, pd3
    call fill_pd

    ; --- enter long mode -------------------------------------------
    mov eax, pml4
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5                      ; PAE
    mov cr4, eax

    mov ecx, 0xC0000080                 ; EFER
    rdmsr
    or eax, 1 << 8                      ; LME
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | 1               ; PG | PE
    mov cr0, eax

    lgdt [gdt64_ptr]
    jmp 0x08:long_mode_entry

; Fill the 512-entry 2 MiB-page directory at edi. Clobbers eax/ecx/edi.
fill_pd:
    xor eax, eax                        ; physical base = 0
    mov ecx, 512
.loop:
    mov edx, eax
    or edx, 0x83                        ; present|rw|ps
    mov [edi], edx
    add edi, 8
    add eax, 0x200000
    dec ecx
    jnz .loop
    ret

bits 64
long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, stack_top
    and rsp, -16
    sub rsp, 8                          ; SysV: 16-byte alignment at the call

    mov edi, [boot_magic]
    mov esi, [boot_info]
    call kernel_main

    cli
.hang:
    hlt
    jmp .hang
