/* linker.ld — размещаем ядро по адресу 1 MiB, как ожидает Multiboot2. */
/* Форма без BLOCK()/ALIGN(...)-в-адресной-экспрессии совместима и с
 * GNU ld, и с lld (Termux). */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    . = ALIGN(4K);
    .text :
    {
        *(.multiboot)
        *(.text)
    }

    . = ALIGN(4K);
    .rodata :
    {
        *(.rodata)
    }

    . = ALIGN(4K);
    .data :
    {
        *(.data)
    }

    /* Zero-initialised data lives far above the loaded image, in a region
     * that every address space shares. The kernel's .bss is a few MiB and
     * would otherwise end past 4 MiB — the user app/stack window that
     * create_address_space() remaps private per process — so kernel
     * globals there would fault the moment the first user process runs.
     * bss.ld zeroes this range at boot before any global is touched. */
    . = 0x00A00000;
    .bss :
    {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        __bss_end = .;
    }
}
