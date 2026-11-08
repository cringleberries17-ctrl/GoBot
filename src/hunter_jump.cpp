// The hunter's jump up onto a floor above: ↑, ↑↑ or Ctrl + ↑ straight up --
// "Nhảy lên bục trên", HunterConfig::jump_up (2026.11.05b). The jump-up plan's
// §6-§9, with the owner's answers of 2026-11-05: the heights "Theo GoC" inside
// the model's ranges, no band round a gate (Ctrl + ↑ on one does not change
// the map), the arc may pass above the box's top, the monster's own floor
// jumped onto and fought on as usual, and on by default.
//
// When: jump_up_rule, pure, in the plan's order; where from: jump_plan.hpp,
// over the map tab's area's own file. How: walked to the take-off along the
// floor (WalkFor::Floor, never a hop), settled as a drop settles, the column it
// stopped on judged afresh, then the key through the hunter's Hands
// (InputManager::jump_once, jump_twice, high_jump) under the coordinator's
// token taken only from nobody. In the air: held still -- no walk, no turn's
// tap, no step back -- attacks allowed. What came of it: your own reports
// (PacketWorld::own_since) and the take-offs by kind, against the planned
// floor. A map change within kPressWatchMs of the key halts the hunter as a
// drop's does, for GoBot's window to stop the hunt and untick jump_up.
//
// One executor, three callers: the target above (jump_toward), the way back's
// legs (go_home_zone), and the seek's (JumpUpFor::Seek, jump_go).
//
// Nothing here turns the character on purpose, nor writes which way it faces
// or the keys that set it: the turn and its bookkeeping are the in-reach
// branch's alone. Never ↓; never InputManager::jump() nor its double.

#include "gobot/hunter.hpp"
#include "gobot/common.hpp"
#include "gobot/jump_check.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace gobot {
namespace {

int round_px(double v) { return static_cast<int>(std::lround(v)); }

/// ↑: the one key a jump up goes on, {0x26, E0 48}.
constexpr std::uint32_t kUpVk = 0x26;

/// Ctrl, either side, by the key code's low byte.
bool is_ctrl(std::uint32_t vk) {
    const std::uint32_t low = vk & 0xFFu;
    return low == 0x11u || low == 0xA2u || low == 0xA3u;
}

std::uint8_t bit_of(JumpKind k) { return static_cast<std::uint8_t>(1u << static_cast<unsigned>(k)); }

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

/// Whether the floor whose top is `top` at column x -- or a hole's width
/// before it, where the planner lands over a hole -- is the floor the standing
/// point `on` is on: followed there, its top within kDropSameFloorPx.
bool same_floor(const GameMapBook::Page& p, int x, int top, MapPoint on) {
    for (int d = 0; d <= GameMapBook::kFollowHolePx; ++d) {
        const std::optional<MapFloor> f = p.floor_near(x - d, top - 1);
        if (!f || f->wall() || std::abs(f->y - top) > GameMapBook::kFollowLinkPx) continue;
        const std::optional<int> there = p.follow_to(x - d, f->y - 1, on.x);
        return there && std::abs(*there - (on.y + 1)) <= Hunter::kDropSameFloorPx;
    }
    return false;
}

/// What a refusal does with the pass (jump_toward, jump_at).
enum class Act : std::uint8_t {
    AsBefore,   ///< not the jump's to judge: walked as before 05b
    Level,      ///< a monster on the level: the target let go of, that one picked
    Passing,    ///< only a moment in the way: stood for
    Lasting,    ///< no jump up there for now: the monster passed over, the settle let go of
};
Act act_of(Hunter::JumpUpWhy w) {
    using W = Hunter::JumpUpWhy;
    switch (w) {
        case W::None:
        case W::Off:
        case W::NoPackets:
        case W::StayPut:
        case W::NoArea:
        case W::NotWired:
        case W::JumpKey:
        case W::NotPacketTarget:
        case W::NotAbove:
        case W::OnOwnFloor:
            return Act::AsBefore;
        case W::LevelMonster:
            return Act::Level;
        case W::KeysOwed:
        case W::NotKnown:
        case W::AfterEntry:
        case W::NotSettled:
        case W::InAir:
        case W::TooSoon:
        case W::AfterDrop:
        case W::StepJudge:
        case W::Walking:
        case W::MinuteCap:
        case W::Held:
        case W::Unfocused:
        case W::Loot:
        case W::Veto:
            return Act::Passing;
        case W::Latched:
        case W::KeyClash:
        case W::OutsideArea:
        case W::TargetCap:
        case W::MapUnknown:
        case W::OffFloor:
        case W::Plan:
        case W::OffColumn:
            break;
    }
    return Act::Lasting;
}

/// What the owner can do something about: said at INFO, once a monster or way.
bool said_at_info(Hunter::JumpUpWhy w) {
    switch (w) {
        case Hunter::JumpUpWhy::JumpKey:
        case Hunter::JumpUpWhy::KeyClash:
        case Hunter::JumpUpWhy::Latched:
        case Hunter::JumpUpWhy::MapUnknown:
        case Hunter::JumpUpWhy::Plan:
        case Hunter::JumpUpWhy::OffColumn:
        case Hunter::JumpUpWhy::OutsideArea:
            return true;
        default:
            return false;
    }
}

/// The minute line's words for what refused a look.
const char* why_short(Hunter::JumpUpWhy w) {
    switch (w) {
        case Hunter::JumpUpWhy::LevelMonster: return "a monster on the level";
        case Hunter::JumpUpWhy::NotKnown:     return "you not known";
        case Hunter::JumpUpWhy::AfterEntry:   return "just after a map entry";
        case Hunter::JumpUpWhy::NotSettled:   return "not settled";
        case Hunter::JumpUpWhy::InAir:        return "in the air";
        case Hunter::JumpUpWhy::TooSoon:      return "too soon";
        case Hunter::JumpUpWhy::AfterDrop:    return "after a drop";
        case Hunter::JumpUpWhy::StepJudge:    return "a step back";
        case Hunter::JumpUpWhy::Walking:      return "walking";
        case Hunter::JumpUpWhy::MinuteCap:    return "minute's used up";
        case Hunter::JumpUpWhy::TargetCap:    return "monster's used up";
        case Hunter::JumpUpWhy::Held:         return "a hold";
        case Hunter::JumpUpWhy::Unfocused:    return "the game behind";
        case Hunter::JumpUpWhy::Loot:         return "pick-ups";
        case Hunter::JumpUpWhy::Veto:         return "GoBot's veto";
        case Hunter::JumpUpWhy::MapUnknown:   return "map not trusted";
        case Hunter::JumpUpWhy::OffFloor:     return "off its floors";
        case Hunter::JumpUpWhy::Plan:         return "no jump there";
        case Hunter::JumpUpWhy::OffColumn:    return "stopped off the column";
        case Hunter::JumpUpWhy::KeyClash:     return "Ctrl on a key";
        case Hunter::JumpUpWhy::Latched:      return "off for the run";
        case Hunter::JumpUpWhy::OutsideArea:  return "outside the area";
        default:                              return Hunter::jump_why_name(w);
    }
}

/// What the jump is for, in the log: "toward Nấm", "on the way home".
std::string for_text(Hunter::JumpUpFor purpose, const std::string& name) {
    if (purpose == Hunter::JumpUpFor::Home) return "on the way home";
    const std::string who = name.empty() ? std::string("the monster") : name;
    return purpose == Hunter::JumpUpFor::Seek ? "toward " + who + " (sought)" : "toward " + who;
}

/// The shortest full arc of a key, s: up and down again at the highest
/// gravity, from its take-off speed (Ctrl + ↑'s, or ↑'s).
double shortest_arc_s(JumpKind k) {
    const double v = k == JumpKind::CtrlUp ? kJumpUpVHigh : kJumpUpV;
    return 2.0 * v / kJumpUpGravity[std::size(kJumpUpGravity) - 1];
}

} // namespace

// ---------------------------------------------------------------- names

const char* Hunter::jump_verdict_name(JumpUpVerdict v) {
    switch (v) {
        case JumpUpVerdict::None:     return "none";
        case JumpUpVerdict::Worked:   return "worked";
        case JumpUpVerdict::FellBack: return "fell back";
        case JumpUpVerdict::KindMiss: return "not the key's jump";
        case JumpUpVerdict::Lower:    return "landed lower";
        case JumpUpVerdict::Higher:   return "landed higher";
        case JumpUpVerdict::Outside:  return "landed outside the area";
        case JumpUpVerdict::OffMap:   return "landed off the map's floors";
        case JumpUpVerdict::Nothing:  return "did nothing";
        case JumpUpVerdict::Unknown:  return "not known";
        case JumpUpVerdict::Gate:     return "changed the map";
    }
    return "?";
}

const char* Hunter::jump_off_name(JumpUpOff o) {
    switch (o) {
        case JumpUpOff::None:       return "on";
        case JumpUpOff::Fails:      return "jumps that did not get up";
        case JumpUpOff::Physics:    return "the character jumps otherwise than GoBot works out";
        case JumpUpOff::OffMap:     return "a landing where the map file has no floor";
        case JumpUpOff::MapChanged: return "a jump changed the map";
        case JumpUpOff::HeldLong:   return "Up was held too long";
        case JumpUpOff::KeysStuck:  return "a key-up did not go out";
    }
    return "?";
}

const char* Hunter::jump_why_name(JumpUpWhy w) {
    switch (w) {
        case JumpUpWhy::None:            return "it goes";
        case JumpUpWhy::Off:             return "jump_up is off";
        case JumpUpWhy::NoPackets:       return "the packets do not steer (packet_positions is not on)";
        case JumpUpWhy::StayPut:         return "Đứng yên một chỗ is ticked";
        case JumpUpWhy::NoArea:          return "no hunting area of the map tab in use with its map's file";
        case JumpUpWhy::Latched:         return "jumps up are off until the next start";
        case JumpUpWhy::KeysOwed:        return "a key-up of a chord has not gone out";
        case JumpUpWhy::JumpKey:         return "the jump key is not Up";
        case JumpUpWhy::KeyClash:        return "a walk or attack key is Ctrl, and only Ctrl+Up would do";
        case JumpUpWhy::NotWired:        return "no map book, veto or air counts";
        case JumpUpWhy::NotPacketTarget: return "the target is the screen's, not the packets'";
        case JumpUpWhy::LevelMonster:    return "a monster on your own level";
        case JumpUpWhy::NotAbove:        return "not far enough above";
        case JumpUpWhy::OutsideArea:     return "the monster is outside the hunting area";
        case JumpUpWhy::OnOwnFloor:      return "it stands on your own floor";
        case JumpUpWhy::NotKnown:        return "the packets do not know where you are";
        case JumpUpWhy::AfterEntry:      return "a map entry a moment ago";
        case JumpUpWhy::NotSettled:      return "not standing settled yet";
        case JumpUpWhy::InAir:           return "in the air";
        case JumpUpWhy::TooSoon:         return "the last drop or jump a moment ago";
        case JumpUpWhy::AfterDrop:       return "a drop worked a moment ago";
        case JumpUpWhy::StepJudge:       return "a step back not judged yet";
        case JumpUpWhy::Walking:         return "a direction key is down";
        case JumpUpWhy::MinuteCap:       return "the jumps of a minute used up";
        case JumpUpWhy::TargetCap:       return "the jumps for this monster used up";
        case JumpUpWhy::Held:            return "a hold";
        case JumpUpWhy::Unfocused:       return "the game is not in front";
        case JumpUpWhy::Loot:            return "pick-ups still owed";
        case JumpUpWhy::Veto:            return "GoBot says not now";
        case JumpUpWhy::MapUnknown:      return "the map is not known, or its file not trusted";
        case JumpUpWhy::OffFloor:        return "where you stand is on no floor of the map file";
        case JumpUpWhy::Plan:            return "no sure jump there";
        case JumpUpWhy::OffColumn:       return "stopped where the jump does not hold";
    }
    return "?";
}

