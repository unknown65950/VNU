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

# Внимание: это НЕ настоящий кросс-компилятор. Мы используем обычный
# хостовый g++ с флагом -m32 и freestanding-окружением — этого
# достаточно для x86-ядра, пока мы не трогаем ничего из libc/host ABI.
# Для "по-настоящему правильной" сборки в будущем стоит завести
# отдельный toolchain i686-elf-gcc (см. wiki.osdev.org/GCC_Cross-Compiler).

set(KERNEL_COMPILE_FLAGS
    -m32
    -ffreestanding
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
# install/blobs.s via this include directory.
find_program(GRUB_MKIMAGE grub-mkimage)
if(NOT GRUB_MKIMAGE)
    message(FATAL_ERROR "grub-mkimage not found - needed for the disk installer payload")
endif()
set(INSTALL_BLOB_DIR ${CMAKE_CURRENT_BINARY_DIR}/install_blobs)
add_custom_command(
    OUTPUT ${INSTALL_BLOB_DIR}/vnu_install_blobs.inc
    COMMAND ${CMAKE_COMMAND} -E make_directory ${INSTALL_BLOB_DIR}
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_install_blobs.sh ${INSTALL_BLOB_DIR}
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
    gui/gui.cpp
    gui/apps.cpp
    gui/wintask.cpp
    install/blobs.s
    install/install.cpp
    runtime/string.cpp
    kernel/kernel.cpp
    mm/pmm.cpp
    mm/paging.cpp
    proc/elf.cpp
    proc/process.cpp
    proc/mboot.cpp
    proc/switch.s
    proc/ctxswitch.s
)
add_dependencies(kernel.elf install_blobs)

target_compile_options(kernel.elf PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:${KERNEL_COMPILE_FLAGS}>
)

set_target_properties(kernel.elf PROPERTIES LINKER_LANGUAGE CXX)

target_link_options(kernel.elf PRIVATE
    -m32
    -ffreestanding
    -nostdlib
    -static
    -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
    -lgcc
)

target_include_directories(kernel.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${INSTALL_BLOB_DIR}
)
