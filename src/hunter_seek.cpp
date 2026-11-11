// The hunter's seek: "Tự tìm quái trong vùng", HunterConfig::seek_in_zone
// (2026.11.05b) -- the seek design's §4 and §6. With the map tab's area in
// use and the packets steering, the hunter goes to any monster inside the box
// it can get to, in view or not, and fights it from a standing point on a
// floor inside the box: the whole map's monsters (Eyes::world, placed on their
// floors), the box's floors from where the character stands and each
// monster's attack point (hunt_seek.hpp), then the walk there -- along the
// floor (WalkFor::Floor, never a hop), off a floor's end, or a jump up through
// the jump-up executor (JumpUpFor::Seek, jump_go). Nothing to go to inside the
// box: it stands where it is (the owner's Q1 (a), kSeekIdleHome).
//
// One in reach is fought first, always; then the monsters a walk on the
// character's own floor gets to, the drop's target below, those below by a
// fall, those above by a jump (the owner's Q2 (a), kSeekTiered) -- the one
// gone for kept unless another is clearly better (kSeekSwitchPx,
// kSeekCommitMs). One that gets no nearer (kSeekStuckMs), or is not there at
// its attack point (kSeekArriveMs), is passed over a while (kSeekPassOverMs).
//
// Never out of the box: the attack points stand kSeekEdgeInsetPx inside where
// a floor leaves it, so walk_kept_in, still asked, never refuses. No key of its
// own: the arrows through walk(), the jumps through the jump-up executor with
// its caps and latches; nothing here turns the character or writes which way
// it faces. Off, or not on this pass (no zone, the screen's plates, a list, the
// walk back first): the hunter of 2026.11.05 and its jumps up, exactly.

#include "gobot/hunter.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"
#include "gobot/mob_place.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace gobot {
namespace {

int round_px(double v) { return static_cast<int>(std::lround(v)); }

const char* way_of(int d) { return d < 0 ? "left" : "right"; }

/// How a plan gets to its attack point from `from`, for the log: "walk 40 px
/// left", "Ctrl+Up at 810 onto floor 274, walk right to 1104, off its end onto
/// floor 499, walk 101 px right".
std::string way_text(const SeekPlan& p, MapPoint from) {
    std::string s;
    char part[160];
    MapPoint at = from;
    for (const SeekLeg& l : p.legs) {
        if (l.kind == SeekLeg::Kind::Jump && l.jump) {
            std::snprintf(part, sizeof part, "%s%s at %d onto floor %d", s.empty() ? "" : ", ",
                          jump_kind_name(l.jump->kind), l.jump->x, l.jump->land_top);
        } else {
            const int end = l.from.x;
            std::snprintf(part, sizeof part, "%swalk %s to %d, off its end onto floor %d", s.empty() ? "" : ", ",
                          way_of(end - at.x), end, l.to.y + 1);
        }
        s += part;
        at = l.to;
    }
    const int last = p.attack.x - at.x;
    if (last != 0 || s.empty()) {
        if (last == 0)
            std::snprintf(part, sizeof part, "stand there");
        else
            std::snprintf(part, sizeof part, "%swalk %d px %s", s.empty() ? "" : ", ", std::abs(last), way_of(last));
        s += part;
    }
    return s;
}

const char* tier_word(SeekTier t) {
    switch (t) {
        case SeekTier::OwnFloor: return "your floor";
        case SeekTier::Below:    return "a fall";
        case SeekTier::Above:    return "a jump";
    }
    return "?";
}

/// The rank a pick goes by (kSeekTiered): your floor, the drop's target
/// below, a fall, a jump; cost alone otherwise -- the drop's target after.
int rank_of(SeekTier t, bool drop) {
    if (!kSeekTiered) return drop ? 1 : 0;
    if (drop) return 1;
    return t == SeekTier::OwnFloor ? 0 : t == SeekTier::Below ? 2 : 3;
}

} // namespace

const char* Hunter::seek_none_name(SeekNone w) {
    switch (w) {
        case SeekNone::None:     return "none";
        case SeekNone::Empty:    return "no monster inside the area";
        case SeekNone::AboveOff: return "only some a jump up gets to, and jump-ups are off";
        case SeekNone::NoWay:    return "none it gets to from where it stands";
        case SeekNone::Passed:   return "only some passed over a while, or another player's";
    }
    return "?";
}

// ---------------------------------------------------------------- where

bool Hunter::seek_wanted(const HunterConfig& cfg) const {
    return cfg.seek_in_zone && cfg.spot && cfg.packet_positions == PacketPositions::On && !cfg.stay_put &&
           spot_mode_ == SpotMode::Use && zone_.page != nullptr && fix_.state == WorldFix::State::Known;
}

SeekRules Hunter::seek_rules(const HunterConfig& cfg) const {
    const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
    SeekRules r;
    r.reach = round_px(cfg.reach * scale);
    r.up = round_px(cfg.up() * scale);
    r.down = round_px(cfg.down() * scale);
    // As the plates lay a monster's height against yours: packet_dy on it.
    r.dy_px = round_px(cfg.packet_dy * scale);
    return r;
}

const Hunter::SeekLook* Hunter::seek_look_of(int slot, std::uint32_t gen) const {
    for (const SeekLook& l : seek_looks_)
        if (l.mob.slot == slot && l.mob.gen == gen) return &l;
    return nullptr;
}

void Hunter::seek_skip(int slot, std::uint32_t gen, std::uint64_t until_ms, SeekNo why) {
    if (slot < 0) return;
    for (SeekSkip& s : seek_skip_)
        if (s.slot == slot && s.gen == gen) {
            s.until_ms = std::max(s.until_ms, until_ms);
            s.why = why;
            return;
        }
    if (seek_skip_.size() >= 64) seek_skip_.erase(seek_skip_.begin());
    seek_skip_.push_back(SeekSkip{ slot, gen, until_ms, why });
}

bool Hunter::seek_skipped(int slot, std::uint32_t gen, std::uint64_t now) const {
    for (const SeekSkip& s : seek_skip_)
        if (s.slot == slot && s.gen == gen && now < s.until_ms) return true;
    return false;
}