// ---------------------------------------------------------------- where

bool Hunter::jump_watch(const HunterConfig& cfg) const {
    return cfg.packet_positions == PacketPositions::On && !cfg.stay_put && cfg.spot &&
           spot_mode_ == SpotMode::Use && zone_.page != nullptr;
}

bool Hunter::jump_ready(const HunterConfig& cfg, const GameMapBook::Page* page) const {
    const bool wired = book_ != nullptr && static_cast<bool>(drop_veto_) && static_cast<bool>(eyes_.air) &&
                       static_cast<bool>(eyes_.own_since);
    return page != nullptr && cfg.packet_positions == PacketPositions::On && !cfg.stay_put && cfg.spot &&
           spot_mode_ == SpotMode::Use && trust_.trusted() && trust_.stem() == page->stem && cfg.jump_key == kUpVk &&
           wired;
}

bool Hunter::jump_area(const HunterConfig& cfg) const { return jump_ready(cfg, zone_.page.get()); }

bool Hunter::jumps_may_go(const HunterConfig& cfg) const {
    return cfg.jump_up && jump_latch_ == JumpUpOff::None && jump_area(cfg);
}

JumpRules Hunter::jump_rules(const HunterConfig& cfg, const std::shared_ptr<const GameMapBook::Page>& page) {
    JumpRules r;
    // The keys off for the run, and Ctrl + ↑ with a walk or attack key on
    // Ctrl: Ctrl + ↑ would be pressed with it.
    r.kinds = static_cast<std::uint8_t>(7u & ~static_cast<unsigned>(jump_kinds_off_));
    for (const std::uint32_t vk : { cfg.left_key, cfg.right_key, cfg.attack_key })
        if (vk != 0 && is_ctrl(vk)) r.kinds = static_cast<std::uint8_t>(r.kinds & ~bit_of(JumpKind::CtrlUp));
    const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
    r.reach_px = round_px(cfg.reach * scale);
    if (!page) return r;
    // The judges' band round a gate, only with kJumpUpGateBand: the owner's
    // answer 2 of 2026-11-05 has none.
    if constexpr (kJumpUpGateBand) {
        jump_gates_ = book_ ? book_->gates(page->stem) : page->gates;
        r.gates = jump_gates_;
        r.gate_px = cfg.drop_gate_px;
    }
    if (book_ == nullptr) return r;
    // What this session learned of the map: a point where this key -- or a
    // stronger one -- did not get onto that floor, and floors solid from below.
    std::vector<GameMapBook::NoJump> nj = book_->no_jumps(page->stem);
    std::vector<MapPoint> solid = book_->solid_floors(page->stem);
    if (nj.empty() && solid.empty()) return r;
    r.refused = [p = page, nj = std::move(nj), solid = std::move(solid)](MapPoint take, int land_top,
                                                                         JumpKind kind) {
        for (const MapPoint& s : solid)
            if (same_floor(*p, take.x, land_top, s)) return JumpNo::Solid;
        for (const GameMapBook::NoJump& n : nj) {
            if (static_cast<int>(kind) > static_cast<int>(n.kind)) continue;
            if (std::abs(take.x - n.take.x) > GameMapBook::kNoJumpXPx ||
                std::abs(take.y - n.take.y) > GameMapBook::kNoJumpYPx)
                continue;
            if (same_floor(*p, take.x, land_top, n.land)) return JumpNo::Learned;
        }
        return JumpNo::None;
    };
    return r;
}

// ---------------------------------------------------------------- the rule

Hunter::JumpUpCall Hunter::jump_up_rule(const HunterConfig& cfg, const JumpUpFacts& f) {
    JumpUpCall c;
    const auto no = [&c](JumpUpWhy why, int px = 0, JumpNo jn = JumpNo::None) {
        c.why = why;
        c.px = px;
        c.no = jn;
        return c;
    };
    const bool target = f.purpose == JumpUpFor::Target;
    const bool monster = f.purpose != JumpUpFor::Home;

    // 1. Switched on: the packets steering, not standing still, the map
    // tab's area in use with its file, and not off for the run.
    if (!cfg.jump_up) return no(JumpUpWhy::Off);
    if (cfg.packet_positions != PacketPositions::On) return no(JumpUpWhy::NoPackets);
    if (cfg.stay_put) return no(JumpUpWhy::StayPut);
    if (!f.area) return no(JumpUpWhy::NoArea);
    if (f.latched) return no(JumpUpWhy::Latched);
    if (f.owed) return no(JumpUpWhy::KeysOwed);
    // 2. The keys: ↑ the jump key; Ctrl + ↑ only with no walk or attack key
    // on Ctrl, and none of a key off for the run.
    if (cfg.jump_key != kUpVk) return no(JumpUpWhy::JumpKey, static_cast<int>(cfg.jump_key));
    std::uint8_t kinds = static_cast<std::uint8_t>(7u & ~static_cast<unsigned>(f.kinds_off));
    bool clash = false;
    for (const std::uint32_t vk : { cfg.left_key, cfg.right_key, cfg.attack_key })
        if (vk != 0 && is_ctrl(vk)) clash = true;
    if (clash) kinds = static_cast<std::uint8_t>(kinds & ~bit_of(JumpKind::CtrlUp));
    if (kinds == 0) return no(clash ? JumpUpWhy::KeyClash : JumpUpWhy::Latched);
    if (f.plan && (kinds & bit_of(f.plan->kind)) == 0)
        return no(clash && f.plan->kind == JumpKind::CtrlUp ? JumpUpWhy::KeyClash : JumpUpWhy::Latched,
                  static_cast<int>(f.plan->kind));
    // 3. The map book, GoBot's say and the packets' counts: without them, never.
    if (!f.wired) return no(JumpUpWhy::NotWired);
    // 4. The monster: the packets' own, nothing on the level, above past the
    // band, inside the area -- and not on the character's own floor, which a
    // walk gets to.
    const MapPoint goal{ round_px(f.target_wx), round_px(f.target_wy) };
    if (target) {
        if (!f.packet_target) return no(JumpUpWhy::NotPacketTarget);
        if (f.level_monster) return no(JumpUpWhy::LevelMonster);
        if (f.dy >= -cfg.up()) return no(JumpUpWhy::NotAbove, -f.dy);
    }
    if (monster && f.packet_target) {
        if (!f.box.contains(goal.x, goal.y)) return no(JumpUpWhy::OutsideArea);
        const std::optional<int> theirs =
            f.page ? target_floor(*f.page, f.target_stood, f.target_stand, goal.x, goal.y) : std::nullopt;
        if (theirs) {
            c.target = { goal.x, *theirs - 1 };
            c.placed = true;
            if (target && f.stand) {
                const std::optional<int> own = f.page->follow_to(f.stand->x, f.stand->y, goal.x);
                if (own && std::abs(*own - *theirs) <= GameMapBook::kFollowLinkPx) return no(JumpUpWhy::OnOwnFloor);
            }
        }
    }
    // 5. The character: known, not just after a map entry -- settled, its
    // stop reported, at the press.
    const WorldFix& fix = f.fix;
    if (fix.state != WorldFix::State::Known || fix.uncertain) return no(JumpUpWhy::NotKnown);
    if (f.now < f.entry_ms + kDropAfterEntryMs)
        return no(JumpUpWhy::AfterEntry, static_cast<int>(f.entry_ms + kDropAfterEntryMs - f.now));
    if (f.press && (!fix.settled() || (f.walk_held && fix.report_ms < f.settle_from_ms)))
        return no(JumpUpWhy::NotSettled);
    // 6. The clock: nothing in the air, kDropGapMs since any press, a drop's
    // landing given kDropStayMs -- but on the way back -- no step back to be
    // judged, no arrow down at the press.
    if (f.in_air) return no(JumpUpWhy::InAir);
    if (f.last_press_ms != 0 && f.now < f.last_press_ms + kDropGapMs) return no(JumpUpWhy::TooSoon);
    if (monster && f.last_drop_worked_ms != 0 && f.now < f.last_drop_worked_ms + kDropStayMs)
        return no(JumpUpWhy::AfterDrop, static_cast<int>(f.last_drop_worked_ms + kDropStayMs - f.now));
    if (f.step_judge) return no(JumpUpWhy::StepJudge);
    if (f.press && (f.left_held || f.right_held)) return no(JumpUpWhy::Walking);
    // 7. Limits.
    if (f.presses_minute >= kJumpUpsPerMinute) return no(JumpUpWhy::MinuteCap, f.presses_minute);
    if (monster && f.presses_target >= kJumpUpsPerTarget) return no(JumpUpWhy::TargetCap, f.presses_target);
    if (monster && f.settles_target >= kJumpUpSettlesPerTarget) return no(JumpUpWhy::TargetCap, f.settles_target);
    // 8. No hold, the game in front, no pick-ups owed, GoBot not against.
    if (f.held) return no(JumpUpWhy::Held);
    if (!f.focused) return no(JumpUpWhy::Unfocused);
    if (f.loot_owed) return no(JumpUpWhy::Loot);
    if (f.veto != nullptr) return no(JumpUpWhy::Veto);
    // 9. The area's map trusted, and the character on its floors.
    if (!f.trusted || !f.page) return no(JumpUpWhy::MapUnknown);
    if (!f.stand) return no(JumpUpWhy::OffFloor);
    const GameMapBook::Page& page = *f.page;
    JumpRules rules = f.rules;
    rules.kinds = kinds;

    // 10. The jump: onto the target's floor (plan_jump_to), its take-off kept
    // while the run it is in still holds it; the way's or the seek's leg as
    // given.
    if (target) {
        if (!c.placed) return no(JumpUpWhy::Plan, 0, JumpNo::Short);
        JumpNo why = JumpNo::None;
        int px = 0;
        std::optional<JumpPlan> best = plan_jump_to(page, f.box, *f.stand, c.target, rules, &why, &px);
        if (!best) return no(JumpUpWhy::Plan, px, why);
        if (f.plan && f.plan->kind == best->kind && f.plan->x >= best->run_l + kJumpUpRunInsetPx &&
            f.plan->x <= best->run_r - kJumpUpRunInsetPx) {
            const JumpColumn kept = judge_column(page, f.box, f.plan->take(), best->kind, rules, 0);
            if (kept.ok()) {
                JumpPlan k = *best;
                k.x = f.plan->x;
                k.feet_y = f.plan->feet_y;
                k.land_x = kept.land_x;
                k.land_top = kept.land_top;
                k.rise = kept.rise;
                k.headroom = kept.headroom;
                k.walk_px = std::abs(k.x - f.stand->x);
                best = k;
            }
        }
        // From where it lands, the monster in reach across -- or the walk to
        // it along the landing floor one Q1 lets go (walk_stays_in).
        const int across = c.target.x - best->land_x;
        if (std::abs(across) > rules.reach_px) {
            const int ahead = std::clamp(std::abs(across) - rules.reach_px + kZoneWalkSlackPx, 1, kZoneWalkAheadPx);
            if (!walk_stays_in(page, f.box, best->land(), across < 0 ? -1 : 1, ahead).ok)
                return no(JumpUpWhy::Plan, best->rise, JumpNo::Far);
        }
        c.plan = best;
    } else {
        if (!f.plan) return no(JumpUpWhy::Plan, 0, JumpNo::Short);
        c.plan = f.plan;
    }

    // 11. At the press: the column it stopped on -- its samples either way
    // too -- passes, and lands on the planned floor.
    if (f.press) {
        const JumpPlan& j = *c.plan;
        // Coming back into the box, the take-off no further out than now (the
        // Closer rule the way was planned by).
        const int out = f.purpose == JumpUpFor::Home ? zone_outside_px(f.box, f.stand->x, f.stand->y) : 0;
        c.column = judge_column(page, f.box, *f.stand, j.kind, rules, out);
        if (!c.column.ok()) return no(JumpUpWhy::OffColumn, f.stand->x - j.x, c.column.why);
        const std::optional<int> there = page.follow_to(c.column.land_x, c.column.land_top - 1, j.land_x);
        if (!there || std::abs(*there - j.land_top) > GameMapBook::kFollowLinkPx)
            return no(JumpUpWhy::OffColumn, f.stand->x - j.x, JumpNo::NotTargetFloor);
        JumpPlan at = j;
        at.x = f.stand->x;
        at.feet_y = f.stand->y;
        at.land_x = c.column.land_x;
        at.land_top = c.column.land_top;
        at.rise = c.column.rise;
        at.headroom = c.column.headroom;
        at.walk_px = 0;
        c.plan = at;
    }
    return c;
}

