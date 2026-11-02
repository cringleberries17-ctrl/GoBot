// gobot_test -- the packet world: you and every monster of the map from the
// game's own frames, worked out to any moment; and the order the screen's
// monsters come in while your own plate is only remembered.
//
// Nothing here goes near the game or the network. Each frame is built by hand
// at the frame bytes docs/protocol-findings.md §4e and §4f give, the way
// main.cpp's test_own_position and test_monster_frames build theirs, and fed
// as a GamePacket stamped by hand on a clock the test winds on. Every id,
// slot, position and HP is made up: no recording of the game's bytes is on
// this machine, so a wrong offset in the docs would pass here as well.

#include "gobot/common.hpp"
#include "gobot/logger.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/own_position.hpp"
#include "gobot/packet_world.hpp"
#include "test_support.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

#ifdef GOBOT_SOURCE_DIR
std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// A function's definition in `src`, from `head` to the brace that closes it
/// -- the first at the start of a line, as every definition here ends -- or ""
/// when there is none: main.cpp's body_of, for the one check here that reads
/// the code.
std::string body_of(const std::string& src, const std::string& head) {
    const std::size_t from = src.find(head);
    if (from == std::string::npos) return {};
    const std::size_t end = src.find("\n}", from);
    return end == std::string::npos ? std::string() : src.substr(from, end + 2 - from);
}
#endif

/// Within a hundredth of a pixel, unless said: positions are worked out in
/// floating point.
bool close_to(float got, double want, double tolerance = 0.01) {
    return std::fabs(static_cast<double>(got) - want) <= tolerance;
}

// ------------------------------------------------- the screen's monsters

/// A monster's plate at `x`, `y` as the detector makes one: `x` the middle of
/// its name, `y` the top of its health bar -- the two the order goes by.
Nameplate plate_at(int x, int y, const std::string& name) {
    Nameplate n;
    n.bar = ScreenRect{ x - 20, y, 40, 3 };
    n.plate = ScreenRect{ x - 30, y - 14, 60, 12 };
    n.name = name;
    return n;
}

/// MobDetector::mobs() serves the monsters nearest first from your plate, and
/// from the plate player() remembers while yours is covered for a moment. They
/// were left in the order they were found then, and the hunter's "nearest" was
/// the topmost plate on screen all that while.
void test_nearest_first() {
    section("Monsters on screen, nearest first from you (MobDetector::nearest_first)");

    // You at 640, 400.
    {
        std::vector<Nameplate> mobs{ plate_at(740, 320, "above"), plate_at(840, 400, "level") };
        MobDetector::nearest_first(mobs, 640, 400);
        check(mobs.size() == 2 && mobs[0].name == "level" && mobs[1].name == "above",
              "a monster level with you 200 px away comes before one 100 px away and 80 px above: a "
              "height counts three times its distance");
    }
    {
        // Forty, at four distances either side of you, in the order a look
        // found them: more than a sort leaves in place by chance.
        std::vector<Nameplate> mobs;
        for (int i = 0; i < 40; ++i) {
            const int away = 60 + 40 * (i % 4);
            const int side = (i / 4) % 2 == 0 ? 1 : -1;
            mobs.push_back(plate_at(640 + side * away, 400, std::to_string(i)));
        }
        MobDetector::nearest_first(mobs, 640, 400);
        bool ordered = mobs.size() == 40;
        for (std::size_t k = 1; ordered && k < mobs.size(); ++k) {
            const int a = std::abs(mobs[k - 1].x() - 640);
            const int b = std::abs(mobs[k].x() - 640);
            ordered = a < b || (a == b && std::stoi(mobs[k - 1].name) < std::stoi(mobs[k].name));
        }
        check(ordered, "forty at four distances either side of you: nearest first, and those as far "
                       "away as each other in the order they were found");
    }

#ifdef GOBOT_SOURCE_DIR
    // Only where this was built: the source tree is not on a tester's machine.
    const std::string src = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/mob_detector.cpp");
    if (src.empty()) {
        skip("the source tree is not here, so mob_detector.cpp is not read\n");
    } else {
        const std::string mobs = body_of(src, "std::vector<Nameplate> MobDetector::mobs() const");
        const std::string player = body_of(src, "bool MobDetector::player(Nameplate& out, bool* fresh) const");
        const std::string rule = "last_player_ms_ != 0 && now_ms() - last_player_ms_ <= kPlayerMemoryMs";
        check(mobs.find("last_player_;") != std::string::npos && mobs.find(rule) != std::string::npos &&
                  player.find(rule) != std::string::npos,
              "MobDetector::mobs() sorts from your remembered plate, last_player_, while yours is not in the "
              "frame -- by player()'s own rule, for no longer than kPlayerMemoryMs");
        check(mobs.find("nearest_first(") != std::string::npos && mobs.find("sort(") == std::string::npos,
              "and in nearest_first's order, decided in that one place");
    }
#endif
}

// ------------------------------------------------------------ the frames

constexpr std::uint32_t kId = 4321;          // your character's id: made up
constexpr std::uint64_t kT0 = 5'000'000;     // where the test's clock starts

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

/// 0x0027: a walk of yours starts (speed 9), stops (0) or turns. Your id at 8
/// and 12, x, y at 16 and 20, the speed at 24, the facing byte at 28.
Built move_frame(float x, float y, float speed, int facing, std::uint32_t id = kId) {
    Built f(0x0027, 48);
    f.u32(8, id); f.u32(12, id);
    f.f32(16, x); f.f32(20, y); f.f32(24, speed);
    f.u8(28, static_cast<std::uint8_t>(facing > 0 ? 0x01 : 0xFF));
    return f;
}

/// 0x002A: off the ground (state 1, 2 or 6) or landed (0), with no facing,
/// and a vertical speed per step, below 0 upward.
Built air_frame(std::uint32_t state, float x, float y, float speed, float rise) {
    Built f(0x002A, 40);
    f.u32(8, kId); f.u32(12, kId); f.u32(16, state);
    f.f32(20, x); f.f32(24, y); f.f32(28, speed); f.f32(32, rise);
    return f;
}

/// 0x0028, one of the four at a map entry: a wrapper's sequence number at 8,
/// your id at 12 and 16, the facing in the low byte of the flags at 32.
Built here_frame(float x, float y, std::uint32_t seq, std::uint32_t id = kId) {
    Built f(0x0028, 36);
    f.u32(8, seq); f.u32(12, id); f.u32(16, id);
    f.f32(20, x); f.f32(24, y); f.f32(28, 0.0f); f.u32(32, 0x024600FFu);
    return f;
}

/// 0x0031: a skill cast, where you cast it in whole pixels.
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

