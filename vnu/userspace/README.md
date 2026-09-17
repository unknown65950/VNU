# VNU userspace

Programs are built on the host with `tools/vcc` + `tools/vld` (freestanding
i386 + vlibc), then embedded into the kernel image for the prototype stage
(no initrd yet).

```bash
# from repository root
./tools/build_userspace.sh
# rebuild kernel ISO after headers update
cd vnu && rm -rf kernel/build && ./build_iso.sh
```

## Boot flow

1. Kernel: GDT, IDT, VFS, process table  
2. `run_program("/bin/vash")` — userspace shell as PID 1  
3. On `exit` / after `execve` of a one-shot tool, kernel **respawns** `/bin/vash`  

## Programs

| Path        | Source                            |
|-------------|-----------------------------------|
| `/bin/vash` | `userspace/vash/vash.c`           |
| `/bin/hello`| `userspace/examples/hello/`       |
| `/bin/man`  | `userspace/man/man.c`             |
| `/bin/*`    | `userspace/vibecoreutils/<name>.c` (one binary per command) |

Native kernel console remains as fallback if init fails.