Hunter::JumpUpFacts Hunter::jump_facts(const HunterConfig& cfg, JumpUpFor purpose, int slot, std::uint32_t gen,
                                       std::optional<MapPoint> target_w, int dy, bool level,
                                       const std::optional<JumpPlan>& plan, std::uint64_t now, bool press) {
    JumpUpFacts f;
    f.purpose = purpose;
    f.press = press;
    f.now = now;
    f.area = spot_mode_ == SpotMode::Use && zone_.page != nullptr;
    f.page = zone_.page;
    f.box = zone_.box;
    f.trusted = zone_.page && trust_.trusted() && trust_.stem() == zone_.page->stem;
    f.latched = jump_latch_ != JumpUpOff::None;
    f.kinds_off = jump_kinds_off_;
    f.owed = owed_passes_ > 0;
    f.wired = book_ != nullptr && static_cast<bool>(drop_veto_) && static_cast<bool>(eyes_.air) &&
              static_cast<bool>(eyes_.own_since);
    f.packet_target = purpose != JumpUpFor::Home && slot >= 0;
    f.slot = slot;
    f.gen = gen;
    f.dy = dy;
    if (target_w) {
        f.target_wx = static_cast<float>(target_w->x);
        f.target_wy = static_cast<float>(target_w->y);
    }
    // Where the monster last stood, by its own frames: its floor.
    if (slot >= 0)
        for (const MobStand& s : mob_stands_)
            if (static_cast<int>(s.slot) == slot && s.gen == gen && s.stood) {
                f.target_stood = true;
                f.target_stand = s.at;
            }
    f.level_monster = level;
    f.fix = drop_fix_on_ ? drop_fix_ : WorldFix{};
    f.entry_ms = drop_entry_on_ ? drop_entry_ms_ : now;
    f.settle_from_ms = jump_.settle_from_ms;
    f.walk_held = jump_.walk_held;
    f.in_air = in_air(now);
    f.last_press_ms = last_press_ms_;
    f.last_drop_worked_ms = last_drop_worked_ms_;
    f.step_judge = back_.judge;
    f.left_held = cfg.left_key != 0 && is_down(cfg.left_key);
    f.right_held = cfg.right_key != 0 && is_down(cfg.right_key);
    f.presses_minute = jump_presses_minute(now);
    f.presses_target = jump_presses_for(slot, gen);
    f.settles_target = jump_settles_for(slot, gen);
    f.held = coord_.critical_active();
    f.focused = focused();
    f.loot_owed = cfg.loots() && loot_left_ > 0;
    f.veto = drop_veto_ ? drop_veto_() : nullptr;
    if (f.page) {
        // Worked out along its floor as it is walked to; at the press, its
        // last report's own point -- standing, that is where it is.
        f.stand = press ? stand_on(*f.page, f.fix, false) : zone_.stand;
        f.rules = jump_rules(cfg, f.page);
    }
    f.plan = plan;
    f.scale = cfg.packet_scale > 0.0f ? cfg.packet_scale : 1.0f;
    return f;
}

