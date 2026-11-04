#pragma once

#include "gobot/byte_reader.hpp"
#include "gobot/common.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace gobot {

// ---- your own character's frames: HP, MP, the bag, gold ----
//
// Read from frames the server sends inside 0x0081, which the capture unpacks
// (FramingSpec::unpack), and one it sends plain over UDP. All of them were
// found in the P0-2 recordings of 2026-10-18 (docs/protocol-findings.md §4e)
// and checked against the HUD there: none has been proven on today's server.
// So nothing acts on them directly. PacketSelf (gobot/packet_self.hpp) keeps
// what they say, and a gate compares it with the screen before anything uses
// it (the packets plan, items D and E).
//
// Each reader takes a frame only from its own side, at its own depth and of
// exactly the size every one of the recordings had. Anything else gives
// nothing, and `fit`, when given, says why -- for the counts PacketSelf keeps:
// a game update that moved the fields would most likely change the size too.
//
// The offsets count from the frame's first byte, header included, as the
// docs count them. A payload starts after the 8-byte header, so each is read
// 8 bytes earlier in it.

/// Why a reader took a frame, or did not.
enum class FrameFit : std::uint8_t {
    Taken,          ///< read
    OtherOpcode,    ///< not the reader's opcode at all
    WrongSide,      ///< from the other side, over the other transport, or at the other depth
    WrongSize,      ///< its side, but not the size every recorded one had
};

/// 0x0051 inside 0x0081: HP and MP now. 24 bytes.
struct SelfVitalsFrame {
    std::uint32_t hp = 0;     ///< at 12
    std::uint32_t mp = 0;     ///< at 16
};

/// 0x0050 inside 0x0081, the stat sheet: at every map load and after a buff
/// lands. 200 bytes.
struct StatSheetFrame {
    std::uint32_t max_hp = 0;     ///< at 68
    std::uint32_t hp = 0;         ///< at 72
    std::uint32_t max_mp = 0;     ///< at 76
    std::uint32_t mp = 0;         ///< at 80
};

/// One bag slot of 0x0068: an item id, 0 for an empty slot, and how many.
struct BagSlotFrame {
    std::uint32_t item = 0;
    std::uint16_t count = 0;
};

/// The bag's 72 slots, as 0x0068 inside 0x0081 lists them. 1528 bytes.
constexpr std::size_t kBagSlots = 72;
using BagFrame = std::array<BagSlotFrame, kBagSlots>;

namespace self_frames_detail {

constexpr std::size_t kHeader = 8;     // frame bytes before the payload

/// `p` is `opcode` from the server: over TCP, inside a wrapper when
/// `wrapped`, straight off the stream otherwise -- or over UDP, straight off
/// a datagram, when `udp`. Its size is checked last, so that a frame of the
/// other side is never counted as one of the wrong size.
inline FrameFit fit_of(const GamePacket& p, std::uint16_t opcode, bool udp, bool wrapped, std::size_t size) {
    if (p.opcode != opcode) return FrameFit::OtherOpcode;
    if (!p.from_server || p.udp != udp) return FrameFit::WrongSide;
    if (wrapped ? p.depth == 0 : p.depth != 0) return FrameFit::WrongSide;
    if (p.payload.size() + kHeader != size) return FrameFit::WrongSize;
    return FrameFit::Taken;
}

inline bool fits(const GamePacket& p, std::uint16_t opcode, bool udp, bool wrapped, std::size_t size,
                 FrameFit* fit) {
    const FrameFit f = fit_of(p, opcode, udp, wrapped, size);
    if (fit) *fit = f;
    return f == FrameFit::Taken;
}

} // namespace self_frames_detail

/// 0x0051 from the server over TCP, inside 0x0081, 24 bytes: HP u32 at 12,
/// MP u32 at 16. DOCUMENTED (P0-2: 9232 and 8902, the HUD's own).
inline std::optional<SelfVitalsFrame> read_self_vitals(const GamePacket& p, FrameFit* fit = nullptr) {
    using namespace self_frames_detail;
    if (!fits(p, 0x0051, false, true, 24, fit)) return std::nullopt;
    ByteReader r(p.payload);
    SelfVitalsFrame out;
    r.seek(12 - kHeader);
    out.hp = r.u32();
    r.seek(16 - kHeader);
    out.mp = r.u32();
    if (!r.ok()) return std::nullopt;
    return out;
}

/// 0x0050 from the server over TCP, inside 0x0081, 200 bytes: max HP u32 at
/// 68, HP at 72, max MP at 76, MP at 80. DOCUMENTED.
inline std::optional<StatSheetFrame> read_stat_sheet(const GamePacket& p, FrameFit* fit = nullptr) {
    using namespace self_frames_detail;
    if (!fits(p, 0x0050, false, true, 200, fit)) return std::nullopt;
    ByteReader r(p.payload);
    StatSheetFrame out;
    r.seek(68 - kHeader);
    out.max_hp = r.u32();
    out.hp = r.u32();
    out.max_mp = r.u32();
    out.mp = r.u32();
    if (!r.ok()) return std::nullopt;
    return out;
}

/// 0x02F0 from the server over UDP, plain, 16 bytes: MP u32 at 12.
/// DOCUMENTED, and only ever written to the log: it came only in a fight, and
/// in what order it and 0x0051 say MP is not known.
inline std::optional<std::uint32_t> read_udp_mp(const GamePacket& p, FrameFit* fit = nullptr) {
    using namespace self_frames_detail;
    if (!fits(p, 0x02F0, true, false, 16, fit)) return std::nullopt;
    ByteReader r(p.payload);
    r.seek(12 - kHeader);
    const std::uint32_t mp = r.u32();
    if (!r.ok()) return std::nullopt;
    return mp;
}

/// 0x0068 from the server over TCP, inside 0x0081, 1528 bytes: for each slot
/// j of 72, the item id u32 at 12 + 4j and the count u16 at 300 + 2j.
/// DOCUMENTED: after each potion, the slot holding it one lower.
inline std::optional<BagFrame> read_bag(const GamePacket& p, FrameFit* fit = nullptr) {
    using namespace self_frames_detail;
    if (!fits(p, 0x0068, false, true, 1528, fit)) return std::nullopt;
    ByteReader r(p.payload);
    BagFrame out{};
    r.seek(12 - kHeader);
    for (std::size_t j = 0; j < kBagSlots; ++j) out[j].item = r.u32();
    r.seek(300 - kHeader);
    for (std::size_t j = 0; j < kBagSlots; ++j) out[j].count = r.u16();
    if (!r.ok()) return std::nullopt;
    return out;
}

/// 0x006B from the server over TCP, inside 0x0081, 28 bytes: your gold, u32
/// at 12. DOCUMENTED: 105 of 105 rises equal the next pickup. Log only.
inline std::optional<std::uint32_t> read_gold(const GamePacket& p, FrameFit* fit = nullptr) {
    using namespace self_frames_detail;
    if (!fits(p, 0x006B, false, true, 28, fit)) return std::nullopt;
    ByteReader r(p.payload);
    r.seek(12 - kHeader);
    const std::uint32_t gold = r.u32();
    if (!r.ok()) return std::nullopt;
    return gold;
}

} // namespace gobot
