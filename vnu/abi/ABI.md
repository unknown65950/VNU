# VNU Userspace ABI v1

Architecture: i386, little-endian, 32-bit pointers.

## System calls
`int $0x80` enters the kernel. `eax` contains the syscall number. Arguments are
`ebx`, `ecx`, `edx`, `esi`, `edi` in that order. Return value is in `eax`.
Failures are negative `-VNU_E*` values. Numbers are append-only and ABI v1 values
must never be renumbered.

### Stable (implemented / reserved by kernel)

| #  | Name   |
|----|--------|
| 0  | read   |
| 1  | write  |
| 2  | open   |
| 3  | close  |
| 4  | exit   |
| 5  | lseek  |
| 6  | stat   |
| 7  | fstat  |
| 8  | brk    |
| 9  | getpid |
| 10 | chdir  |
| 11 | getcwd |
| 12 | fork   |
| 13 | execve |
| 14 | getuid |
| 15 | getgid |
| 16 | mkdir  |
| 17 | rmdir  |
| 18 | unlink |
| 19 | getdents |
| 20 | waitpid |
| 21 | pipe   |
| 22 | dup    |
| 23 | dup2   |
| 24 | kill   |
| 25 | uname  |
| 26 | getppid |
| 27 | isatty |
| 28 | spawn  |
| 29 | blkcount |
| 30 | install |
| 31 | setuid |
| 32 | setgid |
| 33 | chmod  |
| 34 | chown  |
| 35 | reboot |
| 36 | time   |
| 37 | uptime |
| 38 | ping   |
| 39 | netinfo |
| 40 | resolve |
| 41 | socket |
| 42 | connect |
| 43 | send |
| 44 | recv |
| 45 | netclose |
| 46 | bind |
| 47 | listen |
| 48 | accept |
| 49 | audio_open |
| 50 | audio_set_fmt |
| 51 | audio_write |
| 52 | audio_drain |
| 53 | audio_close |
| 54 | audio_pending |
| 55 | audio_pause |
| 56 | audio_reset |
| 57 | dnd_declare |

`stat`/`fstat` report owner/group and permission bits: `st_uid`, `st_gid`,
and the low 9 bits of `st_mode` are the `rwx` bits. `chown(path, uid, gid)`
with `-1` leaves a field unchanged; only root may chown.

### Notes on argument shapes
- `blkcount(ebx)` — returns the number of ATA disks the installer sees.
- `install(ebx, ecx)` — `ebx` is the target ATA disk index; `ecx` is the
  partition size in MiB (0 = as much as the disk/FAT16 allows). Returns 0
  or a negative errno. Root only — the write is destructive.
- `reboot()` — triggers a system reset via the 8042 keyboard controller.
  Root only.
- `time()` — returns the RTC wall-clock time as whole seconds since local
  midnight (0..86399). BCD-aware; the kernel's only clock is the CMOS RTC.
- `uptime()` — returns whole seconds since boot, measured as a delta of
  `time()` against a boot-time snapshot (so it wraps every local midnight;
  one-second resolution, no timer interrupt). Backs the analog-clock GUI
  apps' stopwatch/timer elapsed-time readings.
- `ping(ebx, ecx)` — `ebx` is the target IPv4 address as a
  big-endian uint32 (10.0.2.2 = `0x0A000202`), `ecx` the timeout in
  milliseconds (kernel clamps to 10..2000).
  Returns the round-trip time in ms on success, or
  `-VNU_EIO` (no NIC), `-VNU_EHOSTUNREACH` (ARP resolution failed),
  `-VNU_ETIMEDOUT` (host did not answer in time). Pinging the machine's
  own address returns 0 without sending anything.
- `netinfo(ebx)` — writes a `vnu_netinfo` struct to the caller's buffer:
  `mac[6]`, `ip`, `mask`, `gw` (all IPs big-endian uint32s) and `up`
  (1 = the kernel NIC is initialized). Returns 0, or `-VNU_EIO` if there
  is no NIC.
