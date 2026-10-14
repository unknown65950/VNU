# The architecture layer

`arch/<arch>/` is the only place in the kernel that knows what CPU it is
running on. Everything else - the scheduler, the VFS, the GUI, the
drivers - goes through the interfaces in `include/vnu/arch/` and compiles
unchanged on another machine. The goal is that a port is "add a
directory", not "sprinkle `#ifdef`s through the tree".

```
vnu/kernel/
  include/vnu/arch/   the interfaces (what the core is allowed to call)
    boot.h            CPU bring-up: paging off, GDT/IDT, interrupt controller
    cpu.h             interrupt masking, halt/spin hints, atomics, FPU borrow, reboot
    io.h              programmed ("port") I/O
    serial.h          the early debug log
    rtc.h             the platform wall clock
    mmu.h             the address-space registers the core touches directly
    context.h         the user register context and the frames built for it
    switch.h          the cooperative coroutine switch and the preemption clock
  arch/
    i386/             the only port so far
      include/vnu/arch/context_defs.h  the i386 Registers/TrapFrame layout
      boot/           boot.s, gdt.cpp - CPU bring-up
      syscall/        idt.cpp, syscall.cpp, syscall.s - traps and the syscall gate
      cpu.cpp         vnu/arch/cpu.h
      io.cpp          vnu/arch/io.h
      serial.cpp      vnu/arch/serial.h
      rtc.cpp         vnu/arch/rtc.h
      mmu.cpp         vnu/arch/mmu.h
      context.cpp     vnu/arch/context.h
      switch.cpp      vnu/arch/switch.h
      paging.cpp      the page tables behind vnu::paging
      switch.s        user entry/exit
      ctxswitch.s     coroutine switch and the PIT timer ISR
```

## What the core is allowed to do

The core includes the headers above and calls the declared functions. It
does **not** contain `cli`/`sti`/`hlt`, `in`/`out`, `mov %crN`,
`fxsave`/`fxrstor`, `cpuid`, or a hand-rolled test-and-set. A grep for
`asm` outside `arch/` should come back empty, and a grep for a register
name (`eip`, `eax`, `eflags`, ...) should only hit a comment. The
coroutine switch (`vnu_swtch` and friends, once called straight from
`proc/process.cpp` and `gui/wintask.cpp`) now goes through `switch.h`,
so the core parks a plain `uint32_t` stack slot and names no register.
The one thing that still names the machine in the core is:

- the device drivers under `drivers/`: they are PC hardware (the 8259
  PIC, ATA, PS/2, the Bochs VBE interface), which is a second axis from
  the CPU. They already take all port access through `vnu/arch/io.h`, so
  the compiler never sees raw `in`/`out` outside `arch/`.

## Adding a port

1. Add `arch/<arch>/` with an implementation of every header in
   `include/vnu/arch/`. The functions in `boot.h`, `cpu.h`, `io.h`,
   `serial.h`, `rtc.h`, `mmu.h`, `context.h` and `switch.h` are the
   contract; `serial.h` may be a no-op and `rtc.h` may read any clock
   the machine has. `context.h` is two parts: the accessors are fixed,
   and the concrete `Registers`/`TrapFrame` layout comes from a
   port-provided `include/vnu/arch/context_defs.h` (see
   `arch/i386/include/`), which the build reaches through
   `arch/${VNU_KERNEL_ARCH}/include`.
2. Provide the boot path (`boot.s`/`boot.cpp` or equivalent) and the
   trap/syscall entry in the same directory, and build them the way that
   architecture expects: implement `boot.h` (make paging off, load the
   descriptor table, install the vector table, bring up the interrupt
   controller). `arch/i386/syscall/syscall.s` and `arch/i386/switch.s`
   show the i386 convention: a `TrapFrame` on the stack and
   `vnu_syscall_dispatch`/`vnu_timer_tick` called from asm. The vector
   table itself and the `vnu_syscall_entry` gate are a port detail - the
   core only knows `idt_init()` and, through `context.h`, the frames the
   entry path builds.
3. Implement the page tables. `vnu::paging`'s API
   (`include/vnu/paging.h`) stays the same; `arch/i386/paging.cpp` is
   the reference for the 32-bit two-level layout.
4. Teach the build: `CMakeLists.txt` selects sources from
   `arch/${VNU_KERNEL_ARCH}/`, so a new port is mostly a matter of
   pointing that at the new directory and giving it its own toolchain
   flags next to `VNU_ARCH_FLAG`. Set `VNU_KERNEL_ARCH` (a CMake cache
   variable) to the directory name.
5. Bump `VNU_ARCH` in `include/vnu/version.h` so `uname` reports the new
   machine.

## Status

`i386` is the only port, and it is complete for the interface as it
stands today. A second port would immediately show up the piece that is
still i386-shaped (the PC drivers named above), which is exactly the
point of keeping the surface this small.
