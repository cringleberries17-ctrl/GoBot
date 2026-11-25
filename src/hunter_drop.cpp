// The hunter's drop through a platform: Ctrl + ↓ to the target on a floor
// below -- "Nhảy xuống bậc dưới", HunterConfig::drop_down. The drop plan's
// §3, §4 and §9, with the player's answers of 2026-11-04.
//
// When: drop_rule, pure, in the plan's order. How: a settle -- the walk let go
// of, its stop reported -- then the key through the hunter's Hands
// (InputManager::drop_ctrl_down), with the coordinator's token taken only when
// nobody holds it. What came of it: judged from the packets alone, the
// landing against the floor the game's own map file said (GameMapBook,
// MapTrust). A map change within kPressWatchMs of a press, whatever was
// judged of it, learns both ends as gates and the hunt goes on (2026.11.24;
// until then it halted the hunter for GoBot's window to stop the hunt and
// untick drop_down).
//
// Never a stop (2026.11.24, the owner: "không, 2. Nhảy xuống (Ctrl+↓) khi
// hỏng thì thử lại luôn, hoặc dừng ra skill rồi nhảy xuống chứ không phải là
// dừng 90s"): a press that did not drop is tried again at the next look --
// the first of a row after kRetryPassOverMs, every other after one fight turn
// (kRetryFightMs) -- the place it failed at skipped kLearnedForgetMs so the
// next try comes from another column, and nothing pauses the drops, switches
// them off, unticks them or stops the hunt.
//
// Nothing here turns the character, nor writes which way it faces or the
// keys that set it: the turn and facing_ are the in-reach branch's alone.

#include "gobot/hunter.hpp"
#include "gobot/common.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/jump_plan.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace gobot {
namespace {

int round_px(float v) { return static_cast<int>(std::lround(v)); }

/// Ctrl, either side, by the key code's low byte: a key the game would read
/// held with ↓ -- or that GoBot's own Ctrl would read as pressed.
bool is_ctrl(std::uint32_t vk) {
    const std::uint32_t low = vk & 0xFFu;
    return low == 0x11u || low == 0xA2u || low == 0xA3u;
}

/// "1st", "2nd", "3rd", "4th".
std::string nth(int n) {
    const char* end = n == 1 ? "st" : n == 2 ? "nd" : n == 3 ? "rd" : "th";
    return std::to_string(n) + end;
}

/// What MapTrust went by, for the press's line.
const char* trust_text(MapTrust::How how) {
    switch (how) {
        case MapTrust::How::Entry:  return "map entry by a gate";
        case MapTrust::How::Points: return "standing points on its floors";
        case MapTrust::How::Named:  return "named from standing points";
        case MapTrust::How::None:   break;
    }
    return "not trusted";
}

/// The landing's floor -- its top land_y under x0 -- followed m columns `dir`,
/// across a hole as the game walks over it, and at the column it comes to the
/// first floor more than kDropFeetPx below the character's own there (its
/// floor from x0, y0 followed, or y0 past that floor's end): the same floor,
/// kDropSameFloorPx up or down. No floor between, and the landing's not
/// ending, within m -- on a slope as on the flat: t52_s2's ramp and ground
/// run 0.4 to 0.74 px a column, and "the same height either side" failed on
/// every one of them, landing or standing (2026.11.04d).
bool landing_holds(const GameMapBook::Page& p, int x0, int y0, int land_y, int dir, int m) {
    // One rule with the way back's drops (2026.11.12 review, hunt_zone.cpp).
    return drop_landing_holds(p, x0, y0, land_y, dir, m);
}

/// A plain ↑ in place where a drop lands -- the bar's, ±kBarUpMarginPx as
/// bar_up_landings looks -- comes down on the landing's own floor or the
/// monster's (`theirs` at tx, followed there) and on no other (2026.11.12c
/// review). Another platform within its reach -- the one the fall went
/// through, most often -- and the ↑ lands there, a Ctrl+↓ takes it back down,
/// and round again: t23_s14, 1390 over 1465 at x 1700, the monster on the
/// small 1425 at 1745.
bool one_up_holds(const GameMapBook::Page& p, int x0, int land_top, int theirs, int tx) {
    const int reach = jump_peak(JumpKind::Up).hi + kJumpUpClearPx;
    for (int dx = -kBarUpMarginPx; dx <= kBarUpMarginPx; dx += kBarUpMarginStepPx) {
        const int cx = x0 + dx;
        const std::optional<int> own = dx == 0 ? std::optional<int>(land_top) : p.follow_to(x0, land_top - 1, cx);
        const std::optional<int> mon = p.follow_to(tx, theirs - 1, cx);
        for (const MapFloor& f : p.floors_at(cx)) {
            const int rise = land_top - f.y;
            if (rise <= kJumpUpFeetPx || rise > reach || (own && *own - f.y <= kJumpUpFeetPx)) continue;
            // Solid ground, walls and GoC's blocks: a bump, no landing.
            if (f.attr == 2 || f.attr == 4 || f.attr == 5 || f.attr == 6) continue;
            if (mon && std::abs(f.y - *mon) <= Hunter::kDropSameFloorPx) continue;
            return false;
        }
    }
    return true;
}

/// The minute line's words for what refused a look at a target below.
const char* drop_why_short(Hunter::DropWhy w) {
    switch (w) {
        case Hunter::DropWhy::LevelMonster: return "a monster on the level";
        case Hunter::DropWhy::TooFarSide:   return "too far across";
        case Hunter::DropWhy::NotKnown:     return "you not known";
        case Hunter::DropWhy::AfterEntry:   return "just after a map entry";
        case Hunter::DropWhy::NotSettled:   return "not settled";
        case Hunter::DropWhy::InAir:        return "in the air";
        case Hunter::DropWhy::TooSoon:      return "too soon";
        case Hunter::DropWhy::StepJudge:    return "a step back";
        case Hunter::DropWhy::Walking:      return "walking";
        case Hunter::DropWhy::MinuteCap:    return "minute's used up";
        case Hunter::DropWhy::TargetCap:    return "monster's used up";
        case Hunter::DropWhy::Held:         return "a hold";
        case Hunter::DropWhy::Unfocused:    return "the game behind";
        case Hunter::DropWhy::Loot:         return "pick-ups";
        case Hunter::DropWhy::Veto:         return "GoBot's veto";
        case Hunter::DropWhy::MapUnknown:   return "map not trusted";
        case Hunter::DropWhy::OffFloor:     return "off its floors";
        case Hunter::DropWhy::Bottom:       return "the bottom ground";
        case Hunter::DropWhy::NoLanding:    return "no landing";
        case Hunter::DropWhy::ShortFall:    return "short fall";
        case Hunter::DropWhy::PastMonster:  return "past the monster";
        case Hunter::DropWhy::OutsideArea:  return "outside the area";
        case Hunter::DropWhy::Gate:         return "gate";
        case Hunter::DropWhy::NoDropHere:   return "no drop here before";
        case Hunter::DropWhy::AfterJump:    return "after a jump up";
        default:                            return Hunter::drop_why_name(w);
    }
}

/// What the player can do something about: said at INFO, once per target.
bool said_at_info(Hunter::DropWhy w) {
    switch (w) {
        case Hunter::DropWhy::Gate:
        case Hunter::DropWhy::MapUnknown:
        case Hunter::DropWhy::PastMonster:
        case Hunter::DropWhy::Bottom:
        case Hunter::DropWhy::NoDropHere:
        case Hunter::DropWhy::Spot:
        case Hunter::DropWhy::OutsideArea:
        case Hunter::DropWhy::KeyClash:
            return true;
        default:
            return false;
    }
}

} // namespace

const char* Hunter::drop_why_name(DropWhy w) {
    switch (w) {
        case DropWhy::None:            return "it goes";
        case DropWhy::Off:             return "drop_down is off";
        case DropWhy::NoPackets:       return "the packets do not steer (packet_positions is not on)";
        case DropWhy::StayPut:         return "Đứng yên một chỗ is ticked";
        case DropWhy::Spot:            return "the hunting spot stands for want of a position";
        case DropWhy::KeysOwed:        return "a drop's key-up has not gone out";
        case DropWhy::KeyClash:        return "the jump, left, right or attack key is Ctrl";
        case DropWhy::NotWired:        return "no map book or no veto";
        case DropWhy::NotPacketTarget: return "the target is the screen's, not the packets'";
        case DropWhy::LevelMonster:    return "a monster on your own level";
        case DropWhy::NotBelow:        return "not far enough below";
        case DropWhy::TooFarSide:      return "too far to the side";
        case DropWhy::NotKnown:        return "the packets do not know where you are";
        case DropWhy::AfterEntry:      return "a map entry a moment ago";
        case DropWhy::NotSettled:      return "not standing settled yet";
        case DropWhy::InAir:           return "in the air";
        case DropWhy::TooSoon:         return "the last drop a moment ago";
        case DropWhy::StepJudge:       return "a step back not judged yet";
        case DropWhy::Walking:         return "a direction key is down";
        case DropWhy::MinuteCap:       return "the drops of a minute used up";
        case DropWhy::TargetCap:       return "the drops for this monster used up";
        case DropWhy::Held:            return "a hold";
        case DropWhy::Unfocused:       return "the game is not in front";
        case DropWhy::Loot:            return "pick-ups still owed";
        case DropWhy::Veto:            return "GoBot says not now";
        case DropWhy::MapUnknown:      return "the map is not known, or its file not trusted";
        case DropWhy::OffFloor:        return "where you stand is on no floor of the map file";
        case DropWhy::Bottom:          return "you stand on the bottom ground";
        case DropWhy::NoLanding:       return "no floor below, or not the same one either side";
        case DropWhy::ShortFall:       return "the floor below is too near";
        case DropWhy::PastMonster:     return "the fall would land below the monster's floor";
        case DropWhy::OutsideArea:     return "outside the hunting area";
        case DropWhy::Gate:            return "a gate is near";
        case DropWhy::NoDropHere:      return "Ctrl+Down did not drop here a moment ago (another column first)";
        case DropWhy::AfterJump:       return "a jump up toward a monster worked a moment ago";
    }
    return "?";
}

bool Hunter::drop_lasting(DropWhy w) {
    // From the same standing point the next look refuses again: a gate, the
    // floors under and below, the area, no drop here before, the monster's
    // drops used up. Not the clock, a hold, a walk or the packets.
    switch (w) {
        case DropWhy::Gate:
        case DropWhy::NoDropHere:
        case DropWhy::NoLanding:
        case DropWhy::ShortFall:
        case DropWhy::PastMonster:
        case DropWhy::Bottom:
        case DropWhy::OutsideArea:
        case DropWhy::TargetCap:
            return true;
        default:
            return false;
    }
}

