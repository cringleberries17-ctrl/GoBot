#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace gobot {

/// Bounds-checked cursor over a byte buffer.
///
/// Every read reports failure through `ok()` rather than throwing: packet data
/// is attacker-adjacent (well, server-adjacent) and a truncated frame is a
/// routine event, not an exceptional one. A failed read leaves the cursor put
/// and poisons the reader so later reads also fail — callers can do a run of
/// reads and check `ok()` once at the end.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

    explicit ByteReader(const std::vector<std::uint8_t>& v)
        : data_(v.data()), size_(v.size()) {}

    bool        ok() const { return ok_; }
    std::size_t pos() const { return pos_; }
    std::size_t size() const { return size_; }
    std::size_t remaining() const { return ok_ && pos_ <= size_ ? size_ - pos_ : 0; }

    void seek(std::size_t p) {
        if (p > size_) { ok_ = false; return; }
        pos_ = p;
    }

    void skip(std::size_t n) {
        if (!have(n)) return;
        pos_ += n;
    }

    std::uint8_t  u8()  { return read_int<std::uint8_t>(true); }
    std::int8_t   i8()  { return static_cast<std::int8_t>(read_int<std::uint8_t>(true)); }
    std::uint16_t u16(bool le = true) { return read_int<std::uint16_t>(le); }
    std::int16_t  i16(bool le = true) { return static_cast<std::int16_t>(read_int<std::uint16_t>(le)); }
    std::uint32_t u32(bool le = true) { return read_int<std::uint32_t>(le); }
    std::int32_t  i32(bool le = true) { return static_cast<std::int32_t>(read_int<std::uint32_t>(le)); }
    std::uint64_t u64(bool le = true) { return read_int<std::uint64_t>(le); }
    std::int64_t  i64(bool le = true) { return static_cast<std::int64_t>(read_int<std::uint64_t>(le)); }

    float f32(bool le = true) {
        std::uint32_t raw = read_int<std::uint32_t>(le);
        float out;
        std::memcpy(&out, &raw, sizeof(out));
        return out;
    }

    double f64(bool le = true) {
        std::uint64_t raw = read_int<std::uint64_t>(le);
        double out;
        std::memcpy(&out, &raw, sizeof(out));
        return out;
    }

    /// Raw byte run.
    std::vector<std::uint8_t> bytes(std::size_t n) {
        if (!have(n)) return {};
        std::vector<std::uint8_t> out(data_ + pos_, data_ + pos_ + n);
        pos_ += n;
        return out;
    }

    /// Fixed-width string; stops at the first NUL.
    std::string fixed_string(std::size_t n) {
        if (!have(n)) return {};
        const char* p = reinterpret_cast<const char*>(data_ + pos_);
        std::size_t len = 0;
        while (len < n && p[len] != '\0') ++len;
        std::string out(p, len);
        pos_ += n;
        return out;
    }

    /// Length-prefixed string, prefix width `prefix_bytes` (1, 2 or 4).
    std::string prefixed_string(int prefix_bytes, bool le = true) {
        std::size_t len = 0;
        switch (prefix_bytes) {
            case 1: len = u8(); break;
            case 2: len = u16(le); break;
            case 4: len = u32(le); break;
            default: ok_ = false; return {};
        }
        if (!ok_ || !have(len)) return {};
        std::string out(reinterpret_cast<const char*>(data_ + pos_), len);
        pos_ += len;
        return out;
    }

    /// NUL-terminated string running to the end of the buffer if unterminated.
    std::string cstring() {
        if (!ok_ || pos_ >= size_) { ok_ = false; return {}; }
        const char* p = reinterpret_cast<const char*>(data_ + pos_);
        std::size_t max = size_ - pos_;
        std::size_t len = 0;
        while (len < max && p[len] != '\0') ++len;
        std::string out(p, len);
        pos_ += (len < max) ? len + 1 : len;   // consume the terminator if present
        return out;
    }

private:
    bool have(std::size_t n) {
        if (!ok_) return false;
        if (pos_ + n > size_ || pos_ + n < pos_) { ok_ = false; return false; }
        return true;
    }

    template <typename T>
    T read_int(bool le) {
        if (!have(sizeof(T))) return T{};
        T v{};
        if (le) {
            for (std::size_t i = 0; i < sizeof(T); ++i)
                v |= static_cast<T>(data_[pos_ + i]) << (8 * i);
        } else {
            for (std::size_t i = 0; i < sizeof(T); ++i)
                v = static_cast<T>((v << 8) | data_[pos_ + i]);
        }
        pos_ += sizeof(T);
        return v;
    }

    const std::uint8_t* data_ = nullptr;
    std::size_t         size_ = 0;
    std::size_t         pos_  = 0;
    bool                ok_   = true;
};

/// Read an unsigned integer of 1/2/4/8 bytes at `offset`. Returns false if the
/// span is too short. Used by the framing scanner, which probes speculatively.
bool read_uint_at(const std::uint8_t* data, std::size_t size, std::size_t offset,
                  int width, bool little_endian, std::uint64_t& out);

/// Hex + ASCII dump, `bytes_per_line` columns, indented by `indent` spaces.
std::string hexdump(const std::uint8_t* data, std::size_t size,
                    std::size_t bytes_per_line = 16, int indent = 0);

inline std::string hexdump(const std::vector<std::uint8_t>& v,
                           std::size_t bytes_per_line = 16, int indent = 0) {
    return hexdump(v.data(), v.size(), bytes_per_line, indent);
}

} // namespace gobot
