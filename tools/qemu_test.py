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

    # ---- input --------------------------------------------------------

    def _type(self, text, hold_ms=10):
        """Send one key at a time: HMP `sendkey` takes a '+'/'-' chord or
        key, and a batch would either be rejected (spaces are not a
        separator) or apply the shift of `shift-a` to the whole batch. One
        call per character is unambiguous, and the hold time keeps the
        i8042 buffer from being overrun by the guest's own polling.

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
            time.sleep(0.01)
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
        end = self.wait_prompt(timeout, frm)
        chunk = self.tail(frm)
        return strip_echo(chunk, command), end

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
     [r"VNU system information", r"^OS +: +VNU 0\.5 \(vibe\)$",
      r"^Kernel +: +0\.5\.0$", r"^Host +: +vnu$", r"^Uptime +: +\d+ s$",
      r"^Shell +: +vash$",
      r"^Graphics +: +\w[\w-]* \d+x\d+$", r"^VCC +: +0\.5$",
      r"^Build +: +\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$"], []),
    ("vnu-version", "vnu version",
     [r"VNU version information", r"^OS +: +VNU 0\.5$",
      r"^Kernel +: +0\.5\.0$", r"^ABI +: +1$", r"^Arch +: +i386$"], []),
    ("vnu-size", "vnu size",
     [r"VNU image size", r"^kernel +: .*\(1 object\)$",
      r"^userspace: .*\(4[0-9] objects\)$", r"^libraries: .*\(2 objects\)$",
      r"^fonts +: .*\(1 object\)$", r"^resources: .*\(1[0-9] objects\)$",
      r"^total +: "], []),
    ("vnu-version-flag", "vnu --version", [r"^vnu \(VNU\) 0\.5$"], []),
    ("vnu-help", "vnu --help",
     [r"Usage: vnu \[COMMAND\]\.\.\.", r"fetch", r"version", r"size",
      r"install +install VNU onto a disk"], []),
    ("vnu-unknown", "vnu bogus", [r"unknown command: bogus"], []),
    ("vnu-noargs", "vnu", [r"Usage: vnu"], []),

    # -- the facts behind it --------------------------------------------
    ("proc-version", "cat /proc/version",
     [r"^version 0\.5 \(vibe\) i386$", r"^kernel 0\.5\.0$", r"^abi 1$",
      r"^built \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$"], []),
    ("proc-meminfo", "cat /proc/meminfo",
     [r"^MemTotal: +\d+ kB$", r"^PoolTotal: +\d+ kB$",
      r"^MemFree: +\d+ kB$", r"^MemUsed: +\d+ kB$"], []),
    ("proc-images", "cat /proc/images",
     [r"^kernel\t\d+\t1$", r"^userspace\t\d+\t\d+$",
      r"^libraries\t\d+\t2$", r"^total\t\d+\t\d+$"], []),
    ("proc-gfx", "cat /proc/gfx",
     [r"^driver\t(vga|virtio-gpu)$", r"^resolution\t\d+x\d+$", r"^bpp\t8$"], []),
    ("proc-boot", "cat /proc/boot",
     [r"^rootfs\tvfs$", r"^shell\tvash$"], [r"^installed\tata"]),
    ("proc-status", "cat /proc/self/status",
     [r"^Name:\tvash$", r"^Pid:\t\d+$", r"^PPid:\t\d+$"], []),
    ("proc-cpuinfo", "cat /proc/cpuinfo",
     [r"^processor\t: 0$", r"^vendor_id\t: "], []),
    ("uname-all", "uname -a",
     [r"^VNU vnu 0\.5\.0 vibe \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} i386 "],
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
    ("df", "df", [r"^Filesystem", r"^vfs +\d+ +\d+ +\d+ +\d+% /$"], []),
    ("motd", "cat /etc/motd", [r"Welcome to VNU"], []),
    ("echo", "echo hello world", [r"^hello world$"], []),
    ("seq", "seq 3", [r"^1\n2\n3$"], []),
    ("wc", "wc -l /proc/version", [r"^4 "], []),
    ("head", "head -1 /proc/version", [r"^version 0\.5"], []),
    ("tail", "tail -1 /proc/version", [r"^built "], []),
    ("grep", "grep kernel /proc/version", [r"^kernel 0\.5\.0$"], []),
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
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.list:
        for name, command, _, _ in SUITE:
            print("%-18s %s" % (name, command))
        print("%-18s %s" % ("man-pager", "man vcc (keys, then q)"))
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

    cases = SUITE
    if args.only:
        cases = [c for c in SUITE
                 if any(sel in c[0] for sel in args.only)]
        if not cases:
            print("no case matches %s" % args.only, file=sys.stderr)
            return 1

    result = Result()
    workdir = tempfile.mkdtemp(prefix="vnu-test-")
    try:
        if not args.install_only and not args.wizard_only:
            with Guest(iso=args.iso, gpu=args.gpu, log=args.log) as guest:
                run_suite(guest, cases, result, args.verbose, args.timeout)
                if not args.only or any(sel in "man-pager"
                                        for sel in args.only):
                    run_man_pager(guest, result, args.verbose, args.timeout)
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
