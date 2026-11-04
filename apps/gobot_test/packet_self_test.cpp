// gobot_test -- the game's packets, all session (the packets plan, Slice 1):
// your own character's frames (gobot/self_frames.hpp) and what keeps them
// (PacketSelf), the frames counted by kind for the Load line (FrameTally), the
// one thread the capture is started and stopped on (OneWorker), the macro's
// presses told as they go out, [Packets] in app.ini -- and where GoBot's
// window wires them in, read off its source.
//
// Nothing here goes near the game, the network, the desktop or the registry.
// Every frame is built by hand at the frame bytes docs/protocol-findings.md
// gives, as packet_world_test.cpp builds its own; the macro presses with
// pretend hands on a clock of the test's own; the worker runs plain lambdas.
// HP 9232 and MP 8902 are the P0-2 recording's (main.cpp's test_unpack has
// them too); every other number is made up.

#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/frame_tally.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/load_meter.hpp"
#include "gobot/logger.hpp"
#include "gobot/macro.hpp"
#include "gobot/one_worker.hpp"
#include "gobot/packet_self.hpp"
#include "gobot/self_frames.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

constexpr std::uint64_t kT0 = 7'000'000;     // where the test's clock starts

// ------------------------------------------------------------ the frames

/// A frame of `size` bytes, header included, written field by field at the
/// frame bytes the docs give. Only its body is kept: a GamePacket's payload
/// starts after the 8-byte header.
struct Built {
    std::uint16_t opcode;
    Bytes         body;
    Built(std::uint16_t op, std::size_t size) : opcode(op), body(size - 8, 0) {}
    void u16(std::size_t at, std::uint16_t v) { std::memcpy(body.data() + at - 8, &v, 2); }
    void u32(std::size_t at, std::uint32_t v) { std::memcpy(body.data() + at - 8, &v, 4); }
};

/// From the server over TCP, at `depth` wrappers deep (1: unpacked from an
/// 0x0081), at `t`.
GamePacket server(const Built& f, std::uint64_t t, std::uint8_t depth = 1) {
    GamePacket p;
    p.opcode = f.opcode;
    p.payload = f.body;
    p.from_server = true;
    p.depth = depth;
    p.timestamp_ms = t;
    return p;
}

/// From the server over UDP, straight off a datagram.
GamePacket server_udp(const Built& f, std::uint64_t t) {
    GamePacket p = server(f, t, 0);
    p.udp = true;
    return p;
}

/// From your client, over TCP or UDP.
GamePacket client(const Built& f, std::uint64_t t, bool udp) {
    GamePacket p = server(f, t, 0);
    p.from_server = false;
    p.udp = udp;
    return p;
}

/// 0x0051: HP u32 at 12, MP u32 at 16; 24 bytes.
Built vitals_frame(std::uint32_t hp, std::uint32_t mp) {
    Built f(0x0051, 24);
    f.u32(12, hp);
    f.u32(16, mp);
    return f;
}

/// 0x0050, the stat sheet: max HP at 68, HP at 72, max MP at 76, MP at 80;
/// 200 bytes.
Built sheet_frame(std::uint32_t max_hp, std::uint32_t hp, std::uint32_t max_mp, std::uint32_t mp) {
    Built f(0x0050, 200);
    f.u32(68, max_hp);
    f.u32(72, hp);
    f.u32(76, max_mp);
    f.u32(80, mp);
    return f;
}

/// 0x0068, the bag: slot j's item at 12 + 4j, its count at 300 + 2j; 1528
/// bytes. One item in one slot.
Built bag_frame(std::size_t slot, std::uint32_t item, std::uint16_t count) {
    Built f(0x0068, 1528);
    f.u32(12 + 4 * slot, item);
    f.u16(300 + 2 * slot, count);
    return f;
}

/// The same frame a byte short, or a byte long.
GamePacket resized(GamePacket p, int by) {
    if (by < 0) p.payload.pop_back();
    else        p.payload.push_back(0);
    return p;
}

// ------------------------------------------------------------ the readers