int Hunter::jump_presses_minute(std::uint64_t now) {
    jump_presses_.erase(std::remove_if(jump_presses_.begin(), jump_presses_.end(),
                                       [now](std::uint64_t t) { return now >= t + 60'000; }),
                        jump_presses_.end());
    return static_cast<int>(jump_presses_.size());
}

int Hunter::jump_presses_for(int slot, std::uint32_t gen) const {
    if (slot < 0) return 0;
    for (const TargetPresses& t : jump_targets_)
        if (t.slot == slot && t.gen == gen) return t.n;
    return 0;
}

int Hunter::jump_settles_for(int slot, std::uint32_t gen) const {
    if (slot < 0) return 0;
    for (const TargetPresses& t : jump_targets_)
        if (t.slot == slot && t.gen == gen) return t.settles;
    return 0;
}

// ---------------------------------------------------------------- choosing

void Hunter::jump_marks(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                        const std::vector<Listed>& listed, const GameMapBook::Page& p, MapPoint stand,
                        const WorldFix& f, std::vector<char>& marks) {
    if (!jump_area(cfg)) return;
    const bool go = cfg.jump_up && jump_latch_ == JumpUpOff::None;
    const std::uint64_t now = clock_now();
    const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
    const JumpRules rules = jump_rules(cfg, zone_.page);
    // The floors a jump from here lands on at all (jump_legs_from), worked out
    // once a standing point: a monster whose floor is none of them is not
    // planned for.
    bool legs_known = jump_legs_known_ && jump_legs_from_.x == stand.x && jump_legs_from_.y == stand.y &&
                      same_place(jump_legs_box_, zone_.box) && jump_legs_epoch_ == jump_epoch_ &&
                      jump_legs_kinds_ == rules.kinds;
    const std::vector<JumpPlan>& legs = jump_legs_;
    for (std::size_t i = 0; i < mobs.size() && i < marks.size(); ++i) {
        const Nameplate& n = mobs[i];
        if (n.slot < 0 || n.hp <= cfg.dead_below || marks[i] != 0) continue;
        if (i < listed.size() && refused(listed[i])) continue;
        if (n.y() - me.y() >= -cfg.up()) continue;
        if (go && (jump_presses_for(n.slot, n.gen) >= kJumpUpsPerTarget ||
                   jump_settles_for(n.slot, n.gen) >= kJumpUpSettlesPerTarget))
            continue;
        const MapPoint w{ round_px(f.x + (n.x() - me.x()) * scale),
                          round_px(f.y + (n.y() - me.y() - cfg.packet_dy) * scale) };
        bool stood = false;
        MapPoint stand_at;
        for (const MobStand& s : mob_stands_)
            if (static_cast<int>(s.slot) == n.slot && s.gen == n.gen && s.stood) {
                stood = true;
                stand_at = s.at;
            }
        const std::optional<int> theirs = target_floor(p, stood, stand_at, w.x, w.y);
        if (!theirs || !zone_.box.contains(w.x, w.y)) continue;
        const MapPoint at{ w.x, *theirs - 1 };
        if (!legs_known) {
            jump_legs_ = jump_legs_from(p, zone_.box, stand, rules);
            jump_legs_known_ = legs_known = true;
            jump_legs_from_ = stand;
            jump_legs_box_ = zone_.box;
            jump_legs_epoch_ = jump_epoch_;
            jump_legs_kinds_ = rules.kinds;
        }
        const bool onto = std::any_of(legs.begin(), legs.end(), [&](const JumpPlan& j) {
            const std::optional<int> there = p.follow_to(j.land_x, j.land_top - 1, at.x);
            return there && std::abs(*there - *theirs) <= kDropSameFloorPx;
        });
        if (!onto) continue;
        const std::optional<JumpPlan> plan = plan_jump_to(p, zone_.box, stand, at, rules);
        if (!plan) continue;
        if (go) marks[i] = 2;
        else    jump_would(*plan, JumpUpFor::Target, n.slot, n.gen, n.name, now);
        // The nearest such: choose_target takes the first anyway.
        break;
    }
}

// ---------------------------------------------------------------- a pass

bool Hunter::jump_toward(const HunterConfig& cfg, const Nameplate& target, const Nameplate& me,
                         const std::vector<Nameplate>& mobs, const std::vector<Listed>& listed, int dx, int dy,
                         std::uint64_t now) {
    // In the air: held still -- no walk, no hop -- until it is judged, and
    // after a flight not judged for as long as it may still be in the air: an
    // arrow would steer it.
    if (jump_.phase == JumpUpPhase::Air || jump_in_air(now)) {
        let_go_of_moves(cfg);
        return true;
    }
    const bool mine = jump_.phase != JumpUpPhase::Idle && jump_.purpose == JumpUpFor::Target;
    // Not the jump's: off, no area in use, a screen plate, not above past the
    // band -- walked to as before.
    if (!cfg.jump_up || !jump_watch(cfg) || target.slot < 0 || dy >= -cfg.up()) {
        if (mine && jump_.phase == JumpUpPhase::Settle) jump_settle_end("the target is no longer above", true, now);
        else if (mine) jump_.phase = JumpUpPhase::Idle;
        return false;
    }
    if (mine && (target.slot != jump_.slot || target.gen != jump_.gen)) {
        if (jump_.phase == JumpUpPhase::Settle) jump_settle_end("another target", false, now);
        else jump_.phase = JumpUpPhase::Idle;
    }
    (void)dx;
    // The target in the packets' world, as mark_out_of_area lays it.
    const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
    const WorldFix& fx = drop_fix_on_ ? drop_fix_ : fix_;
    const MapPoint tw{ round_px(fx.x + (target.x() - me.x()) * scale),
                       round_px(fx.y + (target.y() - me.y() - cfg.packet_dy) * scale) };
    jump_.target_w = tw;
    jump_.dy = dy;
    jump_.level = level_monster(cfg, mobs, me, listed);
    // Settling at its take-off: the press's own judging.
    if (jump_.phase == JumpUpPhase::Settle && jump_.purpose == JumpUpFor::Target)
        return jump_at(cfg, JumpUpFor::Target, jump_.plan, target.slot, target.gen, target.name, now);

    const std::optional<JumpPlan> under_way =
        jump_.phase == JumpUpPhase::Approach ? std::optional<JumpPlan>(jump_.plan) : std::nullopt;
    JumpUpFacts f = jump_facts(cfg, JumpUpFor::Target, target.slot, target.gen, tw, dy, jump_.level, under_way, now,
                               false);
    JumpUpCall call = jump_up_rule(cfg, f);
    ++minute_jump_looks_;
    // Only a moment in the way: the jump worked out as if it were not, and
    // walked to -- stood at its take-off until it goes.
    JumpUpCall plan_call = call;
    if (!call.ok() && act_of(call.why) == Act::Passing) {
        JumpUpFacts g = f;
        g.owed = false;
        g.fix.uncertain = false;
        g.fix.state = WorldFix::State::Known;
        g.entry_ms = 0;
        g.in_air = false;
        g.last_press_ms = 0;
        g.last_drop_worked_ms = 0;
        g.step_judge = false;
        g.presses_minute = 0;
        g.held = false;
        g.focused = true;
        g.loot_owed = false;
        g.veto = nullptr;
        plan_call = jump_up_rule(cfg, g);
    }
    // A walk to this target's take-off given up: no settle is under way here
    // (jump_at has those), and another's is not this one's to end.
    const auto stop_walk = [this] {
        if (jump_.phase == JumpUpPhase::Approach && jump_.purpose == JumpUpFor::Target) jump_.phase = JumpUpPhase::Idle;
        publish_jump();
    };
    if (!plan_call.ok()) {
        const Act act = act_of(plan_call.why);
        if (act == Act::AsBefore) {
            if (plan_call.why == JumpUpWhy::JumpKey) jump_refused(cfg, plan_call, JumpUpFor::Target, target.slot,
                                                                  target.gen, target.name, now);
            stop_walk();
            return false;
        }
        jump_refused(cfg, plan_call, JumpUpFor::Target, target.slot, target.gen, target.name, now);
        stop_walk();
        if (act == Act::Level) {
            // Something to fight on the character's own level: the one above
            // is let go of -- not passed over -- and the next look picks the
            // one on the level.
            end_fight("a monster on its own level");
            have_target_ = false;
            return true;
        }
        // No jump up gets onto its floor: passed over, never walked under
        // blindly -- as one a walk does not get to.
        let_go_of_moves(cfg);
        if (gave_up_.size() < 16)
            gave_up_.push_back(GaveUpSpot{ target.x(), target.y(), now + 2ull * cfg.give_up_ms, true });
        end_fight("no jump up to it");
        watch_ = DamageWatch{};
        have_target_ = false;
        return true;
    }
    if (!call.ok()) jump_refused(cfg, call, JumpUpFor::Target, target.slot, target.gen, target.name, now);
    const JumpPlan j = *plan_call.plan;
    // At its take-off, a moment in the way: stood there, the walk to it kept.
    if (!call.ok() && f.stand && std::abs(f.stand->x - j.x) <= kJumpUpLeadPx) {
        if (jump_.phase == JumpUpPhase::Approach && jump_.purpose == JumpUpFor::Target) {
            jump_.plan = j;
            jump_.renewed_pass = passes_;
        }
        let_go_of_moves(cfg);
        publish_jump();
        return true;
    }
    // Walked to its take-off, settled there, pressed (jump_go, jump_at). A
    // walk to it that would leave the area: passed over as above.
    if (jump_go(cfg, JumpUpFor::Target, j, target.slot, target.gen, target.name, now)) return true;
    if (gave_up_.size() < 16)
        gave_up_.push_back(GaveUpSpot{ target.x(), target.y(), now + 2ull * cfg.give_up_ms, true });
    end_fight("no jump up to it");
    watch_ = DamageWatch{};
    have_target_ = false;
    return true;
}

bool Hunter::jump_go(const HunterConfig& cfg, JumpUpFor purpose, const JumpPlan& plan, int slot, std::uint32_t gen,
                     const std::string& name, std::uint64_t now) {
    if (jump_.phase == JumpUpPhase::Air || jump_in_air(now)) {
        let_go_of_moves(cfg);
        return true;
    }
    const std::optional<MapPoint> stand = zone_.stand;
    if (!zone_.page || !stand) {
        // In the air from something else, or past its floor's end: nothing
        // to walk from -- stood for.
        let_go_of_moves(cfg);
        return true;
    }
    // Settling at the take-off for this jump: the press's own judging. One
    // under way for another -- the way back's, another monster's -- is let go
    // of first; this one is then walked to its own take-off, never begun
    // where the other one stopped.
    const bool mine = jump_.phase == JumpUpPhase::Settle && jump_.purpose == purpose && jump_.slot == slot &&
                      jump_.gen == gen;
    if (jump_.phase == JumpUpPhase::Settle && !mine) jump_settle_end("another jump", false, now);
    if (mine || std::abs(stand->x - plan.x) <= kJumpUpLeadPx)
        return jump_at(cfg, purpose, plan, slot, gen, name, now);
    if (jump_.phase == JumpUpPhase::Idle || jump_.purpose != purpose || jump_.slot != slot || jump_.gen != gen) {
        jump_.phase = JumpUpPhase::Approach;
        jump_.purpose = purpose;
        jump_.slot = slot;
        jump_.gen = gen;
        jump_.name = name;
        jump_.taps = 0;
    }
    jump_.plan = plan;
    jump_.renewed_pass = passes_;
    // Along the floor to the take-off, looked along as walk_kept_in looks: the
    // take-offs are on the character's own floor, cut where a walk would
    // leave the box -- never refused but by a floor's end past it.
    const int dir = plan.x < stand->x ? -1 : 1;
    const int to_go = std::abs(plan.x - stand->x);
    const ZoneWalk w = walk_stays_in(*zone_.page, zone_.box, *stand, dir,
                                     std::clamp(to_go + kZoneWalkSlackPx, 1, kZoneWalkAheadPx));
    if (!w.ok) {
        JumpUpCall call;
        call.why = JumpUpWhy::Plan;
        call.no = JumpNo::Outside;
        call.px = w.out_px;
        jump_refused(cfg, call, purpose, slot, gen, name, now);
        let_go_of_moves(cfg);
        jump_.phase = JumpUpPhase::Idle;
        publish_jump();
        return false;
    }
    publish_jump();
    const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
    const WorldFix& fx = drop_fix_on_ ? drop_fix_ : fix_;
    int wdx = static_cast<int>(std::lround((plan.x - fx.x) / scale));
    if (wdx == 0) wdx = dir;
    walk(cfg, wdx, 0, now, WalkFor::Floor);
    return true;
}

bool Hunter::jump_at(const HunterConfig& cfg, JumpUpFor purpose, const JumpPlan& plan, int slot, std::uint32_t gen,
                     const std::string& name, std::uint64_t now) {
    if (jump_.phase == JumpUpPhase::Air || jump_in_air(now)) {
        let_go_of_moves(cfg);
        return true;
    }
    if (jump_.phase == JumpUpPhase::Settle && (jump_.purpose != purpose || jump_.slot != slot || jump_.gen != gen))
        jump_settle_end("another jump", false, now);
    const bool settling = jump_.phase == JumpUpPhase::Settle;
    const bool home = purpose == JumpUpFor::Home;
    const std::optional<MapPoint> target_w = home ? std::nullopt : std::optional<MapPoint>(jump_.target_w);
    // Target: the take-off under way kept (hysteresis); Home and Seek: the leg.
    JumpUpFacts f = jump_facts(cfg, purpose, slot, gen, target_w, jump_.dy, jump_.level,
                               std::optional<JumpPlan>(settling ? jump_.plan : plan), now, settling);
    const JumpUpCall call = jump_up_rule(cfg, f);
    // Waited for at the take-off, the walk let go of -- for a moment it may
    // go: the way back's give-up clock stands. Not through a back-off after a
    // settle refused: that clock runs, and a take-off never got off from is
    // given up on as a walk back that got nowhere.
    const auto stand_by = [&](bool clock_stands) {
        let_go_of_moves(cfg);
        if (home && clock_stands) jump_wait_home_ = true;
        publish_jump();
        return true;
    };

    if (!settling) {
        if (!call.ok()) {
            jump_refused(cfg, call, purpose, slot, gen, name, now);
            if (act_of(call.why) == Act::Passing) return stand_by(true);
            if (act_of(call.why) == Act::Level) end_fight("a monster on its own level");
            jump_.phase = JumpUpPhase::Idle;
            publish_jump();
            return false;
        }
        if (now < jump_backoff_ms_) return stand_by(false);
        // Settle: the walk let go of, and the stop it reports waited for.
        // Neither writes which way the character faces.
        const JumpUpState was = jump_;
        jump_ = JumpUpState{};
        jump_.phase = JumpUpPhase::Settle;
        jump_.purpose = purpose;
        jump_.slot = slot;
        jump_.gen = gen;
        jump_.name = name;
        jump_.plan = *call.plan;
        jump_.target_w = was.target_w;
        jump_.dy = was.dy;
        jump_.level = was.level;
        jump_.settle_from_ms = now;
        jump_.walk_held = walk_vk_ != 0;
        jump_.renewed_pass = passes_;
        ++minute_jump_settles_;
        let_go_of_moves(cfg);
        disengage();
        // No give-up while it settles and flies: the target's clock held
        // still for the settle -- given back at jump_settle_end what it had --
        // and started afresh at the press. The way back's stands in
        // go_home_zone.
        if (!home) {
            jump_.since0 = target_since_ms_;
            jump_.reach0 = reach_ms_;
            target_since_ms_ = reach_ms_ = now;
        }
        if (home) jump_wait_home_ = true;
        publish_jump();
        GB_DEBUG("Hunter: settling to jump up (%s) %s -- at x %d of the take-off %d on floor %d, onto floor %d",
                 jump_kind_name(jump_.plan.kind), for_text(purpose, name).c_str(), f.stand ? f.stand->x : 0,
                 jump_.plan.x, jump_.plan.feet_y + 1, jump_.plan.land_top);
        return true;
    }

    jump_.renewed_pass = passes_;
    if (call.ok()) return press_jump(cfg, call, name, now);
    // Waiting for the stop's report, for kDropSettleMs at most.
    if (call.why == JumpUpWhy::NotSettled && now < jump_.settle_from_ms + kDropSettleMs) {
        if (home) jump_wait_home_ = true;
        return true;
    }
    // Stopped off a column that holds: a fine tap toward the take-off, no
    // more than kJumpUpFineTapsMax -- then the stop waited for again.
    if (call.why == JumpUpWhy::OffColumn && jump_.taps < kJumpUpFineTapsMax && f.stand) {
        const int off = jump_.plan.x - f.stand->x;
        if (off != 0) {
            const int ms = std::clamp(static_cast<int>(std::lround(std::abs(off) * 1000.0 / kZoneWalkPxPerS)), 60, 200);
            bool tapped = false;
            if (auto token = coord_.acquire("hunt-move", Priority::Movement))
                tapped = press(off < 0 ? cfg.left_key : cfg.right_key, ms);
            if (tapped) {
                ++jump_.taps;
                jump_.settle_from_ms = clock_now();
                jump_.walk_held = true;
                if (home) jump_wait_home_ = true;
                GB_DEBUG("Hunter: jump up: stopped at x %d, %d px off the take-off %d (%s) -- tapped %s for %d ms "
                         "(%d of %d)",
                         f.stand->x, std::abs(off), jump_.plan.x, jump_no_name(call.no), off < 0 ? "left" : "right",
                         ms, jump_.taps, kJumpUpFineTapsMax);
                return true;
            }
        }
    }
    // A moment in the way at the press -- the gap since the last press, a
    // drop still in the air, the minute used up: waited out at the take-off.
    if (act_of(call.why) == Act::Passing && call.why != JumpUpWhy::NotSettled) return stand_by(true);
    jump_refused(cfg, call, purpose, slot, gen, name, now);
    jump_settle_end(call.why == JumpUpWhy::NotSettled ? "no report of standing came" : jump_why_name(call.why), true,
                    now);
    return false;
}

Hunter::JumpKeys Hunter::send_jump(JumpKind kind, const std::function<bool()>& go_on) {
    JumpKeys k;
    switch (kind) {
        case JumpKind::Up: {
            const InputManager::JumpPress p = input_.jump_once(kUpVk, go_on);
            k.went = p.sent != InputManager::JumpSent::NotSent;
            k.whole = k.went;
            k.down_ms = k.last_ms = p.first_down_ms;
            k.held_ms = p.held1;
            break;
        }
        case JumpKind::UpUp: {
            const InputManager::JumpPress p = input_.jump_twice(kUpVk, kJumpUpSecondAtMs, go_on);
            k.went = p.sent != InputManager::JumpSent::NotSent;
            k.whole = p.sent == InputManager::JumpSent::Both;
            k.down_ms = p.first_down_ms;
            k.last_ms = k.whole ? p.second_down_ms : p.first_down_ms;
            k.held_ms = std::max(p.held1, p.held2);
            break;
        }
        case JumpKind::CtrlUp: {
            const InputManager::DropPress p = input_.high_jump(go_on);
            k.went = p.sent == InputManager::DropSent::Sent || p.sent == InputManager::DropSent::Cut;
            k.whole = p.sent == InputManager::DropSent::Sent;
            k.down_ms = k.last_ms = p.down_ms;
            k.held_ms = p.held_ms;
            k.lead_ms = p.lead_ms;
            k.tail_ms = p.tail_ms;
            break;
        }
    }
    return k;
}

bool Hunter::press_jump(const HunterConfig& cfg, const JumpUpCall& call, const std::string& name, std::uint64_t now) {
    const JumpPlan j = *call.plan;
    const WorldFix f = drop_fix_;
    const std::shared_ptr<const GameMapBook::Page> page = zone_.page;
    const std::string stem = page ? page->stem : std::string();
    const JumpUpFor purpose = jump_.purpose;
    std::optional<PacketWorld::AirCounts> air0;
    JumpKeys k;
    {
        // The token only when nobody holds it, as the drop's: nothing but a
        // Critical hold takes it while Ctrl is down -- which go_on sees.
        auto token = coord_.acquire_free("hunt-jump", Priority::Combat);
        if (!token) {
            if (jump_.token_from_ms == 0) jump_.token_from_ms = now;
            if (now < jump_.token_from_ms + kJumpUpTokenWaitMs) return true;
            jump_settle_end("the keys stayed busy", true, now);
            return false;
        }
        // The counts the moment before: a jump of the player's own a moment
        // earlier is not this key's.
        air0 = eyes_.air ? eyes_.air() : std::nullopt;
        const std::function<bool()> go_on = [this] {
            return running_.load() && !coord_.critical_active() && focused();
        };
        k = hands_.jump ? hands_.jump(j.kind, go_on) : send_jump(j.kind, go_on);
    }
    const std::uint64_t after = clock_now();
    if (!k.went) {
        GB_INFO("Hunter: jump up not pressed -- no Up went down (a hold, the game behind, or a chord's keys)");
        jump_settle_end("not pressed", true, after);
        return true;
    }

    // Up went out: counted against the limits, and watched for a map change
    // for kPressWatchMs whatever comes of it.
    jump_presses_.push_back(after);
    if (purpose != JumpUpFor::Home && jump_.slot >= 0) {
        bool counted = false;
        for (TargetPresses& t : jump_targets_)
            if (t.slot == jump_.slot && t.gen == jump_.gen) {
                ++t.n;
                counted = true;
            }
        if (!counted) {
            if (jump_targets_.size() >= 64) jump_targets_.erase(jump_targets_.begin());
            jump_targets_.push_back(TargetPresses{ jump_.slot, jump_.gen, 1 });
        }
    }
    const int for_it = jump_presses_for(jump_.slot, jump_.gen);
    ++minute_jumps_[static_cast<std::size_t>(j.kind)];
    last_press_ms_ = after;
    // A drop's press still watched kept watched beside this one; a jump's
    // left from before, closed.
    if (press_.open && !press_.jump) press_other_ = press_;
    else if (press_other_.jump) press_other_.open = false;
    press_ = PressRecord{};
    press_.open = true;
    press_.down_ms = k.down_ms;
    press_.epoch0 = f.epoch;
    press_.x0 = j.x;
    press_.y0 = j.feet_y;
    press_.stem = stem;
    press_.jump = true;
    press_.kind = j.kind;
    jump_t_key_ = k.down_ms;
    jump_landed_ = false;
    if (purpose != JumpUpFor::Home) target_since_ms_ = reach_ms_ = after;

    jump_.phase = JumpUpPhase::Air;
    jump_.plan = j;
    jump_.key_ms = k.down_ms;
    jump_.last_key_ms = k.last_ms != 0 ? k.last_ms : k.down_ms;
    jump_.whole = k.whole;
    jump_.air_known = air0.has_value();
    jump_.air0 = air0.value_or(PacketWorld::AirCounts{});
    jump_.took_off = false;
    jump_.x0 = j.x;
    jump_.y0 = j.feet_y;
    jump_.stem = stem;
    jump_.page = page;
    jump_.box = zone_.box;
    jump_trace_until_ = k.down_ms + kJumpUpTraceMs;
    jump_traced_ms_ = k.down_ms;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++status_.jumps;
    }
    publish_jump();

    // How the keys went, for the log: Ctrl's lead and tail, ↑'s hold, ↑↑'s
    // second.
    char keys[160];
    if (j.kind == JumpKind::CtrlUp)
        std::snprintf(keys, sizeof keys, "Ctrl lead %u ms, Up held %u ms, tail %u ms%s", k.lead_ms, k.held_ms,
                      k.tail_ms, k.whole ? "" : " -- Up cut short");
    else if (j.kind == JumpKind::UpUp && k.whole)
        std::snprintf(keys, sizeof keys, "Up held %u ms, second Up %llu ms after the first", k.held_ms,
                      static_cast<unsigned long long>(k.last_ms > k.down_ms ? k.last_ms - k.down_ms : 0));
    else if (j.kind == JumpKind::UpUp)
        std::snprintf(keys, sizeof keys, "Up held %u ms, second Up not sent (a hold, or too late) -- judged as a "
                      "plain Up", k.held_ms);
    else
        std::snprintf(keys, sizeof keys, "Up held %u ms", k.held_ms);
    const JumpPeak pk = jump_peak(j.kind);
    GB_INFO("Hunter: jump up (%s) %s: from %d,%d on floor %d of %s (trusted: %s), onto floor %d at x %d (rise %d; "
            "peak %d..%d, headroom %d; no floor outside the area within %d above), run %d..%d, inside the area; no "
            "band round a gate (Ctrl+Up on a gate does not change the map); %s for it, %d this minute; %s",
            jump_kind_name(j.kind), for_text(purpose, name).c_str(), j.x, j.feet_y, j.feet_y + 1, stem.c_str(),
            trust_text(trust_.how()), j.land_top, j.land_x, j.rise, pk.lo, pk.hi, j.headroom, kJumpUpCeilingPx, j.run_l,
            j.run_r, purpose == JumpUpFor::Home ? "1st" : nth(std::max(1, for_it)).c_str(), jump_presses_minute(after),
            keys);
    if (k.held_ms > kJumpUpHeldMaxMs) {
        GB_WARN("Hunter: Up was held %u ms (over %u) -- jumps up OFF until the next BẮT ĐẦU", k.held_ms,
                kJumpUpHeldMaxMs);
        latch_jumps(JumpUpOff::HeldLong);
    }
    (void)cfg;
    return true;
}

// ---------------------------------------------------------------- the flight

void Hunter::jump_follow(const HunterConfig& cfg, std::uint64_t now) {
    // Your own 0x002A and 0x0031 for kJumpUpTraceMs after a jump's key: where
    // the flight went, for the next log (DEBUG).
    if (jump_trace_until_ != 0 && eyes_.own_since) {
        for (const PacketWorld::OwnTrace& r : eyes_.own_since(jump_traced_ms_)) {
            if (r.kind != OwnReport::Air && r.kind != OwnReport::Cast) continue;
            GB_DEBUG("Hunter: jump up +%lld ms: %s state %u at %.0f,%.0f, vy %.1f",
                     static_cast<long long>(r.ms) - static_cast<long long>(jump_t_key_),
                     r.kind == OwnReport::Air ? "0x002A" : "0x0031", static_cast<unsigned>(r.state),
                     static_cast<double>(r.x), static_cast<double>(r.y), static_cast<double>(r.vy));
            jump_traced_ms_ = r.ms + 1;
        }
        if (now >= jump_trace_until_) jump_trace_until_ = 0;
    }
    // A settle the pass did not renew at the last look: the target came in
    // reach, went, or the pass never got that far. A walk to a take-off the
    // same, with no back-off.
    if (jump_.phase == JumpUpPhase::Settle && jump_.renewed_pass + 1 < passes_)
        jump_settle_end("not wanted at the last look", true, now);
    if (jump_.phase == JumpUpPhase::Approach && jump_.renewed_pass + 1 < passes_) {
        jump_.phase = JumpUpPhase::Idle;
        publish_jump();
    }
    if (jump_.phase != JumpUpPhase::Air) return;

    const WorldFix& f = drop_fix_;
    if (!drop_fix_on_ || f.state == WorldFix::State::NoCapture || f.state == WorldFix::State::FirstReport) {
        end_flight(cfg, JumpUpVerdict::Unknown, now, 0, 0, "no capture to say", {});
        return;
    }
    if (f.state == WorldFix::State::Lost) {
        end_flight(cfg, JumpUpVerdict::Unknown, now, 0, 0, "the packets lost you", {});
        return;
    }
    // Took off, and as which key: the counts by kind since the key, and your
    // own reports since it.
    int plain = 0, second = 0, high = 0;
    if (jump_.air_known && eyes_.air) {
        if (const std::optional<PacketWorld::AirCounts> a = eyes_.air()) {
            const auto since = [](std::uint64_t now_n, std::uint64_t was) {
                return now_n >= was ? static_cast<int>(std::min<std::uint64_t>(now_n - was, 1000)) : 0;
            };
            plain = std::max(0, since(a->jumps, jump_.air0.jumps) - since(a->walkoffs, jump_.air0.walkoffs));
            second = since(a->second, jump_.air0.second);
            high = since(a->high, jump_.air0.high);
        }
    }
    const std::vector<PacketWorld::OwnTrace> reports =
        eyes_.own_since ? eyes_.own_since(jump_.key_ms) : std::vector<PacketWorld::OwnTrace>{};
    const JumpCheck::Flight fl = JumpCheck::flight_of(reports);
    std::uint64_t first_up_ms = 0;
    for (const PacketWorld::OwnTrace& r : reports) {
        if (r.kind != OwnReport::Air) continue;
        if (r.state == 1 && r.vy < 0.0f) ++plain;
        if (r.state == 2) ++high;
        if (r.state != 0 && r.vy < 0.0f && first_up_ms == 0) first_up_ms = r.ms;
        if (r.state == 6 && (first_up_ms == 0 || r.ms <= first_up_ms + kJumpUpSecondSeenMs)) ++second;
    }
    if (fl.took_off || plain + second + high > 0) jump_.took_off = true;
    const bool reported = f.report_ms >= jump_.key_ms;
    if (f.airborne && reported) jump_.took_off = true;

    // Landed: the first 0x002A on the ground after the take-off, or the first
    // walk's report -- never a cast's, its y held at the top; with none of
    // your reports to go by, the fix -- a cast's report would have it on the
    // ground in mid-air.
    bool landed = false;
    int lx = 0, ly = 0;
    if (fl.landed) {
        landed = true;
        lx = round_px(fl.land.x);
        ly = round_px(fl.land.y);
    } else if (reports.empty() && jump_.took_off && reported && !f.airborne && f.state == WorldFix::State::Known) {
        landed = true;
        lx = round_px(f.rx);
        ly = round_px(f.ry);
    }
    if (!landed) {
        if (!jump_.took_off && now >= jump_.last_key_ms + kJumpUpJudgeMs) {
            end_flight(cfg, JumpUpVerdict::Nothing, now, round_px(f.rx), round_px(f.ry), nullptr, {});
            return;
        }
        if (now >= jump_.key_ms + kJumpUpAirMaxMs)
            end_flight(cfg, JumpUpVerdict::Unknown, now, 0, 0, "still in the air", {});
        return;
    }

    // The kind it should have been: ↑↑ without its second ↑ is a plain ↑.
    JumpKind meant = jump_.plan.kind;
    if (meant == JumpKind::UpUp && !jump_.whole) meant = JumpKind::Up;
    const bool its_kind = meant == JumpKind::Up     ? plain + second + high > 0
                          : meant == JumpKind::UpUp ? second > 0
                                                    : high > 0;
    // The flight, for the log: the take-off's lag, the time up, g≈.
    std::string more;
    std::optional<double> g;
    int flight_ms = -1;
    if (fl.took_off && fl.landed && fl.land.ms >= fl.off.ms) {
        flight_ms = static_cast<int>(fl.land.ms - fl.off.ms);
        if (fl.land.ms > fl.push.ms)
            g = gravity_from(fl.push.y, static_cast<double>(fl.push.vy) * PacketWorld::kStepsPerSecond, fl.land.y,
                             static_cast<double>(fl.land.ms - fl.push.ms) / 1000.0);
        char text[200];
        std::snprintf(text, sizeof text, "0x002A: %s %.0f at +%lld ms, landed +%lld; flight %d ms%s",
                      fl.off.state == 2   ? "high"
                      : fl.off.state == 6 ? "second"
                                          : "jump",
                      static_cast<double>(fl.off.vy), static_cast<long long>(fl.off.ms - jump_.key_ms),
                      static_cast<long long>(fl.land.ms - jump_.key_ms), flight_ms, g ? "" : ", g not known");
        more = text;
        if (g) {
            std::snprintf(text, sizeof text, ", g≈%.0f", *g);
            more += text;
        }
    }

    // Where it came down, against the file: off its floors, outside the
    // area, not the key's jump, the planned floor, a plain ↑ where ↑↑'s second
    // ↑ was not sent, back where it stood, or another floor.
    const GameMapBook::Page* p = jump_.page.get();
    const std::optional<MapFloor> on = p ? p->floor_near(lx, ly) : std::nullopt;
    const std::optional<int> planned = p ? p->follow_to(jump_.plan.land_x, jump_.plan.land_top - 1, lx) : std::nullopt;
    const std::optional<int> own = p ? p->follow_to(jump_.x0, jump_.y0, lx) : std::nullopt;
    const bool cut = jump_.plan.kind == JumpKind::UpUp && !jump_.whole;
    JumpUpVerdict v = JumpUpVerdict::Lower;
    if (!on)
        v = JumpUpVerdict::OffMap;
    else if (!jump_.box.contains(lx, ly))
        v = JumpUpVerdict::Outside;
    else if (!its_kind)
        v = JumpUpVerdict::KindMiss;
    else if (planned && std::abs(on->y - *planned) <= kJumpUpFloorMatchPx)
        v = JumpUpVerdict::Worked;
    else if (cut)
        v = JumpUpVerdict::Unknown;
    else if (own && std::abs(on->y - *own) <= kJumpUpFloorMatchPx && std::abs(ly - jump_.y0) <= kJumpUpLandPx)
        v = JumpUpVerdict::FellBack;
    else if (on->y < (planned ? *planned : jump_.plan.land_top) - kJumpUpFloorMatchPx)
        v = JumpUpVerdict::Higher;

    // g from a clean flight only -- up and down onto the planned floor; a
    // bump from below is no free flight. Logged, never fed back: twice out of
    // range in a run, the jumps are not what the model says.
    if (g && v == JumpUpVerdict::Worked) {
        minute_jump_g_sum_ += *g;
        ++minute_jump_g_n_;
        if ((*g < kJumpUpGravityLo || *g > kJumpUpGravityHi) && ++jump_gravity_out_ >= kJumpUpGravityOut) {
            GB_INFO("Hunter: g≈%.0f measured, %d flights out of %.0f..%.0f this run -- jumps up OFF until the next "
                    "BẮT ĐẦU (the character jumps otherwise than GoBot works out)",
                    *g, jump_gravity_out_, kJumpUpGravityLo, kJumpUpGravityHi);
            latch_jumps(JumpUpOff::Physics);
        }
    }
    // A fall's hint: bumped from below -- a short flight -- or came up short.
    if (flight_ms >= 0 && (v == JumpUpVerdict::FellBack || v == JumpUpVerdict::Lower)) {
        const double arc_ms = shortest_arc_s(meant) * 1000.0;
        more += flight_ms < kJumpUpBumpShare * arc_ms ? "; bumped from below" : "; came up short";
    }
    end_flight(cfg, v, now, lx, ly, v == JumpUpVerdict::Unknown ? "the second Up not sent: a plain Up, as planned for"
                                                               : nullptr,
               more);
}

// ---------------------------------------------------------------- endings

void Hunter::jump_settle_end(const char* why, bool backoff, std::uint64_t now) {
    if (jump_.phase != JumpUpPhase::Settle) return;
    // The give-up clock as the settle found it, the settle's time not counted.
    if (jump_.purpose != JumpUpFor::Home) {
        const std::uint64_t spent = now > jump_.settle_from_ms ? now - jump_.settle_from_ms : 0;
        if (target_since_ms_ == jump_.settle_from_ms) target_since_ms_ = jump_.since0 + spent;
        if (reach_ms_ == jump_.settle_from_ms) reach_ms_ = jump_.reach0 + spent;
        // Counted for its monster: kJumpUpSettlesPerTarget let go of without a
        // press, and no more are begun for it.
        if (jump_.slot >= 0) {
            TargetPresses* it = nullptr;
            for (TargetPresses& t : jump_targets_)
                if (t.slot == jump_.slot && t.gen == jump_.gen) it = &t;
            if (it == nullptr) {
                if (jump_targets_.size() >= 64) jump_targets_.erase(jump_targets_.begin());
                jump_targets_.push_back(TargetPresses{ jump_.slot, jump_.gen, 0 });
                it = &jump_targets_.back();
            }
            ++it->settles;
        }
    }
    jump_.phase = JumpUpPhase::Idle;
    if (backoff) jump_backoff_ms_ = now + kJumpUpSettleBackoffMs;
    GB_DEBUG("Hunter: jump up not pressed -- %s%s", why, backoff ? "; none begun again for 2 s" : "");
    publish_jump();
}

void Hunter::jump_hold(const char* why) {
    if (jump_.phase == JumpUpPhase::Settle) {
        jump_settle_end(why, true, clock_now());
    } else if (jump_.phase == JumpUpPhase::Approach) {
        jump_.phase = JumpUpPhase::Idle;
        publish_jump();
    } else if (jump_.phase == JumpUpPhase::Air) {
        end_flight(config(), JumpUpVerdict::Unknown, clock_now(), 0, 0, why, {});
    }
}

void Hunter::end_flight(const HunterConfig& cfg, JumpUpVerdict v, std::uint64_t now, int lx, int ly, const char* why,
                        const std::string& more) {
    if (jump_.phase != JumpUpPhase::Air) return;
    const JumpUpState j = jump_;
    jump_.phase = JumpUpPhase::Idle;
    const unsigned long long ms = now > j.key_ms ? now - j.key_ms : 0;
    const char* stem = j.stem.empty() ? "this map" : j.stem.c_str();
    const std::string tail = more.empty() ? std::string() : " (" + more + ")";
    const MapPoint take{ j.x0, j.y0 };
    const MapPoint land{ j.plan.land_x, j.plan.land_top - 1 };
    bool learn = false, fail = false, solid = false;
    if (v != JumpUpVerdict::Unknown && v != JumpUpVerdict::Gate) jump_landed_ = true;
    switch (v) {
        case JumpUpVerdict::Worked:
            jump_fails_row_ = 0;
            ++minute_jump_worked_;
            // Up a floor: the steps back counted each way are another floor's.
            retreat_[0] = retreat_[1] = Retreat{};
            if (j.purpose != JumpUpFor::Home) {
                last_jump_worked_ms_ = now;
                if (have_target_ && target_slot_ == j.slot && target_gen_ == j.gen) target_since_ms_ = reach_ms_ = now;
            } else {
                // The way back asked again from the landing: progress.
                way_asked_ = false;
                return_quiet_ms_ = 0;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.jumps_worked;
            }
            GB_INFO("Hunter: jump up worked: landed at %d,%d on floor %d, %d px up, %llu ms after the key%s", lx, ly,
                    j.plan.land_top, j.y0 - ly, ms, tail.c_str());
            break;
        case JumpUpVerdict::FellBack:
        case JumpUpVerdict::Lower:
            learn = fail = true;
            if (v == JumpUpVerdict::FellBack) {
                ++minute_jump_fell_;
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.jumps_fell_back;
            } else {
                ++minute_jump_lower_;
            }
            GB_INFO("Hunter: jump up %s to %d,%d%s -- no %s to floor %d within %d px of x %d this session; %d in a row",
                    v == JumpUpVerdict::FellBack ? "FELL BACK" : "landed LOWER", lx, ly, tail.c_str(),
                    jump_kind_name(j.plan.kind), j.plan.land_top, GameMapBook::kNoJumpXPx, j.x0, jump_fails_row_ + 1);
            if (v == JumpUpVerdict::FellBack && j.page) {
                // A second fall back onto the same floor: solid from below.
                const MapTrail l = j.page->follow(land.x, land.y, -1);
                const MapTrail r = j.page->follow(land.x, land.y, 1);
                if (!l.pts.empty() && !r.pts.empty()) {
                    const MapPoint le = l.pts.back(), re = r.pts.back();
                    FellOnto* it = nullptr;
                    for (FellOnto& e : jump_fell_)
                        if (e.stem == j.stem && e.l_end.x == le.x && e.l_end.y == le.y && e.r_end.x == re.x &&
                            e.r_end.y == re.y)
                            it = &e;
                    if (it == nullptr) {
                        if (jump_fell_.size() >= 64) jump_fell_.erase(jump_fell_.begin());
                        jump_fell_.push_back(FellOnto{ j.stem, le, re, 0 });
                        it = &jump_fell_.back();
                    }
                    if (++it->n >= kJumpUpSolidAfter && book_ && !j.stem.empty()) {
                        solid = true;
                        book_->learn_solid(j.stem, land);
                        GB_INFO("Hunter: floor %d (x %d..%d) taken as solid from below this session -- %d jumps up "
                                "fell back from it",
                                j.plan.land_top, le.x, re.x, it->n);
                    }
                }
            }
            break;
        case JumpUpVerdict::KindMiss: {
            fail = true;
            ++minute_jump_kind_miss_;
            const std::size_t k = static_cast<std::size_t>(j.plan.kind);
            const int n = ++jump_kind_misses_[k];
            GB_INFO("Hunter: %s came out as %s -- %d of %d%s", jump_kind_name(j.plan.kind),
                    j.plan.kind == JumpKind::CtrlUp ? "a plain jump (state 1, no state 2)"
                                                    : "a single jump (no state 6)",
                    n, kJumpUpKindMisses, tail.c_str());
            if (n >= kJumpUpKindMisses) {
                jump_kinds_off_ = static_cast<std::uint8_t>(jump_kinds_off_ | bit_of(j.plan.kind));
                ++jump_epoch_;
                GB_INFO("Hunter: %s OFF until the next BẮT ĐẦU: %d presses the game did not take as that jump",
                        jump_kind_name(j.plan.kind), n);
            }
            break;
        }
        case JumpUpVerdict::Higher:
            ++minute_jump_higher_;
            latch_jumps(JumpUpOff::Physics);
            GB_INFO("Hunter: jump up landed at %d,%d on floor %d, ABOVE the planned %d -- jumps up OFF until the next "
                    "BẮT ĐẦU%s",
                    lx, ly, ly + 1, j.plan.land_top, tail.c_str());
            break;
        case JumpUpVerdict::Outside:
            ++minute_jump_higher_;
            latch_jumps(JumpUpOff::Physics);
            GB_WARN("Hunter: jump up landed at %d,%d, OUTSIDE the hunting area -- jumps up OFF until the next BẮT ĐẦU; "
                    "the walk back takes over%s",
                    lx, ly, tail.c_str());
            break;
        case JumpUpVerdict::OffMap:
            ++minute_jump_unknown_;
            if (book_ && !j.stem.empty()) book_->distrust(j.stem);
            latch_jumps(JumpUpOff::OffMap);
            GB_WARN("Hunter: jump up landed at %d,%d, on no floor of %s's file -- jumps up OFF until the next BẮT ĐẦU; "
                    "the file no longer trusted%s",
                    lx, ly, stem, tail.c_str());
            break;
        case JumpUpVerdict::Nothing:
            fail = true;
            ++minute_jump_nothing_;
            GB_INFO("Hunter: jump up (%s) at %d,%d did nothing in %llu ms -- %d in a row", jump_kind_name(j.plan.kind),
                    j.x0, j.y0, static_cast<unsigned long long>(kJumpUpJudgeMs), jump_fails_row_ + 1);
            break;
        case JumpUpVerdict::Unknown:
        case JumpUpVerdict::Gate:
        case JumpUpVerdict::None:
            ++minute_jump_unknown_;
            GB_DEBUG("Hunter: what came of the jump up at %d,%d is not known -- %s", j.x0, j.y0,
                     why ? why : "not said");
            v = JumpUpVerdict::Unknown;
            break;
    }
    if (learn && book_ && !j.stem.empty()) {
        book_->learn_no_jump(j.stem, GameMapBook::NoJump{ take, land, static_cast<std::uint8_t>(j.plan.kind) });
        ++jump_epoch_;
    }
    if (solid) ++jump_epoch_;
    if (fail) {
        ++jump_fails_row_;
        ++jump_fails_run_;
        if (j.purpose != JumpUpFor::Home) {
            // The monster it was for, passed over a while -- not counted as
            // given up: the minute line counts it among the jumps. By the
            // seek too, in view or not, by its slot.
            if (have_target_ && target_slot_ == j.slot && target_gen_ == j.gen) {
                if (gave_up_.size() < 16)
                    gave_up_.push_back(GaveUpSpot{ target_x_, target_y_, now + 2ull * cfg.give_up_ms, true });
                end_fight("the jump up did not get there");
                have_target_ = false;
            }
            if (j.slot >= 0) seek_skip(j.slot, j.gen, now + 2ull * cfg.give_up_ms, SeekNo::Passed);
            if (j.purpose == JumpUpFor::Seek && seek_.on && seek_.slot == j.slot && seek_.gen == j.gen)
                seek_let_go(nullptr);
        } else if (v == JumpUpVerdict::FellBack || v == JumpUpVerdict::Lower) {
            // On the way back: a strike, as a walk back that got nowhere.
            end_return();
            ++return_strikes_;
            ++minute_return_give_ups_;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.return_give_ups;
            }
            if (return_strikes_ >= kSpotMaxTries) {
                spot_why_ = SpotWhy::GaveUp;
                GB_INFO("Hunter: the way back's jumps up got nowhere %d times in a row -- no more tries",
                        return_strikes_);
            }
        }
        if (jump_fails_row_ >= kJumpUpFailsInRow || jump_fails_run_ >= kJumpUpFailsPerRun) {
            latch_jumps(JumpUpOff::Fails);
            GB_INFO("Hunter: %d jumps up %s did not get there -- jumps up OFF until the next BẮT ĐẦU",
                    jump_fails_row_ >= kJumpUpFailsInRow ? jump_fails_row_ : jump_fails_run_,
                    jump_fails_row_ >= kJumpUpFailsInRow ? "in a row" : "this run");
        }
    }
    // What the press's record has to undo, should the map turn out changed --
    // the newest's, or the one kept beside a drop's since.
    PressRecord* rec = nullptr;
    for (PressRecord* c : { &press_, &press_other_ })
        if (c->open && c->jump && c->down_ms == j.key_ms) rec = c;
    if (rec != nullptr) {
        rec->jverdict = v;
        rec->no_jump = learn && book_ && !j.stem.empty();
        rec->failed = fail;
        rec->solid = solid;
        rec->solid_at = land;
        rec->minute_from = minute_from_ms_;
    }
    publish_jump();
}

