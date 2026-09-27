#pragma once

#include <stddef.h>

// The node name of the machine, the way a Unix system has it.
//
// /etc/hostname is the single source of truth, exactly as on a normal
// Unix: the file holds one line with the name, and everything that needs
// the name (uname(2), the vash prompt, the installer default) reads it
// from there. This VFS is RAM-backed, so the file is created by
// vfs::init() with the built-in default and the installer rewrites it.
//
// The one thing the RAM VFS cannot do is survive a reboot on its own,
// so a hostname given at install time is also stamped into the config
// record of the installed disk; host::load() copies it back into
// /etc/hostname on the next boot.
namespace vnu::host {

// Longest accepted name, plus the NUL. 63 characters is the classic
// Linux DNS label limit and what uname's nodename field holds.
constexpr int MAX = 64;

// The name used when /etc/hostname is missing or unusable.
constexpr const char* DEFAULT_NAME = "vnu";

// The current node name: /etc/hostname trimmed to its first line.
// Falls back to DEFAULT_NAME, so callers never have to check for null.
const char* name();

// True when `s` is a name this system accepts: 1..63 characters of
// letters, digits, '-', '_' and '.', starting and ending with a letter
// or digit. The same rule is applied by the installer before it asks
// the kernel to write the name to a disk.
bool valid(const char* s);

// Write `s` into /etc/hostname (replacing the file's content), after
// checking it with valid(). False when the name is rejected or the
// file is gone.
bool set(const char* s);

// Adopt the hostname stored on an installed disk, if there is one.
// Called once at boot, after ata::init() has found the disks; a live CD
// session keeps the default. Returns true when a name was adopted.
bool load();

} // namespace vnu::host