std::optional<int> Hunter::target_floor(const GameMapBook::Page& p, bool stood, MapPoint stand, int tx, int ty,
                                         int depth) {
    // The floor it last stood on, followed to its column -- a walk moves a
    // monster's x alone, so its point may be as far off that floor as the
    // slope it walked, a pixel a column at most, and kDropMonsterFloorPx
    // more; further, it has gone elsewhere since.
    if (stood) {
        const std::optional<MapFloor> on = p.floor_near(stand.x, stand.y);
        if (on && !on->wall()) {
            const std::optional<int> top = p.follow_to(stand.x, stand.y, tx);
            const int slack = depth + std::abs(tx - stand.x);
            if (top && std::abs(*top - ty) <= slack) return top;
        }
    }
    return monster_floor(p, tx, ty, depth);
}

void Hunter::set_map_book(GameMapBook* book) {
    book_ = book;
    trust_.set_book(book);
}

void Hunter::set_drop_veto(std::function<const char*()> veto) { drop_veto_ = std::move(veto); }

bool Hunter::drop_wanted(const HunterConfig& cfg) {
    return cfg.drop_down && cfg.packet_positions == PacketPositions::On && !cfg.stay_put;
}

bool Hunter::drops_may_go(const HunterConfig& cfg) const {
    // No latch since 2026.11.24: nothing switches the drops off.
    return drop_wanted(cfg) && trust_.trusted() && (!cfg.spot || spot_mode_ == SpotMode::Use);
}

bool Hunter::drop_stop_above(const HunterConfig& cfg, const Nameplate& target, int dx, int dy) const {
    // Q6 (a): walked by dx's sign, the character never stops above it -- every
    // judging of the drop was of a walk going on, 24 px either side, at
    // whichever column the walk had got to. (b), kDropStopAbovePx 0: never.
    if (kDropStopAbovePx <= 0 || target.slot < 0 || !drops_may_go(cfg)) return false;
    return dy > cfg.down() && std::abs(dx) <= kDropStopAbovePx;
}

void Hunter::note_stands(const std::vector<MapTrust::Mob>& mobs) {
    for (const MapTrust::Mob& m : mobs) {
        MobStand* s = nullptr;
        for (MobStand& e : mob_stands_)
            if (e.slot == m.slot) s = &e;
        if (s == nullptr) {
            if (mob_stands_.size() >= 128) mob_stands_.erase(mob_stands_.begin());
            mob_stands_.push_back(MobStand{});
            s = &mob_stands_.back();
            s->slot = m.slot;
            s->gen = m.gen;
            s->y = m.at.y;
            s->y_ms = s->last_ms = m.at_ms;
            continue;
        }
        // Another monster in its slot: nothing of the last one's stands.
        if (s->gen != m.gen) {
            *s = MobStand{};
            s->slot = m.slot;
            s->gen = m.gen;
            s->y = m.at.y;
            s->y_ms = s->last_ms = m.at_ms;
            continue;
        }
        if (m.at_ms == s->last_ms) continue;     // the same frame again
        s->last_ms = m.at_ms;
        if (std::abs(m.at.y - s->y) > MapTrust::kMobSameYPx) {
            s->y = m.at.y;
            s->y_ms = m.at_ms;
        } else if (m.at_ms >= s->y_ms + MapTrust::kMobStandMs) {
            s->stood = true;
            s->at = m.at;
        }
    }
}

bool Hunter::level_monster(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                           const std::vector<Listed>& listed) {
    // On "Đánh trước" or not, the target below waits for any on the level:
    // "Đánh trước" is first among those on its level, as choose_target has it.
    for (std::size_t i = 0; i < mobs.size(); ++i) {
        const Nameplate& n = mobs[i];
        if (n.slot < 0 || n.hp <= cfg.dead_below) continue;
        if (i < listed.size() && refused(listed[i])) continue;
        if (!level_with(cfg, n.y() - me.y())) continue;
        return true;
    }
    return false;
}

