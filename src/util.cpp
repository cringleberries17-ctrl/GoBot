#include "gobot/common.hpp"
#include "gobot/byte_reader.hpp"

#include <chrono>
#include <cstdio>
#include <string>

namespace gobot {

std::uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool read_uint_at(const std::uint8_t* data, std::size_t size, std::size_t offset,
                  int width, bool little_endian, std::uint64_t& out) {
    if (width != 1 && width != 2 && width != 4 && width != 8) return false;
    const std::size_t w = static_cast<std::size_t>(width);
    if (offset + w > size || offset + w < offset) return false;

    std::uint64_t v = 0;
    if (little_endian) {
        for (std::size_t i = 0; i < w; ++i)
            v |= static_cast<std::uint64_t>(data[offset + i]) << (8 * i);
    } else {
        for (std::size_t i = 0; i < w; ++i)
            v = (v << 8) | data[offset + i];
    }
    out = v;
    return true;
}

std::string hexdump(const std::uint8_t* data, std::size_t size,
                    std::size_t bytes_per_line, int indent) {
    if (bytes_per_line == 0) bytes_per_line = 16;
    const std::string pad(indent < 0 ? 0 : static_cast<std::size_t>(indent), ' ');

    std::string out;
    out.reserve(size * 4 + (size / bytes_per_line + 1) * (indent + 12));

    // Wide enough for a 64-bit offset; "%04zx" only pads, it never truncates
    // a large value, and an 8-byte buffer would clip past 0x10000.
    char line[24];
    for (std::size_t off = 0; off < size; off += bytes_per_line) {
        out += pad;
        std::snprintf(line, sizeof(line), "%04zx  ", off);
        out += line;

        const std::size_t n = (size - off < bytes_per_line) ? size - off : bytes_per_line;

        for (std::size_t i = 0; i < bytes_per_line; ++i) {
            if (i < n) {
                std::snprintf(line, sizeof(line), "%02x ", data[off + i]);
                out += line;
            } else {
                out += "   ";
            }
            if (i % 8 == 7) out += ' ';
        }

        out += ' ';
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t c = data[off + i];
            out += (c >= 0x20 && c <= 0x7e) ? static_cast<char>(c) : '.';
        }
        out += '\n';
    }
    return out;
}

} // namespace gobot
