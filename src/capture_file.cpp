#include "gobot/capture_file.hpp"
#include "gobot/common.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>

namespace gobot {
namespace capture_file {
namespace {

bool ends_with(const std::string& s, const char* suffix) {
    const std::string t = suffix;
    return s.size() >= t.size() &&
           std::equal(t.rbegin(), t.rend(), s.rbegin(),
                      [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                      });
}

/// The bytes of a `size`-byte buffer that no frame `s` read accounts for.
std::size_t unread_of(const FrameScanResult& s, std::size_t size) {
    return size - std::min(s.consumed, size) + s.resyncs;
}

} // namespace

bool half_from_name(const std::string& path, Half& out) {
    if (ends_with(path, ".c2s")) { out = Half::Client; return true; }
    if (ends_with(path, ".s2c")) { out = Half::Server; return true; }
    return false;
}

bool udp_from_name(const std::string& path) {
    return ends_with(path, ".udp.s2c") || ends_with(path, ".udp.c2s");
}

Framings load_framings(const std::string& config, bool& have_server) {
    Framings f;
    f.server = load_framing(config, have_server);
    bool have_client = false, have_udp = false;
    const FramingSpec client = load_framing(config, have_client, "FramingClient");
    if (have_client) f.client = client;
    const FramingSpec udp = load_framing(config, have_udp, "FramingUdp");
    if (have_udp) f.udp = udp;
    return f;
}

Fit judge(const Reading& r) {
    if (r.size == 0)   return Fit::Empty;
    if (r.frames == 0) return Fit::NothingRead;
    const std::size_t unread = r.unread();
    if (unread * 20 > r.size) return Fit::TooMuchUnread;          // more than 5%
    if ((r.size - unread) / r.frames > 512) return Fit::Oversized;
    return Fit::Described;
}

std::string reading_warning(const Reading& r) {
    char buf[512];
    switch (judge(r)) {
    case Fit::Described:
        return {};
    case Fit::Empty:
    case Fit::NothingRead:
        return "  !! not one frame was read from it. Either this framing does not fit\n"
               "     the file, or it holds no frames at all.\n";
    case Fit::TooMuchUnread:
        std::snprintf(buf, sizeof(buf),
                      "  !! %zu of %zu bytes (%.1f%%) are in no frame. Either this framing does\n"
                      "     not fit the file, the capture lost packets, or the file holds\n"
                      "     another connection's bytes too. Treat anything below with\n"
                      "     suspicion; --verify shows where those bytes are.\n",
                      r.unread(), r.size,
                      100.0 * static_cast<double>(r.unread()) / static_cast<double>(r.size));
        return buf;
    case Fit::Oversized:
        std::snprintf(buf, sizeof(buf),
                      "  !! its frames average %zu bytes: a length field that is not one, read\n"
                      "     as a few huge frames that swallow the real ones. Treat anything\n"
                      "     below with suspicion.\n",
                      (r.size - r.unread()) / r.frames);
        return buf;
    }
    return {};
}

FrameScanResult tile(const Bytes& recording, const FramingSpec& spec, Direction dir) {
    // Unlocked: a recording may begin part-way through a frame, and the
    // scanner must slide past that rather than wait for it.
    return extract_frames(spec, recording.data(), recording.size(), dir, 0, false);
}

Tiling tile_named(const Bytes& data, const std::string& name, const Framings& f,
                  std::optional<Spec> force) {
    Tiling t;
    Reading& r = t.reading;
    r.size = data.size();
    if (data.empty()) { r.error = name + " is empty"; return t; }
    if (force && *force == Spec::Client && !f.client) {
        r.error = "the config has no [FramingClient] to read " + name + " with";
        return t;
    }

    Half named = Half::Server;
    const bool by_name = half_from_name(name, named);
    if (by_name) {
        r.half = named;
        r.decided_by = HalfSource::Name;
    }
    const auto scan = [&data](const FramingSpec& spec, Half half) {
        return tile(data, spec, half == Half::Client ? Direction::ToServer : Direction::ToClient);
    };

    if (force) {
        r.spec = *force;
        t.spec = (*force == Spec::Client) ? *f.client
               : (*force == Spec::Udp)    ? f.for_udp()
                                          : f.server;
        if (!by_name) {
            r.half = (*force == Spec::Client) ? Half::Client : Half::Server;
            r.decided_by = HalfSource::Forced;
        }
        t.scan = scan(t.spec, r.half);
    } else if (udp_from_name(name)) {
        r.spec = Spec::Udp;
        t.spec = f.for_udp();
        t.scan = scan(t.spec, r.half);
    } else if (by_name && named == Half::Server) {
        r.spec = Spec::Server;
        t.spec = f.server;
        t.scan = scan(t.spec, r.half);
    } else {
        // The client's half, or a name that says neither: both blocks, when
        // the config has both, and the one that reads more of the file.
        // Frames first: a length field that is not one reads a few huge
        // frames that cover most of a file, and no more of it, while the
        // right one reads many. Then the bytes in no frame, which take in a
        // scan that stopped early as resyncs do not. [Framing] on a tie.
        r.spec = Spec::Server;
        t.spec = f.server;
        t.scan = scan(f.server, by_name ? named : Half::Server);
        if (f.client) {
            FrameScanResult c = scan(*f.client, by_name ? named : Half::Client);
            const std::size_t us = unread_of(t.scan, r.size);
            const std::size_t uc = unread_of(c, r.size);
            r.measured = true;
            if (c.outer_frames() > t.scan.outer_frames() ||
                (c.outer_frames() == t.scan.outer_frames() && uc < us)) {
                r.other_frames = t.scan.outer_frames();
                r.other_unread = us;
                r.spec = Spec::Client;
                t.spec = *f.client;
                t.scan = std::move(c);
            } else {
                r.other_frames = c.outer_frames();
                r.other_unread = uc;
            }
        }
        if (!by_name) {
            r.half = (r.spec == Spec::Client) ? Half::Client : Half::Server;
            r.decided_by = HalfSource::Scan;
        }
    }

    const FrameScanResult& s = t.scan;
    r.frames   = s.outer_frames();
    r.resyncs  = s.resyncs;
    r.inner    = s.unwrap.inner;
    r.unwrap   = s.unwrap;
    r.consumed = std::min(s.consumed, r.size);

    // Where the bytes in no frame are: the runs slid past, and whatever the
    // scan never reached, in one run with a slid-past one that ends there.
    r.unread_runs = s.skipped;
    if (r.consumed < r.size) {
        const std::size_t left = r.size - r.consumed;
        if (!r.unread_runs.empty() &&
            r.unread_runs.back().offset + r.unread_runs.back().length == r.consumed)
            r.unread_runs.back().length += left;
        else
            r.unread_runs.push_back(ByteRun{ r.consumed, left });
    }

    // The longest frame read off the file, whole: its header, its body, and
    // the bytes a wrapper carries past its declared length.
    for (const GamePacket& p : s.packets) {
        if (p.depth != 0) continue;
        std::size_t whole = t.spec.header_size + p.payload.size();
        if (t.spec.has_trailer() && p.opcode == t.spec.trailer_opcode) whole += t.spec.trailer_bytes;
        r.largest = std::max(r.largest, whole);
    }
    r.ok = true;
    return t;
}

Reading read_all(const std::string& path, const Framings& framings,
                 std::vector<GamePacket>& out, std::optional<Spec> force) {
    std::ifstream in(utf8_to_wide(path).c_str(), std::ios::binary);
    if (!in) {
        Reading r;
        r.error = "cannot read " + path;
        return r;
    }
    const Bytes buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    Tiling t = tile_named(buf, path, framings, force);
    if (!t.reading.ok) return t.reading;

    // Where they came from, which the bytes cannot say: a tool listing them
    // labels them by it (where_found), as it labels them live.
    const bool udp = udp_from_name(path);
    for (GamePacket& p : t.scan.packets) {
        p.udp = udp;
        out.push_back(std::move(p));
    }
    return t.reading;
}

Reading read_all(const std::string& path,
                 const FramingSpec& server,
                 const FramingSpec& client,
                 std::vector<GamePacket>& out) {
    Framings f;
    f.server = server;
    if (client.plausible()) f.client = client;
    return read_all(path, f, out);
}

Reading extract(const std::string& path,
                const FramingSpec& server,
                const FramingSpec& client,
                std::uint16_t opcode,
                std::vector<Bytes>& out,
                bool inner) {
    std::vector<GamePacket> frames;
    Reading r = read_all(path, server, client, frames);
    if (!r.ok) return r;
    for (const GamePacket& p : frames) {
        if (p.opcode != opcode) continue;
        if ((p.depth > 0) == inner) out.push_back(p.payload);
        else                        ++r.other_kind;
    }
    return r;
}

// ------------------------------------------------ one opcode's frames as numbers

bool number_from_name(const std::string& name, Number& out) {
    std::string s;
    for (char c : name) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    static const struct { const char* name; Number n; } kNames[] = {
        { "u8",  Number::U8 },  { "i8",  Number::I8 },
        { "u16", Number::U16 }, { "i16", Number::I16 },
        { "u32", Number::U32 }, { "i32", Number::I32 },
        { "f32", Number::F32 },
    };
    for (const auto& k : kNames) {
        if (s == k.name) { out = k.n; return true; }
    }
    return false;
}

const char* number_name(Number n) {
    switch (n) {
    case Number::U8:  return "u8";
    case Number::I8:  return "i8";
    case Number::U16: return "u16";
    case Number::I16: return "i16";
    case Number::U32: return "u32";
    case Number::I32: return "i32";
    case Number::F32: return "f32";
    }
    return "?";
}

std::size_t number_size(Number n) {
    switch (n) {
    case Number::U8:
    case Number::I8:  return 1;
    case Number::U16:
    case Number::I16: return 2;
    default:          return 4;
    }
}

bool read_number(const Bytes& body, std::size_t offset, Number n, double& value,
                 std::uint32_t* raw) {
    const std::size_t width = number_size(n);
    if (offset > body.size() || body.size() - offset < width) return false;
    std::uint32_t u = 0;
    for (std::size_t i = 0; i < width; ++i)
        u |= static_cast<std::uint32_t>(body[offset + i]) << (8 * i);
    if (raw) *raw = u;
    switch (n) {
    case Number::U8:
    case Number::U16:
    case Number::U32: value = static_cast<double>(u); break;
    case Number::I8:  value = static_cast<std::int8_t>(u);  break;
    case Number::I16: value = static_cast<std::int16_t>(u); break;
    case Number::I32: value = static_cast<std::int32_t>(u); break;
    case Number::F32: {
        float f = 0.0f;
        std::memcpy(&f, &u, sizeof(f));
        value = f;
        break;
    }
    }
    return true;
}

std::string format_number(double value, Number n) {
    char buf[48];
    if (n != Number::F32) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
        return buf;
    }
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value > 0 ? "inf" : "-inf";
    const double a = std::fabs(value);
    if (a == 0.0 || (a >= 0.01 && a < 1e7)) std::snprintf(buf, sizeof(buf), "%.2f", value);
    else                                    std::snprintf(buf, sizeof(buf), "%.3g", value);
    return buf;
}

bool parse_count(const std::string& text, std::size_t& out) {
    std::size_t i = 0, base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        i = 2;
        base = 16;
    }
    if (i >= text.size()) return false;
    std::size_t value = 0;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        std::size_t digit = 0;
        if (c >= '0' && c <= '9')                    digit = static_cast<std::size_t>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') digit = static_cast<std::size_t>(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') digit = static_cast<std::size_t>(c - 'A' + 10);
        else return false;
        if (value > (SIZE_MAX - digit) / base) return false;
        value = value * base + digit;
    }
    out = value;
    return true;
}

