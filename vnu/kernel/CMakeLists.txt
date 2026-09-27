cmake_minimum_required(VERSION 3.20)

project(vnu-kernel LANGUAGES CXX ASM_NASM)

# CMake's ASM_NASM support bakes CMAKE_ASM_NASM_OBJECT_FORMAT into
# CMAKE_ASM_NASM_COMPILE_OBJECT at enable_language() time (see
# Modules/CMakeASM_NASMInformation.cmake), which happens inside the
# project() call above. On a 64-bit host it defaults to elf64, and
# setting CMAKE_ASM_NASM_OBJECT_FORMAT afterwards has no effect — so we
# override the compile rule itself to force -f elf32.
set(CMAKE_ASM_NASM_COMPILE_OBJECT
    "<CMAKE_ASM_NASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -f elf32 -o <OBJECT> <SOURCE>"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Note: this is NOT a real cross-compiler. We use the ordinary host
# g++ with -m32 and a freestanding environment, which is enough for an
# x86 kernel as long as nothing of the libc/host ABI is touched. For a
# "properly correct" build in the future it would be worth setting up a
# separate i686-elf-gcc toolchain (see wiki.osdev.org/GCC_Cross-Compiler).
#
# On ARM/aarch64 hosts (Termux, say) there is no multilib gcc for -m32;
# there we compile with clang against a bare i386 triple. The other flags
# (freestanding, no SSE/FPU, ...) are shared by both paths.

set(VNU_ARCH_FLAG -m32)
set(VNU_KERNEL_LIBS -lgcc)
set(VNU_USING_CLANG_CROSS OFF)
if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv8)")
    set(VNU_USING_CLANG_CROSS ON)
    set(VNU_ARCH_FLAG "-target" "i386-none-elf")
    # A host libgcc has nothing for a bare i386 - if the kernel ever
    # needs compiler-rt-like helpers (udivdi3 and friends) they are added
    # as a separate TU under runtime/ (runtime/libgcc_shim.cpp).
    set(VNU_KERNEL_LIBS "")
    # The Termux clang has no linker driver for a bare i386 (linking
    # collapses to /usr/bin/false), so in cross mode we drive ld.lld
    # directly. <LINK_FLAGS> then only carries the target_link_options
    # below - the driver's global -O2 -g are not needed. CMake's
    # auto-generated --dependency-file arrives as -Xlinker, which ld.lld
    # does not understand, so that feature is disabled as well.
    find_program(VNU_LD_LLD ld.lld REQUIRED)
    set(CMAKE_CXX_LINK_EXECUTABLE
        "${VNU_LD_LLD} -m elf_i386 <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
    set(CMAKE_CXX_LINK_DEPENDS_USE_LINKER FALSE)
endif()

set(KERNEL_COMPILE_FLAGS
    ${VNU_ARCH_FLAG}
    -ffreestanding
    -nostdinc++
    -fno-exceptions
    -fno-rtti
    -fno-stack-protector
    -fno-pic
    -mno-sse
    -mno-sse2
    -mno-mmx
    -mno-80387
    -mgeneral-regs-only
    -Wall
    -Wextra
)

set_source_files_properties(
    arch/i386/boot/boot.s
    arch/i386/syscall/syscall.s
    proc/switch.s
    proc/ctxswitch.s
    install/blobs.s
    PROPERTIES LANGUAGE ASM_NASM
)

# GRUB stage blobs for the disk installer. core.img contains only GRUB
# modules, so generating it before the kernel links introduces no
# circular dependency; the .inc with the incbin's is pulled in by
# install/blobs.s via this include directory. On hosts without grub-mkimage
# (ARM/Termux) a stub .inc with empty arrays is generated instead — the
# kernel boots fine, only the `install` payload is empty.
find_program(GRUB_MKIMAGE grub-mkimage)
set(INSTALL_BLOB_DIR ${CMAKE_CURRENT_BINARY_DIR}/install_blobs)
if(GRUB_MKIMAGE)
    set(INSTALL_BLOBS_ARGS bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_install_blobs.sh ${INSTALL_BLOB_DIR})
else()
    message(WARNING "grub-mkimage not found - disk installer payload will be empty stubs")
    set(INSTALL_BLOBS_ARGS bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_install_blobs.sh --stub ${INSTALL_BLOB_DIR})
endif()
add_custom_command(
    OUTPUT ${INSTALL_BLOB_DIR}/vnu_install_blobs.inc
    COMMAND ${CMAKE_COMMAND} -E make_directory ${INSTALL_BLOB_DIR}
    COMMAND ${INSTALL_BLOBS_ARGS}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_install_blobs.sh
    COMMENT "Generating GRUB blobs for the disk installer"
    VERBATIM
)
add_custom_target(install_blobs
    DEPENDS ${INSTALL_BLOB_DIR}/vnu_install_blobs.inc)

add_executable(kernel.elf
    arch/i386/boot/boot.s
    arch/i386/boot/gdt.cpp
    arch/i386/syscall/syscall.s
    arch/i386/syscall/syscall.cpp
    arch/i386/syscall/idt.cpp
    fs/vfs.cpp
    fs/pipe.cpp
    fs/fat16.cpp
    console/console.cpp
    console/tty.cpp
    drivers/kbd.cpp
    drivers/vga_gfx.cpp
    drivers/ps2mouse.cpp
    drivers/ata.cpp
    drivers/pci.cpp
    drivers/e1000.cpp
    drivers/ac97.cpp
    drivers/virtio_gpu.cpp
    drivers/tcp.cpp
    gui/gui.cpp
    gui/apps.cpp
    gui/wallpaper.cpp
    gui/wintask.cpp
    install/blobs.s
    install/install.cpp
    host/host.cpp
    runtime/string.cpp
    kernel/kernel.cpp
    mm/pmm.cpp
    mm/paging.cpp
    proc/elf.cpp
    proc/process.cpp
    proc/mboot.cpp
    proc/images.cpp
    proc/switch.s
    proc/ctxswitch.s
    sync/locking.cpp
)
add_dependencies(kernel.elf install_blobs)

target_compile_options(kernel.elf PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${KERNEL_COMPILE_FLAGS}>
)

set_target_properties(kernel.elf PROPERTIES LINKER_LANGUAGE CXX)

# In cross mode (ld.lld called directly) these are plain linker flags;
# with the native gcc driver they become -Wl,-T,... pass-through options,
# and the host libgcc is pulled in there as well.
if(VNU_USING_CLANG_CROSS)
    target_link_options(kernel.elf PRIVATE
        -static
        -T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
    )
else()
    target_link_options(kernel.elf PRIVATE
        -m32
        -ffreestanding
        -nostdlib
        -static
        -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
        ${VNU_KERNEL_LIBS}
    )
endif()

target_include_directories(kernel.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${INSTALL_BLOB_DIR}
)

# One build stamp for the whole kernel, fixed at configure time.
# __DATE__/__TIME__ are per translation unit, so a plain macro would make
# `uname` report one time and /proc/version another; every TU of this
# build instead gets the same "yyyy-mm-dd HH:MM:SS" string.
string(TIMESTAMP VNU_BUILD_STAMP "%Y-%m-%d %H:%M:%S")
target_compile_definitions(kernel.elf PRIVATE
    VNU_BUILD_STAMP="${VNU_BUILD_STAMP}"
)
