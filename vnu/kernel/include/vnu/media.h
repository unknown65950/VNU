#pragma once
// The system media lives under /etc/vnu, not at the VFS root: it is the
// content the machine boots with, not scratch space. The root keeps /bin,
// /dev, /proc, /tmp and the /apps launcher, so `ls /` stays a list of
// the things a user actually types.
//
//   /etc/vnu/pics             the demo photograph pack (picview)
//   /etc/vnu/sounds           the demo WAV clips (play)
//   /etc/vnu/manpages         the system manual pages, one file per
//                             command (read by man, see man.c)
//   /etc/vnu/wallpaper        the desktop background in use
//   /etc/vnu/wallpaper.default  the shipped original, kept so the
//                               factory desktop can always come back
//
// The two wallpaper paths are read and written by gui/wallpaper.cpp and
// seeded from the embedded image by gui/apps.cpp.

// Root of the system's own content.
#define VNU_MEDIA_DIR "/etc/vnu"

// Demo photograph pack, one file per image.
#define VNU_PICS_DIR "/etc/vnu/pics"

// Demo WAV clips, one file per clip.
#define VNU_SOUNDS_DIR "/etc/vnu/sounds"

// Manual pages, one file per command; `man <name>` reads
// /etc/vnu/manpages/<name>. The files are embedded from
// vnu/userspace/man/pages/ by tools/manpages.py and mounted here by
// gui/apps.cpp.
#define VNU_MAN_DIR "/etc/vnu/manpages"

// The wallpaper the desktop draws right now. Any BMP, PNG or JPEG px.h
// understands, up to 1280x1024 pixels; see gui/wallpaper.cpp.
#define VNU_WALLPAPER "/etc/vnu/wallpaper"

// The image shipped with the kernel, never overwritten: the wallpaper
// picker offers it as "default".
#define VNU_WALLPAPER_SHIPPED "/etc/vnu/wallpaper.default"