/// 0x003F: a monster of `type` spawns in `slot` at a spawn point, full of HP.
Built spawn_frame(std::uint16_t slot, std::uint32_t type, int facing, std::uint16_t x, std::uint16_t y,
                  std::uint32_t max_hp) {
    Built f(0x003F, 52);
    f.u32(12, type); f.u8(16, 44); f.u8(18, static_cast<std::uint8_t>(facing));
    f.u16(20, x); f.u16(22, y); f.u32(28, max_hp); f.u16(32, slot); f.u8(44, 1);
    return f;
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
    f.u16(88, 3100); f.u16(90, 560);    // the stale pair beside it, never the position
    return f;
}

/// The monster in `slot`, or one that no position check matches when there is
/// none.
PacketWorld::Mob slot_of(const PacketWorld::View& v, std::uint32_t slot) {
    if (const PacketWorld::Mob* m = v.mob(slot)) return *m;
    PacketWorld::Mob none;
    none.slot = 0xFFFFFFFFu;
    none.x = -1.0f;
    none.y = -1.0f;
    return none;
}

// --------------------------------------------------------------------- you

/// Plain 0x0086, the server's map entry: where you appear. The client's four
/// 0x0028 after it say the same.
void test_map_entry() {
    section("Packet world: a map entry puts you at its spawn point");

    const Built fight = entry_frame(11, 1, 4690, 1180);
    {
        const auto e = read_map_entry(from_server(fight, kT0));
        check(e && e->map == 11 && e->area == 1 && e->x == 4690 && e->y == 1180,
              "0x0086: map u16 at 16, area at 18, x, y at 20 and 22");
        GamePacket up = from_server(fight, kT0);
        up.from_server = false;
        GamePacket dgram = from_server(fight, kT0);
        dgram.udp = true;
        check(!read_map_entry(up) && !read_map_entry(dgram) && !read_map_entry(wrapped(fight, kT0)),
              "never one from the client, over UDP, or from inside a wrapper");
        GamePacket shorter = from_server(fight, kT0);
        shorter.payload.pop_back();
        GamePacket longer = from_server(fight, kT0);
        longer.payload.push_back(0);
        check(!read_map_entry(shorter) && !read_map_entry(longer), "nor one of 23 or 25 bytes: all seven had 24");
    }

    PacketWorld w;
    w.on_packets({ from_server(fight, kT0) });
    PacketWorld::View v = w.view(kT0 + 2000);
    check(v.me.known && v.me.x == 4690.0f && v.me.y == 1180.0f && !v.me.moving && !v.me.airborne &&
              !v.me.uncertain && !v.me.kind && v.me.report_ms == kT0,
          "a map entry puts you at its spawn point, known and standing -- and standing, you stay there, "
          "certain, however long nothing comes");
    check(v.map == 11 && v.area == 1 && v.map_since_ms == kT0 && !v.table_seen && v.mobs.empty(),
          "the place is (11, 1) from then on, with no table and no monsters yet");

    std::vector<GamePacket> copies;
    for (std::uint32_t i = 0; i < 4; ++i)
        copies.push_back(from_client(here_frame(4690.0f, 1180.0f, 1000008 + 8 * i), kT0 + 40));
    w.on_packets(copies);
    v = w.view(kT0 + 3000);
    check(v.me.x == 4690.0f && v.me.y == 1180.0f && !v.me.moving && !v.me.uncertain && v.me.id == kId &&
              v.me.kind == OwnReport::Here && v.me.facing == -1,
          "the client's four 0x0028 at the same point change nothing but the facing they add, and give "
          "your id");
    const PacketWorld::Stats s = w.stats();
    check(s.map_entries == 1 && s.of(0x0086).used == 1 && s.own(OwnReport::Here) == 4 && s.foreign == 0,
          "one map entry and four reports of yours counted");
}

/// Your walk between its reports: on along the facing at 9 x 40 px a second
/// for kOwnReckonMs, then held and marked uncertain. A stop puts you where it
/// says, and there you stay.
void test_own_walk() {
    section("Packet world: your walk, followed between reports");

    PacketWorld w;
    const std::uint64_t t = kT0;
    w.on_packets({ from_client(move_frame(3288.0f, 1155.4f, 9.0f, -1), t) });
    const PacketWorld::View at100 = w.view(t + 100);
    check(at100.me.known && close_to(at100.me.x, 3288.0 - 36.0) && close_to(at100.me.y, 1155.4) &&
              at100.me.moving && !at100.me.uncertain && at100.me.facing == -1 && at100.me.speed == 9.0f &&
              at100.me.kind == OwnReport::Move && at100.me.id == kId,
          "a walk at speed 9 toward smaller x: 36 px on after 100 ms, 360 px a second");
    const PacketWorld::View at_cap = w.view(t + PacketWorld::kOwnReckonMs);
    check(PacketWorld::kOwnReckonMs == 3000 && close_to(at_cap.me.x, 3288.0 - 1080.0) && !at_cap.me.uncertain,
          "1080 px on after 3 s, the longest a walk is followed with no report: a walk held from look to look "
          "sends nothing until it stops");
    const PacketWorld::View past = w.view(t + PacketWorld::kOwnReckonMs + 400);
    check(close_to(past.me.x, 3288.0 - 1080.0) && past.me.uncertain && past.me.moving,
          "400 ms later still 1080 px on, and uncertain: a walk into a wall sends nothing");

    // Stopped 180 ms in, 63 px on rather than the 64.8 worked out.
    w.on_packets({ from_client(move_frame(3225.0f, 1155.4f, 0.0f, -1), t + 180) });
    const PacketWorld::View stopped = w.view(t + 5000);
    check(stopped.me.x == 3225.0f && stopped.me.y == 1155.4f && !stopped.me.moving && !stopped.me.uncertain,
          "a stop puts you where it says, and there you stay, certain, with nothing after it");
    const PacketWorld::Stats s = w.stats();
    check(s.own(OwnReport::Move) == 2 && s.own_walks == 1 && s.own_walks_stopped == 1 &&
              s.own_walks_unstopped == 0 && s.last_own_ms == t + 180,
          "one walk started, and a stop followed it within 1 s");
}

/// A skill cast says where you cast it, in whole pixels, and nothing of how
/// you move: a walk goes on from there.
void test_own_cast() {
    section("Packet world: a skill cast puts you where you cast it");

    PacketWorld w;
    const std::uint64_t t = kT0;
    w.on_packets({ from_client(move_frame(1000.0f, 500.0f, 9.0f, 1), t) });
    // 200 ms in, the walk has you at 1072; the cast says 1070, 501.
    w.on_packets({ from_client(cast_frame(1070, 501), t + 200) });
    const PacketWorld::View at_cast = w.view(t + 200);
    check(at_cast.me.x == 1070.0f && at_cast.me.y == 501.0f && at_cast.me.kind == OwnReport::Cast,
          "a cast puts you at its own x, y");
    const PacketWorld::View on = w.view(t + 300);
    check(close_to(on.me.x, 1106.0) && on.me.y == 501.0f && on.me.moving && on.me.speed == 9.0f &&
              on.me.facing == 1 && !on.me.uncertain,
          "and the walk goes on from there at its speed and facing: 36 px further 100 ms later");
    const PacketWorld::View late = w.view(t + 200 + PacketWorld::kOwnReckonMs + 100);
    check(close_to(late.me.x, 1070.0 + 1080.0) && late.me.uncertain,
          "followed for 3 s from the cast, the latest report, then held and uncertain");

    w.on_packets({ from_client(move_frame(1250.0f, 501.0f, 0.0f, 1), t + 950) });
    w.on_packets({ from_client(cast_frame(1251, 502), t + 1100) });
    const PacketWorld::View standing = w.view(t + 3000);
    check(standing.me.x == 1251.0f && standing.me.y == 502.0f && !standing.me.moving && !standing.me.uncertain,
          "a cast while standing puts you there, and standing you stay");
    check(w.stats().own(OwnReport::Cast) == 2, "both casts counted as reports of yours");
}

