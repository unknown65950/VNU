# VNU freestanding coreutils (alternative to host `vibecoreutils/`)

Host tree `../../vibecoreutils/` uses STL and is **not** linked into the kernel.
This directory is the VNU port: **one binary per command**, built with `vcc`
(each `.c` is a standalone program; shared helpers live in `cu.h`).

```bash
./tools/build_userspace.sh
```

Commands: `echo`, `true`, `false`, `pwd`, `cat`, `ls`, `mkdir`, `rm`,
`touch`, `uname`, `clear`, `wc`, `head`, `tail`, `grep`, `sort`, `cp`, `mv`,
`basename`, `dirname`, `seq`.

`vash` (the shell) and `man` live in their own directories
(`userspace/vash/`, `userspace/man/`).