void Hunter::jump_gate(PressRecord& r, std::uint64_t now) {
    r.open = false;
    const WorldFix& f = drop_fix_;
    // Whatever was judged of it before is undone: a no-jump point, a floor
    // taken as solid, a fail, its count in this minute's line.
    if (r.no_jump && book_) book_->unlearn_no_jump(r.stem, { r.x0, r.y0 });
    if (r.solid && book_) book_->unlearn_solid(r.stem, r.solid_at);
    if (r.no_jump || r.solid) ++jump_epoch_;
    if (r.failed) {
        if (jump_fails_row_ > 0) --jump_fails_row_;
        if (jump_fails_run_ > 0) --jump_fails_run_;
    }
    if (r.jverdict != JumpUpVerdict::None && r.minute_from == minute_from_ms_) {
        int* n = nullptr;
        switch (r.jverdict) {
            case JumpUpVerdict::Worked:   n = &minute_jump_worked_; break;
            case JumpUpVerdict::FellBack: n = &minute_jump_fell_; break;
            case JumpUpVerdict::KindMiss: n = &minute_jump_kind_miss_; break;
            case JumpUpVerdict::Lower:    n = &minute_jump_lower_; break;
            case JumpUpVerdict::Higher:
            case JumpUpVerdict::Outside:  n = &minute_jump_higher_; break;
            case JumpUpVerdict::Nothing:  n = &minute_jump_nothing_; break;
            default:                      n = &minute_jump_unknown_; break;
        }
        if (*n > 0) --*n;
    }
    if (jump_.phase == JumpUpPhase::Air && jump_.key_ms == r.down_ms) jump_.phase = JumpUpPhase::Idle;
    ++minute_jump_gate_;
    // Both ends are gates: where it pressed, on the map it left, and where the
    // entry put it, on the one it came to.
    const MapPoint to{ round_px(f.entry_x), round_px(f.entry_y) };
    if (book_) {
        if (!r.stem.empty()) book_->learn_gate(r.stem, { r.x0, r.y0 });
        if (f.map_known) book_->learn_gate(GameMapBook::stem_of(f.map, f.area), to);
    }
    latch_jumps(JumpUpOff::MapChanged);
    drop_halt_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.jump_map_changed = true;
    }
    publish_jump();
    char where[48] = "a map not known";
    if (f.map_known)
        std::snprintf(where, sizeof where, "map %u/%u", static_cast<unsigned>(f.map), static_cast<unsigned>(f.area));
    GB_WARN("Hunter: MAP CHANGED %.1f s after %s at %d,%d on %s -> %s at %d,%d -- hunting STOPS, Nhảy lên to be "
            "unticked and saved",
            static_cast<double>(now > r.down_ms ? now - r.down_ms : 0) / 1000.0, jump_kind_name(r.kind), r.x0, r.y0,
            r.stem.empty() ? "a map not trusted" : r.stem.c_str(), where, to.x, to.y);
}