/// Your jumps by kind and your casts in the air, for the log's "Positions"
/// line -- what a jump attack or a double jump would show -- and the last
/// map entry, for its own line.
void test_air_counts() {
    section("Packet world: your jumps by kind, casts in the air, the last map entry");

    PacketWorld w;
    const std::uint64_t t = kT0;
    w.on_packets({ from_server(entry_frame(11, 1, 4690, 1180), t) });
    w.on_packets({ from_client(cast_frame(4690, 1180), t + 100) });                      // on the ground
    w.on_packets({ from_client(air_frame(1, 4690.0f, 1180.0f, 0.0f, -20.0f), t + 200) });
    w.on_packets({ from_client(cast_frame(4690, 1130), t + 300) });                      // in the air
    w.on_packets({ from_client(air_frame(6, 4690.0f, 1050.0f, 0.0f, -20.0f), t + 540) });
    w.on_packets({ from_client(air_frame(0, 4690.0f, 1180.0f, 0.0f, 0.0f), t + 1200) });
    w.on_packets({ from_client(air_frame(2, 4690.0f, 1180.0f, 0.0f, -36.0f), t + 2000) });
    // No landing after the high jump, and a cast long after it: the landing
    // was lost, and that cast is not one in the air.
    w.on_packets({ from_client(cast_frame(4690, 1180), t + 2000 + PacketWorld::kAirReckonMs + 100) });
    const PacketWorld::Stats s = w.stats();
    check(s.air_jumps == 1 && s.air_second == 1 && s.air_high == 1 && s.air_landed == 1 && s.air_other == 0 &&
              s.casts_in_air == 1 && s.own(OwnReport::Cast) == 3,
          "a jump, a second jump, a landing and a high jump counted by kind; of three casts, the one "
          "between the jump and its landing is in the air -- not one on the ground, nor one after a "
          "landing that never came");
    check(s.entry_map == 11 && s.entry_area == 1 && s.entry_x == 4690.0f && s.entry_y == 1180.0f &&
              s.entry_ms == t && s.map_entries == 1,
          "the last map entry: (11, 1), the spawn point it put you at, and when");
    w.reset();
    const PacketWorld::Stats none = w.stats();
    check(none.entry_ms == 0 && none.air_jumps == 0 && none.casts_in_air == 0, "and none of it after a reset");
}

/// A jump: up at its vertical speed, slowing under gravity, to its top, where
/// y holds until the landing says where you are; sideways along the facing
/// of the walk it left, for up to kAirReckonMs. A landing is taken whatever
/// its vertical speed, and one lost on the way keeps you in the air only until
/// the first report more than kAirReckonMs after the jump.
void test_own_jump() {
    section("Packet world: your jumps, followed between reports");

    {
        PacketWorld w;
        const std::uint64_t t = kT0;
        w.on_packets({ from_client(move_frame(2000.0f, 1200.0f, 0.0f, 1), t - 1000) });
        w.on_packets({ from_client(air_frame(1, 2000.0f, 1200.0f, 0.0f, -20.0f), t) });
        const PacketWorld::View up = w.view(t + 100);
        check(up.me.airborne && close_to(up.me.y, 1200.0 - 68.0) && up.me.x == 2000.0f && !up.me.moving &&
                  up.me.kind == OwnReport::Air,
              "a jump (state 1, vertical speed -20): 68 px up after 100 ms");
        check(close_to(w.view(t + 333).me.y, 1200.0 - 400.0 / 3.0, 0.05),
              "133 px up at its top, a third of a second in: GoC's jump of -800 px/s under 2400 px/s2");
        const PacketWorld::View held = w.view(t + 800);
        check(close_to(held.me.y, 1200.0 - 400.0 / 3.0) && held.me.airborne && !held.me.uncertain,
              "and held there after it: the way down is the landing's to say");
        check(w.view(t + 1600).me.uncertain, "in the air with no landing for more than 1.5 s: uncertain");

        w.on_packets({ from_client(air_frame(0, 2000.0f, 1200.0f, 0.0f, 0.0f), t + 700) });
        const PacketWorld::View landed = w.view(t + 800);
        check(!landed.me.airborne && landed.me.y == 1200.0f && landed.me.x == 2000.0f && !landed.me.uncertain,
              "landed (state 0): where the landing says, on the ground");
    }
    {
        PacketWorld w;
        const std::uint64_t t = kT0;
        w.on_packets({ from_client(air_frame(6, 700.0f, 900.0f, 0.0f, -20.0f), t) });
        check(w.view(t + 10).me.airborne, "state 6, a second jump in the air: off the ground");
        w.on_packets({ from_client(air_frame(2, 700.0f, 900.0f, 0.0f, -36.0f), t + 1000) });
        check(close_to(w.view(t + 1600).me.y, 900.0 - 432.0, 0.05) &&
                  close_to(w.view(t + 2000).me.y, 900.0 - 432.0, 0.05),
              "a high jump (state 2, -36): 432 px up at its top, 0.6 s in, as GoC's");
    }
    {
        // Out of a walk: 0x002A says no facing, so the walk's is kept.
        PacketWorld w;
        const std::uint64_t t = kT0;
        w.on_packets({ from_client(move_frame(500.0f, 1000.0f, 9.0f, 1), t) });
        w.on_packets({ from_client(air_frame(1, 536.0f, 1000.0f, 9.0f, -20.0f), t + 100) });
        const PacketWorld::View one = w.view(t + 1100);
        check(close_to(one.me.x, 536.0 + 360.0) && one.me.moving && one.me.facing == 1 && !one.me.uncertain,
              "a jump out of a walk goes on along the walk's facing at the jump's speed: 360 px in 1 s");
        const PacketWorld::View late = w.view(t + 2100);
        check(close_to(late.me.x, 536.0 + 540.0) && late.me.uncertain,
              "for 1.5 s with no report, then held and uncertain");
    }
    {
        // Off an edge and a long way down. What a landing's vertical speed
        // holds, no recording says; if it is the speed you came down at, it
        // is 60 after this fall of some 1,160 px. Nothing follows it.
        PacketWorld w;
        const std::uint64_t t = kT0;
        w.on_packets({ from_client(air_frame(1, 900.0f, 400.0f, 0.0f, 12.2f), t) });
        const PacketWorld::View falling = w.view(t + 500);
        check(falling.me.airborne && falling.me.y == 400.0f && !falling.me.uncertain,
              "walking off an edge (state 1, +12.2): in the air, with y held, as the way down is not followed");
        w.on_packets({ from_client(air_frame(0, 900.0f, 1558.0f, 0.0f, 60.2f), t + 800) });
        const PacketWorld::View down = w.view(t + 5000);
        check(!down.me.airborne && down.me.y == 1558.0f && !down.me.uncertain &&
                  w.stats().of(0x002A).refused == 0,
              "a landing at a vertical speed of 60 still lands you where it says: a speed nothing follows "
              "refuses nothing");
        w.on_packets({ from_client(air_frame(1, 900.0f, 1558.0f, 0.0f, -60.0f), t + 6000) });
        check(w.stats().of(0x002A).refused == 1 && !w.view(t + 6100).me.airborne,
              "but a jump rising at 60, which would be followed, is refused: none has risen faster than 36");
    }
    {
        // A landing lost on the way. Without a limit, one datagram the capture
        // missed would keep you in the air until your next jump.
        PacketWorld w;
        const std::uint64_t t = kT0;
        w.on_packets({ from_client(air_frame(1, 2000.0f, 1200.0f, 0.0f, -20.0f), t) });
        w.on_packets({ from_client(move_frame(2000.0f, 1100.0f, 9.0f, 1), t + 1000) });
        check(w.view(t + 1100).me.airborne, "a walk 1 s into a jump, with no landing yet, is a turn in the air");
        w.on_packets({ from_client(move_frame(2300.0f, 1200.0f, 9.0f, 1), t + 2000) });
        const PacketWorld::View walking = w.view(t + 4000);
        check(!walking.me.airborne && close_to(walking.me.x, 2300.0 + 720.0) && !walking.me.uncertain,
              "a report more than 1.5 s after the jump, with still no landing, has you on the ground: a walk "
              "there is followed as a walk -- 2 s on, and certain -- not held at 1.5 s as in the air");
        w.on_packets({ from_client(move_frame(2400.0f, 1200.0f, 0.0f, 1), t + 2300) });
        const PacketWorld::View standing = w.view(t + 9000);
        check(!standing.me.airborne && !standing.me.uncertain && standing.me.x == 2400.0f,
              "and standing there you stay certain, however long nothing comes");
    }
}

