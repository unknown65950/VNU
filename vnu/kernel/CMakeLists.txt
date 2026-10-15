cmake_minimum_required(VERSION 3.20)

project(vnu-kernel LANGUAGES CXX ASM_NASM)

# CMake's ASM_NASM support bakes CMAKE_ASM_NASM_OBJECT_FORMAT into
# CMAKE_ASM_NASM_COMPILE_OBJECT at enable_language() time (see
# Modules/CMakeASM_NASMInformation.cmake), which happens inside the
# project() call above. On a 64-bit host it defaults to elf64, and
# setting CMAKE_ASM_NASM_OBJECT_FORMAT afterwards has no effect — so we
# override the compile rule itself to force -f elf32.
if(VNU_KERNEL_ARCH STREQUAL "x86_64")
  set(CMAKE_ASM_NASM_COMPILE_OBJECT
      "<CMAKE_ASM_NASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -f elf64 -o <OBJECT> <SOURCE>"
  )
else()
  set(CMAKE_ASM_NASM_COMPILE_OBJECT
      "<CMAKE_ASM_NASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -f elf32 -o <OBJECT> <SOURCE>"
  )
endif()


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The architecture the kernel is built for: a directory under arch/ that
# implements the interfaces in include/vnu/arch/. Only i386 exists today;
# a new port adds arch/<name>/ and the toolchain flags immediately below.
# See arch/README.md.
set(VNU_KERNEL_ARCH "i386" CACHE STRING "Kernel target architecture")

# Note: this is NOT a real cross-compiler. We use the ordinary host
# g++ with -m32 and a freestanding environment, which is enough for an
# x86 kernel as long as nothing of the libc/host ABI is touched. For a
# "properly correct" build in the future it would be worth setting up a
# separate i686-elf-gcc toolchain (see wiki.osdev.org/GCC_Cross-Compiler).
#
# On ARM/aarch64 hosts (Termux, say) there is no multilib gcc for -m32;
# there we compile with clang against a bare i386 triple. The other flags
# (freestanding, no SSE/FPU, ...) are shared by both paths.

if(VNU_KERNEL_ARCH STREQUAL "x86_64")
  set(VNU_ARCH_FLAG -m64)
  set(VNU_KERNEL_LIBS -lgcc)
  set(VNU_USING_CLANG_CROSS OFF)
  if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv8)")
      set(VNU_USING_CLANG_CROSS ON)
      set(VNU_ARCH_FLAG "-target" "x86_64-none-elf")
      set(VNU_KERNEL_LIBS "")
      find_program(VNU_LD_LLD ld.lld REQUIRED)
      set(CMAKE_CXX_LINK_EXECUTABLE
          "${VNU_LD_LLD} -m elf_x86_64 <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
      set(CMAKE_CXX_LINK_DEPENDS_USE_LINKER FALSE)
  endif()
else()
  set(VNU_ARCH_FLAG -m32)
  set(VNU_KERNEL_LIBS -lgcc)
  set(VNU_USING_CLANG_CROSS OFF)
  if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv8)")
      set(VNU_USING_CLANG_CROSS ON)
      set(VNU_ARCH_FLAG "-target" "i386-none-elf")
      set(VNU_KERNEL_LIBS "")
      find_program(VNU_LD_LLD ld.lld REQUIRED)
      set(CMAKE_CXX_LINK_EXECUTABLE
          "${VNU_LD_LLD} -m elf_i386 <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
      set(CMAKE_CXX_LINK_DEPENDS_USE_LINKER FALSE)
  endif()
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
    arch/${VNU_KERNEL_ARCH}/boot/boot.s
    arch/${VNU_KERNEL_ARCH}/syscall/syscall.s
    arch/${VNU_KERNEL_ARCH}/switch.s
    arch/${VNU_KERNEL_ARCH}/ctxswitch.s
    install/blobs.s
    PROPERTIES LANGUAGE ASM_NASM
)

