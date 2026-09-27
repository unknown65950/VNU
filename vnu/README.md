# VNU (Vibe's Not UNIX!) — the OS tree

Everything that ends up in a running VNU lives under this directory: the
kernel, the userspace sources, the ABI description and the two scripts
that build and launch the image. For the current feature set and the
development rules, read the top-level `README.md` and `AGENTS.md`; this
file describes how the pieces fit together.

## Layout

```
vnu/
├── kernel/                     the kernel: freestanding C++20, CMake build
│   ├── CMakeLists.txt          builds kernel.elf
│   ├── linker.ld               places the kernel at 1 MiB
│   ├── arch/i386/boot/boot.s   Multiboot2 header + _start (NASM)
│   ├── arch/i386/              GDT/IDT, PIC, paging, syscall entry
│   ├── console/ drv/ mm/ proc/ gui/ install/ host/ fs/
│   ├── kernel/kernel.cpp       kernel_main: bring-up order
│   └── proc/embedded_*.h       userspace ELFs embedded into kernel.elf
├── userspace/                  sources compiled by tools/vcc
│   ├── vash/                   the shell
│   ├── vibecoreutils/          one .c per coreutils command
│   ├── usertools/              id, whoami, su, ...
│   ├── gui/                    desktop apps, picview, prefs, ...
│   ├── editors/                vedit
│   ├── man/                    the manual (self-contained, one binary)
│   ├── vcc/                    the compiler that also runs inside VNU
│   ├── vnu/                    system information (fetch/version/size)
│   ├── examples/               hello, echoserver, tlsserver, ...
│   └── rootfs/                 /etc, /root, /home shipped into the VFS
├── abi/ABI.md                  the syscall ABI, one entry per call
├── build_iso.sh                cmake build + grub-mkrescue -> vnu.iso
├── run.sh                      launch vnu.iso in QEMU
└── README.md
```

## Build and run

From the repository root, through the Makefile:

```bash
make doctor        # check the host has what it needs
make iso           # userspace + kernel -> vnu/vnu.iso
make run           # QEMU window
make run-headless  # no graphics, serial console in the terminal
make run-gpu       # the desktop on a virtio-gpu display
make test          # automated guest tests (tools/qemu_test.py)
```

The scripts stay a working path of their own, and are what the Makefile
calls:

```bash
./build_iso.sh     # cmake build + grub-mkrescue -> vnu.iso
./run.sh           # launch in QEMU
./run.sh --headless
./run.sh --help
```

Ctrl+C or closing the QEMU window stops the emulation.

## Dependencies (Ubuntu/Debian)

```bash
apt-get install -y build-essential cmake nasm \
    qemu-system-x86 grub-pc-bin grub-common xorriso mtools \
    gcc-multilib g++-multilib
```

`gcc-multilib`/`g++-multilib` let the ordinary 64-bit host g++ build
32-bit code (`-m32`). This is **not** a real cross-compiler — it is enough
for an x86 kernel on an x86 host as long as the kernel code touches
nothing of the host libc/ABI (see the warning at the top of
`kernel/CMakeLists.txt`).

## How a boot works

1. GRUB finds the Multiboot2 header in `arch/i386/boot/boot.s` (magic
   number, checksum, tags), switches the CPU into 32-bit protected mode
   and jumps to `_start` with `eax` = the Multiboot2 magic and `ebx` =
   pointer to the multiboot info.
2. `_start` sets up a stack, zeroes `.bss` and calls
   `kernel_main(magic, mbi)`.
3. `kernel_main` brings the machine up in a fixed order — GDT, IDT, PIC
   remap, paging, PMM, heap, console, drivers, VFS, processes — because
   everything after the first `sti` assumes all of it is in place.
4. The first user process is `/sbin/init` (an alias of `vash`), which
   prints the login prompt and starts the session.
5. The kernel also mirrors its output to COM1 (`0x3F8`), which is how
   `make run-headless` and the test harness see the boot log.

## The ABI

`abi/ABI.md` is the source of truth: one entry per syscall, with the
number, the arguments, the return value and the errno values, in append
order. The numbers live in `kernel/include/vnu/abi.h` and are mirrored in
`vlibc/include/vnu/abi.h`. A new syscall is not done until both headers
and `ABI.md` carry it in the same change.

## Manuals

Every user-visible command has a manual page: the vash builtins, each
coreutils binary, the usertools, the GUI apps, `vcc` and `vnu`. The
`man` binary is self-contained (there is no `/usr/share/man` filesystem),
so the pages live in the `pages[]` table in
`userspace/man/man.c`. Read them with:

```bash
man              # every documented command
man ls           # one page
man vash man su  # several at once
```

The rule that makes a page mandatory is in `AGENTS.md` at the repository
root, and is repeated in a comment at the top of `userspace/man/man.c`.