/// What is not yours moves you not at all: the server's frames of your
/// layouts, which are other actors' moves; the pet's frames from your own
/// client; and a report under a second id.
void test_not_yours() {
    section("Packet world: other actors, the pet and a second id are not you");

    PacketWorld w;
    const std::uint64_t t = kT0;
    w.on_packets({ from_client(move_frame(3000.0f, 900.0f, 0.0f, 1), t) });
    const std::uint64_t updates = w.view(t).updates;

    Built pet(0x010D, 52);      // the pet, where it is
    pet.u32(8, kId); pet.u32(12, kId); pet.f32(24, 2660.0f); pet.f32(28, 1070.0f);
    Built hop(0x0117, 28);      // the pet's teleport back to you
    hop.u32(8, kId); hop.u32(12, kId); hop.f32(16, 2660.0f); hop.f32(20, 1070.0f);
    w.on_packets({ from_server(move_frame(100.0f, 100.0f, 9.5f, 1, 777), t + 100), from_client(pet, t + 100),
                   from_client(hop, t + 100), from_client(move_frame(200.0f, 200.0f, 9.0f, -1, kId + 1), t + 100) });
    const PacketWorld::View v = w.view(t + 1000);
    check(v.me.x == 3000.0f && v.me.y == 900.0f && !v.me.moving && v.me.id == kId && v.updates == updates,
          "another actor's 0x0027 from the server, the pet's 0x010D and 0x0117, and a report under a second "
          "id: you stay where you were, and nothing was used");
    const PacketWorld::Stats s = w.stats();
    check(s.foreign == 1 && s.last_foreign_ms == t + 100 && s.of(0x0027).used == 1 && s.of(0x0027).refused == 0 &&
              s.of(0x0027).wrong_size == 0,
          "the second id's report is counted as foreign; the server's 0x0027 is not counted at all");

    // Each map entry lets the next report's id be yours: a character changed
    // at the selection screen comes back through one.
    w.on_packets({ from_server(entry_frame(10, 60, 325, 220), t + 2000) });
    w.on_packets({ from_client(here_frame(325.0f, 220.0f, 1000008, kId + 1), t + 2040) });
    const PacketWorld::View after = w.view(t + 2100);
    check(after.me.id == kId + 1 && after.me.x == 325.0f && w.stats().foreign == 1,
          "after a map entry the next report's id is yours, and no longer foreign");
    w.on_packets({ from_client(move_frame(330.0f, 220.0f, 0.0f, 1, kId), t + 2200) });
    check(w.stats().foreign == 2 && w.view(t + 2300).me.x == 325.0f, "and the old id is foreign from then on");
}

// ---------------------------------------------------------------- monsters

