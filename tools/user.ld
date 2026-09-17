/* VNU userspace — load at 4 MiB */
ENTRY(_start)
SECTIONS
{
    . = 0x400000;
    .text ALIGN(16) : { *(.text .text.*) }
    .rodata ALIGN(16) : { *(.rodata .rodata.*) }
    .data ALIGN(16) : { *(.data .data.*) }
    .bss ALIGN(16) : { *(COMMON) *(.bss .bss.*) }
    /DISCARD/ : { *(.note.*) *(.eh_frame*) *(.comment) }
}