void Hunter::seek_let_go(const char* why) {
    if (seek_.on && why != nullptr) GB_DEBUG("Hunter: not seeking slot %d any more -- %s", seek_.slot, why);
    seek_ = SeekState{};
}

// ---------------------------------------------------------------- a pass

void Hunter::seek_look(const HunterConfig& cfg, std::uint64_t now) {
    seek_wanted_ = seek_wanted(cfg);
    seek_world_on_ = false;
    if (seek_wanted_ && eyes_.world) seek_world_on_ = eyes_.world(seek_world_);
    if (!seek_world_on_) seek_world_.clear();
    // The walk back first (the owner's Q2 of 04d): outside the box with a way
    // back, that comes before any monster -- and while it is latched
    // (2026.11.10), until the character is inside the box.
    seek_on_ = seek_wanted_ && seek_world_on_ && !zone_.back_first && !back_walk_;
    if (!seek_on_) {
        if (seek_.on) seek_let_go("the seek is not on");
        seek_looks_.clear();
        seek_publish();
        return;
    }
    ++minute_seek_passes_;
    const GameMapBook::Page& page = *zone_.page;

    // The floors, worked out again from where the character stands: when the
    // box, the keys or what the session learned changed; at once after a
    // landing on another floor; otherwise every kSeekPlanEveryMs it moved.
    // In the air, or past a floor's end, the last stand.
    if (zone_.stand) {
        const MapPoint stand = *zone_.stand;
        const bool jumps = jump_area(cfg);
        const bool go = jumps && cfg.jump_up && jump_latch_ == JumpUpOff::None;
        JumpRules up;
        if (jumps) up = jump_rules(cfg, zone_.page);
        const std::uint8_t kinds = jumps ? up.kinds : 0;
        const bool same_setup = seek_map_known_ && seek_map_page_ == zone_.page.get() &&
                                same_place(seek_map_box_, zone_.box) && seek_map_epoch_ == jump_epoch_ &&
                                seek_map_kinds_ == kinds && seek_map_jumps_ == jumps && seek_map_go_ == go;
        const bool moved = !seek_map_known_ || seek_map_from_.x != stand.x || seek_map_from_.y != stand.y;
        bool landed = false;
        if (seek_map_known_ && moved) {
            const std::optional<int> top = page.follow_to(seek_map_from_.x, seek_map_from_.y, stand.x);
            landed = !top || std::abs(*top - 1 - stand.y) > GameMapBook::kFollowLinkPx;
        }
        if (!same_setup || landed || (moved && now >= seek_planned_ms_ + kSeekPlanEveryMs)) {
            SeekRules r = seek_rules(cfg);
            r.jumps = jumps ? &up : nullptr;
            r.jumps_go = go;
            seek_map_ = seek_map(page, zone_.box, stand, r);
            seek_map_known_ = true;
            seek_map_from_ = stand;
            seek_map_box_ = zone_.box;
            seek_map_page_ = zone_.page.get();
            seek_map_epoch_ = jump_epoch_;
            seek_map_kinds_ = kinds;
            seek_map_jumps_ = jumps;
            seek_map_go_ = go;
            seek_planned_ms_ = now;
        }
    }
    // Each monster's attack point from those floors, every pass: they walk.
    // One whose jumps up are used up (kJumpUpsPerTarget presses, or
    // kJumpUpSettlesPerTarget settles let go of) is planned without them, as
    // jump_marks leaves it: a walk or a fall to it still goes, a jump never --
    // jump_go would walk to the take-off only for jump_at to refuse (TargetCap).
    seek_looks_.clear();
    if (seek_map_known_) {
        SeekRules r = seek_rules(cfg);
        r.jumps_go = seek_map_go_;
        SeekRules no_jumps = r;
        no_jumps.jumps_go = false;
        for (const SeekMob& m : seek_world_) {
            SeekLook l;
            l.mob = m;
            const bool used_up = seek_map_go_ && (jump_presses_for(m.slot, m.gen) >= kJumpUpsPerTarget ||
                                                  jump_settles_for(m.slot, m.gen) >= kJumpUpSettlesPerTarget);
            l.plan = seek_attack(seek_map_, zone_.box, m, used_up ? no_jumps : r, &l.why, &l.would);
            // Only a jump got there: no way to it -- not "jump-ups off", which
            // they are not.
            if (used_up && l.why == SeekNo::NeedsJumpOff) {
                l.why = SeekNo::JumpsUsed;
                l.would.reset();
            }
            if (l.why != SeekNo::Outside && seek_skipped(m.slot, m.gen, now)) {
                l.plan.reset();
                l.why = SeekNo::Passed;
            }
            seek_looks_.push_back(std::move(l));
        }
    }

    // Where every monster is, every kSeekSnapshotMs: what the next log needs
    // to say why the bot stood or went where it did.
    if (seek_snapshot_ms_ == 0 || now >= seek_snapshot_ms_ + kSeekSnapshotMs) {
        seek_snapshot_ms_ = now;
        const MapPoint you = zone_.stand ? *zone_.stand : MapPoint{ round_px(fix_.x), round_px(fix_.y) };
        std::vector<const SeekLook*> inside, near;
        int elsewhere = 0;
        for (const SeekLook& l : seek_looks_) {
            const int out = zone_outside_px(zone_.box, l.mob.at.x, l.mob.at.y);
            if (out == 0) inside.push_back(&l);
            else if (out <= 200) near.push_back(&l);
            else ++elsewhere;
        }
        const auto nearer = [&](const SeekLook* a, const SeekLook* b) {
            return std::abs(a->mob.at.x - you.x) + std::abs(a->mob.at.y - you.y) <
                   std::abs(b->mob.at.x - you.x) + std::abs(b->mob.at.y - you.y);
        };
        std::sort(inside.begin(), inside.end(), nearer);
        std::sort(near.begin(), near.end(), nearer);
        std::string text;
        char part[200];
        for (std::size_t i = 0; i < inside.size() && i < kSeekSnapshotMax; ++i) {
            const SeekLook& l = *inside[i];
            std::string tag;
            const bool sought = seek_.on && seek_.slot == l.mob.slot && seek_.gen == l.mob.gen;
            if (l.plan) {
                const int d = l.plan->attack.x - you.x;
                if (sought)
                    std::snprintf(part, sizeof part, " [SOUGHT %s %d %s]", seek_leg_letter(l.plan->first.kind),
                                  std::abs(d), d < 0 ? "L" : "R");
                else
                    std::snprintf(part, sizeof part, " [reach from %d,%d]", l.plan->attack.x, l.plan->attack.y);
                tag = part;
            } else {
                std::snprintf(part, sizeof part, " [%s]", seek_no_name(l.why));
                tag = part;
            }
            char placed[48] = "";
            if (l.mob.why == static_cast<std::uint8_t>(DotWhy::Projected))
                std::snprintf(placed, sizeof placed, " (projected %.1f s)",
                              static_cast<double>(l.mob.frame_age_ms) / 1000.0);
            std::snprintf(part, sizeof part, "%ss%d#%u %d,%d floor %d%s%s", text.empty() ? "" : "; ", l.mob.slot,
                          l.mob.gen, l.mob.at.x, l.mob.at.y, l.mob.floor_y, placed, tag.c_str());
            text += part;
        }
        if (inside.size() > kSeekSnapshotMax) {
            std::snprintf(part, sizeof part, "; %zu more inside", inside.size() - kSeekSnapshotMax);
            text += part;
        }
        if (!near.empty()) {
            text += text.empty() ? "outside within 200 px:" : "; outside within 200 px:";
            for (std::size_t i = 0; i < near.size() && i < 6; ++i) {
                const SeekLook& l = *near[i];
                std::snprintf(part, sizeof part, "%s s%d %d,%d (%d px out)", i == 0 ? "" : ",", l.mob.slot,
                              l.mob.at.x, l.mob.at.y, zone_outside_px(zone_.box, l.mob.at.x, l.mob.at.y));
                text += part;
            }
        }
        std::snprintf(part, sizeof part, "%selsewhere %d", text.empty() ? "" : "; ", elsewhere);
        text += part;
        GB_INFO("Hunter: monsters at %d,%d (floor %d, %s the area): %s", you.x, you.y, you.y + 1,
                zone_.outside_px > 0 ? "outside" : "inside", text.c_str());
    }
    seek_publish();
}

