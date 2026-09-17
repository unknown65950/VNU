cmake_minimum_required(VERSION 3.10)
project(vlibc VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Select syscall ABI explicitly. Host builds default to Linux; VNU builds
# must use a 32-bit i386 compiler/toolchain (gcc -m32 or i686-elf-gcc).
set(VLIBC_TARGET "linux" CACHE STRING "vlibc syscall target: linux or vnu")
set_property(CACHE VLIBC_TARGET PROPERTY STRINGS linux vnu)
string(TOLOWER "${VLIBC_TARGET}" VLIBC_TARGET)

if(VLIBC_TARGET STREQUAL "linux")
    add_compile_definitions(VLIBC_TARGET_LINUX=1)
elseif(VLIBC_TARGET STREQUAL "vnu")
    add_compile_definitions(VLIBC_TARGET_VNU=1)
    # Prefer explicit -m32 when the host is 64-bit multiarch.
    add_compile_options(-m32)
    add_link_options(-m32)
    message(STATUS "vlibc: VNU target (i386 freestanding)")
else()
    message(FATAL_ERROR "Unknown VLIBC_TARGET='${VLIBC_TARGET}'. Use linux or vnu")
endif()

# Freestanding: no host libc builtins/headers leakage.
add_compile_options(-ffreestanding -fno-builtin -fno-stack-protector)

set(VLIBC_SOURCES
    src/sys/syscall.c
    src/unistd/write.c
    src/unistd/read.c
    src/unistd/open.c
    src/unistd/close.c
    src/unistd/lseek.c
    src/unistd/exit.c
    src/unistd/getpid.c
    src/unistd/getuid.c
    src/unistd/getgid.c
    src/unistd/chdir.c
    src/unistd/getcwd.c
    src/unistd/fork.c
    src/unistd/execve.c
    src/stdio/putchar.c
    src/stdio/puts.c
    src/stdio/printf.c
    src/string/strlen.c
    src/string/strcpy.c
    src/string/strncpy.c
    src/string/strcmp.c
    src/string/strncmp.c
    src/string/memcpy.c
    src/string/memset.c
    src/stdlib/malloc.c
    src/stdlib/atoi.c
    src/stdlib/atol.c
)

# Static library WITHOUT crt0 — crt0 is linked explicitly by vld/vcc.
add_library(vlibc_static STATIC ${VLIBC_SOURCES})
target_include_directories(vlibc_static PUBLIC include)
set_target_properties(vlibc_static PROPERTIES OUTPUT_NAME vlibc)

add_library(vlibc_shared SHARED ${VLIBC_SOURCES})
target_include_directories(vlibc_shared PUBLIC include)
set_target_properties(vlibc_shared PROPERTIES OUTPUT_NAME vlibc)

if(VLIBC_TARGET STREQUAL "linux")
    # Host test binary uses Linux crt0
    add_executable(test_vlibc tests/test.c src/crt/crt0.c)
    target_link_libraries(test_vlibc vlibc_static)
    target_link_options(test_vlibc PRIVATE -static -nostdlib -ffreestanding)
endif()
