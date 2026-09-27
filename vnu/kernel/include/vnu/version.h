#pragma once

/* Single source of truth for the VNU version numbers and the build
 * stamp. uname(), /proc/version and the `vnu version` command all read
 * these, so a release bump touches this file only.
 *
 * VNU_VERSION       user-visible OS version ("0.5")
 * VNU_KERNEL_VERSION kernel release ("0.5.0"); bumped independently
 *                    when the kernel changes but the userland does not
 * VNU_ARCH          machine architecture, as uname(2) reports it
 * VNU_BUILD_STAMP   "yyyy-mm-dd HH:MM:SS" of this build, so a running
 *                    system can report when it was built
 */
#define VNU_VERSION "0.5"
#define VNU_KERNEL_VERSION "0.5.0"
#define VNU_ARCH "i386"

#define VNU_STRINGIFY_(x) #x
#define VNU_STRINGIFY(x) VNU_STRINGIFY_(x)

/* "0.5" -> "vibe", the release codename suffix uname(2) reports. */
#define VNU_CODENAME "vibe"

/* CMakeLists.txt hands the same stamp to every translation unit of the
 * build: __DATE__/__TIME__ would make `uname` and /proc/version report
 * different times. The fallback covers ad-hoc host builds of one file. */
#ifndef VNU_BUILD_STAMP
#define VNU_BUILD_STAMP "unknown"
#endif