void Hunter::latch_jumps(JumpUpOff why) {
    // A map change and stuck keys stop the hunt: they are said over any other.
    if (jump_latch_ != JumpUpOff::None && why != JumpUpOff::MapChanged && why != JumpUpOff::KeysStuck) return;
    jump_latch_ = why;
    // A walk to a take-off, or a settle, is over; a flight is still judged.
    if (jump_.phase == JumpUpPhase::Settle) jump_settle_end("jumps up are off", false, clock_now());
    else if (jump_.phase == JumpUpPhase::Approach) jump_.phase = JumpUpPhase::Idle;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.jump_off = why;
    }
    publish_jump();
}

// ---------------------------------------------------------------- the log

void Hunter::jump_refused(const HunterConfig& cfg, const JumpUpCall& call, JumpUpFor purpose, int slot,
                          std::uint32_t gen, const std::string& name, std::uint64_t now) {
    // Every refusal acted on, for the minute line's "refused:".
    ++minute_jump_why_[static_cast<std::size_t>(call.why)];
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.jump_wait = call.why;
        status_.jump_wait_px = call.px;
        status_.jump_wait_no = call.no;
        status_.jump_wait_ms = now;
        status_.jump_wait_for = purpose;
        status_.jump_wait_name = purpose == JumpUpFor::Home ? std::string() : name;
    }
    // Said per monster, or for the way back (slot -1).
    const int who = purpose == JumpUpFor::Home ? -1 : slot;
    if (who != jump_said_slot_ || gen != jump_said_gen_) {
        jump_said_slot_ = who;
        jump_said_gen_ = gen;
        jump_said_why_ = JumpUpWhy::None;
        jump_said_info_ = 0;
    }
    if (call.why == jump_said_why_) return;
    jump_said_why_ = call.why;
    char more[256] = "";
    switch (call.why) {
        case JumpUpWhy::Plan:
            std::snprintf(more, sizeof more, " (%s; its floor %d px above yours)", jump_no_name(call.no), call.px);
            break;
        case JumpUpWhy::OffColumn:
            std::snprintf(more, sizeof more, " (%d px off the take-off: %s)", std::abs(call.px), jump_no_name(call.no));
            break;
        case JumpUpWhy::JumpKey:
            std::snprintf(more, sizeof more, " (the jump key is 0x%02X; jumps up go on Up, 0x26)",
                          static_cast<unsigned>(cfg.jump_key));
            break;
        case JumpUpWhy::MapUnknown: {
            const MapTrust::Report r = trust_.report();
            std::snprintf(more, sizeof more, " (%s)", MapTrust::why_text(r.why));
            break;
        }
        case JumpUpWhy::Latched:
            std::snprintf(more, sizeof more, " (%s)",
                          jump_latch_ != JumpUpOff::None ? jump_off_name(jump_latch_)
                                                          : "the key it needs is off for the run");
            break;
        default:
            break;
    }
    if (call.placed) {
        const std::size_t at = std::strlen(more);
        if (at < sizeof more)
            std::snprintf(more + at, sizeof more - at, "; the monster's floor %d at x %d", call.target.y + 1,
                          call.target.x);
    }
    const std::uint64_t bit = 1ull << static_cast<unsigned>(call.why);
    const std::string what = purpose == JumpUpFor::Home
                                 ? std::string("on the way home")
                                 : "to " + (name.empty() ? std::string("the monster") : name);
    if (said_at_info(call.why) && (jump_said_info_ & bit) == 0) {
        jump_said_info_ |= bit;
        GB_INFO("Hunter: not jumping up %s -- %s%s", what.c_str(), jump_why_name(call.why), more);
    } else {
        GB_DEBUG("Hunter: not jumping up %s -- %s%s", what.c_str(), jump_why_name(call.why), more);
    }
}

