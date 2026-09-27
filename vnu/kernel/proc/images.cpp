#include <vnu/images.h>
#include <vnu/mboot.h>

namespace {

struct Registry {
    uint32_t bytes[vnu::images::Count];
    uint32_t files[vnu::images::Count];
};

Registry g_reg;

} // namespace

namespace vnu::images {

void add(Category cat, uint32_t bytes)
{
    if (cat >= Count)
        return;
    g_reg.bytes[cat] += bytes;
    ++g_reg.files[cat];
}

Sizes collect()
{
    /* The kernel image size is the Multiboot2 module the boot loader
     * handed us: install copies exactly that range onto a disk, so it
     * is the kernel as loaded, not an estimate. Zero when booted
     * without `module2` (e.g. a hand-made -kernel invocation). */
    uint32_t ksize = 0;
    vnu::mboot::first_module(ksize);

    Sizes s{};
    for (int i = 0; i < Count; ++i) {
        s.bytes[i] = g_reg.bytes[i];
        s.files[i] = g_reg.files[i];
    }
    /* Not registered like the rest: it is read from the boot info, not
     * a blob the kernel owns, and must not accumulate across reads. */
    s.bytes[Kernel] = ksize;
    s.files[Kernel] = ksize ? 1 : 0;
    return s;
}

uint32_t Sizes::total_bytes() const
{
    uint32_t sum = 0;
    for (int i = 0; i < Count; ++i)
        sum += bytes[i];
    return sum;
}

uint32_t Sizes::total_files() const
{
    uint32_t sum = 0;
    for (int i = 0; i < Count; ++i)
        sum += files[i];
    return sum;
}

} // namespace vnu::images
