// gobot_test -- "Vị trí từ gói tin": the hunter's eyes on the packet world,
// with the screen as the fallback; when the packets may steer; what the
// packets say laid over what the screen shows; when the capture for
// positions is started again; and the game's UDP ports by its process id.
//
// Nothing here goes near the game. The packet world is fed frames built by
// hand at the frame bytes docs/protocol-findings.md §4e and §4f give, as
// packet_world_test.cpp builds them, stamped on a clock the test winds on.
// The screen is a set of scripted plates, the capture a pair of flags, and
// the keyboard writes down every key. Every id, slot, position and HP is
// made up: no recording of the game's bytes has been through any of this.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "gobot/common.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/hunter.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/net_discovery.hpp"
#include "gobot/packet_eyes.hpp"
#include "gobot/packet_world.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

constexpr std::uint32_t kId = 4321;          // your character's id: made up
constexpr std::uint64_t kT0 = 5'000'000;     // where the test's clock starts

// ------------------------------------------------------------ the frames

/// A frame of `size` bytes, header included, written field by field at the
/// frame bytes the docs give. Only its body is kept: a GamePacket's payload
/// starts after the 8-byte header.
struct Built {
    std::uint16_t opcode;
    Bytes         body;
    Built(std::uint16_t op, std::size_t size) : opcode(op), body(size - 8, 0) {}
    void u8(std::size_t at, std::uint8_t v)   { body[at - 8] = v; }
    void u16(std::size_t at, std::uint16_t v) { std::memcpy(body.data() + at - 8, &v, 2); }
    void u32(std::size_t at, std::uint32_t v) { std::memcpy(body.data() + at - 8, &v, 4); }
    void i32(std::size_t at, std::int32_t v)  { std::memcpy(body.data() + at - 8, &v, 4); }
    void f32(std::size_t at, float v)         { std::memcpy(body.data() + at - 8, &v, 4); }
};

/// As your client sent it at `t`: over UDP, read straight off a datagram.
GamePacket from_client(const Built& f, std::uint64_t t) {
    GamePacket p;
    p.opcode = f.opcode;
    p.payload = f.body;
    p.from_server = false;
    p.udp = true;
    p.timestamp_ms = t;
    return p;
}

/// As the server sent it at `t`, straight off the TCP stream.
GamePacket from_server(const Built& f, std::uint64_t t) {
    GamePacket p;
    p.opcode = f.opcode;
    p.payload = f.body;
    p.from_server = true;
    p.timestamp_ms = t;
    return p;
}

/// As the server sent it at `t`, found inside an 0x0081.
GamePacket wrapped(const Built& f, std::uint64_t t) {
    GamePacket p = from_server(f, t);
    p.depth = 1;
    return p;
}

/// 0x0027: a walk of yours starts (speed 9), stops (0) or turns.
Built move_frame(float x, float y, float speed, int facing, std::uint32_t id = kId) {
    Built f(0x0027, 48);
    f.u32(8, id); f.u32(12, id);
    f.f32(16, x); f.f32(20, y); f.f32(24, speed);
    f.u8(28, static_cast<std::uint8_t>(facing > 0 ? 0x01 : 0xFF));
    return f;
}

/// 0x0031: a skill cast of yours, where you cast it in whole pixels -- and
/// nothing of which way you face.
Built cast_frame(std::uint16_t x, std::uint16_t y) {
    Built f(0x0031, 92);
    f.u32(8, kId); f.u32(12, kId);
    f.u16(16, x); f.u16(18, y); f.u32(20, 51102);
    return f;
}

/// 0x0086 from the server: you enter (map, area) at its spawn point.
Built entry_frame(std::uint16_t map, std::uint16_t area, std::uint16_t x, std::uint16_t y) {
    Built f(0x0086, 24);
    f.u16(16, map); f.u16(18, area); f.u16(20, x); f.u16(22, y);
    return f;
}

/// One monster of the map's table.
struct Entry {
    std::uint16_t slot;
    std::uint32_t type;
    std::uint8_t  level;
    std::int8_t   facing;
    std::uint16_t x, y;
    float         speed;
    std::uint32_t hp, max_hp;
};