void Hunter::mark_no_way(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                         std::vector<Listed>& listed, std::uint64_t now) {
    if (!seek_on_ || !seek_map_known_) return;
    const bool drops = drops_may_go(cfg);
    for (std::size_t i = 0; i < mobs.size() && i < listed.size(); ++i) {
        const Nameplate& n = mobs[i];
        if (n.slot < 0 || (listed[i] != Listed::Free && listed[i] != Listed::First)) continue;
        // Within reach it is fought where it stands; a monster below a drop
        // could go to is the drop's to judge.
        if (in_reach(cfg, n.x() - me.x(), n.y() - me.y())) continue;
        if (drops && n.y() - me.y() > std::max(cfg.down(), cfg.drop_min_px)) continue;
        const SeekLook* l = seek_look_of(n.slot, n.gen);
        if (l == nullptr) continue;
        if (l->why == SeekNo::Passed) {
            listed[i] = Listed::GaveUp;
            continue;
        }
        if (l->plan) continue;
        listed[i] = Listed::NoWay;
        // Said once a monster at INFO: why there is no way to it.
        const std::pair<int, std::uint32_t> who{ n.slot, n.gen };
        const bool said = std::find(seek_said_no_way_.begin(), seek_said_no_way_.end(), who) != seek_said_no_way_.end();
        if (!said) {
            if (seek_said_no_way_.size() >= 64) seek_said_no_way_.erase(seek_said_no_way_.begin());
            seek_said_no_way_.push_back(who);
            ++minute_seek_no_way_;
        }
        char more[200] = "";
        const SeekRules r = seek_rules(cfg);
        const char* jumps = !seek_map_jumps_ ? "no jumps up here" : seek_map_go_ ? "jump-ups on" : "jump-ups off";
        if (l->why == SeekNo::NoAttackPoint && !seek_map_.pieces.empty()) {
            const SeekPiece& own = seek_map_.pieces.front();
            std::snprintf(more, sizeof more, ": your floor (x %d..%d) is never within %d px up and %d across; %s",
                          own.use_l, own.use_r, r.up - r.band_margin, r.reach - r.reach_margin, jumps);
        } else if (l->why == SeekNo::NeedsJumpOff && l->would && l->would->first.jump) {
            std::snprintf(more, sizeof more, " (%s at %d onto floor %d would get there)",
                          jump_kind_name(l->would->first.jump->kind), l->would->first.jump->x,
                          l->would->first.jump->land_top);
        }
        Logger::instance().logf(said ? LogLevel::Debug : LogLevel::Info,
                                "Hunter: not going for slot %d gen %u at %d,%d (floor %d) -- %s%s", n.slot, n.gen,
                                l->mob.at.x, l->mob.at.y, l->mob.floor_y, seek_no_name(l->why), more);
    }
    (void)now;
}