Column column(const std::vector<Bytes>& bodies, Number n, std::size_t offset) {
    Column c;
    c.offset = offset;
    std::set<std::uint32_t> seen;
    bool have_prev = false, have_range = false;
    std::uint32_t prev_raw = 0;
    double prev = 0.0;
    for (const Bytes& b : bodies) {
        double v = 0.0;
        std::uint32_t raw = 0;
        if (!read_number(b, offset, n, v, &raw)) continue;
        ++c.frames;
        seen.insert(raw);
        if (!have_prev) {
            c.first = v;
        } else if (raw != prev_raw) {
            ++c.changes;
            if (v > prev)      ++c.rises;
            else if (v < prev) ++c.falls;
        }
        if (!std::isnan(v)) {
            if (!have_range) { c.min = c.max = v; have_range = true; }
            else             { c.min = std::min(c.min, v); c.max = std::max(c.max, v); }
        }
        c.last = v;
        prev = v;
        prev_raw = raw;
        have_prev = true;
    }
    c.distinct = seen.size();
    return c;
}

std::vector<Column> columns(const std::vector<Bytes>& bodies, Number n,
                            std::size_t start, std::size_t step) {
    std::vector<Column> out;
    if (bodies.empty()) return out;
    std::size_t common = bodies.front().size();
    for (const Bytes& b : bodies) common = std::min(common, b.size());
    const std::size_t width = number_size(n);
    if (step == 0) step = width;
    for (std::size_t off = start; off + width <= common; off += step)
        out.push_back(column(bodies, n, off));
    return out;
}

