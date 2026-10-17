# initialD

`initialD` is VNU's userspace init system and runs as PID 1
(`/sbin/init`, built from `vnu/userspace/init/initialD.c`).

Boot contract: the kernel starts `/sbin/init`. initialD enumerates
`/etc/initialD/` and executes the `*.init` startup scripts in lexical
order (each waited for; a failing script is reported and does not stop
the boot), then starts `/bin/vash` on the console and respawns it
whenever it exits. Orphaned processes are adopted by init and reaped.