/// The map's table, 0x0042: every monster with its type and max HP, and the
/// walkers walking on along their facing for a whole walk, a turn included.
void test_monster_table() {
    section("Packet world: the map's table, and monsters walking");

    const std::vector<Entry> fight = {
        { 0, 1004401, 44, 1, 1500, 900, 2.6f, 7000, 7676 },      // walking toward larger x, hurt
        { 7, 1003901, 39, -1, 80, 1210, 0.0f, 3044, 3044 },      // standing
        { 12, 1004401, 44, -1, 3333, 640, 2.6f, 7676, 7676 },    // walking toward smaller x
    };
    PacketWorld w;
    const std::uint64_t t = kT0;
    w.on_packets({ from_server(entry_frame(11, 1, 4690, 1180), t - 600) });
    w.on_packets({ wrapped(table_frame(fight), t) });

    const PacketWorld::View at0 = w.view(t);
    const PacketWorld::Mob a = slot_of(at0, 0), b = slot_of(at0, 7), c = slot_of(at0, 12);
    check(at0.mobs.size() == 3 && a.slot == 0 && b.slot == 7 && c.slot == 12 && at0.table_seen &&
              at0.first_monster_ms == t,
          "a table of three gives three monsters, slot 0 among them");
    check(a.type_id == 1004401 && b.type_id == 1003901 && a.level == 44 && b.level == 39 && a.hp == 7000 &&
              a.max_hp == 7676 && b.max_hp == 3044 && a.max_hp_known && b.max_hp_known && !a.dead && !b.dead,
          "each with its type, level, HP and max HP from the table, one of them hurt before you came");

    const PacketWorld::View at1 = w.view(t + 1000);
    check(close_to(slot_of(at1, 0).x, 1604.0) && slot_of(at1, 0).moving && slot_of(at1, 0).y == 900.0f &&
              close_to(slot_of(at1, 12).x, 3229.0) && slot_of(at1, 7).x == 80.0f && !slot_of(at1, 7).moving,
          "a walker at speed 2.6 is 104 px on along its facing after 1 s, either way; the one standing has "
          "not moved");
    const PacketWorld::View at5 = w.view(t + 5000);
    const PacketWorld::View at6 = w.view(t + 6000);
    check(close_to(slot_of(at5, 0).x, 2020.0) && !slot_of(at5, 0).moving && close_to(slot_of(at6, 0).x, 2020.0),
          "5 s into its walk it stops moving, 520 px on, and is held there");

    // Slot 12 turns 3 s in, 312 px on, and walks back toward larger x.
    w.on_packets({ from_server(event_frame(1, 12, 1, 2.6f, 3021, 640, 7676), t + 3000) });
    const PacketWorld::View turned4 = w.view(t + 4000);
    const PacketWorld::View turned5 = w.view(t + 5000);
    const PacketWorld::View turned7 = w.view(t + 7000);
    check(close_to(slot_of(turned4, 12).x, 3125.0) && slot_of(turned4, 12).moving && slot_of(turned4, 12).facing == 1,
          "a turn 3 s into the walk: on from where it turned, the other way");
    check(close_to(slot_of(turned5, 12).x, 3229.0) && !slot_of(turned5, 12).moving &&
              close_to(slot_of(turned7, 12).x, 3229.0),
          "and it stops 5 s into the walk all the same: a turn does not start its 200 steps again");

    // Slot 0's walk ran its course at 5 s; a walk at 6 s is a new one.
    w.on_packets({ from_server(event_frame(1, 0, -1, 2.6f, 2020, 900, 7000), t + 6000) });
    const PacketWorld::View at8 = w.view(t + 8000);
    check(close_to(slot_of(at8, 0).x, 2020.0 - 208.0) && slot_of(at8, 0).moving,
          "a walk after the last one has run its course is a new walk, followed for its own 5 s");
    const PacketWorld::Stats s = w.stats();
    check(s.tables == 1 && s.of(0x0042).used == 1 && s.walks == 2 && s.last_monster_ms == t + 6000,
          "one table and two walk events counted");
}

/// A kill, and the monster after it: dead at the hit, at 0 HP, where it was
/// struck; then, 3 s later, a new monster at a spawn point, full of HP. A
/// blow's speed refuses no kill, and a table loaded between a kill and its
/// spawn has that monster dead.
void test_kill_respawn() {
    section("Packet world: a kill, and the monster that takes its slot");

    PacketWorld w;
    const std::uint64_t t = kT0;
    w.on_packets({ from_server(event_frame(0, 5, 1, 0.0f, 2400, 1180, 600), t) });
    w.on_packets({ from_server(event_frame(9, 5, 1, 7.0f, 2410, 1180, 600 - 5600, 5600), t + 1000) });
    const PacketWorld::Mob dead = slot_of(w.view(t + 2500), 5);
    check(dead.slot == 5 && dead.dead && dead.dead_ms == t + 1000 && dead.hp == 0 && !dead.moving &&
              dead.x == 2410.0f,
          "a hit that leaves HP at -5000 has killed it, then and there: HP 0, where it was struck, not thrown "
          "on at the blow's speed of 7");
    w.on_packets({ from_server(event_frame(9, 5, 1, 7.0f, 2410, 1180, -9000, 4000), t + 1100) });
    check(w.stats().hits == 2 && w.stats().kills == 1 && slot_of(w.view(t + 1200), 5).dead_ms == t + 1000,
          "a second blow to the dead monster is no second kill");

    // 3 s later, a monster of another type in the slot.
    w.on_packets({ from_server(spawn_frame(5, 2000301, -1, 3100, 1150, 2000), t + 4000) });
    const PacketWorld::Mob back = slot_of(w.view(t + 4000), 5);
    check(back.slot == 5 && !back.dead && back.dead_ms == 0 && back.hp == 2000 && back.max_hp == 2000 &&
              back.max_hp_known && back.type_id == 2000301 && back.x == 3100.0f && back.y == 1150.0f &&
              back.facing == -1 && !back.moving,
          "a spawn in the slot 3 s later: alive again, full of HP, of the new type, standing at the spawn point");
    w.on_packets({ from_server(event_frame(0, 5, -1, 0.0f, 3100, 1172, 2000), t + 4200) });
    const PacketWorld::Mob landed = slot_of(w.view(t + 4300), 5);
    check(landed.x == 3100.0f && landed.y == 1172.0f && landed.hp == 2000 && !landed.dead,
          "its first stop, 22 px lower as it lands, puts it there");
    const PacketWorld::Stats s = w.stats();
    check(s.stops == 2 && s.hits == 2 && s.kills == 1 && s.spawns == 1 && s.of(0x003F).used == 1,
          "two stops, two hits, one kill and one spawn counted");

    {
        // A blow that throws harder than any monster walks. Its speed is not
        // followed, so the frame is not lost over it.
        PacketWorld w2;
        w2.on_packets({ from_server(event_frame(9, 6, -1, 60.0f, 1800, 1180, -20000, 23044), t) });
        const PacketWorld::Mob thrown = slot_of(w2.view(t + 500), 6);
        check(thrown.dead && thrown.hp == 0 && thrown.x == 1800.0f && w2.stats().kills == 1 &&
                  w2.stats().of(0x0038).refused == 0,
              "a killing blow that throws the monster at 60 is a kill all the same, where it was struck");
    }
    {
        // A map loaded 2 s after a kill on it, the spawn still to come. The
        // table's HP is the slot's 0x0038 HP, which the kill left at -5000,
        // and its speed may be the 60 the blow threw the monster at.
        PacketWorld w3;
        w3.on_packets({ wrapped(table_frame({ { 4, 1003901, 39, 1, 2410, 1180, 60.0f,
                                                 static_cast<std::uint32_t>(-5000), 3044 },
                                               { 9, 1003901, 39, -1, 600, 1180, 2.6f, 3044, 3044 } }),
                                t) });
        const PacketWorld::View v = w3.view(t + 1000);
        const PacketWorld::Mob fell = slot_of(v, 4);
        check(v.mobs.size() == 2 && fell.dead && fell.hp == 0 && fell.max_hp == 3044 && fell.dead_ms == t &&
                  !fell.moving && fell.x == 2410.0f,
              "a table with a monster at -5000 HP lists it dead where it fell, with its max HP -- not alive with "
              "some 4 billion -- and not thrown on at 60");
        check(w3.stats().tables == 1 && w3.stats().kills == 0 && close_to(slot_of(v, 9).x, 600.0 - 104.0),
              "the table is taken all the same, its walker walks on, and no kill is counted: no blow was seen");
        w3.on_packets({ from_server(spawn_frame(4, 1003901, 1, 1500, 1150, 3044), t + 2000) });
        const PacketWorld::Mob spawned = slot_of(w3.view(t + 2000), 4);
        check(!spawned.dead && spawned.hp == 3044 && spawned.x == 1500.0f,
              "and the slot's spawn brings it back, as after any kill");
    }
}

