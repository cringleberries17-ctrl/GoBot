#include "gobot/hunt_card.hpp"

namespace gobot {
namespace {

/// The jump key every jump up goes on: ↑.
constexpr std::uint32_t kUpVk = 0x26;

/// Ctrl, either side, by the key code's low byte (as the hunter's own).
bool is_ctrl(std::uint32_t vk) {
    const std::uint32_t low = vk & 0xFFu;
    return vk != 0 && (low == 0x11u || low == 0xA2u || low == 0xA3u);
}

} // namespace

JumpUpTick jump_up_tick(const HunterConfig& c, std::span<const std::uint32_t> own, std::size_t* own_at) {
    // In jump_ready's order, as far as the settings tell.
    if (c.stay_put) return JumpUpTick::StayPut;
    if (c.packet_positions != PacketPositions::On) return JumpUpTick::NoPackets;
    if (!c.spot) return JumpUpTick::NoSpot;
    if (c.jump_key != kUpVk) return JumpUpTick::JumpKey;
    for (std::size_t i = 0; i < own.size(); ++i) {
        const std::uint32_t vk = own[i];
        if (vk == 0 || !(is_ctrl(vk) || (vk & 0xFFu) == kUpVk)) continue;
        if (own_at != nullptr) *own_at = i;
        return JumpUpTick::OwnKey;
    }
    return JumpUpTick::Ok;
}

bool jump_up_no_ctrl(const HunterConfig& c) {
    return is_ctrl(c.left_key) || is_ctrl(c.right_key) || is_ctrl(c.attack_key);
}

SeekTick seek_tick(const HunterConfig& c) {
    // Hunter::seek_wanted's, as far as the settings tell.
    if (c.stay_put) return SeekTick::StayPut;
    if (c.packet_positions != PacketPositions::On) return SeekTick::NoPackets;
    if (!c.spot) return SeekTick::NoSpot;
    return SeekTick::Ok;
}

JumpUpCard jump_up_card(const HunterConfig& c, const Hunter::Status& hs, std::uint64_t now) {
    using Off = Hunter::JumpUpOff;
    using Phase = Hunter::JumpUpPhase;
    JumpUpCard card;
    // Those that stop the hunt say so under BẮT ĐẦU.
    if (hs.jump_off != Off::MapChanged && hs.jump_off != Off::KeysStuck) card.off = hs.jump_off;
    card.kinds_off = static_cast<std::uint8_t>(hs.jump_kinds_off & 7u);
    switch (hs.jump_phase) {
        case Phase::Approach: card.now = JumpUpCard::Now::Approach; break;
        case Phase::Settle:   card.now = JumpUpCard::Now::Settle; break;
        case Phase::Air:      card.now = JumpUpCard::Now::Air; break;
        case Phase::Idle:     break;
    }
    // No sure jump to a monster above, refused a moment ago with the tick on:
    // the way back says its own (the spot's line).
    const bool fresh = hs.jump_wait_ms != 0 && now >= hs.jump_wait_ms && now - hs.jump_wait_ms <= kJumpUpCardFreshMs;
    if (card.now == JumpUpCard::Now::None && c.jump_up && fresh && hs.jump_wait == Hunter::JumpUpWhy::Plan &&
        hs.jump_wait_for != Hunter::JumpUpFor::Home)
        card.now = JumpUpCard::Now::NoPlan;
    card.would = !c.jump_up && hs.jumps_would > 0;
    card.counts = hs.jumps > 0;
    return card;
}

SeekAbove seek_above(const HunterConfig& c, const Hunter::Status& hs) {
    if (hs.seek_above_no_way <= 0) return SeekAbove::None;
    if (!c.jump_up) return SeekAbove::TickOff;
    return SeekAbove::Latched;
}

} // namespace gobot
