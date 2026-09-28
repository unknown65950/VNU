/* linker.ld — place the kernel at 1 MiB, as Multiboot2 expects. */
/* The form without BLOCK()/an ALIGN() inside the address expression
 * works with both GNU ld and lld (Termux). */

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

    /* The PMM pool starts where .bss ends (mm/pmm.cpp reads __bss_end),
     * so a .bss that grew past the pool's top would leave the kernel with
     * no memory to allocate at all: every frame request fails, and the
     * failure is at runtime, on a machine, far from the cause. Fail the
     * link instead, here, where the number that grew is visible. */
    ASSERT(__bss_end < 0x01F70000,
           "kernel .bss overlaps the PMM pool: move POOL_END up or shrink .bss")
}