/// A monster that sends nothing for a long while is still there; a map
/// entry, to any other place, takes every monster away.
void test_map_change() {
    section("Packet world: silent monsters stay; a map entry clears them");

    PacketWorld w;
    const std::uint64_t t = kT0;
    w.on_packets({ from_server(entry_frame(10, 4, 690, 1145), t) });
    w.on_packets({ wrapped(table_frame({ { 3, 1003901, 39, 1, 900, 1145, 0.0f, 3044, 3044 } }), t + 300) });
    const PacketWorld::View quiet = w.view(t + 10'300);
    check(slot_of(quiet, 3).x == 900.0f && quiet.table_seen,
          "a monster silent for 10 s is still listed, where it stood: nothing sweeps it");

    w.on_packets({ from_server(entry_frame(10, 5, 350, 2430), t + 10'400) });
    const PacketWorld::View moved = w.view(t + 10'500);
    check(moved.mobs.empty() && moved.map == 10 && moved.area == 5 && moved.map_since_ms == t + 10'400 &&
              !moved.table_seen && moved.first_monster_ms == 0,
          "from (10, 4) to (10, 5), another area of the same map: its monsters are gone, and no table or "
          "monster frame has come for the new place yet");
    check(moved.me.x == 350.0f && moved.me.y == 2430.0f && !moved.me.moving, "and you stand at its spawn point");
    check(w.stats().map_entries == 2, "two map entries counted");
}

/// The map entries ever (View::map_epoch), for the hunting spot: one more at
/// each 0x0086, never taken back -- not by reset(), a capture started again
/// -- and none for any other frame. And where your last report put you, as
/// it came, beside where a walk has taken you since.
void test_map_epoch() {
    section("Packet world: map entries ever (map_epoch), and your last report's own place");

    PacketWorld w;
    const std::uint64_t t = kT0;
    check(w.view(t).map_epoch == 0, "none yet: 0");
    w.on_packets({ from_server(entry_frame(10, 4, 690, 1145), t) });
    w.on_packets({ wrapped(table_frame({ { 3, 1003901, 39, 1, 900, 1145, 0.0f, 3044, 3044 } }), t + 300),
                   from_client(move_frame(690.0f, 1145.0f, 0.0f, 1), t + 400) });
    const std::uint32_t first = w.view(t + 500).map_epoch;
    w.on_packets({ from_server(entry_frame(10, 5, 350, 2430), t + 1000) });
    const std::uint32_t second = w.view(t + 1100).map_epoch;
    check(first == 1 && second == 2, "one more at each map entry, and none for a table or a report of yours");

    check(w.view(t + 1100).entry_x == 350.0f && w.view(t + 1100).entry_y == 2430.0f,
          "and the view has where the last entry put you: its spawn, 350,2430");

    w.reset();
    const PacketWorld::View after = w.view(t + 1200);
    check(after.map_epoch == 2 && after.map == 0 && after.map_since_ms == 0 && w.stats().map_entries == 0,
          "reset() forgets the map and its count, and not map_epoch: a door taken is still taken");
    check(after.entry_x == 350.0f && after.entry_y == 2430.0f && w.stats().entry_x == 0.0f &&
              w.stats().entry_y == 0.0f,
          "nor the last entry's spawn in the view, which the stats forget: an entry looked at only after a capture "
          "started again is measured from where it put you, not from 0,0");
    w.on_packets({ from_server(entry_frame(10, 4, 700, 1145), t + 2000) });
    const PacketWorld::View back = w.view(t + 2000);
    check(back.map_epoch == 3 && back.me.report_x == 700.0f && back.me.report_y == 1145.0f &&
              back.entry_x == 700.0f && back.entry_y == 1145.0f,
          "the next entry after it is the third, and puts your report's own place, and the entry's spawn, there");

    // A walk from 1000 toward larger x: 360 px a second, worked out; the
    // report's own place stays where the walk began.
    w.on_packets({ from_client(move_frame(1000.0f, 1145.0f, 9.0f, 1), t + 3000) });
    const PacketWorld::View walking = w.view(t + 3500);
    check(close_to(walking.me.x, 1180.0) && walking.me.report_x == 1000.0f && walking.me.report_y == 1145.0f,
          "a walk worked out 180 px on after 0.5 s; report_x stays at 1000, where the report put you");
    w.on_packets({ from_client(move_frame(1150.0f, 1145.0f, 0.0f, 1), t + 3500) });
    const PacketWorld::View stopped = w.view(t + 4000);
    check(stopped.me.x == 1150.0f && stopped.me.report_x == 1150.0f, "its stop moves both to where it says");
}

/// Started mid-map, with no table and no map entry: each slot is added as its
/// frames come, and its max HP is the most HP it has been seen with.
void test_no_table() {
    section("Packet world: monsters without the table, started mid-map");

    PacketWorld w;
    const std::uint64_t t = kT0;
    w.on_packets({ from_server(event_frame(0, 2, 1, 0.0f, 1000, 800, 800), t),
                   from_server(event_frame(1, 9, -1, 2.6f, 2000, 800, 1200), t + 50) });
    const PacketWorld::View v = w.view(t + 1050);
    const PacketWorld::Mob two = slot_of(v, 2);
    check(v.mobs.size() == 2 && two.slot == 2 && two.hp == 800 && two.max_hp == 800 && !two.max_hp_known &&
              two.type_id == 0,
          "with no table, a slot is listed from its first frame, with no type yet and the HP it had for max HP");
    check(close_to(slot_of(v, 9).x, 1896.0) && slot_of(v, 9).moving, "a walk first seen walks on like any other");
    check(v.map_since_ms == 0 && !v.table_seen && v.first_monster_ms == t && !v.me.known,
          "no map entry and no table yet; the first monster frame's time is kept, and you are not known");

    w.on_packets({ from_server(event_frame(9, 2, 1, 7.0f, 1000, 800, 200, 600), t + 2000),
                   from_server(event_frame(9, 4, 1, 7.0f, 1500, 800, 300, 500), t + 2000) });
    const PacketWorld::View hurt = w.view(t + 2000);
    const PacketWorld::Mob hit = slot_of(hurt, 2), first_hit = slot_of(hurt, 4);
    check(hit.hp == 200 && hit.max_hp == 800 &&
              close_to(static_cast<float>(hit.hp) / static_cast<float>(hit.max_hp), 0.25, 1e-6),
          "a blow that leaves 200 of 800: a quarter of the most HP it was seen with");
    check(first_hit.slot == 4 && first_hit.hp == 300 && first_hit.max_hp == 800 && !first_hit.dead,
          "a monster first seen in a blow: the HP it had before it, the HP after plus the damage, stands for "
          "max HP");
    w.on_packets({ from_server(event_frame(0, 2, 1, 0.0f, 1000, 800, 900), t + 3000) });
    check(slot_of(w.view(t + 3000), 2).max_hp == 900, "more HP than the max so far raises it: max HP is never "
                                                      "below HP");
}

// ------------------------------------------------------------ guards, counts

/// What a game update that moved fields about would look like, and what is
/// counted for the log: frames of another size, values no frame has had,
/// states never seen, and walks of yours that no stop followed.
void test_guards() {
    section("Packet world: what it reads, what it refuses, and what is counted");

    check(PacketWorld::opcodes() == std::vector<std::uint16_t>{ 0x0027, 0x0028, 0x0029, 0x002A, 0x0031, 0x0038,
                                                                 0x003F, 0x0042, 0x0086 },
          "it reads 0x0027, 0x0028, 0x0029, 0x002A and 0x0031 of yours, 0x0038, 0x003F and 0x0042 of the "
          "monsters, and 0x0086: a capture's opcode filter needs all nine");

    PacketWorld w;
    const std::uint64_t t = kT0;
    GamePacket short38 = from_server(event_frame(0, 1, 1, 0.0f, 100, 100, 100), t);
    short38.payload.pop_back();
    GamePacket short27 = from_client(move_frame(100.0f, 100.0f, 0.0f, 1), t);
    short27.payload.pop_back();
    w.on_packets({ short38, short27 });
    PacketWorld::Stats s = w.stats();
    check(s.of(0x0038).wrong_size == 1 && s.of(0x0027).wrong_size == 1 && s.of(0x0038).used == 0 &&
              s.of(0x0027).used == 0 && s.last_wrong_size_ms == t && s.last_own_wrong_size_ms == t,
          "a 95-byte 0x0038 and a 47-byte 0x0027 are refused, and counted under their opcodes -- the 0x0027, a "
          "report of yours, said apart too");
    PacketWorld::View v = w.view(t);
    check(v.mobs.empty() && !v.me.known && v.updates == 0, "and neither is used");

    // Taken from its wrapper still packed, as the live capture hands it on
    // until it unpacks: not the size of a table.
    w.on_packets({ wrapped(Built(0x0042, 300), t + 5) });
    check(w.stats().of(0x0042).wrong_size == 1 && !w.view(t + 5).table_seen,
          "a table still packed is refused for its size");

    w.on_packets({ from_server(event_frame(2, 1, 1, 0.0f, 100, 100, 100), t + 10) });
    s = w.stats();
    check(s.unknown_states == 1 && s.of(0x0038).refused == 1 && w.view(t + 10).mobs.empty(),
          "a 0x0038 of state 2, which none was, is refused and counted as a state not known");
    w.on_packets({ from_server(event_frame(1, 1, 1, 80.0f, 100, 100, 100), t + 15) });
    check(w.stats().of(0x0038).refused == 2 && w.stats().unknown_states == 1 && w.view(t + 15).mobs.empty(),
          "nor is a monster walking at speed 80 taken");
    w.on_packets({ wrapped(table_frame({ { 1, 1003901, 39, 1, 100, 100, 80.0f, 3044, 3044 } }), t + 16) });
    check(w.stats().of(0x0042).refused == 1 && !w.view(t + 16).table_seen && w.view(t + 16).mobs.empty(),
          "nor a table whose living monster walks at 80");

    w.on_packets({ from_client(move_frame(70000.0f, 100.0f, 0.0f, 1), t + 20),
                   from_client(move_frame(100.0f, -5.0f, 0.0f, 1), t + 20),
                   from_client(move_frame(100.0f, 100.0f, 60.0f, 1), t + 20),
                   from_client(move_frame(100.0f, 100.0f, -1.0f, 1), t + 20) });
    v = w.view(t + 20);
    check(w.stats().of(0x0027).refused == 4 && !v.me.known,
          "a report of yours at x 70000 or y -5, or at speed 60 or -1, is refused: no report has had those");
    check(v.updates == 0, "and a batch with nothing used in it is no update");

    // A walk of yours that no stop follows within kStopWithinMs -- as long as
    // a walk is followed -- then one that a stop ends 2.5 s in, as a held
    // walk's renewal does.
    w.on_packets({ from_client(move_frame(500.0f, 500.0f, 9.0f, 1), t + 100) });
    const Built tick(0x03C6, 12);   // the server's tick, once a second
    w.on_packets({ from_server(tick, t + 100 + PacketWorld::kStopWithinMs + 100) });
    s = w.stats();
    check(PacketWorld::kStopWithinMs == PacketWorld::kOwnReckonMs && s.own_walks == 1 &&
              s.own_walks_unstopped == 1 && s.own_walks_stopped == 0,
          "a walk with no stop within 3 s -- as long as a walk is followed -- is counted as unstopped once a "
          "later frame, here the server's tick, says the time has gone by");
    const std::uint64_t t2 = t + 100 + PacketWorld::kStopWithinMs + 200;
    w.on_packets({ from_client(move_frame(1600.0f, 500.0f, 0.0f, 1), t2) });
    w.on_packets({ from_client(move_frame(2000.0f, 500.0f, 9.0f, -1), t2 + 100) });
    w.on_packets({ from_client(move_frame(1100.0f, 500.0f, 0.0f, -1), t2 + 100 + 2500) });
    s = w.stats();
    check(s.own_walks == 2 && s.own_walks_stopped == 1 && s.own_walks_unstopped == 1,
          "the late stop changes nothing, and a walk held 2.5 s and then stopped -- a held walk's renewal -- is "
          "counted as stopped, not as one whose stop was lost");
}

/// A 0x0038 of a state none of the recordings had -- the hunt of 2026-10-27,
/// its monsters fighting back, sent 3,399 -- moves its monster to where it
/// says only when that is within kUnknownPlacePx of where the monster was
/// worked out to be; standing there, its HP as it was. Each such state is
/// counted, and its first frames written to the log.
void test_unknown_states() {
    section("Packet world: monster frames of a state not known");

    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_level(LogLevel::Info);
    log.set_ring_capacity(400);
    log.clear_ring();

    PacketWorld w;
    const std::uint64_t t = kT0;
    w.on_packets({ from_server(event_frame(0, 5, 1, 0.0f, 2400, 1180, 6000), t) });
    w.on_packets({ from_server(event_frame(5, 5, -1, 3.0f, 2450, 1170, 1), t + 500) });
    const PacketWorld::Mob placed = slot_of(w.view(t + 600), 5);
    PacketWorld::Stats s = w.stats();
    check(placed.x == 2450.0f && placed.y == 1170.0f && !placed.moving && placed.hp == 6000 &&
              placed.facing == 1 && placed.event_ms == t + 500 && s.unknown_states == 1 && s.unknown_placed == 1 &&
              s.of(0x0038).used == 2 && s.of(0x0038).refused == 0,
          "state 5, 50 px from where the monster stood: put there, standing, its HP and facing as they were");

    w.on_packets({ from_server(event_frame(5, 5, 1, 0.0f, 2900, 1170, 6000), t + 700) });
    s = w.stats();
    check(slot_of(w.view(t + 800), 5).x == 2450.0f && s.unknown_states == 2 && s.unknown_placed == 1 &&
              s.of(0x0038).refused == 1,
          "450 px off where it was: refused, and it stays where it was");

    w.on_packets({ from_server(event_frame(3, 8, 1, 0.0f, 2450, 1170, 6000), t + 800) });
    s = w.stats();
    check(!w.view(t + 800).mob(8) && s.unknown_states == 3 && s.unknown_placed == 1,
          "state 3 for a slot no frame has named: refused -- there is nowhere to judge it against");
    check(s.unknown_kinds[0].state == 5 && s.unknown_kinds[0].count == 2 && s.unknown_kinds[1].state == 3 &&
              s.unknown_kinds[1].count == 1 && s.unknown_kinds[2].count == 0,
          "counted by their states: 5 twice, then 3 once");

    // A walker, worked out to be 104 px on a second into its walk.
    w.on_packets({ from_server(event_frame(1, 9, 1, 2.6f, 1000, 900, 3044), t + 1000) });
    w.on_packets({ from_server(event_frame(7, 9, 1, 2.6f, 1290, 900, 3044), t + 2000) });
    const PacketWorld::Mob walker = slot_of(w.view(t + 3000), 9);
    check(walker.x == 1290.0f && !walker.moving && w.stats().unknown_placed == 2,
          "a walker worked out at 1104, 1 s on: put at 1290 by state 7, 186 px on, and standing there");

    // A dead one stays dead.
    w.on_packets({ from_server(event_frame(9, 5, 1, 7.0f, 2450, 1170, -50, 6050), t + 3000),
                   from_server(event_frame(5, 5, 1, 0.0f, 2460, 1170, 6000), t + 3100) });
    const PacketWorld::Mob dead = slot_of(w.view(t + 3200), 5);
    check(dead.dead && dead.x == 2450.0f && w.stats().unknown_placed == 2, "a state 5 for a monster killed: refused");
    const PacketWorld::Stats::Kind five = w.stats().unknown_kinds[0];
    check(five.state == 5 && five.count == 3 && five.judged == 2 && five.placed == 1 && five.off_px == 500.0,
          "and per state, how far off where the monster was worked out to be: state 5 judged twice, 50 and 450 px "
          "off, placed once -- the frame for the dead one not judged");

    // A frame of a known state that its reader refused is no unknown state.
    w.on_packets({ from_server(event_frame(0, 9, 1, std::nanf(""), 1290, 900, 3044), t + 3300) });
    check(w.stats().unknown_states == 5 && w.stats().of(0x0038).refused == 4,
          "a stop whose speed is no number: refused as it was, not counted as a state not known");

    std::vector<std::string> said;
    for (const LogLine& line : log.ring())
        if (line.text.find("Positions: a monster frame (0x0038) of state") != std::string::npos)
            said.push_back(line.text);
    log.set_level(was);
    log.set_ring_capacity(0);
    const auto of_state = [&said](const char* state) {
        int n = 0;
        for (const std::string& line : said)
            if (line.find(std::string("of state ") + state + ",") != std::string::npos) ++n;
        return n;
    };
    check(said.size() == 4 && of_state("5") == 2 && of_state("3") == 1 && of_state("7") == 1 &&
              said.front().find("slot 5 at 2450,1170") != std::string::npos &&
              said.front().find("its bytes from 8: ") != std::string::npos,
          "the first two frames of each such state written to the log, with their bytes -- " +
              std::to_string(said.size()) + " line(s)");

    w.reset();
    const PacketWorld::Stats after = w.stats();
    check(after.unknown_states == 0 && after.unknown_placed == 0 && after.unknown_kinds[0].count == 0,
          "reset() forgets them");
}

/// reset() forgets all of it, as at a new capture. The update number counts
/// the batches with a frame used, and never goes back.
void test_reset() {
    section("Packet world: what reset() forgets, and what updates counts");

    PacketWorld w;
    const std::uint64_t t = kT0;
    w.on_packets({ from_server(entry_frame(11, 1, 4690, 1180), t),
                   wrapped(table_frame({ { 0, 1004401, 44, 1, 1500, 900, 2.6f, 7000, 7676 } }), t + 300) });
    check(w.view(t + 300).updates == 1, "one batch with two frames used is one update");
    const Built tick(0x03C6, 12);
    Built pet(0x010D, 52);
    pet.u32(8, kId); pet.u32(12, kId);
    GamePacket cut = from_client(here_frame(4690.0f, 1180.0f, 8), t + 400);
    cut.payload.pop_back();
    w.on_packets({ from_server(tick, t + 400), from_client(pet, t + 400), cut });
    check(w.view(t + 400).updates == 1, "a batch of a tick, the pet and a frame refused for its size is none");
    w.on_packets({ from_client(here_frame(4690.0f, 1180.0f, 8), t + 500) });
    check(w.view(t + 500).updates == 2, "and the next batch with a frame used in it is the second");

    w.reset();
    const PacketWorld::View v = w.view(t + 600);
    check(!v.me.known && v.mobs.empty() && v.map == 0 && v.area == 0 && v.map_since_ms == 0 && !v.table_seen &&
              v.first_monster_ms == 0,
          "reset() forgets you, the monsters and the map");
    const PacketWorld::Stats s = w.stats();
    check(s.map_entries == 0 && s.tables == 0 && s.own(OwnReport::Here) == 0 && s.of(0x0028).wrong_size == 0 &&
              s.last_own_ms == 0 && s.last_monster_ms == 0 && s.of(0x0086).opcode == 0x0086,
          "and every count");
    check(v.updates == 2, "but not the update number: it never goes back");
    w.on_packets({ from_client(move_frame(4000.0f, 1000.0f, 0.0f, 1, kId + 9), t + 700) });
    const PacketWorld::View again = w.view(t + 700);
    check(again.me.id == kId + 9 && w.stats().foreign == 0 && again.updates == 3,
          "after it, the next report's id is yours, whatever it was before");
}

} // namespace

void test_packet_world(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_nearest_first();
    test_map_entry();
    test_own_walk();
    test_own_cast();
    test_own_jump();
    test_air_counts();
    test_not_yours();
    test_monster_table();
    test_kill_respawn();
    test_map_change();
    test_map_epoch();
    test_no_table();
    test_guards();
    test_unknown_states();
    test_reset();
}
