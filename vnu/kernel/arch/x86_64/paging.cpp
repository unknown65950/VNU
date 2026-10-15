#include <vnu/paging.h>
#include <vnu/arch/mmu.h>
#include <cstdint>

namespace {
struct PageTable {
    std::uint64_t entries[512];
};
}
void vnu::paging::init() {}
std::uintptr_t vnu::paging::kernel_pgdir_phys() {
    return vnu::arch::mmu::read_cr3() & ~0xFFFULL;
}
void vnu::paging::map_device_region(std::uintptr_t, std::uintptr_t, std::uint32_t) {}
std::uintptr_t vnu::paging::create_address_space() {
    return kernel_pgdir_phys();
}
bool vnu::paging::extend_address_space(std::uintptr_t, std::uintptr_t, std::uint32_t) { return true; }
bool vnu::paging::map_frames(std::uintptr_t, std::uintptr_t, std::uint32_t, std::uintptr_t, bool) { return true; }
bool vnu::paging::map_borrowed_frames(std::uintptr_t pgdir, std::uintptr_t va, std::uint32_t pages, std::uintptr_t phys) {
    return map_frames(pgdir, va, pages, phys, true);
}
bool vnu::paging::unmap_frames(std::uintptr_t, std::uintptr_t, std::uint32_t) { return true; }
void vnu::paging::destroy_address_space(std::uintptr_t) {}
void vnu::paging::switch_to(std::uintptr_t pgdir) {
    vnu::arch::mmu::write_cr3(pgdir);
}
std::uintptr_t vnu::paging::current_pgdir() {
    return kernel_pgdir_phys();
}
std::uintptr_t vnu::paging::phys_frame_at(std::uintptr_t, std::uintptr_t) { return 0; }
