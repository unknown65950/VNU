# VNU host toolchain

| Tool | Role |
|------|------|
| `vcc` | C compiler → i386 freestanding + vlibc |
| `v++` | C++17, no exceptions/RTTI, same link model |
| `vld` | linker (`ld -m elf_i386` + crt0 + libvlibc.a) |
| `build_userspace.sh` | build + embed ELFs into kernel headers |

```bash
./tools/vcc  file.c   -o prog
./tools/v++  file.cpp -o prog
./tools/build_userspace.sh
```

Memory map for userspace ELFs (`user.ld`):
- `0x400000` text/data
- `0x600000` stacks
- `0x700000`–`0x800000` `brk` heap

C++: include `<vlibc/...>` only — no libstdc++/STL.
