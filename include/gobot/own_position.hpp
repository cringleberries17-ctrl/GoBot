#pragma once

#include "gobot/byte_reader.hpp"
#include "gobot/common.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace gobot {

/// Which of the client's frames a report of your own position came from.
///
/// The P0-2 recordings (2026-10-18; docs/protocol-findings.md §4e) showed the
/// game client telling the server where your character is: over UDP, in plain
/// frames with the server's own header. It does so on events, never on a
/// clock -- when a walk starts, stops or turns, when it leaves the ground and
/// lands, at every map entry, and with every skill cast. Standing still sends
/// nothing, and a walk into a wall sends no stop, so whatever uses these has to
/// work out the position between reports and stop it at walls (plan item P0-3).
enum class OwnReport : std::uint8_t {
    Move,    ///< 0x0027: a walk starts, stops or turns
    Air,     ///< 0x002A: leaving the ground, a jump in the air, landing
    Other,   ///< 0x0029: a third movement report; what sends it is not settled
    Here,    ///< 0x0028: four copies at every map entry, a few more in a fight
    Cast,    ///< 0x0031: a skill cast, with the caster's x, y in whole pixels
};

/// One report of your own position, as the client sent it.
struct OwnPosition {
    OwnReport     kind = OwnReport::Move;
    std::uint32_t id   = 0;      ///< your character's id, the same all session
    float         x    = 0.0f;   ///< world pixels
    float         y    = 0.0f;   ///< world pixels, growing downward

    /// Along the facing: 9 walking and 0 stopped in the recordings, which is
    /// GoC's 360 px/s walking speed if the unit is pixels per 25 ms step (an
    /// inference, not measured on screen). Another actor's frames read 9.5, so
    /// it is to be taken from here, not assumed. A skill cast does not say.
    std::optional<float> speed;

    /// +1 toward larger x, -1 toward smaller x, 0 where the frame does not say
    /// (0x002A and 0x0031 carry no facing).
    int facing = 0;

    /// 0x002A only. The state word: 0 landed; 1 left the ground (a jump, or
    /// walking off an edge); 6 a second jump in the air; 2 a high jump; 3 seen
    /// with a vertical speed of -10. And the vertical speed in pixels per 25 ms
    /// step, negative upward: -20 for a jump, -36 for a high jump.
    std::optional<std::uint32_t> air_state;
    std::optional<float>         vertical_speed;
};

/// Read one of the client's reports of its own position; nothing for any
/// other frame.
///
/// Only a frame the client sent over UDP, read straight off a datagram, counts.
/// The server sends 0x0027, 0x0029 and 0x002A in the same layouts for other
/// actors' moves, and those are not you; the client's own 0x010D and 0x0117
/// carry the pet's position, which is not yours either. A frame too short for
/// its fields, or whose x or y is not a finite number, gives nothing.
///
/// The offsets in the comments count from the frame's first byte, header
/// included, as docs/protocol-findings.md §4e and GoC count them. A payload
/// starts after the 8-byte header, so each is read 8 bytes earlier in it.
///
/// Nothing calls this yet: P0-3 wires it into the world model.
inline std::optional<OwnPosition> read_own_position(const GamePacket& p) {
    if (p.from_server || !p.udp || p.depth != 0) return std::nullopt;

    constexpr std::size_t kHeader = 8;     // frame bytes before the payload
    ByteReader r(p.payload);
    // A failed read poisons the reader, so one ok() at the end covers them all.
    auto u8_at  = [&](std::size_t at) { r.seek(at - kHeader); return r.u8(); };
    auto u16_at = [&](std::size_t at) { r.seek(at - kHeader); return r.u16(); };
    auto u32_at = [&](std::size_t at) { r.seek(at - kHeader); return r.u32(); };
    auto f32_at = [&](std::size_t at) { r.seek(at - kHeader); return r.f32(); };
    auto facing_of = [](std::uint32_t v) { return v == 0x01u ? 1 : v == 0xFFu ? -1 : 0; };

    OwnPosition out;
    switch (p.opcode) {
    case 0x0027:   // id 8 and 12; x 16; y 20; speed 24; facing u8 28
        out.kind   = OwnReport::Move;
        out.id     = u32_at(12);
        out.x      = f32_at(16);
        out.y      = f32_at(20);
        out.speed  = f32_at(24);
        out.facing = facing_of(u8_at(28));
        break;
    case 0x002A:   // id 8 and 12; state 16; x 20; y 24; speed 28; vertical speed 32
        out.kind           = OwnReport::Air;
        out.id             = u32_at(12);
        out.air_state      = u32_at(16);
        out.x              = f32_at(20);
        out.y              = f32_at(24);
        out.speed          = f32_at(28);
        out.vertical_speed = f32_at(32);
        break;
    case 0x0029:   // id 8 and 12; facing u32 16; speed 20; x 24; y 28
        out.kind   = OwnReport::Other;
        out.id     = u32_at(12);
        out.facing = facing_of(u32_at(16));
        out.speed  = f32_at(20);
        out.x      = f32_at(24);
        out.y      = f32_at(28);
        break;
    case 0x0028:   // sequence 8; id 12 and 16; x 20; y 24; speed 28; flags 32, facing in the low byte
        out.kind   = OwnReport::Here;
        out.id     = u32_at(12);
        out.x      = f32_at(20);
        out.y      = f32_at(24);
        out.speed  = f32_at(28);
        out.facing = facing_of(u32_at(32) & 0xFFu);
        break;
    case 0x0031:   // id 8 and 12; x u16 16; y u16 18; skill id 20
        out.kind = OwnReport::Cast;
        out.id   = u32_at(12);
        out.x    = static_cast<float>(u16_at(16));
        out.y    = static_cast<float>(u16_at(18));
        break;
    default:
        return std::nullopt;
    }
    if (!r.ok() || !std::isfinite(out.x) || !std::isfinite(out.y)) return std::nullopt;
    if (out.speed && !std::isfinite(*out.speed)) out.speed.reset();
    if (out.vertical_speed && !std::isfinite(*out.vertical_speed)) out.vertical_speed.reset();
    return out;
}

} // namespace gobot
