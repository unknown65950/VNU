# VNU freestanding coreutils (alternative to host `vibecoreutils/`)

Host tree `../../vibecoreutils/` uses STL and is **not** linked into the kernel.
This directory is the VNU port: **one binary per command**, built with `vcc`
(each `.c` is a standalone program; shared helpers live in `cu.h`).

```bash
./tools/build_userspace.sh
```

Commands: `echo`, `true`, `false`, `pwd`, `cat`, `ls`, `mkdir`, `rm`,
`touch`, `uname`, `clear`, `wc`, `head`, `tail`, `grep`, `sort`, `cp`, `mv`,
`basename`, `dirname`, `seq`, `df`, `ping`, `ip`, `fetch`,
`wallpaper`, `nohup`.

The clipboard is not a coreutils command: it is the `clipboard` desktop
app over the `/dev/clipboard` device, and a shell reaches the same text
with redirection (`cat /dev/clipboard`, `echo ... > /dev/clipboard`).

`vash` (the shell) and `man` live in their own directories
(`userspace/vash/`, `userspace/man/`).
