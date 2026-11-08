#pragma once

// What the simple screen says of "Nhảy lên bục trên" (HunterConfig::jump_up)
// and "Tự tìm quái trong vùng" (HunterConfig::seek_in_zone), 2026.11.05b: why
// a tick cannot work as it is set, and which of the card's lines hold while it
// hunts. Pure and tested (hunt_card_test.cpp); the words are the GUI's
// (simple_style.cpp, simple_view.cpp), and what the Bản đồ tab draws of them
// is map_view.hpp's.
//
// Both ticks ship on (the owner's answer 5 of 2026-11-05, and the seek's Q7
// (a)), and both run only where the hunter lets them: the map tab's area in
// use with its map's file, the packets steering, Đứng yên off -- and for the
// jumps the jump key ↑ and a map MapTrust trusts (Hunter::jump_ready). What
// the screen can tell from the settings alone it says at the tick, greyed;
// what only a hunt shows -- no area for this map, the map not trusted -- the
// card and the spot's line say.

#include "gobot/hunter.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace gobot {

/// Why "Nhảy lên bục trên" cannot work as it is set: the first that holds.
enum class JumpUpTick : std::uint8_t {
    Ok,
    StayPut,    ///< Đứng yên một chỗ ticked
    NoPackets,  ///< packet_positions not on
    NoSpot,     ///< Chỗ đánh quái not ticked: no area of the map tab is used
    JumpKey,    ///< the jump key is not ↑ (0x26): every jump up goes on ↑
    /// One of GoBot's own keys is Ctrl or ↑: its own reading of the keys
    /// would take a jump's press for the player's (GuiApp::drop_tick's veto).
    OwnKey,
};
/// `own`: GoBot's own keys -- the bot's switch, the stop, the hunt's switch,
/// the two one-job keys; `own_at`, for OwnKey, which of them.
JumpUpTick jump_up_tick(const HunterConfig& c, std::span<const std::uint32_t> own, std::size_t* own_at = nullptr);
/// Ctrl + ↑ left out, the other keys not: the left, right or attack key is
/// Ctrl, and would go down with it (Hunter::jump_rules).
bool jump_up_no_ctrl(const HunterConfig& c);

/// Why "Tự tìm quái trong vùng" cannot work as it is set.
enum class SeekTick : std::uint8_t {
    Ok,
    StayPut,    ///< Đứng yên một chỗ ticked
    NoPackets,  ///< packet_positions not on
    NoSpot,     ///< Chỗ đánh quái not ticked
};
SeekTick seek_tick(const HunterConfig& c);

/// A reason of the jump up's said on the card while this fresh, as the
/// drop's (simple_drop_hints).
inline constexpr std::uint64_t kJumpUpCardFreshMs = 1500;

/// The card's lines of the jump up while it hunts: each that holds, in the
/// order the card says them.
struct JumpUpCard {
    /// Off until the next start, in red: Fails, Physics, OffMap, HeldLong.
    /// Never MapChanged or KeysStuck -- those stop the hunt, and the line
    /// under BẮT ĐẦU says so in red (GuiApp::drop_stopped_vi_).
    Hunter::JumpUpOff off = Hunter::JumpUpOff::None;
    /// Keys off for the run (bit 1 << JumpKind, kJumpUpKindMisses), in red.
    std::uint8_t kinds_off = 0;
    /// What it is at: walking to a take-off, settled there, in the air; or
    /// no sure jump to the monster above it was asked of, a moment ago.
    enum class Now : std::uint8_t { None, Approach, Settle, Air, NoPlan };
    Now  now = Now::None;
    /// The tick off, and jumps it would have made this run: grey.
    bool would = false;
    /// Presses this run: how many, got up, fell back.
    bool counts = false;
};
/// From the hunter's status at `now`: the tick as set (`c.jump_up`), what it
/// is off for, at, refused a moment ago, would have done, and has done.
JumpUpCard jump_up_card(const HunterConfig& c, const Hunter::Status& hs, std::uint64_t now);

/// Why the monsters above that only a jump up gets to are not gone for, with
/// the seek on (Status::seek_above_no_way): the tick off, or jumps up off
/// until the next start.
enum class SeekAbove : std::uint8_t { None, TickOff, Latched };
SeekAbove seek_above(const HunterConfig& c, const Hunter::Status& hs);

} // namespace gobot