- `resolve(ebx, ecx)` — `ebx` points to a NUL-terminated host name,
  `ecx` to a uint32 that receives the resolved IPv4 (big-endian).
  Resolution checks `/etc/hosts` (`ip alias...` lines) first, then sends
  a DNS A query to each `nameserver` listed in `/etc/resolv.conf`,
  tried in order; with no entry the default server is `1.1.1.1`.
  Returns 0, or `-VNU_ENOENT` (name has no A record / not in hosts),
  `-VNU_EIO` (no NIC), `-VNU_EHOSTUNREACH` (no route), `-VNU_ETIMEDOUT`
  (no answer in time).
- `socket(ebx, ecx)` — opens a TCP socket. `ebx` is the address family
  (2 = AF_INET), `ecx` the type (1 = SOCK_STREAM). Returns a small
  non-negative socket handle, or `-VNU_EMFILE` (24) when the fixed
  8-slot socket table is full. Combined with `bind`/`listen`/`accept`
  a socket becomes a server; on its own the handle talks to a remote
  end via `connect`/`send`/`recv`.
- `connect(ebx, ecx, edx)` — `ebx` is the socket handle, `ecx` the
  remote IPv4 as a big-endian uint32, `edx` the TCP port in host byte
  order. Performs ARP resolution, the three-way handshake and blocks
  until established. Returns 0, or `-VNU_EIO`/`-VNU_EHOSTUNREACH`
  (ARP), `-VNU_ETIMEDOUT` (handshake did not complete in time),
  `-VNU_ECONNREFUSED` (RST).
- `send(ebx, ecx, edx, esi)` — sends `edx` bytes from the caller's
  buffer `ecx` on the socket, with `esi` a timeout in ms. Blocks until
  all bytes are buffered and pushed. Returns the byte count, or
  `-VNU_ETIMEDOUT`/`-VNU_ECONNRESET`/`-VNU_ENOTCONN`.
- `recv(ebx, ecx, edx, esi)` — receives up to `edx` bytes into buffer
  `ecx`, `esi` timeout in ms. Returns bytes read, 0 on orderly close
  (FIN), or `-VNU_ETIMEDOUT`/`-VNU_ECONNRESET`/`-VNU_ENOTCONN`.
- `netclose(ebx)` — closes a socket (FIN if established) and frees its
  slot. Returns 0.
- `bind(ebx, ecx)` — pins the local port of a fresh (not yet
  listening/connected) socket, `ecx` the port in host byte order.
  Required before `listen`. Returns 0, `-VNU_EADDRINUSE` (98) when
  another open socket already holds the port, `-VNU_EINVAL` if the
  socket is already bound/connected or `ecx` is 0 (ephemeral
  assignment is not supported; `socket()` already picked an
  auto-assigned port).
- `listen(ebx, ecx)` — turns the socket into a listener on its local
  port; `ecx` is the backlog (clamped to 1..4, pending half-open
  handshakes + completed connections waiting for accept). Returns 0,
  or `-VNU_EINVAL` if the socket is not a fresh, unbound one... an
  unbound-but-`socket()`ed handle listens on its auto-assigned port.
- `accept(ebx, ecx, edx, esi)` — waits (up to `esi` ms, clamped
  20..20000) for a completed incoming handshake on the listening
  socket `ebx`; writes the peer's IPv4 (big-endian uint32) to `*ecx`
  and port (host order) to `*edx` (either may be NULL), and returns a
  new socket handle for the connection. Returns the handle, or
  `-VNU_EINVAL` if `ebx` is not listening, `-VNU_ETIMEDOUT` when no
  connection arrived in time. The accepted handle talks to the peer
  with the same `send`/`recv`/`netclose` calls; only one pending
  connection is served per accept call.
