#pragma once
#include <stdint.h>

namespace vnu::kbd {

/* Function-key pseudo-codes, in the same >= 0x80 space as the arrows
 * and Home/End/Delete (which vnu/kernel/drivers/kbd.cpp keeps to
 * itself). Same values as VNU_KEY_F* in vlibc/keys.h, so a key a
 * desktop shortcut consumed here cannot disagree with what a program
 * reads from getch(). */
constexpr int K_F1 = 0x88;
constexpr int K_F2 = 0x89;
constexpr int K_F3 = 0x8A;
constexpr int K_F4 = 0x8B;
constexpr int K_F5 = 0x8C;
constexpr int K_F6 = 0x8D;
constexpr int K_F7 = 0x8E;
constexpr int K_F8 = 0x8F;
constexpr int K_F9 = 0x90;
constexpr int K_F10 = 0x91;
constexpr int K_F11 = 0x92;
constexpr int K_F12 = 0x93;

/* The next character typed, waiting for one. The scancodes behind it
 * go through one queue with one decoder state (see kbd.cpp): port 0x60
 * is destructive, and two readers of it lose bytes - and read stale
 * ones - so every reader in the machine takes from that queue. */
char getch_blocking();

/* Discard any bytes the controller and the queue are holding, and the
 * decoder state that goes with them. Used by the GUI before
 * re-initialising the aux device, so a stale keyboard byte left over
 * from an earlier read isn't misread as a mouse command
 * acknowledgement. */
void drain_excess();

/* Non-blocking, shift/ctrl-aware character decode (same mapping as
 * getch_blocking, including the K_* pseudo-codes for arrow/home/end/
 * delete above 0x7F) for exactly one pending scancode. Returns -1 if
 * there's nothing ready, or the byte read didn't complete a character
 * (e.g. it was a modifier key or a key release). */
int poll_char();
}
