#pragma once

/* vnu/console.h - the native kernel console.
 *
 * A blocking keyboard/echo loop with a handful of builtins, with no
 * userspace at all. kernel_main() only reaches it when the image carries
 * no /sbin/init to hand control to, but it is what keeps an otherwise
 * unbootable image usable. It used to be declared ad-hoc as an
 * extern "C" symbol in kernel/kernel.cpp.
 */

namespace vnu::console {

/* Run the console loop. Never returns; the caller halts afterwards. */
void run();

} // namespace vnu::console
