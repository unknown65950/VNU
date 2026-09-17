# VNU freestanding coreutils (alternative to host `vibecoreutils/`)

Host tree `../../vibecoreutils/` uses STL and is **not** linked into the kernel.
This directory is the VNU port: one multi-call binary built with `vcc`.

```bash
./tools/build_userspace.sh
```

Applets: `vash`, `echo`, `ls`, `cat`, `pwd`, `mkdir`, `rm`, `touch`, `uname`, `true`, `false`, `clear`.
