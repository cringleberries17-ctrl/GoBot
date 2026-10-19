#pragma once

#include "gobot/byte_reader.hpp"
#include "gobot/common.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gobot {

// The monsters of the map, as the game's own server frames give them.
//
// The P0-2 fight (2026-10-18; docs/protocol-findings.md §4f) showed every
// monster of the map in three server frames, the whole map and not only what
// is near you:
//
//   - 0x0042, inside 0x0081, once per map load: a table of every monster on
//     the map, with its position, HP and max HP;
//   - plain 0x003F: a monster spawning in a slot, 2-4 s after the last one
//     there was killed;
//   - plain 0x0038: a monster's state event, sent at every change -- it
//     stops, it starts walking or turns, it is hit -- with its position and HP.
//
// A monster is known by its SLOT on the map (0-23 on the fight's map), not by
// an id of its own: a respawn puts a new monster in the slot of a dead one.
// Slot 0 is a monster like any other. The type id comes only in 0x0042 and
// 0x003F, and no frame carries a monster's name.
//
// The offsets in the comments count from the frame's first byte, header
// included, as protocol-findings §4e and §4f count them. A payload starts
// after the 8-byte header, so each is read 8 bytes earlier in it. Every number
// is little-endian.
//
// Nothing calls these yet: P0-3 builds the monster list on them.

/// One monster as the map's table (0x0042) or a respawn (0x003F) gives it.
struct MapMonster {
    std::uint32_t slot    = 0;   ///< its place on the map; what 0x0038 names it by
    std::uint32_t type_id = 0;   ///< what kind of monster; the slot keeps it over respawns

    /// Likely the level: the fight's two types read 44 and 39 here, which are
    /// also the middle digits of their type ids. Not proven.
    std::uint8_t  level   = 0;

    std::uint16_t x = 0;         ///< world pixels, the same space as your own
    std::uint16_t y = 0;         ///< world pixels, growing downward
    int           facing  = 0;   ///< +1 toward larger x, -1 toward smaller x, 0 if neither

    /// Pixels per 25 ms step along the facing: 0 for a monster that stands,
    /// and for a respawn, which stands at first. A walking monster covers
    /// speed x 40 px a second (2.6 on the fight's map, 104 px/s).
    float         speed   = 0.0f;

    std::uint32_t hp      = 0;   ///< now; a respawn comes with it full
    std::uint32_t max_hp  = 0;
};