bool Hunter::level_before_drop(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                               const std::vector<Listed>& listed, int dx) {
    if (!level_monster(cfg, mobs, me, listed)) return false;
    if (!kBarNearestFirst) return true;
    for (std::size_t i = 0; i < mobs.size(); ++i) {
        const Nameplate& n = mobs[i];
        if (n.slot < 0 || n.hp <= cfg.dead_below || (i < listed.size() && refused(listed[i]))) continue;
        if (!level_with(cfg, n.y() - me.y())) continue;
        if (in_reach(cfg, n.x() - me.x(), n.y() - me.y()) || std::abs(n.x() - me.x()) <= std::abs(dx)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- the rule

Hunter::DropCall Hunter::drop_rule(const HunterConfig& cfg, const DropFacts& f) {
    DropCall c;
    const auto no = [&c](DropWhy why, int px = 0) {
        c.why = why;
        c.px = px;
        return c;
    };

    // 1. Switched on: the packets steering, not standing still -- with the
    // spot on, only while it is in use. Never off for the run since
    // 2026.11.24: what did not drop is tried again.
    if (!cfg.drop_down) return no(DropWhy::Off);
    if (cfg.packet_positions != PacketPositions::On) return no(DropWhy::NoPackets);
    if (cfg.stay_put) return no(DropWhy::StayPut);
    if (cfg.spot && !f.spot_use) return no(DropWhy::Spot);
    if (f.owed) return no(DropWhy::KeysOwed);
    // 2. No key of the hunter's on Ctrl: Ctrl + ↓ would be pressed with it.
    for (const std::uint32_t vk : { cfg.jump_key, cfg.left_key, cfg.right_key, cfg.attack_key })
        if (vk != 0 && is_ctrl(vk)) return no(DropWhy::KeyClash, static_cast<int>(vk));
    // 3. The map book and GoBot's say: without them, never.
    if (!f.wired) return no(DropWhy::NotWired);
    // 4. The packets' own target. A drop of the way back (f.home, 2026.11.12
    // review) has none: 4 to 7 are a monster's.
    if (!f.home && !f.packet_target) return no(DropWhy::NotPacketTarget);
    // 5. Nothing to fight on the character's own level.
    if (!f.home && f.level_monster) return no(DropWhy::LevelMonster);
    // 6. Below, past the band: the lower bar alone (2026.11.12, the owner's
    // Q7 -- drop_min_px, at least 100 below until then, is gone).
    if (!f.home && f.dy <= cfg.down()) return no(DropWhy::NotBelow, f.dy);
    // 7. Across.
    const int side = cfg.drop_side_px + (f.press ? kDropPressSlackPx : 0);
    if (!f.home && std::abs(f.dx) > side) return no(DropWhy::TooFarSide, std::abs(f.dx));
    // 8. The packets know where the character is -- settled, its stop
    // reported, at the press -- and not just after a map entry.
    const WorldFix& fix = f.fix;
    if (fix.state != WorldFix::State::Known || fix.uncertain) return no(DropWhy::NotKnown);
    if (f.now < f.entry_ms + kDropAfterEntryMs)
        return no(DropWhy::AfterEntry, static_cast<int>(f.entry_ms + kDropAfterEntryMs - f.now));
    if (f.press && (!fix.settled() || (f.walk_held && fix.report_ms < f.settle_from_ms)))
        return no(DropWhy::NotSettled);
    // 9. The hunter's own clock.
    if (f.in_air) return no(DropWhy::InAir);
    if (f.last_press_ms != 0 && f.now < f.last_press_ms + kDropGapMs) return no(DropWhy::TooSoon);
    // A jump up toward a target that worked a moment ago: a drop would take
    // the character back down the floor it jumped onto (2026.11.05b).
    if (f.last_jump_worked_ms != 0 && f.now < f.last_jump_worked_ms + kJumpUpStayMs)
        return no(DropWhy::AfterJump, static_cast<int>(f.last_jump_worked_ms + kJumpUpStayMs - f.now));
    if (f.step_judge) return no(DropWhy::StepJudge);
    if (f.press && (f.left_held || f.right_held)) return no(DropWhy::Walking);
    // 10. Limits.
    if (f.presses_minute >= kDropsPerMinute) return no(DropWhy::MinuteCap, f.presses_minute);
    if (f.presses_target >= kDropsPerTarget) return no(DropWhy::TargetCap, f.presses_target);
    if (f.settles_target >= kDropSettlesPerTarget) return no(DropWhy::TargetCap, f.settles_target);
    // 11. No hold, the game in front, no pick-ups owed, and GoBot not against.
    if (f.held) return no(DropWhy::Held);
    if (!f.focused) return no(DropWhy::Unfocused);
    if (f.loot_owed) return no(DropWhy::Loot);
    if (f.veto != nullptr) return no(DropWhy::Veto);
    // 12. A trusted map, and -- at the press -- the character on its floor.
    if (!f.page) return no(DropWhy::MapUnknown);
    const GameMapBook::Page& page = *f.page;
    c.x0 = round_px(f.press ? fix.rx : fix.x);
    c.y0 = round_px(f.press ? fix.ry : fix.y);
    // As a settle begins, the fix is worked out along a walk, and its height
    // stays the last report's: on a slope the floor the report stood on,
    // followed to here, is where the character is (2026-11-04 20:40:01, 70
    // px under t52_s2's ramp).
    if (!f.press) {
        const int rx = round_px(fix.rx), ry = round_px(fix.ry);
        const std::optional<MapFloor> was = page.floor_near(rx, ry);
        if (was && !was->wall())
            if (const std::optional<int> top = page.follow_to(rx, ry, c.x0)) c.y0 = *top - 1;
    }
    c.tx = round_px(f.target_wx);
    c.ty = round_px(f.target_wy);
    c.placed = true;
    // The monster's floor: where it last stood, or the first floor at or
    // under its point -- its point alone is often in the air. Looked up here,
    // so every refusal from here on says it (drop_refused).
    const std::optional<int> theirs = target_floor(page, f.target_stood, f.target_stand, c.tx, c.ty);
    c.target_floor = theirs ? *theirs : -1;
    const int x0 = c.x0, y0 = c.y0;
    const std::optional<MapFloor> under = page.floor_near(x0, y0);
    if (f.press && !under) return no(DropWhy::OffFloor);
    // 13. Not the bottom ground: only a platform is dropped through.
    if (under) {
        c.stand_y = under->y;
        if (!under->platform()) return no(DropWhy::Bottom, under->attr);
    }
    // 14. Where the fall lands: the first floor below, a fall a landing is
    // told by (kDropMinFallPx, 25 px since 2026.11.12), not past the monster
    // -- and the same floor either side.
    const std::optional<MapFloor> land = page.floor_below(x0, y0, kDropFeetPx);
    if (!land) return no(DropWhy::NoLanding);
    c.land_y = land->y;
    if (land->y - y0 < kDropMinFallPx) return no(DropWhy::ShortFall, land->y - y0);
    // The landing is on the monster's floor when the landing's floor,
    // followed to the monster, is it: on a slope the two are as far apart in
    // height as across. Otherwise past it when lower than it.
    const int floor_y = theirs ? *theirs : c.ty;
    if (theirs) {
        const std::optional<int> there = page.follow_to(x0, land->y - 1, c.tx);
        c.direct = there && std::abs(*there - *theirs) <= kDropSameFloorPx;
    } else {
        c.direct = std::abs(land->y - c.ty) <= kDropFloorMatchPx;
    }
    // Below it within one plain ↑ (2026.11.12c, the owner's Q4, kDropBelowOneUp):
    // the ↑ past the upper bar takes it from there -- only where that ↑, in
    // place on the landing, comes down on its own floor or the monster's and
    // on no other (one_up_holds, the 2026.11.12c review): else it lands on
    // the floor the fall went through and the two go round, a drop and a ↑,
    // never at the monster.
    if (!c.direct && land->y > floor_y + kDropFloorMatchPx) {
        const bool one_up = kDropBelowOneUp && f.bar_ok && theirs &&
                            land->y <= floor_y + std::max(kDropFloorMatchPx, f.below_one_up_px) &&
                            one_up_holds(page, x0, land->y, *theirs, c.tx);
        if (!one_up) return no(DropWhy::PastMonster, land->y - floor_y);
        c.one_up = true;
    }
    // kDropMarginSettlePx only for a place worked out along a walk, where the
    // stop may come a step on; standing, it is the report's own, and the
    // press's margin holds as it begins. At 24 px the player's own 06:48:49
    // -- 17 px in from the end of the monster's floor -- would never settle.
    // Each side by the floors as they run, sloping or flat (landing_holds).
    const int margin = f.press || fix.settled() ? kDropMarginPressPx : kDropMarginSettlePx;
    for (const int dir : { -1, 1 })
        if (!landing_holds(page, x0, y0, land->y, dir, margin)) return no(DropWhy::NoLanding, dir * margin);
    // The player's answer 2: with the hunting spot on, never a drop that
    // leaves its area -- where it stands, where it lands and the target all
    // inside it: x +- half_w and y +- half_h round the spot taken where the
    // character stood, the map tab's own box for its area (Spot::inside).
    // Not for the way back's own, outside it already: planned in the whole
    // map, as a travel's jumps are (2026.11.12 review).
    if (cfg.spot && !f.home) {
        const Spot& a = f.area;
        const auto inside = [&a](float x, float y) { return a.inside(x, y); };
        if (!inside(static_cast<float>(x0), static_cast<float>(y0)) ||
            !inside(static_cast<float>(x0), static_cast<float>(land->y)) || !inside(f.target_wx, f.target_wy))
            return no(DropWhy::OutsideArea);
    }
    // 15. No gate across within drop_gate_px from a little above where it
    // stands to a little below where it lands: the standing point, the fall
    // and the landing in one.
    int nearest = -1;
    for (const MapGate& g : f.gates) {
        if (g.y < y0 - kDropGateSpanPx || g.y > land->y + kDropGateSpanPx) continue;
        const int across = std::abs(g.x - x0);
        if (nearest < 0 || across < nearest) nearest = across;
    }
    c.gate_px = nearest;
    if (nearest >= 0 && nearest <= cfg.drop_gate_px) return no(DropWhy::Gate, nearest);
    // 16. Not where Ctrl + ↓ did not drop a moment ago: since 2026.11.24 a
    // place is skipped kLearnedForgetMs, the next try from another column.
    if (f.no_drop_here) return no(DropWhy::NoDropHere);
    return c;
}

Hunter::DropFacts Hunter::drop_facts(const HunterConfig& cfg, const Nameplate& target, const Nameplate& me,
                                     const std::vector<Nameplate>& mobs, const std::vector<Listed>& listed, int dx,
                                     int dy, std::uint64_t now, bool press) {
    DropFacts f;
    f.press = press;
    f.now = now;
    f.spot_use = spot_mode_ == SpotMode::Use;
    if (f.spot_use) f.area = spot();
    f.owed = owed_passes_ > 0;
    f.wired = book_ != nullptr && static_cast<bool>(drop_veto_);
    f.packet_target = target.slot >= 0;
    f.dx = dx;
    f.dy = dy;
    f.fix = drop_fix_on_ ? drop_fix_ : WorldFix{};
    // The target in the packets' world: the fix, and the plate's distance
    // from yours laid back over the world's pixels, as mark_out_of_area does.
    const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
    f.target_wx = static_cast<float>(f.fix.x + (target.x() - me.x()) * scale);
    f.target_wy = static_cast<float>(f.fix.y + (target.y() - me.y() - cfg.packet_dy) * scale);
    // A monster on the level first -- with kBarNearestFirst (the owner's Q1 of
    // 2026.11.12c) only one in reach, or nearer across than the target.
    f.level_monster = level_before_drop(cfg, mobs, me, listed, dx);
    f.bar_ok = bar_up_may_go(cfg);
    f.below_one_up_px = static_cast<int>(std::lround((cfg.up() + bar_air(cfg)) * scale));
    f.entry_ms = drop_entry_on_ ? drop_entry_ms_ : now;
    f.settle_from_ms = drop_.settle_from_ms;
    f.walk_held = drop_.walk_held;
    f.in_air = in_air(now);
    f.last_press_ms = last_press_ms_;
    f.last_jump_worked_ms = last_jump_worked_ms_;
    f.step_judge = back_.judge;
    f.left_held = cfg.left_key != 0 && is_down(cfg.left_key);
    f.right_held = cfg.right_key != 0 && is_down(cfg.right_key);
    f.presses_minute = drop_presses_minute(now);
    f.presses_target = drop_presses_for(target.slot, target.gen);
    f.settles_target = drop_settles_for(target.slot, target.gen);
    f.held = coord_.critical_active();
    f.focused = focused();
    f.loot_owed = cfg.loots() && loot_left_ > 0;
    f.veto = drop_veto_ ? drop_veto_() : nullptr;
    f.page = trust_.page();
    if (f.page && book_) {
        f.gates = book_->gates(f.page->stem);
        const MapPoint here{ round_px(press ? f.fix.rx : f.fix.x), round_px(press ? f.fix.ry : f.fix.y) };
        f.no_drop_here = book_->no_drop_near(f.page->stem, here);
    }
    // Where the target last stood, by its own frames: its floor, whatever its
    // point says now.
    if (target.slot >= 0)
        for (const MobStand& s : mob_stands_)
            if (static_cast<int>(s.slot) == target.slot && s.gen == target.gen && s.stood) {
                f.target_stood = true;
                f.target_stand = s.at;
            }
    return f;
}

int Hunter::drop_presses_minute(std::uint64_t now) {
    drop_presses_.erase(std::remove_if(drop_presses_.begin(), drop_presses_.end(),
                                       [now](std::uint64_t t) { return now >= t + 60'000; }),
                        drop_presses_.end());
    return static_cast<int>(drop_presses_.size());
}

int Hunter::drop_presses_for(int slot, std::uint32_t gen) const {
    // Only those of the last kTargetCountMs (2026.11.24): a monster's two
    // presses no longer refuse it for the rest of the run.
    const std::uint64_t now = clock_now();
    for (const TargetPresses& t : drop_targets_)
        if (t.slot == slot && t.gen == gen) return t.fresh(now) ? t.n : 0;
    return 0;
}

int Hunter::drop_settles_for(int slot, std::uint32_t gen) const {
    const std::uint64_t now = clock_now();
    for (const TargetPresses& t : drop_targets_)
        if (t.slot == slot && t.gen == gen) return t.fresh(now) ? t.settles : 0;
    return 0;
}

// ---------------------------------------------------------------- a pass

void Hunter::drop_watch(const HunterConfig& cfg, std::uint64_t now) {
    bool suspend = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        suspend = drop_suspend_asked_;
        drop_suspend_asked_ = false;
    }
    if (press_.open && now >= press_.down_ms + kPressWatchMs) press_.open = false;
    if (press_other_.open && now >= press_other_.down_ms + kPressWatchMs) press_other_.open = false;
    const bool watched = press_.open || press_other_.open;
    drop_fix_ = WorldFix{};
    drop_fix_on_ = false;
    // Logged back in: where the character stands may be another map's.
    if (suspend) {
        trust_.lose("logged back in");
        drop_hold("logged back in");
        jump_hold("logged back in");
    }
    // The jumps up watch the same map, the same press record and the same
    // owed keys: with the map tab's area in use and the packets steering,
    // jump_up on or off -- off, for the "would jump" lines (2026.11.05b).
    // And since 2026.11.12 with the hunting spot on and the packets steering:
    // this run's spot goes by its map's file once trusted (the owner's Q4: the
    // way back, the jumps up), and GoBot's walks there, and the dash, are kept
    // from a pit's edge by it (Q5). Hunting with no spot and drops off asks
    // nothing new, as before.
    const bool drops = drop_wanted(cfg);
    const bool jumps = jump_watch(cfg);
    const bool on = drops || jumps || (cfg.spot && cfg.packet_positions == PacketPositions::On);
    // Switched off mid-drop, or mid-jump, with no press to watch: whatever
    // was under way is over.
    if (!drops && !watched) drop_hold("drop_down switched off");
    if (!jumps && !watched) jump_hold("no area to jump in");
    // The keys a drop owes, sent again at every pass: still owed for
    // kDropOwedPasses, ↓ or Ctrl may be stuck down in Windows -- a walk onto a
    // gate with ↓ down changes the map. Since 2026.11.24 no stop: every key
    // is let go of, once an episode, and the pass holds still (pass()) until
    // the key-ups go out -- SendInput refuses every key then anyway (a locked
    // desktop, UAC) -- then the hunt goes on by itself. Until then the hunter
    // halted for GoBot's window to stop the hunt.
    const bool settled = hands_.settle_owed ? hands_.settle_owed() : input_.settle_owed();
    if (settled) {
        if (owed_said_) GB_INFO("Hunter: the owed key-ups went out -- hunting goes on");
        const bool was_stuck = owed_said_;
        owed_passes_ = 0;
        owed_said_ = false;
        if (was_stuck) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.drop_keys_stuck = false;
        }
    } else if (++owed_passes_ >= kDropOwedPasses && !owed_said_) {
        owed_said_ = true;
        // Whose keys: a high jump's ↑ (no ↓ goes down there), or a drop's.
        const bool up = hands_.up_owed ? hands_.up_owed() : input_.up_owed();
        // Every key let go of: the hunter's own hands when it has them, else
        // the input's every held key and owed up, then a ↓, ↑ or Ctrl Windows
        // still reads down -- ups only, whatever is in front.
        release_movement();
        if (!hands_.release_all) input_.release_stray_drop_keys();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.drop_keys_stuck = true;
        }
        GB_WARN("Hunter: keys owed after a %s (%s or Ctrl up not sent, %d passes) -- every key let go of; nothing "
                "pressed until Windows takes the key-ups, then hunting goes on by itself",
                up ? "jump up" : "drop", up ? "Up" : "Down", owed_passes_);
    }
    // The owed keys above go before this return (2026.11.24 review): pass()
    // holds the hunter still while owed_passes_ >= kDropOwedPasses, and with
    // drop_down unticked at the amber note, no spot and no jump area, the
    // count was never cleared when it sat below -- the hunter idle for the
    // rest of the run. Whatever is ticked, they are sent and the count cleared
    // once they went out.
    if (!on && !watched) {
        // Nothing new is asked of the eyes.
        return;
    }

    // Where the packets put the character, for the drop: asked here, once --
    // or the spot's own look of this pass, asked already (2026.11.12: with
    // the spot on the map is looked for too, and the eyes are asked once a
    // pass all the same).
    if (fix_pass_ == passes_) {
        drop_fix_ = fix_;
        drop_fix_on_ = fix_got_;
    } else {
        drop_fix_on_ = eyes_.where && eyes_.where(drop_fix_);
    }
    if (!drop_fix_on_) drop_fix_ = WorldFix{};
    const WorldFix& f = drop_fix_;
    // The map entry the character is in, and since when: the first look at
    // one -- a start's too -- is where its age counts from.
    if (drop_fix_on_ && (!drop_entry_on_ || f.epoch != drop_entry_epoch_)) {
        drop_entry_on_ = true;
        drop_entry_epoch_ = f.epoch;
        drop_entry_ms_ = now;
    }
    // A map change within kPressWatchMs of a press, however late its entry
    // came and whatever was judged of the press: the press's -- and a press
    // of the other kind still watched may as well have been it: both undone
    // and learned as gates, the gate stored once for the two (the window
    // never reads one before the other) -- and the hunt goes on (2026.11.24:
    // nothing switched off, unticked or stopped).
    if (drop_fix_on_) {
        const bool older = press_other_.open && f.epoch != press_other_.epoch0;
        const bool newest = press_.open && f.epoch != press_.epoch0;
        // 2026.11.21 (the audit's B3): a change the expedition said it would
        // make -- its gate's ↓, an NPC's teleport, the dungeon's door or way
        // out, the game moving the party on -- is no press's. Seconds after a
        // trip walk's drop it was blamed on the drop: a gate learned where
        // there is none, the hunt stopped, Nhảy xuống unticked and saved, and
        // the expedition left waiting for Tự đánh quái for good. The press is
        // forgotten unjudged; a fall or a flight still under way is ended.
        bool expected = false;
        if (older || newest) {
            std::lock_guard<std::mutex> lock(mutex_);
            expected = now < map_change_until_ms_;
        }
        if (expected) {
            const PressRecord& r = newest ? press_ : press_other_;
            GB_INFO("Hunter: the map changed %.1f s after the %s at %d,%d -- the expedition's own map change, the "
                    "press forgotten (nothing learned)",
                    static_cast<double>(now > r.down_ms ? now - r.down_ms : 0) / 1000.0,
                    r.jump ? "jump up" : "drop", r.x0, r.y0);
            press_.open = false;
            press_other_.open = false;
            drop_hold("the expedition changed the map");
            jump_hold("the expedition changed the map");
        } else {
            if (older) drop_gate(press_other_, now);
            if (newest) drop_gate(press_, now);
            if (older && newest)
                press_gate_.store(PressGate::Both);
            else if (older || newest)
                press_gate_.store((newest ? press_.jump : press_other_.jump) ? PressGate::Jump : PressGate::Drop);
        }
    }
    if (!on) return;

    // The map drops go by: from the entry, or named from standing points --
    // the character's own, and the monsters' where two of their frames agree.
    // Only the character's own let it go, two of them (MapTrust).
    MapTrust::Look look;
    look.now_ms = now;
    look.capture = drop_fix_on_ && f.state != WorldFix::State::NoCapture && f.state != WorldFix::State::FirstReport;
    // Not a point taken in a jump up's flight: a cast's report holds its y at
    // the top (2026.11.05b).
    look.settled = f.settled() && !jump_in_air(now);
    look.rx = round_px(f.rx);
    look.ry = round_px(f.ry);
    look.report_ms = f.report_ms;
    look.map_known = f.map_known;
    look.map = f.map;
    look.area = f.area;
    look.epoch = f.epoch;
    look.entry_x = round_px(f.entry_x);
    look.entry_y = round_px(f.entry_y);
    look.report_op = f.report_op;
    look.air_reports = f.air_reports;
    look.airborne = f.airborne || jump_in_air(now);
    look.y_kept = f.y_kept;
    if (look.capture && eyes_.stands) look.mobs = eyes_.stands();
    // Where each monster last stood, from the same frames: the floor a drop
    // to it is judged by.
    note_stands(look.mobs);
    trust_.update(look);
    const MapTrust::Report report = trust_.report();
    std::lock_guard<std::mutex> lock(mutex_);
    status_.drop_map = report;
}

void Hunter::drop_follow(const HunterConfig& cfg, std::uint64_t now) {
    // A settle drop_toward did not renew at the last pass: the target came
    // within reach, went, or the pass never got that far.
    if (drop_.phase == DropPhase::Settle && drop_.renewed_pass + 1 < passes_)
        end_settle("not wanted at the last look", true, now);
    if (drop_.phase != DropPhase::Fall) return;

    const WorldFix& f = drop_fix_;
    if (!drop_fix_on_ || f.state == WorldFix::State::NoCapture || f.state == WorldFix::State::FirstReport) {
        end_fall(cfg, DropVerdict::Unknown, now, 0, 0, "no capture to say");
        return;
    }
    if (f.state == WorldFix::State::Lost) {
        end_fall(cfg, DropVerdict::Unknown, now, 0, 0, "the packets lost you");
        return;
    }
    // Took off: the game's own count of take-offs moved, or a report since
    // ↓ has the character in the air.
    if (drop_.takeoffs_known && eyes_.takeoffs) {
        const std::optional<std::uint64_t> t = eyes_.takeoffs();
        if (t && *t != drop_.takeoffs0) drop_.took_off = true;
    }
    const bool reported = f.report_ms >= drop_.down_ms;
    if (f.airborne && reported) drop_.took_off = true;
    // Landed: a report since ↓, on the ground -- lower, higher, or where it
    // stood after a take-off. Not one at y 0, which is no place (2026.11.12):
    // it would read as a rise -- nor one at y 0 whose height the packets kept
    // (WorldFix::y_kept): it would read as standing where it took off.
    if (reported && !f.airborne && f.state == WorldFix::State::Known && f.ry >= 1.0f && !f.y_kept) {
        const int rx = round_px(f.rx), ry = round_px(f.ry);
        if (ry >= drop_.y0 + kDropLandPx) {
            // Against the floor the file said, at where it came down: that
            // floor followed from under the press to the landing's column --
            // on t52_s2's ramp 16 px across is 12 lower or higher, and a good
            // landing would have read as off the map, the file distrusted.
            int floor_y = drop_.land_y;
            if (drop_.page)
                if (const std::optional<int> top = drop_.page->follow_to(drop_.x0, drop_.land_y - 1, rx)) floor_y = *top;
            if (std::abs(ry - floor_y) <= kDropFloorMatchPx || !drop_.page) {
                end_fall(cfg, std::abs(ry - floor_y) <= kDropFloorMatchPx ? DropVerdict::Worked : DropVerdict::OffMap,
                         now, rx, ry, nullptr);
                return;
            }
            // Not there: the file is let go of only for a landing on none of
            // its floors, as the jump up's verdict judges one (2026.11.12). At
            // t23_s14 22:22:01 the character, pressed at 135,852, was reported
            // at 30,931 -- on the floor it stood on, 105 px down its slope, a
            // skill's move during the press -- and the right file was dropped
            // for the rest of the run: no drops, jumps or way back for 8.5 min.
            const std::optional<int> own = drop_.page->follow_to(drop_.x0, drop_.y0, rx);
            if (own && std::abs(ry - *own) <= kDropFloorMatchPx) {
                // On the floor it stood on: where it pressed, Ctrl + ↓ did
                // nothing. Further across than a no-drop point covers, with no
                // arrow held, something else moved it -- not known, nothing
                // learned, the monster not passed over.
                if (std::abs(rx - drop_.x0) <= GameMapBook::kNoDropXPx) {
                    end_fall(cfg, DropVerdict::Nothing, now, rx, ry, nullptr);
                } else {
                    GB_INFO("Hunter: drop at %d,%d -- reported at %d,%d, %d px %s along the floor it stood on with no "
                            "arrow held: something else moved you; nothing learned, %s's file still gone by",
                            drop_.x0, drop_.y0, rx, ry, std::abs(rx - drop_.x0), rx < drop_.x0 ? "left" : "right",
                            drop_.stem.empty() ? "the map" : drop_.stem.c_str());
                    end_fall(cfg, DropVerdict::Unknown, now, rx, ry, "moved along the floor it stood on");
                }
                return;
            }
            // On none of the file's floors: perhaps still falling (2026.11.15, a
            // report of state 0 in mid-air -- a hit's -- as the jump up's
            // judge saw at 00:27:54): the next report is waited for,
            // kLandSettleMs at most, and read from the fix as it is then.
            if (!drop_.page->floor_near(rx, ry) && now < f.report_ms + kLandSettleMs &&
                now < drop_.down_ms + kDropFallMaxMs)
                return;
            end_fall(cfg, drop_.page->floor_near(rx, ry) ? DropVerdict::OtherFloor : DropVerdict::OffMap, now, rx, ry,
                     nullptr);
            return;
        }
        if (ry <= drop_.y0 - kDropLandPx) {
            end_fall(cfg, DropVerdict::Rose, now, rx, ry, nullptr);
            return;
        }
        if (drop_.took_off) {
            end_fall(cfg, DropVerdict::JumpInPlace, now, rx, ry, nullptr);
            return;
        }
    }
    if (!drop_.took_off && now >= drop_.down_ms + kDropJudgeMs) {
        end_fall(cfg, DropVerdict::Nothing, now, round_px(f.rx), round_px(f.ry), nullptr);
        return;
    }
    if (now >= drop_.down_ms + kDropFallMaxMs) end_fall(cfg, DropVerdict::Unknown, now, 0, 0, "still in the air");
}

bool Hunter::drop_toward(const HunterConfig& cfg, const Nameplate& target, const Nameplate& me,
                         const std::vector<Nameplate>& mobs, const std::vector<Listed>& listed, int dx, int dy,
                         std::uint64_t now) {
    // What refused it this pass, for the seek (drop_refused sets it).
    drop_why_pass_ = DropWhy::None;
    // Falling: held still -- no walk and no hop -- until it is judged, and
    // after a fall not judged for as long as it may still be in the air: a
    // walk would steer the fall.
    if (drop_.phase == DropPhase::Fall || drop_in_air(now)) {
        let_go_of_moves(cfg);
        return true;
    }
    // Off, or not a target below past the band -- the lower bar: walked to
    // as ever, with nothing asked.
    if (!drop_wanted(cfg) || dy <= cfg.down()) {
        if (drop_.phase == DropPhase::Settle) end_settle("the target is no longer below", true, now);
        return false;
    }
    if (drop_.phase == DropPhase::Settle && (target.slot != drop_.slot || target.gen != drop_.gen))
        end_settle("another target", false, now);
    const bool settling = drop_.phase == DropPhase::Settle;

    DropFacts facts = drop_facts(cfg, target, me, mobs, listed, dx, dy, now, settling);
    const DropCall call = drop_rule(cfg, facts);
    // For the minute line: each look at a target below -- and what refused
    // it, counted by drop_refused: a settle's wait for the stop's report is
    // no refusal, and a drop that went after it is not "not settled".
    ++minute_drop_looks_;

    if (!settling) {
        // Something to fight on the character's own level: the one below is
        // let go of -- not passed over, not counted as given up -- and the
        // next look picks the one on the level (choose_target's own rule).
        if (call.why == DropWhy::LevelMonster) {
            drop_refused(cfg, call, target, dy, now);
            end_fight("a monster on its own level");
            have_target_ = false;
            return true;
        }
        if (!call.ok()) {
            drop_refused(cfg, call, target, dy, now);
            // Only the clock in the way -- a hop or the last drop still in the
            // air, or a drop a moment ago -- and a drop from right here would
            // go: stood for, not walked off. A floor in between is dropped
            // from again where it was landed on.
            if (call.why == DropWhy::InAir || call.why == DropWhy::TooSoon) {
                facts.in_air = false;
                facts.last_press_ms = 0;
                if (drop_rule(cfg, facts).ok()) {
                    let_go_of_moves(cfg);
                    return true;
                }
            }
            return false;
        }
        if (now < settle_backoff_ms_) return false;
        // Settle: the walk let go of, and the stop it reports waited for.
        // Neither writes which way the character faces.
        drop_ = DropState{};
        drop_.phase = DropPhase::Settle;
        drop_.settle_from_ms = now;
        drop_.walk_held = walk_vk_ != 0;
        drop_.renewed_pass = passes_;
        drop_.slot = target.slot;
        drop_.gen = target.gen;
        ++minute_drop_settles_;
        let_go_of_moves(cfg);
        disengage();
        // No give-up while it settles and falls: the clock held still for the
        // settle -- given back at end_settle what it had, the settle's time
        // not counted -- and started afresh at the press.
        drop_.since0 = target_since_ms_;
        drop_.reach0 = reach_ms_;
        target_since_ms_ = reach_ms_ = now;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.dropping = true;
        }
        GB_DEBUG("Hunter: settling to drop to the monster %d px below, %d px %s -- from %d,%d to floor %d", dy,
                 std::abs(dx), dx < 0 ? "left" : "right", call.x0, call.y0, call.land_y);
        return true;
    }

    drop_.renewed_pass = passes_;
    if (call.ok()) return press_drop(cfg, call, target, dx, dy, now);
    // Waiting for the stop's report, for kDropSettleMs at most.
    if (call.why == DropWhy::NotSettled && now < drop_.settle_from_ms + kDropSettleMs) return true;
    drop_refused(cfg, call, target, dy, now);
    end_settle(call.why == DropWhy::NotSettled ? "no report of standing came" : drop_why_name(call.why), true, now);
    return false;
}

// ------------------------------------------------- the way back's own drops

bool Hunter::home_drops_may(const HunterConfig& cfg, const GameMapBook::Page& page) const {
    // No latch since 2026.11.24: the way back's Ctrl + ↓ legs are never lost
    // to one.
    if (!drop_wanted(cfg) || book_ == nullptr || !drop_veto_) return false;
    for (const std::uint32_t vk : { cfg.jump_key, cfg.left_key, cfg.right_key, cfg.attack_key })
        if (vk != 0 && is_ctrl(vk)) return false;
    const std::shared_ptr<const GameMapBook::Page> trusted = trust_.page();
    return trust_.trusted() && trusted && trusted->stem == page.stem;
}

ZoneDropRules Hunter::home_drop_rules(const HunterConfig& cfg, const GameMapBook::Page& page) const {
    ZoneDropRules r;
    r.gate_px = cfg.drop_gate_px;
    if (book_ != nullptr) {
        r.gates = book_->gates(page.stem);
        GameMapBook* book = book_;
        const std::string stem = page.stem;
        r.no_drop = [book, stem](MapPoint p) { return book->no_drop_near(stem, p); };
    }
    return r;
}

bool Hunter::home_drop(const HunterConfig& cfg, const ZoneDrop& leg, std::uint64_t now) {
    // In its fall: held still -- no key -- until drop_follow judges it.
    if (drop_.phase == DropPhase::Fall || drop_in_air(now)) {
        let_go_of_moves(cfg);
        return drop_.phase == DropPhase::Fall && drop_.home;
    }
    // A settle for a monster is not this one.
    if (drop_.phase == DropPhase::Settle && !drop_.home) end_settle("the way back's drop", false, now);
    const bool settling = drop_.phase == DropPhase::Settle;

    // The drop rule as at any drop, with no monster: the "target" the leg's
    // landing, the walk back's own count in place of a monster's.
    DropFacts f;
    f.home = true;
    f.press = settling;
    f.now = now;
    f.spot_use = spot_mode_ == SpotMode::Use;
    if (f.spot_use) f.area = spot();
    f.owed = owed_passes_ > 0;
    f.wired = book_ != nullptr && static_cast<bool>(drop_veto_);
    f.fix = drop_fix_on_ ? drop_fix_ : WorldFix{};
    f.target_wx = static_cast<float>(leg.land.x);
    f.target_wy = static_cast<float>(leg.land.y);
    f.target_stood = true;
    f.target_stand = leg.land;
    f.entry_ms = drop_entry_on_ ? drop_entry_ms_ : now;
    f.settle_from_ms = drop_.settle_from_ms;
    f.walk_held = drop_.walk_held;
    f.in_air = in_air(now);
    f.last_press_ms = last_press_ms_;
    f.last_jump_worked_ms = last_jump_worked_ms_;
    f.step_judge = back_.judge;
    f.left_held = cfg.left_key != 0 && is_down(cfg.left_key);
    f.right_held = cfg.right_key != 0 && is_down(cfg.right_key);
    f.presses_minute = drop_presses_minute(now);
    f.held = coord_.critical_active();
    f.focused = focused();
    f.loot_owed = cfg.loots() && loot_left_ > 0;
    f.veto = drop_veto_ ? drop_veto_() : nullptr;
    f.page = trust_.page();
    if (f.page && book_) {
        f.gates = book_->gates(f.page->stem);
        const MapPoint here{ round_px(settling ? f.fix.rx : f.fix.x), round_px(settling ? f.fix.ry : f.fix.y) };
        f.no_drop_here = book_->no_drop_near(f.page->stem, here);
    }
    DropCall call = drop_rule(cfg, f);
    // The walk back's own limit: kZoneTravelDropsMax presses on one.
    if (call.ok() && home_drop_presses_ >= kZoneTravelDropsMax) {
        call.why = DropWhy::TargetCap;
        call.px = home_drop_presses_;
    }

    if (!settling) {
        if (!call.ok()) {
            // Said at INFO once a kWalkKeptSayMs; standing, the give-up clock
            // running -- a lasting refusal ends as a walk back given up on.
            const LogLevel level =
                walk_kept_said_ms_ == 0 || now >= walk_kept_said_ms_ + kWalkKeptSayMs ? LogLevel::Info : LogLevel::Debug;
            if (level == LogLevel::Info) walk_kept_said_ms_ = now;
            Logger::instance().logf(level, "Hunter: the way back's drop from %d,%d not pressed for now -- %s",
                                    leg.take.x, leg.take.y, drop_why_name(call.why));
            let_go_of_moves(cfg);
            return false;
        }
        if (now < settle_backoff_ms_) return false;
        // Settle: the walk let go of, and the stop it reports waited for.
        drop_ = DropState{};
        drop_.phase = DropPhase::Settle;
        drop_.home = true;
        drop_.settle_from_ms = now;
        drop_.walk_held = walk_vk_ != 0;
        drop_.renewed_pass = passes_;
        drop_.since0 = target_since_ms_;
        drop_.reach0 = reach_ms_;
        ++minute_drop_settles_;
        let_go_of_moves(cfg);
        disengage();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.dropping = true;
        }
        GB_DEBUG("Hunter: settling to drop on the way back -- from %d,%d to floor %d", call.x0, call.y0, call.land_y);
        return true;
    }

    drop_.renewed_pass = passes_;
    if (call.ok()) {
        static const Nameplate kWayBack{};
        return press_drop(cfg, call, kWayBack, 0, call.land_y - call.y0, now);
    }
    // Waiting for the stop's report, for kDropSettleMs at most.
    if (call.why == DropWhy::NotSettled && now < drop_.settle_from_ms + kDropSettleMs) return true;
    GB_DEBUG("Hunter: the way back's drop from %d,%d not pressed -- %s", leg.take.x, leg.take.y,
             drop_why_name(call.why));
    end_settle(call.why == DropWhy::NotSettled ? "no report of standing came" : drop_why_name(call.why), true, now);
    return false;
}