// -------------------------------------------- writing a recording, per connection

void FlowHalves::add(std::uint64_t flow, const std::string& name, bool to_client,
                     const std::uint8_t* data, std::size_t size) {
    Halves& h = flows_[flow];
    if (h.name.empty()) h.name = name;
    Bytes& to = to_client ? h.s2c : h.c2s;
    if (data != nullptr && size > 0) to.insert(to.end(), data, data + size);
}

std::vector<FlowHalves::File> FlowHalves::files(const std::string& base, bool write) const {
    // Those that carried anything to the client first, in the order of the
    // map, as every connection --record kept apart always was; then those
    // that carried only the client's half.
    std::vector<const Halves*> order;
    for (const auto& entry : flows_)
        if (!entry.second.s2c.empty()) order.push_back(&entry.second);
    for (const auto& entry : flows_)
        if (entry.second.s2c.empty() && !entry.second.c2s.empty()) order.push_back(&entry.second);

    std::vector<File> out;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const Halves& h = *order[i];
        for (const bool to_client : { true, false }) {
            const Bytes& bytes = to_client ? h.s2c : h.c2s;
            if (bytes.empty()) continue;
            File file;
            file.path = base + ".flow" + std::to_string(i) + (to_client ? ".s2c" : ".c2s");
            file.connection = h.name;
            file.bytes = bytes.size();
            file.to_client = to_client;
            if (write) {
                if (std::FILE* fp = open_file(file.path, "wb")) {
                    file.written = std::fwrite(bytes.data(), 1, bytes.size(), fp) == bytes.size();
                    if (std::fclose(fp) != 0) file.written = false;
                }
            }
            out.push_back(std::move(file));
        }
    }
    return out;
}

} // namespace capture_file
} // namespace gobot