# One file in the kernel is allowed to use the FPU: gui/wallpaper.cpp
# carries px.h's JPEG decoder, whose IDCT is the only IEEE-float code in
# the tree, and -mno-80387 would turn every operation in it into a call to
# a soft-float helper (__addsf3 and friends) that a freestanding kernel
# has nowhere to satisfy. Turning the FPU on for this one translation
# unit is the smaller change: the file borrows the processor state
# between fxsave() and fxrstor() (see FpuBorrow there), so nothing the
# kernel does outside it, and nothing a program left in the registers,
# is disturbed. -mgeneral-regs-only stays on - the two are not exclusive,
# and SSE is still off, so this is x87 and nothing else.
set_source_files_properties(gui/wallpaper.cpp PROPERTIES
    COMPILE_OPTIONS "-m80387")

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
    arch/${VNU_KERNEL_ARCH}/cpu.cpp
    arch/${VNU_KERNEL_ARCH}/io.cpp
    arch/${VNU_KERNEL_ARCH}/serial.cpp
    arch/${VNU_KERNEL_ARCH}/rtc.cpp
    arch/${VNU_KERNEL_ARCH}/mmu.cpp
    arch/${VNU_KERNEL_ARCH}/context.cpp
    arch/${VNU_KERNEL_ARCH}/switch.cpp
    arch/${VNU_KERNEL_ARCH}/paging.cpp
    arch/${VNU_KERNEL_ARCH}/switch.s
    arch/${VNU_KERNEL_ARCH}/ctxswitch.s
    arch/${VNU_KERNEL_ARCH}/boot/boot.s
    arch/${VNU_KERNEL_ARCH}/boot/gdt.cpp
    arch/${VNU_KERNEL_ARCH}/syscall/syscall.s
    arch/${VNU_KERNEL_ARCH}/syscall/syscall.cpp
    arch/${VNU_KERNEL_ARCH}/syscall/idt.cpp
    # The PC hardware drivers: the same port owns the CPU and the machine
    # it sits in (8259/PIT are in the arch core above, these are the rest
    # of the PC chipset). A different port supplies its own.
    arch/${VNU_KERNEL_ARCH}/drivers/kbd.cpp
    arch/${VNU_KERNEL_ARCH}/drivers/vga_gfx.cpp
    arch/${VNU_KERNEL_ARCH}/drivers/ps2mouse.cpp
    arch/${VNU_KERNEL_ARCH}/drivers/ata.cpp
    arch/${VNU_KERNEL_ARCH}/drivers/pci.cpp
    arch/${VNU_KERNEL_ARCH}/drivers/e1000.cpp
    arch/${VNU_KERNEL_ARCH}/drivers/ac97.cpp
    arch/${VNU_KERNEL_ARCH}/drivers/virtio_gpu.cpp
    arch/${VNU_KERNEL_ARCH}/drivers/fb.cpp
    fs/vfs.cpp
    fs/pipe.cpp
    fs/fat16.cpp
    fs/blockdev.cpp
    fs/blockdev.cpp
    console/console.cpp
    console/tty.cpp
    console/font8x16.cpp
    net/tcp.cpp
    gui/gui.cpp
    gui/apps.cpp
    gui/gfxconf.cpp
    gui/wallpaper.cpp
    gui/wintask.cpp
    install/blobs.s
    install/install.cpp
    host/host.cpp
    runtime/string.cpp
    kernel/kernel.cpp
    kernel/clipboard.cpp
    mm/pmm.cpp
    proc/elf.cpp
    proc/process.cpp
    proc/thams.cpp
    proc/mboot.cpp
    proc/images.cpp
    proc/initrd.cpp
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
if(VNU_KERNEL_ARCH STREQUAL "x86_64")
  set(VNU_KERNEL_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64/linker.ld")
else()
  set(VNU_KERNEL_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker.ld")
endif()
if(VNU_USING_CLANG_CROSS)
    target_link_options(kernel.elf PRIVATE
        -static
        -T ${VNU_KERNEL_LINKER_SCRIPT}
    )
else()
    if(VNU_KERNEL_ARCH STREQUAL "x86_64")
      target_link_options(kernel.elf PRIVATE
          -m64
          -ffreestanding
          -nostdlib
          -static
          -Wl,-T,${VNU_KERNEL_LINKER_SCRIPT}
          ${VNU_KERNEL_LIBS}
      )
    else()
      target_link_options(kernel.elf PRIVATE
          -m32
          -ffreestanding
          -nostdlib
          -static
          -Wl,-T,${VNU_KERNEL_LINKER_SCRIPT}
          ${VNU_KERNEL_LIBS}
      )
    endif()
endif()

target_include_directories(kernel.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/arch/${VNU_KERNEL_ARCH}/include
    ${INSTALL_BLOB_DIR}
)

# One build stamp for the whole kernel, fixed at configure time.
# __DATE__/__TIME__ are per translation unit, so a plain macro would make
# `uname` report one time and /proc/version another; every TU of this
# build instead gets the same "yyyy-mm-dd HH:MM:SS" string.
string(TIMESTAMP VNU_BUILD_STAMP "%Y-%m-%d %H:%M:%S")
target_compile_definitions(kernel.elf PRIVATE
    VNU_BUILD_STAMP="${VNU_BUILD_STAMP}"
    VNU_ARCH="${VNU_KERNEL_ARCH}"
)
