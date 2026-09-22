#include <cstddef>
#include <cstdint>

// Minimal freestanding C runtime primitives.
// GCC may emit calls to these even for aggregate zero-initialization such as x = {}.
extern "C" void* memset(void* dst, int value, std::size_t count)
{
    auto* out = static_cast<std::uint8_t*>(dst);
    const auto byte = static_cast<std::uint8_t>(value);
    for (std::size_t i = 0; i < count; ++i)
        out[i] = byte;
    return dst;
}

extern "C" void* memcpy(void* dst, const void* src, std::size_t count)
{
    auto* out = static_cast<std::uint8_t*>(dst);
    const auto* in = static_cast<const std::uint8_t*>(src);
    for (std::size_t i = 0; i < count; ++i)
        out[i] = in[i];
    return dst;
}

extern "C" int memcmp(const void* a, const void* b, std::size_t count)
{
    const auto* lhs = static_cast<const std::uint8_t*>(a);
    const auto* rhs = static_cast<const std::uint8_t*>(b);
    for (std::size_t i = 0; i < count; ++i) {
        if (lhs[i] != rhs[i])
            return lhs[i] < rhs[i] ? -1 : 1;
    }
    return 0;
}

extern "C" void* memmove(void* dst, const void* src, std::size_t count)
{
    auto* out = static_cast<std::uint8_t*>(dst);
    const auto* in = static_cast<const std::uint8_t*>(src);
    if (out == in || count == 0)
        return dst;
    if (out < in) {
        for (std::size_t i = 0; i < count; ++i)
            out[i] = in[i];
    } else {
        for (std::size_t i = count; i != 0; --i)
            out[i - 1] = in[i - 1];
    }
    return dst;
}
