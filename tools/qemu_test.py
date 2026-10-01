#!/usr/bin/env python3
"""VNU guest test harness: boot the ISO in QEMU and assert what it prints.

The kernel mirrors everything it writes (kernel log and userspace fd1/fd2)
to COM1, so with `-serial file:<log>` the whole session ends up in one
file on the host. Input is a different story: the keyboard driver reads
the PS/2 controller, not COM1, so a test cannot simply pipe text into the
console. The harness therefore types into the guest through the QMP
`human-monitor-command sendkey` bridge, exactly like a person at the
keyboard, and synchronises on the shell prompt appearing again in the log.

    ./tools/qemu_test.py                 # the default suite, ISO boot
    ./tools/qemu_test.py --gpu           # same suite on a virtio-gpu display
    ./tools/qemu_test.py --install       # install to a disk, boot it, check
    ./tools/qemu_test.py --list          # names of every case
    ./tools/qemu_test.py --only vnu      # run cases whose name matches

Exit status is 0 when every case passed, 1 otherwise; the serial log is
kept on failure under /tmp (or wherever --log points) for inspection.
"""

import argparse
import json
import os
import re
import shutil
import socket
import string
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The guest shell prompt as it appears in the serial log, prompt included:
# user@hostname:cwd$ , where the host name comes from /etc/hostname (a
# live image is "vnu", an installed disk the name it was installed with).
PROMPT = re.compile(r"[a-z_]+@[a-z0-9][a-z0-9_.-]*:[^\n]*[$#] ")
# The same prompt, but as the *end* of what has been printed: the shell
# echoes each command as it is typed, and that echo is a prompt line, so
# "a prompt showed up" is true the moment the typing ends - long before
# the command has run. See Guest.wait_ready().
PROMPT_END = re.compile(PROMPT.pattern.rstrip() + "$", re.MULTILINE)

# QEMU QKeyCode names for a US layout, so a test can type any of the
# commands below (quotes, pipes, semicolons) exactly as a user would.
KEYMAP = {
    " ": "spc", "\n": "ret", "\t": "tab",
    "-": "minus", "=": "equal", "[": "bracketleft", "]": "bracketright",
    "\\": "backslash", ";": "semicolon", "'": "apostrophe",
    "`": "grave_accent", ",": "comma", ".": "dot", "/": "slash",
    "!": "shift-1", '"': "shift-apostrophe", "#": "shift-3", "$": "shift-4",
    "%": "shift-5", "&": "shift-7", "(": "shift-9", ")": "shift-0",
    "*": "shift-8", "+": "shift-equal", ":": "shift-semicolon",
    "<": "shift-comma", ">": "shift-dot", "?": "shift-slash",
    "@": "shift-2", "^": "shift-6", "_": "shift-minus", "|": "shift-backslash",
    "{": "shift-bracketleft", "}": "shift-bracketright", "~": "shift-grave_accent",
}
for _c in string.digits:
    KEYMAP[_c] = _c
for _c in string.ascii_lowercase:
    KEYMAP[_c] = _c
for _c in string.ascii_uppercase:
    KEYMAP[_c] = "shift-" + _c.lower()

# Keys that have no character of their own. Written as <name> in a string
# handed to _type(), e.g. guest._type("<down><down>q"): the arrow keys are
# what a full-screen program (the man pager) is driven with, and the QEMU
# key names below are the ones its HMP sendkey understands.
SPECIAL_KEYS = {
    "<up>": "up", "<down>": "down", "<left>": "left", "<right>": "right",
    "<home>": "home", "<end>": "end", "<pgup>": "pgup", "<pgdn>": "pgdn",
    "<esc>": "esc", "<ret>": "ret", "<spc>": "spc", "<tab>": "tab",
    "<f1>": "f1", "<f2>": "f2", "<f3>": "f3", "<f4>": "f4",
    "<f5>": "f5", "<f6>": "f6", "<f7>": "f7", "<f8>": "f8",
    "<f9>": "f9", "<f10>": "f10", "<f11>": "f11",
    "<f12>": "f12",
    # A chord, not a character: Ctrl+C is the one key the console turns
    # into a signal, so it never reaches a program as a byte and there
    # is no KEYMAP entry that could type it as one.
    "<ctrl-c>": "ctrl-c",
}