Hunter::SeekChoice Hunter::seek_choose(const HunterConfig& cfg, const std::vector<Nameplate>& mobs,
                                       const Nameplate& me, const std::vector<Listed>& listed, std::uint64_t now,
                                       int kept, bool* wounded) {
    SeekChoice out;
    const auto reach_at = [&](int i) {
        const Nameplate& n = mobs[static_cast<std::size_t>(i)];
        return in_reach(cfg, n.x() - me.x(), n.y() - me.y());
    };
    const auto plate_of = [&](int slot, std::uint32_t gen) {
        for (std::size_t i = 0; i < mobs.size(); ++i)
            if (mobs[i].slot == slot && mobs[i].gen == gen) return static_cast<int>(i);
        return -1;
    };
    // The goal set to `slot`, `gen`: said when it is another than the one gone
    // for -- a switch, or a fresh one.
    const auto take = [&](int slot, std::uint32_t gen, const SeekLook* l, bool drop, int plate, const char* why) {
        const bool same = seek_.on && seek_.slot == slot && seek_.gen == gen;
        if (!same) {
            const bool sought = l != nullptr && l->plan && !drop && why == nullptr;
            const SeekState was = seek_;
            const bool switched = was.on;
            // Said at INFO at most every 2 s, a fresh goal of the seek's own;
            // what is in reach and fought is no goal to tell of.
            const LogLevel level = sought && (seek_said_goal_ms_ == 0 || now >= seek_said_goal_ms_ + 2000)
                                       ? LogLevel::Info
                                       : LogLevel::Debug;
            if (level == LogLevel::Info) seek_said_goal_ms_ = now;
            if (sought) {
                const SeekPlan& p = *l->plan;
                int in_area = 0, reachable = 0, above_off = 0, passed = 0;
                for (const SeekLook& o : seek_looks_) {
                    if (o.why == SeekNo::Outside) continue;
                    ++in_area;
                    if (o.plan) ++reachable;
                    if (o.why == SeekNo::NeedsJumpOff) ++above_off;
                    if (o.why == SeekNo::Passed) ++passed;
                }
                char was_text[160] = "";
                if (switched) {
                    const SeekLook* old = seek_look_of(was.slot, was.gen);
                    if (old != nullptr && old->plan && old->plan->cost >= p.cost + kSeekSwitchPx &&
                        old->plan->tier == p.tier)
                        std::snprintf(was_text, sizeof was_text, " instead of slot %d -- %d px cheaper (%s both)",
                                      was.slot, old->plan->cost - p.cost, seek_tier_name(p.tier));
                    else if (old != nullptr && old->plan)
                        std::snprintf(was_text, sizeof was_text, " instead of slot %d -- %s, it %s", was.slot,
                                      tier_word(p.tier), tier_word(old->plan->tier));
                    else
                        std::snprintf(was_text, sizeof was_text, " instead of slot %d", was.slot);
                }
                Logger::instance().logf(
                    level,
                    "Hunter: seeking slot %d gen %u at %d,%d (floor %d, %s%s)%s -- attack from %d,%d: %s (%s; cost "
                    "%d); in the area %d: reachable %d, above with no way up %d%s, passed over %d",
                    slot, gen, l->mob.at.x, l->mob.at.y, l->mob.floor_y, plate >= 0 ? "in view" : "out of view",
                    l->mob.why == static_cast<std::uint8_t>(DotWhy::Projected) ? ", projected" : "", was_text,
                    p.attack.x, p.attack.y, way_text(p, seek_map_.from).c_str(), tier_word(p.tier), p.cost, in_area,
                    reachable, above_off, above_off > 0 ? " (jump-ups off)" : "", passed);
                ++minute_seek_goals_[static_cast<std::size_t>(p.tier)];
            } else {
                GB_DEBUG("Hunter: going for slot %d gen %u -- %s", slot, gen,
                         drop ? "the drop's target below" : why != nullptr ? why : "picked");
            }
            if (switched) ++minute_seek_switched_;
            // A pick is a switch for the commit's clock: two monsters a like
            // way off never pass the bot back and forth from its first pick.
            seek_ = SeekState{};
            seek_.on = true;
            seek_.slot = slot;
            seek_.gen = gen;
            seek_.since_ms = now;
            seek_.switch_ms = now;
        }
        seek_.by_drop = drop;
        seek_.in_view = plate >= 0;
        seek_.planned = l != nullptr && l->plan.has_value();
        if (seek_.planned) seek_.plan = *l->plan;
        if (l != nullptr) {
            seek_.mob = l->mob.at;
            seek_.mob_why = l->mob.why;
            seek_.mob_age_ms = l->mob.frame_age_ms;
        }
        if (!same) seek_.mark_cost = seek_.planned ? seek_.plan.cost : 0;
    };

    // 1. The target kept, in reach: fought on -- got to, when it was walked to.
    if (kept >= 0 && reach_at(kept)) {
        const Nameplate& n = mobs[static_cast<std::size_t>(kept)];
        const bool walked = seek_.on && seek_.slot == n.slot && seek_.gen == n.gen && seek_.planned;
        if (walked && !seek_.reached) {
            seek_.reached = true;
            ++minute_seek_reached_;
        }
        if (!walked) take(n.slot, n.gen, seek_look_of(n.slot, n.gen), false, kept, "in reach");
        out.index = kept;
        return out;
    }
    // 2. One in reach now: before any walk, whatever is gone for.
    if (!mobs.empty()) {
        int i = choose_target(cfg, mobs, &me, listed, wounded, false, nullptr);
        if (i < 0 || !reach_at(i)) {
            if (wounded != nullptr) *wounded = false;
            i = choose_in_reach(cfg, mobs, me, listed);
        }
        if (i >= 0 && reach_at(i)) {
            const Nameplate& n = mobs[static_cast<std::size_t>(i)];
            take(n.slot, n.gen, seek_look_of(n.slot, n.gen), false, i, "in reach now");
            seek_.reached = true;
            out.index = i;
            return out;
        }
    }

    // 3. The box's monsters the character gets to, in view or not; and the
    // drop's target below (choose_target's below_first), when a drop may go.
    struct Cand {
        int             slot = -1;
        std::uint32_t   gen = 0;
        int             rank = 0;
        int             cost = 0;
        int             plate = -1;
        const SeekLook* look = nullptr;
        bool            drop = false;
    };
    std::vector<Cand> cands;
    for (const SeekLook& l : seek_looks_) {
        if (!l.plan || l.why != SeekNo::None) continue;
        const int p = plate_of(l.mob.slot, l.mob.gen);
        if (p >= 0 && (refused(listed[static_cast<std::size_t>(p)]) || mobs[static_cast<std::size_t>(p)].hp <= cfg.dead_below))
            continue;
        if (p < 0 && l.mob.hp <= cfg.dead_below) continue;
        cands.push_back(Cand{ l.mob.slot, l.mob.gen, rank_of(l.plan->tier, false), l.plan->cost, p, &l, false });
    }
    if (drops_may_go(cfg)) {
        // The drop's target kept, as 05's kept target was: the one gone for --
        // or the hunter's target -- while it is still below; else the nearest
        // below (choose_target's below_first). Plates come nearest first, and
        // two walking a like way off trade places from look to look. One whose
        // drop was refused from right above it a moment ago, for good, goes by
        // its own way instead (seek_no_drop_).
        int first = -1, chosen = -1;
        for (std::size_t i = 0; i < mobs.size(); ++i) {
            const Nameplate& n = mobs[i];
            if (n.slot < 0 || n.hp <= cfg.dead_below || refused(listed[i]) ||
                n.y() - me.y() <= std::max(cfg.down(), cfg.drop_min_px))
                continue;
            bool no_drop = false;
            for (const SeekSkip& s : seek_no_drop_)
                if (s.slot == n.slot && s.gen == n.gen && now < s.until_ms) no_drop = true;
            const SeekLook* own = seek_look_of(n.slot, n.gen);
            if (no_drop && own != nullptr && own->plan) continue;
            if (first < 0) first = static_cast<int>(i);
            if (static_cast<int>(i) == kept || (seek_.on && n.slot == seek_.slot && n.gen == seek_.gen)) {
                chosen = static_cast<int>(i);
                break;
            }
        }
        if (chosen < 0) chosen = first;
        if (chosen >= 0) {
            const Nameplate& n = mobs[static_cast<std::size_t>(chosen)];
            cands.push_back(Cand{ n.slot, n.gen, rank_of(SeekTier::Below, true), 0, chosen,
                                  seek_look_of(n.slot, n.gen), true });
        }
    }
    const auto better = [](const Cand& a, const Cand& b) { return a.rank != b.rank ? a.rank < b.rank : a.cost < b.cost; };
    const Cand* best = nullptr;
    const Cand* cur = nullptr;
    for (const Cand& c : cands) {
        if (best == nullptr || better(c, *best)) best = &c;
        if (seek_.on && c.slot == seek_.slot && c.gen == seek_.gen && (cur == nullptr || c.drop == seek_.by_drop))
            cur = &c;
    }
    // Kept unless another is in a better tier, or kSeekSwitchPx cheaper with
    // kSeekCommitMs since the last switch -- a walk turned round needs that
    // too: two monsters either side never pass the bot back and forth.
    const Cand* pick = best;
    if (cur != nullptr && best != nullptr && cur != best) {
        const bool tier_up = best->rank < cur->rank;
        const bool cheaper = best->cost + kSeekSwitchPx <= cur->cost && now >= seek_.switch_ms + kSeekCommitMs;
        if (!tier_up && !cheaper) pick = cur;
    }
    if (pick == nullptr) {
        if (seek_.on) seek_let_go("nothing inside the area to go to");
        return out;
    }
    take(pick->slot, pick->gen, pick->look, pick->drop, pick->plate, nullptr);
    out.index = pick->plate;
    out.goal = pick->plate < 0;
    return out;
}