bool Hunter::press_drop(const HunterConfig& cfg, const DropCall& call, const Nameplate& target, int dx, int dy,
                        std::uint64_t now) {
    const WorldFix f = drop_fix_;
    const std::string stem = trust_.page() ? trust_.page()->stem : std::string();
    std::optional<std::uint64_t> takeoffs0;
    InputManager::DropPress p;
    {
        // The token only when nobody holds it: nothing taken over, and
        // nothing but a Critical hold can take it while Ctrl is down -- which
        // go_on sees at once.
        auto token = coord_.acquire_free("hunt-drop", Priority::Combat);
        if (!token) {
            if (drop_.token_from_ms == 0) drop_.token_from_ms = now;
            if (now < drop_.token_from_ms + kDropTokenWaitMs) return true;
            end_settle("the keys stayed busy", true, now);
            return false;
        }
        takeoffs0 = eyes_.takeoffs ? eyes_.takeoffs() : std::nullopt;
        const std::function<bool()> go_on = [this] {
            return running_.load() && !coord_.critical_active() && focused();
        };
        p = hands_.drop ? hands_.drop(go_on) : input_.drop_ctrl_down(go_on);
    }
    const std::uint64_t after = clock_now();
    if (p.sent == InputManager::DropSent::NotSent || p.sent == InputManager::DropSent::Aborted) {
        GB_INFO("Hunter: drop not pressed -- %s", p.sent == InputManager::DropSent::Aborted
                                                      ? "Ctrl went up again with no Down (a hold, or the game behind)"
                                                      : "nothing was sent");
        end_settle("not pressed", true, after);
        return true;
    }

    // Down went out: counted against the limits, and watched for a map
    // change for kPressWatchMs whatever comes of it. The monster's count is
    // of its last kTargetCountMs (TargetPresses::touch, 2026.11.24), and a
    // press judged to have done nothing leaves the minute's again (end_fall).
    drop_presses_.push_back(after);
    bool counted = false;
    for (TargetPresses& t : drop_targets_)
        if (t.slot == drop_.slot && t.gen == drop_.gen) {
            t.touch(after);
            ++t.n;
            counted = true;
        }
    if (!counted) {
        if (drop_targets_.size() >= 64) drop_targets_.erase(drop_targets_.begin());
        drop_targets_.push_back(TargetPresses{ drop_.slot, drop_.gen, 1, 0, after });
    }
    const int for_it = drop_presses_for(drop_.slot, drop_.gen);
    ++minute_drops_;
    last_press_ms_ = after;
    // A jump up's press still watched kept watched beside this one; a drop's
    // left from before, closed.
    if (press_.open && press_.jump) press_other_ = press_;
    else if (!press_other_.jump) press_other_.open = false;
    press_ = PressRecord{};
    press_.open = true;
    press_.down_ms = p.down_ms;
    press_.epoch0 = f.epoch;
    press_.x0 = call.x0;
    press_.y0 = call.y0;
    press_.stem = stem;
    drop_t_down_ = p.down_ms;
    drop_landed_ = false;
    target_since_ms_ = reach_ms_ = after;

    drop_.phase = DropPhase::Fall;
    drop_.down_ms = p.down_ms;
    drop_.takeoffs_known = takeoffs0.has_value();
    drop_.takeoffs0 = takeoffs0.value_or(0);
    drop_.took_off = false;
    drop_.x0 = call.x0;
    drop_.y0 = call.y0;
    drop_.land_y = call.land_y;
    drop_.one_up = call.one_up;
    drop_.stem = stem;
    drop_.page = trust_.page();
    drop_.minute_at = after;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++status_.drops;
        status_.dropping = true;
    }

    char gate[48] = "no gate in its band";
    if (call.gate_px >= 0) std::snprintf(gate, sizeof gate, "nearest gate %d px", call.gate_px);
    if (drop_.home) {
        // The way back's own (2026.11.12 review): no monster.
        ++home_drop_presses_;
        GB_INFO("Hunter: drop (Ctrl+Down) on the way back: %d px down -- you at %d,%d on floor %d of %s (trusted: %s), "
                "landing on floor %d, %s; %d on this walk back, %d this minute; Down held %u ms%s",
                dy, call.x0, call.y0, call.stand_y, stem.c_str(), trust_text(trust_.how()), call.land_y, gate,
                home_drop_presses_, drop_presses_minute(after), p.held_ms,
                p.sent == InputManager::DropSent::Cut ? " -- cut short" : "");
    } else {
        GB_INFO("Hunter: drop (Ctrl+Down) toward %s: %d px %s, %d px down -- you at %d,%d on floor %d of %s "
                "(trusted: %s), landing on floor %d (%s), %s; nothing on your level in view; %s for it, %d this "
                "minute; Down held %u ms%s",
                target.name.empty() ? "the monster" : target.name.c_str(), std::abs(dx), dx < 0 ? "left" : "right",
                dy, call.x0, call.y0, call.stand_y, stem.c_str(), trust_text(trust_.how()), call.land_y,
                call.direct ? "the monster's" : "one between", gate, nth(for_it).c_str(), drop_presses_minute(after),
                p.held_ms, p.sent == InputManager::DropSent::Cut ? " -- cut short" : "");
    }
    // ↓ held long: a slow PC, said -- the press judged as any other, nothing
    // switched off (2026.11.24; drops off until the next start until then).
    if (p.held_ms > kDropHeldMaxMs)
        GB_WARN("Hunter: Down was held %u ms (over %u) -- a slow PC; the press is judged as any other", p.held_ms,
                kDropHeldMaxMs);
    if (p.sent == InputManager::DropSent::Cut) end_fall(cfg, DropVerdict::Unknown, after, 0, 0, "Down cut short");
    return true;
}

