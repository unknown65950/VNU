# hello — example VNU userspace program

```bash
# from repo root (parent of tools/ and vlibc/)
./tools/vcc vnu/userspace/examples/hello/hello.c -o hello
# binary is ELF32 freestanding, linked with vlibc
file hello
```

To run inside VNU you need an ELF loader + processes (roadmap). For now
the native shell recognizes `vcc` / `vld` / `hello` as reserved commands.