/// 0x0042, the map's table, in its columns of 50 entries.
Built table_frame(const std::vector<Entry>& entries) {
    Built t(0x0042, 2568);
    t.u32(12, static_cast<std::uint32_t>(entries.size()));
    for (std::size_t k = 0; k < 50; ++k) t.u16(316 + 2 * k, 0xFFFF);
    for (std::size_t k = 0; k < entries.size(); ++k) {
        const Entry& e = entries[k];
        t.u8(16 + k, static_cast<std::uint8_t>(e.speed != 0.0f ? 1 : 0));
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
}

/// 0x0038: a monster's stop (state 0), walk (1) or hit (9), with the HP it
/// leaves -- below 0 for a kill -- and in a hit your id and the damage.
Built event_frame(std::uint8_t state, std::uint32_t slot, int facing, float speed, std::uint16_t x,
                  std::uint16_t y, std::int32_t hp, std::uint32_t damage = 0) {
    Built f(0x0038, 96);
    f.u8(12, state); f.u32(16, slot);
    f.u8(24, static_cast<std::uint8_t>(facing)); f.f32(28, speed);
    f.u16(32, x); f.u16(34, y);
    f.u32(48, state == 9 ? kId : 0xFFFFFFFFu); f.u32(52, damage); f.i32(64, hp);
    return f;
}

/// A plate standing at `x`, `y` on the screen, as hunter_test.cpp makes one:
/// `x` the middle of its name, `y` its feet, where the health bar is.
Nameplate plate(int x, int y, const std::string& name = {}, float hp = 1.0f) {
    Nameplate n;
    n.bar = ScreenRect{ x - 20, y, 40, 3 };
    n.plate = ScreenRect{ x - 30, y - 14, 60, 12 };
    n.name = name;
    n.name_confidence = name.empty() ? 0.0f : 0.9f;
    n.hp = hp;
    return n;
}

/// A monster where `world` says, as PacketWorld::View lists one.
PacketWorld::Mob world_mob(std::uint32_t slot, float x, float y, bool moving = false) {
    PacketWorld::Mob m;
    m.slot = slot;
    m.x = x;
    m.y = y;
    m.hp = 100;
    m.max_hp = 100;
    m.moving = moving;
    return m;
}

// ------------------------------------------------------- packet_health

/// A world healthy from kT0 + 400: a map entry, its table, a report of yours.
void healthy_world(PacketWorld& w) {
    w.on_packets({ from_server(entry_frame(11, 1, 4690, 1180), kT0) });
    w.on_packets({ wrapped(table_frame({ { 3, 1004401, 44, 1, 4800, 1180, 0.0f, 7676, 7676 } }), kT0 + 300) });
    w.on_packets({ from_client(move_frame(4690.0f, 1180.0f, 0.0f, 1), kT0 + 400) });
}

/// The capture as it is at `at`: running, the game's TCP heard half a
/// second before.
PacketLink link_at(std::uint64_t at) {
    PacketLink l;
    l.capturing = true;
    l.last_tcp_ms = at - 500;
    return l;
}

void test_packet_health() {
    section("Vị trí từ gói tin: when the packets may steer (packet_health)");

    HunterConfig on;
    on.packet_positions = PacketPositions::On;
    const auto judge = [](const PacketWorld& w, const HunterConfig& cfg, const PacketLink& l, std::uint64_t at) {
        return packet_health(w.view(at), w.stats(), l, cfg, at);
    };

    PacketWorld w;
    healthy_world(w);
    const std::uint64_t now = kT0 + 10'000;
    {
        const PacketHealth h = judge(w, on, link_at(now), now);
        check(h.ok && h.why == PacketWhy::InUse,
              "a map entry, its table, a report of yours, the game's TCP half a second ago, no list: healthy");
    }
    {
        HunterConfig off = on, watch = on;
        off.packet_positions = PacketPositions::Off;
        watch.packet_positions = PacketPositions::Watch;
        const PacketHealth h_off = judge(w, off, link_at(now), now);
        const PacketHealth h_watch = judge(w, watch, link_at(now), now);
        check(!h_off.ok && h_off.why == PacketWhy::Off && !h_watch.ok && h_watch.why == PacketWhy::Watch,
              "Off and Watch never steer, however healthy the packets");
    }
    {
        PacketLink no_npcap = link_at(now), stopped = link_at(now), quiet = link_at(now), never = link_at(now),
                   just = link_at(now);
        no_npcap.npcap = false;
        stopped.capturing = false;
        quiet.last_tcp_ms = now - 3001;
        never.last_tcp_ms = 0;
        just.last_tcp_ms = now - 3000;
        const PacketHealth a = judge(w, on, no_npcap, now), b = judge(w, on, stopped, now),
                           c = judge(w, on, quiet, now), d = judge(w, on, never, now), e = judge(w, on, just, now);
        check(a.why == PacketWhy::NoNpcap && b.why == PacketWhy::NoCapture && c.why == PacketWhy::NoTraffic &&
                  d.why == PacketWhy::NoTraffic && !a.ok && !b.ok && !c.ok && !d.ok,
              "no Npcap, no capture, the game's TCP silent for more than 3 s or never heard: the screen, and "
              "each says why");
        check(e.ok, "silent for exactly 3 s: still healthy");
    }
    {
        HunterConfig skipping = on, first = on, only = on;
        skipping.skip_names = { "NgaQuy" };
        first.first_names = { "HoLy" };
        only.first_only = true;
        const bool all = judge(w, skipping, link_at(now), now).why == PacketWhy::ListInUse &&
                         judge(w, first, link_at(now), now).why == PacketWhy::ListInUse &&
                         judge(w, only, link_at(now), now).why == PacketWhy::ListInUse;
        PacketLink no_npcap = link_at(now);
        no_npcap.npcap = false;
        check(all, "Không đánh, Đánh trước or 'only those' in use: the screen, whose plates have names");
        check(judge(w, skipping, no_npcap, now).why == PacketWhy::NoNpcap,
              "the first rule broken is the reason: no Npcap before a list");
    }
    {
        // A map entry and its table, and no report of yours: where the server
        // put you, not where you are, should the capture miss the client.
        PacketWorld seen;
        seen.on_packets({ from_server(entry_frame(11, 1, 4690, 1180), kT0) });
        seen.on_packets({ wrapped(table_frame({ { 3, 1004401, 44, 1, 4800, 1180, 0.0f, 7676, 7676 } }), kT0 + 300) });
        const PacketHealth h = judge(seen, on, link_at(now), now);
        check(!h.ok && h.why == PacketWhy::NotMoved,
              "no report of yours since the capture began, only the map entry: the screen, until you move");
    }
    {
        PacketWorld two;
        healthy_world(two);
        two.on_packets({ from_client(move_frame(100.0f, 100.0f, 0.0f, 1, kId + 1), kT0 + 2000) });
        const PacketHealth during = judge(two, on, link_at(kT0 + 31'999), kT0 + 31'999);
        const PacketHealth after = judge(two, on, link_at(kT0 + 32'000), kT0 + 32'000);
        check(during.why == PacketWhy::Foreign && after.ok,
              "another character's report: the screen for 30 s after it, a second client on this PC");
    }
    {
        PacketWorld odd;
        healthy_world(odd);
        GamePacket cut = from_server(event_frame(0, 3, 1, 0.0f, 4800, 1180, 7676), kT0 + 2000);
        cut.payload.pop_back();
        odd.on_packets({ cut });
        const PacketHealth during = judge(odd, on, link_at(kT0 + 61'999), kT0 + 61'999);
        const PacketHealth after = judge(odd, on, link_at(kT0 + 62'000), kT0 + 62'000);
        check(during.why == PacketWhy::WrongSize && after.ok,
              "a frame of a size never seen: the screen for 60 s after it, a game update maybe");
    }
    {
        PacketWorld moved;
        healthy_world(moved);
        moved.on_packets({ from_server(entry_frame(11, 2, 900, 600), kT0 + 20'000) });
        const PacketHealth entered = judge(moved, on, link_at(kT0 + 21'000), kT0 + 21'000);
        moved.on_packets({ wrapped(table_frame({ { 1, 1004401, 44, 1, 1000, 600, 0.0f, 7676, 7676 } }),
                                   kT0 + 21'200) });
        const PacketHealth tabled = judge(moved, on, link_at(kT0 + 21'300), kT0 + 21'300);
        check(entered.why == PacketWhy::MapChanged && tabled.ok,
              "a map entry: the screen until its table comes, then the packets");

        PacketWorld no_table;
        healthy_world(no_table);
        no_table.on_packets({ from_server(entry_frame(11, 2, 900, 600), kT0 + 20'000) });
        const PacketHealth early = judge(no_table, on, link_at(kT0 + 25'999), kT0 + 25'999);
        const PacketHealth late = judge(no_table, on, link_at(kT0 + 26'000), kT0 + 26'000);
        check(early.why == PacketWhy::MapChanged && late.ok, "or, with no table, until 6 s after the entry");
    }
    {
        // Begun mid-map: no entry, no table, the monsters one event at a time.
        PacketWorld mid;
        mid.on_packets({ from_client(move_frame(2000.0f, 900.0f, 0.0f, 1), kT0) });
        const PacketHealth none = judge(mid, on, link_at(kT0 + 9000), kT0 + 9000);
        mid.on_packets({ from_server(event_frame(0, 4, 1, 0.0f, 2100, 900, 3044), kT0 + 10'000) });
        const PacketHealth early = judge(mid, on, link_at(kT0 + 15'999), kT0 + 15'999);
        const PacketHealth late = judge(mid, on, link_at(kT0 + 16'000), kT0 + 16'000);
        check(none.why == PacketWhy::NoMonsters && early.why == PacketWhy::NoMonsters && late.ok,
              "begun mid-map: the screen until 6 s after the first monster frame");
    }
    {
        PacketWorld walk;
        healthy_world(walk);
        walk.on_packets({ from_client(move_frame(4690.0f, 1180.0f, 9.0f, -1), kT0 + 5000) });
        // Followed as long as the view says: 3 s for a walk the hunter holds,
        // 0.6 s for any other.
        const auto judged = [&](std::uint64_t follow, std::uint64_t at) {
            return packet_health(walk.view(at, follow), walk.stats(), link_at(at), on, at);
        };
        bool both = true;
        for (const std::uint64_t follow : { PacketWorld::kOwnReckonMs, PacketWorld::kOwnStepReckonMs }) {
            const std::uint64_t late = follow + PacketHealth::kReportLateMs;
            const bool followed = judged(follow, kT0 + 5000 + follow).ok;
            const bool uncertain = walk.view(kT0 + 5000 + late, follow).me.uncertain;
            const bool still = judged(follow, kT0 + 5000 + late).ok;
            const bool lost = judged(follow, kT0 + 5001 + late).why == PacketWhy::Uncertain;
            both = both && followed && uncertain && still && lost;
        }
        check(PacketWorld::kOwnReckonMs == 3000 && PacketWorld::kOwnStepReckonMs == 600 && both,
              "a walk with no report: followed 3 s when the hunter holds it -- let go of and taken up again "
              "inside that -- and 0.6 s when not; then held there, uncertain, 300 ms more for the report of its "
              "end, and then the screen");
        // Moving with no way to follow it: a map entry says no facing, and a
        // walk whose facing byte is neither way keeps none.
        PacketWorld lost_way;
        lost_way.on_packets({ from_server(entry_frame(11, 1, 4690, 1180), kT0) });
        lost_way.on_packets({ wrapped(table_frame({ { 3, 1004401, 44, 1, 4800, 1180, 0.0f, 7676, 7676 } }), kT0 + 300) });
        GamePacket no_facing = from_client(move_frame(4690.0f, 1180.0f, 9.0f, 1), kT0 + 5000);
        no_facing.payload[28 - 8] = 0;      // neither 0x01 nor 0xFF
        lost_way.on_packets({ no_facing });
        const PacketWorld::View nv = lost_way.view(kT0 + 5000);
        const PacketHealth at_once = judge(lost_way, on, link_at(kT0 + 5000), kT0 + 5000);
        check(nv.me.moving && nv.me.facing == 0 && at_once.why == PacketWhy::Uncertain,
              "a walk with no facing to follow: the screen at once, with no time for a report");
        PacketWorld still;
        healthy_world(still);
        const PacketHealth h = judge(still, on, link_at(kT0 + 600'000), kT0 + 600'000);
        check(h.ok, "standing still ten minutes with nothing sent: healthy -- nothing moved");
    }
}

/// What the simple screen and the log say of each reason.
void test_position_lines() {
    section("Vị trí từ gói tin: what the screens say of it (position_line_vi, packet_why_name)");

    check(position_line_vi(PacketWhy::InUse) == "Vị trí: từ gói tin", "the packets steer: \"Vị trí: từ gói tin\"");
    const PacketWhy others[] = {
        PacketWhy::Off,      PacketWhy::Watch,   PacketWhy::NoNpcap,   PacketWhy::NoCapture,
        PacketWhy::NoTraffic, PacketWhy::ListInUse, PacketWhy::NotMoved, PacketWhy::Foreign,
        PacketWhy::WrongSize, PacketWhy::MapChanged, PacketWhy::NoMonsters, PacketWhy::Uncertain,
        PacketWhy::Settling,
    };
    const std::string screen = "Vị trí: từ màn hình — ";
    std::set<std::string> lines, names;
    bool each = true;
    for (const PacketWhy why : others) {
        const std::string line = position_line_vi(why);
        each = each && line.rfind(screen, 0) == 0 && line.size() > screen.size();
        lines.insert(line);
        names.insert(packet_why_name(why));
    }
    names.insert(packet_why_name(PacketWhy::InUse));
    check(each && lines.size() == std::size(others),
          "every other reason: \"Vị trí: từ màn hình — \" and a reason of its own");
    check(names.size() == std::size(others) + 1 && names.count("?") == 0, "and a name of its own in the log");
    const auto says = [](PacketWhy why, const char* words) {
        return position_line_vi(why).find(words) != std::string::npos;
    };
    check(says(PacketWhy::NoNpcap, "Npcap") && says(PacketWhy::NoTraffic, "không thấy gói tin") &&
              says(PacketWhy::NotMoved, "chưa di chuyển") && says(PacketWhy::MapChanged, "đổi bản đồ") &&
              says(PacketWhy::ListInUse, "Lọc quái") && says(PacketWhy::Foreign, "nhân vật khác") &&
              says(PacketWhy::Off, "tắt") && says(PacketWhy::Watch, "theo dõi"),
          "no Npcap, no game traffic, not moved since the capture began, the map just changed, a list in "
          "use, another character, off and watching: each in its own words");
    check(says(PacketWhy::NoCapture, "không bắt gói tin") && says(PacketWhy::NoCapture, "Stop capture") &&
              says(PacketWhy::NoTraffic, "đang bắt gói tin") && !says(PacketWhy::NoCapture, "không thấy gói tin"),
          "no capture running -- it would not start, or Stop capture was pressed -- and one running that hears "
          "nothing of the game: told apart");
}

// -------------------------------------------------------- compare_views

void test_compare_views() {
    section("Vị trí từ gói tin: what the packets say laid over the screen (compare_views)");

    const ScreenRect seen{ 0, 100, 1280, 500 };      // where plates are looked for
    const Nameplate me = plate(640, 400, "Me");
    const std::vector<Nameplate> plates = { plate(480, 400), plate(760, 400), plate(880, 390) };
    const std::uint64_t now = kT0 + 1000;

    // The world: the screen, 4000 px on and 700 px down.
    PacketWorld::View v;
    v.me.known = true;
    v.me.x = 4640.0f;
    v.me.y = 1100.0f;
    v.me.report_ms = kT0;
    v.mobs = { world_mob(1, 4480.0f, 1100.0f), world_mob(2, 4760.0f, 1100.0f), world_mob(3, 4880.0f, 1090.0f) };
    {
        const ViewComparison c = compare_views(v, now, me, plates, seen);
        check(c.valid && c.offset_x == 4000.0f && c.offset_y == 700.0f,
              "you standing, a second after your last report: compared, at an offset of (4000, 700)");
        check(c.pairs.size() == 3 && c.median_x == 0.0f && c.median_y == 0.0f && c.screen_only == 0 &&
                  c.packets_only == 0,
              "three plates, three monsters: each paired, no residual, nothing left over");
        check(c.scale && std::fabs(*c.scale - 1.0) < 1e-9, "and a scale of 1.00, the plates spanning 400 px");
    }
    {
        // World pixels a quarter bigger than the screen's, about you.
        PacketWorld::View big = v;
        big.mobs = { world_mob(1, 4640.0f - 200.0f, 1100.0f), world_mob(2, 4640.0f + 150.0f, 1100.0f),
                     world_mob(3, 4640.0f + 300.0f, 1090.0f) };
        // (Not called "near": windows.h makes that word mean nothing.)
        const std::vector<Nameplate> close_by = { plate(480, 400), plate(760, 400), plate(880, 390) };
        const ViewComparison c = compare_views(big, now, me, close_by, seen);
        check(c.pairs.size() == 3 && c.scale && std::fabs(*c.scale - 1.25) < 1e-6,
              "a world 1.25 times the screen: a scale of 1.25");
    }
    {
        // Level with you in the world, 15 px below you on screen: the bar's
        // top below your name's.
        const std::vector<Nameplate> lower = { plate(480, 415), plate(760, 415), plate(880, 405) };
        const ViewComparison c = compare_views(v, now, me, lower, seen);
        check(c.pairs.size() == 3 && c.median_y == -15.0f && c.median_x == 0.0f,
              "monsters 15 px lower on screen than in the packets: a residual y of -15 (packet_dy 15 fits)");
    }
    {
        // A plate with no monster under it, and a standing monster on screen
        // with no plate.
        std::vector<Nameplate> more = plates;
        more.push_back(plate(1100, 400));
        PacketWorld::View extra = v;
        extra.mobs.push_back(world_mob(9, 4200.0f, 1100.0f));
        const ViewComparison c = compare_views(extra, now, me, more, seen);
        check(c.pairs.size() == 3 && c.screen_only == 1 && c.packets_only == 1,
              "a plate the packets have nothing under, and a monster the screen shows no plate for: one each");
    }
    {
        // A walking monster is not paired -- its y is a guess between events
        // -- nor is its plate counted as one the packets miss.
        PacketWorld::View walking = v;
        walking.mobs[1].moving = true;
        const ViewComparison c = compare_views(walking, now, me, plates, seen);
        check(c.valid && c.pairs.size() == 2 && c.screen_only == 0,
              "a walking monster: left out of the pairs, and its plate not counted as unseen by the packets");
    }
    {
        PacketWorld::View moving = v, airborne = v, unsure = v, fresh = v, unknown = v;
        moving.me.moving = true;
        airborne.me.airborne = true;
        unsure.me.uncertain = true;
        fresh.me.report_ms = now - 100;
        unknown.me.known = false;
        const bool none = !compare_views(moving, now, me, plates, seen).valid &&
                          !compare_views(airborne, now, me, plates, seen).valid &&
                          !compare_views(unsure, now, me, plates, seen).valid &&
                          !compare_views(fresh, now, me, plates, seen).valid &&
                          !compare_views(unknown, now, me, plates, seen).valid;
        check(none, "you walking, in the air, uncertain, reported 100 ms ago, or not known: nothing compared");
    }
    {
        PacketWorld::View moving = v;
        moving.me.moving = true;
        const std::vector<ViewComparison> looks = { compare_views(v, now, me, plates, seen),
                                                    compare_views(moving, now, me, plates, seen),
                                                    compare_views(v, now + 1000, me, plates, seen) };
        const ViewSummary s = summarize_views(looks);
        check(s.looks == 3 && s.compared == 2 && s.pairs == 6 && s.median_x && *s.median_x == 0.0f &&
                  s.median_y && *s.median_y == 0.0f && s.spread_y == 0.0f && s.scale &&
                  std::fabs(*s.scale - 1.0) < 1e-9,
              "three looks pooled: two compared, six pairs, no residual, a scale of 1.00");
        const ViewSummary empty = summarize_views({});
        check(empty.looks == 0 && empty.pairs == 0 && !empty.median_x && !empty.scale,
              "and none: nothing to say");
    }
}

// ------------------------------------------------------- should_reattach

void test_reattach() {
    section("Vị trí từ gói tin: when the capture is started again (should_reattach)");

    GameLink before;
    before.pid = 1200;
    before.servers = { Endpoint{ 0xCB00710A, 15101 } };
    before.udp_ports = { 50123 };
    const char* why = "x";
    const bool same = should_reattach(before, before, 0, 60'000, &why);
    check(!same && why == nullptr, "the same game, the same sockets, its TCP alive: left as it is");

    GameLink restarted = before;
    restarted.pid = 1300;
    const bool pid = should_reattach(before, restarted, 0, 10'000, &why);
    check(pid && why != nullptr, std::string("the game started again, a new process: started again -- ") +
                                     (why ? why : "no reason"));
    const bool early = should_reattach(before, restarted, 0, 9'999);
    check(!early, "but not within 10 s of the last start");

    GameLink reconnected = before;
    reconnected.servers.push_back(Endpoint{ 0xCB00710B, 15112 });
    GameLink new_port = before;
    new_port.udp_ports.push_back(50200);
    GameLink closed = before;
    closed.udp_ports.clear();
    const bool conn = should_reattach(before, reconnected, 0, 10'000);
    const bool port = should_reattach(before, new_port, 0, 10'000);
    const bool gone = should_reattach(before, closed, 0, 10'000);
    check(conn && port && !gone,
          "a connection or a UDP port the capture was not built with: started again; one gone: no reason to");

    const bool silent = should_reattach(before, before, 5'000, 10'000);
    const bool not_yet = should_reattach(before, before, 4'999, 10'000);
    check(silent && !not_yet, "the game's TCP silent on it for 5 s: started again");

    GameLink no_game;
    GameLink login = before;
    login.servers.clear();
    const bool a = should_reattach(before, no_game, 60'000, 60'000);
    const bool b = should_reattach(before, login, 60'000, 60'000);
    check(!a && !b, "no game, or a game with no connection open: nothing to build a capture from yet");
}

// ------------------------------------------------- the game's UDP by pid

void test_udp_ports_for_pid() {
    section("The game's own UDP ports, by its process id (udp_ports_for_pid)");

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        skip("Winsock would not start here\n");
        return;
    }
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_port = 0;
    inet_pton(AF_INET, "127.0.0.1", &at.sin_addr);
    std::uint16_t port = 0;
    if (s != INVALID_SOCKET &&
        bind(s, reinterpret_cast<const sockaddr*>(&at), static_cast<int>(sizeof(at))) == 0) {
        int len = static_cast<int>(sizeof(at));
        if (getsockname(s, reinterpret_cast<sockaddr*>(&at), &len) == 0) port = ntohs(at.sin_port);
    }
    if (port == 0) {
        skip("no UDP socket could be opened on 127.0.0.1 here\n");
    } else {
        const std::vector<std::uint16_t> mine = udp_ports_for_pid(GetCurrentProcessId());
        const std::vector<std::uint16_t> system = udp_ports_for_pid(4);
        const bool listed = std::find(mine.begin(), mine.end(), port) != mine.end();
        const bool elsewhere = std::find(system.begin(), system.end(), port) != system.end();
        check(listed, "a UDP socket this test opened on 127.0.0.1 is listed under its own process id -- port " +
                          std::to_string(port));
        check(!elsewhere, "and not under pid 4, the System process");
        check(std::is_sorted(mine.begin(), mine.end()) &&
                  std::adjacent_find(mine.begin(), mine.end()) == mine.end(),
              "lowest first, each once");
    }
    check(udp_ports_for_pid(0).empty(), "pid 0, no window's: none");
    if (s != INVALID_SOCKET) closesocket(s);
    WSACleanup();
}

// ------------------------------------------------------------ the eyes

/// The pretend game, seen both ways -- the screen's plates, and the frames
/// the packet world is fed -- with a clock, a capture and every key.
struct Game {
    std::uint64_t t = kT0;
    bool          focused = true;

    // The screen: where your plate is, the monsters' plates, and the
    // detector's count of pictures, one a pass.
    bool                   see_me = true;
    int                    screen_me_x = 640, screen_me_y = 400;
    std::vector<Nameplate> screen_mobs;
    std::uint64_t          pictures = 0;

    // The capture.
    bool          npcap = true;
    bool          capturing = true;
    bool          tcp_alive = true;      ///< the server's frames keep coming: its tick, at least
    std::uint64_t last_tcp_ms = 0;
    int           link_asks = 0;         ///< how often PacketEyes asked about it

    // You, in the world. A step of the hunter's sends the client's reports of
    // its start and its stop, as the client does over UDP.
    bool  walks_report = true;
    float me_x = 4640.0f, me_y = 1100.0f;

    // Keys held as long as asked, the clock going on meanwhile, as
    // InputManager holds them; and then the report of a step's end comes
    // kReportComesMs after its key is up, at the next pass after that.
    // Off, a key takes no time and both reports are there at once.
    bool                    holds_take_time = false;
    std::vector<GamePacket> on_the_way;
    static constexpr std::uint64_t kReportComesMs = 40;

    struct Press {
        std::uint64_t at;
        std::uint32_t vk;
        int           hold;
    };
    std::vector<Press> presses;

    /// Keys put down and left down (Hands::key_down): a held walk. The client
    /// reports its start as the key goes down and its stop as it comes up,
    /// and meanwhile you walk, a pass at a time: 9 px a step of 25 ms in the
    /// world, GoC's 355 px a second on the screen. Its Press is recorded with
    /// kHeld for a hold.
    std::set<std::uint32_t> held;
    static constexpr int kHeld = 1 << 19;

    std::vector<Press> of(std::uint32_t vk, int min_hold = 0) const {
        std::vector<Press> out;
        for (const Press& p : presses)
            if (p.vk == vk && p.hold >= min_hold) out.push_back(p);
        return out;
    }
};

/// A hunter whose eyes are a PacketEyes over a packet world -- or, with
/// `packet_eyes` false, the screen's three handed to it straight, as the
/// hunter had them before.
struct EyesRig {
    PacketWorld          world;
    Game                 g;
    PacketEyes           eyes;
    InputManager         input;
    ExecutionCoordinator coord;
    MobDetector          detector;
    Hunter               hunter{ input, coord, detector };

    explicit EyesRig(const HunterConfig& cfg, bool packet_eyes = true) : eyes(world, screen(), inputs()) {
        if (packet_eyes) {
            hunter.set_eyes(eyes.eyes());
        } else {
            const PacketEyes::Screen s = screen();
            Hunter::Eyes e;
            e.player = s.player;
            e.mobs = s.mobs;
            e.frame = s.frame;
            hunter.set_eyes(e);
        }
        Hunter::Hands hands;
        hands.focused = [this] { return g.focused; };
        hands.key = [this](std::uint32_t vk, int hold) {
            const std::uint64_t down = g.t;
            const std::uint64_t up = down + static_cast<std::uint64_t>(hold);
            g.presses.push_back(Game::Press{ down, vk, hold });
            if (g.holds_take_time) g.t = up;
            if ((vk == VK_LEFT || vk == VK_RIGHT) && hold >= 50) {
                const int facing = vk == VK_RIGHT ? 1 : -1;
                // On screen your plate moves as GoC's sums have it...
                g.screen_me_x += facing * hold * 355 / 1000;
                // ...and in the world the client says where the step began and ended.
                if (g.walks_report) {
                    const float to = g.me_x + static_cast<float>(facing * hold) * 9.0f * 40.0f / 1000.0f;
                    world.on_packets({ from_client(move_frame(g.me_x, g.me_y, 9.0f, facing), down) });
                    if (g.holds_take_time)
                        g.on_the_way.push_back(
                            from_client(move_frame(to, g.me_y, 0.0f, facing), up + Game::kReportComesMs));
                    else
                        world.on_packets({ from_client(move_frame(to, g.me_y, 0.0f, facing), up) });
                    g.me_x = to;
                }
            }
            return true;
        };
        hands.key_down = [this](std::uint32_t vk) {
            g.presses.push_back(Game::Press{ g.t, vk, Game::kHeld });
            g.held.insert(vk);
            if ((vk == VK_LEFT || vk == VK_RIGHT) && g.walks_report)
                world.on_packets({ from_client(move_frame(g.me_x, g.me_y, 9.0f, vk == VK_RIGHT ? 1 : -1), g.t) });
            return true;
        };
        const auto let_go = [this](std::uint32_t vk) {
            if (g.held.erase(vk) == 0) return;
            if ((vk != VK_LEFT && vk != VK_RIGHT) || !g.walks_report) return;
            const int facing = vk == VK_RIGHT ? 1 : -1;
            if (g.holds_take_time)
                g.on_the_way.push_back(
                    from_client(move_frame(g.me_x, g.me_y, 0.0f, facing), g.t + Game::kReportComesMs));
            else
                world.on_packets({ from_client(move_frame(g.me_x, g.me_y, 0.0f, facing), g.t) });
        };
        hands.key_up = let_go;
        hands.held = [this](std::uint32_t vk) { return g.held.count(vk) != 0; };
        hands.release_all = [this, let_go] {
            const std::set<std::uint32_t> keys = g.held;
            for (const std::uint32_t vk : keys) let_go(vk);
        };
        hunter.set_hands(hands);

        Hunter::Clock clock;
        clock.now_ms = [this] { return g.t; };
        clock.sleep_ms = [this](std::uint32_t ms) { g.t += ms; };
        hunter.set_clock(clock);

        hunter.set_config(cfg);
        hunter.start_stepped();
    }
    ~EyesRig() { hunter.stop(); }

    EyesRig(const EyesRig&) = delete;
    EyesRig& operator=(const EyesRig&) = delete;

    PacketEyes::Screen screen() {
        PacketEyes::Screen s;
        s.player = [this](Nameplate& out, bool* fresh) {
            if (fresh) *fresh = g.see_me;
            if (!g.see_me) return false;
            out = plate(g.screen_me_x, g.screen_me_y, "Me");
            out.is_player = true;
            out.is_self = true;
            return true;
        };
        s.mobs = [this] {
            std::vector<Nameplate> out = g.screen_mobs;
            MobDetector::nearest_first(out, g.screen_me_x, g.screen_me_y);
            return out;
        };
        s.frame = [this] { return g.pictures; };
        return s;
    }

    PacketEyes::Inputs inputs() {
        PacketEyes::Inputs in;
        in.link = [this] {
            ++g.link_asks;
            PacketLink l;
            l.npcap = g.npcap;
            l.capturing = g.capturing;
            l.last_tcp_ms = g.last_tcp_ms;
            return l;
        };
        in.now = [this] { return g.t; };
        in.view_size = [](int& w, int& h) {
            w = 1280;
            h = 720;
            return true;
        };
        in.config = [this] { return hunter.config(); };
        in.walk_held_until = [this] { return hunter.walk_held_until_ms(); };
        return in;
    }

    void feed(const GamePacket& p) { world.on_packets({ p }); }

    /// The game as the capture first hears it, healthy: a map entry where you
    /// stand, its table, and a report of yours.
    void enter(const std::vector<Entry>& monsters) {
        const auto px = [](float v) { return static_cast<std::uint16_t>(std::lround(v)); };
        feed(from_server(entry_frame(11, 1, px(g.me_x), px(g.me_y)), g.t));
        feed(wrapped(table_frame(monsters), g.t));
        feed(from_client(move_frame(g.me_x, g.me_y, 0.0f, 1), g.t));
    }

    /// One pass, then 60 ms on: the hunter thread's own rhythm. The server's
    /// frames keep coming while tcp_alive, and the detector takes a picture.
    void pass() {
        if (g.tcp_alive) g.last_tcp_ms = g.t;
        // The client's reports that have come since the last pass.
        for (std::size_t i = 0; i < g.on_the_way.size();) {
            if (g.on_the_way[i].timestamp_ms <= g.t) {
                feed(g.on_the_way[i]);
                g.on_the_way.erase(g.on_the_way.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        ++g.pictures;
        hunter.step();
        // A walk held through the 60 ms to the next pass.
        for (const int vk : { VK_LEFT, VK_RIGHT }) {
            if (!g.held.count(static_cast<std::uint32_t>(vk))) continue;
            const int facing = vk == VK_RIGHT ? 1 : -1;
            g.screen_me_x += facing * 60 * 355 / 1000;
            if (g.walks_report) g.me_x += static_cast<float>(facing) * 9.0f * 40.0f * 60.0f / 1000.0f;
        }
        g.t += 60;
    }
    void run(std::uint64_t ms) {
        for (const std::uint64_t until = g.t + ms; g.t < until;) pass();
    }
    /// Passes until the packets steer, up to `ms`; whether they do.
    bool until_packets(std::uint64_t ms = 3000) {
        for (const std::uint64_t until = g.t + ms; g.t < until;) {
            pass();
            if (eyes.source() == PacketEyes::Source::Packets) return true;
        }
        return false;
    }
};

HunterConfig packets_on() {
    HunterConfig cfg;
    cfg.packet_positions = PacketPositions::On;
    return cfg;
}

/// One standing monster of the table, `dx` to your right and `dy` below you
/// (you stand at 4640, 1100), at `hp` of 7676.
Entry monster(std::uint16_t slot, int dx, int dy = 0, std::uint32_t hp = 7676, std::uint32_t max_hp = 7676) {
    return Entry{ slot, 1004401, 44, 1, static_cast<std::uint16_t>(4640 + dx), static_cast<std::uint16_t>(1100 + dy),
                  0.0f, hp, max_hp };
}

/// No capture, or the packets off: the screen's three passed through, and
/// the hunter presses exactly what it did on them before.
void test_eyes_pass_through() {
    section("Vị trí từ gói tin: no capture -- the screen, exactly as before");

    {
        EyesRig a(packets_on(), false);
        EyesRig b(packets_on());
        b.g.capturing = false;
        b.enter({ monster(5, 100) });      // the packets have a monster: no capture says they do not count
        for (EyesRig* r : { &a, &b })
            r->g.screen_mobs = { plate(700, 400, "Moc", 0.6f), plate(980, 400, "Da"), plate(660, 330, "Cao") };
        a.run(8000);
        bool screen = true;
        for (const std::uint64_t until = b.g.t + 8000; b.g.t < until;) {
            b.pass();
            screen = screen && b.eyes.source() == PacketEyes::Source::Screen;
        }
        bool same = !a.g.presses.empty() && a.g.presses.size() == b.g.presses.size();
        for (std::size_t i = 0; same && i < a.g.presses.size(); ++i)
            same = a.g.presses[i].vk == b.g.presses[i].vk && a.g.presses[i].hold == b.g.presses[i].hold &&
                   a.g.presses[i].at == b.g.presses[i].at;
        check(same, "no capture: every key, held as long, at the same moment as the hunter on the screen alone -- " +
                        std::to_string(b.g.presses.size()) + " keys");
        check(screen && b.eyes.view() == 0 && b.eyes.why() == PacketWhy::NoCapture,
              "every pass on the screen, the view never moved, and it says why: no capture");
    }
    {
        HunterConfig off;
        off.packet_positions = PacketPositions::Off;
        EyesRig r(off);
        r.enter({ monster(5, 100) });
        r.g.screen_mobs = { plate(700, 400, "Moc") };
        r.run(4000);
        check(r.eyes.source() == PacketEyes::Source::Screen && r.eyes.view() == 0 &&
                  r.eyes.why() == PacketWhy::Off && r.g.link_asks == 0 && r.eyes.stats().healthy_ms == 0,
              "Off: the screen, nothing asked of the capture, and nothing judged");
    }
    {
        HunterConfig watch;
        watch.packet_positions = PacketPositions::Watch;
        EyesRig r(watch);
        r.enter({ monster(5, 100) });
        r.g.screen_mobs = { plate(700, 400, "Moc") };
        r.run(4000);
        const PacketEyes::Stats s = r.eyes.stats();
        check(r.eyes.source() == PacketEyes::Source::Screen && r.eyes.view() == 0 &&
                  r.eyes.why() == PacketWhy::Watch && s.packets_ms == 0 && s.healthy_ms >= 3800,
              "Watch: the screen fights, and the time the packets could have steered is counted -- " +
                  std::to_string(s.healthy_ms) + " ms of 4000");
        r.g.npcap = false;
        r.pass();
        check(r.eyes.why() == PacketWhy::NoNpcap, "and with no Npcap it says that, not \"watching\"");
    }
}

/// After kHealthyForMs of health the packets steer: the hunter goes for a
/// monster the screen does not show, and attacks it once in reach.
void test_eyes_take_over() {
    section("Vị trí từ gói tin: the packets take over after 2 s of health");

    EyesRig r(packets_on());
    r.enter({ monster(5, 400) });        // 400 px to your right; the screen shows nothing
    r.run(1500);
    const bool waited = r.eyes.source() == PacketEyes::Source::Screen && r.eyes.why() == PacketWhy::Settling &&
                        r.g.presses.empty();
    check(waited, "healthy for 1.5 s: still the screen, which shows no monster, and nothing pressed");
    r.run(600);
    const bool took = r.eyes.source() == PacketEyes::Source::Packets && r.eyes.view() == 1 &&
                      r.eyes.why() == PacketWhy::InUse;
    check(took, "past 2 s: the packets steer, the view moved once, and it says so");

    std::uint64_t first_swing = 0;
    float swung_from = 0.0f;
    for (const std::uint64_t until = r.g.t + 3000; r.g.t < until && first_swing == 0;) {
        r.pass();
        const std::vector<Game::Press> swings = r.g.of('Z');
        if (!swings.empty()) {
            first_swing = swings.front().at;
            swung_from = r.g.me_x;
        }
    }
    const std::vector<Game::Press> walks = r.g.of(VK_RIGHT, Game::kHeld);
    check(walks.size() == 1 && first_swing != 0 && 4640.0f + 400.0f - swung_from <= 150.0f &&
              4640.0f + 400.0f - swung_from >= 120.0f,
          "it walks to the packet monster the screen never showed -- the key held, " + std::to_string(walks.size()) +
              " walk(s), followed by the packets as it goes -- and attacks it once within reach, " +
              std::to_string(std::lround(5040.0f - swung_from)) + " px away");
    const Hunter::Status st = r.hunter.status();
    check(st.state == Hunter::State::Attacking && st.has_target && st.target.x() == 5040 && st.target.y() == 1100,
          "its target is the packet monster, where the packets put it");
}

/// Whatever breaks the packets' health puts the next pass on the screen:
/// one view change, and the target let go of as no kill.
void test_eyes_fall_back() {
    section("Vị trí từ gói tin: back to the screen at once, with one view change and no loot owed");

    const char* const kWhat[] = { "the game's TCP silent for 3 s", "the capture stopped",
                                  "another character's report", "a walk of yours no report follows" };
    const PacketWhy kWhy[] = { PacketWhy::NoTraffic, PacketWhy::NoCapture, PacketWhy::Foreign,
                               PacketWhy::Uncertain };
    for (int c = 0; c < 4; ++c) {
        HunterConfig cfg = packets_on();
        cfg.loot = true;
        cfg.loot_key = 'A';
        cfg.loot_interval_ms = 600'000;     // only what a kill owes
        EyesRig r(cfg);
        r.g.walks_report = false;
        r.g.screen_mobs = { plate(700, 400, "TrungXanh") };    // the screen sees one too, in its own pixels
        r.enter({ monster(5, 100) });
        const bool steering = r.until_packets();
        r.run(300);
        const bool fighting = steering && r.hunter.status().state == Hunter::State::Attacking;
        const std::uint32_t view = r.eyes.view();
        const std::size_t looted = r.g.of('A').size();
        const std::uint64_t finished = r.hunter.status().targets_finished;

        // When the rule is first broken: at once for a stopped capture and
        // another character's report; once the TCP heard last is more than
        // 3 s old; once the walk has gone unreported kOwnStepReckonMs -- not
        // the hunter's: it is fighting, and holds none -- and 300 ms more for
        // its report. A crawl toward the monster, so it stays the target in
        // reach throughout, held across the switch.
        std::uint64_t broken_at = r.g.t;
        if (c == 0) {
            r.g.tcp_alive = false;
            broken_at = r.g.last_tcp_ms + PacketHealth::kTcpQuietMs + 1;
        }
        if (c == 1) r.g.capturing = false;
        if (c == 2) r.feed(from_client(move_frame(100.0f, 100.0f, 0.0f, 1, kId + 1), r.g.t));
        if (c == 3) {
            r.feed(from_client(move_frame(r.g.me_x, r.g.me_y, 0.5f, 1), r.g.t));
            broken_at = r.g.t + PacketWorld::kOwnStepReckonMs + PacketHealth::kReportLateMs + 1;
        }
        // The pass the screen came back at: it must be the first one at or
        // after that moment, passes being 60 ms apart -- no waiting on the
        // way back.
        std::uint64_t switched_at = 0;
        bool fought_on = true;      // the target held, in reach, up to the switch
        for (const std::uint64_t until = r.g.t + 4000; r.g.t < until && switched_at == 0;) {
            const std::uint64_t at = r.g.t;
            r.pass();
            if (r.eyes.source() == PacketEyes::Source::Screen) switched_at = at;
            else fought_on = fought_on && r.hunter.status().state == Hunter::State::Attacking;
        }
        if (c == 3)
            check(fought_on, std::string(kWhat[c]) + ": the packets' monster fought, in reach, up to the switch");
        const bool first_pass = switched_at >= broken_at && switched_at < broken_at + 60;
        const long long late =
            switched_at == 0 ? -1 : static_cast<long long>(switched_at) - static_cast<long long>(broken_at);
        const bool once = switched_at != 0 && r.eyes.view() == view + 1 && r.eyes.why() == kWhy[c];
        r.run(1200);
        const Hunter::Status st = r.hunter.status();
        const std::size_t after = r.g.of('A').size() - looted;
        check(fighting && first_pass,
              std::string(kWhat[c]) + ": the screen from the first pass after the rule is broken, " +
                  std::to_string(late) + " ms after it");
        check(fighting && once && r.eyes.view() == view + 1 && after == 0 && st.targets_finished == finished &&
                  st.state == Hunter::State::Attacking && st.target.x() == 700,
              std::string(kWhat[c]) + ": one view change, no loot pressed (" + std::to_string(after) +
                  "), nothing counted finished, and the screen's monster fought");
    }
}

/// A map entry clears the monsters: the screen until the new map's table, or
/// until 6 s after the entry, and the packets 2 s after that.
void test_eyes_map_change() {
    section("Vị trí từ gói tin: a map entry -- the screen until it settles");

    for (const bool table : { true, false }) {
        EyesRig r(packets_on());
        r.g.walks_report = false;
        r.enter({ monster(5, 100) });
        const bool steering = r.until_packets();
        const std::uint32_t view = r.eyes.view();
        const std::uint64_t entered = r.g.t;
        r.feed(from_server(entry_frame(11, 2, 4640, 1100), entered));
        r.pass();
        const bool back = r.eyes.source() == PacketEyes::Source::Screen && r.eyes.view() == view + 1 &&
                          r.eyes.why() == PacketWhy::MapChanged;
        std::uint64_t settled = entered + 6000;
        if (table) {
            r.run(900);
            settled = r.g.t;
            r.feed(wrapped(table_frame({ monster(1, 200) }), settled));
        }
        bool early = false;
        while (r.g.t < settled + 2000 - 60) {
            r.pass();
            if (r.eyes.source() == PacketEyes::Source::Packets) early = true;
        }
        const bool took = r.until_packets(200) && r.eyes.view() == view + 2;
        check(steering && back && !early && took,
              table ? "a map entry: the screen at once, until its table and 2 s more, then the packets"
                    : "with no table: the screen until 6 s after the entry and 2 s more, then the packets");
    }
}

/// A kill: the target shown at 0 a moment, counted finished once, and picked
/// up after with three presses.
void test_eyes_kill() {
    section("Vị trí từ gói tin: a kill is counted once, and picked up after");

    HunterConfig cfg = packets_on();
    cfg.loot = true;
    cfg.loot_key = 'A';
    cfg.loot_interval_ms = 600'000;
    EyesRig r(cfg);
    r.g.walks_report = false;
    r.enter({ monster(5, 100) });
    r.until_packets();
    r.run(800);
    const bool swung = !r.g.of('Z').empty();
    const std::uint64_t finished = r.hunter.status().targets_finished;
    const std::size_t looted = r.g.of('A').size();
    // The blow that leaves its HP below 0.
    r.feed(from_server(event_frame(9, 5, 1, 7.0f, 4740, 1100, -5000, 12676), r.g.t));
    r.run(1200);
    const Hunter::Status st = r.hunter.status();
    const std::size_t picked = r.g.of('A').size() - looted;
    check(swung && st.targets_finished == finished + 1 && picked == 3,
          "a hit that leaves HP below 0: one more finished, and three pick-up presses -- " +
              std::to_string(picked));
    r.run(600);
    check(r.eyes.mobs().empty() && r.hunter.status().targets_finished == finished + 1,
          "1.5 s on, the dead monster is gone from the list, and counted once");
}

/// A living monster is never shown as finished, however hurt.
void test_eyes_wounded() {
    section("Vị trí từ gói tin: a monster at 3% is still attacked");

    EyesRig r(packets_on());
    r.g.walks_report = false;
    r.enter({ monster(6, 100, 0, 30, 1000) });
    r.until_packets();
    const std::size_t before = r.g.of('Z').size();
    r.run(1500);
    const std::vector<Nameplate> mobs = r.eyes.mobs();
    const float hp = mobs.empty() ? -1.0f : mobs.front().hp;
    const HunterConfig cfg = r.hunter.config();
    check(hp > cfg.dead_below && hp < 0.1f, "30 of 1000 HP is offered above dead_below: " + std::to_string(hp));
    check(r.g.of('Z').size() > before && r.hunter.status().targets_finished == 0 &&
              r.hunter.status().state == Hunter::State::Attacking,
          "and attacked, never let go of as finished");

    // Every blow still shows on its bar, however low. Held up at a floor, the
    // last few percent would never move it, and with a Kiểu đánh a monster
    // nearly dead would be given up on as one the attacks do not hurt.
    std::vector<float> bars;
    for (const std::int32_t left : { 20, 10, 1 }) {
        r.feed(from_server(event_frame(9, 6, 1, 0.0f, 4740, 1100, left, 10), r.g.t));
        r.pass();
        const std::vector<Nameplate> hit = r.eyes.mobs();
        bars.push_back(hit.empty() ? -1.0f : hit.front().hp);
    }
    std::string shown;
    for (const float b : bars) shown += " " + std::to_string(b);
    check(bars.size() == 3 && hp > bars[0] && bars[0] > bars[1] && bars[1] > bars[2] && bars[2] > cfg.dead_below,
          "and every blow shows, down to 1 HP of 1000, still above dead_below:" + shown);
}

/// A monster list keeps the screen in charge: the packets carry no names.
void test_eyes_lists() {
    section("Vị trí từ gói tin: a monster list in use keeps the screen");

    HunterConfig cfg = packets_on();
    cfg.skip_names = { "NgaQuy" };
    EyesRig r(cfg);
    r.enter({ monster(5, 100) });
    r.g.screen_mobs = { plate(700, 400, "TrungXanh") };
    r.run(4000);
    check(r.eyes.source() == PacketEyes::Source::Screen && r.eyes.view() == 0 &&
              r.eyes.why() == PacketWhy::ListInUse && r.hunter.status().target.x() == 700,
          "Không đánh in use: the screen, whose plates have names, and it says why");
}

/// Which monsters are offered, and in what order.
void test_eyes_box() {
    section("Vị trí từ gói tin: monsters near you, nearest first");

    {
        EyesRig r(packets_on());
        r.g.walks_report = false;
        r.enter({ monster(1, 800), monster(2, -700), monster(3, 200, -100), monster(4, 330),
                  monster(5, 0, -440) });
        r.until_packets();
        r.eyes.frame();
        const std::vector<Nameplate> mobs = r.eyes.mobs();
        Nameplate me;
        bool fresh = false;
        const bool seen = r.eyes.player(me, &fresh);
        check(seen && fresh && me.x() == 4640 && me.y() == 1100 && me.is_self,
              "you, where the packets put you, seen just now");
        check(mobs.size() == 3, "0.6 of a 1280 x 720 game around you: 800 px to the side and 440 px up are left "
                                "out, 700 px to the side is not -- " + std::to_string(mobs.size()));
        check(mobs.size() == 3 && mobs[0].x() == 4970 && mobs[1].x() == 4840 && mobs[1].y() == 1000 &&
                  mobs[2].x() == 3940,
              "nearest first, a height counting three times: 330 px level before 200 px and 100 px up, and "
              "700 px last");
        check(mobs.size() == 3 && mobs[0].hp == 1.0f && mobs[1].hp == 1.0f && mobs[2].hp == 1.0f,
              "each at full health reads exactly 1: anything less the hunter takes for wounded");
    }
    {
        // Laid over the screen by hunt.ini's two numbers.
        HunterConfig cfg = packets_on();
        cfg.packet_scale = 2.0f;
        cfg.packet_dy = 15;
        EyesRig r(cfg);
        r.g.walks_report = false;
        r.enter({ monster(1, 300) });
        r.until_packets();
        Nameplate me;
        r.eyes.player(me, nullptr);
        const std::vector<Nameplate> mobs = r.eyes.mobs();
        check(me.x() == 2320 && me.y() == 550 && mobs.size() == 1 && mobs[0].x() == 2470 && mobs[0].y() == 565,
              "packet_scale 2 halves every distance, and packet_dy 15 puts each monster 15 px lower");
    }
    {
        EyesRig r(packets_on());
        r.g.walks_report = false;
        r.enter({ monster(1, 100) });
        r.until_packets();
        const std::uint32_t view = r.eyes.view();
        r.eyes.restart();
        check(r.eyes.source() == PacketEyes::Source::Screen && r.eyes.view() == view + 1,
              "restart(), as hunting starts: back to the screen, in another view");
    }
}

/// A step back is judged on a picture taken wholly after it: from the
/// packets, two of the packet world's updates on.
void test_eyes_backstep() {
    section("Vị trí từ gói tin: a step back is judged two updates on");

    HunterConfig cfg = packets_on();
    cfg.keep_distance = true;               // 50 px with no style
    EyesRig r(cfg);
    r.g.walks_report = false;
    r.enter({ monster(7, 20) });            // 20 px to your right: too close
    r.until_packets();
    r.run(600);
    const std::size_t first = r.g.of(VK_LEFT, 50).size();
    // The client's report of the step: 10 px back. Still too close -- but the
    // step is judged only two updates on.
    r.feed(from_client(move_frame(4630.0f, 1100.0f, 0.0f, -1), r.g.t));
    r.run(600);
    const std::size_t one = r.g.of(VK_LEFT, 50).size();
    r.feed(from_client(move_frame(4625.0f, 1100.0f, 0.0f, -1), r.g.t));
    r.run(600);
    const std::size_t two = r.g.of(VK_LEFT, 50).size();
    check(first == 1 && one == 1 && two == 2,
          "one step back; after one update, though still too close, no other (" + std::to_string(one) +
              "); after the second, the first judged and the next one taken (" + std::to_string(two) + ")");
}

/// The longest step back, on the packets, with keys that take the time they
/// are held: the hunter asks for its picture the moment the key is up, 860 ms
/// after the walk's report -- past the 600 ms a walk of yours was followed
/// before the held walk, inside the 3 s it is now -- and the report of the
/// stop comes a moment later. Back on the screen there and then, the hunter
/// would forget the step it had just taken, and the packets would be lost for
/// 2 s at every long step back. The turn back to the monster after it, a tap
/// of Hunter::kTurnTapMs, walks you 21.6 px in again here, as a key held that
/// long walks you: the step is held as much longer than GoC's 800 ms, so the
/// turn leaves you as far out as GoC's step meant, and there is no second.
void test_eyes_long_backstep() {
    section("Vị trí từ gói tin: the longest step back stays on the packets");

    HunterConfig cfg = packets_on();
    cfg.keep_distance = true;
    cfg.reach = 400;
    cfg.min_distance = 300;                 // from 20 px: GoC's longest step, 800 ms, and the turn's 60
    EyesRig r(cfg);
    r.g.holds_take_time = true;
    r.enter({ monster(9, 2000) });          // far off, outside the box: nothing to fight yet
    const bool steering = r.until_packets();
    const std::uint32_t view = r.eyes.view();
    const std::uint64_t switches = r.eyes.stats().switches;
    // One comes up 20 px to your right: too close.
    r.feed(from_server(event_frame(0, 7, 1, 0.0f, 4660, 1100, 7676), r.g.t));
    r.run(2000);
    const std::vector<Game::Press> steps = r.g.of(VK_LEFT, 50);
    const Hunter::Status st = r.hunter.status();
    const bool stayed = r.eyes.source() == PacketEyes::Source::Packets && r.eyes.view() == view &&
                        r.eyes.stats().switches == switches;
    const std::vector<Game::Press> turns = r.g.of(VK_RIGHT, 50);
    check(steering && steps.size() == 1 && steps.front().hold == 800 + Hunter::kTurnTapMs && st.backsteps == 1 &&
              turns.size() == 1 && turns.front().hold == Hunter::kTurnTapMs,
          "one step back from the monster 20 px away, held 860 ms, and one turn back to it -- no second step "
          "after the turn -- " + std::to_string(steps.size()) + " step(s), " + std::to_string(turns.size()) +
              " turn(s)");
    check(stayed && st.state == Hunter::State::Attacking && st.dx == 308,
          "the packets the source throughout, the view never moved, and the monster attacked from where the "
          "reports put you: 330 px from it after the step, 308 after the turn -- " + std::to_string(st.dx) + " px");
}

/// A walk held longer than Hunter::kWalkRenewMs, on the packets: the client
/// says nothing between its start and its stop, so the hunter lets go of it
/// and takes it up again, and the stop and the start that come of that give
/// the packets a fresh place before they would stop following it. Steered by
/// the packets from first to last, and never uncertain.
void test_eyes_held_walk() {
    section("Vị trí từ gói tin: a walk held 3 s stays on the packets");

    EyesRig r(packets_on());
    r.enter({ monster(9, 700) });           // 700 px to your right, in the box
    // Counted from before the packets take over: the walk begins at the very
    // pass they do.
    const std::uint64_t reports = r.world.stats().own(OwnReport::Move);
    const bool steering = r.until_packets();
    const std::uint32_t view = r.eyes.view();
    const std::uint64_t switches = r.eyes.stats().switches;
    // It walks off to the right at 200 px a second: 160 px a second closer
    // as you walk after it, and 3.4 s to come within reach.
    r.feed(from_server(event_frame(1, 9, 1, 5.0f, 4640 + 700, 1100, 7676), r.g.t));
    // Up to the first swing: the monster walks on after it, and is walked
    // after again.
    bool packets = true, sure = true;
    std::uint64_t attacked_at = 0;
    for (const std::uint64_t until = r.g.t + 4500; r.g.t < until && attacked_at == 0;) {
        r.pass();
        packets = packets && r.eyes.source() == PacketEyes::Source::Packets;
        Nameplate me;
        bool fresh = false;
        r.eyes.player(me, &fresh);
        sure = sure && fresh;
        if (attacked_at == 0 && !r.g.of('Z').empty()) attacked_at = r.g.t;
    }
    const std::vector<Game::Press> walks = r.g.of(VK_RIGHT, Game::kHeld);
    const std::uint64_t moves = r.world.stats().own(OwnReport::Move) - reports;
    check(steering && walks.size() == 2 && walks[1].at - walks[0].at >= Hunter::kWalkRenewMs &&
              walks[1].at - walks[0].at < Hunter::kWalkRenewMs + 120 && moves == 4,
          "one walk, let go of and taken up again 2.5 s in: its start, stop, start and stop reported -- " +
              std::to_string(walks.size()) + " walk(s), " + std::to_string(moves) + " report(s)");
    check(packets && sure && r.eyes.view() == view && r.eyes.stats().switches == switches && attacked_at != 0 &&
              r.hunter.status().state == Hunter::State::Attacking,
          "the packets steering throughout, your place never uncertain, and the monster attacked once reached");
}

/// Where the packets put you in their world (world_fix, PacketEyes::where),
/// for the hunting spot: by the capture's and your own rules alone, whatever
/// steers -- a list in use, the monsters not settled, a frame of a strange
/// size change nothing of where you are.
void test_eyes_where() {
    section("Chỗ đánh quái: where the packets put you in their world (world_fix, where)");

    const std::uint64_t now = kT0 + 10'000;
    const auto fix_of = [](const PacketWorld& w, std::uint64_t at,
                           std::uint64_t follow = PacketWorld::kOwnReckonMs) {
        return world_fix(w.view(at, follow), w.stats(), link_at(at), at);
    };
    {
        PacketWorld w;
        healthy_world(w);
        PacketLink stopped = link_at(now), no_npcap = link_at(now), quiet = link_at(now);
        stopped.capturing = false;
        no_npcap.npcap = false;
        quiet.last_tcp_ms = now - 3001;
        bool none = true;
        for (const PacketLink& l : { stopped, no_npcap, quiet })
            none = none && world_fix(w.view(now), w.stats(), l, now).state == WorldFix::State::NoCapture;
        check(none, "no capture, no Npcap, or the game's TCP silent past 3 s: NoCapture");
        const WorldFix f = fix_of(w, now);
        check(f.state == WorldFix::State::Known && f.x == 4690.0f && f.y == 1180.0f && f.rx == 4690.0f &&
                  f.ry == 1180.0f && f.report_ms == kT0 + 400 && !f.moving && !f.airborne && !f.uncertain &&
                  f.settled(),
              "a map entry, its table and a report of yours: Known where the report put you, settled on the ground");
        check(f.map_known && f.map == 11 && f.area == 1 && f.epoch == 1 && f.entry_x == 4690.0f &&
                  f.entry_y == 1180.0f,
              "on 11/1, after the first map entry ever, whose spawn was 4690,1180");
    }
    {
        // Which way you face, for the hunter's turn: the last 0x0027 that
        // said so, and when it came. A cast after it moves neither; a map
        // entry makes it not known.
        PacketWorld w;
        healthy_world(w);       // its report of yours faces right, at kT0 + 400
        const WorldFix right = fix_of(w, now);
        w.on_packets({ from_client(move_frame(4690.0f, 1180.0f, 0.0f, -1), kT0 + 12'000) });
        const WorldFix left = fix_of(w, kT0 + 12'100);
        w.on_packets({ from_client(cast_frame(4690, 1180), kT0 + 12'500) });
        const WorldFix cast = fix_of(w, kT0 + 12'600);
        check(right.state == WorldFix::State::Known && right.facing == 1 && right.facing_ms == kT0 + 400 &&
                  left.facing == -1 && left.facing_ms == kT0 + 12'000,
              "the fix says which way you face, and when a report said so: right as of the first report, then left "
              "as of a 0x0027 facing left");
        check(cast.state == WorldFix::State::Known && cast.facing == -1 && cast.facing_ms == kT0 + 12'000 &&
                  cast.report_ms == kT0 + 12'500,
              "a cast after it: still left, as of the 0x0027, though the cast is the last report");
        w.on_packets({ from_server(entry_frame(11, 2, 900, 600), kT0 + 13'000) });
        const WorldFix entered = fix_of(w, kT0 + 13'100);
        check(entered.state == WorldFix::State::Known && entered.facing == 0 && entered.facing_ms == 0,
              "a map entry: Known at its spawn, the way you face not known, nor when");
    }
    {
        PacketWorld w;
        w.on_packets({ from_server(entry_frame(11, 1, 4690, 1180), kT0) });
        const WorldFix f = fix_of(w, now);
        check(f.state == WorldFix::State::FirstReport && f.map_known && f.epoch == 1 && f.x == 4690.0f,
              "a map entry and no report of yours: FirstReport, at the spawn, the map known and the entry counted");
    }
    {
        PacketWorld w;
        healthy_world(w);
        w.on_packets({ from_server(entry_frame(11, 2, 900, 600), kT0 + 20'000) });
        const WorldFix moved = fix_of(w, kT0 + 20'100);
        check(moved.state == WorldFix::State::Known && moved.map == 11 && moved.area == 2 && moved.epoch == 2 &&
                  moved.x == 900.0f && moved.y == 600.0f && moved.entry_x == 900.0f && moved.entry_y == 600.0f,
              "an entry to 11/2 at once: Known at its spawn, the second entry ever -- where you are does not wait for "
              "its monsters");
        w.reset();
        w.on_packets({ from_client(move_frame(950.0f, 600.0f, 0.0f, 1), kT0 + 21'000) });
        const WorldFix restarted = fix_of(w, kT0 + 21'100);
        check(restarted.state == WorldFix::State::Known && !restarted.map_known && restarted.epoch == 2 &&
                  restarted.x == 950.0f && restarted.entry_x == 900.0f && restarted.entry_y == 600.0f,
              "the capture started again (reset), and a report: Known, the map not known again, the epoch still 2 "
              "and the last entry's spawn still 900,600");
    }
    {
        PacketWorld w;
        healthy_world(w);
        w.on_packets({ from_client(move_frame(100.0f, 100.0f, 0.0f, 1, kId + 1), kT0 + 2000) });
        check(fix_of(w, kT0 + 31'999).state == WorldFix::State::Lost &&
                  fix_of(w, kT0 + 32'000).state == WorldFix::State::Known,
              "another character's report: Lost for 30 s, then Known again");
    }
    {
        PacketWorld w;
        healthy_world(w);
        w.on_packets({ from_client(move_frame(4690.0f, 1180.0f, 9.0f, -1), kT0 + 5000) });
        const std::uint64_t step = PacketWorld::kOwnStepReckonMs;
        const WorldFix walking = fix_of(w, kT0 + 5300, step);
        const WorldFix late = fix_of(w, kT0 + 5000 + step + PacketHealth::kReportLateMs, step);
        const WorldFix lost = fix_of(w, kT0 + 5001 + step + PacketHealth::kReportLateMs, step);
        check(walking.state == WorldFix::State::Known && walking.moving && !walking.settled() &&
                  std::abs(walking.x - 4582.0f) < 0.1f && walking.rx == 4690.0f,
              "walking: Known, worked out 108 px on in 0.3 s while rx stays where the report put you -- not settled");
        check(late.state == WorldFix::State::Known && late.uncertain && lost.state == WorldFix::State::Lost,
              "past the time a walk is followed, uncertain; its report 300 ms late on top of that: Lost");
    }
    {
        PacketWorld w;
        healthy_world(w);
        GamePacket cut = from_server(event_frame(0, 3, 1, 0.0f, 4800, 1180, 7676), kT0 + 2000);
        cut.payload.pop_back();
        w.on_packets({ cut });
        HunterConfig on;
        on.packet_positions = PacketPositions::On;
        check(packet_health(w.view(now), w.stats(), link_at(now), on, now).why == PacketWhy::WrongSize &&
                  fix_of(w, now).state == WorldFix::State::Known && w.stats().last_own_wrong_size_ms == 0,
              "a monster frame of a strange size: the packets do not steer for 60 s, and you are still Known");
    }
    {
        // A report of yours of a strange size -- a game update that moved
        // your walk's fields: its walks go unread, and the fix would stand
        // where the last one it read began.
        PacketWorld w;
        healthy_world(w);
        GamePacket cut = from_client(move_frame(4700.0f, 1180.0f, 9.0f, 1), kT0 + 2000);
        cut.payload.pop_back();
        w.on_packets({ cut });
        const std::uint64_t over = kT0 + 2000 + PacketHealth::kWrongSizeMs;
        check(w.stats().last_own_wrong_size_ms == kT0 + 2000 && fix_of(w, now).state == WorldFix::State::Lost &&
                  fix_of(w, over - 1).state == WorldFix::State::Lost &&
                  fix_of(w, over).state == WorldFix::State::Known,
              "a report of yours of a strange size: Lost for 60 s, then Known again");
    }
    {
        HunterConfig off;
        off.packet_positions = PacketPositions::Off;
        EyesRig r(off);
        r.enter({ monster(5, 100) });
        r.run(300);
        WorldFix f;
        f.state = WorldFix::State::Known;
        check(!r.eyes.where(f) && f.state == WorldFix::State::NoCapture,
              "packet_positions off: where() says there is none to be had -- NoCapture");
    }
    {
        HunterConfig watch;
        watch.packet_positions = PacketPositions::Watch;
        watch.skip_names = { "NgaQuy" };
        EyesRig r(watch);
        r.enter({ monster(5, 100) });
        r.run(300);
        WorldFix f;
        const bool got = r.eyes.where(f);
        check(got && f.state == WorldFix::State::Known && f.x == 4640.0f && f.y == 1100.0f && f.map == 11 &&
                  f.area == 1 && f.epoch == 1 && r.eyes.source() == PacketEyes::Source::Screen,
              "watched only, with a monster list in use: the screen fights, and where() has you Known at "
              "4640,1100 on 11/1");
        WorldFix via;
        const Hunter::Eyes e = r.eyes.eyes();
        check(e.where && e.where(via) && via.state == WorldFix::State::Known && via.x == f.x,
              "and the hunter's eyes ask the same where()");
        r.g.capturing = false;
        r.pass();
        check(r.eyes.where(f) && f.state == WorldFix::State::NoCapture && f.epoch == 1,
              "the capture stopped: NoCapture, the map entries still counted");
    }
}

/// The hunting spot on the packets: taken where you stand, walked back to by
/// where the reports put you -- the walk renewed every 2.5 s whether the
/// packets steer or are only watched, or the packets would lose it past 3 s
/// -- and let go of in the very pass a map entry comes.
void test_eyes_spot_walk() {
    section("Chỗ đánh quái: the walk back on the packets, steering or only watched");

    for (const PacketPositions how : { PacketPositions::Watch, PacketPositions::On }) {
        const std::string mode = how == PacketPositions::Watch ? "watched: " : "steering: ";
        HunterConfig cfg;
        cfg.packet_positions = how;
        cfg.spot = true;
        EyesRig r(cfg);
        r.enter({});
        r.pass();
        const Hunter::Spot spot = r.hunter.spot();
        check(spot.set && spot.x == 4640.0f && spot.y == 1100.0f && spot.half_w == 300 && spot.half_h == 100 &&
                  spot.epoch == 1 && !spot.suspended,
              mode + "the spot taken where you stand at the first look: 4640,1100, +-300 / +-100, map entry 1");

        // Pushed 1500 px to the right -- the report says so -- with nothing
        // to fight: 4.2 s of walking back, past the 3 s the packets follow a
        // walk with no report.
        r.g.me_x = 6140.0f;
        r.g.screen_me_x += 1500;
        r.feed(from_client(move_frame(6140.0f, 1100.0f, 0.0f, 1), r.g.t));
        bool lost = false;
        for (const std::uint64_t until = r.g.t + 7000; r.g.t < until;) {
            r.pass();
            WorldFix f;
            r.eyes.where(f);
            if (f.state != WorldFix::State::Known) lost = true;
        }
        const std::vector<Game::Press> walks = r.g.of(VK_LEFT, Game::kHeld);
        const Hunter::Status st = r.hunter.status();
        check(walks.size() == 2 && walks[1].at - walks[0].at >= Hunter::kWalkRenewMs && !lost &&
                  r.g.of(VK_RIGHT).empty() && r.g.held.empty(),
              mode + "walked back left, renewed once 2.5 s in, the packets never losing you -- " +
                  std::to_string(walks.size()) + " walk(s)");
        check(std::abs(r.g.me_x - 4640.0f) <= 48.0f && st.returns == 1 && st.return_give_ups == 0 &&
                  st.state == Hunter::State::NoTargets && st.spot_why == Hunter::SpotWhy::None,
              mode + "let go of within 48 px of the spot -- " +
                  std::to_string(std::lround(r.g.me_x - 4640.0f)) + " px -- one walk back, none given up");

        // Pushed again, and a map entry comes while it walks back: let go of
        // in that pass, and the spot stands aside.
        r.g.me_x = 5100.0f;
        r.g.screen_me_x += 460;
        r.feed(from_client(move_frame(5100.0f, 1100.0f, 0.0f, 1), r.g.t));
        for (int i = 0; i < 20 && r.g.held.empty(); ++i) r.pass();
        const bool walking = r.g.held.count(VK_LEFT) == 1;
        const std::size_t before = r.g.of(VK_LEFT, Game::kHeld).size();
        r.feed(from_server(entry_frame(11, 2, 900, 600), r.g.t));
        r.g.me_x = 900.0f;
        r.g.me_y = 600.0f;
        r.pass();
        const Hunter::Status at_entry = r.hunter.status();
        const bool let_go = r.g.held.empty();
        r.run(3000);
        check(walking && let_go && at_entry.spot_suspended && at_entry.spot_why == Hunter::SpotWhy::Suspended &&
                  r.g.of(VK_LEFT, Game::kHeld).size() == before && r.g.of(VK_RIGHT, Game::kHeld).empty(),
              mode + "a map entry mid-walk: the walk let go of in that same pass, the spot suspended, and no walk "
                     "after it");
    }
}

} // namespace

void test_packet_eyes(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_packet_health();
    test_position_lines();
    test_compare_views();
    test_reattach();
    test_udp_ports_for_pid();
    test_eyes_pass_through();
    test_eyes_take_over();
    test_eyes_fall_back();
    test_eyes_map_change();
    test_eyes_kill();
    test_eyes_wounded();
    test_eyes_lists();
    test_eyes_box();
    test_eyes_backstep();
    test_eyes_long_backstep();
    test_eyes_held_walk();
    test_eyes_where();
    test_eyes_spot_walk();
}
