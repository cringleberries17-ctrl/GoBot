// gobot_test -- your casts and buffs from the packets, paired with the
// macro's presses (gobot/cast_match.hpp; the packets plan, items F and H, log
// only); and, read off GoBot's source, where its window wires them in, the
// keeper's hints (item I) and the jump test by the packets (item M). The jump
// test's own checks with take-offs are in jump_test.cpp, beside the others.
//
// Nothing here goes near the game, the network or the desktop. Every frame is
// built by hand at the frame bytes docs/protocol-findings.md gives, as
// packet_world_test.cpp builds its own, and stamped on a clock the test winds
// on. Skills 51102 and 13104 and the value 72,000 are the P0-2 recording's;
// every other number is made up.

#include "gobot/cast_match.hpp"
#include "gobot/common.hpp"
#include "gobot/macro.hpp"
#include "gobot/packet_world.hpp"
#include "gobot/self_frames.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

constexpr std::uint64_t kT0 = 9'000'000;     // where the test's clock starts
constexpr std::uint32_t kId = 4321;          // your character's id: made up
constexpr std::uint32_t kKey2 = '2';         // a skill key
constexpr std::uint32_t kKey3 = '3';         // another
constexpr std::uint32_t kKey5 = '5';         // a buff key
constexpr std::uint32_t kKey6 = '6';         // another

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
GamePacket client_udp(const Built& f, std::uint64_t t) {
    GamePacket p;
    p.opcode = f.opcode;
    p.payload = f.body;
    p.from_server = false;
    p.udp = true;
    p.timestamp_ms = t;
    return p;
}

/// As the server sent it at `t` over TCP, `depth` wrappers deep (1: unpacked
/// from an 0x0081).
GamePacket server(const Built& f, std::uint64_t t, std::uint8_t depth) {
    GamePacket p;
    p.opcode = f.opcode;
    p.payload = f.body;
    p.from_server = true;
    p.depth = depth;
    p.timestamp_ms = t;
    return p;
}

/// 0x0031: a cast of `skill` of `kind` (1 an attack, 2 a buff) from x, y,
/// naming the monster in `slot` when it is not -1.
Built cast_frame(std::uint32_t skill, std::uint8_t kind, std::uint16_t x = 1000, std::uint16_t y = 500,
                 int slot = -1, std::uint32_t counter = 0) {
    Built f(0x0031, 92);
    f.u32(8, kId); f.u32(12, kId);
    f.u16(16, x); f.u16(18, y); f.u32(20, skill);
    f.u8(24, kind);
    if (slot >= 0) {
        f.u8(26, 2);
        f.u16(28, static_cast<std::uint16_t>(slot));
    } else {
        f.u16(28, 0xFFFF);
    }
    f.u32(88, counter);
    return f;
}

/// 0x0077: a skill and a value.
Built time_frame(std::uint32_t skill, std::uint32_t value) {
    Built f(0x0077, 24);
    f.u32(12, skill); f.u32(16, value);
    return f;
}

/// 0x005A: a skill, its change and the value it left.
Built stat_frame(std::uint32_t skill, std::int32_t change, std::uint32_t value) {
    Built f(0x005A, 32);
    f.u32(12, skill); f.i32(20, change); f.u32(24, value);
    return f;
}

/// 0x002E, which follows a buff cast.
Built follow_frame() {
    Built f(0x002E, 72);
    f.u32(8, kId); f.u32(12, kId);
    return f;
}

/// 0x0038: the monster in `slot` starts a walk from x, y along `facing` at
/// `speed` steps of 25 ms.
Built walk_frame(std::uint32_t slot, int facing, float speed, std::uint16_t x, std::uint16_t y) {
    Built f(0x0038, 96);
    f.u8(12, 1); f.u32(16, slot);
    f.u8(24, static_cast<std::uint8_t>(facing)); f.f32(28, speed);
    f.u16(32, x); f.u16(34, y);
    f.u32(48, 0xFFFFFFFFu); f.i32(64, 5000);
    return f;
}

CastFrame cast_of(std::uint32_t skill, std::uint8_t kind) {
    return *read_cast(client_udp(cast_frame(skill, kind), kT0));
}

CastMatch::Press press_of(std::uint32_t vk, std::uint64_t at) {
    CastMatch::Press p;
    p.vk = vk;
    p.at_ms = at;
    return p;
}

const CastMatch::Key* key_in(const std::vector<CastMatch::Key>& keys, std::uint32_t vk) {
    for (const CastMatch::Key& k : keys)
        if (k.vk == vk) return &k;
    return nullptr;
}

// ------------------------------------------------------------ the readers