/// Every monster in the map's table, 0x0042, read off the frame unpacked from
/// its 0x0081 (FramingSpec::unpack); nothing for any other frame.
///
/// The table is 2,568 bytes, in columns of 50 entries (the room it has), and
/// entry k is a monster for k below the count, a u32 at byte 12:
///
///   facing i8 at 166 + k        slot u16 at 316 + 2k
///   level u8 at 66 + k          x, y u16 at 416 + 2k and 516 + 2k
///   type id u32 at 1516 + 4k    speed f32 at 1716 + 4k
///   HP u32 at 2016 + 4k         max HP u32 at 2216 + 4k
///
/// It is the map as it stands at the load: the HP of a monster hurt before
/// you came, and a walking monster's speed, which it keeps until its first
/// 0x0038. A count of 0 -- a map without monsters, as on all six loads of the
/// shop run -- gives an empty list. Read only a frame of exactly that size,
/// from the server, found inside a wrapper: until P0-3 turns the live
/// capture's unpacking on, the frame it finds inside a wrapper is still packed
/// past its first body byte, and of another size. A count over 50 gives
/// nothing; an entry whose slot reads 0xFFFF, the column's mark for no
/// monster, or whose speed is not a number, is left out.
inline std::optional<std::vector<MapMonster>> read_map_monsters(const GamePacket& p) {
    constexpr std::size_t kFrame = 2568, kHeader = 8, kRoom = 50;
    if (p.opcode != 0x0042 || !p.from_server || p.udp || p.depth == 0) return std::nullopt;
    if (p.payload.size() != kFrame - kHeader) return std::nullopt;

    ByteReader r(p.payload);
    auto u8_at  = [&](std::size_t at) { r.seek(at - kHeader); return r.u8(); };
    auto i8_at  = [&](std::size_t at) { r.seek(at - kHeader); return r.i8(); };
    auto u16_at = [&](std::size_t at) { r.seek(at - kHeader); return r.u16(); };
    auto u32_at = [&](std::size_t at) { r.seek(at - kHeader); return r.u32(); };
    auto f32_at = [&](std::size_t at) { r.seek(at - kHeader); return r.f32(); };

    const std::uint32_t count = u32_at(12);
    if (!r.ok() || count > kRoom) return std::nullopt;

    std::vector<MapMonster> out;
    for (std::size_t k = 0; k < count; ++k) {
        MapMonster m;
        m.slot    = u16_at(316 + 2 * k);
        m.type_id = u32_at(1516 + 4 * k);
        m.level   = u8_at(66 + k);
        m.x       = u16_at(416 + 2 * k);
        m.y       = u16_at(516 + 2 * k);
        const std::int8_t facing = i8_at(166 + k);
        m.facing  = facing == 1 ? 1 : facing == -1 ? -1 : 0;
        m.speed   = f32_at(1716 + 4 * k);
        m.hp      = u32_at(2016 + 4 * k);
        m.max_hp  = u32_at(2216 + 4 * k);
        if (!r.ok()) return std::nullopt;
        if (m.slot == 0xFFFFu || !std::isfinite(m.speed)) continue;
        out.push_back(m);
    }
    return out;
}

/// A monster spawning in a slot: plain 0x003F, from the server; nothing for
/// any other frame.
///
///   type id u32 at 12      level u8 at 16      facing i8 at 18
///   x, y u16 at 20, 22     max HP u32 at 28    slot u16 at 32
///
/// In the fight one came 2-4 s after every kill, 122 of 122, in the dead
/// monster's slot and of its type, at one of the map's spawn points. The new
/// monster stands there full of HP: the slot's next 0x0038 is a stop at the
/// same x, with HP equal to max HP and y 9-36 px lower, as it settles on the
/// ground.
inline std::optional<MapMonster> read_monster_spawn(const GamePacket& p) {
    constexpr std::size_t kHeader = 8;
    if (p.opcode != 0x003F || !p.from_server || p.udp || p.depth != 0) return std::nullopt;

    ByteReader r(p.payload);
    auto u8_at  = [&](std::size_t at) { r.seek(at - kHeader); return r.u8(); };
    auto i8_at  = [&](std::size_t at) { r.seek(at - kHeader); return r.i8(); };
    auto u16_at = [&](std::size_t at) { r.seek(at - kHeader); return r.u16(); };
    auto u32_at = [&](std::size_t at) { r.seek(at - kHeader); return r.u32(); };

    MapMonster m;
    m.type_id = u32_at(12);
    m.level   = u8_at(16);
    const std::int8_t facing = i8_at(18);
    m.facing  = facing == 1 ? 1 : facing == -1 ? -1 : 0;
    m.x       = u16_at(20);
    m.y       = u16_at(22);
    m.max_hp  = u32_at(28);
    m.hp      = m.max_hp;
    m.slot    = u16_at(32);
    if (!r.ok()) return std::nullopt;
    return m;
}

/// What a monster's state event says happened.
enum class MonsterEvent : std::uint8_t {
    Stop,   ///< state 0: it stands where it is, and stays there until its next event
    Walk,   ///< state 1: it starts walking, or turns in mid-walk
    Hit,    ///< state 9: it was hit, and the HP is what the hit left
};

/// One plain 0x0038: a monster's state at a change.
struct MonsterState {
    MonsterEvent  event = MonsterEvent::Stop;
    std::uint32_t slot  = 0;