bool Hunter::seek_holds_clock(const Nameplate& target, std::uint64_t now) const {
    // Only while seek_go keeps its clock: a pass that no longer reaches it --
    // a drop's settle, a stop above a target below -- lets the give-up run.
    return seek_on_ && seek_.on && !seek_.by_drop && seek_.planned && target.slot == seek_.slot &&
           target.gen == seek_.gen && seek_.quiet_ms < kSeekStuckMs && seek_.quiet_pass != 0 &&
           now <= seek_.quiet_seen_ms + 500;
}

bool Hunter::seek_walks_away(const Nameplate& target) {
    if (!seek_on_ || !seek_.on || seek_.by_drop || !seek_.away || seek_.arrived_ms != 0) return false;
    if (target.slot != seek_.slot || target.gen != seek_.gen || !zone_.stand) return false;
    // By this pass's look, not the last walk's: one whose way is gone --
    // another player's (Q6), out of the box, no attack point any more -- or
    // is no longer a walk is fought where it is in reach; seek_go would not
    // walk to the old aim, and the walk toward it would only go round it.
    const SeekLook* l = seek_look_of(seek_.slot, seek_.gen);
    if (l == nullptr || !l->plan || l->plan->first.kind != SeekLeg::Kind::Walk) {
        seek_.away = false;
        return false;
    }
    const int to = l->plan->attack.x - zone_.stand->x;
    const bool past = seek_.dir != 0 && to != 0 && (to < 0 ? -1 : 1) != seek_.dir;
    // Got there -- or past it: fought from here on, the turn and all.
    if (std::abs(to) <= kSeekAimPx || past) {
        seek_.away = false;
        return false;
    }
    return true;
}