void test_cast_readers() {
    section("Casts and buffs from the packets: the frames read (0x0031, 0x0077, 0x005A, 0x002E)");

    const auto c = read_cast(client_udp(cast_frame(51102, 1, 1200, 640, 7, 88), kT0));
    check(c && c->id == kId && c->x == 1200 && c->y == 640 && c->skill == 51102 && c->kind == 1 && c->named &&
              c->slot == 7 && c->counter == 88,
          "0x0031: your id at 12, x, y u16 at 16 and 18, the skill at 20, its kind at 24, byte 26 2 with the "
          "target's slot at 28, the counter at 88");
    const auto plain = read_cast(client_udp(cast_frame(13104, 2), kT0));
    check(plain && !plain->named && plain->slot == 0xFFFF && plain->kind == 2,
          "a cast naming no target: no slot, and a buff's kind 2");

    const auto refused = [](GamePacket p) {
        FrameFit fit = FrameFit::Taken;
        const bool none = !read_cast(p, &fit);
        return std::make_pair(none, fit);
    };
    GamePacket from_server = client_udp(cast_frame(51102, 1), kT0);
    from_server.from_server = true;
    GamePacket over_tcp = client_udp(cast_frame(51102, 1), kT0);
    over_tcp.udp = false;
    GamePacket wrapped = client_udp(cast_frame(51102, 1), kT0);
    wrapped.depth = 1;
    GamePacket shorter = client_udp(cast_frame(51102, 1), kT0);
    shorter.payload.pop_back();
    GamePacket other = client_udp(cast_frame(51102, 1), kT0);
    other.opcode = 0x0027;
    const auto s = refused(from_server), t = refused(over_tcp), w = refused(wrapped), z = refused(shorter),
               o = refused(other);
    check(s.first && s.second == FrameFit::WrongSide && t.first && t.second == FrameFit::WrongSide && w.first &&
              w.second == FrameFit::WrongSide && z.first && z.second == FrameFit::WrongSize && o.first &&
              o.second == FrameFit::OtherOpcode,
          "never one from the server, over TCP or from inside a wrapper (the other side), nor one of 91 bytes "
          "(the wrong size): each refusal says why");

    const auto timed = read_buff_time(server(time_frame(13104, 72000), kT0, 0));
    FrameFit fit = FrameFit::Taken;
    const bool time_wrapped = !read_buff_time(server(time_frame(13104, 72000), kT0, 1), &fit) &&
                              fit == FrameFit::WrongSide;
    GamePacket time_long = server(time_frame(13104, 72000), kT0, 0);
    time_long.payload.push_back(0);
    const bool time_size = !read_buff_time(time_long, &fit) && fit == FrameFit::WrongSize;
    check(timed && timed->skill == 13104 && timed->value == 72000 && time_wrapped && time_size,
          "0x0077, plain from the server, 24 bytes: the skill at 12 and the value at 16 -- never from inside a "
          "wrapper, nor of 25 bytes");

    const auto changed = read_buff_stat(server(stat_frame(13104, -1002, 2150), kT0, 1));
    const bool stat_plain = !read_buff_stat(server(stat_frame(13104, -1002, 2150), kT0, 0), &fit) &&
                            fit == FrameFit::WrongSide;
    check(changed && changed->skill == 13104 && changed->change == -1002 && changed->value == 2150 && stat_plain,
          "0x005A inside 0x0081, 32 bytes: the skill at 12, the change as a signed number at 20, the value at 24 "
          "-- never a plain one");

    GamePacket follow_server = client_udp(follow_frame(), kT0);
    follow_server.from_server = true;
    GamePacket follow_short = client_udp(follow_frame(), kT0);
    follow_short.payload.pop_back();
    check(buff_follow_fit(client_udp(follow_frame(), kT0)) == FrameFit::Taken &&
              buff_follow_fit(follow_server) == FrameFit::WrongSide &&
              buff_follow_fit(follow_short) == FrameFit::WrongSize,
          "0x002E from your client over UDP, 72 bytes: counted, never read");
}

// ------------------------------------------------------------ the pairing

