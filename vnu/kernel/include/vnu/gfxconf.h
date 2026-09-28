#pragma once

// The display mode this machine runs the desktop at, kept in
// /etc/vnuconfig/gfx.conf.
//
// Shaped exactly like /etc/hostname: a plain text file is the single
// source of truth, the desktop reads it before it programs the card and
// rewrites it on every change, and anything else in the system (a
// settings app, a script) can read or edit the same file. One line:
//
//     mode 1024x768
//
// The name of the directory is the roadmap's, shared with the vsshd
// daemon config that is planned to live beside it.
//
// One honest limit: this VFS is RAM-backed, so the file describes the
// running system and not the disk. A machine that boots the installed
// image starts from the built-in default again. Stamping the mode into
// the installed disk's config record (as host::load() does for the node
// name) is what would close that gap; until then this file is the
// session's setting, and a program that wants a mode remembered across
// reboots should say so rather than assume it.

namespace vnu::gfxconf {

// The file itself, inside the settings directory vfs::init() creates.
constexpr const char* PATH = "/etc/vnuconfig/gfx.conf";

// Program the mode recorded in the file, when it names one this build
// supports. Call this before vgfx::enter_gfx_mode(): the card is still
// in text mode then, so vgfx::set_resolution() only records the choice
// and the desktop programs it on the way in. Returns true when a mode
// was read and accepted; a missing, empty or hand-mangled file leaves
// the built-in default in place and returns false.
bool load();

// Record w x h as the mode in use, replacing the file's content.
// Returns false when the file could not be written. The screen has
// already changed by then, so a false here is a lost setting, not a
// failed mode switch, and callers should not undo the switch over it.
bool store(int w, int h);

} // namespace vnu::gfxconf
