#pragma once
#include <stdint.h>

namespace vnu::kbd {
char getch_blocking();

/* Non-blocking raw access, used by the GUI event loop so it can poll
 * keyboard + mouse in the same frame instead of blocking on a key. */
bool scancode_ready();
uint8_t read_raw_scancode();

/* Non-blocking, shift/ctrl-aware character decode (same mapping as
 * getch_blocking, including the K_* pseudo-codes for arrow/home/end/
 * delete above 0x7F) for exactly one pending scancode. Returns -1 if
 * there's nothing ready, or the byte read didn't complete a character
 * (e.g. it was a modifier key or a key release). */
int poll_char();
}
