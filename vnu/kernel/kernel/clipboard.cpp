// clipboard.cpp — the system clipboard, the storage behind /dev/clipboard.
//
// One buffer, global to the kernel and therefore shared by every process:
// a windowed app, a console command and the shell all see the same text.
// The VFS character-device handlers in fs/vfs.cpp call in here; nothing
// about the clipboard depends on the desktop running, so `clipboard` and
// `cat /dev/clipboard` work from a plain console too.
//
// There is no locking, on purpose: like the rest of the kernel it is
// single-threaded and cooperatively scheduled (see wintask.h), so a
// syscall runs to completion before any other task gets a slice. The
// buffer is touched only from those syscall paths.

#include <vnu/clipboard.h>

namespace vnu::clip {

namespace {
char g_buf[CAP];
uint32_t g_len = 0;
} // namespace

void clear()
{
    g_len = 0;
}

uint32_t size()
{
    return g_len;
}

uint32_t read_at(uint32_t pos, void* buf, uint32_t count)
{
    if (!buf || pos >= g_len)
        return 0;
    uint32_t avail = g_len - pos;
    uint32_t take = count < avail ? count : avail;
    auto* out = static_cast<char*>(buf);
    for (uint32_t i = 0; i < take; ++i)
        out[i] = g_buf[pos + i];
    return take;
}

uint32_t append(const void* data, uint32_t count)
{
    if (!data || count == 0 || g_len >= CAP)
        return 0;
    uint32_t room = CAP - g_len;
    if (count > room)
        count = room;
    const auto* in = static_cast<const char*>(data);
    for (uint32_t i = 0; i < count; ++i)
        g_buf[g_len + i] = in[i];
    g_len += count;
    return count;
}

} // namespace vnu::clip