bool Hunter::seek_go(const HunterConfig& cfg, const Nameplate* target, std::uint64_t now) {
    // In a jump's flight: held still -- no key -- until it lands (an arrow
    // would steer it).
    if (jump_.phase == JumpUpPhase::Air || jump_in_air(now)) {
        let_go_of_moves(cfg);
        return true;
    }
    if (!seek_on_ || !seek_.on || seek_.by_drop) return false;
    const SeekLook* look = seek_look_of(seek_.slot, seek_.gen);
    if (look == nullptr || !look->plan) {
        seek_.away = false;
        return false;
    }
    const SeekPlan& plan = *look->plan;
    seek_.plan = plan;
    seek_.planned = true;
    seek_.mob = look->mob.at;
    // Past its floor's end -- falling -- or in the air: as it is, until it
    // lands and the floors are worked out again from there.
    if (!zone_.stand) {
        let_go_of_moves(cfg);
        return true;
    }
    // A hold -- a trash pass, a chat line, the charms, a revive, the shop --
    // or the game behind with require_focus off: walk() gets no token, or the
    // input refuses the arrow, and the character stands. Not counted toward
    // Stuck or NotFound: nothing pressed, the progress clock left as it was
    // and the next pass not one in a row. Seen meanwhile, so the target's
    // give-up stays held (seek_holds_clock); NotFound's wait starts again
    // after it.
    if (coord_.critical_active() || !focused()) {
        let_go_of_moves(cfg);
        seek_.arrived_ms = 0;
        seek_.quiet_seen_ms = now;
        return true;
    }
    const MapPoint stand = *zone_.stand;
    const double scale = cfg.packet_scale > 0.0f ? static_cast<double>(cfg.packet_scale) : 1.0;
    const auto screen = [scale](int world, int dir) {
        const int s = static_cast<int>(std::lround(world / scale));
        return s != 0 ? s : dir;
    };
    // Passed over, said, let go of: Stuck, NotFound, a way refused.
    const auto pass_over = [&](const char* what, std::uint64_t ms, bool info) {
        seek_skip(seek_.slot, seek_.gen, now + ms, SeekNo::Passed);
        Logger::instance().logf(info ? LogLevel::Info : LogLevel::Debug, "Hunter: slot %d %s; passed over for %llu s",
                                seek_.slot, what, static_cast<unsigned long long>(ms / 1000));
        let_go_of_moves(cfg);
        if (have_target_ && target_slot_ == seek_.slot && target_gen_ == seek_.gen) {
            end_fight("not got to");
            watch_ = DamageWatch{};
            have_target_ = false;
        }
        seek_let_go(nullptr);
        return true;
    };

    // Getting there: the cost to its attack point kSeekProgressPx less within
    // kSeekStuckMs of walking -- passes in a row only; a jump's settle, its
    // wait at the take-off and its flight, a hold, the game behind, not
    // counted.
    const SeekLeg& first = plan.first;
    const bool jumping = (jump_.phase == JumpUpPhase::Settle && jump_.purpose == JumpUpFor::Seek) ||
                         (first.kind == SeekLeg::Kind::Jump && first.jump &&
                          std::abs(stand.x - first.jump->x) <= kJumpUpLeadPx);
    if (seek_.quiet_pass == 0 || plan.cost + kSeekProgressPx <= seek_.mark_cost) {
        seek_.mark_cost = plan.cost;
        seek_.quiet_ms = 0;
    } else if (!jumping && !coord_.critical_active() && focused() && seek_.quiet_pass + 1 == passes_ &&
               now > seek_.quiet_seen_ms) {
        seek_.quiet_ms += std::min<std::uint64_t>(now - seek_.quiet_seen_ms, 250);
    }
    seek_.quiet_pass = passes_;
    seek_.quiet_seen_ms = now;
    if (seek_.quiet_ms >= kSeekStuckMs) {
        ++minute_seek_stuck_;
        char what[160];
        std::snprintf(what, sizeof what, "at %d,%d got no %d px nearer its attack point %d,%d in %.1f s of walking "
                      "(cost %d)",
                      seek_.mob.x, seek_.mob.y, kSeekProgressPx, plan.attack.x, plan.attack.y,
                      static_cast<double>(seek_.quiet_ms) / 1000.0, plan.cost);
        return pass_over(what, kSeekPassOverMs, true);
    }

    const SeekLeg& leg = first;
    switch (leg.kind) {
        case SeekLeg::Kind::Walk: {
            // In reach by the map from a standing point (the full band).
            const SeekRules r = seek_rules(cfg);
            const auto reach_from = [&](MapPoint s) {
                const int mdx = look->mob.at.x - s.x;
                const int mdy = look->mob.at.y - s.y + r.dy_px;
                return std::abs(mdx) <= r.reach && mdy >= -r.up && mdy <= r.down;
            };
            // Walked to from the monster's side -- away from it, up a slope
            // into its band: not stopped for it in reach at the band's edge
            // (seek_walks_away) -- the turn to it would walk the character
            // back out -- but at the attack point, which has it in reach after
            // the turn too (SeekRules::turn_px).
            const int toward = look->mob.at.x < plan.attack.x ? -1 : 1;
            if (!seek_.away && seek_.arrived_ms == 0 && (stand.x - plan.attack.x) * toward > kSeekArrivePx)
                seek_.away = true;
            seek_.aim_x = plan.attack.x;
            const int to = plan.attack.x - stand.x;
            const int dir = to < 0 ? -1 : 1;
            // There: at the point, or past it; walked toward the monster, in
            // reach by the map within kSeekArrivePx -- on a slope 20 px short
            // is 10 px of height. Stood for its plate to come in reach then:
            // NotFound when it does not in kSeekArriveMs.
            if (seek_.arrived_ms != 0 && std::abs(to) > 2 * kSeekArrivePx) seek_.arrived_ms = 0;
            const bool past = seek_.dir != 0 && to != 0 && dir != seek_.dir;
            const bool there = std::abs(to) <= kSeekAimPx || seek_.arrived_ms != 0 ||
                               (std::abs(to) <= kSeekArrivePx && (past || (!seek_.away && reach_from(stand))));
            if (there) {
                seek_.away = false;
                let_go_of_moves(cfg);
                if (seek_.arrived_ms == 0) seek_.arrived_ms = now;
                if (now < seek_.arrived_ms + kSeekArriveMs) return true;
                ++minute_seek_not_found_;
                char what[200];
                if (target != nullptr) {
                    std::snprintf(what, sizeof what, "not found at %d,%d -- in reach on the map from %d,%d but not by "
                                  "its plate for %.1f s",
                                  seek_.mob.x, seek_.mob.y, stand.x, stand.y,
                                  static_cast<double>(kSeekArriveMs) / 1000.0);
                } else {
                    std::snprintf(what, sizeof what, "not found at %d,%d -- in reach on the map from %d,%d, no plate "
                                  "of it in view for %.1f s",
                                  seek_.mob.x, seek_.mob.y, stand.x, stand.y,
                                  static_cast<double>(kSeekArriveMs) / 1000.0);
                }
                return pass_over(what, kSeekPassOverMs, true);
            }
            seek_.arrived_ms = 0;
            // Q1's own look along the floor, as every walk toward a target:
            // the attack point inset, it never refuses.
            if (target != nullptr) {
                if (walk_kept_in(cfg, *target, screen(to, dir), 0, now, std::abs(to))) {
                    seek_let_go(nullptr);
                    return true;
                }
            } else if (zone_.page) {
                const ZoneWalk w = walk_stays_in(*zone_.page, zone_.box, stand, dir,
                                                 std::clamp(std::abs(to) + kZoneWalkSlackPx, 1, kZoneWalkAheadPx));
                if (!w.ok) {
                    char what[160];
                    std::snprintf(what, sizeof what, "-- the walk to it %s at %d,%d", zone_walk_name(w.why), w.at.x,
                                  w.at.y);
                    return pass_over(what, 2ull * cfg.give_up_ms, true);
                }
            }
            seek_.dir = dir;
            walk(cfg, screen(to, dir), 0, now, WalkFor::Floor);
            return true;
        }
        case SeekLeg::Kind::Fall: {
            // To its end and past it: the fall lands inside the box at any
            // speed (zone_falls); the floors worked out again from the landing.
            const int to = leg.walk_x - stand.x;
            const int dir = leg.walk_x < leg.from.x ? -1 : 1;
            seek_.arrived_ms = 0;
            seek_.dir = dir;
            walk(cfg, screen(to != 0 ? to : dir, dir), 0, now, WalkFor::Floor);
            return true;
        }
        case SeekLeg::Kind::Jump: {
            if (!leg.jump) return false;
            seek_.arrived_ms = 0;
            // Through the jump-up executor, its caps and latches the
            // monster's (JumpUpFor::Seek): walked to the take-off, settled,
            // pressed. A lasting refusal: passed over as a give-up.
            jump_.target_w = look->mob.at;
            const std::string name = "slot " + std::to_string(seek_.slot);
            if (jump_go(cfg, JumpUpFor::Seek, *leg.jump, seek_.slot, seek_.gen, name, now)) return true;
            return pass_over("-- no jump up to it for now", 2ull * cfg.give_up_ms, false);
        }
    }
    return false;
}

