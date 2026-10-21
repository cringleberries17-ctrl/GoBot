// gobot_test — self-tests for the parts that can be checked without the game.
//
// The protocol layers are the ones most likely to be subtly wrong and the
// hardest to debug live: by the time a framing bug shows up as "the bot walks
// into a wall", the cause is six layers down. These tests drive each layer
// with synthetic data whose correct answer is known in advance.

#include "gobot/byte_reader.hpp"
#include "gobot/config.hpp"
#include "gobot/capture_file.hpp"
#include "gobot/framing.hpp"
#include "gobot/command_executor.hpp"
#include "gobot/game_state.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/camera_tracker.hpp"
#include "gobot/field_finder.hpp"
#include "gobot/hunter.hpp"
#include "gobot/macro.hpp"
#include "gobot/map_file.hpp"
#include "gobot/opcode_registry.hpp"
#include "gobot/own_position.hpp"
#include "gobot/monster_frames.hpp"
#include "gobot/net_discovery.hpp"
#include "gobot/packet_capture.hpp"
#include "gobot/pathfinder.hpp"
#include "gobot/protocol_analyzer.hpp"
#include "gobot/minimap_tracker.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/map_table.hpp"
#include "gobot/template_matcher.hpp"
#include "gobot/world_anchor.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/protocol_handler.hpp"
#include "gobot/tcp_reassembler.hpp"
#include "gobot/auto_chat.hpp"
#include "gobot/auto_login.hpp"
#include "gobot/bag.hpp"
#include "gobot/dialog_driver.hpp"
#include "gobot/gatherer.hpp"
#include "gobot/party.hpp"
#include "gobot/party_chat.hpp"
#include "gobot/routine.hpp"
#include "gobot/crash_watch.hpp"
#include "gobot/game_install.hpp"
#include "gobot/no_real_input.hpp"
#include "gobot/party_invite.hpp"
#include "gobot/player_list.hpp"
#include "gobot/reviver.hpp"
#include "gobot/screen_layout.hpp"
#include "gobot/session_manager.hpp"
#include "gobot/vitals_reader.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <windows.h>
#include <winioctl.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace gobot;

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool cond, const std::string& what) {
    if (cond) {
        ++g_passed;
        std::printf("  [ ok ] %s\n", what.c_str());
    } else {
        ++g_failed;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
    // Out at once, a whole line at a time. Sent to a file, stdout waits for a
    // full buffer while the log writes to stderr straight away, and its lines
    // landed in the middle of a check's name -- one a search for it missed.
    std::fflush(stdout);
}

void section(const char* name) {
    std::printf("\n== %s ==\n", name);
    std::fflush(stdout);
}

/// A writable path for tests that need a real file.
///
/// Not the working directory: that is wherever the shell happened to be, and
/// when it is somewhere like Program Files the write fails and the test reports
/// a product bug that does not exist. The temp directory is writable by
/// definition.
///
/// And in a folder of this run's own there, gobot_test_<process id>, made as
/// the run starts and removed as it ends (alerts_test.cpp and quest_test.cpp
/// name the same one). With fixed names in %TEMP% itself, two runs at once --
/// a reviewer's and a tester's -- wrote, read and deleted each other's files,
/// and both failed checks for nothing in the code.
std::string temp_file(const char* name) {
    char dir[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return name;      // fall back to CWD
    return std::string(dir) + "gobot_test_" + std::to_string(GetCurrentProcessId()) + "\\" + name;
}

/// The folder temp_file() puts things in, as a wide path, for making it and
/// removing it.
std::wstring run_temp_dir() {
    wchar_t dir[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return {};
    return std::wstring(dir) + L"gobot_test_" + std::to_wstring(GetCurrentProcessId());
}

// ---------------------------------------------------------------- ByteReader

void test_byte_reader() {
    section("ByteReader");

    const std::uint8_t buf[] = {
        0x01,                                  // u8
        0x02, 0x00,                            // u16 le = 2
        0x00, 0x03,                            // u16 be = 3
        0x00, 0x00, 0x80, 0x3F,                // f32 le = 1.0
        0x03, 'a', 'b', 'c',                   // str8 "abc"
        'x', 'y', 0x00,                        // cstring "xy"
    };

    ByteReader r(buf, sizeof(buf));
    check(r.u8() == 1, "u8");
    check(r.u16(true) == 2, "u16 little-endian");
    check(r.u16(false) == 3, "u16 big-endian");
    check(std::abs(r.f32() - 1.0f) < 1e-6f, "f32");
    check(r.prefixed_string(1) == "abc", "length-prefixed string");
    check(r.cstring() == "xy", "NUL-terminated string");
    check(r.ok(), "reader still ok after valid reads");

    // Over-read must poison rather than throw or read out of bounds.
    ByteReader r2(buf, 2);
    r2.u32();
    check(!r2.ok(), "over-read sets not-ok");
    check(r2.u8() == 0, "reads after failure return zero");

    ByteReader r3(nullptr, 0);
    r3.u8();
    check(!r3.ok(), "empty buffer handled");
}

// ------------------------------------------------------------------- framing

/// Build a stream in a known framing so the detector has a correct answer to
/// find. Header: [u16 le total length][u16 le opcode].
std::vector<std::uint8_t> synth_stream(std::size_t frames, std::uint32_t seed) {
    std::mt19937 rng(seed);
    // A small recurring opcode set, as a real protocol has.
    const std::uint16_t opcodes[] = { 0x0101, 0x0102, 0x0203, 0x0310, 0x0405, 0x0406 };

    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < frames; ++i) {
        const std::uint16_t op = opcodes[rng() % (sizeof(opcodes) / sizeof(opcodes[0]))];

        // Mostly small messages, with the occasional large one — map data,
        // inventory and party lists all run into the hundreds of bytes. This
        // matters for the detector: if every frame fits in 255 bytes, a
        // one-byte length field explains the stream exactly as well as the
        // real two-byte one, and no scoring rule can honestly separate them.
        const bool large = (rng() % 10) == 0;
        const std::uint16_t body = large
            ? static_cast<std::uint16_t>(300 + rng() % 900)
            : static_cast<std::uint16_t>(4 + rng() % 28);
        const std::uint16_t total = static_cast<std::uint16_t>(4 + body);

        out.push_back(static_cast<std::uint8_t>(total & 0xFF));
        out.push_back(static_cast<std::uint8_t>(total >> 8));
        out.push_back(static_cast<std::uint8_t>(op & 0xFF));
        out.push_back(static_cast<std::uint8_t>(op >> 8));
        for (std::uint16_t b = 0; b < body; ++b)
            out.push_back(static_cast<std::uint8_t>(rng() & 0xFF));
    }
    return out;
}

FramingSpec reference_spec() {
    FramingSpec s;
    s.header_size = 4;
    s.length_offset = 0;
    s.length_size = 2;
    s.length_le = true;
    s.length_includes_header = true;
    s.length_adjust = 0;
    s.opcode_offset = 2;
    s.opcode_size = 2;
    s.opcode_le = true;
    s.min_frame = 4;
    s.max_frame = 32768;
    return s;
}

void test_framing_extract() {
    section("Frame extraction");

    const auto stream = synth_stream(50, 1234);
    const auto spec = reference_spec();

    auto scan = extract_frames(spec, stream.data(), stream.size(), Direction::ToClient, 0);

    check(scan.packets.size() == 50, "extracts all 50 frames");
    check(scan.consumed == stream.size(), "consumes the whole buffer");
    check(scan.resyncs == 0, "no resyncs on a clean stream");

    // A truncated tail must be left buffered, not mis-parsed.
    auto partial = stream;
    partial.resize(partial.size() - 3);
    auto scan2 = extract_frames(spec, partial.data(), partial.size(), Direction::ToClient, 0);
    check(scan2.packets.size() == 49, "truncated final frame is withheld");
    check(scan2.consumed < partial.size(), "incomplete frame stays buffered");

    // Garbage in front must be resynced past, not silently accepted. This is
    // the normal case when joining an established connection mid-stream.
    std::vector<std::uint8_t> noisy(7, 0xFF);
    noisy.insert(noisy.end(), stream.begin(), stream.end());
    auto scan3 = extract_frames(spec, noisy.data(), noisy.size(), Direction::ToClient, 0);
    check(scan3.resyncs > 0, "leading garbage counted as resync");
    check(scan3.packets.size() >= 49, "re-locks and recovers frames after resync");

    // Joining mid-stream on bytes that happen to parse as a huge length must
    // not stall the scanner waiting for data that will never come. Passing
    // locked=false is what tells it those opening bytes are untrustworthy.
    std::vector<std::uint8_t> trap = { 0xFF, 0x7F, 0x00, 0x00 };
    trap.insert(trap.end(), stream.begin(), stream.end());
    auto scan4 = extract_frames(spec, trap.data(), trap.size(), Direction::ToClient, 0, false);
    check(scan4.packets.size() >= 49, "unlocked stream slides past a bogus huge length");
    check(scan4.locked, "scanner reports lock once frames parse");

    // The mirror case: while locked, an over-long frame is real and merely
    // incomplete. Sliding past it would shred a half-delivered large packet.
    const auto big = synth_stream(6, 555);
    auto half = big;
    half.resize(half.size() - 40);
    auto scan5 = extract_frames(spec, half.data(), half.size(), Direction::ToClient, 0, true);
    check(scan5.consumed < half.size(), "locked stream waits for an incomplete frame");
    check(scan5.resyncs == 0, "locked stream does not resync on incomplete data");

    check(!extract_frames(FramingSpec{}, nullptr, 0, Direction::ToClient, 0).packets.size(),
          "null buffer is safe");
}

void test_framing_detect() {
    section("Framing auto-detection");

    const auto stream = synth_stream(800, 99);
    const auto results = detect_framing(stream.data(), stream.size(), 5);

    check(!results.empty(), "detector returns candidates");
    if (results.empty()) return;

    const auto& best = results.front();
    std::printf("         best: %s (coverage %.1f%%, %zu frames)\n",
                best.spec.describe().c_str(), best.coverage * 100.0, best.frames_parsed);

    check(best.coverage > 0.98, "best candidate explains >98% of the stream");
    check(best.spec.length_offset == 0, "recovers length offset");
    check(best.spec.length_size == 2, "recovers length width");
    check(best.spec.length_le, "recovers length endianness");
    check(best.spec.opcode_offset == 2, "recovers opcode offset");
    check(best.spec.header_size == 4, "recovers header size");

    // The recovered spec must actually reproduce the original frames.
    auto scan = extract_frames(best.spec, stream.data(), stream.size(), Direction::ToClient, 0);
    check(scan.packets.size() == 800, "recovered spec re-extracts all frames");
    check(scan.resyncs == 0, "recovered spec needs no resyncs");

    // Random bytes have no framing; claiming one would be a false positive.
    std::mt19937 rng(7);
    std::vector<std::uint8_t> noise(8192);
    for (auto& b : noise) b = static_cast<std::uint8_t>(rng() & 0xFF);
    const auto junk = detect_framing(noise.data(), noise.size(), 5);
    const bool confident = !junk.empty() && junk.front().coverage > 0.98 &&
                           junk.front().frames_parsed > 100;
    check(!confident, "does not confidently claim framing for random noise");
}

// -------------------------------------------------------------- checksums

/// Build a stream in GhostOnline's actual shape: an 8-byte header of four
/// u16s where the last is the sum of the first three.
///
///   off0 marker | off2 opcode | off4 total length | off6 checksum
///
/// `corrupt_every` drops a run of bytes periodically, standing in for the
/// packet loss and undescribed bodies a real capture always contains.
std::vector<std::uint8_t> synth_checksummed(std::size_t frames, std::uint32_t seed,
                                            std::size_t corrupt_every = 0) {
    std::mt19937 rng(seed);
    const std::uint16_t opcodes[] = { 0x0038, 0x0081, 0x03C6, 0x0723, 0x004D };
    const std::uint16_t sizes[]   = { 96, 270, 12, 16, 40 };

    std::vector<std::uint8_t> out;
    auto put16 = [&out](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v & 0xFF));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };

    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t k = rng() % 5;
        const std::uint16_t marker = 0x0105;
        const std::uint16_t op     = opcodes[k];
        const std::uint16_t total  = sizes[k];
        const std::uint16_t check  = static_cast<std::uint16_t>((marker + op + total) & 0xFFFF);

        put16(marker); put16(op); put16(total); put16(check);

        // Mostly zeros, as real game payloads are — which is what makes the
        // degenerate "0 + 0 == 0" checksum match a genuine hazard.
        for (std::uint16_t b = 8; b < total; ++b)
            out.push_back((rng() % 3 == 0) ? static_cast<std::uint8_t>(rng() & 0xFF) : 0);

        if (corrupt_every && i && i % corrupt_every == 0) {
            for (int g = 0; g < 37; ++g) out.push_back(static_cast<std::uint8_t>(rng() & 0xFF));
        }
    }
    return out;
}

void test_checksum_framing() {
    section("Checksum-validated framing");

    const auto clean = synth_checksummed(400, 2024);
    const auto found = detect_checksum(clean.data(), clean.size());

    check(!found.empty(), "finds a header checksum");
    if (!found.empty()) {
        check(found.front().header_size == 8, "recovers the header size");
        check(found.front().offset == 6, "recovers the check word offset");
        check(static_cast<double>(found.front().hits) > found.front().expected * 20.0,
              "hit count is far above chance");
    }

    // Random bytes must not produce a phantom checksum.
    std::mt19937 rng(88);
    std::vector<std::uint8_t> noise(40000);
    for (auto& b : noise) b = static_cast<std::uint8_t>(rng() & 0xFF);
    check(detect_checksum(noise.data(), noise.size()).empty(),
          "no checksum claimed for random bytes");

    // Mostly-zero bytes must not either: 0 + 0 == 0 holds everywhere in a zero
    // run, and that false signal is what swamped the real one first time.
    std::vector<std::uint8_t> zeros(40000, 0);
    for (std::size_t i = 0; i < zeros.size(); i += 97)
        zeros[i] = static_cast<std::uint8_t>(rng() & 0xFF);
    check(detect_checksum(zeros.data(), zeros.size()).empty(),
          "no checksum claimed for a mostly-zero buffer");

    // The payoff: a stream with gaps. Without checksum validation the scanner
    // latches onto a stray length after each gap and swallows the frames
    // behind it; with it, every boundary is provable and it stays locked.
    const auto damaged = synth_checksummed(400, 7, 25);

    FramingSpec spec;
    spec.header_size = 8;
    spec.length_offset = 4; spec.length_size = 2; spec.length_le = true;
    spec.length_includes_header = true;
    spec.opcode_offset = 2; spec.opcode_size = 2; spec.opcode_le = true;
    spec.min_frame = 8; spec.max_frame = 65535;

    auto without = extract_frames(spec, damaged.data(), damaged.size(),
                                  Direction::ToClient, 0, false);
    spec.checksum_offset = 6;
    auto with = extract_frames(spec, damaged.data(), damaged.size(),
                               Direction::ToClient, 0, false);

    std::printf("         damaged stream: %zu frames without checksum, %zu with\n",
                without.packets.size(), with.packets.size());

    check(with.packets.size() > without.packets.size(),
          "checksum validation recovers more frames from a damaged stream");
    check(with.packets.size() >= 380, "recovers nearly every frame despite the gaps");

    for (const auto& p : with.packets) {
        if (p.payload.size() > 400) {
            check(false, "checksum validation still produced an oversized frame");
            break;
        }
    }
    check(true, "no oversized frames survive validation");

    // And detection should now pick this layout out of the search.
    const auto ranked = detect_framing(damaged.data(), damaged.size(), 5);
    check(!ranked.empty(), "detector returns candidates for a checksummed stream");
    if (!ranked.empty()) {
        const auto& best = ranked.front().spec;
        check(best.header_size == 8, "detector recovers header size 8");
        check(best.length_offset == 4 && best.length_size == 2,
              "detector recovers the length field");
        check(best.opcode_offset == 2, "detector recovers the opcode offset");
        check(best.has_checksum() && best.checksum_offset == 6,
              "detector attaches the checksum to the winning spec");
    }
}

// ------------------------------------------------------- trailing bytes

/// A stream where one opcode carries bytes past its declared length.
///
/// This is the real GhostOnline server stream: 0x0081 is followed by twelve
/// bytes the length field does not count.
std::vector<std::uint8_t> synth_with_trailer(std::size_t frames, std::uint32_t seed,
                                             std::uint16_t trailer_op,
                                             std::size_t trailer_len) {
    std::mt19937 rng(seed);
    const std::uint16_t opcodes[] = { 0x0038, 0x0081, 0x03C6, 0x0723 };
    const std::uint16_t sizes[]   = { 96, 25, 12, 16 };

    std::vector<std::uint8_t> out;
    auto put16 = [&out](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v & 0xFF));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };

    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t k = rng() % 4;
        const std::uint16_t marker = 0x0105;
        const std::uint16_t op     = opcodes[k];
        const std::uint16_t total  = sizes[k];      // what the header claims
        const std::uint16_t check  = static_cast<std::uint16_t>((marker + op + total) & 0xFFFF);

        put16(marker); put16(op); put16(total); put16(check);
        for (std::uint16_t b = 8; b < total; ++b)
            out.push_back((rng() % 3 == 0) ? static_cast<std::uint8_t>(rng() & 0xFF) : 0);

        // The bytes the length field does not admit to.
        if (op == trailer_op)
            for (std::size_t t = 0; t < trailer_len; ++t)
                out.push_back(static_cast<std::uint8_t>(0xA0 + (t & 0x0F)));
    }
    return out;
}

/// Frames that carry more bytes than they declare.
///
/// Worth its own test because the failure is so disproportionate. Twelve
/// unaccounted bytes do not cost one frame: the scanner loses alignment and
/// slides a byte at a time, swallowing whole real frames, until something
/// parses again. On live captures one 0x0081 every few hundred frames hid
/// nearly three quarters of the stream, and it presented as a wrong
/// FramingSpec rather than a right one with an exception in it.
void test_frame_trailer() {
    section("Frames carrying bytes past their declared length");

    constexpr std::uint16_t kOp = 0x0081;
    constexpr std::size_t   kTrailer = 12;
    const auto stream = synth_with_trailer(300, 4242, kOp, kTrailer);

    FramingSpec spec;
    spec.header_size = 8;
    spec.length_offset = 4; spec.length_size = 2; spec.length_le = true;
    spec.length_includes_header = true;
    spec.opcode_offset = 2; spec.opcode_size = 2; spec.opcode_le = true;
    spec.min_frame = 8; spec.max_frame = 65535;
    spec.checksum_offset = 6;

    const auto blind = extract_frames(spec, stream.data(), stream.size(),
                                      Direction::ToClient, 0, false);

    spec.trailer_opcode = kOp;
    spec.trailer_bytes  = kTrailer;
    const auto aware = extract_frames(spec, stream.data(), stream.size(),
                                      Direction::ToClient, 0, false);

    std::printf("         %zu frames / %zu resyncs blind, %zu / %zu aware\n",
                blind.packets.size(), blind.resyncs,
                aware.packets.size(), aware.resyncs);

    check(aware.resyncs == 0, "a spec that knows about the trailer never resyncs");
    check(aware.packets.size() == 300, "and recovers every frame");
    check(blind.resyncs > 0, "one that does not, does resync");
    check(aware.packets.size() > blind.packets.size(),
          "so the trailer-aware spec recovers strictly more frames");

    // The trailer is framing, not content: a field at body offset N must be at
    // offset N whether or not its opcode carries one.
    std::size_t checked = 0;
    for (const auto& p : aware.packets) {
        if (p.opcode != kOp) continue;
        check(p.payload.size() == 25 - 8, "the trailer is stripped from the payload");
        if (++checked == 1) break;
    }
    check(checked == 1, "the stream contained a trailer-carrying frame to check");

    // Opcodes that carry no trailer must be untouched by the rule.
    for (const auto& p : aware.packets) {
        if (p.opcode != 0x0038) continue;
        check(p.payload.size() == 96 - 8, "other opcodes keep their normal length");
        break;
    }
}

// ------------------------------------------------- frames inside 0x0081

void put16le(Bytes& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}

/// One server frame as the game sends it: marker, opcode, total length and a
/// check word equal to the sum of those three, then the body.
Bytes game_frame(std::uint16_t opcode, const Bytes& body, std::uint16_t marker = 0x0005) {
    Bytes f;
    const auto total = static_cast<std::uint16_t>(8 + body.size());
    put16le(f, marker);
    put16le(f, opcode);
    put16le(f, total);
    put16le(f, static_cast<std::uint16_t>((marker + opcode + total) & 0xFFFF));
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

/// An 0x0081 frame around `inner`, laid out the way GoC's parser reads one:
/// it occupies its declared length + 12 bytes, and the inner frame is bytes 13
/// to the end. So the declared length is the inner frame's size + 1, and five
/// bytes of something nobody has read yet sit between the header and it.
Bytes wrapped(const Bytes& inner, std::uint16_t marker = 0x0005) {
    Bytes w;
    const auto declared = static_cast<std::uint16_t>(inner.size() + 1);
    put16le(w, marker);
    put16le(w, 0x0081);
    put16le(w, declared);
    put16le(w, static_cast<std::uint16_t>((marker + 0x0081 + declared) & 0xFFFF));
    for (int b : { 0xA1, 0xA2, 0xA3, 0xA4, 0xA5 }) w.push_back(static_cast<std::uint8_t>(b));
    w.insert(w.end(), inner.begin(), inner.end());
    return w;
}

Bytes some_body(std::size_t n, std::uint8_t seed) {
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>(seed + i * 7);
    return b;
}

Bytes joined(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

/// The shipped server framing, unwrapping as the config readers now set it.
FramingSpec unwrapping_spec() {
    FramingSpec spec;
    spec.header_size = 8;
    spec.length_offset = 4; spec.length_size = 2; spec.length_le = true;
    spec.length_includes_header = true;
    spec.opcode_offset = 2; spec.opcode_size = 2; spec.opcode_le = true;
    spec.min_frame = 8; spec.max_frame = 16384;
    spec.checksum_offset = 6;
    spec.trailer_opcode = 0x0081; spec.trailer_bytes = 12;
    spec.inner_offset = 13;
    return spec;
}

/// The body of a move as GoC's client reads one: x and y as floats. Where they
/// sit is not known yet; here at body offsets 8 and 12, so the test can read
/// them back where it put them.
Bytes move_body() {
    Bytes b = some_body(21, 0x30);      // a 29-byte frame, GoC's minimum for 0x0027
    const float x = 891.25f, y = 1096.0f;
    std::memcpy(b.data() + 8, &x, 4);
    std::memcpy(b.data() + 12, &y, 4);
    return b;
}

bool same_frames(const std::vector<GamePacket>& a, const std::vector<GamePacket>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].opcode != b[i].opcode || a[i].payload != b[i].payload ||
            a[i].depth != b[i].depth || a[i].from_server != b[i].from_server)
            return false;
    return true;
}

/// GoC's parser treats every 0x0081 as a wrapper around a whole frame: bytes
/// 13 to the declared length + 12. GoBot dropped that as a trailer and decoded
/// nothing inside. These are hand-built frames whose answer is known: a plain
/// one, a wrapped one, one wrapped twice, one cut by a segment boundary, and
/// the cases GoC's code handles specially.
void test_unwrap() {
    section("Frames inside 0x0081");

    const FramingSpec spec = unwrapping_spec();
    const Bytes body38 = some_body(88, 3);
    const Bytes plain  = game_frame(0x0038, body38);          // 96 bytes
    const Bytes body27 = move_body();
    const Bytes move   = game_frame(0x0027, body27);          // 29 bytes
    const Bytes w1     = wrapped(move);                       // 42 bytes, declares 30

    check(spec.unwraps(), "the shipped spec, with inner_offset 13, unwraps");

    // A plain frame: exactly as before.
    {
        const auto scan = extract_frames(spec, plain.data(), plain.size(),
                                         Direction::ToClient, 7, false);
        check(scan.packets.size() == 1 && scan.packets[0].opcode == 0x0038 &&
                  scan.packets[0].payload == body38 && scan.packets[0].depth == 0 &&
                  !scan.packets[0].udp && scan.packets[0].plain(),
              "a plain frame comes out as before: its opcode, its body, depth 0");
        check(scan.unwrap.inner == 0 && scan.resyncs == 0 && scan.consumed == plain.size(),
              "with nothing unwrapped and nothing resynced");
    }

    // A wrapped frame: the wrapper as before, then the frame inside it.
    {
        const auto scan = extract_frames(spec, w1.data(), w1.size(),
                                         Direction::ToClient, 7, false);
        check(scan.packets.size() == 2,
              "a wrapped frame gives two frames: the wrapper and the one inside -- got " +
                  std::to_string(scan.packets.size()));
        check(scan.consumed == w1.size() && scan.resyncs == 0,
              "the wrapper's declared length + 12 is consumed, with no resync");
        if (scan.packets.size() == 2) {
            const GamePacket& outer = scan.packets[0];
            const GamePacket& in    = scan.packets[1];
            check(outer.opcode == 0x0081 && outer.depth == 0,
                  "the wrapper comes first, in its place, at depth 0");
            check(outer.payload == Bytes(w1.begin() + 8, w1.begin() + 30),
                  "with the payload it always had: bytes 8 up to its declared length");
            check(in.opcode == 0x0027 && in.depth == 1 && !in.plain(),
                  "then the inner frame, with its own opcode, at depth 1");
            check(in.payload == body27,
                  "and its own payload: its body, from its byte 8 to the wrapper's end");
            check(in.from_server && in.timestamp_ms == 7 && !in.udp,
                  "in the wrapper's direction, with its time");
            float x = 0, y = 0;
            if (in.payload.size() >= 16) {
                std::memcpy(&x, in.payload.data() + 8, 4);
                std::memcpy(&y, in.payload.data() + 12, 4);
            }
            check(x == 891.25f && y == 1096.0f,
                  "floats at the inner frame's body offsets 8 and 12 read back there");
        }
        check(scan.unwrap.inner == 1 && scan.outer_frames() == 1,
              "one frame counted as found inside, one read off the stream");
        check(scan.unwrap.length_differs == 0 && scan.unwrap.checksum_bad == 0,
              "the inner frame's own length and check word agree with the room it fills");
    }

    // What was read before is read exactly the same.
    {
        FramingSpec off = spec;
        off.inner_offset = 0;
        const Bytes stream = joined({ plain, w1, plain });
        const auto before = extract_frames(off, stream.data(), stream.size(),
                                           Direction::ToClient, 0, false);
        const auto after  = extract_frames(spec, stream.data(), stream.size(),
                                           Direction::ToClient, 0, false);
        std::vector<GamePacket> outer_after;
        for (const auto& p : after.packets)
            if (p.depth == 0) outer_after.push_back(p);
        check(!off.unwraps() && same_frames(before.packets, outer_after),
              "frames off the stream are the same, in the same order, with unwrapping as without");
        check(before.consumed == after.consumed && before.resyncs == after.resyncs,
              "and so are the bytes consumed and resynced");
        check(after.packets.size() == 4 && after.packets[1].opcode == 0x0081 &&
                  after.packets[2].opcode == 0x0027 && after.packets[2].depth == 1 &&
                  after.packets[3].opcode == 0x0038,
              "the inner frame comes straight after its wrapper, before the next frame");
    }

    // The stream the trailer test above uses, read with unwrapping on: its
    // 0x0081 frames hold random bytes, not frames, and still nothing read off
    // the stream changes.
    {
        const auto stream = synth_with_trailer(300, 4242, 0x0081, 12);
        FramingSpec off = spec;
        off.inner_offset = 0;
        const auto before = extract_frames(off, stream.data(), stream.size(),
                                           Direction::ToClient, 0, false);
        const auto after  = extract_frames(spec, stream.data(), stream.size(),
                                           Direction::ToClient, 0, false);
        std::vector<GamePacket> outer_after;
        for (const auto& p : after.packets)
            if (p.depth == 0) outer_after.push_back(p);
        check(after.outer_frames() == 300 && after.resyncs == 0 &&
                  same_frames(before.packets, outer_after),
              "on the trailer test's stream, unwrapping leaves all 300 frames and 0 resyncs as they were");
        std::printf("         and finds %zu frames inside its 0x0081s, whose insides are random bytes\n",
                    after.unwrap.inner);
    }

    // Wrapped twice, as GoC's parser handles by calling itself.
    {
        const Bytes body2a = some_body(28, 9);
        const Bytes jump   = game_frame(0x002A, body2a);       // 36 bytes, GoC's minimum for 0x002A
        const Bytes w2     = wrapped(wrapped(jump));
        const auto scan = extract_frames(spec, w2.data(), w2.size(), Direction::ToClient, 0, false);
        check(scan.packets.size() == 3,
              "a doubly wrapped frame gives both wrappers and the frame inside -- got " +
                  std::to_string(scan.packets.size()));
        if (scan.packets.size() == 3) {
            check(scan.packets[0].opcode == 0x0081 && scan.packets[0].depth == 0 &&
                      scan.packets[1].opcode == 0x0081 && scan.packets[1].depth == 1,
                  "the outer wrapper at depth 0, the inner one at depth 1");
            check(scan.packets[2].opcode == 0x002A && scan.packets[2].depth == 2 &&
                      scan.packets[2].payload == body2a,
                  "and the innermost frame at depth 2, with its own opcode and payload");
        }
        check(scan.consumed == w2.size() && scan.resyncs == 0 && scan.unwrap.inner == 2,
              "all of it consumed, no resync, two frames found inside");
    }

    // Cut by a segment boundary in the middle of the frame inside: nothing is
    // read early, and when the rest comes the inner frame is whole.
    {
        const Bytes stream = joined({ plain, w1 });
        const std::size_t cut = plain.size() + 20;
        const auto a = extract_frames(spec, stream.data(), cut, Direction::ToClient, 0, true);
        check(a.packets.size() == 1 && a.packets[0].opcode == 0x0038 &&
                  a.consumed == plain.size() && a.resyncs == 0,
              "a wrapper cut by a segment boundary waits in the buffer, nothing resynced");
        const Bytes rest(stream.begin() + static_cast<std::ptrdiff_t>(a.consumed), stream.end());
        const auto b = extract_frames(spec, rest.data(), rest.size(), Direction::ToClient, 0, true);
        check(b.packets.size() == 2 && b.packets[1].opcode == 0x0027 &&
                  b.packets[1].depth == 1 && b.packets[1].payload == body27,
              "when the rest arrives, the frame inside comes out whole");
    }

    // A wrapper whose marker reads 0x0020: GoC stops reading the segment; here
    // its inside is left shut and counted, and framing goes on after it.
    {
        const Bytes stream = joined({ wrapped(move, 0x0020), plain });
        const auto scan = extract_frames(spec, stream.data(), stream.size(),
                                         Direction::ToClient, 0, false);
        check(scan.packets.size() == 2 && scan.packets[0].opcode == 0x0081 &&
                  scan.packets[1].opcode == 0x0038,
              "a wrapper marked 0x0020 comes out as itself, and the next frame after it");
        check(scan.unwrap.unopened == 1 && scan.unwrap.inner == 0,
              "its inside is not read, and that is counted, not silent");
        check(scan.resyncs == 0 && scan.consumed == stream.size(),
              "nothing after it is lost or resynced");
    }

    // Inside a wrapper, where no check word proves where frames start, GoC's
    // rule is kept to the letter: a wrapper left shut ends the run it is in.
    {
        const Bytes run = joined({ wrapped(move, 0x0020), game_frame(0x0038, some_body(8, 4)) });
        const Bytes w = wrapped(run);
        const auto scan = extract_frames(spec, w.data(), w.size(), Direction::ToClient, 0, false);
        check(scan.packets.size() == 2 && scan.packets[1].opcode == 0x0081 &&
                  scan.packets[1].depth == 1,
              "a shut wrapper inside a wrapper comes out as itself");
        check(scan.unwrap.unopened == 1 && scan.unwrap.inner == 1,
              "and the run of frames it was in is read no further, as GoC reads it; counted");
        check(scan.consumed == w.size() && scan.resyncs == 0, "the stream itself goes on");
    }

    // Wrappers inside wrappers stop at the depth limit, counted.
    {
        Bytes deep = move;
        for (int i = 0; i < kMaxWrapDepth + 2; ++i) deep = wrapped(deep);
        const auto scan = extract_frames(spec, deep.data(), deep.size(),
                                         Direction::ToClient, 0, false);
        std::uint8_t deepest = 0;
        for (const auto& p : scan.packets) deepest = std::max(deepest, p.depth);
        check(deepest == kMaxWrapDepth, "nested wrappers are opened down to kMaxWrapDepth (" +
                                            std::to_string(kMaxWrapDepth) + ") and no further");
        check(scan.unwrap.too_deep == 1, "the wrapper at the limit is counted, not opened");
        check(scan.consumed == deep.size() && scan.resyncs == 0,
              "and the stream is still read whole");
    }

    // An inner frame shorter than its room is taken as the whole room, as GoC
    // takes it, and the disagreement is counted.
    {
        Bytes roomy = move;
        for (int i = 0; i < 3; ++i) roomy.push_back(0xEE);
        const Bytes w = wrapped(roomy);
        const auto scan = extract_frames(spec, w.data(), w.size(), Direction::ToClient, 0, false);
        check(scan.packets.size() == 2 && scan.packets[1].opcode == 0x0027 &&
                  scan.packets[1].payload.size() == body27.size() + 3,
              "an inner frame that declares less than its room still fills the room");
        check(scan.unwrap.length_differs == 1 && scan.unwrap.length_longer == 0,
              "and the difference is counted, as one that claims less");
    }

    // One that declares more than its room -- every disagreeing inner frame
    // of the P0-2 recordings does -- is taken as the room too, and counted
    // apart: so the tools can say no room held a second frame.
    {
        Bytes claims = move;                      // declares 29 bytes; the room holds 29
        const auto more = static_cast<std::uint16_t>(claims.size() + 40);
        claims[4] = static_cast<std::uint8_t>(more & 0xFF);
        claims[5] = static_cast<std::uint8_t>(more >> 8);
        const auto sum = static_cast<std::uint16_t>((0x0005 + 0x0027 + more) & 0xFFFF);
        claims[6] = static_cast<std::uint8_t>(sum & 0xFF);
        claims[7] = static_cast<std::uint8_t>(sum >> 8);
        const Bytes w = wrapped(claims);
        const auto scan = extract_frames(spec, w.data(), w.size(), Direction::ToClient, 0, false);
        check(scan.packets.size() == 2 && scan.packets[1].payload == body27 &&
                  scan.unwrap.checksum_bad == 0,
              "an inner frame that declares more than its room is still the room");
        check(scan.unwrap.length_differs == 1 && scan.unwrap.length_longer == 1,
              "and is counted as one claiming more than the room holds");
        UnwrapCounts sum_of;
        sum_of += scan.unwrap;
        sum_of += scan.unwrap;
        check(sum_of.length_longer == 2 && sum_of.length_differs == 2,
              "counts claiming more add up as the others do");
    }

    // A wrapper inside that claims more room than there is: not read, counted.
    {
        const Bytes w = wrapped(game_frame(0x0081, some_body(10, 1)));
        const auto scan = extract_frames(spec, w.data(), w.size(), Direction::ToClient, 0, false);
        check(scan.packets.size() == 1 && scan.unwrap.unreadable == 1,
              "a frame inside that runs past the wrapper's end is counted unreadable, not read");
        check(scan.consumed == w.size() && scan.resyncs == 0, "and the stream goes on");
    }

    // An inner offset inside the header cannot be a frame of its own.
    {
        FramingSpec low = spec;
        low.inner_offset = 5;
        const auto scan = extract_frames(low, w1.data(), w1.size(), Direction::ToClient, 0, false);
        check(!low.unwraps() && scan.packets.size() == 1,
              "an inner offset inside the header unwraps nothing");
    }

    // How the tools name where a frame was found.
    {
        GamePacket p;
        check(where_found(p).empty(), "a frame off the TCP stream needs no label");
        p.depth = 1;
        check(where_found(p) == "inside 0x0081", "one inside a wrapper is labelled so");
        p.depth = 2;
        check(where_found(p) == "inside 0x0081 x2", "and one inside two");
        p.udp = true;
        p.depth = 0;
        check(where_found(p) == "UDP", "one over UDP");
        p.depth = 1;
        check(where_found(p) == "UDP, inside 0x0081", "and one inside a wrapper over UDP");
    }
}

// ------------------------------------------------------------------- UDP

Bytes ethernet_ipv4(std::uint8_t proto, std::uint32_t src_ip, std::uint32_t dst_ip,
                    const Bytes& l4, std::uint16_t frag = 0x4000) {
    Bytes f(12, 0);                         // the two MAC addresses, unused
    f.push_back(0x08); f.push_back(0x00);   // IPv4
    auto be16 = [&f](std::uint32_t v) {
        f.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        f.push_back(static_cast<std::uint8_t>(v & 0xFF));
    };
    f.push_back(0x45); f.push_back(0);      // version 4, a 20-byte header
    be16(static_cast<std::uint32_t>(20 + l4.size()));
    be16(0x1234);                           // identification
    be16(frag);                             // 0x4000: don't fragment
    f.push_back(64); f.push_back(proto);
    be16(0);                                // header checksum: not checked
    be16(src_ip >> 16); be16(src_ip & 0xFFFF);
    be16(dst_ip >> 16); be16(dst_ip & 0xFFFF);
    f.insert(f.end(), l4.begin(), l4.end());
    return f;
}

Bytes udp_packet(std::uint32_t src_ip, std::uint16_t src_port,
                 std::uint32_t dst_ip, std::uint16_t dst_port,
                 const Bytes& payload, std::uint16_t frag = 0x4000) {
    Bytes u;
    for (std::uint32_t v : { std::uint32_t{ src_port }, std::uint32_t{ dst_port },
                             static_cast<std::uint32_t>(8 + payload.size()), std::uint32_t{ 0 } }) {
        u.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        u.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }
    u.insert(u.end(), payload.begin(), payload.end());
    return ethernet_ipv4(17, src_ip, dst_ip, u, frag);
}

Bytes tcp_packet(std::uint32_t src_ip, std::uint16_t src_port,
                 std::uint32_t dst_ip, std::uint16_t dst_port,
                 std::uint32_t seq, const Bytes& payload) {
    Bytes t;
    auto be16 = [&t](std::uint32_t v) {
        t.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        t.push_back(static_cast<std::uint8_t>(v & 0xFF));
    };
    be16(src_port); be16(dst_port);
    be16(seq >> 16); be16(seq & 0xFFFF);
    be16(0); be16(0);                       // acknowledgement number
    t.push_back(5 << 4);                    // a 20-byte header
    t.push_back(0x18);                      // PSH, ACK
    be16(65535); be16(0); be16(0);          // window, checksum, urgent
    t.insert(t.end(), payload.begin(), payload.end());
    return ethernet_ipv4(6, src_ip, dst_ip, t);
}

/// GoC captures UDP as well as TCP and frames each datagram the same way;
/// GoBot never captured UDP at all. A datagram holding frames must decode
/// exactly as the same bytes on the TCP stream, through the same capture code
/// a live run uses, and a recording must keep the two apart.
void test_udp_datagrams() {
    section("UDP datagrams, framed like the TCP stream");

    const FramingSpec spec = unwrapping_spec();
    const Bytes body38   = some_body(88, 3);
    const Bytes plain    = game_frame(0x0038, body38);
    const Bytes body27   = move_body();
    const Bytes w1       = wrapped(game_frame(0x0027, body27));
    const Bytes datagram = joined({ plain, w1 });

    // The framing itself.
    {
        const auto tcp = extract_frames(spec, datagram.data(), datagram.size(),
                                        Direction::ToClient, 5, false);
        const auto udp = extract_datagram(spec, datagram.data(), datagram.size(),
                                          Direction::ToClient, 5);
        check(udp.packets.size() == 3 && same_frames(tcp.packets, udp.packets),
              "a datagram holding a frame and a wrapped one decodes as the same bytes on the "
              "TCP stream: same opcodes, payloads and depths");
        bool marked = !udp.packets.empty();
        for (const auto& p : udp.packets) marked = marked && p.udp && !p.plain();
        for (const auto& p : tcp.packets) marked = marked && !p.udp;
        check(marked, "and its frames are marked as UDP");
        check(udp.resyncs == 0 && udp.consumed == datagram.size(),
              "with every byte accounted for");

        const std::size_t cut = datagram.size() - 5;
        const auto c = extract_datagram(spec, datagram.data(), cut, Direction::ToClient, 5);
        check(c.packets.size() == 1 && c.resyncs == w1.size() - 5 && c.consumed == cut,
              "a frame cut short at a datagram's end is counted unread, not kept for a next one");
    }

    constexpr std::uint32_t kServer = 0x0A000002, kClient = 0x0A000001;
    constexpr std::uint16_t kServerPort = 15101, kClientPort = 50000;

    // Through PacketCapture: Ethernet, IPv4, UDP, as the capture thread sees them.
    std::vector<GamePacket> over_udp;
    {
        PacketCapture cap;
        cap.set_framing(spec);
        cap.set_packet_callback([&](const std::vector<GamePacket>& v) {
            over_udp.insert(over_udp.end(), v.begin(), v.end());
        });
        const Bytes pkt = udp_packet(kServer, kServerPort, kClient, kClientPort, datagram);
        check(cap.replay(pkt.data(), pkt.size(), 1), "a UDP packet runs through the capture's own code");
        check(over_udp.size() == 3 && over_udp[0].udp && over_udp[0].from_server &&
                  over_udp[0].opcode == 0x0038 && over_udp[0].payload == body38 &&
                  over_udp[2].opcode == 0x0027 && over_udp[2].depth == 1 &&
                  over_udp[2].payload == body27,
              "and its frames come out, server to client, the wrapped one unwrapped");
        const auto s = cap.stats();
        check(s.udp_datagrams == 1 && s.udp_frames == 2 && s.unwrap.inner == 1 &&
                  s.udp_unread_bytes == 0 && s.frames_decoded == 0 && s.resync_bytes == 0,
              "counted as UDP, never as TCP frames or resyncs");

        // A piece of a datagram that IP split: counted, not read.
        const std::size_t before = over_udp.size();
        const Bytes piece = udp_packet(kServer, kServerPort, kClient, kClientPort, datagram, 0x2000);
        cap.replay(piece.data(), piece.size(), 2);
        check(over_udp.size() == before && cap.stats().udp_fragments == 1,
              "a fragment of a datagram is counted and not read as frames");
    }

    // The same bytes over TCP, in two segments cut inside the wrapped frame,
    // through the reassembler: the same frames, not marked UDP.
    {
        PacketCapture cap;
        cap.set_framing(spec);
        std::vector<GamePacket> got;
        cap.set_packet_callback([&](const std::vector<GamePacket>& v) {
            got.insert(got.end(), v.begin(), v.end());
        });
        const std::size_t cut = plain.size() + 20;
        const Bytes first(datagram.begin(), datagram.begin() + static_cast<std::ptrdiff_t>(cut));
        const Bytes second(datagram.begin() + static_cast<std::ptrdiff_t>(cut), datagram.end());
        const Bytes seg1 = tcp_packet(kServer, kServerPort, kClient, kClientPort, 1000, first);
        const Bytes seg2 = tcp_packet(kServer, kServerPort, kClient, kClientPort,
                                      1000 + static_cast<std::uint32_t>(cut), second);
        cap.replay(seg1.data(), seg1.size(), 1);
        check(got.size() == 1 && got[0].opcode == 0x0038,
              "over TCP, the first segment gives the frame before the cut wrapper");
        cap.replay(seg2.data(), seg2.size(), 2);
        bool tcp_marked = got.size() == 3;
        for (const auto& p : got) tcp_marked = tcp_marked && !p.udp;
        check(got.size() == 3 && got[2].opcode == 0x0027 && got[2].depth == 1 &&
                  got[2].payload == body27,
              "the second gives the wrapper and the frame inside it, whole");
        check(tcp_marked && same_frames(got, over_udp),
              "the same frames as the datagram gave, marked as TCP");
        const auto s = cap.stats();
        check(s.frames_decoded == 2 && s.unwrap.inner == 1 && s.resync_bytes == 0 &&
                  s.udp_datagrams == 0,
              "counted as two frames off the stream and one found inside");
    }

    // A recording keeps the two apart: each tap is given only its own bytes,
    // once and whole, with the direction the ports imply.
    {
        PacketCapture rec;
        rec.set_raw_only(true);
        Bytes tcp_seen, udp_seen;
        std::vector<Direction> udp_dirs;
        rec.set_raw_tap([&](const FlowKey&, Direction, const std::uint8_t* d, std::size_t n) {
            tcp_seen.insert(tcp_seen.end(), d, d + n);
        });
        rec.set_raw_udp_tap([&](const FlowKey&, Direction dir, const std::uint8_t* d, std::size_t n) {
            udp_seen.insert(udp_seen.end(), d, d + n);
            udp_dirs.push_back(dir);
        });
        const Bytes seg  = tcp_packet(kServer, kServerPort, kClient, kClientPort, 5000, plain);
        const Bytes down = udp_packet(kServer, kServerPort, kClient, kClientPort, w1);
        const Bytes up   = udp_packet(kClient, kClientPort, kServer, kServerPort, move_body());
        rec.replay(seg.data(), seg.size(), 1);
        rec.replay(down.data(), down.size(), 2);
        rec.replay(up.data(), up.size(), 3);
        check(tcp_seen == plain, "the TCP tap sees the TCP bytes and nothing else");
        check(udp_seen == joined({ w1, move_body() }),
              "the UDP tap sees each datagram whole and nothing of the TCP stream");
        check(udp_dirs.size() == 2 && udp_dirs[0] == Direction::ToClient &&
                  udp_dirs[1] == Direction::ToServer,
              "server-to-client and client-to-server datagrams are told apart, for the two files");
    }

    // The game's UDP to a host that is none of its servers -- a relay, another
    // player -- on ports the port rule reads backwards. This machine's own
    // address says which end is the game's, both ways round.
    constexpr std::uint32_t kPeer = 0xCB007132;              // 203.0.113.50
    const std::vector<Endpoint> servers = { Endpoint{ kServer, kServerPort } };
    const std::vector<std::uint32_t> here = { kClient };
    {
        struct Ports { std::uint16_t client, peer; const char* what; };
        for (const Ports& pp : { Ports{ 50123, 60000, "two high ports" },
                                 Ports{ 9000, 15101, "two low ports" } }) {
            const Endpoint client{ kClient, pp.client }, peer{ kPeer, pp.peer };
            bool up_guessed = true, down_guessed = true, old_guessed = false;
            const Direction up =
                PacketCapture::datagram_direction(client, peer, servers, here, &up_guessed);
            const Direction down =
                PacketCapture::datagram_direction(peer, client, servers, here, &down_guessed);
            const Direction by_ports =
                PacketCapture::datagram_direction(client, peer, servers, {}, &old_guessed);
            check(up == Direction::ToServer && down == Direction::ToClient && !up_guessed &&
                      !down_guessed,
                  std::string("to and from a host that is not the game's server, on ") + pp.what +
                      " with the client's the lower: this machine's address tells them apart");
            check(by_ports == Direction::ToClient && old_guessed,
                  std::string("where the port numbers alone read them backwards, on ") + pp.what +
                      ", and say that they guessed");
        }

        bool guessed = true;
        check(PacketCapture::datagram_direction(Endpoint{ kServer, 60000 }, Endpoint{ kPeer, 50123 },
                                                servers, here, &guessed) == Direction::ToClient &&
                  !guessed,
              "a known server settles it first, on any port, with this machine at neither end");
        bool both = false, neither = false;
        PacketCapture::datagram_direction(Endpoint{ kClient, 50123 }, Endpoint{ kClient, 15101 },
                                          {}, here, &both);
        PacketCapture::datagram_direction(Endpoint{ kPeer, 50123 }, Endpoint{ kPeer + 1, 15101 },
                                          {}, here, &neither);
        check(both && neither,
              "with this machine at both ends, or at neither, only the ports are left, and that "
              "is said");
    }

    // Through the capture's own code, as a recording files them: with this
    // machine's addresses, which start() reads and a replay is given, the two
    // go to the right files and nothing is guessed; without, the ports file
    // them the wrong way round, and each guess is counted.
    for (const bool told : { true, false }) {
        PacketCapture rec;
        rec.set_raw_only(true);
        const bool set = !told || rec.set_local_addresses({ "10.0.0.1", "not an address" });
        std::vector<Direction> dirs;
        rec.set_raw_udp_tap([&](const FlowKey&, Direction dir, const std::uint8_t*, std::size_t) {
            dirs.push_back(dir);
        });
        const Bytes up   = udp_packet(kClient, 50123, kPeer, 60000, move_body());
        const Bytes down = udp_packet(kPeer, 60000, kClient, 50123, move_body());
        rec.replay(up.data(), up.size(), 1);
        rec.replay(down.data(), down.size(), 2);
        const bool right = dirs.size() == 2 && dirs[0] == Direction::ToServer &&
                           dirs[1] == Direction::ToClient;
        if (told)
            check(set && right && rec.stats().udp_by_port == 0,
                  "replayed to and from that host, given this machine's address: the client's "
                  "datagram is filed client-to-server, the host's server-to-client, none guessed");
        else
            check(!right && dirs.size() == 2 && rec.stats().udp_by_port == 2,
                  "and without it, both are filed by port alone, the wrong way round, and counted");
    }
}

/// The client framing the shipped config carries: opcode at 6, and a length
/// that counts the body + 4.
FramingSpec client_spec() {
    FramingSpec client;
    client.header_size = 8;
    client.length_offset = 4; client.length_size = 2; client.length_le = true;
    client.length_includes_header = false; client.length_adjust = 4;
    client.opcode_offset = 6; client.opcode_size = 2; client.opcode_le = true;
    client.min_frame = 8; client.max_frame = 32768;
    return client;
}

/// The P0-2 recordings hold the client's datagrams in the server's layout,
/// tiling 100% so. Under [FramingClient] 1.8% of shop's bytes and 10.5% of
/// walk's are in a frame, and the fight's 79.6% is 7 false frames, one of
/// them 17,602 bytes. A live capture framed a client-to-server datagram with
/// [FramingClient] whenever the config had one, and so read none of it.
void test_udp_framing() {
    section("UDP framed in the server's layout both ways, never with [FramingClient]");

    const FramingSpec server = unwrapping_spec();
    const FramingSpec client = client_spec();
    const Bytes body27 = move_body();
    const Bytes body10d = some_body(44, 5);
    const Bytes up = joined({ game_frame(0x0027, body27), game_frame(0x010D, body10d) });
    constexpr std::uint32_t kServer = 0x0A000002, kClient = 0x0A000001;

    {
        PacketCapture cap;
        cap.set_framing(server);
        cap.set_client_framing(client);
        std::vector<GamePacket> got;
        cap.set_packet_callback([&](const std::vector<GamePacket>& v) {
            got.insert(got.end(), v.begin(), v.end());
        });
        const Bytes pkt = udp_packet(kClient, 50000, kServer, 15101, up);
        check(cap.replay(pkt.data(), pkt.size(), 1), "a client-to-server datagram runs through the capture");
        check(got.size() == 2 && got[0].opcode == 0x0027 && got[0].payload == body27 &&
                  got[1].opcode == 0x010D && got[1].payload == body10d,
              "and its frames, in the server's layout, decode with [Framing] though a client "
              "framing is set");
        check(got.size() == 2 && !got[0].from_server && got[0].udp && !got[1].from_server,
              "labelled client to server, over UDP");
        const auto s = cap.stats();
        check(s.udp_frames == 2 && s.udp_unread_bytes == 0 && s.resync_bytes == 0,
              "every byte of it read");
    }

    // A frame whose check word is off: [Framing] refuses it, and a
    // [FramingUdp] without a check word reads it -- both ways.
    FramingSpec loose = server;
    loose.checksum_offset = FramingSpec::kNoChecksum;
    Bytes off = game_frame(0x0027, body27);
    off[6] ^= 0x5A;
    {
        PacketCapture cap;
        cap.set_framing(server);
        cap.set_client_framing(client);
        std::vector<GamePacket> got;
        cap.set_packet_callback([&](const std::vector<GamePacket>& v) {
            got.insert(got.end(), v.begin(), v.end());
        });
        const Bytes pkt = udp_packet(kClient, 50000, kServer, 15101, off);
        cap.replay(pkt.data(), pkt.size(), 1);
        check(got.empty() && cap.stats().udp_unread_bytes == off.size(),
              "without a UDP framing, [Framing]'s check word refuses a frame that fails it");
    }
    {
        PacketCapture cap;
        cap.set_framing(server);
        cap.set_client_framing(client);
        cap.set_udp_framing(loose);
        std::vector<GamePacket> got;
        cap.set_packet_callback([&](const std::vector<GamePacket>& v) {
            got.insert(got.end(), v.begin(), v.end());
        });
        const Bytes to_server = udp_packet(kClient, 50000, kServer, 15101, off);
        const Bytes to_client = udp_packet(kServer, 15101, kClient, 50000, off);
        cap.replay(to_server.data(), to_server.size(), 1);
        cap.replay(to_client.data(), to_client.size(), 2);
        check(got.size() == 2 && got[0].opcode == 0x0027 && !got[0].from_server &&
                  got[1].opcode == 0x0027 && got[1].from_server && got[1].payload == body27,
              "set_udp_framing frames datagrams both ways, the client's included");
    }

    // [FramingUdp] in a config, and without it.
    {
        const std::string path = temp_file("udp_framing.ini");
        {
            std::ofstream out(path, std::ios::binary);
            out << server.to_ini();
        }
        bool have = false, found = true;
        const FramingSpec s = load_framing(path, have);
        const FramingSpec u = load_udp_framing(path, s, found);
        check(have && !found && u.opcode_offset == 2 && u.checksum_offset == 6 &&
                  u.header_size == s.header_size && u.trailer_bytes == 12,
              "a config with no [FramingUdp] frames UDP with [Framing]");

        std::string block = loose.to_ini();
        block.replace(0, std::strlen("[Framing]"), "[FramingUdp]");
        {
            std::ofstream out(path, std::ios::binary | std::ios::app);
            out << "\n" << block;
        }
        const FramingSpec u2 = load_udp_framing(path, s, found);
        check(found && !u2.has_checksum() && u2.opcode_offset == 2,
              "and one with a [FramingUdp] block frames UDP with that");
        std::remove(path.c_str());
    }

    // What a datagram leaves unread is placed, as well as counted.
    {
        const Bytes d = joined({ game_frame(0x0027, body27), game_frame(0x010D, body10d) });
        const std::size_t cut = d.size() - 5;
        const auto r = extract_datagram(server, d.data(), cut, Direction::ToClient, 0);
        const std::size_t first = 8 + body27.size();
        check(r.packets.size() == 1 && r.resyncs == cut - first && r.skipped.size() == 1 &&
                  r.skipped[0].offset == first && r.skipped[0].length == cut - first,
              "a datagram's frame cut short is in one unread run, from where it starts");
    }
}

/// Your own position, as the P0-2 recordings show the client sending it: over
/// UDP, in the server's layout, at the frame bytes docs/protocol-findings.md §4e
/// gives. Hand-built frames whose answer is known; the id, the sequence number
/// and the values are made up, not taken from a recording.
void test_own_position() {
    section("Your own position, read from the client's UDP reports");

    // A frame of `size` bytes, header included, its first word 0x0105 as the
    // game's frames have it. Fields are written at frame bytes.
    struct Frame {
        std::uint16_t opcode;
        Bytes         body;
        Frame(std::uint16_t op, std::size_t size) : opcode(op), body(size - 8, 0) {}
        void u8(std::size_t at, std::uint8_t v)   { body[at - 8] = v; }
        void u16(std::size_t at, std::uint16_t v) { std::memcpy(body.data() + at - 8, &v, 2); }
        void u32(std::size_t at, std::uint32_t v) { std::memcpy(body.data() + at - 8, &v, 4); }
        void f32(std::size_t at, float v)         { std::memcpy(body.data() + at - 8, &v, 4); }
        Bytes bytes() const { return game_frame(opcode, body, 0x0105); }
    };
    // As the client sent it, read straight off a datagram.
    auto sent = [](const Frame& f) {
        GamePacket p;
        p.opcode = f.opcode;
        p.payload = f.body;
        p.from_server = false;
        p.udp = true;
        return p;
    };
    constexpr std::uint32_t kId = 4321;

    Frame move(0x0027, 48);
    move.u32(8, kId); move.u32(12, kId);
    move.f32(16, 3288.0f); move.f32(20, 1155.4f); move.f32(24, 9.0f); move.u8(28, 0xFF);

    Frame air(0x002A, 40);
    air.u32(8, kId); air.u32(12, kId); air.u32(16, 1);
    air.f32(20, 4335.4f); air.f32(24, 1210.0f); air.f32(28, 9.0f); air.f32(32, -20.0f);

    Frame other(0x0029, 44);
    other.u32(8, kId); other.u32(12, kId); other.u32(16, 1);
    other.f32(20, 9.0f); other.f32(24, 3805.9f); other.f32(28, 1399.4f);

    Frame here(0x0028, 36);
    here.u32(8, 1000008); here.u32(12, kId); here.u32(16, kId);
    here.f32(20, 4690.0f); here.f32(24, 1180.0f); here.f32(28, 0.0f); here.u32(32, 0x024600FFu);

    Frame cast(0x0031, 92);
    cast.u32(8, kId); cast.u32(12, kId);
    cast.u16(16, 4216); cast.u16(18, 1267); cast.u32(20, 51102);

    Frame pet(0x010D, 52);      // the pet's position, not yours
    pet.u32(8, kId); pet.u32(12, kId); pet.f32(24, 2660.0f); pet.f32(28, 1070.0f);
    Frame hop(0x0117, 28);      // the pet's teleport
    hop.u32(8, kId); hop.u32(12, kId); hop.f32(16, 2660.0f); hop.f32(20, 1070.0f);

    // One datagram from the client, framed as the capture frames it.
    const Bytes datagram = joined({ move.bytes(), air.bytes(), other.bytes(), here.bytes(),
                                    cast.bytes(), pet.bytes(), hop.bytes() });
    const auto up = extract_datagram(unwrapping_spec(), datagram.data(), datagram.size(),
                                     Direction::ToServer, 0);
    check(up.packets.size() == 7 && up.resyncs == 0,
          "seven frames off one client datagram, every byte read");
    if (up.packets.size() != 7) return;
    std::vector<std::optional<OwnPosition>> got;
    for (const GamePacket& p : up.packets) got.push_back(read_own_position(p));

    const auto& m = got[0];
    check(m && m->kind == OwnReport::Move && m->id == kId && m->x == 3288.0f && m->y == 1155.4f &&
              m->speed == 9.0f && m->facing == -1 && !m->air_state && !m->vertical_speed,
          "0x0027, a move: x, y at 16 and 20, speed at 24, and facing byte 0xFF at 28, "
          "toward smaller x");
    const auto& a = got[1];
    check(a && a->kind == OwnReport::Air && a->id == kId && a->x == 4335.4f && a->y == 1210.0f &&
              a->speed == 9.0f && a->air_state == 1u && a->vertical_speed == -20.0f &&
              a->facing == 0,
          "0x002A, in the air: state at 16, x, y at 20 and 24, speed at 28, vertical speed at 32, "
          "and no facing");
    const auto& o = got[2];
    check(o && o->kind == OwnReport::Other && o->x == 3805.9f && o->y == 1399.4f &&
              o->speed == 9.0f && o->facing == 1,
          "0x0029: facing at 16 (1, toward larger x), speed at 20, x, y at 24 and 28");
    const auto& h = got[3];
    check(h && h->kind == OwnReport::Here && h->id == kId && h->x == 4690.0f && h->y == 1180.0f &&
              h->speed == 0.0f && h->facing == -1,
          "0x0028, at a map entry: the id from 12, not the sequence number at 8; x, y at 20 "
          "and 24; the facing in the low byte of the flags at 32");
    const auto& c = got[4];
    check(c && c->kind == OwnReport::Cast && c->id == kId && c->x == 4216.0f && c->y == 1267.0f &&
              !c->speed && c->facing == 0,
          "0x0031, a skill cast: the caster's x, y as u16 at 16 and 18, with no speed or facing");
    check(!got[5] && !got[6], "0x010D and 0x0117, the pet's position and its teleport, are not yours");

    // The server sends these layouts for other actors' moves.
    const auto down = extract_datagram(unwrapping_spec(), datagram.data(), datagram.size(),
                                       Direction::ToClient, 0);
    bool none = down.packets.size() == 7;
    for (const GamePacket& p : down.packets) none = none && !read_own_position(p);
    check(none, "the same frames from the server are another actor's moves, never yours");

    GamePacket tcp = sent(move);
    tcp.udp = false;
    check(!read_own_position(tcp), "nor a move off the TCP stream");
    GamePacket inner = sent(move);
    inner.depth = 1;
    check(!read_own_position(inner), "nor one found inside a wrapper");

    // GoC's client wants 29 bytes of an 0x0027 and 36 of an 0x002A: up to the
    // facing byte and to the end of the vertical speed. One byte less is short.
    {
        Frame least(0x0027, 29);
        least.f32(16, 100.0f); least.f32(20, 200.0f); least.u8(28, 0x01);
        GamePacket p = sent(least);
        const auto r = read_own_position(p);
        check(r && r->x == 100.0f && r->y == 200.0f && r->facing == 1,
              "a 29-byte 0x0027, the least GoC's client reads, gives its position");
        p.payload.pop_back();
        check(!read_own_position(p), "one byte shorter gives nothing");

        Frame jump(0x002A, 36);
        jump.f32(20, 300.0f); jump.f32(24, 400.0f); jump.f32(32, -36.0f);
        GamePacket q = sent(jump);
        const auto s = read_own_position(q);
        check(s && s->x == 300.0f && s->vertical_speed == -36.0f,
              "a 36-byte 0x002A, the least GoC's client reads, gives its position");
        q.payload.pop_back();
        check(!read_own_position(q), "and one byte shorter gives nothing");
    }

    // Values that cannot be a position, and a facing byte that says neither.
    {
        Frame lost = move;
        lost.f32(16, std::nanf(""));
        check(!read_own_position(sent(lost)), "an x that is not a number gives nothing");

        Frame odd = move;
        odd.u8(28, 0x07);
        const auto r = read_own_position(sent(odd));
        check(r && r->facing == 0 && r->x == 3288.0f,
              "a facing byte other than 0x01 or 0xFF leaves the facing unknown");
    }
}

/// The disconnect watchdog stops the bot after 15 s of silence from the game.
/// Now that the filter lets UDP in as well, from the same host and port, a
/// datagram must not count as the session being alive: the session is the
/// TCP connection, and its silence is what the watchdog waits for.
void test_watchdog_clock() {
    section("The disconnect watchdog goes by the TCP connection alone");

    constexpr std::uint32_t kServer = 0x0A000002, kClient = 0x0A000001;
    constexpr std::uint16_t kServerPort = 15101, kClientPort = 50000;
    const Bytes frame = game_frame(0x0038, some_body(88, 3));

    PacketCapture cap;
    cap.set_framing(unwrapping_spec());
    const Bytes datagram = udp_packet(kServer, kServerPort, kClient, kClientPort, frame);
    cap.replay(datagram.data(), datagram.size(), 1000);
    auto s = cap.stats();
    check(s.last_packet_ms == 1000 && s.last_tcp_ms == 0,
          "a datagram is a packet seen, and leaves the watchdog's clock where it was");
    check(PacketCapture::silent_for_ms(s, 60'000) == 0,
          "and with no segment yet there is no silence for the watchdog to act on");

    const Bytes segment = tcp_packet(kServer, kServerPort, kClient, kClientPort, 7000, frame);
    cap.replay(segment.data(), segment.size(), 2000);
    cap.replay(datagram.data(), datagram.size(), 3000);
    s = cap.stats();
    check(s.last_tcp_ms == 2000 && s.last_packet_ms == 3000,
          "a TCP segment sets it, and UDP arriving after the connection went quiet does not move "
          "it on");

    // What the watchdog itself decides by. A datagram a second ago changes
    // nothing: 16 s after the last segment the session has been silent for
    // 16 s, past the watchdog's limit, and it reports the game gone.
    cap.replay(datagram.data(), datagram.size(), 17'000);
    s = cap.stats();
    const std::uint64_t silent = PacketCapture::silent_for_ms(s, 18'001);
    check(s.last_packet_ms == 17'000 && silent == 16'001 &&
              silent > PacketCapture::kDisconnectSilenceMs,
          "16 s after the last segment the watchdog reads 16 s of silence, past its 15, though a "
          "datagram came 1 s ago");
    check(PacketCapture::silent_for_ms(s, 2'500) == 500 &&
              PacketCapture::silent_for_ms(s, 2'500) <= PacketCapture::kDisconnectSilenceMs,
          "while half a second after it, it reads half a second, and lets the bot go on");
}

// ------------------------------------------------------- capture filter

void test_capture_filters() {
    section("The capture filter takes UDP as well as TCP");

    CaptureConfig none;
    check(PacketCapture::build_filter(none) == "(tcp or udp)",
          "with nothing named, TCP and UDP both");

    CaptureConfig game;
    game.server_ip_start = game.server_ip_end = "203.0.113.10";
    game.port_start = game.port_end = 15101;
    check(PacketCapture::build_filter(game) ==
              "(tcp or udp) and host 203.0.113.10 and port 15101",
          "on the game server's address and port: " + PacketCapture::build_filter(game));

    CaptureConfig range = game;
    range.port_end = 15112;
    check(PacketCapture::build_filter(range) ==
              "(tcp or udp) and host 203.0.113.10 and portrange 15101-15112",
          "and on a port range, as GoC's \"(tcp or udp) and portrange\" does");

    CaptureConfig own = game;
    own.bpf_filter = "tcp and port 15101";
    check(PacketCapture::build_filter(own) == "tcp and port 15101",
          "an explicit filter (--filter, bpf_filter) is used exactly as given, as before");

    ProcessConnection c;
    c.remote.ip = 0xCB00710A;            // 203.0.113.10
    c.remote.port = 15101;
    check(build_filter_for({ c }) ==
              "((tcp or udp) and ((host 203.0.113.10 and port 15101))) or (udp and (port 15101))",
          "a connection found by --process is captured over TCP and UDP, and UDP on its port from "
          "any host, as GoC's own \"(tcp or udp) and port\" takes it");
    check(build_filter_for({ c }, { 50123 }) ==
              "((tcp or udp) and ((host 203.0.113.10 and port 15101))) or "
              "(udp and (port 15101 or port 50123))",
          "and the game's own UDP sockets by their port: " + build_filter_for({ c }, { 50123 }));
    ProcessConnection d = c;
    d.remote.ip = 0xCB00710B;            // 203.0.113.11
    d.remote.port = 15112;
    ProcessConnection e = c;
    e.remote.ip = 0xCB00710C;            // 203.0.113.12, on the first one's port
    check(build_filter_for({ c, d, e }, { 15101 }) ==
              "((tcp or udp) and ((host 203.0.113.10 and port 15101) or (host 203.0.113.11 and "
              "port 15112) or (host 203.0.113.12 and port 15101))) or "
              "(udp and (port 15101 or port 15112))",
          "each port once, however many servers and sockets share it: " +
              build_filter_for({ c, d, e }, { 15101 }));
    check(build_filter_for({}, { 50123 }) == "udp and (port 50123)",
          "the game's own UDP sockets alone, with no connection");
    check(build_filter_for({}) == "(tcp or udp)", "with no connection at all, TCP and UDP");

    // What gobot_sniff --record refuses: a capture that nothing narrows,
    // which would write every program's traffic on the machine to the files.
    CaptureConfig port_only;
    port_only.port_start = port_only.port_end = 15101;
    CaptureConfig not_an_address;
    not_an_address.server_ip_start = not_an_address.server_ip_end = "game.example";
    CaptureConfig found;
    found.bpf_filter = build_filter_for({ c }, { 50123 });
    check(PacketCapture::captures_everything(none),
          "with nothing named, a capture takes every flow on the machine: --record refuses it");
    check(!PacketCapture::captures_everything(game) &&
              !PacketCapture::captures_everything(port_only) &&
              !PacketCapture::captures_everything(own) && !PacketCapture::captures_everything(found),
          "an address, a port, a filter of one's own or the game's own sockets narrow it");
    check(PacketCapture::captures_everything(not_an_address),
          "and a --host that is not an IPv4 address narrows nothing");
}

/// --process picks sockets by their owner. Asked as Game.exe -- what a great
/// many programs are called -- it must take the game's alone: another program
/// of that name running beside it would put its traffic into a recording of
/// the game's.
void test_socket_owners() {
    section("--process Game.exe takes the game's sockets, by where it runs from");

    const std::string game_exe = "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe";
    GameInstall install;
    install.programs = { game_exe };
    GameInstall::Drives drives;          // no disk to ask: the paths' text decides

    // The processes owning sockets on a pretend machine, and how often each
    // was looked up.
    const std::map<std::uint32_t, SocketOwners::Owner> machine = {
        { 1001, { "Game.exe", "C:\\Program Files\\Some Game\\Game.exe" } },
        { 1002, { "Game.exe", game_exe } },
        { 1003, { "Game.exe", "" } },
        { 1004, { "GAME.EXE", "c:\\vtczone\\games\\ghost_online_pc_vtc\\GAME.EXE" } },
        { 1005, { "chrome.exe", "C:\\Program Files\\Google\\Chrome\\chrome.exe" } },
        { 1006, { "GhostOnlineVTC.exe",
                  "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\GhostOnlineVTC.exe" } },
        { 1007, { "GhostClient.exe", "D:\\Games\\Ghost\\GhostClient.exe" } },
        { 1008, { "GhostClient.exe", "E:\\Copy\\GhostClient.exe" } },
    };
    std::map<std::uint32_t, int> asked;
    const SocketOwners::LookUp look_up = [&](std::uint32_t pid) {
        ++asked[pid];
        const auto it = machine.find(pid);
        return it == machine.end() ? SocketOwners::Owner{} : it->second;
    };
    const auto taken_by = [&](SocketOwners& owners) {
        std::vector<std::uint32_t> out;
        for (const auto& entry : machine)
            if (owners.takes(entry.first)) out.push_back(entry.first);
        return out;
    };

    SocketOwners game("Game.exe", look_up, install, drives);
    check(taken_by(game) == std::vector<std::uint32_t>{ 1002, 1004 },
          "the game's own Game.exe is taken, whatever the case, and no other: not one elsewhere, "
          "nor one whose path cannot be read");
    check(game.passed_over() == 2, "and the two of its name left out are counted, to be said");
    for (int i = 0; i < 3; ++i) {
        game.takes(1001);
        game.takes(1002);
    }
    bool once = true;
    for (const auto& entry : machine) once = once && asked[entry.first] == 1;
    check(once && game.passed_over() == 2,
          "each process is looked up once, however many sockets it holds");

    SocketOwners other("chrome.exe", look_up, install, drives);
    check(taken_by(other) == std::vector<std::uint32_t>{ 1005 } && other.passed_over() == 0,
          "any other name is matched by name alone, as before");

    SocketOwners nowhere("Game.exe", look_up, GameInstall{}, drives);
    check(taken_by(nowhere).empty(),
          "with nowhere known for the game, no Game.exe is taken for it");

    GameInstall renamed;
    renamed.programs = { "D:\\Games\\Ghost\\GhostClient.exe" };
    SocketOwners renamed_game("ghostclient.exe", look_up, renamed, drives);
    check(taken_by(renamed_game) == std::vector<std::uint32_t>{ 1007 },
          "and a game installed under another name is taken by that name, by where it runs from "
          "too");
}

// ------------------------------------------------- the config, old and new

/// The player's own protocol.ini is never replaced by a build, and it was
/// written before inner_frame_offset existed: its absence must mean 13.
void test_framing_config() {
    section("Reading [Framing], with and without inner_frame_offset");

    const char* old_block =
        "[Framing]\n"
        "verified=true\n"
        "header_size=8\n"
        "length_offset=4\n"
        "length_size=2\n"
        "length_endian=little\n"
        "length_includes_header=true\n"
        "length_adjust=0\n"
        "opcode_offset=2\n"
        "opcode_size=2\n"
        "opcode_endian=little\n"
        "min_frame=8\n"
        "max_frame=16384\n"
        "checksum_offset=6\n"
        "trailer_opcode=0x0081\n"
        "trailer_bytes=12\n";

    const std::string path = temp_file("protocol_before_p01.ini");
    {
        std::ofstream out(path, std::ios::binary);
        out << old_block;
    }
    bool found = false;
    const FramingSpec spec = load_framing(path, found);
    check(found && spec.trailer_opcode == 0x0081 && spec.trailer_bytes == 12 &&
              spec.checksum_offset == 6,
          "a [Framing] block from before 2026-10-18 reads as it did");
    check(spec.inner_offset == 13 && spec.unwraps(),
          "and, with no inner_frame_offset in it, unwraps from byte 13");
    {
        const Bytes w = wrapped(game_frame(0x0027, move_body()));
        const auto scan = extract_frames(spec, w.data(), w.size(), Direction::ToClient, 0, false);
        check(scan.packets.size() == 2 && scan.packets[1].opcode == 0x0027,
              "so the player's own config finds the frame inside 0x0081");
    }

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << old_block << "inner_frame_offset=0\n";
    }
    const FramingSpec off = load_framing(path, found);
    check(found && off.inner_offset == 0 && !off.unwraps(), "inner_frame_offset=0 turns it off");

    // What to_ini() writes reads back the same.
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << unwrapping_spec().to_ini();
    }
    const FramingSpec again = load_framing(path, found);
    check(found && again.inner_offset == 13 && again.unwraps() && again.trailer_opcode == 0x0081,
          "a spec written by to_ini() reads back with its inner offset");

    bool client_found = true;
    load_framing(path, client_found, "FramingClient");
    check(!client_found, "a missing section is reported missing");
    std::remove(path.c_str());

#ifdef GOBOT_SOURCE_DIR
    const std::string shipped = std::string(GOBOT_SOURCE_DIR) + "/configs/protocol.ini";
    IniFile ini;
    if (ini.load(shipped)) {
        const FramingSpec s = load_framing(shipped, found);
        check(ini.has_key("Framing", "inner_frame_offset") && s.inner_offset == 13 && s.unwraps(),
              "the shipped protocol.ini says inner_frame_offset=13 itself");
        bool c_found = false;
        const FramingSpec c = load_framing(shipped, c_found, "FramingClient");
        check(c_found && !c.unwraps(), "and the client's half, which has no wrapper, unwraps nothing");
    } else {
        std::printf("  [skip] the source tree's configs/protocol.ini is not here\n");
    }
#endif
}

// --------------------------------------------------- recordings, inside

void test_capture_file_inner() {
    section("Reading the frames inside 0x0081 out of a recording");

    const FramingSpec server = unwrapping_spec();
    FramingSpec client;
    client.header_size = 8;
    client.length_offset = 4; client.length_size = 2; client.length_le = true;
    client.length_includes_header = false; client.length_adjust = 4;
    client.opcode_offset = 6; client.opcode_size = 2; client.opcode_le = true;
    client.min_frame = 8; client.max_frame = 32768;

    const Bytes on_stream = some_body(21, 0x10);
    const Bytes inside    = move_body();
    const Bytes rec = joined({ game_frame(0x0027, on_stream), wrapped(game_frame(0x0027, inside)),
                               game_frame(0x0027, on_stream), wrapped(game_frame(0x0027, inside)),
                               wrapped(game_frame(0x0027, inside)) });
    const std::string path = temp_file("inner_test.s2c");
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(rec.data()), static_cast<std::streamsize>(rec.size()));
    }

    std::vector<Bytes> got;
    const capture_file::Reading plain_read =
        capture_file::extract(path, server, client, 0x0027, got);
    check(plain_read.ok && got.size() == 2 && got[0] == on_stream,
          "by default, only the frames read straight off the stream, as before");
    check(plain_read.frames == 5 && plain_read.inner == 3 && plain_read.other_kind == 3,
          "with the frames inside counted, and those of the opcode left out said so");

    got.clear();
    const capture_file::Reading inner_read =
        capture_file::extract(path, server, client, 0x0027, got, true);
    check(inner_read.ok && got.size() == 3 && got[0] == inside && inner_read.other_kind == 2,
          "with inner, only the frames found inside 0x0081, never the two mixed");

    // What --verify reads, and the real-capture test below.
    Bytes data;
    {
        std::ifstream in(path, std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const FrameScanResult t = capture_file::tile(data, server);
    check(t.outer_frames() == 5 && t.unwrap.inner == 3 && t.resyncs == 0 && t.consumed == rec.size(),
          "tiling the recording: 5 frames off the stream, 3 inside, no resync");
    const FrameScanResult as_client = capture_file::tile(data, server, Direction::ToServer);
    bool labelled = as_client.packets.size() == t.packets.size() && !t.packets.empty();
    for (std::size_t i = 0; labelled && i < t.packets.size(); ++i)
        labelled = t.packets[i].from_server && !as_client.packets[i].from_server &&
                   as_client.packets[i].payload == t.packets[i].payload;
    check(labelled, "the direction --verify gives a client half only labels the frames it reads");

    // Every frame at once, as the --from commands read it. A recording of UDP
    // -- <name>.udp.s2c, the datagrams one after another -- reads the same,
    // each frame marked as having come over UDP.
    {
        const std::string udp_path = temp_file("inner_test.udp.s2c");
        {
            std::ofstream out(udp_path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(rec.data()), static_cast<std::streamsize>(rec.size()));
        }
        std::vector<GamePacket> over_tcp, over_udp;
        const capture_file::Reading rt = capture_file::read_all(path, server, client, over_tcp);
        const capture_file::Reading ru = capture_file::read_all(udp_path, server, client, over_udp);
        bool tcp_plain = over_tcp.size() == 8, udp_marked = over_udp.size() == 8;
        std::size_t unwrapped = 0;
        for (const auto& p : over_tcp) tcp_plain = tcp_plain && !p.udp;
        for (const auto& p : over_udp) {
            udp_marked = udp_marked && p.udp;
            if (p.depth == 1 && p.opcode == 0x0027 && p.payload == inside) ++unwrapped;
        }
        check(rt.ok && ru.ok && rt.half == capture_file::Half::Server && tcp_plain && udp_marked &&
                  unwrapped == 3,
              "every frame of a recording at once, wrappers' insides after them; a .udp.s2c one's "
              "marked as come over UDP");
        check(capture_file::udp_from_name("walk.udp.c2s") &&
                  capture_file::udp_from_name("WALK.UDP.S2C") &&
                  !capture_file::udp_from_name("walk.s2c") &&
                  !capture_file::udp_from_name("walk.flow0.s2c"),
              "and only a recording named .udp.s2c or .udp.c2s holds UDP");
        std::remove(udp_path.c_str());
    }
    std::remove(path.c_str());
}

// ---------------------------------------------- the world model holds back

void test_protocol_handler_holds_back() {
    section("The world model leaves frames from inside 0x0081 and UDP alone");

    OpcodeRegistry reg;
    PacketSpec mv;
    mv.opcode = 0x0201;
    mv.semantic = Semantic::SelfMove;
    mv.fields = OpcodeRegistry::parse_field_list("id:u32,x:f32,y:f32", nullptr);
    reg.add(mv);

    GameState st;
    ProtocolHandler ph(reg, st);

    auto move_to = [](float x, float y) {
        GamePacket p;
        p.opcode = 0x0201;
        p.payload.assign(12, 0);
        p.payload[0] = 0x2A;
        std::memcpy(p.payload.data() + 4, &x, 4);
        std::memcpy(p.payload.data() + 8, &y, 4);
        return p;
    };

    const Vec2 start = st.self().pos;
    GamePacket inner = move_to(100.0f, 100.0f);
    inner.depth = 1;
    GamePacket udp = move_to(200.0f, 200.0f);
    udp.udp = true;
    ph.on_packet(inner);
    ph.on_packet(udp);
    check(st.self().pos.x == start.x && st.self().pos.y == start.y,
          "a mapped frame from inside 0x0081, or over UDP, moves nothing");

    ph.on_packet(move_to(640.0f, 480.0f));
    check(std::abs(st.self().pos.x - 640.0f) < 0.01f, "the same frame off the TCP stream still does");

    const auto s = ph.stats();
    check(s.held_back == 2 && s.decoded == 1 && s.unknown == 0 && s.malformed == 0,
          "the two held back are counted as such, not as unknown or malformed");
}

// ------------------------------------------------------ a real capture

/// A capture taken from the game still tiles, read the way gobot_sniff
/// --verify reads it. Real captures stay out of the repository -- they can
/// hold a character's name -- so the file is named by GOBOT_TEST_CAPTURE.
void test_real_capture() {
    section("A real capture still tiles (GOBOT_TEST_CAPTURE)");

    wchar_t buf[1024]{};
    const DWORD n = GetEnvironmentVariableW(L"GOBOT_TEST_CAPTURE", buf,
                                            static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) {
        std::printf("  [skip] SKIPPED: GOBOT_TEST_CAPTURE is not set, so there is no "
                    "real capture to tile\n");
        return;
    }
    const std::string path = wide_to_utf8(std::wstring(buf, n));

    // The shipped config, as package.ps1 ships it; the copy beside this test
    // when the source tree is not here.
    std::string config = exe_relative("configs/protocol.ini");
#ifdef GOBOT_SOURCE_DIR
    if (path_exists(std::string(GOBOT_SOURCE_DIR) + "/configs/protocol.ini"))
        config = std::string(GOBOT_SOURCE_DIR) + "/configs/protocol.ini";
#endif
    bool found = false;
    const FramingSpec spec = load_framing(config, found);
    check(found, "[Framing] read from " + config);

    Bytes data;
    {
        std::ifstream in(utf8_to_wide(path).c_str(), std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    check(!data.empty(), "the capture is read: " + path);
    if (data.empty() || !found) return;

    const FrameScanResult scan = capture_file::tile(data, spec);
    std::printf("         %zu bytes: %zu frames off the stream, %zu inside 0x0081, "
                "%zu consumed, %zu resync bytes\n",
                data.size(), scan.outer_frames(), scan.unwrap.inner, scan.consumed, scan.resyncs);
    check(scan.outer_frames() > 0, "it holds frames");
    check(scan.resyncs == 0, "and tiles with 0 resyncs");

    FramingSpec off = spec;
    off.inner_offset = 0;
    const FrameScanResult before = capture_file::tile(data, off);
    std::vector<GamePacket> outer;
    for (const auto& p : scan.packets)
        if (p.depth == 0) outer.push_back(p);
    check(same_frames(before.packets, outer) && before.consumed == scan.consumed &&
              before.resyncs == scan.resyncs,
          "reading the same frames off the stream as it did with unwrapping off");
}

/// Each direction needs its own framing, and the shipped config must carry both.
///
/// The two halves are framed differently -- opcode at offset 2 coming down, 6
/// going up -- so a capture told only about [Framing] decodes the server and
/// grinds the client into resync noise. Every tool but two was doing exactly
/// that, and it showed up as 44,413 resyncs against 505 frames in --status
/// while the same traffic parsed with zero resyncs offline.
void test_both_framings_configured() {
    section("Both directions are framed");

    IniFile ini;
    const std::string path = exe_relative("configs/protocol.ini");
    check(ini.load(path), "the shipped protocol config loads");
    check(ini.has_section("Framing"), "it has [Framing] for the server's half");
    check(ini.has_section("FramingClient"),
          "and [FramingClient] for the client's half -- without it that whole "
          "direction is discarded, including the movement commands");

    // The two must actually differ, or one of them is a copy-paste.
    const int s_op = ini.get_int("Framing", "opcode_offset", -1);
    const int c_op = ini.get_int("FramingClient", "opcode_offset", -1);
    check(s_op >= 0 && c_op >= 0, "both declare an opcode offset");
    check(s_op != c_op,
          "and they differ -- if they matched, one spec would do and this "
          "whole class of bug could not arise");

    // A client frame must not parse under the server's spec, which is the
    // property that makes using the wrong one so destructive.
    FramingSpec server, client;
    server.header_size = 8; server.length_offset = 4; server.length_size = 2;
    server.length_le = true; server.length_includes_header = true;
    server.opcode_offset = 2; server.opcode_size = 2; server.opcode_le = true;
    server.min_frame = 8; server.max_frame = 16384; server.checksum_offset = 6;

    client.header_size = 8; client.length_offset = 4; client.length_size = 2;
    client.length_le = true; client.length_includes_header = false;
    client.length_adjust = 4;
    client.opcode_offset = 6; client.opcode_size = 2; client.opcode_le = true;
    client.min_frame = 8; client.max_frame = 32768;

    // One client frame: 8-byte header, 40-byte body, opcode 0x00C5 at offset 6.
    std::vector<std::uint8_t> frame(48, 0);
    frame[4] = 36; frame[5] = 0;            // length: body + adjust
    frame[6] = 0xC5; frame[7] = 0x00;       // opcode
    for (std::size_t i = 8; i < frame.size(); ++i)
        frame[i] = static_cast<std::uint8_t>(i * 7);

    const auto right = extract_frames(client, frame.data(), frame.size(),
                                      Direction::ToServer, 0, false);
    check(right.packets.size() == 1 && right.packets[0].opcode == 0x00C5,
          "the client spec reads a client frame");
    check(right.resyncs == 0, "with no resyncs");

    const auto wrong = extract_frames(server, frame.data(), frame.size(),
                                      Direction::ToServer, 0, false);
    check(wrong.packets.empty() || wrong.packets[0].opcode != 0x00C5,
          "the server spec does NOT read it correctly");
    std::printf("         same 48 bytes: client spec %zu frame(s)/%zu resyncs, "
                "server spec %zu/%zu\n",
                right.packets.size(), right.resyncs,
                wrong.packets.size(), wrong.resyncs);
}

// --------------------------------------------------------- capture files

/// Reading back a recording, and getting its direction right.
///
/// This exists because the first version got it wrong in a way that produced
/// no error at all. It chose between the two framings on resync count, and
/// both tile a short client capture with zero resyncs -- the scanner stops at
/// the first header it cannot use rather than running to the end of the file,
/// so "no resyncs" can mean "read two frames and gave up". The tie went to
/// whichever was tried first and client recordings were read as server ones,
/// silently, with plausible output.
void test_capture_file() {
    section("Reading a recorded capture");

    FramingSpec server;
    server.header_size = 8;
    server.length_offset = 4; server.length_size = 2; server.length_le = true;
    server.length_includes_header = true;
    server.opcode_offset = 2; server.opcode_size = 2; server.opcode_le = true;
    server.min_frame = 8; server.max_frame = 16384; server.checksum_offset = 6;

    FramingSpec client;
    client.header_size = 8;
    client.length_offset = 4; client.length_size = 2; client.length_le = true;
    client.length_includes_header = false; client.length_adjust = 4;
    client.opcode_offset = 6; client.opcode_size = 2; client.opcode_le = true;
    client.min_frame = 8; client.max_frame = 32768;

    // The name decides the direction when it says one.
    {
        capture_file::Half h{};
        check(capture_file::half_from_name("walk.c2s", h) &&
              h == capture_file::Half::Client, "a .c2s name reads as the client half");
        check(capture_file::half_from_name("probe.flow0.s2c", h) &&
              h == capture_file::Half::Server, "and .flow0.s2c as the server half");
        check(!capture_file::half_from_name("capture.bin", h),
              "a name that says nothing is left to the scan");
    }

    // A client recording: 8-byte header, opcode at offset 6, body of 40.
    std::vector<std::uint8_t> c2s;
    for (int i = 0; i < 6; ++i) {
        const std::uint16_t body = 36;              // length + adjust
        c2s.push_back(static_cast<std::uint8_t>(i));   // sequence
        c2s.push_back(0);
        c2s.push_back(0); c2s.push_back(0);
        c2s.push_back(static_cast<std::uint8_t>(body & 0xFF));
        c2s.push_back(static_cast<std::uint8_t>(body >> 8));
        c2s.push_back(0xC5); c2s.push_back(0x00);   // opcode 0x00C5 at offset 6
        for (int b = 8; b < 48; ++b)
            c2s.push_back(static_cast<std::uint8_t>(b * 3 + i));
    }

    const std::string path = temp_file("gobot_capture_test.c2s");
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(c2s.data()),
                  static_cast<std::streamsize>(c2s.size()));
    }

    std::vector<Bytes> got;
    const capture_file::Reading r =
        capture_file::extract(path, server, client, 0x00C5, got);

    check(r.ok, "the recording is read");
    check(r.decided_by == capture_file::HalfSource::Name,
          "and its direction comes from the name, not a guess");
    check(r.half == capture_file::Half::Client,
          "a .c2s file is read as the client half -- this is the bug that was "
          "silent: both framings tile it, so a scan picks whichever is first");
    check(r.spec == capture_file::Spec::Client && r.measured,
          "framed with [FramingClient], which reads more of a file in its layout than "
          "[Framing] does");
    check(got.size() == 6, "every packet of the opcode comes back -- got " +
                           std::to_string(got.size()));
    if (!got.empty())
        check(got.front().size() == 40, "with the header stripped");

    // The wrong spec must not quietly produce something that looks fine.
    {
        std::vector<Bytes> wrong;
        const auto as_server = extract_frames(server, c2s.data(), c2s.size(),
                                              Direction::ToClient, 0, false);
        for (const auto& p : as_server.packets)
            if (p.opcode == 0x00C5) wrong.push_back(p.payload);
        check(wrong.size() != got.size() || as_server.packets.empty(),
              "read under the server spec it does NOT yield the same packets");
    }

    std::remove(path.c_str());

    // A missing file is an error, not an empty result that reads as "nothing
    // of that opcode here".
    {
        std::vector<Bytes> none;
        const auto bad = capture_file::extract("no_such_capture.c2s", server,
                                               client, 0x0038, none);
        check(!bad.ok && !bad.error.empty(), "a missing file reports an error");
    }
}

// ------------------------------- recordings: which block frames a file

/// The client's TCP half as the P0-2 recordings hold it, in shape only: a
/// frame in the server's layout, opcode 0x0081, with a first word that
/// varies, the declared length, and a check word equal to the sum of the
/// three words before it. The 12 bytes past its declared length are what
/// [FramingClient] reads as "body + 4", and the check word what it reads as
/// an opcode.
Bytes client_wrapper(std::uint16_t first, std::size_t total, std::uint8_t seed) {
    Bytes f;
    const auto declared = static_cast<std::uint16_t>(total - 12);
    put16le(f, first);
    put16le(f, 0x0081);
    put16le(f, declared);
    put16le(f, static_cast<std::uint16_t>((first + 0x0081 + declared) & 0xFFFF));
    for (std::size_t i = 8; i < total; ++i) f.push_back(static_cast<std::uint8_t>(seed + i * 13));
    return f;
}

void write_bytes(const std::string& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// A recording's file is framed with the block that fits it: a .udp one as
/// UDP is, both ways; a .c2s one with whichever of [Framing] and
/// [FramingClient] reads more of it. And a reading that leaves much of its
/// file in no frame is never called a description of it, whatever its resync
/// count (round 1b review, R1b-REV-1).
void test_capture_file_framings() {
    section("Which block frames a recording, and how much of it is read");

    capture_file::Framings f;
    f.server = unwrapping_spec();
    f.client = client_spec();

    // Twelve client frames in the server's layout, and 18 bytes in the middle
    // that are no frame of either layout. Read as [FramingClient] reads a
    // header, those bytes declare 0x7000 bytes -- more than the file holds --
    // so a scan locked onto the frames before them stops there.
    Bytes first_six, last_six;
    const std::uint16_t words[] = { 0x0020, 0x0100, 0x0018 };
    for (std::size_t i = 0; i < 6; ++i) {
        const Bytes a = client_wrapper(words[i % 3], 41 + i, static_cast<std::uint8_t>(i));
        first_six.insert(first_six.end(), a.begin(), a.end());
        const Bytes b = client_wrapper(words[(i + 1) % 3], 48 - i, static_cast<std::uint8_t>(40 + i));
        last_six.insert(last_six.end(), b.begin(), b.end());
    }
    const Bytes foreign = { 0x11, 0x22, 0x33, 0x44, 0x00, 0x70, 0x55, 0x66, 0x77,
                            0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77 };
    const Bytes rec = joined({ first_six, foreign, last_six });

    // A .c2s file: [Framing] reads more of it, and is kept.
    {
        const capture_file::Tiling t = capture_file::tile_named(rec, "p.c2s", f);
        const capture_file::Reading& r = t.reading;
        check(r.ok && r.spec == capture_file::Spec::Server && r.measured,
              "a .c2s file is framed with [Framing] when [Framing] reads more of it");
        check(r.half == capture_file::Half::Client && r.decided_by == capture_file::HalfSource::Name,
              "and is still the client's half, as its name says");
        bool labelled = r.frames == 12;
        for (const auto& p : t.scan.packets)
            if (p.depth == 0) labelled = labelled && !p.from_server && p.opcode == 0x0081;
        check(labelled, "its 12 frames read as 0x0081, labelled client to server");
        check(r.resyncs == 18 && r.unread() == 18 && r.unread_runs.size() == 1 &&
                  r.unread_runs[0].offset == first_six.size() && r.unread_runs[0].length == 18,
              "the 18 bytes that are no frame are the only ones unread, placed where they are");
        check(r.other_frames == 6 && r.other_unread == foreign.size() + last_six.size(),
              "[FramingClient], tried too, stops at them: 6 frames and the rest unread");
        check(capture_file::judge(r) == capture_file::Fit::Described && r.largest == 48,
              "so the reading describes the file; its longest frame is 48 bytes, whole");
        check(r.resyncs > r.frames && capture_file::reading_warning(r).empty(),
              "and the --from readers warn of nothing, as --verify finds nothing wrong, though "
              "its 18 resync bytes outnumber its 12 frames (R3-REV2-1)");
    }

    // The same file forced through [FramingClient]: the scan stops at the
    // false length with no resync at all, and the rest is never read. Once,
    // that was called a description of the stream.
    {
        const capture_file::Tiling t =
            capture_file::tile_named(rec, "p.c2s", f, capture_file::Spec::Client);
        const capture_file::Reading& r = t.reading;
        check(r.ok && r.spec == capture_file::Spec::Client && !r.measured && r.frames == 6 &&
                  r.resyncs == 0,
              "forced through [FramingClient]: 6 frames, and not one resync byte");
        check(r.consumed == first_six.size() && r.unread() == rec.size() - first_six.size() &&
                  r.unread_runs.size() == 1 && r.unread_runs[0].offset == first_six.size() &&
                  r.unread_runs[0].length == rec.size() - first_six.size(),
              "but everything from the false length on is unread, counted and placed");
        check(capture_file::judge(r) == capture_file::Fit::TooMuchUnread,
              "so it is not called a description of the file");
        const std::string warning = capture_file::reading_warning(r);
        check(warning.rfind("  !! ", 0) == 0 && warning.find("are in no frame") != std::string::npos &&
                  warning.back() == '\n',
              "and the --from readers warn of the bytes in no frame, from the same verdict");
        check(!t.scan.packets.empty() &&
                  t.scan.packets.front().opcode == ((0x0020 + 0x0081 + (41 - 12)) & 0xFFFF),
              "and the opcode it reads at offset 6 is [Framing]'s check word");
    }

    // A name that says neither: the block that reads more, and the half it
    // stands for -- or the one --framing names.
    {
        const auto t = capture_file::tile_named(rec, "capture.bin", f);
        check(t.reading.ok && t.reading.spec == capture_file::Spec::Server &&
                  t.reading.decided_by == capture_file::HalfSource::Scan &&
                  t.reading.half == capture_file::Half::Server,
              "a name that says no half: the block that reads more, and its half");
        const auto forced = capture_file::tile_named(rec, "capture.bin", f, capture_file::Spec::Client);
        check(forced.reading.ok && forced.reading.half == capture_file::Half::Client &&
                  forced.reading.decided_by == capture_file::HalfSource::Forced,
              "or the block --framing names, and its half");
    }

    // With no [FramingClient] in the config.
    {
        capture_file::Framings only = f;
        only.client.reset();
        const auto e = capture_file::tile_named(rec, "p.c2s", only, capture_file::Spec::Client);
        check(!e.reading.ok && !e.reading.error.empty(),
              "forcing [FramingClient] with none in the config is an error, not a quiet [Framing]");
        const auto n = capture_file::tile_named(rec, "p.c2s", only);
        check(n.reading.ok && n.reading.spec == capture_file::Spec::Server && !n.reading.measured &&
                  n.reading.frames == 12,
              "and without one a .c2s file is framed with [Framing]");
    }

    // The client's datagrams: framed as UDP is, never with [FramingClient].
    {
        const Bytes body27 = move_body();
        const Bytes dgrams = joined({ game_frame(0x0027, body27), game_frame(0x002A, some_body(32, 2)),
                                      game_frame(0x0027, body27) });
        const auto t = capture_file::tile_named(dgrams, "p.udp.c2s", f);
        check(t.reading.ok && t.reading.spec == capture_file::Spec::Udp && !t.reading.measured &&
                  t.reading.frames == 3 && t.reading.unread() == 0 &&
                  t.reading.half == capture_file::Half::Client && t.spec.checksum_offset == 6,
              "a .udp.c2s file is framed as UDP is -- [Framing], with no [FramingUdp] -- and "
              "not with [FramingClient]");

        const std::string path = temp_file("p.udp.c2s");
        write_bytes(path, dgrams);
        std::vector<GamePacket> got;
        const auto r = capture_file::read_all(path, f, got);
        check(r.ok && got.size() == 3 && got[0].udp && !got[0].from_server &&
                  got[0].opcode == 0x0027 && got[0].payload == body27,
              "read from the file: its frames, client to server, marked UDP");
        got.clear();
        const auto old = capture_file::read_all(path, f.server, *f.client, got);
        check(old.ok && old.spec == capture_file::Spec::Udp && got.size() == 3,
              "and the same through the two-spec read_all");
        std::remove(path.c_str());

        capture_file::Framings with_udp = f;
        FramingSpec loose = f.server;
        loose.checksum_offset = FramingSpec::kNoChecksum;
        with_udp.udp = loose;
        const auto u = capture_file::tile_named(dgrams, "p.udp.s2c", with_udp);
        check(u.reading.ok && u.reading.spec == capture_file::Spec::Udp && !u.spec.has_checksum() &&
                  u.reading.half == capture_file::Half::Server,
              "with a [FramingUdp], that block frames it");
    }

    // The other verdicts.
    {
        const auto e = capture_file::tile_named(Bytes{}, "p.s2c", f);
        check(!e.reading.ok && capture_file::judge(e.reading) == capture_file::Fit::Empty,
              "an empty file is an error, and judged empty");
        const auto junk = capture_file::tile_named(Bytes(50, 0xEE), "p.s2c", f);
        check(junk.reading.ok && junk.reading.frames == 0 && junk.reading.unread() == 50 &&
                  capture_file::judge(junk.reading) == capture_file::Fit::NothingRead,
              "a file of no frames at all: all of it unread, judged so");
        Bytes big;
        for (std::uint8_t i = 0; i < 3; ++i) {
            const Bytes one = game_frame(0x0042, some_body(2000, i));
            big.insert(big.end(), one.begin(), one.end());
        }
        const auto o = capture_file::tile_named(big, "p.s2c", f);
        check(o.reading.ok && o.reading.frames == 3 && o.reading.unread() == 0 &&
                  capture_file::judge(o.reading) == capture_file::Fit::Oversized,
              "frames that average over 512 bytes are judged a length field that is not one");
        check(capture_file::reading_warning(e.reading).find("not one frame") != std::string::npos &&
                  capture_file::reading_warning(junk.reading).find("not one frame") != std::string::npos &&
                  capture_file::reading_warning(o.reading).find("frames average 2008 bytes") !=
                      std::string::npos,
              "and each verdict but a description has its warning for the --from readers");
    }

    // The three blocks out of a config.
    {
        const std::string path = temp_file("framings.ini");
        {
            std::ofstream out(path, std::ios::binary);
            out << f.server.to_ini();
        }
        bool have = false;
        const capture_file::Framings g = capture_file::load_framings(path, have);
        check(have && !g.client && !g.udp && g.for_udp().checksum_offset == 6,
              "load_framings: [Framing] alone, and UDP framed with it");
        std::remove(path.c_str());
    }
}

// ------------------------------------------ unpacking 0x0081 (liblzf)

/// `raw` as a liblzf stream of literal runs only, 32 bytes at most each: what
/// liblzf writes for bytes it cannot shrink, and unpacks back to `raw`.
Bytes lzf_literals(const Bytes& raw) {
    Bytes out;
    for (std::size_t i = 0; i < raw.size(); i += 32) {
        const std::size_t n = std::min<std::size_t>(32, raw.size() - i);
        out.push_back(static_cast<std::uint8_t>(n - 1));
        out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(i),
                   raw.begin() + static_cast<std::ptrdiff_t>(i + n));
    }
    return out;
}

/// An 0x0081 wrapper as the server sends one (protocol-findings 4e): its
/// first word the length of the frame inside unpacked, the packed length, a
/// check word over those three words, four bytes that mean nothing, and the
/// packed frame.
Bytes packed_wrapper(std::size_t unpacked, const Bytes& packed) {
    Bytes w;
    const auto first = static_cast<std::uint16_t>(unpacked);
    const auto len = static_cast<std::uint16_t>(packed.size());
    put16le(w, first);
    put16le(w, 0x0081);
    put16le(w, len);
    put16le(w, static_cast<std::uint16_t>((first + 0x0081 + len) & 0xFFFF));
    for (int b : { 0x18, 0xBE, 0xEF, 0x0D }) w.push_back(static_cast<std::uint8_t>(b));
    w.insert(w.end(), packed.begin(), packed.end());
    return w;
}

/// A frame as the game's frames are, first word 0x0105.
Bytes plain_frame(std::uint16_t opcode, const Bytes& body) {
    return game_frame(opcode, body, 0x0105);
}

/// An 0x0081 holds one frame packed with liblzf, not a frame from byte 13
/// (protocol-findings 4e), so a field read past the first body byte of
/// GoC's inner frame was never a field. The tools that read recordings
/// unpack it, so that what they show inside one is the frame's own; one that
/// does not unpack -- every wrapper the client sends is ciphered -- is left
/// shut and counted, not read as packed bytes. Hand-built streams whose
/// answer is known.
void test_unpack() {
    section("Unpacking 0x0081 (liblzf)");

    // liblzf itself.
    {
        Bytes out;
        const Bytes lit = { 0x02, 0x61, 0x62, 0x63 };
        check(lzf_unpack(lit.data(), lit.size(), 100, out) && out == Bytes{ 0x61, 0x62, 0x63 },
              "a control byte under 32 is followed by that many bytes + 1, copied as they are");
        const Bytes copy = { 0x02, 0x61, 0x62, 0x63, 0x20, 0x02 };
        check(lzf_unpack(copy.data(), copy.size(), 100, out) &&
                  out == Bytes{ 0x61, 0x62, 0x63, 0x61, 0x62, 0x63 },
              "any other copies (c >> 5) + 2 bytes from as far back as the next byte + 1");
        const Bytes run = { 0x00, 0x61, 0x60, 0x00 };
        check(lzf_unpack(run.data(), run.size(), 100, out) && out == Bytes(6, 0x61),
              "a copy from one byte back runs on into what it writes: a run of one byte");
        const Bytes longest = { 0x01, 0x61, 0x62, 0xE0, 0xFF, 0x01 };
        bool alternates = lzf_unpack(longest.data(), longest.size(), 1000, out) && out.size() == 266;
        for (std::size_t i = 0; alternates && i < out.size(); ++i)
            alternates = out[i] == ((i % 2 == 0) ? 0x61 : 0x62);
        check(alternates, "a count of 7 adds the byte after: 264 bytes, liblzf's longest copy");
        const Bytes first300 = some_body(300, 5);
        // (Not called "far": windows.h makes that word mean nothing.)
        Bytes distant = lzf_literals(first300);
        distant.push_back(0x21);    // copy 3 bytes from 256 + ...
        distant.push_back(0x2B);    // ... 43 + 1 = 300 back: the very first
        check(lzf_unpack(distant.data(), distant.size(), 1000, out) && out.size() == 303 &&
                  std::equal(first300.begin(), first300.end(), out.begin()) &&
                  out[300] == first300[0] && out[301] == first300[1] && out[302] == first300[2],
              "the control byte's low five bits reach 256 bytes further back each");
        check(lzf_unpack(nullptr, 0, 10, out) && out.empty(), "no bytes unpack to none");
    }

    // What is not a whole stream is refused, and leaves nothing out.
    {
        Bytes out = { 0x99 };
        const Bytes before_start = { 0x20, 0x00 };
        check(!lzf_unpack(before_start.data(), before_start.size(), 100, out) && out.empty(),
              "a copy from before the first byte is refused, with nothing out");
        const Bytes short_run = { 0x05, 0x61 };
        check(!lzf_unpack(short_run.data(), short_run.size(), 100, out),
              "so is a run of bytes the stream does not hold");
        const Bytes no_distance = { 0x00, 0x61, 0x20 };
        const Bytes no_count = { 0x00, 0x61, 0xE0 };
        check(!lzf_unpack(no_distance.data(), no_distance.size(), 100, out) &&
                  !lzf_unpack(no_count.data(), no_count.size(), 100, out),
              "and a copy cut off before its distance byte, or its count byte");
        const Bytes five = { 0x04, 1, 2, 3, 4, 5 };
        check(!lzf_unpack(five.data(), five.size(), 4, out) &&
                  lzf_unpack(five.data(), five.size(), 5, out) && out.size() == 5,
              "and a stream that would pass the limit; reaching it is fine");
    }

    const FramingSpec shipped = unwrapping_spec();
    FramingSpec spec = shipped;
    spec.unpack = true;

    // HP 9232 and MP 8902 at frame bytes 12 and 16 of an 0x0051, as P0-2
    // found them: 24 bytes liblzf cannot shrink, so 25 packed.
    Bytes body51(16, 0);
    const std::uint32_t hp = 9232, mp = 8902;
    std::memcpy(body51.data() + 4, &hp, 4);
    std::memcpy(body51.data() + 8, &mp, 4);
    const Bytes f51 = plain_frame(0x0051, body51);
    const Bytes w51 = packed_wrapper(f51.size(), lzf_literals(f51));

    // 600 bytes of zeros after a header, as liblzf packs them: nine bytes as
    // they are -- the header and the first body byte -- then copies of the
    // byte before: 264, 264 and 71.
    const Bytes f4f = plain_frame(0x004F, Bytes(600, 0));
    Bytes zeros = { 0x08 };
    zeros.insert(zeros.end(), f4f.begin(), f4f.begin() + 9);
    for (int b : { 0xE0, 0xFF, 0x00, 0xE0, 0xFF, 0x00, 0xE0, 0x3E, 0x00 })
        zeros.push_back(static_cast<std::uint8_t>(b));
    const Bytes w4f = packed_wrapper(f4f.size(), zeros);

    // One wrapper at a time.
    {
        Bytes frame;
        check(unpack_wrapper(spec, w51.data(), w51.size(), frame) && frame == f51,
              "an 0x0081 unpacks to the one frame inside it, whole");
        check(unpack_wrapper(spec, w4f.data(), w4f.size(), frame) && frame == f4f && w4f.size() == 31,
              "608 bytes out of 19 packed: nine as they are, then copies of the byte before");

        const auto with_first = [](Bytes w, std::uint16_t first) {
            const auto len = static_cast<std::uint16_t>(w[4] | (w[5] << 8));
            const auto sum = static_cast<std::uint16_t>((first + 0x0081 + len) & 0xFFFF);
            w[0] = static_cast<std::uint8_t>(first & 0xFF);
            w[1] = static_cast<std::uint8_t>(first >> 8);
            w[6] = static_cast<std::uint8_t>(sum & 0xFF);
            w[7] = static_cast<std::uint8_t>(sum >> 8);
            return w;
        };
        check(!unpack_wrapper(spec, with_first(w51, 25).data(), w51.size(), frame) && frame.empty() &&
                  !unpack_wrapper(spec, with_first(w51, 23).data(), w51.size(), frame),
              "refused when the first word is not the length it unpacks to, either way");

        Bytes bad_check = f51;
        bad_check[6] = static_cast<std::uint8_t>(bad_check[6] + 1);
        const Bytes w_bad = packed_wrapper(bad_check.size(), lzf_literals(bad_check));
        const Bytes old_marker = game_frame(0x0051, body51);         // first word 0x0005
        const Bytes w_old = packed_wrapper(old_marker.size(), lzf_literals(old_marker));
        check(!unpack_wrapper(spec, w_bad.data(), w_bad.size(), frame) &&
                  !unpack_wrapper(spec, w_old.data(), w_old.size(), frame),
              "and when what comes out is not a frame: its check word off, or a first word not 0x0105");

        const Bytes ciphered = client_wrapper(0x0020, 48, 7);
        check(!unpack_wrapper(spec, ciphered.data(), ciphered.size(), frame),
              "a client wrapper's ciphered bytes do not unpack");
        check(!unpack_wrapper(spec, w51.data(), w51.size() - 1, frame) &&
                  !unpack_wrapper(client_spec(), w51.data(), w51.size(), frame),
              "nor does a wrapper cut short, or any under a spec that has no wrapper opcode");
    }

    // Off a stream: each wrapper as before, and straight after it its frame.
    {
        const Bytes p38 = game_frame(0x0038, some_body(88, 3));
        const Bytes stream = joined({ p38, w51, p38, w4f, p38 });
        const auto on  = extract_frames(spec, stream.data(), stream.size(), Direction::ToClient, 5, false);
        const auto off = extract_frames(shipped, stream.data(), stream.size(), Direction::ToClient, 5, false);
        check(on.packets.size() == 7 && on.consumed == stream.size() && on.resyncs == 0,
              "two wrappers in a stream: five frames off it, two inside, every byte read");
        if (on.packets.size() == 7) {
            const GamePacket& in51 = on.packets[2];
            check(in51.opcode == 0x0051 && in51.depth == 1 && in51.payload == body51 &&
                      in51.from_server && in51.timestamp_ms == 5 && !in51.udp,
                  "the first one's frame right after it, unpacked: its own body, at depth 1");
            std::uint32_t got_hp = 0, got_mp = 0;
            if (in51.payload.size() >= 12) {
                std::memcpy(&got_hp, in51.payload.data() + 4, 4);
                std::memcpy(&got_mp, in51.payload.data() + 8, 4);
            }
            check(got_hp == 9232 && got_mp == 8902,
                  "HP and MP read back at body @4 and @8: frame bytes 12 and 16");
            check(on.packets[5].opcode == 0x004F && on.packets[5].payload == Bytes(600, 0),
                  "and the second one's 600 bytes of zeros, whole");
        }
        check(on.unwrap.unpacked == 2 && on.unwrap.inner == 2 && on.unwrap.unpack_failed == 0 &&
                  on.unwrap.length_differs == 0 && on.unwrap.checksum_bad == 0,
              "counted: two wrappers unpacked, two frames inside, none disagreeing");
        std::vector<GamePacket> outer_on, outer_off;
        for (const auto& p : on.packets)
            if (p.depth == 0) outer_on.push_back(p);
        for (const auto& p : off.packets)
            if (p.depth == 0) outer_off.push_back(p);
        check(same_frames(outer_on, outer_off) && on.consumed == off.consumed && off.resyncs == 0,
              "the frames off the stream are the same as without unpacking");
        check(off.packets.size() == 7 && off.packets[5].opcode == 0x004F &&
                  off.packets[5].depth == 1 && off.packets[5].payload != Bytes(600, 0),
              "GoC's frame from byte 13 has the opcode right and the zeros still packed");
    }

    // One that does not unpack: left shut, counted, and the stream goes on.
    {
        const Bytes p38 = game_frame(0x0038, some_body(88, 3));
        const Bytes stream = joined({ client_wrapper(0x0020, 48, 7), p38 });
        const auto scan = extract_frames(spec, stream.data(), stream.size(), Direction::ToServer, 0, false);
        check(scan.packets.size() == 2 && scan.packets[0].opcode == 0x0081 &&
                  scan.packets[1].opcode == 0x0038 && scan.packets[1].depth == 0,
              "a wrapper that does not unpack comes out as itself, and the next frame after it");
        check(scan.unwrap.unpack_failed == 1 && scan.unwrap.inner == 0 && scan.unwrap.unopened == 0 &&
                  scan.consumed == stream.size() && scan.resyncs == 0,
              "nothing inside it is read, which is counted, and nothing after it is lost");
    }

    // GoC's 0x0020 rule is a frame of 32 bytes: unpacked like any other.
    {
        const Bytes f5a = plain_frame(0x005A, some_body(24, 9));
        const Bytes w5a = packed_wrapper(f5a.size(), lzf_literals(f5a));
        const auto on  = extract_frames(spec, w5a.data(), w5a.size(), Direction::ToClient, 0, false);
        const auto off = extract_frames(shipped, w5a.data(), w5a.size(), Direction::ToClient, 0, false);
        check(w5a[0] == 0x20 && w5a[1] == 0x00 && on.packets.size() == 2 &&
                  on.packets[1].opcode == 0x005A && on.unwrap.unopened == 0,
              "a wrapper whose first word is 0x0020, a 32-byte frame, is unpacked");
        check(off.packets.size() == 1 && off.unwrap.unopened == 1,
              "where GoC's reading leaves it shut, as GoC does");
    }

    // Wrappers inside wrappers, and the depth limit.
    {
        const Bytes outer = packed_wrapper(w51.size(), lzf_literals(w51));
        const auto scan = extract_frames(spec, outer.data(), outer.size(), Direction::ToClient, 0, false);
        check(scan.packets.size() == 3 && scan.packets[1].opcode == 0x0081 &&
                  scan.packets[1].depth == 1 && scan.packets[2].opcode == 0x0051 &&
                  scan.packets[2].depth == 2 && scan.packets[2].payload == body51 &&
                  scan.unwrap.unpacked == 2,
              "a wrapper packed inside a wrapper is unpacked in turn");
        Bytes deep = f51;
        for (int i = 0; i <= kMaxWrapDepth; ++i) deep = packed_wrapper(deep.size(), lzf_literals(deep));
        const auto limit = extract_frames(spec, deep.data(), deep.size(), Direction::ToClient, 0, false);
        std::uint8_t deepest = 0;
        for (const auto& p : limit.packets) deepest = std::max(deepest, p.depth);
        check(deepest == kMaxWrapDepth && limit.unwrap.too_deep == 1 &&
                  limit.consumed == deep.size(),
              "down to kMaxWrapDepth, the wrapper at the limit counted and not opened");
    }

    // A datagram is read the same way.
    {
        const auto d = extract_datagram(spec, w51.data(), w51.size(), Direction::ToClient, 0);
        check(d.packets.size() == 2 && d.packets[1].udp && d.packets[1].payload == body51 &&
                  d.unwrap.unpacked == 1,
              "a wrapper in a datagram is unpacked too, its frame marked UDP");
    }

    // A recording, read with every block unpacking, as the --from readers read one.
    {
        capture_file::Framings f;
        f.server = spec;
        f.client = client_spec();
        f.client->unpack = true;
        const Bytes rec = joined({ game_frame(0x0038, some_body(88, 3)), w51, w4f });
        const auto t = capture_file::tile_named(rec, "p.s2c", f);
        check(t.reading.ok && t.reading.inner == 2 && t.reading.unwrap.unpacked == 2 &&
                  t.reading.unwrap.unpack_failed == 0 && t.reading.unread() == 0,
              "a server recording: each wrapper unpacked");
        Bytes c2s;
        for (std::size_t i = 0; i < 4; ++i) {
            const auto first = static_cast<std::uint16_t>(i % 2 == 0 ? 0x0020 : 0x0018);
            const Bytes w = client_wrapper(first, 44 + i, static_cast<std::uint8_t>(i));
            c2s.insert(c2s.end(), w.begin(), w.end());
        }
        const auto c = capture_file::tile_named(c2s, "p.c2s", f);
        check(c.reading.ok && c.reading.frames == 4 && c.reading.inner == 0 &&
                  c.reading.unwrap.unpack_failed == 4 && c.reading.half == capture_file::Half::Client,
              "a client recording: its ciphered wrappers left shut, every one counted");
    }

    // Off unless code turns it on: no config sets it.
    {
        check(!FramingSpec{}.unpack && !shipped.unpack, "a spec does not unpack unless told to");
        const std::string path = temp_file("unpack.ini");
        {
            std::ofstream out(path, std::ios::binary);
            out << spec.to_ini();
        }
        bool found = false;
        const FramingSpec loaded = load_framing(path, found);
        check(found && loaded.unwraps() && !loaded.unpack && spec.to_ini() == shipped.to_ini(),
              "nor does one read from a config: the setting is neither written nor read");
        std::remove(path.c_str());
        check(spec.describe().find("unpacked") != std::string::npos &&
                  shipped.describe().find("inner@13") != std::string::npos,
              "a spec says how it reads a wrapper");
        UnwrapCounts one, sum;
        one.unpacked = 2;
        one.unpack_failed = 3;
        sum += one;
        sum += one;
        check(sum.unpacked == 4 && sum.unpack_failed == 6, "unpacked and failed counts add up");
    }
}

// ----------------------------------------- monsters, from the server's frames

/// The map's monsters as the P0-2 fight showed the server sending them
/// (docs/protocol-findings.md §4f): the table at a map load, a monster
/// spawning in a slot, and a monster's state events -- a stop, a walk, a
/// hit -- at the frame bytes given there. Hand-built frames whose answer is
/// known; every slot, id, position and HP is made up, none taken from a
/// recording.
void test_monster_frames() {
    section("Monsters, read from the server's frames");

    // A frame of `size` bytes, header included, its first word 0x0105 as the
    // game's frames have it. Fields are written at frame bytes.
    struct Frame {
        std::uint16_t opcode;
        Bytes         body;
        Frame(std::uint16_t op, std::size_t size) : opcode(op), body(size - 8, 0) {}
        void u8(std::size_t at, std::uint8_t v)   { body[at - 8] = v; }
        void u16(std::size_t at, std::uint16_t v) { std::memcpy(body.data() + at - 8, &v, 2); }
        void u32(std::size_t at, std::uint32_t v) { std::memcpy(body.data() + at - 8, &v, 4); }
        void i32(std::size_t at, std::int32_t v)  { std::memcpy(body.data() + at - 8, &v, 4); }
        void f32(std::size_t at, float v)         { std::memcpy(body.data() + at - 8, &v, 4); }
        Bytes bytes() const { return plain_frame(opcode, body); }
    };
    // As the server sent it, straight off the TCP stream; or found inside a
    // wrapper, unpacked.
    const auto sent = [](const Frame& f) {
        GamePacket p;
        p.opcode = f.opcode;
        p.payload = f.body;
        p.from_server = true;
        return p;
    };
    const auto inside = [&sent](const Frame& f) {
        GamePacket p = sent(f);
        p.depth = 1;
        return p;
    };
    // The same frame from the client, and over UDP: `reader` takes neither.
    const auto never = [](const GamePacket& p, const auto& reader) {
        GamePacket up = p;
        up.from_server = false;
        GamePacket dgram = p;
        dgram.udp = true;
        return !reader(up) && !reader(dgram);
    };

    // ---- 0x0042: the map's table, three monsters, slot 0 among them.
    struct Entry {
        std::uint16_t slot;
        std::uint32_t type;
        std::uint8_t  level;
        std::int8_t   facing;
        std::uint16_t x, y;
        float         speed;
        std::uint32_t hp, max_hp;
    };
    const Entry entries[] = {
        { 0, 2000501, 50, -1, 1500, 900, 3.0f, 420, 600 },        // walking, hurt
        { 7, 2000301, 30, 1, 80, 1210, 0.0f, 2000, 2000 },        // standing
        { 12, 2000501, 50, 1, 3333, 640, 3.0f, 600, 600 },
    };
    const auto table_of = [&entries](std::size_t n) {
        Frame t(0x0042, 2568);
        t.u32(12, static_cast<std::uint32_t>(n));
        for (std::size_t k = 0; k < 50; ++k) t.u16(316 + 2 * k, 0xFFFF);
        for (std::size_t k = 0; k < n; ++k) {
            const Entry& e = entries[k];
            t.u8(16 + k, static_cast<std::uint8_t>(e.speed != 0.0f ? 1 : 0));    // moving
            t.u8(66 + k, e.level);
            t.u8(166 + k, static_cast<std::uint8_t>(e.facing));
            t.u8(216 + k, 1);
            t.u16(316 + 2 * k, e.slot);
            t.u16(416 + 2 * k, e.x);
            t.u16(516 + 2 * k, e.y);
            t.u32(1516 + 4 * k, e.type);
            t.f32(1716 + 4 * k, e.speed);
            t.u32(2016 + 4 * k, e.hp);
            t.u32(2216 + 4 * k, e.max_hp);
        }
        return t;
    };
    const Frame table = table_of(3);
    const Bytes wrapper = packed_wrapper(table.bytes().size(), lzf_literals(table.bytes()));
    FramingSpec unpacking = unwrapping_spec();
    unpacking.unpack = true;
    {
        const auto scan = extract_frames(unpacking, wrapper.data(), wrapper.size(), Direction::ToClient,
                                         0, false);
        const bool whole = scan.packets.size() == 2 && scan.packets[1].opcode == 0x0042 &&
                           scan.packets[1].depth == 1 && scan.packets[1].payload == table.body;
        check(whole, "the table, packed into an 0x0081, comes out of it whole");
        const auto got = whole ? read_map_monsters(scan.packets[1]) : std::nullopt;
        check(got && got->size() == 3, "and gives the three monsters its count says");
        bool fields = got && got->size() == 3;
        for (std::size_t k = 0; fields && k < 3; ++k) {
            const MapMonster& m = (*got)[k];
            const Entry& e = entries[k];
            fields = m.slot == e.slot && m.type_id == e.type && m.level == e.level &&
                     m.facing == e.facing && m.x == e.x && m.y == e.y && m.speed == e.speed &&
                     m.hp == e.hp && m.max_hp == e.max_hp;
        }
        check(fields, "each with its slot, type id, level, facing, x, y, speed, HP and max HP from "
                      "its columns; slot 0 a monster like the others, and HP below max as the "
                      "table had it");

        // Taken from byte 13, as the live capture takes it until P0-3 turns
        // unpacking on: the opcode is right, the body still packed.
        const auto goc = extract_frames(unwrapping_spec(), wrapper.data(), wrapper.size(),
                                        Direction::ToClient, 0, false);
        check(goc.packets.size() == 2 && goc.packets[1].opcode == 0x0042 &&
                  !read_map_monsters(goc.packets[1]),
              "taken from byte 13, still packed, it gives nothing");
    }
    {
        const auto none = read_map_monsters(inside(table_of(0)));
        check(none && none->empty(), "a table of none, a map without monsters, gives an empty list");

        Frame over = table_of(3);
        over.u32(12, 51);
        check(!read_map_monsters(inside(over)), "a count past the table's 50 entries gives nothing");

        Frame gap = table_of(3);
        gap.u16(316 + 2 * 1, 0xFFFF);
        Frame lost = table_of(3);
        lost.f32(1716 + 4 * 2, std::nanf(""));
        const auto g = read_map_monsters(inside(gap));
        const auto l = read_map_monsters(inside(lost));
        check(g && g->size() == 2 && (*g)[1].slot == 12 && l && l->size() == 2 && (*l)[1].slot == 7,
              "an entry marked no monster (slot 0xFFFF), or whose speed is no number, is left out");

        GamePacket cut = inside(table);
        cut.payload.pop_back();
        check(!read_map_monsters(cut), "a table one byte short is not the table");
        check(!read_map_monsters(sent(table)), "nor is one straight off the stream, never seen so");
        check(never(inside(table), [](const GamePacket& p) { return read_map_monsters(p).has_value(); }),
              "nor one from the client, or over UDP");
    }

    // ---- 0x003F: a monster spawning in slot 7.
    Frame spawn(0x003F, 52);
    spawn.u32(12, 2000301);
    spawn.u8(16, 30);
    spawn.u8(18, 0xFF);
    spawn.u8(19, 1);
    spawn.u16(20, 2468);
    spawn.u16(22, 1300);
    spawn.u32(28, 2000);
    spawn.u16(32, 7);
    spawn.u8(44, 1);
    {
        const auto s = read_monster_spawn(sent(spawn));
        check(s && s->slot == 7 && s->type_id == 2000301 && s->level == 30 && s->facing == -1 &&
                  s->x == 2468 && s->y == 1300 && s->max_hp == 2000,
              "0x003F: type id at 12, level 16, facing 18, x, y at 20 and 22, max HP 28, slot 32");
        check(s && s->hp == s->max_hp && s->speed == 0.0f,
              "the new monster full of HP, and standing");
        GamePacket least = sent(spawn);
        least.payload.resize(34 - 8);
        GamePacket shorter = least;
        shorter.payload.pop_back();
        check(read_monster_spawn(least) && !read_monster_spawn(shorter),
              "a frame that ends with the slot is enough; one byte shorter gives nothing");
        check(!read_monster_spawn(inside(spawn)) &&
                  never(sent(spawn), [](const GamePacket& p) { return read_monster_spawn(p).has_value(); }),
              "nor does one inside a wrapper, from the client, or over UDP");
    }

    // ---- 0x0038: slot 12 stands, walks, and is killed.
    const auto state_frame = [](std::uint8_t state, std::uint32_t slot) {
        Frame f(0x0038, 96);
        f.u8(12, state);
        f.u32(16, slot);
        f.u32(48, 0xFFFFFFFFu);
        f.u16(88, 3100);    // the pair [Packet.0x0038] reads as x, y: not the position
        f.u16(90, 560);
        return f;
    };
    Frame stop = state_frame(0, 12);
    stop.u8(24, 0x01);
    stop.u16(32, 3400);
    stop.u16(34, 640);
    stop.i32(64, 600);
    Frame walk = state_frame(1, 12);
    walk.u8(24, 0xFF);
    walk.f32(28, 3.0f);
    walk.u16(32, 3400);
    walk.u16(34, 640);
    walk.i32(64, 600);
    Frame hit = state_frame(9, 12);
    hit.u8(24, 0x01);
    hit.f32(28, 7.0f);
    hit.u16(32, 3310);
    hit.u16(34, 640);
    hit.u32(48, 4321);
    hit.u32(52, 5600);
    hit.i32(64, 600 - 5600);
    {
        const auto s = read_monster_state(sent(stop));
        check(s && s->event == MonsterEvent::Stop && s->slot == 12 && s->x == 3400 && s->y == 640 &&
                  s->facing == 1 && s->speed == 0.0f && s->hp == 600,
              "0x0038, a stop: state 0 at 12, slot 16, facing 24, speed 28, x, y at 32 and 34, HP 64");
        check(s && !s->attacker && s->damage == 0 && !s->killed(),
              "no attacker (0xFFFFFFFF), no damage, nothing killed");
        const auto w = read_monster_state(sent(walk));
        check(w && w->event == MonsterEvent::Walk && w->facing == -1 && w->speed == 3.0f &&
                  w->x == 3400 && w->hp == 600,
              "a walk: state 1, facing -1 toward smaller x, its speed per 25 ms step");
        const auto h = read_monster_state(sent(hit));
        check(h && h->event == MonsterEvent::Hit && h->attacker == 4321u && h->damage == 5600 &&
                  h->hp == -5000 && h->killed() && h->x == 3310,
              "a hit: state 9, the attacker at 48, the damage at 52, and HP after it at 64, below "
              "zero: killed");
        check(h && s && s->hp - static_cast<std::int32_t>(h->damage) == h->hp,
              "HP before less the damage is HP after");
        check(s && h && s->x != 3100 && h->y != 560,
              "the pair at 88 and 90 is never taken for the position");

        Frame grazed = hit;
        grazed.i32(64, 100);
        const auto g = read_monster_state(sent(grazed));
        check(g && g->event == MonsterEvent::Hit && !g->killed(),
              "a hit that leaves HP above zero has not killed (never seen: every hit of the fight "
              "killed)");

        const auto zero = read_monster_state(sent(state_frame(0, 0)));
        check(zero && zero->slot == 0, "slot 0 is read like any other");
    }
    {
        check(!read_monster_state(sent(state_frame(2, 12))) &&
                  !read_monster_state(sent(state_frame(8, 12))),
              "a state other than 0, 1 or 9 gives nothing: none was seen");
        Frame odd = walk;
        odd.f32(28, std::nanf(""));
        check(!read_monster_state(sent(odd)), "nor does a speed that is no number");
        GamePacket least = sent(hit);
        least.payload.resize(68 - 8);
        GamePacket shorter = least;
        shorter.payload.pop_back();
        check(read_monster_state(least) && !read_monster_state(shorter),
              "a frame that ends with the HP is enough; one byte shorter gives nothing");
        check(!read_monster_state(inside(stop)) &&
                  never(sent(stop), [](const GamePacket& p) { return read_monster_state(p).has_value(); }),
              "nor does one inside a wrapper, from the client, or over UDP");
        check(!read_monster_state(sent(spawn)) && !read_monster_spawn(sent(stop)) &&
                  !read_map_monsters(sent(stop)) && !read_monster_state(inside(table)),
              "each reader takes its own opcode only");
    }

    // ---- One stream from the server, framed and unpacked as a recording is
    // read: each reader finds its own frames in it.
    {
        const Bytes stream = joined({ stop.bytes(), wrapper, walk.bytes(), hit.bytes(), spawn.bytes() });
        const auto scan = extract_frames(unpacking, stream.data(), stream.size(), Direction::ToClient,
                                         0, false);
        std::size_t tables = 0, spawns = 0, states = 0, kills = 0;
        for (const GamePacket& p : scan.packets) {
            if (const auto t = read_map_monsters(p); t && t->size() == 3) ++tables;
            if (read_monster_spawn(p)) ++spawns;
            if (const auto s = read_monster_state(p)) {
                ++states;
                if (s->killed()) ++kills;
            }
        }
        check(scan.packets.size() == 6 && scan.resyncs == 0 && tables == 1 && spawns == 1 &&
                  states == 3 && kills == 1,
              "a stream of a stop, the table, a walk, a kill and a respawn: one table, three "
              "events, one of them a kill, and one respawn");
    }
}

// ------------------------------------- one opcode's frames as numbers

void test_frame_numbers() {
    section("One opcode's frames as numbers (--rows)");
    using capture_file::Number;

    Number n = Number::U8;
    check(capture_file::number_from_name("F32", n) && n == Number::F32 &&
              capture_file::number_from_name("i16", n) && n == Number::I16 &&
              !capture_file::number_from_name("f64", n) && !capture_file::number_from_name("", n),
          "a number's type is read by name, in any case, and an unknown one refused");
    check(capture_file::number_size(Number::I8) == 1 && capture_file::number_size(Number::U16) == 2 &&
              capture_file::number_size(Number::F32) == 4 &&
              std::string(capture_file::number_name(Number::I32)) == "i32",
          "with its size and name");

    // FE FF 34 12 00 00 80 3F: u16 0xFFFE, then 0x1234, then 1.0f.
    const Bytes b = { 0xFE, 0xFF, 0x34, 0x12, 0x00, 0x00, 0x80, 0x3F };
    double v = 0.0;
    std::uint32_t raw = 0;
    check(capture_file::read_number(b, 0, Number::U8, v) && v == 254.0 &&
              capture_file::read_number(b, 0, Number::I8, v) && v == -2.0,
          "u8 and i8");
    check(capture_file::read_number(b, 0, Number::U16, v) && v == 65534.0 &&
              capture_file::read_number(b, 0, Number::I16, v) && v == -2.0 &&
              capture_file::read_number(b, 2, Number::U16, v) && v == 0x1234,
          "u16 and i16, little-endian");
    check(capture_file::read_number(b, 0, Number::U32, v) && v == static_cast<double>(0x1234FFFEu) &&
              capture_file::read_number(b, 0, Number::I32, v) && v == static_cast<double>(0x1234FFFE),
          "u32 and i32");
    check(capture_file::read_number(b, 4, Number::F32, v, &raw) && v == 1.0 && raw == 0x3F800000u,
          "f32, with its raw bits");
    check(!capture_file::read_number(b, 6, Number::U32, v) &&
              !capture_file::read_number(b, 8, Number::U8, v) &&
              !capture_file::read_number(b, 1000, Number::U8, v),
          "a number past the end of the body is not read");
    check(capture_file::format_number(1155.4f, Number::F32) == "1155.40" &&
              capture_file::format_number(0.0, Number::F32) == "0.00" &&
              capture_file::format_number(1e-45f, Number::F32) == "1.4e-45" &&
              capture_file::format_number(-2.0, Number::I16) == "-2" &&
              capture_file::format_number(4294967295.0, Number::U32) == "4294967295" &&
              capture_file::format_number(std::nan(""), Number::F32) == "nan",
          "numbers print as integers, or floats with two decimals or an exponent");

    // A float at 0 that goes 1, 2, 2, 0.5, and a byte at 4 that stays 7.
    const auto body_with = [](float x, std::uint8_t k) {
        Bytes body(8, 0);
        std::memcpy(body.data(), &x, sizeof(x));
        body[4] = k;
        return body;
    };
    std::vector<Bytes> bodies = { body_with(1.0f, 7), body_with(2.0f, 7), body_with(2.0f, 7),
                                  body_with(0.5f, 7) };
    const capture_file::Column c = capture_file::column(bodies, Number::F32, 0);
    check(c.frames == 4 && c.changes == 2 && c.rises == 1 && c.falls == 1 && c.distinct == 3,
          "a column counts its changes, rises, falls and distinct values, in frame order");
    check(c.first == 1.0 && c.last == 0.5 && c.min == 0.5 && c.max == 2.0 && !c.constant(),
          "and its first, last, least and greatest");
    const capture_file::Column k = capture_file::column(bodies, Number::U8, 4);
    check(k.constant() && k.distinct == 1 && k.first == 7.0, "a byte that never changes is constant");

    bodies.push_back(body_with(std::nanf(""), 7));
    const capture_file::Column with_nan = capture_file::column(bodies, Number::F32, 0);
    check(with_nan.changes == 3 && with_nan.distinct == 4 && with_nan.min == 0.5 &&
              with_nan.max == 2.0,
          "a NaN is a value of its own, left out of the range");

    const std::vector<Bytes> four(bodies.begin(), bodies.begin() + 4);
    const auto f32s = capture_file::columns(four, Number::F32, 0, 0);
    const auto every = capture_file::columns(four, Number::F32, 0, 1);
    check(f32s.size() == 2 && f32s[0].offset == 0 && f32s[1].offset == 4 && every.size() == 5,
          "columns go every number's size by default, or every byte");
    bodies.push_back(Bytes{ 1, 2 });
    const auto shortest = capture_file::columns(bodies, Number::U8, 0, 0);
    check(shortest.size() == 2 && capture_file::column(bodies, Number::F32, 0).frames == 5,
          "columns stop where every body ends; a body too short is left out of a column");

    // --at and --step take their numbers so (R3-REV2-5).
    std::size_t at = 99;
    check(capture_file::parse_count("12", at) && at == 12 && capture_file::parse_count("012", at) &&
              at == 12 && capture_file::parse_count("08", at) && at == 8 &&
              capture_file::parse_count("0", at) && at == 0,
          "an offset is decimal, and a leading zero leaves it decimal: 012 is twelve, not ten");
    check(capture_file::parse_count("0x1C", at) && at == 28 && capture_file::parse_count("0X0a", at) &&
              at == 10,
          "or hex after 0x");
    at = 99;
    check(!capture_file::parse_count("", at) && !capture_file::parse_count("-1", at) &&
              !capture_file::parse_count("+1", at) && !capture_file::parse_count(" 1", at) &&
              !capture_file::parse_count("12a", at) && !capture_file::parse_count("0x", at) &&
              !capture_file::parse_count("0x1G", at) && !capture_file::parse_count("1e3", at) &&
              !capture_file::parse_count("99999999999999999999", at) && at == 99,
          "and anything else is refused, the number left as it was");
}

// ----------------------------------------- recordings, per connection

void test_flow_halves() {
    section("--record keeps each connection's two halves apart");

    const Bytes a1 = some_body(10, 1), a2 = some_body(5, 2), b1 = some_body(7, 3);
    const Bytes c1 = some_body(4, 4), c2 = some_body(3, 5);
    capture_file::FlowHalves h;
    // Connection 200 carries only the client's half; 100 and 300 carry the
    // server's, and 100 the client's as well.
    h.add(200, "c", false, c1.data(), c1.size());
    h.add(100, "a", true,  a1.data(), a1.size());
    h.add(100, "a, renamed", false, a2.data(), a2.size());
    h.add(300, "b", true,  b1.data(), b1.size());
    h.add(200, "c", false, c2.data(), c2.size());
    h.add(100, "a", true,  a2.data(), a2.size());

    const std::string base = temp_file("rec");
    const auto files = h.files(base, false);
    check(h.connections() == 3 && files.size() == 4, "three connections, four halves that carried anything");
    if (files.size() == 4) {
        check(files[0].path == base + ".flow0.s2c" && files[0].to_client && files[0].bytes == 15 &&
                  files[0].connection == "a",
              "the server halves are numbered first, as they always were, under the first name given");
        check(files[1].path == base + ".flow0.c2s" && !files[1].to_client && files[1].bytes == 5,
              "and a connection's client half takes the same number");
        check(files[2].path == base + ".flow1.s2c" && files[2].bytes == 7,
              "the next connection the next number");
        check(files[3].path == base + ".flow2.c2s" && files[3].bytes == 7 && files[3].connection == "c",
              "and one that carried only the client's half is numbered after them");
        check(!files[0].written, "nothing is written unless asked");
    }

    const auto written = h.files(base, true);
    bool all = written.size() == 4;
    for (const auto& file : written) all = all && file.written;
    const auto read_back = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    check(all && read_back(base + ".flow0.s2c") == joined({ a1, a2 }) &&
              read_back(base + ".flow0.c2s") == a2 && read_back(base + ".flow2.c2s") == joined({ c1, c2 }),
          "written, each half's bytes in the order they came");
    for (const auto& file : written) std::remove(file.path.c_str());
}

// ------------------------------------------------------------- reassembly

void test_reassembler() {
    section("TCP reassembly");

    TcpReassembler re;
    std::vector<std::uint8_t> received;

    re.set_callback([&](const FlowKey&, Direction, const std::uint8_t* d,
                        std::size_t n, std::uint64_t) -> std::size_t {
        received.insert(received.end(), d, d + n);
        return n;      // consume everything
    });

    Endpoint client{ 0x0A000001, 50000 };
    Endpoint server{ 0x0A000002, 15000 };
    re.set_server(server);

    const std::uint8_t a[] = { 1, 2, 3, 4 };
    const std::uint8_t b[] = { 5, 6, 7, 8 };
    const std::uint8_t c[] = { 9, 10 };

    re.on_segment(server, client, 1000, false, false, false, false, a, sizeof(a), 0);
    re.on_segment(server, client, 1004, false, false, false, false, b, sizeof(b), 0);
    re.on_segment(server, client, 1008, false, false, false, false, c, sizeof(c), 0);

    check(received.size() == 10, "in-order segments reassemble");
    check(received[0] == 1 && received[9] == 10, "bytes in correct order");

    // Out-of-order: the middle segment arrives last and must slot in.
    re.clear();
    received.clear();
    re.on_segment(server, client, 2000, false, false, false, false, a, sizeof(a), 0);
    re.on_segment(server, client, 2008, false, false, false, false, c, sizeof(c), 0);   // ahead
    check(received.size() == 4, "segment past a gap is withheld");
    re.on_segment(server, client, 2004, false, false, false, false, b, sizeof(b), 0);   // fills it
    check(received.size() == 10, "gap fill releases the buffered segment");
    check(received[4] == 5 && received[8] == 9, "out-of-order data lands in order");

    // Retransmits must not duplicate.
    re.clear();
    received.clear();
    re.on_segment(server, client, 3000, false, false, false, false, a, sizeof(a), 0);
    re.on_segment(server, client, 3000, false, false, false, false, a, sizeof(a), 0);
    check(received.size() == 4, "exact retransmit dropped");

    // Partial overlap: only the fresh tail is taken.
    re.clear();
    received.clear();
    const std::uint8_t ov[] = { 1, 2, 3, 4, 5, 6 };
    re.on_segment(server, client, 4000, false, false, false, false, a, sizeof(a), 0);
    re.on_segment(server, client, 4002, false, false, false, false, ov, sizeof(ov), 0);
    check(received.size() == 8, "overlapping retransmit contributes only new bytes");

    // Direction labelling.
    re.clear();
    Direction last = Direction::ToServer;
    re.set_callback([&](const FlowKey&, Direction d, const std::uint8_t*,
                        std::size_t n, std::uint64_t) -> std::size_t {
        last = d;
        return n;
    });
    re.set_server(server);
    re.on_segment(server, client, 5000, false, false, false, false, a, sizeof(a), 0);
    check(last == Direction::ToClient, "server->client direction labelled");
    re.on_segment(client, server, 6000, false, false, false, false, a, sizeof(a), 0);
    check(last == Direction::ToServer, "client->server direction labelled");

    // --- attaching to a connection already in progress ---
    //
    // The bot normally starts after the game is logged in, so there is no
    // handshake to observe and the first segment seen is as likely to be from
    // the server as the client. Guessing "first talker is the client" inverts
    // direction half the time, and a consumer that wants only one side then
    // silently receives nothing.
    auto first_direction_when_server_speaks_first = [&](bool tell_it_the_server) {
        TcpReassembler r2;
        Direction seen = Direction::ToServer;
        bool got = false;
        r2.set_callback([&](const FlowKey&, Direction d, const std::uint8_t*,
                            std::size_t n, std::uint64_t) -> std::size_t {
            if (!got) { seen = d; got = true; }
            return n;
        });
        if (tell_it_the_server) r2.set_server(server);
        // No SYN: mid-stream. Server transmits first.
        r2.on_segment(server, client, 9000, false, true, false, false, a, sizeof(a), 0);
        return seen;
    };

    check(first_direction_when_server_speaks_first(true) == Direction::ToClient,
          "known server endpoint fixes direction mid-stream");
    check(first_direction_when_server_speaks_first(false) == Direction::ToClient,
          "ephemeral port heuristic fixes direction with no server hint");

    // The handshake, when we do catch it: bare SYN is the client, SYN-ACK the
    // server, regardless of port numbers.
    {
        TcpReassembler r3;
        Direction seen = Direction::ToClient;
        r3.set_callback([&](const FlowKey&, Direction d, const std::uint8_t*,
                            std::size_t n, std::uint64_t) -> std::size_t {
            seen = d;
            return n;
        });
        r3.on_segment(client, server, 100, true, false, false, false, nullptr, 0, 0);
        r3.on_segment(server, client, 500, true, true, false, false, nullptr, 0, 0);
        r3.on_segment(server, client, 501, false, true, false, false, a, sizeof(a), 0);
        check(seen == Direction::ToClient, "handshake identifies the server side");
    }
}

// ------------------------------------------------------------- stream tap

/// The GUI records raw bytes for framing detection while simultaneously
/// decoding frames for the opcode table. Both read the same stream callback,
/// and the tap must see every byte exactly once — no gaps, no duplicates.
///
/// This models the reassembler's contract directly: it hands over the whole
/// buffer each call and keeps whatever the framer did not consume, so the
/// leftovers reappear at the front of the next call.
void test_stream_tap() {
    section("Raw stream tap");

    const auto stream = synth_stream(40, 31337);
    const auto spec = reference_spec();

    std::vector<std::uint8_t> tapped;
    std::vector<std::uint8_t> buffer;
    std::size_t already = 0;

    // Feed the stream in awkward chunks so frames straddle boundaries.
    std::mt19937 rng(9);
    std::size_t pos = 0;
    while (pos < stream.size()) {
        const std::size_t chunk = std::min<std::size_t>(stream.size() - pos, 7 + rng() % 60);
        buffer.insert(buffer.end(), stream.begin() + static_cast<std::ptrdiff_t>(pos),
                      stream.begin() + static_cast<std::ptrdiff_t>(pos + chunk));
        pos += chunk;

        if (already > buffer.size()) already = 0;
        if (buffer.size() > already)
            tapped.insert(tapped.end(),
                          buffer.begin() + static_cast<std::ptrdiff_t>(already),
                          buffer.end());

        auto scan = extract_frames(spec, buffer.data(), buffer.size(),
                                   Direction::ToClient, 0, true);
        already = buffer.size() - std::min(scan.consumed, buffer.size());
        buffer.erase(buffer.begin(),
                     buffer.begin() + static_cast<std::ptrdiff_t>(std::min(scan.consumed, buffer.size())));
    }

    check(tapped.size() == stream.size(), "tap sees every byte exactly once");
    check(tapped == stream, "tapped bytes match the original stream in order");
}

// -------------------------------------------------------------- opcode map

void test_opcode_registry() {
    section("Opcode registry");

    std::string err;
    auto fields = OpcodeRegistry::parse_field_list("id:u32,x:f32,y:f32,hp:i32", &err);
    check(fields.size() == 4, "parses a simple field list");
    check(err.empty(), "no error on valid list");
    check(fields[0].type == FieldType::U32, "u32 parsed");
    check(fields[1].type == FieldType::F32, "f32 parsed");

    fields = OpcodeRegistry::parse_field_list("a:u16be,b:i16@8,c:i16*0.125,d:skip4,e:strf6", &err);
    check(fields.size() == 5, "parses modifiers");
    check(!fields[0].little_endian, "be suffix recognised");
    check(fields[1].has_fixed_offset && fields[1].fixed_offset == 8, "@offset recognised");
    check(std::abs(fields[2].scale - 0.125) < 1e-9, "*scale recognised");
    check(fields[3].type == FieldType::Skip && fields[3].count == 4, "skipN recognised");
    check(fields[4].type == FieldType::StrFixed && fields[4].count == 6, "strfN recognised");

    OpcodeRegistry::parse_field_list("bogus", &err);
    check(!err.empty(), "missing ':' reported");
    OpcodeRegistry::parse_field_list("a:nosuchtype", &err);
    check(!err.empty(), "unknown type reported");

    // Round-trip a layout through a decode.
    OpcodeRegistry reg;
    PacketSpec spec;
    spec.opcode = 0x0405;
    spec.name = "MonsterUpdate";
    spec.semantic = Semantic::MonsterUpdate;
    spec.fields = OpcodeRegistry::parse_field_list("id:u32,x:f32,y:f32,hp:i32", nullptr);
    reg.add(spec);

    GamePacket pkt;
    pkt.opcode = 0x0405;
    pkt.payload = {
        0x39, 0x30, 0x00, 0x00,              // id = 12345
        0x00, 0x00, 0x20, 0x44,              // x  = 640.0
        0x00, 0x00, 0xF0, 0x43,              // y  = 480.0
        0xC8, 0x00, 0x00, 0x00,              // hp = 200
    };

    auto decoded = reg.decode(pkt);
    check(decoded.has_value(), "decodes a mapped packet");
    if (decoded) {
        check(decoded->get_int("id") == 12345, "u32 field value");
        check(std::abs(decoded->get_float("x") - 640.0) < 0.01, "f32 x value");
        check(std::abs(decoded->get_float("y") - 480.0) < 0.01, "f32 y value");
        check(decoded->get_int("hp") == 200, "i32 field value");
        check(decoded->semantic == Semantic::MonsterUpdate, "semantic preserved");
    }

    // A layout longer than the payload must fail, not read garbage.
    GamePacket truncated = pkt;
    truncated.payload.resize(8);
    check(!reg.decode(truncated).has_value(), "short payload rejected");

    // Unmapped opcode.
    GamePacket unknown;
    unknown.opcode = 0x9999;
    check(!reg.decode(unknown).has_value(), "unmapped opcode returns nothing");

    // Repeat groups.
    PacketSpec party;
    party.opcode = 0x0500;
    party.semantic = Semantic::PartyUpdate;
    party.repeat_count_field = "count";
    party.fields = OpcodeRegistry::parse_field_list("count:u8,id:u32,hp:i32", nullptr);
    reg.add(party);

    GamePacket pp;
    pp.opcode = 0x0500;
    pp.payload = {
        0x02,
        0x01, 0x00, 0x00, 0x00,  0x64, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,  0xC8, 0x00, 0x00, 0x00,
    };
    auto pd = reg.decode(pp);
    check(pd.has_value() && pd->groups.size() == 2, "repeat_count_field yields groups");
    if (pd && pd->groups.size() == 2) {
        check(DecodedPacket::group_int(pd->groups[0], "id") == 1, "group 0 id");
        check(DecodedPacket::group_int(pd->groups[1], "hp") == 200, "group 1 hp");
    }
}

// ------------------------------------------------------------- world model

void test_game_state() {
    section("GameState");

    GameState st;
    st.update_self_id(42);
    st.update_self_position(100, 200);
    st.update_self_hp(50, 100);
    st.update_self_mp(30, 60);

    const Self me = st.self();
    check(me.id == 42, "self id");
    check(std::abs(me.hp_pct() - 50.0f) < 0.01f, "hp percent");
    check(std::abs(me.mp_pct() - 50.0f) < 0.01f, "mp percent");

    Monster m;
    m.id = 7;
    m.pos = { 300, 200 };
    m.hp = 80;
    m.max_hp = 100;
    st.upsert_monster(m);
    check(st.monster_count() == 1, "monster added");

    // A position-only update must not wipe known HP.
    Monster pos_only;
    pos_only.id = 7;
    pos_only.pos = { 310, 200 };
    pos_only.hp = -1;
    pos_only.max_hp = -1;
    st.upsert_monster(pos_only);

    auto got = st.monster(7);
    check(got.has_value(), "monster still present");
    if (got) {
        check(std::abs(got->pos.x - 310.0f) < 0.01f, "position updated");
        check(got->hp == 80, "partial update preserves hp");
    }

    // Death removes it.
    st.remove_monster(7);
    check(st.monster_count() == 0, "monster removed");

    // A map change must clear entities; stale mobs from the old map would be
    // attacked in the new one.
    st.upsert_monster(m);
    st.set_map("NewMap", 2);
    check(st.monster_count() == 0, "map change clears entities");

    // Facing inferred from movement.
    st.update_self_position(100, 200);
    st.update_self_position(150, 200);
    check(st.self().facing == Facing::Right, "facing inferred rightward");
    st.update_self_position(50, 200);
    check(st.self().facing == Facing::Left, "facing inferred leftward");
}

// -------------------------------------------------------------- pathfinding

void test_pathfinder() {
    section("PathFinder");

    // Two ledges with a gap, and a higher shelf on the right:
    //
    //   row 3            ####      <- shelf
    //   row 6   #####  #######     <- ground, split by a gap
    MapData map;
    map.name = "test";
    map.width = 24;
    map.height = 10;
    map.tile_w = 32;
    map.tile_h = 32;
    map.attrs.assign(static_cast<std::size_t>(map.width) * map.height, Tile_Empty);

    auto set = [&](int x, int y, std::uint8_t v) {
        map.attrs[static_cast<std::size_t>(y) * map.width + x] = v;
    };

    for (int x = 0; x < 5; ++x)   set(x, 6, Tile_Ground);
    for (int x = 7; x < 14; ++x)  set(x, 6, Tile_Ground);
    for (int x = 16; x < 20; ++x) set(x, 3, Tile_Platform);

    PathFinder pf;
    check(pf.init(map), "builds the platform graph");
    check(pf.platform_count() == 3, "finds 3 platforms");

    const Vec2 left_ledge  = map.to_world(2, 5);
    const Vec2 right_ledge = map.to_world(10, 5);

    auto p1 = pf.platform_at(left_ledge);
    check(p1.has_value(), "locates the platform under a point");

    auto path = pf.find_path(left_ledge, right_ledge);
    check(!path.empty(), "finds a route across the gap");

    // Same-surface movement should not go through A*.
    auto direct = pf.find_path(left_ledge, map.to_world(4, 5));
    check(direct.size() == 1 && direct[0].kind == MoveKind::Walk,
          "same-platform move is a single walk step");

    check(pf.same_platform(left_ledge, map.to_world(4, 5)), "same_platform true within a run");
    check(!pf.same_platform(left_ledge, right_ledge), "same_platform false across a gap");

    auto h = pf.height_at(left_ledge);
    check(h.has_value(), "ground height found under a platform");

    auto over_pit = pf.height_at(map.to_world(6, 1));
    check(!over_pit.has_value(), "no ground height over a pit");

    // An invalid map must be rejected rather than producing a broken graph.
    MapData bad;
    bad.width = 10;
    bad.height = 10;      // attrs left empty
    PathFinder pf2;
    check(!pf2.init(bad), "rejects a malformed map");
}

// --------------------------------------------------------------- analyzer

namespace {

void put_u32(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

void put_f32(Bytes& b, float f) {
    std::uint32_t raw;
    std::memcpy(&raw, &f, 4);
    put_u32(b, raw);
}

const SemanticGuess* guess_for(const std::vector<SemanticGuess>& gs, std::uint16_t op) {
    for (const auto& g : gs) {
        if (g.opcode == op) return &g;
    }
    return nullptr;
}

} // namespace

/// The analyzer's whole value is that it identifies real packets rather than
/// producing plausible-looking noise. These plant packets whose meaning is
/// known and check it recovers them.
void test_protocol_analyzer() {
    section("Protocol analyzer");

    std::map<std::uint16_t, OpcodeObservations> data;

    // 0x0201 — client-sent movement: a character walking right, then back.
    // Two adjacent floats changing in small steps.
    {
        auto& o = data[0x0201];
        float x = 1200.0f;
        const float ground = 640.0f;
        for (int i = 0; i < 120; ++i) {
            // Walking, with an occasional jump: x reverses partway, y sits on
            // the floor most of the time and arcs up now and then. That is
            // what a side-scroller's own position packet actually looks like.
            const int phase = i % 30;
            const float y = (phase < 6) ? ground - static_cast<float>(6 - phase) * 8.0f
                                        : ground;
            Bytes p;
            put_u32(p, 7777);          // session id, constant
            put_f32(p, x);
            put_f32(p, y);
            o.payloads.push_back(p);
            o.timestamps_ms.push_back(static_cast<std::uint64_t>(i) * 100);
            x += (i < 60) ? 6.0f : -6.0f;
        }
        o.to_server = 120;             // client -> server
    }

    // 0x0405 — server-sent entity updates: four monsters walking, reported
    // round-robin. Each entity's own track drifts smoothly; read straight down
    // the stream the coordinate jumps between monsters, which is exactly the
    // interleaving the analyzer has to undo by grouping on the id.
    {
        auto& o = data[0x0405];
        const std::uint32_t ids[] = { 50001, 50002, 50003, 50004 };
        float x[4]  = { 800.0f, 1500.0f, 2200.0f, 300.0f };
        float y[4]  = { 640.0f, 512.0f, 700.0f, 480.0f };
        float vx[4] = { 5.0f, -4.0f, 6.0f, -3.0f };
        int   ticks[4] = { 0, 0, 0, 0 };

        for (int i = 0; i < 200; ++i) {
            const int w = i % 4;
            Bytes p;
            put_u32(p, ids[w]);
            put_f32(p, x[w]);
            put_f32(p, y[w]);
            put_u32(p, 500);
            o.payloads.push_back(p);
            o.timestamps_ms.push_back(static_cast<std::uint64_t>(i) * 80);

            // Walk, turning around every so often — a patrol, not a jitter.
            // y stays on the floor, as ground monsters do.
            x[w] += vx[w];
            if (++ticks[w] % 12 == 0) vx[w] = -vx[w];
            (void)y;
        }
        o.to_client = 200;             // server -> client only
    }

    // 0x0310 — HP: current drifts down then heals, maximum never moves.
    {
        auto& o = data[0x0310];
        std::int32_t hp = 12000;
        for (int i = 0; i < 60; ++i) {
            Bytes p;
            put_u32(p, static_cast<std::uint32_t>(hp));
            put_u32(p, 12214);         // max, constant
            o.payloads.push_back(p);
            o.timestamps_ms.push_back(static_cast<std::uint64_t>(i) * 500);
            hp -= 150;
            if (hp < 3000) hp = 12000;
        }
        o.to_client = 60;
    }

    // 0x0311 — HP *and* MP in one packet, which is the common arrangement:
    // two adjacent current/max pairs. Finding only the first would leave MP
    // unmapped and MP potions would never fire.
    {
        auto& o = data[0x0311];
        std::int32_t hp = 11593, mp = 11698;
        for (int i = 0; i < 70; ++i) {
            Bytes p;
            put_u32(p, static_cast<std::uint32_t>(hp));
            put_u32(p, 12214);                 // max hp, constant
            put_u32(p, static_cast<std::uint32_t>(mp));
            put_u32(p, 12089);                 // max mp, constant
            o.payloads.push_back(p);
            o.timestamps_ms.push_back(static_cast<std::uint64_t>(i) * 400);

            hp -= 210;  if (hp < 2500)  hp = 11593;    // taking damage, healing
            mp -= 330;  if (mp < 1800)  mp = 11698;    // casting, regenerating
        }
        o.to_client = 70;
    }

    // 0x00FF — a heartbeat: fixed payload, no information. Must not be
    // mistaken for anything.
    {
        auto& o = data[0x00FF];
        for (int i = 0; i < 40; ++i) {
            o.payloads.push_back(Bytes{ 0x01, 0x00, 0x00, 0x00 });
            o.timestamps_ms.push_back(static_cast<std::uint64_t>(i) * 1000);
        }
        o.to_client = 40;
    }

    const auto guesses = analyze_protocol(data);
    check(!guesses.empty(), "analyzer produces suggestions");

    // Print what it actually decided, so a failure says why rather than just
    // that it happened.
    for (const auto& g : guesses) {
        std::printf("         0x%04X %-16s %3d%%  %s\n",
                    g.opcode, to_string(g.semantic),
                    static_cast<int>(g.confidence * 100),
                    OpcodeRegistry::field_list_to_string(g.fields).c_str());
    }

    const SemanticGuess* move = guess_for(guesses, 0x0201);
    check(move != nullptr, "identifies the client-sent movement packet");
    if (move) {
        check(move->semantic == Semantic::SelfMove, "classifies it as self movement");
        check(move->confidence > 0.7, "high confidence on a client-sent packet");
        check(move->fields.size() >= 2, "proposes x and y fields");
        if (move->fields.size() >= 2) {
            check(move->fields[0].fixed_offset == 4, "finds x at the right offset");
            check(move->fields[1].fixed_offset == 8, "finds y at the right offset");
            check(move->fields[0].type == FieldType::F32, "reads the coordinates as floats");
        }
    }

    const SemanticGuess* mob = guess_for(guesses, 0x0405);
    check(mob != nullptr, "identifies the server-sent entity packet");
    if (mob) {
        check(mob->semantic == Semantic::MonsterUpdate, "classifies it as an entity update");
        check(mob->fields.size() >= 3, "proposes id plus coordinates");
        if (!mob->fields.empty())
            check(mob->fields[0].name == "id" && mob->fields[0].fixed_offset == 0,
                  "finds the entity id at offset 0");
    }

    const SemanticGuess* hp = guess_for(guesses, 0x0310);
    check(hp != nullptr, "identifies the HP packet");
    if (hp) {
        check(hp->semantic == Semantic::SelfHpMp, "classifies it as HP/MP");
        if (hp->fields.size() >= 2) {
            check(hp->fields[0].fixed_offset == 0, "current value at offset 0");
            check(hp->fields[1].fixed_offset == 4, "maximum at offset 4");
        }
    }

    const SemanticGuess* vitals = guess_for(guesses, 0x0311);
    check(vitals != nullptr, "identifies the combined HP+MP packet");
    if (vitals) {
        check(vitals->semantic == Semantic::SelfHpMp, "classifies it as HP/MP");
        check(vitals->fields.size() == 4, "proposes all four vitals fields, not just HP");

        bool has_hp = false, has_mp = false, has_max_hp = false, has_max_mp = false;
        for (const auto& f : vitals->fields) {
            if (f.name == "hp"     && f.fixed_offset == 0)  has_hp = true;
            if (f.name == "max_hp" && f.fixed_offset == 4)  has_max_hp = true;
            if (f.name == "mp"     && f.fixed_offset == 8)  has_mp = true;
            if (f.name == "max_mp" && f.fixed_offset == 12) has_max_mp = true;
        }
        check(has_hp && has_max_hp, "maps hp and max_hp at the right offsets");
        check(has_mp && has_max_mp, "maps mp and max_mp -- MP potions depend on this");
    }

    check(guess_for(guesses, 0x00FF) == nullptr,
          "constant heartbeat produces no false suggestion");

    // Pure noise must not yield confident claims.
    std::map<std::uint16_t, OpcodeObservations> noise;
    {
        std::mt19937 rng(4242);
        auto& o = noise[0x1234];
        for (int i = 0; i < 60; ++i) {
            Bytes p(16);
            for (auto& b : p) b = static_cast<std::uint8_t>(rng() & 0xFF);
            o.payloads.push_back(p);
            o.timestamps_ms.push_back(static_cast<std::uint64_t>(i) * 100);
        }
        o.to_client = 60;
    }
    const auto junk = analyze_protocol(noise);
    bool confident_junk = false;
    for (const auto& g : junk) {
        if (g.confidence > 0.7) confident_junk = true;
    }
    check(!confident_junk, "random bytes yield no confident classification");
}

// ---------------------------------------------------------------- map files

/// Exercises the parser against the game's own data when it is installed.
///
/// Skips rather than fails when the game is absent, so the suite still passes
/// on a machine without it — but when it is there, these check the format
/// claim against all 781 real files rather than a fixture I wrote to agree
/// with me.
void test_map_file() {
    section("Game map files");

    const std::string dir = MapFile::default_map_dir();
    if (dir.empty()) {
        std::printf("         game not installed -- skipping\n");
        return;
    }
    std::printf("         %s\n", dir.c_str());

    const auto stems = MapFile::list(dir);
    check(!stems.empty(), "finds map files");
    if (stems.empty()) return;

    // The size identity is the format claim: every file must be exactly
    // 8 + width*height*72 bytes. Checking all of them is the real test.
    int ok = 0, bad = 0;
    std::size_t widest = 0;
    for (const auto& s : stems) {
        MapFile m;
        if (m.load(MapFile::find(dir, s))) {
            ++ok;
            widest = std::max<std::size_t>(widest, static_cast<std::size_t>(m.width_cells()));
        } else {
            ++bad;
        }
    }
    std::printf("         parsed %d of %zu files (%d rejected)\n", ok, stems.size(), bad);
    check(ok > 700, "parses essentially every shipped map");
    check(bad * 50 < ok, "rejections are a rounding error, not a pattern");

    // A specific map, checked in detail.
    MapFile m;
    const std::string p = MapFile::find(dir, "t14_s0");
    if (p.empty() || !m.load(p)) {
        std::printf("         t14_s0 absent -- skipping detail checks\n");
        return;
    }

    check(m.width_cells() == 40 && m.height_cells() == 32, "t14_s0 is 40 x 32 cells");
    check(m.width_px() == 1280 && m.height_px() == 1024, "which is 1280 x 1024 px");
    check(m.surface_count() > 500, "has a substantial surface");

    // Surface heights must land inside the map, which is what proves the
    // column-major indexing: row-major puts them outside or bunched.
    bool in_bounds = true;
    int min_y = 1 << 30, max_y = -1;
    for (int layer = 0; layer < MapFile::kLayers; ++layer) {
        for (const auto& [px, py] : m.surfaces(layer)) {
            if (px < 0 || py < 0 || px >= m.width_px() || py >= m.height_px())
                in_bounds = false;
            min_y = std::min(min_y, py);
            max_y = std::max(max_y, py);
        }
    }
    check(in_bounds, "every surface point lies inside the map");
    std::printf("         surface y spans %d..%d of %d\n", min_y, max_y, m.height_px());

    // Terrain is horizontally coherent: neighbouring columns sit at similar
    // heights. Under the wrong indexing this collapses into vertical stripes.
    const auto& surf = m.surfaces(0);
    if (surf.size() > 50) {
        std::map<int, int> by_x;
        for (const auto& [px, py] : surf) by_x[px] = py;
        int adjacent = 0, smooth = 0;
        for (const auto& [px, py] : by_x) {
            auto it = by_x.find(px + 1);
            if (it == by_x.end()) continue;
            ++adjacent;
            if (std::abs(it->second - py) <= 2) ++smooth;
        }
        const double frac = adjacent ? static_cast<double>(smooth) / adjacent : 0.0;
        std::printf("         %d adjacent column pairs, %.1f%% within 2px\n",
                    adjacent, frac * 100.0);
        check(adjacent > 100, "surfaces form long horizontal runs");
        check(frac > 0.9, "neighbouring columns differ by at most a couple of pixels");
    }

    // And it must produce a grid the existing pathfinder accepts.
    const MapData md = m.to_map_data(8);
    check(md.valid(), "converts to a MapData grid");
    if (md.valid()) {
        PathFinder pf;
        check(pf.init(md), "pathfinder accepts the grid");
        check(pf.platform_count() > 0, "graph has platforms");
        std::printf("         %zu platforms from a %d x %d grid\n",
                    pf.platform_count(), md.width, md.height);
    }
}

// ------------------------------------------------------------------- macro

void test_macro_config() {
    section("Macro config");

    MacroConfig cfg;
    cfg.apply_defaults();
    check(!cfg.keys.empty(), "defaults populate some keys");
    check(cfg.require_focus, "focus requirement defaults on");
    check(cfg.focus_on_start, "starting brings the game to the front by default");

    // Staggered start offsets matter: without them every timer comes due on
    // the same tick and the coordinator drops all but one.
    bool staggered = false;
    for (std::size_t i = 1; i < cfg.keys.size(); ++i) {
        if (cfg.keys[i].start_delay_ms != cfg.keys[0].start_delay_ms) staggered = true;
    }
    check(staggered, "default timers are staggered");

    cfg.keys.resize(3);
    cfg.keys[0].label = "Test";
    cfg.keys[0].keycode = 'Q';
    cfg.keys[0].interval_ms = 1234;
    cfg.keys[0].enabled = true;
    cfg.keys[0].jitter_pct = 7;
    cfg.global_jitter_pct = 12;
    cfg.toggle_key = 122;
    cfg.focus_on_start = false;
    cfg.sequence_enabled = true;
    cfg.sequence.assign(2, MacroStep{});
    cfg.sequence[0].keycode = 'W';
    cfg.sequence[0].delay_after_ms = 321;

    const std::string path = temp_file("gobot_macro_roundtrip_test.ini");
    check(cfg.save(path), "saves");

    MacroConfig back;
    check(back.load(path), "loads");
    check(back.keys.size() == 3, "key count survives");
    if (back.keys.size() == 3) {
        check(back.keys[0].keycode == 'Q', "keycode survives");
        check(back.keys[0].interval_ms == 1234, "interval survives");
        check(back.keys[0].jitter_pct == 7, "per-key jitter survives");
        check(back.keys[0].label == "Test", "label survives");
    }
    check(back.global_jitter_pct == 12, "global jitter survives");
    check(back.toggle_key == 122, "toggle key survives");
    // Turning it off has to stick, or the macro grabs the foreground from
    // someone who explicitly asked it not to.
    check(!back.focus_on_start, "focus-on-start survives being switched off");
    check(back.sequence.size() == 2, "sequence survives");
    if (back.sequence.size() == 2)
        check(back.sequence[0].delay_after_ms == 321, "step delay survives");

    std::remove(path.c_str());
}

/// Triggers and bar geometry have to survive a save/load, or a potion key
/// quietly reverts to a timer and drinks the inventory.
void test_macro_trigger_config() {
    section("Macro trigger config");

    MacroConfig cfg;
    cfg.apply_defaults();

    bool found_hp = false;
    for (const auto& k : cfg.keys)
        if (k.trigger == MacroTrigger::HpBelow) found_hp = true;
    check(found_hp, "a default key is wired to HP");
    check(!cfg.needs_vitals(), "needs_vitals() false while those keys are off");

    cfg.keys.resize(2);
    cfg.keys[0].keycode = '1';
    cfg.keys[0].enabled = true;
    cfg.keys[0].trigger = MacroTrigger::HpBelow;
    cfg.keys[0].threshold_pct = 37;
    cfg.keys[1].keycode = '2';
    cfg.keys[1].trigger = MacroTrigger::MpBelow;
    cfg.keys[1].threshold_pct = 22;
    check(cfg.needs_vitals(), "needs_vitals() true once an enabled key depends on one");

    cfg.vitals_enabled = true;
    cfg.hp_bar = BarSpec{ 70, 38, 180, 9, 210, 45, 45, 55 };
    cfg.mp_bar = BarSpec{ 70, 52, 180, 9, 45, 110, 220, 50 };

    const std::string path = temp_file("gobot_macro_trigger_test.ini");
    check(cfg.save(path), "saves");

    MacroConfig back;
    check(back.load(path), "loads");
    if (back.keys.size() == 2) {
        check(back.keys[0].trigger == MacroTrigger::HpBelow, "HP trigger survives");
        check(back.keys[0].threshold_pct == 37, "HP threshold survives");
        check(back.keys[1].trigger == MacroTrigger::MpBelow, "MP trigger survives");
        check(back.keys[1].threshold_pct == 22, "MP threshold survives");
    } else {
        check(false, "key count survives");
    }
    check(back.vitals_enabled, "vitals flag survives");
    check(back.hp_bar.x == 70 && back.hp_bar.width == 180 && back.hp_bar.height == 9,
          "HP bar geometry survives");
    check(back.hp_bar.r == 210 && back.hp_bar.tolerance == 55, "HP bar colour survives");
    check(back.mp_bar.b == 220, "MP bar colour survives");

    // An unrecognised trigger name must fall back to a plain timer rather than
    // to a conditional key that silently never fires.
    check(macro_trigger_from_string("nonsense") == MacroTrigger::Always,
          "an unknown trigger name reads as a plain timer");
    check(macro_trigger_from_string(to_string(MacroTrigger::MpBelow)) == MacroTrigger::MpBelow,
          "trigger names round-trip");

    std::remove(path.c_str());
}

// ----------------------------------------------------------- vitals reader

Frame make_frame(int w, int h) {
    Frame f;
    f.width = w;
    f.height = h;
    f.pixels.assign(static_cast<std::size_t>(w) * h * 4, 0);
    return f;
}

void put_px(Frame& f, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (x < 0 || y < 0 || x >= f.width || y >= f.height) return;
    const std::size_t i = (static_cast<std::size_t>(y) * f.width + x) * 4;
    f.pixels[i] = b;
    f.pixels[i + 1] = g;
    f.pixels[i + 2] = r;
    f.pixels[i + 3] = 255;
}

/// Paint a bar: `filled` columns in the fill colour, the rest dark.
void paint_bar(Frame& f, const BarSpec& s, int filled,
               std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    for (int y = s.y; y < s.y + s.height; ++y) {
        for (int i = 0; i < s.width; ++i) {
            if (i < filled) put_px(f, s.x + i, y, r, g, b);
            else            put_px(f, s.x + i, y, 32, 30, 34);
        }
    }
}

void test_vitals_reader() {
    section("Vitals reader");

    BarSpec hp;
    hp.x = 20; hp.y = 10; hp.width = 100; hp.height = 6;
    hp.r = 200; hp.g = 40; hp.b = 40; hp.tolerance = 60;

    Frame f = make_frame(300, 60);

    paint_bar(f, hp, 100, 200, 40, 40);
    check(std::fabs(VitalsReader::measure(f, hp) - 100.0f) < 1.5f, "full bar reads 100%");

    paint_bar(f, hp, 60, 200, 40, 40);
    check(std::fabs(VitalsReader::measure(f, hp) - 60.0f) < 1.5f, "60/100 reads 60%");

    paint_bar(f, hp, 0, 200, 40, 40);
    check(VitalsReader::measure(f, hp) == 0.0f, "empty bar reads 0%");

    // Within tolerance: bars are drawn with a vertical gradient, so the colour
    // the user sampled is never the exact colour of every pixel.
    paint_bar(f, hp, 50, 170, 70, 55);
    check(std::fabs(VitalsReader::measure(f, hp) - 50.0f) < 1.5f,
          "a shade off still reads, within tolerance");

    // Well outside it: a blue bar must not read as a red one, or HP and MP
    // would be interchangeable and a potion rule meaningless.
    paint_bar(f, hp, 80, 40, 90, 230);
    check(VitalsReader::measure(f, hp) == 0.0f, "wrong colour reads 0%, not 80%");

    // The dangerous case. A capture that comes back blank -- the window
    // minimised, moved off screen, or a PrintWindow that painted nothing --
    // must read as unreadable, not as an empty bar: "0%" is what makes a
    // potion key fire, and firing because the reader lost the window is worse
    // than not firing at all.
    Frame white = make_frame(300, 60);
    for (int y = 0; y < white.height; ++y)
        for (int x = 0; x < white.width; ++x) put_px(white, x, y, 255, 255, 255);
    check(VitalsReader::measure(white, hp) < 0.0f, "a blank white frame is unreadable");

    Frame grey = make_frame(300, 60);
    for (int y = 0; y < grey.height; ++y)
        for (int x = 0; x < grey.width; ++x) put_px(grey, x, y, 160, 160, 165);
    check(VitalsReader::measure(grey, hp) < 0.0f, "a flat light region is unreadable");

    // But a genuinely empty bar -- a dark trough -- must still read 0%, or the
    // potion would stop working at exactly the moment it is needed.
    Frame dark = make_frame(300, 60);
    paint_bar(dark, hp, 0, 200, 40, 40);
    check(dark.valid() && VitalsReader::measure(dark, hp) == 0.0f,
          "an empty dark trough still reads 0%");

    // The decisive case. The game prints the numbers across the bar in white,
    // so the fill is interrupted by writing. A scan running forward from the
    // left stops at the first glyph and reads a full bar as two thirds; the
    // fill is found from the right instead, where the writing is just more of
    // the stuff that is not fill.
    paint_bar(f, hp, 100, 200, 40, 40);
    for (int y = hp.y + 1; y < hp.y + hp.height - 1; ++y)
        for (int i = 55; i < 80; i += 2) put_px(f, hp.x + i, y, 255, 255, 255);
    check(std::fabs(VitalsReader::measure(f, hp) - 100.0f) < 2.0f,
          "writing over the bar does not cut the reading short");

    // But a stray matching pixel out in the empty part must not stretch the
    // reading to the full width -- hence requiring a run, not one pixel.
    paint_bar(f, hp, 40, 200, 40, 40);
    for (int y = hp.y; y < hp.y + hp.height; ++y) put_px(f, hp.x + 90, y, 200, 40, 40);
    check(std::fabs(VitalsReader::measure(f, hp) - 40.0f) < 2.0f,
          "a lone matching pixel out in the empty part is ignored");

    // Garbage in: a region off the end of the frame, and an unset spec.
    BarSpec off = hp;
    off.y = 500;
    check(VitalsReader::measure(f, off) < 0.0f,
          "a region off the frame reads as unreadable, not as empty");
    check(VitalsReader::measure(f, BarSpec{}) < 0.0f, "an unset bar reads as unreadable");
    check(VitalsReader::measure(Frame{}, hp) < 0.0f, "no frame reads as unreadable");

    // The reader itself, with no window: it must start, stop, and report
    // nothing rather than a stale or made-up figure.
    VitalsReader vr;
    check(vr.hp_percent() < 0.0f, "reports unreadable before it runs");
    vr.set_hp_bar(hp);
    check(vr.hp_bar().width == 100, "remembers the bar it was given");
    vr.start();
    check(vr.running(), "starts");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    check(vr.hp_percent() < 0.0f, "still unreadable with no target window");
    vr.stop();
    check(!vr.running(), "stops");
    vr.stop();
    check(true, "double stop is safe");
}

/// Finding the bars without being told where they are.
///
/// Built to mirror the real HUD, including the parts that mislead: a second
/// red bar below HP (experience), white text drawn over the fill, and a
/// saturated patch of artwork elsewhere in the panel.
/// Draw the HUD the way the client draws it: a red bar with a blue one under
/// it, same left edge, same width, a few pixels apart, with the figures
/// printed across both.
void paint_hud(Frame& f, int x, int y) {
    const struct { int dy, fill; std::uint8_t r, g, b; } kBars[] = {
        { 0,  118, 239, 0,   0   },
        { 14, 152, 0,   49,  206 },
    };
    for (const auto& bar : kBars) {
        for (int row = 0; row < 8; ++row)
            for (int i = 0; i < 159; ++i)
                put_px(f, x + i, y + bar.dy + row,
                       static_cast<std::uint8_t>(i < bar.fill ? bar.r : 14),
                       static_cast<std::uint8_t>(i < bar.fill ? bar.g : 14),
                       static_cast<std::uint8_t>(i < bar.fill ? bar.b : 16));
        // The caption, as a white smear -- its shape does not matter here,
        // only that it interrupts the fill the way the real one does.
        for (int i = 0; i < 50; ++i)
            for (int row = 1; row < 7; ++row)
                if ((i + row) % 3) put_px(f, x + 102 + i, y + bar.dy + row, 250, 250, 250);
    }
}

/// Reading the other helper's pictures out of its assembly, with their names.
///
/// The names are the whole value. Scanning the file for PNG signatures finds
/// the same bytes and calls them t000..t356, and a picture called t137 has to
/// be identified by eye against a running game before it is good for anything
/// -- which is why the auto-login flow sat with six unidentified pages.
void test_assembly_templates() {
    section("Templates from an assembly");

    const char* dll = "C:\\Program Files\\TH_VGO_Bot_Helper\\TungHaVGOBot.dll";
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        std::printf("  [skip] the other helper is not installed here\n");
        return;
    }

    TemplateStore store;
    check(store.load_from_assembly(dll, {}), "the assembly loads");
    check(store.size() > 300, "and carries the pictures it is supposed to");

    // Named, not numbered. One of each kind this project needs, so a format
    // change that silently falls back to numbering fails here rather than
    // three features later.
    const char* kWanted[] = {
        "hpmp_indicator2",          // the HUD anchor HP/MP is measured from
        "fishing_rod",
        "pickaxe",
        "broken_pickaxe",
        "mining/npc_menu",
        "party/exp_share_1",
        "party/loot_2",
        "hoisinh_confirm",          // the revive dialog
        "inventory_empty_slot",
        "login_page",               // VTCZone's sign-in button, which stops the login flow
        "app_select_page",
        "start_game_button2",
        "game_ready_indicator",
        "channel_first_index",
        "select_char_indicator",
        "auto_login_success_indicator",
        "close_ic/close_2",
    };
    for (const char* want : kWanted)
        check(store.has(want), std::string("has '") + want + "'");

    // And they decode. A name attached to bytes that are not a picture is
    // worse than no name at all.
    const Frame* anchor = store.get("hpmp_indicator2");
    check(anchor && anchor->valid() && anchor->width > 8 && anchor->height > 8,
          "the HUD anchor decodes to a real picture");

    check(!store.has("t000"), "nothing fell back to being numbered");

    // Where the anchor sits relative to the bars, measured against a real
    // capture rather than guessed at. Printed rather than asserted: the frame
    // is whatever was last saved beside the executable and may be of anything.
    {
        const std::string shot_dir = temp_file("gobot_anchor_probe");
        CreateDirectoryA(shot_dir.c_str(), nullptr);
        const std::string copy = shot_dir + "\\shot.png";
        // The frame beside this executable, as the other tests take it, not
        // one on a path of the machine this was written on.
        if (CopyFileW(utf8_to_wide(exe_relative("frame.png")).c_str(),
                      utf8_to_wide(copy).c_str(), FALSE)) {
            TemplateStore shots;
            const Frame* shot = shots.load(shot_dir) ? shots.get("shot") : nullptr;
            if (anchor && shot && shot->valid()) {
                const auto at = TemplateMatcher::find(
                    *shot, ScreenRect{ 0, 0, shot->width, shot->height }, *anchor, 0.80f);
                if (at.found) {
                    std::printf("         anchor at %d,%d %dx%d score %.3f, frame %dx%d\n",
                                at.at.x, at.at.y, at.at.w, at.at.h,
                                static_cast<double>(at.score), shot->width, shot->height);
                    BarSpec hp, mp;
                    if (VitalsReader::find_bars(*shot, hp, mp) == 2)
                        std::printf("         HP %d,%d  MP %d,%d  ->  offsets %+d,%+d and %+d,%+d\n",
                                    hp.x, hp.y, mp.x, mp.y,
                                    hp.x - at.at.x, hp.y - at.at.y,
                                    mp.x - at.at.x, mp.y - at.at.y);
                } else {
                    std::printf("         (the saved frame does not show this HUD)\n");
                }
            }
            DeleteFileA(copy.c_str());
        }
        RemoveDirectoryA(shot_dir.c_str());
    }
}

/// Inviting whoever asks for it in chat.
///
/// A player types the word, the bot invites them. Everything hard about it is
/// in the exceptions, and those are what is tested: the word has to be a word
/// and not part of one, the same request must not invite somebody twice, and a
/// line read wrongly once must not count.
void test_party_chat() {
    section("Inviting from chat");

    // Splitting "Name: message".
    {
        PartyChat::Line line;
        check(PartyChat::split("Someone: pt pls", line) &&
              line.who == "Someone" && line.said == "pt pls",
              "a chat line splits into who and what");
        check(PartyChat::split("  Spaced  :   pt  ", line) &&
              line.who == "Spaced" && line.said == "pt",
              "and the spacing around it does not matter");
        check(!PartyChat::split("no colon here", line),
              "a line with no colon is not somebody speaking");
        check(!PartyChat::split(": orphaned", line), "nor is one with no name");
        check(!PartyChat::split("Nobody:", line), "nor a name that said nothing");
        check(!PartyChat::split("", line), "nor an empty line");
    }

    // The keyword has to be a word.
    {
        PartyChat::Settings s;              // keyword "pt", whole words only
        check(PartyChat::asks_to_join("pt", s), "the bare word asks to join");
        check(PartyChat::asks_to_join("cho pt voi", s), "and so does it in a sentence");
        check(PartyChat::asks_to_join("PT", s), "regardless of case");
        check(PartyChat::asks_to_join("pt!", s), "with punctuation after it");

        // The one that matters: a word merely containing the keyword is not a
        // request, or anybody saying "spotted" gets invited.
        check(!PartyChat::asks_to_join("spotted a boss", s),
              "a longer word containing it does not");
        check(!PartyChat::asks_to_join("optional", s), "nor one containing it inside");
        check(!PartyChat::asks_to_join("", s), "nor an empty message");

        s.whole_word_only = false;
        check(PartyChat::asks_to_join("captain", s),
              "unless whole words are turned off, which is the caller's business");
    }

    // Names that nearly match are the same player.
    {
        check(PartyChat::alike("Someone", "Someone") > 0.99, "a name matches itself");
        check(PartyChat::alike("Someone", "Somene") > 0.8,
              "and a name with a letter dropped is still close");
        check(PartyChat::alike("[Tag]Someone", "Someone") > 0.6,
              "punctuation is not what tells two names apart");
        check(PartyChat::alike("Someone", "Different") < 0.6,
              "while two real names are not close");
        check(PartyChat::alike("", "") > 0.99, "two nothings are alike");
        check(PartyChat::alike("A", "") < 0.01, "something and nothing are not");
    }

    // Tự chat's own lines, read back off the chat, are not requests.
    {
        const std::string advert = "tuyen pt lv 50 ban do gia re";
        check(PartyChat::echoes("[Kenh]MyChar : tuyen pt lv 50 ban do gia re", advert),
              "our own advert read back, channel and name in front, is ours");
        check(PartyChat::echoes("l l KenhlMyChar tuyen pt lv 50 ban d", advert),
              "and so is it cut off where the box wrapped it, with the reader's mess in front");
        check(PartyChat::echoes("do gia re", advert), "and the rest of it, on a line of its own");
        check(PartyChat::echoes("[Kenh]MyChar : tuyen pt lv 5O ban do gia re", advert),
              "a letter misread still leaves eight in a row");
        check(!PartyChat::echoes("[Kenh]Someone : pt", advert),
              "somebody's own 'pt' is not ours");
        check(!PartyChat::echoes("[Kenh]Someone : cho pt voi, lv 50", advert),
              "nor a request with a word or two of ours in it");
        check(PartyChat::echoes("[Kenh]MyChar : tim pt", "tim pt"),
              "a short line of ours is recognised whole");
        check(!PartyChat::echoes("[Kenh]Someone : tuyen thu pt", "tim pt"),
              "while a long line holding its letters in order is not it");
        check(!PartyChat::echoes("[Kenh]Someone : pt", "pt"),
              "and echoes alone cannot tell a line of ours that is only the keyword from a request -- own_line can");
    }

    // The player's own lines, whatever their length, are never requests
    // (PartyChat::own_line): on 2026-10-20 a Tự chat line that was just the
    // keyword came back as the player asking. An invented name, MyChar.
    {
        PartyChat::Settings s;      // keyword "pt"
        const std::string me = "MyChar";
        using Typed = std::vector<std::pair<std::string, std::uint64_t>>;
        const Typed none;
        const std::uint64_t now = 10'000'000;
        const Typed pt30 = { { "pt", now - 30'000 } }, pt61 = { { "pt", now - 61'000 } };
        check(PartyChat::own_line("[Kenh]MyChar : pt", me, none, now, s) &&
                  PartyChat::own_line("[Kenh]MyChar : tim pt lv 50", me, none, now, s),
              "said by the player's own character: ours, the keyword alone or a longer line, nothing typed on record");
        check(PartyChat::own_line("l l KenhlMyChar pt", me, none, now, s),
              "and with the colon lost and the reader's mess in front");
        check(PartyChat::own_line("[Kenh]MyCha : pt", me, none, now, s),
              "and with a letter of the name dropped by the reader");
        check(!PartyChat::own_line("[Kenh]Someone : pt", me, none, now, s),
              "somebody else's keyword, nothing typed: a request");
        check(!PartyChat::own_line("[Kenh]TuanAnh : pt", "Anh", none, now, s) &&
                  !PartyChat::own_line("[Kenh]Anh2 : pt", "Anh", none, now, s) &&
                  PartyChat::own_line("[Kenh]Anh : pt", "Anh", none, now, s),
              "a short name only exactly: TuanAnh and Anh2 are not Anh, Anh is");
        check(PartyChat::own_line("[Kenh]anh : pt", "lanh", none, now, s),
              "a name that starts with l, whose l the reader's cleaning ate, is still the player's");
        check(!PartyChat::own_line("[Kenh]Someone : pt", "", none, now, s),
              "no name set on Đánh quái: the speaker never makes a line ours");
        check(PartyChat::own_line("[Kenh]Someone : pt", "", pt30, now, s) &&
                  !PartyChat::own_line("[Kenh]Someone : pt", "", pt61, now, s),
              "the keyword typed by GoBot 30 s ago: a line of just the keyword is ours, whoever it reads as; "
              "61 s ago, one line on its own no longer says so -- without_own carries it on (below)");
        check(!PartyChat::own_line("[Kenh]Someone : cho pt voi", "", pt30, now, s),
              "but a sentence with the keyword in it is a request all the same");
        const std::string advert = "tuyen pt lv 50 ban do gia re";
        check(PartyChat::own_line("[Kenh]Someone : tuyen pt lv 50 ban do gia re", "",
                                  Typed{ { advert, now - 20 * 60'000ull } }, now, s) &&
                  !PartyChat::own_line("[Kenh]Someone : tuyen pt lv 50 ban do gia re", "",
                                       Typed{ { advert, now - 31 * 60'000ull } }, now, s),
              "a long advert of ours read back: ours 20 minutes on, not 31");
        check(PartyChat::own_line("[Kenh]MyChar2 : pt", me, none, now, s),
              "the cost, as told to the player: a name of five or more, ours with one letter more, counts as ours");
        // The notes' claims about names (DOC-TRUOC.txt, START-HERE), as the code does it: only plain letters
        // a-z and digits are compared, case aside -- a letter with a Vietnamese mark, and a leading l, I or
        // L, are left out of both names, and out of the count. With five or more left, a name a letter
        // longer or shorter at most and 80% alike counts as the player's; with two to four, only exactly;
        // with fewer than two, the name is not compared at all, as if none were set. Made-up names.
        check(PartyChat::own_line("[Kenh]Kopa : pt", "Kopra", none, now, s) &&
                  PartyChat::own_line("[Kenh]Kopra2 : pt", "Kopra", none, now, s) &&
                  !PartyChat::own_line("[Kenh]Kxxra : pt", "Kopra", none, now, s),
              "a name of five letters: one letter short or over is the player's, two letters wrong is not");
        check(PartyChat::own_line("[Kenh]Zemo : pt", "Zemo", none, now, s) &&
                  !PartyChat::own_line("[Kenh]Zemo2 : pt", "Zemo", none, now, s) &&
                  !PartyChat::own_line("[Kenh]Zmo : pt", "Zemo", none, now, s),
              "a name of four letters: only exactly");
        check(PartyChat::own_line("[Kenh]Bọxar : pt", "Bọxar", none, now, s) &&
                  !PartyChat::own_line("[Kenh]Bọxar2 : pt", "Bọxar", none, now, s) &&
                  PartyChat::own_line("[Kenh]Boxar2 : pt", "Boxar", none, now, s),
              "a letter with a Vietnamese mark does not count towards the five: Bọxar is four letters, Boxar five");
        check(PartyChat::own_line("[Kenh]Bẫxar : pt", "Bấxar", none, now, s),
              "and two names that differ only in a letter with a mark are the same name");
        check(PartyChat::own_line("[Kenh]Kopa8 : pt", "Kopa9", none, now, s) &&
                  !PartyChat::own_line("[Kenh]Lopa8 : pt", "Lopa9", none, now, s) &&
                  PartyChat::own_line("[Kenh]Lopa9 : pt", "Lopa9", none, now, s),
              "a leading L does not count towards the five either (R8bR-3): Kopa8 is Kopa9's, while Lopa9 is four "
              "letters and digits, and only Lopa9 itself is the player's");
        check(PartyChat::own_line("[Kenh]Xo : pt", "Xo", none, now, s) &&
                  !PartyChat::own_line("[Kenh]Xo2 : pt", "Xo", none, now, s) &&
                  !PartyChat::own_line("[Kenh]Lq : pt", "Lq", none, now, s) &&
                  !PartyChat::own_line("[Kenh]Ẩq : pt", "Ẩq", none, now, s),
              "two letters left: only exactly; fewer than two (Lq, Ẩq) and the name is not compared at all, as if "
              "none were set (R8bR-3)");
        // End to end: what reaches the votes. The player's own keyword line,
        // seen on three looks, never makes a requester; somebody else's does.
        PartyChat chat;
        std::vector<std::string> asked;
        for (int i = 0; i < 3; ++i) {
            const std::uint64_t t = now + 150ull * static_cast<std::uint64_t>(i);
            const auto lines = chat.without_own({ "[Kenh]MyChar : pt", "[Kenh]Someone : pt" }, me, none, t);
            for (const std::string& who : chat.saw(lines, t)) asked.push_back(who);
        }
        check(asked.size() == 1 && asked[0] == "Someone",
              "three looks at both lines: Someone asks, the player's own line never");
#ifdef GOBOT_SOURCE_DIR
        // The party worker goes by it, not by echoes alone.
        std::ifstream in(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/gui_app.cpp", std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::size_t loop = src.find("void GuiApp::party_loop()");
        const std::size_t own = src.find("chat.without_own(lines", loop);
        const std::size_t saw = src.find("chat.saw(lines", loop);
        check(loop != std::string::npos && own != std::string::npos && saw != std::string::npos && own < saw,
              "gui_app.cpp: party_loop takes the player's own lines out (without_own) before the votes");
#endif
    }

    // The keyword GoBot typed stays on the chat for minutes: on 2026-10-20 it
    // was still the top line 2 and 3 minutes after it was typed, and was taken
    // for the player asking 193 s after (R8R-1). Through the party worker's
    // steps on every look -- without_own, then the votes -- a look every
    // 200 ms. Invented names.
    {
        PartyChat::Settings s;      // keyword "pt", 3 votes, 30 s cooldown
        using Typed = std::vector<std::pair<std::string, std::uint64_t>>;
        const std::uint64_t typed_at = 50'000'000;
        const Typed typed = { { "pt", typed_at } };
        // The first look, in ms after the keyword was typed, at which somebody
        // is taken for asking -- and who; -1 never. `on_chat` is what the chat
        // shows at a time.
        struct Asked {
            long long at = -1;
            std::string who;
        };
        const auto first_ask = [&](PartyChat& chat, const std::function<std::vector<std::string>(std::uint64_t)>& on_chat,
                                   const std::string& own, const Typed& said, std::uint64_t from_ms,
                                   std::uint64_t to_ms) {
            Asked a;
            for (std::uint64_t t = typed_at + from_ms; t < typed_at + to_ms; t += 200) {
                const auto lines = chat.without_own(on_chat(t), own, said, t);
                const auto who = chat.saw(lines, t);
                if (!who.empty()) {
                    a.at = static_cast<long long>(t - typed_at);
                    a.who = who[0];
                    return a;
                }
            }
            return a;
        };
        const auto fresh = [&] {
            PartyChat c;
            c.set_settings(s);
            return c;
        };
        const auto only = [](const char* line) {
            return [line](std::uint64_t) { return std::vector<std::string>{ line }; };
        };
        {
            PartyChat chat = fresh();
            const Asked a = first_ask(chat, only("[Kenh]MyChar : pt"), "", typed, 1000, 240'000);
            check(a.at < 0, "no name set: the player's own keyword line on the chat from 1 s to 240 s after GoBot "
                            "typed it is never taken for asking" +
                                (a.at < 0 ? std::string() : " -- taken at " + std::to_string(a.at) + " ms"));
        }
        {
            PartyChat chat = fresh();
            const Asked a = first_ask(chat, only("[Kenh]Mlnh : pt"), "Minh", typed, 1000, 240'000);
            PartyChat other = fresh();
            const Asked b = first_ask(other, only("[Kenh]MvChor : pt"), "MyChar", typed, 1000, 240'000);
            check(a.at < 0 && b.at < 0,
                  "nor with a four-letter name read a letter wrong, nor a six-letter one read two letters wrong");
        }
        {
            // A quiet channel: nothing said after it for 40 minutes, the line
            // still the last on the chat.
            PartyChat chat = fresh();
            const Asked a = first_ask(chat, only("[Kenh]MyChar : pt"), "", typed, 1000, 40 * 60'000);
            check(a.at < 0, "nor 40 minutes on, in a channel where nothing else was said");
        }
        {
            // Somebody else's bare keyword after the first minute is asking:
            // the player's line is told by the speaker it was read with.
            PartyChat chat = fresh();
            const auto shows = [](std::uint64_t t) {
                std::vector<std::string> lines = { "[Kenh]MyChar : pt" };
                if (t >= typed_at + 90'000) lines.push_back("[Kenh]Someone : pt");
                return lines;
            };
            const Asked a = first_ask(chat, shows, "", typed, 1000, 240'000);
            check(a.who == "Someone" && a.at >= 90'000 && a.at < 91'000,
                  "somebody else's keyword alone, 90 s after GoBot typed it, the player's still on the chat: "
                  "Someone asks, on the third look");
        }
        {
            // The party worker started 2 minutes after the line was typed: it
            // never saw the line's first minute, and has nothing to tell the
            // player's line by -- so a bare keyword is passed over, until GoBot
            // types it again and the worker sees that line in its first minute.
            PartyChat chat = fresh();
            const auto before = [](std::uint64_t t) {
                std::vector<std::string> lines = { "[Kenh]MyChar : pt" };
                if (t >= typed_at + 150'000) lines.push_back("[Kenh]Someone : pt");
                return lines;
            };
            const Asked late = first_ask(chat, before, "", typed, 120'000, 300'000);
            const Typed again = { { "pt", typed_at }, { "pt", typed_at + 300'000 } };
            const auto after_it = [](std::uint64_t t) {
                std::vector<std::string> lines = { "[Kenh]MyChar : pt" };
                if (t >= typed_at + 380'000) lines.push_back("[Kenh]Someone : pt");
                return lines;
            };
            const Asked after = first_ask(chat, after_it, "", again, 300'200, 480'000);
            check(late.at < 0 && after.who == "Someone" && after.at >= 380'000 && after.at < 381'000,
                  "the worker started 2 minutes late: nobody's bare keyword taken for asking until GoBot typed the "
                  "keyword again and saw its line; then Someone saying it asks");
        }
        {
            // How long that lasts, as the notes say (DOC-TRUOC.txt, START-HERE): until GoBot types the
            // keyword again, and 30 minutes at most from the last time it did.
            PartyChat chat = fresh();
            const Asked a = first_ask(chat, only("[Kenh]Someone : pt"), "", typed, 120'000, 40 * 60'000);
            check(a.who == "Someone" && a.at >= 30 * 60'000 && a.at < 30 * 60'000 + 1000,
                  "nothing learned in the first minute: a bare keyword is passed over for 30 minutes after GoBot typed "
                  "it, and asks after (at +" + std::to_string(a.at) + " ms)");
        }
        {
            // Somebody saying the keyword alone in the first minute is taken
            // for the player too, while that line stays on the chat; once it
            // has been off the chat a minute of looks, they ask as anybody.
            PartyChat chat = fresh();
            const auto shows = [](std::uint64_t t) {
                if (t < typed_at + 40'000) return std::vector<std::string>{ "[Kenh]MyChar : pt", "[Kenh]Someone : pt" };
                if (t < typed_at + 200'000) return std::vector<std::string>{ "[Kenh]Other : hello", "[Kenh]Other : hi" };
                return std::vector<std::string>{ "[Kenh]Other : hi", "[Kenh]Someone : pt" };
            };
            const Asked a = first_ask(chat, shows, "", typed, 1000, 300'000);
            check(a.who == "Someone" && a.at >= 200'000 && a.at < 201'000,
                  "somebody's bare keyword in the first minute: passed over while it is on the chat, and asking "
                  "when they say it again after it has been gone a minute");
        }
        {
            // The chat out of sight -- nothing read -- for three minutes does
            // not make the player's line forgotten.
            PartyChat chat = fresh();
            const auto shows = [](std::uint64_t t) {
                if (t >= typed_at + 5'000 && t < typed_at + 200'000) return std::vector<std::string>{};
                return std::vector<std::string>{ "[Kenh]MyChar : pt" };
            };
            const Asked a = first_ask(chat, shows, "", typed, 1000, 300'000);
            check(a.at < 0 && chat.own_speakers().size() == 1,
                  "the chat out of sight for three minutes: the player's line, back in view, is still the player's");
        }
        {
            // With a name set, a bare keyword from a speaker nothing like it is
            // passed over in the first minute only, and not remembered.
            PartyChat chat = fresh();
            const auto both = [](std::uint64_t) {
                return std::vector<std::string>{ "[Kenh]MyChar : pt", "[Kenh]Someone : pt" };
            };
            const Asked a = first_ask(chat, both, "MyChar", typed, 1000, 240'000);
            const auto kept = chat.own_speakers();
            check(a.who == "Someone" && a.at >= 60'000 && a.at < 61'000 && kept.size() == 1 && kept[0] == "mychar",
                  "a name set: somebody unlike it saying the keyword alone in the first minute is passed over then, "
                  "not remembered, and asking after");
        }
        {
            // A name with fewer than two plain letters and digits left -- marked letters and a leading l, I
            // or L taken off -- is as none set (R8bR-3): every speaker of the keyword alone in the first
            // minute is taken for the player's.
            PartyChat chat = fresh();
            const auto both = [](std::uint64_t) {
                return std::vector<std::string>{ "[Kenh]MyChar : pt", "[Kenh]Someone : pt" };
            };
            const Asked a = first_ask(chat, both, "Ẩq", typed, 1000, 240'000);
            check(a.at < 0 && chat.own_speakers().size() == 2,
                  "a name of fewer than two plain letters and digits (Ẩq) is as none: both speakers of the keyword "
                  "alone in the first minute are taken for the player's");
        }
        {
            // reset() forgets what was learned.
            PartyChat chat = fresh();
            chat.without_own({ "[Kenh]MyChar : pt" }, "", typed, typed_at + 1000);
            const bool learned = chat.own_speakers().size() == 1;
            chat.reset();
            check(learned && chat.own_speakers().empty(), "the speaker taken for the player is forgotten by reset()");
        }
    }

    // ---- round 8b (R8bR-1): a party watcher in a process that did not type the line -- the GUI started
    //      again while the player's line is still on the chat, or started after gobot_cli --chat; gobot_cli
    //      --autoparty beside the GUI's Tự chat -- knows it from SaidLately's file, written just before
    //      every sending Enter. Invented names, a made-up clock, and a file of this run's own. ----
    {
        using Line = SaidLately::Line;
        // A machine started at `started` by the system clock: now_ms() counts from then.
        const std::uint64_t started = 1'790'000'000'000ull;
        const std::uint64_t now = 7'200'000;        // two hours on
        const auto wall_at = [started](std::uint64_t steady) { return started + steady; };

        // The rows.
        const Line pt{ "pt", now - 20'000, wall_at(now - 20'000) };
        const std::vector<Line> back = SaidLately::parse(SaidLately::format(pt));
        check(back.size() == 1 && back[0].text == "pt" && back[0].steady_ms == pt.steady_ms &&
                  back[0].wall_ms == pt.wall_ms,
              "said_lately.txt: a row reads back as it was written");
        const std::vector<Line> odd = SaidLately::parse(SaidLately::format(Line{ "two\nrows", 5, 6 }) + "junk\n" +
                                                        "12 x pt\n" + "12 34\n" + "1 2 ok\r\n" + "3 4 half");
        check(odd.size() == 2 && odd[0].text == "two rows" && odd[1].text == "ok" && odd[1].steady_ms == 1 &&
                  odd[1].wall_ms == 2,
              "a line end in the text becomes a space; rows that do not read, and an unfinished last one, are passed "
              "over");

        // Which rows are still of use: of the last half hour, on this start of the machine.
        const std::vector<Line> rows = {
            { "old", now - 31 * 60'000ull, wall_at(now - 31 * 60'000ull) },
            { "pt", now - 20'000, wall_at(now - 20'000) },
            // Written on the start before this one, five hours earlier, near its own two-hour mark.
            { "before", now - 60'000, started - 5 * 3'600'000ull + (now - 60'000) },
            { "ahead", now + 5'000, wall_at(now + 5'000) },
            { "drift", now - 30'000, wall_at(now - 30'000) + 9'000 },
            { "set", now - 40'000, wall_at(now - 40'000) + 15'000 },
        };
        const std::vector<Line> kept = SaidLately::kept(rows, now, wall_at(now));
        check(kept.size() == 2 && kept[0].text == "drift" && kept[1].text == "pt",
              "rows kept: those of the last half hour on this start of the machine, oldest first -- not one over "
              "half an hour old, one from before the machine was started again, one ahead of the clock, nor one "
              "whose system clock was 15 s off (9 s is the same start)");
        std::vector<Line> many;
        for (int i = 0; i < 25; ++i)
            many.push_back(Line{ "l" + std::to_string(i), now - 25'000 + 1000ull * i, wall_at(now - 25'000 + 1000ull * i) });
        const std::vector<Line> newest = SaidLately::kept(many, now, wall_at(now));
        check(newest.size() == SaidLately::kKeep && newest.front().text == "l5" && newest.back().text == "l24",
              "and the newest 20 of them at most");

        // The file.
        const std::string path = temp_file("said_lately_test.txt");
        remove_file(path);
        const SaidLately file(path);
        check(file.read(now, wall_at(now)).empty(), "no file: nothing typed on record");
        const std::string advert = "tuyen pt lv 50 ban do gia re";
        const bool added = file.add("pt", now - 20'000, wall_at(now - 20'000)) &&
                           file.add(advert, now - 10'000, wall_at(now - 10'000));
        const SaidLately::Said got = file.read(now, wall_at(now));
        check(added && got.size() == 2 && got[0].first == "pt" && got[0].second == now - 20'000 &&
                  got[1].first == advert && got[1].second == now - 10'000,
              "two lines added, and read back each with its time on the steady clock");
        bool all_added = true;
        for (int i = 0; i < 300; ++i)
            all_added = file.add("line " + std::to_string(i), now - 300 + static_cast<std::uint64_t>(i),
                                 wall_at(now - 300 + static_cast<std::uint64_t>(i))) &&
                        all_added;
        std::ifstream sized(path, std::ios::binary | std::ios::ate);
        const long long size = static_cast<long long>(sized.tellg());
        sized.close();
        const SaidLately::Said last = file.read(now, wall_at(now));
        check(all_added && size > 0 && size <= SaidLately::kCutAt + 200 && last.size() == SaidLately::kKeep &&
                  last.back().first == "line 299",
              "300 lines more: the file is cut down as it grows (" + std::to_string(size) +
                  " bytes), and reads the newest 20");
        remove_file(path);
        {
            // Where the GUI and gobot_cli keep it: outside the program folder, as the webhook's file is -- a
            // new version unzipped into a new folder finds it. Only the name is looked at here.
            wchar_t env[MAX_PATH * 2] = {};
            const DWORD room = static_cast<DWORD>(MAX_PATH * 2);
            const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", env, room);
            const std::string where = SaidLately::default_path();
            check(n > 0 && n < room && where == wide_to_utf8(std::wstring(env) + L"\\GoBot\\said_lately.txt") &&
                      where.rfind(exe_relative(""), 0) != 0,
                  "the file is %LOCALAPPDATA%\\GoBot\\said_lately.txt, outside the program folder");
        }

        // End to end: the watcher of a process started 20 s, 90 s or 5 minutes after the line was typed in
        // another, looking every 150 ms (party.ini's vote_gap_ms as shipped), no name set (as shipped),
        // with what was typed read from the file on every look as the GUI and gobot_cli --autoparty read
        // it -- and (control) with nothing typed on record, as before this round.
        struct Asked {
            long long at = -1;
            std::string who;
        };
        const auto watch = [&](const std::function<std::vector<std::string>(std::uint64_t)>& on_chat,
                               const SaidLately* said, std::uint64_t from, std::uint64_t to) {
            PartyChat chat;
            chat.set_settings(PartyChat::Settings{});
            Asked a;
            for (std::uint64_t t = from; t < to; t += 150) {
                const SaidLately::Said typed = said != nullptr ? said->read(t, wall_at(t)) : SaidLately::Said{};
                const auto who = chat.saw(chat.without_own(on_chat(t), "", typed, t), t);
                if (!who.empty()) {
                    a.at = static_cast<long long>(t - from);
                    a.who = who[0];
                    return a;
                }
            }
            return a;
        };
        const std::string e2e_path = temp_file("said_lately_e2e.txt");
        const SaidLately e2e(e2e_path);
        struct Kind {
            const char* said;
            const char* on_chat;
        };
        const Kind kinds[] = { { "pt", "[Kenh]MyChar : pt" },
                               { "tuyen pt lv 50 ban do gia re", "[Kenh]MyChar : tuyen pt lv 50 ban do gia re" } };
        const std::uint64_t typed_at = 9'000'000;
        for (const Kind& kind : kinds)
            for (const std::uint64_t later : { 20'000ull, 90'000ull, 5 * 60'000ull }) {
                remove_file(e2e_path);
                const bool wrote = e2e.add(kind.said, typed_at, wall_at(typed_at));
                const std::uint64_t start = typed_at + later;
                const std::string own = kind.on_chat;
                const auto own_only = [&own](std::uint64_t) {
                    return std::vector<std::string>{ "[Kenh]Other : hello all", own };
                };
                const std::string tag = std::string("\"") + kind.said + "\" typed by another process " +
                                        std::to_string(later / 1000) + " s before the watcher's first look: ";
                const Asked with = watch(own_only, &e2e, start, start + 240'000);
                check(wrote && with.at < 0, tag + "with the file, the player's own line is never taken for asking" +
                                                (with.at < 0 ? std::string()
                                                             : " -- taken at +" + std::to_string(with.at) + " ms"));
                const Asked without = watch(own_only, nullptr, start, start + 240'000);
                check(without.who == "MyChar" && without.at >= 0 && without.at <= 300,
                      tag + "(control) without it, the same line is taken for the player asking, at +" +
                          std::to_string(without.at) + " ms");
                // Somebody else's keyword alone, 90 s into the watch, still asks where the watcher could tell the
                // player's line by its speaker (the keyword's first minute seen) or by its words (the advert).
                if (later == 20'000 || kind.said != std::string("pt")) {
                    const Asked other = watch(
                        [&own, start](std::uint64_t t) {
                            std::vector<std::string> lines = { "[Kenh]Other : hello all", own };
                            if (t >= start + 90'000) lines.push_back("[Kenh]Someone : pt");
                            return lines;
                        },
                        &e2e, start, start + 240'000);
                    check(other.who == "Someone" && other.at >= 90'000 && other.at < 91'000,
                          tag + "and somebody else's keyword alone, 90 s into the watch, asks (at +" +
                              std::to_string(other.at) + " ms)");
                }
            }
        // A row from before the machine was started again does not count: that line is long gone from the
        // game.
        remove_file(e2e_path);
        const bool wrote_before = e2e.add("pt", typed_at - 10'000, started - 5 * 3'600'000ull + typed_at - 10'000);
        const auto mine = [](std::uint64_t) { return std::vector<std::string>{ "[Kenh]MyChar : pt" }; };
        const Asked stale = watch(mine, &e2e, typed_at, typed_at + 60'000);
        check(wrote_before && stale.who == "MyChar" && stale.at >= 0 && stale.at <= 300,
              "a row of the machine's start before this one is not on record: its line is taken as anybody's");
        remove_file(e2e_path);
    }

    // The voting. One reading is not enough; three agreeing is.
    {
        PartyChat chat;
        PartyChat::Settings s;
        s.votes = 3;
        chat.set_settings(s);

        std::uint64_t now = 100000;
        check(chat.saw({ "Someone: pt" }, now).empty(), "one look is not enough");
        now += 150;
        check(chat.saw({ "Someone: pt" }, now).empty(), "nor two");
        now += 150;
        const auto ready = chat.saw({ "Someone: pt" }, now);
        check(ready.size() == 1 && ready[0] == "Someone", "three agreeing is enough");
    }

    // A misread that does not repeat never reaches three.
    {
        PartyChat chat;
        PartyChat::Settings s;
        s.votes = 3;
        chat.set_settings(s);

        std::uint64_t now = 100000;
        check(chat.saw({ "Ghost: pt" }, now).empty(), "a one-off reading counts once");
        now += 150;
        check(chat.saw({ "Someone: pt" }, now).empty(), "a different name starts its own count");
        now += 150;
        check(chat.saw({ "Someone: pt" }, now).empty(),
              "and the one-off does not lend its vote to it");
    }

    // Votes go stale, so a name seen once an hour ago does not combine with
    // one seen now.
    {
        PartyChat chat;
        PartyChat::Settings s;
        s.votes = 2;
        s.vote_gap_ms = 150;
        chat.set_settings(s);

        std::uint64_t now = 100000;
        check(chat.saw({ "Someone: pt" }, now).empty(), "seen once");
        now += 60 * 1000;      // an hour later, as far as this is concerned
        check(chat.saw({ "Someone: pt" }, now).empty(),
              "and seen again much later, which does not make two");
    }

    // Having been invited, the same player is left alone for a while -- their
    // line stays on screen and would otherwise be read on every pass.
    {
        PartyChat chat;
        PartyChat::Settings s;
        s.votes = 1;
        s.cooldown_s = 30;
        chat.set_settings(s);

        std::uint64_t now = 100000;
        check(chat.saw({ "Someone: pt" }, now).size() == 1, "invited the first time");
        now += 1000;
        check(chat.saw({ "Someone: pt" }, now).empty(),
              "and not again a second later, with the line still on screen");

        const auto waiting = chat.cooling(now);
        check(waiting.size() == 1 && waiting.begin()->second > 25,
              "and says who is waiting, and for how long");

        now += 31 * 1000;
        check(chat.saw({ "Someone: pt" }, now).size() == 1,
              "but again once the cooldown has passed");
    }

    // Several people asking at once are all picked up.
    {
        PartyChat chat;
        PartyChat::Settings s;
        s.votes = 1;
        chat.set_settings(s);
        const auto ready = chat.saw({ "Alice: pt", "Bob: hello", "Carol: pt please" },
                                    100000);
        check(ready.size() == 2, "two of three asked -- got " +
                                 std::to_string(ready.size()));
    }
}

/// Parties.
///
/// The detecting is the other helper's: its PartyStatusDetector takes the
/// game's window and answers, and its PartyConfigureDetector works out where
/// the sharing dropdowns are. What is ours is reading that answer and deciding
/// who may be invited, and those are what is tested here -- all of it without
/// .NET, without that helper installed, and without a game running.
void test_party() {
    section("Party");

    // Off until a file says otherwise. A party.ini that is missing, or held
    // by another program as it is read, left the code's own "on" in force:
    // the tab showed both boxes ticked, and the worker took anyone's
    // invitation and invited whoever typed the keyword.
    {
        const Party::Settings fresh;
        check(!fresh.auto_accept && !fresh.auto_invite,
              "accepting and inviting are off when no file has said otherwise");
        Party::Settings missing;
        check(!missing.load(temp_file("gobot_test_no_party.ini")) && !missing.auto_accept &&
                  !missing.auto_invite,
              "and a party.ini that cannot be read turns neither on");
    }

    // Reading the bridge's answer. This is the exact text that came back from
    // the live client, so a change in their field names fails here rather than
    // in the middle of a party.
    {
        const std::string reply =
            "status=Alone\n"
            "is_player_have_party=0\n"
            "is_player_key_party=0\n"
            "can_auto_accept=1\n"
            "can_auto_invite=1\n"
            "can_transfer_party_key=0\n"
            "client_width=1024\n"
            "exp_dropdown=777,76\n"
            "loot_dropdown=727,76\n"
            "exp_equal=777,116\n"
            "exp_more=777,152\n"
            "loot_free=727,116\n"
            "loot_contribution=727,152\n";

        const auto state = Party::parse(reply);
        check(state.read, "the detector's answer parses");
        check(state.status == Party::Status::Alone, "alone is read as alone");
        check(!state.in_party && !state.is_leader, "and so is being in no party");
        check(state.can_accept && state.can_invite && !state.can_transfer_key,
              "their own rules come through unchanged");
        check(state.client_width == 1024, "the client width survives");
        check(state.points.size() == 6, "all six click points are read -- got " +
                                        std::to_string(state.points.size()));

        int x = 0, y = 0;
        check(state.point("exp_more", x, y) && x == 777 && y == 152,
              "and each is where their code put it");
    }

    // Their status word and their two flags can disagree. The flags are the
    // ones that mean something.
    {
        const auto state = Party::parse(
            "status=InParty\nis_player_have_party=1\nis_player_key_party=1\n");
        check(state.status == Party::Status::Leader,
              "holding the key means leading, whatever the status word says");
        check(state.in_party, "and being in one");
    }
    {
        const auto state = Party::parse(
            "status=Alone\nis_player_have_party=1\nis_player_key_party=0\n");
        check(state.status == Party::Status::InParty,
              "being in one outranks a status word that says otherwise");
    }

    // Failures are reported, not guessed at.
    {
        check(!Party::parse("").read, "an empty answer is not a reading");
        check(!Party::parse("error=no PartyStatusDetector").read,
              "and neither is an error");
        check(Party::parse("error=no detector").trouble.find("no detector") !=
                  std::string::npos, "whose reason is kept");
        check(!Party::parse("something=else\n").read,
              "nor is an answer with no status in it");
    }

    // Who may be invited. The blacklist is the one that has to win.
    {
        Party::Settings s;
        s.use_blacklist = true;
        s.blacklist = { "Nuisance", "OtherNuisance" };
        s.by_list = true;
        s.whitelist = { "Friend", "Nuisance" };   // on both, deliberately

        check(s.may_invite("Friend"), "someone on the whitelist may be invited");
        check(!s.may_invite("Nuisance"),
              "someone on both lists may not -- the blacklist wins");
        check(!s.may_invite("Stranger"),
              "and someone on neither may not, while the list is in force");
        check(!s.may_invite(""), "a nameless player is not invited");
        check(!s.may_invite("nuisance"), "the blacklist ignores case");

        s.by_list = false;
        check(s.may_invite("Stranger"),
              "with the list off, anyone not blacklisted may be invited");
        check(!s.may_invite("Nuisance"), "but the blacklist still holds");

        // A priority name counts as invitable even when the whitelist is the
        // gate: reserving a slot for somebody you would not invite is not a
        // state worth having.
        s.by_list = true;
        s.priority = { "Reserved" };
        check(s.may_invite("Reserved"), "a priority name may be invited");
    }

    // Who may be accepted. Not the invite question: this one goes by the
    // accept-only list, which answering invitations used to ignore -- so
    // "only accept from these" accepted everybody.
    {
        Party::Settings s;
        check(s.accept_all, "everyone is accepted by default");
        check(s.may_accept("Stranger"), "so a stranger's invitation is taken");
        check(s.may_accept(""), "and one whose name could not be read, with no list to check");

        s.accept_all = false;
        s.accept_only = { "Friend" };
        check(s.may_accept("Friend"), "a name on the accept-only list is accepted");
        check(s.may_accept("friend"), "whatever its case");
        check(!s.may_accept("Stranger"), "and nobody else, while the list is in force");
        check(!s.may_accept(""), "nor an unreadable name, which cannot be checked");

        // The invite list says nothing about accepting.
        s.by_list = true;
        s.whitelist = { "Stranger" };
        check(!s.may_accept("Stranger"), "the invite list does not let anyone in");

        s.accept_all = true;
        s.use_blacklist = true;
        s.blacklist = { "Friend" };
        check(!s.may_accept("Friend"), "the blacklist wins over accepting everyone");
        check(!s.may_accept(""),
              "and an unreadable name is refused once there is a blacklist to check");
        check(s.may_accept("Stranger"), "while anyone not on it is still taken");
    }

    // "Accept everyone" is a key of GoBot's own, so a file without it -- the
    // commercial client's -- has to mean what that file meant.
    {
        const std::string path = temp_file("gobot_party_accept.ini");
        {
            std::ofstream f(path);
            f << "[Party]\nparty_accept_only=Friend,Other\n";
        }
        Party::Settings s;
        check(s.load(path) && !s.accept_all,
              "a file with an accept-only list and no switch accepts only those");
        {
            std::ofstream f(path);
            f << "[Party]\nparty_accept_only=\n";
        }
        check(s.load(path) && s.accept_all, "and one with an empty list accepts everyone");

        s.accept_all = false;
        s.accept_only = { "Friend" };
        check(s.save(path), "the switch writes");
        Party::Settings back;
        check(back.load(path) && !back.accept_all && back.accept_only.size() == 1,
              "and reads back");
        std::remove(path.c_str());
    }

    // The settings use the commercial client's own keys, so they round trip.
    {
        Party::Settings s;
        s.magic_word = "party";
        s.use_blacklist = true;
        s.blacklist = { "A", "B" };
        s.whitelist = { "C" };
        s.exp_share = Party::ExpShare::More;
        s.loot_share = Party::LootShare::Contribution;

        const std::string path = temp_file("gobot_party_roundtrip.ini");
        check(s.save(path), "party settings write");

        Party::Settings back;
        check(back.load(path), "and read back");
        check(back.magic_word == "party" && back.use_blacklist &&
              back.blacklist.size() == 2 && back.whitelist.size() == 1 &&
              back.exp_share == Party::ExpShare::More &&
              back.loot_share == Party::LootShare::Contribution,
              "with every setting intact");
        std::remove(path.c_str());
    }

    // And the commercial client's own file, as it ships.
    {
        Party::Settings s;
        const std::string theirs =
            "C:\\Program Files\\ZTeam\\GoC\\configs\\party_s0_default.ini";
        if (GetFileAttributesA(theirs.c_str()) != INVALID_FILE_ATTRIBUTES) {
            check(s.load(theirs), "the commercial client's own party file loads");
            check(s.magic_word == "pt",
                  "and its magic word comes through -- got '" + s.magic_word + "'");
        } else {
            std::printf("  [skip] that client is not installed here\n");
        }
    }
}

/// The shipped macro config: filled in, and nothing on.
///
/// What ships is what a tester's first start runs, and nothing that acts on
/// the game may be on until the player ticks it -- the sequence included,
/// which the simple screen does not show and which pressed Z, X and C in a
/// loop whenever any key was ticked. But every row comes filled in, the
/// potions above all: the first version of this test was written for "HP/MP
/// reads fine but it never drinks", where the reader worked, the triggers
/// worked, and nothing in the suite noticed that the potion key in the file
/// could not drink. So each kind of key is checked to be there, with what it
/// needs, ready for its tick. Read from the source tree: the copy beside the
/// executable is the player's own.
void test_shipped_macro_works() {
    section("The shipped macro: filled in, and nothing on");

#ifdef GOBOT_SOURCE_DIR
    const std::string path = std::string(GOBOT_SOURCE_DIR) + "/configs/macro.ini";
#else
    const std::string path;
#endif
    MacroConfig cfg;
    if (path.empty() || !cfg.load(path)) {
        std::printf("  [skip] the source tree's configs/macro.ini is not here\n");
        return;
    }

    check(cfg.timers_enabled, "its timers are on");
    check(cfg.vitals_enabled, "and it is set to read the bars -- which only looks");
    check(!cfg.sequence_enabled,
          "the sequence, which the simple screen does not show, is off");
    bool none_on = !cfg.keys.empty();
    for (const MacroKey& k : cfg.keys) none_on = none_on && !k.enabled;
    check(none_on, "every key is off -- " + std::to_string(cfg.keys.size()) + " keys in the file");

    // Each kind, filled in for its row.
    const struct { MacroKeyKind kind; const char* what; } kKinds[] = {
        { MacroKeyKind::Skill, "a skill" },
        { MacroKeyKind::Buff, "a buff" },
        { MacroKeyKind::HpPotion, "an HP potion" },
        { MacroKeyKind::MpPotion, "an MP potion" },
    };
    for (const auto& want : kKinds) {
        const MacroKey* found = nullptr;
        for (const MacroKey& k : cfg.keys)
            if (macro_key_kind(k) == want.kind && k.keycode != 0) {
                found = &k;
                break;
            }
        bool usable = found != nullptr;
        if (found && found->conditional())
            usable = found->threshold_pct > 0 && found->threshold_pct < 100;
        if (found && want.kind == MacroKeyKind::Buff)
            usable = clamp_buff_seconds(static_cast<int>(found->interval_ms / 1000)) ==
                     static_cast<int>(found->interval_ms / 1000);
        check(usable, std::string(want.what) + " key is filled in, ready to tick");
    }

    // Ticked, the potions bring the reader up with them.
    MacroConfig ticked = cfg;
    for (MacroKey& k : ticked.keys) k.enabled = true;
    check(ticked.needs_vitals(),
          "ticked, the potion keys need a reading, so starting the macro brings the reader up");

    // Skills following the fight change how a setup fights: off, as it ships,
    // with GoC's numbers ready for the tick -- and the file says each key.
    IniFile raw;
    bool every_key = raw.load(path);
    for (std::size_t i = 0; every_key && i < cfg.keys.size(); ++i) {
        const std::string p = "key_" + std::to_string(i) + "_";
        every_key = raw.has_key("Keys", p + "always_timer") && raw.has_key("Keys", p + "range_px");
    }
    check(!cfg.skills_follow_fight && cfg.sequential_skills && cfg.skill_mp_floor_pct == 10 &&
              cfg.skill_gap_ms == 50 && raw.has_key("Macro", "skills_follow_fight") &&
              raw.has_key("Macro", "sequential_skills") && raw.has_key("Macro", "skill_mp_floor_pct") &&
              raw.has_key("Macro", "skill_gap_ms"),
          "skills_follow_fight=false, sequential_skills=true, skill_mp_floor_pct=10, skill_gap_ms=50");
    bool timers = every_key;
    for (const MacroKey& k : cfg.keys) timers = timers && !k.always_timer && k.range_px == 0;
    check(timers, "and every key has always_timer=false and range_px=0 written");
}

/// Everything else that ships: whatever acts on the game, off, and nothing of
/// any one player's in it.
///
/// The package goes to players who will press BẮT ĐẦU before reading a word,
/// so a switch left on in a file here is a key pressed, an invitation taken
/// or a line said in somebody else's game. And the files here were once this
/// project's own player's: a character's name went out in one. Read from the
/// source tree, which is what package.ps1 copies.
void test_shipped_configs() {
    section("The shipped configs: nothing on, nothing of anybody's");

#ifdef GOBOT_SOURCE_DIR
    const std::string dir = std::string(GOBOT_SOURCE_DIR) + "/configs/";
#else
    const std::string dir;
#endif
    IniFile app;
    if (dir.empty() || !app.load(dir + "app.ini")) {
        std::printf("  [skip] the source tree's configs are not here\n");
        return;
    }
    check(!app.get_bool("Simple", "keep_alive", true) && !app.get_bool("Simple", "revive", true),
          "app.ini: the client keeper and the revive are off");
    check(!app.get_bool("Capture", "autostart", true), "app.ini: the packet bot does not start by itself");
    check(app.get_bool("Simple", "refocus", false),
          "app.ini: bringing the game forward after 30 s idle stays on -- it presses nothing");
    check(app.get_bool("Paths", "new_log_each_start", false),
          "app.ini: gobot_gui.log starts afresh each session, so a tester's holds two, not weeks");

    HunterConfig hunt;
    check(hunt.load(dir + "hunt.ini") && !hunt.enabled && !hunt.loot && !hunt.stay_put,
          "hunt.ini: hunting, looting and standing still are off");
    check(hunt.character_name.empty(), "hunt.ini: no character's name");
    IniFile hunt_ini;
    check(hunt_ini.load(dir + "hunt.ini") && hunt_ini.has_key("Hunt", "skip_monsters") &&
              hunt_ini.has_key("Hunt", "first_monsters") && hunt_ini.has_key("Hunt", "first_only") &&
              hunt.skip_names.empty() && hunt.first_names.empty() && !hunt.first_only,
          "hunt.ini: the monster lists are there, empty, and 'only Đánh trước' is off");
    // What fights differently is the player's to choose: no style, no
    // stepping back, and no jump key -- a key that acts on the game.
    bool style_keys = true;
    for (const char* key : { "style", "min_distance", "up_reach", "down_reach", "up_no_jump",
                             "down_no_jump", "keep_distance", "keep_distance_steps",
                             "facing_tolerance", "no_damage_ms", "jump_key", "jump_checked" })
        style_keys = style_keys && hunt_ini.has_key("Hunt", key);
    check(style_keys && hunt.style.empty() && !hunt.goc_rules() && hunt.min_distance == 0 &&
              hunt.up_reach == -1 && hunt.down_reach == -1 && hunt.up_no_jump == -1 &&
              hunt.down_no_jump == -1 && hunt.reach == 150 && hunt.height_tolerance == 60,
          "hunt.ini: Kiểu đánh is 'Như trước' -- no style, the band it always had");
    check(!hunt.keep_distance && hunt.keep_distance_steps == 3 && hunt.facing_tolerance == 12 &&
              hunt.no_damage_ms == 15000,
          "hunt.ini: keep_distance=false, 3 steps, facing_tolerance=12");
    check(hunt.jump_key == 0 && !hunt.jump_checked,
          "hunt.ini: jump_key=0 -- no jump key until the player sets one -- and not tried");

    Party::Settings party;
    check(party.load(dir + "party.ini") && !party.auto_accept && !party.auto_invite,
          "party.ini: accepting and inviting are off");
    check(party.whitelist.empty() && party.blacklist.empty() && party.accept_only.empty() &&
              party.priority.empty(),
          "party.ini: every list of names is empty");

    CleanSettings trash;
    check(trash.load(dir + "trash.ini") && !trash.enabled && trash.delete_ids.empty() &&
              trash.drop_ids.empty(),
          "trash.ini: cleaning is off and nothing is on the list");
    check(!path_exists(dir + "trash"), "and no folder of photographed items is there to ship");

    ChatSettings chat;
    check(chat.load(dir + "chat.ini") && !chat.enabled && chat.lines.empty(),
          "chat.ini: Tự chat is off and has no lines");

    IniFile quest;
    check(quest.load(dir + "quest.ini") && !quest.get_bool("Quest", "enabled", true),
          "quest.ini: quick quest is off");

    IniFile alerts;
    check(alerts.load(dir + "alerts.ini") && !alerts.get_bool("Alerts", "enabled", true) &&
              alerts.get("Alerts", "label", "").empty() && alerts.get("Alerts", "webhook", "").empty(),
          "alerts.ini: alerts are off, with no label and no webhook");

    AutoLogin::Settings login;
    check(login.load(dir + "login.ini") && login.channel_index == 0 && login.character_index == 0,
          "login.ini: channel 1, the first character");
    IniFile login_ini;
    bool account = false;
    if (login_ini.load(dir + "login.ini"))
        for (const char* key : { "username", "password", "account", "user" })
            account = account || !login_ini.get("Login", key, "").empty();
    check(!account, "login.ini: no account and no password");
}

/// Fishing and mining: the deciding half.
///
/// This is why `decide` takes a situation and returns an intention instead of
/// pressing anything. A loop that decides and acts in one step can only be
/// tested by standing at a fishing spot with the game running for an hour;
/// this way every branch -- tool gone, tool broken, bag full, health low,
/// cannot afford a replacement -- is a few lines.
void test_gatherer() {
    section("Gathering");

    auto mining = [] {
        Gatherer::Settings s;
        s.name = "mining";
        s.tool_picture = "pickaxe";
        s.broken_picture = "broken_pickaxe";
        s.buy_routine = "buy_pickaxe";
        s.hand_in_routine = "mining_exchange";
        s.work_key = 'E';
        s.hand_in_every = 5;
        return s;
    };

    // The ordinary case: tool in hand, nothing wrong, get on with it.
    {
        Gatherer g;
        g.set_settings(mining());
        Gatherer::Look look;
        look.tool_present = true;
        check(g.decide(look) == Gatherer::Action::Work, "works when it has a tool");
    }

    // The tool has gone, or broken. Both mean replace, and replacing comes
    // before everything else -- working without a tool presses a key at nothing
    // and looks exactly like working.
    {
        Gatherer g;
        g.set_settings(mining());
        Gatherer::Look gone;          // tool_present stays false
        check(g.decide(gone) == Gatherer::Action::Replace,
              "replaces the tool when it has gone");

        Gatherer::Look broken;
        broken.tool_present = true;
        broken.tool_broken = true;
        check(g.decide(broken) == Gatherer::Action::Replace,
              "and when it is broken but still in the slot");
    }

    // Handing in happens before the next cycle, not after: a bag that fills
    // mid-cycle loses whatever came out of it.
    {
        Gatherer g;
        g.set_settings(mining());
        Gatherer::Look look;
        look.tool_present = true;

        for (int i = 0; i < 4; ++i) {
            check(g.decide(look) == Gatherer::Action::Work,
                  "cycle " + std::to_string(i + 1) + " of five is work");
            g.worked();
        }
        g.worked();        // the fifth
        check(g.decide(look) == Gatherer::Action::HandIn,
              "hands in once it has done the configured number");
        g.handed_in();
        check(g.decide(look) == Gatherer::Action::Work,
              "and goes back to work afterwards");
    }

    // Low health holds the loop rather than working through it.
    {
        Gatherer::Settings s = mining();
        s.pause_below_hp_pct = 40;
        Gatherer g;
        g.set_settings(s);

        Gatherer::Look look;
        look.tool_present = true;
        look.hp_pct = 30.0f;
        check(g.decide(look) == Gatherer::Action::Nothing, "holds while health is low");

        look.hp_pct = 80.0f;
        check(g.decide(look) == Gatherer::Action::Work, "and carries on once it is not");

        // No reading at all must not hold it forever. This is the one that
        // would turn a reader that stopped working into a bot that did nothing
        // all night and said nothing about it.
        look.hp_pct = -1.0f;
        check(g.decide(look) == Gatherer::Action::Work,
              "an absent reading does not hold it indefinitely");
    }

    // Giving up. A loop that cannot afford a new pickaxe must not spend the
    // night walking into a shop.
    {
        Gatherer::Settings s = mining();
        s.give_up_after = 3;
        Gatherer g;
        g.set_settings(s);

        Gatherer::Look look;       // no tool
        for (int i = 0; i < 2; ++i) {
            check(g.decide(look) == Gatherer::Action::Replace,
                  "tries again after failure " + std::to_string(i + 1));
            g.replaced(false);
        }
        g.replaced(false);         // the third
        check(g.decide(look) == Gatherer::Action::Stop, "gives up after three failures");
        check(g.trouble().find("could not replace") != std::string::npos,
              "and says why -- got: " + g.trouble());

        // A success in between clears the run. Three failures spread over an
        // hour is not the same thing as three in a row.
        g.reset();
        g.replaced(false);
        g.replaced(false);
        g.replaced(true);
        g.replaced(false);
        check(g.decide(look) == Gatherer::Action::Replace,
              "a success in between clears the run of failures");
    }

    // Stopping when asked.
    {
        Gatherer::Settings s = mining();
        s.stop_after = 2;
        s.hand_in_every = 0;
        Gatherer g;
        g.set_settings(s);

        Gatherer::Look look;
        look.tool_present = true;
        g.worked();
        check(g.decide(look) == Gatherer::Action::Work, "works up to the limit");
        g.worked();
        check(g.decide(look) == Gatherer::Action::Stop, "and stops at it");
    }

    // A configuration that cannot work refuses to start, rather than pressing
    // an unset key every few seconds.
    {
        Gatherer::Settings s = mining();
        s.work_key = 0;
        Gatherer g;
        g.set_settings(s);
        Gatherer::Look look;
        look.tool_present = true;
        check(g.decide(look) == Gatherer::Action::Stop, "refuses to run with no key set");
        check(g.trouble().find("no key is set") != std::string::npos,
              "and says so -- got: " + g.trouble());
    }
    {
        Gatherer::Settings s = mining();
        s.hand_in_every = 10;
        s.hand_in_routine.clear();
        Gatherer g;
        g.set_settings(s);
        Gatherer::Look look;
        look.tool_present = true;
        check(g.decide(look) == Gatherer::Action::Stop,
              "refuses when told to hand in with no routine to do it with");
    }

    // The shipped config loads, and names routines that exist.
    {
        IniFile ini;
        const std::string path = exe_relative("configs/gathering.ini");
        check(ini.load(path), "the shipped gathering config loads");

        const auto routines = Routine::load(exe_relative("configs/routines.ini"));
        for (const char* section : { "Fishing", "Mining" }) {
            Gatherer::Settings s;
            s.load(ini, section);
            check(!s.tool_picture.empty() && !s.buy_routine.empty(),
                  std::string(section) + " names a tool and a way to replace it");
            check(Routine::find(routines, s.buy_routine) != nullptr,
                  std::string(section) + "'s buy routine exists: " + s.buy_routine);
            if (s.hand_in_every > 0)
                check(Routine::find(routines, s.hand_in_routine) != nullptr,
                      std::string(section) + "'s hand-in routine exists: " +
                          s.hand_in_routine);
            // Shipped with no key on purpose, so it cannot start by accident.
            check(s.work_key == 0,
                  std::string(section) + " ships with no key set, so it cannot "
                  "start until somebody sets one");
        }
    }
}

/// Routines read out of a config file.
void test_routines() {
    section("Routines");

    // Round trip first, so a parser that drops a field fails here rather than
    // three dialogs into a live run.
    {
        Routine buy;
        buy.name = "buy_thing";
        DialogDriver::Step pick;
        pick.find = "thing_in_shop";
        pick.confirm = "buy_button";
        pick.click_dx = 12;
        pick.click_dy = 30;
        pick.score = 0.72f;
        pick.wait_ms = 9000;
        pick.attempts = 5;
        DialogDriver::Step press;
        press.find = "buy_button";
        press.gone = true;
        press.optional = true;
        buy.steps = { pick, press };

        const std::string path = temp_file("gobot_routines_test.ini");
        check(Routine::save({ buy }, path), "a routine writes");

        const auto back = Routine::load(path);
        check(back.size() == 1 && back[0].name == "buy_thing", "and reads back");
        if (back.size() == 1 && back[0].steps.size() == 2) {
            const auto& a = back[0].steps[0];
            const auto& b = back[0].steps[1];
            check(a.find == "thing_in_shop" && a.confirm == "buy_button" &&
                  a.click_dx == 12 && a.click_dy == 30 &&
                  std::fabs(a.score - 0.72f) < 0.001f &&
                  a.wait_ms == 9000 && a.attempts == 5,
                  "every field of a step survives");
            check(b.gone && b.optional, "and so do its flags");
        } else {
            check(false, "the routine came back with the wrong number of steps");
        }
        std::remove(path.c_str());
    }

    // A gap truncates rather than renumbering. Somebody commenting out step two
    // must not find steps three and four silently moved up into its place.
    {
        IniFile ini;
        ini.set("Routine.gappy", "1.find", "one");
        ini.set("Routine.gappy", "1.confirm", "a");
        ini.set("Routine.gappy", "3.find", "three");
        ini.set("Routine.gappy", "3.confirm", "c");
        const std::string path = temp_file("gobot_routines_gap.ini");
        check(ini.save(path), "writes a routine with a gap in it");

        const auto back = Routine::load(path);
        check(back.size() == 1 && back[0].steps.size() == 1,
              "a missing step number ends the routine rather than closing the gap");
        std::remove(path.c_str());
    }

    // The shipped table loads, and every picture it names exists.
    {
        const std::string path = exe_relative("configs/routines.ini");
        const auto routines = Routine::load(path);
        check(routines.size() >= 6, "the shipped routines load -- got " +
                                    std::to_string(routines.size()));

        check(Routine::find(routines, "buy_fishing_rod") != nullptr,
              "including the one the fishing loop depends on");
        check(Routine::find(routines, "nothing_called_this") == nullptr,
              "and nothing that is not there");

        // Every step can prove it worked. This is the rule the driver enforces
        // at run time; checking it here means a bad table is caught by the test
        // suite rather than halfway through a shop visit.
        bool all_provable = true;
        std::string offender;
        for (const Routine& r : routines)
            for (const DialogDriver::Step& s : r.steps)
                if (s.confirm.empty() && !s.gone) {
                    all_provable = false;
                    offender = r.name + " / " + s.find;
                }
        check(all_provable,
              "every shipped step can tell whether its click landed" +
                  (offender.empty() ? std::string{} : " -- " + offender + " cannot"));

        // And every picture they name is one the helper's assembly carries. A
        // routine naming a picture nobody has fails halfway, having already
        // clicked three things.
        const char* dll = "C:\\Program Files\\TH_VGO_Bot_Helper\\TungHaVGOBot.dll";
        if (GetFileAttributesA(dll) != INVALID_FILE_ATTRIBUTES) {
            TemplateStore store;
            if (store.load_from_assembly(dll, {})) {
                const auto missing = Routine::missing_pictures(routines, store);
                std::string list;
                for (const std::string& m : missing) {
                    if (!list.empty()) list += ", ";
                    list += m;
                }
                check(missing.empty(),
                      "every picture the routines name exists" +
                          (list.empty() ? std::string{} : " -- missing: " + list));
            }
        }
    }
}

/// Telling apart two icons that differ only in colour.
///
/// The game's sets are full of these: four crest tiers, sixteen exchange items,
/// the same drawing in another hue. The hand-rolled matcher reduced every
/// picture to grey before correlating and could not separate them at all --
/// which is one of the reasons matching moved to OpenCV, and the reason this
/// is a test rather than an assumption.
void test_colour_templates() {
    section("Templates that differ only in colour");

    // Printed every run. OpenCV is linked into the executable rather than
    // sitting beside it as a DLL, so a build folder with no DLLs in it looks
    // exactly like a build that is not using OpenCV at all.
    std::printf("         matching: %s\n", TemplateMatcher::backend().c_str());

    // A mostly-transparent picture must not match a flat expanse.
    //
    // This is the one that was actually wrong against the live game, and no
    // synthetic test caught it because every synthetic test pastes a template
    // into a frame and then finds it. Masked normalised correlation has
    // positions where the denominator collapses, and those come back as
    // infinity; clamping them into the valid range made the worst possible
    // match score exactly 1.000. An eighteen pixel close button that is a
    // third transparent matched an empty stretch of a notification banner at a
    // perfect score, and the bot clicked that empty patch six times running
    // and reported success each time.
    {
        Frame button = make_frame(18, 18);
        for (int y = 0; y < 18; ++y)
            for (int x = 0; x < 18; ++x) {
                // A ring with a cross in it, and transparent corners -- the
                // shape of a close button.
                const int dx = x - 9, dy = y - 9;
                const bool inside = dx * dx + dy * dy < 64;
                if (!inside) continue;                 // left transparent
                const bool stroke = std::abs(dx) == std::abs(dy) && std::abs(dx) < 5;
                const auto v = static_cast<std::uint8_t>(stroke ? 240 : 40);
                put_px(button, x, y, v, v, v);
            }

        // A screen of flat colour, with nothing on it at all.
        Frame flat = make_frame(400, 300);
        for (int y = 0; y < flat.height; ++y)
            for (int x = 0; x < flat.width; ++x) put_px(flat, x, y, 36, 44, 72);

        const auto ghost = TemplateMatcher::find(
            flat, ScreenRect{ 0, 0, flat.width, flat.height }, button, 0.85f);
        check(!ghost.found,
              "a mostly-transparent picture finds nothing on a flat screen" +
                  (ghost.found ? " -- claimed " + std::to_string(ghost.score) +
                                 " at " + std::to_string(ghost.at.x) + "," +
                                 std::to_string(ghost.at.y) : ""));

        // And still finds itself when it really is there, so the guard above
        // has not simply broken masked matching.
        Frame with_it = flat;
        for (int y = 0; y < button.height; ++y)
            for (int x = 0; x < button.width; ++x) {
                const std::size_t i =
                    (static_cast<std::size_t>(y) * button.width + x) * 4;
                if (button.pixels[i + 3] < 128) continue;     // transparent
                std::uint8_t r, g, b;
                button.get(x, y, r, g, b);
                put_px(with_it, 200 + x, 120 + y, r, g, b);
            }
        const auto real = TemplateMatcher::find(
            with_it, ScreenRect{ 0, 0, with_it.width, with_it.height }, button, 0.85f);
        check(real.found && real.at.x == 200 && real.at.y == 120,
              "and finds it where it really is");
    }

    if (!TemplateMatcher::uses_opencv()) {
        std::printf("  [skip] built without OpenCV, which cannot tell these apart\n");
        return;
    }

    auto tinted = [](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        Frame m = make_frame(28, 28);
        for (int y = 0; y < m.height; ++y)
            for (int x = 0; x < m.width; ++x) {
                const bool edge = x < 3 || y < 3 || x >= 25 || y >= 25;
                if (edge) put_px(m, x, y, 18, 18, 22);
                else      put_px(m, x, y, r, g, b);
            }
        return m;
    };

    // Identical shapes, four hues. The crest tiers, in effect.
    const Frame red   = tinted(210, 40, 40);
    const Frame green = tinted(40, 200, 60);
    const Frame blue  = tinted(50, 70, 220);
    const Frame gold  = tinted(220, 180, 40);

    Frame screen = make_frame(500, 300);
    std::mt19937 rng(4242);
    for (int y = 0; y < screen.height; ++y)
        for (int x = 0; x < screen.width; ++x)
            put_px(screen, x, y, static_cast<std::uint8_t>(rng() % 70),
                   static_cast<std::uint8_t>(rng() % 70),
                   static_cast<std::uint8_t>(rng() % 70));

    const struct { const Frame* icon; int x, y; const char* name; } kPlaced[] = {
        { &red,   40,  40,  "red"   },
        { &green, 160, 40,  "green" },
        { &blue,  280, 40,  "blue"  },
        { &gold,  400, 40,  "gold"  },
    };
    for (const auto& p : kPlaced)
        for (int y = 0; y < p.icon->height; ++y)
            for (int x = 0; x < p.icon->width; ++x) {
                std::uint8_t r, g, b;
                p.icon->get(x, y, r, g, b);
                put_px(screen, p.x + x, p.y + y, r, g, b);
            }

    // Each is found where it actually is, not at one of its three
    // identically-shaped siblings.
    for (const auto& p : kPlaced) {
        const auto hit = TemplateMatcher::find(
            screen, ScreenRect{ 0, 0, screen.width, screen.height }, *p.icon, 0.90f,
            TemplateMatcher::Tone::Colour);
        check(hit.found && hit.at.x == p.x && hit.at.y == p.y,
              std::string("the ") + p.name + " icon is found at its own place" +
                  (hit.found ? " -- got " + std::to_string(hit.at.x) + "," +
                               std::to_string(hit.at.y) : " -- not found"));
    }

    // And asking for every copy of one hue turns up one, not four.
    const auto all = TemplateMatcher::find_all(
        screen, ScreenRect{ 0, 0, screen.width, screen.height }, red, 0.90f, 8,
        TemplateMatcher::Tone::Colour);
    check(all.size() == 1,
          "searching for one hue finds one icon, not its siblings -- got " +
              std::to_string(all.size()));

    // What one look costs over a screen the size of the game's. The features
    // ahead match dozens of pictures against every frame -- forty inventory
    // slots, sixteen exchange items -- so this number decides whether they are
    // feasible at all.
    {
        Frame full = make_frame(1024, 768);
        std::mt19937 r2(99);
        for (int y = 0; y < full.height; ++y)
            for (int x = 0; x < full.width; ++x)
                put_px(full, x, y, static_cast<std::uint8_t>(r2() % 80),
                       static_cast<std::uint8_t>(r2() % 80),
                       static_cast<std::uint8_t>(r2() % 80));
        for (int y = 0; y < blue.height; ++y)
            for (int x = 0; x < blue.width; ++x) {
                std::uint8_t r, g, b;
                blue.get(x, y, r, g, b);
                put_px(full, 700 + x, 500 + y, r, g, b);
            }

        const auto started = std::chrono::steady_clock::now();
        const int looks = 20;
        int hits = 0;
        for (int i = 0; i < looks; ++i)
            if (TemplateMatcher::find(full, ScreenRect{ 0, 0, 1024, 768 }, blue, 0.90f).found)
                ++hits;
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count() / looks;

        check(hits == looks, "found every time over a full-size screen");
        std::printf("         one 28x28 look over 1024x768: %.1f ms\n", ms);
    }
}

/// The dialog driver, over scripted screens.
///
/// The failure paths are the point. A driver that clicks and moves on walks a
/// whole sequence into nothing and reports success, so what has to be tested is
/// what it does when the click misses, when the page never arrives, and when
/// the step has no way of telling either.
void test_dialog_driver() {
    section("Dialog driver");

    auto noise_frame = [](unsigned seed) {
        Frame f = make_frame(400, 300);
        std::mt19937 r(seed);
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x)
                put_px(f, x, y, static_cast<std::uint8_t>(r() % 90),
                       static_cast<std::uint8_t>(r() % 90),
                       static_cast<std::uint8_t>(r() % 90));
        return f;
    };
    // Two marks that differ in shape, not in colour.
    //
    // They differed only in colour at first, and the test failed in a way worth
    // keeping a note of: the matcher reduces everything to grey before
    // correlating, so a red square and a blue square of the same shape are the
    // same picture to it, and the second mark matched the first everywhere it
    // appeared.
    //
    // That is a real limit rather than a test artefact. Several of the pictures
    // still to be used are one icon in different colours -- the four crest
    // tiers, the sixteen exchange items -- and telling those apart needs the
    // colour. It belongs on the list for when matching moves to OpenCV, which
    // can correlate the channels rather than their average.
    auto make_mark = [](bool low_right) {
        Frame m = make_frame(24, 18);
        for (int y = 0; y < m.height; ++y)
            for (int x = 0; x < m.width; ++x) {
                const bool edge = x < 3 || y < 3 || x >= 21 || y >= 15;
                const bool blob = low_right ? (x >= 13 && y >= 9) : (x < 11 && y < 9);
                const auto v = static_cast<std::uint8_t>(edge ? 20 : (blob ? 55 : 225));
                put_px(m, x, y, v, v, v);
            }
        return m;
    };
    auto paste = [](Frame& into, const Frame& what, int px, int py) {
        for (int y = 0; y < what.height; ++y)
            for (int x = 0; x < what.width; ++x) {
                std::uint8_t r, g, b;
                what.get(x, y, r, g, b);
                put_px(into, px + x, py + y, r, g, b);
            }
    };

    const Frame button = make_mark(false);
    const Frame after  = make_mark(true);

    TemplateStore store;
    store.put("button", button);
    store.put("after", after);

    InputManager input;      // no window: the clicks go nowhere, which is fine

    // ---- the click lands ----
    {
        int served = 0;
        DialogDriver driver(input, store, [&](Frame& f) {
            f = noise_frame(7);
            if (served++ < 2) paste(f, button, 100, 80);
            else              paste(f, after, 100, 80);
            return true;
        });
        DialogDriver::Step step;
        step.find = "button";
        step.confirm = "after";
        step.wait_ms = 1500;
        step.confirm_ms = 1500;

        const auto r = driver.run(step);
        check(r.ok, "a step succeeds when the confirmation appears");
        check(r.at.x == 100 && r.at.y == 80, "and reports where it clicked");
        check(r.attempts_used == 1, "on the first attempt");
    }

    // ---- the click misses, every time ----
    {
        DialogDriver driver(input, store, [&](Frame& f) {
            f = noise_frame(11);
            paste(f, button, 60, 40);        // the button never goes away
            return true;
        });
        DialogDriver::Step step;
        step.find = "button";
        step.confirm = "after";
        step.wait_ms = 400;
        step.confirm_ms = 400;
        step.attempts = 3;

        const auto r = driver.run(step);
        check(!r.ok, "a step fails when the confirmation never appears");
        check(r.attempts_used == 3, "after using its whole retry budget");
        check(r.trouble.find("after") != std::string::npos,
              "and says what it was waiting for -- got: " + r.trouble);
    }

    // ---- confirmed by the button going away instead ----
    {
        int served = 0;
        DialogDriver driver(input, store, [&](Frame& f) {
            f = noise_frame(13);
            if (served++ < 1) paste(f, button, 200, 150);
            return true;
        });
        DialogDriver::Step step;
        step.find = "button";
        step.gone = true;
        step.wait_ms = 1000;
        step.confirm_ms = 1000;

        check(driver.run(step).ok, "a step can be confirmed by the thing vanishing");
    }

    // ---- the page never arrives ----
    {
        DialogDriver driver(input, store,
                            [&](Frame& f) { f = noise_frame(17); return true; });
        DialogDriver::Step step;
        step.find = "button";
        step.confirm = "after";
        step.wait_ms = 300;
        step.attempts = 2;

        const auto r = driver.run(step);
        check(!r.ok, "a step fails when its page never appears");
        check(r.trouble.find("never appeared") != std::string::npos,
              "and says so -- got: " + r.trouble);

        step.optional = true;
        check(driver.run(step).ok, "an optional step passes when its page is absent");
    }

    // ---- a step that cannot prove anything is refused ----
    {
        DialogDriver driver(input, store,
                            [&](Frame& f) { f = noise_frame(19); return true; });
        DialogDriver::Step step;
        step.find = "button";       // no confirm, no gone
        const auto r = driver.run(step);
        check(!r.ok && r.trouble.find("whether the click landed") != std::string::npos,
              "a step with no way to tell it worked is refused outright");
    }

    // ---- a sequence stops where it breaks, and says where ----
    {
        DialogDriver driver(input, store, [&](Frame& f) {
            f = noise_frame(23);
            paste(f, after, 10, 10);     // step one's confirmation is always up
            return true;
        });
        DialogDriver::Step ok_step;
        ok_step.find = "after";
        ok_step.confirm = "after";
        ok_step.wait_ms = 300; ok_step.confirm_ms = 300;

        DialogDriver::Step bad_step;
        bad_step.find = "button";       // never on screen
        bad_step.confirm = "after";
        bad_step.wait_ms = 300; bad_step.attempts = 1;

        std::string trouble;
        const std::size_t got = driver.run_all({ ok_step, bad_step }, &trouble);
        check(got == 1, "a sequence reports how far it got");
        check(trouble.find("step 2 of 2") != std::string::npos,
              "and which step broke -- got: " + trouble);
    }

    // ---- cancelling ----
    {
        DialogDriver driver(input, store,
                            [&](Frame& f) { f = noise_frame(29); return true; });
        driver.cancel();
        DialogDriver::Step step;
        step.find = "button";
        step.confirm = "after";
        step.wait_ms = 5000;
        const auto r = driver.run(step);
        check(!r.ok && r.trouble == "cancelled", "cancelling stops it at once");
    }
}

/// Finding the bars by the HUD's landmark instead of by colour.
///
/// The case that matters is the one that broke the old way: a bar drained far
/// enough that its coloured part runs out underneath the figures printed across
/// it. Scanning for runs then measures the empty part starting from white text,
/// decides the bar is three quarters as wide as it is, and every reading taken
/// from it is wrong with nothing to say so. The landmark does not care what the
/// bars read.
void test_anchored_bars() {
    section("Finding the bars by landmark");

    const char* dll = "C:\\Program Files\\TH_VGO_Bot_Helper\\TungHaVGOBot.dll";
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        std::printf("  [skip] the landmark picture is not installed here\n");
        return;
    }
    TemplateStore store;
    const Frame* anchor = store.load_from_assembly(dll, {})
                        ? store.get("hpmp_indicator2") : nullptr;
    if (!anchor || !anchor->valid()) {
        std::printf("  [skip] no landmark picture in that assembly\n");
        return;
    }

    // A HUD laid out the way this client lays it out: landmark at 44,4 and the
    // bars at 83,8 and 83,22, each 159 wide -- and each drained to about 72%,
    // which puts the end of the fill underneath the caption.
    Frame hud = make_frame(1024, 300);
    for (int y = 0; y < hud.height; ++y)
        for (int x = 0; x < hud.width; ++x) put_px(hud, x, y, 26, 28, 34);

    for (int y = 0; y < anchor->height; ++y)
        for (int x = 0; x < anchor->width; ++x) {
            std::uint8_t r, g, b;
            anchor->get(x, y, r, g, b);
            put_px(hud, 44 + x, 4 + y, r, g, b);
        }

    const struct { int y; int fill; std::uint8_t r, g, b; } kBars[] = {
        { 7,  115, 239, 0,   0   },
        { 21, 115, 0,   49,  206 },
    };
    for (const auto& bar : kBars) {
        for (int row = 0; row < 8; ++row)
            for (int i = 0; i < 159; ++i)
                put_px(hud, 83 + i, bar.y + row,
                       static_cast<std::uint8_t>(i < bar.fill ? bar.r : 14),
                       static_cast<std::uint8_t>(i < bar.fill ? bar.g : 14),
                       static_cast<std::uint8_t>(i < bar.fill ? bar.b : 16));
        // The caption, painted across the end of the fill -- the whole problem.
        for (int i = 0; i < 52; ++i)
            for (int row = 1; row < 7; ++row)
                if ((i + row) % 3) put_px(hud, 185 + i, bar.y + row, 250, 250, 250);
    }

    VitalsReader reader;
    reader.set_anchor(*anchor);
    check(reader.has_anchor(), "the reader takes the landmark");

    // The old way, on this frame, for comparison.
    BarSpec scan_hp, scan_mp;
    const int scanned = VitalsReader::find_bars(hud, scan_hp, scan_mp);
    const bool scan_wrong = scanned != 2 || std::abs(scan_hp.width - 159) > 12;
    check(scan_wrong,
          "scanning for colour gets this frame wrong, as expected" +
              std::string(scanned == 2 ? " -- it says " +
                          std::to_string(scan_hp.width) + " wide" : ""));

    // The landmark way. Driven through the public surface by starting the
    // reader against no window, which leaves the geometry to be set by the
    // layout rather than by a capture; so check the arithmetic directly.
    const VitalsReader::HudLayout l = reader.layout();
    check(l.hp_dx == 39 && l.hp_dy == 4 && l.mp_dx == 39 && l.mp_dy == 18 &&
          l.bar_w == 159,
          "the shipped offsets are the ones measured off this client");

    const auto found = TemplateMatcher::find(
        hud, ScreenRect{ 0, 0, hud.width, hud.height }, *anchor, 0.75f);
    check(found.found && found.at.x == 44 && found.at.y == 4,
          "the landmark is found where it was put");
    if (found.found) {
        check(found.at.x + l.hp_dx == 83 && found.at.y + l.hp_dy == 8,
              "and the HP bar follows from it exactly");
        check(found.at.x + l.mp_dx == 83 && found.at.y + l.mp_dy == 22,
              "and so does MP");

        // The caption box built on that geometry covers the figures, which is
        // the thing the wrong width broke.
        BarSpec hp;
        hp.x = found.at.x + l.hp_dx; hp.y = found.at.y + l.hp_dy;
        hp.width = l.bar_w; hp.height = l.bar_h;
        const ScreenRect box = VitalsReader::caption_box(hp);
        check(box.x <= 185 && box.right() >= 237,
              "and the caption box covers the figures");
    }
}

void test_session_manager() {
    section("Session watcher");

    // In the world: the HUD is there, so the bot may act.
    {
        Frame f = make_frame(1024, 768);
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x) put_px(f, x, y, 30, 40, 55);
        paint_hud(f, 83, 8);
        check(SessionManager::hud_present(f), "a frame with the HUD reads as playing");
    }

    // The states that must not read as playing. Each of these is a real thing
    // the client shows, and the bot pressing keys at any of them is the whole
    // problem this watcher exists for.
    {
        // A loading screen: dark, with a bright banner across it.
        Frame f = make_frame(1024, 768);
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x) put_px(f, x, y, 8, 8, 12);
        for (int y = 300; y < 340; ++y)
            for (int x = 200; x < 800; ++x) put_px(f, x, y, 210, 30, 30);
        check(!SessionManager::hud_present(f), "a loading screen does not read as playing");
    }
    {
        // A menu with a red banner and a blue one, but nothing like a pair of
        // bars -- different places, different widths. This is the case that
        // catches a lazy test: "some red and some blue" is not a HUD.
        Frame f = make_frame(1024, 768);
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x) put_px(f, x, y, 20, 20, 24);
        for (int y = 40; y < 60; ++y)
            for (int x = 100; x < 400; ++x) put_px(f, x, y, 230, 20, 20);
        for (int y = 150; y < 170; ++y)
            for (int x = 600; x < 700; ++x) put_px(f, x, y, 20, 40, 230);
        check(!SessionManager::hud_present(f),
              "a red banner and an unrelated blue one do not read as playing");
    }
    {
        Frame nothing;
        check(!SessionManager::hud_present(nothing), "no frame does not read as playing");
    }

    // Settings survive a trip through a config file, including the one that
    // turns the machine off -- a setting that silently failed to load would be
    // either a bot that never stops or a machine that shuts down unasked.
    {
        SessionManager::Settings s;
        s.poll_ms = 1500;
        s.lost_grace_s = 40;
        s.stop_after_minutes = 180;
        s.shutdown_when_done = true;
        s.shutdown_delay_s = 90;

        IniFile ini;
        s.save(ini);

        SessionManager::Settings back;
        back.load(ini);
        check(back.poll_ms == 1500 && back.lost_grace_s == 40 &&
              back.stop_after_minutes == 180 && back.shutdown_when_done &&
              back.shutdown_delay_s == 90,
              "session settings survive a config round trip");
    }

    // Nonsense in the file does not become nonsense in the settings. A poll
    // interval of zero would spin a core flat out capturing the screen.
    {
        IniFile ini;
        ini.set_int("Session", "poll_ms", 0);
        ini.set_int("Session", "lost_grace_s", -5);
        ini.set_int("Session", "shutdown_delay_s", 1);

        SessionManager::Settings s;
        s.load(ini);
        check(s.poll_ms >= 250 && s.lost_grace_s >= 3 && s.shutdown_delay_s >= 10,
              "unusable values from a config file are clamped");
    }

    // The process check answers truthfully about a process that is certainly
    // running -- this one.
    {
        check(SessionManager::process_running("gobot_test.exe"),
              "notices a process that is running");
        check(!SessionManager::process_running("surely_nothing_is_called_this.exe"),
              "does not invent a process that is not");
    }
}

/// The game's windows on this desktop now (class GhostSoul), where they are.
/// Only looked at: for keeping the test's own windows off them.
std::vector<RECT> game_windows_on_screen() {
    std::vector<RECT> out;
    for (HWND g = FindWindowExW(nullptr, nullptr, L"GhostSoul", nullptr); g != nullptr;
         g = FindWindowExW(nullptr, g, L"GhostSoul", nullptr)) {
        RECT r{};
        if (IsWindowVisible(g) && !IsIconic(g) && GetWindowRect(g, &r)) out.push_back(r);
    }
    return out;
}

/// Whether `r` is over any of `games`.
bool over_the_game(const RECT& r, const std::vector<RECT>& games) {
    for (const RECT& g : games) {
        RECT both{};
        if (IntersectRect(&both, &r, &g)) return true;
    }
    return false;
}

/// Where the test's windows that are kept on top can go: `w` by `h`, `margin`
/// in from a corner of the work area, and over no window of the game's.
///
/// They sit over whatever is there for the moments a test needs them. Over
/// the game they would hide its HUD -- the landmark, the HP and MP bars --
/// from a GoBot the player is running, which reads this game off the screen,
/// and take the player's clicks there. The top left, as always, when the game
/// is not up; false when no corner is clear of it, and the test is left out.
bool clear_of_the_game(int w, int h, int margin, POINT& at) {
    RECT work{};
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    const std::vector<RECT> games = game_windows_on_screen();
    const POINT corners[] = {
        { work.left + margin, work.top + margin },
        { work.right - margin - w, work.top + margin },
        { work.left + margin, work.bottom - margin - h },
        { work.right - margin - w, work.bottom - margin - h },
    };
    for (const POINT& c : corners) {
        if (!over_the_game(RECT{ c.x, c.y, c.x + w, c.y + h }, games)) {
            at = c;
            return true;
        }
    }
    return false;
}

/// The auto-login with no game, no launcher and no window: its table, its
/// settings file old and new, how it decides where it is, and what it says.
/// Nothing here can start or click anything -- every flow that is run stops
/// at a check made before the first window is touched.
void test_auto_login() {
    section("Auto-login");

    // The crossing is seven screens and the table has to cover them in the
    // order they are met. A page out of order is a flow that clicks the wrong
    // thing on a page that has not appeared yet.
    const auto pages = AutoLogin::default_pages();
    check(pages.size() == 7, "the table covers the crossing: seven pages");
    {
        bool ordered = true;
        for (std::size_t i = 1; i < pages.size(); ++i)
            if (static_cast<int>(pages[i].step) <= static_cast<int>(pages[i - 1].step))
                ordered = false;
        check(ordered, "the pages are in the order they are met");
        check(!pages.empty() && pages.front().key == "game_list" &&
                  pages.front().step == AutoLogin::Step::GameList,
              "it starts at VTCZone's list of games, signed in, as GoC's does");
        check(std::none_of(pages.begin(), pages.end(),
                           [](const AutoLogin::Page& p) { return p.key == "login_page"; }),
              "and no page of it is a sign-in form to type into");
    }

    // The table carries what ships, so a missing or old file still crosses.
    {
        const AutoLogin::Settings s;
        check(s.pages.size() == 7 && s.unidentified().empty(),
              "a fresh table names a picture for every page");
        const AutoLogin::Page* channel = nullptr;
        const AutoLogin::Page* who = nullptr;
        for (const auto& p : s.pages) {
            if (p.step == AutoLogin::Step::Channel)   channel = &p;
            if (p.step == AutoLogin::Step::Character) who = &p;
        }
        check(channel != nullptr && channel->picture == "channel_first_index" &&
                  channel->pick == AutoLogin::Pick::Row && channel->click_dx == 54 &&
                  channel->click_dy == 140 && channel->row_step == 31 && channel->row_count == 8,
              "the channel list ships with its measured rows");
        check(who != nullptr && who->picture == "select_char_indicator" &&
                  who->pick == AutoLogin::Pick::Slot && who->click_dx == 81 &&
                  who->click_dy == 25 && who->row_dx == -518 && who->row_dy == -160 &&
                  who->row_step == 202 && who->row_across && who->row_count == 3,
              "and the character select with its measured slots");
        check(s.signin_picture == "login_page" && s.signin_score > 0.79f && s.signin_score < 0.81f,
              "VTCZone's sign-in form is known by its button, at 0.80");
        check(s.launcher_path == "C:\\Program Files\\VTCZone\\VTCZone.exe" &&
                  s.launcher_window == "VTCZone" && s.game_window == "GhostOnlineVTC" &&
                  s.client_class == "#32770" && s.client_process == "GhostOnlineVTC.exe",
              "and the three windows are the ones that ship");
    }

    // Nothing runs until every page has been identified. This is the check
    // that stops the flow starting a launcher it cannot then drive, and
    // leaving a window open for somebody else to close.
    {
        AutoLogin::Settings s;
        for (auto& page : s.pages) page.picture.clear();
        check(s.unidentified().size() == s.pages.size(),
              "a blank table counts every page as unidentified");

        for (auto& page : s.pages) page.picture = "whatever";
        check(s.unidentified().empty(), "a filled table counts none");
    }

    // The settings survive a file, pages and all. Click offsets that did not
    // come back would click the middle of the picture instead of the entry.
    {
        AutoLogin::Settings s;
        s.launcher_path = "C:\\somewhere\\Launcher.exe";
        s.launcher_window = "TheLauncher";
        s.signin_picture = "signin_marker";
        s.signin_score = 0.9f;
        s.channel_index = 5;
        s.game_slot = 2;
        s.pages[0].picture = "games_marker";
        s.pages[0].click_dx = 40;
        s.pages[0].click_dy = 120;
        s.pages[1].picture = "start_marker";
        s.pages[1].row_step = 37;

        const std::string path = temp_file("gobot_login_roundtrip_test.ini");
        check(s.save(path), "the login config writes");

        AutoLogin::Settings back;
        check(back.load(path), "and reads back");
        check(back.launcher_path == s.launcher_path &&
              back.launcher_window == s.launcher_window &&
              back.signin_picture == "signin_marker" && back.signin_score > 0.89f &&
              back.channel_index == 5 && back.game_slot == 2,
              "the launcher, the sign-in check and the choices survive");
        check(back.pages.size() == s.pages.size() &&
              back.pages[0].picture == "games_marker" &&
              back.pages[0].click_dx == 40 && back.pages[0].click_dy == 120 &&
              back.pages[1].row_step == 37,
              "each page's picture and click offsets survive");
        check(!back.stale_keys, "a file GoBot wrote has nothing old in it");
        std::remove(path.c_str());
    }

    // A file from before, when GoBot typed an account into VTCZone: an account
    // name, an encrypted password and a sign-in page, on top of the pages that
    // ship. It still loads and still crosses, and nothing of the account is
    // read, kept or written anywhere.
    {
        const std::string path = temp_file("gobot_login_old_test.ini");
        AutoLogin::Settings shipped;
        shipped.channel_index = 4;
        shipped.character_index = 1;
        const std::string sealed = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        check(shipped.save(path) &&
                  IniFile::set_in_place(path, "Login", "username", "someone") &&
                  IniFile::set_in_place(path, "Login", "password", sealed) &&
                  // Whatever an old file named for it; the picture does not matter.
                  IniFile::set_in_place(path, "Page.login_page", "picture", "daily_popup") &&
                  IniFile::set_in_place(path, "Page.login_page", "score", "0.55"),
              "an old login.ini, account and sign-in page and all, is written");

        AutoLogin::Settings old;
        check(old.load(path), "it loads");
        check(old.pages.size() == 7 &&
                  std::none_of(old.pages.begin(), old.pages.end(),
                               [](const AutoLogin::Page& p) { return p.key == "login_page"; }),
              "with the seven pages, and no sign-in page among them");
        check(old.unidentified().empty(), "none of which counts as unidentified");
        check(old.channel_index == 4 && old.character_index == 1,
              "the channel and the character are as they were");
        check(old.stale_keys, "the old keys are noticed -- and only noticed");

        const auto text_of = [](const std::string& p) {
            std::ifstream in(p);
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        };
        const std::string again = temp_file("gobot_login_old_resaved_test.ini");
        check(old.save(again), "it saves");
        const std::string saved = text_of(again);
        check(saved.find("username") == std::string::npos &&
                  saved.find("password") == std::string::npos &&
                  saved.find(sealed) == std::string::npos &&
                  saved.find("Page.login_page") == std::string::npos,
              "and what it saves has no account, no password and no sign-in page in it");

        // The player's own file is theirs: changing the channel from the
        // screen changes that one key and leaves the rest as it was.
        check(IniFile::set_in_place(path, "Login", "channel_index", 6),
              "a channel picked on the screen is written into the old file");
        const std::string kept = text_of(path);
        check(kept.find("channel_index=6") != std::string::npos &&
                  kept.find("username=someone") != std::string::npos &&
                  kept.find("password=" + sealed) != std::string::npos,
              "which leaves the old lines exactly as they were");
        std::remove(again.c_str());
        std::remove(path.c_str());
    }

    // The configs/login.ini that ships, from the source tree, against the
    // table: the same pages, pictures and measurements, and the same [Login].
    // The table is what a missing file is replaced with, so the two differing
    // would be a bot that crosses one way with the file and another without.
    {
#ifdef GOBOT_SOURCE_DIR
        const std::string repo = std::string(GOBOT_SOURCE_DIR) + "/configs/login.ini";
#else
        const std::string repo;
#endif
        AutoLogin::Settings shipped;
        if (repo.empty() || !shipped.load(repo)) {
            std::printf("  [skip] the source tree's configs/login.ini is not here\n");
        } else {
            const AutoLogin::Settings table;
            bool same = shipped.pages.size() == table.pages.size();
            for (std::size_t i = 0; same && i < table.pages.size(); ++i) {
                const AutoLogin::Page& a = shipped.pages[i];
                const AutoLogin::Page& b = table.pages[i];
                same = a.key == b.key && a.picture == b.picture && a.on == b.on &&
                       a.click_dx == b.click_dx && a.click_dy == b.click_dy &&
                       a.wait_s == b.wait_s && std::fabs(a.score - b.score) < 1e-4f &&
                       a.pick == b.pick && a.row_step == b.row_step &&
                       a.row_across == b.row_across && a.row_dx == b.row_dx &&
                       a.row_dy == b.row_dy && a.step_dx == b.step_dx && a.step_dy == b.step_dy &&
                       a.step_pause_ms == b.step_pause_ms && a.row_count == b.row_count;
            }
            check(same, "the shipped configs/login.ini has the table's pages, pictures and "
                        "measurements, field by field");
            check(shipped.launcher_path == table.launcher_path &&
                      shipped.launcher_window == table.launcher_window &&
                      shipped.client_class == table.client_class &&
                      shipped.client_process == table.client_process &&
                      shipped.game_window == table.game_window &&
                      shipped.signin_picture == table.signin_picture &&
                      std::fabs(shipped.signin_score - table.signin_score) < 1e-4f &&
                      shipped.channel_index == table.channel_index &&
                      shipped.character_index == table.character_index &&
                      shipped.game_slot == table.game_slot && shipped.attempts == table.attempts &&
                      shipped.heal_on_arrival == table.heal_on_arrival &&
                      shipped.heal_hp_key == table.heal_hp_key &&
                      shipped.heal_mp_key == table.heal_mp_key && !shipped.stale_keys,
                  "and its [Login] is the table's, with nothing old in it");
        }
    }

    // The file the player is running on, read and never written. Not held to
    // the table: putting a page right after VTCZone or the client changes is
    // what that file is for. Whatever it has, it loads into the seven pages,
    // each with a picture named.
    {
        AutoLogin::Settings live;
        if (!live.load(exe_relative("configs/login.ini"))) {
            std::printf("  [skip] there is no configs/login.ini beside this test\n");
        } else {
            check(live.pages.size() == pages.size() && live.unidentified().empty(),
                  "the configs/login.ini here loads into the seven pages, each named");
        }
    }

    // Each page knows which of the three windows it is drawn on.
    //
    // Hard-coding the switch at a fixed step was wrong in a way that only
    // showed when the flow resumed into the middle: skip-forward would find
    // the channel list on the game, then look for it on the publisher's
    // launcher and wait out the whole minute.
    {
        bool right = true;
        for (const auto& p : pages) {
            const bool launcher = p.step == AutoLogin::Step::GameList ||
                                  p.step == AutoLogin::Step::StartGame;
            const bool client   = p.step == AutoLogin::Step::ClientLauncher;
            const auto want = launcher ? AutoLogin::On::Publisher
                            : client   ? AutoLogin::On::Client
                                       : AutoLogin::On::Game;
            if (p.on != want) right = false;
        }
        check(right, "every page names the window it is drawn on");
    }

    // Three ways of turning an index into a click, because the client uses
    // three. A list moves the click; a carousel cannot, because every entry
    // is drawn in the same place.
    {
        auto pick_of = [&](AutoLogin::Step step) {
            for (const auto& p : pages) if (p.step == step) return p.pick;
            return AutoLogin::Pick::One;
        };
        check(pick_of(AutoLogin::Step::Channel) == AutoLogin::Pick::Row,
              "the channel list is a list");
        check(pick_of(AutoLogin::Step::Character) == AutoLogin::Pick::Slot,
              "the characters are picked where they stand");
        check(pick_of(AutoLogin::Step::Connect) == AutoLogin::Pick::One,
              "a plain button ignores the index");

        check(AutoLogin::pick_from("row", AutoLogin::Pick::One) == AutoLogin::Pick::Row,
              "pick=row reads back");
        check(AutoLogin::pick_from("slot", AutoLogin::Pick::One) == AutoLogin::Pick::Slot,
              "pick=slot reads back");
        check(AutoLogin::pick_from("step", AutoLogin::Pick::One) == AutoLogin::Pick::Step,
              "pick=step reads back");
        check(AutoLogin::pick_from("nonsense", AutoLogin::Pick::Row) == AutoLogin::Pick::Row,
              "an unknown pick keeps what the page already had");
    }

    // The channel and character settings survive the file. A character index
    // that did not come back would silently play the wrong character all
    // night, which is the kind of wrong nobody notices until morning.
    {
        AutoLogin::Settings s;
        s.pages = AutoLogin::default_pages();
        for (auto& p : s.pages) p.picture = "x";
        s.channel_index = 4;
        s.character_index = 2;
        for (auto& p : s.pages) {
            if (p.step == AutoLogin::Step::Channel) {
                p.click_dx = 54; p.click_dy = 140; p.row_step = 31; p.row_count = 9;
            }
            if (p.step == AutoLogin::Step::Character) {
                p.row_dx = -518; p.row_dy = -160; p.row_step = 202;
                p.row_across = true; p.row_count = 3; p.step_pause_ms = 1200;
            }
        }

        const std::string path = temp_file("gobot_login_pick_test.ini");
        check(s.save(path), "the picks write");

        AutoLogin::Settings back;
        check(back.load(path), "and read back");
        check(back.channel_index == 4 && back.character_index == 2,
              "the channel and the character survive");

        const AutoLogin::Page* channel = nullptr;
        const AutoLogin::Page* who = nullptr;
        for (const auto& p : back.pages) {
            if (p.step == AutoLogin::Step::Channel)   channel = &p;
            if (p.step == AutoLogin::Step::Character) who = &p;
        }
        check(channel != nullptr && channel->pick == AutoLogin::Pick::Row &&
              channel->row_step == 31 && channel->row_count == 9 &&
              channel->click_dx == 54 && channel->click_dy == 140,
              "the channel list's measured geometry survives");
        // Negative offsets are the case that matters: the character stage is a
        // long way to the left of, and above, the start button the picture
        // marks, so a field that clamped at zero would click the button twice
        // and start whichever character was already chosen.
        check(who != nullptr && who->pick == AutoLogin::Pick::Slot &&
              who->row_dx == -518 && who->row_dy == -160 &&
              who->row_step == 202 && who->row_across && who->row_count == 3 &&
              who->step_pause_ms == 1200,
              "the stage's slot geometry survives, negative offsets and all");
        check(who != nullptr && (who->click_dx != who->row_dx ||
                                 who->click_dy != who->row_dy),
              "and confirming is a separate point from the slot");
        std::remove(path.c_str());
    }

    // Where the crossing goes on from, given what is on screen. The same rule
    // starts each attempt and moves it on while it waits for a page.
    {
        const auto at = [&](const char* key) {
            for (std::size_t i = 0; i < pages.size(); ++i)
                if (pages[i].key == key) return i;
            return AutoLogin::kNone;
        };
        const std::size_t games = at("game_list"), start = at("start_game");
        const std::size_t client = at("client_start"), connect = at("connect");
        const std::size_t channel = at("channel_list"), character = at("character");
        const auto same_as_connect = [&](std::size_t i) {
            return pages[i].picture == pages[connect].picture;
        };

        check(AutoLogin::furthest_up(pages, 0, [](std::size_t) { return false; }) ==
                  AutoLogin::kNone,
              "nothing up is nowhere to go on from");
        check(AutoLogin::furthest_up(pages, 0, [&](std::size_t i) {
                  return i == games || i == start; }) == start,
              "VTCZone showing the game and Choi game goes on from Choi game");
        check(AutoLogin::furthest_up(pages, 0, same_as_connect) == connect,
              "the connect button proves only the first connect, not the one after the channel");
        check(AutoLogin::furthest_up(pages, connect + 1, same_as_connect) == AutoLogin::kNone,
              "so waiting for one connect, the other is never a page further on");
        check(AutoLogin::furthest_up(pages, start + 1, [&](std::size_t i) {
                  return i == client; }) == client,
              "waiting on Choi game with the game's START up goes on to START, not a second "
              "Choi game");
        check(AutoLogin::furthest_up(pages, client + 1, [&](std::size_t i) {
                  return i == channel; }) == channel,
              "and a client already on its channel list costs no wait");
        check(AutoLogin::furthest_up(pages, 0, [&](std::size_t i) {
                  return i == character; }, "channel_list") == channel,
              "nothing goes past the page a person asked to be left on");

        // Nothing of VTCZone's while the game or its launcher is open. Its
        // own picture of itself still shows Choi game under a game that is
        // part way in, and pressing it starts a second launcher.
        check(AutoLogin::first_allowed_page(pages, false, false) == 0 &&
                  AutoLogin::first_allowed_page(pages, true, false) == client &&
                  AutoLogin::first_allowed_page(pages, false, true) == connect &&
                  AutoLogin::first_allowed_page(pages, true, true) == connect,
              "the first page that may be touched: VTCZone's with nothing open, START with "
              "the launcher open, Ket Noi with the game open");
        const auto up_of = [](std::initializer_list<std::size_t> shown) {
            const std::vector<std::size_t> list(shown);
            return [list](std::size_t i) {
                return std::find(list.begin(), list.end(), i) != list.end();
            };
        };
        const std::size_t with_game = AutoLogin::resume_from(pages, false, true, up_of({ start }));
        check(with_game == connect && pages[with_game].on == AutoLogin::On::Game,
              "with the game up and Choi game seen in VTCZone's picture, the crossing goes on "
              "from the game's first page, never Choi game");
        check(AutoLogin::resume_from(pages, false, true, up_of({ games, start, channel })) ==
                  channel,
              "and from the game's furthest page that is up");
        const std::size_t with_launcher =
            AutoLogin::resume_from(pages, true, false, up_of({ games, start }));
        check(with_launcher == client && pages[with_launcher].on == AutoLogin::On::Client,
              "with only the game's launcher up, from its START, never Choi game");
        check(AutoLogin::resume_from(pages, false, false, up_of({ games, start })) == start,
              "with neither up, Choi game is carried on from as before");
        check(AutoLogin::resume_from(pages, false, true, up_of({ start }), "start_game") >= connect,
              "and a page asked to be stopped at that may not be touched is not walked back to");
    }

    // A frame of VTCZone, read for whether it is signed in. Pictures nothing
    // else looks like -- blocks of random colour, big enough to survive being
    // looked at at a quarter of the size -- stand in for the real ones, on a
    // smooth background that looks like none of them. A background of random
    // blocks too did not work: a small picture of a few dozen blocks matches
    // some patch of a large random one by chance, well above 0.7.
    const auto blocks = [](unsigned seed, int w, int h) {
        Frame f = make_frame(w, h);
        std::mt19937 r(seed);
        for (int by = 0; by < h; by += 4)
            for (int bx = 0; bx < w; bx += 4) {
                const auto red = static_cast<std::uint8_t>(r() % 256);
                const auto green = static_cast<std::uint8_t>(r() % 256);
                for (int y = by; y < std::min(h, by + 4); ++y)
                    for (int x = bx; x < std::min(w, bx + 4); ++x)
                        put_px(f, x, y, red, green, static_cast<std::uint8_t>(255 - red));
            }
        return f;
    };
    const auto backdrop = [](int w, int h) {
        Frame f = make_frame(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                put_px(f, x, y, static_cast<std::uint8_t>(x * 255 / w),
                       static_cast<std::uint8_t>(y * 255 / h), 128);
        return f;
    };
    const auto paste = [](Frame& into, const Frame& what, int px, int py) {
        for (int y = 0; y < what.height; ++y)
            for (int x = 0; x < what.width; ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * what.width + x) * 4;
                if (what.pixels[i + 3] < 128) continue;
                put_px(into, px + x, py + y, what.pixels[i + 2], what.pixels[i + 1],
                       what.pixels[i]);
            }
    };
    {
        const Frame poster = blocks(101, 64, 80);
        const Frame play = blocks(202, 56, 24);
        const Frame signin = blocks(303, 72, 24);
        TemplateStore store;
        store.put("app_select_page", poster);
        store.put("start_game_page", play);
        store.put("login_page", signin);
        AutoLogin::Settings s;
        using Seen = AutoLogin::Publisher;

        Frame shown = backdrop(640, 480);
        check(AutoLogin::look_at_publisher(shown, store, s) == Seen::Unknown,
              "VTCZone showing none of it is not known to be either");
        paste(shown, signin, 300, 260);
        check(AutoLogin::look_at_publisher(shown, store, s) == Seen::SignIn,
              "its sign-in button with no game in sight is the sign-in form");
        paste(shown, play, 120, 380);
        check(AutoLogin::look_at_publisher(shown, store, s) == Seen::Ready,
              "but Choi game anywhere on it means signed in, whatever else is there");

        Frame games = backdrop(640, 480);
        paste(games, poster, 40, 60);
        check(AutoLogin::look_at_publisher(games, store, s) == Seen::Ready,
              "and so does the game's poster");

        AutoLogin::Settings off = s;
        off.signin_picture.clear();
        Frame form = backdrop(640, 480);
        paste(form, signin, 200, 200);
        check(AutoLogin::look_at_publisher(form, store, off) == Seen::Unknown,
              "with the check turned off, the sign-in form is not looked for");
        check(AutoLogin::look_at_publisher(Frame{}, store, s) == Seen::Unknown,
              "and a window that could not be captured says nothing");
    }

    // The same with the pictures GoBot ships, where they are installed.
    {
        TemplateStore real;
        if (!AutoLogin::load_pictures(real, exe_relative("configs/templates.ini")) ||
            !real.get("login_page") || !real.get("start_game_page")) {
            std::printf("  [skip] the login pictures are not installed here\n");
        } else {
            const AutoLogin::Settings s;
            check(s.recognised(real) == s.pages.size(),
                  "every page of the crossing has its picture among those installed");
            Frame form = backdrop(1024, 768);
            paste(form, *real.get("login_page"), 360, 420);
            check(AutoLogin::look_at_publisher(form, real, s) == AutoLogin::Publisher::SignIn,
                  "the shipped sign-in button is found as the sign-in form");
            Frame ready = backdrop(1024, 768);
            paste(ready, *real.get("start_game_page"), 402, 547);
            check(AutoLogin::look_at_publisher(ready, real, s) == AutoLogin::Publisher::Ready,
                  "and the shipped Choi game as signed in");
        }
    }

    // How many pages can be recognised: named and loaded, not just named.
    {
        AutoLogin::Settings s;
        TemplateStore store;
        check(s.recognised(store) == 0, "with no pictures loaded no page can be recognised");
        store.put("app_select_page", blocks(1, 16, 16));
        store.put("start_game_page", blocks(2, 16, 16));
        check(s.recognised(store) == 2, "a page counts once its picture is loaded");
        s.pages[0].picture.clear();
        check(s.recognised(store) == 1, "and a page with no picture named never does");

        // What a keeper keeps for its hours of watching: the crossing's own.
        const AutoLogin::Settings shipped;
        TemplateStore every;
        for (const auto& p : shipped.pages) every.put(p.picture, blocks(3, 16, 16));
        every.put(shipped.signin_picture, blocks(4, 16, 16));
        every.put("inventory", blocks(5, 16, 16));
        every.put("hpmp_indicator2", blocks(6, 16, 16));
        const TemplateStore kept = AutoLogin::pictures_for(shipped, every);
        // Six pages' pictures -- both connect steps are one button -- and the
        // sign-in form's.
        check(kept.size() == 7 && shipped.recognised(kept) == shipped.pages.size() &&
                  kept.get(shipped.signin_picture) != nullptr,
              "a keeper keeps the crossing's pictures and the sign-in form's");
        check(!kept.has("inventory") && !kept.has("hpmp_indicator2"),
              "and none of the rest of the set");
    }

    // The Security Alert box the game's launcher can put up, which is a
    // person's to answer.
    {
        check(AutoLogin::is_security_alert("#32770", "Security Alert"),
              "the Security Alert box is known by its class and its title");
        check(AutoLogin::is_security_alert("#32770", "  security alert "),
              "whatever the case and the spaces");
        check(!AutoLogin::is_security_alert("#32770", ""),
              "the game's own untitled launcher is not one");
        check(!AutoLogin::is_security_alert("Chrome_WidgetWin_1", "Security Alert"),
              "nor is some other window that happens to have the title");
    }

    // Whether VTCZone is where the file says -- by the wide name, so an
    // install folder with Vietnamese in its name is found too.
    {
        AutoLogin::Settings s;
        s.launcher_path = temp_file("gobot_login_launcher_test.exe");
        { std::ofstream(s.launcher_path) << "x"; }
        check(s.launcher_found(), "a launcher that is there is found");
        std::remove(s.launcher_path.c_str());
        check(!s.launcher_found(), "and one that is not, is not");

        const std::string dir = temp_file("");
        s.launcher_path = dir;
        check(!s.launcher_found(), "a folder is not a launcher");
        s.launcher_path.clear();
        check(!s.launcher_found(), "nor is no path at all");

        const std::string named = dir + "gobot_thử_vtczone.exe";
        const int n = MultiByteToWideChar(CP_UTF8, 0, named.c_str(), -1, nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(n > 0 ? n : 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, named.c_str(), -1, wide.data(), n);
        const HANDLE h = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            std::printf("  [skip] could not make a file with a Vietnamese name here\n");
        } else {
            CloseHandle(h);
            s.launcher_path = named;
            check(s.launcher_found(), "one in a folder with a Vietnamese name is found too");
            DeleteFileW(wide.c_str());
        }
    }

    // VTCZone installed somewhere other than the file says: while it runs,
    // its own path is used -- and never over a path in the file that is
    // there, nor in place of no path at all. Decided on a path handed in as
    // the running one's: reading a running program's path is refused in this
    // process (see test_no_real_input), so no VTCZone is asked anything.
    {
        const std::string elsewhere = temp_file("gobot_test_vtczone_elsewhere.exe");
        { std::ofstream(elsewhere) << "x"; }
        AutoLogin::Settings moved;
        moved.launcher_path = temp_file("gobot_no_vtczone_here.exe");
        check(moved.adopt_launcher(elsewhere) && moved.launcher_path == elsewhere &&
                  moved.launcher_found(),
              "a launcher path that names nothing, with VTCZone running elsewhere: the running "
              "one's is used");
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        AutoLogin::Settings kept;
        kept.launcher_path = wide_to_utf8(self);
        check(!kept.adopt_launcher(elsewhere) && kept.launcher_path == wide_to_utf8(self),
              "and a path that is there is kept, whatever else is running");
        // The 2026-10-15 case: a test's empty path became the running
        // VTCZone's, and the crossing it was in went on to the real desktop.
        AutoLogin::Settings none;
        none.launcher_path.clear();
        check(!none.adopt_launcher(elsewhere) && none.launcher_path.empty(),
              "and no path at all stays none: no launcher to start is not undone by one running");
        AutoLogin::Settings gone;
        gone.launcher_path = temp_file("gobot_no_vtczone_here.exe");
        check(!gone.adopt_launcher(temp_file("gobot_test_vtczone_gone.exe")) &&
                  gone.launcher_path == temp_file("gobot_no_vtczone_here.exe"),
              "nor is a running path taken that is not there");
        std::remove(elsewhere.c_str());
    }

    // What the simple screen says when the crossing stops, and whether
    // another attempt is worth making.
    {
        using Why = AutoLogin::Why;
        const std::string signin = AutoLogin::advice(Why::SignIn);
        check(signin.rfind("VTCZone đang hiện màn hình đăng nhập", 0) == 0 &&
                  signin.find("ghi nhớ") != std::string::npos,
              "the sign-in form: sign in to VTCZone once, with its remember option");
        check(AutoLogin::advice(Why::LauncherMissing, "D:\\VTC\\VTCZone.exe")
                      .find("D:\\VTC\\VTCZone.exe") != std::string::npos,
              "a launcher that is not there is named");
        check(AutoLogin::advice(Why::GamePage, "60").find("60 giây") != std::string::npos &&
                  AutoLogin::advice(Why::ClientStart, "90").find("Security Alert") != std::string::npos,
              "a page that never came says how long it was waited for");
        check(AutoLogin::advice(Why::NoPicture, "channel_first_index")
                      .find("channel_first_index") != std::string::npos,
              "a missing picture is named");
        bool all_said = true;
        for (int w = static_cast<int>(Why::Cancelled); w <= static_cast<int>(Why::BadPoint); ++w)
            if (AutoLogin::advice(static_cast<Why>(w), "x").empty()) all_said = false;
        check(all_said && AutoLogin::advice(Why::None).empty(),
              "every reason to stop has words for the screen, and no reason has none");

        check(!AutoLogin::worth_retrying(Why::SignIn) &&
                  !AutoLogin::worth_retrying(Why::LauncherMissing) &&
                  !AutoLogin::worth_retrying(Why::NoPicture) &&
                  !AutoLogin::worth_retrying(Why::SecurityAlert) &&
                  !AutoLogin::worth_retrying(Why::BadPoint) &&
                  !AutoLogin::worth_retrying(Why::Cancelled),
              "what another attempt cannot get past stops the crossing at once");

        // A point off the window is an offset written wrong, said as one: the
        // page, its window, and the keys of configs/login.ini to put right.
        const std::string bad = AutoLogin::advice(Why::BadPoint, "character");
        check(bad.find("[Page.character]") != std::string::npos &&
                  bad.find("configs/login.ini") != std::string::npos &&
                  bad.find("click_dx/click_dy") != std::string::npos &&
                  bad.find("chọn nhân vật") != std::string::npos &&
                  bad.find("cửa sổ game") != std::string::npos,
              "a point off the window names [Page.character], its screen and window, and "
              "configs/login.ini");
        check(AutoLogin::advice(Why::BadPoint, "start_game").find("cửa sổ VTCZone") !=
                  std::string::npos,
              "and VTCZone, for a page drawn on VTCZone");
        check(std::string(AutoLogin::step_name_vi(AutoLogin::Step::EndClient)) ==
                  "đóng game đang mở dở",
              "ending a half-started game has its Vietnamese words");
        check(AutoLogin::worth_retrying(Why::GamePage) &&
                  AutoLogin::worth_retrying(Why::Channel) &&
                  AutoLogin::worth_retrying(Why::World) &&
                  AutoLogin::worth_retrying(Why::LauncherNoWindow),
              "and a page that was slow to come gets the next attempt");
        check(AutoLogin::gave_up(3, Why::Channel) ==
                  "Đã thử 3 lần không vào lại được: Không thấy danh sách kênh (Chọn Kênh). "
                  "Cần người kiểm tra.",
              "after every attempt: how many, and the last reason, one full stop each");

        bool all_named = true;
        for (int st = static_cast<int>(AutoLogin::Step::Idle);
             st <= static_cast<int>(AutoLogin::Step::Blocked); ++st)
            if (std::string(AutoLogin::step_name_vi(static_cast<AutoLogin::Step>(st))) == "?")
                all_named = false;
        check(all_named, "every step has a Vietnamese name for the screen");
    }

    // The flow refuses, before touching any window, whatever it cannot cross.
    {
        using Why = AutoLogin::Why;
        TemplateStore all;
        for (const auto& p : pages) all.put(p.picture, blocks(9, 16, 16));

        AutoLogin::Settings blank;
        for (auto& p : blank.pages) p.picture.clear();
        AutoLogin unnamed;
        unnamed.set_settings(blank);
        unnamed.set_templates(&all);
        check(!unnamed.run() && unnamed.why() == Why::NotSetUp && unnamed.attempts_made() == 0,
              "a table with no pictures named refuses before the first attempt");

        TemplateStore none;
        AutoLogin missing;
        missing.set_settings(AutoLogin::Settings{});
        missing.set_templates(&none);
        check(!missing.run() && missing.why() == Why::NoPicture &&
                  missing.why_detail() == pages.front().picture && missing.attempts_made() == 0,
              "a picture that is not installed is named, and nothing is started");

        AutoLogin::Settings nowhere;
        nowhere.launcher_path.clear();
        AutoLogin no_launcher;
        no_launcher.set_settings(nowhere);
        no_launcher.set_templates(&all);
        check(!no_launcher.run() && no_launcher.why() == Why::NoLauncher &&
                  no_launcher.attempts_made() == 0,
              "and so does a file with no launcher in it");
        check(no_launcher.step() == AutoLogin::Step::Blocked,
              "and leaves it blocked for a person");
    }

    // Numbers in the file that are not what they look like. A comma for the
    // decimal point reads "0,80" as 0, and a picture matched at 0 is found in
    // every frame; a wait one digit too long ran the clock over.
    {
        const std::string path = temp_file("gobot_login_numbers_test.ini");
        {
            std::ofstream out(path);
            out << "[Login]\n"
                   "signin_score=0,80\n"
                   "[Page.connect]\n"
                   "score=0\n"
                   "wait_s=2147483647\n"
                   "[Page.channel_list]\n"
                   "score=1.5\n"
                   "wait_s=-5\n"
                   "[Page.character]\n"
                   "score=0.65\n"
                   "step_pause_ms=999999999\n";
        }
        AutoLogin::Settings s;
        check(s.load(path), "a file with odd numbers in it loads");
        const auto page_of = [&](const char* key) -> const AutoLogin::Page* {
            for (const auto& p : s.pages)
                if (p.key == key) return &p;
            return nullptr;
        };
        const AutoLogin::Page* connect = page_of("connect");
        const AutoLogin::Page* channel = page_of("channel_list");
        const AutoLogin::Page* who = page_of("character");
        check(s.signin_score > 0.79f && s.signin_score < 0.81f,
              "signin_score=0,80 is not taken for 0: the usual 0.80 is used");
        check(connect && connect->score > 0.69f && connect->score < 0.71f && channel &&
                  channel->score > 0.69f && channel->score < 0.71f,
              "nor is a page's score of 0, or of 1.5");
        check(who && who->score > 0.64f && who->score < 0.66f,
              "while a real score, lowered, is kept");
        check(connect && connect->wait_s == AutoLogin::kMaxWaitS && channel && channel->wait_s == 1,
              "a wait is held to between one second and the most a page is waited for");
        check(who && who->step_pause_ms == 10000, "and a pause between two clicks to ten seconds");
        std::remove(path.c_str());
    }

    // Which file is missing, and which only will not open just now. Only the
    // first is ever written over with the table: the second is somebody's.
    {
        const std::string none = temp_file("gobot_login_surely_not_here.ini");
        std::remove(none.c_str());
        check(AutoLogin::Settings::file_missing(none), "a file that is not there is missing");
        check(AutoLogin::Settings::file_missing(temp_file("gobot_no_such_folder\\login.ini")),
              "and so is one in a folder that is not there");
        check(!AutoLogin::Settings::file_missing(temp_file("")) &&
                  !AutoLogin::Settings::file_missing(""),
              "a folder is not a missing file, and nor is no name at all");

        const std::string held = temp_file("gobot_login_held_test.ini");
        { std::ofstream(held) << "# somebody's own notes\n[Login]\nchannel_index=6\n"; }
        const HANDLE lock = CreateFileA(held.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock == INVALID_HANDLE_VALUE) {
            std::printf("  [skip] could not hold a file open here\n");
        } else {
            AutoLogin::Settings s;
            const bool read = s.load(held);
            const bool missing = AutoLogin::Settings::file_missing(held);
            CloseHandle(lock);
            check(!read && !missing,
                  "a file held open by something else does not load, and is not missing "
                  "either -- so it is not written over");
        }
        std::remove(held.c_str());
    }

    // How long VTCZone's sign-in form has to stay up before it is stopped for.
    {
        using A = AutoLogin;
        check(!A::signin_settled(A::kSignInHoldMs - 1, -1) &&
                  A::signin_settled(A::kSignInHoldMs, -1),
              "a VTCZone that was running already: the form, up five seconds, stops it");
        check(!A::signin_settled(A::kSignInHoldMs * 3, A::kSignInGraceMs - 1),
              "one this flow has just started gets its first half minute to sign itself in");
        check(A::signin_settled(A::kSignInHoldMs, A::kSignInGraceMs) &&
                  !A::signin_settled(A::kSignInHoldMs - 1, A::kSignInGraceMs * 2),
              "and after that the same five seconds as any other");
        check(A::kSignInGraceMs + A::kSignInHoldMs < 60 * 1000,
              "which still stops it well inside the minute its first page is waited for");
    }

    // The pictures a crossing needs, by name: what a keeper that reads the
    // file again compares, to load pictures again only when they changed.
    {
        const AutoLogin::Settings s;
        const auto names = AutoLogin::picture_names(s);
        check(names.size() == 7 && std::is_sorted(names.begin(), names.end()) &&
                  std::find(names.begin(), names.end(), "login_page") != names.end(),
              "the shipped crossing needs seven pictures, the sign-in form's among them");
        AutoLogin::Settings picked = s;
        picked.channel_index = 5;
        picked.character_index = 2;
        check(AutoLogin::picture_names(picked) == names,
              "a channel or a character picked changes none of them");
        AutoLogin::Settings renamed = s;
        renamed.pages[2].picture = "start_game_button_new";
        check(AutoLogin::picture_names(renamed) != names, "a page given another picture does");
        AutoLogin::Settings off = s;
        off.signin_picture.clear();
        check(AutoLogin::picture_names(off).size() == 6,
              "and with the sign-in check off there is one fewer");
    }

    // A click held back by a window in the way, in Vietnamese.
    {
        using Why = AutoLogin::Why;
        const std::string other = AutoLogin::advice(Why::Covered, "chrome.exe");
        check(other.find("chrome.exe") != std::string::npos &&
                  other.find("che chỗ") != std::string::npos,
              "a window in the way is named by its program");
        check(AutoLogin::advice(Why::Covered, "gobot_gui.exe").rfind("Cửa sổ GoBot", 0) == 0,
              "GoBot's own window is called GoBot's");
        check(AutoLogin::advice(Why::Covered, "?").find('?') == std::string::npos,
              "one whose program cannot be told is just another window");
        check(AutoLogin::advice(Why::Covered, "").find("màn hình") != std::string::npos,
              "and a point off the screen says so");
        check(AutoLogin::worth_retrying(Why::Covered),
              "the next attempt brings the windows to the front again, so it is worth one");
        // The taskbar cannot be moved aside as a window can: "move it away"
        // was advice nobody could follow.
        const std::string bar_click = AutoLogin::advice(Why::Covered, "taskbar");
        const std::string bar_look = AutoLogin::hidden_advice("taskbar");
        check(bar_click.rfind("Thanh tác vụ", 0) == 0 && bar_click.find("tự ẩn") != std::string::npos &&
                  bar_look.rfind("Thanh tác vụ", 0) == 0 && bar_look.find("tự ẩn") != std::string::npos &&
                  bar_look.find("dời cửa sổ") == std::string::npos,
              "the taskbar in the way: have it hide itself, or the window higher -- not \"move it\"");
    }

    // What lies over a client area, by where it is drawn and where the
    // screens are -- the rule behind uncovered(), with no window of anybody's.
    {
        using Box = AutoLogin::Box;
        const std::vector<Box> laptop{ Box{ 0, 0, 1366, 768 } };
        // A 1024x768 game under a title bar: its client reaches 31 px past
        // the bottom of a 768-px screen.
        const Box game{ 171, 31, 1195, 799 };
        check(!AutoLogin::lies_over(Box{ 0, 766, 1366, 806 }, game, laptop, true),
              "a taskbar hidden past the bottom, 2 px of it left on the screen, is not over the game");
        check(AutoLogin::lies_over(Box{ 0, 728, 1366, 768 }, game, laptop, true),
              "the same taskbar shown, over the game's bottom 40 px, is");
        check(AutoLogin::lies_over(Box{ 0, 766, 1366, 806 }, game, laptop, false),
              "and a 2-px strip that is not the taskbar is over it like any other window");
        check(!AutoLogin::lies_over(Box{ 300, 780, 500, 800 }, game, laptop),
              "a window wholly past the bottom edge, over only the part of the game nobody sees, "
              "is not over it");
        check(AutoLogin::lies_over(Box{ 300, 700, 500, 760 }, game, laptop),
              "while one on the screen over the game is");
        check(!AutoLogin::lies_over(Box{ 1200, 300, 1300, 400 }, game, laptop),
              "and one beside it is not");
        // Two screens side by side, the second shorter: the part of a game on
        // the second counts, the gap under the second screen does not.
        const std::vector<Box> two{ Box{ 0, 0, 1366, 768 }, Box{ 1366, 0, 2646, 720 } };
        const Box wide{ 1000, 100, 2024, 868 };
        check(AutoLogin::lies_over(Box{ 1500, 200, 1600, 300 }, wide, two),
              "a window over the part of the game on the second screen is over it");
        check(!AutoLogin::lies_over(Box{ 1500, 740, 1600, 800 }, wide, two),
              "one under the shorter second screen, on no screen at all, is not");
    }

    // Whether a click lands where it is meant to. Small windows of this
    // test's own, kept on top without taking the focus and gone again at
    // once: nothing the player has open is touched, and nothing is clicked.
    {
        const HINSTANCE self = GetModuleHandleA(nullptr);
        WNDCLASSA wc{};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = self;
        wc.lpszClassName = "GoBotTestClickTarget";
        RegisterClassA(&wc);        // already there from an earlier run: the same
        const auto make = [&](DWORD ex, DWORD style, const char* title, int x, int y, int w,
                              int h, HWND parent) {
            return CreateWindowExA(ex, wc.lpszClassName, title, style, x, y, w, h, parent,
                                   nullptr, self, nullptr);
        };
        const DWORD on_top = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        // Room for the target, every window put over or beside it, and the
        // launcher's stand-in below -- clear of the game, if it is up.
        POINT spot{};
        const bool clear = clear_of_the_game(320, 210, 40, spot);
        const int x0 = static_cast<int>(spot.x);
        const int y0 = static_cast<int>(spot.y);

        HWND target = clear ? make(on_top, WS_POPUP, "", x0, y0, 240, 160, nullptr) : nullptr;
        if (target) ShowWindow(target, SW_SHOWNOACTIVATE);
        const POINT middle{ x0 + 120, y0 + 80 };
        if (!clear) {
            std::printf("  [skip] the game is up and no corner of the screen is clear of it: the "
                        "click tests' windows would cover it\n");
        } else if (target == nullptr || WindowFromPoint(middle) != target) {
            // Something else kept on top there, or no desktop to show on.
            std::printf("  [skip] a test window could not be put on top here\n");
        } else {
            std::string in_way = "x";
            check(AutoLogin::lands_on(target, 120, 80, &in_way) && in_way.empty(),
                  "a click on a window nothing covers lands on it");
            check(!AutoLogin::lands_on(target, -1, 80) && !AutoLogin::lands_on(target, 240, 80) &&
                      !AutoLogin::lands_on(target, 120, 160),
                  "a point off its client area does not");
            using L = AutoLogin::Landing;
            in_way = "x";
            check(AutoLogin::landing(target, -1, 5, &in_way) == L::OffClient && in_way.empty() &&
                      AutoLogin::landing(target, 240, 5) == L::OffClient &&
                      AutoLogin::landing(target, 5, 160) == L::OffClient,
                  "and says why: (-1,5) and (client width,5) are off its client area, an offset "
                  "gone wrong, with no window blamed");
            check(AutoLogin::landing(target, 239, 80) == L::Lands,
                  "while its last column is on it");

            HWND child = make(0, WS_CHILD | WS_VISIBLE, "", 10, 10, 60, 40, target);
            check(child != nullptr && AutoLogin::lands_on(target, 30, 30),
                  "a point on a child of it -- a button, the view inside a frame -- does");
            check(AutoLogin::uncovered(target),
                  "and with only its own child on it, the window is uncovered");

            HWND cover = make(on_top, WS_POPUP, "", x0 + 140, y0, 160, 160, nullptr);
            if (cover) ShowWindow(cover, SW_SHOWNOACTIVATE);
            in_way.clear();
            const bool held = !AutoLogin::lands_on(target, 200, 80, &in_way);
            std::string named = in_way;
            for (char& c : named)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            check(cover != nullptr && held && named.rfind("gobot_test", 0) == 0,
                  "a point another window covers does not, and that window's program is named");
            check(AutoLogin::landing(target, 200, 80) == L::Covered,
                  "-- covered, which is a hold, not a bad offset");
            check(AutoLogin::lands_on(target, 60, 80),
                  "while a point beside the other window still does");
            check(!AutoLogin::uncovered(target),
                  "a window with another over a part of it is not uncovered, so a frame of "
                  "it read off the screen is not trusted to show the sign-in form");
            std::string over;
            const bool hid = !AutoLogin::uncovered(target, &over);
            for (char& c : over)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            check(cover != nullptr && hid && over.rfind("gobot_test", 0) == 0,
                  "and what is over it is named, for the crossing to say while it waits");
            if (cover) ShowWindow(cover, SW_HIDE);
            check(AutoLogin::lands_on(target, 200, 80) && AutoLogin::uncovered(target),
                  "and once that window is gone, the point it covered does too");

            // A window over a strip along its bottom edge, where the connect
            // page's button is, touching none of the five points across it.
            // The frame used to be trusted, the page read as absent, and a
            // game waiting on Kết Nối was ended as stuck.
            HWND strip = make(on_top, WS_POPUP, "", x0 + 90, y0 + 135, 60, 20, nullptr);
            if (strip) ShowWindow(strip, SW_SHOWNOACTIVATE);
            over.clear();
            const bool strip_hid = !AutoLogin::uncovered(target, &over);
            for (char& c : over)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            check(strip != nullptr && strip_hid && over.rfind("gobot_test", 0) == 0 &&
                      AutoLogin::over(target) == strip,
                  "a window over a strip of it that no point across it touches: not uncovered, "
                  "and named");
            if (strip) DestroyWindow(strip);
            check(AutoLogin::uncovered(target), "and gone again, nothing is over it");

            // Above it but beside it: not over it.
            HWND beside = make(on_top, WS_POPUP, "", x0 + 250, y0, 60, 60, nullptr);
            if (beside) ShowWindow(beside, SW_SHOWNOACTIVATE);
            check(beside != nullptr && AutoLogin::uncovered(target),
                  "a window above it but beside it leaves it uncovered");
            if (beside) DestroyWindow(beside);

            // A popup of its own -- a dialog of the game's -- is part of it.
            HWND popup = make(on_top, WS_POPUP, "", x0 + 90, y0 + 135, 60, 20, target);
            if (popup) ShowWindow(popup, SW_SHOWNOACTIVATE);
            check(popup != nullptr && AutoLogin::uncovered(target),
                  "and a popup of its own over it is its own, not in the way");
            if (popup) DestroyWindow(popup);

            // Dragged part off the screen: its left quarter points are on no
            // screen and read black, which hides nothing -- before, a game
            // like that was never looked at, and its wait ran out as "not
            // found".
            const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
            // The quarter of it left on the screen is where the screen
            // begins, which has to be clear of the game as well.
            if (over_the_game(RECT{ left, y0, left + 140, y0 + 160 }, game_windows_on_screen())) {
                std::printf("  [skip] the game is at the screen's left edge, where the test "
                            "window would go half off the screen\n");
            } else {
                SetWindowPos(target, nullptr, left - 100, y0, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                if (AutoLogin::landing(target, 60, 40) == L::OffScreen &&
                    AutoLogin::landing(target, 120, 80) == L::Lands &&
                    AutoLogin::landing(target, 180, 40) == L::Lands &&
                    AutoLogin::landing(target, 180, 120) == L::Lands) {
                    check(AutoLogin::uncovered(target),
                          "a window dragged part off the screen, nothing over it, is uncovered");

                    // Kept on top, wholly past the screen's edge, over only
                    // the part of it out there: nobody sees that part, the
                    // frame included. Counted, the bottom of a game taller
                    // than the screen -- under a hidden taskbar's rest -- had
                    // a working game waited on until it was given up.
                    HWND past = make(on_top, WS_POPUP, "", left - 90, y0 + 20, 80, 60, nullptr);
                    if (past) ShowWindow(past, SW_SHOWNOACTIVATE);
                    std::string by = "x";
                    check(past != nullptr && AutoLogin::over(target) == nullptr &&
                              AutoLogin::uncovered(target, &by) && by.empty(),
                          "a window kept on top wholly off the screen, over only the part of it "
                          "out there, is in nobody's way");
                    if (past) DestroyWindow(past);
                } else {
                    std::printf("  [skip] the test window could not be put half off the screen\n");
                }
            }

            SetWindowPos(target, nullptr, left - 1000, y0, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            in_way = "x";
            check(!AutoLogin::lands_on(target, 120, 80, &in_way) && in_way.empty(),
                  "a window dragged off every screen is not clicked, and nothing is blamed");
            check(AutoLogin::landing(target, 120, 80) == L::OffScreen,
                  "-- off the screen, which is a hold too");
            check(!AutoLogin::uncovered(target),
                  "and with nothing of it on a screen, it is not uncovered either");
            if (cover) DestroyWindow(cover);
        }
        if (target) DestroyWindow(target);
        check(!AutoLogin::lands_on(nullptr, 0, 0) && !AutoLogin::uncovered(nullptr),
              "and no window is never landed on, nor uncovered");

        // A minimised VTCZone, found among its program's windows -- where
        // window_of never looks -- so that it can be brought back.
        char exe[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string me = exe;
        me = me.substr(me.find_last_of("\\/") + 1);
        const std::string title = "GoBot test launcher " + std::to_string(GetCurrentProcessId());
        AutoLogin::Settings s;
        s.launcher_window = title;
        HWND down = clear ? make(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_OVERLAPPEDWINDOW,
                                 title.c_str(), x0, y0, 300, 200, nullptr)
                          : nullptr;
        if (!clear) {
            std::printf("  [skip] the game is up and no corner of the screen is clear of it: the "
                        "launcher's stand-in, shown again, would cover it\n");
        } else if (down == nullptr) {
            std::printf("  [skip] could not make a window to minimise here\n");
        } else {
            ShowWindow(down, SW_SHOWMINNOACTIVE);
            check(IsIconic(down) && AutoLogin::minimised_window(s, me) == down,
                  "a minimised launcher is found by its program and its title");
            check(!AutoLogin::lands_on(down, 10, 10), "and is never clicked while minimised");
            check(AutoLogin::landing(down, 10, 10) == AutoLogin::Landing::Minimised &&
                      AutoLogin::landing(down, 5000, 5000) == AutoLogin::Landing::Minimised,
                  "and minimised is never taken for a bad offset, wherever the point is");
            AutoLogin::Settings other = s;
            other.launcher_window = title + " (another)";
            check(AutoLogin::minimised_window(other, me) == nullptr &&
                      AutoLogin::minimised_window(s, "surely_not_this_one.exe") == nullptr,
                  "another title is not it, nor is another program's window");
            ShowWindow(down, SW_SHOWNOACTIVATE);
            check(!IsIconic(down) && AutoLogin::minimised_window(s, me) == nullptr,
                  "and once it is up again, it is not a minimised one");
            DestroyWindow(down);
        }
    }

    // The crossing itself, on a pretend desktop: a window of this test's
    // standing in for the game, pictures made up for its pages, and hands
    // that only count. The hands are the only way the crossing reaches the
    // desktop, so nothing real is clicked, brought forward or started, and
    // VTCZone, the game's launcher and the game are never asked after.
    {
        using Why = AutoLogin::Why;
        using On = AutoLogin::On;
        const HINSTANCE self = GetModuleHandleA(nullptr);
        WNDCLASSA wc{};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = self;
        wc.lpszClassName = "GoBotTestPretendGame";
        RegisterClassA(&wc);        // already there from an earlier run: the same
        // Kept on top for every crossing below: clear of the real game, if it
        // is up.
        POINT spot{};
        const bool clear = clear_of_the_game(240, 160, 60, spot);
        const int x0 = static_cast<int>(spot.x);
        const int y0 = static_cast<int>(spot.y);
        HWND game = clear ? CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                            wc.lpszClassName, "", WS_POPUP, x0, y0, 240, 160,
                                            nullptr, nullptr, self, nullptr)
                          : nullptr;
        if (game) ShowWindow(game, SW_SHOWNOACTIVATE);
        // VTCZone's stand-in is never shown: only its picture is looked at.
        HWND vtczone = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, "",
                                       WS_POPUP, x0, y0, 400, 300, nullptr, nullptr, self, nullptr);
        const bool on_top = game != nullptr && WindowFromPoint(POINT{ x0 + 120, y0 + 80 }) == game;

        // One picture per page, nothing like the others or the backdrop.
        TemplateStore made_up;
        unsigned seed = 700;
        for (const auto& p : pages)
            if (!made_up.has(p.picture)) made_up.put(p.picture, blocks(seed++, 48, 32));
        made_up.put("login_page", blocks(seed++, 48, 32));

        Frame hud = make_frame(1024, 768);
        for (int y = 0; y < hud.height; ++y)
            for (int x = 0; x < hud.width; ++x) put_px(hud, x, y, 30, 40, 55);
        paint_hud(hud, 83, 8);
        const Frame nowhere = backdrop(240, 160);       // a screen the crossing does not know
        Frame at_connect = backdrop(240, 160);
        paste(at_connect, *made_up.get("game_ready_indicator"), 20, 20);
        Frame vtczone_play = backdrop(400, 300);
        paste(vtczone_play, *made_up.get("app_select_page"), 40, 40);
        paste(vtczone_play, *made_up.get("start_game_page"), 200, 200);

        struct Desk {
            Frame game_shows;
            bool  vtczone_up = false;
            Frame vtczone_shows;
            int   clicks = 0, focuses = 0, starts = 0, minimised = 0, ended = 0;
            std::function<void()> on_grab;      // the test's own doing, as the game is looked at
            std::function<void()> on_focus;     // ... and as a window is brought forward
        };
        const auto hands_for = [&](Desk& d) {
            AutoLogin::Hands h;
            h.find = [&d, game, vtczone](On w) -> void* {
                if (w == On::Game) return game;
                if (w == On::Publisher && d.vtczone_up) return vtczone;
                return nullptr;
            };
            h.grab = [&d](On w, void*, Frame& out) {
                if (w == On::Game) {
                    if (d.on_grab) d.on_grab();
                    out = d.game_shows;
                    return out.valid();
                }
                if (w == On::Publisher && d.vtczone_up) {
                    out = d.vtczone_shows;
                    return out.valid();
                }
                return false;
            };
            h.focus = [&d](void*) {
                ++d.focuses;
                if (d.on_focus) d.on_focus();
                return true;
            };
            h.click = [&d](int, int) { ++d.clicks; return true; };
            h.running = [](const std::string&) { return false; };
            h.start = [&d](const std::string&) { ++d.starts; return false; };
            h.minimise = [&d](void*) { ++d.minimised; };
            return h;
        };
        // The table, every page waited for one second, one attempt, and no
        // program of the real desktop named: a slip into VTCZone's half would
        // stop at once on a launcher that is not there.
        AutoLogin::Settings quick;
        for (auto& p : quick.pages) p.wait_s = 1;
        quick.attempts = 1;
        quick.launcher_path = temp_file("gobot_test_no_such_launcher.exe");
        quick.client_process = "gobot_test_no_such_launcher.exe";
        quick.game_process = "gobot_test_no_such_game.exe";
        const auto seconds_since = [](std::chrono::steady_clock::time_point t) {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
        };

        if (!clear) {
            std::printf("  [skip] the game is up and no corner of the screen is clear of it: the "
                        "pretend crossings' game window would cover it\n");
        } else if (game == nullptr || vtczone == nullptr) {
            std::printf("  [skip] could not make the pretend game's windows here\n");
        } else {
            {
                Desk d;
                d.game_shows = hud;
                AutoLogin flow;
                flow.set_settings(quick);
                flow.set_templates(&made_up);
                flow.set_hands(hands_for(d));
                const bool in = flow.run();
                check(in && flow.step() == AutoLogin::Step::InWorld && flow.attempts_made() == 1 &&
                          d.clicks == 0 && d.starts == 0 && d.minimised == 0,
                      "a game already in the world at the first look: done, with nothing clicked, "
                      "nothing started and VTCZone left alone");
            }
            {
                // The game up at Ket Noi, VTCZone's own picture showing Choi
                // game: nothing of VTCZone's is pressed.
                Desk d;
                d.game_shows = nowhere;
                d.vtczone_up = true;
                d.vtczone_shows = vtczone_play;
                AutoLogin flow;
                flow.set_settings(quick);
                flow.set_templates(&made_up);
                flow.set_hands(hands_for(d));
                const bool in = flow.run();
                check(!in && d.clicks == 0 && d.starts == 0,
                      "with the game up, Choi game in VTCZone's picture is never clicked");
                check(d.minimised == 1, "and VTCZone is put away once, as GoC does");
            }
            {
                // An offset that puts the point past the game's right edge.
                Desk d;
                d.game_shows = at_connect;
                AutoLogin::Settings s = quick;
                s.attempts = 3;
                for (auto& p : s.pages)
                    if (p.key == "connect") { p.click_dx = 400; p.click_dy = 5; }
                AutoLogin flow;
                flow.set_settings(s);
                flow.set_templates(&made_up);
                flow.set_hands(hands_for(d));
                const auto t0 = std::chrono::steady_clock::now();
                const bool in = flow.run();
                check(!in && flow.why() == Why::BadPoint && flow.why_detail() == "connect" &&
                          flow.attempts_made() == 1 && d.clicks == 0 && seconds_since(t0) < 10.0,
                      "a point off the game's client area stops the crossing on the first try, "
                      "at once, with nothing clicked");
                check(flow.advice() == AutoLogin::advice(Why::BadPoint, "connect") &&
                          flow.trouble().find("[Page.connect]") != std::string::npos,
                      "and says which [Page.*] of configs/login.ini to put right, not 'tried 3 "
                      "times'");
            }
            {
                // A stop asked for once the page is found, before its click.
                Desk d;
                d.game_shows = at_connect;
                AutoLogin flow;
                bool armed = false;
                int focused_then = -1;
                flow.on_step([&](AutoLogin::Step s, const std::string&) {
                    if (s == AutoLogin::Step::Connect) armed = true;
                });
                d.on_grab = [&] {
                    if (armed && focused_then < 0) {
                        flow.cancel();
                        focused_then = d.focuses;
                    }
                };
                flow.set_settings(quick);
                flow.set_templates(&made_up);
                flow.set_hands(hands_for(d));
                const bool in = flow.run();
                check(!in && flow.why() == Why::Cancelled && d.clicks == 0 &&
                          focused_then >= 0 && d.focuses == focused_then,
                      "a stop asked for after the page was found: no click, and the window is "
                      "not even brought forward for one");
            }
            if (!on_top) {
                std::printf("  [skip] the pretend game could not be put on top here\n");
            } else {
                // A stop asked for while the window is being brought forward
                // for the click: the last look before the click sees it.
                Desk d;
                d.game_shows = at_connect;
                AutoLogin flow;
                bool armed = false;
                flow.on_step([&](AutoLogin::Step s, const std::string&) {
                    if (s == AutoLogin::Step::Connect) armed = true;
                });
                d.on_focus = [&] { if (armed) flow.cancel(); };
                flow.set_settings(quick);
                flow.set_templates(&made_up);
                flow.set_hands(hands_for(d));
                const bool in = flow.run();
                check(!in && flow.why() == Why::Cancelled && d.clicks == 0,
                      "a stop asked for while the window is brought forward for the click: no "
                      "click");
            }
            {
                // Two attempts, each ending on a game that sits on a screen
                // the crossing does not know.
                Desk d;
                d.game_shows = nowhere;
                AutoLogin::Settings s = quick;
                s.attempts = 2;
                AutoLogin flow;
                flow.set_settings(s);
                flow.set_templates(&made_up);
                flow.set_hands(hands_for(d));
                flow.set_end_client([&d] { ++d.ended; return true; }, 0);
                const bool in = flow.run();
                check(!in && flow.attempts_made() == 2 && d.ended == 1 && d.clicks == 0 &&
                          d.starts == 0,
                      "a game stuck on a screen the crossing does not know is ended once, between "
                      "the attempts -- and not after the last, which is left for a person");
                check(flow.why() == Why::Connect, "the attempt itself fails as it always did");
            }
            {
                Desk d;
                d.game_shows = nowhere;
                AutoLogin flow;
                flow.set_settings(quick);
                flow.set_templates(&made_up);
                flow.set_hands(hands_for(d));
                const bool in = flow.run();
                check(!in && flow.why() == Why::Connect && d.clicks == 0 && d.starts == 0,
                      "with nothing to end it with, that attempt just fails, and nothing is ended");
            }
            {
                // The HUD covered at the first look, and seen when the
                // crossing looks at the game again after the wait ran out.
                Desk d;
                d.game_shows = nowhere;
                d.on_focus = [&] { if (d.focuses == 2) d.game_shows = hud; };
                AutoLogin::Settings s = quick;
                s.attempts = 2;
                AutoLogin flow;
                flow.set_settings(s);
                flow.set_templates(&made_up);
                flow.set_hands(hands_for(d));
                int ended = 0;
                flow.set_end_client([&ended] { ++ended; return true; }, 0);
                const bool in = flow.run();
                check(in && flow.step() == AutoLogin::Step::InWorld && ended == 0 && d.clicks == 0,
                      "a game found in the world after its page wait ran out is done, not ended");
            }

            // A real frame of the world with the bag open, on which the HP and
            // MP bars alone were missed: the MP bar was taken from the panel
            // beside it. The HUD's "Level" plate is where it always is.
            Frame real, landmark;
            const bool have_real =
                load_png(exe_relative("trash_debug\\20261009_003950_0_no_dialog.png"), real) &&
                real.valid() &&
                TemplateStore::load_one("hpmp_indicator2", exe_relative("configs/templates.ini"),
                                        landmark);
            if (!have_real) {
                std::printf("  [skip] no saved frame of the world, or no HUD landmark, here\n");
            } else {
                check(AutoLogin::in_world(real, &landmark),
                      std::string("a real frame of the world with the bag open is in the world by "
                                  "the HUD's landmark (the bars alone: ") +
                          (SessionManager::hud_present(real) ? "found" : "missed") + ")");
                check(!AutoLogin::in_world(nowhere, &landmark) &&
                          !AutoLogin::in_world(at_connect, &landmark) &&
                          AutoLogin::in_world(real, nullptr) == SessionManager::hud_present(real),
                      "a screen that is not the world is not, and without the landmark only the "
                      "bars decide");
                // The same plate somewhere else on the screen is not the HUD.
                Frame moved = make_frame(real.width, real.height);
                for (int y = 0; y < moved.height; ++y)
                    for (int x = 0; x < moved.width; ++x) put_px(moved, x, y, 30, 40, 55);
                for (int y = 0; y < landmark.height; ++y)
                    for (int x = 0; x < landmark.width; ++x) {
                        std::uint8_t r, g, b;
                        real.get(44 + x, 4 + y, r, g, b);
                        put_px(moved, 300 + x, 200 + y, r, g, b);
                        put_px(moved, 44 + 6 + x, 4 + 6 + y, r, g, b);
                    }
                check(!AutoLogin::in_world(moved, &landmark),
                      "nor is the plate anywhere but where the HUD puts it, 6 pixels off included");
                Frame placed = moved;
                for (int y = 0; y < landmark.height; ++y)
                    for (int x = 0; x < landmark.width; ++x) {
                        std::uint8_t r, g, b;
                        real.get(44 + x, 4 + y, r, g, b);
                        put_px(placed, 44 + 3 + x, 4 - 2 + y, r, g, b);
                    }
                check(AutoLogin::in_world(placed, &landmark),
                      "while a few pixels off where it sits still is");

                {
                    Desk d;
                    d.game_shows = real;
                    AutoLogin::Settings s = quick;
                    s.attempts = 3;
                    AutoLogin flow;
                    flow.set_settings(s);
                    flow.set_templates(&made_up);
                    flow.set_landmark(landmark);
                    flow.set_hands(hands_for(d));
                    int ended = 0;
                    flow.set_end_client([&ended] { ++ended; return true; }, 0);
                    const bool in = flow.run();
                    check(in && flow.step() == AutoLogin::Step::InWorld &&
                              flow.attempts_made() == 1 && ended == 0 && d.clicks == 0 &&
                              d.starts == 0,
                          "a crossing that finds that frame is in the world at once: nothing "
                          "clicked, and nothing ended");
                }
                {
                    // Not seen at the first look, and seen when the crossing
                    // judges the game after its page wait: in the world, not
                    // a client to end.
                    Desk d;
                    d.game_shows = nowhere;
                    d.on_focus = [&] { if (d.focuses == 2) d.game_shows = real; };
                    AutoLogin::Settings s = quick;
                    s.attempts = 3;
                    AutoLogin flow;
                    flow.set_settings(s);
                    flow.set_templates(&made_up);
                    flow.set_landmark(landmark);
                    flow.set_hands(hands_for(d));
                    int ended = 0;
                    flow.set_end_client([&ended] { ++ended; return true; }, 0);
                    const bool in = flow.run();
                    check(in && flow.step() == AutoLogin::Step::InWorld && ended == 0 &&
                              d.clicks == 0,
                          "and judged after a page wait ran out, that frame is the world too: the "
                          "end hook is called 0 times");
                }
            }
            {
                // Judged stuck only on two looks it can trust, and none of
                // them the world: one look in three showing the HUD is enough
                // to leave the game alone.
                Desk d;
                d.game_shows = nowhere;
                int grabs_after = 0;
                bool judging = false;
                d.on_focus = [&] { if (d.focuses == 2) judging = true; };
                d.on_grab = [&] {
                    if (!judging) return;
                    ++grabs_after;
                    d.game_shows = grabs_after == 3 ? hud : nowhere;
                };
                AutoLogin::Settings s = quick;
                s.attempts = 2;
                AutoLogin flow;
                flow.set_settings(s);
                flow.set_templates(&made_up);
                flow.set_hands(hands_for(d));
                int ended = 0;
                flow.set_end_client([&ended] { ++ended; return true; }, 0);
                const bool in = flow.run();
                check(in && ended == 0 && grabs_after == 3,
                      "a game that shows its HUD on the third of three looks is in the world, "
                      "and not ended");
            }
        }
        if (vtczone) DestroyWindow(vtczone);
        if (game) DestroyWindow(game);
    }
}

void test_party_invite() {
    section("Party invitations");

    // Folding: lowercase, accents off, spaces gone.
    //
    // Needed in two places at once. configs/party.ini has to be editable by
    // somebody who cannot type ủ, and the bitmap reader gets a mark wrong far
    // more often than it gets the letter wrong.
    {
        check(PartyInvite::fold("c\xE1\xBB\xA7""a") == "cua", "của folds to cua");
        check(PartyInvite::fold("Y\xC3\xAAu c\xE1\xBA\xA7u") == "yeucau",
              "a whole phrase folds, spaces and all");
        check(PartyInvite::fold("ExampleWin") == "examplewin", "plain text lowercases");
        check(PartyInvite::fold("") .empty(), "nothing in, nothing out");
        // Malformed input is skipped rather than guessed at.
        check(PartyInvite::fold("\xFF\xFE") .empty(), "rubbish bytes are dropped");
    }

    // Pulling the name off the dialog.
    //
    // The line below is what the reader actually produced from a live
    // invitation: "ExampleWin" came back as "Exampley in", because the W is
    // drawn as two strokes and the reader saw two characters with a gap. The
    // first version of this took the first word, which gave "Exampley" and
    // threw the rest away.
    {
        const std::string real =
            "Y\xC3\xAAu c\xE1\xBA\xA7u c\xE1\xBB\xA7""a Exampley in";
        check(PartyInvite::name_in(real, "cua") == "Exampleyin",
              "the live misread joins back into one name");
        check(PartyChat::alike(PartyInvite::fold("Exampleyin"),
                               PartyInvite::fold("ExampleWin")) >= 0.7,
              "and is close enough to the real name to match on a list");

        check(PartyInvite::name_in(
                  "Y\xC3\xAAu c\xE1\xBA\xA7u c\xE1\xBB\xA7""a Someone.", "cua")
                  == "Someone",
              "a clean read, full stop removed");

        // A modal that is not an invitation has no business being accepted.
        check(PartyInvite::name_in("Connect Error", "cua").empty(),
              "a line without the prefix yields no name");
        check(PartyInvite::name_in("", "cua").empty(), "nor does an empty line");
    }

    // The settings survive the file. Offsets that came back wrong would click
    // the wrong button, and one of the two buttons declines.
    {
        PartyInvite::Settings s;
        s.accept_dx = 31; s.accept_dy = 23;
        s.decline_dx = 73; s.decline_dy = 23;
        s.name_dx = -82; s.name_dy = -26;
        s.score = 0.8f;

        IniFile ini;
        s.save(ini);
        PartyInvite::Settings back;
        back.load(ini);
        check(back.accept_dx == 31 && back.accept_dy == 23 &&
              back.decline_dx == 73 && back.decline_dy == 23,
              "both buttons survive a config round trip");
        check(back.name_dx == -82 && back.name_dy == -26,
              "and the negative offsets to the text above them");
        check(back.accept_dx != back.decline_dx,
              "accept and decline are not the same point");
    }

    // A name read off a screen is matched against the lists loosely, because
    // an exact match is a match that never happens.
    {
        Party::Settings rules;
        rules.use_blacklist = true;
        rules.blacklist = { "ExampleWin" };
        check(!rules.may_invite("Exampleyin"),
              "a misread name is still recognised on the blacklist");
        check(rules.may_invite("SomebodyElse"),
              "and somebody else is not");
    }
}

/// The "no real input" switch, which main() turns on before the first test.
///
/// Each kind of reaching the real desktop is tried through the path GoBot
/// itself takes, and each has to refuse, say so once, and leave nothing done.
/// Every one is aimed where nothing could happen even with the guard broken --
/// at a window of this test's own that has gone, at a hidden one that is
/// never in front, at a key no program listens for, at a program that is not
/// there, at a process number nobody has -- so that a broken guard shows as a
/// failed check, never as a key in somebody's editor.
void test_no_real_input() {
    section("No real input: every way of reaching the desktop is refused");

    check(real_input_forbidden(), "the switch is on before the first test runs");

    // How many times the guard refused during one call.
    const auto refusals_in = [](const std::function<void()>& call) {
        const std::uint64_t before = real_input_refusals();
        call();
        return real_input_refusals() - before;
    };

    // A window of this test's own, never shown, so never in front: the focus
    // gate behind the guard would stop a key or a character as well.
    const HINSTANCE self = GetModuleHandleA(nullptr);
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = self;
    wc.lpszClassName = "GoBotTestNoRealInput";
    RegisterClassA(&wc);        // already there from an earlier run: the same
    HWND hidden = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, "",
                                  WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, self, nullptr);
    // Whatever reached it -- a key, a character, a command, a request to
    // close -- taken off its queue and counted.
    const auto reached_it = [&] {
        int n = 0;
        MSG m{};
        while (hidden != nullptr && PeekMessageA(&m, hidden, 0, 0, PM_REMOVE))
            if (m.message == WM_KEYDOWN || m.message == WM_KEYUP || m.message == WM_CHAR ||
                m.message == WM_IME_CHAR || m.message == WM_COMMAND || m.message == WM_CLOSE)
                ++n;
        return n;
    };
    reached_it();       // anything Windows put there itself, before counting

    // ---- a key ----
    {
        // Unassigned: no program anywhere listens for it.
        constexpr std::uint32_t kNobodysKey = 0x97;
        InputManager keys;
        keys.set_target_window(hidden);
        bool pressed = true, held = true, typed = true, unicode = true;
        check(refusals_in([&] { pressed = keys.key_press(kNobodysKey, 1); }) == 1 && !pressed,
              "a key press is refused, and said once");
        check(refusals_in([&] { held = keys.key_down(kNobodysKey); }) == 1 && !held,
              "so is holding a key down");
        check(refusals_in([&] { typed = keys.type_key(kNobodysKey, true, 1); }) == 1 && !typed &&
                  refusals_in([&] { unicode = keys.type_unit(L'x'); }) == 1 && !unicode,
              "and typing one, with Shift or as Unicode");
        check(hidden != nullptr && !keys.is_held(kNobodysKey) &&
                  (GetAsyncKeyState(static_cast<int>(kNobodysKey)) & 0x8000) == 0 &&
                  reached_it() == 0,
              "and nothing is held, by GoBot or by Windows, nor reached the window aimed at");
    }

    // A window of this test's own that has gone: aimed at, there is nowhere
    // for a click, the pointer or the focus to go. Its handle is not taken
    // again in the moments this runs -- Windows hands a reused slot a new one.
    HWND gone = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, "",
                                WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, self, nullptr);
    if (gone != nullptr) DestroyWindow(gone);

    // ---- a click, and the pointer ----
    if (gone == nullptr || IsWindow(gone)) {
        std::printf("  [skip] could not make a window and let it go here\n");
    } else {
        // Where the pointer is, before and after: read, never set.
        POINT before{}, after{};
        const bool read_before = GetCursorPos(&before) != FALSE;
        InputManager pointer;
        pointer.set_target_window(gone);
        bool clicked = true, right = true, dragged = true, moved = true;
        check(refusals_in([&] { clicked = pointer.click_at(5, 5); }) == 1 && !clicked &&
                  refusals_in([&] { right = pointer.click_at(5, 5, true); }) == 1 && !right,
              "a click is refused, left or right, and said once");
        check(refusals_in([&] { dragged = pointer.drag(5, 5, 40, 40); }) == 1 && !dragged,
              "so is a drag");
        check(refusals_in([&] { moved = pointer.move_pointer(5, 5); }) == 1 && !moved,
              "and the pointer is not moved");
        const bool read = read_before && GetCursorPos(&after) != FALSE;
        if (!read)
            std::printf("  [skip] Windows would not say where the pointer is, so it was not "
                        "compared\n");
        else if (after.x != before.x || after.y != before.y)
            // Each call above was refused before Windows was asked, and aimed
            // at a window that is gone: something else moved it -- a hand on
            // the mouse, most likely -- which is not this test's to judge.
            std::printf("  [skip] the pointer moved while these ran, by a hand on the mouse most "
                        "likely, so where it is was not compared\n");
        else
            check(after.x == before.x && after.y == before.y,
                  "and Windows reads the pointer exactly where it was before them");
    }

    // ---- focus ----
    if (gone != nullptr && !IsWindow(gone)) {
        InputManager front;
        front.set_target_window(gone);
        bool brought = true;
        check(refusals_in([&] { brought = front.focus_window(); }) == 1 && !brought,
              "bringing a window to the front is refused, and said once");
    }

    // With no window at all nothing is aimed at, as before the switch, so
    // there is nothing to refuse: a test whose pretend game has no window does
    // not fill the log with refusals that were never a reach for the desktop.
    {
        InputManager none;
        bool any = false;
        const auto n = refusals_in([&] {
            any = none.key_press(0x97, 1) || none.click_at(5, 5) || none.move_pointer(5, 5) ||
                  none.drag(5, 5, 40, 40) || none.focus_window() || none.post_char(L'x', false);
        });
        check(!any && n == 0, "aimed at no window at all, nothing happens and nothing is refused");
    }

    // ---- a message to another program's window ----
    {
        InputManager chat;
        chat.set_target_window(hidden);
        bool posted = true, ime = true;
        check(refusals_in([&] { posted = chat.post_char(L'x', false); }) == 1 && !posted &&
                  refusals_in([&] { ime = chat.post_char(L'x', true); }) == 1 && !ime,
              "a character posted to a window is refused, as WM_CHAR and as WM_IME_CHAR");

        // The keeper's shut-down, with a box to dismiss that is that hidden
        // window, and programs to close and end that nobody has.
        CrashWatch::Settings s;
        s.game_process = "gobot_test_no_such_game.exe";
        s.launcher_process = "gobot_test_no_such_launcher.exe";
        s.reporter_process = {};
        CrashWatch keeper(s);
        GameInstall nowhere;
        nowhere.programs = { temp_file("gobot_test_no_such_game.exe") };
        keeper.set_install(nowhere);
        CrashWatch::Look box;
        box.what = CrashWatch::Trouble::ErrorBox;
        box.dialog = hidden;
        bool shut = true;
        const auto t0 = std::chrono::steady_clock::now();
        const auto n = refusals_in([&] { shut = keeper.shut_down(box); });
        const double took =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        check(!shut && n == 1 && took < 1.0,
              "the keeper's shut-down is refused at once, said once, before it dismisses, "
              "closes, ends or waits on anything");
        check(reached_it() == 0 && IsWindow(hidden),
              "and nothing was posted to the box it would have dismissed");
    }

    // ---- a program started ----
    {
        bool started = true;
        check(refusals_in([&] {
                  started = SessionManager::start_program(
                      temp_file("gobot_test_no_such_program.exe"));
              }) == 1 &&
                  !started && !SessionManager::process_running("gobot_test_no_such_program.exe"),
              "starting a program is refused, said once, and nothing runs under its name");
    }

    // ---- a program ended ----
    {
        // A process number nobody has: above any Windows hands out, and asked
        // after to be sure -- without the right to do anything to it.
        const unsigned long nobody = 0xFFFFFFF0ul;
        const HANDLE there = OpenProcess(SYNCHRONIZE, FALSE, nobody);
        if (there != nullptr) CloseHandle(there);
        GameInstall nowhere;
        nowhere.programs = { temp_file("gobot_test_no_such_game.exe") };
        if (there != nullptr) {
            std::printf("  [skip] a process numbered 0x%lX is running here\n", nobody);
        } else {
            bool ended = true;
            check(refusals_in([&] { ended = end_game_program(nobody, nowhere); }) == 1 && !ended,
                  "ending a program is refused, and said once");
        }
        int how_many = -1;
        check(refusals_in([&] { how_many = end_game("gobot_test_no_such_game.exe", nowhere); }) ==
                      1 &&
                  how_many == 0,
              "so is ending the game, by its name and where it is installed");
        how_many = -1;
        check(refusals_in([&] {
                  how_many = end_beside_the_game("gobot_test_no_such_launcher.exe", nowhere);
              }) == 1 &&
                  how_many == 0,
              "and ending the game's launcher, by its name and the game's folder");
    }

    // ---- a launcher's path, read off a running program to adopt it ----
    {
        // Asked after a program nothing runs, as the starting and ending
        // above are: with the switch broken, there would still be no real
        // launcher's path read, nor kept for the adopting below.
        std::string seen = "x";
        check(refusals_in([&] {
                  seen = AutoLogin::running_launcher(true, "gobot_test_no_such_launcher.exe");
              }) == 1 &&
                  seen.empty(),
              "reading a running launcher's path is refused, said once, and nothing is read");
        AutoLogin::Settings s;
        s.launcher_path = temp_file("gobot_test_no_vtczone_here.exe");
        bool adopted = true;
        // Without a fresh look, so that the refusal is the adopting's own.
        check(refusals_in([&] { adopted = s.adopt_running_launcher(false); }) == 1 && !adopted &&
                  s.launcher_path == temp_file("gobot_test_no_vtczone_here.exe"),
              "and so is adopting one: the path in the file stays as it was");
    }

    if (hidden) DestroyWindow(hidden);
    check(real_input_forbidden(), "and the switch is still on: nothing turned it off");
}

/// Make the folder `link` a junction to the folder `target`, as mklink /J
/// does -- which needs no rights beyond the folder's own. Both full paths.
/// False when this disk will not have one.
bool make_junction(const std::wstring& link, const std::wstring& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(link.c_str());
        return false;
    }
    // A mount point's reparse data: the tag, the length of what follows the
    // first eight bytes, then where in the path buffer the kernel's form of
    // the target and its printable form each are, then the two, each ended.
    const std::wstring kernel = L"\\??\\" + target;
    const auto sub = static_cast<USHORT>(kernel.size() * sizeof(wchar_t));
    const auto print = static_cast<USHORT>(target.size() * sizeof(wchar_t));
    const auto paths = static_cast<USHORT>(sub + sizeof(wchar_t) + print + sizeof(wchar_t));
    std::vector<unsigned char> data(16 + paths, 0);
    const auto put = [&data](std::size_t at, const void* from, std::size_t n) {
        std::memcpy(data.data() + at, from, n);
    };
    const ULONG tag = IO_REPARSE_TAG_MOUNT_POINT;
    const USHORT after_header = static_cast<USHORT>(8 + paths);
    const USHORT print_at = static_cast<USHORT>(sub + sizeof(wchar_t));
    const USHORT zero = 0;
    put(0, &tag, 4);
    put(4, &after_header, 2);
    put(8, &zero, 2);
    put(10, &sub, 2);
    put(12, &print_at, 2);
    put(14, &print, 2);
    put(16, kernel.c_str(), sub);
    put(16 + print_at, target.c_str(), print);
    DWORD got = 0;
    const BOOL made = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, data.data(),
                                      static_cast<DWORD>(data.size()), nullptr, 0, &got, nullptr);
    CloseHandle(h);
    if (!made) RemoveDirectoryW(link.c_str());
    return made != FALSE;
}

/// The one rule for the game: by where a program runs from, never by its
/// name alone. Pretend lists of what is running and a pretend disk, so that
/// nothing here depends on the game being installed, or running, here.
void test_game_install() {
    section("The game, told by where it runs from");

    GameInstall install;
    install.programs = { "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe" };
    // Two drives, and the 8.3 names this pretend disk gives two folders.
    GameInstall::Drives drives;
    drives.devices = { { "C:", "\\Device\\HarddiskVolume3" },
                       { "D:", "\\Device\\HarddiskVolume7" } };
    drives.long_name = [](const std::string& path) -> std::string {
        std::string up = path;
        for (char& c : up)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (up == "C:\\VTCZONE\\GAMES\\GHOST_~1\\GAME.EXE")
            return "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe";
        if (up == "C:\\PROGRA~1\\SOMEGA~1\\GAME.EXE") return "C:\\Program Files\\Some Game\\Game.exe";
        return {};
    };

    const std::vector<RunningProgram> running = {
        { 1001, "Game.exe", "C:\\Program Files\\Some Game\\Game.exe" },
        { 1002, "Game.exe", "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe" },
        { 1003, "Game.exe", "" },
        { 1004, "GAME.EXE", "c:\\vtczone\\games\\ghost_online_pc_vtc\\GAME.exe" },
        { 1005, "Game.exe", "\\\\?\\C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe" },
        { 1006, "Game.exe", "\\Device\\HarddiskVolume3\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe" },
        { 1007, "Game.exe", "\\Device\\HarddiskVolume7\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe" },
        { 1008, "Game.exe", "C:/VTCZone/Games/Ghost_Online_PC_VTC/Game.exe" },
        { 1009, "GAME.EXE", "C:\\VTCZONE\\GAMES\\GHOST_~1\\GAME.EXE" },
        { 1010, "GAME.EXE", "C:\\PROGRA~1\\SOMEGA~1\\GAME.EXE" },
        { 1011, "Game.exe", "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\bin\\Game.exe" },
        { 1012, "Game.exe", "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC_old\\Game.exe" },
        { 1013, "Game.exe", "\\Device\\HarddiskVolume30\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe" },
        { 1014, "Game.exe", "\\\\server\\share\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe" },
        { 1015, "Game.exe", "C:\\VTCZone\\Games\\Other\\..\\Ghost_Online_PC_VTC\\.\\Game.exe" },
        { 1016, "Game.exe", "\\??\\C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe" },
        { 1017, "Game.exe",
          "\\\\?\\GLOBALROOT\\Device\\HarddiskVolume3\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe" },
        { 1018, "Game.exe", "C:\\VTCZone\\Games\\\\Ghost_Online_PC_VTC\\Game.exe" },
        { 1019, "GhostOnlineVTC.exe", "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\GhostOnlineVTC.exe" },
    };
    const auto is = [&](unsigned long pid) {
        for (const RunningProgram& p : running)
            if (p.pid == pid) return install.is_game(p, drives);
        return false;
    };
    check(!is(1001), "a Game.exe outside the game's folder is not the game");
    check(is(1002), "the game's own Game.exe is");
    check(!is(1003), "one whose path cannot be read is not the game");
    check(is(1004), "whatever the case of its path");
    check(is(1005) && is(1016) && is(1017),
          "with Windows' \\\\?\\ in front of its path, or the kernel's \\??\\ or \\\\?\\GLOBALROOT");
    check(is(1006), "in the kernel's \\Device\\ form, on the drive the game is installed on");
    check(!is(1007) && !is(1013),
          "but not in the same folders on another drive, nor on a device whose name only starts "
          "the same");
    check(is(1008), "with forward slashes");
    check(is(1015) && is(1018), "with '.', '..' or a doubled separator in its path");
    check(is(1009), "by its 8.3 short name");
    check(!is(1010), "while another Game.exe's short name is still another Game.exe");
    check(!is(1011) && !is(1012),
          "a Game.exe in a folder inside the game's is not it, nor one in a folder whose name "
          "starts the same");
    check(!is(1014), "nor one on a share with the same folders");
    check(!is(1019), "and the game's launcher beside it is not the game");

    std::vector<unsigned long> taken;
    for (const RunningProgram& p : install.games_among(running, drives)) taken.push_back(p.pid);
    check(taken == std::vector<unsigned long>{ 1002, 1004, 1005, 1006, 1008, 1009, 1015, 1016,
                                               1017, 1018 },
          "of everything running, only the game's own are taken -- what a keeper would end");

    GameInstall unknown;
    check(unknown.games_among(running, drives).empty() && unknown.names_the_game("Game.exe"),
          "with nowhere known for the game, nothing is the game -- and a program asked for as "
          "Game.exe still has to be, so none is taken for it");

    check(install.names_the_game("game.exe") && install.names_the_game(" Game.exe ") &&
              !install.names_the_game("GhostOnlineVTC.exe") && !install.names_the_game(""),
          "a program asked for as Game.exe, whatever the case, has to be the game; others need not");
    GameInstall renamed;
    renamed.programs = { "D:\\Games\\Ghost\\GhostClient.exe" };
    check(renamed.names_the_game("ghostclient.exe") &&
              renamed.is_game("D:\\Games\\Ghost\\GhostClient.exe", drives),
          "and a game installed under another name is asked for by that name too");

    check(GameInstall::program_in_icon("C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe") ==
                  "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe" &&
              GameInstall::program_in_icon("\"C:\\Games\\Ghost Online\\Game.exe\",0") ==
                  "C:\\Games\\Ghost Online\\Game.exe" &&
              GameInstall::program_in_icon(" C:\\Games\\Ghost\\Game.exe,0 ") ==
                  "C:\\Games\\Ghost\\Game.exe",
          "the installer's record is read whether its path is quoted or not, with or without "
          "which icon");
    check(GameInstall::program_in_icon("C:\\Games\\Ghost\\ghost.ico") ==
                  "C:\\Games\\Ghost\\Game.exe" &&
              GameInstall::program_in_icon("C:\\Games, old\\Game.exe") ==
                  "C:\\Games, old\\Game.exe" &&
              GameInstall::program_in_icon("").empty(),
          "an icon file names the Game.exe beside it, a comma in a folder's name stays, and an "
          "empty record names nothing");

    {
        GameInstall vn;
        vn.programs = { "D:\\Trò chơi\\Ghost Online\\Game.exe" };
        check(vn.is_game("d:\\TRÒ CHƠI\\ghost online\\GAME.EXE", drives),
              "a folder named in Vietnamese, whatever the case of its letters");
    }

    // A game installed through a junction -- C:\VTCZone moved to D:\ and a
    // junction left behind -- or on a subst drive: its record names the path
    // it was installed by, and Windows gives the running program's path as
    // where the file really is. The two are told to be one file by what is on
    // disk, here a pretend disk whose files say which they are.
    {
        GameInstall moved;
        moved.programs = { "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe" };
        GameInstall::Drives disk = drives;
        int asked = 0;
        disk.file_id = [&asked](const std::string& path) -> std::string {
            ++asked;
            std::string up = path;
            for (char& c : up)
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            if (up == "C:\\VTCZONE\\GAMES\\GHOST_ONLINE_PC_VTC\\GAME.EXE" ||
                up == "D:\\VTCZONE\\GAMES\\GHOST_ONLINE_PC_VTC\\GAME.EXE" ||
                up == "D:\\ELSEWHERE\\RENAMED.EXE")
                return "7:100";
            if (up == "C:\\VTCZONE\\GAMES\\GHOST_ONLINE_PC_VTC" ||
                up == "D:\\VTCZONE\\GAMES\\GHOST_ONLINE_PC_VTC")
                return "7:99";
            if (up == "E:\\COPY\\GAME.EXE") return "9:100";
            if (up == "E:\\COPY") return "9:99";
            return {};
        };
        check(moved.is_game("D:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe", disk),
              "a game reached through a junction, running from where it leads, is the game");
        check(!moved.is_game("E:\\Copy\\Game.exe", disk) && !moved.is_game("F:\\Gone\\Game.exe", disk),
              "while another file of its name is not, nor is one that cannot be opened");
        asked = 0;
        const bool other_name = moved.is_game("D:\\Elsewhere\\Renamed.exe", disk);
        check(!other_name && asked == 0,
              "and a program of another name is never even asked of the disk: every window on the "
              "screen is held up against the game this way");
        check(!install.is_game("D:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe", drives),
              "with no way to ask the disk, the paths' text decides, as before");

        check(moved.beside_the_game("C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\GhostOnlineVTC.exe",
                                    disk) &&
                  moved.beside_the_game(
                      "d:\\vtczone\\games\\ghost_online_pc_vtc\\GhostOnlineVTC.exe", disk),
              "the game's launcher sits beside it -- in its folder, or there through the junction");
        check(!moved.beside_the_game("C:\\Users\\Someone\\Downloads\\GhostOnlineVTC.exe", disk) &&
                  !moved.beside_the_game("E:\\Copy\\GhostOnlineVTC.exe", disk) &&
                  !moved.beside_the_game(
                      "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\bin\\GhostOnlineVTC.exe", disk) &&
                  !moved.beside_the_game("", disk) &&
                  !GameInstall{}.beside_the_game(
                      "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\GhostOnlineVTC.exe", disk),
              "and one of its name anywhere else is not the game's launcher: another folder, a copy "
              "of the game, a folder inside the game's, no path, or no game known");
    }

    // Where the game is installed, out of what this machine says: the
    // installer's records, the usual place, the programs seen behind the
    // game's window -- with a pretend disk saying which files are there.
    {
        const std::string usual = "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe";
        const std::string elsewhere = "D:\\Games\\Ghost_Online_PC_VTC\\Game.exe";
        const auto on_disk = [](const std::vector<std::string>& there) {
            return [there](const std::string& path) {
                return std::find(there.begin(), there.end(), path) != there.end();
            };
        };
        check(GameInstall::found({ elsewhere, "" }, {}, on_disk({ usual }), drives).programs ==
                  std::vector<std::string>{ usual },
              "a record naming a folder since deleted or moved is dropped, and the game in the "
              "usual place is known all the same");
        check(GameInstall::found({ elsewhere }, {}, on_disk({ elsewhere, usual }), drives)
                      .programs == std::vector<std::string>{ elsewhere, usual },
              "a record that is there is kept, and the usual place beside it");
        check(GameInstall::found({ usual }, {}, on_disk({ usual }), drives).programs ==
                  std::vector<std::string>{ usual },
              "the usual place, recorded, is one place and not two");
        check(GameInstall::found({ elsewhere }, {}, on_disk({}), drives).programs.empty() &&
                  GameInstall::found({}, { "E:\\Seen\\Game.exe" }, on_disk({}), drives).programs ==
                      std::vector<std::string>{ "E:\\Seen\\Game.exe" },
              "with nothing there, nowhere -- but a program seen behind the game's window needs no "
              "asking");
    }

    // This machine's own drives and short names, on this test's own program.
    // Nothing is printed about where that is: a folder with somebody's name
    // in it.
    {
        const GameInstall::Drives here = GameInstall::Drives::here();
        wchar_t self[MAX_PATH]{};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        const std::string me = wide_to_utf8(self);
        GameInstall mine;
        mine.programs = { me };
        check(mine.is_game(me, here) && mine.is_game("\\\\?\\" + me, here),
              "on this machine: this test's own program is itself, in the \\\\?\\ form too");
        wchar_t native[1024]{};
        DWORD size = 1024;
        if (QueryFullProcessImageNameW(GetCurrentProcess(), PROCESS_NAME_NATIVE, native, &size))
            check(mine.is_game(wide_to_utf8(std::wstring(native, size)), here),
                  "and as the kernel names it, \\Device\\ and all, read off this machine's drives");
        else
            std::printf("  [skip] Windows would not give this program's path in the kernel's form\n");
        wchar_t shorter[MAX_PATH]{};
        const DWORD n = GetShortPathNameW(self, shorter, MAX_PATH);
        if (n > 0 && n < MAX_PATH && std::wstring(shorter) != std::wstring(self))
            check(mine.is_game(wide_to_utf8(shorter), here),
                  "and by the short name this disk gives it");
        else
            std::printf("  [skip] this disk gives this test's folder no short names\n");
        check(!mine.is_game(temp_file("gobot_test.exe"), here),
              "while a program of the same name in another folder is not it");

        // And off the list of what is really running.
        bool listed = false;
        for (const RunningProgram& p : programs_called("GOBOT_TEST.EXE"))
            if (p.pid == GetCurrentProcessId()) listed = !p.path.empty();
        GameInstall away;
        away.programs = { temp_file("gobot_test.exe") };
        check(listed && game_running("gobot_test.exe", mine) &&
                  !game_running("gobot_test.exe", away),
              "this test is among the programs running under its name, path and all -- and is "
              "the game only where the game is installed");
    }

    // And on this machine's own disk, in this run's scratch folder: a
    // junction and a hard link each lead to one file by another path, as a
    // game installed through a junction does.
    {
        const GameInstall::Drives here = GameInstall::Drives::here();
        const std::string real = temp_file("gi_real");
        const std::string copy = temp_file("gi_copy");
        const std::string hard = temp_file("gi_hard");
        const std::string link = temp_file("gi_link");
        const auto put_file = [](const std::string& path, const char* text) {
            std::ofstream f(std::filesystem::path(utf8_to_wide(path)), std::ios::binary);
            f << text;
            return static_cast<bool>(f);
        };
        const bool made = CreateDirectoryW(utf8_to_wide(real).c_str(), nullptr) &&
                          CreateDirectoryW(utf8_to_wide(copy).c_str(), nullptr) &&
                          CreateDirectoryW(utf8_to_wide(hard).c_str(), nullptr) &&
                          put_file(real + "\\Game.exe", "the game") &&
                          put_file(copy + "\\Game.exe", "another program");
        const bool joined =
            made && make_junction(utf8_to_wide(link), utf8_to_wide(real));
        const bool linked =
            made && CreateHardLinkW(utf8_to_wide(hard + "\\Game.exe").c_str(),
                                    utf8_to_wide(real + "\\Game.exe").c_str(), nullptr) != FALSE;
        if (made)
            check(here.file_id &&
                      !here.file_id(GameInstall::plain(real + "\\Game.exe", here)).empty() &&
                      here.file_id(GameInstall::plain(copy + "\\Game.exe", here)) !=
                          here.file_id(GameInstall::plain(real + "\\Game.exe", here)) &&
                      here.file_id("\\\\gobot-test-no-such-server\\share\\Game.exe").empty() &&
                      here.file_id("\\Device\\Mup\\gobot-test-no-such-server\\share\\Game.exe")
                          .empty(),
                  "a file on this disk is known by its number there, two files by two numbers -- "
                  "and nothing across the network is ever asked");
        if (!joined) {
            std::printf("  [skip] this disk would not take a junction here\n");
        } else {
            GameInstall through;
            through.programs = { link + "\\Game.exe" };
            check(through.is_game(real + "\\Game.exe", here),
                  "on this disk: a record naming the junction's path, and the game running from "
                  "where it leads, are one program");
            check(!through.is_game(copy + "\\Game.exe", here),
                  "while a file of the same name in another folder is another program");
            check(through.beside_the_game(real + "\\GhostOnlineVTC.exe", here) &&
                      !through.beside_the_game(copy + "\\GhostOnlineVTC.exe", here),
                  "and a launcher in the folder the junction leads to is beside the game, one in "
                  "another folder is not");
        }
        if (!linked) {
            std::printf("  [skip] this disk would not take a hard link here\n");
        } else {
            GameInstall at;
            at.programs = { real + "\\Game.exe" };
            check(at.is_game(hard + "\\Game.exe", here) && !at.is_game(copy + "\\Game.exe", here),
                  "a hard link to the game's own file is that file; a copy of it is not");
        }
        // The junction first, by itself: removing it leaves what it leads to.
        if (joined) RemoveDirectoryW(utf8_to_wide(link).c_str());
        for (const std::string& file : { hard + "\\Game.exe", copy + "\\Game.exe",
                                          real + "\\Game.exe" })
            DeleteFileW(utf8_to_wide(file).c_str());
        for (const std::string& folder : { hard, copy, real })
            RemoveDirectoryW(utf8_to_wide(folder).c_str());
    }
}

void test_crash_watch() {
    section("Crash watch");

    // Reading the code out of a box rather than recognising the box.
    //
    // This is the whole reason the dialog's text is read instead of matched
    // against a picture: there are a great many of these, nobody has a picture
    // of each, and a code never seen before still has to come out right.
    {
        check(CrashWatch::code_in("Error: 134") == "134", "a number after Error");
        check(CrashWatch::code_in("Lỗi 2001") == "2001", "and after the Vietnamese");
        check(CrashWatch::code_in("Mã lỗi: 77") == "77", "and after 'mã lỗi'");
        check(CrashWatch::code_in("error code #42") == "42", "'#' in the way");
        check(CrashWatch::code_in("The program failed (1053)") == "1053",
              "a parenthesised number");
        check(CrashWatch::code_in("Exception 0x8007000E raised") == "0x8007000E",
              "a hex code, which is what Windows itself produces");

        // The one that matters: this client's own disconnect box, copied out
        // of a live one. SQUARE brackets, hard against the full stop, with
        // "Error" on a different line from the number -- so looking after the
        // word finds nothing. Every invented case above passed before this
        // dialog was ever seen. It did not.
        check(CrashWatch::code_in(
                  "Connect Error\nOK\nK\xE1\xBA\xBFt n\xE1\xBB\x91i v\xE1\xBB\x9Bi "
                  "m\xC3\xA1y ch\xE1\xBB\xA7 \xC4\x91\xC3\xA3 b\xE1\xBB\x8B "
                  "ng\xE1\xBA\xAFt.[122]") == "122",
              "the client's real disconnect box, read out of a live one");
        check(CrashWatch::code_in("Disconnected {4096}") == "4096",
              "and braces, while we are here");

        // Hex wins over decimal, because a dialog carrying both is carrying
        // the hex one as the error and the decimal as a line number or a size.
        check(CrashWatch::code_in("Error 5: fault at 0xC0000005") == "0xC0000005",
              "the hex code is preferred");

        // And nothing is invented. A dialog with no code in it is ordinary.
        check(CrashWatch::code_in("Disconnected from the server").empty(),
              "no code where there is none");
        check(CrashWatch::code_in("").empty(), "nothing in, nothing out");
        check(CrashWatch::code_in("error: something went wrong").empty(),
              "the word alone is not a code");
    }

    // Telling a message box from a window.
    {
        check(CrashWatch::looks_like_a_box("#32770", "Error", "It broke"),
              "a standard dialog is a box");
        check(CrashWatch::looks_like_a_box("Static", "Game.exe",
                                           "Game.exe has stopped working"),
              "and so is whatever Windows puts that sentence in");
        check(!CrashWatch::looks_like_a_box("GhostOnlineClass", "GhostOnlineVTC", ""),
              "the game's own window is not a box");
    }

    // Windows' crash reporter puts up the same box for every program that
    // crashes. Whose crash it is, is on its command line: the process after
    // -p. Only a reporter reporting on the game speaks for the game.
    {
        check(CrashWatch::reported_pid("C:\\WINDOWS\\SysWOW64\\WerFault.exe -u -p 9876 -s 1088") ==
                      9876 &&
                  CrashWatch::reported_pid(
                      "\"C:\\Windows\\system32\\WerFault.exe\" -pss -s 464 -p 1812 -ip 1812") ==
                      1812 &&
                  CrashWatch::reported_pid("werfault.exe /h /shared Global\\a1b2 /t 4300 /P 3504") ==
                      3504,
              "a crash reporter's command line names the process it reports on, after -p or /p, "
              "whatever the case");
        check(CrashWatch::reported_pid("C:\\WINDOWS\\system32\\WerFault.exe -k -rq") == 0 &&
                  CrashWatch::reported_pid("WerFault.exe -pss -s 464") == 0 &&
                  CrashWatch::reported_pid("WerFault.exe -u -p 18x -s 1") == 0 &&
                  CrashWatch::reported_pid("WerFault.exe -u -p") == 0 &&
                  CrashWatch::reported_pid("WerFault.exe -u -p 99999999999") == 0 &&
                  CrashWatch::reported_pid("") == 0,
              "and one that names none -- a queued report, -pss alone, a number that is not one -- "
              "reports on nothing");
        check(CrashWatch::reported_pid("\"C:\\Program Files\\x -p 7\\WerFault.exe\" -u -p 42") == 42,
              "a -p inside the quoted path of the program is not taken for one");

        const std::vector<RunningProgram> games = {
            { 1812, "Game.exe", "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe" }
        };
        check(CrashWatch::reports_on("WerFault.exe -u -p 1812 -s 5", games) &&
                  !CrashWatch::reports_on("WerFault.exe -u -p 4242 -s 5", games) &&
                  !CrashWatch::reports_on("WerFault.exe -k -rq", games) &&
                  !CrashWatch::reports_on("WerFault.exe -u -p 1812 -s 5", {}),
              "a crash reporter is the game's only while it reports on one of the game's programs: "
              "not on another program's crash, and not with the game not running");

        // Windows tells a program's command line for the asking: this test's
        // own, here, as it was started.
        std::string mine = CrashWatch::command_line_of(GetCurrentProcessId());
        for (char& c : mine)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        check(mine.find("gobot_test") != std::string::npos &&
                  CrashWatch::command_line_of(0).empty() &&
                  CrashWatch::command_line_of(0xFFFFFFF0ul).empty(),
              "a program's command line is read off it, and a program that is not there has none");
    }

    // The settings survive the file. A restart cap that did not load would be
    // a bot that keeps reconnecting to a server which is refusing it.
    {
        CrashWatch::Settings s;
        s.game_process = "Other.exe";
        s.hung_grace_s = 45;
        s.max_restarts = 2;
        s.cooldown_s = 30;

        IniFile ini;
        s.save(ini);

        CrashWatch::Settings back;
        back.load(ini);
        check(back.game_process == "Other.exe" && back.hung_grace_s == 45 &&
              back.max_restarts == 2 && back.cooldown_s == 30,
              "crash settings survive a config round trip");

        IniFile bad;
        bad.set_int("Crash", "hung_grace_s", 0);
        bad.set_int("Crash", "max_restarts", -3);
        CrashWatch::Settings clamped;
        clamped.load(bad);
        check(clamped.hung_grace_s >= 5 && clamped.max_restarts >= 0,
              "unusable values are clamped");
    }

    // Where the game is installed, for the looks below: this test's own
    // program stands in for it, and a folder of the test's own for anywhere
    // else -- so nothing here depends on the game being installed, or
    // running, on this machine.
    wchar_t self_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self_path, MAX_PATH);
    GameInstall here_too;
    here_too.programs = { wide_to_utf8(self_path) };
    GameInstall elsewhere;
    elsewhere.programs = { temp_file("gobot_test.exe") };

    // A look at a machine where the game is certainly not running says so,
    // and says it without a capture, a template or a window.
    {
        CrashWatch::Settings s;
        s.game_process = "surely_nothing_is_called_this.exe";
        s.reporter_process = {};
        CrashWatch watch(s);
        watch.set_install(elsewhere);
        const auto seen = watch.look();
        check(seen.what == CrashWatch::Trouble::Gone,
              "a client that is not running reads as gone");
        check(seen.dialog == nullptr, "with no box to dismiss");
    }

    // And this process, which is running and answering, reads as fine.
    {
        CrashWatch::Settings s;
        s.game_process = "gobot_test.exe";
        s.reporter_process = {};
        CrashWatch watch(s);
        watch.set_install(here_too);
        check(!watch.look().bad(), "a running, answering program reads as fine");

        // The same program with the game installed somewhere else is only
        // called what the game is called: not watched, so not followed, and
        // never ended as a client that fell over.
        CrashWatch other(s);
        other.set_install(elsewhere);
        check(other.look().what == CrashWatch::Trouble::Gone,
              "a program merely called what the game is called, running from another folder, "
              "is not the game: the client reads as gone");
    }

    // A crash box up for another program: not the game's, so the keeper
    // neither answers it nor ends its reporter. The box is a window of this
    // test's own, put up off every screen; the reporter is this test's own
    // program, whose command line names no -p. Before, the reporter's name
    // alone made such a box the game's, and the keeper's shut-down then ended
    // the game and every program of the reporter's name.
    {
        const HINSTANCE self = GetModuleHandleA(nullptr);
        WNDCLASSA wc{};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = self;
        wc.lpszClassName = "GoBotTestOtherCrashBox";
        RegisterClassA(&wc);        // already there from an earlier run: the same
        const int far_x = GetSystemMetrics(SM_XVIRTUALSCREEN) - 4000;
        const int far_y = GetSystemMetrics(SM_YVIRTUALSCREEN) - 4000;
        HWND box = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                                   "Some Program has stopped working", WS_POPUP, far_x, far_y,
                                   200, 100, nullptr, nullptr, self, nullptr);
        if (box) ShowWindow(box, SW_SHOWNOACTIVATE);
        // What the rule is for: a box of the reporter's program, read as one.
        bool a_box = false;
        for (const auto& w : InputManager::windows_of("gobot_test.exe"))
            if (box != nullptr && w.hwnd == box)
                a_box = CrashWatch::looks_like_a_box(w.window_class, w.title,
                                                     InputManager::window_text(w.hwnd));
        if (!a_box) {
            std::printf("  [skip] could not put up a box of this test's own here\n");
        } else {
            CrashWatch::Settings s;
            s.game_process = "gobot_test_no_such_game.exe";
            s.reporter_process = "gobot_test.exe";
            CrashWatch watch(s);
            watch.set_install(elsewhere);
            const auto seen = watch.look();
            check(seen.what == CrashWatch::Trouble::Gone && seen.dialog == nullptr,
                  "another program's crash box, up while the game is not running, is not the "
                  "game's: the client reads as gone, with no box to answer");
        }
        if (box) DestroyWindow(box);
    }

    // What a keeper does about one look. BẮT ĐẦU with the game closed waits
    // on the keeper to open it; a game that came up by itself meanwhile is
    // followed, not crossed into again, and is no restart.
    {
        using M = CrashWatch::Move;
        CrashWatch::Look fine;
        CrashWatch::Look gone;
        gone.what = CrashWatch::Trouble::Gone;
        CrashWatch::Look hung;
        hung.what = CrashWatch::Trouble::Hung;
        check(CrashWatch::next_move(fine, false, true) == M::Follow,
              "a cold start whose game is up already: follow it and start the jobs");
        check(CrashWatch::next_move(gone, false, true) == M::Start,
              "a cold start with the game closed: cross in, as a start");
        check(CrashWatch::next_move(fine, true, false) == M::Watch &&
                  CrashWatch::next_move(fine, false, false) == M::Watch,
              "a game that is up with nothing waiting on it: just watch");
        check(CrashWatch::next_move(gone, true, false) == M::Restart &&
                  CrashWatch::next_move(hung, false, false) == M::Restart &&
                  CrashWatch::next_move(gone, true, true) == M::Restart,
              "and one that was up and fell over, or hangs, is a restart");
    }
}

void test_find_bars() {
    section("Finding the bars");

    Frame f = make_frame(640, 300);

    // Panel background.
    for (int y = 0; y < 60; ++y)
        for (int x = 0; x < 400; ++x) put_px(f, x, y, 24, 24, 28);

    const struct { int x, y, h, fill, full; std::uint8_t r, g, b; } kHud[] = {
        { 82, 6,  9, 103, 103, 255, 8,   8   },   // HP, nearly full
        { 82, 20, 9, 80,  103, 0,   57,  206 },   // MP, part drained
        { 82, 34, 5, 40,  103, 170, 20,  20  },   // experience -- also red
    };
    for (const auto& b : kHud) {
        for (int y = b.y; y < b.y + b.h; ++y) {
            for (int i = 0; i < b.full; ++i) {
                if (i < b.fill) put_px(f, b.x + i, y, b.r, b.g, b.b);
                else            put_px(f, b.x + i, y, 10, 10, 12);   // trough
            }
        }
    }

    // The numbers the game prints over the fill, and a lump of saturated
    // artwork that is not a bar.
    for (int i = 0; i < 24; ++i) put_px(f, 150 + i, 10, 255, 255, 255);
    for (int y = 70; y < 120; ++y)
        for (int x = 300; x < 380; ++x) put_px(f, x, y, 40, 200, 60);

    BarSpec hp, mp;
    const int n = VitalsReader::find_bars(f, hp, mp);
    check(n == 2, "finds both bars");

    // The spec reads the middle of the bar rather than all of it: real bars
    // are vertical gradients that are flat in the centre and dark at the
    // edges, so a few rows around the middle match one colour where the full
    // height cannot. What matters is that it lands inside the bar, is tall
    // enough for a median, and measures correctly.
    check(hp.x == 82, "HP x");
    check(hp.y >= 6 && hp.y + hp.height <= 6 + 9, "HP rows lie inside the bar");
    check(hp.height >= 3, "HP keeps enough rows to take a median over");
    check(hp.width == 103, "HP width includes the empty trough");
    check(hp.r > 200 && hp.g < 60 && hp.b < 60, "HP colour is the fill colour");

    check(mp.x == 82, "MP x");
    check(mp.y >= 20 && mp.y + mp.height <= 20 + 9, "MP rows lie inside the bar");
    check(mp.height >= 3, "MP keeps enough rows to take a median over");
    check(mp.width == 103, "MP width is fill plus trough, not just the fill");
    check(mp.b > 150 && mp.r < 60, "MP colour is the fill colour");

    // The point of the whole exercise: the found geometry measures correctly.
    const float hp_pct = VitalsReader::measure(f, hp);
    const float mp_pct = VitalsReader::measure(f, mp);
    check(std::fabs(hp_pct - 100.0f) < 2.0f, "found HP bar measures 100%");
    check(std::fabs(mp_pct - 77.7f) < 2.0f, "found MP bar measures 78%");

    // The experience bar must not be mistaken for HP. It is red, it is the
    // same shape, and it sits directly underneath -- the only thing separating
    // them is that HP is above it.
    check(hp.y < 34, "picks HP over the experience bar below it");

    // The HUD can be resized from the game's own settings. The bars get longer,
    // the numbers move on top of them, and the empty part is light grey rather
    // than near black -- all three of which defeated the first version of this.
    {
        Frame wide = make_frame(640, 300);
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 400; ++x) put_px(wide, x, y, 24, 24, 28);

        const struct { int x, y, h, fill, full; std::uint8_t r, g, b; } kWide[] = {
            { 80, 6,  11, 158, 160, 255, 8,  8   },
            { 80, 20, 11, 26,  160, 0,   57, 206 },
        };
        for (const auto& bar : kWide) {
            for (int y = bar.y; y < bar.y + bar.h; ++y) {
                for (int i = 0; i < bar.full; ++i) {
                    if (i < bar.fill) put_px(wide, bar.x + i, y, bar.r, bar.g, bar.b);
                    else              put_px(wide, bar.x + i, y, 176, 176, 180);
                }
            }
            // The numbers, printed across the right-hand end of the bar.
            for (int y = bar.y + 2; y < bar.y + bar.h - 2; ++y)
                for (int i = 100; i < 150; i += 2)
                    put_px(wide, bar.x + i, y, 250, 250, 250);
        }

        BarSpec whp, wmp;
        check(VitalsReader::find_bars(wide, whp, wmp) == 2, "finds a resized HUD's bars");
        check(whp.width > 140, "the writing across the bar does not cut its length short");
        check(std::abs(whp.width - wmp.width) <= 6, "both bars come out the same length");
        check(std::fabs(VitalsReader::measure(wide, whp) - 98.75f) < 3.0f,
              "the full bar measures right");
        check(std::fabs(VitalsReader::measure(wide, wmp) - 16.25f) < 3.0f,
              "the drained bar measures right over a light grey empty part");

        // And the old, narrow geometry must be recognised as no longer matching,
        // which is what makes the reader go and look again.
        BarSpec stale = whp;
        stale.width = 102;
        check(VitalsReader::bar_is_stale(wide, stale),
              "a bar narrower than the real one reads as stale");
        check(!VitalsReader::bar_is_stale(wide, whp), "the right geometry does not");
    }

    // Nothing to find: a frame with no bars at all must say so rather than
    // latch onto whatever is most colourful.
    Frame plain = make_frame(640, 300);
    for (int y = 0; y < plain.height; ++y)
        for (int x = 0; x < plain.width; ++x)
            put_px(plain, x, y, static_cast<std::uint8_t>(60 + (x % 40)), 90, 70);
    BarSpec a, b;
    check(VitalsReader::find_bars(plain, a, b) == 0, "finds nothing in a frame with no bars");
    check(VitalsReader::find_bars(Frame{}, a, b) == 0, "no frame finds nothing");
}

// ------------------------------------------------------------ mob detector

/// Paint the game's name plate: a dark box with white writing, a red health
/// bar under it, and optionally a blue mana bar under that.
void paint_plate(Frame& f, int cx, int feet_y, int bar_len, int bar_full,
                 bool player, const char* = nullptr) {
    const int box_w = std::max(bar_full + 2, 40);
    const int box_h = 16;
    const int box_x = cx - box_w / 2;
    const int box_y = feet_y - box_h;

    for (int y = box_y; y < box_y + box_h; ++y)
        for (int x = box_x; x < box_x + box_w; ++x)
            put_px(f, x, y, 48, 50, 54);

    // The name: bright, colourless, and centred on the creature.
    //
    // Centred because that is what the game does, and because the detector now
    // takes the creature's position from the writing rather than from the box
    // behind it -- the box is translucent and turns the colour of whatever it
    // is drawn over, so no fixed description of it survives walking onto grass.
    for (int i = 0; i < 24; ++i)
        put_px(f, cx - 11 + (i % 12) * 2, box_y + 5 + (i / 12) * 4, 240, 240, 240);

    const int bar_x = cx - bar_full / 2;
    for (int y = feet_y; y < feet_y + 3; ++y) {
        for (int i = 0; i < bar_full; ++i) {
            if (i < bar_len) put_px(f, bar_x + i, y, 230, 10, 10);
            else             put_px(f, bar_x + i, y, 12, 10, 12);
        }
    }
    if (player) {
        for (int y = feet_y + 3; y < feet_y + 5; ++y)
            for (int i = 0; i < bar_full / 2; ++i)
                put_px(f, bar_x + i, y, 10, 60, 210);
    }
}

void test_template_matcher() {
    section("Finding a picture on the screen");

    // A busy screen with a distinctive little panel sitting somewhere in it.
    Frame screen = make_frame(1024, 768);
    std::mt19937 rng(99);
    for (int y = 0; y < screen.height; ++y)
        for (int x = 0; x < screen.width; ++x)
            put_px(screen, x, y, static_cast<std::uint8_t>(40 + (rng() % 60)),
                   static_cast<std::uint8_t>(60 + (rng() % 60)),
                   static_cast<std::uint8_t>(80 + (rng() % 60)));

    const int tw = 64, th = 24;
    Frame panel = make_frame(tw, th);
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            const bool edge = x < 2 || y < 2 || x >= tw - 2 || y >= th - 2;
            const bool bar = y > 8 && y < 16 && x > 6 && x < tw - 6;
            put_px(panel, x, y, edge ? 250 : (bar ? 20 : 150),
                                edge ? 240 : (bar ? 30 : 140),
                                edge ? 20  : (bar ? 25 : 130));
        }

    const int at_x = 377, at_y = 251;
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            std::uint8_t r, g, b;
            panel.get(x, y, r, g, b);
            put_px(screen, at_x + x, at_y + y, r, g, b);
        }

    const auto hit = TemplateMatcher::find(screen, ScreenRect{ 0, 0, 1024, 768 }, panel);
    check(hit.found, "finds a panel pasted into a noisy screen");
    if (hit.found) {
        check(std::abs(hit.at.x - at_x) <= 1 && std::abs(hit.at.y - at_y) <= 1,
              "to the pixel (" + std::to_string(hit.at.x) + "," +
              std::to_string(hit.at.y) + " vs " + std::to_string(at_x) + "," +
              std::to_string(at_y) + ")");
        check(hit.score > 0.95f, "with a high score");
    }

    // The same panel drawn darker, as the game does when a dialog is behind
    // something. Correlation is supposed to survive that; a difference test
    // would not.
    {
        Frame dim = make_frame(1024, 768);
        for (int y = 0; y < dim.height; ++y)
            for (int x = 0; x < dim.width; ++x) {
                std::uint8_t r, g, b;
                screen.get(x, y, r, g, b);
                put_px(dim, x, y, static_cast<std::uint8_t>(r * 3 / 5),
                       static_cast<std::uint8_t>(g * 3 / 5),
                       static_cast<std::uint8_t>(b * 3 / 5));
            }
        const auto faded = TemplateMatcher::find(dim, ScreenRect{ 0, 0, 1024, 768 }, panel);
        check(faded.found && std::abs(faded.at.x - at_x) <= 1,
              "still finds it when the whole screen is dimmed");
    }

    // And says so when it is not there.
    {
        Frame other = make_frame(tw, th);
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x)
                put_px(other, x, y, static_cast<std::uint8_t>(x * 4),
                       static_cast<std::uint8_t>(y * 10), 90);
        const auto miss = TemplateMatcher::find(screen, ScreenRect{ 0, 0, 1024, 768 }, other);
        check(!miss.found, "reports nothing for a picture that is not there");
    }

    // Templates of many shapes, because the search steps down through reduced
    // copies and how it steps depends on the size. A run of sizes is the only
    // way to catch a level that lands askew: the first version of this worked
    // for a sixty-four pixel template and silently failed for an eighteen
    // pixel one, because the one's levels happened to halve cleanly and the
    // other's did not.
    for (int side : { 11, 15, 18, 19, 24, 31, 37, 48, 60, 97 }) {
        Frame noisy = make_frame(600, 400);
        std::mt19937 r2(static_cast<unsigned>(side) * 7919u);
        for (int y = 0; y < noisy.height; ++y)
            for (int x = 0; x < noisy.width; ++x)
                put_px(noisy, x, y, static_cast<std::uint8_t>(r2() % 200),
                       static_cast<std::uint8_t>(r2() % 200),
                       static_cast<std::uint8_t>(r2() % 200));

        // A picture with structure at every scale, which is what the search
        // needs and what real artwork has: a dark border, a pale fill and a
        // block in one corner, each sized as a fraction of the whole.
        //
        // Deliberately not a fine checkerboard. The search works by averaging
        // the picture down into blocks, and a checkerboard averages to flat
        // grey the moment it does; there is then nothing left to match, and
        // the failure belongs to the pattern rather than to the matcher.
        const int px = 173, py = 109;
        const int edge = std::max(1, side / 8);
        Frame mark = make_frame(side, side);
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x) {
                const bool border = x < edge || y < edge ||
                                    x >= side - edge || y >= side - edge;
                const bool corner = x < side / 2 && y < side / 3;
                const auto v = static_cast<std::uint8_t>(border ? 20
                                                       : corner ? 215 : 110);
                put_px(mark, x, y, v, v, static_cast<std::uint8_t>(255 - v));
                put_px(noisy, px + x, py + y, v, v, static_cast<std::uint8_t>(255 - v));
            }

        const auto got = TemplateMatcher::find(noisy, ScreenRect{ 0, 0, 600, 400 }, mark);
        check(got.found && std::abs(got.at.x - px) <= 1 && std::abs(got.at.y - py) <= 1,
              std::to_string(side) + "x" + std::to_string(side) +
              " is found where it was put");
    }

    // Several copies, counted once each.
    {
        Frame many = screen;
        const int spots[][2] = { { 100, 100 }, { 600, 420 }, { 820, 90 } };
        for (const auto& s : spots)
            for (int y = 0; y < th; ++y)
                for (int x = 0; x < tw; ++x) {
                    std::uint8_t r, g, b;
                    panel.get(x, y, r, g, b);
                    put_px(many, s[0] + x, s[1] + y, r, g, b);
                }
        const auto all = TemplateMatcher::find_all(many, ScreenRect{ 0, 0, 1024, 768 },
                                                   panel, 0.9f, 16);
        check(all.size() == 4, "finds every copy exactly once (" +
                               std::to_string(all.size()) + ")");
    }
}

void test_map_table() {
    section("The game's own list of places");

    MapTable table;
    const std::string path = MapTable::default_path();
    if (path.empty() || !table.load(path)) {
        // The table belongs to the game, read from wherever it is installed.
        // A machine without the game cannot run this, and the rest of the
        // suite should not start needing one.
        std::printf("  [skip] the game is not installed here\n");
        return;
    }
    check(table.entries().size() > 500, "reads the whole table");

    // Every row should have come out intact. A format misread does not fail
    // halfway -- it desynchronises, and the tail turns to rubbish, so the test
    // that matters is on the last rows as much as the first.
    int lo_theme = 1 << 30, hi_theme = -(1 << 30);
    int lo_stage = 1 << 30, hi_stage = -(1 << 30);
    std::size_t longest_name = 0;
    for (const MapTable::Entry& e : table.entries()) {
        lo_theme = std::min(lo_theme, e.theme);
        hi_theme = std::max(hi_theme, e.theme);
        lo_stage = std::min(lo_stage, e.stage);
        hi_stage = std::max(hi_stage, e.stage);
        longest_name = std::max(longest_name, e.name.size());
    }
    check(lo_theme >= 0 && hi_theme < 10000 && lo_stage >= 0 && hi_stage < 10000,
          "themes " + std::to_string(lo_theme) + ".." + std::to_string(hi_theme) +
          " and stages " + std::to_string(lo_stage) + ".." + std::to_string(hi_stage) +
          " are all plausible");
    check(longest_name < 200,
          "the longest name is " + std::to_string(longest_name) + " bytes");

    const MapTable::Entry& last = table.entries().back();
    check(!last.name.empty() && last.theme >= 0,
          "the last row is intact: " + last.stem() + " " + last.name);

    const MapTable::Entry* coc = table.find("ThanhÂmCốc[1]");
    check(coc != nullptr, "finds a place by its exact name");
    if (coc) check(coc->stem() == "t4_s1",
                   "and names its collision file (" + coc->stem() + ")");

    // Names are not unique, which is the whole reason the creature list has to
    // break the tie.
    const MapTable::Entry* second = table.find("ThanhÂmCốc[2]");
    check(second != nullptr && second->stem() == "t4_s2",
          "the same place name exists twice, as different maps");
}

void test_world_anchor() {
    section("Tying the wire to the screen");

    // A level with creatures scattered along it, and a camera looking at part
    // of it. The transform the anchor has to recover: world = 1.25 * (screen +
    // camera) + 900. Nothing tells it either number.
    const double kScale = 1.25;
    const double kOffset = 900.0;

    std::vector<Monster> world;
    const double spots[] = { 120, 480, 910, 1180, 1600, 2050, 2390, 2800,
                             3310, 3720, 4080, 4550, 5010, 5400 };
    std::int32_t id = 1;
    for (double sx : spots) {
        Monster m;
        m.id = id++;
        m.pos.x = static_cast<float>(kScale * sx + kOffset);
        m.pos.y = static_cast<float>(kScale * 600.0 + kOffset);
        world.push_back(m);
    }

    // Walk the camera along, showing whatever falls in the window. Each
    // creature is seen many times from different camera positions, which is
    // the whole point: no single frame shows enough of them at once.
    WorldAnchor anchor;
    for (int step = 0; step < 240; ++step) {
        const double camera = step * 20.0;
        std::vector<Nameplate> seen;
        for (double sx : spots) {
            const double on_screen = sx - camera;
            if (on_screen < 0.0 || on_screen > 1000.0) continue;
            Nameplate n;
            n.plate = ScreenRect{ static_cast<int>(on_screen) - 10,
                                  static_cast<int>(600.0 - camera * 0.0) - 6, 20, 12 };
            n.bar = ScreenRect{ n.plate.x, n.plate.y + 14, 20, 3 };
            seen.push_back(n);
        }
        anchor.observe(world, seen, camera, 0.0);
    }

    const WorldAnchor::Fix fix = anchor.current();
    check(fix.valid, "finds the transform from landmarks seen at different times");
    if (fix.valid) {
        check(std::fabs(fix.scale - kScale) < 0.08,
              "recovers the scale (" + std::to_string(fix.scale) + ")");
        Vec2 w;
        check(anchor.to_world(500, 600, 1000.0, 0.0, w), "converts a screen position");
        const double want = kScale * (500 + 1000.0) + kOffset;
        check(std::fabs(w.x - want) < 90.0,
              "puts a screen position in the right place (" + std::to_string(w.x) +
              " vs " + std::to_string(want) + ")");
    }

    // And a screen with nothing recognisable on it says nothing. A bot that
    // invents a position when it cannot see walks into walls.
    {
        WorldAnchor blind;
        for (int step = 0; step < 240; ++step) {
            std::vector<Nameplate> seen;
            Nameplate n;
            n.plate = ScreenRect{ 400, 600, 20, 12 };
            n.bar = ScreenRect{ 400, 614, 20, 3 };
            seen.push_back(n);
            blind.observe(world, seen, step * 20.0, 0.0);
        }
        check(!blind.current().valid, "one landmark is not a position");
    }
}

void test_glyph_font() {
    section("Glyph font");

    GlyphFont font;
    const std::string dir = GlyphFont::default_dir();
    if (dir.empty() || !font.load(dir)) {
        // The glyph set is the game's own, read in place from wherever it is
        // installed, so a machine without it cannot run this. Say so rather
        // than failing: the rest of the suite has no such dependency and
        // should not start needing one.
        std::printf("  [skip] no glyph set installed -- nothing to read with\n");
        return;
    }
    check(font.size() > 50, "the glyph set loaded");

    // Anything drawn with these glyphs has to come back exactly. Any failure
    // here is the reader's own and has nothing to do with the game: no window,
    // no capture, nobody logged in.
    const char* samples[] = {
        "ThanhAmCoc",       // plain letters
        "KhiCon",           // the narrow ones -- an 'i' is a single column
        "0123456789",
        "Thanh Am Coc",     // word breaks, not just letter spacing
        "ThanhÂmCốc",       // accents above, which is where it used to give up
        "KhỉCon",
    };
    for (const char* want : samples) {
        Frame drawn;
        if (!font.render_line(want, drawn)) {
            check(false, std::string("no glyph for some letter of \"") + want + "\"");
            continue;
        }
        const std::string got =
            font.read_line(drawn, ScreenRect{ 0, 0, drawn.width, drawn.height });
        check(got == want, std::string("reads back \"") + want + "\"" +
                           (got == want ? "" : " -- got \"" + got + "\""));
    }

    // And blank paper says nothing. A reader that finds letters in an empty
    // frame will find them in grass, in rock and in sky, which is exactly how
    // a temple roof came to be read as nine monsters.
    {
        Frame blank = make_frame(80, 20);
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 80; ++x) put_px(blank, x, y, 30, 34, 40);
        const std::string got = font.read_line(blank, ScreenRect{ 0, 0, 80, 20 });
        check(got.empty(), "reads nothing off an empty patch");
    }
    {
        // A bright, flat expanse -- a cloud, a wall, the sky -- is not writing
        // either, however bright it is.
        Frame sky = make_frame(80, 20);
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 80; ++x) put_px(sky, x, y, 240, 244, 250);
        const std::string got = font.read_line(sky, ScreenRect{ 0, 0, 80, 20 });
        check(got.empty(), "reads nothing off a bright expanse");
    }

    // Reading the figures the game prints across its health bars.
    //
    // Built the way the game builds it: a coloured bar with its caption
    // right-aligned inside it, overlapping the fill. This is the layout that
    // defeats measuring the bar -- the writing paints out the end of the fill,
    // so the colour runs out early and a bar near full measures two-thirds --
    // and the whole point of reading the caption instead.
    {
        BarSpec bar;
        bar.x = 83; bar.y = 8; bar.width = 159; bar.height = 5;
        bar.r = 239; bar.g = 0; bar.b = 0; bar.tolerance = 80;

        // Written with a space where the game writes a slash. The game's set
        // has no slash glyph -- it is letters, digits and Vietnamese accents
        // and nothing else -- so the live reader never sees one either: the
        // slash comes back as a gap, and both numbers survive it. Rendering a
        // character the set does not have would test nothing but the renderer.
        Frame drawn;
        if (!font.render_line("6744 9232", drawn)) {
            std::printf("  [skip] no glyph for the caption's characters\n");
        } else {
            Frame hud = make_frame(400, 60);
            for (int y = 0; y < hud.height; ++y)
                for (int x = 0; x < hud.width; ++x) put_px(hud, x, y, 24, 24, 28);
            // The bar: fill to three-quarters, dark trough after it.
            for (int y = bar.y - 3; y < bar.y + bar.height + 3; ++y)
                for (int x = bar.x; x < bar.x + bar.width; ++x)
                    put_px(hud, x, y,
                           static_cast<std::uint8_t>(x < bar.x + bar.width * 3 / 4 ? 239 : 18),
                           0, 0);

            // The caption, right-aligned and sitting across the bar's middle.
            const ScreenRect box = VitalsReader::caption_box(bar);
            const int left = bar.right() - 7 - drawn.width;
            const int top  = box.y + (box.h - drawn.height) / 2;
            for (int y = 0; y < drawn.height; ++y)
                for (int x = 0; x < drawn.width; ++x) {
                    std::uint8_t r, g, b;
                    drawn.get(x, y, r, g, b);
                    put_px(hud, left + x, top + y, r, g, b);
                }

            int current = 0, maximum = 0;
            check(VitalsReader::read_numbers(hud, bar, font, current, maximum) &&
                  current == 6744 && maximum == 9232,
                  "reads 6744/9232 off a bar it is printed across" +
                      std::string(current ? " -- got " + std::to_string(current) + "/" +
                                            std::to_string(maximum) : ""));

            // And the measurement it replaces gets this wrong, which is why it
            // is the fallback and not the answer.
            const float measured = VitalsReader::measure(hud, bar);
            check(measured >= 0.0f && std::abs(measured - 73.0f) > 5.0f,
                  "measuring the same bar does not agree, as expected");

            // Still read when the bar is measured too narrow.
            //
            // This is not hypothetical. Working out a bar's width means
            // measuring the colour and then the empty part after it, and when
            // the fill happens to run out underneath the caption the second
            // measurement starts on white text and stops at once. The live
            // reader saw 115 where the bar is 159. If the caption box hangs
            // off the right edge the digits fall outside it and the figures
            // silently stop updating.
            for (const int shortfall : { 10, 24, 44 }) {
                BarSpec narrow = bar;
                narrow.width = bar.width - shortfall;
                int c = 0, m = 0;
                check(VitalsReader::read_numbers(hud, narrow, font, c, m) &&
                      c == 6744 && m == 9232,
                      "still reads the figures with the bar measured " +
                          std::to_string(shortfall) + "px too narrow");
            }

            // A value with a '1' in it, laid out the way the game lays it out.
            //
            // The game puts its digits on a fixed pitch and the glyph files are
            // cropped to their ink, so a '1' -- two pixels of ink in a six pixel
            // cell -- leaves a gap behind it as wide as the gap across the '/'.
            // The reader cannot tell those apart by spacing, and "6710/9232"
            // came back as "671 0 9232".
            //
            // Rendering a whole line would not catch this: the renderer spaces
            // glyphs by their ink, so its '1' sits snug and the bug never
            // appears. The digits have to be placed one at a time, on the pitch,
            // to reproduce what the game actually draws.
            {
                Frame pitched = make_frame(400, 60);
                for (int y = 0; y < pitched.height; ++y)
                    for (int x = 0; x < pitched.width; ++x) put_px(pitched, x, y, 20, 0, 0);

                const char* caption = "67109232";
                const int cell = 6;
                const int slash_after = 4;      // "6710" then "/" then "9232"
                const int baseline_x = bar.right() - 7 - (8 * cell + cell);
                const int baseline_y = box.y + 5;

                bool drew = true;
                for (int i = 0; caption[i]; ++i) {
                    Frame digit;
                    if (!font.render_line(std::string(1, caption[i]), digit)) {
                        drew = false;
                        break;
                    }
                    const int at = baseline_x + i * cell + (i >= slash_after ? cell : 0);
                    for (int y = 0; y < digit.height; ++y)
                        for (int x = 0; x < digit.width; ++x) {
                            std::uint8_t r, g, b;
                            digit.get(x, y, r, g, b);
                            if (r < 60 && g < 60 && b < 60) continue;   // keep the ground
                            put_px(pitched, at + x, baseline_y + y, r, g, b);
                        }
                }

                if (drew) {
                    int c = 0, m = 0;
                    check(VitalsReader::read_numbers(pitched, bar, font, c, m) &&
                          c == 6710 && m == 9232,
                          "reads 6710/9232, where the '1' leaves a slash-sized gap" +
                              std::string(c ? " -- got " + std::to_string(c) + "/" +
                                              std::to_string(m) : ""));
                }
            }

            // And still reads them with something else written further right.
            //
            // Reaching past the bar's measured end is what makes the above
            // work, and on this client it catches the edge of the avatar
            // portrait sitting beside the bars. A stray figure off that used
            // to throw the whole line away.
            {
                Frame busy = hud;
                Frame junk;
                if (font.render_line("88", junk)) {
                    for (int y = 0; y < junk.height; ++y)
                        for (int x = 0; x < junk.width; ++x) {
                            std::uint8_t r, g, b;
                            junk.get(x, y, r, g, b);
                            put_px(busy, bar.right() + 24 + x, box.y + 3 + y, r, g, b);
                        }
                    int c = 0, m = 0;
                    check(VitalsReader::read_numbers(busy, bar, font, c, m) &&
                          c == 6744 && m == 9232,
                          "reads the caption past writing further right");
                }
            }
        }

        // A bar with nothing written on it is not a reading. Returning a stale
        // or invented number here is worse than returning none: a potion rule
        // acts on it.
        Frame plain = make_frame(400, 60);
        for (int y = 0; y < plain.height; ++y)
            for (int x = 0; x < plain.width; ++x)
                put_px(plain, x, y, static_cast<std::uint8_t>(
                           x >= bar.x && x < bar.x + 100 &&
                           y >= bar.y - 3 && y < bar.y + 8 ? 239 : 18), 0, 0);
        int current = -1, maximum = -1;
        check(!VitalsReader::read_numbers(plain, bar, font, current, maximum),
              "refuses a bar with no figures on it");
    }
}

void test_mob_detector() {
    section("Mob detector");

    Frame f = make_frame(1024, 768);
    // A sky so the frame is not uniformly black.
    for (int y = 0; y < 768; ++y)
        for (int x = 0; x < 1024; ++x)
            put_px(f, x, y, 120, 150, 190);

    paint_plate(f, 300, 400, 60, 60, false);     // a monster, unhurt
    paint_plate(f, 700, 500, 20, 60, false);     // a monster, badly hurt
    paint_plate(f, 512, 450, 50, 50, true);      // another player: a mana bar too

    // A red flower on a dark rock: red, with something dark above it, and no
    // writing anywhere. This is the shape of every false positive the real
    // game produced, so it is the one the test has to reject.
    for (int y = 596; y < 616; ++y)
        for (int x = 190; x < 240; ++x) put_px(f, x, y, 40, 38, 44);
    for (int y = 616; y < 620; ++y)
        for (int x = 200; x < 230; ++x) put_px(f, x, y, 220, 30, 30);

    const DetectorTuning t = DetectorTuning::defaults();
    const std::vector<Nameplate> found = MobDetector::detect(f, t);

    int monsters = 0, players = 0;
    for (const Nameplate& n : found) {
        if (n.is_player) ++players; else ++monsters;
    }
    check(monsters == 2, "finds both monsters");
    check(players == 1, "finds the other player, by its mana bar");
    check(found.size() == 3, "and nothing else -- the flower is not a monster");

    // Position comes from the name box, which does not move as health drops.
    for (const Nameplate& n : found) {
        if (n.is_player) {
            check(std::abs(n.x() - 512) <= 3, "a player's x from the name box");
            check(std::abs(n.y() - 450) <= 3, "a player's y at its feet");
            check(!n.is_self, "and a plate found by its bars is never yours");
        }
    }
    bool hurt_ok = false;
    for (const Nameplate& n : found) {
        if (n.is_player) continue;
        if (std::abs(n.x() - 700) <= 3) {
            hurt_ok = true;
            check(n.hp < 0.5f, "a hurt monster reads under half health");
            check(std::abs(n.x() - 700) <= 3,
                  "a hurt monster's position does not drift with its health bar");
        }
    }
    check(hurt_ok, "the hurt monster was found");

    // Outside the search area nothing is reported, which is how the game's own
    // panels -- all full of red -- are kept out.
    DetectorTuning narrow = t;
    narrow.area = ScreenRect{ 0, 0, 400, 768 };
    const std::vector<Nameplate> fewer = MobDetector::detect(f, narrow);
    check(fewer.size() == 1, "the search area excludes what is outside it");

    DetectorTuning blocked = t;
    blocked.exclude = ScreenRect{ 250, 350, 200, 120 };
    bool gone = true;
    for (const Nameplate& n : MobDetector::detect(f, blocked))
        if (std::abs(n.x() - 300) <= 4) gone = false;
    check(gone, "the excluded rectangle is skipped");

    // You are the plate found by your name, never one found by its bars: a
    // plate with bars may be yours or another player's. Taking the player
    // nearest the middle for you had the bot walk and hit relative to a party
    // member, or with no name typed at all, to whoever stood in view.
    Nameplate me;
    check(!MobDetector::pick_player(found, f.width, me),
          "another player in the middle of the screen is not taken for you");
    {
        std::vector<Nameplate> with_self = found;
        Nameplate self;
        self.plate = ScreenRect{ 200, 440, 40, 10 };
        self.bar = ScreenRect{ 200, 440, 40, 1 };
        self.is_player = true;
        self.is_self = true;
        with_self.push_back(self);
        check(MobDetector::pick_player(with_self, f.width, me) && me.is_self && me.x() == 220,
              "the plate made where your name was read is you, though another player stands "
              "nearer the middle");
    }

    // Found by name on a frame, as the running detector finds it on every
    // frame: with a stranger's plate in the middle of the screen as well.
    {
        GlyphFont font;
        const std::string dir = GlyphFont::default_dir();
        Frame name;
        const char* const who = "ThanhAmCoc";
        if (dir.empty() || !font.load(dir)) {
            std::printf("  [skip] no glyph set installed -- no name to look for\n");
        } else if (!font.render_line(who, name)) {
            check(false, "the test's name can be drawn with the glyphs");
        } else {
            Frame g = f;
            const int nx = 180, ny = 560;       // in the play field, left of the stranger
            for (int y = 0; y < name.height; ++y)
                for (int x = 0; x < name.width; ++x) {
                    std::uint8_t r = 0, gr = 0, b = 0;
                    name.get(x, y, r, gr, b);
                    put_px(g, nx + x, ny + y, r, gr, b);
                }
            Nameplate self;
            float sure = 0.0f;
            const bool got = MobDetector::find_self(g, t, font, who, self, &sure);
            check(got && self.is_self && self.is_player &&
                      std::abs(self.x() - (nx + name.width / 2)) <= 4,
                  "your character is found where its name is written (" +
                      std::to_string(static_cast<int>(sure * 100.0f)) + "% sure)");
            std::vector<Nameplate> all = MobDetector::detect(g, t, &font);
            if (got) all.push_back(self);
            Nameplate picked;
            check(got && MobDetector::pick_player(all, g.width, picked) && picked.is_self &&
                      std::abs(picked.x() - self.x()) <= 1,
                  "and that, not the stranger nearer the middle, is who the bot walks from");
            Nameplate none;
            check(!MobDetector::find_self(g, t, font, "", none),
                  "with no name typed there is no you at all");

            // Only the whole name on its plate is you. A player whose name had
            // yours inside it was read as well as your own plate, and taken for
            // you whenever theirs was met first -- as here, written higher up.
            const auto write = [&font](Frame& on, const std::string& text, int x0, int y0) {
                Frame drawn;
                if (!font.render_line(text, drawn)) return -1;
                for (int y = 0; y < drawn.height; ++y)
                    for (int x = 0; x < drawn.width; ++x) {
                        std::uint8_t r = 0, gr = 0, b = 0;
                        drawn.get(x, y, r, gr, b);
                        put_px(on, x0 + x, y0 + y, r, gr, b);
                    }
                return x0 + drawn.width / 2;
            };
            struct Pair { const char* mine; const char* theirs; };
            for (const Pair& c : { Pair{ "Anh", "TuanAnh" }, Pair{ "KhiCon", "KhiCon2" } }) {
                Frame both = f;
                const int yours = write(both, c.mine, 180, 560);
                const int theirs = write(both, c.theirs, 600, 300);
                Nameplate seen;
                const bool got_one = MobDetector::find_self(both, t, font, c.mine, seen);
                check(yours > 0 && theirs > 0 && got_one && std::abs(seen.x() - yours) <= 4,
                      std::string("\"") + c.mine + "\" is found on its own plate, not inside \"" +
                          c.theirs + "\" written above it");
                Frame theirs_only = f;
                write(theirs_only, c.theirs, 600, 300);
                check(!MobDetector::find_self(theirs_only, t, font, c.mine, seen),
                      std::string("and with only \"") + c.theirs + "\" on screen, nobody is you");
            }

            // Nor a name a letter away from yours, or yours a letter short, or
            // yours with an accent, while your own plate is out of sight.
            {
                Frame other = f;
                write(other, "ThanhAn", 600, 300);
                Nameplate seen;
                check(!MobDetector::find_self(other, t, font, "ThanhAm", seen),
                      "\"ThanhAn\" alone on screen is not \"ThanhAm\"");
                Frame shorter = f;
                write(shorter, "KhiCon", 600, 300);
                check(!MobDetector::find_self(shorter, t, font, "KhiCon2", seen),
                      "\"KhiCon\" alone on screen is not \"KhiCon2\"");
                Frame accented = f;
                write(accented, "Ánh", 600, 300);
                check(!MobDetector::find_self(accented, t, font, "Anh", seen),
                      "\"Ánh\" alone on screen is not \"Anh\"");
            }

            // A letter that hangs below the line is still one of the name's:
            // the "g" of "Long", drawn as the game draws it, two rows lower
            // than the letters beside it. Stood on the line, it was never
            // found, and a name ending in one was never found whole.
            {
                Frame hung = f;
                Frame head, tail;
                if (font.render_line("Lon", head) && font.render_line("g", tail)) {
                    const int x0 = 180, y0 = 560;
                    for (int y = y0; y < y0 + head.height + 3; ++y)
                        for (int x = x0; x < x0 + head.width + 8; ++x) put_px(hung, x, y, 0, 0, 0);
                    write(hung, "Lon", x0, y0);
                    const int foot = y0 + head.height - 3;          // where "Lon" stands
                    const int gy = foot - (tail.height - 6) - 3 + 2;
                    const int gx = x0 + head.width - 6;
                    for (int y = 0; y < tail.height; ++y)
                        for (int x = 0; x < tail.width; ++x) {
                            std::uint8_t r = 0, gr = 0, b = 0;
                            tail.get(x, y, r, gr, b);
                            if (r > 128) put_px(hung, gx + x, gy + y, r, gr, b);
                        }
                    Nameplate seen;
                    check(MobDetector::find_self(hung, t, font, "Long", seen) &&
                              std::abs(seen.x() - (x0 + (head.width + tail.width - 6) / 2)) <= 4,
                          "\"Long\", its g hanging below the line, is found whole");
                } else {
                    check(false, "the test's letters can be drawn with the glyphs");
                }
            }

            // A letter eaten by something that is not writing -- a pet, a
            // glow, grass in front of it -- still leaves you found.
            {
                Frame covered = f;
                const int centre = write(covered, who, 180, 560);
                Frame head, letter;
                if (font.render_line("ThanhA", head) && font.render_line("m", letter)) {
                    const int lx = 180 + head.width - 3;      // the "m", after "ThanhA"
                    const int lw = letter.width - 7;          // its own width, no padding
                    for (int y = 560 + 3; y < 560 + name.height - 3; ++y)
                        for (int x = lx; x < lx + lw; ++x) put_px(covered, x, y, 60, 170, 70);
                }
                Nameplate seen;
                check(MobDetector::find_self(covered, t, font, who, seen) &&
                          std::abs(seen.x() - centre) <= 4,
                      "with one letter covered by a green pet, your name is still you");
            }

            // Your name in two places -- your plate, and a chat bubble that
            // says it -- is you at the one nearer where you were last seen,
            // not whichever the search meets first.
            {
                Frame twice = f;
                const int low = write(twice, who, 180, 560);
                const int high = write(twice, who, 600, 300);
                Nameplate seen;
                const ScreenRect near_high{ 520, 200, 60, 8 };
                const ScreenRect near_low{ 250, 650, 60, 8 };
                check(MobDetector::find_self(twice, t, font, who, seen, nullptr, &near_high) &&
                          std::abs(seen.x() - high) <= 4,
                      "your name twice on screen: the one nearer where you were (above)");
                check(MobDetector::find_self(twice, t, font, who, seen, nullptr, &near_low) &&
                          std::abs(seen.x() - low) <= 4,
                      "your name twice on screen: the one nearer where you were (below)");
            }
        }
    }

    check(MobDetector::detect(Frame{}, t).empty(), "no frame finds nothing");

    // The box and what is cut out of it follow the client's size: the
    // player's client is 1280x720, and a box measured on 1024x768 cut its
    // right fifth off and took in its hotbar.
    {
        const auto same = [](const ScreenRect& a, const ScreenRect& b) {
            return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
        };
        const DetectorTuning small = t.resolved(1024, 768);
        check(t.automatic() && same(small.area, ScreenRect{ 0, 104, 1024, 566 }) &&
                  same(small.exclude, ScreenRect{ 812, 0, 212, 360 }) &&
                  same(small.exclude2, ScreenRect{ 984, 568, 40, 200 }),
              "at 1024x768 the box is the one measured there, plus the icon column");
        const DetectorTuning wide = t.resolved(1280, 720);
        check(same(wide.area, ScreenRect{ 0, 104, 1280, 518 }) &&
                  same(wide.exclude, ScreenRect{ 1068, 0, 212, 360 }) &&
                  same(wide.exclude2, ScreenRect{ 1240, 520, 40, 200 }),
              "and at 1280x720 it runs the whole width, stops above the hotbar, and cuts "
              "out the notices and the icon column against the right edge");
        check(same(narrow.resolved(1024, 768).area, narrow.area) &&
                  same(blocked.resolved(1024, 768).exclude, blocked.exclude),
              "a rectangle written down is used as written");
    }
    {
        Frame g = make_frame(1280, 720);
        for (int y = 0; y < 720; ++y)
            for (int x = 0; x < 1280; ++x) put_px(g, x, y, 120, 150, 190);
        const auto found_near = [&](int x, int y) {
            for (const Nameplate& n : MobDetector::detect(g, t))
                if (std::abs(n.x() - x) <= 4 && std::abs(n.y() - y) <= 4) return true;
            return false;
        };
        paint_plate(g, 1150, 400, 50, 60, false);
        check(found_near(1150, 400), "on a 1280x720 client a monster right of x 1024 is found");
        paint_plate(g, 1150, 200, 50, 60, false);
        check(!found_near(1150, 200), "one under the notices column is not");
        paint_plate(g, 1260, 560, 20, 30, false);
        check(!found_near(1260, 560), "nor one whose bar starts in the icon column");
        paint_plate(g, 600, 650, 50, 60, false);
        check(!found_near(600, 650), "nor one in the hotbar band");
    }
    // On the player's own frames: nothing in the hotbar band, nothing in the
    // icon column. What is found is not printed -- those frames have names
    // in them.
    {
        std::vector<std::string> shots{ exe_relative("frame.png") };
        WIN32_FIND_DATAA fd{};
        const std::string dir = exe_relative("trash_debug");
        HANDLE h = FindFirstFileA((dir + "\\*.png").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { shots.push_back(dir + "\\" + fd.cFileName); } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        int looked = 0, wrong = 0;
        for (const std::string& path : shots) {
            Frame shot;
            if (!load_png(path, shot) || !shot.valid()) continue;
            ++looked;
            const DetectorTuning used = t.resolved(shot.width, shot.height);
            for (const Nameplate& n : MobDetector::detect(shot, t))
                if (n.bar.y >= shot.height - 98 || used.exclude2.contains(n.bar.x, n.bar.y))
                    ++wrong;
        }
        if (looked == 0)
            std::printf("  [skip] no saved frames of the client here\n");
        else
            check(wrong == 0, "on " + std::to_string(looked) + " saved frames of the client, "
                              "nothing is taken from the hotbar band or the icon column");
    }
    // Edited by hand on the full screen, the tuning is kept in hunt.ini's
    // [Detect]; left alone, it stays following the client's size.
    {
        const std::string path = temp_file("gobot_detect_test.ini");
        { std::ofstream(path) << "# somebody's hunt.ini\n[Hunt]\nreach=150\n"; }
        DetectorTuning edited = DetectorTuning::defaults();
        edited.red_min = 160;
        edited.area = ScreenRect{ 10, 120, 900, 500 };
        check(edited.save(path), "the tuning saves into [Detect]");
        IniFile ini;
        DetectorTuning back = DetectorTuning::defaults();
        check(ini.load(path), "and the file reads");
        back.load(ini);
        check(back.red_min == 160 && back.area.x == 10 && back.area.w == 900 &&
                  !back.exclude.valid() && !back.exclude2.valid() && !back.automatic() &&
                  ini.get_int("Hunt", "reach", 0) == 150,
              "an edited box comes back as written, the rectangles left alone still follow "
              "the client, and the rest of the file is kept");
        check(DetectorTuning::defaults().save(path), "a reset tuning saves too");
        IniFile again;
        DetectorTuning reset = edited;
        again.load(path);
        reset.load(again);
        check(reset.automatic() && again.get("Detect", "area") == "auto",
              "and reads back as following the client's size");
        std::remove(path.c_str());
    }
}

// --------------------------------------------------------- camera tracking

/// Scenery: enough detail that one alignment is clearly better than another,
/// which is the whole premise. `ox` slides it, standing in for the camera.
void paint_scenery(Frame& f, int ox, int oy) {
    for (int y = 0; y < f.height; ++y) {
        for (int x = 0; x < f.width; ++x) {
            const int wx = x + ox, wy = y + oy;
            // A deterministic hash, so the same world point is the same colour
            // whichever frame it appears in.
            unsigned h = static_cast<unsigned>(wx * 374761393 + wy * 668265263);
            h = (h ^ (h >> 13)) * 1274126177u;
            const std::uint8_t v = static_cast<std::uint8_t>(40 + (h >> 24) % 180);
            put_px(f, x, y, v, static_cast<std::uint8_t>(v / 2 + 30),
                   static_cast<std::uint8_t>(200 - v / 2));
        }
    }
}

void test_camera_tracker() {
    section("Camera tracking");

    const ScreenRect area{ 100, 100, 600, 400 };

    const auto shift_between = [&](int ox, int oy, int& dx, int& dy, float& conf) {
        Frame a = make_frame(1024, 768), b = make_frame(1024, 768);
        paint_scenery(a, 0, 0);
        paint_scenery(b, ox, oy);

        std::vector<std::uint8_t> prev(static_cast<std::size_t>(area.w) * area.h);
        for (int y = 0; y < area.h; ++y)
            for (int x = 0; x < area.w; ++x) {
                std::uint8_t r, g, bl;
                a.get(area.x + x, area.y + y, r, g, bl);
                prev[static_cast<std::size_t>(y) * area.w + x] =
                    static_cast<std::uint8_t>((r * 77 + g * 151 + bl * 28) >> 8);
            }
        return CameraTracker::estimate_shift(prev, area.w, area.h, b, area, 0, 0, dx, dy, conf);
    };

    int dx = 0, dy = 0;
    float conf = 0.0f;

    check(shift_between(0, 0, dx, dy, conf), "a still view matches");
    check(dx == 0 && dy == 0, "and reports no movement");
    check(conf > CameraTracker::kMinConfidence, "with confidence");

    check(shift_between(17, 0, dx, dy, conf), "a view scrolled right matches");
    check(dx == 17 && dy == 0, "and the distance is exact");

    check(shift_between(-23, 9, dx, dy, conf), "a diagonal scroll matches");
    check(dx == -23 && dy == 9, "and both distances are exact");

    // Detail is what makes one alignment better than another. A flat wall is
    // equally good everywhere, and reporting the best of those as a position
    // would send the bot walking off on nothing.
    {
        Frame flat = make_frame(1024, 768);
        for (int y = 0; y < flat.height; ++y)
            for (int x = 0; x < flat.width; ++x) put_px(flat, x, y, 70, 70, 74);
        std::vector<std::uint8_t> prev(static_cast<std::size_t>(area.w) * area.h, 70);
        check(!CameraTracker::estimate_shift(prev, area.w, area.h, flat, area, 0, 0, dx, dy, conf),
              "a featureless view reports nothing rather than guessing");
    }

    // Accumulated over several frames, which is what makes a position.
    {
        CameraTracker t;
        t.set_play_area(area);
        int ox = 0;
        const int steps[] = { 0, 12, 12, 12, -30, 5, 5 };
        for (int s : steps) {
            ox += s;
            Frame f = make_frame(1024, 768);
            paint_scenery(f, ox, 0);
            t.update(f);
        }
        const auto fx = t.fix();
        check(fx.tracking, "still tracking after a run of frames");
        check(static_cast<int>(std::lround(fx.x)) == ox,
              "the camera position is the sum of the steps, with no drift");
        check(fx.updates == 6, "one update per frame after the first");

        // A world position survives the view moving: the same place in the
        // level, seen from two different camera positions, is the same number.
        double wx = 0.0, wy = 0.0;
        t.to_world(300 - ox, 200, wx, wy);
        check(std::fabs(wx - 300.0) < 1.5, "a fixed point in the level keeps its position");

        int sx = 0, sy = 0;
        t.to_screen(wx, wy, sx, sy);
        check(std::abs(sx - (300 - ox)) <= 1, "and converts back to where it is on screen");

        t.reset();
        check(!t.fix().tracking, "reset forgets where we were");
    }
}

// -------------------------------------------------------- finding fields

/// A packet stream shaped like the real one: several creatures sharing an
/// opcode, each with its own id, and only one of them moving the way the
/// screen said we did.
void test_field_finder() {
    section("Finding packet fields");

    FieldFinder finder;

    // Payload: [0..3] id, [4..5] x, [6..7] y, [8..9] something unrelated.
    const auto make = [](std::uint32_t id, int x, int y, int junk) {
        GamePacket p;
        p.opcode = 0x0038;
        p.from_server = true;
        p.payload.assign(32, 0);
        for (int i = 0; i < 4; ++i)
            p.payload[i] = static_cast<std::uint8_t>(id >> (i * 8));
        p.payload[4] = static_cast<std::uint8_t>(x);
        p.payload[5] = static_cast<std::uint8_t>(x >> 8);
        p.payload[6] = static_cast<std::uint8_t>(y);
        p.payload[7] = static_cast<std::uint8_t>(y >> 8);
        p.payload[8] = static_cast<std::uint8_t>(junk);
        p.payload[9] = static_cast<std::uint8_t>(junk >> 8);
        return p;
    };

    // The character walks; the server reports it at four times the screen
    // scale with its origin a long way off. Three other creatures mill about.
    std::mt19937 rng(1234);
    for (int t = 0; t < 200; ++t) {
        const double screen_x = 500.0 + 9.0 * t;
        const double screen_y = 640.0 + 40.0 * std::sin(t * 0.1);
        finder.set_truth(screen_x, screen_y, 100.0f, 50.0f, true);

        finder.on_packet(make(0xAABBCCDD,
                              static_cast<int>(12000 + 4.0 * screen_x),
                              static_cast<int>(3000 + 4.0 * screen_y),
                              static_cast<int>(rng() % 60000)));

        for (std::uint32_t other = 1; other <= 3; ++other)
            finder.on_packet(make(0x1000 + other,
                                  static_cast<int>(rng() % 20000),
                                  static_cast<int>(rng() % 2000),
                                  static_cast<int>(rng() % 60000)));
    }

    const auto summary = finder.summarise();
    check(!summary.empty(), "summarises what it captured");
    if (!summary.empty()) {
        check(summary[0].count == 800, "counts every packet");
        check(summary[0].entities == 4, "separates the four creatures");
    }

    const auto found = finder.analyse(20, 0.9);
    check(!found.empty(), "finds something");

    bool got_x = false, got_y = false;
    for (const auto& c : found) {
        if (!c.grouped || c.id_value != 0xAABBCCDD) continue;
        if (c.reading.offset == 4 && c.reading.width == 2 && !c.reading.big_endian &&
            c.quantity == FieldFinder::Quantity::X) {
            got_x = true;
            check(std::fabs(c.scale - 4.0) < 0.1, "recovers the x scale");
            check(c.correlation > 0.99, "x correlates almost perfectly");
        }
        if (c.reading.offset == 6 && c.reading.width == 2 && !c.reading.big_endian &&
            c.quantity == FieldFinder::Quantity::Y) {
            got_y = true;
            check(std::fabs(c.scale - 4.0) < 0.2, "recovers the y scale");
        }
    }
    check(got_x, "finds the x field, attributed to the right creature");
    check(got_y, "finds the y field");

    // The decisive property. Another creature's position must not be claimed
    // as ours, or the bot would steer by a monster.
    bool impostor = false;
    for (const auto& c : found)
        if (c.quantity == FieldFinder::Quantity::X && c.grouped &&
            c.id_value != 0xAABBCCDD && std::abs(c.correlation) > 0.95)
            impostor = true;
    check(!impostor, "no other creature is matched to our movement");

    // Health never changed, so nothing can correlate with it. A constant is
    // not a match however perfectly it fits -- and getting this wrong is how
    // the first run came back full of NaNs sorted to the top.
    bool claimed_hp = false;
    for (const auto& c : found)
        if (c.quantity == FieldFinder::Quantity::Hp) claimed_hp = true;
    check(!claimed_hp, "a quantity that never varied produces no matches");

    for (const auto& c : found)
        check(std::isfinite(c.correlation) && std::isfinite(c.scale),
              "every reported number is finite");
}

// ----------------------------------------------------------- map identity

void test_minimap_tracker() {
    section("Map identity");

    const auto layout = MinimapTracker::default_layout(1024, 768);
    check(layout.valid(), "default layout is usable");
    check(layout.body.right() <= 1024, "minimap sits inside the window");
    check(layout.title.bottom() <= layout.body.y, "the name bar is above the picture");

    // Two different maps, and the same map a moment later with its weather
    // moved on. The first pair has to come out different and the second pair
    // the same, or either the map never changes or it changes constantly.
    const auto paint_map = [&](Frame& f, int seed, int noise) {
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x)
                put_px(f, x, y, 20, 24, 30);
        for (int i = 0; i < 40; ++i) {
            const int bx = layout.body.x + ((i * 37 + seed * 53) % (layout.body.w - 20));
            const int by = layout.body.y + ((i * 23 + seed * 17) % (layout.body.h - 10));
            for (int y = by; y < by + 9; ++y)
                for (int x = bx; x < bx + 19; ++x)
                    put_px(f, x, y, static_cast<std::uint8_t>(60 + (i * 7 + seed) % 180),
                           150, 90);
        }
        // Weather: a scatter of bright specks, a couple of percent of the area.
        for (int i = 0; i < noise; ++i) {
            const int bx = layout.body.x + (i * 991 + noise * 7) % layout.body.w;
            const int by = layout.body.y + (i * 131 + noise * 3) % layout.body.h;
            put_px(f, bx, by, 240, 240, 255);
        }
    };

    Frame a = make_frame(1024, 768), a2 = make_frame(1024, 768), b = make_frame(1024, 768);
    paint_map(a,  1, 0);
    paint_map(a2, 1, 400);      // same map, different weather
    paint_map(b,  2, 0);

    const auto fingerprint_of = [&](const Frame& f) {
        MinimapTracker t;
        t.set_layout(layout);
        for (int i = 0; i < 8; ++i) t.update(f);
        return t.fingerprint();
    };

    const std::uint64_t fa = fingerprint_of(a);
    const std::uint64_t fa2 = fingerprint_of(a2);
    const std::uint64_t fb = fingerprint_of(b);

    check(fa != 0, "a fingerprint is produced");
    check(MinimapTracker::distance(fa, fa2) <= MinimapTracker::kSameMap,
          "weather does not make it a different map");
    check(MinimapTracker::distance(fa, fb) > MinimapTracker::kSameMap,
          "a different map reads as different");

    // It must not announce a change before the picture has settled: the panel
    // churns through intermediate states while a map loads.
    MinimapTracker t;
    t.set_layout(layout);
    t.update(a);
    check(t.fingerprint() == 0, "says nothing after one frame");
    for (int i = 0; i < 8; ++i) t.update(a);
    check(t.fingerprint() == fa, "settles on the map once it holds steady");
    check(t.take_changed(), "reports the change once");
    check(!t.take_changed(), "and not again");

    // Names, remembered against the picture.
    MapBook book;
    book.set_name(fa, "Thanh Am Coc");
    book.set_map_file(fa, "t14_s0");
    check(book.name_for(fa) == "Thanh Am Coc", "remembers a name");
    check(book.name_for(fa2) == "Thanh Am Coc",
          "recognises the same map through its weather");
    check(book.name_for(fb).empty(), "does not claim to know a different map");
    check(book.size() == 1, "one entry");

    const std::string path = temp_file("gobot_maps_test.ini");
    check(book.save(path), "saves");
    MapBook back;
    check(back.load(path), "loads");
    check(back.name_for(fa) == "Thanh Am Coc", "name survives");
    check(back.map_file_for(fa) == "t14_s0", "linked map file survives");
    std::remove(path.c_str());
}

// ----------------------------------------------------------------- hunter

void test_hunter_config() {
    section("Hunter config");

    HunterConfig cfg;
    check(cfg.require_focus, "focus required by default");
    check(cfg.focus_on_start, "starting brings the game to the front by default");
    check(cfg.reach > 0 && cfg.height_tolerance > 0, "reach and height set");

    cfg.focus_on_start = false;
    cfg.attack_key = 'X';
    cfg.reach = 211;
    cfg.height_tolerance = 44;
    cfg.attack_interval_ms = 950;
    cfg.give_up_ms = 7000;
    cfg.jump_to_reach = false;
    cfg.toggle_key = 119;

    // Picking up is its own switch, so turning it off keeps the key.
    check(!cfg.loot && cfg.loot_key == 0 && !cfg.loots(), "picks nothing up by default");
    cfg.loot_key = 'A';
    check(!cfg.loots(), "a key alone does not switch it on");
    cfg.loot = true;
    check(cfg.loots(), "the switch and a key do");
    cfg.loot_interval_ms = 1700;
    check(!cfg.stay_put, "walks to monsters by default");
    cfg.stay_put = true;

    const std::string path = temp_file("gobot_hunt_test.ini");
    check(cfg.save(path), "saves");

    HunterConfig back;
    check(back.load(path), "loads");
    check(back.attack_key == 'X', "attack key survives");
    check(back.reach == 211, "reach survives");
    check(back.height_tolerance == 44, "height tolerance survives");
    check(back.attack_interval_ms == 950, "swing interval survives");
    check(back.give_up_ms == 7000, "give-up survives");
    check(!back.jump_to_reach, "jump flag survives");
    check(back.toggle_key == 119, "toggle key survives");
    check(back.loot && back.loot_key == 'A' && back.loot_interval_ms == 1700,
          "picking up survives");
    check(back.stay_put, "standing still survives");
    // Off has to stick, or hunting grabs the foreground from someone who
    // explicitly asked it not to.
    check(!back.focus_on_start, "focus-on-start survives being switched off");
    std::remove(path.c_str());
}

/// The hunter drives real input, so what is checked here is what can be
/// checked without a window to type into: that it starts and stops cleanly and
/// that it refuses to act when it cannot see the character.
void test_hunter() {
    section("Hunter");

    InputManager input;
    ExecutionCoordinator coord;
    MobDetector detector;
    Hunter hunter(input, coord, detector);

    HunterConfig cfg;
    cfg.require_focus = true;
    hunter.set_config(cfg);

    check(!hunter.running(), "starts stopped");
    hunter.start();
    check(hunter.running(), "start() runs it");

    // Waited for, up to 5 s, rather than read after a fixed sleep: on a
    // loaded machine its thread may not have had a turn in 220 ms.
    const auto idle = [&] {
        const auto s = hunter.status();
        return s.state == Hunter::State::Unfocused || s.state == Hunter::State::NoPlayer;
    };
    for (int i = 0; i < 250 && !idle(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto st = hunter.status();
    check(!st.has_target, "no target without a detector running");
    check(st.state == Hunter::State::Unfocused || st.state == Hunter::State::NoPlayer,
          "says why it is doing nothing");
    check(st.attacks == 0, "pressed nothing");

    hunter.stop();
    check(!hunter.running(), "stop() halts it");
    hunter.stop();
    check(true, "double stop is safe");

    Hunter other(input, coord, detector);
    other.stop();
    check(true, "stopping a never-started hunter is safe");

    hunter.start();
    check(hunter.running(), "restarts after a stop");
    hunter.stop();
}

// -------------------------------------------------------------------- bag

namespace {

/// A frame the colour of an empty bag slot.
Frame bag_scene(int w, int h, std::uint8_t grey = 14) {
    Frame f;
    f.width = w;
    f.height = h;
    f.pixels.assign(static_cast<std::size_t>(w) * h * 4, 255);
    for (std::size_t i = 0; i < f.pixels.size(); i += 4)
        f.pixels[i] = f.pixels[i + 1] = f.pixels[i + 2] = grey;
    return f;
}

void put_px(Frame& f, int x, int y, int r, int g, int b) {
    if (x < 0 || y < 0 || x >= f.width || y >= f.height) return;
    const std::size_t i = (static_cast<std::size_t>(y) * f.width + x) * 4;
    f.pixels[i] = static_cast<std::uint8_t>(b);
    f.pixels[i + 1] = static_cast<std::uint8_t>(g);
    f.pixels[i + 2] = static_cast<std::uint8_t>(r);
}

/// Something colourful in a slot, inside its edge.
void paint_item(Frame& f, const ScreenRect& s, int seed) {
    for (int y = 4; y < s.h - 4; ++y)
        for (int x = 4; x < s.w - 4; ++x) {
            const int r = 128 + static_cast<int>(100 * std::sin(x * 0.5 + seed));
            const int g = 128 + static_cast<int>(100 * std::cos(y * 0.4 + seed * 2));
            const int b = 40 + (x * y * (seed + 3)) % 200;
            put_px(f, s.x + x, s.y + y, r, g, b);
        }
}

/// A stack count in the corner: white marks that depend on the number.
void paint_count(Frame& f, const ScreenRect& s, int value) {
    for (int i = 0; i < 12; ++i)
        if ((value >> (i % 7)) & 1)
            for (int y = 2; y < 9; ++y) put_px(f, s.x + 2 + i, s.y + y, 250, 250, 250);
}

void blit(Frame& f, const Frame& t, int at_x, int at_y) {
    for (int y = 0; y < t.height; ++y)
        for (int x = 0; x < t.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * t.width + x) * 4;
            if (t.pixels[i + 3] < 128) continue;
            put_px(f, at_x + x, at_y + y, t.pixels[i + 2], t.pixels[i + 1], t.pixels[i]);
        }
}

void fill(Frame& f, const ScreenRect& r, int red, int green, int blue) {
    for (int y = r.y; y < r.bottom(); ++y)
        for (int x = r.x; x < r.right(); ++x) put_px(f, x, y, red, green, blue);
}

/// How far the client prints a digit after the one before: its digits are 5
/// pixels wide and 6 apart, a 1 included (3 pixels of ink at the left of its
/// cell). Measured after a 1 on every saved bag and hotbar slot of the live
/// client.
constexpr int kDigitStep = 6;

/// A number in the client's own digits, white with a dark outline, the way
/// the game prints stack counts and quantity boxes.
void print_number(Frame& f, int x, int y, const std::string& text, const std::string& font_dir) {
    for (const char c : text) {
        Frame g;
        load_png(font_dir + "\\lower\\" + std::string(1, c) + ".png", g);
        for (int pass = 0; pass < 2; ++pass)
            for (int gy = 0; gy < g.height; ++gy)
                for (int gx = 0; gx < g.width; ++gx) {
                    std::uint8_t r, gg, b;
                    g.get(gx, gy, r, gg, b);
                    if ((r + gg + b) / 3 <= 128) continue;
                    if (pass == 0) {
                        for (int oy = -1; oy <= 1; ++oy)
                            for (int ox = -1; ox <= 1; ++ox)
                                put_px(f, x + gx + ox, y + gy + oy, 10, 10, 10);
                    } else {
                        put_px(f, x + gx, y + gy, 250, 250, 250);
                    }
                }
        x += kDigitStep;
    }
}

/// Where a quantity box's caret stands with `chars` digits in the box, as on
/// the live client: five pixels in for an empty box and six more for each
/// digit -- over the last column of a 2, clear of a 1.
int caret_x(const ScreenRect& box, std::size_t chars) {
    return box.x + 5 + 6 * static_cast<int>(chars);
}

/// The caret there: a yellow bar two pixels wide from two pixels below the
/// box's top to past its bottom, brighter where a stroke is under it.
void draw_caret(Frame& f, const ScreenRect& box, int x0) {
    for (int y = box.y + 2; y < box.y + 17; ++y)
        for (int x = x0; x < x0 + 2; ++x) {
            if (x < 0 || y < 0 || x >= f.width || y >= f.height) continue;
            std::uint8_t r, g, b;
            f.get(x, y, r, g, b);
            if ((r + g + b) / 3 > 175) put_px(f, x, y, 255, 255, 55);
            else                       put_px(f, x, y, 214, 214, 16);
        }
}

/// The frames the live client draws round its slots: one flat line down the
/// right side of each, which the row with the buy-more-room button has none of.
void frame_slots(Frame& f, const BagView& v, const BagLayout& l, int rows) {
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < l.cols; ++c) {
            const ScreenRect s = v.slot(l, c, r);
            for (int y = s.y; y < s.y + l.cell; ++y) put_px(f, s.x + l.cell + 1, y, 82, 82, 82);
        }
}

/// The scroll bar as the live client draws it: lighter ends, a dark middle,
/// and a white thumb as long and as far down as the rows say.
void draw_track(Frame& f, const BagView& v, const BagLayout& l, int first_row, int total_rows) {
    const int x = v.strip.x + l.scroll_up_dx;
    const int top = v.strip.y + l.track_top_dy, bottom = v.strip.y + l.track_bottom_dy;
    const int span = bottom - top + 1;
    for (int y = top; y <= bottom; ++y) put_px(f, x, y, 65, 65, 65);
    put_px(f, x, top, 249, 249, 249);
    put_px(f, x, bottom, 247, 247, 247);
    const int len = static_cast<int>(std::lround(static_cast<double>(span) * l.rows / total_rows));
    const int at = total_rows > l.rows
        ? static_cast<int>(std::lround(static_cast<double>(span - len) * first_row /
                                       (total_rows - l.rows)))
        : 0;
    for (int y = top + at; y < top + at + len && y <= bottom; ++y) put_px(f, x, y, 255, 255, 255);
}

} // namespace

void test_bag() {
    section("Bag and trash");

    // Where the slots are, from the strip.
    BagLayout l;
    BagView v;
    v.found = true;
    v.strip = ScreenRect{ 100, 200, 246, 19 };
    const auto slots = v.slots(l);
    check(slots.size() == 24, "six slots by four");
    check(slots[0].x == 101 && slots[0].y == 225 && slots[0].w == 34 && slots[0].h == 34,
          "the first just under the strip");
    check(slots[7].x == 101 + 37 && slots[7].y == 225 + 37, "the rest on a 37 pixel pitch");
    int tx = 0, ty = 0;
    v.tab_point(l, 2, tx, ty);
    check(tx == 100 + 2 * 37 + 18 && ty == 209, "a tab is clicked in its middle");
    check(std::string(bag_tab_name(2)) == "Tiêu Dùng", "and the third is Tiêu Dùng");

    // Empty or not.
    Frame f = bag_scene(500, 450);
    paint_item(f, slots[3], 1);
    check(bag::is_empty(f, slots[0], l), "a dark slot is empty");
    check(!bag::is_empty(f, slots[3], l), "a slot with something in it is not");

    // The corner with the count and the edge with the highlight are left out.
    const Frame mask = bag::masked(bag::cut(f, slots[3]), l);
    const auto alpha = [&](int x, int y) {
        return mask.pixels[(static_cast<std::size_t>(y) * mask.width + x) * 4 + 3];
    };
    check(alpha(0, 0) == 0 && alpha(33, 20) == 0, "the edge is see-through");
    check(alpha(6, 5) == 0, "and so is the count corner");
    check(alpha(20, 20) == 255, "but the item itself is not");

    // Recognising a listed item.
    TrashList list;
    list.add_picture(bag::cut(f, slots[3]), TrashList::Action::Delete, l);
    check(list.match(f, slots[3], 0.90f) == 0, "a listed item is known where it was cut from");

    Frame g = bag_scene(500, 450);
    paint_item(g, slots[10], 1);
    paint_count(g, slots[10], 67);
    // Matched first and reported after: the order a function's arguments are
    // worked out in is not fixed, and the score would be read before it was set.
    float score = 0.0f;
    const int same_item = list.match(g, slots[10], 0.90f, &score);
    check(same_item == 0,
          "and in another slot, with a different count -- score " + std::to_string(score));
    paint_item(g, slots[11], 2);
    check(list.match(g, slots[11], 0.90f) < 0, "a different item is not");
    check(list.match(g, slots[0], 0.90f) < 0, "nor an empty slot");

    // The drawing decides, not the colour: the bag draws fragments gold,
    // orange or red as they pulse, and the player's rule is that the same
    // drawing is the same item whatever its colour. Here the item redrawn in
    // one orange-red ramp, its brightness kept, and twenty levels lighter.
    {
        Frame h = bag_scene(500, 450);
        for (int y = 4; y < slots[13].h - 4; ++y)
            for (int x = 4; x < slots[13].w - 4; ++x) {
                std::uint8_t r, gg, b;
                f.get(slots[3].x + x, slots[3].y + y, r, gg, b);
                const int grey = (r * 77 + gg * 151 + b * 28) >> 8;
                put_px(h, slots[13].x + x, slots[13].y + y, std::min(255, grey + 40),
                       grey * 45 / 100, grey * 15 / 100);
                put_px(h, slots[14].x + x, slots[14].y + y, std::min(255, r + 20),
                       std::min(255, gg + 20), std::min(255, b + 20));
            }
        float red = 0.0f, lighter = 0.0f;
        const int as_red = list.match(h, slots[13], 0.90f, &red);
        const int as_lighter = list.match(h, slots[14], 0.90f, &lighter);
        check(as_red == 0, "the same drawing in another colour is the same item -- score " +
                               std::to_string(red));
        check(as_lighter == 0, "and so is it twenty levels lighter -- score " +
                                   std::to_string(lighter));
    }

    // While an item waits in the bin, its slot shows it enlarged. That copy
    // is never to be taken for an item of its own and dragged.
    {
        Frame h = bag_scene(500, 450);
        const ScreenRect at = slots[13];
        for (int y = 0; y < at.h; ++y)
            for (int x = 0; x < at.w; ++x) {
                // 1.35 times the size, about the middle of the slot.
                const int sx = slots[3].x + 17 + static_cast<int>(std::lround((x - 17) / 1.35));
                const int sy = slots[3].y + 17 + static_cast<int>(std::lround((y - 17) / 1.35));
                std::uint8_t r, gg, b;
                f.get(sx, sy, r, gg, b);
                put_px(h, at.x + x, at.y + y, r, gg, b);
            }
        float enlarged = 0.0f;
        const int taken = list.match(h, at, 0.90f, &enlarged);
        check(taken < 0, "nor the enlarged copy of it a slot shows while it waits in the bin -- "
                         "score " + std::to_string(enlarged));
    }

    // Something unlisted, and drawn differently, that looks as much like the
    // slot is as good an answer, and the slot is left alone.
    {
        TrashList rivalled;
        rivalled.add_picture(bag::cut(f, slots[3]), TrashList::Action::Delete, l);
        TrashList other;
        other.add_picture(bag::cut(f, slots[3]), TrashList::Action::Delete, l);
        other.items[0].id = "spend/craft/0000001";
        rivalled.rivals.push_back(other.items[0]);
        const TrashList::Verdict k = rivalled.judge(f, slots[3], 0.90f);
        check(k.item < 0 && k.doubt == TrashList::Verdict::Doubt::Rival &&
                  k.rival == "spend/craft/0000001" && k.rival_score > 0.9f,
              "a listed picture that ties with an unlisted one is left alone");
        // The same drawing is the same thing, by the player's rule, and is no
        // rival: a red piece photographed is the gold piece's icon too.
        rivalled.items[0].kin = { "spend/craft/0000001" };
        check(rivalled.judge(f, slots[3], 0.90f).item == 0,
              "but not when that one is the same drawing as the listed one");
        rivalled.items[0].kin.clear();
        rivalled.rivals.clear();
        check(rivalled.judge(f, slots[3], 0.90f).item == 0, "and taken once that one is gone");
    }

    // Telling a page from the next by its slots. Every slot has to agree: a
    // page with two items and the empty page below it differ in two slots.
    {
        Frame a = bag_scene(500, 450), b = bag_scene(500, 450);
        paint_item(a, slots[2], 3);
        paint_item(a, slots[7], 5);
        check(!bag::same_page(a, v, b, v, l), "a page with two items is not the empty page");
        check(bag::same_page(a, v, a, v, l), "a page is the same page as itself");
        Frame c = a;
        paint_item(c, slots[20], 2);
        check(!bag::same_page(a, v, c, v, l), "nor is it once one more slot has something");
    }

    // A slot still holding what it held.
    {
        const Frame before = bag::cut(f, slots[3]);
        check(bag::alike(before, bag::cut(f, slots[3]), l) > 0.99f, "a slot is alike to itself");
        check(bag::alike(before, bag::cut(g, slots[11]), l) < 0.80f,
              "and not to a slot with another item in it");
    }

    // The list on disk.
    {
        const std::string dir = temp_file("gobot_trash_test");
        TrashList on_disk;
        const std::string file =
            on_disk.add(dir, bag::cut(f, slots[3]), TrashList::Action::Drop, l);
        check(!file.empty() && GetFileAttributesA(file.c_str()) != INVALID_FILE_ATTRIBUTES,
              "a picture is saved");
        TrashList back;
        check(back.load(dir, l) && back.items.size() == 1 &&
                  back.items[0].action == TrashList::Action::Drop,
              "and read back, in the folder for its action");
        check(back.match(f, slots[3], 0.90f) == 0, "and still recognised");
        check(back.remove(0) && back.items.empty() &&
                  GetFileAttributesA(file.c_str()) == INVALID_FILE_ATTRIBUTES,
              "removing it deletes the file");
        RemoveDirectoryA((dir + "\\drop").c_str());
        RemoveDirectoryA(dir.c_str());
        TrashList none;
        check(none.load(temp_file("gobot_trash_nowhere"), l) && none.items.empty(),
              "a folder that is not there is an empty list");
    }

    // The settings. Their defaults, and the file beside the program -- which
    // is the player's own once they have ticked anything, so only what nobody
    // changes from the screen is held to its shipped value.
    {
        const CleanSettings d;
        check(!d.enabled, "trash cleaning is off until somebody turns it on");
        check(d.bag_key == 'I' && d.tabs[0] && !d.tabs[4] && d.min_score >= 0.89f &&
                  d.catalog_score >= 0.89f,
              "with the bag on I, the pet tab left alone and strict matches");
        CleanSettings s;
        check(s.load(exe_relative("configs/trash.ini")), "the trash settings load");
        check(s.layout.pitch == 37 && s.layout.grid_dy == 25, "with the measured layout");
        check(s.layout.track_top_dy == 39 && s.layout.track_bottom_dy == 155,
              "and the scroll bar's track, measured on the live client");

        // A file caught half written -- emptied, about to be filled again by
        // the trash tab -- is not the defaults. It is refused, and what was
        // read before stands. The file has the fragments_loose key an earlier
        // GoBot wrote: colour counts for nothing now, and it is passed over.
        const std::string path = temp_file("gobot_trash_half.ini");
        {
            std::ofstream out(path);
            out << "[Trash]\ntab_equip1=false\ntab_use=true\nfragments_loose=true\n"
                   "min_score=0.5\ncatalog_score=0.93\n"
                   "[Bag]\ntrack_top_dy=40\n[Items]\ndelete=spend/fragment/1103023\n";
        }
        CleanSettings kept;
        check(kept.load(path) && !kept.tabs[0] && kept.tabs[2] &&
                  kept.layout.track_top_dy == 40 && kept.delete_ids.size() == 1,
              "the settings read back, tabs, track and all, an old fragments_loose key and all");
        check(kept.min_score >= 0.85f && std::abs(kept.catalog_score - 0.93f) < 1e-4f,
              "with a score too low to tell drawings apart raised to 0.85 -- " +
                  std::to_string(kept.min_score));
        {
            std::ofstream out(path, std::ios::trunc);
            out << "# nothing yet\n";
        }
        check(!kept.load(path) && !kept.tabs[0] && kept.delete_ids.size() == 1,
              "a file with no [Trash] in it is refused, and the last reading stands");
        std::remove(path.c_str());
    }

    // Finding the bag, and a look-only pass over it, with the game's pictures.
    TemplateStore store;
    const bool have = store.load_best(exe_relative("configs/templates.ini")) &&
                      store.get("grabbed/inventory") && store.get("inven/0/trash_can");
    if (!have) {
        std::printf("  [skip] the bag pictures are not installed here\n");
        return;
    }
    check(l.bin_dx == 228 && l.bin_dy == 178, "the bin is measured from the live client");
    Frame scene = bag_scene(1024, 768, 60);
    for (int y = 300; y < 300 + 220; ++y)            // the bag's dark panel
        for (int x = 290; x < 290 + 272; ++x) put_px(scene, x, y, 20, 22, 30);
    blit(scene, *store.get("grabbed/inventory"), 300, 300);
    blit(scene, *store.get("inven/0/trash_can"), 300 + l.bin_dx, 300 + l.bin_dy);

    const BagView found = bag::find(scene, store, l);
    check(found.found && found.strip.x == 300 && found.strip.y == 300,
          "the bag is found by its tab strip");
    check(found.bin_empty && std::abs(found.trash.x - (300 + l.bin_dx)) <= 1 &&
              std::abs(found.trash.y - (300 + l.bin_dy)) <= 1,
          "and its trash bin, empty, in the corner");
    check(!found.scroll_known() && found.slots(l).size() == 24,
          "with no scroll bar to read, and all four rows taken for slots");

    // Which rows are slots, and where the scroll bar says the page is.
    {
        Frame drawn = scene;
        frame_slots(drawn, found, l, 3);
        for (int c = 0; c < l.cols; ++c) {         // the last row: one panel, a button in it
            const ScreenRect s = found.slot(l, c, 3);
            for (int y = 0; y < l.cell; ++y)
                for (int x = 0; x <= l.pitch; ++x) put_px(drawn, s.x + x, s.y + y, 0, 0, 4 * y / 3);
        }
        fill(drawn, ScreenRect{ found.slot(l, 1, 3).x + 10, found.slot(l, 0, 3).y + 9, 150, 16 },
             200, 205, 215);
        draw_track(drawn, found, l, 6, 10);
        const BagView bottom = bag::find(drawn, store, l);
        check(bottom.slot_rows == 3 && bottom.slots(l).size() == 18,
              "the row with the buy-more-room button is not taken for slots");
        check(bottom.scroll_known() && bottom.first_row == 6 && bottom.total_rows == 10 &&
                  bottom.at_bottom(l) && !bottom.at_top(),
              "the thumb at the bottom: rows 7 to 10 of 10 -- read " +
                  std::to_string(bottom.first_row) + " of " + std::to_string(bottom.total_rows));

        Frame mid = scene;
        frame_slots(mid, found, l, 4);
        bool all = true;
        for (const int row : { 0, 2, 3, 5 }) {
            draw_track(mid, found, l, row, 10);
            const BagView at = bag::find(mid, store, l);
            all = all && at.slot_rows == 4 && at.first_row == row && at.total_rows == 10 &&
                  at.at_top() == (row == 0) && !at.at_bottom(l);
        }
        check(all, "the thumb part way down says which row is at the top");

        Frame covered = mid;
        fill(covered, ScreenRect{ found.strip.x + l.scroll_up_dx - 3, found.strip.y + 50, 60, 30 },
             216, 216, 216);
        check(!bag::find(covered, store, l).scroll_known(),
              "and a dialog over the scroll bar makes it unreadable, not wrong");
    }

    // With an item waiting in the bin the can is gone -- and a can-like
    // picture elsewhere in the bag must not be taken for the bin. That is what
    // sent drags into the middle of the bag on the first live try.
    {
        Frame loaded = scene;
        const ScreenRect bin{ 300 + l.bin_dx - 4, 300 + l.bin_dy - 2, 24, 24 };
        paint_item(loaded, bin, 6);
        const auto in_bag = found.slots(l)[8];
        blit(loaded, *store.get("inven/0/trash_can"), in_bag.x + 9, in_bag.y + 6);
        const BagView busy = bag::find(loaded, store, l);
        check(busy.found && !busy.bin_empty, "a bin with something in it is not empty");
        check(busy.trash.x == 300 + l.bin_dx && busy.trash.y == 300 + l.bin_dy,
              "and is still where the layout puts it, not where a can-like item is");
    }

    const auto bag_slots = found.slots(l);
    paint_item(scene, bag_slots[5], 1);
    paint_item(scene, bag_slots[9], 4);
    TrashList wanted;
    wanted.add_picture(bag::cut(scene, bag_slots[5]), TrashList::Action::Delete, l);

    InputManager no_window;          // no target: nothing it does reaches anything
    BagCleaner cleaner(no_window, store, [&](Frame& out) { out = scene; return true; });
    CleanSettings cs;
    cs.tabs[0] = true;
    cs.tabs[1] = cs.tabs[2] = cs.tabs[3] = cs.tabs[4] = false;
    cleaner.set_settings(cs);
    cleaner.set_trash(&wanted);
    cleaner.set_dry_run(true);
    cleaner.set_skip_focus_check(true);
    const auto res = cleaner.run();
    check(res.ran, "a look-only pass searches the bag");
    check(res.matched == 1, "finds the one listed item and not the other -- found " +
                            std::to_string(res.matched));
    check(res.deleted == 0 && res.dropped == 0 && res.failed == 0, "and throws nothing away");

    // Nothing is judged while the bin holds something: the slot its item came
    // from shows an enlarged copy of it, which is no item to drag. Looking
    // only says what is in the way, and touches nothing.
    {
        Frame waiting = scene;
        paint_item(waiting, ScreenRect{ 300 + l.bin_dx - 4, 300 + l.bin_dy - 2, 24, 24 }, 6);
        BagCleaner looking(no_window, store, [&](Frame& out) { out = waiting; return true; });
        looking.set_settings(cs);
        looking.set_trash(&wanted);
        looking.set_dry_run(true);
        looking.set_skip_focus_check(true);
        const auto r = looking.run();
        check(r.matched == 0 && !r.trouble.empty() && !r.stuck,
              "with something waiting in the bin no slot is judged -- " + r.trouble);
    }

    TrashList empty_list;
    cleaner.set_trash(&empty_list);
    check(!cleaner.run().ran, "with nothing listed, it does not open the bag at all");

    // With the game behind another window it does not reach for the mouse.
    {
        BagCleaner behind(no_window, store, [&](Frame& out) { out = scene; return true; });
        behind.set_settings(cs);
        behind.set_trash(&wanted);
        behind.set_dry_run(true);
        const auto r = behind.run();
        check(r.matched == 0 && !r.trouble.empty(),
              "and with the game not in front it clicks nothing -- " + r.trouble);
    }

    // A stop that comes before the pass gets going still stops it.
    {
        BagCleaner early(no_window, store, [&](Frame& out) { out = scene; return true; });
        early.set_settings(cs);
        early.set_trash(&wanted);
        early.set_skip_focus_check(true);
        early.cancel();
        check(!early.run().ran, "a pass cancelled before it starts does not run");
    }
}

void test_bag_counts() {
    section("Bag stack counts and the item catalog");

    // The count is printed in the client's own 5x8 digits, white with a dark
    // outline, over the item. Drawn here from those digits, so the reader is
    // tested against exactly what the game draws.
    CountReader counts;
    const std::string font_dir = GlyphFont::default_dir();
    if (font_dir.empty() || !counts.load(font_dir)) {
        std::printf("  [skip] the client's digit glyphs are not installed here\n");
        return;
    }
    check(counts.loaded(), "the ten digits load");

    const auto slot_with = [&](const std::string& text, bool white_item) {
        Frame f = bag_scene(80, 60);
        const ScreenRect s{ 10, 10, 34, 34 };
        // An item filling the slot -- white, the hard case, or coloured.
        for (int y = 3; y < 31; ++y)
            for (int x = 3; x < 31; ++x)
                if (white_item) put_px(f, s.x + x, s.y + y, 235, 235, 240);
                else            put_px(f, s.x + x, s.y + y, 220, 170, 40);
        int x = s.x + 2;
        for (const char c : text) {
            Frame g;
            load_png(font_dir + "\\lower\\" + std::string(1, c) + ".png", g);
            // The outline first, then the digit.
            for (int pass = 0; pass < 2; ++pass)
                for (int gy = 0; gy < g.height; ++gy)
                    for (int gx = 0; gx < g.width; ++gx) {
                        std::uint8_t r, gg, b;
                        g.get(gx, gy, r, gg, b);
                        if ((r + gg + b) / 3 <= 128) continue;
                        if (pass == 0) {
                            for (int oy = -1; oy <= 1; ++oy)
                                for (int ox = -1; ox <= 1; ++ox)
                                    put_px(f, x + gx + ox, s.y + 3 + gy + oy, 10, 10, 10);
                        } else {
                            put_px(f, x + gx, s.y + 3 + gy, 250, 250, 250);
                        }
                    }
            x += kDigitStep;
        }
        return std::make_pair(f, s);
    };
    for (const char* text : { "7", "97", "100", "67", "1", "2345", "8086" }) {
        auto [f, s] = slot_with(text, false);
        const int got = counts.read(f, s);
        check(got == std::atoi(text), std::string("reads ") + text + " -- got " + std::to_string(got));
    }
    {
        auto [f, s] = slot_with("42", true);
        check(counts.read(f, s) == 42, "and reads it over a white item too");
    }
    {
        // The client prints counts from 2 up. A single item of a kind that
        // stacks has nothing in its corner, and the bin still asks how many,
        // with the game's own 1 in the box: so nothing printed reads as 1.
        auto [f, s] = slot_with("", false);
        check(counts.read(f, s) == 1, "an item with no count printed is a stack of one");
    }
    {
        // White in the corner that is not a number -- a white icon, a digit
        // half covered -- is not a count of one.
        auto [f, s] = slot_with("", false);
        for (int y = 3; y < 11; ++y)
            for (int x = 2; x < 13; ++x)
                if ((x * 7 + y * 3) % 4 == 0) put_px(f, s.x + x, s.y + y, 250, 250, 250);
        check(counts.read(f, s) == CountReader::kUnreadable,
              "white marks that do not read as a number are unreadable, not 1");
    }

    // The quantity box of the bin's dialog: the same digits on a dark field,
    // and a yellow caret after them that blinks -- over the last column of a
    // digit five pixels wide while it is on.
    {
        const ScreenRect box{ 10, 10, 82, 15 };
        const auto box_with = [&](const std::string& text, bool caret) {
            Frame f = bag_scene(120, 40, 216);
            for (int y = box.y; y < box.bottom(); ++y)
                for (int x = box.x; x < box.right(); ++x)
                    put_px(f, x, y, 65 + (y - box.y), 65 + (y - box.y), 70 + (y - box.y));
            if (!text.empty()) print_number(f, box.x + 7, box.y + 6, text, font_dir);
            if (caret) draw_caret(f, box, caret_x(box, text.size()));
            return f;
        };
        check(counts.read_field(box_with("", false), box) == 0,
              "an empty quantity box reads as nothing");
        check(counts.read_field(box_with("", true), box) == 0,
              "and so does an empty box with its caret on: the caret is no number");
        check(counts.read_field(box_with("1", true), box) == 1,
              "the game's 1 in the box reads as 1, the caret beside it");
        check(counts.read_field(box_with("2", false), box) == 2 &&
                  counts.read_field(box_with("2", true), box) == 2,
              "a 2 reads as 2 with the caret over its last column as well as without it -- "
              "the stack of two the live run could not delete");
        std::string wrong;
        for (const char* text : { "0", "3", "4", "5", "6", "7", "8", "9", "16", "22", "100", "2345" }) {
            const int got = counts.read_field(box_with(text, true), box);
            if (got != std::atoi(text)) wrong += std::string(" ") + text + " read " + std::to_string(got);
        }
        check(wrong.empty(), "every digit, and longer numbers, read with the caret on" + wrong);
        {
            // Two of the 2's five columns out of sight is too much of it to
            // say which digit it is.
            Frame f = box_with("2", false);
            draw_caret(f, box, box.x + 10);
            check(counts.read_field(f, box) == CountReader::kUnreadable,
                  "a digit with too much of it under the caret is not guessed at");
        }

        // A digit that does not read takes the number with it: the digits
        // either side of it are not the number in the box. The caret stands
        // where the client never draws it here -- over the middle of a digit,
        // or over a 6's first column, which leaves a 6 looking like a 3.
        // `column` is from the digit's left edge.
        const auto digit_x = [&](const std::string& /*text*/, std::size_t i) {
            // As box_with prints it: kDigitStep from digit to digit.
            return box.x + 7 + kDigitStep * static_cast<int>(i);
        };
        const auto caret_over = [&](const std::string& text, std::size_t digit, int column) {
            Frame f = box_with(text, false);
            draw_caret(f, box, digit_x(text, digit) + column);
            return counts.read_field(f, box);
        };
        {
            const int middle = caret_over("12", 1, 1), later = caret_over("12", 1, 2);
            check(middle == CountReader::kUnreadable && later == CountReader::kUnreadable,
                  "12 with the caret over the middle columns of its 2 does not read, rather than "
                  "reading 1 -- read " + std::to_string(middle) + " and " + std::to_string(later));
        }
        {
            struct Hidden { const char* text; std::size_t digit; int column; };
            std::string misread;
            for (const Hidden& h : { Hidden{ "125", 2, 2 }, Hidden{ "120", 2, 1 }, Hidden{ "26", 0, 1 },
                                     Hidden{ "6", 0, -1 }, Hidden{ "16", 1, -1 } }) {
                const int got = caret_over(h.text, h.digit, h.column);
                if (got != CountReader::kUnreadable)
                    misread += std::string(" ") + h.text + " read " + std::to_string(got);
            }
            check(misread.empty(), "nor does a number with its last digit, its first or a 6 under "
                                   "the caret: never 12 for 125, 6 for 26, or 3 for 6" + misread);
        }
    }

    // The catalog of the client's item icons.
    ItemCatalog catalog;
    if (!catalog.load(ItemCatalog::default_dir())) {
        std::printf("  [skip] the client's item icons are not installed here\n");
        return;
    }
    check(catalog.size() >= 50, "the item icons load -- " + std::to_string(catalog.size()));
    const ItemCatalog::Entry* bottle = catalog.find("spend/mana/8810101");
    check(bottle != nullptr && bottle->icon.valid() && bottle->tile.width == 34,
          "an icon is found by its id, with a slot-sized tile for the screen");
    check(catalog.find("spend/mana/0000000") == nullptr, "an unknown id is not");
    if (bottle == nullptr) return;

    // Chosen from the catalog, recognised in the bag -- under a stack count.
    TrashList list;
    list.add_catalog(catalog, { "spend/mana/8810101", "no/such/item" },
                     TrashList::Action::Delete, 0.85f);
    check(list.items.size() == 1 && list.items[0].id == "spend/mana/8810101",
          "ids the catalog has are added, others skipped");

    auto [f, s] = slot_with("", false);
    for (int y = 0; y < 34; ++y)                     // an empty slot again
        for (int x = 0; x < 34; ++x) put_px(f, s.x + x, s.y + y, 14, 14, 14);
    blit(f, bottle->icon, s.x + 3, s.y + 2);
    for (int y = 3; y < 11; ++y)                     // "97" in white over the corner
        for (int x = 2; x < 13; ++x)
            if ((x + y) % 3) put_px(f, s.x + x, s.y + y, 250, 250, 250);
    float score = 0.0f;
    const int which = list.match(f, s, 0.90f, &score);
    check(which == 0, "the catalog icon is recognised in a slot, count and all -- score " +
                      std::to_string(score));

    const ItemCatalog::Entry* other = catalog.find("spend/hp/8820051");
    if (other != nullptr) {
        auto [g, gs] = slot_with("", false);
        for (int y = 0; y < 34; ++y)
            for (int x = 0; x < 34; ++x) put_px(g, gs.x + x, gs.y + y, 14, 14, 14);
        blit(g, other->icon, gs.x + 2, gs.y + 4);
        check(list.match(g, gs, 0.90f) < 0, "a different item is not");
    }

    // An icon in a slot, as the bag draws it.
    const auto slot_of = [&](const ItemCatalog::Entry& e) {
        auto [g, gs] = slot_with("", false);
        for (int y = 0; y < 34; ++y)
            for (int x = 0; x < 34; ++x) put_px(g, gs.x + x, gs.y + y, 14, 14, 14);
        blit(g, e.icon, gs.x + (34 - e.icon.width) / 2, gs.y + (34 - e.icon.height) / 2);
        return std::make_pair(g, gs);
    };

    // One drawing in two colours is one item, by the player's rule: listing
    // either throws both away. Two drawings that only look alike -- a bag
    // with a potion beside it and the same bag alone, two bowls of different
    // soup, a gourd with a tag and one without -- are told apart by the rest
    // of the catalog, and the one not listed is left alone.
    {
        const struct { const char* listed; const char* in_bag; bool same; } kAlike[] = {
            { "spend/buff/8871061", "spend/buff/8871051", true },    // pink seal, grey seal
            { "spend/craft/8800960", "spend/craft/8800852", true },  // one scroll, two colours
            { "seal/8219021", "seal/8211045", true },                // one ring, two gems
            { "spend/hp/8820011", "spend/mana/8810011", true },      // one gourd, red and blue
            { "spend/hp/8843017", "spend/hp/8843015", false },       // the bag, and the bag and potion
            { "spend/mana/8810071", "spend/mana/8810081", false },   // two soups
            { "spend/hp/8820011", "spend/hp/8820013", false },       // the gourd, and it with a tag
        };
        for (const auto& pair : kAlike) {
            const ItemCatalog::Entry* bagged = catalog.find(pair.in_bag);
            if (bagged == nullptr || catalog.find(pair.listed) == nullptr) continue;
            const std::vector<std::string> kin = catalog.same_drawing(pair.listed);
            const bool found_same = std::find(kin.begin(), kin.end(), pair.in_bag) != kin.end();
            check(found_same == pair.same, std::string(pair.listed) + " and " + pair.in_bag +
                                               (pair.same ? " are one drawing" : " are two drawings"));
            TrashList chosen;
            chosen.add_catalog(catalog, { pair.listed }, TrashList::Action::Delete, 0.90f);
            chosen.add_rivals(catalog);
            auto [g, gs] = slot_of(*bagged);
            const TrashList::Verdict k = chosen.judge(g, gs, 0.90f);
            if (pair.same)
                check(k.item >= 0, std::string("listing ") + pair.listed + " throws " + pair.in_bag +
                                       " away too -- score " + std::to_string(k.score));
            else
                check(k.item < 0, std::string("listing ") + pair.listed + " leaves " + pair.in_bag +
                                      " alone -- " +
                                      (k.doubt == TrashList::Verdict::Doubt::Rival
                                           ? "it is " + k.rival : std::string("no likeness")));
            TrashList right;
            right.add_catalog(catalog, { pair.in_bag }, TrashList::Action::Delete, 0.90f);
            right.add_rivals(catalog);
            // Matched first and reported after, as above.
            float right_score = 0.0f;
            const int taken = right.match(g, gs, 0.90f, &right_score);
            check(taken == 0, std::string("while listing ") + pair.in_bag + " takes it -- score " +
                                  std::to_string(right_score));
        }
    }

    // A few ids are one picture, and the same drawing in another colour is
    // on the list along with what was ticked, saying which brought it.
    {
        if (catalog.find("spend/craft/8800975") != nullptr &&
            catalog.find("spend/craft/8800956") != nullptr) {
            const std::vector<std::string> kin = catalog.same_drawing("spend/craft/8800975");
            check(std::find(kin.begin(), kin.end(), "spend/craft/8800956") != kin.end(),
                  "8800975 and 8800956 are found to be one picture");
            auto [g, gs] = slot_of(*catalog.find("spend/craft/8800956"));
            TrashList one;
            one.add_catalog(catalog, { "spend/craft/8800975" }, TrashList::Action::Delete, 0.90f);
            one.add_rivals(catalog);
            check(one.match(g, gs, 0.90f) >= 0, "so listing one of them throws both away");
            bool brought = false;
            for (const TrashList::Item& item : one.items)
                brought = brought || (item.id == "spend/craft/8800956" &&
                                      item.via == "spend/craft/8800975");
            check(brought, "the other is on the list as drawn like the one ticked");
            bool rival = false;
            for (const TrashList::Item& r : one.rivals) rival = rival || r.id == "spend/craft/8800956";
            check(!rival, "and is no rival to it");
        }
        check(catalog.same_drawing("spend/mana/8810101").empty(),
              "most icons have no other drawn like them");
        TrashList none;
        none.add_catalog(catalog, { "spend/mana/8810101" }, TrashList::Action::Delete, 0.90f);
        check(none.items.size() == 1, "and bring nothing else onto the list");
    }

    // The same for a picture cut from the bag. The gourd with a tag and the
    // plain one differ mostly in the corner a badge goes in, which a slot is
    // matched without, so a picture of either scores over 0.90 on the other's
    // slot. Which icons a picture is drawn like is decided with that corner
    // in, as for two icons: the other gourd stays a rival, and its icon,
    // which matches its own slot better, keeps the picture off it.
    if (const ItemCatalog::Entry* tagged = catalog.find("spend/mana/8810013")) {
        if (const ItemCatalog::Entry* plain = catalog.find("spend/mana/8810011")) {
            const BagLayout layout;
            // Drawn as the client draws an icon: a pixel right of the middle
            // of the slot, blended over its dark.
            const auto drawn = [&](const ItemCatalog::Entry& e) {
                Frame f = bag_scene(80, 60);
                const ScreenRect s{ 10, 10, 34, 34 };
                const int ox = s.x + (36 - e.icon.width) / 2, oy = s.y + (34 - e.icon.height) / 2;
                for (int y = 0; y < e.icon.height; ++y)
                    for (int x = 0; x < e.icon.width; ++x) {
                        const std::size_t i = (static_cast<std::size_t>(y) * e.icon.width + x) * 4;
                        const int a = e.icon.pixels[i + 3];
                        std::uint8_t r, gg, b;
                        f.get(ox + x, oy + y, r, gg, b);
                        put_px(f, ox + x, oy + y, (e.icon.pixels[i + 2] * a + r * (255 - a)) / 255,
                               (e.icon.pixels[i + 1] * a + gg * (255 - a)) / 255,
                               (e.icon.pixels[i] * a + b * (255 - a)) / 255);
                    }
                return std::make_pair(f, s);
            };
            const std::pair<const ItemCatalog::Entry*, const ItemCatalog::Entry*> kGourds[] = {
                { tagged, plain }, { plain, tagged } };
            for (const auto& [from, to] : kGourds) {
                auto [g, gs] = drawn(*from);
                auto [h, hs] = drawn(*to);
                TrashList cut;
                cut.add_picture(bag::cut(g, gs), TrashList::Action::Delete, layout);
                cut.add_rivals(catalog);
                const TrashList::Verdict on_other = cut.judge(h, hs, 0.90f);
                check(on_other.item < 0 && on_other.doubt == TrashList::Verdict::Doubt::Rival,
                      "a picture cut from a slot of " + from->id + " leaves " + to->id +
                          " alone, though alike enough to be taken -- " +
                          (on_other.doubt == TrashList::Verdict::Doubt::Rival
                               ? std::to_string(on_other.like_score) + ", and " + on_other.rival +
                                     " " + std::to_string(on_other.rival_score)
                               : on_other.item >= 0 ? "TAKEN at " + std::to_string(on_other.score)
                                                    : std::string("no likeness")));
                const auto& kin = cut.items[0].kin;
                const bool own = std::find(kin.begin(), kin.end(), from->id) != kin.end();
                const bool others = std::find(kin.begin(), kin.end(), to->id) != kin.end();
                check(own && !others, "it is drawn like " + from->id + ", and not like " + to->id);
                check(cut.judge(g, gs, 0.90f).item == 0, "while it takes " + from->id + " itself");
            }
        }
    }

    // Fragments change colour in the bag, gold to orange to red and back, and
    // are thrown away at any point of it: the drawing decides.
    if (const ItemCatalog::Entry* piece = catalog.find("spend/fragment/1103023")) {
        const auto tinted = [&](int dr, int dg, int db) {
            auto [g, gs] = slot_of(*piece);
            for (int y = 0; y < 34; ++y)
                for (int x = 0; x < 34; ++x) {
                    std::uint8_t r, gg, b;
                    g.get(gs.x + x, gs.y + y, r, gg, b);
                    if (r + gg + b <= 3 * 14) continue;
                    put_px(g, gs.x + x, gs.y + y, std::clamp(r + dr, 0, 255),
                           std::clamp(gg + dg, 0, 255), std::clamp(b + db, 0, 255));
                }
            return std::make_pair(g, gs);
        };
        CleanSettings one_piece;
        one_piece.delete_ids = { "spend/fragment/1103023" };
        TrashList list_one;
        list_one.build(temp_file("gobot_trash_nowhere"), one_piece, catalog);
        check(!list_one.rivals.empty() && list_one.rivals.size() + 1 == catalog.size(),
              "a list built for a pass has every other catalog icon as a rival");

        auto [pulse, ps] = tinted(10, -45, -40);           // mid-pulse, as on the live bag
        check(list_one.match(pulse, ps, 0.90f) == 0, "a fragment mid-pulse is taken");
        auto [red, rs] = tinted(-60, -110, -70);           // the red of GoC's pictures
        check(list_one.match(red, rs, 0.90f) == 0, "and red, as the bag draws it too");
        auto [same, ss] = slot_of(*piece);
        check(list_one.match(same, ss, 0.90f) == 0, "and gold, as its icon");
        if (const ItemCatalog::Entry* other_piece = catalog.find("spend/fragment/1103063")) {
            auto [o, os] = slot_of(*other_piece);
            check(list_one.match(o, os, 0.90f) < 0, "but not another piece's drawing");
        }

        // The enlarged copy a slot shows while its item waits in the bin.
        {
            auto [g, gs] = slot_of(*piece);
            Frame big = g;
            for (int y = 0; y < 34; ++y)
                for (int x = 0; x < 34; ++x) {
                    const int sx = 17 + static_cast<int>(std::lround((x - 17) / 1.35));
                    const int sy = 17 + static_cast<int>(std::lround((y - 17) / 1.35));
                    std::uint8_t r, gg, b;
                    g.get(gs.x + sx, gs.y + sy, r, gg, b);
                    put_px(big, gs.x + x, gs.y + y, r, gg, b);
                }
            float enlarged = 0.0f;
            const int taken = list_one.match(big, gs, 0.90f, &enlarged);
            check(taken < 0, "and not the enlarged copy shown while it waits in the bin -- score " +
                                 std::to_string(enlarged));
        }
    }

    // GoC's own pictures of fragments in the bag ("mảnh quái rơi"): all nine
    // drawings, red, on the bag's slots. Every piece is taken for its own
    // gold icon, and for no other.
    {
        TemplateStore pictures;
        pictures.load_best(exe_relative("configs/templates.ini"));
        const Frame* roi1 = pictures.get("grabbed/manhquai_roi1");
        const Frame* roi3 = pictures.get("grabbed/manhquai_roi3");
        if (roi1 == nullptr || roi3 == nullptr || catalog.find("spend/fragment/1103093") == nullptr) {
            std::printf("  [skip] GoC's pictures of fragments in the bag are not installed here\n");
        } else {
            // Slots measured off the pictures: 37 apart, as in the bag, and
            // the piece each holds.
            struct Slot { const Frame* in; int x, y; const char* id; };
            const Slot kPieces[] = {
                { roi1, 77, 1, "1103083" },  { roi1, 114, 1, "1103053" },
                { roi1, 151, 1, "1103043" }, { roi1, 188, 1, "1103043" },
                { roi1, 3, 38, "1103033" },  { roi1, 40, 38, "1103033" },
                { roi1, 77, 38, "1103023" }, { roi1, 114, 38, "1103013" },
                { roi1, 151, 38, "1103093" }, { roi1, 188, 38, "1103083" },
                { roi1, 3, 75, "1103073" },  { roi1, 40, 75, "1103053" },
                { roi1, 77, 75, "1103033" }, { roi1, 114, 75, "1103013" },
                { roi3, 16, 11, "1103013" }, { roi3, 90, 11, "1103033" },
                { roi3, 53, 48, "1103053" }, { roi3, 16, 85, "1103073" },
                { roi3, 53, 85, "1103083" }, { roi3, 90, 85, "1103093" },
            };
            CleanSettings all_nine;
            for (const char* id : { "1103013", "1103023", "1103033", "1103043", "1103053",
                                    "1103063", "1103073", "1103083", "1103093" })
                all_nine.delete_ids.push_back(std::string("spend/fragment/") + id);
            TrashList nine;
            nine.build(temp_file("gobot_trash_nowhere"), all_nine, catalog);
            int right = 0, total = 0;
            float lowest = 1.0f;
            std::string wrong;
            for (const Slot& p : kPieces) {
                const ScreenRect at{ p.x, p.y, 34, 34 };
                const TrashList::Verdict k = nine.judge(*p.in, at, 0.90f);
                ++total;
                const std::string want = std::string("spend/fragment/") + p.id;
                if (k.item >= 0 && nine.items[static_cast<std::size_t>(k.item)].id == want) {
                    ++right;
                    lowest = std::min(lowest, k.score);
                } else {
                    wrong += std::string(" ") + p.id;
                }
            }
            check(right == total, "every red piece is taken for its gold icon: " +
                                      std::to_string(right) + " of " + std::to_string(total) +
                                      ", the lowest at " + std::to_string(lowest) +
                                      (wrong.empty() ? "" : "; wrong:" + wrong));

            // One listed: only its pieces go.
            CleanSettings only;
            only.delete_ids = { "spend/fragment/1103033" };
            TrashList one_kind;
            one_kind.build(temp_file("gobot_trash_nowhere"), only, catalog);
            int taken = 0, should = 0;
            for (const Slot& p : kPieces) {
                const bool is = std::string(p.id) == "1103033";
                should += is ? 1 : 0;
                const int k = one_kind.match(*p.in, ScreenRect{ p.x, p.y, 34, 34 }, 0.90f);
                if ((k >= 0) == is) ++taken;
            }
            check(taken == total && should == 4,
                  "and listing one piece takes its four and leaves the other sixteen -- " +
                      std::to_string(taken) + " of " + std::to_string(total) + " right");
        }
    }

    // The ids chosen beforehand, as configs/trash.ini keeps them.
    {
        const std::string path = temp_file("gobot_trash_items.ini");
        {
            std::ofstream out(path);
            out << "[Trash]\nenabled=false\n"
                   "[Items]\ndelete=spend/fragment/1103063, spend/mana/8810101\ndrop=\n";
        }
        CleanSettings cs;
        check(cs.load(path) && cs.delete_ids.size() == 2 && cs.drop_ids.empty() &&
                  cs.delete_ids[1] == "spend/mana/8810101",
              "the chosen ids read back from [Items]");
        std::remove(path.c_str());
    }
}

namespace {

/// The bin's quantity dialog as the live client draws it: the picture of its
/// words, then the row with the box and the two buttons, the box holding
/// `in_box` -- with the caret after it, when `caret`. The buttons end up at
/// `at` + (100, 50), as measured.
void draw_count_dialog(Frame& f, const TemplateStore& store, int x, int y,
                       const std::string& in_box, const std::string& font_dir,
                       bool caret = false) {
    blit(f, *store.get("grabbed/del_item_confirm_2"), x, y);
    fill(f, ScreenRect{ x, y + 49, 196, 30 }, 216, 216, 216);
    const ScreenRect pair{ x + 100, y + 50, 85, 21 };
    const ScreenRect box{ pair.x - 88, pair.y + 3, 82, 15 };
    for (int yy = box.y; yy < box.bottom(); ++yy)
        for (int xx = box.x; xx < box.right(); ++xx)
            put_px(f, xx, yy, 65 + (yy - box.y), 65 + (yy - box.y), 70 + (yy - box.y));
    if (!in_box.empty()) print_number(f, box.x + 7, box.y + 6, in_box, font_dir);
    if (caret) draw_caret(f, box, caret_x(box, in_box.size()));
    blit(f, *store.get("grabbed/confirm_button"), pair.x, pair.y);
}

/// The time as a pretend game and the cleaner looking at it both see it: on
/// only by the cleaner's own waits and by each look at the game, which costs
/// kFrameMs -- never by the machine being busy. Every decision the cleaner
/// makes by how long something took then comes out the same on every run; a
/// probe under load once clicked the bin before the third look, which the
/// test had the bin's question come up on.
struct PretendClock {
    static constexpr int kFrameMs = 40;
    std::chrono::steady_clock::time_point t =
        std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    void advance(int ms) { t += std::chrono::milliseconds(ms); }
    long long ms_since(std::chrono::steady_clock::time_point then) const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(t - then).count();
    }
    BagCleaner::Clock clock() {
        BagCleaner::Clock c;
        c.now = [this] { return t; };
        c.sleep = [this](int ms) { advance(ms); };
        return c;
    }
};

/// A pretend game for the cleaner's hands: the bag with one item in it, the
/// bin, and the bin's dialogs, answering a drag, a click and a key the way the
/// live client was seen to. Drag the item onto the bin and the bin holds it;
/// click the bin and it asks, the whole stack already in the box and the caret
/// blinking after it; Xác Nhận with the right number and the item is gone,
/// Hủy Bỏ and it is back in its slot. The box takes digit keys and BACKSPACE
/// as the live one does, and every key pressed and every click into it while
/// it is up is counted: the cleaner is to make none. Its time is its own
/// clock's, which `look` moves on by a frame.
struct PretendBin {
    const TemplateStore* store = nullptr;
    std::string font_dir;
    Frame       base;          ///< the bag, the item in its slot, the can in the bin
    ScreenRect  slot;          ///< the item's
    ScreenRect  bin;
    int         count = 1;     ///< in the stack; what the box has to say
    bool        stacks = true; ///< asks how many, rather than whether

    enum class State { Idle, InBin, Asking } state = State::Idle;
    bool        gone = false;
    std::string box;           ///< what the quantity box holds
    /// What the game puts in the box when it asks. Empty for the whole
    /// stack, which is what the live client does.
    std::string prefill;
    bool        blank = false;       ///< the box comes up with nothing in it
    /// The caret: 0 not drawn, 1 on all the time, 2 blinking half a second on
    /// and half off, starting on when the dialog comes up -- as live.
    int         caret = 2;
    PretendClock time;
    std::chrono::steady_clock::time_point asked_at = time.t;
    bool        scribble = false;    ///< white marks in the box that are no number
    std::string flicker;             ///< in the box every other look instead
    mutable int looks = 0;
    /// When not -1: the box shows its number for that long after it came up,
    /// on the pretend clock, and white marks that are no number from then on.
    int         readable_ms = -1;
    bool        revive_up = false;   ///< another dialog is up as well
    bool        wrong_dialog = false; ///< the bin brings up somebody else's dialog
    int         silent = 0;          ///< clicks on the bin, holding the item, that bring nothing up
    /// The bin asks as the item lands on it, before it is clicked -- as the
    /// live client may for an item that does not stack, for all anyone has
    /// seen of it.
    bool        asks_on_drop = false;
    int         bin_clicks = 0;      ///< clicks on the bin, whatever came of them
    bool        two_step = false;    ///< after the quantity, the question as well
    bool        again = false;       ///< that question is the one up
    ScreenRect  strip;               ///< the bag's tab strip
    bool        bag_shut = false;    ///< the bag closed: no strip, until its key
    int         drags = 0, confirms = 0, cancels = 0, wrong_answers = 0;
    /// Keys pressed while the box was up, any of them, and clicks into it:
    /// none of either for a box left as the game filled it.
    int         box_keys = 0, box_clicks = 0;
    int         keys_into_dialog = 0; ///< the bag's key pressed with a dialog up

    bool box_up() const { return state == State::Asking && stacks && !wrong_dialog && !again; }
    ScreenRect box_rect() const {
        const ScreenRect p{ kDialogX + 100, kDialogY + 50, 85, 21 };
        return ScreenRect{ p.x - 88, p.y + 3, 82, 15 };
    }
    bool caret_on() const {
        if (caret != 2) return caret == 1;
        return (time.ms_since(asked_at) / 500) % 2 == 0;
    }

    static constexpr int kDialogX = 600, kDialogY = 120;
    /// Where the question comes up after the quantity: somewhere else.
    static constexpr int kAgainX = kDialogX + 60, kAgainY = kDialogY + 40;
    ScreenRect pair() const {
        if (again) return ScreenRect{ kAgainX + 101, kAgainY + 51, 85, 21 };
        return stacks || wrong_dialog ? ScreenRect{ kDialogX + 100, kDialogY + 50, 85, 21 }
                                      : ScreenRect{ kDialogX + 101, kDialogY + 51, 85, 21 };
    }

    Frame render() const {
        Frame f = base;
        if (gone) fill(f, ScreenRect{ slot.x + 2, slot.y + 2, slot.w - 4, slot.h - 4 }, 14, 14, 14);
        if (state == State::InBin) paint_item(f, ScreenRect{ bin.x - 4, bin.y - 2, 24, 26 }, 6);
        if (state == State::Asking) {
            if (again) {
                blit(f, *store->get("grabbed/del_item_confirm_1"), kAgainX, kAgainY);
            } else if (wrong_dialog) {
                blit(f, *store->get("grabbed/sell_confirm"), kDialogX, kDialogY - 3);
            } else if (stacks) {
                const bool other = !flicker.empty() && looks++ % 2 == 1;
                const bool marks =
                    scribble || (readable_ms >= 0 && time.ms_since(asked_at) >= readable_ms);
                draw_count_dialog(f, *store, kDialogX, kDialogY,
                                  marks ? std::string() : other ? flicker : box, font_dir,
                                  caret_on());
                if (marks) {
                    const ScreenRect b = box_rect();
                    for (int y = 3; y < 11; ++y)
                        for (int x = 2; x < 13; ++x)
                            if ((x * 7 + y * 3) % 4 == 0) put_px(f, b.x + 5 + x, b.y + 2 + y, 250, 250, 250);
                }
            } else {
                blit(f, *store->get("grabbed/del_item_confirm_1"), kDialogX, kDialogY);
            }
        }
        if (revive_up) blit(f, *store->get("grabbed/hoisinh_confirm"), 640, 420);
        if (bag_shut) fill(f, strip, 20, 22, 30);
        return f;
    }

    /// The bin asks about the item it holds, the whole stack in the box.
    void ask() {
        state = State::Asking;
        box = blank ? std::string() : prefill.empty() ? std::to_string(count) : prefill;
        asked_at = time.t;
    }

    /// One look at the game, as the cleaner's grab: a frame's worth of time
    /// passes, then the screen as it is.
    Frame look() {
        time.advance(PretendClock::kFrameMs);
        return render();
    }

    BagCleaner::Hands hands() {
        BagCleaner::Hands h;
        h.drag = [this](int fx, int fy, int tx, int ty) {
            ++drags;
            const ScreenRect near_bin{ bin.x - 4, bin.y - 4, bin.w + 8, bin.h + 8 };
            if (state == State::Idle && !gone && slot.contains(fx, fy) && near_bin.contains(tx, ty)) {
                if (asks_on_drop) ask();
                else              state = State::InBin;
            }
            return true;
        };
        h.click = [this](int x, int y) {
            const ScreenRect near_bin{ bin.x - 4, bin.y - 4, bin.w + 8, bin.h + 8 };
            if (near_bin.contains(x, y)) ++bin_clicks;
            if (state == State::InBin && near_bin.contains(x, y) && silent > 0) {
                --silent;                  // the game let the click go by
                return true;
            }
            if (state == State::InBin && near_bin.contains(x, y)) {
                ask();
                return true;
            }
            if (box_up() && box_rect().contains(x, y)) {
                ++box_clicks;
                return true;
            }
            const ScreenRect p = pair();
            if (state == State::Asking && p.contains(x, y)) {
                if (x < p.x + p.w / 2) {
                    ++confirms;
                    if (wrong_dialog) {
                        ++wrong_answers;
                    } else if (again) {
                        gone = true;
                    } else if (!stacks || box == std::to_string(count)) {
                        if (two_step) {
                            again = true;          // and the question is up instead
                            return true;
                        }
                        gone = true;
                    }
                } else {
                    ++cancels;
                }
                again = false;
                state = State::Idle;
            }
            return true;
        };
        h.key = [this](std::uint32_t vk) {
            if (box_up()) {
                ++box_keys;
                if (vk == VK_BACK && !box.empty()) box.pop_back();
                if (vk >= '0' && vk <= '9') box += static_cast<char>(vk);
            }
            if (vk == 'I') {
                if (state == State::Asking) ++keys_into_dialog;
                else                        bag_shut = !bag_shut;
            }
            return true;
        };
        return h;
    }
};

} // namespace

void test_bag_dialogs() {
    section("Bag: the bin's dialogs, and an item's way into the bin");

    TemplateStore store;
    const std::string font_dir = GlyphFont::default_dir();
    CountReader counts;
    const bool have = store.load_best(exe_relative("configs/templates.ini")) &&
                      store.get("grabbed/inventory") && store.get("inven/0/trash_can") &&
                      store.get("grabbed/confirm_button") &&
                      store.get("grabbed/del_item_confirm_1") &&
                      store.get("grabbed/del_item_confirm_2") &&
                      store.get("grabbed/hoisinh_confirm") && store.get("grabbed/sell_confirm") &&
                      store.get("grabbed/drop_confirm") && !font_dir.empty() &&
                      counts.load(font_dir);
    if (!have) {
        std::printf("  [skip] the dialog pictures or the digits are not installed here\n");
        return;
    }

    // Told apart by their words, where the words sit relative to the buttons.
    {
        Frame f = bag_scene(1280, 720, 60);
        draw_count_dialog(f, store, 354, 306, "1", font_dir);      // where the live one was
        blit(f, *store.get("grabbed/del_item_confirm_1"), 40, 40);
        blit(f, *store.get("grabbed/hoisinh_confirm"), 700, 60);
        blit(f, *store.get("grabbed/drop_confirm"), 700, 300);
        blit(f, *store.get("grabbed/sell_confirm"), 700, 500);
        const auto dialogs = bag::find_dialogs(f, store);
        const auto at = [&](int x, int y) -> const bag::Dialog* {
            for (const bag::Dialog& d : dialogs)
                if (std::abs(d.pair.x - x) <= 2 && std::abs(d.pair.y - y) <= 2) return &d;
            return nullptr;
        };
        const bag::Dialog* count = at(454, 356);
        check(count != nullptr && count->ours() && count->kind == bag::kCountDialog &&
                  count->quantity && count->box.x == 366 && count->box.y == 359,
              "the bin's quantity dialog is known, its box beside the buttons");
        check(count != nullptr && counts.read_field(f, count->box) == 1,
              "and the 1 in its box reads as 1");
        const bag::Dialog* ask = at(141, 91);
        check(ask != nullptr && ask->ours() && ask->kind == bag::kAskDialog && !ask->quantity,
              "the bin's question is known, by the buttons as it draws them");
        const bag::Dialog* revive = at(803, 115);
        check(revive != nullptr && !revive->ours(),
              "the revive question is found and is not the bin's");
        const bag::Dialog* drop = at(802, 353);
        check(drop != nullptr && !drop->ours(),
              "nor is a dialog with no words, the drop picture's");
        const bag::Dialog* sell = at(800, 553);
        check(sell != nullptr && !sell->ours(), "nor a shop's quantity box");
        check(dialogs.size() == 5, "and nothing else is taken for a dialog -- found " +
                                   std::to_string(dialogs.size()));
        check(bag::can_check(store, TrashList::Action::Delete) &&
                  !bag::can_check(store, TrashList::Action::Drop),
              "the bin's dialogs can be checked; the drop question cannot, so nothing is dropped");
    }

    // The pretend game: the bag with an item in slot 5 and the can in the bin.
    BagLayout l;
    Frame scene = bag_scene(1024, 768, 60);
    for (int y = 300; y < 300 + 220; ++y)
        for (int x = 290; x < 290 + 272; ++x) put_px(scene, x, y, 20, 22, 30);
    blit(scene, *store.get("grabbed/inventory"), 300, 300);
    blit(scene, *store.get("inven/0/trash_can"), 300 + l.bin_dx, 300 + l.bin_dy);
    const BagView v = bag::find(scene, store, l);
    if (!v.found) {
        check(false, "the pretend bag is found");
        return;
    }
    const ScreenRect slot = v.slots(l)[5];
    paint_item(scene, slot, 1);
    // Nothing white where a count would be: this is an item of one.
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 28; ++x) {
            std::uint8_t r, gg, b;
            scene.get(slot.x + x, slot.y + y, r, gg, b);
            if ((r + gg + b) / 3 > 150) put_px(scene, slot.x + x, slot.y + y, 200, 120, 60);
        }
    TrashList wanted;
    wanted.add_picture(bag::cut(scene, slot), TrashList::Action::Delete, l);

    CleanSettings cs;
    cs.tabs[0] = true;
    cs.tabs[1] = cs.tabs[2] = cs.tabs[3] = cs.tabs[4] = false;

    const auto pass = [&](PretendBin& game, bool look_only = false,
                          std::vector<std::string>* said = nullptr) {
        InputManager no_window;
        BagCleaner cleaner(no_window, store, [&](Frame& out) { out = game.look(); return true; });
        cleaner.set_settings(cs);
        cleaner.set_trash(&wanted);
        cleaner.set_counts(&counts);
        cleaner.set_dry_run(look_only);
        cleaner.set_hands(game.hands());
        cleaner.set_clock(game.time.clock());
        if (said) cleaner.on_report([said](const std::string& line) { said->push_back(line); });
        return cleaner.run();
    };
    const auto game_with = [&](const std::string& corner) {
        PretendBin game;
        game.store = &store;
        game.font_dir = font_dir;
        game.base = scene;
        game.slot = slot;
        game.bin = v.trash;
        game.strip = v.strip;
        if (corner == "?") {
            for (int y = 3; y < 11; ++y)
                for (int x = 2; x < 13; ++x)
                    if ((x * 7 + y * 3) % 4 == 0) put_px(game.base, slot.x + x, slot.y + y, 250, 250, 250);
        } else if (!corner.empty()) {
            print_number(game.base, slot.x + 3, slot.y + 3, corner, font_dir);
            game.count = std::atoi(corner.c_str());
        }
        return game;
    };

    const auto untouched = [](const PretendBin& game) {
        return game.box_keys == 0 && game.box_clicks == 0;
    };
    const auto says = [](const std::vector<std::string>& lines, const std::string& what) {
        for (const std::string& line : lines)
            if (line.find(what) != std::string::npos) return true;
        return false;
    };
    {
        PretendBin game = game_with("");
        const auto r = pass(game);
        check(r.ran && r.deleted == 1 && r.failed == 0 && game.gone && game.confirms == 1 &&
                  untouched(game),
              "a stack of one: dragged, the bin clicked, the game's 1 in the box left as it is, "
              "Xác Nhận -- deleted " + std::to_string(r.deleted) + ", failed " +
                  std::to_string(r.failed));
        check(!r.stuck, "and an item deleted leaves nothing held");
    }
    {
        // The stack the live run could not delete: the game puts its 2 in the
        // box, and here the caret stands over the 2's last column every time
        // the box is looked at.
        PretendBin game = game_with("2");
        game.caret = 1;
        const auto r = pass(game);
        check(r.deleted == 1 && game.gone && game.confirms == 1 && untouched(game),
              "a stack of two, the caret over the game's 2: Xác Nhận without touching the box -- "
              "deleted " + std::to_string(r.deleted) + ", keys " + std::to_string(game.box_keys));
    }
    {
        PretendBin game = game_with("7");
        const auto r = pass(game);
        check(r.deleted == 1 && game.gone && untouched(game) && game.box == "7",
              "a stack of seven, the caret blinking: the game's 7 confirmed as it is");
    }
    {
        // Another number in the box than the slot's: the count off the slot
        // was misread, or the bin holds something else. Nothing is ever typed
        // over the game's number, and it is not confirmed.
        PretendBin game = game_with("12");
        game.prefill = "1";
        std::vector<std::string> said;
        const auto r = pass(game, false, &said);
        check(r.deleted == 0 && r.failed == 1 && !game.gone && game.confirms == 0 &&
                  game.cancels == 1 && untouched(game) && game.box == "1",
              "a 1 in the box for a stack of twelve: Hủy Bỏ, the game's 1 left in the box -- "
              "cancels " + std::to_string(game.cancels) + ", keys " + std::to_string(game.box_keys) +
                  ", the box holds '" + game.box + "'");
        check(says(said, "ô số lượng ghi 1 chứ không phải 12"), "and it says what the box held");
    }
    {
        // The bag changed under the pass: what went into the bin is a stack
        // of fifty, where the slot that was judged said 2.
        PretendBin game = game_with("2");
        game.prefill = "50";
        const auto r = pass(game);
        check(r.deleted == 0 && !game.gone && game.confirms == 0 && game.cancels == 1 &&
                  untouched(game) && game.box == "50",
              "50 in the box where the slot said 2: Hủy Bỏ, and nothing typed over the 50 -- "
              "keys " + std::to_string(game.box_keys) + ", clicks into the box " +
                  std::to_string(game.box_clicks));
    }
    {
        PretendBin game = game_with("2");
        game.blank = true;
        std::vector<std::string> said;
        const auto r = pass(game, false, &said);
        check(r.deleted == 0 && !game.gone && game.confirms == 0 && game.cancels == 1 &&
                  untouched(game) && game.box.empty() && says(said, "ô số lượng trống"),
              "an empty box for a stack of two: Hủy Bỏ, and nothing typed into it");
    }
    {
        PretendBin game = game_with("2");
        game.scribble = true;
        const auto r = pass(game);
        check(r.deleted == 0 && !game.gone && game.confirms == 0 && game.cancels == 1 &&
                  untouched(game),
              "a box that does not read through a whole blink: Hủy Bỏ, and the box is not "
              "erased -- cancels " + std::to_string(game.cancels));
    }
    {
        PretendBin game = game_with("2");
        game.flicker = "3";
        const auto r = pass(game);
        check(r.deleted == 0 && !game.gone && game.confirms == 0 && game.cancels == 1 &&
                  untouched(game),
              "a box that reads 2 one look and 3 the next: no reading, so Hủy Bỏ");
    }
    // How many looks the box needs, on the pretend clock: the bin's dialog
    // is first seen 190 ms after the bin is clicked, and the box is then
    // looked at every 190 ms until 1200 ms of watching have passed -- at 380,
    // 570, 760 ms and on after the click.
    {
        PretendBin game = game_with("7");
        game.readable_ms = 650;         // the looks at 380 and 570 ms read it
        const auto r = pass(game);
        check(r.deleted == 1 && game.gone && game.confirms == 1 && untouched(game),
              "two looks inside the box's watch that read the same number are its number: "
              "confirmed -- deleted " + std::to_string(r.deleted));
    }
    {
        PretendBin game = game_with("7");
        game.readable_ms = 470;         // only the look at 380 ms reads it
        std::vector<std::string> said;
        const auto r = pass(game, false, &said);
        check(r.deleted == 0 && !game.gone && game.confirms == 0 && game.cancels == 1 &&
                  untouched(game) && says(said, "không đọc được số trong ô số lượng"),
              "one look alone is not: the box counts as unreadable, and Hủy Bỏ -- cancels " +
                  std::to_string(game.cancels));
    }
    {
        // The dialog goes away while its box is watched -- the game closed it,
        // and the item is back in the bag: not confirmed, and nothing left to
        // say no to.
        PretendBin game = game_with("2");
        int asking = 0;
        std::vector<std::string> said;
        InputManager no_window;
        BagCleaner cleaner(no_window, store, [&](Frame& out) {
            if (game.state == PretendBin::State::Asking && ++asking == 3)
                game.state = PretendBin::State::Idle;
            out = game.look();
            return true;
        });
        cleaner.set_settings(cs);
        cleaner.set_trash(&wanted);
        cleaner.set_counts(&counts);
        cleaner.set_hands(game.hands());
        cleaner.set_clock(game.time.clock());
        cleaner.on_report([&](const std::string& line) { said.push_back(line); });
        const auto r = cleaner.run();
        check(r.deleted == 0 && r.failed == 1 && !game.gone && game.confirms == 0 &&
                  untouched(game) && !r.stuck && says(said, "không còn ở chỗ cũ"),
              "a box whose dialog goes away while it is watched: not confirmed -- confirms " +
                  std::to_string(game.confirms) + ", failed " + std::to_string(r.failed));
    }
    {
        PretendBin game = game_with("?");
        const auto r = pass(game);
        check(r.deleted == 0 && r.failed == 1 && !game.gone && game.confirms == 0 &&
                  game.cancels == 1 && game.state == PretendBin::State::Idle,
              "a count that does not read: Hủy Bỏ, and the item is back in the bag -- "
              "cancels " + std::to_string(game.cancels));
    }
    {
        PretendBin game = game_with("");
        game.stacks = false;
        const auto r = pass(game);
        check(r.deleted == 1 && game.gone && untouched(game),
              "an item that does not stack: the question answered Xác Nhận");
    }
    {
        // The state the first live run left the game in: the quantity dialog
        // up, holding the item, with 1 in the box.
        PretendBin game = game_with("");
        game.state = PretendBin::State::Asking;
        game.box = "1";
        const auto r = pass(game);
        check(r.ran && game.cancels == 1 && r.deleted == 1 && game.gone,
              "a dialog left open by an earlier pass is said no to first, then the pass goes on");
    }
    {
        // The same with the bag shut: the key that opens it would go into the
        // dialog's box, so the dialog is said no to before the key is pressed.
        PretendBin game = game_with("");
        game.state = PretendBin::State::Asking;
        game.box = "1";
        game.bag_shut = true;
        const auto r = pass(game);
        check(game.keys_into_dialog == 0 && game.cancels == 1 && r.ran && r.deleted == 1 &&
                  game.gone,
              "with the bag shut and the bin's dialog up, Hủy Bỏ first, and only then the bag's "
              "key -- keys into the dialog " + std::to_string(game.keys_into_dialog));
    }
    {
        PretendBin game = game_with("");
        game.revive_up = true;
        const auto r = pass(game);
        check(!r.ran && !r.trouble.empty() && game.drags == 0 && game.confirms == 0 &&
                  game.cancels == 0,
              "with somebody else's dialog up, nothing is touched -- " + r.trouble);
        check(!r.stuck, "and nothing of the pass's is held, so nothing is held off for it");
    }
    {
        PretendBin game = game_with("");
        game.wrong_dialog = true;
        const auto r = pass(game);
        check(r.deleted == 0 && game.wrong_answers == 0 && game.confirms == 0 && !game.gone &&
                  !r.trouble.empty(),
              "when the bin brings up a dialog that is not its own, it is not answered");
        check(r.stuck, "and the pass says the item may still be held, so the keys stay off");

        // The next pass meets that dialog, still holding the item, and leaves
        // it alone without getting anywhere -- stuck or not, it has seen
        // nothing of the item coming back, so the hold stays on.
        const bool held = r.keeps_hold(false);
        const auto next = pass(game);
        check(held && !next.ran && !next.stuck && next.keeps_hold(held) &&
                  game.state == PretendBin::State::Asking,
              "a pass after it that meets the same dialog keeps everything held off");
        const auto look = pass(game, true);
        check(!look.ran && look.keeps_hold(true) && !look.keeps_hold(false),
              "so does a look-only pass, which leaves the hold as it found it");
        // Once the player has said no to it, a pass gets to a clean screen,
        // and that ends the hold.
        game.wrong_dialog = false;
        game.state = PretendBin::State::Idle;
        const auto after = pass(game);
        check(after.ran && after.deleted == 1 && !after.keeps_hold(true),
              "and a pass that gets to a clean screen ends it");
    }
    {
        // The bin asks how many, then whether: the question comes up
        // somewhere else, and the wordless notice picture matches it at 0.84.
        // It is the next step, not a refusal.
        PretendBin game = game_with("7");
        game.two_step = true;
        const auto r = pass(game);
        check(r.deleted == 1 && game.gone && game.confirms == 2 && game.cancels == 0,
              "the quantity and then the question, both answered -- confirms " +
                  std::to_string(game.confirms) + ", deleted " + std::to_string(r.deleted));
    }
    {
        // A stop that comes once the bin's dialog is up gives the item back
        // rather than pressing Xác Nhận.
        PretendBin game = game_with("");
        InputManager no_window;
        BagCleaner* running = nullptr;
        BagCleaner cleaner(no_window, store, [&](Frame& out) {
            out = game.look();
            if (game.state == PretendBin::State::Asking && running) running->cancel();
            return true;
        });
        running = &cleaner;
        cleaner.set_settings(cs);
        cleaner.set_trash(&wanted);
        cleaner.set_counts(&counts);
        cleaner.set_hands(game.hands());
        cleaner.set_clock(game.time.clock());
        const auto r = cleaner.run();
        check(game.confirms == 0 && game.cancels >= 1 && !game.gone && r.deleted == 0 && !r.stuck &&
                  game.state == PretendBin::State::Idle,
              "a stop while the bin asks answers Hủy Bỏ, and the item is back -- cancels " +
                  std::to_string(game.cancels));
    }

    // A probe: what the bin asks for one item, answered Hủy Bỏ whatever it
    // is, with nothing confirmed, nothing typed and the item back in its
    // slot -- the first row's sixth here.
    // `frames_dir` keeps its frames; `asks_after` has the bin ask by itself
    // on the look that many after it first holds the item.
    const auto probe = [&](PretendBin& game, int row, int col, std::vector<std::string>* said = nullptr,
                           bool stop_when_asked = false, const std::string& frames_dir = std::string(),
                           int asks_after = 0) {
        InputManager no_window;
        BagCleaner* running = nullptr;
        BagCleaner cleaner(no_window, store, [&](Frame& out) {
            if (asks_after > 0 && game.state == PretendBin::State::InBin && --asks_after == 0) game.ask();
            out = game.look();
            if (stop_when_asked && game.state == PretendBin::State::Asking && running) running->cancel();
            return true;
        });
        running = &cleaner;
        cleaner.set_settings(cs);
        cleaner.set_counts(&counts);
        cleaner.set_hands(game.hands());
        cleaner.set_clock(game.time.clock());
        if (!frames_dir.empty()) cleaner.set_debug_dir(frames_dir);
        if (said) cleaner.on_report([said](const std::string& line) { said->push_back(line); });
        return cleaner.probe(0, row, col);
    };
    const auto left_alone = [](const PretendBin& game) {
        return game.confirms == 0 && game.box_keys == 0 && game.box_clicks == 0 &&
               game.keys_into_dialog == 0 && !game.gone;
    };
    // The probe frames kept in `dir`, cleared away: how many there were.
    const auto probe_frames = [](const std::string& dir) {
        std::vector<std::string> names;
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA((dir + "\\probe_*.png").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { names.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        for (const std::string& n : names) DeleteFileA((dir + "\\" + n).c_str());
        return static_cast<int>(names.size());
    };
    {
        // A stack, the bag shut to begin with: its key first, on a screen
        // with no dialog up, and then the bin's quantity dialog -- once the
        // bin is clicked, which is what asks.
        PretendBin game = game_with("7");
        game.bag_shut = true;
        std::vector<std::string> said;
        const auto r = probe(game, 0, 5, &said);
        check(r.moved && r.dialog == "quantity" && !r.at_drop && game.bin_clicks >= 1 &&
                  r.words >= 0.90f && r.frames > 0,
              "a probe sees the bin's quantity dialog, once the bin is clicked -- " + r.dialog +
                  (r.at_drop ? "@drop" : "") + ", words " + std::to_string(r.words) + ", " +
                  std::to_string(r.frames) + " frames");
        check(left_alone(game) && game.cancels == 1 && game.state == PretendBin::State::Idle && r.back,
              "and answers it Hủy Bỏ: nothing confirmed, nothing typed, the item back -- cancels " +
                  std::to_string(game.cancels));
        check(says(said, "hộp thoại ở") && says(said, "ô ghi 7"),
              "and says what each frame showed, the game's 7 in the box among it");
    }
    {
        // The question comes up as the item lands, before the bin is
        // clicked. Seen there and kept from the drag on, said to be the
        // drop's, and answered Hủy Bỏ with the bin never clicked.
        PretendBin game = game_with("");
        game.stacks = false;
        game.asks_on_drop = true;
        std::vector<std::string> said;
        const std::string dir = temp_file("gobot_probe_frames");
        CreateDirectoryA(dir.c_str(), nullptr);
        probe_frames(dir);
        const auto r = probe(game, 0, 5, &said, false, dir);
        const int kept = probe_frames(dir);
        RemoveDirectoryA(dir.c_str());
        check(r.moved && r.dialog == "ask" && r.at_drop && r.words >= 0.90f && r.frames > 0,
              "a question that comes up as the item lands is seen there -- " + r.dialog +
                  (r.at_drop ? "@drop" : "") + ", words " + std::to_string(r.words));
        check(left_alone(game) && game.bin_clicks == 0 && game.cancels == 1 && r.back,
              "and answered Hủy Bỏ, the bin never clicked, the item back -- clicks on the bin " +
                  std::to_string(game.bin_clicks) + ", cancels " + std::to_string(game.cancels));
        check(kept == r.frames && says(said, "ngay khi món đồ vào thùng rác") &&
                  says(said, "sau khi thả vào thùng rác") && !says(said, "sau khi bấm vào thùng rác"),
              "every frame from the drag on is kept and read, and said to be the drop's -- " +
                  std::to_string(kept) + " kept of " + std::to_string(r.frames));
    }
    {
        // A moment after the item shows in the bin, before the bin would be
        // clicked: still the drop's.
        PretendBin game = game_with("7");
        std::vector<std::string> said;
        const auto r = probe(game, 0, 5, &said, false, std::string(), 3);
        check(r.dialog == "quantity" && r.at_drop && game.bin_clicks == 0 && left_alone(game) &&
                  game.cancels == 1 && r.back && says(said, "ô ghi 7"),
              "and so is one that comes up a moment after the item shows in the bin -- " + r.dialog +
                  (r.at_drop ? "@drop" : "") + ", clicks on the bin " + std::to_string(game.bin_clicks));
    }
    // The edges of that moment: the bin is clicked once it has shown the
    // item for 300 ms with nothing up. On the pretend clock a look comes
    // every 40 ms from the drop, so the looks either side of it are 280 ms
    // and 320 ms after the item first showed.
    {
        PretendBin game = game_with("7");
        const auto r = probe(game, 0, 5, nullptr, false, std::string(), 8);
        check(r.dialog == "quantity" && r.at_drop && game.bin_clicks == 0 && left_alone(game) &&
                  game.cancels == 1 && r.back,
              "a question first seen 280 ms after the item showed in the bin is the drop's: the "
              "bin is never clicked -- " + r.dialog + (r.at_drop ? "@drop" : "") +
                  ", clicks on the bin " + std::to_string(game.bin_clicks));
    }
    {
        PretendBin game = game_with("7");
        const auto r = probe(game, 0, 5, nullptr, false, std::string(), 10);
        check(r.dialog == "quantity" && !r.at_drop && game.bin_clicks == 1 && left_alone(game) &&
                  game.cancels == 1 && r.back,
              "one not up by the look 320 ms on is not: the bin is clicked exactly once, what it "
              "asks is answered Hủy Bỏ and the item is back -- " + r.dialog +
                  (r.at_drop ? "@drop" : "") + ", clicks on the bin " +
                  std::to_string(game.bin_clicks));
    }
    {
        PretendBin game = game_with("");
        game.asks_on_drop = true;
        game.wrong_dialog = true;
        const auto r = probe(game, 0, 5);
        check(r.dialog == "foreign" && r.at_drop && r.words < 0.90f && left_alone(game) &&
                  game.wrong_answers == 0 && game.bin_clicks == 0 && game.cancels == 1 && r.back,
              "somebody else's dialog as the item lands: foreign, at the drop, and Hủy Bỏ all the "
              "same -- " + r.dialog + (r.at_drop ? "@drop" : ""));
    }
    {
        PretendBin game = game_with("");
        game.stacks = false;
        const auto r = probe(game, 0, 5);
        check(r.dialog == "ask" && r.words >= 0.90f && left_alone(game) && game.cancels == 1 && r.back,
              "an item that does not stack: the bin's question, answered Hủy Bỏ -- " + r.dialog);
    }
    {
        // The question for an item that does not stack, if its words were not
        // the picture's: somebody else's dialog, as far as anything can tell.
        // A pass leaves that alone; a probe says no to it all the same.
        PretendBin game = game_with("");
        game.wrong_dialog = true;
        const auto r = probe(game, 0, 5);
        check(r.dialog == "foreign" && r.words < 0.90f && left_alone(game) && game.wrong_answers == 0 &&
                  game.cancels == 1 && r.back,
              "a dialog that is not the bin's comes up: foreign, and Hủy Bỏ all the same -- " +
                  r.dialog + ", cancels " + std::to_string(game.cancels));
    }
    {
        PretendBin game = game_with("");
        game.silent = 1;
        std::vector<std::string> said;
        const auto r = probe(game, 0, 5, &said);
        check(r.dialog == "none" && says(said, "không thấy hộp thoại nào") && left_alone(game) &&
                  game.cancels == 1 && r.back,
              "nothing comes up: it says so, and gives the item back as a pass does -- cancels " +
                  std::to_string(game.cancels));
    }
    {
        PretendBin game = game_with("2");
        const auto r = probe(game, 0, 5, nullptr, true);
        check(left_alone(game) && game.cancels == 1 && r.back,
              "a stop while it watches: Hủy Bỏ straight away, and the item back");
    }
    {
        PretendBin game = game_with("");
        game.revive_up = true;
        const auto r = probe(game, 0, 5);
        check(!r.trouble.empty() && !r.moved && game.drags == 0 && game.cancels == 0 &&
                  game.confirms == 0 && r.dialog == "none" && !r.back,
              "with another dialog up, a probe touches nothing -- " + r.trouble);
    }
    {
        PretendBin game = game_with("");
        const auto r = probe(game, 0, 0);
        check(!r.trouble.empty() && !r.moved && game.drags == 0 && !r.back,
              "nor on an empty slot -- " + r.trouble);
    }

    // Anything else that presses Xác Nhận keeps its hands off the bin's
    // dialogs: the party invitation's footer picture -- the same two buttons
    // -- is found on the bin's quantity dialog, and its line does not read as
    // an invitation's.
    {
        Frame f = bag_scene(1280, 720, 60);
        draw_count_dialog(f, store, 354, 306, "1", font_dir);
        check(bag::bin_asking(f, store), "the bin's quantity dialog is the bin asking");
        if (store.get("grabbed/party_invite_buttons") != nullptr) {
            GlyphFont font;
            const bool have_font = font.load(GlyphFont::default_dir());
            PartyInvite invite;
            const auto seen = invite.look(f, store, have_font ? &font : nullptr);
            const ScreenRect pair{ 454, 356, 85, 21 };
            check(!seen.up || pair.contains(seen.accept_x, seen.accept_y),
                  "the invitation's picture, where it is found on it, puts Xác Nhận on the bin's");
            check(seen.who.empty(), "and no invitation's name is read off its words");
        }
        Frame g = bag_scene(1280, 720, 60);
        blit(g, *store.get("grabbed/hoisinh_confirm"), 700, 60);
        check(!bag::bin_asking(g, store), "while the revive question is not the bin asking");
    }
}

namespace {

/// A pretend game for the reviver: a 1280x720 frame of the world with the
/// death panel on it, and what the game does with a click there. The panel
/// is the player's own (grabbed/chardeadpanel), centred where the client
/// draws a dialog; its item button brings up the question, and Xác Nhận on
/// that revives -- or, with no revive item left, only closes it and leaves
/// the panel. Every click is kept, and where it landed counted.
struct PretendDeath {
    const TemplateStore* store = nullptr;
    Frame base;
    bool panel = true;
    bool question = false;
    bool bin = false;               ///< the bin's question up as well
    bool has_item = true;           ///< a revive item in the bag
    bool asks = true;               ///< the item button brings up the question
    /// With no revive item, Xác Nhận puts the panel away for this long before
    /// it is back -- what the live game does then is not known yet.
    int  blink_ms = 0;
    std::chrono::steady_clock::time_point hidden_until{};
    std::function<void(Frame&)> also;   ///< anything else on the screen
    PretendClock time;

    static constexpr int kPanelX = 432, kPanelY = 244;
    static constexpr int kAskX = 541, kAskY = 319;

    // Where the buttons are on that frame, as measured on the pictures.
    static ScreenRect item()    { return ScreenRect{ kPanelX + 157, kPanelY + 58, 105, 43 }; }
    static ScreenRect village() { return ScreenRect{ kPanelX + 34, kPanelY + 58, 105, 43 }; }
    static ScreenRect cash()    { return ScreenRect{ kPanelX + 280, kPanelY + 58, 105, 43 }; }
    static ScreenRect tick1()   { return ScreenRect{ 684, 432, 13, 12 }; }
    static ScreenRect tick2()   { return ScreenRect{ 811, 432, 13, 12 }; }
    static ScreenRect confirm() { return ScreenRect{ kAskX + 104, kAskY + 56, 39, 20 }; }
    static ScreenRect cancel()  { return ScreenRect{ kAskX + 146, kAskY + 56, 39, 20 }; }

    int item_clicks = 0, confirm_clicks = 0, cancel_clicks = 0;
    int village_clicks = 0, cash_clicks = 0, tick_clicks = 0, other_clicks = 0;
    std::vector<std::pair<int, int>> clicks;

    Frame render() const {
        Frame f = base;
        if (panel && time.t >= hidden_until)
            blit(f, *store->get("grabbed/chardeadpanel"), kPanelX, kPanelY);
        if (question) blit(f, *store->get("grabbed/hoisinh_confirm"), kAskX, kAskY);
        if (bin) blit(f, *store->get("grabbed/del_item_confirm_1"), 120, 120);
        if (also) also(f);
        return f;
    }
    /// What a look costs on the pretend clock: a little more than one takes
    /// on the player's machine, so that the looks of ten pretend minutes of a
    /// death -- each a few full-frame matches, in real time -- stay few.
    static constexpr int kLookCostMs = 700;
    Frame look() {
        time.advance(kLookCostMs);
        return render();
    }
    Reviver::Hands hands() {
        Reviver::Hands h;
        h.click = [this](int x, int y) {
            clicks.emplace_back(x, y);
            if (village().contains(x, y)) ++village_clicks;
            if (cash().contains(x, y)) ++cash_clicks;
            if (tick1().contains(x, y) || tick2().contains(x, y)) ++tick_clicks;
            if (question && confirm().contains(x, y)) {
                ++confirm_clicks;
                question = false;
                if (has_item) panel = false;
                else if (blink_ms > 0) hidden_until = time.t + std::chrono::milliseconds(blink_ms);
            } else if (question && cancel().contains(x, y)) {
                ++cancel_clicks;
                question = false;
            } else if (panel && !question && item().contains(x, y)) {
                ++item_clicks;
                if (asks) question = true;
                else if (has_item) panel = false;
            } else {
                ++other_clicks;
            }
            return true;
        };
        return h;
    }
    Reviver::Clock clock() {
        Reviver::Clock c;
        c.now = [this] { return time.t; };
        c.sleep = [this](int ms) { time.advance(ms); };
        return c;
    }
};

} // namespace

/// Reviving, as GoC does it: the panel found by two corners of its buttons,
/// its revive-item button, Xác Nhận on the question -- three tries a death,
/// never the village or Cash, and nothing while the bin's dialog is up.
void test_reviver() {
    section("Revive: the death panel's item button, then Xác Nhận; never the village");
    const auto started = std::chrono::steady_clock::now();

    TemplateStore store;
    const bool have = store.load_best(exe_relative("configs/templates.ini")) &&
                      store.get("grabbed/chardeadpanel") && store.get("grabbed/hoisinh_confirm") &&
                      store.get("dialog_panic") && store.get("grabbed/confirm_button") &&
                      store.get("grabbed/del_item_confirm_1") &&
                      store.get("grabbed/del_item_confirm_2") && store.get("grabbed/drop_confirm") &&
                      store.get("grabbed/sell_confirm");
    if (!have) {
        std::printf("  [skip] the death panel's or the dialogs' pictures are not installed here\n");
        return;
    }
    const std::string font_dir = GlyphFont::default_dir();

    // The world to die in: a real frame of the client when there is one.
    Frame world;
    if (!load_png(exe_relative("frame.png"), world) || world.width != 1280 || world.height != 720)
        world = bag_scene(1280, 720, 40);

    InputManager no_window;
    {
        Reviver r(no_window, store, [](Frame&) { return false; });
        std::string why;
        check(r.ready(&why), "the pictures it needs are here: " + (why.empty() ? std::string("all") : why));
    }
    const auto seen_on = [&](const Frame& f) {
        Reviver r(no_window, store, [](Frame&) { return false; });
        return r.look(f);
    };

    // ---- what one look sees ----
    {
        Frame f = world;
        blit(f, *store.get("grabbed/chardeadpanel"), 432, 244);
        const auto s = seen_on(f);
        check(s.panel && s.by_anchors && s.item.x == 589 && s.item.y == 302 &&
                  s.item.w == 105 && s.item.h == 42 && !s.question,
              "the player's panel at 432,244 is found by GoC's two corners (" +
                  std::to_string(s.panel_score).substr(0, 4) + "); the item button at 589,302");
        check(ScreenRect{ 589, 302, 105, 43 }.contains(s.item_x(), s.item_y()) &&
                  !PretendDeath::village().contains(s.item_x(), s.item_y()) &&
                  !PretendDeath::cash().contains(s.item_x(), s.item_y()),
              "its middle is inside 589..693, 302..344 -- the village and Cash buttons are not");
    }
    {
        // GoC's own picture of the panel, in English: the corners carry no
        // words, so the same button is found.
        Frame f = world;
        blit(f, *store.get("dialog_panic"), 430, 241);
        const auto s = seen_on(f);
        check(s.panel && s.by_anchors && s.item.x == 589 && s.item.y == 302,
              "GoC's English panel put there instead gives the same item button");
    }
    {
        // Without GoC's picture, the player's own panel, whole.
        TemplateStore some;
        for (const char* n : { "grabbed/chardeadpanel", "grabbed/hoisinh_confirm",
                               "grabbed/confirm_button", "grabbed/del_item_confirm_1",
                               "grabbed/del_item_confirm_2" })
            some.put(n, *store.get(n));
        Frame f = world;
        blit(f, *store.get("grabbed/chardeadpanel"), 432, 244);
        Reviver r(no_window, some, [](Frame&) { return false; });
        const auto s = r.look(f);
        check(r.ready() && s.panel && !s.by_anchors && s.item.x == 589 && s.item.y == 302,
              "without GoC's picture the whole panel is the fallback, with the same button");
    }
    {
        // The item button lit up under the pointer that has just clicked it:
        // its corner no longer matches, and the whole panel still does --
        // still dead, rather than taken for revived.
        Frame f = world;
        blit(f, *store.get("grabbed/chardeadpanel"), 432, 244);
        for (int t = 0; t < 2; ++t)
            for (int i = 0; i < 105; ++i) {
                put_px(f, 589 + i, 302 + t, 255, 255, 200);
                put_px(f, 589 + i, 302 + 41 - t, 255, 255, 200);
                if (i < 42) {
                    put_px(f, 589 + t, 302 + i, 255, 255, 200);
                    put_px(f, 589 + 104 - t, 302 + i, 255, 255, 200);
                }
            }
        const auto s = seen_on(f);
        check(s.panel && !s.by_anchors && s.item.x == 589 && s.item.y == 302,
              "the item button lit up: its corner is not found, the whole panel round the "
              "village corner is (" + std::to_string(s.panel_score).substr(0, 4) + ")");
    }
    {
        // The village button lit up instead -- the pointer resting on it when
        // the character died. Its corner is gone, and the strongest place
        // left for that picture is the Cash button's corner: the panel is
        // looked for round that too, and its item button is the same.
        Frame f = world;
        blit(f, *store.get("grabbed/chardeadpanel"), 432, 244);
        const ScreenRect v = PretendDeath::village();
        for (int t = 0; t < 2; ++t)
            for (int i = 0; i < 105; ++i) {
                put_px(f, v.x + i, v.y + t, 255, 255, 200);
                put_px(f, v.x + i, v.y + 41 - t, 255, 255, 200);
                if (i < 42) {
                    put_px(f, v.x + t, v.y + i, 255, 255, 200);
                    put_px(f, v.x + 104 - t, v.y + i, 255, 255, 200);
                }
            }
        const auto s = seen_on(f);
        check(s.panel && s.item.x == 589 && s.item.y == 302 &&
                  !PretendDeath::village().contains(s.item_x(), s.item_y()) &&
                  !PretendDeath::cash().contains(s.item_x(), s.item_y()),
              "the village button lit up: the panel is still found -- round the Cash corner -- "
              "and the item button with it (" + std::to_string(s.panel_score).substr(0, 4) + ")");
    }
    {
        Frame f = world;
        blit(f, *store.get("grabbed/hoisinh_confirm"), 541, 319);
        const auto s = seen_on(f);
        check(s.question && std::abs(s.confirm_x() - 664) <= 2 && std::abs(s.confirm_y() - 384) <= 2 &&
                  PretendDeath::confirm().contains(s.confirm_x(), s.confirm_y()),
              "the revive question at 541,319 (" + std::to_string(s.question_score).substr(0, 4) +
                  "): Xác Nhận at " + std::to_string(s.confirm_x()) + "," +
                  std::to_string(s.confirm_y()) + ", inside 645..683, 375..394");
        check(!s.bin_asking && s.others.empty(), "and its own buttons are not taken for another dialog");
    }
    {
        // The bin's dialogs and the others score up to 0.87 against the
        // question's picture: none of them is taken for it.
        struct Case { const char* what; std::function<void(Frame&)> paint; };
        const Case cases[] = {
            { "the bin's question", [&](Frame& f) { blit(f, *store.get("grabbed/del_item_confirm_1"), 541, 319); } },
            { "the bin's count dialog", [&](Frame& f) { draw_count_dialog(f, store, 541, 319, "1", font_dir); } },
            { "the drop question", [&](Frame& f) { blit(f, *store.get("grabbed/drop_confirm"), 541, 319); } },
            { "a shop's quantity box", [&](Frame& f) { blit(f, *store.get("grabbed/sell_confirm"), 541, 319); } },
        };
        for (const Case& c : cases) {
            Frame f = world;
            c.paint(f);
            const auto s = seen_on(f);
            check(!s.question && !s.panel && !s.dead(),
                  std::string(c.what) + " alone is not the revive question (" +
                      std::to_string(s.question_score).substr(0, 4) + ")");
        }
    }
    {
        // Real frames of the client: never a panel, never a question.
        std::vector<std::string> shots{ exe_relative("frame.png") };
        const std::string dir = exe_relative("trash_debug");
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA((dir + "\\*.png").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do shots.push_back(dir + "\\" + fd.cFileName);
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        int looked = 0, dead = 0;
        double ms = 0.0;
        Reviver r(no_window, store, [](Frame&) { return false; });
        for (const std::string& path : shots) {
            Frame shot;
            if (!load_png(path, shot) || !shot.valid()) continue;
            const auto t0 = std::chrono::steady_clock::now();
            const auto s = r.look(shot);
            ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++looked;
            if (s.dead()) ++dead;
        }
        if (looked == 0) {
            std::printf("  [skip] no saved frames of the client here\n");
        } else {
            check(dead == 0, "on " + std::to_string(looked) +
                                 " real frames of the client nobody is dead: no panel, no question");
            std::printf("  [info] a look takes %.0f ms on a 1280x720 frame\n", ms / looked);
        }
    }

    // ---- turns on a pretend game ----
    //
    // The whole 1280x720 frame where it counts, and its top-left 880x520 --
    // everything the panel and the dialogs need -- for the pretend minutes,
    // which are a few hundred looks each.
    const Frame corner = bag::cut(world, ScreenRect{ 0, 0, 880, 520 });
    const auto game = [&](bool whole_frame = false) {
        PretendDeath g;
        g.store = &store;
        g.base = whole_frame ? world : corner;
        return g;
    };
    // What the reviver told: each revive, each stop request, and the two the
    // alerts are raised from -- a death begun, and a death come back from.
    struct Heard { int revived = 0, out = 0, deaths = 0, back = 0; std::string reason; };
    const auto reviver_for = [&](PretendDeath& g, Heard& heard) {
        auto r = std::make_unique<Reviver>(no_window, store, [&g](Frame& out) {
            out = g.look();
            return true;
        });
        r->set_hands(g.hands());
        r->set_clock(g.clock());
        r->on_revived([&heard] { ++heard.revived; });
        r->on_death([&heard] { ++heard.deaths; });
        r->on_back([&heard] { ++heard.back; });
        r->on_out_of_tries([&heard](const std::string& why) {
            ++heard.out;
            heard.reason = why;
        });
        return r;
    };
    // Turns a little over a second apart, as the GUI's worker takes them --
    // or further apart, where only the turns' answers matter.
    const auto turns_for = [](Reviver& r, PretendDeath& g, int seconds, int gap_ms = 1200) {
        std::vector<Reviver::Turn> out;
        const auto until = g.time.t + std::chrono::seconds(seconds);
        while (g.time.t < until) {
            out.push_back(r.turn());
            g.time.advance(gap_ms);
        }
        return out;
    };
    const auto never_wrong = [](const PretendDeath& g) {
        return g.village_clicks == 0 && g.cash_clicks == 0 && g.tick_clicks == 0 &&
               g.cancel_clicks == 0 && g.other_clicks == 0;
    };
    {
        PretendDeath g = game(true);
        Heard heard;
        auto r = reviver_for(g, heard);
        // The re-cast it sets off: the macro's own, on a clock of its own.
        InputManager keys;
        ExecutionCoordinator coord;
        MacroEngine macro(keys, coord);
        std::uint64_t macro_now = 3'600'000;
        MacroEngine::Clock mc;
        mc.now_ms = [&macro_now] { return macro_now; };
        macro.set_clock(mc);
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys.resize(1);
        cfg.keys[0].keycode = 'F';
        cfg.keys[0].enabled = true;
        cfg.keys[0].interval_ms = 1600000;
        cfg.keys[0].trigger = MacroTrigger::Always;
        macro.set_config(cfg);
        macro.start_stepped();
        macro_now += 60'000;
        int recasts = 0;
        r->on_revived([&] {
            ++heard.revived;
            if (macro.recast_buffs("a revive")) ++recasts;
        });

        const auto turn = r->turn();
        check(turn == Reviver::Turn::Revived && g.item_clicks == 1 && g.confirm_clicks == 1 &&
                  never_wrong(g) && !g.panel && !g.question,
              "the panel at 432,244: the item button once, then Xác Nhận once -- revived");
        check(g.clicks.size() == 2 && PretendDeath::item().contains(g.clicks[0].first, g.clicks[0].second) &&
                  PretendDeath::confirm().contains(g.clicks[1].first, g.clicks[1].second),
              "exactly two clicks: inside 589..693, 302..344, then inside Xác Nhận");
        check(r->revived() == 1 && heard.revived == 1 && recasts == 1,
              "the revive is counted once, and its re-cast hook re-casts the buffs once");
        const auto later = turns_for(*r, g, 20, 4000);
        check(g.clicks.size() == 2 &&
                  std::all_of(later.begin(), later.end(),
                              [](Reviver::Turn t) { return t == Reviver::Turn::Alive; }),
              "and alive afterwards, nothing more is clicked");
        // No HP reader here: the revive was taken from the panel staying
        // away, so its try is kept until the character has stayed up a minute.
        check(r->tries() == 1 && heard.deaths == 1 && heard.back == 0,
              "with HP unreadable its try is still counted 20 s on, and the revive not yet told");
        (void)turns_for(*r, g, 45, 5000);
        check(r->tries() == 0 && heard.deaths == 1 && heard.back == 1,
              "given back once the character has stayed up for 60 s -- and the revive told then, "
              "once");
        macro.stop();
    }
    {
        // The alerts' two hooks, and the stop request: a death with a revive
        // item tells of the death and of the revive, once each; one without
        // tells of the death and asks for the stop, once each.
        PretendDeath with = game();
        Heard a;
        auto r1 = reviver_for(with, a);
        r1->set_hp([&with] { return with.panel ? 0.0f : 80.0f; });
        (void)turns_for(*r1, with, 30);
        check(a.deaths == 1 && a.revived == 1 && a.back == 1 && a.out == 0,
              "a death with a revive item: told dead once, revived once, no stop -- " +
                  std::to_string(a.deaths) + " / " + std::to_string(a.back) + " / " +
                  std::to_string(a.out));
        PretendDeath without = game();
        without.has_item = false;
        Heard b;
        auto r2 = reviver_for(without, b);
        r2->set_hp([&without] { return without.panel ? 0.0f : 80.0f; });
        (void)turns_for(*r2, without, 60);
        (void)turns_for(*r2, without, 300, 30000);
        check(b.deaths == 1 && b.back == 0 && b.revived == 0 && b.out == 1 &&
                  without.item_clicks == 3,
              "a death with no revive item: told dead once, never revived, one stop request -- " +
                  std::to_string(b.deaths) + " / " + std::to_string(b.back) + " / " +
                  std::to_string(b.out));
    }
    {
        // Revived on the spot among whatever killed it, and dead again 3 s
        // later, over and over -- with revive items left. HP read above 0
        // confirms each revive, so each death is a new one with its own
        // tries: the bot never stops for want of items it still has.
        PretendDeath g = game();
        Heard heard;
        auto r = reviver_for(g, heard);
        r->set_hp([&g] { return g.panel ? 0.0f : 80.0f; });
        bool was_up = false;
        auto up_since = g.time.t;
        const auto until = g.time.t + std::chrono::seconds(120);
        while (g.time.t < until) {
            (void)r->turn();
            g.time.advance(1200);
            if (!g.panel && !was_up) {
                was_up = true;
                up_since = g.time.t;
            }
            if (!g.panel && g.time.ms_since(up_since) >= 3000) {
                g.panel = true;           // killed again
                was_up = false;
            }
        }
        check(heard.out == 0 && heard.revived >= 6 && heard.deaths >= heard.revived && heard.deaths <= heard.revived + 1 &&
                  heard.back == heard.revived && never_wrong(g),
              "dead again 3 s after each revive, items left: revived every time and never stopped -- " +
                  std::to_string(heard.revived) + " revives, " + std::to_string(heard.deaths) +
                  " deaths told, " + std::to_string(heard.out) + " stop requests");
    }
    {
        // No revive item, HP unreadable, and the game puts the panel away
        // for 10 s after each Xác Nhận. Each of those passes for a revive,
        // but the panel is back long before the character has stayed up for
        // a minute: the same death, so the third try is still the last, and
        // the bot stops.
        for (const int blink : { 10000, 15000 }) {
            PretendDeath g = game();
            g.has_item = false;
            g.blink_ms = blink;
            Heard heard;
            auto r = reviver_for(g, heard);
            (void)turns_for(*r, g, 120);
            (void)turns_for(*r, g, 180, 30000);
            check(heard.out == 1 && g.item_clicks == 3 && g.confirm_clicks == 3 && heard.deaths == 1 &&
                      heard.back == 0 && never_wrong(g),
                  "no item, HP unreadable, the panel away " + std::to_string(blink / 1000) +
                      " s after each Xác Nhận: still 3 tries, one death told, then the stop -- " +
                      std::to_string(g.item_clicks) + " tries, " + std::to_string(heard.revived) +
                      " taken for revives, " + std::to_string(heard.out) + " stop requests");
        }
    }
    {
        // The panel and the question both up -- the item button pressed by
        // hand, say: Xác Nhận on the question straight away.
        PretendDeath g = game();
        g.question = true;
        Heard heard;
        auto r = reviver_for(g, heard);
        check(r->turn() == Reviver::Turn::Revived && g.confirm_clicks == 1 && g.item_clicks == 0 &&
                  g.clicks.size() == 1 && never_wrong(g),
              "the panel and the question both up: Xác Nhận on the question, and nothing else");
    }
    {
        // The question with nobody dead: a dialog with the same frame and
        // the same words, about an item the player clicked. Left alone.
        PretendDeath g = game();
        g.panel = false;
        g.question = true;
        Heard heard;
        auto r = reviver_for(g, heard);
        const auto turns = turns_for(*r, g, 60, 5000);
        const bool alive = std::all_of(turns.begin(), turns.end(),
                                       [](Reviver::Turn t) { return t == Reviver::Turn::Alive; });
        check(g.clicks.empty() && alive && r->tries() == 0 && heard.revived == 0,
              "the question up with no death panel seen: alive, and nothing clicked in a minute");
        check(r->look(g.render()).question,
              "though a look for the dry run still finds it -- and says it would be left alone");
    }
    {
        // The panel seen at the death, then the question up over it --
        // covering it, or with the panel gone: that is the death's question,
        // and Xác Nhận is pressed on it.
        PretendDeath g = game();
        Heard heard;
        auto r = reviver_for(g, heard);
        bool allow = false;
        Reviver::Guard guard;
        guard.take = [&allow] { return allow; };
        r->set_guard(guard);
        const auto first = r->turn();           // the panel seen; put off by the guard
        g.panel = false;
        g.question = true;
        allow = true;
        const auto second = r->turn();
        check(first == Reviver::Turn::Waiting && second == Reviver::Turn::Revived &&
                  g.confirm_clicks == 1 && g.item_clicks == 0 && never_wrong(g),
              "the panel seen, then the question up without it: Xác Nhận on the question");
    }
    for (const bool asks : { true, false }) {
        // No revive item: the panel never goes away.
        PretendDeath g = game();
        g.has_item = false;
        g.asks = asks;
        Heard heard;
        auto r = reviver_for(g, heard);
        // Ten minutes: the first, a turn a little over a second apart, has
        // all three tries in it; the other nine are looked at every 30 s.
        auto turns = turns_for(*r, g, 60);
        const auto rest = turns_for(*r, g, 540, 30000);
        turns.insert(turns.end(), rest.begin(), rest.end());
        const auto count = [&](Reviver::Turn t) {
            return static_cast<int>(std::count(turns.begin(), turns.end(), t));
        };
        const std::string how = asks ? "the question comes and Xác Nhận does nothing"
                                     : "the item button brings nothing up";
        check(g.item_clicks == 3 && g.confirm_clicks == (asks ? 3 : 0) && never_wrong(g) &&
                  g.clicks.size() == static_cast<std::size_t>(asks ? 6 : 3),
              "no revive item (" + how + "): exactly 3 tries -- " + std::to_string(g.item_clicks) +
                  " item clicks, " + std::to_string(g.confirm_clicks) + " Xác Nhận");
        check(heard.out == 1 && count(Reviver::Turn::Failed) == 2 && r->out_of_tries() &&
                  heard.reason.find("revive items") != std::string::npos,
              "then one stop request, with the reason, and no 4th click in ten minutes");
        if (asks) {
            // Alive for a while, and dead again: a new death, with its own tries.
            g.panel = false;
            (void)turns_for(*r, g, 9);
            g.panel = true;
            (void)turns_for(*r, g, 5);
            check(g.item_clicks == 4 && heard.out == 1,
                  "a death after being seen alive for 5 s gets its own tries again");
        }
    }
    {
        // The panel gone for a moment between tries is the same death.
        PretendDeath g = game();
        g.has_item = false;
        Heard heard;
        auto r = reviver_for(g, heard);
        (void)r->turn();
        g.panel = false;
        (void)turns_for(*r, g, 2);
        g.panel = true;
        (void)turns_for(*r, g, 60);
        check(g.item_clicks == 3 && heard.out == 1,
              "the panel gone for 2 s between tries is the same death: still 3 tries in all");
    }
    {
        // HP read as 0 with the panel gone: not revived.
        PretendDeath g = game();
        Heard heard;
        auto r = reviver_for(g, heard);
        r->set_hp([] { return 0.0f; });
        const auto turn = r->turn();
        check(turn == Reviver::Turn::Failed && heard.revived == 0 && r->tries() == 1,
              "the panel gone while HP reads 0 is not a revive");
    }
    {
        // No revive item, and the game puts the panel away for a second and
        // a half after Xác Nhận. With HP not to be read, two clear looks were
        // taken for a revive and the tries given back every time: the bot
        // clicked on for ever instead of stopping.
        PretendDeath g = game();
        g.has_item = false;
        g.blink_ms = 1500;
        Heard heard;
        auto r = reviver_for(g, heard);
        (void)turns_for(*r, g, 75);
        check(heard.revived == 0 && heard.out == 1 && g.item_clicks == 3 && g.confirm_clicks == 3 &&
                  never_wrong(g),
              "no item and the panel away for 1.5 s after Xác Nhận, HP unreadable: no revive "
              "counted, 3 tries, then the stop -- " + std::to_string(g.item_clicks) + " tries");
    }
    {
        // Away longer than that -- 5 s, past the 3 s it has to stay gone --
        // and each comes back as a "revive"; but a panel back before the
        // character has stayed up a minute is the same death, its tries not given
        // back, so this still ends in the stop rather than going on.
        PretendDeath g = game();
        g.has_item = false;
        g.blink_ms = 5000;
        Heard heard;
        auto r = reviver_for(g, heard);
        (void)turns_for(*r, g, 60);
        (void)turns_for(*r, g, 180, 30000);
        check(heard.out == 1 && g.item_clicks == 3 && r->out_of_tries() && never_wrong(g),
              "no item and the panel away for 5 s each time: still 3 tries in all, then the stop "
              "-- " + std::to_string(g.item_clicks) + " tries, " +
                  std::to_string(heard.revived) + " taken for revives");
    }
    {
        // With HP read above 0, two clear looks are still enough.
        PretendDeath g = game();
        Heard heard;
        auto r = reviver_for(g, heard);
        r->set_hp([&g] { return g.panel ? 0.0f : 80.0f; });
        const auto before = g.time.t;
        const auto turn = r->turn();
        const long long took = g.time.ms_since(before);
        check(turn == Reviver::Turn::Revived && heard.revived == 1 && took < 5000,
              "with HP read above 0 a revive is taken at once, without the 3 s wait -- " +
                  std::to_string(took) + " ms of pretend time");
    }
    {
        // Nothing is clicked in any of these, however long they last.
        struct Case { const char* what; std::function<void(PretendDeath&)> set; };
        const Case cases[] = {
            { "nothing up", [](PretendDeath& g) { g.panel = false; } },
            { "the bin's question alone", [](PretendDeath& g) { g.panel = false; g.bin = true; } },
            { "the bin's count dialog alone",
              [&](PretendDeath& g) {
                  g.panel = false;
                  g.also = [&](Frame& f) { draw_count_dialog(f, store, 120, 120, "1", font_dir); };
              } },
            { "the drop question alone",
              [&](PretendDeath& g) {
                  g.panel = false;
                  g.also = [&](Frame& f) { blit(f, *store.get("grabbed/drop_confirm"), 541, 319); };
              } },
            { "a shop's quantity box alone",
              [&](PretendDeath& g) {
                  g.panel = false;
                  g.also = [&](Frame& f) { blit(f, *store.get("grabbed/sell_confirm"), 541, 319); };
              } },
            { "the revive question with the bin's dialog up",
              [](PretendDeath& g) { g.panel = false; g.question = true; g.bin = true; } },
            { "the panel and the revive question with the bin's dialog up",
              [](PretendDeath& g) { g.question = true; g.bin = true; } },
            { "the panel with the bin's dialog up", [](PretendDeath& g) { g.bin = true; } },
            { "the panel with a shop's box over its item button",
              [&](PretendDeath& g) {
                  g.also = [&](Frame& f) { blit(f, *store.get("grabbed/sell_confirm"), 600, 318); };
              } },
        };
        for (const Case& c : cases) {
            PretendDeath g = game();
            c.set(g);
            Heard heard;
            auto r = reviver_for(g, heard);
            const auto turns = turns_for(*r, g, 60, 10000);
            const bool waited = std::all_of(turns.begin(), turns.end(), [](Reviver::Turn t) {
                return t == Reviver::Turn::Waiting || t == Reviver::Turn::Alive;
            });
            check(g.clicks.empty() && r->tries() == 0 && heard.out == 0 && waited,
                  std::string(c.what) + ": nothing clicked in a minute, and no try counted");
        }
    }
    {
        // Put off by whoever runs it -- the mouse taken, everything held off
        // by somebody else: nothing clicked, nothing counted.
        PretendDeath g = game();
        Heard heard;
        auto r = reviver_for(g, heard);
        bool allow = false;
        int taken = 0, given_back = 0;
        Reviver::Guard guard;
        guard.take = [&] { ++taken; return allow; };
        guard.done = [&] { ++given_back; };
        r->set_guard(guard);
        const auto first = r->turn();
        check(first == Reviver::Turn::Waiting && g.clicks.empty() && r->tries() == 0,
              "put off before its first click, it clicks nothing and counts nothing");
        allow = true;
        const auto second = r->turn();
        check(second == Reviver::Turn::Revived && taken == 2 && given_back == 1,
              "let go ahead, it revives, and gives the mouse back after");
    }
    {
        // The screen changes while whoever runs it makes the reviver wait --
        // a trash pass finishing, the player at the keyboard. The look that
        // decided the attempt is from before that: it is looked at again
        // once let go, and nothing is clicked on the strength of the old one.
        struct Case {
            const char* what;
            std::function<void(PretendDeath&)> before;     ///< the screen when it decides
            std::function<void(PretendDeath&)> meanwhile;  ///< what changes during the wait
        };
        const Case cases[] = {
            { "revived by hand meanwhile", [](PretendDeath&) {},
              [](PretendDeath& g) { g.panel = false; } },
            { "the bin's question up meanwhile", [](PretendDeath&) {},
              [](PretendDeath& g) { g.bin = true; } },
            { "the revive question gone and the bin's in its place",
              [](PretendDeath& g) { g.question = true; },
              [&](PretendDeath& g) {
                  g.question = false;
                  g.also = [&](Frame& f) {
                      blit(f, *store.get("grabbed/del_item_confirm_1"), PretendDeath::kAskX,
                           PretendDeath::kAskY);
                  };
              } },
            { "a shop's box over the item button meanwhile", [](PretendDeath&) {},
              [&](PretendDeath& g) {
                  g.also = [&](Frame& f) { blit(f, *store.get("grabbed/sell_confirm"), 600, 318); };
              } },
        };
        for (const Case& c : cases) {
            PretendDeath g = game();
            c.before(g);
            Heard heard;
            auto r = reviver_for(g, heard);
            int given_back = 0;
            Reviver::Guard guard;
            guard.take = [&] {
                c.meanwhile(g);
                return true;
            };
            guard.done = [&] { ++given_back; };
            r->set_guard(guard);
            const auto turn = r->turn();
            check(turn == Reviver::Turn::Waiting && g.clicks.empty() && r->tries() == 0 &&
                      given_back == 1,
                  std::string(c.what) + ": nothing clicked, no try counted, the mouse given back");
        }
    }
    std::printf("  [info] the revive checks took %.0f s\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
}

/// The readers that follow the client's size: the chat lines from the chat
/// bar, the player list from its button, the HUD landmark from the staged
/// pictures. The player's client is 1280x720; what was measured was 1024x768.
void test_screen_layout() {
    section("Readers placed from the client's own landmarks");

    const auto same = [](const ScreenRect& a, const ScreenRect& b) {
        return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
    };

    // ---- the chat lines, from the chat bar ----
    {
        // The bar's top-left as the client draws it: 84 in, 22 up.
        const auto small = ChatLines::above_bar(84, 746, 2, 768);
        check(small.size() == 2 && same(small[0], ScreenRect{ 6, 710, 390, 16 }) &&
                  same(small[1], ScreenRect{ 6, 726, 390, 16 }),
              "at 1024x768 the two lines are read at 710 and 726, top first");
        const auto wide = ChatLines::above_bar(84, 698, 2, 720);
        check(wide.size() == 2 && same(wide[0], ScreenRect{ 6, 662, 390, 16 }) &&
                  same(wide[1], ScreenRect{ 6, 678, 390, 16 }),
              "at 1280x720 at 662 and 678, inside the frame");
        bool clear = true;
        for (const ScreenRect& box : ChatLines::above_bar(84, 698, 6, 720))
            if (box.y < 698 - 41) clear = false;
        check(clear && ChatLines::above_bar(84, 698, 6, 720).size() == 2,
              "and asked for six, still only the two below the tab strip: its labels are "
              "never read");
    }
    {
        // A party.ini from before, with the 1024x768 numbers in it: having
        // them is not asking for them.
        IniFile old;
        old.set_int("Chat", "chat_x", 6);
        old.set_int("Chat", "chat_y", 710);
        old.set_int("Chat", "line_height", 13);
        ChatLines::Settings s;
        s.load(old);
        check(!s.fixed && s.lines == 2, "the old chat numbers in party.ini leave the lines placed "
                                        "from the bar");
        old.set("Chat", "place", "fixed");
        ChatLines::Settings f;
        f.load(old);
        ChatLines fixed;
        fixed.set_settings(f);
        const auto boxes = fixed.boxes(make_frame(1024, 768), TemplateStore{});
        check(f.fixed && boxes.size() == 3 && same(boxes[0], ScreenRect{ 6, 710, 390, 16 }) &&
                  fixed.bar() == ChatLines::Bar::Fixed,
              "and place = fixed reads them as written, three lines as it always did");
    }
    {
        TemplateStore store;
        GlyphFont font;
        const bool have = store.load_best(exe_relative("configs/templates.ini")) &&
                          store.get("inven/0/dialog_chat_res3.spr/1") != nullptr &&
                          font.load(GlyphFont::default_dir());
        if (!have) {
            std::printf("  [skip] the chat bar's picture or the glyphs are not installed here\n");
        } else {
            const Frame& bar = *store.get("inven/0/dialog_chat_res3.spr/1");
            // A chat panel: the bar, and two lines written above it in the
            // game's own glyphs, under made-up names.
            const auto panel = [&](int w, int h, int bar_y, bool with_bar) {
                Frame f = make_frame(w, h);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) put_px(f, x, y, 40, 46, 58);
                if (with_bar) blit(f, bar, 84, bar_y);
                // No letter that hangs below the line in the names: drawn
                // here, those sit on it.
                const char* said[] = { "NhanThu xin chao", "NhanThu pt" };
                for (int i = 0; i < 2; ++i) {
                    Frame text;
                    if (!font.render_line(said[i], text)) continue;
                    blit(f, text, 8, bar_y - 34 + 16 * i);
                }
                return f;
            };
            const auto read = [&](ChatLines& lines, const Frame& f) {
                std::vector<std::string> out;
                for (const ScreenRect& box : lines.boxes(f, store)) {
                    const std::string text = font.read_line(f, box);
                    if (!text.empty()) out.push_back(text);
                }
                return out;
            };
            for (const auto& size : { std::pair<int, int>{ 1280, 720 }, std::pair<int, int>{ 1024, 768 } }) {
                const int w = size.first, h = size.second;
                ChatLines lines;
                const Frame f = panel(w, h, h - 22, true);
                const auto got = read(lines, f);
                const std::string at = std::to_string(w) + "x" + std::to_string(h);
                check(lines.bar() == ChatLines::Bar::Found && lines.bar_x() == 84 &&
                          lines.bar_y() == h - 22,
                      "at " + at + " the chat bar is found where the client draws it");
                check(got.size() == 2 && got[1].find("pt") != std::string::npos,
                      "and both lines above it are read, the newest last -- " +
                          std::to_string(got.size()) + " read");
                PartyChat chat;
                PartyChat::Settings rules;
                chat.set_settings(rules);
                std::vector<std::string> asked;
                for (int look = 0; look < 3; ++look)
                    for (const std::string& who : chat.saw(got, 100000 + 200 * look))
                        asked.push_back(who);
                check(asked.size() == 1 && PartyChat::alike(asked[0], "NhanThu") > 0.8,
                      "and the keyword vote sees 'pt', from the one who wrote it");
            }
            {
                // What is said in the log, caught for the length of it.
                Logger::instance().set_level(LogLevel::Info);
                Logger::instance().set_ring_capacity(400);
                Logger::instance().clear_ring();
                ChatLines lines;
                const Frame f = panel(1280, 720, 698, false);
                const auto first = lines.boxes(f, store);
                lines.boxes(f, store);
                Logger::instance().set_level(LogLevel::Error);
                int said = 0;
                for (const LogLine& line : Logger::instance().ring())
                    if (line.text.find("Chat: the chat bar") != std::string::npos) ++said;
                check(lines.bar() == ChatLines::Bar::Assumed && lines.bar_y() == 720 - 22 &&
                          first.size() == 2 && same(first[0], ScreenRect{ 6, 662, 390, 16 }),
                      "with no bar on screen the lines are read where it always is, 22 rows up");
                check(said == 1, "and that is said once in the log, not on every look -- " +
                                     std::to_string(said));
                // Found once, then covered: where it was found last.
                ChatLines seen;
                seen.boxes(panel(1280, 720, 640, true), store);
                seen.boxes(f, store);
                check(seen.bar() == ChatLines::Bar::Remembered && seen.bar_y() == 640,
                      "and a bar seen before and covered now is read where it was last seen");
            }
        }
    }

    // ---- on the player's own frames, where they are here ----
    {
        TemplateStore store;
        Frame wide, small;
        const bool have = store.load_best(exe_relative("configs/templates.ini")) &&
                          store.get("inven/0/dialog_chat_res3.spr/1") != nullptr &&
                          store.get("player_in_map_icon") != nullptr;
        const bool have_wide = have && load_png(exe_relative("frame.png"), wide) &&
                               wide.width == 1280 && wide.height == 720;
        const bool have_small = have && load_png(exe_relative("playerlist.png"), small) &&
                                small.width == 1024 && small.height == 768;
        if (!have_wide && !have_small) {
            std::printf("  [skip] no saved frames of the client here\n");
        } else if (!have_wide || !have_small) {
            // Said, not left out quietly: `gobot_cli --read` and
            // `--playerlist` write these two files, and one taken at another
            // size takes these checks with it.
            std::printf("  [skip] %s is missing here, or not the %s frame -- its checks are left "
                        "out\n",
                        have_wide ? "playerlist.png" : "frame.png",
                        have_wide ? "1024x768" : "1280x720");
        }
        for (const Frame* f : { have_wide ? &wide : nullptr, have_small ? &small : nullptr }) {
            if (f == nullptr) continue;
            const std::string at = std::to_string(f->width) + "x" + std::to_string(f->height);
            ChatLines lines;
            const auto boxes = lines.boxes(*f, store);
            const std::string where = ChatLines::bar_name(lines.bar()) + std::string(" at ") +
                                      std::to_string(lines.bar_x()) + "," +
                                      std::to_string(lines.bar_y());
            check(lines.bar() == ChatLines::Bar::Found && lines.bar_x() == 84 &&
                      lines.bar_y() == f->height - 22 && boxes.size() == 2 && boxes[0].x == 6,
                  "on the player's " + at + " frame the chat bar is found at 84," +
                      std::to_string(f->height - 22) + ", the lines read from x 6 -- " + where);
            const auto button = TemplateMatcher::find(
                *f, ScreenRect{ 0, 0, f->width, f->height }, *store.get("player_in_map_icon"),
                PlayerList::Settings{}.open_score);
            check(button.found && button.at.x == f->width - 68 && button.at.y == 139,
                  "and the player list's button at " + std::to_string(f->width - 68) +
                      ",139, against the right edge");
        }
    }

    // ---- the player list, from its button ----
    {
        PlayerList::Settings measured;
        const PlayerList::Settings small = measured.at_button(956, 139);
        check(small.roi_x == 744 && small.roi_y == 160 && small.roi_w == 276 &&
                  small.roi_h == 214 && small.level_x == 748 && small.name_x == 795 &&
                  small.click_x == 862 && small.first_row_y == 172,
              "the list placed from its button at 956,139 is the list measured on 1024x768");
        const PlayerList::Settings wide = measured.at_button(1212, 139);
        check(wide.roi_x == 1000 && wide.name_x == 1051 && wide.click_x == 1118 &&
                  wide.level_x == 1004 && wide.first_row_y == 172,
              "and from its button at 1212,139, where it is on 1280x720, it is against the "
              "right edge");

        PlayerList list;
        list.set_settings(measured);
        int clicks = 0;
        list.set_click([&](int, int) { ++clicks; return true; });
        InputManager no_window;
        ScreenCapture capture;
        std::string why;
        PlayerList::Row row;
        row.index = 0;
        row.y = 182;
        check(!list.invite(no_window, capture, TemplateStore{}, row, &why) && clicks == 0 &&
                  list.read(make_frame(1280, 720), GlyphFont{}).empty(),
              "nothing is read or clicked before the list is placed from its button");
        list.place(1212, 139, 1280, 720);
        check(list.in_panel(1118, 182) && !list.in_panel(862, 182) && !list.in_panel(1118, 500),
              "placed on 1280x720, a row of it is in the list and the 1024x768 click point "
              "is not");
        row.y = 600;
        why.clear();
        const bool refused = !list.invite(no_window, capture, TemplateStore{}, row, &why);
        check(refused && clicks == 0 && !why.empty(),
              "a row outside the list is refused, with nothing clicked -- " + why);
        row.y = 362;        // the tenth row: its Req Party would be below the list
        check(!list.invite(no_window, capture, TemplateStore{}, row, &why) && clicks == 0,
              "and so is a row whose Req Party would land below the list");

        PlayerList::Settings pinned = measured;
        pinned.fixed = true;
        PlayerList old_way;
        old_way.set_settings(pinned);
        old_way.place(1212, 139, 1280, 720);
        check(old_way.geometry().click_x == 862 && old_way.geometry().roi_x == 744,
              "with place = fixed the numbers in party.ini are used as written");
        IniFile ini;
        ini.set_int("PlayerList", "roi_x", 744);
        PlayerList::Settings loaded;
        loaded.load(ini);
        check(!loaded.fixed, "and a party.ini with only the old numbers is not fixed");
    }

    // ---- the HUD landmark, from the staged pictures ----
    {
        Frame landmark;
        std::string from;
        if (!TemplateStore::load_one("hpmp_indicator2", exe_relative("configs/templates.ini"),
                                     landmark, &from)) {
            std::printf("  [skip] no picture of the HUD landmark here\n");
        } else {
            check(from.find("templates") != std::string::npos &&
                      from.find("hpmp_indicator2") != std::string::npos,
                  "the HUD landmark comes from the pictures staged beside GoBot first");
            Logger::instance().set_level(LogLevel::Info);
            Logger::instance().set_ring_capacity(400);
            Logger::instance().clear_ring();
            VitalsReader reader;
            reader.start();
            const bool anchored = reader.has_anchor();
            reader.stop();
            Logger::instance().set_level(LogLevel::Error);
            bool logged = false;
            for (const LogLine& line : Logger::instance().ring())
                if (line.text.find("Vitals: using the HUD landmark from") != std::string::npos &&
                    line.text.find("templates") != std::string::npos)
                    logged = true;
            check(anchored && logged,
                  "and a started reader takes it from there, and says where it came from");
            Frame shot;
            if (!load_png(exe_relative("frame.png"), shot) || !shot.valid()) {
                std::printf("  [skip] no saved frame of the client here\n");
            } else {
                const auto hit = TemplateMatcher::find(
                    shot, ScreenRect{ 0, 0, std::min(shot.width, 560), std::min(shot.height, 280) },
                    landmark, 0.75f);
                check(hit.found && hit.at.x == 44 && hit.at.y == 4,
                      "on the player's 1280x720 frame it is found at 44,4, where it is on "
                      "1024x768 too");
            }
        }
    }
}

/// A macro engine on a clock of the test's own, with no game window to type
/// into: never in front, so every press is refused at the focus check, as the
/// real input with no window refuses it. `run(ms)` takes a pass of the timers
/// every 10 ms of that clock, as the timer thread does -- so a check counts
/// the test's time, never how much of a loaded CPU a thread got (R7b2-5: one
/// of these failed once under heavy load, on the real threads and a 200-ms
/// sleep).
struct SteppedMacro {
    InputManager         input;
    ExecutionCoordinator coord;
    MacroEngine          engine{ input, coord };
    std::uint64_t        t = 5'000'000;

    SteppedMacro() {
        MacroEngine::Clock clock;
        clock.now_ms = [this] { return t; };
        clock.sleep_ms = [this](std::uint32_t ms) { t += ms; };
        engine.set_clock(clock);
        MacroEngine::Hands hands;
        hands.focused = [] { return false; };
        hands.key = [](std::uint32_t, int) { return true; };
        engine.set_hands(hands);
    }
    ~SteppedMacro() { engine.stop(); }
    void run(std::uint64_t ms) {
        for (const std::uint64_t until = t + ms; t < until; t += 10) engine.step();
    }
};

/// The gate between a reading and a key press.
///
/// Runs with require_focus on, so nothing is actually typed anywhere: what is
/// under test is whether the engine decides to press, which the counters show.
void test_macro_triggers() {
    section("Macro triggers");

    SteppedMacro rig;
    MacroEngine& engine = rig.engine;

    MacroConfig cfg;
    cfg.apply_defaults();
    cfg.keys.resize(1);
    cfg.keys[0].label = "HP potion";
    cfg.keys[0].keycode = '1';
    cfg.keys[0].enabled = true;
    cfg.keys[0].interval_ms = 20;
    cfg.keys[0].jitter_pct = 0;
    cfg.keys[0].trigger = MacroTrigger::HpBelow;
    cfg.keys[0].threshold_pct = 50;
    cfg.sequence_enabled = false;
    cfg.require_focus = true;
    engine.set_config(cfg);

    float hp = 0.0f, mp = 0.0f;
    engine.current_vitals(hp, mp);
    check(hp < 0.0f && mp < 0.0f, "no reading before any is pushed");

    engine.start_stepped();

    // 1. No reading at all: hold, and say which kind of hold it was.
    rig.run(200);
    auto st = engine.stats();
    check(st.held_no_reading > 0, "holds when there is no reading");
    check(st.held_condition == 0, "does not call that a false condition");
    check(st.skipped_unfocused == 0, "never got as far as trying to press");

    // 2. A reading above the threshold: hold, for the other reason.
    engine.set_vitals(90.0f, 90.0f);
    const std::uint64_t no_reading_before = st.held_no_reading;
    rig.run(200);
    st = engine.stats();
    check(st.held_condition > 0, "holds while HP is above the threshold");
    check(st.held_no_reading == no_reading_before, "stops counting it as missing");
    check(st.skipped_unfocused == 0, "still did not try to press");

    // 3. A reading below it: the key fires. With focus required and no window
    //    the press is refused, which is exactly the counter that proves the
    //    gate opened.
    engine.set_vitals(20.0f, 90.0f);
    rig.run(200);
    st = engine.stats();
    check(st.skipped_unfocused > 0, "fires once HP drops below the threshold");
    check(st.presses == 0, "and still sends nothing without a focused window");

    engine.current_vitals(hp, mp);
    check(hp == 20.0f, "the pushed value is what it acts on");

    // 4. Readings expire. A frozen GUI must make the key hold rather than keep
    //    drinking against a figure from a minute ago, so after the staleness
    //    window the engine reports no reading and goes back to holding.
    const std::uint64_t unfocused_before = st.skipped_unfocused;
    const std::uint64_t t_before = rig.t;
    rig.run(1800);
    check(rig.t - t_before >= 1800, "(the pretend clock moved 1800 ms)");
    engine.current_vitals(hp, mp);
    check(hp < 0.0f, "a reading older than the staleness window is discarded");

    st = engine.stats();
    const std::uint64_t no_reading_at_expiry = st.held_no_reading;
    rig.run(200);
    st = engine.stats();
    check(st.held_no_reading > no_reading_at_expiry, "and the key goes back to holding");
    check(st.skipped_unfocused > unfocused_before, "after having fired while fresh");

    engine.stop();

    // A key on the other vital must be independent: MP at 90 with a 30%
    // threshold stays shut while HP is at 20.
    cfg.keys[0].trigger = MacroTrigger::MpBelow;
    cfg.keys[0].threshold_pct = 30;
    engine.set_config(cfg);
    engine.reset_stats();
    engine.start_stepped();
    engine.set_vitals(20.0f, 90.0f);
    rig.run(200);
    st = engine.stats();
    check(st.held_condition > 0, "an MP key ignores HP");
    check(st.skipped_unfocused == 0, "and does not fire on it");
    engine.stop();
}

/// The engine drives real SendInput, so these exercise what can be checked
/// without a window to type into: that it starts and stops cleanly, and that
/// it refuses to press anything when focus is required and absent. What it
/// counts is counted on the test's own clock (SteppedMacro); starting and
/// stopping on the real threads, which assert no timing.
void test_macro_engine() {
    section("Macro engine");

    MacroConfig cfg;
    cfg.apply_defaults();
    cfg.require_focus = true;          // no game window in a test run
    cfg.timers_enabled = true;
    for (auto& k : cfg.keys) {
        k.enabled = true;
        k.interval_ms = 20;            // fire often, so the window is short
    }

    {
        SteppedMacro rig;
        MacroEngine& engine = rig.engine;
        engine.set_config(cfg);

        check(!engine.running(), "starts stopped");
        engine.start_stepped();
        check(engine.running(), "start() runs it");

        rig.run(220);

        const auto st = engine.stats();
        check(st.presses == 0, "presses nothing with no focused window");
        check(st.skipped_unfocused > 0, "counts the skipped presses instead of failing silently");

        const auto due = engine.next_due_ms();
        check(due.size() == cfg.keys.size(), "reports a due time per key");

        // Suspending holds every key without stopping, and says so in the
        // tally rather than letting an hour of nothing look like an hour of
        // working.
        //
        // This is what stands between an overnight run and six hours of
        // potion keys pressed at a disconnect dialog, so it is worth a test
        // that does not depend on a window: with focus required and absent,
        // nothing presses either way, but which counter moves says which rule
        // did the holding.
        {
            // Suspended first, and counted from a moment later, as on the
            // real threads: a pass under way when it was set ends at the
            // focus check.
            engine.set_suspended(true, "testing");
            rig.run(250);
            engine.reset_stats();
            check(engine.suspended(), "suspends");
            check(engine.suspended_reason() == "testing", "and remembers why");

            rig.run(220);
            const auto held = engine.stats();
            check(held.presses == 0, "presses nothing while suspended");
            check(held.held_suspended > 0, "counts what it held back");
            check(held.skipped_unfocused == 0,
                  "and holds before it ever gets as far as the focus check");

            engine.set_suspended(false);
            check(!engine.suspended() && engine.suspended_reason().empty(),
                  "resumes, and forgets why it stopped");

            rig.run(220);
            check(engine.stats().skipped_unfocused > held.skipped_unfocused,
                  "and is trying again afterwards");
        }
        engine.stop();
    }

    // On the real threads: nothing here waits on them to have done anything.
    InputManager input;
    ExecutionCoordinator coord;
    MacroEngine engine(input, coord);
    engine.set_config(cfg);
    engine.start();
    check(engine.running(), "start() runs it, on its own threads");
    engine.stop();
    check(!engine.running(), "stop() halts it");

    // Stopping twice, and stopping when never started, must both be safe:
    // the panic key can arrive at any moment.
    engine.stop();
    check(true, "double stop is safe");

    MacroEngine other(input, coord);
    other.stop();
    check(true, "stopping a never-started engine is safe");

    // Restart must work — the toggle key does exactly this.
    engine.start();
    check(engine.running(), "restarts after a stop");
    engine.stop();
}

void test_macro_key_kind() {
    section("Macro key kinds");

    // The simple screen groups keys by what they are for, and keys carry no
    // role of their own -- so its rows are only as right as this is.
    MacroKey k;
    k.trigger = MacroTrigger::HpBelow;
    check(macro_key_kind(k) == MacroKeyKind::HpPotion, "an HP trigger is an HP potion");
    k.trigger = MacroTrigger::MpBelow;
    check(macro_key_kind(k) == MacroKeyKind::MpPotion, "an MP trigger is an MP potion");

    k.trigger = MacroTrigger::Always;
    k.interval_ms = 1200;
    check(macro_key_kind(k) == MacroKeyKind::Skill, "a short timer is a skill");
    k.interval_ms = kBuffMinIntervalMs - 1;
    check(macro_key_kind(k) == MacroKeyKind::Skill, "just under a minute is still a skill");
    k.interval_ms = kBuffMinIntervalMs;
    check(macro_key_kind(k) == MacroKeyKind::Buff, "a minute is a buff");

    // The simple screen's buff interval goes down to one minute, so a buff
    // edited there must not turn into a skill and jump rows.
    k.interval_ms = 1 * 60000;
    check(macro_key_kind(k) == MacroKeyKind::Buff, "its shortest buff interval stays a buff");

    // A potion on a long cooldown is still a potion: the trigger decides.
    k.trigger = MacroTrigger::HpBelow;
    k.interval_ms = 300000;
    check(macro_key_kind(k) == MacroKeyKind::HpPotion, "the trigger wins over the interval");

    // The shipped defaults land in the rows the simple screen shows.
    MacroConfig cfg;
    cfg.apply_defaults();
    int skills = 0, buffs = 0, hp = 0, mp = 0;
    for (const auto& key : cfg.keys) {
        switch (macro_key_kind(key)) {
            case MacroKeyKind::Skill:    ++skills; break;
            case MacroKeyKind::Buff:     ++buffs;  break;
            case MacroKeyKind::HpPotion: ++hp;     break;
            case MacroKeyKind::MpPotion: ++mp;     break;
        }
    }
    check(skills == 4 && buffs == 2 && hp == 1 && mp == 1,
          "defaults: four skills, two buffs, a potion of each");

    // The per-key tally behind "Bình HP: 3". Nothing reaches a test run --
    // there is no focused game window -- so nothing may be counted against
    // any key, whatever else the engine tried.
    InputManager input;
    ExecutionCoordinator coord;
    MacroEngine engine(input, coord);
    for (auto& key : cfg.keys) { key.enabled = true; key.interval_ms = 20; }
    cfg.require_focus = true;
    engine.set_config(cfg);
    engine.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    engine.stop();
    const auto st = engine.stats();
    check(st.presses == 0, "nothing pressed without a window");
    check(std::all_of(st.key_presses.begin(), st.key_presses.end(),
                      [](std::uint64_t n) { return n == 0; }),
          "and nothing counted against any key");
}

/// The buffs on their own clocks, and cast again after a death: the engine
/// driven a step at a time on a clock of the test's own, with a pretend
/// keyboard -- hours of it in a moment, no thread, no sleep, and the same
/// answer however busy the machine is.
void test_macro_buffs() {
    section("Macro: a clock for each buff, and every buff again after a death");

    // The simple screen's seconds box, and what it writes.
    check(clamp_buff_seconds(59) == 60 && clamp_buff_seconds(9000) == 7200 &&
              clamp_buff_seconds(1600) == 1600 && clamp_buff_seconds(60) == 60,
          "a buff's seconds are held to 60..7200: 59 is 60, 9000 is 7200");
    {
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys.clear();
        const struct { std::uint32_t vk; int seconds; } kBuffs[] = { { 'A', 1600 }, { 'S', 1600 },
                                                                     { 'F', 60 } };
        for (const auto& b : kBuffs) {
            MacroKey k;
            k.keycode = b.vk;
            k.label = std::string(1, static_cast<char>(b.vk));
            k.enabled = true;
            k.jitter_pct = 5;
            k.interval_ms = static_cast<std::uint32_t>(clamp_buff_seconds(b.seconds)) * 1000u;
            cfg.keys.push_back(k);
        }
        const std::string path = temp_file("gobot_test_buffs.ini");
        MacroConfig back;
        IniFile raw;
        const bool ok = cfg.save(path) && back.load(path) && raw.load(path);
        check(ok && back.keys.size() == 3 && back.keys[0].interval_ms == 1600000 &&
                  back.keys[1].interval_ms == 1600000 && back.keys[2].interval_ms == 60000 &&
                  raw.get_int("Keys", "key_2_interval_ms", 0) == 60000,
              "buffs at 1600, 1600 and 60 s are written as 1600000, 1600000 and 60000 ms, and "
              "read back the same");
        check(ok && back.keys.size() == 3 &&
                  std::all_of(back.keys.begin(), back.keys.end(),
                              [](const MacroKey& k) { return macro_key_kind(k) == MacroKeyKind::Buff; }),
              "and all three are still buffs");
        std::remove(path.c_str());
    }

    // The pretend world: a clock, whether the game is in front, and every
    // key that reached it, with when.
    struct Pretend {
        std::uint64_t t = 3'600'000;
        bool focused = true;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> pressed;
        std::vector<std::uint64_t> when(std::uint32_t vk) const {
            std::vector<std::uint64_t> out;
            for (const auto& p : pressed)
                if (p.first == vk) out.push_back(p.second);
            return out;
        }
    };
    struct Rig {
        InputManager input;
        ExecutionCoordinator coord;
        MacroEngine engine{ input, coord };
        Pretend p;
        explicit Rig(std::vector<MacroKey> keys) {
            MacroEngine::Clock c;
            c.now_ms = [this] { return p.t; };
            engine.set_clock(c);
            MacroEngine::Hands h;
            h.focused = [this] { return p.focused; };
            h.key = [this](std::uint32_t vk, int) {
                p.pressed.emplace_back(vk, p.t);
                return true;
            };
            engine.set_hands(h);
            MacroConfig cfg;
            cfg.apply_defaults();
            cfg.keys = std::move(keys);
            cfg.sequence_enabled = false;
            cfg.require_focus = true;
            cfg.global_jitter_pct = 0;
            engine.set_config(cfg);
            engine.start_stepped();
        }
        ~Rig() { engine.stop(); }
        /// `ms` of pretend time, a pass of the timers every 10 ms, as the thread does.
        void run(std::uint64_t ms) {
            for (const std::uint64_t until = p.t + ms; p.t < until; p.t += 10) engine.step();
        }
    };
    const auto key = [](std::uint32_t vk, std::uint32_t interval_ms, int jitter,
                        std::uint32_t delay = 0) {
        MacroKey k;
        k.keycode = vk;
        k.label = std::string(1, static_cast<char>(vk));
        k.enabled = true;
        k.interval_ms = interval_ms;
        k.jitter_pct = jitter;
        k.start_delay_ms = delay;
        return k;
    };
    const auto spaced = [](std::vector<std::uint64_t> times, std::uint64_t gap) {
        std::sort(times.begin(), times.end());
        for (std::size_t i = 1; i < times.size(); ++i)
            if (times[i] - times[i - 1] < gap) return false;
        return true;
    };

    {
        // The player's own: F every 60 s, A every 1600 s.
        Rig rig({ key('F', 60000, 5), key('A', 1600000, 10, 150) });
        const std::uint64_t t0 = rig.p.t;
        rig.run(3600 * 1000);
        const auto f = rig.p.when('F');
        const auto a = rig.p.when('A');
        check(f.size() >= 57 && f.size() <= 63,
              "over an hour a 60-s buff (5% either way) is pressed 57..63 times -- " +
                  std::to_string(f.size()));
        const auto s = [&](std::size_t i) { return static_cast<double>(a[i] - t0) / 1000.0; };
        check(a.size() == 3 && s(0) < 2.0 && s(1) >= 1440.0 && s(1) <= 1765.0 && s(2) >= 2880.0 &&
                  s(2) <= 3530.0,
              "and a 1600-s buff exactly 3 times, at about 0, 1600 and 3200 s -- " +
                  std::to_string(a.size()) + " times");
    }
    {
        // Three buffs set up 150 ms apart, as the simple screen adds them.
        Rig rig({ key('A', 600000, 10, 0), key('S', 600000, 10, 150), key('F', 600000, 10, 300) });
        rig.run(10'000);
        std::vector<std::uint64_t> all;
        for (const auto& p : rig.p.pressed) all.push_back(p.second);
        check(rig.p.pressed.size() == 3 && spaced(all, MacroEngine::kBuffGapMs),
              "at the start every buff goes out once, at least 1.2 s apart");
    }
    {
        // Out of the world for 15 s -- dead, reviving -- then back.
        Rig rig({ key('A', 600000, 10, 0), key('S', 900000, 10, 150), key('F', 1200000, 10, 300) });
        rig.run(100'000);
        rig.p.pressed.clear();
        rig.engine.set_suspended(true, "dead");
        rig.run(15'000);
        const bool held = rig.p.pressed.empty();
        const std::uint64_t back = rig.p.t;
        rig.engine.set_suspended(false);
        rig.run(20'000);
        std::vector<std::uint64_t> all;
        bool each_once = true;
        for (const std::uint32_t vk : { 'A', 'S', 'F' }) {
            const auto t = rig.p.when(vk);
            each_once = each_once && t.size() == 1 && t[0] >= back && t[0] - back <= 5000;
            all.insert(all.end(), t.begin(), t.end());
        }
        check(held && each_once && spaced(all, MacroEngine::kBuffGapMs),
              "after 15 s out of the world every buff is cast once within 5 s of coming back, at "
              "least 1.2 s apart");
        rig.run(500'000);
        check(rig.p.when('A').size() == 1 && rig.p.when('S').size() == 1 && rig.p.when('F').size() == 1,
              "and then waits its whole interval again");
    }
    {
        // A map change: 3 s out of the world adds nothing.
        Rig rig({ key('A', 600000, 10, 0), key('S', 900000, 10, 150) });
        rig.run(100'000);
        rig.p.pressed.clear();
        rig.engine.set_suspended(true, "loading");
        rig.run(3'000);
        rig.engine.set_suspended(false);
        rig.run(20'000);
        check(rig.p.pressed.empty(), "3 s out of the world casts nothing again");
    }
    {
        // Asked for while still out of the world: done the moment it is back.
        Rig rig({ key('A', 600000, 10, 0), key('S', 900000, 10, 150) });
        rig.run(100'000);
        rig.p.pressed.clear();
        rig.engine.set_suspended(true, "dead");
        const bool taken = rig.engine.recast_buffs("a revive");
        rig.run(3'000);
        const bool quiet = rig.p.pressed.empty();
        const std::uint64_t back = rig.p.t;
        rig.engine.set_suspended(false);
        rig.run(10'000);
        const auto a = rig.p.when('A');
        const auto s = rig.p.when('S');
        check(taken && quiet && a.size() == 1 && s.size() == 1 && a[0] - back <= 5000 &&
                  s[0] - back <= 5000,
              "a re-cast asked for out of the world is done as soon as the character is back");
    }
    {
        // One revive asked about twice -- the revive itself, then HP back
        // from 0 a couple of seconds later: once.
        Rig rig({ key('A', 600000, 10, 0), key('S', 900000, 10, 150) });
        rig.run(100'000);
        rig.p.pressed.clear();
        const bool first = rig.engine.recast_buffs("a revive");
        rig.run(2'000);
        const bool second = rig.engine.recast_buffs("HP came back from 0");
        rig.run(25'000);
        check(first && !second && rig.p.when('A').size() == 1 && rig.p.when('S').size() == 1,
              "one revive asked about twice, 2 s apart, casts each buff once");
        // Killed again among whatever killed it, and revived 25 s after the
        // first re-cast: the 30 s this once waited swallowed that, and the
        // buffs stayed off for a whole interval.
        const bool again = rig.engine.recast_buffs("a revive");
        rig.run(10'000);
        check(again && rig.p.when('A').size() == 2 && rig.p.when('S').size() == 2,
              "a second death revived 25 s after the first re-cast casts them again");
    }
    {
        // Quicker than that: HP read at 0 in between, so it is a death of
        // its own however soon it comes.
        Rig rig({ key('A', 600000, 10, 0), key('S', 900000, 10, 150) });
        rig.run(100'000);
        rig.p.pressed.clear();
        rig.engine.set_vitals(80.0f, 50.0f);
        const bool first = rig.engine.recast_buffs("a revive");
        rig.run(2'000);
        rig.engine.set_vitals(0.0f, 50.0f);      // dead again
        rig.run(1'000);
        rig.engine.set_vitals(60.0f, 50.0f);     // and back
        const bool second = rig.engine.recast_buffs("a revive");
        rig.run(15'000);
        check(first && second && rig.p.when('A').size() == 2 && rig.p.when('S').size() == 2,
              "dead again 3 s after a revive -- HP read at 0 between -- is re-cast too");
    }
    {
        // Killed and revived 20 s after BẮT ĐẦU: a start is not a re-cast.
        Rig rig({ key('A', 1600000, 10, 0), key('S', 1600000, 10, 150) });
        rig.run(20'000);
        const auto at_start = rig.p.pressed.size();
        const bool taken = rig.engine.recast_buffs("a revive");
        rig.run(10'000);
        check(at_start == 2 && taken && rig.p.when('A').size() == 2 && rig.p.when('S').size() == 2,
              "a death and revive 20 s after the start is re-cast, not swallowed by the start");
    }
    {
        // A buff's seconds box turned down during a run: due at its last
        // press plus the new interval, not when the old one runs out.
        Rig rig({ key('F', 300000, 0, 0) });
        rig.run(10'000);
        const auto first = rig.p.when('F');
        MacroConfig cfg = rig.engine.config();
        cfg.keys[0].interval_ms = 60000;
        rig.engine.set_config(cfg);
        rig.run(60'000);
        const auto f = rig.p.when('F');
        check(first.size() == 1 && f.size() == 2 && f[1] - f[0] >= 59'000 && f[1] - f[0] <= 61'000,
              "a buff turned down from 300 s to 60 s during a run goes out 60 s after its last "
              "press -- " + std::to_string(f.size() == 2 ? (f[1] - f[0]) / 1000 : 0) + " s");
        cfg.keys[0].interval_ms = 1600000;
        rig.engine.set_config(cfg);
        rig.run(70'000);
        check(rig.p.when('F').size() == 3,
              "and turned up again, it goes out once more where it was due, then waits the longer "
              "interval");
    }
    {
        // The keys held for 40 s by a dialog -- the trash pass's hold: the
        // buff that fell due then goes out right after, the skill keeps to
        // its own beat.
        Rig rig({ key('Z', 5000, 0, 0), key('F', 60000, 0, 0) });
        const std::uint64_t t0 = rig.p.t;
        rig.run(59'500);
        rig.coord.engage_critical("trash cleaning");
        const std::size_t before = rig.p.pressed.size();
        rig.run(40'000);
        const bool none_held = rig.p.pressed.size() == before;
        rig.coord.disengage_critical();
        const std::uint64_t freed = rig.p.t;
        rig.run(20'000);
        std::uint64_t first_f = 0;
        for (const std::uint64_t t : rig.p.when('F'))
            if (t >= freed) { first_f = t; break; }
        check(none_held && first_f != 0 && first_f - freed <= 3000,
              "a buff that fell due while the keys were held for 40 s goes out within 3 s of "
              "the hold ending -- " + std::to_string(first_f ? first_f - freed : 0) + " ms");
        const auto z = rig.p.when('Z');
        check(!z.empty() && std::all_of(z.begin(), z.end(),
                                        [&](std::uint64_t t) { return (t - t0) % 5000 == 0; }),
              "while a skill refused the same way keeps to its own 5-s beat, as before");
    }
    {
        // The game not in front: a buff is tried again every couple of
        // seconds, not a whole interval later.
        Rig rig({ key('F', 60000, 5, 0) });
        rig.p.focused = false;
        rig.run(10'000);
        const bool none = rig.p.pressed.empty();
        rig.p.focused = true;
        const std::uint64_t front = rig.p.t;
        rig.run(5'000);
        const auto f = rig.p.when('F');
        check(none && f.size() == 1 && f[0] - front <= 2200,
              "a buff refused for want of the game in front goes out within ~2 s of it coming "
              "back");
    }
}

/// The executor paused, as the GUI pauses it after a stop that may have left
/// the trash bin's quantity dialog open.
///
/// Its buff and potion threads run from launch, and the only thing that kept
/// them out of that dialog's box was the trash pass's hold on the
/// coordinator, which the stop ends. Nothing reaches the keyboard in a test
/// run -- there is no game window -- so what is watched is whether it asks
/// the coordinator for the keys at all, which it does before it looks for
/// the window: something holding them at the lowest priority loses them to
/// a buff or a potion that asks.
void test_executor_pause() {
    section("Executor: paused while the bin may be asking");

    BotConfig cfg;
    cfg.auto_buff = true;
    cfg.buffs[0].enabled = true;
    cfg.buffs[0].keycode = '6';
    cfg.buffs[0].cooldown_sec = 60;          // the live config's buff
    cfg.hp_heal1.enabled = true;
    cfg.hp_heal1.keycode = '1';
    cfg.hp_heal1.trigger_pct = 50;
    GameState state;
    state.update_self_hp(10, 100);           // a potion is due as well
    InputManager input;
    ExecutionCoordinator coord;

    // What the hold did, and why it is not enough: while the trash pass
    // holds everything off a buff is refused, and once a stop ends that hold
    // nothing refuses it.
    coord.engage_critical("trash cleaning");
    check(!coord.request("buff", Priority::Buff), "a buff is refused while the pass holds the keys");
    coord.disengage_critical();
    check(coord.request("buff", Priority::Buff), "and let through once the stop ends that hold");
    coord.release("buff");

    CommandExecutor exec(cfg, state, input, coord);
    exec.set_paused(true);
    check(exec.paused(), "pauses");
    check(coord.request("test", Priority::Idle), "something low holds the keys");
    exec.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    const auto held = exec.stats();
    check(coord.held() && coord.current_owner() == "test",
          "paused, neither the buff timer nor the potion asks for the keys -- held by '" +
              coord.current_owner() + "'");
    check(held.held_paused > 0 && held.buffs_cast == 0 && held.heals == 0,
          "and what fell due is counted as held -- " + std::to_string(held.held_paused));
    check(!exec.execute_now(Command::buff('6', 0)) && coord.current_owner() == "test",
          "a command is refused before it asks for the keys");

    // Let go: the buff is still due, and now it asks.
    exec.set_paused(false);
    bool asked = false;
    for (int i = 0; i < 40 && !asked; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        asked = coord.current_owner() != "test";
    }
    check(asked, "let go, it asks for the keys again");
    exec.stop();
    check(exec.stats().buffs_cast == 0 && exec.stats().heals == 0,
          "and with no game window, still nothing is pressed");
}

/// The packet bot's own buff timer in a run that reads the screen.
///
/// The executor starts with the GUI, and its buff timer used to ask for the
/// keys -- and press bot_default.ini's buff keys whenever the game was in
/// front -- with nothing started at all: only the buffs set on the simple
/// screen are the player's. Its heal loop already waited for the character's
/// own HP from packets, which a screen run never has.
void test_executor_screen_only() {
    section("Executor: no buffs of its own without the packets' picture of the character");

    BotConfig cfg;
    cfg.auto_buff = true;
    cfg.buffs[0].enabled = true;
    cfg.buffs[0].keycode = '6';
    cfg.buffs[0].cooldown_sec = 60;          // due at once
    GameState state;                         // no HP of the character's own
    InputManager input;
    ExecutionCoordinator coord;
    check(coord.request("test", Priority::Idle), "something low holds the keys");

    CommandExecutor exec(cfg, state, input, coord);
    exec.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    check(coord.current_owner() == "test" && exec.stats().buffs_cast == 0,
          "with no packet HP its buff timer never asks for the keys -- held by '" +
              coord.current_owner() + "'");

    // The packet bot running: its HP known, and the buff asks as before.
    state.update_self_hp(100, 100);
    bool asked = false;
    for (int i = 0; i < 40 && !asked; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        asked = coord.current_owner() != "test";
    }
    check(asked, "with the character's HP from packets, the buff asks for the keys as before");
    exec.stop();
}

// -------------------------------------------------------------- auto chat

namespace {

/// A pretend game for the chat sender's hands and look: the chat bar in the
/// bottom-left, drawn from the game's own pictures, open or shut. Enter opens
/// the shut line and sends and shuts the open one; Backspace rubs a character
/// out; a letter key puts one in -- unless `ignore_text`, when the game takes
/// the key but shows nothing, as it did Unicode input in the bin's box. Every
/// Enter pressed while a dialog is up and every key pressed while the line is
/// shut is counted: the sender is to press none.
///
/// It keeps a clock of its own, which waits and key holds move on. A key acts
/// `lag` ms after it is pressed, as the live game takes a key a frame or more
/// later -- the Enter that sends the line `send_lag` ms after, when that is
/// set, and every key behind a late one waits for it; a look shows the game
/// as it is when it is taken.
struct PretendChat {
    const TemplateStore* store = nullptr;
    const ChatSettings*  settings = nullptr;
    // Where the open picture's top-left sits. By 690 the 24-pixel-tall bar
    // ends at 713, inside a 720-tall frame, with its divider at frame row 695
    // -- the corner look_chat_bar searches is the bottom 145 rows.
    int bx = 0, by = 690;
    enum class State { Shut, Open } state = State::Shut;
    int  typed = 0;            ///< characters in the open line now
    bool focused = true;
    bool caps = false;
    std::string dialog;        ///< a dialog is up: its name, for dialog_up
    bool ignore_text = false;  ///< the game shows nothing for a typed character
    int  draft = 0;            ///< characters a shut line still holds, shown when it opens
    int  shut_after = -1;      ///< the game shuts the line after this many letters
    bool hide_on_shut = false; ///< and the bar goes out of sight as it does
    bool deaf = false;         ///< Enter on the shut line does nothing
    bool stays_open = false;   ///< Enter on the open line sends it, and it stays open
    int  field_grey = 49;      ///< the open field's grey: GoC's picture's is 49
    int  flicker = -1;         ///< this frame, counted from 1, shows the line shut whatever it is
    bool hidden = false;       ///< the bar out of sight: a window over the corner
    int  hide_after_enters = -1;   ///< the bar goes out of sight once this many Enters have acted
    int  stuck_ink = 0;        ///< something drawn this far into the open field that Backspace does not remove
    int  lag = 0;              ///< ms from a key's press to the game acting on it
    int  send_lag = 0;         ///< and for the Enter that sends the open line (text in it): this instead of `lag`
    int  dialog_ms = 0;        ///< ms a look for the dialogs takes
    int  draw_lag = 0;         ///< ms from a character going into the line to the game drawing it
    int  drop_first = 0;       ///< the first this many Enters on the shut line do nothing
    int  drop_every = 0;       ///< and every this many-th, from the first: 2 drops the 1st, 3rd, ...
    int  ignore_first_letters = 0; ///< the first this many letters go in as nothing
    int  caret_shift = 0;      ///< with ignore_text: each letter moves the caret this far, nothing more
    int  px_per_char = 5;      ///< how far each character reaches in the field
    int  shut_enters = 0;      ///< Enters that came to the shut line
    int  caret_moved = 0;
    std::vector<std::uint64_t> letter_at;              ///< when each character went in, for draw_lag
    std::vector<std::pair<std::uint32_t, int>> holds;  ///< every key pressed, and its hold
    /// The pretend clock at which `to_cancel` is told to stop: DỪNG at a moment.
    std::uint64_t cancel_at = ~0ull;
    ChatSender*   to_cancel = nullptr;
    /// Called after every key is pressed, with its key code: a test's stop, at a moment.
    std::function<void(std::uint32_t)> after_key;

    std::vector<int> sent;     ///< how many characters each sent line had
    std::vector<std::uint64_t> sent_at;    ///< the pretend clock at which the game sent each line
    std::uint64_t acted_at = 0;            ///< the clock a key is acted on at: when it was due, not when the wait ended
    int enters_with_dialog = 0;    ///< Enter pressed with a dialog up -- must stay 0
    int keys_while_shut = 0;       ///< a letter or Backspace with the line shut
    int enters = 0, backspaces = 0, letters = 0;
    mutable int frames = 0;        ///< frames drawn so far
    std::uint64_t clock = 0;       ///< the pretend clock, in ms
    std::vector<std::pair<std::uint64_t, std::uint32_t>> queue;   ///< pressed, not acted on yet

    void act(std::uint32_t vk) {
        if (vk == VK_RETURN) {
            ++enters;
            if (!dialog.empty()) {
                ++enters_with_dialog;
            } else if (state == State::Shut) {
                ++shut_enters;
                const bool dropped = deaf || shut_enters <= drop_first ||
                                     (drop_every > 0 && (shut_enters - 1) % drop_every == 0);
                if (!dropped) { state = State::Open; typed = draft; }
            } else {
                if (typed > 0) {
                    sent.push_back(typed);
                    sent_at.push_back(acted_at);
                }
                typed = 0;
                letter_at.clear();
                caret_moved = 0;
                if (!stays_open) state = State::Shut;
            }
            if (hide_after_enters >= 0 && enters >= hide_after_enters) hidden = true;
        } else if (vk == VK_BACK) {
            ++backspaces;
            if (state == State::Open) {
                if (typed > 0) --typed;
                if (!letter_at.empty()) letter_at.pop_back();
                caret_moved = 0;
            } else {
                ++keys_while_shut;
            }
        } else {
            ++letters;
            if (state == State::Open) {
                if (ignore_first_letters > 0) {
                    --ignore_first_letters;
                } else if (!ignore_text) {
                    ++typed;
                    letter_at.push_back(clock);
                } else {
                    caret_moved += caret_shift;
                }
                if (shut_after >= 0 && letters >= shut_after) {
                    state = State::Shut;
                    if (hide_on_shut) hidden = true;
                }
            } else {
                ++keys_while_shut;
            }
        }
    }

    void advance(std::uint64_t ms) {
        clock += ms;
        while (!queue.empty() && queue.front().first <= clock) {
            const std::uint32_t vk = queue.front().second;
            acted_at = queue.front().first;
            queue.erase(queue.begin());
            act(vk);
        }
        if (to_cancel != nullptr && clock >= cancel_at) to_cancel->cancel();
    }

    Frame render() const {
        // The screen around the bar, made once: a frame this size is slow to
        // fill, and the sender looks hundreds of times.
        static const Frame background = [] {
            Frame f = make_frame(1280, 720);
            for (std::size_t i = 0; i < f.pixels.size(); i += 4) {
                f.pixels[i] = 20; f.pixels[i + 1] = 22; f.pixels[i + 2] = 30; f.pixels[i + 3] = 255;
            }
            return f;
        }();
        Frame f = background;
        if (hidden) return f;
        const Frame* open = store->get(settings->open_picture);
        const Frame* shut = store->get(settings->closed_picture);
        blit(f, *open, bx, by);        // the bar frame, and a dark open field
        const bool flickers = ++frames == flicker;
        if ((state == State::Shut || flickers) && shut != nullptr) {
            // The light-grey closed field over the open one, where it sits for
            // the real client: five right of the divider, two up.
            blit(f, *shut, bx + 85, by + 3);
        } else {
            // The field in another grey than GoC's picture has, when asked.
            if (field_grey != 49)
                fill(f, ScreenRect{ bx + 84, by + 3, 308, 15 }, field_grey, field_grey, field_grey);
            // A caret, and a mark for each character, so the field shows ink
            // that grows with what was typed -- as far as the field goes. A
            // character the game has not drawn yet (draw_lag) is not there.
            int shown = typed;
            if (draw_lag > 0)
                for (const std::uint64_t at : letter_at)
                    if (at + static_cast<std::uint64_t>(draw_lag) > clock) --shown;
            shown = std::max(0, shown);
            for (int i = 0; i <= shown && 88 + i * px_per_char < 84 + 308; ++i)
                for (int y = by + 4; y < by + 17; ++y)
                    put_px(f, bx + 88 + i * px_per_char, y, 0, 0, 0);
            if (caret_moved > 0 && 88 + caret_moved < 84 + 308)
                for (int y = by + 4; y < by + 17; ++y) put_px(f, bx + 88 + caret_moved, y, 0, 0, 0);
            if (stuck_ink > 0)
                for (int y = by + 4; y < by + 17; ++y) put_px(f, bx + 84 + stuck_ink - 1, y, 0, 0, 0);
        }
        return f;
    }

    std::string dialog_up(const Frame&) {
        if (dialog_ms > 0) advance(static_cast<std::uint64_t>(dialog_ms));
        return dialog;
    }

    ChatSender::Hands hands() {
        ChatSender::Hands h;
        h.key = [this](std::uint32_t vk, bool, int hold) -> bool {
            if (!focused) return false;
            holds.emplace_back(vk, hold);
            // The Enter that sends the open line is late by send_lag when that
            // is set; every key behind a late one waits for it (keys keep
            // their order).
            const bool sending = vk == VK_RETURN && state == State::Open && typed > 0;
            const int delay = sending && send_lag > 0 ? send_lag : lag;
            if (delay > 0 || !queue.empty()) {
                queue.emplace_back(clock + static_cast<std::uint64_t>(delay), vk);
            } else {
                acted_at = clock;
                act(vk);
            }
            if (after_key) after_key(vk);
            advance(static_cast<std::uint64_t>(std::max(0, hold)));
            return true;
        };
        h.text = [this](const std::wstring& t, ChatMethod) -> bool {
            if (!focused || state != State::Open) return false;
            if (!ignore_text) typed += static_cast<int>(t.size());
            return true;
        };
        h.focused = [this] { return focused; };
        h.caps_lock = [this] { return caps; };
        h.sleep = [this](int ms) { advance(static_cast<std::uint64_t>(std::max(0, ms))); };
        h.now = [this] { return clock; };
        return h;
    }
};

} // namespace

void test_auto_chat() {
    section("Auto chat");

    // ---- CA1: text preparation ----
    const auto vk = [](const std::string& s) {
        return prepare_chat(s, ChatMethod::Vk, true, 60);
    };
    check(vk("Mua đồ giá rẻ, inbox!").typed == "Mua do gia re inbox",
          "marks off, punctuation out, spaces tidied");
    check(vk("ĐỘI 5 cần người").typed == "DOI 5 can nguoi", "capitals and đ, with a digit");
    {
        const ChatText t = vk(std::string(70, 'a'));
        check(t.typed.size() == 60 && t.cut, "a 70-character line is cut to 60");
    }
    check(vk("!!!").typed.empty(), "a line of only punctuation is left with nothing");
    check(vk("a\t\tb   c").typed == "a b c", "tabs and double spaces collapse to one");
    check(vk("  giữa  ").typed == "giua", "ends trimmed, marks off");
    {
        const ChatText t = vk("xin chào");
        check(t.stripped && !t.dropped && t.typed == "xin chao", "a stripped line is marked stripped");
    }
    // Composed and decomposed Vietnamese come out the same.
    check(prepare_chat("che\xCC\x82\x31", ChatMethod::Vk, true, 60).typed ==
          prepare_chat("ch\xC3\xAA\x31", ChatMethod::Vk, true, 60).typed,
          "a mark typed on its own is the same letter as one built in");
    // Unicode keeps the marks but still drops control characters.
    {
        const ChatText u = prepare_chat("thử tiếng Việt", ChatMethod::Unicode, false, 60);
        check(u.typed == "thử tiếng Việt" && !u.stripped, "unicode keeps the marks");
        const ChatText uk = prepare_chat("thử", ChatMethod::Unicode, true, 60);
        check(uk.typed == "thu", "strip_diacritics=true strips even for unicode");
        // As shipped, the marks come off for vk only: the other methods are
        // there to find out whether the game takes them.
        const ChatSettings shipped;
        check(!shipped.strip_diacritics, "strip_diacritics is off unless chat.ini turns it on");
        check(prepare_chat("thử tiếng Việt", ChatMethod::Unicode, shipped.strip_diacritics, 60).typed ==
                  "thử tiếng Việt",
              "so unicode keeps the marks by default");
        check(prepare_chat("thử tiếng Việt", ChatMethod::Vk, shipped.strip_diacritics, 60).typed ==
                  "thu tieng Viet",
              "while vk takes them off all the same, having no keys for them");
    }

    // ---- CA2: the key plan ----
    {
        const auto keys = chat_keys("Ab 1", false);
        check(keys.size() == 4 && keys[0].vk == 'A' && keys[0].shift &&
              keys[1].vk == 'B' && !keys[1].shift &&
              keys[2].vk == VK_SPACE && !keys[2].shift &&
              keys[3].vk == '1' && !keys[3].shift,
              "Caps off: Shift for the capital, none for the rest");
        const auto on = chat_keys("Ab 1", true);
        check(on.size() == 4 && on[0].vk == 'A' && !on[0].shift && on[1].vk == 'B' && on[1].shift,
              "Caps on: the small letter gets Shift, the capital none");
        bool digit_or_space_shifted = false;
        for (const ChatKey& k : chat_keys("A b 9 Z", false))
            if ((k.vk == VK_SPACE || (k.vk >= '0' && k.vk <= '9')) && k.shift)
                digit_or_space_shifted = true;
        check(!digit_or_space_shifted, "never Shift around a digit or a space");
        bool every_scan = true;
        for (const ChatKey& k : chat_keys("Hello 123", false))
            every_scan = every_scan && InputManager::scan_code(k.vk) != 0;
        check(every_scan, "every key has a scan code");
    }

    // ---- CA3: the scheduler ----
    {
        ChatSettings s;
        s.interval_s = 10;      // asked below 30
        s.first_delay_s = 15;
        s.lines = { "a", "b", "c" };
        ChatSchedule sch(1234);
        sch.start(1000, s);
        check(sch.due_at() == 1000 + 15'000, "the first line comes first_delay_s after start");
        check(!sch.due(1000 + 14'000) && sch.due(1000 + 15'000), "and not before");
        check(sch.turn() == 0, "in turn, the first line is line 1");
        std::vector<int> order;
        std::uint64_t now = sch.due_at();
        for (int i = 0; i < 4; ++i) { order.push_back(sch.turn()); sch.sent(now); now = sch.due_at(); }
        check((order == std::vector<int>{ 0, 1, 2, 0 }), "in turn it cycles 1, 2, 3, 1");
        check(sch.draw_gap_ms() >= 30'000, "an interval of 10 is floored at 30 s");
    }
    {
        // random never repeats the previous line over 1,000 draws.
        ChatSettings s;
        s.random_order = true;
        s.lines = { "a", "b", "c", "d" };
        ChatSchedule sch(99);
        int last = -1;
        bool repeated = false;
        std::uint64_t now = 0;
        sch.start(now, s);
        for (int i = 0; i < 1000; ++i) {
            const int t = sch.turn();
            if (t == last) repeated = true;
            last = t;
            now = sch.due_at();
            sch.sent(now);
        }
        check(!repeated, "random never says the same line twice in a row");
    }
    {
        // Jitter stays within the percentage; a postponed line keeps its turn.
        ChatSettings s;
        s.interval_s = 100;
        s.jitter_pct = 15;
        s.lines = { "a", "b" };
        ChatSchedule sch(7);
        sch.start(0, s);
        bool within = true;
        for (int i = 0; i < 500; ++i) {
            const std::uint64_t g = sch.draw_gap_ms();
            within = within && g >= 85'000 && g <= 115'000;
        }
        check(within, "the jitter stays within +/-15%");
        sch.sent(0);
        const int turn = sch.turn();
        sch.postpone(0, 5000);
        check(sch.turn() == turn && sch.due_at() == 5000, "a postponed line keeps its turn");
    }

    // ---- CA4: the gate ----
    {
        const auto base = [] {
            ChatGateInput in;
            in.window = in.focused = in.looked = in.hud = true;
            in.bar = ChatBarState::Closed;
            in.hp_pct = 90;
            in.hp_floor = 50;
            return in;
        };
        check(chat_gate(base()).send, "everything clear: send");
        { auto in = base(); in.dialog = "thùng rác"; check(!chat_gate(in).send, "a dialog up: wait"); }
        { auto in = base(); in.trash = true; check(!chat_gate(in).send, "a trash pass or hold: wait"); }
        { auto in = base(); in.held_by = "reviving"; check(!chat_gate(in).send, "another hold: wait"); }
        { auto in = base(); in.hp_pct = 30;
          const ChatGate g = chat_gate(in);
          check(!g.send && g.wait_ms == 2000, "HP 30% with floor 50: wait 2 s"); }
        { auto in = base(); in.bar = ChatBarState::Open;
          const ChatGate g = chat_gate(in);
          check(!g.send && g.wait_ms == 15000, "the line already open: wait 15 s"); }
        { auto in = base(); in.focused = false; check(!chat_gate(in).send, "the game not in front: wait"); }
        { auto in = base(); in.bar = ChatBarState::Unknown;
          const ChatGate g = chat_gate(in);
          check(!g.send && g.why.find("ô chat") != std::string::npos,
                "the bar not found: wait with the 'chưa thấy ô chat' message"); }
        { auto in = base(); in.user_active = true; check(!chat_gate(in).send, "a person typing: wait"); }
    }

    // ---- CA6: the config round-trips ----
    {
        const std::string path = temp_file("gobot_chat.ini");
        std::remove(path.c_str());
        ChatSettings missing;
        check(!missing.load(path), "a missing chat.ini does not load, so Tự chat is off");

        check(ChatSettings::save_key(path, "enabled", "true"), "a key is written, making the file");
        check(ChatSettings::save_key(path, "interval_s", "45"), "interval");
        check(ChatSettings::save_key(path, "order", "random"), "order");
        check(ChatSettings::save_key(path, "method", "wmchar"), "method");
        const std::vector<std::string> lines = { "Mua đồ giá rẻ", "pt nào", "" };
        check(ChatSettings::save_lines(path, lines), "the lines are written");

        ChatSettings got;
        check(got.load(path), "and it reads back");
        check(got.enabled && got.interval_s == 45 && got.random_order &&
              got.method == ChatMethod::WmChar && got.method_note.empty(), "every setting survived");
        check(got.lines.size() == 2 && got.lines[0] == "Mua đồ giá rẻ" && got.lines[1] == "pt nào",
              "the Vietnamese lines survived and the empty one was dropped");

        // Writing the lines again replaces them rather than stacking up.
        check(ChatSettings::save_lines(path, { "chỉ một câu" }), "the lines are written again");
        ChatSettings again;
        again.load(path);
        check(again.lines.size() == 1 && again.lines[0] == "chỉ một câu" && again.enabled,
              "the old lines are gone and the other settings are kept");

        // Out-of-range values are brought into range on load.
        ChatSettings::save_key(path, "interval_s", "5");
        ChatSettings::save_key(path, "jitter_pct", "90");
        ChatSettings floor;
        floor.load(path);
        check(floor.interval_s == 30 && floor.jitter_pct == 30, "values out of range are clamped");
        std::remove(path.c_str());
    }

    // ---- chat.ini edited by hand ----
    {
        const std::string path = temp_file("gobot_chat_hand.ini");
        const auto write = [&](const std::string& bytes) {
            std::ofstream o(path, std::ios::binary | std::ios::trunc);
            o << bytes;
        };
        const auto read = [&] {
            std::ifstream in(path, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };

        // Notepad's "UTF-8 with BOM", [Chat] on the first line, sections and
        // keys in another case.
        write("\xEF\xBB\xBF[chat]\r\nEnabled=true\r\nINTERVAL_S=90\r\nempty_ink=3\r\n\r\n"
              "[LINES]\r\nLINE2=hai\r\nline1=một\r\n");
        ChatSettings hand;
        check(hand.load(path), "a BOM before [Chat] on the first line does not hide it");
        check(hand.enabled && hand.interval_s == 90, "sections and keys are read in any case");
        check(hand.lines.size() == 2 && hand.lines[0] == "một" && hand.lines[1] == "hai",
              "LINE2= counts as line2, in its turn");
        check(hand.empty_ink == 3, "empty_ink is read");

        // Saved by the tab, it stays a CRLF file with its BOM, every key in place.
        check(ChatSettings::save_key(path, "interval_s", "120"), "a key saved into it");
        check(ChatSettings::save_key(path, "order", "random"), "and a key it did not have");
        check(ChatSettings::save_lines(path, { "ba" }), "and its lines");
        const std::string after = read();
        std::size_t lf = 0, crlf = 0;
        for (std::size_t i = 0; i < after.size(); ++i)
            if (after[i] == '\n') { ++lf; if (i > 0 && after[i - 1] == '\r') ++crlf; }
        check(after.compare(0, 3, "\xEF\xBB\xBF") == 0, "the BOM is kept");
        check(lf > 0 && lf == crlf, "every line still ends CRLF");
        check(after.find("INTERVAL_S") == std::string::npos &&
                  after.find("interval_s=120") != std::string::npos &&
                  after.find("Enabled=true") != std::string::npos,
              "the key was replaced where it was, whatever its case, and the rest left as written");
        ChatSettings back;
        check(back.load(path) && back.interval_s == 120 && back.random_order && back.enabled &&
                  back.lines.size() == 1 && back.lines[0] == "ba",
              "and it all reads back");
        check(GetFileAttributesA((path + ".tmp").c_str()) == INVALID_FILE_ATTRIBUTES,
              "no .tmp file is left beside it");

        // A file of LF lines stays LF.
        write("[Chat]\nenabled=false\n");
        ChatSettings::save_key(path, "enabled", "true");
        check(read() == "[Chat]\nenabled=true\n", "an LF file stays LF, with no BOM added");

        // empty_ink is brought into range.
        write("[Chat]\nempty_ink=500\n");
        ChatSettings wide;
        wide.load(path);
        write("[Chat]\nempty_ink=0\n");
        ChatSettings narrow;
        narrow.load(path);
        check(wide.empty_ink == 40 && narrow.empty_ink == 1, "empty_ink is kept within 1 to 40");

        // Numbers as people write them: a leading zero is not octal, hex
        // after 0x still works, and a decimal comma is a point.
        write("[Chat]\ninterval_s=090\nmax_length=0120\njitter_pct=0x0A\nclosed_score=0,95\n");
        ChatSettings nums;
        check(nums.load(path) && nums.interval_s == 90 && nums.max_length == 120 &&
                  nums.jitter_pct == 10 && std::abs(nums.closed_score - 0.95f) < 0.001f,
              "090 is ninety, 0120 a hundred and twenty, 0x0A ten, and 0,95 is 0.95");

        // Notepad's "Unicode": UTF-16 with its BOM, read, and saved as it was.
        {
            const std::wstring wide_text = L"[Chat]\r\nenabled=true\r\ninterval_s=75\r\n\r\n[Lines]\r\nline1=một\r\n";
            std::string bytes = "\xFF\xFE";
            for (const wchar_t c : wide_text) {
                bytes += static_cast<char>(c & 0xFF);
                bytes += static_cast<char>((c >> 8) & 0xFF);
            }
            write(bytes);
            ChatSettings u;
            check(u.load(path) && u.enabled && u.interval_s == 75 && u.lines.size() == 1 &&
                      u.lines[0] == "một",
                  "a UTF-16 chat.ini is read, Vietnamese and all");
            check(ChatSettings::save_key(path, "interval_s", "90") &&
                      ChatSettings::save_lines(path, { "một", "hai" }),
                  "keys and lines are saved into it");
            const std::string saved = read();
            check(saved.compare(0, 2, "\xFF\xFE") == 0 && saved.find("interval_s") == std::string::npos,
                  "and it is still UTF-16, with nothing in UTF-8 added to it");
            ChatSettings u2;
            check(u2.load(path) && u2.interval_s == 90 && u2.enabled && u2.lines.size() == 2 &&
                      u2.lines[0] == "một" && u2.lines[1] == "hai",
                  "and it reads back");
        }

        // A method no longer offered, or none at all, is typed with vk -- and
        // the tab is told why.
        write("[Chat]\nmethod=paste\n");
        ChatSettings pasted;
        check(pasted.load(path) && pasted.method == ChatMethod::Vk && !pasted.method_note.empty(),
              "method=paste, no longer offered, is typed with vk, and says so");
        write("[Chat]\nmethod=sendkeys\n");
        ChatSettings odd;
        check(odd.load(path) && odd.method == ChatMethod::Vk && !odd.method_note.empty(),
              "so is a method that never was");
        write("[Chat]\nmethod=Unicode\n");
        ChatSettings uni;
        check(uni.load(path) && uni.method == ChatMethod::Unicode && uni.method_note.empty(),
              "a method still offered is taken, with nothing to say");
        std::remove(path.c_str());
    }

    // ---- round 8: how the keys of a line go, set in chat.ini for the live
    //      test of why the game took half of them on 2026-10-20 ----
    {
        const std::string path = temp_file("gobot_chat_keys.ini");
        const auto write = [&](const std::string& bytes) {
            std::ofstream o(path, std::ios::binary | std::ios::trunc);
            o << bytes;
        };
        ChatSettings shipped_like;
        check(shipped_like.hold_min_ms == 0 && shipped_like.hold_max_ms == 0 && shipped_like.enter_gap_ms == 750 &&
                  shipped_like.quiet_before_ms == 300 && shipped_like.type_wait_ms == 0 &&
                  shipped_like.max_enters == 2 && shipped_like.type_tries == 1 &&
                  shipped_like.inject == InputManager::KeyInject::Vk,
              "as shipped: each key its own hold, an Enter watched 750 ms, 300 ms quiet, no wait before typing, "
              "2 Enters, 1 typing, vk -- what GoBot has always done");
        write("[Chat]\nhold_min_ms=150\nhold_max_ms=250\nenter_gap_ms=1000\nquiet_before_ms=1500\n"
              "type_wait_ms=200\nmax_enters=3\ntype_tries=2\ninject=scancode\n");
        ChatSettings set;
        check(set.load(path) && set.hold_min_ms == 150 && set.hold_max_ms == 250 && set.enter_gap_ms == 1000 &&
                  set.quiet_before_ms == 1500 && set.type_wait_ms == 200 && set.max_enters == 3 &&
                  set.type_tries == 2 && set.inject == InputManager::KeyInject::Scancode,
              "each read back as written");
        write("[Chat]\nhold_min_ms=5\nhold_max_ms=9000\nenter_gap_ms=10\nquiet_before_ms=-3\n"
              "type_wait_ms=99999\nmax_enters=9\ntype_tries=0\ninject=paste\n");
        ChatSettings wild;
        check(wild.load(path) && wild.hold_min_ms == 10 && wild.hold_max_ms == ChatSettings::kMaxHold &&
                  wild.enter_gap_ms == 250 && wild.quiet_before_ms == 0 && wild.type_wait_ms == 3000 &&
                  wild.max_enters == ChatSettings::kMaxEnters && wild.type_tries == 1 &&
                  wild.inject == InputManager::KeyInject::Vk,
              "out of range: brought into range, and an inject it does not know is vk");
        write("[Chat]\nhold_min_ms=200\n");
        ChatSettings one;
        check(one.load(path) && one.hold_min_ms == 200 && one.hold_max_ms == 200, "one hold set alone: a fixed hold");
        write("[Chat]\nenabled=false\n");
        ChatSettings none;
        check(none.load(path) && none.hold_max_ms == 0 && none.enter_gap_ms == 750 && none.quiet_before_ms == 300 &&
                  none.max_enters == 2 && none.type_tries == 1 && none.inject == InputManager::KeyInject::Vk,
              "absent: as shipped");
        std::remove(path.c_str());
#ifdef GOBOT_SOURCE_DIR
        ChatSettings file;
        check(file.load(std::string(GOBOT_SOURCE_DIR) + "/configs/chat.ini") && file.hold_min_ms == 0 &&
                  file.hold_max_ms == 0 && file.enter_gap_ms == 750 && file.quiet_before_ms == 300 &&
                  file.type_wait_ms == 0 && file.max_enters == 2 && file.type_tries == 1 &&
                  file.inject == InputManager::KeyInject::Vk,
              "the shipped chat.ini names each, at what GoBot has always done");
#endif
        // The scan code alone, as GoC sends keys -- built, not sent.
        const InputManager::KeyEvent sd = InputManager::typed_key_event('A', true, InputManager::KeyInject::Scancode);
        const InputManager::KeyEvent su = InputManager::typed_key_event('A', false, InputManager::KeyInject::Scancode);
        const InputManager::KeyEvent vd = InputManager::typed_key_event('A', true, InputManager::KeyInject::Vk);
        const std::uint16_t scan = static_cast<std::uint16_t>(MapVirtualKeyA('A', MAPVK_VK_TO_VSC));
        check(sd.vk == 0 && sd.scan == scan && scan != 0 && (sd.flags & KEYEVENTF_SCANCODE) != 0 &&
                  (sd.flags & KEYEVENTF_KEYUP) == 0 && su.vk == 0 && (su.flags & KEYEVENTF_SCANCODE) != 0 &&
                  (su.flags & KEYEVENTF_KEYUP) != 0,
              "inject=scancode: no key code, the scan code, KEYEVENTF_SCANCODE, down and up");
        check(vd.vk == 'A' && vd.scan == scan && (vd.flags & KEYEVENTF_SCANCODE) == 0,
              "inject=vk: the key code with its scan code, as always");
    }

    // ---- the method names round-trip, and paste is not one ----
    {
        bool all = true;
        for (ChatMethod m : { ChatMethod::Vk, ChatMethod::Unicode, ChatMethod::WmChar,
                              ChatMethod::ImeChar }) {
            ChatMethod back;
            all = all && chat_method_from(chat_method_name(m), back) && back == m;
        }
        ChatMethod bad;
        check(all && !chat_method_from("nope", bad), "the method names round-trip");
        ChatMethod kept = ChatMethod::WmChar;
        check(!chat_method_from("paste", kept) && !chat_method_from(" PASTE ", kept) &&
                  kept == ChatMethod::WmChar,
              "paste is not a method: nothing of GoBot's touches the clipboard");
    }

    // ---- CA5 and CA7: the bar pictures and the sender, with the game's own
    //       chat-bar pictures ----
    TemplateStore store;
    ChatSettings settings;
    const bool have = store.load_best(exe_relative("configs/templates.ini")) &&
                      can_look_for_chat(store, settings);
    if (!have) {
        std::printf("  [skip] the chat-bar pictures are not installed here\n");
        return;
    }

    PretendChat game;
    game.store = &store;
    game.settings = &settings;

    // CA5: the shut line reads shut, with the closed picture matching well; a
    // field of the open grey reads open.
    {
        game.state = PretendChat::State::Shut;
        const ChatBar shut = look_chat_bar(game.render(), store, settings);
        check(shut.found && shut.state == ChatBarState::Closed,
              "the chat bar is found, and the shut line reads shut");
        check(shut.closed_score >= 0.95f, "the closed picture matches the shut field well");
        check(shut.dark < 100, "the shut field has little of the open field's grey");
        game.state = PretendChat::State::Open;
        const ChatBar open = look_chat_bar(game.render(), store, settings);
        check(open.found && open.state == ChatBarState::Open, "the open line reads open");
        check(open.dark >= 100, "the open field is full of the open field's grey");
    }

    InputManager no_window;    // never used: the hands stand in for it
    const auto make_sender = [&](PretendChat& g) {
        auto s = std::make_unique<ChatSender>(no_window, store,
                                              [&g](Frame& f) { f = g.render(); return true; });
        s->set_settings(settings);
        s->set_dialog_check([&g](const Frame& f) { return g.dialog_up(f); });
        s->set_hands(g.hands());
        return s;
    };

    // CA7: a whole line, start to finish -- opened, typed, sent, shut -- with
    // no Enter on a dialog and no key pressed while the line was shut.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        auto sender = make_sender(g);
        const ChatText line = prepare_chat("test 1", settings.method, settings.strip_diacritics,
                                           settings.max_length);
        const ChatSender::Result r = sender->send(line);
        check(r.sent && r.opened && !r.left_open, "a line is opened, typed and sent");
        check(r.typed == static_cast<int>(line.typed.size()), "every character's key went");
        check(g.sent.size() == 1 && g.state == PretendChat::State::Shut,
              "the game got one line and the chat line is shut");
        check(g.enters_with_dialog == 0 && g.keys_while_shut == 0,
              "no Enter on a dialog, and no key pressed while the line was shut");
    }

    // The line already open when it starts: the player is typing, so nothing
    // is pressed and it waits.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.state = PretendChat::State::Open;
        auto sender = make_sender(g);
        const ChatText line = prepare_chat("test", ChatMethod::Vk, true, 60);
        const ChatSender::Result r = sender->send(line);
        check(r.not_now && !r.sent && g.sent.empty(),
              "the line already open: nothing sent, try again later");
        check(g.enters_with_dialog == 0 && g.keys_while_shut == 0, "and nothing pressed");
    }

    // A dialog up: no Enter, nothing sent.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.dialog = "thùng rác";
        auto sender = make_sender(g);
        const ChatText line = prepare_chat("test", ChatMethod::Vk, true, 60);
        const ChatSender::Result r = sender->send(line);
        check(!r.sent && g.sent.empty() && g.enters_with_dialog == 0,
              "a dialog up: no Enter, nothing sent");
    }

    // The game not in front: nothing is typed.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.focused = false;
        auto sender = make_sender(g);
        const ChatText line = prepare_chat("test", ChatMethod::Vk, true, 60);
        const ChatSender::Result r = sender->send(line);
        check(!r.sent && r.typed == 0 && g.sent.empty(), "the game not in front: nothing typed");
    }

    // Cancelled before it starts: nothing pressed, the line left shut.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        auto sender = make_sender(g);
        sender->cancel();
        const ChatText line = prepare_chat("test", ChatMethod::Vk, true, 60);
        const ChatSender::Result r = sender->send(line);
        check(!r.sent && g.sent.empty() && g.state == PretendChat::State::Shut,
              "cancelled before the first Enter: the line stays shut, nothing sent");
    }

    // Stopped in the middle of typing: what was typed is rubbed out and the
    // line closed, never sent. (The guard by what the pretend game has seen,
    // not by how often it is asked: it is asked through every wait too.)
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        auto sender = make_sender(g);
        sender->set_guard([&g]() -> std::string {
            return g.letters >= 3 ? std::string("Máu thấp — dừng câu đang gõ.") : std::string();
        });
        const ChatText line = prepare_chat("abcdefghij", ChatMethod::Vk, true, 60);
        const ChatSender::Result r = sender->send(line);
        check(!r.sent && !r.left_open && g.sent.empty() && g.letters == 3,
              "stopped mid-line, three letters in: nothing sent and the line not left open");
        check(g.state == PretendChat::State::Shut && g.typed == 0,
              "what was typed is rubbed out and the line closed");
    }

    // The game shows nothing for the typed characters (as it ignored Unicode
    // in the bin's box): the line is not sent, and it is rubbed out and closed.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.ignore_text = true;
        auto sender = make_sender(g);
        const ChatText line = prepare_chat("test 1", ChatMethod::Vk, true, 60);
        const ChatSender::Result r = sender->send(line);
        check(!r.sent && g.sent.empty(), "text that does not show up is never sent");
        check(g.state == PretendChat::State::Shut, "and the line is closed again");
    }

    const auto line_of = [](const char* text) { return prepare_chat(text, ChatMethod::Vk, true, 60); };

    // What an empty line shows, measured once it is cleared, is kept for
    // closing it later.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("test"));
        check(r.sent && r.empty_ink == 5, "the cleared line's caret is measured and handed back");
    }

    // The game shuts the line part way through -- a click in the game, a map
    // change: not one key more goes in, a Backspace least of all, and it is a
    // failure rather than a line put off.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.shut_after = 5;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("ban do lv 50 zalo 0912345678"));
        check(!r.sent && g.sent.empty(), "a line the game shuts part way is not sent");
        check(g.keys_while_shut == 0 && g.letters == 5,
              "and not one key goes into the game once it has shut (" +
                  std::to_string(g.keys_while_shut) + " did)");
        check(!r.aborted && !r.left_open, "a failure, with the line seen shut");
    }
    // The same, with the bar going out of sight as the line shuts: the line
    // cannot be seen, so nothing is rubbed out blind into a shut game -- it is
    // reported as maybe open, for whoever watches it after.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.shut_after = 8;
        g.hide_on_shut = true;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("ban do lv 50 zalo 0912345678"));
        check(!r.sent && g.keys_while_shut == 0 && g.letters == 8,
              "the bar out of sight mid-line: not a Backspace into the shut game (" +
                  std::to_string(g.keys_while_shut) + " went)");
        check(r.left_open && r.aborted && !r.stop,
              "the line put off and reported as maybe open, for the watcher");
    }
    // One frame that shows the line shut while it is not is looked past: the
    // look after it, with no key pressed between, has the last word.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.flicker = 6;              // a look before one of the first keys
        auto sender = make_sender(g);
        const ChatText line = line_of("test 12345");
        const ChatSender::Result r = sender->send(line);
        check(r.sent && g.sent.size() == 1 && g.sent[0] == line.chars && g.keys_while_shut == 0,
              "one frame wrongly showing the line shut does not end the line");
    }

    // A line that will not open: two Enters, never three -- a game that takes
    // them late is left as it was, shut -- and the line reported as maybe
    // open, so that it is watched for.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.deaf = true;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("test"));
        check(!r.sent && g.enters == 2 && g.letters == 0 && g.backspaces == 0,
              "a line that will not open: two Enters and nothing typed");
        check(r.left_open && !r.aborted, "a failure, and the line left to be watched");
    }

    // The line opens but looks like neither picture -- the live client's open
    // line has never been photographed: nothing is typed, one Enter more
    // shuts it again, and Tự chat is told to stop.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.field_grey = 64;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("test"));
        check(!r.sent && r.stop && g.letters == 0 && g.backspaces == 0,
              "an open line that looks like neither picture: nothing typed, and a stop");
        check(g.state == PretendChat::State::Shut && !r.left_open && g.sent.empty() && g.enters == 2,
              "and one Enter more has shut it again, sending nothing");
    }
    // The first Enter not taken, and the bar out of sight after it: no bar
    // at all is not a bar that looks like neither picture. No Enter more --
    // it could open a shut line -- and the line is left to be watched.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.deaf = true;
        g.hide_after_enters = 1;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("test"));
        check(g.enters == 1 && g.letters == 0 && g.backspaces == 0,
              "the bar gone after the first Enter: no Enter more, nothing typed (" +
                  std::to_string(g.enters) + " Enters)");
        check(!r.sent && r.left_open && r.aborted && !r.stop,
              "put off, not stopped, and left to be watched");
    }

    // A shut line that still holds ten characters of the player's: rubbed
    // out before typing, so the line goes out as typed and nothing else.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.draft = 10;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("test"));
        check(r.sent && g.sent.size() == 1 && g.sent[0] == 4,
              "a draft left in the line is rubbed out first: the game gets the 4 characters typed");
    }
    // Drafts of other lengths: one or two characters left after the six
    // Backspaces look no wider than the caret, and long ones take more than
    // a round or two. The game gets the line as typed every time.
    for (const int draft : { 7, 8, 12, 40, 61, 90, 100 }) {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.draft = draft;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("test"));
        check(r.sent && g.sent.size() == 1 && g.sent[0] == 4 && g.keys_while_shut == 0,
              "a draft of " + std::to_string(draft) + " is rubbed out: the game gets only 'test' (got " +
                  (g.sent.empty() ? std::string("nothing") : std::to_string(g.sent[0])) + ")");
    }
    // One too long to rub out within the most Backspaces pressed at once:
    // not typed after it, and not sent.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.draft = 200;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("test"));
        check(!r.sent && g.sent.empty() && g.letters == 0,
              "a draft too long to rub out: nothing typed after it, nothing sent");
    }
    // Something in the field that Backspace does not take away: nothing is
    // typed, nothing sent, and the line is left to be watched rather than
    // shut with Enter, which would send whatever it is.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.stuck_ink = 40;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("test"));
        check(!r.sent && !r.aborted && g.letters == 0 && g.sent.empty() && r.left_open,
              "a field Backspace cannot clear: a failure, nothing typed or sent, left to be watched");
    }

    // The same, stopped while the line is being cleared: what was left in it
    // is rubbed out too, and the line shut empty -- never sent.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.draft = 10;
        auto sender = make_sender(g);
        ChatSender* const s = sender.get();
        int bs = 0;
        g.after_key = [&](std::uint32_t vk) { if (vk == VK_BACK && ++bs == 2) s->cancel(); };
        const ChatSender::Result r = sender->send(line_of("test"));
        check(!r.sent && g.sent.empty(), "a stop while the line is cleared never sends what was in it");
        check(g.state == PretendChat::State::Shut && g.typed == 0, "the line is rubbed out and shut");
        check(r.aborted, "and a stop is a line put off, not a failure");
    }

    // A stop that comes while the screen is looked at just before the first
    // Enter: no Enter -- nothing would be left to see to it.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        auto sender = make_sender(g);
        ChatSender* const s = sender.get();
        sender->set_dialog_check([&g, s](const Frame& f) {
            s->cancel();
            return g.dialog_up(f);
        });
        const ChatSender::Result r = sender->send(line_of("test"));
        check(g.enters == 0 && !r.sent && !r.left_open && g.state == PretendChat::State::Shut,
              "a stop during the look before the first Enter: no Enter pressed");
    }
    // A stop just after the first Enter, in a game that takes keys a third of
    // a second late: the line a look still shows shut opens a moment later.
    // It is not taken for shut until the Enter has had time to show, and it
    // is closed again -- never left open with the sender saying it is shut.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.lag = 300;
        auto sender = make_sender(g);
        ChatSender* const s = sender.get();
        g.after_key = [s](std::uint32_t vk) { if (vk == VK_RETURN) s->cancel(); };
        const ChatSender::Result r = sender->send(line_of("test"));
        g.advance(2000);            // whatever was still on its way lands
        check(g.state == PretendChat::State::Shut && !r.left_open && g.sent.empty(),
              "a stop just after a late Enter: the line is seen open after all, and shut again");
    }
    // Stops at every moment of the first second and a half of a short line,
    // in a game that takes keys 120 ms late and looks for dialogs that take a
    // tenth of a second: the line never ends open while the sender says it
    // is shut, and nothing is ever sent.
    {
        int unreported = 0, sent = 0, runs = 0;
        for (std::uint64_t at = 0; at <= 1500; at += 100) {
            PretendChat g;
            g.store = &store; g.settings = &settings;
            g.lag = 120;
            g.dialog_ms = 100;
            auto sender = make_sender(g);
            g.to_cancel = sender.get();
            g.cancel_at = at;
            const ChatSender::Result r = sender->send(line_of("test 1"));
            g.to_cancel = nullptr;
            g.advance(2000);
            ++runs;
            if (g.state == PretendChat::State::Open && !r.left_open) ++unreported;
            if (!g.sent.empty()) ++sent;
        }
        check(unreported == 0 && sent == 0,
              "a stop at any moment never leaves the line open unreported (" + std::to_string(unreported) +
                  " of " + std::to_string(runs) + " did), and sends nothing (" + std::to_string(sent) + ")");
    }
    // A stop, or the guard, that comes while the screen is looked at for the
    // last time before the sending Enter -- the dialog check alone takes a
    // while: the line does not go out. Rubbed out, shut, and put off.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        auto sender = make_sender(g);
        ChatSender* const s = sender.get();
        sender->set_dialog_check([&g, s](const Frame& f) {
            if (g.letters == 4) s->cancel();        // every letter of "test" typed
            return g.dialog_up(f);
        });
        const ChatSender::Result r = sender->send(line_of("test"));
        check(!r.sent && r.aborted && g.sent.empty() && g.typed == 0 &&
                  g.state == PretendChat::State::Shut && !r.left_open,
              "a stop during the last look before the sending Enter: not sent, rubbed out and shut");
    }
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        auto sender = make_sender(g);
        sender->set_guard([&g]() -> std::string {
            return g.letters >= 4 ? std::string("Máu thấp — dừng câu đang gõ.") : std::string();
        });
        const ChatSender::Result r = sender->send(line_of("test"));
        check(!r.sent && r.aborted && g.sent.empty() && g.typed == 0 &&
                  g.state == PretendChat::State::Shut,
              "HP falling after the last key, before the sending Enter: not sent, rubbed out and shut");
    }

    // A line left open with something in it, closed by a sender that never
    // cleared it and so does not know its caret: all of it is rubbed out, and
    // only an empty line gets the Enter.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.state = PretendChat::State::Open;
        g.typed = 15;
        auto closer = make_sender(g);
        check(closer->close_left_open(3), "a line left open with text in it is closed");
        check(g.sent.empty() && g.typed == 0 && g.state == PretendChat::State::Shut,
              "and nothing that was in it is sent, though only 3 Backspaces were asked for");
    }
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.state = PretendChat::State::Open;
        auto closer = make_sender(g);
        closer->set_empty_ink(5);
        check(closer->close_left_open(0) && g.backspaces == 0 && g.sent.empty(),
              "an empty line, as it showed once cleared, is closed with no Backspace");
    }

    // Ended part way for something that is not the line's fault: put off, not
    // failed -- HP falling under the potion's mark, a dialog coming up, the
    // game losing the front.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        auto sender = make_sender(g);
        sender->set_guard([&g]() -> std::string {
            return g.letters >= 3 ? std::string("Máu thấp — dừng câu đang gõ.") : std::string();
        });
        const ChatSender::Result r = sender->send(line_of("abcdefghij"));
        check(!r.sent && r.aborted && r.wait_ms > 0, "HP falling mid-line: put off, not failed");
    }
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.after_key = [&g](std::uint32_t) { if (g.letters == 14) g.dialog = "lời mời tổ đội"; };
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("abcdefghijklmnopqrstuvwxyz"));
        check(!r.sent && r.aborted && !r.dialog.empty() && g.enters_with_dialog == 0 && g.typed == 0,
              "a dialog coming up mid-line: rubbed out, no Enter, put off");
    }
    // The same dialog on a frame that wrongly shows the line shut: believed
    // shut only on a second look, which shows it open -- so it is rubbed out,
    // and reported as maybe open rather than let go with the text in it.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        // Frames: the first Enter's look, the look that sees it open, two
        // while clearing, one before each of the first twelve keys -- the
        // seventeenth is the look before key 13, which takes in the dialogs.
        g.flicker = 17;
        g.after_key = [&g](std::uint32_t) { if (g.letters == 12) g.dialog = "lời mời tổ đội"; };
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("abcdefghijklmnopqrstuvwxyz"));
        check(!r.sent && r.left_open && g.typed == 0 && g.enters_with_dialog == 0 && g.frames >= 18,
              "a dialog on a frame wrongly showing the line shut: rubbed out, and left to be watched");
    }
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.after_key = [&g](std::uint32_t) { if (g.letters == 3) g.focused = false; };
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("abcdefghij"));
        check(!r.sent && r.aborted && r.left_open && g.enters == 1,
              "the game losing the front for good: no Enter, put off, the line reported as left open");
    }

    // Sent, and then the line will not shut: sent it was, and Tự chat is told
    // to stop all the same -- the open line is where the fighting keys go.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.stays_open = true;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("test"));
        check(r.sent && r.stop && r.left_open && g.sent.size() == 1 && g.enters == 4,
              "a line that will not shut after sending: sent, a stop, two Enters at most to shut it");
    }

    // Somebody at the mouse or keyboard just before the first Enter: nothing
    // is pressed, and the line waits.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        auto sender = make_sender(g);
        sender->set_ready_check([] {
            return std::string("Bạn đang dùng chuột hoặc bàn phím — chat sau.");
        });
        const ChatSender::Result r = sender->send(line_of("test"));
        check(r.not_now && g.enters == 0 && g.letters == 0,
              "the player busy just before the line: nothing pressed");
    }

    // How long the line took, as the potions saw it: from the hold, which
    // starts before the line does.
    {
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.clock = 10'000;
        auto sender = make_sender(g);
        sender->set_held_since(7'500);
        std::string closed;
        sender->on_report([&closed](const std::string& s) {
            if (s.find("đã đóng") != std::string::npos) closed = s;
        });
        const ChatSender::Result r = sender->send(line_of("test"));
        check(r.sent && closed.find("bot bị giữ") != std::string::npos,
              "the closing report says how long the bot was held, counted from the hold: " + closed);
    }

    // ---- a chat line left in doubt is held on to until it shows shut ----
    struct DoubtWatch {
        int holds = 0, releases = 0;
        std::string released, said;
        bool same = true, quiet = true;
        ChatDoubt::Hooks hooks() {
            ChatDoubt::Hooks h;
            h.hold = [this] { ++holds; };
            h.release = [this](const std::string& why) { ++releases; released = why; };
            h.same_game = [this] { return same; };
            h.when_quiet = [this](const std::function<bool()>& close) { return quiet && close(); };
            h.say = [this](const std::string& why) { said = why; };
            return h;
        }
    };
    const auto left_open = [](int typed) {
        ChatSender::Result r;
        r.left_open = true;
        r.typed = typed;
        return r;
    };
    // Steps every 100 ms of the pretend clock, as the worker takes them.
    const auto steps = [](ChatDoubt& d, ChatSender& s, PretendChat& g, int n) {
        for (int i = 0; i < n && d.active(); ++i) {
            g.advance(100);
            d.step(s, g.clock);
        }
    };
    {
        // Open with text in it, nobody at the mouse or keyboard: rubbed out
        // and shut, nothing sent, and let go once, after it is seen shut.
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.state = PretendChat::State::Open;
        g.typed = 12;
        auto closer = make_sender(g);
        DoubtWatch w;
        ChatDoubt d(w.hooks());
        d.note(left_open(10), 18, g.clock);
        check(d.active() && w.holds >= 1 && w.releases == 0,
              "a line left maybe open: held on to, everything else held off");
        steps(d, *closer, g, 40);
        check(!d.active() && w.releases == 1 && g.state == PretendChat::State::Shut && g.sent.empty(),
              "it is closed with nothing sent, and only then let go -- once (" + w.released + ")");
    }
    {
        // Out of sight: held for as long as that lasts, and nothing pressed
        // into a game that cannot be seen; let go once it shows shut.
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.state = PretendChat::State::Open;
        g.typed = 3;
        g.hidden = true;
        auto closer = make_sender(g);
        DoubtWatch w;
        ChatDoubt d(w.hooks());
        d.note(left_open(3), 9, g.clock);
        steps(d, *closer, g, 600);   // a minute
        check(d.active() && w.releases == 0 && g.enters == 0 && g.backspaces == 0 && !w.said.empty(),
              "a line out of sight for a minute: still held, nothing pressed, and the tab told why");
        g.hidden = false;
        g.state = PretendChat::State::Shut;   // the player shut it
        steps(d, *closer, g, 30);
        check(!d.active() && w.releases == 1, "seen shut at last: let go");
    }
    {
        // Somebody at the keyboard -- typing in the line, maybe: not touched,
        // still held; the player sends it, the line shuts, and it is let go.
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.state = PretendChat::State::Open;
        g.typed = 5;
        auto closer = make_sender(g);
        DoubtWatch w;
        w.quiet = false;
        ChatDoubt d(w.hooks());
        d.note(left_open(0), 8, g.clock);
        steps(d, *closer, g, 100);
        check(d.active() && w.releases == 0 && g.enters == 0 && g.backspaces == 0,
              "somebody at the mouse or keyboard: the open line is not touched, and still held");
        g.state = PretendChat::State::Shut;
        steps(d, *closer, g, 30);
        check(!d.active() && w.releases == 1, "the player shuts it: let go");
    }
    {
        // A dialog up: no Enter, no Backspace, still held.
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.state = PretendChat::State::Open;
        g.dialog = "lời mời tổ đội";
        auto closer = make_sender(g);
        DoubtWatch w;
        ChatDoubt d(w.hooks());
        d.note(left_open(0), 8, g.clock);
        steps(d, *closer, g, 50);
        check(d.active() && w.releases == 0 && g.enters == 0 && g.backspaces == 0,
              "a dialog up over the open line: nothing pressed, still held");
    }
    {
        // The game gone: let go, as the line went with it.
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.state = PretendChat::State::Open;
        g.hidden = true;
        auto closer = make_sender(g);
        DoubtWatch w;
        ChatDoubt d(w.hooks());
        d.note(left_open(0), 8, g.clock);
        steps(d, *closer, g, 5);
        w.same = false;
        steps(d, *closer, g, 5);
        check(!d.active() && w.releases == 1 && g.enters == 0, "the game closed: let go");
    }
    {
        // Handed over, when the player takes over: nothing let go here.
        PretendChat g;
        g.store = &store; g.settings = &settings;
        auto closer = make_sender(g);
        DoubtWatch w;
        ChatDoubt d(w.hooks());
        d.note(left_open(0), 8, g.clock);
        d.hand_over();
        steps(d, *closer, g, 20);
        check(!d.active() && w.releases == 0, "handed over on a stop: nothing let go by it");
        // A line that shows shut leaves nothing in doubt.
        ChatSender::Result fine;
        d.note(fine, 0, g.clock);
        check(!d.active(), "a line seen shut starts no doubt");
    }
    {
        // What a line left in doubt shows, one look at a time.
        PretendChat g;
        g.store = &store; g.settings = &settings;
        auto s = make_sender(g);
        std::string why;
        check(s->check_left_open(&why) == ChatSender::LeftOpen::Shut, "shut reads shut");
        g.state = PretendChat::State::Open;
        check(s->check_left_open(&why) == ChatSender::LeftOpen::Closable, "open with nothing in the way: closable");
        g.focused = false;
        check(s->check_left_open(&why) == ChatSender::LeftOpen::Busy && !why.empty(),
              "the game not in front: not to be touched now");
        g.focused = true;
        g.hidden = true;
        check(s->check_left_open(&why) == ChatSender::LeftOpen::Unseen && !why.empty(), "out of sight: unseen");
        check(g.enters == 0 && g.backspaces == 0 && g.letters == 0, "and looking presses nothing");
    }

    // ---- round 8: a game that takes the keys late, or drops them -- Tự chat
    //      on 2026-10-20 got 7 of 13 Enters and 4 of 6 typed lines through ----
    const auto sender_with = [&](PretendChat& g, const ChatSettings& s) {
        auto sender = make_sender(g);
        sender->set_settings(s);
        return sender;
    };
    {
        // Drawn 400 ms after it went in: the field is looked at again before
        // it is called empty, and the line goes.
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.draw_lag = 400;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("test"));
        check(r.sent && g.sent.size() == 1 && g.sent[0] == 4 && r.trace.after.size() >= 2,
              "the game drawing the typed text 400 ms late: looked at again, and sent -- " + chat_trace_line(r.trace));
    }
    {
        // Nothing drawn at all: given up a little over a second after the
        // last key, the line rubbed out and shut, no Backspace into it shut.
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.ignore_text = true;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("test"));
        const int waited = r.trace.after.empty() ? -1 : r.trace.after.back().first;
        check(!r.sent && g.sent.empty() && g.state == PretendChat::State::Shut && g.keys_while_shut == 0 &&
                  r.trace.outcome == "no_text" && r.trace.after.size() == 3 && waited > 0 && waited <= 1200,
              "nothing drawn: three looks, the last " + std::to_string(waited) +
                  " ms after the last key, then not sent, rubbed out and shut, nothing pressed into it shut");
    }
    {
        // Narrow letters: i and l are a pixel wide with one between them.
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.px_per_char = 2;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("lil"));
        check(r.sent && g.sent.size() == 1 && g.sent[0] == 3, "'lil', two pixels a letter, is taken for text and sent");
    }
    {
        // A caret moving 4 px is no letter.
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.ignore_text = true;
        g.caret_shift = 4;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("a"));
        check(!r.sent && g.sent.empty() && r.trace.outcome == "no_text",
              "one letter that shows only as the caret 4 px on: not taken for text, not sent");
    }
    {
        // The trace of a line that needed a second Enter: each Enter's hold
        // and the looks after it, and none of the line's words.
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.drop_first = 1;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("hello there"));
        const std::string t = chat_trace_line(r.trace);
        bool first_shut = r.trace.enters.size() == 2 && r.trace.enters[0].looks.size() >= 3;
        if (first_shut)
            for (const auto& look : r.trace.enters[0].looks) first_shut = first_shut && look.second == ChatBarState::Closed;
        check(r.sent && first_shut && !r.trace.enters[1].looks.empty() &&
                  r.trace.enters[1].looks.back().second == ChatBarState::Open && t.rfind("Chat trace: Enter 1 held ", 0) == 0 &&
                  t.find("; Enter 2 held ") != std::string::npos && t.find(" shut") != std::string::npos &&
                  t.find(" open") != std::string::npos && t.find("11 keys typed in 1 try, held 50 ms (vk)") != std::string::npos &&
                  t.find("; sent") != std::string::npos && t.find("hello") == std::string::npos &&
                  t.find("there") == std::string::npos,
              "the trace of a line that took two Enters: " + t);
        PretendChat d;
        d.store = &store; d.settings = &settings;
        d.ignore_text = true;
        auto deaf_sender = make_sender(d);
        const ChatSender::Result n = deaf_sender->send(line_of("hello there"));
        const std::string nt = chat_trace_line(n.trace);
        check(!n.sent && nt.find("after the last key: +") != std::string::npos &&
                  nt.find("; no_text") != std::string::npos && nt.find("hello") == std::string::npos,
              "and of one the game showed nothing of: " + nt);
    }
    {
        // Every key held as chat.ini says, letters and Backspace too; as
        // shipped, each its own.
        ChatSettings s = settings;
        s.hold_min_ms = 150;
        s.hold_max_ms = 250;
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.ignore_first_letters = 4;     // so that rubbing out and a second Enter come into it too
        auto sender = sender_with(g, s);
        sender->send(line_of("test"));
        bool in = !g.holds.empty();
        bool letters = false, backspaces = false;
        for (const auto& [key_vk, held] : g.holds) {
            in = in && held >= 150 && held <= 250;
            letters = letters || (key_vk != VK_RETURN && key_vk != VK_BACK);
            backspaces = backspaces || key_vk == VK_BACK;
        }
        check(in && letters && backspaces,
              "hold_min_ms=150, hold_max_ms=250: every one of " + std::to_string(g.holds.size()) +
                  " keys held 150-250 ms, Enter, Backspace and letters alike");
        PretendChat d;
        d.store = &store; d.settings = &settings;
        auto plain = make_sender(d);
        plain->send(line_of("test"));
        bool as_ever = !d.holds.empty();
        for (std::size_t i = 0; i < d.holds.size(); ++i) {
            const auto [key_vk, held] = d.holds[i];
            if (key_vk == VK_RETURN) as_ever = as_ever && (i == 0 ? held >= 60 && held <= 130 : held == 50);
            else if (key_vk == VK_BACK) as_ever = as_ever && ((held >= 40 && held <= 70) || held == 25);
            else as_ever = as_ever && held == 50;
        }
        check(as_ever, "as shipped: the opening Enter 60-130 ms, letters and the sending Enter 50, Backspace 40-70");
    }
    {
        // An Enter dropped every other press: opens with the second.
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.drop_every = 2;
        auto sender = make_sender(g);
        const ChatSender::Result r = sender->send(line_of("test"));
        check(r.sent && r.trace.enters.size() == 2 && g.sent.size() == 1,
              "the game dropping every other Enter: opened with the second, and sent");
        // Deaf to Enter: two pressed, as shipped, then not opened.
        PretendChat d;
        d.store = &store; d.settings = &settings;
        d.deaf = true;
        auto deaf_sender = make_sender(d);
        const ChatSender::Result n = deaf_sender->send(line_of("test"));
        check(!n.sent && n.left_open && n.typed == 0 && d.enters == 2 && n.trace.outcome == "not_opened" &&
                  n.trouble.find("2 lần") != std::string::npos,
              "deaf to Enter: two pressed, nothing typed, not opened -- " + n.trouble);
        // max_enters 3, the first two dropped: opens with the third.
        ChatSettings three = settings;
        three.max_enters = 3;
        PretendChat t;
        t.store = &store; t.settings = &settings;
        t.drop_first = 2;
        auto third = sender_with(t, three);
        const ChatSender::Result r3 = third->send(line_of("test"));
        check(r3.sent && r3.trace.enters.size() == 3, "max_enters=3, the first two Enters dropped: opened with the third");
        // max_enters 1, the first dropped: not opened after the one.
        ChatSettings one = settings;
        one.max_enters = 1;
        PretendChat o;
        o.store = &store; o.settings = &settings;
        o.drop_first = 1;
        auto single = sender_with(o, one);
        const ChatSender::Result r1 = single->send(line_of("test"));
        check(!r1.sent && o.enters == 1 && r1.trouble.find("1 lần") != std::string::npos,
              "max_enters=1, the first dropped: not opened after one Enter -- " + r1.trouble);
    }
    {
        // A game that takes nothing of the first typing: with type_tries 1
        // the line is not sent; with 2 it goes on the second -- once.
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.ignore_first_letters = 4;
        auto once = make_sender(g);
        const ChatSender::Result r = once->send(line_of("test"));
        check(!r.sent && g.sent.empty() && r.trace.outcome == "no_text",
              "the first typing taken as nothing, type_tries=1: not sent");
        ChatSettings two = settings;
        two.type_tries = 2;
        PretendChat h;
        h.store = &store; h.settings = &settings;
        h.ignore_first_letters = 4;
        auto twice = sender_with(h, two);
        const ChatSender::Result r2 = twice->send(line_of("test"));
        check(r2.sent && h.sent.size() == 1 && h.sent[0] == 4 && r2.trace.tries == 2,
              "type_tries=2: typed again into the empty line, and the line goes with the text once");
        // Deaf to text for good: two tries, then not sent, tidied.
        PretendChat d;
        d.store = &store; d.settings = &settings;
        d.ignore_text = true;
        auto deaf_sender = sender_with(d, two);
        const ChatSender::Result n = deaf_sender->send(line_of("test"));
        check(!n.sent && n.trace.tries == 2 && d.sent.empty() && d.state == PretendChat::State::Shut &&
                  d.keys_while_shut == 0,
              "type_tries=2, nothing ever drawn: two tries, not sent, the line shut, nothing pressed into it shut");
        // Drawn late, with a second try allowed: the late text is taken, and
        // there is no second try to double it.
        PretendChat l;
        l.store = &store; l.settings = &settings;
        l.draw_lag = 400;
        auto late = sender_with(l, two);
        const ChatSender::Result rl = late->send(line_of("test"));
        check(rl.sent && rl.trace.tries == 1 && l.sent.size() == 1 && l.sent[0] == 4,
              "type_tries=2 and the text drawn 400 ms late: taken on a later look, never typed twice");
    }

    // ---- round 8b (R8R2-1): the Enter that sends is taken late. The game sends the line a second or two
    //      after the Enter -- after ChatSender's looks have seen the text still in the open line ("not
    //      sent"), or after send() has returned altogether -- and the party worker, looking at the chat the
    //      moment the line is on it, has to know already that the line is the player's own. The GUI
    //      remembers a line just before its sending Enter goes down (ChatSender::on_send_pressed ->
    //      remember_said), as the hook below does. Invented names. ----
    {
        using Typed = std::vector<std::pair<std::string, std::uint64_t>>;
        struct Asked {
            long long at = -1;
            std::string who;
        };
        // The party worker's steps on every look -- without_own, then the votes -- a look every `gap` ms,
        // 100 unless said, faster than the worker's own, from `begin`. The memory a look has is what had
        // been remembered by then (an entry counts from its own time on).
        const auto first_ask = [](const std::function<std::vector<std::string>(std::uint64_t)>& on_chat,
                                  const Typed& said, std::uint64_t begin, std::uint64_t to_ms,
                                  std::uint64_t gap = 100) {
            PartyChat chat;
            chat.set_settings(PartyChat::Settings{});       // keyword "pt", 3 votes; no name set, as shipped
            Asked a;
            for (std::uint64_t t = begin; t < begin + to_ms; t += gap) {
                Typed known;
                for (const auto& s : said)
                    if (s.second <= t) known.push_back(s);
                const auto who = chat.saw(chat.without_own(on_chat(t), "", known, t), t);
                if (!who.empty()) {
                    a.at = static_cast<long long>(t - begin);
                    a.who = who[0];
                    break;
                }
            }
            return a;
        };
        // What Tự chat says: the keyword alone, and an advert with the keyword in it. Each is read back off
        // the chat with the player's character in front of it.
        struct Kind {
            const char* said;
            const char* on_chat;
        };
        const Kind kinds[] = { { "pt", "[Kenh]MyChar : pt" },
                               { "tuyen pt lv 50 ban do gia re", "[Kenh]MyChar : tuyen pt lv 50 ban do gia re" } };
        const auto run = [&](const Kind& kind, int dialog_ms, int late) {
            const std::string own_line = kind.on_chat;
            const auto own_only = [&own_line](std::uint64_t) { return std::vector<std::string>{ own_line }; };
            PretendChat g;
            g.store = &store; g.settings = &settings;
            g.clock = 50'000'000;
            g.send_lag = late;
            // Each look for the dialogs takes this long, which keeps send() from returning for that much
            // longer once the line is on the chat.
            g.dialog_ms = dialog_ms;
            std::uint64_t pressed_at = 0;       // the clock at the Enter that sends: letters are in
            g.after_key = [&](std::uint32_t key_vk) {
                if (key_vk == VK_RETURN && g.letters > 0 && pressed_at == 0) pressed_at = g.clock;
            };
            auto sender = make_sender(g);
            const ChatText line = line_of(kind.said);
            Typed said;                         // what the GUI's remember_said keeps
            int calls = 0;
            sender->on_send_pressed([&] {
                ++calls;
                said.emplace_back(line.typed, g.clock);
            });
            const ChatSender::Result r = sender->send(line);
            const std::uint64_t returned_at = g.clock;
            g.advance(5000);                    // whatever is still on its way lands
            const std::string tag = std::string("\"") + kind.said + "\", the Enter that sends taken " +
                                    std::to_string(late) + " ms late (dialog look " + std::to_string(dialog_ms) +
                                    " ms): ";
            check(g.sent.size() == 1 && g.sent_at.size() == 1, tag + "the game sent the line, once");
            check(r.send_pressed && calls == 1, tag + "send_pressed, and the hook called once");
            if (late == 0) check(r.sent && !r.left_open, tag + "seen sent");
            else check(!r.sent && r.trace.outcome.rfind("not seen sent (the game may send it late)", 0) == 0,
                       tag + "not seen sent: r.sent says so, as it did, and the trace says the game may send it late");
            if (late == 2500 && std::string(kind.said) == "pt")
                check(r.left_open, tag + "and left open, for the doubt");
            check(pressed_at != 0 && !said.empty() && said[0].second <= pressed_at &&
                      pressed_at - said[0].second <= 50,
                  tag + "remembered just before the Enter that sends went down, not after its hold nor when send() "
                        "returns (" + std::to_string(returned_at - pressed_at) + " ms after it)");
            if (g.sent_at.empty() || said.empty()) return;
            const std::uint64_t begin = g.sent_at[0];       // the looks begin the moment the game sends it
            const Asked own = first_ask(own_only, said, begin, 240'000);
            check(own.at < 0, tag + "the player's own line, on the chat for 4 minutes, is never taken for asking" +
                                  (own.at < 0 ? std::string() : " -- taken at +" + std::to_string(own.at) + " ms"));
            const Asked other = first_ask(
                [begin, &own_line](std::uint64_t t) {
                    std::vector<std::string> lines = { own_line };
                    if (t >= begin + 90'000) lines.push_back("[Kenh]Someone : pt");
                    return lines;
                },
                said, begin, 240'000);
            check(other.who == "Someone" && other.at >= 90'000 && other.at < 91'000,
                  tag + "and somebody else's keyword alone, 90 s on, still asks, on the third look (at +" +
                      std::to_string(other.at) + " ms)");
            if (late > 0) {
                const Asked none = first_ask(own_only, Typed{}, begin, 240'000);
                check(none.at >= 0 && none.at <= 400,
                      tag + "(control) with nothing remembered the same line is taken for asking, at +" +
                          std::to_string(none.at) + " ms");
            }
        };
        for (const Kind& kind : kinds)
            for (const int dialog_ms : { 0, 100 })
                for (const int late : { 0, 1500, 2500 }) run(kind, dialog_ms, late);

        // chat.ini's holds go up to half a second, every key alike, and a game that acts on the key-down
        // shows the line while the sending Enter is still held: remembered before that key goes down, the
        // line is the player's from the moment it is on the chat, whatever the hold (R8bR-4). A worker
        // looking every 50 ms -- three looks in 100 ms -- from the moment the game sends it.
        for (const int hold_ms : { 131, 500 }) {
            ChatSettings held = settings;
            held.hold_min_ms = hold_ms;
            held.hold_max_ms = hold_ms;
            PretendChat g;
            g.store = &store; g.settings = &settings;
            g.clock = 60'000'000;
            std::uint64_t pressed_at = 0;       // the clock as the Enter that sends went down
            g.after_key = [&](std::uint32_t key_vk) {
                if (key_vk == VK_RETURN && g.letters > 0 && pressed_at == 0) pressed_at = g.clock;
            };
            auto sender = sender_with(g, held);
            const ChatText line = line_of("pt");
            Typed said;
            sender->on_send_pressed([&] { said.emplace_back(line.typed, g.clock); });
            const ChatSender::Result r = sender->send(line);
            g.advance(3000);
            const std::string tag = "chat.ini's holds at " + std::to_string(hold_ms) +
                                    " ms, the game sending on the key-down: ";
            check(r.sent && r.send_pressed && g.sent_at.size() == 1 && pressed_at != 0 && g.sent_at[0] == pressed_at,
                  tag + "the line goes as the Enter that sends goes down");
            check(said.size() == 1 && said[0].second <= pressed_at,
                  tag + "remembered before that key went down, not once its hold was over");
            if (g.sent_at.empty() || said.empty()) continue;
            const std::uint64_t begin = g.sent_at[0];
            const auto own_only = [](std::uint64_t) { return std::vector<std::string>{ "[Kenh]MyChar : pt" }; };
            const Asked own = first_ask(own_only, said, begin, 240'000, 50);
            check(own.at < 0, tag + "the player's own line, looked at every 50 ms from the moment it is on the chat, "
                                    "is never taken for asking" +
                                  (own.at < 0 ? std::string() : " -- taken at +" + std::to_string(own.at) + " ms"));
            // (control) Remembered once the key's hold was over, as before this round: taken for asking
            // before that.
            const Asked late = first_ask(own_only, Typed{ { line.typed, pressed_at + hold_ms } }, begin, 240'000, 50);
            check(late.at >= 0 && late.at < hold_ms,
                  tag + "(control) remembered after the hold, the same line is taken for asking at +" +
                      std::to_string(late.at) + " ms, before the hold is over");
        }

        // No Enter to send, no line remembered: nothing pressed, or the line ended before its sending Enter.
        const auto not_sent = [&](const char* what, const std::function<void(PretendChat&)>& set) {
            PretendChat g;
            g.store = &store; g.settings = &settings;
            set(g);
            auto sender = make_sender(g);
            int calls = 0;
            sender->on_send_pressed([&] { ++calls; });
            const ChatSender::Result r = sender->send(line_of("test"));
            g.advance(3000);
            check(calls == 0 && !r.send_pressed && g.sent.empty(),
                  std::string(what) + ": no Enter to send, nothing remembered");
        };
        not_sent("the line already open (the player typing)",
                 [](PretendChat& g) { g.state = PretendChat::State::Open; });
        not_sent("the game not in front", [](PretendChat& g) { g.focused = false; });
        not_sent("a dialog up", [](PretendChat& g) { g.dialog = "thùng rác"; });
        not_sent("deaf to Enter", [](PretendChat& g) { g.deaf = true; });
        not_sent("the game showing nothing of the typed text", [](PretendChat& g) { g.ignore_text = true; });
        {
            PretendChat g;
            g.store = &store; g.settings = &settings;
            auto sender = make_sender(g);
            int calls = 0;
            sender->on_send_pressed([&] { ++calls; });
            sender->set_guard([&g]() -> std::string {
                return g.letters >= 3 ? std::string("Máu thấp — dừng câu đang gõ.") : std::string();
            });
            const ChatSender::Result r = sender->send(line_of("abcdefghij"));
            check(calls == 0 && !r.send_pressed && r.aborted && g.sent.empty(),
                  "stopped mid-line by the guard: rubbed out, no Enter to send, nothing remembered");
        }
    }

    // ---- round 8 (R8R-2): chat.ini's waits are spent with the potions held
    //      off -- the pause before a line up to 5 s, the Enter's watch and the
    //      type wait up to 3 s each. HP falling under the potion's mark ends
    //      each at once, as it does between keys ----
    {
        std::uint64_t t = 0;
        const auto sleep = [&t](int ms) { t += static_cast<std::uint64_t>(std::max(0, ms)); };
        const auto low_from = [&t](std::uint64_t at) {
            return [&t, at]() -> std::string { return t >= at ? std::string("Máu thấp") : std::string(); };
        };
        const std::string why = chat_quiet_pause(5000, low_from(1000), sleep);
        check(why == "Máu thấp" && t >= 1000 && t <= 1000 + 100,
              "quiet_before_ms=5000, HP under the mark 1 s in: the pause, and the hold with it, ends " +
                  std::to_string(t >= 1000 ? t - 1000 : 0) + " ms after");
        t = 0;
        check(chat_quiet_pause(5000, low_from(~0ull), sleep).empty() && t == 5000,
              "nothing in the way: the whole 5 s");
        t = 0;
        check(chat_quiet_pause(300, low_from(0), sleep) == "Máu thấp" && t == 0,
              "HP under the mark already: not a moment of the pause");
        t = 0;
        check(chat_quiet_pause(0, low_from(~0ull), sleep).empty() && t == 0, "quiet_before_ms=0: no pause at all");
    }
    {
        // The Enter's watch, 3 s, on a game that does not open the line: HP
        // under the mark 1 s into it.
        ChatSettings s = settings;
        s.enter_gap_ms = 3000;
        PretendChat g;
        g.store = &store; g.settings = &settings;
        g.deaf = true;
        bool entered = false;           // the pretend clock starts at 0: the first Enter may go at 0
        std::uint64_t enter_at = 0;
        g.after_key = [&](std::uint32_t key_vk) {
            if (key_vk == VK_RETURN && !entered) {
                entered = true;
                enter_at = g.clock;
            }
        };
        auto sender = sender_with(g, s);
        sender->set_guard([&]() -> std::string {
            return entered && g.clock >= enter_at + 1000 ? std::string("Máu thấp — dừng câu đang gõ.")
                                                         : std::string();
        });
        const ChatSender::Result r = sender->send(line_of("test"));
        const long long after = static_cast<long long>(g.clock) - static_cast<long long>(enter_at + 1000);
        check(!r.sent && r.aborted && r.wait_ms >= 5000 && g.enters == 1 && r.trouble.rfind("Máu thấp", 0) == 0 &&
                  entered && after >= 0 && after <= 300 && g.state == PretendChat::State::Shut,
              "enter_gap_ms=3000, HP under the mark 1 s into the Enter's watch: the line ends " +
                  std::to_string(after) + " ms after, one Enter pressed, put off");
    }
    {
        // The type wait, 3 s, the line open and cleared: HP under the mark
        // 1 s into it. Not a letter goes; the line is rubbed out and shut.
        ChatSettings s = settings;
        s.type_wait_ms = 3000;
        PretendChat g;
        g.store = &store; g.settings = &settings;
        bool cleared = false;
        std::uint64_t cleared_at = 0;
        auto sender = sender_with(g, s);
        sender->on_report([&](const std::string& said) {
            if (!cleared && said.rfind("ô chat đã xoá", 0) == 0) {
                cleared = true;
                cleared_at = g.clock;
            }
        });
        sender->set_guard([&]() -> std::string {
            return cleared && g.clock >= cleared_at + 1000 ? std::string("Máu thấp — dừng câu đang gõ.")
                                                           : std::string();
        });
        const ChatSender::Result r = sender->send(line_of("test"));
        const long long after = static_cast<long long>(g.clock) - static_cast<long long>(cleared_at + 1000);
        check(!r.sent && r.aborted && g.letters == 0 && g.sent.empty() && g.state == PretendChat::State::Shut &&
                  cleared && after >= 0 && after <= 1000,
              "type_wait_ms=3000, HP under the mark 1 s into it: no letter typed, the line rubbed out and shut " +
                  std::to_string(after) + " ms after");
    }
#ifdef GOBOT_SOURCE_DIR
    {
        // The GUI's pause before a line goes by it, with the HP check.
        std::ifstream in(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/gui_app.cpp", std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::size_t loop = src.find("void GuiApp::chat_loop()");
        const std::size_t quiet = loop == std::string::npos ? loop : src.find("chat_quiet_pause(", loop);
        const std::size_t low = quiet == std::string::npos ? quiet : src.find("if (hp_low()) return kChatLowHp;", quiet);
        check(low != std::string::npos && src.find("nap(settings.quiet_before_ms)") == std::string::npos,
              "gui_app.cpp: the pause before a line is chat_quiet_pause's, ended by HP under the potion's mark");
    }
    {
        // R8R2-1: the GUI remembers a line for the party worker just before its sending Enter goes down
        // (ChatSender::on_send_pressed), in attempt() -- once, and nowhere after the result, where a line
        // the game takes late was missed.
        std::ifstream in(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/gui_app.cpp", std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::size_t attempt = src.find("const auto attempt = [&](const ChatText& line, bool asked)");
        const std::size_t hook = attempt == std::string::npos ? attempt : src.find("sender.on_send_pressed(", attempt);
        const std::size_t said = hook == std::string::npos ? hook : src.find("remember_said(line.typed)", hook);
        const std::size_t sends = hook == std::string::npos ? hook : src.find("sender.send(line)", hook);
        std::size_t calls = 0;
        for (std::size_t at = src.find("remember_said("); at != std::string::npos;
             at = src.find("remember_said(", at + 1))
            ++calls;
        check(hook != std::string::npos && said != std::string::npos && sends != std::string::npos && said < sends &&
                  calls == 2,
              "gui_app.cpp: attempt() remembers the line when its sending Enter goes down, before it sends, and "
              "nothing else does (remember_said: its definition and that one call)");
    }
    {
        // R8bR-1: what GoBot typed is kept where a party watcher of another process reads it. The GUI's
        // remember_said puts the line in SaidLately's file as well as in memory, and said_lately() -- what
        // its party worker and its 30-second floor go by -- reads the file; gobot_cli --chat puts its line
        // there just before the sending Enter, and gobot_cli --autoparty reads the file on every look.
        const auto npos = std::string::npos;
        const auto text_of = [](const char* rel) {
            std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        const std::string gui = text_of("/apps/gobot_gui/gui_app.cpp");
        const std::size_t remember = gui.find("void GuiApp::remember_said(const std::string& typed) {");
        const std::size_t lately = gui.find("GuiApp::said_lately() {");
        const std::size_t last = gui.find("std::uint64_t GuiApp::last_said_ms() {");
        const std::size_t adds =
            remember == npos ? npos : gui.find("SaidLately(SaidLately::default_path()).add(typed, now, wall_ms())", remember);
        const std::size_t reads =
            lately == npos ? npos : gui.find("SaidLately(SaidLately::default_path()).read(now, wall_ms())", lately);
        const std::size_t by_both = last == npos ? npos : gui.find("said_lately()", last);
        check(remember != npos && lately != npos && last != npos && adds != npos && adds < lately && reads != npos &&
                  reads < last && by_both != npos,
              "gui_app.cpp: remember_said keeps the line in SaidLately's file too, and said_lately() -- and with it "
              "last_said_ms() -- reads the file");
        const std::string cli = text_of("/apps/gobot_cli/main.cpp");
        const std::size_t chat = cli.find("int run_chat(const Options& o, const AppConfig& app) {");
        const std::size_t hook =
            chat == npos ? npos
                         : cli.find("sender.on_send_pressed([&] { said_kept = said_file.add(line.typed, now_ms(), "
                                    "wall_ms()); });",
                                    chat);
        const std::size_t sends = hook == npos ? npos : cli.find("sender.send(line)", hook);
        const std::size_t party = cli.find("int run_auto_party(const Options& o, const AppConfig& app, int seconds) {");
        const std::size_t own =
            party == npos ? npos : cli.find("chat.without_own(lines, own_name, said_file.read(now, wall_ms()), now)", party);
        const std::size_t votes = own == npos ? npos : cli.find("chat.saw(lines, now)", own);
        check(hook != npos && sends != npos && own != npos && votes != npos,
              "gobot_cli: --chat keeps its line in SaidLately's file before its sending Enter, and --autoparty reads "
              "the file on every look before the votes");
    }
#endif

    // ---- the dialogs, looked for side by side, are still seen: none on a
    //      clear screen, one with the death dialog up ----
    if (ChatDialogs::can_check(store)) {
        ChatDialogs dialogs;
        Frame clear = game.render();
        check(dialogs.up(clear, store).empty(), "a screen with no dialog: none");
        if (const Frame* dead = store.get(dialogs.death)) {
            Frame died = clear;
            blit(died, *dead, 500, 300);
            // Seen as a dialog, whatever it is taken for: its two buttons are the
            // invitation's too, and any dialog at all keeps Enter back.
            check(!dialogs.up(died, store).empty(), "the death dialog is seen among the four looks");
        }
    } else {
        std::printf("  [skip] the dialogs' pictures are not installed here\n");
    }
}

// ------------------------------------------------------------------ paths

/// A GoBot folder with Vietnamese letters in its name.
///
/// Testers unzip into their Desktop, and a Windows user called Đức has one
/// whose path the machine's code page cannot spell. Through the narrow calls
/// exe_relative's path came out as another one, and no config, picture or
/// glyph in the folder was found. Paths are UTF-8 now, and go to Windows wide.
/// On a machine whose code page is UTF-8 -- this one, it may be -- both read
/// the same and this passes either way; it is for the Vietnamese ones.
void test_unicode_paths() {
    section("Paths: a folder named with Vietnamese letters");

    wchar_t tmp[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n > MAX_PATH) {
        std::printf("  [skip] no temp folder\n");
        return;
    }
    const std::string base = wide_to_utf8(std::wstring(tmp) + L"gobot_test_" +
                                          std::to_wstring(GetCurrentProcessId()));
    const std::string dir = base + "\\GoBot thử của Đức";
    check(wide_to_utf8(utf8_to_wide(dir)) == dir, "a UTF-8 path goes to Windows and back unchanged");
    check(make_folder(dir) && is_folder(dir) && path_exists(dir),
          "the folder is made, and found again by its name");

    // A config, written, read back, and changed in place.
    const std::string ini_path = dir + "\\cấu hình.ini";
    IniFile ini;
    ini.set("Hunt", "character_name", "Tên Nhân Vật");
    ini.set_bool("Hunt", "enabled", true);
    IniFile back;
    check(ini.save(ini_path) && back.load(ini_path) && back.get_bool("Hunt", "enabled", false) &&
              back.get("Hunt", "character_name", "") == "Tên Nhân Vật",
          "an ini in it is written and read back");
    IniFile changed;
    check(IniFile::set_in_place(ini_path, "Hunt", "enabled", "false") && changed.load(ini_path) &&
              !changed.get_bool("Hunt", "enabled", true),
          "and changed in place");

    // A picture, saved and loaded, alone and through the store.
    Frame f;
    f.width = 4;
    f.height = 3;
    f.pixels.assign(static_cast<std::size_t>(4 * 3 * 4), 0);
    for (std::size_t i = 0; i < f.pixels.size(); i += 4) {
        f.pixels[i] = static_cast<std::uint8_t>(i * 5);
        f.pixels[i + 1] = 200;
        f.pixels[i + 2] = 30;
        f.pixels[i + 3] = 255;
    }
    Frame g;
    check(save_png(f, dir + "\\hình ảnh.png") && load_png(dir + "\\hình ảnh.png", g) && g.valid() &&
              g.width == 4 && g.height == 3 && g.pixels == f.pixels,
          "a picture in it is saved and loaded, pixel for pixel");
    TemplateStore store;
    check(store.load(dir) && store.get("hình ảnh") != nullptr,
          "and the picture store finds it there, by its name");
    const std::vector<std::string> found = files_in(dir, "*.png");
    check(found.size() == 1 && found[0] == "hình ảnh.png", "its file is listed by its own name");

    // A log, written and read.
    const std::string log_path = dir + "\\nhật ký.log";
    if (std::FILE* out = open_file(log_path, "ab")) {
        std::fputs("dòng một\n", out);
        std::fclose(out);
    }
    std::string text;
    if (std::FILE* in = open_file(log_path, "rb")) {
        char buf[64]{};
        text.assign(buf, std::fread(buf, 1, sizeof buf, in));
        std::fclose(in);
    }
    check(text == "dòng một\n", "a file in it is written and read");

    check(remove_file(ini_path) && remove_file(dir + "\\hình ảnh.png") && remove_file(log_path) &&
              !path_exists(ini_path),
          "and each file is removed again");
    RemoveDirectoryW(utf8_to_wide(dir).c_str());
    check(!path_exists(dir), "and the folder");

    // Where this program is, in UTF-8: whatever the folder is called, the
    // configs beside it are found.
    const std::string here = exe_relative("configs\\app.ini");
    check(path_exists(here) == (GetFileAttributesW(utf8_to_wide(here).c_str()) !=
                                INVALID_FILE_ATTRIBUTES) &&
              wide_to_utf8(utf8_to_wide(here)) == here,
          "exe_relative's path is UTF-8, and the wide call finds what it names");
}

/// VERSION.txt, which package.ps1 writes beside the program: the version on
/// line 1, "commit <hash>" on line 2. The title and the simple screen show
/// the first, gobot_gui.log both.
void test_version_file() {
    section("Version: VERSION.txt");

    const std::string file = "2026.10.15\r\ncommit abc1234\r\n";
    check(version_line(file, 0) == "2026.10.15" && version_line(file, 1) == "commit abc1234",
          "line 1 the version, line 2 the commit, with CRLF");
    check(version_line("2026.10.15\ncommit abc1234+\n", 1) == "commit abc1234+", "and with LF");
    check(version_line("\xEF\xBB\xBF" "2026.10.15\r\n", 0) == "2026.10.15", "a BOM in front is skipped");
    check(version_line("   2026.10.15  \r\n", 0) == "2026.10.15", "spaces round it are trimmed");
    check(version_line("", 0).empty() && version_line("2026.10.15", 1).empty() &&
              version_line("\r\n2026", 0).empty(),
          "a missing line, or an empty one, is nothing");
    check(version_line("2026\x01.10\xC3\xA9.15\t\r\n", 0) == "2026.10.15",
          "only printable ASCII is kept: it goes into a window title and the log");
    check(version_line(std::string(100, '9') + "\n", 0) == std::string(40, '9'),
          "and at most 40 characters of it");

    // Beside this program there may be one or not: a build of its own has
    // none, and says so.
    if (path_exists(exe_relative("VERSION.txt")))
        check(!package_version().empty(), "the version is read from beside the program");
    else
        check(package_version().empty() && package_commit().empty(),
              "with no VERSION.txt beside the program, there is no version");
    check(version_shown() == (package_version().empty() ? std::string("bản tự build") : package_version()),
          "what is shown: the version, or \"bản tự build\" -- now \"" + version_shown() + "\"");
}

/// gobot_gui.log afresh each session, the one before kept beside it: what a
/// tester sends with a problem holds two sessions, not every character name,
/// chat line and folder path since the day the package was unzipped.
void test_log_each_start() {
    section("Log: afresh each session, the one before kept");

    check(Logger::old_name("C:\\GoBot\\gobot_gui.log") == "C:\\GoBot\\gobot_gui.old.log" &&
              Logger::old_name("C:\\Go.Bot\\gobot") == "C:\\Go.Bot\\gobot.old",
          "the session before goes beside it, as gobot_gui.old.log");

    const std::string log = temp_file("gobot_test_session.log");
    const std::string old = Logger::old_name(log);
    remove_file(log);
    remove_file(old);
    const auto read = [](const std::string& path) {
        std::string text;
        if (std::FILE* f = open_file(path, "rb")) {
            char buf[4096];
            std::size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        return text;
    };
    const auto has = [](const std::string& text, const char* what) {
        return text.find(what) != std::string::npos;
    };

    // Three sessions, a line each. The tests run with no log file, and are
    // left with none.
    Logger& logger = Logger::instance();
    const LogLevel was = logger.level();
    logger.set_level(LogLevel::Info);
    for (const char* line : { "first session", "second session", "third session" }) {
        logger.set_file(log, true);
        GB_INFO("gobot_test: %s", line);
    }
    logger.set_file("");
    const std::string now = read(log), before = read(old);
    check(has(now, "third session") && !has(now, "second session") && !has(now, "first session"),
          "the log holds this session alone");
    check(has(before, "second session") && !has(before, "first session"),
          "the .old one the session before, and the one before that is gone");

    // Without it -- gobot_cli, or a configs folder older than the key --
    // a session is added to the end, as always.
    logger.set_file(log, false);
    GB_INFO("gobot_test: %s", "fourth session");
    logger.set_file("");
    logger.set_level(was);
    const std::string added = read(log);
    check(has(added, "third session") && has(added, "fourth session"),
          "without it, a session is added to the end of the one file");
    remove_file(log);
    remove_file(old);
}

// ------------------------------------------------------------------ config

void test_config() {
    section("Config");

    IniFile ini;
    ini.set("S", "a", "1");
    ini.set_bool("S", "b", true);
    ini.set_float("S", "c", 2.5f);

    check(ini.get_int("S", "a") == 1, "int round-trip");
    check(ini.get_bool("S", "b"), "bool round-trip");
    check(std::abs(ini.get_float("S", "c") - 2.5f) < 0.01f, "float round-trip");
    check(ini.get("S", "missing", "def") == "def", "default for missing key");
    check(ini.get_int("S", "nope", 42) == 42, "default for missing int");

    // Hex values matter: opcodes are written as 0x1234.
    ini.set("S", "hex", "0x1234");
    check(ini.get_int("S", "hex") == 0x1234, "hex value parsed");

    check(InputManager::parse_key("F10") == 121, "parse F10");
    check(InputManager::parse_key("Z") == 90, "parse Z");
    check(InputManager::parse_key("space") == 32, "parse space");
    check(InputManager::parse_key("0x5A") == 90, "parse hex keycode");
    check(InputManager::parse_key("90") == 90, "parse decimal keycode");
    check(InputManager::key_name(121) == "F10", "name F10");
    check(InputManager::key_name(90) == "Z", "name Z");

    // Changing one key without rewriting the file.
    //
    // This is what stands between picking a channel in the GUI and losing
    // every comment in configs/login.ini -- which is where the measured
    // geometry of each page of the crossing is written down, along with how
    // each number was arrived at. `save` would keep the values and throw all
    // of that away.
    {
        const std::string path = temp_file("gobot_inplace_test.ini");
        {
            std::ofstream out(path);
            out << "# a note at the top\n"
                   "\n"
                   "[Login]\n"
                   "# which channel, counting from zero\n"
                   "channel_index=0\n"
                   "username=someone\n"
                   "\n"
                   "[Page.channel_list]\n"
                   "# measured: rows 31 apart\n"
                   "row_step=31\n";
        }

        check(IniFile::set_in_place(path, "Login", "channel_index", 4),
              "an existing key is changed");

        std::string whole;
        {
            std::ifstream in(path);
            std::stringstream ss;
            ss << in.rdbuf();
            whole = ss.str();
        }
        check(whole.find("channel_index=4") != std::string::npos, "to the new value");
        check(whole.find("channel_index=0") == std::string::npos, "and not the old");
        check(whole.find("# a note at the top") != std::string::npos,
              "the comments at the top survive");
        check(whole.find("# which channel, counting from zero") != std::string::npos,
              "and the one above the key itself");
        check(whole.find("# measured: rows 31 apart") != std::string::npos,
              "and the measurements in another section");
        check(whole.find("username=someone") != std::string::npos,
              "and the keys that were not asked about");

        // A key the section has not got is added to it, not to the end of the
        // file where it would land in whatever section came last.
        check(IniFile::set_in_place(path, "Login", "character_index", 2),
              "a missing key is added");
        IniFile back;
        check(back.load(path), "and the file still parses");
        check(back.get_int("Login", "character_index") == 2,
              "in the section it was asked for");
        check(back.get_int("Page.channel_list", "row_step") == 31,
              "without disturbing the next section");
        check(back.get_int("Login", "channel_index") == 4, "or the earlier change");

        // A missing section is created.
        check(IniFile::set_in_place(path, "Crash", "max_restarts", 2),
              "a missing section is added");
        IniFile again;
        again.load(path);
        check(again.get_int("Crash", "max_restarts") == 2, "with its key in it");
        check(again.get_int("Login", "channel_index") == 4, "and nothing else moved");

        // A commented-out key is a note about what the value used to be, and
        // must not be mistaken for the value.
        check(IniFile::set_in_place(path, "Login", "username", "else"),
              "a string value too");
        IniFile third;
        third.load(path);
        check(third.get("Login", "username") == "else", "which reads back");

        check(!IniFile::set_in_place("no_such_file_at_all.ini", "A", "b", 1),
              "a file that is not there is a failure, not a new file");
        std::remove(path.c_str());
    }

    // And the file keeps its own line endings. A text-mode write turned a
    // file that was all LF into one that was all CRLF when one key changed:
    // every line the same, and the file not.
    {
        const auto bytes_of = [](const std::string& p) {
            std::ifstream in(p, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        };
        const auto put = [](const std::string& p, const std::string& bytes) {
            std::ofstream(p, std::ios::binary) << bytes;
        };
        const std::string path = temp_file("gobot_inplace_eol_test.ini");
        const std::string bom = "\xEF\xBB\xBF";

        put(path, bom + "# notes, Kênh 1\n[Login]\nchannel_index=0\n");
        check(IniFile::set_in_place(path, "Login", "channel_index", 4) &&
                  bytes_of(path) == bom + "# notes, Kênh 1\n[Login]\nchannel_index=4\n",
              "an LF file stays LF, with its BOM and its Vietnamese as they were");
        check(IniFile::set_in_place(path, "Login", "character_index", 1) &&
                  bytes_of(path) ==
                      bom + "# notes, Kênh 1\n[Login]\nchannel_index=4\ncharacter_index=1\n",
              "and a key added to it goes in with an LF");

        put(path, "[Login]\r\nchannel_index=0\r\n");
        check(IniFile::set_in_place(path, "Login", "channel_index", 2) &&
                  bytes_of(path) == "[Login]\r\nchannel_index=2\r\n",
              "a CRLF file stays CRLF");
        std::remove(path.c_str());
    }

    // Naming a window.
    //
    // The case that matters is the untitled one: the game's launcher has no
    // title, so it can only be named by class and program, and a day went
    // into finding that out the hard way.
    {
        auto s = InputManager::parse_spec("VTCZone");
        check(s.title == "VTCZone", "a plain spec is a title");
        check(s.window_class.empty() && s.process.empty(), "and nothing else");

        s = InputManager::parse_spec("class=#32770,proc=GhostOnlineVTC.exe");
        check(s.title.empty(), "an untitled window needs no title");
        check(s.window_class == "#32770", "class read out");
        check(s.process == "GhostOnlineVTC.exe", "program read out");

        s = InputManager::parse_spec("title=Game, class=#32770");
        check(s.title == "Game", "title term read out");
        check(s.window_class == "#32770", "spaces around a term are ignored");

        // A title may contain a comma, and must not be chopped at one.
        s = InputManager::parse_spec("Ghost, the game");
        check(s.title == "Ghost, the game", "a comma in a bare title survives");

        check(InputManager::parse_spec("").empty(), "an empty spec names nothing");
    }

    // Finding the window by evidence rather than by filter.
    //
    // The old rule required every identifier given to match and skipped the
    // window otherwise, which made the one window being looked for the one
    // discarded as soon as a single identifier differed -- another build of
    // the client with a different window class, or a process name that could
    // not be read. The bot then said the game was not running while it was on
    // the screen. These cases are what that rule got wrong.
    {
        // This process is certainly running and certainly has a window class
        // that is not the game's, which makes it a safe stand-in for "a window
        // whose class differs from what was asked for".
        InputManager input;

        // A title that only appears inside another window's title must not be
        // enough on its own: that is how combat keys end up in a browser tab.
        check(!input.find_game_window("surely_no_window_is_called_this", {}, {}),
              "a title that matches nothing finds nothing");

        // Asking for a process that does not exist finds nothing, however
        // well the rest matches.
        check(!input.find_game_window({}, {}, "surely_nothing.exe"),
              "a process that is not running finds nothing");
    }
}

// --------------------------------------------------------- protocol handler

void test_protocol_handler() {
    section("ProtocolHandler");

    OpcodeRegistry reg;

    PacketSpec mv;
    mv.opcode = 0x0201;
    mv.semantic = Semantic::SelfMove;
    mv.fields = OpcodeRegistry::parse_field_list("id:u32,x:f32,y:f32", nullptr);
    reg.add(mv);

    PacketSpec hp;
    hp.opcode = 0x0310;
    hp.semantic = Semantic::SelfHpMp;
    hp.fields = OpcodeRegistry::parse_field_list("hp:i32,max_hp:i32", nullptr);
    reg.add(hp);

    GameState st;
    ProtocolHandler ph(reg, st);

    GamePacket move;
    move.opcode = 0x0201;
    move.payload = {
        0x2A, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x20, 0x44,     // 640.0
        0x00, 0x00, 0xF0, 0x43,     // 480.0
    };
    ph.on_packet(move);

    const Self me = st.self();
    check(std::abs(me.pos.x - 640.0f) < 0.01f, "self position applied from packet");

    GamePacket hpp;
    hpp.opcode = 0x0310;
    hpp.payload = { 0x32, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00 };
    ph.on_packet(hpp);
    check(st.self().hp == 50 && st.self().max_hp == 100, "hp applied from packet");

    // A combined vitals packet must drive MP as well as HP. The heal loop
    // gates MP potions on mp_pct(), so an unmapped MP means max_mp stays zero
    // and MP potions silently never fire.
    PacketSpec vitals;
    vitals.opcode = 0x0311;
    vitals.semantic = Semantic::SelfHpMp;
    vitals.fields = OpcodeRegistry::parse_field_list(
        "hp:i32,max_hp:i32,mp:i32,max_mp:i32", nullptr);
    reg.add(vitals);

    GamePacket vp;
    vp.opcode = 0x0311;
    vp.payload = {
        0x49, 0x2D, 0x00, 0x00,     // hp     = 11593
        0xB6, 0x2F, 0x00, 0x00,     // max_hp = 12214
        0xB2, 0x2D, 0x00, 0x00,     // mp     = 11698
        0x39, 0x2F, 0x00, 0x00,     // max_mp = 12089
    };
    ph.on_packet(vp);

    const Self me2 = st.self();
    check(me2.hp == 11593 && me2.max_hp == 12214, "hp and max_hp from a combined packet");
    check(me2.mp == 11698 && me2.max_mp == 12089, "mp and max_mp from a combined packet");
    check(me2.max_mp > 0, "max_mp is non-zero, so mp_pct() is meaningful");
    check(std::abs(me2.mp_pct() - 96.76f) < 0.1f, "mp_pct computes correctly");
    check(std::abs(me2.hp_pct() - 94.91f) < 0.1f, "hp_pct computes correctly");

    // Unmapped traffic must be counted, not dropped silently — it is the
    // input to the next round of protocol discovery.
    GamePacket unknown;
    unknown.opcode = 0x7777;
    unknown.payload = { 1, 2, 3, 4 };
    ph.on_packet(unknown);

    const auto stats = ph.stats();
    check(stats.decoded == 3, "decoded count");   // move, hp, combined vitals
    check(stats.unknown == 1, "unknown count");
    check(stats.unknown_opcodes.count(0x7777) == 1, "unknown opcode recorded");
}

} // namespace

// The alerts' tests, in alerts_test.cpp, reporting through check() and section().
void test_alerts(void (*check)(bool, const std::string&), void (*section)(const char*));
// Quick quest's, in quest_test.cpp, the same way.
void test_quest(void (*check)(bool, const std::string&), void (*section)(const char*));
// GoBot's own keys', in hotkey_test.cpp.
void test_hotkeys(void (*check)(bool, const std::string&), void (*section)(const char*));
// Potion stock's, in potion_test.cpp.
void test_potions(void (*check)(bool, const std::string&), void (*section)(const char*));
// The popup closer's, in popup_test.cpp.
void test_popups(void (*check)(bool, const std::string&), void (*section)(const char*));
// The keeper's watchdog's, in stall_test.cpp.
void test_stall(void (*check)(bool, const std::string&), void (*section)(const char*));
// The party HUD's, in party_hud_test.cpp.
void test_party_hud(void (*check)(bool, const std::string&), void (*section)(const char*));
// Tự chat's "Chỉ khi chưa có tổ đội", in chat_alone_test.cpp.
void test_chat_alone(void (*check)(bool, const std::string&), void (*section)(const char*));
// "Dùng" in Dọn rác, in use_test.cpp.
void test_use(void (*check)(bool, const std::string&), void (*section)(const char*));
// The hunter on scripted plates and the monster lists, in hunter_test.cpp.
void test_hunter_fight(void (*check)(bool, const std::string&), void (*section)(const char*));
// The potion tiers, potions first, and Quái đang thấy, in macro_fight_test.cpp.
void test_macro_fight(void (*check)(bool, const std::string&), void (*section)(const char*));
// Kiểu đánh, Giữ khoảng cách, facing and the fight the skills follow, in style_test.cpp.
void test_hunter_style(void (*check)(bool, const std::string&), void (*section)(const char*));
// The jump key's Thử and what else the key is, in jump_test.cpp.
void test_jump(void (*check)(bool, const std::string&), void (*section)(const char*));
// Skills that follow the fight, in skills_test.cpp.
void test_skills(void (*check)(bool, const std::string&), void (*section)(const char*));

int main() {
    // Before anything else, and never turned off. From here on nothing in this
    // process presses a key, clicks, moves the pointer, changes the focus,
    // posts to another program's window, starts or ends a program, or adopts
    // a running launcher's path: a test that reaches for the real desktop is
    // refused, in the log as an error, instead of acting. On 2026-10-15 one
    // drove a real login crossing on the player's PC for three minutes.
    forbid_real_input();

    Logger::instance().set_level(LogLevel::Error);   // keep test output readable

    std::printf("GoBot self-tests\n");
    // This run's scratch folder: see temp_file.
    if (!run_temp_dir().empty()) CreateDirectoryW(run_temp_dir().c_str(), nullptr);

    test_no_real_input();
    // Every refusal from here on is a test reaching for the real desktop.
    const std::uint64_t guard_refusals = real_input_refusals();
    test_byte_reader();
    test_framing_extract();
    test_framing_detect();
    test_checksum_framing();
    test_frame_trailer();
    test_unwrap();
    test_udp_datagrams();
    test_udp_framing();
    test_own_position();
    test_watchdog_clock();
    test_capture_filters();
    test_socket_owners();
    test_framing_config();
    test_capture_file_inner();
    test_protocol_handler_holds_back();
    test_real_capture();
    test_capture_file();
    test_capture_file_framings();
    test_unpack();
    test_monster_frames();
    test_frame_numbers();
    test_flow_halves();
    test_both_framings_configured();
    test_reassembler();
    test_stream_tap();
    test_opcode_registry();
    test_game_state();
    test_pathfinder();
    test_protocol_analyzer();
    test_map_file();
    test_macro_config();
    test_macro_trigger_config();
    test_macro_engine();
    test_macro_key_kind();
    test_macro_buffs();
    test_executor_pause();
    test_executor_screen_only();
    test_vitals_reader();
    test_find_bars();
    test_macro_triggers();
    test_template_matcher();
    test_assembly_templates();
    test_routines();
    test_gatherer();
    test_party_chat();
    test_party();
    test_shipped_macro_works();
    test_shipped_configs();
    test_colour_templates();
    test_dialog_driver();
    test_anchored_bars();
    test_session_manager();
    test_auto_login();
    test_game_install();
    test_crash_watch();
    test_party_invite();
    test_map_table();
    test_world_anchor();
    test_glyph_font();
    test_mob_detector();
    test_camera_tracker();
    test_field_finder();
    test_minimap_tracker();
    test_hunter_config();
    test_hunter();
    test_bag();
    test_bag_counts();
    test_bag_dialogs();
    test_reviver();
    test_screen_layout();
    test_auto_chat();
    test_alerts(check, section);
    test_quest(check, section);
    test_hotkeys(check, section);
    test_potions(check, section);
    test_popups(check, section);
    test_stall(check, section);
    test_party_hud(check, section);
    test_chat_alone(check, section);
    test_use(check, section);
    test_hunter_fight(check, section);
    test_macro_fight(check, section);
    test_hunter_style(check, section);
    test_jump(check, section);
    test_skills(check, section);
    test_config();
    test_unicode_paths();
    test_version_file();
    test_log_each_start();
    test_protocol_handler();

    // A reach outside the switch's own test was refused, and its error is in
    // the log -- but a line in a log is what a check by hand misses, and
    // package.ps1 goes by the exit code alone. So it fails the run.
    section("Nothing but the switch's own test reached for the real desktop");
    const std::uint64_t outside = real_input_refusals() - guard_refusals;
    check(outside == 0,
          outside == 0 ? std::string("no test reached for the real desktop outside the switch's own")
                       : std::to_string(outside) +
                             " reach(es) for the real desktop outside the switch's own test -- "
                             "give that test pretend hands");

    std::printf("\n%s\n", std::string(50, '-').c_str());
    // Each refusal is in the log above, as an error naming what was refused.
    std::printf("%llu reach(es) for the real desktop refused\n",
                static_cast<unsigned long long>(real_input_refusals()));
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    if (!run_temp_dir().empty()) {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(run_temp_dir()), ec);
    }
    return g_failed == 0 ? 0 : 1;
}
