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
#include "gobot/game_state.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/camera_tracker.hpp"
#include "gobot/field_finder.hpp"
#include "gobot/hunter.hpp"
#include "gobot/macro.hpp"
#include "gobot/map_file.hpp"
#include "gobot/opcode_registry.hpp"
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
#include "gobot/auto_login.hpp"
#include "gobot/bag.hpp"
#include "gobot/dialog_driver.hpp"
#include "gobot/gatherer.hpp"
#include "gobot/party.hpp"
#include "gobot/party_chat.hpp"
#include "gobot/routine.hpp"
#include "gobot/crash_watch.hpp"
#include "gobot/party_invite.hpp"
#include "gobot/session_manager.hpp"
#include "gobot/vitals_reader.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
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
}

void section(const char* name) {
    std::printf("\n== %s ==\n", name);
}

/// A writable path for tests that need a real file.
///
/// Not the working directory: that is wherever the shell happened to be, and
/// when it is somewhere like Program Files the write fails and the test reports
/// a product bug that does not exist. The temp directory is writable by
/// definition.
std::string temp_file(const char* name) {
    char dir[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return name;      // fall back to CWD
    return std::string(dir) + name;
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
        "login_popup1",             // the launcher pages the login flow needs
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
        if (CopyFileA("C:\\Users\\Duong\\source\\GoBot\\build\\bin\\Release\\frame.png",
                      copy.c_str(), FALSE)) {
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

/// The shipped macro config can actually do something.
///
/// This is the test that was missing, and the gap it left was the whole of
/// "HP/MP reads fine but it never drinks". The reader worked, the trigger logic
/// worked and had its own passing tests, the keys were configured with sensible
/// thresholds -- and `key_6_enabled` was false in the file that ships, so the
/// potion key was never armed and nothing in the suite noticed. Every part was
/// tested; the thing as delivered was not.
void test_shipped_macro_works() {
    section("The shipped macro is armed");

    MacroConfig cfg;
    const std::string path = exe_relative("configs/macro.ini");
    check(cfg.load(path), "the shipped macro config loads");

    check(cfg.timers_enabled, "its timers are on");
    check(cfg.vitals_enabled,
          "and it is set to read the bars -- without that the potion keys have "
          "nothing to test against and hold forever");

    // At least one armed key for each of the two things people actually want.
    const struct { MacroTrigger trigger; const char* what; } kWanted[] = {
        { MacroTrigger::HpBelow, "health" },
        { MacroTrigger::MpBelow, "mana" },
    };
    for (const auto& want : kWanted) {
        const MacroKey* found = nullptr;
        for (const auto& key : cfg.keys)
            if (key.enabled && key.trigger == want.trigger && key.keycode != 0) {
                found = &key;
                break;
            }
        check(found != nullptr,
              std::string("a key is armed to drink when ") + want.what + " drops");
        if (found)
            check(found->threshold_pct > 0 && found->threshold_pct < 100,
                  std::string("and its ") + want.what + " threshold is a usable "
                  "percentage -- " + std::to_string(found->threshold_pct) + "%");
    }

    check(cfg.needs_vitals(),
          "and the config knows it needs a reading, so starting the macro "
          "brings the reader up with it");
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

void test_auto_login() {
    section("Auto-login");

    // The crossing is nine screens and the table has to cover them in the
    // order they are met. A page out of order is a flow that clicks the wrong
    // thing on a page that has not appeared yet.
    const auto pages = AutoLogin::default_pages();
    check(pages.size() >= 7, "the table covers the crossing");
    {
        bool ordered = true;
        for (std::size_t i = 1; i < pages.size(); ++i)
            if (static_cast<int>(pages[i].step) <= static_cast<int>(pages[i - 1].step))
                ordered = false;
        check(ordered, "the pages are in the order they are met");
    }

    // Nothing runs until every page has been identified. This is the check
    // that stops the flow starting a launcher it cannot then drive, and
    // leaving a window open for somebody else to close.
    {
        AutoLogin::Settings s;
        s.pages = AutoLogin::default_pages();
        check(s.unidentified().size() == s.pages.size(),
              "a blank table counts every page as unidentified");

        for (auto& page : s.pages) page.picture = "whatever";
        check(s.unidentified().empty(), "a filled table counts none");
    }

    // The settings survive a file, pages and all. Click offsets that did not
    // come back would click the middle of the picture instead of the field
    // above it, and type an account name into nothing.
    {
        AutoLogin::Settings s;
        s.pages = AutoLogin::default_pages();
        s.launcher_path = "C:\\somewhere\\Launcher.exe";
        s.launcher_window = "TheLauncher";
        s.username = "someone";
        s.channel_index = 5;
        s.game_slot = 2;
        s.pages[0].picture = "login_marker";
        s.pages[0].click_dx = 40;
        s.pages[0].click_dy = 120;
        s.pages[1].picture = "games_marker";
        s.pages[1].row_step = 37;

        const std::string path = temp_file("gobot_login_roundtrip_test.ini");
        check(s.save(path), "the login config writes");

        AutoLogin::Settings back;
        check(back.load(path), "and reads back");
        check(back.launcher_path == s.launcher_path &&
              back.launcher_window == s.launcher_window &&
              back.username == s.username &&
              back.channel_index == 5 && back.game_slot == 2,
              "the account and the windows survive");
        check(back.pages.size() == s.pages.size() &&
              back.pages[0].picture == "login_marker" &&
              back.pages[0].click_dx == 40 && back.pages[0].click_dy == 120 &&
              back.pages[1].row_step == 37,
              "each page's picture and click offsets survive");
        std::remove(path.c_str());
    }

    // The password goes in and comes back, and nothing else does.
    {
        const std::string secret = "a password with spaces & symbols";
        const std::string stored = AutoLogin::protect(secret);
        check(!stored.empty() && stored != secret,
              "the password is not written down as itself");
        check(AutoLogin::unprotect(stored) == secret, "and comes back intact");

        check(AutoLogin::protect({}).empty(), "nothing in, nothing out");
        check(AutoLogin::unprotect("not hex at all").empty(),
              "rubbish does not come back as a password");
        check(AutoLogin::unprotect("deadbeef").empty(),
              "and neither does well-formed rubbish");
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
            const bool launcher = p.step == AutoLogin::Step::LoginPage ||
                                  p.step == AutoLogin::Step::GameList ||
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

    // A look at a machine where the game is certainly not running says so,
    // and says it without a capture, a template or a window.
    {
        CrashWatch::Settings s;
        s.game_process = "surely_nothing_is_called_this.exe";
        s.reporter_process = {};
        CrashWatch watch(s);
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
        check(!watch.look().bad(), "a running, answering program reads as fine");
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
    paint_plate(f, 512, 450, 50, 50, true);      // the character

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
    check(players == 1, "finds the character");
    check(found.size() == 3, "and nothing else -- the flower is not a monster");

    // Position comes from the name box, which does not move as health drops.
    for (const Nameplate& n : found) {
        if (n.is_player) {
            check(std::abs(n.x() - 512) <= 3, "character x from the name box");
            check(std::abs(n.y() - 450) <= 3, "character y at its feet");
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

    // Nearest-first, measured from the character.
    Nameplate me;
    check(MobDetector::pick_player(found, f.width, me), "picks the character out");

    check(MobDetector::detect(Frame{}, t).empty(), "no frame finds nothing");
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

    std::this_thread::sleep_for(std::chrono::milliseconds(220));
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

/// Something colourful in a slot, inside its edge. `swap` draws the same
/// picture with red and blue exchanged: one shape, another colour.
void paint_item(Frame& f, const ScreenRect& s, int seed, bool swap = false) {
    for (int y = 4; y < s.h - 4; ++y)
        for (int x = 4; x < s.w - 4; ++x) {
            const int r = 128 + static_cast<int>(100 * std::sin(x * 0.5 + seed));
            const int g = 128 + static_cast<int>(100 * std::cos(y * 0.4 + seed * 2));
            const int b = 40 + (x * y * (seed + 3)) % 200;
            put_px(f, s.x + x, s.y + y, swap ? b : r, g, swap ? r : b);
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
    paint_item(g, slots[12], 1, true);
    float colour_score = 0.0f;
    list.match(g, slots[12], 0.0f, &colour_score);
    check(list.match(g, slots[12], 0.90f) < 0,
          "nor the same drawing in another colour -- score " + std::to_string(colour_score));
    check(list.match(g, slots[0], 0.90f) < 0, "nor an empty slot");

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
                  d.catalog_score >= 0.84f,
              "with the bag on I, the pet tab left alone and strict matches");
        CleanSettings s;
        check(s.load(exe_relative("configs/trash.ini")), "the trash settings load");
        check(s.layout.pitch == 37 && s.layout.grid_dy == 25, "with the measured layout");
    }

    // Finding the bag, and a look-only pass over it, with the game's pictures.
    TemplateStore store;
    const bool have = store.load_best(exe_relative("configs/templates.ini")) &&
                      store.get("grabbed/inventory") && store.get("inven/0/trash_can");
    if (!have) {
        std::printf("  [skip] the bag pictures are not installed here\n");
        return;
    }
    Frame scene = bag_scene(1024, 768, 60);
    for (int y = 300; y < 300 + 220; ++y)            // the bag's dark panel
        for (int x = 290; x < 290 + 272; ++x) put_px(scene, x, y, 20, 22, 30);
    blit(scene, *store.get("grabbed/inventory"), 300, 300);
    blit(scene, *store.get("inven/0/trash_can"), 300 + 215, 300 + 180);

    const BagView found = bag::find(scene, store);
    check(found.found && found.strip.x == 300 && found.strip.y == 300,
          "the bag is found by its tab strip");
    check(found.has_trash && std::abs(found.trash.x - 515) <= 1 &&
              std::abs(found.trash.y - 480) <= 1,
          "and the trash can in its corner");

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
    const auto res = cleaner.run();
    check(res.ran, "a look-only pass searches the bag");
    check(res.matched == 1, "finds the one listed item and not the other -- found " +
                            std::to_string(res.matched));
    check(res.deleted == 0 && res.dropped == 0 && res.failed == 0, "and throws nothing away");

    TrashList empty_list;
    cleaner.set_trash(&empty_list);
    check(!cleaner.run().ran, "with nothing listed, it does not open the bag at all");
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
            x += g.width + 1;
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
        auto [f, s] = slot_with("", false);
        check(counts.read(f, s) == 0, "an item with no count reads as none");
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

    // The ids chosen beforehand, as configs/trash.ini keeps them.
    {
        const std::string path = temp_file("gobot_trash_items.ini");
        {
            std::ofstream out(path);
            out << "[Items]\ndelete=spend/fragment/1103063, spend/mana/8810101\ndrop=\n";
        }
        CleanSettings cs;
        check(cs.load(path) && cs.delete_ids.size() == 2 && cs.drop_ids.empty() &&
                  cs.delete_ids[1] == "spend/mana/8810101",
              "the chosen ids read back from [Items]");
        std::remove(path.c_str());
    }
}

/// The gate between a reading and a key press.
///
/// Runs with require_focus on, so nothing is actually typed anywhere: what is
/// under test is whether the engine decides to press, which the counters show.
void test_macro_triggers() {
    section("Macro triggers");

    InputManager input;
    ExecutionCoordinator coord;
    MacroEngine engine(input, coord);

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

    engine.start();

    // 1. No reading at all: hold, and say which kind of hold it was.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto st = engine.stats();
    check(st.held_no_reading > 0, "holds when there is no reading");
    check(st.held_condition == 0, "does not call that a false condition");
    check(st.skipped_unfocused == 0, "never got as far as trying to press");

    // 2. A reading above the threshold: hold, for the other reason.
    engine.set_vitals(90.0f, 90.0f);
    const std::uint64_t no_reading_before = st.held_no_reading;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    st = engine.stats();
    check(st.held_condition > 0, "holds while HP is above the threshold");
    check(st.held_no_reading == no_reading_before, "stops counting it as missing");
    check(st.skipped_unfocused == 0, "still did not try to press");

    // 3. A reading below it: the key fires. With focus required and no window
    //    the press is refused, which is exactly the counter that proves the
    //    gate opened.
    engine.set_vitals(20.0f, 90.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    st = engine.stats();
    check(st.skipped_unfocused > 0, "fires once HP drops below the threshold");
    check(st.presses == 0, "and still sends nothing without a focused window");

    engine.current_vitals(hp, mp);
    check(hp == 20.0f, "the pushed value is what it acts on");

    // 4. Readings expire. A frozen GUI must make the key hold rather than keep
    //    drinking against a figure from a minute ago, so after the staleness
    //    window the engine reports no reading and goes back to holding.
    const std::uint64_t unfocused_before = st.skipped_unfocused;
    std::this_thread::sleep_for(std::chrono::milliseconds(1800));
    engine.current_vitals(hp, mp);
    check(hp < 0.0f, "a reading older than the staleness window is discarded");

    st = engine.stats();
    const std::uint64_t no_reading_at_expiry = st.held_no_reading;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
    engine.start();
    engine.set_vitals(20.0f, 90.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    st = engine.stats();
    check(st.held_condition > 0, "an MP key ignores HP");
    check(st.skipped_unfocused == 0, "and does not fire on it");
    engine.stop();
}

/// The engine drives real SendInput, so these exercise what can be checked
/// without a window to type into: that it starts and stops cleanly, and that
/// it refuses to press anything when focus is required and absent.
void test_macro_engine() {
    section("Macro engine");

    InputManager input;
    ExecutionCoordinator coord;
    MacroEngine engine(input, coord);

    MacroConfig cfg;
    cfg.apply_defaults();
    cfg.require_focus = true;          // no game window in a test run
    cfg.timers_enabled = true;
    for (auto& k : cfg.keys) {
        k.enabled = true;
        k.interval_ms = 20;            // fire often, so the window is short
    }
    engine.set_config(cfg);

    check(!engine.running(), "starts stopped");
    engine.start();
    check(engine.running(), "start() runs it");

    std::this_thread::sleep_for(std::chrono::milliseconds(220));

    const auto st = engine.stats();
    check(st.presses == 0, "presses nothing with no focused window");
    check(st.skipped_unfocused > 0, "counts the skipped presses instead of failing silently");

    const auto due = engine.next_due_ms();
    check(due.size() == cfg.keys.size(), "reports a due time per key");

    // Suspending holds every key without stopping, and says so in the tally
    // rather than letting an hour of nothing look like an hour of working.
    //
    // This is what stands between an overnight run and six hours of potion
    // keys pressed at a disconnect dialog, so it is worth a test that does not
    // depend on a window: with focus required and absent, nothing presses
    // either way, but which counter moves says which rule did the holding.
    {
        engine.reset_stats();
        engine.set_suspended(true, "testing");
        check(engine.suspended(), "suspends");
        check(engine.suspended_reason() == "testing", "and remembers why");

        std::this_thread::sleep_for(std::chrono::milliseconds(220));
        const auto held = engine.stats();
        check(held.presses == 0, "presses nothing while suspended");
        check(held.held_suspended > 0, "counts what it held back");
        check(held.skipped_unfocused == 0,
              "and holds before it ever gets as far as the focus check");

        engine.set_suspended(false);
        check(!engine.suspended() && engine.suspended_reason().empty(),
              "resumes, and forgets why it stopped");

        std::this_thread::sleep_for(std::chrono::milliseconds(220));
        check(engine.stats().skipped_unfocused > held.skipped_unfocused,
              "and is trying again afterwards");
    }

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

int main() {
    Logger::instance().set_level(LogLevel::Error);   // keep test output readable

    std::printf("GoBot self-tests\n");

    test_byte_reader();
    test_framing_extract();
    test_framing_detect();
    test_checksum_framing();
    test_frame_trailer();
    test_capture_file();
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
    test_colour_templates();
    test_dialog_driver();
    test_anchored_bars();
    test_session_manager();
    test_auto_login();
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
    test_config();
    test_protocol_handler();

    std::printf("\n%s\n", std::string(50, '-').c_str());
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