// ---------------------------------------------------------------- endings

void Hunter::end_settle(const char* why, bool backoff, std::uint64_t now) {
    if (drop_.phase != DropPhase::Settle) return;
    // The give-up clock as the settle found it, the settle's time not counted:
    // only what the settle itself wrote -- a fresh pick, the Đánh trước
    // switch or a hold since wrote their own. Settles that kept failing, each
    // begun 2 s after the last, would otherwise hold it back for good.
    const std::uint64_t spent = now > drop_.settle_from_ms ? now - drop_.settle_from_ms : 0;
    if (target_since_ms_ == drop_.settle_from_ms) target_since_ms_ = drop_.since0 + spent;
    if (reach_ms_ == drop_.settle_from_ms) reach_ms_ = drop_.reach0 + spent;
    // Counted for its monster: kDropSettlesPerTarget let go of without a
    // press in kTargetCountMs, and no more are begun for it until the last
    // of them is that old (2026.11.24: for the rest of the run until then).
    TargetPresses* it = nullptr;
    for (TargetPresses& t : drop_targets_)
        if (t.slot == drop_.slot && t.gen == drop_.gen) it = &t;
    if (it == nullptr) {
        if (drop_targets_.size() >= 64) drop_targets_.erase(drop_targets_.begin());
        drop_targets_.push_back(TargetPresses{ drop_.slot, drop_.gen, 0, 0, now });
        it = &drop_targets_.back();
    }
    it->touch(now);
    ++it->settles;
    drop_.phase = DropPhase::Idle;
    if (backoff) settle_backoff_ms_ = now + kDropSettleBackoffMs;
    GB_DEBUG("Hunter: drop not pressed -- %s%s", why, backoff ? "; none begun again for 2 s" : "");
    std::lock_guard<std::mutex> lock(mutex_);
    status_.dropping = false;
}

