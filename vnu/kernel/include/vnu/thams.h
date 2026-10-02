#pragma once
#include <stdint.h>
/*
 * thams.h — third-party app modules.
 *
 * A THAM is a program that lives in thams/ instead of inside the
 * system tree, and is compiled and embedded by the build rather than
 * by hand. The point is separation: the module keeps its own
 * directory, its own sources and its own manual page, and nothing
 * about the OS has to be edited to add it. tools/thams.py scans
 * thams/ and writes the table below into
 * vnu/kernel/proc/embedded_thams.h.
 *
 * Every module is its own ELF at /bin/<name>, so the shell runs it
 * like any other command. A module marked GUI=1 is also installed
 * under /apps/<name> with a tile on the desktop; a module that ships
 * a manual page gets it mounted at /apps/<name>/man/<name>, which is
 * where `man` looks once its built-in pages have missed.
 *
 * The table is generated, so this header describes the shape of an
 * entry rather than the modules themselves — an empty thams/ (or
 * `make thamoff`) leaves one sentinel entry and a count of zero.
 */

/* An embedded program: a path in the image and the bytes behind it.
 * The proc layer looks programs up through this shape, so it lives
 * here rather than in process.cpp: a module entry has to be one of
 * these too, and the kernel may include only <vnu/...>. */
struct EmbeddedProg {
    const char* path;
    const uint8_t* data;
    uint32_t size;
};

namespace vnu::thams {

/* One module. `prog` is first so a caller that only wants the
 * program — the shell looking for /bin/tree, say — can take &m.prog
 * without copying anything out; the rest is what the desktop and
 * `man` need and proc has no use for. */
struct Module {
    EmbeddedProg prog;
    const char* name;
    const uint8_t* man;   /* the manual page, or null */
    uint32_t man_size;
    bool gui;
    uint8_t color;        /* tile colour, a vnu::vgfx::COLOR_* slot */
    uint8_t glyph;        /* tile glyph, an IconGlyph value */
};

/* The modules compiled into this image, and how many there are: 0
 * when thams/ is empty or switched off. */
const Module* list(int* count);
const Module* find(const char* name);

} // namespace vnu::thams