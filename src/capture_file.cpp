#include "gobot/capture_file.hpp"
#include "gobot/common.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

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

} // namespace

bool half_from_name(const std::string& path, Half& out) {
    if (ends_with(path, ".c2s")) { out = Half::Client; return true; }
    if (ends_with(path, ".s2c")) { out = Half::Server; return true; }
    return false;
}

bool udp_from_name(const std::string& path) {
    return ends_with(path, ".udp.s2c") || ends_with(path, ".udp.c2s");
}

FrameScanResult tile(const Bytes& recording, const FramingSpec& spec, Direction dir) {
    // Unlocked: a recording may begin part-way through a frame, and the
    // scanner must slide past that rather than wait for it.
    return extract_frames(spec, recording.data(), recording.size(), dir, 0, false);
}

Reading read_all(const std::string& path,
                 const FramingSpec& server,
                 const FramingSpec& client,
                 std::vector<GamePacket>& out) {
    Reading r;

    std::ifstream in(utf8_to_wide(path).c_str(), std::ios::binary);
    if (!in) { r.error = "cannot read " + path; return r; }
    const std::vector<std::uint8_t> buf((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
    if (buf.empty()) { r.error = path + " is empty"; return r; }

    const bool have_client = client.plausible();

    Half named{};
    FrameScanResult scan;
    if (half_from_name(path, named) && (named == Half::Server || have_client)) {
        r.half = named;
        r.decided_by = HalfSource::Name;
        const FramingSpec& spec = (named == Half::Client) ? client : server;
        const Direction dir = (named == Half::Client) ? Direction::ToServer
                                                      : Direction::ToClient;
        scan = extract_frames(spec, buf.data(), buf.size(), dir, 0, false);
    } else {
        r.decided_by = HalfSource::Scan;
        // Frames first, resyncs only to break a tie: a spec that reads more of
        // the file is reading more of the file, whereas a low resync count can
        // simply mean the scanner gave up early.
        FrameScanResult best;
        bool have_best = false;
        Half best_half = Half::Server;

        const FrameScanResult s = extract_frames(server, buf.data(), buf.size(),
                                                 Direction::ToClient, 0, false);
        best = s; have_best = true; best_half = Half::Server;

        if (have_client) {
            // Frames off the stream decide it, not what unwrapping adds: only
            // one of the two specs looks inside wrappers at all.
            const FrameScanResult c = extract_frames(client, buf.data(), buf.size(),
                                                     Direction::ToServer, 0, false);
            if (c.outer_frames() > best.outer_frames() ||
                (c.outer_frames() == best.outer_frames() && c.resyncs < best.resyncs)) {
                best = c; best_half = Half::Client;
            }
        }
        if (!have_best) { r.error = "no usable framing"; return r; }
        scan = std::move(best);
        r.half = best_half;
    }

    r.frames  = scan.outer_frames();
    r.resyncs = scan.resyncs;
    r.inner   = scan.unwrap.inner;
    r.unwrap  = scan.unwrap;
    // Where they came from, which the bytes cannot say: a tool listing them
    // labels them by it (where_found), as it labels them live.
    const bool udp = udp_from_name(path);
    for (GamePacket& p : scan.packets) {
        p.udp = udp;
        out.push_back(std::move(p));
    }
    r.ok = true;
    return r;
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

} // namespace capture_file
} // namespace gobot
