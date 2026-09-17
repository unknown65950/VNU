/* linker.ld — размещаем ядро по адресу 1 MiB, как ожидает Multiboot2. */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }

    .data BLOCK(4K) : ALIGN(4K)
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
    .bss BLOCK(4K) : ALIGN(4K)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        __bss_end = .;
    }
}
