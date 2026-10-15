#include <vnu/arch/rtc.h>
#include <vnu/arch/io.h>

static std::uint8_t cmos_read(std::uint8_t reg) {
    vnu::arch::io_out8(0x70, reg);
    return vnu::arch::io_in8(0x71);
}
static std::uint8_t unbcd(std::uint8_t v) {
    return (v & 0x0F) + ((v >> 4) & 0x0F) * 10;
}
unsigned long vnu::arch::rtc_seconds_of_day() {
    std::uint8_t s = unbcd(cmos_read(0x00));
    std::uint8_t m = unbcd(cmos_read(0x02));
    std::uint8_t h = unbcd(cmos_read(0x04));
    return h * 3600UL + m * 60UL + s;
}
