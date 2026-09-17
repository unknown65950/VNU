#pragma once
#include <stdint.h>
#include <stddef.h>

namespace vnu::tty {

void init();
void clear();
void putc(char c);
void write(const char* s, size_t n);
void write_cstr(const char* s);
void set_cursor(uint16_t row, uint16_t col);
void get_cursor(uint16_t* row, uint16_t* col);
/* Flush pending hardware cursor update (after a batch of putc). */
void flush_cursor();

} // namespace vnu::tty
