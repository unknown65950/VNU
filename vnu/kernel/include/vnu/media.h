#pragma once
// The system media lives under /etc/vnu, not at the VFS root: it is the
// content the machine boots with, not scratch space. The root keeps /bin,
// /dev, /proc, /tmp and the /apps launcher, so `ls /` stays a list of
// the things a user actually types.
//
//   /etc/vnu/pics             the demo photograph pack (picview)
//   /etc/vnu/sounds           the demo WAV clips (play)
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

// The wallpaper the desktop draws right now. Any BMP or PNG px.h
// understands, up to 512x384 and 64 KiB; see gui/wallpaper.cpp.
#define VNU_WALLPAPER "/etc/vnu/wallpaper"

// The image shipped with the kernel, never overwritten: the wallpaper
// picker offers it as "default".
#define VNU_WALLPAPER_SHIPPED "/etc/vnu/wallpaper.default"
