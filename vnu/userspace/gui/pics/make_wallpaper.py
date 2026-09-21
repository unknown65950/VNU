#!/usr/bin/env python3
"""Regenerate wallpaper.png (512x384) for the VNU desktop.

The desktop runs in 8-bpp VGA with the 16-entry Catppuccin Mocha DAC
palette (kernel/drivers/vga_gfx.cpp CATT_PAL), and the kernel loads the
wallpaper from /wallpaper with px.h and quantizes every pixel back to
those same 16 colors. So the artwork is designed *in* the palette: the
flat color regions survive the round-trip lossless, and only the sky's
dithering produces the intended two-tone bands.

Painting at half the desktop's 1024x768 on purpose: the kernel upplevels
the decoded BMP/PNG/JPEG with nearest-neighbour upscaling, and a 512x384
image is the largest that keeps a 24-bit BMP under the VFS 64 KiB file
cap while still covering the whole screen at exactly 2x.
"""

from PIL import Image

W, H = 512, 384

# The desktop's 16 Color_* indices -> their *8-bit* Catppuccin values.
# (The DAC table stores 6-bit channels; the LCD/framebuffer truth is
# those values shifted left by 2.) Names mirror vga_gfx.cpp CATT_PAL.
PAL = {
    "base":      (0x1E, 0x1E, 0x2E),  #  0 BLACK
    "blue":      (0x89, 0xB4, 0xFA),  #  1 BLUE
    "green":     (0xA6, 0xE3, 0xA1),  #  2 GREEN
    "sapphire":  (0x74, 0xC7, 0xEC),  #  3 CYAN
    "red":       (0xF3, 0x8B, 0xA8),  #  4 RED
    "mauve":     (0xCB, 0xA6, 0xF7),  #  5 MAGENTA
    "peach":     (0xFA, 0xB3, 0x87),  #  6 BROWN
    "surface0":  (0x31, 0x32, 0x44),  #  7 LGRAY
    "mantle":    (0x18, 0x18, 0x25),  #  8 DGRAY
    "lavender":  (0xB4, 0xBE, 0xFE),  #  9 LBLUE
    "teal":      (0x94, 0xE2, 0xD5),  # 10 LGREEN
    "sky":       (0x89, 0xDC, 0xEB),  # 11 LCYAN
    "maroon":    (0xEB, 0xA0, 0xAC),  # 12 LRED
    "pink":      (0xF5, 0xC2, 0xE7),  # 13 LMAGENTA
    "yellow":    (0xF9, 0xE2, 0xAF),  # 14 YELLOW
    "text":      (0xCD, 0xD6, 0xF4),  # 15 WHITE
}

# 4x4 Bayer matrix (kernel HILL/order), each entry scaled to 0..255.
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]

# Quarter-sine profile for the hill silhouettes (vdnu kernel HILL_T).
HILL_T = [0, 31, 62, 92, 120, 146, 170, 191, 209, 224, 236, 245, 251, 254,
          255, 254, 251, 245, 236, 224, 209, 191, 170, 146, 120, 92, 62,
          31, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]

img = Image.new("RGB", (W, H))


def px(x, y, color):
    img.putpixel((x, y), PAL[color])


def fill_circle(cx, cy, r, color):
    for dy in range(-r, r + 1):
        hw = 0
        while hw * hw + dy * dy <= r * r:
            hw += 1
        hw -= 1
        for x in range(cx - hw, cx + hw + 1):
            for y in range(cy + dy, cy + dy + 1):
                if 0 <= x < W and 0 <= y < H:
                    px(x, y, color)


# Sky: three dithered bands — lavender->sky, sky->sapphire, sapphire->surface0.
for y in range(H):
    t = y * 256 // H
    if t <= 96:
        top, bottom, lo, hi = "lavender", "sky", 0, 96
    elif t <= 168:
        top, bottom, lo, hi = "sky", "sapphire", 96, 168
    else:
        top, bottom, lo, hi = "sapphire", "surface0", 168, 255
    u = (t - lo) * 256 // (hi - lo + 1)
    for x in range(W):
        color = bottom if u >= BAYER[y % 4][x % 4] * 17 else top
        px(x, y, color)

# Sun: peach halo around a warm yellow core.
fill_circle(424, 75, 29, "peach")
fill_circle(424, 75, 20, "yellow")

# Clouds: puffy text-white blobs.
for cx, cy, r in ((90, 70, 13), (103, 75, 13), (74, 76, 11),
                  (280, 60, 10), (291, 64, 10), (270, 65, 8)):
    fill_circle(cx, cy, r, "text")

# Far hill range in mantle...
for x in range(W):
    idx = (x * 3 * 64 // W) % 64
    h = 75 * HILL_T[idx] // 256
    for y in range(350 - h, 350):
        px(x, y, "mantle")

# ...nearer, greener range...
for x in range(W):
    idx = (x * 2 * 64 // W + 16) % 64
    h = 55 * HILL_T[idx] // 256
    for y in range(374 - h, 374):
        px(x, y, "green")

# ...and the grassy ground strip.
for y in range(374, H):
    for x in range(W):
        px(x, y, "green")

img.save("wallpaper.png", optimize=True)
print("wrote wallpaper.png", img.size)