#include <vnu/arch/rtc.h>

// arch/i386/rtc.cpp - the CMOS MC146818 real-time clock, through ports
// 0x70 (address) and 0x71 (data). BCD-encoded unless the status
// register says otherwise, which is the PC default and what QEMU does.

namespace vnu::arch {

namespace {

uint8_t cmos_read(uint8_t reg)
{
    asm volatile("outb %0, $0x70" : : "a"(reg));
    uint8_t v;
    asm volatile("inb $0x71, %0" : "=a"(v));
    return v;
}

uint8_t unbcd(uint8_t v)
{
    return static_cast<uint8_t>((v & 0x0F) + ((v >> 4) * 10));
}

} // namespace

uint32_t rtc_seconds_of_day()
{
    while (cmos_read(0x0A) & 0x80) {
    } /* wait out an update-in-progress */
    uint8_t sec = cmos_read(0x00);
    uint8_t min = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t status_b = cmos_read(0x0B);
    if (!(status_b & 0x04)) { /* BCD mode */
        sec = unbcd(sec);
        min = unbcd(min);
        hour = unbcd(static_cast<uint8_t>(hour & 0x7F));
    }
    return static_cast<uint32_t>(hour) * 3600u +
           static_cast<uint32_t>(min) * 60u + static_cast<uint32_t>(sec);
}

} // namespace vnu::arch