void Hunter::jump_would(const JumpPlan& plan, JumpUpFor purpose, int slot, std::uint32_t gen, const std::string& name,
                        std::uint64_t now) {
    // Drawn by the map tab while fresh: the plan as it is now, every pass.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.jump_would_plan = plan;
        status_.jump_would_ms = now;
    }
    // Once a monster or way -- the way by its key and landing floor, its
    // take-off moving as it is worked out from each standing point.
    const int who = purpose == JumpUpFor::Home ? -1 : slot;
    const int kind = static_cast<int>(plan.kind);
    if (who == jump_would_slot_ && gen == jump_would_gen_ && kind == jump_would_kind_ &&
        std::abs(plan.land_top - jump_would_top_) <= GameMapBook::kFollowLinkPx)
        return;
    jump_would_slot_ = who;
    jump_would_gen_ = gen;
    jump_would_kind_ = kind;
    jump_would_top_ = plan.land_top;
    ++minute_jump_would_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++status_.jumps_would;
    }
    if (jump_would_ms_ != 0 && now < jump_would_ms_ + kJumpUpSayMs) return;
    jump_would_ms_ = now;
    const HunterConfig cfg = config();
    GB_INFO("Hunter: a jump up would go here (%s): %s at x %d from floor %d onto floor %d, %s",
            !cfg.jump_up ? "jump_up is off" : "jumps up are off until the next BẮT ĐẦU", jump_kind_name(plan.kind),
            plan.x, plan.feet_y + 1, plan.land_top, for_text(purpose, name).c_str());
}