void test_self_readers() {
    section("Your own frames: HP and MP, the stat sheet, MP over UDP, the bag, gold (self_frames.hpp)");

    {
        FrameFit fit = FrameFit::OtherOpcode;
        const auto v = read_self_vitals(server(vitals_frame(9232, 8902), kT0), &fit);
        check(v && v->hp == 9232 && v->mp == 8902 && fit == FrameFit::Taken,
              "0x0051 inside 0x0081, 24 bytes: HP 9232 at 12, MP 8902 at 16 -- the P0-2 recording's, the HUD's own");
    }
    {
        // Refused, and why: the side, the transport and the depth are one
        // reason, the size another.
        const Built f = vitals_frame(9232, 8902);
        FrameFit plain = FrameFit::Taken, udp = FrameFit::Taken, up = FrameFit::Taken, up_udp = FrameFit::Taken;
        FrameFit shorter = FrameFit::Taken, longer = FrameFit::Taken, other = FrameFit::Taken;
        const bool none = !read_self_vitals(server(f, kT0, 0), &plain) &&
                          !read_self_vitals(server_udp(f, kT0), &udp) &&
                          !read_self_vitals(client(f, kT0, false), &up) &&
                          !read_self_vitals(client(f, kT0, true), &up_udp) &&
                          !read_self_vitals(resized(server(f, kT0), -1), &shorter) &&
                          !read_self_vitals(resized(server(f, kT0), +1), &longer) &&
                          !read_self_vitals(server(Built(0x0052, 24), kT0), &other);
        check(none && plain == FrameFit::WrongSide && udp == FrameFit::WrongSide && up == FrameFit::WrongSide &&
                  up_udp == FrameFit::WrongSide && shorter == FrameFit::WrongSize && longer == FrameFit::WrongSize &&
                  other == FrameFit::OtherOpcode,
              "and never one straight off the stream, over UDP or from your client -- each said to be the wrong "
              "side -- nor one of 23 or 25 bytes, said to be the wrong size");
    }
    {
        FrameFit fit = FrameFit::OtherOpcode, cut = FrameFit::Taken;
        const auto s = read_stat_sheet(server(sheet_frame(9232, 8100, 8902, 7000), kT0), &fit);
        const auto bad = read_stat_sheet(resized(server(sheet_frame(1, 1, 1, 1), kT0), -1), &cut);
        check(s && s->max_hp == 9232 && s->hp == 8100 && s->max_mp == 8902 && s->mp == 7000 &&
                  fit == FrameFit::Taken && !bad && cut == FrameFit::WrongSize,
              "0x0050 inside 0x0081, 200 bytes: max HP at 68, HP at 72, max MP at 76, MP at 80; 199 bytes refused");
    }
    {
        Built f(0x02F0, 16);
        f.u32(12, 6230);
        FrameFit fit = FrameFit::OtherOpcode, tcp = FrameFit::Taken;
        const auto mp = read_udp_mp(server_udp(f, kT0), &fit);
        const auto wrong = read_udp_mp(server(f, kT0, 0), &tcp);
        check(mp && *mp == 6230 && fit == FrameFit::Taken && !wrong && tcp == FrameFit::WrongSide,
              "0x02F0 over the server's UDP, plain, 16 bytes: MP at 12 -- and not over TCP");
    }
    {
        const auto bag = read_bag(server(bag_frame(6, 8810121, 71), kT0));
        const auto after = read_bag(server(bag_frame(6, 8810121, 60), kT0 + 1000));
        FrameFit cut = FrameFit::Taken;
        const auto bad = read_bag(resized(server(bag_frame(6, 8810121, 71), kT0), -1), &cut);
        bool others_empty = bag.has_value();
        for (std::size_t j = 0; bag && j < kBagSlots; ++j)
            if (j != 6 && ((*bag)[j].item != 0 || (*bag)[j].count != 0)) others_empty = false;
        check(bag && (*bag)[6].item == 8810121 && (*bag)[6].count == 71 && after && (*after)[6].count == 60 &&
                  others_empty && !bad && cut == FrameFit::WrongSize,
              "0x0068 inside 0x0081, 1528 bytes: slot 6 holds 8810121, 71 of them, then 60; the other 71 slots "
              "empty; 1527 bytes refused");
    }
    {
        Built f(0x006B, 28);
        f.u32(12, 1'234'567);
        const auto gold = read_gold(server(f, kT0));
        const auto plain = read_gold(server(f, kT0, 0));
        check(gold && *gold == 1'234'567 && !plain, "0x006B inside 0x0081, 28 bytes: gold at 12; never plain");
    }
}

// ------------------------------------------------------------ the store

