# initialD
`initialD` is VNU's userspace init system and is intended to run as PID 1.

Boot contract: the kernel starts `/sbin/initialD`. initialD enumerates `/etc/initialD/` and executes startup scripts in lexical order once `opendir/readdir` and `fork/execve` are available.

For the current POSIX v0.1 stage this directory and script format are part of the system image contract; script execution is intentionally deferred until POSIX v0.4.