    /// Where the monster is at this event, world pixels (y growing
    /// downward). Between events a stopped monster stays put and a walking
    /// one moves speed x 40 px a second along its facing; a walk, its turns
    /// included, lasts about 200 steps of 25 ms (§4f).
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    int           facing = 0;   ///< +1 toward larger x, -1 toward smaller x, 0 if neither

    /// Pixels per 25 ms step: 2.6 walking on the fight's map, 0 stopped. In a
    /// hit it reads 7 or 18, which is not a walking speed (likely how far the
    /// blow throws the monster).
    float         speed = 0.0f;

    /// HP now; in a hit, what the hit left: below zero when it killed, as
    /// every hit of the fight did. Read as a signed number for that.
    std::int32_t  hp = 0;

    /// Who hit it: your id in every hit of the fight. None (0xFFFFFFFF in
    /// the frame) in every stop and walk.
    std::optional<std::uint32_t> attacker;

    std::uint32_t damage = 0;   ///< a hit's damage: HP before - damage = HP after; 0 otherwise

    /// The hit killed it. No other frame says so: the slot is silent until
    /// its 0x003F brings the next monster, 2-4 s later.
    bool killed() const { return event == MonsterEvent::Hit && hp <= 0; }
};

/// A monster's state event: plain 0x0038, from the server; nothing for any
/// other frame.
///
///   state u8 at 12 (0 stop, 1 walk, 9 hit)    slot u32 at 16
///   facing i8 at 24                           speed f32 at 28
///   x, y u16 at 32, 34                        attacker u32 at 48
///   damage u32 at 52                          HP u32 at 64, i32 after a hit
///
/// Sent for every monster of the map at each change: 1,050 in the 2-minute
/// fight, at most 5.6 s apart for one slot. Not the u16 pair at 88 and 90,
/// which protocol.ini's [Packet.0x0038] reads as x, y: that is a point beside
/// the monster, rewritten when it is attacked and stale otherwise. A state
/// other than 0, 1 or 9 gives nothing, since none was seen and its layout is
/// not known; so does a speed that is not a number.
inline std::optional<MonsterState> read_monster_state(const GamePacket& p) {
    constexpr std::size_t kHeader = 8;
    if (p.opcode != 0x0038 || !p.from_server || p.udp || p.depth != 0) return std::nullopt;

    ByteReader r(p.payload);
    auto u8_at  = [&](std::size_t at) { r.seek(at - kHeader); return r.u8(); };
    auto i8_at  = [&](std::size_t at) { r.seek(at - kHeader); return r.i8(); };
    auto u16_at = [&](std::size_t at) { r.seek(at - kHeader); return r.u16(); };
    auto u32_at = [&](std::size_t at) { r.seek(at - kHeader); return r.u32(); };
    auto i32_at = [&](std::size_t at) { r.seek(at - kHeader); return r.i32(); };
    auto f32_at = [&](std::size_t at) { r.seek(at - kHeader); return r.f32(); };

    MonsterState s;
    const std::uint8_t state = u8_at(12);
    s.slot = u32_at(16);
    const std::int8_t facing = i8_at(24);
    s.facing = facing == 1 ? 1 : facing == -1 ? -1 : 0;
    s.speed  = f32_at(28);
    s.x      = u16_at(32);
    s.y      = u16_at(34);
    const std::uint32_t attacker = u32_at(48);
    s.damage = u32_at(52);
    s.hp     = i32_at(64);
    if (!r.ok() || !std::isfinite(s.speed)) return std::nullopt;

    switch (state) {
    case 0:  s.event = MonsterEvent::Stop; break;
    case 1:  s.event = MonsterEvent::Walk; break;
    case 9:  s.event = MonsterEvent::Hit;  break;
    default: return std::nullopt;
    }
    if (attacker != 0xFFFFFFFFu) s.attacker = attacker;
    return s;
}

} // namespace gobot