void test_cast_pairing() {
    section("Casts paired with the macro's presses: the window, learning a key's skill, unpaired casts");

    {
        CastMatch m;
        m.add(kT0 + 300, cast_of(51102, 1));
        m.pair({ press_of(kKey2, kT0) }, {}, kT0 + 500);
        const bool waited = m.casts()[0].vk == 0 && m.keys().empty();
        m.pair({ press_of(kKey2, kT0) }, {}, kT0 + 1100);
        const std::vector<CastMatch::Cast> casts = m.casts();
        const std::vector<CastMatch::Key> keys = m.keys();
        check(waited && casts.size() == 1 && casts[0].vk == kKey2 && casts[0].press_ms == kT0 && keys.size() == 1 &&
                  keys[0].pressed_all == 1 && keys[0].cast_all == 1 && m.learned(kKey2) == 0,
              "a press is paired only kSettleMs after it, its cast then come for sure: key 2's press with the "
              "cast 300 ms later; one pair learns nothing");
        // The same press handed again: paired once only.
        m.pair({ press_of(kKey2, kT0) }, {}, kT0 + 3000);
        check(m.keys()[0].pressed_all == 1, "a press already paired is not counted again");
    }

    const auto paired_at = [](std::int64_t offset) {
        CastMatch m;
        m.add(static_cast<std::uint64_t>(static_cast<std::int64_t>(kT0) + offset), cast_of(51102, 1));
        m.pair({ press_of(kKey2, kT0) }, {}, kT0 + 5000);
        return m.casts()[0].vk == kKey2;
    };
    check(paired_at(700) && !paired_at(701) && paired_at(-150) && paired_at(-200) && !paired_at(-250),
          "the window: a cast up to 700 ms after the press, or up to 200 ms before it -- slack for the stamps -- and "
          "no further");

    {
        // Two presses, two casts: each the first cast not yet taken.
        CastMatch m;
        m.add(kT0 + 100, cast_of(51102, 1));
        m.add(kT0 + 500, cast_of(51101, 1));
        m.pair({ press_of(kKey2, kT0), press_of(kKey3, kT0 + 400) }, {}, kT0 + 5000);
        const std::vector<CastMatch::Cast> casts = m.casts();
        check(casts.size() == 2 && casts[0].vk == kKey2 && casts[1].vk == kKey3,
              "two presses 400 ms apart, two casts: the earlier press takes the first, the later the second");
    }

    {
        // A potion pressed just before a skill. A drink sends no cast, so the
        // potion's presses are never handed over (GuiApp::more_tick): handed,
        // the older press would take the skill's cast.
        const std::uint32_t kPotion = 'G';
        CastMatch all, skills_only;
        all.add(kT0 + 150, cast_of(51102, 1));
        skills_only.add(kT0 + 150, cast_of(51102, 1));
        all.pair({ press_of(kPotion, kT0), press_of(kKey2, kT0 + 120) }, {}, kT0 + 5000);
        skills_only.pair({ press_of(kKey2, kT0 + 120) }, {}, kT0 + 5000);
        const std::vector<CastMatch::Key> mixed = all.keys(), kept = skills_only.keys();
        const CastMatch::Key* took = key_in(mixed, kPotion);
        const CastMatch::Key* lost = key_in(mixed, kKey2);
        const CastMatch::Key* own = key_in(kept, kKey2);
        check(took && took->cast_all == 1 && lost && lost->cast_all == 0 && own && own->cast_all == 1 &&
                  key_in(kept, kPotion) == nullptr && skills_only.casts()[0].vk == kKey2,
              "a potion pressed 120 ms before a skill would take the skill's cast; its press left out, the skill "
              "key keeps its own cast, and the potion key is never listed");
    }

    {
        // Learning: kLearnPairs pairs and at least kLearnPct% for one skill.
        CastMatch m;
        std::vector<CastMatch::Press> presses;
        const auto one = [&](int i, std::uint32_t skill) {
            const std::uint64_t at = kT0 + static_cast<std::uint64_t>(i) * 2000;
            presses.push_back(press_of(kKey2, at));
            m.add(at + 120, cast_of(skill, 1));
            m.pair(presses, {}, at + 1500);
        };
        for (int i = 0; i < 4; ++i) one(i, 51102);
        const bool four = m.learned(kKey2) == 0;
        one(4, 51102);
        const bool five = m.learned(kKey2) == 51102;
        one(5, 51101);
        const std::uint32_t six = m.learned(kKey2);
        one(6, 51101);
        const std::uint32_t seven = m.learned(kKey2);
        const std::vector<CastMatch::Key> keys = m.keys();
        const CastMatch::Key* k = key_in(keys, kKey2);
        check(four && five && six == 51102 && seven == 0 && k != nullptr && k->top_skill == 51102 &&
                  k->top_votes == 5 && k->pairs == 7,
              "key 2: nothing learned after 4 pairs, 51102 after 5 (5/5); kept at 5 of 6 (83%), dropped at 5 of 7 "
              "(71%)");
    }

    {
        // The kind: a buff key should cast kind 2, a skill key kind 1.
        CastMatch m;
        m.add(kT0 + 100, cast_of(13104, 2));
        m.add(kT0 + 3100, cast_of(51102, 1));
        m.add(kT0 + 6100, cast_of(13111, 2));
        m.pair({ press_of(kKey5, kT0), press_of(kKey6, kT0 + 3000), press_of(kKey2, kT0 + 6000) }, { kKey5, kKey6 },
               kT0 + 9000);
        const std::vector<CastMatch::Key> keys = m.keys();
        const CastMatch::Key* buff = key_in(keys, kKey5);
        const CastMatch::Key* buff_attacking = key_in(keys, kKey6);
        const CastMatch::Key* skill_buffing = key_in(keys, kKey2);
        check(buff && buff->buff && !buff->kind_mismatch() && buff_attacking && buff_attacking->buff &&
                  buff_attacking->kind_mismatch() && skill_buffing && !skill_buffing->buff &&
                  skill_buffing->kind_mismatch(),
              "a buff key casting kind 2 is as it should be; a buff key casting kind 1, or a skill key casting kind "
              "2, is said");
        const std::string line = casts_line(m.take_minute());
        check(has(line, "Casts (packets): 3 this minute (attacks 1, buffs 2, named 0)") &&
                  has(line, "key 5 (buff): 1 pressed, 1 cast -> not learned yet (13104 1/1), kind 2") &&
                  has(line, "key 6 (buff): 1 pressed, 1 cast -> not learned yet (51102 1/1), kinds: attack 1, buff "
                            "0, other 0 -- a buff key's casts should be kind 2") &&
                  has(line, "key 2: 1 pressed, 1 cast -> not learned yet (13111 1/1), kind 2 (a buff's) 1 times"),
              "and the Casts line says each: " + line);
    }

    {
        // Casts no press of the macro's made: the hunter's attack, a key
        // pressed by hand.
        CastMatch m;
        for (int i = 0; i < 3; ++i) m.add(kT0 + static_cast<std::uint64_t>(i) * 1000, cast_of(51102, 1));
        m.add(kT0 + 10'000, cast_of(51102, 1));
        m.pair({ press_of(kKey2, kT0 + 9'900) }, {}, kT0 + 9'500);
        const std::uint64_t early = m.take_minute().unpaired;
        m.pair({ press_of(kKey2, kT0 + 9'900) }, {}, kT0 + 12'000);
        const CastMatch::Minute minute = m.take_minute();
        const std::string line = casts_line(minute);
        check(early == 3 && minute.unpaired == 0 && minute.casts == 0 && has(line, "no macro key 0") &&
                  has(line, "key 2: 1 pressed, 1 cast"),
              "three casts with no press: counted as unpaired once no press can take them any more, the fourth "
              "paired with its press -- " + line);
        const CastMatch::Minute quiet = m.take_minute();
        check(casts_line(quiet).empty() && quiet.casts == 0 && key_in(quiet.keys, kKey2) &&
                  key_in(quiet.keys, kKey2)->pressed == 0 && key_in(quiet.keys, kKey2)->pressed_all == 1,
              "a minute with nothing cast or pressed writes no line; the counts start again, the totals stay");
    }
}

// ------------------------------------------------------------ the reach

void test_cast_reach() {
    section("A named cast's target: how far it was, where the packet world had it at the cast's own time");

    PacketWorld world;
    CastMatch m;
    // Slot 7 walks toward larger x from 2000 at 5 steps: 200 px a second.
    world.on_packets({ server(walk_frame(7, 1, 5.0f, 2000, 800), kT0, 0) });
    // A second later you cast at it from 1900, 780 -- and at slot 9, which the
    // world has never seen.
    const std::vector<GamePacket> batch{ client_udp(cast_frame(51102, 1, 1900, 780, 7), kT0 + 1000),
                                         client_udp(cast_frame(51102, 1, 1900, 780, 9), kT0 + 1100) };
    world.on_packets(batch);
    m.on_packets(batch, world);
    const std::vector<CastMatch::Cast> casts = m.casts();
    check(casts.size() == 2 && casts[0].reach_known && casts[0].dx == 300 && casts[0].dy == 20 &&
              !casts[1].reach_known,
          "the walker 200 px on at the cast's time: 300 px away in x, 20 in y -- not 700, where it is two seconds "
          "later; a slot the world does not have: not placed");
    const CastMatch::Minute minute = m.take_minute();
    check(minute.named == 2 && minute.named_unplaced == 1 && minute.reach.size() == 1 &&
              minute.reach[0].skill == 51102 && minute.reach[0].most_dx == 300 && minute.reach[0].most_dy == 20 &&
              has(casts_line(minute), "reach seen 51102: |dx| <= 300, |dy| <= 20 (1)"),
          "the farthest seen per skill, for the Casts line: " + casts_line(minute));

    // Frames of other kinds and of your casts' wrong size: passed over, counted.
    GamePacket shorter = client_udp(cast_frame(51102, 1), kT0 + 2000);
    shorter.payload.pop_back();
    m.on_packets({ shorter, server(time_frame(13104, 72000), kT0 + 2000, 0) }, world);
    const CastMatch::Minute after = m.take_minute();
    check(after.casts == 0 && after.wrong_size == 1, "a 0x0031 of 91 bytes is counted, and no cast");
}

// ------------------------------------------------------------ the buffs

void test_buff_watch() {
    section("Buffs from the packets: 0x0077 and 0x005A beside the buff key learned for their skill");

    PacketWorld world;
    CastMatch casts;
    BuffWatch buffs;
    std::vector<CastMatch::Press> presses;
    // Key 5 casts 13104 five times, 10 s apart; each cast followed by a
    // 0x002E, a 0x0077 400 ms after the press and a 0x005A.
    for (int i = 0; i < 5; ++i) {
        const std::uint64_t at = kT0 + static_cast<std::uint64_t>(i) * 10'000;
        presses.push_back(press_of(kKey5, at));
        const std::vector<GamePacket> batch{ client_udp(cast_frame(13104, 2), at + 150),
                                             client_udp(follow_frame(), at + 300),
                                             server(time_frame(13104, 72000), at + 400, 0),
                                             server(stat_frame(13104, 120, 1520), at + 450, 1) };
        casts.on_packets(batch, world);
        buffs.on_packets(batch);
    }
    // And one 0x002E long after any buff cast, and a 0x0077 for a skill no
    // key casts.
    buffs.on_packets({ client_udp(follow_frame(), kT0 + 48'000),
                       server(time_frame(13111, 72000), kT0 + 48'500, 0) });
    casts.pair(presses, { kKey5 }, kT0 + 60'000);

    const CastMatch::Minute cm = casts.take_minute();
    const BuffWatch::Minute bm = buffs.take_minute();
    const std::vector<BuffSeen> seen = pair_buffs(cm.keys, bm);
    check(bm.buff_casts == 5 && bm.follows == 6 && bm.follows_after == 5,
          "five buff casts, each followed by a 0x002E within 2 s; one more 0x002E long after: not after a buff");
    check(seen.size() == 2 && seen[0].vk == kKey5 && seen[0].skill == 13104 && seen[0].timed &&
              seen[0].time_value == 72000 && seen[0].after_press_ms == 400 && seen[0].stat && seen[0].change == 120 &&
              seen[0].value == 1520 && seen[1].vk == 0 && seen[1].skill == 13111 && seen[1].after_press_ms == -1,
          "13104 paired with key 5, learned through its casts: its 0x0077 72000 400 ms after the key's press, and "
          "its 0x005A; 13111 with no key learned for it");
    const std::string line = buffs_line(seen, bm, { { kKey5, 60 } });
    check(has(line, "Buffs (packets): key 5 -> 13104: 0x0077 72000 0.4 s after the press; 0x005A +120 -> 1520; "
                    "macro interval 60 s; ") &&
              has(line, "skill 13111 (no key learned for it): 0x0077 72000; ") &&
              has(line, "buff casts 5, 0x002E 6 (5 after a buff cast)"),
          "the Buffs line: " + line);
    check(has(casts_line(cm), "key 5 (buff): 5 pressed, 5 cast -> skill 13104 (5/5), kind 2"),
          "and the Casts line has key 5 learned: " + casts_line(cm));

    // A change downward, and a minute with nothing new.
    buffs.on_packets({ server(stat_frame(13104, -1002, 2150), kT0 + 70'000, 1) });
    const BuffWatch::Minute next = buffs.take_minute();
    const std::string down = buffs_line(pair_buffs(cm.keys, next), next, {});
    const BuffWatch::Minute quiet = buffs.take_minute();
    check(has(down, "key 5 -> 13104: 0x005A -1002 -> 2150") && pair_buffs(cm.keys, quiet).empty() &&
              buffs_line(pair_buffs(cm.keys, quiet), quiet, {}).empty() && quiet.skills.size() == 2 &&
              quiet.skills[0].value == 2150,
          "a 0x005A alone, its change signed: " + down + "; a minute with nothing writes no line, and the last "
          "values stay");
}

// ------------------------------------------- a buff key's bursts (2026.11.04d)

void test_buff_bursts() {
    section("A buff key's burst of presses: a kind-2 cast with it or none, whichever press the pairing gave it to");

    // The live minute of 2026-11-04 20:39:12: A's 4 presses 200 ms apart, its
    // buff cast 196 ms after the first -- with skill key 2 pressed 300 ms
    // before A, so the oldest-first pairing gives the buff cast to key 2. And
    // S's 4 presses at 20:39:13.3, with only attacks cast around them.
    const std::uint32_t kA = 'A', kS = 'S';
    CastMatch m;
    std::vector<CastMatch::Press> presses;
    presses.push_back(press_of(kKey2, kT0 - 300));
    for (int i = 0; i < 4; ++i) presses.push_back(press_of(kA, kT0 + static_cast<std::uint64_t>(i) * 200));
    m.add(kT0 + 196, cast_of(13304, 2));
    const std::uint64_t s0 = kT0 + 1300;
    for (int i = 0; i < 4; ++i) presses.push_back(press_of(kS, s0 + static_cast<std::uint64_t>(i) * 200));
    m.add(s0 + 250, cast_of(51305, 1));
    m.add(s0 + 900, cast_of(51302, 1));
    m.pair(presses, { kA, kS }, kT0 + 1500);
    const bool early = m.take_bursts().empty();
    m.pair(presses, { kA, kS }, s0 + 600 + CastMatch::kBurstGapMs + CastMatch::kSettleMs + 1);
    const std::vector<CastMatch::Burst> bursts = m.take_bursts();
    const std::vector<CastMatch::Cast> casts = m.casts();
    check(early && bursts.size() == 2 && casts.size() == 3 && casts[0].vk == kKey2,
          "nothing judged while a press may still join; then both bursts -- the buff cast paired with key 2, "
          "pressed before A, as the live pairing did");
    if (bursts.size() == 2) {
        const CastMatch::Burst& a = bursts[0];
        const CastMatch::Burst& s = bursts[1];
        check(a.vk == kA && a.presses == 4 && a.first_ms == kT0 && a.last_ms == kT0 + 600 && a.seen &&
                  a.skill == 13304 && a.cast_ms == kT0 + 196 &&
                  burst_line(a) == "Buff (packets): key A -- a kind-2 cast of yours, skill 13304, 196 ms after its "
                                   "first press (4 presses)",
              "A: its buff cast seen with its burst all the same -- " + burst_line(a));
        check(s.vk == kS && s.presses == 4 && !s.seen &&
                  burst_line(s) == "Buff (packets): key S -- no kind-2 cast of yours from 200 ms before its first "
                                   "press to 700 ms after its last: maybe not cast (4 presses)",
              "S: only attacks cast round its presses -- " + burst_line(s));
    }
    const std::string line = casts_line(m.take_minute());
    check(has(line, "key A (buff): 4 pressed, 0 cast") && has(line, "a kind-2 cast with 1 of its 1 burst") &&
              has(line, "key S (buff): 4 pressed") && has(line, "a kind-2 cast with 0 of its 1 burst"),
          "and the Casts line says it of each buff key: " + line);

    // Presses of one key more than kBurstGapMs apart are two bursts; a press
    // within it joins.
    CastMatch two;
    two.add(kT0 + 4300, cast_of(13304, 2));
    two.pair({ press_of(kA, kT0), press_of(kA, kT0 + 1900), press_of(kA, kT0 + 4000) }, { kA }, kT0 + 10'000);
    const std::vector<CastMatch::Burst> split = two.take_bursts();
    check(split.size() == 2 && split[0].presses == 2 && !split[0].seen && split[1].presses == 1 && split[1].seen &&
              split[1].cast_ms == kT0 + 4300 && two.take_bursts().empty(),
          "1.9 s apart: one burst; 2.1 s after it: another, its own cast its own; each given once");
    // A skill key's presses make no burst.
    CastMatch skill;
    skill.pair({ press_of(kKey2, kT0) }, {}, kT0 + 10'000);
    check(skill.take_bursts().empty(), "a skill key's presses: no burst");

    // The hunter's own attack key, handed in beside the macro's: its cast its
    // own, not the macro key's pressed after it.
    const std::uint32_t kZ = 'Z';
    CastMatch with_z, without_z;
    with_z.add(kT0 + 150, cast_of(51305, 1));
    without_z.add(kT0 + 150, cast_of(51305, 1));
    with_z.pair({ press_of(kZ, kT0), press_of(kKey2, kT0 + 100) }, {}, kT0 + 5000);
    without_z.pair({ press_of(kKey2, kT0 + 100) }, {}, kT0 + 5000);
    check(with_z.casts()[0].vk == kZ && without_z.casts()[0].vk == kKey2,
          "Z pressed 100 ms before key 2: its cast is Z's; without Z's press handed in, key 2 took it");
}

// ------------------------------------- how evenly they came (2026.11.08)

/// The test's own clock text: how long after kT0.
std::string test_clock(std::uint64_t at_ms) { return "T+" + std::to_string(at_ms - kT0); }

void test_cast_gaps() {
    section("Your casts' gaps, each skill's shortest re-cast, GoBot's newest press to each cast (2026.11.08)");

    // Five casts: gaps of 700, 800, 800 and 1700 ms. Skill 51205 three times,
    // 1.5 and 2.5 s apart. GoBot's presses 40, 80, 100 and 60 ms before four
    // of them; the one at +1500 none took -- the last press 880 ms before it,
    // the next 700 ms after -- and it came while the keys were held.
    const std::uint32_t kZ = 'Z', kX = 'X', kC = 'C';
    CastMatch m;
    m.add(kT0, cast_of(51205, 1));
    m.add(kT0 + 700, cast_of(51201, 1));
    m.set_held(true);
    m.add(kT0 + 1500, cast_of(51205, 1));
    m.set_held(false);
    m.add(kT0 + 2300, cast_of(51202, 1));
    m.add(kT0 + 4000, cast_of(51205, 1));
    const std::vector<CastMatch::Press> presses{ press_of(kZ, kT0 - 40), press_of(kX, kT0 + 620),
                                                 press_of(kC, kT0 + 2200), press_of(kZ, kT0 + 3940) };
    m.pair(presses, {}, kT0 + 6000);
    const CastMatch::Minute minute = m.take_minute();
    check(minute.gaps == std::vector<std::uint64_t>{ 700, 800, 800, 1700 } && minute.longest_ms == 1700 &&
              minute.longest_at_ms == kT0 + 4000 && minute.skills.size() == 3 && minute.skills[0].skill == 51205 &&
              minute.skills[0].casts == 3 && minute.skills[0].recasts == 2 && minute.skills[0].shortest_ms == 1500 &&
              minute.skills[1].skill == 51201 && minute.skills[1].recasts == 0 && minute.skills[2].skill == 51202,
          "from the ring: each gap from the cast before, the longest and when it ended, each skill's casts -- the "
          "most cast first -- and its shortest gap from its own cast before");
    check(minute.delays == std::vector<std::uint64_t>{ 40, 80, 100, 60 } && minute.no_press_before == 1 &&
              minute.unpaired == 1 && minute.lone.size() == 1 && minute.lone[0].at_ms == kT0 + 1500 &&
              minute.lone[0].held && minute.lone[0].before_ms == kT0 + 620 && minute.lone[0].before_vk == kX &&
              minute.lone[0].after_ms == kT0 + 2200 && minute.lone[0].after_vk == kC,
          "once settled: GoBot's newest press to each cast within 700 ms; the one no press took, with the presses "
          "either side and the hold");
    const std::string gaps = cast_gaps_line(minute, &test_clock);
    check(has(gaps, "Cast gaps (packets): 4 gaps -- p50 0.80 s, p90 1.70 s, longest 1.70 s ending at T+4000; "
                    "over 1.0 s 1, over 1.5 s 1") &&
              has(gaps, "; newest press to cast p10 40 ms, p50 60 ms, p90 100 ms (4 casts), 1 with no press of "
                        "GoBot's in the 700 ms before") &&
              has(gaps, "; by skill 51205 3 casts, shortest re-cast 1.50 s / 51201 1 cast / 51202 1 cast"),
          "the line after the Casts line: " + gaps);
    const std::string lone = lone_cast_line(minute.lone[0], &test_clock);
    check(lone == "Cast (packets) with no GoBot key at T+1500: skill 51205, kind 1; GoBot's last press 880 ms before "
                  "it (key X), the first after it 700 ms after (key C); the keys held for a job then",
          "and the cast no press took: " + lone);
    check(has(casts_line(minute), "Casts (packets): 5 this minute") && has(casts_line(minute), "no macro key 1"),
          "the Casts line as before");

    // The next minute: its one cast's gap from the minute before's last, and
    // its skill's re-cast likewise.
    m.add(kT0 + 4800, cast_of(51205, 1));
    m.pair({}, {}, kT0 + 7000);
    const CastMatch::Minute next = m.take_minute();
    check(next.gaps == std::vector<std::uint64_t>{ 800 } && next.skills.size() == 1 && next.skills[0].casts == 1 &&
              next.skills[0].shortest_ms == 800 && next.delays.empty() && next.no_press_before == 1 &&
              next.lone.size() == 1 && !next.lone[0].held && next.lone[0].before_ms == kT0 + 3940 &&
              next.lone[0].after_ms == 0,
          "the next minute: only its own casts, each gap and re-cast from the cast before it, the minute before's "
          "too; a cast with GoBot's last press 860 ms before it has none in the 700 ms before");
    check(has(lone_cast_line(next.lone[0], &test_clock), "GoBot's last press 860 ms before it (key Z), the first "
                                                         "after it none in the 1200 ms after; no hold then"),
          "with no press after it yet: none, up to where the presses are known -- " +
              lone_cast_line(next.lone[0], &test_clock));
    const CastMatch::Minute quiet = m.take_minute();
    check(cast_gaps_line(quiet).empty() && quiet.gaps.empty() && quiet.skills.empty(),
          "a minute with no cast writes no gaps line");

    // Casts no press took, GoBot's last press 1-6.4 s before: the first
    // kLoneSaid described, the rest counted.
    CastMatch hand;
    for (int i = 0; i < 7; ++i) hand.add(kT0 + static_cast<std::uint64_t>(i) * 900, cast_of(51205, 1));
    hand.pair({ press_of(kZ, kT0 - 1000) }, {}, kT0 + 9000);
    const CastMatch::Minute played = hand.take_minute();
    check(played.unpaired == 7 && played.lone.size() == CastMatch::kLoneSaid && played.lone_more == 2 &&
              played.lone_far == 0 && played.lone[0].before_ms == kT0 - 1000 &&
              has(lone_cast_line(played.lone[0]), "at t=" + std::to_string(kT0) + " ms: skill 51205, kind 1; "
                                                  "GoBot's last press 1000 ms before it (key Z)"),
          "seven casts near GoBot's last press: five described, two more counted; with no clock given, the clock's "
          "own ms");
    // By hand: no press of GoBot's within kLoneNearMs either side -- counted
    // only, unless the keys were held then.
    CastMatch distant;
    distant.add(kT0, cast_of(51205, 1));
    distant.add(kT0 + 11'000, cast_of(51205, 1));
    distant.set_held(true);
    distant.add(kT0 + 22'000, cast_of(51205, 1));
    distant.set_held(false);
    distant.pair({ press_of(kZ, kT0 - 20'000) }, {}, kT0 + 25'000);
    const CastMatch::Minute alone = distant.take_minute();
    check(alone.unpaired == 3 && alone.lone_far == 2 && alone.lone_more == 0 && alone.lone.size() == 1 &&
              alone.lone[0].held && alone.lone[0].at_ms == kT0 + 22'000,
          "no press of GoBot's within 10 s either side: counted only; the one in a hold still described");

    // Gaps only within a run: across the macro's stop and start, or a capture
    // started again, left out and counted.
    CastMatch runs;
    runs.set_run(true, false, 1);
    runs.add(kT0, cast_of(51205, 1));
    runs.add(kT0 + 700, cast_of(51201, 1));
    runs.set_run(false, false, 1);
    runs.set_run(true, false, 1);
    runs.add(kT0 + 10'700, cast_of(51205, 1));
    runs.add(kT0 + 11'500, cast_of(51202, 1));
    runs.set_run(true, false, 2);
    runs.add(kT0 + 40'000, cast_of(51201, 1));
    runs.add(kT0 + 40'750, cast_of(51205, 1));
    const CastMatch::Minute split = runs.take_minute();
    const std::string split_line = cast_gaps_line(split, &test_clock);
    check(split.gaps == std::vector<std::uint64_t>{ 700, 800, 750 } && split.longest_ms == 800 &&
              split.longest_at_ms == kT0 + 11'500 && split.gaps_left_out == 2 &&
              has(split_line, "3 gaps -- p50 0.75 s, p90 0.80 s, longest 0.80 s ending at T+11500; over 1.0 s 0, "
                              "over 1.5 s 0; 2 gaps across a stop and start or a capture restart, left out"),
          "a stop and start, and a capture started again: no gap across either -- " + split_line);
    // The hunter or your hands alone, the macro off all along: one run.
    CastMatch hunted;
    hunted.set_run(false, true, 1);
    hunted.add(kT0, cast_of(51205, 1));
    hunted.set_run(false, true, 1);
    hunted.add(kT0 + 900, cast_of(51205, 1));
    const CastMatch::Minute by_hunter = hunted.take_minute();
    check(by_hunter.gaps == std::vector<std::uint64_t>{ 900 } && by_hunter.gaps_left_out == 0 &&
              !has(cast_gaps_line(by_hunter), "left out"),
          "with the macro off, the hunter's casts still one run: their gaps kept");
}

/// The macro's rows of 2026-11-08 22:13-22:40: A and S attack skills at 3 s,
/// and the buff rows key_4 and key_5 switched off, still on A and S.
void test_buff_rows_off() {
    section("A buff row switched off with a skill's key on it: that key is no buff key (2026.11.08)");

    const auto row = [](std::uint32_t vk, std::uint32_t ms, bool on, bool buff) {
        MacroKey k;
        k.keycode = vk;
        k.interval_ms = ms;
        k.enabled = on;
        k.buff = buff;
        return k;
    };
    const std::uint32_t kA = 'A', kS = 'S';
    const std::vector<MacroKey> keys{ row('Z', 500, true, false),  row(kA, 60'000, false, true),
                                      row(kS, 300'000, false, true), row(kS, 3000, true, false),
                                      row(kA, 3000, true, false),    row(kKey5, 60'000, true, true) };
    check(macro_buff_keycodes(keys) == std::vector<std::uint32_t>{ kKey5 } && !macro_buff_on(keys[1]) &&
              !macro_buff_on(keys[2]) && macro_key_kind(keys[1]) == MacroKeyKind::Buff && macro_buff_on(keys[5]) &&
              !macro_buff_on(keys[4]),
          "the buff rows off on A and S are buffs by their kind, but not pressed: A and S are no buff keys; the buff "
          "row on and in use is");

    // A's presses, each with an attack cast: as a skill key, no burst and no
    // "(buff)" -- where 05b, going by the kind alone, judged each a burst
    // "maybe not cast".
    std::vector<CastMatch::Press> presses;
    CastMatch now, before;
    for (int i = 0; i < 4; ++i) {
        const std::uint64_t at = kT0 + static_cast<std::uint64_t>(i) * 4000;
        presses.push_back(press_of(kA, at));
        now.add(at + 60, cast_of(51305, 1));
        before.add(at + 60, cast_of(51305, 1));
    }
    now.pair(presses, macro_buff_keycodes(keys), kT0 + 20'000);
    before.pair(presses, { kA, kS, kKey5 }, kT0 + 20'000);
    const std::string line = casts_line(now.take_minute());
    const std::vector<CastMatch::Burst> bursts = before.take_bursts();
    check(now.take_bursts().empty() && has(line, "key A: 4 pressed, 4 cast") && !has(line, "(buff)") &&
              bursts.size() == 4 && !bursts[0].seen && has(casts_line(before.take_minute()), "key A (buff)"),
          "A pressed four times, an attack cast each: a skill key, no burst -- " + line +
              " -- where every buff row by its kind had four bursts judged not cast");
}

// ------------------------------------------------------------ the wiring

#ifdef GOBOT_SOURCE_DIR
std::string source(const std::string& rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// A function's definition in `src`, from `head` to the brace that closes it
/// -- the first at the start of a line -- or "" when there is none.
std::string body(const std::string& src, const std::string& head) {
    const std::size_t from = src.find(head);
    if (from == std::string::npos) return {};
    const std::size_t end = src.find("\n}", from);
    return end == std::string::npos ? std::string() : src.substr(from, end + 2 - from);
}

int count_in(const std::string& text, const std::string& part) {
    int n = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
    return n;
}
#endif

void test_more_wiring() {
    section("Casts, buffs, the keeper's hints and the jump test by the packets, wired in (their source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string more = source("/apps/gobot_gui/packets_more.cpp");
    if (more.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::string fed = body(more, "void GuiApp::more_on_packets(");
    const std::string tick = body(more, "void GuiApp::more_tick(");
    const std::string takeoffs = body(more, "std::optional<std::uint64_t> GuiApp::packet_takeoffs() const {");
    check(has(fed, "part.casts.on_packets(all, packet_world_);") && has(fed, "part.buffs.on_packets(all);") &&
              has(tick, "packet_self_.presses_since(part.casts.pair_from())") && has(tick, "part.casts.pair("),
          "packets_more.cpp: the capture thread hands every batch to the casts, with the packet world, and to the "
          "buffs; GoBot's thread pairs the macro's presses from PacketSelf");
    const std::string potions = body(more, "std::vector<std::uint32_t> potion_keys_of(const MacroConfig& cfg) {");
    check(has(tick, "potion_keys_of(macro_config_)") && has(potions, "MacroKeyKind::HpPotion") &&
              has(potions, "MacroKeyKind::MpPotion") && has(potions, "MacroKeyKind::PetPotion") &&
              has(potions, "cfg.sequence"),
          "more_tick passes the potion keys' presses over -- a key a skill, a buff or the sequence also presses "
          "kept");
    check(has(takeoffs, "if (!capture_.running()) return std::nullopt;") && has(takeoffs, "on_game") &&
              has(takeoffs, "s.last_own_ms < s.entry_ms") && has(takeoffs, "s.last_foreign_ms") &&
              has(takeoffs, "me.known") && has(takeoffs, "return s.air_jumps + s.air_second + s.air_high;"),
          "packet_takeoffs: only with a capture on the game GoBot follows, your id locked since the last entry, "
          "you known and no other client's report lately -- the running total of your take-offs");
    // 2026.11.04d: the hunter's attack key's presses handed in beside the
    // macro's, and each buff key's burst written once judged.
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string hunter = source("/src/hunter.cpp");
    check(has(gui, "hunter_->set_on_attack([this](std::uint32_t vk, std::uint64_t at_ms) { more_note_attack(vk, "
                   "at_ms); });") &&
              has(hunter, "if (press(cfg.attack_key, 60) && on_attack_) on_attack_(cfg.attack_key, down_ms);") &&
              has(body(more, "void GuiApp::more_note_attack("), "part.attacks.push_back(") &&
              has(tick, "presses.insert(presses.end(), part.attacks.begin(), part.attacks.end());") &&
              has(tick, "for (const CastMatch::Burst& b : part.casts.take_bursts())") &&
              has(tick, "burst_line(b)"),
          "the hunter's attack presses go to the pairing; each buff burst judged is written");
    const std::string cast = source("/src/cast_match.cpp");
    check(!has(more, "light_") && !has(more + cast, "game_link_of(") && !has(more + cast, "tcp_connections(") &&
              !has(more + cast, "npcap_available(") && !has(more + cast, "key_press(") &&
              !has(more + cast, "send_key("),
          "and none of it tests light_, looks at the socket tables or Npcap, or presses a key");

    // The keeper's hints: log lines only, keeper_wake's answers as they were.
    const std::string keeper =
        body(source("/apps/gobot_gui/simple_keeper.cpp"), "GuiApp::KeeperWake GuiApp::keeper_wake(");
    const std::size_t said =
        keeper.find("GB_INFO(\"Keeper: the screen saw nobody in the world for %d min; the packets: %s\"");
    const std::size_t stalled = keeper.find("return KeeperWake::Stalled;");
    check(said != std::string::npos && stalled != std::string::npos && said < stalled && stalled - said < 200 &&
              count_in(keeper, "return KeeperWake::Stalled;") == 1 &&
              count_in(keeper, "return KeeperWake::None;") == 3 && count_in(keeper, "return KeeperWake::Quit;") == 2 &&
              count_in(keeper, "return go_on ? KeeperWake::None : KeeperWake::Quit;") == 1,
          "keeper_wake: the packets' link written just before a stall is answered, and every answer as before");
    const std::string loop = body(source("/apps/gobot_gui/gui_app.cpp"), "void GuiApp::keeper_loop() {");
    const std::size_t in = loop.find("crossed_in = true;");
    const std::size_t hint = loop.find("Keeper: in by the screen; the packets: map entry %s; %s");
    const std::size_t restart = loop.find("keeper_stall_restart();");
    check(in != std::string::npos && hint != std::string::npos && restart != std::string::npos && in < hint &&
              hint < restart,
          "keeper_loop: after a crossing, the packets' map entry and link written beside the screen's word");

    // The jump test: the take-offs handed to it, and the log saying which decided.
    const std::string style = source("/apps/gobot_gui/simple_style.cpp");
    const std::string trying = body(style, "void GuiApp::simple_jump_try() {");
    check(has(trying, "eyes.takeoffs = [this] { return packet_takeoffs(); };") &&
              has(body(style, "void GuiApp::simple_style_tick() {"), "if (r.by_packets) {"),
          "simple_style.cpp: Thử asks the packets for your take-offs, and its line says when they decided");
#else
    skip("no source folder\n");
#endif
}

void test_log_wiring() {
    section("The log of 2026.11.08 wired in: buff rows on, gaps and lone casts, who toggled, what changed (source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string more = source("/apps/gobot_gui/packets_more.cpp");
    const std::string simple = source("/apps/gobot_gui/simple_view.cpp");
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    if (more.empty() || simple.empty() || gui.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    // Only rows the timers press count: the buff keys, the buff intervals,
    // and the skill and buff keys that keep a potion key's presses.
    const std::string buffs = body(more, "std::vector<std::uint32_t> buff_keys_of(const MacroConfig& cfg) {");
    const std::string potions = body(more, "std::vector<std::uint32_t> potion_keys_of(const MacroConfig& cfg) {");
    const std::string minute = body(more, "void GuiApp::note_casts_minute(std::uint64_t) {");
    const std::string tick = body(more, "void GuiApp::more_tick(");
    check(has(buffs, "return macro_buff_keycodes(cfg.keys);") && has(tick, "buff_keys_of(macro_config_)") &&
              has(potions, "if (macro_timer_on(k) && (kind == MacroKeyKind::Skill || kind == MacroKeyKind::Buff))") &&
              has(minute, "if (macro_buff_on(k)) intervals.emplace_back(") &&
              count_in(more, "macro_key_kind(k) == MacroKeyKind::Buff") == 0,
          "packets_more.cpp: a buff row switched off is no buff key, no buff interval, and keeps no potion key's "
          "presses");
    // The gaps line and the lone casts after the Casts line; the hold as the
    // batch came, an atomic, on the capture thread.
    const std::string fed = body(more, "void GuiApp::more_on_packets(");
    const std::size_t held = fed.find("part.casts.set_held(coord_.critical_active());");
    const std::size_t run = fed.find("part.casts.set_run(");
    check(held != std::string::npos && held < fed.find("part.casts.on_packets(all, packet_world_);") &&
              run != std::string::npos && run < fed.find("part.casts.on_packets(all, packet_world_);") &&
              has(fed, "part.casts.set_run(macro_ && macro_->running(), hunter_ && hunter_->running(), "
                       "capture_.start_count());") &&
              has(minute, "cast_gaps_line(casts, &cast_clock_text)") &&
              has(minute, "for (const CastMatch::Lone& c : casts.lone)") &&
              has(minute, "lone_cast_line(c, &cast_clock_text)") && has(minute, "casts.lone_more") &&
              has(minute, "casts.lone_far"),
          "the gaps line and each lone cast written once a minute; the hold and the run stamped before each batch");
    // Written off the window's thread, one batch a minute in the order said;
    // waited for at the close.
    const std::string later = body(more, "void GuiApp::log_later(std::vector<std::string> lines) {");
    const std::string writes = body(more, "void GuiApp::write_logged_later() {");
    const std::string closing = body(gui, "GuiApp::~GuiApp() {");
    const std::size_t last_minute = closing.find("packets_minute(now_ms());");
    const std::size_t waited = closing.find("log_worker_.wait_idle();");
    check(has(minute, "log_later(std::move(lines));") && count_in(minute, "Logger::instance().write") == 0 &&
              count_in(minute, "GB_") == 0 && has(later, "log_worker_.post(") && !has(later, "Logger::") &&
              has(writes, "Logger::instance().write(LogLevel::Info, l);") && last_minute != std::string::npos &&
              waited != std::string::npos && last_minute < waited && has(closing, "log_worker_.stop();"),
          "the minute's lines written on log_worker_, not the window's thread -- in their order, the last minute's "
          "waited for at the close");
    // Who toggled: the simple screen's own three ways, said before each.
    check(has(body(gui, "void GuiApp::tick() {"), "if (simple_view_) simple_toggle(RunFrom::ToggleKey);") &&
              has(simple, "if (ImGui::Button(caption.c_str(), ImVec2(-1, 64))) simple_toggle(RunFrom::Button);") &&
              has(simple, "if (simple_running()) simple_stop(RunFrom::ChatButton);") &&
              has(body(simple, "void GuiApp::simple_toggle(RunFrom from) {"),
                  "if (simple_running()) simple_stop(from); else simple_start(from);") &&
              has(body(simple, "void GuiApp::simple_start(RunFrom from) {"), "simple_start();") &&
              has(body(simple, "void GuiApp::simple_stop(RunFrom from) {"), "simple_stop();"),
          "F10, the big button and the chat card's button each say who asked, then start or stop as before");
    // What changed during a run: in memory, against the run's start.
    const std::string changed = body(simple, "void GuiApp::simple_changed() {");
    const std::string said = body(simple, "void GuiApp::simple_log_settings_changed() {");
    const std::string asked_start = body(simple, "void GuiApp::simple_start(RunFrom from) {");
    const std::string asked_stop = body(simple, "void GuiApp::simple_stop(RunFrom from) {");
    check(has(changed, "if (simple_running()) simple_log_settings_changed();") &&
              has(said, "settings_changes(simple_settings_seen_, now)") && !has(said, "save(") &&
              !has(said, "load(") && has(body(simple, "void GuiApp::simple_track_run(bool running) {"),
                                         "simple_settings_seen_ = simple_settings_ini();"),
          "simple_changed says what changed during a run, against the settings it began with, reading and "
          "writing no file for it");
    check(!has(said, "GB_") && !has(said, "Logger::") && !has(asked_start, "GB_") &&
              !has(asked_start, "Logger::") && !has(asked_stop, "GB_") && !has(asked_stop, "Logger::") &&
              has(said, "log_later(") && has(asked_start, "log_later(") && has(asked_stop, "log_later("),
          "who toggled and what changed: said through log_later, no file written on the window's thread");
    // And what changed while stopped, said at the next start, against the
    // last run's settings -- before the new ones are taken.
    const std::string track = body(simple, "void GuiApp::simple_track_run(bool running) {");
    const std::size_t told = track.find("log_later({ \"Settings changed since the last run: \" + changes });");
    check(has(track, "if (simple_settings_known_) {") &&
              has(track, "settings_changes(simple_settings_seen_, simple_settings_ini())") &&
              told != std::string::npos && told < track.find("simple_settings_seen_ = simple_settings_ini();") &&
              has(track, "simple_settings_known_ = true;"),
          "a start says what changed while stopped, against the last run's settings, before taking the new ones");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_cast_match(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_cast_readers();
    test_cast_pairing();
    test_cast_reach();
    test_buff_watch();
    test_buff_bursts();
    test_cast_gaps();
    test_buff_rows_off();
    test_more_wiring();
    test_log_wiring();
}