void Hunter::publish_jump() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.jump_phase = jump_.phase;
    status_.jump_for = jump_.purpose;
    status_.jump_kind = jump_.plan.kind;
    status_.jump_x = jump_.plan.x;
    status_.jump_land_top = jump_.plan.land_top;
    status_.jump_feet_y = jump_.plan.feet_y;
    status_.jump_land_x = jump_.plan.land_x;
    status_.jump_off = jump_latch_;
    status_.jump_kinds_off = jump_kinds_off_;
}

std::string Hunter::jump_minute_text() const {
    std::string out;
    const int pressed = minute_jumps_[0] + minute_jumps_[1] + minute_jumps_[2];
    if (minute_jump_looks_ == 0 && minute_jump_would_ == 0 && minute_jump_settles_ == 0 && pressed == 0 &&
        minute_jump_worked_ == 0 && minute_jump_fell_ == 0 && minute_jump_kind_miss_ == 0 &&
        minute_jump_nothing_ == 0 && minute_jump_lower_ == 0 && minute_jump_higher_ == 0 &&
        minute_jump_unknown_ == 0 && minute_jump_gate_ == 0)
        return out;
    char text[400];
    std::snprintf(text, sizeof text,
                  "; jump-ups: looks %d, would %d, settles %d, pressed %d (Up %d, Up,Up %d, Ctrl+Up %d), worked %d, "
                  "fell back %d, kind missed %d, nothing %d, lower/higher %d/%d, unknown %d, map changes %d",
                  minute_jump_looks_, minute_jump_would_, minute_jump_settles_, pressed, minute_jumps_[0],
                  minute_jumps_[1], minute_jumps_[2], minute_jump_worked_, minute_jump_fell_, minute_jump_kind_miss_,
                  minute_jump_nothing_, minute_jump_lower_, minute_jump_higher_, minute_jump_unknown_,
                  minute_jump_gate_);
    out = text;
    if (minute_jump_g_n_ > 0) {
        std::snprintf(text, sizeof text, "; g≈%.0f (%d flight%s)", minute_jump_g_sum_ / minute_jump_g_n_,
                      minute_jump_g_n_, minute_jump_g_n_ == 1 ? "" : "s");
        out += text;
    }
    // What refused the rest, most first.
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < kJumpUpWhys; ++i)
        if (minute_jump_why_[i] > 0) order.push_back(i);
    std::stable_sort(order.begin(), order.end(),
                     [this](std::size_t a, std::size_t b) { return minute_jump_why_[a] > minute_jump_why_[b]; });
    for (std::size_t k = 0; k < order.size(); ++k) {
        std::snprintf(text, sizeof text, "%s%s %d", k == 0 ? "; refused: " : ", ",
                      why_short(static_cast<JumpUpWhy>(order[k])), minute_jump_why_[order[k]]);
        out += text;
    }
    return out;
}

} // namespace gobot
