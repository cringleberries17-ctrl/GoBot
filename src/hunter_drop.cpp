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
// judged of it, halts the hunter for GoBot's window to stop the hunt and
// untick drop_down.
//
// Nothing here turns the character, nor writes which way it faces or the
// keys that set it: the turn and facing_ are the in-reach branch's alone.

#include "gobot/hunter.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
        case Hunter::DropWhy::Latched:
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
        case DropWhy::Latched:         return "drops are off until the next start";
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
        case DropWhy::NoDropHere:      return "Ctrl+Down did not drop here before";
    }
    return "?";
}

const char* Hunter::drop_off_name(DropOff o) {
    switch (o) {
        case DropOff::None:       return "on";
        case DropOff::Fails:      return "presses that did not drop";
        case DropOff::Rose:       return "Ctrl+Down acts as a jump here";
        case DropOff::OffMap:     return "a fall landed where the map file did not say";
        case DropOff::MapChanged: return "a press changed the map";
        case DropOff::HeldLong:   return "Down was held too long";
        case DropOff::KeysStuck:  return "a key-up of a drop did not go out";
    }
    return "?";
}

void Hunter::set_map_book(GameMapBook* book) {
    book_ = book;
    trust_.set_book(book);
}

void Hunter::set_drop_veto(std::function<const char*()> veto) { drop_veto_ = std::move(veto); }