class Qmp:
    """Minimal QMP client: one JSON object per line, one request at a time."""

    def __init__(self, path, timeout=20.0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(path)
        self.fp = self.sock.makefile("rwb")
        self._read()                      # greeting
        self.cmd("qmp_capabilities")

    def _read(self):
        line = self.fp.readline()
        if not line:
            raise RuntimeError("QMP closed the connection")
        return json.loads(line.decode())

    def cmd(self, execute, **arguments):
        request = {"execute": execute}
        if arguments:
            request["arguments"] = arguments
        self.fp.write((json.dumps(request) + "\n").encode())
        self.fp.flush()
        while True:
            msg = self._read()
            if "error" in msg:
                raise RuntimeError("QMP %s: %s" % (execute, msg["error"]))
            if "return" in msg:
                return msg["return"]

    def hmp(self, command_line):
        return self.cmd("human-monitor-command", **{"command-line": command_line})

    def close(self):
        try:
            self.fp.close()
            self.sock.close()
        except OSError:
            pass


class Guest:
    """One QEMU boot of a VNU image, driven from the serial log."""

    def __init__(self, iso=None, disk=None, gpu=False, log=None, boot="d",
                 mem=32):
        self.iso = iso
        self.disk = disk
        self.gpu = gpu
        self.boot = boot
        self.mem = mem
        self.log = log or os.path.join(tempfile.gettempdir(), "vnu-test.log")
        self.qmp_path = self.log + ".qmp"
        self.proc = None
        self.qmp = None
        self.qmp_waited = 0.0

    # ---- lifecycle ----------------------------------------------------

    def start(self):
        for path in (self.log, self.qmp_path):
            if os.path.exists(path):
                os.unlink(path)
        cmd = ["qemu-system-i386", "-m", str(self.mem), "-boot", self.boot,
               "-display", "none", "-no-reboot",
               "-serial", "file:" + self.log,
               "-monitor", "none",
               "-qmp", "unix:%s,server,nowait" % self.qmp_path]
        if self.iso:
            cmd += ["-cdrom", self.iso]
        if self.disk:
            cmd += ["-hda", self.disk]
        if self.gpu:
            cmd += ["-vga", "none", "-device", "virtio-vga"]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.STDOUT)

    def connect(self, timeout=30.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if os.path.exists(self.qmp_path):
                try:
                    self.qmp = Qmp(self.qmp_path)
                    return
                except (OSError, RuntimeError):
                    pass
            if self.proc.poll() is not None:
                raise RuntimeError("qemu exited early (rc=%s); see %s"
                                   % (self.proc.returncode, self.log))
            time.sleep(0.2)
        raise RuntimeError("QMP socket never appeared: %s" % self.qmp_path)

    def close(self):
        if self.qmp:
            self.qmp.close()
            self.qmp = None
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def __enter__(self):
        self.start()
        self.connect()
        return self

    def __exit__(self, *exc):
        self.close()

    # ---- log watching -------------------------------------------------

    def tail(self, frm):
        """Log text from offset `frm`, with the serial CRs normalized away
        so the expectations below can use plain `$` anchors."""
        try:
            with open(self.log, "r", errors="replace") as f:
                f.seek(frm)
                return f.read().replace("\r\n", "\n").replace("\r", "\n")
        except FileNotFoundError:
            return ""

    def wait(self, pattern, timeout=30.0, frm=0):
        """Block until `pattern` shows up in the log; return its offset."""
        regex = re.compile(pattern, re.MULTILINE)
        deadline = time.time() + timeout
        while time.time() < deadline:
            text = self.tail(frm)
            match = regex.search(text)
            if match:
                return frm + match.end()
            if self.proc.poll() is not None:
                raise RuntimeError("qemu exited while waiting for %r" % pattern)
            time.sleep(0.1)
        raise TimeoutError("timed out after %ss waiting for %r (log: %s)"
                           % (timeout, pattern, self.log))

    def wait_prompt(self, timeout=60.0, frm=0):
        return self.wait(PROMPT.pattern, timeout, frm)

    def wait_ready(self, frm=0, timeout=60.0):
        """Block until the shell has finished printing and is back at its
        prompt, and return the offset of that prompt.

        What ends a command is a prompt at the end of the output, not
        the first prompt after the typing: the shell echoes the command
        line as it is typed, so a search for "a prompt" is satisfied by
        the echo and the check then runs against output the command has
        not produced yet. One case then reads the previous case's text,
        and every case after it inherits that."""
        deadline = time.time() + timeout
        while True:
            text = self.tail(frm).rstrip()
            match = PROMPT_END.search(text)
            if match:
                return frm + match.end()
            if self.proc.poll() is not None:
                raise RuntimeError("qemu exited while waiting for the shell")
            if time.time() >= deadline:
                raise TimeoutError("timed out after %ss waiting for the "
                                   "shell to be ready again (log: %s)"
                                   % (timeout, self.log))
            time.sleep(0.1)

    # ---- input --------------------------------------------------------

    def _type(self, text, hold_ms=50, step=0.05):
        """Send one key at a time: HMP `sendkey` takes a '+'/'-' chord or
        key, and a batch would either be rejected (spaces are not a
        separator) or apply the shift of `shift-a` to the whole batch. One
        call per character is unambiguous, and the hold time keeps the
        i8042 buffer from being overrun by the guest's own polling.

        The 50 ms hold and the 50 ms between keys are not decoration.
        QEMU's PS/2 model hands the guest a scancode from a one-byte
        mailbox and refills it from a queue as the guest reads, and
        under a burst it gets that wrong: a byte that is already spent
        comes back a second time and the two behind it are dropped, so
        `cat /proc/gfx` arrives as `cat /procgfx`. It needs the guest to
        be keeping up, and right after a desktop session the guest is
        busy copying console frames around, which is exactly when it is
        not. Typing at a speed a person can manage keeps that queue
        shallow, so a failing check is the guest's and not the model's.

        A <name> token from SPECIAL_KEYS (e.g. <down>) stands for a key
        that is not a character, such as an arrow. Returns the number of
        keys sent."""
        keys, missing = [], []
        i = 0
        while i < len(text):
            if text[i] == "<":
                token = text[i:text.find(">", i) + 1]
                if token in SPECIAL_KEYS:
                    keys.append(SPECIAL_KEYS[token])
                    i += len(token)
                    continue
            if text[i] in KEYMAP:
                keys.append(KEYMAP[text[i]])
            else:
                missing.append(text[i])
            i += 1
        if missing:
            raise RuntimeError("no key mapping for %r" % sorted(set(missing)))
        for k in keys:
            reply = self.qmp.hmp("sendkey %s %d" % (k, hold_ms))
            if reply and "invalid" in reply:
                raise RuntimeError("sendkey %s: %s" % (k, reply))
            time.sleep(step)
        return len(keys)

    def type_line(self, text, expect=None, timeout=60.0):
        """Type `text` + Enter, then wait for `expect` to show up in the log.

        Defaults to the shell prompt, which is what ends a normal command.
        The login prompt needs its own pattern ("Password: "), since the
        password itself is not echoed."""
        frm = len(self.tail(0))
        self._type(text + "\n")
        time.sleep(0.2)
        if expect is None:
            return self.wait_prompt(timeout, frm)
        return self.wait(expect, timeout, frm)

    def sh(self, command, timeout=60.0):
        """Run `command`, return (output, offset_after_prompt)."""
        frm = len(self.tail(0))
        self._type(command + "\n")
        time.sleep(0.2)
        end = self.wait_ready(frm, timeout)
        return strip_echo(self.tail(frm), command), end

    def login(self, user="root", password="root", timeout=30.0):
        """Type through the login prompt (which is not a shell prompt)."""
        self.type_line(user, r"Password: ", timeout)
        self.type_line(password, None, timeout)


def strip_echo(chunk, command):
    """Drop the echoed command line and the trailing prompt from a chunk."""
    lines = chunk.splitlines()
    if lines and command[:20] in lines[0]:
        lines = lines[1:]
    while lines and not lines[-1].strip():
        lines.pop()
    if lines and PROMPT.match(lines[-1] + " "):
        lines.pop()
    return "\n".join(lines)


class Result:
    def __init__(self):
        self.passed = 0
        self.failed = []
        self.skipped = 0


def check(name, output, expects, forbids, result, verbose):
    problems = []
    for pattern in expects:
        if not re.search(pattern, output, re.MULTILINE):
            problems.append("missing /%s/" % pattern)
    for pattern in forbids:
        if re.search(pattern, output, re.MULTILINE):
            problems.append("unexpected /%s/" % pattern)
    if problems:
        result.failed.append((name, output, problems))
        print("FAIL %-18s %s" % (name, "; ".join(problems)))
        return False
    result.passed += 1
    if verbose:
        print("ok   %-18s" % name)
    return True


# ---------------------------------------------------------------------------
# The default suite. Each case is (name, command, expected, forbidden);
# `expected` patterns are regular expressions matched against the output of
# the command alone.
# ---------------------------------------------------------------------------

SUITE = [
    # -- the system information command ---------------------------------
    ("vnu-fetch", "vnu fetch",
     [r"VNU system information", r"^OS +: +VNU 0\.7 \(Canyon\)$",
      r"^Kernel +: +0\.7\.0$", r"^Host +: +vnu$", r"^Uptime +: +\d+ s$",
      r"^Shell +: +vash$",
      r"^Graphics +: +\w[\w-]* \d+x\d+ \d+bpp$", r"^VCC +: +0\.5$",
      r"^Build +: +\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$"], []),
    ("vnu-version", "vnu version",
     [r"VNU version information", r"^OS +: +VNU 0\.7$",
      r"^Kernel +: +0\.7\.0$", r"^ABI +: +1$", r"^Arch +: +i386$"], []),
    ("vnu-size", "vnu size",
     [r"VNU image size", r"^kernel +: .*\(1 object\)$",
      r"^userspace: .*\(4[0-9] objects\)$", r"^libraries: .*\(2 objects\)$",
      r"^fonts +: .*\(1 object\)$", r"^resources: .*\(1[0-9] objects\)$",
      r"^total +: "], []),
    ("vnu-version-flag", "vnu --version", [r"^vnu \(VNU\) 0\.7$"], []),
    ("vnu-help", "vnu --help",
     [r"Usage: vnu \[COMMAND\]\.\.\.", r"fetch", r"version", r"size",
      r"install +install VNU onto a disk"], []),
    ("vnu-unknown", "vnu bogus", [r"unknown command: bogus"], []),
    ("vnu-noargs", "vnu", [r"Usage: vnu"], []),

    # -- the facts behind it --------------------------------------------
    ("proc-version", "cat /proc/version",
     [r"^version 0\.7 \(Canyon\) i386$", r"^kernel 0\.7\.0$", r"^abi 1$",
      r"^built \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$"], []),
    ("proc-meminfo", "cat /proc/meminfo",
     [r"^MemTotal: +\d+ kB$", r"^PoolTotal: +\d+ kB$",
      r"^MemFree: +\d+ kB$", r"^MemUsed: +\d+ kB$"], []),
    ("proc-images", "cat /proc/images",
     [r"^kernel\t\d+\t1$", r"^userspace\t\d+\t\d+$",
      r"^libraries\t\d+\t2$", r"^total\t\d+\t\d+$"], []),
    ("proc-gfx", "cat /proc/gfx",
     # The depth belongs to the driver, not to the build: 8 on the VBE
     # card, 32 where a virtio-gpu presents. Which one this run is gets
     # checked against the driver line in the graphics checks below,
     # which is where the two have to agree.
     [r"^driver\t(vga|virtio-gpu)$", r"^resolution\t\d+x\d+$",
      r"^bpp\t(8|32)$"], []),
    ("proc-boot", "cat /proc/boot",
     [r"^rootfs\tvfs$", r"^shell\tvash$"], [r"^installed\tata"]),
    ("proc-status", "cat /proc/self/status",
     [r"^Name:\tvash$", r"^Pid:\t\d+$", r"^PPid:\t\d+$"], []),
    ("proc-cpuinfo", "cat /proc/cpuinfo",
     [r"^processor\t: 0$", r"^vendor_id\t: "], []),
    ("uname-all", "uname -a",
     [r"^VNU vnu 0\.7\.0 Canyon \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} i386 "],
     []),
    ("uname-plain", "uname", [r"^VNU$"], []),
    ("uname-node", "uname -n", [r"^vnu$"], []),
    ("vnu-fetch-host", "vnu fetch", [r"^Host +: +vnu$"], []),

    # The host name is a file like any other: root can rewrite it and
    # uname(2) follows. Restored below so the rest of the suite sees the
    # default name again.
    ("hostname-file", "cat /etc/hostname", [r"^vnu$"], []),
    ("hostname-set", "echo mybox > /etc/hostname", [], []),
    ("hostname-new", "cat /etc/hostname", [r"^mybox$"], []),
    ("hostname-uname", "uname -n", [r"^mybox$"], []),
    ("hostname-restore", "echo vnu > /etc/hostname", [], []),
    ("hostname-back", "uname -n", [r"^vnu$"], []),

    # The system's own content lives under /etc/vnu, not at the root:
    # the picture pack, the sound clips and the two wallpaper files.
    ("media-tree", "ls /etc/vnu",
     [r"^pics/$", r"^sounds/$", r"^wallpaper$", r"^wallpaper\.default$"], []),
    ("media-pics", "ls /etc/vnu/pics",
     [r"^flower\.bmp$", r"^sunset\.png$", r"^logo\.jpg$",
      r"^gray_alpha\.png$", r"^shapes_pal\.png$", r"^shapes_rgba\.png$"], []),
    ("media-sounds", "ls /etc/vnu/sounds",
     [r"^chime\.wav$", r"^melody\.wav$", r"^beep\.wav$"], []),
    # Both wallpaper files hold the shipped image (4436 bytes as built).
    ("media-wall", "wc /etc/vnu/wallpaper",
     [r"^\d+ \d+ 4436 /etc/vnu/wallpaper$"], []),
    ("media-wall-shipped", "wc /etc/vnu/wallpaper.default",
     [r"^\d+ \d+ 4436 /etc/vnu/wallpaper\.default$"], []),
    # The wallpaper is a file the kernel decodes on request, so the
    # desktop background can be set and read back from the shell too.
    ("wallpaper-show", "wallpaper", [r"^none$"], []),
    ("wallpaper-list", "wallpaper --list",
     [r"^sunset\.png$", r"^logo\.jpg  \(jpeg: the kernel cannot decode it\)$",
      r"^/etc/vnu/wallpaper\.default  \(the desktop VNU ships with\)$"], []),
    ("wallpaper-bad-jpeg", "wallpaper /etc/vnu/pics/logo.jpg",
     [r"^wallpaper: /etc/vnu/pics/logo\.jpg: not a usable wallpaper"], []),
    ("wallpaper-missing", "wallpaper /etc/vnu/pics/nope.png",
     [r"^wallpaper: /etc/vnu/pics/nope\.png: no such file$"], []),
    # A file that is not an image at all: the kernel must refuse it and
    # leave the desktop alone (vash has no ';', so two cases).
    ("wallpaper-notimage-make", "echo hello > /tmp/notapic.png",
     [], [r"[a-z]"]),
    ("wallpaper-notimage-read", "cat /tmp/notapic.png", [r"^hello$"], []),
    ("wallpaper-text-rejected", "wallpaper /tmp/notapic.png",
     [r"not a usable wallpaper"], []),
    ("wallpaper-set", "wallpaper /etc/vnu/pics/sunset.png",
     [r"^sunset\.png$"], []),
    ("wallpaper-live", "wallpaper", [r"^sunset\.png$"], []),
    ("wallpaper-file", "wc /etc/vnu/wallpaper",
     [r"^\d+ \d+ 2733 /etc/vnu/wallpaper$"], []),
    ("wallpaper-proc", "cat /proc/gfx",
     [r"^wallpaper\tsunset\.png$"], []),
    ("wallpaper-default", "wallpaper /etc/vnu/wallpaper.default",
     [r"^wallpaper\.default$"], []),
    ("wallpaper-back", "wallpaper", [r"^wallpaper\.default$"], []),
    ("wallpaper-help", "wallpaper --help",
     [r"^Usage: wallpaper", r"-l, --list", r"/etc/vnu/wallpaper"], []),

    # ...and nothing is left at the root, where the pack used to be.
    ("media-gone-pics", "ls /pics", [r"^ls: cannot open /pics$"], []),
    ("media-gone-sounds", "ls /sounds", [r"^ls: cannot open /sounds$"], []),
    ("media-gone-wall", "ls /wallpaper", [r"^ls: cannot open /wallpaper$"], []),
    # The installer insists on a name and refuses a bad one, both before
    # it ever looks for a disk - so these run on the live image too.
    ("vnu-install-nohost", "vnu install 0",
     [r"host name is required"], [r"complete"]),
    ("vnu-install-badhost", "vnu install 0 -bad",
     [r"not a valid host name"], [r"complete"]),

    # -- the rest of userspace still works ------------------------------
    # On a terminal man pages the page and waits for a key, so the page
    # *content* is checked through a redirect: the pager must stay out of
    # the way there (no status bar) and print the page instead. Each pair
    # shares one /tmp file, which the VFS keeps between commands (vash has
    # no ';' to chain them). The interactive side - status bar, arrows,
    # help, q - is run_man_pager() below.
    ("man-redirect", "man vcc > /tmp/man-vcc.out", [], [r"press h for help"]),
    ("man-vcc-text", "cat /tmp/man-vcc.out", [r"^SYNOPSIS$", r"^OPTIONS$"], []),
    ("vcc-version", "vcc --version", [r"^vcc \(VNU\) 0\.5$"], []),
    ("bin-list", "ls /bin",
     [r"^vash$", r"^vcc$", r"^vnu$", r"^man$", r"^df$"], [r"^install$"]),
    ("man-vnu", "man vnu > /tmp/man-vnu.out", [], [r"press h for help"]),
    ("man-vnu-text", "cat /tmp/man-vnu.out",
     [r"^NAME$", r"^SYNOPSIS$", r"^COMMANDS$", r"^FILES$",
      r"/proc/images"], []),
    ("man-ls", "man ls > /tmp/man-ls.out", [], [r"press h for help"]),
    ("man-ls-text", "cat /tmp/man-ls.out", [r"^NAME$", r"ls -"], []),
    ("man-hostname", "man hostname > /tmp/man-hostname.out", [],
     [r"press h for help"]),
    ("man-hostname-text", "cat /tmp/man-hostname.out",
     [r"^NAME$", r"hostname -", r"/etc/hostname"], []),
    # `install` is a `vnu` subcommand now, so it has no page of its own.
    ("man-no-install", "man install",
     [r"^man: no manual page for install$"], []),
    ("man-vibecommander", "man vibecommander > /tmp/man-vc.out",
     [], [r"press h for help"]),
    ("man-vibecommander-text", "cat /tmp/man-vc.out",
     [r"^NAME$", r"vibecommander - two-panel file manager",
      r"^KEYS$", r"\bF5 copy\b"], []),
    ("man-sticky", "man sticky > /tmp/man-sticky.out",
     [], [r"press h for help"]),
    ("man-sticky-text", "cat /tmp/man-sticky.out",
     [r"^NAME$", r"sticky - a sticky note for the desktop",
      r"^KEYS$", r"^FILES$", r"/root/\.sticky-note"], []),
    # -- fork(2) --------------------------------------------------------
    # `forkdemo` prints one line per claim the kernel makes about a
    # forked child: two processes that run at the same time, separate
    # pid/ppid, a private address space, and a reaped child with its
    # exit status. The two marker lines are the memory claim, and their
    # order is the concurrency claim: the parent prints while the child
    # is still asleep, which a child that only ran at the parent's exit
    # could not manage.
    ("fork-man", "man forkdemo > /tmp/man-forkdemo.out", [],
     [r"press h for help"]),
    ("fork-man-text", "cat /tmp/man-forkdemo.out",
     [r"^NAME$", r"forkdemo -", r"^SYNOPSIS$", r"^SEE ALSO$"], []),
    ("fork-run", "forkdemo",
     [r"^forkdemo: parent pid=\d+$",
      r"^forkdemo: parent forked pid=\d+, marker=1$",
      r"^forkdemo: child pid=0 getpid=\d+ getppid=\d+$",
      r"^forkdemo: child marker=2 \(its own copy\)$",
      r"^forkdemo: parent marker=1, child alive\? yes$",
      r"^forkdemo: parent marker=1 after the child wrote 2$",
      r"^forkdemo: waitpid -> pid=\d+ status=7$"], []),
    ("df", "df", [r"^Filesystem", r"^vfs +\d+ +\d+ +\d+ +\d+% /$"], []),
    ("motd", "cat /etc/motd", [r"Welcome to VNU"], []),
    ("echo", "echo hello world", [r"^hello world$"], []),
    ("seq", "seq 3", [r"^1\n2\n3$"], []),
    ("wc", "wc -l /proc/version", [r"^4 "], []),
    ("head", "head -1 /proc/version", [r"^version 0\.7"], []),
    ("tail", "tail -1 /proc/version", [r"^built "], []),
    ("grep", "grep kernel /proc/version", [r"^kernel 0\.7\.0$"], []),
    ("basename", "basename /a/b/c", [r"^c$"], []),
    ("dirname", "dirname /a/b/c", [r"^/a/b$"], []),
    ("whoami", "whoami", [r"^root$"], []),
    ("true-cmd", "true", [], [r"[a-z]"]),
    ("false-cmd", "false", [], [r"[a-z]"]),
    ("hello", "hello", [r"[Hh]ello"], []),
    ("sort", "sort /etc/motd", [r"Welcome to VNU"], []),
    ("files-touch", "touch /tmp/mk-test", [], [r"[a-z]"]),
    ("files-rm", "rm /tmp/mk-test", [], [r"[a-z]"]),
]


def run_suite(guest, cases, result, verbose, timeout, boot_timeout=120.0):
    print("==> %d cases" % len(cases))
    guest.wait(r"VNU login:", boot_timeout)
    guest.login(timeout=min(timeout, 30.0))
    for name, command, expects, forbids in cases:
        try:
            output, _ = guest.sh(command, timeout)
        except (TimeoutError, RuntimeError) as exc:
            result.failed.append((name, "", [str(exc)]))
            print("FAIL %-18s %s" % (name, exc))
            continue
        check(name, output, expects, forbids, result, verbose)
    return result


# The status bar the man pager draws on the last console row, and the white
# background that goes with it (SGR 0;30;47 right after the row is picked).
MAN_STATUS = r"Manual page vcc\(1\) line (\d+) \(press h for help or q to quit\)"
MAN_BAR = "\x1b[25;1H\x1b[0;30;47m"


def run_man_pager(guest, result, verbose, timeout=60.0):
    """Drive the man pager: the status bar, arrow keys, the help screen.

    This is not a SUITE case because the pager owns the terminal until q
    is pressed - there is no prompt for sh() to wait on - so the keys are
    sent here and the whole session is checked at the end."""
    print("==> man pager (interactive)")
    frm = len(guest.tail(0))
    guest._type("man vcc\n")
    guest.wait(MAN_STATUS, timeout, frm)
    guest.wait(r"SYNOPSIS", timeout, frm)

    def lines():
        return [int(n) for n in re.findall(MAN_STATUS, guest.tail(frm))]

    def last():
        seen = lines()
        return seen[-1] if seen else None

    def press(keys):
        """Type keys and wait for the bar to be redrawn once per key.

        Every key the pager acts on repaints the status line, so counting
        those repaints is a precise synchronisation - no guessed sleep."""
        seen = len(lines())
        sent = guest._type(keys)
        deadline = time.time() + timeout
        while time.time() < deadline and len(lines()) < seen + sent:
            time.sleep(0.05)
        return last()

    problems = []

    # three Down arrows: the bar counts down one line at a time
    if press("<down><down><down>") != 4:
        problems.append("after 3x Down the bar says line %r, expected 4" % last())
    # a space scrolls a whole screen and stops at the end of the document
    end_line = press("<spc>")
    if end_line is None or end_line <= 4:
        problems.append("space did not scroll a screen (line %r)" % end_line)
    # Home and End jump to the two ends
    if press("<home>") != 1:
        problems.append("Home did not return to line 1 (line %r)" % last())
    if press("<end>") != end_line:
        problems.append("End went to line %r, expected %r" % (last(), end_line))
    # b scrolls back up one screen
    if press("b") != 1:
        problems.append("b did not scroll up a screen (line %r)" % last())

    # h: the key list, then any key back to the page
    f2 = len(guest.tail(0))
    guest._type("h")
    guest.wait(r"how to move around a page", timeout, f2)
    f2b = len(guest.tail(0))
    guest._type("<down>")
    guest.wait(r"SYNOPSIS", timeout, f2b)
    if "man - how to move around a page" in guest.tail(f2b):
        problems.append("the help screen was still up after a key")

    # q: back to the shell
    f3 = len(guest.tail(0))
    guest._type("q")
    guest.wait_prompt(timeout, f3)

    chunk = guest.tail(frm)
    if MAN_BAR not in chunk:
        problems.append("the bar was not drawn white on row 25")
    if "\x1b[25;80H" not in chunk:
        problems.append("the cursor was not parked on the last cell")
    if problems:
        result.failed.append(("man-pager", "", problems))
        print("FAIL %-18s %s" % ("man-pager", "; ".join(problems)))
        return False
    result.passed += 1
    if verbose:
        print("ok   %-18s" % "man-pager")
    return True


def run_vibecommander(guest, result, verbose, timeout=60.0):
    """Drive vibecommander through a session: browse, create and delete a
    directory, quit.

    Like the man pager, the manager owns the terminal until F10, so this
    is a function, not a SUITE case: the keys are sent here and each step
    is checked on the console output it leaves in the log."""
    print("==> vibecommander (interactive)")
    VC_BAR = (r"1Help.{0,40}3View.{0,40}4Edit.{0,40}5Copy.{0,40}"
              r"6Move.{0,40}7Mkdir.{0,40}8Delete.{0,40}10Quit")
    problems = []
    frm = len(guest.tail(0))
    try:
        guest._type("vibecommander /\n")
        guest.wait(VC_BAR, timeout, frm)
        # The cursor opens the first entry (/apps on this filesystem).
        guest._type("<ret>")
        guest.wait(r"\[/apps\]", timeout, frm)
        # Left returns to the parent directory (the root panel).
        guest._type("<left>")
        guest.wait(r"\[/\]", timeout, frm)
        # A letter key jumps to a matching entry; bin is such a directory.
        guest._type("b<ret>")
        guest.wait(r"\[/bin\]", timeout, frm)
        guest._type("<left>")
        guest.wait(r"\[/\]", timeout, frm)
        # Jump to /tmp with a letter key and enter it.
        guest._type("t<ret>")
        guest.wait(r"\[/tmp\]", timeout, frm)
        # F7 makes a directory; the prompt takes its name.
        guest._type("<f7>vctest\n")
        guest.wait(r"Make directory: vctest", timeout, frm)
        guest.wait(r"created", timeout, frm)
        # Jump to it and F8 deletes it after a confirmation.
        guest._type("v")
        guest._type("<f8>y\n")
        guest.wait(r"Delete vctest \?", timeout, frm)
        guest.wait(r"deleted", timeout, frm)
        # F10 wraps the session up and the shell prompt comes back.
        fq = len(guest.tail(0))
        guest._type("<f10>")
        guest.wait_prompt(timeout, fq)
    except (TimeoutError, RuntimeError) as exc:
        problems.append(str(exc))

    if problems:
        result.failed.append(("vibecommander", "", problems))
        print("FAIL %-18s %s" % ("vibecommander", "; ".join(problems)))
        return False
    result.passed += 1
    if verbose:
        print("ok   %-18s" % "vibecommander")
    return True


def run_prefs_wallpaper(guest, result, verbose, timeout=60.0):
    """Drive the Wallpaper pane of prefs and check the result from the shell.

    prefs is a gfx-windowed app, so nothing it draws reaches the serial
    log - but what it does to the machine does: the pane is driven with
    the arrows and Enter exactly as a user would, and the background it
    picked is then read back with `wallpaper`. The window is not on
    screen here (no desktop is running), which is precisely why the
    assertion is about the effect and not the pixels.
    """
    print("==> prefs wallpaper pane (interactive)")
    problems = []
    out, _ = guest.sh("wallpaper -l", timeout)
    entries = [line for line in out.splitlines() if line.strip()]
    if len(entries) < 2:
        problems.append("wallpaper --list offered nothing to pick (%r)" % out)
        result.failed.append(("prefs-wallpaper", out, problems))
        print("FAIL %-18s %s" % ("prefs-wallpaper", "; ".join(problems)))
        return False

    # Start from a background the pane is not going to pick, so the
    # check below cannot pass on whatever was already in place.
    out, _ = guest.sh("wallpaper /etc/vnu/pics/sunset.png", timeout)
    check("prefs-wallpaper-setup", out, [r"^sunset\.png$"], [],
          result, verbose)

    frm = len(guest.tail(0))
    guest._type("/apps/prefs/bin\n")
    # A windowed app prints nothing, so there is nothing to wait for;
    # the pause is for the process to start and draw its first frame.
    time.sleep(2.0)
    guest._type("jjjj")                       # About, Memory, Mounts, CPU, Wallpaper
    guest._type("<down>" * len(entries))      # past the pack, onto the shipped desktop
    guest._type("\n")                        # apply
    time.sleep(0.5)
    # vgfx_poll() needs a byte after a bare Esc to make a key of it, so
    # the first one is swallowed - the same reason the desktop wraps its
    # Esc as a mouse button.
    guest._type("<esc><esc>")
    guest.wait_prompt(timeout, frm)

    out, _ = guest.sh("wallpaper", timeout)
    check("prefs-wallpaper", out, [r"^wallpaper\.default$"],
          [r"^none$"], result, verbose)
    if problems:
        result.failed.append(("prefs-wallpaper", "", problems))
        print("FAIL %-18s %s" % ("prefs-wallpaper", "; ".join(problems)))
        return False
    return True


def qt_ink(frame):
    m = console_mask(frame)
    return sum(m) if m else None


def run_gfx_surface(guest, result, verbose, timeout=60.0):
    """Check that a windowed app's pixels really reach the screen.

    Every other check asserts on the serial log, but a gfx window is
    exactly the case where that proves nothing: the app draws into a
    buffer and the kernel hands the very same pages to the compositor,
    so nothing is ever written to the terminal. So this one looks at the
    screen instead - QEMU's screendump, compared frame against frame.

    The harness has no mouse (input events never make it through to the
    guest's PS/2 mouse), so the app is opened the way a user without one
    would: `gui <app>` starts the desktop with that app already up. The
    bare desktop is screenshotted first, in a session of its own, to have
    something to compare the window against.
    """
    print("==> gfx window pixels (screendump)")
    shotdir = tempfile.mkdtemp(prefix="vnu-shot-")
    problems = []
    try:
        def shot(name):
            path = os.path.join(shotdir, name)
            return settled_shot(guest, path)

        # 1. The shell's text console, 720x400.
        before = shot("before.ppm")

        # 2. The bare desktop: backdrop and panel, no window.
        guest._type("gui\n")
        time.sleep(3.0)
        bare = shot("bare.ppm")
        if not esc_to_shell(guest, timeout):
            problems.append("the desktop did not give the console back")

        # 3. The same desktop with a window in it.
        guest._type("gui picview\n")
        time.sleep(5.0)
        open_ = shot("open.ppm")
        if not esc_to_shell(guest, timeout):
            problems.append("the window did not close, or the desktop did "
                            "not give the console back")
        after = shot("after.ppm")

        for name, frame in (("bare", bare), ("open", open_), ("after", after)):
            if (frame[0], frame[1]) != (bare[0], bare[1]) and name != "after":
                problems.append("%s is %dx%d, the desktop is %dx%d"
                                % (name, frame[0], frame[1],
                                   bare[0], bare[1]))
        if (before[0], before[1]) != (after[0], after[1]):
            problems.append("the console is %dx%d after the desktop, was "
                            "%dx%d before it"
                            % (after[0], after[1], before[0], before[1]))

        # The check is spatial: picview shows the very picture the
        # desktop draws as its backdrop, so look for what a window looks
        # like on screen - a block hundreds of rows deep, as wide as a
        # 480x340 canvas scaled to a window. The frame and title bar are
        # the compositor's own work and ~20 rows tall, so a band that
        # deep can only be the app's canvas: a window that fell back to
        # the text path, or drew into a private buffer, leaves a blank
        # rectangle instead.
        #
        # A row counts if *anything* in it changed, not if a fixed number
        # of pixels did: a canvas that draws the picture in the display's
        # own colours agrees with the backdrop pixel for pixel over most
        # of the window, so a threshold on the row's pixel count would be
        # measuring the gaps in the picture rather than the window. What
        # has to be there is the band itself, and the width of the rows in
        # it that did change wholesale.
        rows = changed_rows(open_[2], bare[2], open_[0], open_[1])
        top, bottom = longest_run(rows, min_changed=1)
        if bottom - top < 200:
            problems.append("the window changed %d rows, too few for a "
                            "480x340 canvas: the app's pixels never reached "
                            "the screen" % (bottom - top))
        if not top <= open_[1] // 2 <= bottom:
            problems.append("the changed band is rows %d..%d, not where a "
                            "centred window goes" % (top, bottom))
        else:
            # And it has to be a *picture* in there, not one flat colour.
            #
            # How many colours is the question, not how they are
            # arranged: on a display with a DAC in front of it the
            # picture can only ever be the palette's sixteen, and at
            # true colour it can be everything in the PNG. So the floor
            # is the palette on one driver and above it on the other,
            # and a canvas that never got its pixels onto the screen -
            # blank, or drawn in a format the compositor cannot read -
            # fails both.
            proc, _ = guest.sh("cat /proc/gfx", timeout)
            drv, depth = driver_name(proc), depth_name(proc)
            pal = palette_colours(proc)
            if drv is None or depth is None or not pal:
                problems.append("/proc/gfx has no driver, bpp or palette "
                                "line: %r" % proc)
            else:
                x0, x1 = window_columns(open_[2], bare[2], open_[0], rows,
                                        300)
                colours = distinct_colours(open_[2], open_[0], top, bottom,
                                           x0, x1)
                if x1 - x0 < 400:
                    problems.append("the window is %d pixels wide, too "
                                    "narrow for a 480-wide canvas: the app's "
                                    "pixels did not reach the screen"
                                    % (x1 - x0))
                elif len(colours) < 6:
                    problems.append("the window holds %d colours, not the "
                                    "handful a picture needs: a blank canvas, "
                                    "or one drawn in a format the compositor "
                                    "cannot read" % len(colours))
                elif depth == "32" and not (colours - pal):
                    # The window is nothing but colours the palette could
                    # have produced, so nothing on the screen says the
                    # canvas was 32bpp: either the app drew nothing, or
                    # what it drew came back through sixteen slots. A
                    # true-colour picture of a posterised drawing could
                    # look like this, but the sky in this one cannot -
                    # its gradient is finer than sixteen steps, and no
                    # amount of dithering reaches past them.
                    problems.append("every one of the window's %d colours "
                                    "is a palette entry: on a 32bpp canvas "
                                    "the picture is not being drawn in true "
                                    "colour" % len(colours))
        # The console is bracketed around two desktop sessions, and the
        # harness types a command for each one, so compare it allowing
        # for the lines that scrolled (scrolled_diff) rather than pixel
        # for pixel. What did change is the commands typed in between, so
        # what is left has to be a small part of the console: a quarter
        # of it is a screenful of glyphs that did not come back.
        import os as _os
        _os.makedirs("/tmp/vnu-probe", exist_ok=True)
        for _n, _f in (("before", before), ("bare", bare), ("open", open_),
                       ("after", after)):
            with open("/tmp/vnu-probe/gfx-%s.ppm" % _n, "wb") as _fh:
                _fh.write(b"P6\n%d %d\n255\n" % (_f[0], _f[1]))
                _fh.write(_f[2])
        print("PROBE bare=%s open=%s after=%s ink_after=%s" % (
            qt_ink(bare), qt_ink(open_), qt_ink(after), qt_ink(after)))
        diff = scrolled_diff(before, after)
        if diff is None:
            problems.append("there is no console on one side of the session "
                            "to compare")
        elif diff > TEXT_COLS * 8 * TEXT_H // 4:
            problems.append("the console did not come back the way it was "
                            "(%d pixels still differ)" % diff)
    except (TimeoutError, RuntimeError, OSError) as exc:
        problems.append(str(exc))

    if problems:
        keep_frames(shotdir, "gfx-surface")
    shutil.rmtree(shotdir, ignore_errors=True)
    if problems:
        result.failed.append(("gfx-surface", "", problems))
        print("FAIL %-18s %s" % ("gfx-surface", "; ".join(problems)))
        return False
    result.passed += 1
    if verbose:
        print("ok   %-18s" % "gfx-surface")
    return True


# The taskbar's height in pixels, as gui.cpp draws it. The checks that
# measure a window start below it: the panel is not flat (a clock, a
# button per window), so a bare and a windowed desktop always differ in
# it, whatever the desktop did.
PANEL_H = 34

# The mode ladder the driver offers, and the one a fresh boot starts in.
MODE_DEFAULT = (1024, 768)
MODE_TOP = (1280, 1024)
MODE_SMALL = (640, 480)


def frame_is_desktop(frame):
    """Whether a frame holds a drawn desktop rather than a blank or
    garbled screen. Two things say so, and both are cheap: the taskbar is
    one flat band of its own colour, so the top corners match, and the
    desktop below it is not a single colour. A flat fill fails the second
    test, uninitialised video memory the first."""
    width, height, px = frame

    def at(x, y):
        base = y * width * 3 + x * 3
        return px[base:base + 3]

    step_x = max(1, width // 40)
    step_y = max(1, (height - PANEL_H) // 40)
    seen = {at(x, y) for y in range(PANEL_H, height, step_y)
            for x in range(0, width, step_x)}
    return at(2, 2) == at(width - 3, 2) and len(seen) >= 6


def check_window_inside(problems, when, frame, bare):
    """The window drawn in `frame` is inside it, and not under the panel."""
    if frame is None or bare is None:
        return
    if (frame[0], frame[1]) != (bare[0], bare[1]):
        return          # a size mismatch is already reported
    box = changed_bbox(frame, bare, frame[0], PANEL_H)
    if box is None:
        problems.append("no window on screen %s" % when)
        return
    x0, y0, x1, y1 = box
    if x0 < 0 or y0 < PANEL_H or x1 >= frame[0] or y1 >= frame[1]:
        problems.append("the window %s is at %s, outside the %dx%d screen "
                        "below the %d px taskbar"
                        % (when, box, frame[0], frame[1], PANEL_H))


def run_gfx_resolution(guest, result, verbose, timeout=60.0):
    """Check that the display really changes mode while the desktop runs.

    Resolution is the one feature whose result is not in the serial log
    at all: it is the size of the frame QEMU hands out, so this looks at
    screendumps, the way run_gfx_surface does. F12 steps to the next
    mode in the ladder (there is no userspace command for it - see
    ABI.md), which is also the only way a session with no program in it
    can reach the feature.

    Covered, in the order they would bite a user: the mode really changes
    on screen; the choice is written to /etc/vnuconfig/gfx.conf; the next
    `gui` comes up in it; and a window that no longer fits the new screen
    is still drawn inside it.
    """
    print("==> resolution switching (F12, screendump)")
    shotdir = tempfile.mkdtemp(prefix="vnu-mode-")
    problems = []
    try:
        def shot(name):
            path = os.path.join(shotdir, name)
            return settled_shot(guest, path)

        def step_to(mode, name):
            """F12 until the frame is `mode`, then check what is on it."""
            for _ in range(8):
                frame = shot(name)
                if (frame[0], frame[1]) == mode:
                    if not frame_is_desktop(frame):
                        problems.append("at %dx%d the screen is not a drawn "
                                        "desktop" % mode)
                    return frame
                guest._type("<f12>")
                time.sleep(1.5)
            problems.append("F12 never reached %dx%d" % mode)
            return None

        # A fresh image starts in the built-in default, and says so in
        # the file the desktop reads it from.
        output, _ = guest.sh("cat /etc/vnuconfig/gfx.conf", timeout)
        check("mode-config-boot", output, [r"^mode %dx%d$" % MODE_DEFAULT],
              [], result, verbose)

        guest._type("gui\n")
        time.sleep(3.0)
        first = shot("first.ppm")
        if (first[0], first[1]) != MODE_DEFAULT:
            problems.append("the desktop came up %dx%d, the configured "
                            "default is %dx%d"
                            % (first[0], first[1], MODE_DEFAULT[0],
                               MODE_DEFAULT[1]))

        # The ladder's top mode, with the mode from the config file read
        # back on the way in - the persistence half of the feature. This
        # is a bare desktop, so the frame is kept: the window check at the
        # end needs a windowless screen of the same size to diff against,
        # and a smaller mode's screen will not do.
        bare_top = step_to(MODE_TOP, "top.ppm")
        if not esc_to_shell(guest, timeout):
            problems.append("the desktop did not give the console back")
        # With the console back, the display is in the console's own mode
        # again - TEXT_W x TEXT_H, the text mode it booted in - because
        # that is where it was before the desktop started and where the
        # user left it. run_gfx_surface is the other half of that: the
        # screendump has to come back at the text mode's size, not stay
        # at whatever the desktop was using. The mode the *next* session
        # comes up in is the config file's business, checked below.
        output, _ = guest.sh("cat /proc/gfx", timeout)
        check("mode-proc", output,
              [r"^resolution\t%dx%d$" % (TEXT_W, TEXT_H),
               r"^modes\t640x480,800x600,1024x768,1280x1024$"], [], result,
              verbose)

        # The depth is the driver's, so the driver and the depth on the
        # same line have to be the pair that belongs together: 8bpp
        # palette indices on the VBE card, 32bpp B8G8R8X8 where a
        # virtio-gpu presents. Either one wrong is a driver that lost
        # track of what it is drawing into.
        if not driver_name(output) or not depth_name(output):
            problems.append("/proc/gfx has no driver or bpp line: %r"
                            % output)
        elif depth_name(output) != WANT_DEPTH[driver_name(output)]:
            problems.append("/proc/gfx says driver %s at %sbpp, which is "
                            "not a pair this driver has"
                            % (driver_name(output), depth_name(output)))

        # The same display asked for through the syscall instead of
        # through /proc: `vnu fetch` prints what gfx_getinfo(2) answered.
        # Every field of the line is read out of the file rather than
        # written here, so the two have to agree: a struct still carrying
        # a stale mode, or a driver that disagreed with the text file,
        # fails this.
        driver = re.search(r"^driver\t(\S+)$", output, re.MULTILINE)
        depth = re.search(r"^bpp\t(\d+)$", output, re.MULTILINE)
        res = re.search(r"^resolution\t(\d+)x(\d+)$", output, re.MULTILINE)
        fetch, _ = guest.sh("vnu fetch", timeout)
        if not driver or not depth or not res:
            problems.append("/proc/gfx has no driver, bpp or resolution "
                            "line: %r" % output)
        else:
            check("mode-info", fetch,
                  [r"^Graphics +: +%s %sx%s %sbpp$"
                   % (driver.group(1), res.group(1), res.group(2),
                      depth.group(1))], [], result, verbose)
        output, _ = guest.sh("cat /etc/vnuconfig/gfx.conf", timeout)
        check("mode-config", output, [r"^mode %dx%d$" % MODE_TOP], [],
              result, verbose)

        guest._type("gui\n")
        time.sleep(3.0)
        again = shot("again.ppm")
        if (again[0], again[1]) != MODE_TOP:
            problems.append("the second desktop came up %dx%d, the mode "
                            "recorded in gfx.conf is %dx%d"
                            % (again[0], again[1], MODE_TOP[0], MODE_TOP[1]))
        if not esc_to_shell(guest, timeout):
            problems.append("the second desktop did not give the console back")

        # A window has to survive the change, not just the screen: the
        # bare desktop at the smallest mode is the reference, and the
        # bounding box of what differs from it is the window. It must sit
        # inside the frame and below the taskbar - a window left at its
        # old coordinates would be off the edge, or under the panel.
        # The ladder wraps, so stepping is by size, not by a count.
        guest._type("gui\n")
        time.sleep(4.0)
        step_to(MODE_SMALL, "small-bare.ppm")
        if not esc_to_shell(guest, timeout):
            problems.append("the desktop did not give the console back")
        bare_small = shot("small-bare.ppm")

        guest._type("gui calc\n")
        time.sleep(5.0)
        small = step_to(MODE_SMALL, "small-win.ppm")
        if small and not frame_is_desktop(small):
            problems.append("at %dx%d with a window open the screen is not "
                            "a drawn desktop" % (small[0], small[1]))
        check_window_inside(problems, "at the smallest mode", small,
                            bare_small)
        # And back up: the same window on the largest screen, where it
        # must not stay pinned to a corner it was dragged to.
        top = step_to(MODE_TOP, "top-win.ppm")
        check_window_inside(problems, "after a change back to the top mode",
                            top, bare_top)
        if not esc_to_shell(guest, timeout):
            problems.append("the window did not close, or the desktop did "
                            "not give the console back")
        else:
            # The console is on the display again, not just in the log.
            # Taken once the repaint settles: VGA text mode redraws its
            # cells over a few refreshes, so a dump taken at the first
            # prompt back can still be holding the desktop's last frame.
            shot = settled_shot(guest, os.path.join(shotdir, "back.ppm"))
            console_on_screen(shot, problems, "after a desktop session")
    except (TimeoutError, RuntimeError, OSError) as exc:
        problems.append(str(exc))

    if problems:
        keep_frames(shotdir, "gfx-resolution")
    shutil.rmtree(shotdir, ignore_errors=True)
    if problems:
        result.failed.append(("gfx-resolution", "", problems))
        print("FAIL %-18s %s" % ("gfx-resolution", "; ".join(problems)))
        return False
    result.passed += 1
    if verbose:
        print("ok   %-18s" % "gfx-resolution")
    return True


# The line typed but never entered, and the command typed after the ^C.
# The first is a line the shell has read and echoed but not run, so ^C
# has something to throw away; the second proves the shell is still
# there afterwards. Both are echoed by the shell, so their text is in the
# log exactly once each when all is well.
CTRLC_LINE = "echo ctrlc-discarded"
CTRLC_AFTER = "echo ctrlc-survived"


def run_ctrlc(guest, result, verbose, timeout=60.0):
    """Ctrl+C at the console is a signal, and the shell lives through it.

    Not a SUITE case because the thing under test is a keystroke, not a
    command: the console turns Ctrl+C into SIGINT for the process at the
    keyboard, the blocked read fails with EINTR, and the shell drops the
    line it was reading and prints a fresh prompt. The keys are sent here
    and the session is checked at the end.

    What makes this more than "the shell printed something" is what it
    checks besides the ^C. The line is typed but *not* entered, so the
    only thing that can discard it is the signal: a kernel that turned
    Ctrl+C into a byte instead would leave the shell waiting for an
    Enter that the check never sends, and the fresh prompt after it would
    not come. And the command typed afterwards has to run, because a
    sigreturn that resumed the process on the wrong stack looks exactly
    like a working one until the next thing the program does touches the
    stack - which is the failure this was written for.
    """
    print("==> ctrl+c (SIGINT at the console)")
    problems = []
    try:
        frm = guest.wait_ready()
        # Type a line and leave it unentered: the echo is what says the
        # shell has it, so the ^C lands on a line rather than on an idle
        # prompt with nothing to discard.
        guest._type(CTRLC_LINE)
        guest.wait(re.escape(CTRLC_LINE), timeout, frm)
        typed = len(guest.tail(frm))
        guest._type("<ctrl-c>")
        # The ^C and the newline after it are the shell's report that the
        # read it was waiting on failed, and the prompt behind them is a
        # new one - the signal returned the line to the shell, which
        # printed a fresh prompt instead of the old line's.
        after = guest.wait(r"\^C", timeout, typed)
        guest.wait_ready(after)
        out = guest.tail(frm)
        if out.count(CTRLC_LINE) != 1:
            problems.append("the unentered line %r appears %d times, "
                            "expected once (its echo): it was run, or the "
                            "^C did not discard it"
                            % (CTRLC_LINE, out.count(CTRLC_LINE)))
        # And the shell is still a shell afterwards.
        output, _ = guest.sh(CTRLC_AFTER, timeout)
        check("ctrlc-after", output,
              [r"^%s$" % re.escape(CTRLC_AFTER.split()[1])], [], result,
              verbose)
    except (TimeoutError, RuntimeError) as exc:
        problems.append(str(exc))

    if problems:
        result.failed.append(("ctrl-c", "", problems))
        print("FAIL %-18s %s" % ("ctrl-c", "; ".join(problems)))
        return False
    result.passed += 1
    if verbose:
        print("ok   %-18s" % "ctrl-c")
    return True


# The desktop's panel height in pixels, the row below which is the first
# one a window may cover: matches PANEL_H in gui/gui.cpp. A redraw above
# it is the panel's own (its clock ticks), which is why the checks here
# look below this row and not above.
PANEL_H = 34

# The text console's own geometry, which is what the shell session draws
# into: matches the 720x400 text mode the console boots in.
CONSOLE_W, CONSOLE_H = 720, 400


def present_rect(proc_gfx):
    """What /proc/gfx says the last present() moved, as (w, h, x, y) in
    pixels plus the bytes it moved - or (None, 0) when the line is
    missing. The box is the damage that pass ended up with, so an idle
    desktop reports 0x0 and no bytes at all, which is the answer damage
    tracking is for."""
    m = re.search(r"^last_present\t(\d+)x(\d+)\+(\d+)\+(\d+)\t(\d+) bytes$",
                  proc_gfx, re.MULTILINE)
    if not m:
        return None, 0
    return (int(m.group(1)), int(m.group(2)),
            int(m.group(3)), int(m.group(4))), int(m.group(5))


def esc_to_shell(guest, timeout, tries=4):
    """Leave the desktop and wait for the shell back.

    Esc is the desktop's own "close the front window, and quit when the
    last one is gone". A vgfx reader wants a byte after a bare Esc to
    make a key of it, so the first press can be swallowed - hence the
    retries. Waiting on the prompt from the current offset matters: the
    log still holds every prompt typed before the desktop ever started.
    """
    for _ in range(tries):
        frm = len(guest.tail(0))
        guest._type("<esc>")
        try:
            guest.wait_prompt(timeout, frm)
            return True
        except TimeoutError:
            continue
    return False


def run_console_screen(guest, result, verbose, timeout=60.0):
    """The text console is on the display, on whichever display there is,
    and it is the *live* one.

    Not a SUITE case because the answer is in pixels: nothing typed
    reaches the screen, and the serial log is exactly where the text
    *is* even when the monitor shows nothing at all. So the check looks
    at what QEMU is showing, on a machine at its boot prompt and again
    after a desktop session has given the display back.

    The second half is not only about the console being *there*: a
    driver that takes the screen, draws the console once and then never
    repaints it passes every shape check above with a still picture of
    the prompt from the moment the desktop quit - the shell goes on
    working, into a text plane the host is no longer showing. So a
    command is typed at it and the display has to move.
    """
    print("==> console on screen (screendump)")
    shotdir = tempfile.mkdtemp(prefix="vnu-console-")
    problems = []
    try:
        # At the prompt, before anything has taken the display.
        shot = settled_shot(guest, os.path.join(shotdir, "boot.ppm"))
        console_at_boot(shot, problems, "at the boot prompt")

        # And after a desktop has run and quit: a driver that takes the
        # screen and does not hand it back leaves the last frame there.
        # Typed, not sh(): the desktop owns the terminal until Esc, so
        # there is no prompt for sh() to wait on.
        guest._type("gui\n")
        time.sleep(3.0)
        if not esc_to_shell(guest, timeout):
            problems.append("the desktop did not give the console back")
        else:
            shot = settled_shot(guest, os.path.join(shotdir, "back.ppm"))
            console_on_screen(shot, problems, "after a desktop session")

            # And it has to still be the console being used: type at it
            # and the display has to show it. Typed, not sh()'d - the
            # echo is the point, and there is no prompt to wait on.
            guest._type("echo console-live-marker\n")
            time.sleep(1.0)
            live = settled_shot(guest, os.path.join(shotdir, "live.ppm"))
            if not any(changed_rows(live[2], shot[2], live[0], live[1])):
                problems.append("after a desktop session, typing a command "
                                "changed nothing on the display: the "
                                "console on it is a still picture")
            else:
                console_on_screen(live, problems, "while typing")
    except (TimeoutError, RuntimeError, OSError) as exc:
        problems.append(str(exc))

    if problems:
        keep_frames(shotdir, "console-screen")
    shutil.rmtree(shotdir, ignore_errors=True)
    if problems:
        result.failed.append(("console-screen", "", problems))
        print("FAIL %-18s %s" % ("console-screen", "; ".join(problems)))
        return False
    result.passed += 1
    if verbose:
        print("ok   %-18s" % "console-screen")
    return True


def run_gfx_damage(guest, result, verbose, timeout=60.0):
    """Check that a pass which changes little hands the host little.

    Damage tracking is the claim that a redraw sends the part of the
    screen that changed instead of all of it, and neither half of it is
    visible in the serial log, so this is two checks in one function.

    The first is what the desktop draws: with a window up, typing a
    line into it must change that window's rows and leave the desktop
    around it alone, and closing the window must leave the desktop bare
    again. That is what a per-region repaint looks like from outside,
    and it fails in both directions - damage too small shows as pixels
    the redraw never reached, damage too large as changed rows outside
    the window. The panel is excused (its clock ticks on its own), so
    only rows below it are judged.

    The second is /proc/gfx, which reports what the last present()
    moved: the line has to be there, and the bytes it moved have to
    agree with the rectangle they moved for. It does not bound how small
    a present gets, and that is worth being straight about: a pass that
    changes little has to be *seen* in the picture, and the picture is
    identical whether the copy was of the region or of the frame. So the
    size of a present is bounded by the code (the damage_add() calls in
    gui/gui.cpp and the box in vgfx::present_with), and what is checked
    here is that the region it is given is enough - which is the half
    that is wrong on screen when it is not.
    """
    print("==> gfx damage tracking (screendump + /proc/gfx)")
    problems = []
    shotdir = tempfile.mkdtemp(prefix="vnu-dmg-")

    try:
        def shot(name):
            return settled_shot(guest, os.path.join(shotdir, name))

        # 1. The redraw, seen from outside the desktop. `calc` is the app
        #    for this: it repaints its window on every key (so typing is
        #    visible) and Esc closes it (so the window can be put away -
        #    Esc reaches a windowed app as a close, while a text window
        #    just gets the character). A bare desktop first, to know
        #    where the window goes.
        guest._type("gui\n")
        time.sleep(4.0)
        bare = shot("bare.ppm")
        if not esc_to_shell(guest, timeout):
            problems.append("the bare desktop did not give the console back")

        guest._type("gui calc\n")
        time.sleep(5.0)
        opened = shot("open.ppm")
        guest._type("7*6=")
        time.sleep(1.0)
        typed = shot("typed.ppm")

        for name, frame in (("bare", bare), ("opened", opened),
                            ("typed", typed)):
            if (frame[0], frame[1]) != (bare[0], bare[1]):
                problems.append("%s is %dx%d, the desktop is %dx%d"
                                % (name, frame[0], frame[1],
                                   bare[0], bare[1]))
        if (opened[0], opened[1]) == (bare[0], bare[1]):
            # Where the window is, from the frame that has it against the
            # one that has not. The panel gains a button in the same diff
            # and is a third of the height, so the longest run of changed
            # rows is the window's and not the panel's.
            grew = longest_run(changed_rows(bare[2], opened[2], bare[0],
                                            bare[1]), 1)
            if grew[0] < 0:
                problems.append("opening a window changed nothing on screen")
            else:
                win_top, win_bottom = grew
                # Typing in it has to stay inside the window: a redraw
                # that reached the desktop around it is a region that
                # grew, and one that stayed inside is what a per-window
                # pass looks like from out here.
                rows = changed_rows(opened[2], typed[2], opened[0],
                                    opened[1])
                top, bottom = longest_run(rows, 1)
                if verbose:
                    print("   window rows %d..%d, changed %d..%d, "
                          "changed rows outside: %d"
                          % (win_top, win_bottom, top, bottom,
                             sum(1 for y, n in enumerate(rows)
                                 if n and not win_top <= y <= win_bottom)))
                if top < 0:
                    problems.append("typing 7*6= into the calculator "
                                    "changed nothing on screen: the redraw "
                                    "did not reach the window")
                elif top < win_top or bottom > win_bottom:
                    stray = [y for y, n in enumerate(rows)
                             if n and not win_top <= y <= win_bottom]
                    problems.append("typing in the window changed rows "
                                    "%d..%d, outside its %d..%d: the "
                                    "redraw was not confined to the window"
                                    % (top, bottom, win_top, win_bottom)
                                    + (" (first is row %d)" % stray[0]
                                       if stray else ""))
                # And the ground it vacates belongs to the desktop again:
                # once the window is closed the screen has to be the bare
                # one, which is the wallpaper surviving a repaint that
                # only redrew the region the window was on. The panel is
                # not compared - its clock has moved on.
                #
                # Closing the last window does not end the session: the
                # desktop is still there, empty, until Esc with nothing
                # focused leaves it. So this is one Esc, a look, and
                # another Esc if the window is still up (a windowed app
                # can swallow the first one).
                gone = None
                for attempt in range(3):
                    guest._type("<esc>")
                    time.sleep(1.5)
                    gone = shot("gone%d.ppm" % attempt)
                    if changed_bbox(bare, gone, bare[0], PANEL_H) is None:
                        break
                left = changed_bbox(bare, gone, bare[0], PANEL_H)
                if left is not None:
                    problems.append("rows %d..%d of the desktop still differ "
                                    "once the window is closed: the ground "
                                    "it vacated was not repainted"
                                    % (left[1], left[3]))
                if not esc_to_shell(guest, timeout):
                    problems.append("the desktop did not give the console "
                                    "back")

        # 2. The transfer, as a number. Read in the shell, and only now:
        #    a console on the text plane is drawn by the card and never
        #    presented at all, so before any desktop has run there is
        #    nothing here to read.
        #
        #    What is here to read depends on the driver, which is the
        #    honest shape of this check. A VGA card puts its text plane
        #    back on the monitor when the desktop leaves, and the console
        #    is the card's own drawing again: there is no transfer to
        #    measure and none is made. A virtio display is still showing
        #    the scanout resource, which is not a text plane, so the
        #    console is ours to present - a keystroke's row of it, which
        #    is the number this is here to bound.
        #
        #    Two reads either way: the first console present after a
        #    desktop session is a whole frame, because the frame was
        #    handed back to the pool when the desktop left and what the
        #    host shows is a picture of the desktop. The one after that
        #    is a keystroke's worth of rows.
        first, _ = guest.sh("cat /proc/gfx", timeout)
        if verbose:
            print(first)
        if present_rect(first)[0] is None:
            problems.append("/proc/gfx has no last_present line: %r" % first)
        guest.sh("echo damage", timeout)
        time.sleep(1.0)
        proc, _ = guest.sh("cat /proc/gfx", timeout)
        if verbose:
            print(proc)
        rect, moved = present_rect(proc)
        if rect is not None:
            w, h, x, y = rect
            if moved and moved > w * h * 4:
                problems.append("%d bytes moved for a %dx%d rectangle: more "
                                "than 4 per pixel, which no path here does"
                                % (moved, w, h))
            if verbose:
                # Said out loud, because it is the shape of the thing:
                # once the desktop has been, neither driver is presenting
                # a console to measure (a VGA card draws the console on
                # its text plane, a virtio display has given its scanout
                # back), so the number here is the desktop's last pass and
                # there is no small one to wait for. What the compositor
                # sends per pass is bounded by the code, and what the host
                # ends up holding is checked above, in pixels.
                print("   (last present is the desktop's own pass: %dx%d "
                      "at %d,%d)" % (w, h, x, y))
    except TimeoutError as exc:
        problems.append("the desktop session timed out: %s" % exc)
    finally:
        if result.failed or verbose:
            keep_frames(shotdir, "gfx-damage")
        else:
            shutil.rmtree(shotdir, ignore_errors=True)

    if problems:
        result.failed.append(("gfx-damage", "", problems))
        print("FAIL %-18s %s" % ("gfx-damage", "; ".join(problems)))
    else:
        result.passed += 1
        print("ok   %-18s" % "gfx-damage")


# The geometry the calculator's own layout implies, in gui.cpp's terms:
# a window is the canvas plus 8 across (2px border either side), and
# TITLE_H + the canvas + 12 down (20px title bar, 2px top and bottom
# border, 8px padding under the bar).
CALC_CHROME_W = 8
CALC_TITLE_H = 20
CALC_CHROME_H = CALC_TITLE_H + 12
# The size calc asks for, and the slack allowed on what comes back.
CALC_WANT_W, CALC_WANT_H = 220, 220
CALC_GRANT_SLACK = 8
# How much the window has to grow by to count as resized. The desktop
# rounds the canvas up to a 64x64 granule, so a resize inside one
# granule costs nothing and changes no pixels; the drag below is several
# granules wide precisely so that this cannot be mistaken for one.
RESIZE_DRAG_X = 180
RESIZE_DRAG_Y = 140
RESIZE_MIN_GROW = 96


def window_box(frame, bare, min_px=30):
    """The window's bounding box, from `frame` against the bare desktop.

    The mouse pointer is drawn on top of whatever it is over and is part
    of the diff too, so a plain bounding box would grow to include it
    wherever it stands - and after a drag it stands at the corner the
    drag was aimed at, which is exactly the measurement this wants to
    make. A window is hundreds of pixels wide on every row it owns while
    the pointer is about a dozen, so counting a row's changed pixels and
    dropping the sparse ones leaves the window and only the window."""
    w, h = frame[0], frame[1]
    a, b = frame[2], bare[2]
    xs0 = ys0 = None
    x1 = y1 = -1
    for y in range(PANEL_H, h):
        base = y * w * 3
        n = sum(1 for x in range(w)
                if a[base + x * 3:base + x * 3 + 3]
                != b[base + x * 3:base + x * 3 + 3])
        if n < min_px:
            continue
        if ys0 is None:
            ys0 = y
        y1 = y
        for x in range(w):
            if a[base + x * 3:base + x * 3 + 3] != \
               b[base + x * 3:base + x * 3 + 3]:
                if xs0 is None:
                    xs0 = x
                x1 = x
    if xs0 is None:
        return None
    return xs0, ys0, x1, y1


def run_gfx_canvas_resize(guest, result, verbose, timeout=60.0):
    """Check that a window's canvas follows the window, in both directions.

    Milestone E's whole claim is that a gfx window is drawn at the size
    its client area is instead of scaling a fixed 480x340 into it, and
    neither half of that is visible in the serial log: the app's pixels
    never go through a console. So this looks at the screen.

    Two directions are checked, because the canvas can be moved from two
    sides and each has its own way of going wrong:

      1. The app asks. `calc` asks for a 220x220 canvas before it draws
         anything, so the window it opens in must be 220 wide and 220
         tall (plus chrome) rather than the 480x340 the canvas used to
         be. A canvas that ignored the request still opens at 480x340,
         and the calculator's keys are laid out from whatever size it is
         given, so the window on screen is the measurement of the
         negotiation.

      2. The user drags. The window's bottom-right corner is pulled out
         by a few granules and the window has to grow by about that much,
         with the calculator's rows inside it redistributing to fill the
         new height - which is what "the layout is computed from the
         canvas" means on screen. A canvas pinned at its old size either
         leaves the window alone or shows the old picture stretched.

    The drag needs the mouse, which reaches the guest only because the
    pointer navigation below verifies every leg against a frame - a
    packet can be dropped, and a drag that lost one is a resize to the
    wrong size rather than a failed test. So the corner is parked by
    reading where the pointer actually is, and the drag's outcome is
    checked against the window, not against the packets that were sent.
    """
    print("==> gfx canvas follows the window (screendump)")
    problems = []
    shotdir = tempfile.mkdtemp(prefix="vnu-canvas-")
    try:
        def shot(name):
            return settled_shot(guest, os.path.join(shotdir, name))

        guest._type("gui\n")
        time.sleep(3.0)
        bare = shot("bare.ppm")
        if not frame_is_desktop(bare):
            problems.append("the bare desktop is not a drawn desktop")
        if not esc_to_shell(guest, timeout):
            problems.append("the desktop did not give the console back")

        # 1. The app asks for a size and gets it.
        guest._type("gui calc\n")
        time.sleep(5.0)
        asked = shot("asked.ppm")
        box = window_box(asked, bare)
        if box is None:
            problems.append("no window on screen after gui calc")
        else:
            x0, y0, x1, y1 = box
            win_w, win_h = x1 - x0 + 1, y1 - y0 + 1
            if verbose:
                print("   window %dx%d at %d,%d (asked for %dx%d)"
                      % (win_w, win_h, x0, y0, CALC_WANT_W, CALC_WANT_H))
            want_w = CALC_WANT_W + CALC_CHROME_W
            want_h = CALC_WANT_H + CALC_CHROME_H
            if abs(win_w - want_w) > CALC_GRANT_SLACK or \
                    abs(win_h - want_h) > CALC_GRANT_SLACK:
                problems.append(
                    "the window is %dx%d, not the %dx%d its %dx%d canvas "
                    "needs: gfx_canvas(2) did not size the window"
                    % (win_w, win_h, want_w, want_h,
                       CALC_WANT_W, CALC_WANT_H))
            # The canvas itself has to be full of the app's own drawing,
            # not a blank rectangle: a window the right size with nothing
            # in it would pass the geometry above and mean nothing.
            rows = changed_rows(bare[2], asked[2], bare[0], bare[1])
            top, bottom = longest_run(rows, min_changed=1)
            if bottom - top < CALC_WANT_H - 2 * CALC_GRANT_SLACK:
                problems.append("the window changed %d rows, too few for a "
                                "%d-tall canvas: the app's pixels did not "
                                "reach the screen" % (bottom - top,
                                                      CALC_WANT_H))

        # 2. The user drags the window's corner.
        #
        # The pointer is brought to the corner with the verified
        # navigation the occlusion sessions use, because the corner is on
        # top of a window and the arrow cannot be read there; the run is
        # checked by taking it out and back, and the drag's outcome is
        # checked against the window, not against the packets that were
        # sent.
        if box is not None:
            x0, y0, x1, y1 = box
            corner = (x1 - 3, y1 - 3)
            dragged = None
            grown = None
            for attempt in range(3):
                if occl_goto(guest, corner) is None:
                    continue
                mouse_button(guest, True)
                time.sleep(0.3)
                occl_run(guest, RESIZE_DRAG_X, RESIZE_DRAG_Y)
                mouse_button(guest, False)
                time.sleep(1.5)
                dragged = shot("dragged%d.ppm" % attempt)
                grown = window_box(dragged, bare)
                if grown is not None and \
                        (grown[2] - grown[0]) - (x1 - x0) >= RESIZE_MIN_GROW:
                    break
                # Nothing (or too little) happened: the pointer was not on
                # the corner. Walk the drag back and try the approach again.
                occl_run(guest, -RESIZE_DRAG_X, -RESIZE_DRAG_Y)
            if grown is None:
                problems.append("the window vanished during the drag")
            else:
                gx0, gy0, gx1, gy1 = grown
                grow_w = (gx1 - gx0 + 1) - (x1 - x0 + 1)
                grow_h = (gy1 - gy0 + 1) - (y1 - y0 + 1)
                if verbose:
                    print("   after the drag: %dx%d at %d,%d "
                          "(grew %d x %d, attempts %d)"
                          % (gx1 - gx0 + 1, gy1 - gy0 + 1, gx0, gy0,
                             grow_w, grow_h, attempt + 1))
                if grow_w < RESIZE_MIN_GROW:
                    problems.append(
                        "dragging the corner out by %d px left the "
                        "window %d px wider: the canvas did not follow "
                        "the drag" % (RESIZE_DRAG_X, grow_w))
                if grow_h < RESIZE_MIN_GROW:
                    problems.append(
                        "dragging the corner out by %d px left the "
                        "window %d px taller: the canvas did not follow "
                        "the drag" % (RESIZE_DRAG_Y, grow_h))
                # The rows the window grew into are the app's own drawing
                # at a size it computed: the keys are laid out from the
                # canvas, so the old last row is no longer the last row.
                # A canvas that did not follow shows the old picture
                # stretched instead, which leaves those rows as bare
                # desktop or as stretched old content.
                fresh = changed_rows(asked[2], dragged[2], bare[0],
                                     bare[1])
                below = sum(1 for y in range(y1 + 1, gy1 + 1)
                            if fresh[y] > 2)
                if below < RESIZE_MIN_GROW // 2:
                    problems.append(
                        "only %d of the %d rows the window grew into "
                        "changed: the canvas is still %d tall and the "
                        "new area is not the app's drawing"
                        % (below, gy1 - y1, y1 - y0))

        if not esc_to_shell(guest, timeout):
            problems.append("the desktop did not give the console back")
    except TimeoutError as exc:
        problems.append("the desktop session timed out: %s" % exc)
    finally:
        if result.failed or verbose:
            keep_frames(shotdir, "gfx-canvas-resize")
        else:
            shutil.rmtree(shotdir, ignore_errors=True)

    if problems:
        result.failed.append(("gfx-canvas-resize", "", problems))
        print("FAIL %-18s %s" % ("gfx-canvas-resize", "; ".join(problems)))
    else:
        result.passed += 1
        print("ok   %-18s" % "gfx-canvas-resize")


def present_total(proc_gfx):
    """The cumulative count of bytes the desktop has transferred to the
    host since boot, as /proc/gfx reports it - or None when the kernel
    predates the counter (a /proc/gfx without the line) or already had
    one transfer of an enormous size (overflow is impossible)."""
    m = re.search(r"^present_total\t(\d+) bytes$", proc_gfx, re.MULTILINE)
    return int(m.group(1)) if m else None


# The desktop icons are laid out on a 4-by-9 grid (icon_rect(),
# gui/gui.cpp: 24x24 tiles on 80x48 cells from (8, 42)), and the tiles
# are in the order install_demo_apps() seeded /apps - hello, vedit,
# term, calc, files, picview, prefs, clock, play. These are the tile
# centres the harness clicks; a change to the /apps seed order moves
# them and fails every click below loudly.
ICONS = {
    "hello": (20, 54),
    "vedit": (100, 54),
    "term": (180, 54),
    "calc": (260, 54),
    "files": (20, 102),
    "picview": (100, 102),
    "prefs": (180, 102),
    "clock": (260, 102),
    "play": (20, 150),
    "sticky": (100, 150),
}

# QEMU delivers pointer motion to the guest's PS/2 mouse (the model
# feeds the 8042, and ps2mouse.cpp reads the deltas 1:1), so the host
# coordinates are the guest's pixels. Three edges of the protocol
# matter:
#
#   - a delta beyond ±127 has to be re-signed and QEMU gets that wrong,
#     so a leg travels in MOUSE_CHUNK steps;
#   - the i8042 model drops bytes when events arrive faster than the
#     guest drains them (the same mailbox swallow the _type() keys
#     fight, see above): a packet that is spent comes back a second
#     time and the one behind it is dropped, so the guest ends up
#     mid-packet and the driver's sync catch discards the rest. OSHA
#     says space the events like a person types - one packet a pass is
#     reliable, three a pass is a coin flip. MOUSE_STEP is that breath;
#   - the very first event of a desktop session can be swallowed too,
#     so mouse_warmup() spends one real move on it rather than a real
#     one later. A (0, 0) packet may not emit anything at all, so the
#     warmup is an actual small move in a direction that does not
#     matter.
MOUSE_CHUNK = 60
MOUSE_STEP = 0.12


def mouse_warmup(guest):
    """Let the session's first PS/2 event die on a move to nowhere: a
    real (small) move, so there is a packet to swallow. The desktop
    starts the pointer at the screen centre, so a few pixels either way
    on the bare desktop changes nothing visible nor countable."""
    mouse_move(guest, -3, 5)
    time.sleep(0.5)


def mouse_move(guest, dx, dy):
    """Move the pointer by `dx`, `dy` pixels, in steps the protocol can
    carry. The PS/2 path is 1:1, so the arithmetic is exact."""
    while dx or dy:
        x = max(-MOUSE_CHUNK, min(MOUSE_CHUNK, dx))
        y = max(-MOUSE_CHUNK, min(MOUSE_CHUNK, dy))
        dx -= x
        dy -= y
        guest.qmp.hmp("mouse_move %d %d" % (x, y))
        time.sleep(MOUSE_STEP)


def mouse_button(guest, down):
    """Set the left button to `down`, as a state and not as a toggle.

    QMP's input-send-event says which state the button is in, where the
    human-monitor `mouse_button` command only ever presses: a
    `mouse_button 0` there is not a release, so one click anywhere in
    the run leaves the host holding the button down for the rest of the
    boot. Every mouse packet after that carries "button down", the
    desktop's press-and-release edge never happens again, and the next
    click of the run does nothing at all.

    That matters most for the click that ends a desktop session (the
    panel's Exit button): the guest has already turned reporting off by
    the time the release would be sent, so the host keeps the button
    down into the next session."""
    event = {"type": "btn", "data": {"down": down, "button": "left"}}
    guest.qmp.cmd("input-send-event", events=[event])
    time.sleep(0.25)


def mouse_click(guest):
    """Press and release the left button, then let the desktop settle
    and the click's effects clear.

    The release comes first, and not just to be tidy: a click whose
    press closed the desktop leaves nothing to release it, so the next
    click in the run would be swallowed by a button that is still down
    from the last one (see mouse_button). Sending the release while the
    desktop is up again re-establishes the state the press needs."""
    mouse_button(guest, False)
    mouse_button(guest, True)
    mouse_button(guest, False)
    time.sleep(0.5)


def panel_calc_label(shot):
    """True when the panel shows the calculator's button label.

    The panel base is (48, 48, 71) everywhere short of the right-hand
    clock, and a desktop running only the terminal has nothing in the
    span x~140..200 that shares the dark (31, 31, 47) of a button
    outline. Spawning the calculator puts its label there (x~148..154)
    even though the window itself is fully covered and invisible, so it
    is a reliable sign the spawn happened."""
    w, h, px = shot
    for y in range(2, 30):
        for x in range(140, 200):
            b = (y * w + x) * 3
            if px[b] == 0x1F and px[b + 1] == 0x1F and px[b + 2] == 0x2F:
                return True
    return False


# Verifiable pointer navigation for the occlusion sessions.
#
# The pointer is aimed from screenshots, and that is all there is to aim
# it with: the desktop swallows the keyboard while it is up, so nothing
# can be typed at the guest to move the pointer, and /proc/gfx has no
# pointer in it. What a screenshot does offer is the pointer itself - but
# only where the desktop is not covered by its own windows. The terminal
# fills most of the screen, so a position on top of it cannot be read at
# all: a move there repaints nothing that shows the arrow, and a reading
# taken from such a frame is a reading of whatever else the desktop was
# doing. So the navigation works in two halves:
#
#   - on the open desktop - the floor below the terminal, the widest
#     part of the screen - the pointer is read exactly, one packet at a
#     time, each leg verified by the frame it lands in. That is where
#     the pointer is put when it has to be *known*, which is the parked
#     screenshot's spot;
#
#   - and where the pointer has to *end*, over a covered window or on
#     the panel, the run of packets that carries it there cannot be
#     watched - everything along the way is covered - so the run is
#     checked by coming back. A run that lost a packet puts the pointer
#     back somewhere other than where it left from, which is a spot the
#     screen can show, so the check is a fact rather than a hope. The
#     run is then sent once more and the click that follows is what the
#     caller verifies: the calculator's panel button, or the desktop
#     leaving.
#
# Both halves assume nothing about how reliably a packet arrives. The
# PS/2 path is not something to trust like that (see MOUSE_STEP above):
# the first event or two of a desktop session are swallowed, a packet
# can be dropped outright, and the screen edge clamps the last of a run.
# A leg that is measured is a leg that counts, and one that was lost
# costs a leg instead of the test.
#
# The spots themselves, in the desktop's own terms (gui.cpp: PANEL_H=34,
# ICON_ORIGIN_Y=PANEL_H+8, 24x24 tiles on 80x48 cells from (8, 42); the
# left cluster of the panel is Close, Min, Reboot, Exit, each button
# text_width(label)+10 wide from x 6).
MOUSE_STEP_GAP = 0.2                   # the breath between packets
PARK_TOL = 16                          # a park this close is on the spot
PARK_PACKET = 60                       # one packet: what a leg moves
OCCL_SCREEN_W, OCCL_SCREEN_H = 1024, 768   # the desktop's screen
OCCL_FLOOR_Y = 500                    # below the windows: open
                                       # desktop the whole width of it
BEACON = (700, 600)                    # open floor, where both sessions
                                       # park for their screenshot
EXIT_BUTTON = (22, 16)                 # the panel's Exit button: it starts
                                       # at x 6 and is 32 wide, and one
                                       # slot over is Reboot
OCCL_DBG_DIR = None                    # a dir to trace the frames the
                                       # navigation is measured from
OCCL_DBG_N = [0]
TRACE_NAV = False                      # --trace-nav: fill that dir


def occl_ground(guest):
    """A fresh screendump, available between navigation steps.

    OCCL_DBG_DIR is for a failing run only: the navigation is measured
    from frames, so a trace of them is the only way to see what a
    misreading was looking at - and a session draws hundreds of them,
    which is more than /tmp wants to hold, so it is off unless asked."""
    global OCCL_DBG_DIR
    if OCCL_DBG_DIR is not None:
        OCCL_DBG_N[0] += 1
        p = os.path.join(OCCL_DBG_DIR, "%03d.ppm" % OCCL_DBG_N[0])
        guest.qmp.hmp("screendump " + p)
        time.sleep(0.25)
        return read_ppm(p)
    path = tempfile.mktemp(suffix=".ppm")
    guest.qmp.hmp("screendump " + path)
    time.sleep(0.25)
    shot = read_ppm(path)
    os.unlink(path)
    return shot


def occl_diff(frm, cur):
    """The pixels where `cur` differs from `frm`, as a set of (x, y).

    A frame is whole 3-byte pixels, so the two are compared as 32-bit
    words (a 1024-pixel row is 3072 bytes, so no word straddles one): a
    pixel that changed changes the bytes of the word it sits in, and the
    word's offset says which pixel that is."""
    pa = memoryview(frm[2]).cast("I")
    pb = memoryview(cur[2]).cast("I")
    w = frm[0]
    return {(((i * 4) // 3) % w, ((i * 4) // 3) // w)
            for i in range(len(pa)) if pa[i] != pb[i]}


def occl_settle(guest, tries=14, wait=0.5):
    """A frame the desktop has finished drawing, found by waiting for two
    frames in a row to be the same.

    The desktop puts a move on the screen late and at no fixed rate - the
    same move has shown up half a second after it was sent and gone
    unseen for three - so a frame taken on a timer is a frame of
    whatever the screen happens to be holding, and a difference taken
    against one is a difference against a moment that has not finished
    happening. Two frames in a row that agree is the one way to ask
    whether it has."""
    prev = occl_ground(guest)
    for _ in range(tries):
        time.sleep(wait)
        cur = occl_ground(guest)
        if cur == prev:
            return cur
        prev = cur
    return prev


def occl_spots(blobs, near=24):
    """The changed pixels as spots: where each one is centred, and the
    pixels in it relative to its own top-left corner.

    That last part is what tells the two spots of a leg apart from two
    unrelated things that happen to change: the pointer is one fixed
    40-pixel arrow, so the spot it left and the spot it arrived at are
    the same shape, and nothing else on the desktop is.

    Pixels join the first spot close enough to hold them, and closeness
    is measured from where a spot started rather than from its middle.
    The pointer is drawn as a run of pixels with gaps in it, so a spot
    has to be gathered by reach and not by neighbour - and a centre that
    moved while the spot was being gathered would let its own pixels be
    counted into it twice, and two gaps one packet apart would join into
    one spot. Two footprints of a leg are a packet apart, which is
    further than `near`, so a leg's two spots are two spots."""
    spots = []
    for (x, y) in blobs:
        for s in spots:
            if abs(x - s["seed"][0]) < near and abs(y - s["seed"][1]) < near:
                s["pix"].add((x, y))
                break
        else:
            spots.append({"seed": (x, y), "pix": {(x, y)}})
    for s in spots:
        x0 = min(x for (x, y) in s["pix"])
        y0 = min(y for (x, y) in s["pix"])
        s["at"] = (sum(x for (x, y) in s["pix"]) // len(s["pix"]),
                   sum(y for (x, y) in s["pix"]) // len(s["pix"]))
        s["n"] = len(s["pix"])
        s["shape"] = frozenset((x - x0, y - y0) for (x, y) in s["pix"])
    return spots


OCCL_ARROW = [None]                   # the pointer's own bitmap, learned
OCCL_SAY = None                       # a file to trace navigation into


def occl_say(*what):
    """Trace a navigation step when asked, on one line each: the legs,
    what each one read, and how long it took, which is the whole of what
    there is to see when a run went somewhere nobody expected."""
    if OCCL_SAY is not None:
        OCCL_SAY.write(" ".join(str(w) for w in what) + "\n")
        OCCL_SAY.flush()


def occl_is_arrow(spot):
    """Whether a spot is the pointer, checked against the pointer's own
    shape and not against the shape of what it was drawn over.

    The pointer is one arrow in a 7-by-12 box, and it is the only thing
    on the desktop that is - no text, no icon, no piece of wallpaper is
    a moving sprite of that shape - so the first spot of that shape is
    what the pointer looks like and every later one is matched against
    it.

    How much of the arrow a spot shows is up to the floor underneath it:
    the arrow is drawn with a colour of its own over a wallpaper, and a
    pixel that lands on the exact colour it is drawn over does not change
    at all, so the same arrow reads as 45 changed pixels over one part of
    the floor and as 31 over another. A spot is therefore counted as the
    pointer when two thirds of it is the arrow, and the arrow is widened
    with every pixel a reading adds to it - the reference is what the
    pointer has been seen to be, not what it looked like once."""
    shape = spot["shape"]
    if not shape or not 24 <= spot["n"] <= 52:
        return False
    if max(x for (x, y) in shape) + 1 > 9:
        return False
    if not 9 <= max(y for (x, y) in shape) + 1 <= 14:
        return False
    if OCCL_ARROW[0] is None:
        OCCL_ARROW[0] = shape
        return True
    if len(shape & OCCL_ARROW[0]) < max(20, spot["n"] * 2 // 3):
        return False
    OCCL_ARROW[0] = OCCL_ARROW[0] | shape
    return True


def occl_arrive(guest, before, dx, dy, at=None, tries=8, wait=0.8):
    """Where the leg of (dx, dy) came to rest, or None when the screen
    never shows where.

    A leg is one packet, and the screen answers a leg by showing the
    pointer at the end of it. When the pointer was at `at` before the
    leg, that is a place to look: the changed pixels are split along the
    line across the middle of the leg, and the pointer's own half - a
    bitmap the same one it always is - is the answer. Nothing else in
    the frame is held against it, because a desktop erases where the
    pointer was in its own time, and a frame taken between one move and
    the next shows the pointer's old place and the one before that
    besides: a reading that turned on how many arrows the frame holds
    would throw away every leg that followed a fast one.

    Without `at` there is no middle to split along, and then the leg is
    read the other way round - the pointer is the arrow furthest along
    the leg, and the footprint it came from is a packet behind that.

    Either way it has to be waited for, because the desktop puts a move
    on the screen late and at no fixed rate - the same leg has shown up
    half a second after it was sent and gone unseen for three - so the
    frame is polled until the leg is in it, and a leg that never turns
    up costs a wait and not a guess."""
    if not dx and not dy:
        return None
    for _ in range(tries):
        time.sleep(wait)
        blobs = occl_diff(before, occl_ground(guest))
        if at is not None:
            # The line across the middle of the leg, so that the pointer
            # at the end of it and the footprint at the start of it are
            # two halves of the frame however short the leg is.
            mx = (at[0] + at[0] + dx) / 2.0
            my = (at[1] + at[1] + dy) / 2.0
            want = (at[0] + dx, at[1] + dy)
            # The half of the frame the pointer moved into, and inside it
            # only what lies where the pointer came to rest: the desktop
            # can still be showing an older move of its own, and that
            # ghost is not this leg's answer.
            pix = {(x, y) for (x, y) in blobs
                   if (x - mx) * dx + (y - my) * dy > 0
                   and (x - want[0]) ** 2 + (y - want[1]) ** 2 <= 144}
            if not pix:
                continue
            spot = {"at": (sum(x for (x, y) in pix) // len(pix),
                           sum(y for (x, y) in pix) // len(pix)),
                    "n": len(pix)}
            x0 = min(x for (x, y) in pix)
            y0 = min(y for (x, y) in pix)
            spot["shape"] = frozenset((x - x0, y - y0) for (x, y) in pix)
            if occl_is_arrow(spot):
                return spot["at"]
            continue
        spots = [s for s in occl_spots(blobs) if occl_is_arrow(s)]
        if len(spots) < 2:
            continue
        along = (1 if dx > 0 else -1) if dx else 0
        down = (1 if dy > 0 else -1) if dy else 0
        spots.sort(key=lambda s: s["at"][0] * along + s["at"][1] * down)
        arrived, left = spots[-1], spots[-2]
        if abs(arrived["at"][0] - left["at"][0] - dx) > 12 \
                or abs(arrived["at"][1] - left["at"][1] - dy) > 12:
            continue
        return arrived["at"]
    return None


def occl_step(guest, dx, dy, at=None):
    """Carry the pointer one leg of (dx, dy) and report where it came to
    rest, or None when the screen cannot say. `at` is where the pointer
    was, when the caller knows."""
    before = occl_settle(guest)
    mouse_move(guest, dx, dy)
    where = occl_arrive(guest, before, dx, dy, at)
    occl_say("leg (%+d,%+d) from %r -> %r" % (dx, dy, at, where))
    return where


def occl_find(guest, deadline=None):
    """Where the pointer is, read off the screen, or None when a sweep
    cannot find it.

    The pointer spends most of a session behind a window - the desktop
    starts it in the middle of the screen, which is behind the terminal,
    and a covered spot is a spot with one footprint and no reading - so
    finding it is a walk out onto the open floor, one packet a leg, and
    the first leg that lands where the screen shows both of its ends is
    the reading.

    Down first and as many legs as it takes, because the floor is open
    the whole width of the screen and a pointer anywhere in the top of
    it only has to keep going down to reach it. Then right, then back up
    and back left, for a pointer that has already been walked off the
    bottom of the screen."""
    here = None
    for dx, dy, legs in ((0, PARK_PACKET, 8), (PARK_PACKET, 0, 6),
                         (0, -PARK_PACKET, 6), (-PARK_PACKET, 0, 6)):
        for _ in range(legs):
            landed = occl_step(guest, dx, dy, here)
            if landed is not None:
                occl_say("find -> %r" % (landed,))
                return landed
            if deadline is not None and time.time() > deadline:
                return None
    occl_say("find -> None")
    return None


def occl_park(guest, target, tries=32, deadline=None):
    """Park the pointer on `target` on the open desktop, and return where
    it came to rest, or None when it never got there.

    One packet a leg, and every leg re-read, so a leg that was lost, or
    that arrived short, or that arrived on the spot and was read once
    more, all cost a leg and nothing else. A leg is a whole packet while
    the spot is a packet away and the exact distance once it is closer,
    because two footprints only read as two while they are further apart
    than the pointer is wide."""
    here = None
    for _ in range(tries):
        if here is None:
            here = occl_find(guest, deadline)
            if here is None:
                continue
        dx = target[0] - here[0]
        dy = target[1] - here[1]
        if abs(dx) <= PARK_TOL and abs(dy) <= PARK_TOL:
            return here
        if abs(dx) > PARK_PACKET or abs(dy) > PARK_PACKET:
            step = (PARK_PACKET if dx > 0 else -PARK_PACKET,
                    PARK_PACKET if dy > 0 else -PARK_PACKET)
        elif abs(dx) >= 16 or abs(dy) >= 16:
            step = (dx, dy)
        elif abs(dx) >= abs(dy):
            step = (16 if dx > 0 else -16, 0)
        else:
            step = (0, 16 if dy > 0 else -16)
        here = occl_step(guest, step[0], step[1], here)
        if here is not None and abs(target[0] - here[0]) <= PARK_TOL \
                and abs(target[1] - here[1]) <= PARK_TOL:
            occl_say("park %r on %r" % (here, target))
            return here
        if deadline is not None and time.time() > deadline:
            occl_say("park %r gave up" % (target,))
            return None
    occl_say("park %r -> None" % (target,))
    return None


def occl_run(guest, dx, dy):
    """Carry the pointer (dx, dy) in one-packet legs, without overshoot:
    a run that overshoots is a run that cannot come back, and the last
    packet of a run onto a button is the one that has to land on it."""
    while dx or dy:
        x = max(-PARK_PACKET, min(PARK_PACKET, dx))
        y = max(-PARK_PACKET, min(PARK_PACKET, dy))
        dx -= x
        dy -= y
        guest.qmp.hmp("mouse_move %d %d" % (x, y))
        time.sleep(MOUSE_STEP_GAP)
    occl_settle(guest)
    return True


def occl_where(guest, near):
    """Where the pointer is, read off the screen - and leave it there.
    None when it cannot be read.

    Reading a position costs a leg, because two footprints are what make
    a leg readable and one of them is where the pointer came from. This
    takes that leg on the open side of `near`, reads the spot the
    pointer came to rest on, and walks the leg back, so that the answer
    is the pointer's own position and the pointer is still on it: a
    caller told where the pointer is can move it from there, and a
    caller that finds it somewhere else can run from there without
    counting a leg twice.

    A leg the screen cannot show is walked back too, so that a read
    nobody got costs a wait and not a move."""
    if near is None:
        return None
    if near[0] + PARK_PACKET <= OCCL_SCREEN_W - 8:
        leg = (PARK_PACKET, 0)
    else:
        leg = (0, PARK_PACKET if near[1] + PARK_PACKET <= OCCL_SCREEN_H - 8
               else -PARK_PACKET)
    before = occl_settle(guest)
    mouse_move(guest, leg[0], leg[1])
    where = occl_arrive(guest, before, leg[0], leg[1], near)
    if where is None:
        return None
    occl_run(guest, -leg[0], -leg[1])
    return (where[0] - leg[0], where[1] - leg[1])


def occl_goto(guest, target, tries=3):
    """Carry the pointer to `target` where the desktop covers it, and
    leave it there - or None when no run could be checked.

    There is nothing to read on the way, so the run is taken out and
    taken back and the screen is asked afterwards: the pointer is read
    by a leg on the open desktop, and if it reads back exactly where it
    left from, then the way out and the way back came to nothing, which
    is the only claim a run the desktop covers can be held to. A run
    that lost a packet, one the screen edge clamped and one the guest
    dropped are each answered with a spot other than the one asked for,
    and the run is taken again from where the pointer actually is."""
    here = None
    for _ in range(tries):
        if here is None:
            here = occl_where(guest, occl_find(guest))
            if here is None:
                continue
        dx, dy = target[0] - here[0], target[1] - here[1]
        if not occl_run(guest, dx, dy) or not occl_run(guest, -dx, -dy):
            continue
        where = occl_where(guest, here)
        if where == here:
            return occl_run(guest, target[0] - where[0],
                            target[1] - where[1])
        if where is not None:
            here = where
    return None


# What the two occlusion sessions are expected to cost, and how far apart
# their frames may be. A session is not free: the terminal repaints itself
# whenever the shell inside it blinks or prints, and every one of those
# repaints is charged for as the window's whole rectangle joined to the
# pointer's box, since a transfer presents one bounding box for all of its
# damage. A session that walks the pointer off the calculator's tile, parks
# it on the floor and walks it to the Exit button moves about 15MB that
# way, so OCCLUSION_CEIL is many times over: a sanity bound saying a session
# costs about what an honest session costs, which is what catches a desktop
# that has started painting the screen on every pass - a whole frame is 3MB
# and a session has hundreds of passes.
#
# The bytes are not what proves the occlusion, and cannot be. A window under
# the terminal is damaged inside the terminal's own rectangle, and one
# bounding box covers both, so a compositor that composited the hidden
# window anyway would move exactly the same bytes as one that culled it.
# What proves the occlusion is OCCLUSION_SHOT_TOL below: the window under
# the terminal is on the screen or it is not, and the two sessions' frames,
# taken with the pointer parked on the same spot, say which.
OCCLUSION_CEIL = 260 * 1024 * 1024
OCCLUSION_SHOT_TOL = 2000
# A session with a window behind the terminal costs a little more than one
# without it, but not a fraction of a screen more, which is what painting
# it on every pass would come to. Only part of that extra scales with the
# session: the rest is the window being there at all - the click that
# opened it, its first composite, the button it puts in the panel - and
# that is one rectangle of damage however long the session goes on. So the
# extra is bounded by whichever is larger, a tenth of the control or one
# whole frame, and it is the tenth that still catches the regression this
# exists for: a compositor that composited the hidden window anyway moved
# 24MB more than the session without it, which is eight frames, so the
# bound keeps the whole of it with room for the noise either side.
#
# The frame is in there because the control is not a fixed size, and has
# just got seven times smaller. It used to move about 15MB, nearly all of
# it the pointer walking over the calculator's tile and being answered
# with a repaint of the whole window under it; a desktop that repaints
# only the pixels the pointer covered moves a seventh of that, and against
# a seventh the tenth no longer covers the cost of opening the window at
# all - which is the point of the fix, not a thing to be lenient about.
OCCLUSION_EXTRA = 0.10
OCCLUSION_FRAME = 1024 * 768 * 4


def run_occlusion(guest, result, verbose):
    """Two desktop sessions that move the same bytes twice, and the window
    neither of them ever shows.

    The occlusion case is the one the compositor exists for: a window
    drawn where the terminal is on top of it costs the desktop a blit
    per redraw and the user gets nothing at all for it, and no test that
    only looks at what is on screen can see a byte of that. So the
    desktop is run twice, the two sessions doing the same thing - open a
    terminal, move the pointer, sit there - and the second one also
    clicking the calculator's icon, which opens a window entirely under
    the terminal. What is checked:

      - the control session moves fewer than OCCLUSION_CEIL bytes in
        total, which is a session with nothing hidden behind the
        terminal costing about what an honest session costs;
      - the session with the calculator costs no more than
        OCCLUSION_EXTRA of the control session more, which is the click
        that opens it and the button it leaves in the panel, and not a
        compositor painting a screenful nobody can see on every pass;
      - and the two frames taken with the pointer parked on the same spot
        differ by no more than OCCLUSION_SHOT_TOL pixels. This is the
        one that proves the occlusion: a window under the terminal is on
        the screen in neither sense - not its own pixels, and not its
        absence either - and the bytes cannot tell the two apart, because
        the hidden window's damage lies inside the terminal's own
        rectangle.

    This is not a SUITE case because each session is a desktop of its
    own and the desktop swallows every key while it is up: there is no
    prompt for sh() to wait on, so the sessions are driven from here and
    the whole thing is judged at the end.
    """
    print("==> occlusion (two desktop sessions)")
    problems = []
    shotdir = tempfile.mkdtemp(prefix="vnu-occl-")

    def read_total():
        """The cumulative present() byte count the guest reports, read
        through the serial shell - which only answers with the desktop
        down, so this is how a session's own transfer is measured: on
        the way in and on the way out."""
        return present_total(guest.sh("cat /proc/gfx")[0])

    def session(tag, calculated):
        """One desktop session: (bytes moved, its parked frame).

        `calculated` is the whole difference between the two sessions -
        the click that opens the covered window. Everything else, the
        route the pointer walks and the time it spends on the screen, is
        the same, because a session's cost is its covered time times its
        pass rate and a session that skipped the route would be timed
        differently rather than measured the same."""
        global OCCL_DBG_DIR
        if TRACE_NAV:
            OCCL_DBG_DIR = os.path.join(shotdir, "nav-" + tag)
            os.makedirs(OCCL_DBG_DIR, exist_ok=True)
        before = read_total()
        if before is None:
            problems.append("the kernel's /proc/gfx has no present_total "
                            "line, so this check has nothing to measure")
            return None, None
        # The desktop swallows keystrokes, so any prompt that shows in
        # the log from here on means the desktop left.
        exit_frm = len(guest.tail(0))
        guest._type("gui term\n")
        time.sleep(4.0)
        mouse_warmup(guest)
        up = settled_shot(guest, os.path.join(shotdir, "%s-up.ppm" % tag))
        if not frame_is_desktop(up):
            problems.append("%s: the desktop did not come up (%dx%d)"
                            % (tag, up[0], up[1]))
            return None, None
        # Park the pointer on the calculator's tile and, in the second
        # session, click it. The click is the only difference between the
        # two sessions, and the window it opens is under the terminal: it
        # is on screen in neither sense, so what it would draw is painted
        # over by the terminal above it. The panel is the proof it
        # spawned - the covered window is never seen, but its panel
        # button label shows up a moment after the click, so a second
        # look catches it. A click that really did not spawn it is a
        # session failure and not a quiet pass: a session without the
        # calculator would be the terminal-only one bit for bit.
        if not occl_goto(guest, ICONS["calc"]):
            problems.append("%s: could not carry the pointer to the "
                            "calculator's tile" % tag)
            return None, None
        if not calculated:
            time.sleep(4.35)
        else:
            mouse_click(guest)
            check = os.path.join(shotdir, "%s-spawn.ppm" % tag)
            for wait in (2.0, 1.5):
                time.sleep(wait)
                guest.qmp.hmp("screendump " + check)
                time.sleep(0.4)
                if panel_calc_label(read_ppm(check)):
                    break
            if not panel_calc_label(read_ppm(check)):
                problems.append("%s: the calculator did not spawn (its panel "
                                "button never appeared)" % tag)
                return None, None
        # Park both sessions on the same spot on the open floor, so the
        # frames below have the pointer in the same place and what they
        # differ by is the desktop, not the cursor.
        if occl_park(guest, BEACON) is None:
            problems.append("%s: could not park the pointer on the floor"
                            % tag)
            return None, None
        shot = settled_shot(guest, os.path.join(shotdir, "%s.ppm" % tag))
        # Leave via the panel's Exit button, carried onto it and checked
        # by the way back, because one slot over is Reboot: a click that
        # lands wrong restarts the machine and answers with a login
        # prompt, which is a loud failure and never a quiet pass. The run
        # is a straight line up from the floor, so a packet lost on the
        # way puts the click on the panel's bare background instead -
        # which does nothing at all, and is caught by the desktop still
        # being up afterwards.
        if not occl_goto(guest, EXIT_BUTTON):
            problems.append("%s: could not carry the pointer to the panel's "
                            "Exit button" % tag)
            return None, None
        mouse_click(guest)
        try:
            guest.wait_ready(exit_frm, timeout=8.0)
        except TimeoutError:
            problems.append("%s: the desktop did not leave via the panel's "
                            "Exit button" % tag)
            return None, None
        after = read_total()
        if after is None or after < before:
            problems.append("%s: the guest reported a present_total that "
                            "went backwards" % tag)
            return None, None
        return after - before, shot

    bare, bare_shot = session("a", False)
    if bare is not None:
        print("    control session moved %d bytes" % bare)
        if bare > OCCLUSION_CEIL:
            problems.append("a desktop with nothing behind the terminal "
                            "moved %d bytes, over the %d a session with no "
                            "hidden window should cost"
                            % (bare, OCCLUSION_CEIL))
    covered, covered_shot = session("b", True)
    if covered is not None:
        print("    session with the calculator moved %d bytes" % covered)
        if covered > OCCLUSION_CEIL:
            problems.append("a desktop with a window hidden behind the "
                            "terminal moved %d bytes, over the %d an "
                            "honest session costs"
                            % (covered, OCCLUSION_CEIL))
        if bare is not None:
            allowed = max(int(bare * OCCLUSION_EXTRA), OCCLUSION_FRAME)
            if covered - bare > allowed:
                problems.append("the window under the terminal cost %d bytes "
                                "more than the session without it, over the %d "
                                "more such a session is allowed"
                                % (covered - bare, allowed))
    if bare_shot is not None and covered_shot is not None:
        apart = diff_pixels(bare_shot, covered_shot)
        print("    the two parked frames differ by %d pixels" % apart)
        if apart > OCCLUSION_SHOT_TOL:
            problems.append("the frames of the two sessions differ by %d "
                            "pixels, over the %d a window hidden under the "
                            "terminal can account for"
                            % (apart, OCCLUSION_SHOT_TOL))

    if problems:
        for p in problems:
            print("    FAIL %s" % p)
        keep_frames(shotdir, "occlusion")
        result.failed.append(("occlusion", "", problems))
    else:
        result.passed += 1
        shutil.rmtree(shotdir, ignore_errors=True)


def read_ppm(path):
    """(width, height, rgb bytes) of a binary PPM, as screendump writes."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise RuntimeError("%s is not a binary PPM" % path)
    fields, i = [], 2
    while len(fields) < 3:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            i = data.index(b"\n", i) + 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    width, height, _maxval = fields
    return width, height, data[i + 1:i + 1 + width * height * 3]


def colours(px):
    """The set of RGB triples in a frame."""
    return set(zip(px[0::3], px[1::3], px[2::3]))


def keep_frames(shotdir, name):
    """Copy the frames of a failed graphics check where they can be read.

    A screendump is the only evidence those checks have, and it goes away
    with the scratch directory, so a failure that only shows up in a full
    run is unrepeatable without it. The copy is named after the check.
    """
    keep = "/tmp/vnu-%s-frames" % name
    shutil.rmtree(keep, ignore_errors=True)
    shutil.copytree(shotdir, keep)
    print("    frames kept in %s" % keep)
    return keep


def settled_shot(guest, path, budget=2.0, quiet=200):
    """Screendump into path, once the display has stopped changing.

    A dump is a snapshot of what QEMU has repainted so far, and VGA text
    mode redraws its cells over a few refreshes, so a dump taken the
    instant the desktop gives the console back catches a half-drawn
    screen and reads as a console that came back wrong. So keep dumping
    until two frames in a row agree. The budget is small (a blinking
    cursor is worth a couple of dozen pixels, hence `quiet`) and a mode
    switch that really does stall the guest stays visible: two seconds
    is far more than a repaint needs, and the caller still gets whatever
    was on screen if the budget runs out.
    """
    prev = path + ".prev"
    frame = None
    deadline = time.time() + budget
    while True:
        guest.qmp.hmp("screendump " + prev)
        time.sleep(0.2)
        guest.qmp.hmp("screendump " + path)
        a, frame = read_ppm(prev), read_ppm(path)
        if (a[0], a[1]) == (frame[0], frame[1]) and \
                diff_pixels(a[2], frame[2]) <= quiet:
            return frame
        if time.time() >= deadline:
            return frame


def diff_pixels(a, b):
    """How many pixels differ between two frames of the same size."""
    return sum(1 for k in range(0, min(len(a), len(b)), 3)
               if a[k:k + 3] != b[k:k + 3])


# The console is 80x25 cells of 16 pixels, the text mode QEMU renders as
# 720x400 with a 9-pixel cell (the ninth column of a VGA text cell is
# blank, and it is what makes the mode 720 rather than 640 wide). A
# display too narrow for 80 such cells gets 8-pixel ones - that is what
# the kernel draws on a 640-wide screen - so a console on a graphics
# display is 720 wide, or 640 on a narrow one.
TEXT_COLS, TEXT_ROWS = 80, 25
TEXT_CELL_W, TEXT_ROW_H = 9, 16
TEXT_W, TEXT_H = TEXT_COLS * TEXT_CELL_W, TEXT_ROWS * TEXT_ROW_H
TEXT_CELL_W_NARROW = 8
TEXT_W_NARROW = TEXT_COLS * TEXT_CELL_W_NARROW

# Pixels brighter than this count as "something is on the screen". A
# VGA text cell is never lit everywhere (a space is black), so this is
# well under the lit area of a single glyph and well over a black frame.
LIT_MIN = 24

# A text console with a boot banner and a prompt on it lights a few
# thousand pixels; a blinking cursor lights a few dozen. The floor sits
# between the two on purpose: a screen that lost its console is black
# (0), and anything that drew something at all passes.
CONSOLE_LIT_MIN = 1000

# A text console is mostly nothing: a few lines of text and a cursor in
# 2000 cells. A frame that is a desktop with something on it is not, and
# the floor below is far enough under 80% to notice either way.
BLANK_CELLS_MIN = 0.70


def lit_pixels(frame):
    """How many pixels of a frame are not (near) black."""
    data = frame[2]
    return sum(1 for k in range(0, len(data), 3)
               if data[k] > LIT_MIN or data[k + 1] > LIT_MIN or
                  data[k + 2] > LIT_MIN)


def console_cells(frame, x0=0, y0=0, cell_w=TEXT_CELL_W):
    """The console-sized region of a frame, as 9x16 cells of lit counts.

    A text mode is redrawn over a few refreshes, so two frames of the same
    console differ in more than the cursor cell and a pixel-for-pixel
    comparison of the two would be a flaky test. What a console has and
    a picture of a desktop does not is its shape: text in a few lines of
    cells, and nothing at all in the rest.
    """
    width, height, data = frame
    grid_w = TEXT_COLS * cell_w
    if x0 < 0 or y0 < 0 or x0 + grid_w > width or y0 + TEXT_H > height:
        return None
    cells = []
    for row in range(TEXT_ROWS):
        for col in range(TEXT_COLS):
            lit = 0
            for y in range(y0 + row * TEXT_ROW_H, y0 + row * TEXT_ROW_H + TEXT_ROW_H):
                base = (y * width + x0 + col * cell_w) * 3
                for k in range(base, base + cell_w * 3, 3):
                    if (data[k] > LIT_MIN or data[k + 1] > LIT_MIN or
                            data[k + 2] > LIT_MIN):
                        lit += 1
            cells.append(lit)
    return cells


def console_grid(frame):
    """Where the console is in a frame, and how wide its cells are.

    Two kinds of display can show one. A text mode is the console, the
    whole frame, 720x400 with 9-pixel cells. A display with no text mode
    of its own - a virtio-gpu - has the same grid drawn in the middle of
    whatever graphics mode was last in use, 9-pixel cells too unless the
    screen is too narrow for 80 of those, which is 8-pixel cells and a
    640-wide grid. None if the frame cannot hold a console at all.
    """
    width, height = frame[0], frame[1]
    if (width, height) == (TEXT_W, TEXT_H):
        return 0, 0, TEXT_CELL_W
    narrow = width < TEXT_W
    cell_w = TEXT_CELL_W_NARROW if narrow else TEXT_CELL_W
    grid_w = TEXT_W_NARROW if narrow else TEXT_W
    if width < grid_w or height < TEXT_H:
        return None
    return (width - grid_w) // 2, (height - TEXT_H) // 2, cell_w


def console_at_boot(frame, problems, where):
    """At a prompt, before anything has taken the display, what the host
    shows is the card's own text mode: 720x400, with the console on it.

    A display that comes up as a black graphics mode instead - a driver
    that took the scanout and never gave it back - fails here, which is
    what the report of this bug looked like.
    """
    if (frame[0], frame[1]) != (TEXT_W, TEXT_H):
        problems.append("%s: the display is %dx%d, the text mode is %dx%d "
                        "- the console is not on the screen"
                        % (where, frame[0], frame[1], TEXT_W, TEXT_H))
        return
    check_console_shape(frame, 0, 0, TEXT_CELL_W, problems, where)


def console_on_screen(frame, problems, where):
    """The console is on the display, and not only in the serial log.

    A prompt lives in 0xB8000, and a host pointed at a virtio-gpu scanout
    renders that nowhere: a driver that takes the screen for itself and
    does not give it back leaves the last frame there while the serial log
    goes on typing into a console nobody can see. So this is checked on
    pixels.

    Two kinds of display can show a console. One does a text mode, and the
    console is the whole frame. The other has none - a virtio-gpu - and
    draws the same 720x400 grid in the middle of whatever graphics mode was
    last in use, which is what vgfx::present_text does when a desktop
    session ends.
    """
    grid = console_grid(frame)
    if grid is None:
        problems.append("%s: the display is %dx%d, too small for a console"
                        % (where, frame[0], frame[1]))
        return
    x0, y0, cell_w = grid
    check_console_shape(frame, x0, y0, cell_w, problems, where)


def check_console_shape(frame, x0, y0, cell_w, problems, where):
    """Text is on the console region, and it is shaped like a console."""
    cells = console_cells(frame, x0, y0, cell_w)
    if cells is None:
        problems.append("%s: no console in the %dx%d display"
                        % (where, frame[0], frame[1]))
        return
    lit = sum(cells)
    if lit < CONSOLE_LIT_MIN:
        problems.append("%s: a console with %d lit pixels, expected more "
                        "than %d - the console is on the screen and nobody "
                        "can read it"
                        % (where, lit, CONSOLE_LIT_MIN))
    blank = sum(1 for c in cells if not c) / float(len(cells))
    if blank < BLANK_CELLS_MIN:
        problems.append("%s: %.0f%% of the console cells are empty, expected "
                        "at least %.0f%% - what is on the screen is not a "
                        "text console" % (where, blank * 100.0,
                                          BLANK_CELLS_MIN * 100.0))


def console_mask(frame):
    """The console grid of a frame as lit pixels, on 8-pixel cells.

    A 9-pixel cell's ninth column is blank, so a grid of 9-pixel cells
    and one of 8-pixel cells are the same console at two sizes. Dropping
    that column puts every console on 8-pixel cells, which is what lets
    frames from displays in different modes be compared at all.

    A text mode redraws its cells over a few refreshes and a console drawn
    as pixels rounds the DAC to 8-bit channels differently (168 against
    170 for the same 6-bit gray), so what two frames of the same console
    have in common is the pattern of what is lit, not the bytes.
    """
    grid = console_grid(frame)
    if grid is None:
        return None
    x0, y0, cell_w = grid
    width, data = frame[0], frame[2]
    out = bytearray()
    for y in range(y0, y0 + TEXT_H):
        base = (y * width + x0) * 3
        for col in range(TEXT_COLS):
            cell = base + col * cell_w * 3
            for k in range(cell, cell + 8 * 3, 3):
                out.append(1 if (data[k] > LIT_MIN or data[k + 1] > LIT_MIN or
                                 data[k + 2] > LIT_MIN) else 0)
    return bytes(out)


def driver_name(proc_gfx):
    """The driver /proc/gfx says owns the display."""
    m = re.search(r"^driver\t(\S+)$", proc_gfx, re.MULTILINE)
    return m.group(1) if m else None


def palette_colours(proc_gfx):
    """The palette /proc/gfx reports, as (r, g, b) tuples: sixteen
    RRGGBB words, the same table a program gets from gfx_palette(2)."""
    m = re.search(r"^palette\t([0-9a-f ]+)$", proc_gfx, re.MULTILINE)
    if not m:
        return set()
    out = set()
    for word in m.group(1).split():
        if len(word) != 6:
            return set()
        out.add((int(word[0:2], 16), int(word[2:4], 16), int(word[4:6], 16)))
    return out


def depth_name(proc_gfx):
    """The depth /proc/gfx reports, as a string."""
    m = re.search(r"^bpp\t(\d+)$", proc_gfx, re.MULTILINE)
    return m.group(1) if m else None


# The depth each driver composites in, which is the depth a program draws
# in (see vgfx.h): the VBE card is 8bpp with a DAC in front of it, a
# virtio-gpu scanout is 32bpp with none.
WANT_DEPTH = {"vga": "8", "virtio-gpu": "32"}


def scrolled_diff(before, after):
    """How much a text console changed, allowing for what scrolled.

    The console frames a desktop session is bracketed by are not taken at
    the same point in the same scrollback: between them the harness types
    the commands that start and stop the desktop, and every command's echo
    is a line, so the console has legitimately scrolled by then - by more
    than a line or two when a lot has been typed since the last clear. A
    full screen of text shifted even one line looks like a wholesale
    change even though every glyph came back, so the frames are compared
    at every line offset and the smallest difference is what says whether
    the console came back the way it was. Anything actually broken (a
    garbled font, the desktop still up) matches at no offset at all.

    Only the console is compared, and only for what is lit in it: a
    display with no text mode shows the same grid drawn into the middle of
    a graphics frame, and the two spellings of the same gray are not the
    same bytes. None if either frame has no console in it to compare.
    """
    a = console_mask(after)
    b = console_mask(before)
    if a is None or b is None:
        return None
    width = TEXT_COLS * 8
    best = None
    for step in range(0, TEXT_ROWS):
        top = step * TEXT_ROW_H
        span = (TEXT_ROWS - step) * width * TEXT_ROW_H
        diff = sum(1 for x, y in zip(b[top * width * TEXT_ROW_H:], a[:span])
                   if x != y)
        if best is None or diff < best:
            best = diff
    return best


def changed_rows(a, b, width, height):
    """Per-row count of pixels that differ between two frames."""
    counts = []
    for y in range(height):
        base = y * width * 3
        counts.append(sum(1 for x in range(width)
                          if a[base + x * 3:base + x * 3 + 3]
                          != b[base + x * 3:base + x * 3 + 3]))
    return counts


def changed_bbox(a, b, width, y_from):
    """(x0, y0, x1, y1) of the pixels that differ between two frames, from
    row `y_from` down. A bounding box is what a window is: the frame
    around a desktop is the whole screen, so a box is how a check can
    tell "the app drew here" from "something changed"."""
    xs0 = ys0 = None
    x1 = y1 = -1
    for y in range(y_from, a[1]):
        base = y * width * 3
        for x in range(width):
            if a[2][base + x * 3:base + x * 3 + 3] != \
               b[2][base + x * 3:base + x * 3 + 3]:
                if xs0 is None:
                    xs0, ys0 = x, y
                x1, y1 = x, y
    if xs0 is None:
        return None
    return xs0, ys0, x1, y1


def longest_run(counts, min_changed):
    """First and last row of the longest run of rows with >= min_changed
    differences. Returns (-1, -1) when no row qualifies."""
    best_top = best_bottom = -1
    top = None
    for y, n in enumerate(list(counts) + [0]):
        if n >= min_changed:
            if top is None:
                top = y
        elif top is not None:
            if y - top > best_bottom - best_top:
                best_top, best_bottom = top, y - 1
            top = None
    return best_top, best_bottom


def window_columns(px, q, width, rows, min_changed):
    """The columns the window covers, from the rows of `rows` that
    changed wholesale: those are canvas rows, and the first and the last
    column that differ from the bare desktop on any of them are the two
    edges of the frame.

    Rows that changed only in a few columns cannot be used to measure
    the width, and the middle of the window is often one of them: a
    canvas in the display's own colours agrees with the backdrop the
    desktop drew there, and what is left is the frame - two columns."""
    x0, x1 = None, None
    for y, n in enumerate(rows):
        if n < min_changed:
            continue
        base = y * width * 3
        for x in range(width):
            if px[base + x * 3:base + x * 3 + 3] != q[base + x * 3:base + x * 3 + 3]:
                if x0 is None:
                    x0 = x
                x1 = x
                break
        for x in range(width - 1, -1, -1):
            if px[base + x * 3:base + x * 3 + 3] != q[base + x * 3:base + x * 3 + 3]:
                if x1 is None or x > x1:
                    x1 = x
                break
    return (x0, x1) if x0 is not None else (0, width - 1)


def distinct_colours(px, width, top, bottom, x0, x1):
    """The colours inside the window's rows and columns, sampled every
    fourth pixel: a picture in one of them, a blank canvas in one or
    two, a canvas the compositor could not read in none at all."""
    seen = set()
    for y in range(top, bottom + 1, 4):
        base = y * width * 3
        for x in range(x0, x1 + 1, 4):
            seen.add(px[base + x * 3:base + x * 3 + 3])
    return seen


# The name the installed system is given, and therefore the one its
# prompt shows on the next boot. A dash and a digit are in it on
# purpose: both are legal and the harness must survive them.
INSTALLED_HOST = "vnu-test"


def run_install_scenario(iso, result, verbose, timeout, workdir):
    """vnu install onto a fresh disk, boot it, check what it boots as."""
    disk = os.path.join(workdir, "vnu-install.vhd")
    subprocess.run(["qemu-img", "create", "-f", "vpc", disk, "64M"],
                   check=True, stdout=subprocess.DEVNULL)
    print("==> install scenario (ISO -> %s)" % disk)
    with Guest(iso=iso, disk=disk, log=os.path.join(workdir, "install1.log")) as g:
        g.wait(r"VNU login:", 120.0)
        g.login()
        output, _ = g.sh("vnu install 0 %s" % INSTALLED_HOST, 180.0)
        check("install-write", output,
              [r"vnu install: complete\.",
               r"now called %s" % INSTALLED_HOST], [], result, verbose)
        # The machine the installer ran on took the name too, so the
        # very next prompt is the new one.
        check("install-prompt", g.tail(0),
              [r"root@%s:~\$ $" % INSTALLED_HOST], [], result, verbose)
        output, _ = g.sh("cat /etc/hostname", timeout)
        check("hostname-live", output, [r"^%s$" % INSTALLED_HOST], [],
              result, verbose)
        output, _ = g.sh("uname -n", timeout)
        check("uname-node-live", output, [r"^%s$" % INSTALLED_HOST], [],
              result, verbose)
    # Same disk, now without the ISO: this boot can only come from the
    # MBR/GRUB the installer stamped, and the name can only come from
    # the config record it wrote there.
    with Guest(disk=disk, boot="c",
               log=os.path.join(workdir, "install2.log")) as g:
        g.wait(r"VNU login:", 120.0)
        g.login()
        # The kernel adopted the name out of the disk's config record at
        # boot (its own boot note goes to the VGA console, not the log),
        # so the file and uname both hold it now.
        output, _ = g.sh("cat /etc/hostname", timeout)
        check("hostname-disk", output, [r"^%s$" % INSTALLED_HOST], [],
              result, verbose)
        output, _ = g.sh("uname -n", timeout)
        check("uname-node-disk", output, [r"^%s$" % INSTALLED_HOST], [],
              result, verbose)
        output, _ = g.sh("cat /proc/boot", timeout)
        check("proc-boot-disk", output,
              [r"^rootfs\tvfs$", r"^installed\tata0$", r"^shell\tvash$"], [],
              result, verbose)
        output, _ = g.sh("vnu fetch", timeout)
        check("vnu-fetch-disk", output,
              [r"installed on ata0", r"^Host +: +%s$" % INSTALLED_HOST], [],
              result, verbose)
    return result


# The host name the wizard types for itself.
WIZARD_HOST = "wizard-box"


def run_install_wizard(iso, result, verbose, timeout, workdir):
    """`vnu install` with no arguments: the full-screen wizard.

    The wizard owns the terminal until it is done, so - like the man
    pager - this is not a SUITE case: the keys are sent here, waiting for
    each prompt in turn, and the whole session is checked at the end.
    """
    disk = os.path.join(workdir, "vnu-wizard.vhd")
    subprocess.run(["qemu-img", "create", "-f", "vpc", disk, "64M"],
                   check=True, stdout=subprocess.DEVNULL)
    print("==> install wizard (ISO -> %s)" % disk)
    with Guest(iso=iso, disk=disk, log=os.path.join(workdir, "wizard.log")) as g:
        g.wait(r"VNU login:", 120.0)
        g.login()
        frm = len(g.tail(0))
        g._type("vnu install\n")
        # disk number, then the size (empty = whole disk), then the name
        g.wait(r"Enter the disk number", timeout, frm)
        g._type("0\n")
        g.wait(r"Size in MiB", timeout, frm)
        g._type("\n")
        g.wait(r"Host name", timeout, frm)
        g._type(WIZARD_HOST + "\n")
        g.wait(r"Continue\?", timeout, frm)
        g._type("y\n")
        g.wait(r"Install complete\.", 180.0, frm)
        # The wizard offers to reboot into what it just wrote; this
        # scenario stops here, the other one boots that disk.
        g.wait(r"Reboot now", timeout, frm)
        g._type("n\n")
        g.wait_prompt(timeout, frm)
    out = g.tail(frm)
    check("wizard-screens", out,
          [r"Select the disk to install VNU onto",
           r"Choose the partition size",
           r"Name this machine",
           r"Review your choices",
           r"^Host: +%s$" % WIZARD_HOST,
           r"Installing to disk 0",
           r"Install complete\.",
           r"disk is ready; reboot when you want to use it",
           r"now called %s" % WIZARD_HOST,
           r"root@%s:~\$ $" % WIZARD_HOST], [], result, verbose)
    return result


def main():
    global TRACE_NAV
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", default=os.path.join(REPO, "vnu/vnu.iso"),
                    help="image to boot (default: vnu/vnu.iso)")
    ap.add_argument("--gpu", action="store_true",
                    help="boot with a virtio-gpu display")
    ap.add_argument("--install", action="store_true",
                    help="also test installing to a disk and booting it")
    ap.add_argument("--install-only", action="store_true",
                    help="run only the install-to-disk scenarios")
    ap.add_argument("--wizard-only", action="store_true",
                    help="run only the interactive `vnu install` wizard")
    ap.add_argument("--only", action="append", default=[],
                    help="run only cases whose name contains this")
    ap.add_argument("--list", action="store_true", help="list case names")
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="per-command timeout in seconds")
    ap.add_argument("--log", help="serial log path (default: /tmp/vnu-test.log)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the temporary disk of the install scenario")
    ap.add_argument("--trace-nav", action="store_true",
                    help="keep every frame the occlusion check navigates "
                         "from (hundreds of MB, for a failing run only)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    TRACE_NAV = args.trace_nav

    if args.list:
        for name, command, _, _ in SUITE:
            print("%-18s %s" % (name, command))
        print("%-18s %s" % ("console-screen",
                            "text prompt on the display (screendump)"))
        print("%-18s %s" % ("man-pager", "man vcc (keys, then q)"))
        print("%-18s %s" % ("ctrl-c", "ctrl+c at the prompt (SIGINT)"))
        print("%-18s %s" % ("prefs-wallpaper",
                            "/apps/prefs/bin (arrows, enter, esc)"))
        print("%-18s %s" % ("gfx-surface",
                            "gui picview (screendump, esc)"))
        print("%-18s %s" % ("gfx-resolution",
                            "gui + F12 mode steps (screendump)"))
        print("%-18s %s" % ("gfx-damage",
                            "typing in a window (screendump + /proc/gfx)"))
        print("%-18s %s" % ("gfx-canvas-resize",
                            "gui calc (app request + corner drag)"))
        print("%-18s %s" % ("install-write", "vnu install 0 vnu-test"))
        print("%-18s %s" % ("install-wizard", "vnu install (keys)"))
        return 0

    if not os.path.exists(args.iso):
        print("no image at %s -- run 'make iso' first" % args.iso,
              file=sys.stderr)
        return 1
    for tool in ("qemu-system-i386", "qemu-img"):
        if shutil.which(tool) is None:
            print("missing host tool: %s" % tool, file=sys.stderr)
            return 1

    # The interactive checks are functions, not SUITE rows, so a filter
    # that names one of them selects no shell case and still runs.
    interactive = ("console-screen", "man-pager", "ctrl-c",
                   "prefs-wallpaper", "gfx-surface", "gfx-resolution",
                   "gfx-damage", "gfx-canvas-resize", "occlusion",
                   "vibecommander")
    cases = SUITE
    if args.only:
        cases = [c for c in SUITE
                 if any(sel in c[0] for sel in args.only)]
        if not cases and not any(sel in name for name in interactive
                                 for sel in args.only):
            print("no case matches %s" % args.only, file=sys.stderr)
            return 1

    result = Result()
    workdir = tempfile.mkdtemp(prefix="vnu-test-")
    try:
        if not args.install_only and not args.wizard_only:
            with Guest(iso=args.iso, gpu=args.gpu, log=args.log) as guest:
                run_suite(guest, cases, result, args.verbose, args.timeout)
                if not args.only or any(sel in "console-screen"
                                        for sel in args.only):
                    run_console_screen(guest, result, args.verbose,
                                       args.timeout)
                if not args.only or any(sel in "man-pager"
                                        for sel in args.only):
                    run_man_pager(guest, result, args.verbose, args.timeout)
                if not args.only or any(sel in "ctrl-c"
                                        for sel in args.only):
                    run_ctrlc(guest, result, args.verbose, args.timeout)
                if not args.only or any(sel in "prefs-wallpaper"
                                        for sel in args.only):
                    run_prefs_wallpaper(guest, result, args.verbose,
                                        args.timeout)
                if not args.only or any(sel in "gfx-surface"
                                        for sel in args.only):
                    run_gfx_surface(guest, result, args.verbose,
                                    args.timeout)
                if not args.only or any(sel in "gfx-resolution"
                                        for sel in args.only):
                    run_gfx_resolution(guest, result, args.verbose,
                                       args.timeout)
                if not args.only or any(sel in "gfx-damage"
                                        for sel in args.only):
                    run_gfx_damage(guest, result, args.verbose,
                                   args.timeout)
                if not args.only or any(sel in "gfx-canvas-resize"
                                        for sel in args.only):
                    run_gfx_canvas_resize(guest, result, args.verbose,
                                          args.timeout)
                if not args.only or any(sel in "occlusion"
                                        for sel in args.only):
                    run_occlusion(guest, result, args.verbose)
                if not args.only or any(sel in "vibecommander"
                                        for sel in args.only):
                    run_vibecommander(guest, result, args.verbose,
                                      args.timeout)
        if args.wizard_only:
            run_install_wizard(args.iso, result, args.verbose,
                               args.timeout, workdir)
        elif args.install or args.install_only:
            run_install_scenario(args.iso, result, args.verbose,
                                 args.timeout, workdir)
            run_install_wizard(args.iso, result, args.verbose,
                               args.timeout, workdir)
    except (TimeoutError, RuntimeError) as exc:
        print("harness error: %s" % exc, file=sys.stderr)
        result.failed.append(("harness", "", [str(exc)]))
    finally:
        if args.keep:
            print("workdir kept: %s" % workdir)
        else:
            shutil.rmtree(workdir, ignore_errors=True)

    total = result.passed + len(result.failed)
    print("\n%d/%d passed" % (result.passed, total))
    for name, output, problems in result.failed:
        if output:
            print("\n--- %s output ---\n%s" % (name, output))
    if result.failed and args.log is None:
        print("serial log: /tmp/vnu-test.log")
    return 1 if result.failed else 0


if __name__ == "__main__":
    sys.exit(main())
