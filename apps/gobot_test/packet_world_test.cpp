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
#include "gobot/hunter.hpp"
#include "gobot/logger.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/monster_frames.hpp"
#include "gobot/own_position.hpp"
#include "gobot/packet_eyes.hpp"
#include "gobot/packet_world.hpp"
#include "test_support.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
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

/// 0x0029, the third report of yours: your id at 8 and 12, the facing as a
/// word at 16 (1 toward larger x, 0xFF toward smaller), the speed at 20, x, y
/// at 24 and 28.
Built other_frame(float x, float y, float speed, int facing) {
    Built f(0x0029, 44);
    f.u32(8, kId); f.u32(12, kId);
    f.u32(16, facing > 0 ? 0x01u : 0xFFu);
    f.f32(20, speed); f.f32(24, x); f.f32(28, y);
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

/// Which way you face, and which way and since when a 0x0027 said so
/// (Me::said_facing, Me::facing_ms). The facing a walk is followed by is set
/// by a 0x0027, 0x0029 or 0x0028 -- each says it; the one the hunter turns
/// the character by only by a 0x0027, whose facing byte is the one checked
/// against the game. A cast or a jump says nothing of it and leaves all three
/// as they were, though it is the last report; a map entry makes the facing
/// not known again, and so does a reset. What the hunter weighs its own last
/// turning key against (WorldFix::facing, Hunter::kFacingTrustMs).
void test_own_facing() {
    section("Packet world: which way you face, and when a report last said so");

    PacketWorld w;
    const std::uint64_t t = kT0;
    w.on_packets({ from_server(entry_frame(11, 1, 4690, 1180), t) });
    PacketWorld::View v = w.view(t + 10);
    check(v.me.known && v.me.facing == 0 && v.me.said_facing == 0 && v.me.facing_ms == 0,
          "a map entry alone: you are known, the way you face is not, and nothing has said it");

    w.on_packets({ from_client(move_frame(4690.0f, 1180.0f, 0.0f, 1), t + 100) });
    v = w.view(t + 150);
    check(v.me.facing == 1 && v.me.said_facing == 1 && v.me.facing_ms == t + 100,
          "a 0x0027 facing right: right, as of its own time");

    w.on_packets({ from_client(cast_frame(4690, 1180), t + 300) });
    w.on_packets({ from_client(air_frame(1, 4690.0f, 1180.0f, 0.0f, -20.0f), t + 400) });
    w.on_packets({ from_client(air_frame(0, 4690.0f, 1180.0f, 0.0f, 0.0f), t + 1000) });
    w.on_packets({ from_client(cast_frame(4690, 1180), t + 1050) });
    v = w.view(t + 1100);
    check(v.me.facing == 1 && v.me.said_facing == 1 && v.me.facing_ms == t + 100 && v.me.report_ms == t + 1050 &&
              v.me.kind == OwnReport::Cast,
          "a cast, a jump, its landing and another cast: still right, as of the 0x0027 -- though the last report "
          "is the cast's");

    w.on_packets({ from_client(other_frame(4690.0f, 1180.0f, 0.0f, -1), t + 1200) });
    v = w.view(t + 1250);
    check(v.me.facing == -1 && v.me.said_facing == 1 && v.me.facing_ms == t + 100 &&
              v.me.kind == OwnReport::Other,
          "a 0x0029 facing left: followed as left, but what a 0x0027 said -- right, as of its time -- stands");

    w.on_packets({ from_client(move_frame(4690.0f, 1180.0f, 0.0f, 1), t + 1400) });
    w.on_packets({ from_client(here_frame(4690.0f, 1180.0f, 1000008), t + 1500) });
    v = w.view(t + 1550);
    check(v.me.facing == -1 && v.me.said_facing == 1 && v.me.facing_ms == t + 1400 &&
              v.me.kind == OwnReport::Here,
          "right again by a 0x0027, then a 0x0028 facing left: followed as left, said right as of the 0x0027");

    w.on_packets({ from_server(entry_frame(11, 2, 900, 600), t + 2000) });
    v = w.view(t + 2050);
    check(v.me.known && v.me.facing == 0 && v.me.said_facing == 0 && v.me.facing_ms == 0,
          "a map entry: the way you face not known again, nor when");

    w.on_packets({ from_client(move_frame(900.0f, 600.0f, 0.0f, 1), t + 2100) });
    const bool said = w.view(t + 2150).me.facing_ms == t + 2100;
    w.reset();
    v = w.view(t + 2200);
    check(said && !v.me.known && v.me.facing == 0 && v.me.said_facing == 0 && v.me.facing_ms == 0,
          "said again after it, then a reset: forgotten with the rest of you");
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
    w.on_packets({ from_server(event_frame(4, 9, 1, 2.6f, 1290, 900, 3044), t + 2000) });
    const PacketWorld::Mob walker = slot_of(w.view(t + 3000), 9);
    check(walker.x == 1290.0f && !walker.moving && w.stats().unknown_placed == 2,
          "a walker worked out at 1104, 1 s on: put at 1290 by state 4, 186 px on, and standing there");

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
    check(said.size() == 3 && of_state("5") == 2 && of_state("3") == 0 && of_state("4") == 1 &&
              said.front().find("slot 5 at 2450,1170") != std::string::npos &&
              said.front().find("its bytes from 8: ") != std::string::npos,
          "the first two frames of each such state written to the log, with their bytes -- not state 3's, a "
          "contact -- " + std::to_string(said.size()) + " line(s)");

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

/// The capture started again on the same game process and connections:
/// relink() keeps what the server said of the place, and the counts, and
/// forgets you and the monsters as every start of a capture always has --
/// the facing with the rest, so the hunter's turning sees nothing new.
void test_relink() {
    section("Packet world: relink() -- the capture started again on the same game");

    PacketWorld w;
    const std::uint64_t t = kT0;
    w.on_packets({ from_server(entry_frame(11, 1, 4690, 1180), t),
                   wrapped(table_frame({ { 3, 1004401, 44, 1, 1500, 900, 0.0f, 7000, 7676 } }), t + 300) });
    w.on_packets({ from_client(move_frame(4700.0f, 1180.0f, 9.0f, 1), t + 500) });
    // Another character's report, and one of yours cut short: both stamped.
    GamePacket cut = from_client(here_frame(4700.0f, 1180.0f, 8), t + 600);
    cut.payload.pop_back();
    w.on_packets({ cut, from_client(move_frame(100.0f, 100.0f, 0.0f, 1, kId + 1), t + 600) });
    const PacketWorld::Stats before = w.stats();
    const PacketWorld::View was = w.view(t + 700);
    check(was.me.known && was.me.said_facing == 1 && was.me.facing_ms == t + 500 && was.table_seen &&
              was.mob(3) != nullptr && before.last_own_ms == t + 500 && before.last_foreign_ms == t + 600 &&
              before.last_own_wrong_size_ms == t + 600 && before.foreign == 1,
          "before it: you known and facing right, the map's table and a monster, another character's report and "
          "a report of yours of the wrong size");

    const std::uint64_t r = t + 1000;
    w.relink(r);
    const PacketWorld::View v = w.view(r);
    const PacketWorld::Stats s = w.stats();
    check(v.map == 11 && v.area == 1 && v.map_epoch == was.map_epoch && v.entry_x == 4690.0f &&
              v.entry_y == 1180.0f && s.entry_map == 11 && s.entry_area == 1 && s.entry_ms == t &&
              v.map_since_ms == r,
          "relink() keeps the map, the area, the map epoch and the entry -- the place settled again from the "
          "relink");
    check(v.mobs.empty() && !v.table_seen && v.first_monster_ms == 0, "and forgets the monsters and the table");
    check(!v.me.known && v.me.id == 0 && v.me.said_facing == 0 && v.me.facing_ms == 0 && v.me.facing == 0,
          "and you, as after reset(): not known, no id, no facing");
    check(s.last_own_ms == 0 && s.last_foreign_ms == 0 && s.last_wrong_size_ms == 0 &&
              s.last_own_wrong_size_ms == 0,
          "and when the last report of yours, another's, and one of the wrong size came");
    check(s.map_entries == before.map_entries && s.tables == before.tables &&
              s.of(0x0027).used == before.of(0x0027).used && s.of(0x0028).wrong_size == 1 && s.foreign == 1,
          "the counts are kept, for the session's tallies");

    // What the packets may steer by, from the relink on.
    HunterConfig on;
    on.packet_positions = PacketPositions::On;
    const auto health = [&w, &on](std::uint64_t at) {
        PacketLink link;
        link.capturing = true;
        link.last_tcp_ms = at;
        return packet_health(w.view(at), w.stats(), link, on, at).why;
    };
    check(health(r + 100) == PacketWhy::NotMoved, "no report of yours since: not to be gone by (NotMoved)");
    w.on_packets({ from_client(move_frame(4720.0f, 1180.0f, 0.0f, -1, kId + 7), r + 200) });
    const PacketWorld::View back = w.view(r + 200);
    check(back.me.known && back.me.id == kId + 7 && back.me.said_facing == -1 && w.stats().foreign == 1,
          "a report under another id than before is yours after a relink, as after a map entry");
    check(health(r + 300) == PacketWhy::MapChanged &&
              health(r + PacketHealth::kSettleMs + 1) == PacketWhy::InUse,
          "then the place settles: MapChanged until kSettleMs after the relink, then the packets may steer");

    // The counts since a moment: what came after it; the times and the last
    // entry as they are.
    w.on_packets({ from_server(event_frame(0, 3, 1, 0.0f, 1500, 900, 7000), r + 400),
                   from_server(entry_frame(11, 2, 100, 200), r + 500) });
    const PacketWorld::Stats d = w.stats().since(before);
    check(d.stops == 1 && d.map_entries == 1 && d.tables == 0 && d.of(0x0027).used == 1 && d.foreign == 0 &&
              d.of(0x0028).wrong_size == 0 && d.entry_area == 2 && d.entry_ms == r + 500 &&
              d.last_own_ms == r + 200 && d.of(0x0027).opcode == 0x0027,
          "since(): the counts less those before -- one stop, one entry, one report -- the times and the last "
          "entry as they are now");
    PacketWorld::Stats five, two;
    five.stops = 5;
    two.stops = 2;
    check(five.since(two).stops == 3 && two.since(five).stops == 2,
          "a count lower than before was started again meanwhile (reset) and is taken from 0: 5 since 2 is 3, 2 "
          "since 5 is 2");
    check(s.since(PacketWorld::Stats{}).tables == s.tables && s.since(PacketWorld::Stats{}).map_entries == 1,
          "and since nothing at all is everything");

    // A reset() between the two -- the keeper's log back in mid-hunt -- and
    // more after it than there were before: all of what came since the reset,
    // not those less the counts from before it.
    PacketWorld again;
    for (std::uint16_t i = 0; i < 2; ++i) again.on_packets({ from_server(entry_frame(11, i, 100, 200), t + i) });
    const PacketWorld::Stats hunt_on = again.stats();
    again.reset();
    for (std::uint16_t i = 0; i < 3; ++i) again.on_packets({ from_server(entry_frame(12, i, 100, 200), t + 10 + i) });
    const PacketWorld::Stats after_reset = again.stats();
    check(hunt_on.map_entries == 2 && after_reset.map_entries == 3 && after_reset.resets == hunt_on.resets + 1 &&
              after_reset.since(hunt_on).map_entries == 3 && after_reset.since(hunt_on).entry_map == 12,
          "a reset() since: everything after it, 3 map entries -- not 3 less the 2 before it");
}

// ----------------------------------------------------- hurts, and who hits

// Ten 0x0038 frames of the live logs -- gobot_gui.log lines 60, 63 and 64 of
// 2026-11-02, gobot_gui.old.log lines 37, 178, 180, 183, 204, 205 and 207 --
// each as its line gave its bytes from 8: the payload. Unlike everything
// above, these are the game's own bytes. They hold no names, and are kept
// here because the logs rotate and lose them (the packets plan's Appendix).
//
// In payload offsets: the state at 4, the slot at 8, the facing at 16, the
// speed at 20, x, y at 24 and 26, the hitter at 40, the damage at 44, the HP
// at 56.
const char* const kLog60 =      // state 6, slot 1, hitter 0x2F7, damage 40413, HP 67668
    "00 00 00 00 06 00 00 00 01 00 00 00 00 00 00 00 01 fb 85 98 00 00 00 40 3c 00 3d 02 00 00 00 00 00 00 66 56 "
    "00 00 00 00 f7 02 00 00 dd 9d 00 00 00 ca a0 56 00 00 00 00 54 08 01 00 00 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 a8 00 41 02 06 00 00 00";
const char* const kLog63 =      // state 3, slot 1, 0x2F7, damage 0, HP 67668 unchanged
    "00 00 00 00 03 9e 94 ad 01 00 00 00 00 00 00 00 01 79 ff ff 00 00 00 00 9c 00 27 02 00 00 00 00 00 00 5c 56 "
    "00 00 00 00 f7 02 00 00 00 00 00 00 00 02 00 00 00 00 00 00 54 08 01 00 00 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 a8 00 41 02 06 00 00 00";
const char* const kLog64 =      // state 7, slot 1, 0x2F7, damage 24103, HP 43565
    "00 00 00 00 07 00 00 00 01 00 00 00 00 00 00 00 01 fb 85 98 00 00 00 40 9f 00 27 02 00 00 00 00 00 00 66 56 "
    "00 00 00 00 f7 02 00 00 27 5e 00 00 05 ca a0 56 00 00 00 00 2d aa 00 00 c8 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 e9 00 2e 02 06 00 00 00";
const char* const kOld178 =     // state 6, slot 12, hitter 0x6F, HP 696500
    "00 00 00 00 06 00 00 00 0c 00 00 00 00 00 00 00 01 10 76 98 00 00 00 40 f1 02 0f 01 00 00 00 00 00 00 6b 56 "
    "00 00 00 00 6f 00 00 00 78 ff 00 00 00 6a a6 56 00 00 00 00 b4 a0 0a 00 00 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 5f 03 de 00 07 01 00 00";
const char* const kOld180 =     // state 7, slot 12, 0x6F, damage 28695, HP 667805
    "00 00 00 00 07 00 00 00 0c 00 00 00 00 00 00 00 01 10 76 98 00 00 00 40 0f 03 10 01 00 00 00 00 00 00 6b 56 "
    "00 00 00 00 6f 00 00 00 17 70 00 00 00 6a a6 56 00 00 00 00 9d 30 0a 00 00 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 3f 03 c7 00 07 00 00 00";
const char* const kOld183 =     // state 7, slot 12, 0x6F, damage 27842, HP 639963
    "00 00 00 00 07 00 00 00 0c 00 00 00 00 00 00 00 01 10 76 98 00 00 00 40 3c 03 12 01 00 00 00 00 00 00 6b 56 "
    "00 00 00 00 6f 00 00 00 c2 6c 00 00 00 6a a6 56 00 00 00 00 db c3 09 00 00 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 40 03 c6 00 07 00 00 00";
const char* const kOld204 =     // state 6, slot 20, hitter 0x6F, HP 210579
    "00 00 00 00 06 00 00 00 14 00 00 00 00 00 00 00 01 25 76 98 00 00 00 40 47 03 12 01 00 00 00 00 00 00 6b 56 "
    "00 00 00 00 6f 00 00 00 f8 8e 00 00 05 6a a6 56 00 00 00 00 93 36 03 00 bf 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 28 03 ec 00 07 01 00 00";
const char* const kOld205 =     // state 6, slot 20, 0x6F, damage 24663, HP 185916
    "00 00 00 00 06 00 00 00 14 00 00 00 00 00 00 00 01 25 76 98 00 00 00 40 41 03 12 01 00 00 00 00 00 00 6b 56 "
    "00 00 00 00 6f 00 00 00 57 60 00 00 05 6a a6 56 00 00 00 00 3c d6 02 00 c8 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 72 03 ec 00 07 00 00 00";
const char* const kOld207 =     // state 7, slot 20, 0x6F, damage 36705, HP 149211
    "00 00 00 00 07 00 00 00 14 00 00 00 00 00 00 00 01 25 76 98 00 00 00 40 3b 03 11 01 00 00 00 00 00 00 6b 56 "
    "00 00 00 00 6f 00 00 00 61 8f 00 00 04 6a a6 56 00 00 00 00 db 46 02 00 c8 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 6f 03 e0 00 07 00 00 00";
const char* const kOld37 =      // state 7, slot 21, another player's 0x55C, damage 52372, HP 426870
    "00 00 00 00 07 00 00 00 15 00 00 00 00 00 00 00 01 27 76 98 00 00 00 40 64 06 52 03 00 00 00 00 00 00 6b 56 "
    "00 00 00 00 5c 05 00 00 94 cc 00 00 01 6a a6 56 00 00 00 00 76 83 06 00 bf 00 00 00 00 00 00 00 00 00 00 00 "
    "00 00 00 00 00 00 00 00 4b 06 52 03 07 00 00 00";

/// One of them as the server sent it at `t`, straight off the TCP stream.
GamePacket recorded(const char* bytes, std::uint64_t t) {
    GamePacket p;
    p.opcode = 0x0038;
    p.from_server = true;
    p.timestamp_ms = t;
    for (const char* at = bytes; *at != '\0';) {
        if (*at == ' ') {
            ++at;
            continue;
        }
        char* end = nullptr;
        const unsigned long b = std::strtoul(at, &end, 16);
        if (end == at) break;
        p.payload.push_back(static_cast<std::uint8_t>(b));
        at = end;
    }
    return p;
}

/// 0x0038 of today's server, by hand: state 6 or 7 a hurt by `actor` of
/// `damage`, leaving `hp`; state 3 or 11 a contact, damage 0.
Built hurt_frame(std::uint8_t state, std::uint32_t slot, std::uint16_t x, std::uint16_t y, std::uint32_t actor,
                 std::uint32_t damage, std::int32_t hp) {
    Built f(0x0038, 96);
    f.u8(12, state); f.u32(16, slot);
    f.u8(24, 0x01); f.f32(28, 2.0f);
    f.u16(32, x); f.u16(34, y);
    f.u32(48, actor); f.u32(52, damage); f.i32(64, hp);
    return f;
}

/// What is said at Info while `run` runs: the lines with `text` in them.
int said_while(const std::function<void()>& run, const char* text) {
    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_level(LogLevel::Info);
    log.set_ring_capacity(400);
    log.clear_ring();
    run();
    int n = 0;
    for (const LogLine& line : log.ring())
        if (line.text.find(text) != std::string::npos) ++n;
    log.set_level(was);
    log.set_ring_capacity(0);
    return n;
}

/// The live logs' hurts, read and chained through the world: the HP before
/// less the damage is the HP after, slot by slot.
void test_recorded_hurts() {
    section("Packet world: the hurts of today's server, from the live logs (states 6 and 7)");

    const auto h60 = read_monster_state(recorded(kLog60, kT0));
    const auto h64 = read_monster_state(recorded(kLog64, kT0));
    const auto c63 = read_monster_contact(recorded(kLog63, kT0));
    check(recorded(kLog60, kT0).payload.size() == 88 && h60 && h60->event == MonsterEvent::Hurt && h60->state == 6 &&
              h60->slot == 1 && h60->attacker == 0x2F7u && h60->damage == 40413 && h60->hp == 67668 &&
              h60->x == 60 && h60->y == 573 && h60->facing == 1,
          "log:60, state 6 of 88 bytes: slot 1, hit by 0x2F7 for 40413, 67668 left, at 60,573");
    check(h64 && h64->event == MonsterEvent::Hurt && h64->state == 7 && h64->damage == 24103 && h64->hp == 43565 &&
              h60 && h60->hp - static_cast<std::int32_t>(h64->damage) == h64->hp,
          "log:64, state 7: 24103 off log:60's 67668 leaves 43565 -- the HP before less the damage is the HP after");
    check(c63 && c63->state == 3 && c63->slot == 1 && c63->actor == 0x2F7u && c63->hp == 67668 &&
              !read_monster_state(recorded(kLog63, kT0)),
          "log:63, state 3: slot 1 and 0x2F7, the HP unchanged -- a contact, and no hurt");

    // The three chains through the world, with nothing else known of it.
    PacketWorld w;
    const std::uint64_t t = kT0;
    w.on_packets({ recorded(kLog60, t) });
    const PacketWorld::Mob first = slot_of(w.view(t), 1);
    check(first.slot == 1 && first.x == 60.0f && first.y == 573.0f && first.hp == 67668 &&
              first.max_hp == 67668 + 40413 && !first.dead && !first.moving && first.gen != 0,
          "a hurt for a slot no frame had named: the monster put where it says, standing, with the HP left and, "
          "for max HP, what it had before");
    w.on_packets({ recorded(kLog63, t + 100) });
    w.on_packets({ recorded(kLog64, t + 200) });
    w.on_packets({ recorded(kOld178, t + 300) });
    w.on_packets({ recorded(kOld180, t + 400) });
    w.on_packets({ recorded(kOld183, t + 500) });
    w.on_packets({ recorded(kOld204, t + 600) });
    w.on_packets({ recorded(kOld205, t + 700) });
    w.on_packets({ recorded(kOld207, t + 800) });
    const PacketWorld::View v = w.view(t + 900);
    const PacketWorld::Stats s = w.stats();
    check(s.hurts == 8 && s.hurts_chained == 5 && s.hurts_new_slot == 3 && s.hurts_far == 0 && s.hurts_rose == 0 &&
              s.hurts_on_dead == 0 && s.of(0x0038).used == 9 && s.of(0x0038).refused == 0,
          "eight hurts on three slots, the first of each a slot new to it, and the other five chained -- " +
              std::to_string(s.hurts_chained) + " chained");
    const PacketWorld::Mob one = slot_of(v, 1), twelve = slot_of(v, 12), twenty = slot_of(v, 20);
    check(one.hp == 43565 && one.x == 159.0f && twelve.hp == 639963 && twelve.max_hp == 696500 + 65400 &&
              twelve.x == 828.0f && twenty.hp == 149211 && twenty.max_hp == 210579 + 36600 && twenty.x == 827.0f &&
              !one.dead && !twelve.dead && !twenty.dead,
          "each slot with the HP its last hurt left, where that hurt put it -- slot 12 at 639963 of 761900");
    check(s.unknown_states == 1 && s.contacts == 1 && one.contact_actor == 0x2F7 && one.contact_ms == t + 100,
          "states 6 and 7 are no unknown states now; state 3 still is, and counted as a contact, kept on its "
          "monster");
    check(s.hurts_yours == 0 && s.hurts_others == 0 && s.contacts_yours == 0 && s.contacts_others == 0 &&
              one.hurt_ms == t + 200 && one.hurt_by_you_ms == 0 && one.hurt_by_other_ms == 0 && !v.actor_checked,
          "with no report of yours, whose a hurt is not said: neither yours nor another's");
    check(s.hitters[0].id == 0x2F7 && s.hitters[0].hits == 2 && s.hitters[1].id == 0x6F &&
              s.hitters[1].hits == 6 && s.hitters[2].hits == 0,
          "the hitters met, each with how many: 0x2F7 twice, 0x6F six times");
}

/// Whose a blow is, once your id is known from your own report: yours or
/// another player's, for the hurts, the kills and the contacts; and your id
/// said in the log once for each id.
void test_hurt_attribution() {
    section("Packet world: whose a hurt, a kill or a contact is, by your id");

    const std::uint64_t t = kT0;
    PacketWorld w;
    const int id_lines = said_while(
        [&] {
            w.on_packets({ from_client(move_frame(100.0f, 573.0f, 0.0f, 1, 759), t) });
            w.on_packets({ recorded(kLog60, t + 100) });
            w.on_packets({ recorded(kLog63, t + 150) });
            w.on_packets({ recorded(kLog64, t + 200) });
            w.on_packets({ recorded(kOld37, t + 300) });
            // Said once: not for the same id again, nor after a map entry
            // that lets the next report's id be learned anew.
            w.on_packets({ from_client(move_frame(100.0f, 573.0f, 0.0f, 1, 759), t + 400) });
            w.on_packets({ from_server(entry_frame(10, 4, 100, 573), t + 500) });
            w.on_packets({ from_client(move_frame(100.0f, 573.0f, 0.0f, 1, 759), t + 600) });
        },
        "Positions: your id is 759 (0x02F7), from your 0x0027");
    const PacketWorld::Stats s = w.stats();
    check(s.hurts_yours == 2 && s.hurts_others == 1 && s.contacts_yours == 1 && s.contacts_others == 0,
          "your id 759 from your 0x0027: log:60 and log:64 are yours, old:37 another player's, log:63 a contact "
          "of yours");
    check(id_lines == 1, "\"your id is 759 (0x02F7)\" said once -- " + std::to_string(id_lines) + " time(s)");

    {
        PacketWorld y;
        y.on_packets({ from_client(move_frame(100.0f, 573.0f, 0.0f, 1, 759), t) });
        y.on_packets({ recorded(kLog64, t + 100), recorded(kOld37, t + 200) });
        const PacketWorld::View v = y.view(t + 300);
        const PacketWorld::Mob mine = slot_of(v, 1), theirs = slot_of(v, 21);
        check(mine.hurt_by_you_ms == t + 100 && mine.hurt_by_other_ms == 0 && mine.last_actor == 759 &&
                  theirs.hurt_by_other_ms == t + 200 && theirs.hurt_by_you_ms == 0 && theirs.last_actor == 0x55C &&
                  theirs.x == 1636.0f && theirs.y == 850.0f,
              "each monster says when you last hurt it and when someone else did");
    }
    {
        // The kills split the same way, and before your id is known neither.
        PacketWorld k;
        k.on_packets({ from_server(event_frame(9, 3, 1, 7.0f, 500, 900, -5, 600), t) });
        k.on_packets({ from_client(move_frame(500.0f, 900.0f, 0.0f, 1), t + 100) });
        k.on_packets({ from_server(event_frame(0, 5, 1, 0.0f, 700, 900, 600), t + 200),
                       from_server(event_frame(9, 5, 1, 7.0f, 700, 900, -100, 700), t + 300) });
        k.on_packets({ from_server(event_frame(0, 6, 1, 0.0f, 900, 900, 600), t + 400),
                       from_server(hurt_frame(7, 6, 900, 900, kId + 50, 700, -100), t + 500) });
        const PacketWorld::Stats ks = k.stats();
        const PacketWorld::Mob six = slot_of(k.view(t + 600), 6);
        check(ks.kills == 3 && ks.kills_before_id == 1 && ks.kills_yours == 1 && ks.kills_others == 1 &&
                  six.dead && six.hp == 0 && six.dead_ms == t + 500,
              "a kill before your id is known, one of yours, and a hurt that leaves none by another player -- a "
              "kill as well, the monster dead");
    }
}

/// States 3 and 11 read as contacts are counted, kept on their monster and
/// placed as before, but never written to the log as states not known: 4
/// such lines a session, for 601 frames of yours in the farm of 2026-11-03.
/// A state truly not known still is, its first two frames.
void test_contacts_quiet() {
    section("Packet world: contacts (states 3 and 11) are not logged as states not known");

    const std::uint64_t t = kT0;
    PacketWorld w;
    std::vector<std::string> said;
    {
        Logger& log = Logger::instance();
        const LogLevel was = log.level();
        log.set_level(LogLevel::Info);
        log.set_ring_capacity(400);
        log.clear_ring();
        w.on_packets({ from_client(move_frame(2400.0f, 1180.0f, 0.0f, 1), t) });
        w.on_packets({ from_server(event_frame(0, 5, 1, 0.0f, 2400, 1180, 6000), t + 100) });
        for (std::uint64_t i = 0; i < 4; ++i)
            w.on_packets({ from_server(hurt_frame(3, 5, 2405, 1180, kId, 0, 6000), t + 200 + 200 * i),
                           from_server(hurt_frame(11, 5, 2410, 1180, kId, 0, 6000), t + 300 + 200 * i) });
        for (std::uint64_t i = 0; i < 3; ++i)
            w.on_packets({ from_server(event_frame(5, 5, 1, 0.0f, 2420, 1180, 6000), t + 1100 + 100 * i) });
        for (const LogLine& line : log.ring())
            if (line.text.find("Positions: a monster frame (0x0038) of state") != std::string::npos)
                said.push_back(line.text);
        log.set_level(was);
        log.set_ring_capacity(0);
    }
    const PacketWorld::Stats s = w.stats();
    const PacketWorld::Mob five = slot_of(w.view(t + 1500), 5);
    check(s.contacts == 8 && s.contacts_yours == 8 && s.unknown_states == 11 && five.contact_actor == kId &&
              five.contact_ms == t + 900,
          "8 contacts of yours counted as ever, the last kept on its monster; still among the 11 other states");
    int threes = 0, elevens = 0, fives = 0;
    for (const std::string& line : said) {
        if (line.find("of state 3,") != std::string::npos) ++threes;
        if (line.find("of state 11,") != std::string::npos) ++elevens;
        if (line.find("of state 5,") != std::string::npos) ++fives;
    }
    check(threes == 0 && elevens == 0 && fives == 2 && said.size() == 2,
          "none of the 8 contact frames written down as not known; state 5, not known, its first two -- " +
              std::to_string(said.size()) + " line(s)");
    int placed = 0;
    for (const PacketWorld::Stats::Kind& k : s.unknown_kinds) placed += static_cast<int>(k.placed);
    check(s.unknown_placed == 11 && placed == 11 && five.x == 2420.0f,
          "and each still puts the monster where it says, as any other state does");
}

/// What becomes of a hurt that is not where the monster was, of one on a
/// monster killed, and of one that leaves more HP than the last.
void test_hurt_rules() {
    section("Packet world: a hurt far off, on a dead monster, and a monster healing");

    const std::uint64_t t = kT0;
    PacketWorld w;
    w.on_packets({ from_server(event_frame(0, 30, 1, 0.0f, 1000, 900, 500), t) });
    w.on_packets({ from_server(hurt_frame(6, 30, 1050, 900, 777, 100, 400), t + 100) });
    PacketWorld::Mob m = slot_of(w.view(t + 100), 30);
    check(m.x == 1050.0f && m.hp == 400 && w.stats().hurts_chained == 1,
          "a hurt 50 px from where the monster stood: put there, 100 off its 500, chained");
    w.on_packets({ from_server(hurt_frame(6, 30, 1450, 900, 777, 100, 300), t + 200) });
    m = slot_of(w.view(t + 200), 30);
    check(m.x == 1050.0f && m.hp == 300 && w.stats().hurts_far == 1 && w.stats().hurts_chained == 2,
          "one 400 px off: its place counted far and passed over, its HP taken all the same");
    w.on_packets({ from_server(hurt_frame(7, 30, 1050, 900, 777, 0, 350), t + 300) });
    check(slot_of(w.view(t + 300), 30).hp == 350 && w.stats().hurts_rose == 1 && w.stats().hurts_chained == 2,
          "one that leaves more than the last: the monster healing -- rose, not chained");

    // A kill, then a hurt on the dead monster: passed over until a stop
    // brings one alive into the slot, a generation of its own.
    w.on_packets({ from_server(event_frame(9, 30, 1, 7.0f, 1050, 900, -10, 360), t + 400) });
    const std::uint32_t killed_gen = slot_of(w.view(t + 400), 30).gen;
    w.on_packets({ from_server(hurt_frame(6, 30, 1060, 900, 777, 10, 500), t + 500) });
    const PacketWorld::Mob still_dead = slot_of(w.view(t + 500), 30);
    check(still_dead.dead && still_dead.hp == 0 && still_dead.x == 1050.0f && w.stats().hurts_on_dead == 1 &&
              still_dead.gen == killed_gen,
          "a hurt on a monster killed: passed over, counted on dead -- it stays dead where it fell");
    w.on_packets({ from_server(event_frame(0, 30, 1, 0.0f, 1300, 900, 500), t + 2500) });
    const PacketWorld::Mob back = slot_of(w.view(t + 2500), 30);
    check(!back.dead && back.hp == 500 && back.gen != killed_gen && back.hurt_ms == 0 && back.last_actor == 0,
          "a stop with HP in the slot: alive again, a new generation, nobody's blows on it");
}

/// The generation: new at each spawn, table and dead slot alive again, and
/// never given twice, whatever starts again in between.
void test_generations() {
    section("Packet world: the generation of a slot's monster (Mob::gen)");

    const std::uint64_t t = kT0;
    PacketWorld w;
    w.on_packets({ from_server(event_frame(0, 5, 1, 0.0f, 2400, 1180, 600), t) });
    const std::uint32_t first = slot_of(w.view(t), 5).gen;
    w.on_packets({ from_server(event_frame(1, 5, 1, 2.6f, 2400, 1180, 600), t + 100) });
    const std::uint32_t walked = slot_of(w.view(t + 100), 5).gen;
    w.on_packets({ from_server(event_frame(9, 5, 1, 7.0f, 2410, 1180, -50, 650), t + 200) });
    w.on_packets({ from_server(spawn_frame(5, 2000301, -1, 3100, 1150, 2000), t + 3000) });
    const std::uint32_t spawned = slot_of(w.view(t + 3000), 5).gen;
    check(first != 0 && walked == first && spawned > first,
          "a slot first seen has a generation, a walk keeps it, and the spawn after its kill is the next one -- " +
              std::to_string(first) + ", " + std::to_string(walked) + ", " + std::to_string(spawned));
    w.reset();
    w.on_packets({ wrapped(table_frame({ { 5, 1003901, 39, 1, 900, 1145, 0.0f, 3044, 3044 } }), t + 4000) });
    check(slot_of(w.view(t + 4000), 5).gen > spawned,
          "after a reset, the table's monster in the same slot is a newer one still: never a number given twice");
}

/// Your id proven the hitter's after kActorCheckHits blows of yours: said
/// once, and from then on each view says so.
void test_actor_checked() {
    section("Packet world: your id proven the hitter's (actor_checked)");

    const std::uint64_t t = kT0;
    PacketWorld w;
    bool before = true;
    const int said = said_while(
        [&] {
            w.on_packets({ from_client(move_frame(1000.0f, 900.0f, 0.0f, 1), t) });
            w.on_packets({ from_server(event_frame(0, 8, 1, 0.0f, 1100, 900, 100000), t + 10) });
            std::int32_t hp = 100000;
            for (std::uint64_t i = 0; i < PacketWorld::kActorCheckHits; ++i) {
                if (i + 1 == PacketWorld::kActorCheckHits) before = w.view(t + 100 * i).actor_checked;
                hp -= 1000;
                const std::uint8_t state = static_cast<std::uint8_t>(i % 2 != 0 ? 7 : 6);
                w.on_packets({ from_server(hurt_frame(state, 8, 1100, 900, kId, 1000, hp), t + 100 * (i + 1)) });
            }
            w.on_packets({ from_server(hurt_frame(6, 8, 1100, 900, kId, 1000, hp - 1000), t + 5000) });
        },
        "seen as the hitter 20 times");
    const PacketWorld::Stats s = w.stats();
    check(!before && w.view(t + 5000).actor_checked && s.hurts_yours == 21 && s.hurts_chained == 21,
          "19 blows of yours: not yet; the 20th proves it");
    check(said == 1, "and that is said once in the log -- " + std::to_string(said) + " time(s)");
    w.reset();
    check(!w.view(t + 6000).actor_checked, "a reset -- another game, maybe another id -- proves it no longer");
}

/// The counts since a moment, for the Positions line from HUNT ON: the new
/// ones too, and the hitters by their ids.
void test_hurt_counts_since() {
    section("Packet world: the hurts, kills and hitters since a moment (Stats::since)");

    const std::uint64_t t = kT0;
    PacketWorld w;
    w.on_packets({ from_client(move_frame(100.0f, 573.0f, 0.0f, 1, 759), t) });
    w.on_packets({ recorded(kLog60, t + 100) });
    const PacketWorld::Stats before = w.stats();
    w.on_packets({ recorded(kLog63, t + 150), recorded(kLog64, t + 200), recorded(kOld37, t + 300) });
    const PacketWorld::Stats d = w.stats().since(before);
    check(d.hurts == 2 && d.hurts_chained == 1 && d.hurts_yours == 1 && d.hurts_others == 1 && d.contacts == 1 &&
              d.contacts_yours == 1 && d.hurts_new_slot == 1,
          "since(): the hurts after it -- log:64 chained and yours, old:37 another's in a slot new to it -- and "
          "log:63's contact");
    check(d.hitters[0].id == 759 && d.hitters[0].hits == 1 && d.hitters[1].id == 0x55C && d.hitters[1].hits == 1,
          "and each hitter's blows since, by its id: 759 once, 0x55C once");
}

/// Who says the map has changed (map_change_by): the packets, once a map
/// entry has been seen since the world started afresh and they are live on
/// the game and not just relinked; the minimap otherwise, as before.
void test_map_change_by() {
    section("Map changes: the packets' map entries, or the minimap (map_change_by)");

    const std::uint64_t now = kT0 + 60'000;
    PacketWorld::Place p;
    p.entries = 1;
    p.epoch = 1;
    p.since_ms = p.entry_ms = kT0;
    check(map_change_by(p, true, now - 500, now) == MapChangeBy::Packets,
          "an entry seen, on the game, the server heard half a second ago: the packets");
    PacketWorld::Place none = p;
    none.entries = 0;
    check(map_change_by(none, true, now - 500, now) == MapChangeBy::Minimap,
          "no entry since the world started afresh: the minimap");
    check(map_change_by(p, false, now - 500, now) == MapChangeBy::Minimap, "not on the game's process: the minimap");
    check(map_change_by(p, true, now - kMapByPacketsQuietMs, now) == MapChangeBy::Packets &&
              map_change_by(p, true, now - kMapByPacketsQuietMs - 1, now) == MapChangeBy::Minimap &&
              map_change_by(p, true, 0, now) == MapChangeBy::Minimap,
          "the server quiet 5 s: still the packets; a moment longer, or never heard: the minimap");
    PacketWorld::Place relinked = p;
    relinked.since_ms = now - kMapByPacketsSettleMs + 1;
    PacketWorld::Place settled = p;
    settled.since_ms = now - kMapByPacketsSettleMs;
    check(map_change_by(relinked, true, now - 500, now) == MapChangeBy::Minimap &&
              map_change_by(settled, true, now - 500, now) == MapChangeBy::Packets,
          "relinked under 10 s ago: the minimap; 10 s on: the packets");

    // The world's own place, asked as the GUI asks it.
    PacketWorld w;
    check(w.place().entries == 0 && w.place().epoch == 0, "nothing yet: no entry, epoch 0");
    w.on_packets({ from_server(entry_frame(10, 4, 690, 1145), kT0) });
    const PacketWorld::Place in = w.place();
    check(in.map == 10 && in.area == 4 && in.epoch == 1 && in.entries == 1 && in.since_ms == kT0 &&
              in.entry_ms == kT0 && map_change_by(in, true, kT0 + 1000, kT0 + 1000) == MapChangeBy::Packets,
          "an entry: its place, epoch 1, and the packets say");
    w.relink(kT0 + 2000);
    const PacketWorld::Place again = w.place();
    check(again.epoch == 1 && again.entries == 1 && again.since_ms == kT0 + 2000 &&
              map_change_by(again, true, kT0 + 3000, kT0 + 3000) == MapChangeBy::Minimap &&
              map_change_by(again, true, kT0 + 12'000, kT0 + 12'000) == MapChangeBy::Packets,
          "a relink: the minimap for 10 s, then the packets again");
    w.reset();
    check(map_change_by(w.place(), true, kT0 + 20'000, kT0 + 20'000) == MapChangeBy::Minimap &&
              w.place().epoch == 1,
          "a reset: the minimap until the next entry, the epoch kept");

#ifdef GOBOT_SOURCE_DIR
    const std::string src = read_all(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/gui_app.cpp");
    if (src.empty()) {
        skip("the source tree is not here, so gui_app.cpp is not read\n");
    } else {
        const std::string tick = body_of(src, "void GuiApp::tick() {");
        const std::size_t npos = std::string::npos;
        const std::size_t followed = tick.find("const bool packet_entered = packet_place.epoch != map_epoch_seen_;");
        const std::size_t running = tick.find("if (detector_running_) {");
        const std::size_t taken = tick.find("const bool fingerprint = minimap_.take_changed();");
        const std::size_t asked =
            tick.find("map_change_by(packet_place, on_game, packet_self_.link(at).last_server_ms, at)");
        const std::size_t under = tick.find("if (by_packets ? packet_entered : fingerprint) {");
        const std::size_t reset = under == npos ? npos : tick.find("camera_.reset();", under);
        const std::size_t anchor = under == npos ? npos : tick.find("anchor_.reset();", under);
        check(followed != npos && running != npos && followed < running && taken != npos && asked != npos &&
                  under != npos && taken < under && asked < under && reset != npos && reset - under < 80 &&
                  anchor != npos && anchor - under < 120 && tick.find("if (minimap_.take_changed()) {") == npos &&
                  tick.find("++minimap_overruled_;") != npos,
              "tick(): the minimap's reset sits under the packets' test -- an entry when they say, the fingerprint "
              "otherwise -- the entries followed at every tick, and a fingerprint they overrule counted");
        // One line a change: the tracker's own goes to the debug log, tick()
        // says the fingerprint once -- with "ignored" when the packets overrule it.
        const std::string tracker = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/minimap_tracker.cpp");
        const std::size_t ignored = tick.find("-- ignored");
        const std::size_t ignored_from = ignored == npos ? npos : tick.rfind("GB_INFO(", ignored);
        const std::size_t acts = tick.find("if (!by_packets && fingerprint)");
        check(!tracker.empty() && tracker.find("GB_INFO(\"Map changed") == npos &&
                  tracker.find("GB_DEBUG(\"Map changed") != npos && ignored_from != npos &&
                  tick.find("%016llx", ignored_from) < ignored && acts != npos && under != npos && acts < under,
              "one \"Map changed -- minimap fingerprint\" line a change: the tracker's at debug, tick()'s with the "
              "fingerprint, and \"ignored\" on it when the packets overrule");
    }
#endif
}

} // namespace

void test_packet_world(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_nearest_first();
    test_map_entry();
    test_own_walk();
    test_own_cast();
    test_own_facing();
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
    test_relink();
    test_recorded_hurts();
    test_hurt_attribution();
    test_contacts_quiet();
    test_hurt_rules();
    test_generations();
    test_actor_checked();
    test_hurt_counts_since();
    test_map_change_by();
}