bool Hunter::drop_wanted(const HunterConfig& cfg) {
    return cfg.drop_down && cfg.packet_positions == PacketPositions::On && !cfg.stay_put;
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

// ---------------------------------------------------------------- the rule

Hunter::DropCall Hunter::drop_rule(const HunterConfig& cfg, const DropFacts& f) {
    DropCall c;
    const auto no = [&c](DropWhy why, int px = 0) {
        c.why = why;
        c.px = px;
        return c;
    };

    // 1. Switched on: the packets steering, not standing still -- with the
    // spot on, only while it is in use -- and not off for the run.
    if (!cfg.drop_down) return no(DropWhy::Off);
    if (cfg.packet_positions != PacketPositions::On) return no(DropWhy::NoPackets);
    if (cfg.stay_put) return no(DropWhy::StayPut);
    if (cfg.spot && !f.spot_use) return no(DropWhy::Spot);
    if (f.latched) return no(DropWhy::Latched);
    if (f.owed) return no(DropWhy::KeysOwed);
    // 2. No key of the hunter's on Ctrl: Ctrl + ↓ would be pressed with it.
    for (const std::uint32_t vk : { cfg.jump_key, cfg.left_key, cfg.right_key, cfg.attack_key })
        if (vk != 0 && is_ctrl(vk)) return no(DropWhy::KeyClash, static_cast<int>(vk));
    // 3. The map book and GoBot's say: without them, never.
    if (!f.wired) return no(DropWhy::NotWired);
    // 4. The packets' own target.
    if (!f.packet_target) return no(DropWhy::NotPacketTarget);
    // 5. Nothing to fight on the character's own level.
    if (f.level_monster) return no(DropWhy::LevelMonster);
    // 6. Below, past the band, and drop_min_px at least.
    if (f.dy <= std::max(cfg.down(), cfg.drop_min_px)) return no(DropWhy::NotBelow, f.dy);
    // 7. Across.
    const int side = cfg.drop_side_px + (f.press ? kDropPressSlackPx : 0);
    if (std::abs(f.dx) > side) return no(DropWhy::TooFarSide, std::abs(f.dx));
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
    const int x0 = c.x0, y0 = c.y0;
    const std::optional<MapFloor> under = page.floor_near(x0, y0);
    if (f.press && !under) return no(DropWhy::OffFloor);
    // 13. Not the bottom ground: only a platform is dropped through.
    if (under) {
        c.stand_y = under->y;
        if (!under->platform()) return no(DropWhy::Bottom, under->attr);
    }
    // 14. Where the fall lands: the first floor below, a real fall down, not
    // past the monster -- and the same floor either side.
    const std::optional<MapFloor> land = page.floor_below(x0, y0, kDropFeetPx);
    if (!land) return no(DropWhy::NoLanding);
    c.land_y = land->y;
    if (land->y - y0 < kDropMinFallPx) return no(DropWhy::ShortFall, land->y - y0);
    const int ty = round_px(f.target_wy);
    if (land->y > ty + kDropFloorMatchPx) return no(DropWhy::PastMonster, land->y - ty);
    // kDropMarginSettlePx only for a place worked out along a walk, where the
    // stop may come a step on; standing, it is the report's own, and the
    // press's margin holds as it begins. At 24 px the player's own 06:48:49
    // -- 17 px in from the end of the monster's floor -- would never settle.
    const int margin = f.press || fix.settled() ? kDropMarginPressPx : kDropMarginSettlePx;
    for (const int x : { x0 - margin, x0 + margin }) {
        const std::optional<MapFloor> o = page.floor_below(x, y0, kDropFeetPx);
        if (!o || std::abs(o->y - land->y) > kDropSameFloorPx) return no(DropWhy::NoLanding, x - x0);
    }
    c.direct = std::abs(land->y - ty) <= kDropFloorMatchPx;
    // The player's answer 2: with the hunting spot on, never a drop that
    // leaves its area -- where it stands, where it lands and the target all
    // inside it.
    if (cfg.spot) {
        const Spot& a = f.area;
        const auto inside = [&a](float x, float y) {
            return std::abs(x - a.x) <= static_cast<float>(a.half_w) &&
                   std::abs(y - a.y) <= static_cast<float>(a.half_h);
        };
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
    // 16. Not where Ctrl + ↓ did not drop before.
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
    f.latched = drop_latch_ != DropOff::None;
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
    f.level_monster = level_monster(cfg, mobs, me, listed);
    f.entry_ms = drop_entry_on_ ? drop_entry_ms_ : now;
    f.settle_from_ms = drop_.settle_from_ms;
    f.walk_held = drop_.walk_held;
    f.in_air = in_air(now);
    f.last_press_ms = last_press_ms_;
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
    return f;
}

int Hunter::drop_presses_minute(std::uint64_t now) {
    drop_presses_.erase(std::remove_if(drop_presses_.begin(), drop_presses_.end(),
                                       [now](std::uint64_t t) { return now >= t + 60'000; }),
                        drop_presses_.end());
    return static_cast<int>(drop_presses_.size());
}

int Hunter::drop_presses_for(int slot, std::uint32_t gen) const {
    for (const TargetPresses& t : drop_targets_)
        if (t.slot == slot && t.gen == gen) return t.n;
    return 0;
}

int Hunter::drop_settles_for(int slot, std::uint32_t gen) const {
    for (const TargetPresses& t : drop_targets_)
        if (t.slot == slot && t.gen == gen) return t.settles;
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
    drop_fix_ = WorldFix{};
    drop_fix_on_ = false;
    // Logged back in: where the character stands may be another map's.
    if (suspend) {
        trust_.lose("logged back in");
        drop_hold("logged back in");
    }
    const bool on = drop_wanted(cfg);
    if (!on && !press_.open) {
        // Switched off mid-drop: whatever was under way is over. Nothing new
        // is asked of the eyes.
        drop_hold("drop_down switched off");
        return;
    }

    // The keys a drop owes, sent again at every pass: still owed for
    // kDropOwedPasses, ↓ or Ctrl may be stuck down in Windows -- a walk onto a
    // gate with ↓ down changes the map -- and the hunter halts for GoBot's
    // window to stop the hunt.
    const bool settled = hands_.settle_owed ? hands_.settle_owed() : input_.settle_owed();
    if (settled) {
        owed_passes_ = 0;
    } else if (++owed_passes_ >= kDropOwedPasses && !drop_halt_) {
        latch_drops(DropOff::KeysStuck);
        drop_halt_ = true;
        drop_stop_.store(DropStop::KeysStuck);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.drop_keys_stuck = true;
        }
        GB_WARN("Hunter: keys owed after a drop (Down or Ctrl up not sent, %d passes) -- hunting stops", owed_passes_);
    }

    // Where the packets put the character, for the drop: asked here, once.
    drop_fix_on_ = eyes_.where && eyes_.where(drop_fix_);
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
    // came and whatever was judged of the press: the press's.
    if (press_.open && drop_fix_on_ && f.epoch != press_.epoch0) drop_gate(now);
    if (!on) return;

    // The map drops go by: from the entry, or named from standing points --
    // the character's own, and the monsters' where their frames put them.
    MapTrust::Look look;
    look.now_ms = now;
    look.capture = drop_fix_on_ && f.state != WorldFix::State::NoCapture && f.state != WorldFix::State::FirstReport;
    look.settled = f.settled();
    look.rx = round_px(f.rx);
    look.ry = round_px(f.ry);
    look.report_ms = f.report_ms;
    look.map_known = f.map_known;
    look.map = f.map;
    look.area = f.area;
    look.epoch = f.epoch;
    look.entry_x = round_px(f.entry_x);
    look.entry_y = round_px(f.entry_y);
    if (look.capture && eyes_.stands) look.mobs = eyes_.stands();
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
    // stood after a take-off.
    if (reported && !f.airborne && f.state == WorldFix::State::Known) {
        const int rx = round_px(f.rx), ry = round_px(f.ry);
        if (ry >= drop_.y0 + kDropLandPx) {
            end_fall(cfg, std::abs(ry - drop_.land_y) <= kDropFloorMatchPx ? DropVerdict::Worked : DropVerdict::OffMap,
                     now, rx, ry, nullptr);
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
    // Falling: held still -- no walk and no hop -- until it is judged, and
    // after a fall not judged for as long as it may still be in the air: a
    // walk would steer the fall.
    if (drop_.phase == DropPhase::Fall || drop_in_air(now)) {
        let_go_of_moves(cfg);
        return true;
    }
    // Off, or not a target below past the band and drop_min_px: walked to as
    // ever, with nothing asked.
    if (!drop_wanted(cfg) || dy <= std::max(cfg.down(), cfg.drop_min_px)) {
        if (drop_.phase == DropPhase::Settle) end_settle("the target is no longer below", true, now);
        return false;
    }
    if (drop_.phase == DropPhase::Settle && (target.slot != drop_.slot || target.gen != drop_.gen))
        end_settle("another target", false, now);
    const bool settling = drop_.phase == DropPhase::Settle;

    DropFacts facts = drop_facts(cfg, target, me, mobs, listed, dx, dy, now, settling);
    const DropCall call = drop_rule(cfg, facts);

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
    // change for kPressWatchMs whatever comes of it.
    drop_presses_.push_back(after);
    bool counted = false;
    for (TargetPresses& t : drop_targets_)
        if (t.slot == drop_.slot && t.gen == drop_.gen) {
            ++t.n;
            counted = true;
        }
    if (!counted) {
        if (drop_targets_.size() >= 64) drop_targets_.erase(drop_targets_.begin());
        drop_targets_.push_back(TargetPresses{ drop_.slot, drop_.gen, 1 });
    }
    const int for_it = drop_presses_for(drop_.slot, drop_.gen);
    ++minute_drops_;
    last_press_ms_ = after;
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
    drop_.stem = stem;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++status_.drops;
        status_.dropping = true;
    }

    char gate[48] = "no gate in its band";
    if (call.gate_px >= 0) std::snprintf(gate, sizeof gate, "nearest gate %d px", call.gate_px);
    GB_INFO("Hunter: drop (Ctrl+Down) toward %s: %d px %s, %d px down -- you at %d,%d on floor %d of %s "
            "(trusted: %s), landing on floor %d (%s), %s; nothing on your level in view; %s for it, %d this minute; "
            "Down held %u ms%s",
            target.name.empty() ? "the monster" : target.name.c_str(), std::abs(dx), dx < 0 ? "left" : "right", dy,
            call.x0, call.y0, call.stand_y, stem.c_str(), trust_text(trust_.how()), call.land_y,
            call.direct ? "the monster's" : "one between", gate, nth(for_it).c_str(), drop_presses_minute(after),
            p.held_ms, p.sent == InputManager::DropSent::Cut ? " -- cut short" : "");
    if (p.held_ms > kDropHeldMaxMs) {
        GB_WARN("Hunter: Down was held %u ms (over %u) -- drops OFF until the next BẮT ĐẦU", p.held_ms,
                kDropHeldMaxMs);
        latch_drops(DropOff::HeldLong);
    }
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
    // press, and no more are begun for it.
    TargetPresses* it = nullptr;
    for (TargetPresses& t : drop_targets_)
        if (t.slot == drop_.slot && t.gen == drop_.gen) it = &t;
    if (it == nullptr) {
        if (drop_targets_.size() >= 64) drop_targets_.erase(drop_targets_.begin());
        drop_targets_.push_back(TargetPresses{ drop_.slot, drop_.gen, 0 });
        it = &drop_targets_.back();
    }
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
    if (drop_.phase != DropPhase::Fall) return;
    drop_.phase = DropPhase::Idle;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.dropping = false;
        if (v == DropVerdict::Worked) ++status_.drops_worked;
    }
    const unsigned long long ms = now > drop_.down_ms ? now - drop_.down_ms : 0;
    const MapPoint at{ drop_.x0, drop_.y0 };
    const char* stem = drop_.stem.empty() ? "this map" : drop_.stem.c_str();
    bool learn = false, fail = false;
    switch (v) {
        case DropVerdict::Worked:
            drop_landed_ = true;
            // Down a floor: the steps back counted each way are another
            // floor's, and a hop toward the target would take it back up.
            retreat_[0] = retreat_[1] = Retreat{};
            last_drop_worked_ms_ = now;
            drop_fails_row_ = 0;
            ++minute_drop_worked_;
            GB_INFO("Hunter: drop worked: landed at %d,%d, %d px down, %llu ms after Down (%s)", rx, ry, ry - drop_.y0,
                    ms, drop_.took_off ? "took off" : "no take-off seen");
            break;
        case DropVerdict::OffMap:
            drop_landed_ = true;
            ++minute_drop_off_map_;
            if (book_ && !drop_.stem.empty()) book_->distrust(drop_.stem);
            latch_drops(DropOff::OffMap);
            GB_WARN("Hunter: drop landed at %d,%d, not on floor %d the map file gave -- drops OFF until the next "
                    "BẮT ĐẦU; %s's file no longer trusted",
                    rx, ry, drop_.land_y, stem);
            break;
        case DropVerdict::Rose:
            drop_landed_ = true;
            learn = true;
            ++minute_drop_rose_;
            latch_drops(DropOff::Rose);
            GB_INFO("Hunter: drop ROSE to %d,%d -- Ctrl+Down acts as a jump here; drops OFF until the next BẮT ĐẦU", rx,
                    ry);
            break;
        case DropVerdict::JumpInPlace:
            drop_landed_ = true;
            learn = fail = true;
            ++minute_drop_jump_;
            GB_INFO("Hunter: drop was a jump in place (landed at %d,%d) -- remembered as no drop; the monster passed "
                    "over for %u s",
                    rx, ry, 2 * cfg.give_up_ms / 1000);
            break;
        case DropVerdict::Nothing:
            learn = fail = true;
            ++minute_drop_nothing_;
            GB_INFO("Hunter: drop did nothing in %llu ms at %d,%d -- no drops within %d px of it this session; the "
                    "monster passed over for %u s (%d in a row)",
                    static_cast<unsigned long long>(kDropJudgeMs), drop_.x0, drop_.y0, GameMapBook::kNoDropXPx,
                    2 * cfg.give_up_ms / 1000, drop_fails_row_ + 1);
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
    if (learn && book_ && !drop_.stem.empty()) book_->learn_no_drop(drop_.stem, at);
    if (fail) {
        ++drop_fails_row_;
        ++drop_fails_run_;
        // The monster it was for, passed over a while -- not counted as given
        // up: the minute line counts it among the drops.
        if (have_target_ && target_slot_ == drop_.slot && target_gen_ == drop_.gen) {
            if (gave_up_.size() < 16)
                gave_up_.push_back(GaveUpSpot{ target_x_, target_y_, now + 2ull * cfg.give_up_ms, true });
            end_fight("the drop did not drop");
            have_target_ = false;
        }
        if (drop_fails_row_ >= kDropFailsInRow || drop_fails_run_ >= kDropFailsPerRun) {
            latch_drops(DropOff::Fails);
            GB_INFO("Hunter: %d presses of Ctrl+Down %s did not drop -- drops OFF until the next BẮT ĐẦU",
                    drop_fails_row_ >= kDropFailsInRow ? drop_fails_row_ : drop_fails_run_,
                    drop_fails_row_ >= kDropFailsInRow ? "in a row" : "this run");
        }
    }
    // What the press's record has to undo, should the map turn out changed.
    if (press_.open && press_.down_ms == drop_.down_ms) {
        press_.verdict = v;
        press_.no_drop = learn && book_ && !drop_.stem.empty();
        press_.failed = fail;
        press_.minute_from = minute_from_ms_;
    }
}

void Hunter::drop_gate(std::uint64_t now) {
    PressRecord& r = press_;
    r.open = false;
    const WorldFix& f = drop_fix_;
    // Whatever was judged of it before is undone: a no-drop point learned, a
    // fail counted, its count in this minute's line.
    if (r.no_drop && book_) book_->unlearn_no_drop(r.stem, { r.x0, r.y0 });
    if (r.failed) {
        if (drop_fails_row_ > 0) --drop_fails_row_;
        if (drop_fails_run_ > 0) --drop_fails_run_;
    }
    if (r.verdict != DropVerdict::None && r.minute_from == minute_from_ms_) {
        // By DropVerdict: None, Worked, Nothing, JumpInPlace, Rose, OffMap,
        // Gate, Unknown.
        int* const counts[] = { nullptr,           &minute_drop_worked_,  &minute_drop_nothing_, &minute_drop_jump_,
                                &minute_drop_rose_, &minute_drop_off_map_, nullptr,
                                &minute_drop_unknown_ };
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
    // entry put it, on the one it came to.
    const MapPoint to{ round_px(f.entry_x), round_px(f.entry_y) };
    if (book_) {
        if (!r.stem.empty()) book_->learn_gate(r.stem, { r.x0, r.y0 });
        if (f.map_known) book_->learn_gate(GameMapBook::stem_of(f.map, f.area), to);
    }
    latch_drops(DropOff::MapChanged);
    drop_halt_ = true;
    drop_stop_.store(DropStop::MapChanged);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.drop_map_changed = true;
    }
    char where[48] = "a map not known";
    if (f.map_known)
        std::snprintf(where, sizeof where, "map %u/%u", static_cast<unsigned>(f.map), static_cast<unsigned>(f.area));
    GB_WARN("Hunter: MAP CHANGED %.1f s after Ctrl+Down at %d,%d on %s -> %s at %d,%d -- hunting STOPS, Nhảy xuống "
            "to be unticked and saved",
            static_cast<double>(now > r.down_ms ? now - r.down_ms : 0) / 1000.0, r.x0, r.y0,
            r.stem.empty() ? "a map not trusted" : r.stem.c_str(), where, to.x, to.y);
}

void Hunter::latch_drops(DropOff why) {
    // A map change and stuck keys stop the hunt: they are said over any other.
    if (drop_latch_ != DropOff::None && why != DropOff::MapChanged && why != DropOff::KeysStuck) return;
    drop_latch_ = why;
    std::lock_guard<std::mutex> lock(mutex_);
    status_.drop_off = why;
}

void Hunter::drop_refused(const HunterConfig& cfg, const DropCall& call, const Nameplate& target, int dy,
                          std::uint64_t now) {
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
    char more[96] = "";
    switch (call.why) {
        case DropWhy::Gate:
            std::snprintf(more, sizeof more, " (%d px across; drop_gate_px is %d)", call.px, cfg.drop_gate_px);
            break;
        case DropWhy::PastMonster:
            std::snprintf(more, sizeof more, " (floor %d, %d px below it)", call.land_y, call.px);
            break;
        case DropWhy::MapUnknown:
            std::snprintf(more, sizeof more, " (%s)", MapTrust::why_text(trust_.report().why));
            break;
        default:
            break;
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
    if (minute_drops_ == 0 && minute_drop_worked_ == 0 && minute_drop_nothing_ == 0 && minute_drop_jump_ == 0 &&
        minute_drop_rose_ == 0 && minute_drop_off_map_ == 0 && minute_drop_gate_ == 0 && minute_drop_unknown_ == 0)
        return {};
    char text[200];
    std::snprintf(text, sizeof text,
                  "; drops %d (%d worked, %d did nothing, %d jumps in place, %d rose, %d off the map, %d map changes, "
                  "%d unknown)",
                  minute_drops_, minute_drop_worked_, minute_drop_nothing_, minute_drop_jump_, minute_drop_rose_,
                  minute_drop_off_map_, minute_drop_gate_, minute_drop_unknown_);
    return text;
}

} // namespace gobot