void Hunter::drop_hold(const char* why) {
    if (drop_.phase == DropPhase::Settle) end_settle(why, true, clock_now());
    else if (drop_.phase == DropPhase::Fall) end_fall(config(), DropVerdict::Unknown, clock_now(), 0, 0, why);
}

void Hunter::end_fall(const HunterConfig& cfg, DropVerdict v, std::uint64_t now, int rx, int ry, const char* why) {
    (void)cfg;
    if (drop_.phase != DropPhase::Fall) return;
    drop_.phase = DropPhase::Idle;
    // The way back is worked out again with what this one learned (2026.11.12
    // review).
    ++drop_epoch_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.dropping = false;
        if (v == DropVerdict::Worked) ++status_.drops_worked;
    }
    // What a drop that did not drop passes over: its monster -- or, the way
    // back's own, nothing: the way is worked out again without it. Never the
    // drops themselves (2026.11.24, the owner: "khi hỏng thì thử lại luôn,
    // hoặc dừng ra skill rồi nhảy xuống"): the first press of a row that did
    // not drop only kRetryPassOverMs -- the next look presses again -- and
    // every other one fight turn, kRetryFightMs, then again. Until 2026.11.24
    // the second in a row passed it over 2 x give_up_ms, and three in a row
    // paused every drop 30 / 60 / 120 s.
    const bool first_fail = drop_fails_row_ == 0;
    const std::uint64_t over_ms = first_fail ? kRetryPassOverMs : kRetryFightMs;
    char over[96];
    if (drop_.home)
        std::snprintf(over, sizeof over, "the way back worked out again");
    else if (first_fail)
        std::snprintf(over, sizeof over, "the monster passed over for %.1f s", static_cast<double>(over_ms) / 1000.0);
    else
        std::snprintf(over, sizeof over, "the monster passed over for %llu s: a fight turn, then tried again",
                      static_cast<unsigned long long>(over_ms / 1000));
    const unsigned long long ms = now > drop_.down_ms ? now - drop_.down_ms : 0;
    const unsigned long long skip_s = kLearnedForgetMs / 1000;
    const MapPoint at{ drop_.x0, drop_.y0 };
    const char* stem = drop_.stem.empty() ? "this map" : drop_.stem.c_str();
    // One press that did nothing, or jumped in place, is no evidence of the
    // place (2026.11.15): it is learned as one Ctrl + ↓ does not drop at only
    // by the second near it within kNoDropSuspectMs. A rise is another thing.
    // Learned, a place is skipped kLearnedForgetMs (2026.11.24), not the rest
    // of the session.
    const bool miss = v == DropVerdict::Nothing || v == DropVerdict::JumpInPlace;
    const bool second = miss && book_ != nullptr && !drop_.stem.empty() && drop_second_strike(drop_.stem, at, now);
    bool learn = false, fail = false;
    switch (v) {
        case DropVerdict::Worked:
            drop_landed_ = true;
            // Down a floor: the steps back counted each way are another
            // floor's, and a hop toward the target would take it back up.
            retreat_[0] = retreat_[1] = Retreat{};
            last_drop_worked_ms_ = now;
            last_drop_one_up_ = drop_.one_up;
            drop_fails_row_ = 0;
            // The place no suspect (2026.11.15).
            drop_worked_at(drop_.stem, at);
            ++minute_drop_worked_;
            GB_INFO("Hunter: drop worked: landed at %d,%d, %d px down, %llu ms after Down (%s)", rx, ry, ry - drop_.y0,
                    ms, drop_.took_off ? "took off" : "no take-off seen");
            break;
        case DropVerdict::OffMap:
            // Off the file's floors: the hunter's MapTrust lets the file go --
            // gone by again as soon as kTrustPoints standing points on its
            // floors agree, with no clock -- and nothing is switched off
            // (2026.11.24). Until then the map book distrusted it kDistrustMs
            // (120 s) and the drops were off until the next start.
            drop_landed_ = true;
            ++minute_drop_off_map_;
            trust_.lose("a drop landed off the file's floors");
            GB_WARN("Hunter: drop landed at %d,%d, not on floor %d the map file gave -- %s's file let go of until %d "
                    "standing points on its floors agree again (no clock); drops go on then",
                    rx, ry, drop_.land_y, stem, MapTrust::kTrustPoints);
            break;
        case DropVerdict::OtherFloor:
            // Down a floor all the same, onto one the file has: as Worked for
            // where it stands now -- no hop or jump straight back up -- with
            // nothing learned.
            drop_landed_ = true;
            retreat_[0] = retreat_[1] = Retreat{};
            last_drop_worked_ms_ = now;
            last_drop_one_up_ = false;
            drop_fails_row_ = 0;
            drop_worked_at(drop_.stem, at);
            ++minute_drop_other_;
            GB_INFO("Hunter: drop landed at %d,%d, %d px down, %s -- not floor %d the map file gave from %d,%d; "
                    "%s's file still gone by (pushed aside in the fall)",
                    rx, ry, ry - drop_.y0,
                    drop_.page ? drop_.page->gap_text(rx, ry).c_str() : "a floor of the file", drop_.land_y,
                    drop_.x0, drop_.y0, stem);
            break;
        case DropVerdict::Rose:
            // A failed press like any other (2026.11.24): the place learned at
            // once -- skipped kLearnedForgetMs, another column first -- the
            // drops going on. Until then drops were off until the next start.
            drop_landed_ = true;
            learn = true;
            fail = true;
            ++minute_drop_rose_;
            GB_INFO("Hunter: drop ROSE to %d,%d -- Ctrl+Down acts as a jump here; this place skipped %llu s, another "
                    "column first; drops go on (%s)",
                    rx, ry, skip_s, over);
            break;
        case DropVerdict::JumpInPlace:
            drop_landed_ = true;
            learn = second;
            fail = true;
            ++minute_drop_jump_;
            if (second)
                GB_INFO("Hunter: drop was a jump in place (landed at %d,%d) -- remembered as no drop for %llu s, another "
                        "column first; %s",
                        rx, ry, skip_s, over);
            else
                GB_INFO("Hunter: drop was a jump in place (landed at %d,%d) -- not held against the place yet (a second "
                        "near it within %llu s is); %s",
                        rx, ry, static_cast<unsigned long long>(kNoDropSuspectMs / 1000), over);
            break;
        case DropVerdict::Nothing:
            learn = second;
            fail = true;
            ++minute_drop_nothing_;
            if (second)
                GB_INFO("Hunter: drop did nothing in %llu ms at %d,%d -- no drops within %d px of it for %llu s, another "
                        "column first; %s (%d in a row)",
                        static_cast<unsigned long long>(kDropJudgeMs), drop_.x0, drop_.y0, GameMapBook::kNoDropXPx,
                        skip_s, over, drop_fails_row_ + 1);
            else
                GB_INFO("Hunter: drop did nothing in %llu ms at %d,%d -- not held against the place yet (a second near "
                        "it within %llu s is); %s (%d in a row)",
                        static_cast<unsigned long long>(kDropJudgeMs), drop_.x0, drop_.y0,
                        static_cast<unsigned long long>(kNoDropSuspectMs / 1000), over, drop_fails_row_ + 1);
            break;
        case DropVerdict::Unknown:
        case DropVerdict::Gate:
        case DropVerdict::None:
            ++minute_drop_unknown_;
            GB_DEBUG("Hunter: what came of the drop at %d,%d is not known -- %s", drop_.x0, drop_.y0,
                     why ? why : "not said");
            v = DropVerdict::Unknown;
            break;
    }
    if (learn && book_ && !drop_.stem.empty()) {
        book_->learn_no_drop(drop_.stem, at);
        note_learned(LearnedKind::NoDrop, drop_.stem, at, now);
    }
    // A press that did nothing, or jumped in place, moved nothing: out of the
    // minute's count again (2026.11.24), so a lost press never has the next
    // try wait for the minute -- kDropsPerMinute counts only presses that
    // moved the character.
    if (miss && drop_.minute_at != 0) {
        const auto it = std::find(drop_presses_.begin(), drop_presses_.end(), drop_.minute_at);
        if (it != drop_presses_.end()) drop_presses_.erase(it);
        drop_.minute_at = 0;
    }
    if (fail) {
        ++drop_fails_row_;
        // The monster it was for, passed over a while -- not counted as given
        // up: the minute line counts it among the drops. No pause, no latch:
        // the next look tries again (2026.11.24).
        if (have_target_ && target_slot_ == drop_.slot && target_gen_ == drop_.gen) {
            if (gave_up_.size() < 16)
                gave_up_.push_back(GaveUpSpot{ target_x_, target_y_, now + over_ms, true });
            end_fight("the drop did not drop");
            have_target_ = false;
        }
    }
    // What the press's record has to undo, should the map turn out changed --
    // the newest's, or the one kept beside a jump's since.
    PressRecord* rec = nullptr;
    for (PressRecord* c : { &press_, &press_other_ })
        if (c->open && !c->jump && c->down_ms == drop_.down_ms) rec = c;
    if (rec != nullptr) {
        rec->verdict = v;
        rec->no_drop = learn && book_ && !drop_.stem.empty();
        rec->failed = fail;
        rec->minute_from = minute_from_ms_;
    }
}

