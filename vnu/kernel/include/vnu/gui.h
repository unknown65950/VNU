#pragma once

// VibeGraphics — Windows 3.1-style window manager for VNU. Runs a
// 1024x768 VBE linear-framebuffer desktop with a taskbar along the top
// (analog clock from the RTC, active-window name, one button per open
// window, window control (minimize/close) and session (reboot/exit GUI)
// buttons), draggable cascading windows, and z-order focus management.
//
// Every launched app runs as its own *paged* wintask (see
// kernel/gui/wintask.cpp): a private page directory lets multiple apps
// share the fixed 0x400000 link address, and control cooperatively
// switches between the GUI and each task via vnu_swtch_pd.

namespace vnu::gui {

// Blocks until the user presses Esc with no windows open (or clicks
// Exit GUI). Safe to call repeatedly.
void run();

} // namespace vnu::gui