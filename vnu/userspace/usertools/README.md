# VNU standalone account and install tools

One separate binary per command, built with `vcc`; shared helpers live in
`ue.h` as `static inline` (each binary is one self-contained `.c`).

Commands: `id`, `whoami`, `groups`, `useradd`, `passwd`, `su`, `install`.

These used to be vash builtins; they are now standalone programs that read the
same `/etc/passwd`, `/etc/group` and `/tmp/.session` as the shell, and keep the
same behaviour. `su` authenticates against `/etc/passwd`, switches uid/gid and
hands control to a fresh `/bin/vash` as the target user.