void Hunter::drop_gate(PressRecord& r, std::uint64_t now) {
    // A jump up's key: its own undoing (hunter_jump.cpp).
    if (r.jump) {
        jump_gate(r, now);
        return;
    }
    r.open = false;
    const WorldFix& f = drop_fix_;
    // Whatever was judged of it before is undone: a no-drop point learned --
    // and its note (2026.11.24) -- a fail counted, its count in this minute's
    // line.
    if (r.no_drop && book_) {
        book_->unlearn_no_drop(r.stem, { r.x0, r.y0 });
        drop_learned_note(LearnedKind::NoDrop, r.stem, { r.x0, r.y0 });
    }
    if (r.failed && drop_fails_row_ > 0) --drop_fails_row_;
    if (r.verdict != DropVerdict::None && r.minute_from == minute_from_ms_) {
        // By DropVerdict: None, Worked, Nothing, JumpInPlace, Rose, OffMap,
        // Gate, Unknown, OtherFloor.
        int* const counts[] = { nullptr,           &minute_drop_worked_,  &minute_drop_nothing_, &minute_drop_jump_,
                                &minute_drop_rose_, &minute_drop_off_map_, nullptr,
                                &minute_drop_unknown_, &minute_drop_other_ };
        int* const n = counts[static_cast<std::size_t>(r.verdict)];
        if (n != nullptr && *n > 0) --*n;
    }
    if (drop_.phase == DropPhase::Fall && drop_.down_ms == r.down_ms) {
        drop_.phase = DropPhase::Idle;
        std::lock_guard<std::mutex> lock(mutex_);
        status_.dropping = false;
    }
    ++minute_drop_gate_;
    // Both ends are gates: where it pressed, on the map it left, and where the
    // entry put it, on the one it came to -- knowledge, kept for the session:
    // no drop or jump near them again.
    const MapPoint to{ round_px(f.entry_x), round_px(f.entry_y) };
    if (book_) {
        if (!r.stem.empty()) book_->learn_gate(r.stem, { r.x0, r.y0 });
        if (f.map_known) book_->learn_gate(GameMapBook::stem_of(f.map, f.area), to);
    }
    // And the hunt goes on where the character is (2026.11.24): no latch, no
    // halt, nothing unticked or saved. The moves this pass let go of once
    // (let_go_, pass()), the fight disengaged; the spot is set aside as after
    // any door, and the way back and the jumps' plans asked again with the
    // gates learned. Until 2026.11.24 drops went off and the hunter halted
    // for GoBot's window to stop the hunt and untick Nhảy xuống.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++status_.press_gates;
    }
    let_go_ = true;
    ++drop_epoch_;
    ++jump_epoch_;
    way_asked_ = false;
    travel_asked_ = false;
    char where[48] = "a map not known";
    if (f.map_known)
        std::snprintf(where, sizeof where, "map %u/%u", static_cast<unsigned>(f.map), static_cast<unsigned>(f.area));
    GB_WARN("Hunter: MAP CHANGED %.1f s after Ctrl+Down at %d,%d on %s -> %s at %d,%d -- both ends learned as gates "
            "(no drop or jump near them again this session); hunting goes on, Nhảy xuống stays ticked",
            static_cast<double>(now > r.down_ms ? now - r.down_ms : 0) / 1000.0, r.x0, r.y0,
            r.stem.empty() ? "a map not trusted" : r.stem.c_str(), where, to.x, to.y);
}