- `audio_open()` — opens the kernel's single AC'97 playback device
  (QEMU `-soundhw ac97`, PCI 8086:2415). Returns a non-negative handle,
  or `-VNU_ENODEV` (19, no sound hardware), `-VNU_EBUSY` (device already
  open). Session state (descriptor ring, format, position) starts fresh.
- `audio_set_fmt(ebx, ecx, edx)` — selects the PCM format to play:
  `ebx` = sample rate in Hz (8000..48000), `ecx` = channels (1 or 2),
  `edx` = bits per sample (8 or 16). Must be called after `audio_open`
  and before `audio_write`. Programs the codec's sample-rate register
  (`PCM_Front_DAC_Rate`); the hardware itself only plays 16-bit stereo,
  so the kernel converts mono and/or 8-bit input to 16-bit stereo while
  feeding the DMA ring. Returns 0, or `-VNU_EINVAL` for unsupported
  values, `-VNU_EIO` when the device is not open.
- `audio_write(ebx, ecx, edx)` — submits `edx` bytes of little-endian
  PCM from user buffer `ecx` (the format given to `audio_set_fmt`).
  **Non-blocking**: the driver copies only what fits into its ~1.5 s
  DMA ring and returns the number of *input* bytes consumed (0 when the
  ring is full — poll and retry later; a paused device accepts nothing).
  The GUI player relies on this: a windowed task must never stall the
  cooperative desktop scheduler. Returns the byte count, or
  `-VNU_EIO` (device not open), `-VNU_EINVAL` (no format set / bad
  arguments).
- `audio_drain()` — blocks (bounded spin) until all currently queued PCM
  has played out of the ring. For console-style producers that queue a
  whole clip and wait; a windowed task should poll `audio_pending`
  instead. Returns 0, or `-VNU_EIO` if the stream is not open.
- `audio_pending()` — bytes of queued PCM still to play, counted in
  16-bit stereo output bytes (divide by 4 and multiply by the input
  sample size to convert to source-file bytes). Lets a player render a
  progress bar and detect end-of-track without blocking. Returns the
  count, or `-VNU_EIO` if the stream is not open.
- `audio_pause()` — halts the DMA engine where it is; queued PCM stays
  buffered and the position is preserved (resume = `audio_reset()` +
  re-feed from the saved position; the unplayed tail of the ring is
  dropped by the reset, at most ~1.5 s). Returns 0, or `-VNU_EIO` if
  the stream is not open.
- `audio_reset()` — stops the engine, drops queued PCM and rewinds the
  ring so the next `audio_write` starts fresh. Returns 0, or `-VNU_EIO`
  if the stream is not open.
- `audio_close()` — stops playback, resets the bus-master DMA state and
  closes the session, releasing the device for the next `audio_open`.
  Returns 0, or `-VNU_EIO` if the device was never opened.

### Removed

Not applicable — numbers are never reused; old gaps stay reserved.

### Future (append-only, start at 58)

Unsupported calls return `-VNU_ENOSYS`.

`dnd_declare(ebx=path)` (57) is called by a gfx-windowed app (the file
manager) when the left button is pressed over one if its files: it hands
the kernel the full VFS path of that item so the GUI can arm a drag. The
kernel tracks the press only while the button is held and the item
belongs to that press, so misshots never stick.

### Errors added with networking

`VNU_ETIMEDOUT` (110) and `VNU_EHOSTUNREACH` (113) were added in the
same change as the `ping`/`netinfo` syscalls; values match Linux's.
`VNU_ECONNRESET` (104), `VNU_ENOTCONN` (107) and `VNU_ECONNREFUSED`
(111) were added with the TCP socket syscalls; values match Linux's.
`VNU_EADDRINUSE` (98) was added with `bind`; value matches Linux's.
`VNU_ENODEV` (19) was added with the audio syscalls to report missing
sound hardware; value matches Linux's.

## Interrupt gate
Vector `0x80`, selector `0x08`, DPL=3, present 32-bit interrupt gate (`0xEE`).
The kernel must validate all userspace pointers before dereferencing them once
ring-3 address spaces are enabled.
