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
    i386/             the port
      include/vnu/arch/context_defs.h  the i386 Registers/TrapFrame layout
      boot/           boot.s, gdt.cpp - CPU bring-up
      syscall/        idt.cpp, syscall.cpp, syscall.s - traps and the syscall gate
      drivers/        the PC chipset: 8042 kbd/mouse, ATA, PCI, VBE/VGA,
                      AC'97, e1000, virtio-gpu, /dev/fb0
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
    x86_64/           the long-mode bring-up port (unfinished; see Status)
      include/vnu/arch/context_defs.h  the 64-bit Registers/TrapFrame layout
      boot/           boot.s (32-bit entry -> long mode), gdt.cpp
      syscall/        idt.cpp (16-byte gates), syscall.cpp, syscall.s
      drivers/        a copy of the i386 PC chipset (see Status)
      cpu.cpp io.cpp serial.cpp rtc.cpp mmu.cpp context.cpp switch.cpp
      paging.cpp      four-level page tables
      switch.s ctxswitch.s
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
The machine's devices work the same way: the core reads a disk through
`vnu::blockdev`, sends a frame through `vnu::net`/`vnu::tcp`, and paints
the screen through `vnu::vgfx` - each declared in `include/vnu/`. The
implementations of those headers are the port's: the PC chipset (8042
keyboard and mouse, ATA, PCI, Bochs VBE/VGA, AC'97, e1000, virtio-gpu,
`/dev/fb0`) lives in `arch/i386/drivers/`. Port access still goes through
`vnu/arch/io.h`, so the compiler never sees raw `in`/`out` outside
`arch/`.

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
4. Provide the machine's devices behind the core's device headers
   (`blockdev.h`, `net.h`, `tcp.h`, `fb.h`, `kbd.h`, `ps2mouse.h`,
   `vga_gfx.h`, `virtio_gpu.h`, `ata.h`, `pci.h`, `netcore.h`,
   `audio.h`). `arch/i386/drivers/` is the PC set; a machine that shares
   the chipset can reuse those files, one that does not provides its own
   (or none, for a device the machine simply has not got). `x86_64/`
   currently carries a copy of the i386 PC set, adapted only where a
   32-bit assumption had to become a `uintptr_t`.
5. Teach the build: `CMakeLists.txt` selects sources from
   `arch/${VNU_KERNEL_ARCH}/`, so a new port is mostly a matter of
   pointing that at the new directory and giving it its own toolchain
   flags next to `VNU_ARCH_FLAG`. Set `VNU_KERNEL_ARCH` (a CMake cache
   variable) to the directory name.
6. Bump `VNU_ARCH` in `include/vnu/version.h` so `uname` reports the new
   machine. `VNU_ARCH` defaults to the host's `VNU_KERNEL_ARCH` and can
   be overridden per build.

## Status

`i386` is the complete port: every header in `include/vnu/arch/` and the
PC chipset drivers under `arch/i386/drivers/`, reached by the core only
through the headers in `include/vnu/`.

`x86_64` is a bring-up port and **is not finished**. What works today:

* it builds and links as an `ELF64` image (`VNU_KERNEL_ARCH=x86_64`);
* its `boot.s` walks the CPU from the 32-bit Multiboot2 entry into long
  mode (a 2 MiB-page identity map, PAE, `EFER.LME`) and calls
  `kernel_main` in 64-bit code;
* four-level paging (`arch/x86_64/paging.cpp`), the 16-byte IDT, the PIC
  and the timer all come up, and `kernel_main` runs to the scheduler: the
  serial log shows the network self-test completing, and the running
  kernel is confirmed 64-bit by `info registers` (CS64).

What does not work yet, and why a full port is more than a scaffold:

* **The userspace is 32-bit.** Every program in `sysroot/` is an
  `EM_386` ELF, so `vnu::elf::load` returns a 32-bit entry and the port
  jumps to it in long mode, which faults (the port reports this on the
  serial log and stops). Running it needs either a 64-bit userspace
  toolchain or a compatibility-mode entry path: a `GDT` with 32-bit user
  segments, a `TSS64` with `rsp0`, and an `iretq` into compat mode.
* **The syscall dispatcher is a stub.** `arch/x86_64/syscall/syscall.cpp`
  returns `-ENOSYS`; the real dispatcher (one case per `vnu/abi.h` call)
  still lives in `arch/i386/syscall/syscall.cpp`. Porting it is mostly a
  matter of the trap-frame field names, and is only meaningful once the
  userspace can actually run.
* **The trap frame is 64-bit.** `context_defs.h` describes a native
  long-mode frame; the 32-bit processes' registers, signal frames and the
  preemption frame (`vnu_timer_tick(uint32_t frame_esp)` walks the i386
  `pushad` layout) are not reconciled with it yet.

The core it shares is architecture-neutral by construction: the only
pointer-width assumptions left were a handful of
`reinterpret_cast<uint32_t>(pointer)` stores, now `uintptr_t` (see the
`kernel must stay portable` rule in `AGENTS.md`). The remaining work is
all inside `arch/x86_64/` plus the userspace toolchain decision above.