Hunter::SeekIdle Hunter::seek_idle(const HunterConfig& cfg, const SeekChoice& c, std::uint64_t now) {
    // In a flight or a fall: held still until it lands.
    if (jump_.phase == JumpUpPhase::Air || jump_in_air(now) || drop_in_air(now)) {
        let_go_of_moves(cfg);
        return SeekIdle::Going;
    }
    if (c.goal && seek_go(cfg, nullptr, now)) return SeekIdle::Going;
    // Nothing to go to: outside the box, the walk back as before (Q2 of
    // 04d); inside it, stood for where it is -- the owner's Q1 (a). With
    // kSeekIdleHome, back to the home as 05.
    if (kSeekIdleHome || zone_.outside_px > 0) return SeekIdle::NotMine;
    let_go_of_moves(cfg);
    int alive = 0, in_area = 0, reachable = 0, no_way_up = 0, passed = 0, others = 0, near = 0;
    std::string up_text, passed_text;
    char part[96];
    const SeekPlan* would = nullptr;
    const SeekMob* would_mob = nullptr;
    for (const SeekLook& l : seek_looks_) {
        ++alive;
        const int out = zone_outside_px(zone_.box, l.mob.at.x, l.mob.at.y);
        if (out > 0) {
            if (out <= 200) ++near;
            continue;
        }
        ++in_area;
        if (l.plan) ++reachable;
        if (l.why == SeekNo::NeedsJumpOff) {
            if (l.would && (would == nullptr || l.would->cost < would->cost)) {
                would = &*l.would;
                would_mob = &l.mob;
            }
            if (no_way_up++ < 4) {
                std::snprintf(part, sizeof part, "%ss%d %d,%d", up_text.empty() ? "" : "; ", l.mob.slot, l.mob.at.x,
                              l.mob.at.y);
                up_text += part;
            }
        }
        if (l.why == SeekNo::Passed) {
            std::uint64_t left = 0;
            for (const SeekSkip& s : seek_skip_)
                if (s.slot == l.mob.slot && s.gen == l.mob.gen && s.until_ms > now) left = s.until_ms - now;
            if (passed++ < 4) {
                std::snprintf(part, sizeof part, "%ss%d, %llu s left", passed_text.empty() ? "" : "; ", l.mob.slot,
                              static_cast<unsigned long long>((left + 999) / 1000));
                passed_text += part;
            }
        }
        if (l.why == SeekNo::Others) ++others;
    }
    // 05b's dry run (the owner's answer 5): with jump-ups off, the jump the
    // seek would have gone for -- jumps its last tier, so only standing here
    // with nothing else to go to; the cheapest, as seek_publish's arrow.
    if (would != nullptr && would_mob != nullptr && !(cfg.jump_up && jump_latch_ == JumpUpOff::None)) {
        for (const SeekLeg& leg : would->legs)
            if (leg.jump) {
                jump_would(*leg.jump, JumpUpFor::Seek, would_mob->slot, would_mob->gen,
                           "slot " + std::to_string(would_mob->slot), now);
                break;
            }
    }
    const SeekNone why = in_area == 0          ? SeekNone::Empty
                         : no_way_up > 0       ? SeekNone::AboveOff
                         : passed + others > 0 ? SeekNone::Passed
                                               : SeekNone::NoWay;
    // Said as it begins, then every kSeekSaidMs while it lasts.
    const bool begins = seek_none_since_ms_ == 0;
    if (begins) seek_none_since_ms_ = now;
    seek_none_ = true;
    seek_none_why_ = why;
    if (begins || now >= seek_none_said_ms_ + kSeekSaidMs) {
        seek_none_said_ms_ = now;
        const MapPoint at = zone_.stand ? *zone_.stand : MapPoint{ round_px(fix_.x), round_px(fix_.y) };
        GB_INFO("Hunter: nothing to seek for %.1f s at %d,%d (floor %d, inside the area) -- %d alive on the map, %d "
                "inside the area: reachable %d, no way up %d%s%s%s, passed over %d%s%s%s, hurt by others %d; outside "
                "within 200 px %d -- standing where it is",
                static_cast<double>(now - seek_none_since_ms_) / 1000.0, at.x, at.y, at.y + 1, alive, in_area,
                reachable, no_way_up, up_text.empty() ? "" : " (", up_text.c_str(), up_text.empty() ? "" : ")", passed,
                passed_text.empty() ? "" : " (", passed_text.c_str(), passed_text.empty() ? "" : ")", others, near);
    }
    seek_publish();
    return SeekIdle::Standing;
}

// ---------------------------------------------------------------- the log

void Hunter::seek_time(std::uint64_t now) {
    if (pass_at_ms_ != 0 && now > pass_at_ms_) {
        State state = State::Idle;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state = status_.state;
        }
        PassKind k = PassKind::Waiting;
        if (held_before_)                          k = PassKind::Held;
        else if (state == State::Unfocused)        k = PassKind::Behind;
        else if (state == State::NoPlayer)         k = PassKind::NoPlayer;
        else if (seek_wanted_ && !seek_world_on_)  k = PassKind::Screen;
        else if (state == State::Attacking)        k = PassKind::Attacking;
        else if (state == State::Approaching)      k = seek_.on && !seek_.in_view ? PassKind::Seeking
                                                                                  : PassKind::Approaching;
        else if (state == State::Returning)        k = PassKind::WalkingBack;
        else if (seek_none_)                       k = PassKind::Nothing;
        const std::uint64_t spent = std::min<std::uint64_t>(now - pass_at_ms_, 250);
        minute_time_ms_[static_cast<std::size_t>(k)] += spent;
        if (k == PassKind::Nothing) minute_seek_none_ms_ += spent;
    }
    pass_at_ms_ = now;
    // A pass that stood with nothing to seek ends its run unless this one does too.
    if (!seek_none_) seek_none_since_ms_ = 0;
    seek_none_ = false;
}

