/* linker.ld - place the kernel at 1 MiB, as Multiboot2 expects. The
 * x86_64 counterpart of ../i386/linker.ld; the .bss placement and the
 * pool-overlap assertion are the same shape, only the output format
 * differs. */

OUTPUT_FORMAT(elf64-x86-64)
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

    /* See ../i386/linker.ld: .bss lives high, in a region every address
     * space shares, so kernel globals never fall inside the per-process
     * user window that create_address_space() remaps. The boot stub
     * zeroes this range before any global is touched. */
    . = 0x00A00000;
    .bss :
    {
        __bss_start = .;
        *(COMMON)
        *(.bss)
        __bss_end = .;
    }

    ASSERT(__bss_end < 0x01F70000,
           "kernel .bss overlaps the PMM pool: move POOL_END up or shrink .bss")
}