void test_self_store() {
    section("PacketSelf: what it keeps of you, and when a value may still be the game's (its epoch)");

    PacketSelf self;
    const std::uint32_t e0 = self.epoch();
    self.on_packets({ server(vitals_frame(9232, 8902), kT0) }, 1);
    PacketSelf::Vitals v = self.vitals();
    const std::uint32_t e1 = self.epoch();
    check(e1 == e0 + 1 && self.start_count() == 1 && v.hp == 9232 && v.mp == 8902 && v.hp_ms == kT0 &&
              v.hp_epoch == e1 && v.mp_epoch == e1,
          "the first batch of a capture raises the epoch, and 0x0051's HP and MP are of it");
    check(v.max_hp == 9232 && v.max_mp == 8902 && v.max_from == PacketSelf::MaxFrom::Seen,
          "with no stat sheet, the highest HP and MP seen stand for the maxima");

    // A gap: what is kept is of an older epoch until the next 0x0051.
    self.mark_gap(kT0 + 100, "frames dropped");
    v = self.vitals();
    const bool stale_gap = v.hp_epoch != self.epoch() && v.hp == 9232;
    self.on_packets({ server(vitals_frame(9000, 8800), kT0 + 200) }, 1);
    v = self.vitals();
    const PacketSelf::Stats gs = self.stats();
    check(stale_gap && v.hp_epoch == self.epoch() && v.hp == 9000 && gs.gaps == 1 && gs.last_gap_ms == kT0 + 100 &&
              std::string(gs.last_gap_why) == "frames dropped",
          "a gap makes HP and MP stale -- kept, of an older epoch -- until the next 0x0051 brings them again");

    self.relink();
    v = self.vitals();
    const bool stale_relink = v.hp_epoch != self.epoch() && v.hp == 9000 && v.mp == 8800;
    self.on_packets({ server(vitals_frame(8990, 8800), kT0 + 300) }, 1);
    check(stale_relink && self.vitals().hp_epoch == self.epoch() && self.stats().relinks == 1,
          "so does a relink, the values kept for the log");

    // Another start of the capture, whoever started it: its first batch.
    const std::uint32_t before_start = self.epoch();
    self.on_packets({ server(Built(0x03C6, 12), kT0 + 400, 0) }, 2);
    v = self.vitals();
    const bool stale_start = self.epoch() == before_start + 1 && v.hp_epoch != self.epoch() && self.start_count() == 2;
    self.on_packets({ server(vitals_frame(8900, 8700), kT0 + 500) }, 2);
    check(stale_start && self.vitals().hp_epoch == self.epoch() && self.stats().starts_seen == 2,
          "and a new start count: a batch of the capture started again makes them stale until the next 0x0051");

    // The stat sheet, and HP above it.
    self.on_packets({ server(sheet_frame(9232, 8900, 8902, 8700), kT0 + 600) }, 2);
    v = self.vitals();
    check(v.max_hp == 9232 && v.max_mp == 8902 && v.max_from == PacketSelf::MaxFrom::StatSheet &&
              v.max_ms == kT0 + 600 && v.over_max == 0,
          "0x0050 gives the maxima");
    self.on_packets({ server(vitals_frame(9500, 8700), kT0 + 700) }, 2);
    v = self.vitals();
    check(v.over_max == 1 && v.max_hp == 9500 && v.hp == 9500,
          "HP above the stat sheet's maximum is counted (over_max), and the maximum is never below the HP: "
          "max(max, hp)");
    self.set_max_from_screen(10'000, 10'000, kT0 + 800);
    check(self.vitals().max_from == PacketSelf::MaxFrom::StatSheet && self.vitals().max_hp == 9500,
          "the bars' own figures never replace a stat sheet's maximum");
    const std::vector<PacketSelf::VitalsPoint> h = self.history();
    check(h.size() == 6 && h.front().hp == 9232 && h.back().hp == 9500 && h.back().at_ms == kT0 + 700,
          "each HP/MP frame kept in order, with its time, for the check against the bars");
    bool seq_in_order = h.size() == 6;
    for (std::size_t i = 0; i < h.size(); ++i) seq_in_order = seq_in_order && h[i].seq == i + 1;
    check(seq_in_order && self.vitals().points == 6,
          "each with its seq, 1 to 6 -- 0x0050's among them -- and the newest's in the vitals");

    // MP over UDP: for the log alone.
    Built udp(0x02F0, 16);
    udp.u32(12, 100);
    self.on_packets({ server_udp(udp, kT0 + 900) }, 2);
    v = self.vitals();
    check(v.udp_mp == 100 && v.udp_mp_ms == kT0 + 900 && v.mp == 8700,
          "0x02F0's MP is kept beside the rest and changes nothing of it: it is the log's");

    // The bag and gold.
    self.on_packets({ server(bag_frame(6, 8810121, 71), kT0 + 1000) }, 2);
    self.on_packets({ server(bag_frame(6, 8810121, 60), kT0 + 1100) }, 2);
    Built gold(0x006B, 28);
    gold.u32(12, 5000);
    self.on_packets({ server(gold, kT0 + 1200) }, 2);
    const PacketSelf::Bag bag = self.bag();
    const std::vector<PacketSelf::BagSeen> frames = self.bag_frames();
    check(bag.known && bag.slots[6].item == 8810121 && bag.slots[6].count == 60 && bag.at_ms == kT0 + 1100 &&
              bag.epoch == self.epoch() && bag.gold == 5000 && frames.size() == 2 && frames.front().slots[6].count == 71,
          "the bag as its last frame has it, of this epoch, with the frames before it and gold for the log");

    // What it counts: used, the wrong size, the wrong side.
    self.on_packets({ resized(server(vitals_frame(1, 1), kT0 + 1300), +1), server(vitals_frame(1, 1), kT0 + 1300, 0),
                      client(vitals_frame(1, 1), kT0 + 1300, true) },
                    2);
    const PacketSelf::Stats::Frames f51 = self.stats().of(0x0051);
    check(f51.used == 5 && f51.wrong_size == 1 && f51.wrong_side == 2 && self.vitals().hp == 9500,
          "0x0051s used, refused for their size and for their side, each counted -- and the refused change "
          "nothing");

    // reset(): a new game process.
    const std::uint32_t before_reset = self.epoch();
    const std::uint64_t seq_before = self.vitals().points;
    self.reset();
    v = self.vitals();
    check(self.epoch() == before_reset + 1 && v.hp == 0 && v.hp_ms == 0 && v.max_from == PacketSelf::MaxFrom::None &&
              !self.bag().known && self.history().empty() && self.bag_frames().empty() &&
              self.stats().of(0x0051).used == 5 && self.stats().resets == 1,
          "reset() forgets HP, MP, the maxima and the bag, and raises the epoch; the counts are kept");
    check(seq_before == 6 && v.points == 0, "the refused frames take no seq; reset() leaves the vitals none");

    // Two frames in one millisecond -- one segment, one read of the capture.
    self.on_packets({ server(vitals_frame(9000, 8000), kT0 + 2000), server(vitals_frame(9000, 7400), kT0 + 2000) }, 2);
    const std::vector<PacketSelf::VitalsPoint> same = self.history();
    v = self.vitals();
    check(same.size() == 2 && same[0].at_ms == same[1].at_ms && same[0].seq == seq_before + 1 &&
              same[1].seq == seq_before + 2 && same[1].mp == 7400 && v.mp == 7400 && v.points == same[1].seq,
          "two frames in one millisecond are both kept, told apart by seq, the later the vitals' -- and the seq "
          "keeps rising across reset()");
}

