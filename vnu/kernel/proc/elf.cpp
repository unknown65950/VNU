#include <vnu/elf.h>

namespace {

void* memcpy_local(void* dst, const void* src, size_t n)
{
    auto* d = static_cast<uint8_t*>(dst);
    auto* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; ++i)
        d[i] = s[i];
    return dst;
}

void* memset_local(void* dst, int v, size_t n)
{
    auto* d = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; ++i)
        d[i] = static_cast<uint8_t>(v);
    return dst;
}

} // namespace

namespace vnu::elf {

uint32_t load(const uint8_t* image, size_t size)
{
    if (!image || size < sizeof(Ehdr))
        return 0;

    auto* eh = reinterpret_cast<const Ehdr*>(image);
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return 0;
    if (eh->e_ident[4] != 1) /* ELFCLASS32 */
        return 0;
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_386)
        return 0;
    if (eh->e_phoff + static_cast<uint32_t>(eh->e_phnum) * eh->e_phentsize > size)
        return 0;

    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        auto* ph = reinterpret_cast<const Phdr*>(
            image + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;
        if (ph->p_offset + ph->p_filesz > size)
            return 0;
        /* Single address space: load at the ELF preferred vaddr. */
        auto* dest = reinterpret_cast<uint8_t*>(ph->p_vaddr);
        memcpy_local(dest, image + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset_local(dest + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
    }
    return eh->e_entry;
}

} // namespace vnu::elf
