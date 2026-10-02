/*
 * thams.cpp — the module table tools/thams.py generates.
 *
 * The only translation unit that includes embedded_thams.h, and that
 * on purpose: the header holds the module binaries themselves, and a
 * blob included twice would be stored twice in the kernel image. Both
 * users of the table — proc/process.cpp, which runs a module, and
 * gui/apps.cpp, which installs it under /apps — go through the two
 * functions below instead of touching the blobs.
 */
#include <vnu/thams.h>

#include "embedded_thams.h"

namespace vnu::thams {

namespace {

/* The kernel may include only <vnu/...>, so no <string.h> here: the
 * only comparison this file needs is the one that matches a name. */
bool same(const char* a, const char* b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

} // namespace

const Module* list(int* count)
{
    if (count)
        *count = tham_module_count;
    return tham_modules;
}

const Module* find(const char* name)
{
    if (!name)
        return nullptr;
    for (int i = 0; i < tham_module_count; ++i)
        if (same(tham_modules[i].name, name))
            return &tham_modules[i];
    return nullptr;
}

} // namespace vnu::thams