bool Hunter::drop_second_strike(const std::string& stem, MapPoint at, std::uint64_t now) {
    // Notes older than kNoDropSuspectMs are as good as none.
    drop_suspects_.erase(std::remove_if(drop_suspects_.begin(), drop_suspects_.end(),
                                        [now](const NoDropSuspect& s) { return now >= s.ms + kNoDropSuspectMs; }),
                         drop_suspects_.end());
    const auto near_it = [&](const NoDropSuspect& s) {
        return s.stem == stem && std::abs(s.at.x - at.x) <= GameMapBook::kNoDropXPx &&
               std::abs(s.at.y - at.y) <= GameMapBook::kNoDropYPx;
    };
    // The second near a place: that place is learned, its notes are done with.
    const std::size_t before = drop_suspects_.size();
    drop_suspects_.erase(std::remove_if(drop_suspects_.begin(), drop_suspects_.end(), near_it), drop_suspects_.end());
    if (drop_suspects_.size() != before) return true;
    // The first: noted.
    if (drop_suspects_.size() >= 16) drop_suspects_.erase(drop_suspects_.begin());
    drop_suspects_.push_back(NoDropSuspect{ stem, at, now });
    return false;
}

void Hunter::drop_worked_at(const std::string& stem, MapPoint at) {
    drop_suspects_.erase(std::remove_if(drop_suspects_.begin(), drop_suspects_.end(),
                                        [&](const NoDropSuspect& s) {
                                            return s.stem == stem && std::abs(s.at.x - at.x) <= GameMapBook::kNoDropXPx &&
                                                   std::abs(s.at.y - at.y) <= GameMapBook::kNoDropYPx;
                                        }),
                         drop_suspects_.end());
}

void Hunter::note_learned(LearnedKind kind, const std::string& stem, MapPoint at, std::uint64_t now) {
    if (book_ == nullptr || stem.empty()) return;
    // One note for each thing learned: each is taken back once. Room made for
    // it -- the oldest given back to the book at once, whatever its time.
    while (learned_.size() >= kLearnedMax) {
        const LearnedNote oldest = learned_.front();
        learned_.erase(learned_.begin());
        unlearn_place(oldest.kind, oldest.stem, oldest.at, true);
    }
    learned_.push_back(LearnedNote{ kind, stem, at, now });
}

void Hunter::drop_learned_note(LearnedKind kind, const std::string& stem, MapPoint at) {
    const auto it = std::find_if(learned_.begin(), learned_.end(), [&](const LearnedNote& n) {
        return n.kind == kind && n.stem == stem && n.at.x == at.x && n.at.y == at.y;
    });
    if (it != learned_.end()) learned_.erase(it);
}

void Hunter::forget_learned(std::uint64_t now) {
    // 2026.11.24, the owner's "thử lại luôn": a place a press failed at was
    // kept for the session -- a no-drop point, a jump's take-off onto a floor,
    // a floor solid from below -- and with every column of a floor learned
    // nothing was ever tried there again. Each is skipped kLearnedForgetMs,
    // the next try from another column while it lasts, then taken back out of
    // the map book and tried again: the way back, the seek and the marks asked
    // afresh (the epochs). Until 2026.11.24 cool_downs_end ended here the
    // drops' and jumps' 30 / 60 / 120 s pauses and gave back a file a landing
    // off its floors had distrusted for 120 s -- all gone.
    for (std::size_t i = 0; i < learned_.size();) {
        if (now < learned_[i].ms + kLearnedForgetMs) {
            ++i;
            continue;
        }
        const LearnedNote n = learned_[i];
        learned_.erase(learned_.begin() + static_cast<std::ptrdiff_t>(i));
        unlearn_place(n.kind, n.stem, n.at, false);
    }
}

void Hunter::unlearn_place(LearnedKind kind, const std::string& stem, MapPoint at, bool early) {
    if (book_ == nullptr) return;
    bool had = false;
    switch (kind) {
        case LearnedKind::NoDrop: had = book_->unlearn_no_drop(stem, at); break;
        case LearnedKind::NoJump: had = book_->unlearn_no_jump(stem, at); break;
        case LearnedKind::Solid:  had = book_->unlearn_solid(stem, at); break;
    }
    // Already gone from the book -- a map change took it back, or the book's
    // own room: nothing to say.
    if (!had) return;
    ++drop_epoch_;
    ++jump_epoch_;
    way_asked_ = false;
    travel_asked_ = false;
    const char* what = kind == LearnedKind::NoDrop ? "Ctrl+Down" : "the jump";
    if (early)
        GB_INFO("Hunter: the place at %d,%d on %s where %s did not work is tried again (the oldest of %zu places "
                "learned, given back early)",
                at.x, at.y, stem.c_str(), what, kLearnedMax);
    else
        GB_INFO("Hunter: the place at %d,%d on %s where %s did not work is tried again (%llu s on)", at.x, at.y,
                stem.c_str(), what, static_cast<unsigned long long>(kLearnedForgetMs / 1000));
}

void Hunter::drop_refused(const HunterConfig& cfg, const DropCall& call, const Nameplate& target, int dy,
                          std::uint64_t now) {
    drop_why_pass_ = call.why;
    // Every refusal acted on, for the minute line's "refused:".
    ++minute_drop_why_[static_cast<std::size_t>(call.why)];
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.drop_wait = call.why;
        status_.drop_wait_px = call.px;
        status_.drop_wait_ms = now;
    }
    if (target.slot != drop_said_slot_ || target.gen != drop_said_gen_) {
        drop_said_slot_ = target.slot;
        drop_said_gen_ = target.gen;
        drop_said_why_ = DropWhy::None;
        drop_said_info_ = 0;
    }
    if (call.why == drop_said_why_) return;
    drop_said_why_ = call.why;
    char more[320] = "";
    switch (call.why) {
        case DropWhy::Gate:
            std::snprintf(more, sizeof more, " (%d px across; drop_gate_px is %d)", call.px, cfg.drop_gate_px);
            break;
        case DropWhy::PastMonster:
            // The one-Up reach a landing below it may be within (2026.11.12c)
            // -- where that Up comes down on no other floor (its review).
            std::snprintf(more, sizeof more,
                          " (floor %d, %d px below it; one Up reaches %d where it comes down on no other floor)",
                          call.land_y, call.px,
                          kDropBelowOneUp && bar_up_may_go(cfg)
                              ? std::max(kDropFloorMatchPx,
                                         round_px((cfg.up() + bar_air(cfg)) *
                                                  (cfg.packet_scale > 0.0f ? cfg.packet_scale : 1.0f)))
                              : kDropFloorMatchPx);
            break;
        case DropWhy::MapUnknown: {
            // Found wrong: which point of yours did it, and how far from the
            // floors -- a monster's never does.
            const MapTrust::Report r = trust_.report();
            if (r.why == MapTrust::Why::Mismatch)
                std::snprintf(more, sizeof more, " (%s: the last at %d,%d, %s)", MapTrust::why_text(r.why), r.miss.x,
                              r.miss.y, r.miss_gap.empty() ? "?" : r.miss_gap.c_str());
            else
                std::snprintf(more, sizeof more, " (%s)", MapTrust::why_text(r.why));
            break;
        }
        case DropWhy::OffFloor:
            // The press's own check, one report: this press only, the file
            // still gone by (MapTrust lets it go at two).
            if (const std::shared_ptr<const GameMapBook::Page> page = trust_.page())
                std::snprintf(more, sizeof more, " (you at %d,%d: %s)", call.x0, call.y0,
                              page->gap_text(call.x0, call.y0).c_str());
            break;
        default:
            break;
    }
    // Where it was judged from: the character, the floor under it, the
    // monster and its floor -- the log of 2026-11-04 had to be rebuilt from
    // the map by hand to say. Only once the map was gone by.
    if (call.placed) {
        const std::size_t at = std::strlen(more);
        char under[16] = "none";
        if (call.stand_y >= 0) std::snprintf(under, sizeof under, "%d", call.stand_y);
        char theirs[48] = "no floor within 100 px under it";
        if (call.target_floor >= 0) std::snprintf(theirs, sizeof theirs, "its floor %d", call.target_floor);
        if (at < sizeof more)
            std::snprintf(more + at, sizeof more - at, "; you at %d,%d (floor %s), the monster at %d,%d (%s), %d px %s",
                          call.x0, call.y0, under, call.tx, call.ty, theirs, std::abs(call.tx - call.x0),
                          call.tx < call.x0 ? "left" : "right");
    }
    // Once a target for what the player can do something about; the rest,
    // and any again, at DEBUG.
    const std::uint64_t bit = 1ull << static_cast<unsigned>(call.why);
    if (said_at_info(call.why) && (drop_said_info_ & bit) == 0) {
        drop_said_info_ |= bit;
        GB_INFO("Hunter: not dropping to the monster %d px below -- %s%s", dy, drop_why_name(call.why), more);
    } else {
        GB_DEBUG("Hunter: not dropping to the monster %d px below -- %s%s", dy, drop_why_name(call.why), more);
    }
}

std::string Hunter::drop_minute_text() const {
    std::string out;
    if (minute_drops_ != 0 || minute_drop_worked_ != 0 || minute_drop_nothing_ != 0 || minute_drop_jump_ != 0 ||
        minute_drop_rose_ != 0 || minute_drop_off_map_ != 0 || minute_drop_gate_ != 0 || minute_drop_unknown_ != 0 ||
        minute_drop_other_ != 0) {
        char text[240];
        std::snprintf(text, sizeof text,
                      "; drops %d (%d worked, %d did nothing, %d jumps in place, %d rose, %d off the map, %d on "
                      "another floor, %d map changes, %d unknown)",
                      minute_drops_, minute_drop_worked_, minute_drop_nothing_, minute_drop_jump_, minute_drop_rose_,
                      minute_drop_off_map_, minute_drop_other_, minute_drop_gate_, minute_drop_unknown_);
        out = text;
    }
    // And the looks at a target below that asked the rule, pressed or not:
    // the settles begun, and what refused the rest, most first.
    if (minute_drop_looks_ > 0) {
        char text[64];
        std::snprintf(text, sizeof text, "; drop looks %d (settles %d", minute_drop_looks_, minute_drop_settles_);
        out += text;
        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < kDropWhys; ++i)
            if (minute_drop_why_[i] > 0) order.push_back(i);
        std::stable_sort(order.begin(), order.end(),
                         [this](std::size_t a, std::size_t b) { return minute_drop_why_[a] > minute_drop_why_[b]; });
        for (std::size_t k = 0; k < order.size(); ++k) {
            std::snprintf(text, sizeof text, "%s%s %d", k == 0 ? "; refused: " : ", ",
                          drop_why_short(static_cast<DropWhy>(order[k])), minute_drop_why_[order[k]]);
            out += text;
        }
        out += ")";
    }
    return out;
}

} // namespace gobot
