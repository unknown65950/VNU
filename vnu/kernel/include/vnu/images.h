#pragma once

#include <cstdint>

/* Size accounting of everything the running system consists of, as
 * reported by /proc/images (and printed by `vnu size`).
 *
 * Each owner of embedded blobs registers its contribution once, at
 * init: the kernel image size (from the Multiboot2 module tag), the
 * userspace ELFs (proc/process.cpp), the toolchain libraries
 * (fs/vfs.cpp), the VGA font (drivers/vga_gfx.cpp) and the media blobs
 * (gui/apps.cpp). Nothing here is a build-time constant, so the
 * numbers describe the image that is actually running.
 */
namespace vnu::images {

enum Category : uint8_t {
    Kernel = 0,     /* the kernel image loaded by the boot loader */
    Userspace,      /* every /bin command, ELF by ELF            */
    Libraries,      /* crt0 and the C runtime the commands link  */
    Fonts,          /* console/glyph font data                   */
    Resources,      /* sounds and pictures shipped with the OS   */
    Count,
};

struct Sizes {
    uint32_t bytes[Count];
    uint32_t files[Count];

    uint32_t total_bytes() const;
    uint32_t total_files() const;
};

/* Add `bytes` to a category. Safe to call from any init path; called
 * once per blob, so `files` counts objects, not unique pointers. */
void add(Category cat, uint32_t bytes);

/* Current totals. */
Sizes collect();

} // namespace vnu::images