void test_self_link() {
    section("PacketSelf: the link by its frames -- the tick, world-entry bursts, 0x0014 -- and the macro's presses");

    PacketSelf self;
    const Built tick(0x03C6, 12);
    std::vector<GamePacket> ticks;
    for (int i = 0; i < 90; ++i) ticks.push_back(server(tick, kT0 + 1000 * static_cast<std::uint64_t>(i), 0));
    self.on_packets(ticks, 1);
    PacketSelf::Link l = self.link(kT0 + 89'500);
    check(l.ticks == 90 && l.ticks_last_min == 60 && l.last_tick_ms == kT0 + 89'000 &&
              l.last_server_ms == kT0 + 89'000 && self.stats().of(0x03C6).used == 90,
          "the server's tick, plain and 12 bytes, once a second: 90 of them, 60 within the last minute");

    // A map entry with the wrapped table and HP within 2 s: a burst. A map
    // entry alone is not.
    Built entry(0x0086, 24);
    entry.u16(16, 11);
    entry.u16(18, 1);
    self.on_packets({ server(entry, kT0 + 100'000, 0) }, 1);
    const bool lone = self.link(kT0 + 100'000).bursts == 0;
    self.on_packets({ server(Built(0x0042, 2568), kT0 + 100'400), server(vitals_frame(9232, 8902), kT0 + 100'600) }, 1);
    l = self.link(kT0 + 101'000);
    self.on_packets({ server(sheet_frame(9232, 9232, 8902, 8902), kT0 + 100'700) }, 1);
    const PacketSelf::Link again = self.link(kT0 + 101'000);
    self.on_packets({ server(entry, kT0 + 200'000, 0), server(Built(0x0042, 2568), kT0 + 203'000),
                      server(vitals_frame(9232, 8902), kT0 + 204'000) },
                    1);
    const PacketSelf::Link late = self.link(kT0 + 205'000);
    check(lone && l.entries == 1 && l.last_entry_ms == kT0 + 100'000 && l.bursts == 1 && again.bursts == 1 &&
              late.entries == 2 && late.bursts == 1,
          "a map entry with two of wrapped 0x001E/0x0042/0x0051/0x0050 within 2 s is one world-entry burst, "
          "counted once; one whose frames came 3 s on is none");

    // 0x0014: counted by where it came, never read.
    Built pick(0x0014, 40);
    self.on_packets({ server(pick, kT0 + 300'000, 1), server(pick, kT0 + 300'100, 0), server(pick, kT0 + 300'200, 0),
                      client(pick, kT0 + 300'300, false) },
                    1);
    l = self.link(kT0 + 300'300);
    check(l.selected == 1 && l.channel == 2 && self.stats().of(0x0014).wrong_side == 1,
          "0x0014 inside 0x0081 a character selected, plain a channel switch: counted apart, your client's not");

    // The macro's presses, kept in a ring.
    for (std::uint32_t i = 0; i < PacketSelf::kPresses + 10; ++i) self.note_press('1' + i % 3, kT0 + i);
    const std::vector<PacketSelf::Press> all = self.presses_since(0);
    const std::vector<PacketSelf::Press> late_presses = self.presses_since(kT0 + PacketSelf::kPresses + 5);
    check(all.size() == PacketSelf::kPresses && all.front().at_ms == kT0 + 10 &&
              all.back().at_ms == kT0 + PacketSelf::kPresses + 9 && late_presses.size() == 5,
          "the macro's last 128 presses kept, oldest first, asked for from a moment on");

    check(PacketSelf::opcodes() == std::vector<std::uint16_t>{ 0x0051, 0x0050, 0x02F0, 0x0068, 0x006B, 0x0077,
                                                                0x005A, 0x002E, 0x0014, 0x03C6 },
          "its opcodes, for Apply's filter: every one the packets plan reads beside the packet world's");
}

/// How many of the log's `lines` have `part` in them.
int lines_with(const std::vector<LogLine>& lines, const std::string& part) {
    int n = 0;
    for (const LogLine& l : lines)
        if (has(l.text, part)) ++n;
    return n;
}

void test_self_first_frames() {
    section("PacketSelf: the first frames of a kind, written to the log -- never one that may hold a name");

    check(PacketSelf::first_frame_kind(0x0014) == PacketSelf::FirstFrame::Never &&
              PacketSelf::first_frame_kind(0x001E) == PacketSelf::FirstFrame::Never &&
              PacketSelf::first_frame_kind(0x0065) == PacketSelf::FirstFrame::Never &&
              PacketSelf::first_frame_kind(0x0038) == PacketSelf::FirstFrame::Never,
          "never 0x0014, 0x001E or 0x0065, which carry the character's name -- nor any kind not listed");
    check(PacketSelf::first_frame_kind(0x0051) == PacketSelf::FirstFrame::Hex &&
              PacketSelf::first_frame_kind(0x010D) == PacketSelf::FirstFrame::Hex &&
              PacketSelf::first_frame_kind(0x0050) == PacketSelf::FirstFrame::Decoded &&
              PacketSelf::first_frame_kind(0x0068) == PacketSelf::FirstFrame::Decoded,
          "0x0051's and the rest's bytes; the stat sheet's and the bag's fields only");

    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_level(LogLevel::Info);
    log.set_ring_capacity(400);
    log.clear_ring();

    PacketSelf self;
    std::vector<GamePacket> batch;
    for (int i = 0; i < 3; ++i) batch.push_back(server(vitals_frame(9232, 8902), kT0 + static_cast<std::uint64_t>(i)));
    batch.push_back(server(Built(0x0014, 40), kT0 + 5));
    batch.push_back(server(Built(0x001E, 600), kT0 + 6));
    batch.push_back(server(Built(0x0065, 400), kT0 + 7));
    batch.push_back(client(vitals_frame(1, 1), kT0 + 8, false));
    batch.push_back(server(sheet_frame(9232, 9232, 8902, 8902), kT0 + 9));
    batch.push_back(server(bag_frame(6, 8810121, 71), kT0 + 10));
    self.on_packets(batch, 1);
    const std::vector<LogLine> first = log.ring();
    self.reset();
    log.clear_ring();
    self.on_packets({ server(vitals_frame(9232, 8902), kT0 + 20) }, 1);
    const std::vector<LogLine> after_reset = log.ring();
    log.set_level(was);
    log.set_ring_capacity(0);

    check(lines_with(first, "Packets: first 0x0051 (inside 0x0081, 24 B): 00 00 00 00 10 24 00 00 c6 22 00 00") == 2,
          "the first two 0x0051 from the server, as their payload's bytes -- not the third, nor your client's");
    check(lines_with(first, "Packets: first 0x0050 (inside 0x0081, 200 B): max HP 9232, HP 9232, max MP 8902, MP 8902") == 1 &&
              lines_with(first, "Packets: first 0x0068 (inside 0x0081, 1528 B): 1 of 72 slots used: slot 6 8810121 x71") == 1,
          "the stat sheet and the bag by their fields");
    check(lines_with(first, "Packets: first 0x0014") == 0 && lines_with(first, "Packets: first 0x001E") == 0 &&
              lines_with(first, "Packets: first 0x0065") == 0,
          "and nothing of 0x0014, 0x001E or 0x0065");
    check(lines_with(after_reset, "Packets: first 0x0051") == 1, "once per game process: again after reset()");
}

// ------------------------------------------------------------ the macro's presses

/// The engine on a clock of the test's own, with pretend hands that write
/// down every key, and the presses it tells of.
struct PressRig {
    InputManager         input;
    ExecutionCoordinator coord;
    MacroEngine          engine{ input, coord };
    std::uint64_t        t = 9'000'000;
    bool                 focused = true;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> pressed;   ///< each key, as it went down
    std::vector<std::pair<std::uint32_t, std::uint64_t>> told;
    std::uint64_t        held_ms = 0;      ///< the keys' holds, all of them

    PressRig() {
        MacroEngine::Clock c;
        c.now_ms = [this] { return t; };
        c.sleep_ms = [this](std::uint32_t ms) { t += ms; };
        engine.set_clock(c);
        MacroEngine::Hands h;
        h.focused = [this] { return focused; };
        // Down at t, up once the hold has gone by, as a key is pressed.
        h.key = [this](std::uint32_t vk, int hold) {
            pressed.emplace_back(vk, t);
            if (hold > 0) {
                t += static_cast<std::uint64_t>(hold);
                held_ms += static_cast<std::uint64_t>(hold);
            }
            return true;
        };
        h.held = [](std::uint32_t) { return false; };
        h.release_all = [] {};
        engine.set_hands(h);
        engine.set_on_press([this](std::uint32_t vk, std::uint64_t at) { told.emplace_back(vk, at); });
        MacroKey k;
        k.keycode = 'Z';
        k.label = "Skill";
        k.enabled = true;
        k.interval_ms = 1000;
        k.jitter_pct = 0;
        k.hold_ms = 90;             // held as a key is: its press told at its key-down, not 90 ms on
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys = { k };
        cfg.sequence_enabled = false;
        cfg.require_focus = true;
        cfg.global_jitter_pct = 0;
        engine.set_config(cfg);
        engine.start_stepped();
    }
    ~PressRig() { engine.stop(); }

    void run(std::uint64_t ms) {
        for (const std::uint64_t until = t + ms; t < until; t += 10) {
            engine.set_vitals(100.0f, 100.0f);
            engine.step();
        }
    }
};

void test_press_hook() {
    section("The macro's presses told as they go out (MacroEngine::set_on_press), pretend hands");

    PressRig rig;
    rig.run(3500);
    check(rig.pressed.size() >= 3 && rig.told == rig.pressed && rig.held_ms > 0,
          "every press that went out told once, with its key and the time on the engine's clock its key went "
          "down -- not when it came up, " + std::to_string(rig.held_ms) + " ms of holds later in all -- " +
              std::to_string(rig.pressed.size()) + " pressed, " + std::to_string(rig.told.size()) + " told");

    // Refused: the game not in front.
    rig.pressed.clear();
    rig.told.clear();
    rig.focused = false;
    const std::uint64_t unfocused_before = rig.engine.stats().skipped_unfocused;
    rig.run(3000);
    const bool unfocused = rig.pressed.empty() && rig.told.empty() &&
                           rig.engine.stats().skipped_unfocused > unfocused_before;
    rig.focused = true;

    // Refused: out of the world.
    rig.engine.set_suspended(true, "a test");
    const std::uint64_t suspended_before = rig.engine.stats().held_suspended;
    rig.run(3000);
    const bool suspended = rig.pressed.empty() && rig.told.empty() &&
                           rig.engine.stats().held_suspended > suspended_before;
    rig.engine.set_suspended(false);

    // Refused: the keys held for something else.
    rig.coord.engage_critical("a test");
    const std::uint64_t busy_before = rig.engine.stats().skipped_busy;
    rig.run(3000);
    const bool held = rig.pressed.empty() && rig.told.empty() && rig.engine.stats().skipped_busy > busy_before;
    rig.coord.disengage_critical();

    check(unfocused && suspended && held,
          "a press refused -- the game not in front, out of the world, the keys held -- is never told");
    rig.run(2000);
    check(!rig.pressed.empty() && rig.told == rig.pressed, "and once they go out again, each is");
}

// ------------------------------------------------------------ [Packets]

void test_packets_settings() {
    section("[Packets] in app.ini: vitals, bag and detector_rest, auto or off");

    const PacketsSettings none = packets_settings(IniFile{});
    check(none.vitals == PacketUse::Auto && none.bag == PacketUse::Auto && none.detector_rest == PacketUse::Auto &&
              describe_packets_settings(none) == "vitals auto, bag auto, detector_rest auto",
          "an app.ini without them: all three auto");
    IniFile ini;
    ini.set("Packets", "vitals", "off");
    ini.set("Packets", "bag", " OFF ");
    ini.set("Packets", "detector_rest", "maybe");
    const PacketsSettings s = packets_settings(ini);
    check(s.vitals == PacketUse::Off && s.bag == PacketUse::Off && s.detector_rest == PacketUse::Auto,
          "off as written, in any case and with spaces; anything else auto");
#ifdef GOBOT_SOURCE_DIR
    IniFile shipped;
    const bool read = shipped.load(std::string(GOBOT_SOURCE_DIR) + "/configs/app.ini");
    const PacketsSettings sh = packets_settings(shipped);
    check(read && shipped.get("Packets", "vitals") == "auto" && shipped.get("Packets", "bag") == "auto" &&
              shipped.get("Packets", "detector_rest") == "auto" && sh.vitals == PacketUse::Auto &&
              sh.bag == PacketUse::Auto && sh.detector_rest == PacketUse::Auto,
          "the shipped app.ini: [Packets] vitals=auto, bag=auto, detector_rest=auto");
#endif
}

// ------------------------------------------------------------ the worker

/// Waits up to two seconds for `done`.
bool within_2s(const std::atomic<bool>& done) {
    for (int i = 0; i < 2000 && !done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return done.load();
}

void test_one_worker() {
    section("OneWorker: the capture's jobs, one at a time on a thread of their own (plain lambdas)");

    {
        OneWorker w("test jobs");
        std::mutex m;
        std::vector<int> order;
        bool posted = true;
        for (int i = 0; i < 4; ++i)
            posted = w.post([&m, &order, i] {
                         std::this_thread::sleep_for(std::chrono::milliseconds(2));
                         std::lock_guard<std::mutex> lock(m);
                         order.push_back(i);
                     }) && posted;
        w.wait_idle();
        check(posted && order == std::vector<int>{ 0, 1, 2, 3 } && !w.busy() && w.done() == 4,
              "jobs run one at a time, in the order they were posted; wait_idle() returns once all have run");
    }
    {
        OneWorker w("test jobs");
        std::atomic<bool> began{ false }, release{ false };
        w.post([&began, &release] {
            began = true;
            for (int i = 0; i < 5000 && !release.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
        const bool running = within_2s(began);
        int accepted = 0;
        for (int i = 0; i < static_cast<int>(OneWorker::kMaxJobs); ++i) accepted += w.post([] {}) ? 1 : 0;
        const bool refused = !w.post([] {});
        const bool busy = w.busy();
        release = true;
        w.wait_idle();
        check(running && accepted == static_cast<int>(OneWorker::kMaxJobs) && refused && busy && !w.busy() &&
                  w.done() == OneWorker::kMaxJobs + 1,
              "one running and four waiting: a fifth is refused, busy() says so until all have run");
    }
    {
        OneWorker w("test jobs");
        std::atomic<bool> began{ false }, finished{ false };
        std::atomic<int> dropped_ran{ 0 };
        w.post([&began, &finished] {
            began = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            finished = true;
        });
        w.post([&dropped_ran] { ++dropped_ran; });
        const bool running = within_2s(began);
        w.stop();
        const bool finished_at_stop = finished.load();
        const bool after = w.post([&dropped_ran] { ++dropped_ran; });
        w.wait_idle();
        check(running && finished_at_stop && dropped_ran.load() == 0 && !after && !w.busy(),
              "stop() lets the job running finish and joins it, drops the ones waiting, and takes no more; "
              "wait_idle() then returns at once");
    }
    {
        OneWorker idle("test jobs");
        idle.wait_idle();
        check(!idle.busy() && idle.done() == 0, "a worker given nothing is idle, with no thread to wait for");
    }
}

// ------------------------------------------------------------ the frames' tally

void test_frame_tally() {
    section("FrameTally: the frames by kind for the Load line -- classes, order, the cap, clearing, threads");

    const Built f38(0x0038, 96), f3c6(0x03C6, 12), f51(0x0051, 24), f2f0(0x02F0, 16), f81(0x0081, 30),
        f27(0x0027, 48);
    check(FrameTally::class_of(server(f38, kT0, 0)) == 'S' && FrameTally::class_of(server(f51, kT0, 1)) == 'W' &&
              FrameTally::class_of(server(f51, kT0, 2)) == 'W' && FrameTally::class_of(server_udp(f2f0, kT0)) == 'U' &&
              FrameTally::class_of(client(f81, kT0, false)) == 'C' && FrameTally::class_of(client(f27, kT0, true)) == 'D',
          "S the server's stream, W inside 0x0081, U the server's UDP, C your client's TCP, D your client's UDP");

    FrameTally tally;
    std::vector<GamePacket> b;
    for (int i = 0; i < 5; ++i) b.push_back(server(f38, kT0, 0));
    for (int i = 0; i < 2; ++i) b.push_back(server(f3c6, kT0, 0));
    for (int i = 0; i < 3; ++i) b.push_back(server(f51, kT0, 1));
    b.push_back(server_udp(f2f0, kT0));
    for (int i = 0; i < 4; ++i) b.push_back(client(f81, kT0, false));
    for (int i = 0; i < 2; ++i) b.push_back(client(f27, kT0, true));
    check(!tally.top_changed(), "nothing counted: nothing changed");
    tally.add(b);
    check(tally.counted() == 17 && tally.top_changed(), "17 frames counted, and before the first take that is news");
    const std::string first = tally.take(12);
    check(first == "S 0038:5 03C6:2 | W 0051:3 | U 02F0:1 | C 0081:4 | D 0027:2",
          "each class's kinds, most frames first: " + first);
    check(tally.counted() == 0 && tally.take().empty(), "take() starts the counts again");
    tally.add(b);
    const bool same = !tally.top_changed();
    tally.take();
    std::vector<GamePacket> hunt;
    for (int i = 0; i < 10; ++i) hunt.push_back(server(Built(0x003F, 52), kT0, 0));
    tally.add(hunt);
    check(same && tally.top_changed(), "the same kinds again are no change; a new kind among the top three is");
    tally.take();

    // More kinds than asked for, and more text than the line may carry.
    std::vector<GamePacket> many;
    for (std::uint16_t op = 1; op <= 15; ++op)
        for (int i = 0; i < op; ++i) many.push_back(server(Built(op, 12), kT0, 0));
    tally.add(many);
    const std::string top = tally.take(12);
    check(top.rfind("S 000F:15 000E:14 000D:13", 0) == 0 && has(top, " 0004:4 +3 kinds (6)") && !has(top, "0001:1"),
          "the 12 most frequent of a class, then how many kinds and frames are left out: " + top);
    std::vector<GamePacket> flood;
    for (std::uint32_t op = 0x100; op < 0x400; ++op) flood.push_back(server(Built(static_cast<std::uint16_t>(op), 12), kT0, 0));
    tally.add(flood);
    const std::string cut = tally.take(10'000);
    check(cut.size() <= FrameTally::kMaxText && cut.size() > FrameTally::kMaxText - 32 &&
              cut.substr(cut.size() - 4) == " ...",
          "held to " + std::to_string(FrameTally::kMaxText) + " bytes, its end said cut: " +
              std::to_string(cut.size()) + " bytes");

    // New kinds this session, in the order they came.
    FrameTally fresh;
    fresh.add({ server(f38, kT0, 0), server(f51, kT0, 1), server(f38, kT0, 0) });
    const std::string kinds = fresh.take_new_kinds();
    fresh.add({ server(f38, kT0, 0), client(f27, kT0, true) });
    const std::string more = fresh.take_new_kinds();
    fresh.take();
    fresh.add({ server(f51, kT0, 1) });
    check(kinds == "S 0038, W 0051" && more == "D 0027" && fresh.take_new_kinds().empty(),
          "the kinds seen for the first time, once each, whatever the takes between: " + kinds + "; " + more);

    // From two threads at once.
    FrameTally shared;
    const auto feed = [&shared, &f38] {
        for (int i = 0; i < 2000; ++i) shared.add({ server(f38, kT0, 0) });
    };
    std::thread a(feed), c(feed);
    a.join();
    c.join();
    check(shared.counted() == 4000 && shared.take() == "S 0038:4000", "two threads adding at once: none lost");
}

void test_frames_line() {
    section("The Load line's frames: line, with and without Apply's filter (made-up numbers)");

    LoadSnapshot before, now;
    before.at_ms = 1'000'000;
    now.at_ms = 1'060'000;
    before.capture_running = now.capture_running = true;
    const std::string none = load_report(before, now);
    now.frame_kinds = "over 600 s: S 0038:1234 03C6:600 | W 0051:30";
    const std::string with = load_report(before, now);
    now.frames_filtered = true;
    const std::string filtered = load_report(before, now);
    check(!has(none, "frames:") &&
              has(with, " | log 0.0 lines/min, 0 KB\n      frames: over 600 s: S 0038:1234 03C6:600 | W 0051:30\n") &&
              !has(with, "filtered") &&
              has(filtered, "\n      frames: over 600 s: S 0038:1234 03C6:600 | W 0051:30 (filtered by Apply's "
                            "opcode list)\n"),
          "a frames: line under reads: only when the window filled it, said to be filtered while Apply's opcode "
          "filter is on" + std::string("\n") + filtered);
}

// ------------------------------------------------------------ the wiring

#ifdef GOBOT_SOURCE_DIR
std::string source(const std::string& rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string body(const std::string& src, const std::string& head) {
    const std::size_t from = src.find(head);
    if (from == std::string::npos) return {};
    const std::size_t end = src.find("\n}", from);
    return end == std::string::npos ? std::string() : src.substr(from, end + 2 - from);
}

/// The `n` characters of `text` after the first `anchor`; "" with none.
std::string after(const std::string& text, const std::string& anchor, std::size_t n) {
    const std::size_t at = text.find(anchor);
    return at == std::string::npos ? std::string() : text.substr(at + anchor.size(), n);
}

/// The `n` characters of `text` before the first `anchor`; "" with none.
std::string before(const std::string& text, const std::string& anchor, std::size_t n) {
    const std::size_t at = text.find(anchor);
    if (at == std::string::npos) return {};
    const std::size_t from = at > n ? at - n : 0;
    return text.substr(from, at - from);
}

int count_in(const std::string& text, const std::string& part) {
    int n = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
    return n;
}
#endif

void test_self_wiring() {
    section("The packets all session wired in: the job reads of HP and MP, [Packets], the presses (their source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    if (gui.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::string quest = source("/apps/gobot_gui/simple_quest.cpp");
    const std::string popups = source("/apps/gobot_gui/simple_popups.cpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    const std::string skills = source("/apps/gobot_gui/simple_skills.cpp");

    // Every job's HP read through hp_percent_now() -- the bars' as before
    // until Slice 3 -- and the skills' MP floor through mp_percent_now().
    const std::string kNow = "const float hp = hp_percent_now();";
    const struct { const char* where; std::string near; } kReads[] = {
        { "the charms' start", before(gui, "return not_now(\"Chưa đọc được máu của nhân vật, dùng bùa sau.\");", 120) },
        { "the shop's start", before(gui, "return not_now(\"Chưa đọc được máu của nhân vật, mua bình sau.\", 15'000);", 120) },
        { "the trash's start", before(gui, "tell(\"Máu đang thấp, dọn rác sau.\");", 200) },
        { "the chat's hp_low", after(gui, "const auto hp_low = [this] {", 80) },
        { "the quest's low_hp", after(quest, "hands.low_hp = [this] {", 80) },
        { "the popups' hp_low", after(popups, "const auto hp_low = [this] {", 80) },
        { "the front keeper's HP", before(view, "const bool hp_low = vitals_running_ && hp >= 0.0f", 80) },
    };
    std::string missed;
    for (const auto& r : kReads)
        if (!has(r.near, kNow) || has(r.near, "vitals_.hp_percent()")) missed += std::string(" ") + r.where + ";";
    const int guards = count_in(gui, "\"trash cleaning\")) return false;");
    const int guards_now = count_in(gui, "\"trash cleaning\")) return false;\n            " + kNow);
    check(missed.empty() && guards == 3 && guards_now == 3 && has(gui, "in.hp_pct = hp_percent_now();") &&
              has(quest, "in.hp_pct = hp_percent_now();") && !has(gui + quest, "in.hp_pct = vitals_.hp_percent();") &&
              has(skills, "const float mp = vitals_running_ ? mp_percent_now() : -1.0f;"),
          "every job's HP read -- the charms', the shop's and the trash's starts and guards, the chat's, the "
          "quest's, the popups', the front keeper's -- is hp_percent_now(), and the skills' MP mp_percent_now()" +
              (missed.empty() ? std::string() : " -- not:" + missed));
    check(has(gui, "reviver.set_hp([this] { return vitals_.hp_percent(); });") &&
              has(body(gui, "void GuiApp::tick() {"), "const float hp = vitals_running_ ? vitals_.hp_percent() : -1.0f;"),
          "the reviver's HP and the HP-back recast still read the bars: death stays on the screen");

    // [Packets] read where [Performance] is, and the macro's presses told.
    const std::string init = body(gui, "void GuiApp::initialise() {");
    const std::size_t perf = init.find("load_performance(app_ini);");
    const std::size_t packets = init.find("packets_settings_ = packets_settings(app_ini);");
    check(perf != std::string::npos && packets != std::string::npos && perf < packets && packets - perf < 300,
          "initialise() reads [Packets] straight after [Performance]");
    const std::size_t fight = init.find("macro_->set_fight(");
    const std::size_t told = init.find("macro_->set_on_press([this](std::uint32_t vk, std::uint64_t at_ms) { "
                                       "packet_self_.note_press(vk, at_ms); });");
    check(fight != std::string::npos && told != std::string::npos && fight < told,
          "and hands the macro's presses to PacketSelf, beside set_fight");
    const std::string macro = source("/src/macro.cpp");
    const std::string press = body(macro, "bool MacroEngine::press(");
    const std::size_t counted = press.find("++stats_.presses;");
    const std::size_t hook = press.find("if (ok && on_press_) on_press_(keycode, down_ms);");
    const std::size_t down = press.find("const std::uint64_t down_ms = clock_now();");
    const std::size_t keyed = press.find("hands_.key ? hands_.key(keycode, hold)");
    check(counted != std::string::npos && hook != std::string::npos && counted < hook && down != std::string::npos &&
              keyed != std::string::npos && down < keyed,
          "macro.cpp: press() tells of a press that went out after its count, outside the stats' lock, at the time "
          "its key went down");

    // The capture's start count, and its gaps.
    const std::string cap = source("/src/packet_capture.cpp");
    const std::string start = body(cap, "bool PacketCapture::start(const CaptureConfig& cfg) {");
    const std::size_t counts = start.find("starts_.fetch_add(1);");
    const std::size_t threads = start.find("capture_thread_  = std::thread(");
    check(counts != std::string::npos && threads != std::string::npos && counts < threads &&
              has(body(cap, "PacketCapture::Stats PacketCapture::stats() const {"), "out.starts = starts_.load();") &&
              has(cap, "if (dir == Direction::ToClient) stats_.server_resync_bytes += scan.resyncs;"),
          "packet_capture.cpp: the start counted before its threads start, never reset; the server's resyncs "
          "counted apart");
    const std::string watch = source("/apps/gobot_gui/packets_watch.cpp");
    const std::string gaps = body(watch, "void GuiApp::note_capture_gaps(std::uint64_t now) {");
    check(has(gaps, "if (cs.starts != capture_starts_seen_) {") && has(gaps, "packet_self_.mark_gap(now, why);") &&
              has(gaps, "cs.pcap_dropped") && has(gaps, "cs.unwrap.unpack_failed") && has(gaps, "cs.server_resync_bytes"),
          "packets_watch.cpp: a gap is frames dropped, a wrapper that would not unpack or the server's stream "
          "resynced -- measured from each start");
    const std::size_t opened = gaps.find("if (cs.starts != capture_starts_seen_) {");
    const std::size_t live = gaps.find("if (!capture_.running()) return;");
    const std::string fresh = opened != std::string::npos && live != std::string::npos && opened < live
                                  ? gaps.substr(opened, live - opened)
                                  : std::string();
    check(!fresh.empty() && !has(fresh, "return") && has(fresh, "capture_seen_ = PacketCapture::Stats{};") &&
              has(fresh, "capture_seen_.server_resync_bytes = cs.server_resync_bytes;"),
          "a new start's first look compares too, from 0 -- a drop or a broken wrapper in its first second is a "
          "gap -- the server's resyncs alone measured from there (joining its stream costs one)");
    const std::string snap = body(gui, "LoadSnapshot GuiApp::load_snapshot() {");
    check(has(snap, "frame_tally_.take(12)") && has(snap, "capture_.filtering()") && has(snap, "600'000") &&
              has(snap, "frame_tally_.top_changed()"),
          "load_snapshot: the frames by kind every ten minutes, or when they change, said filtered under Apply");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_packet_self(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_self_readers();
    test_self_store();
    test_self_link();
    test_self_first_frames();
    test_press_hook();
    test_packets_settings();
    test_one_worker();
    test_frame_tally();
    test_frames_line();
    test_self_wiring();
}