void Hunter::seek_publish() {
    const bool on = seek_on_;
    const std::uint64_t now = clock_now();
    int in_area = 0, above = 0, passed = 0;
    std::vector<std::uint32_t> grey;
    std::vector<Status::SeekGrey> marks;
    // The cheapest monster only a jump up gets to, with jump-ups off: where
    // that jump would go, for the map tab's arrow.
    const SeekPlan* would = nullptr;
    for (const SeekLook& l : seek_looks_) {
        if (l.why == SeekNo::Outside) continue;
        ++in_area;
        if (l.why == SeekNo::NeedsJumpOff) {
            ++above;
            if (l.would && (would == nullptr || l.would->cost < would->cost)) would = &*l.would;
        }
        if (l.why == SeekNo::Passed) ++passed;
        if (!l.plan && l.mob.slot >= 0) {
            grey.push_back(static_cast<std::uint32_t>(l.mob.slot));
            Status::SeekGrey g;
            g.slot = static_cast<std::uint32_t>(l.mob.slot);
            g.gen = l.mob.gen;
            g.why = l.why;
            for (const SeekSkip& k : seek_skip_)
                if (k.slot == l.mob.slot && k.gen == l.mob.gen && k.until_ms > now) g.left_ms = k.until_ms - now;
            marks.push_back(g);
        }
    }
    const JumpPlan* would_jump = nullptr;
    if (on && would != nullptr)
        for (const SeekLeg& leg : would->legs)
            if (leg.jump) {
                would_jump = &*leg.jump;
                break;
            }
    const SeekLeg* const jump_leg = seek_.planned && seek_.plan.first.jump ? &seek_.plan.first : nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    Status& s = status_;
    if (would_jump != nullptr) {
        s.jump_would_plan = *would_jump;
        s.jump_would_ms = now;
    }
    s.seek_planned = on && seek_.on && seek_.planned;
    s.seek_you_x = seek_map_known_ ? seek_map_from_.x : 0;
    s.seek_you_y = seek_map_known_ ? seek_map_from_.y : 0;
    s.seek_jump_kind = jump_leg != nullptr ? jump_leg->jump->kind : JumpKind::Up;
    s.seek_jump_land_top = jump_leg != nullptr ? jump_leg->jump->land_top : 0;
    s.seek_grey = on ? std::move(marks) : std::vector<Status::SeekGrey>{};
    s.seek_active = on;
    s.seek_on = on && seek_.on;
    s.seek_slot = s.seek_on ? seek_.slot : -1;
    s.seek_gen = s.seek_on ? seek_.gen : 0;
    s.seek_mob_x = seek_.mob.x;
    s.seek_mob_y = seek_.mob.y;
    s.seek_in_view = seek_.in_view;
    s.seek_projected = seek_.mob_why == static_cast<std::uint8_t>(DotWhy::Projected);
    s.seek_at_x = seek_.planned ? seek_.plan.attack.x : 0;
    s.seek_at_y = seek_.planned ? seek_.plan.attack.y : 0;
    s.seek_tier = seek_.planned ? seek_.plan.tier : SeekTier::OwnFloor;
    s.seek_leg = seek_.planned ? seek_leg_letter(seek_.plan.first.kind)[0] : 'W';
    s.seek_leg_x = seek_.planned ? seek_.plan.first.walk_x : 0;
    s.seek_none = on && seek_none_;
    s.seek_none_why = s.seek_none ? seek_none_why_ : SeekNone::None;
    s.seek_in_area = on ? in_area : 0;
    s.seek_above_no_way = on ? above : 0;
    s.seek_passed = on ? passed : 0;
    s.seek_grey_slots = on ? std::move(grey) : std::vector<std::uint32_t>{};
}

std::string Hunter::seek_minute_text() const {
    if (minute_seek_passes_ == 0) return {};
    const auto s = [this](PassKind k) {
        return static_cast<double>(minute_time_ms_[static_cast<std::size_t>(k)]) / 1000.0;
    };
    char text[640];
    std::snprintf(text, sizeof text,
                  "; seek: goals %d (your floor %d, below %d, above %d), reached %d, switched %d, not found %d, stuck "
                  "%d, no way to %d; nothing to seek %.1f s; time: attacking %.1f, approaching %.1f, seeking %.1f, "
                  "nothing to seek %.1f, walking back %.1f, waiting %.1f, held %.1f, game behind %.1f, no player "
                  "%.1f, screen's plates %.1f s",
                  minute_seek_goals_[0] + minute_seek_goals_[1] + minute_seek_goals_[2], minute_seek_goals_[0],
                  minute_seek_goals_[1], minute_seek_goals_[2], minute_seek_reached_, minute_seek_switched_,
                  minute_seek_not_found_, minute_seek_stuck_, minute_seek_no_way_,
                  static_cast<double>(minute_seek_none_ms_) / 1000.0, s(PassKind::Attacking), s(PassKind::Approaching),
                  s(PassKind::Seeking), s(PassKind::Nothing), s(PassKind::WalkingBack), s(PassKind::Waiting),
                  s(PassKind::Held), s(PassKind::Behind), s(PassKind::NoPlayer), s(PassKind::Screen));
    return text;
}

void Hunter::seek_minute_reset() {
    for (int& n : minute_seek_goals_) n = 0;
    minute_seek_reached_ = minute_seek_switched_ = minute_seek_not_found_ = minute_seek_stuck_ = 0;
    minute_seek_no_way_ = minute_seek_passes_ = 0;
    minute_seek_none_ms_ = 0;
    for (std::uint64_t& t : minute_time_ms_) t = 0;
}

} // namespace gobot
