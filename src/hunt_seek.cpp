#include "gobot/hunt_seek.hpp"
#include "gobot/hunter.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <utility>

namespace gobot {

// The inset is the look-ahead a walk toward a target is judged by: a walk to
// an attack point, cut short of it by kZoneWalkSlackPx, is never refused.
static_assert(kSeekEdgeInsetPx == Hunter::kZoneWalkSlackPx,
              "an attack point stands as far inside the box as walk_kept_in looks ahead");

namespace {

/// A floor of the box from one entry: its columns walked to, x order, and
/// whether each way reaches the floor's end (the trail not cut by the box).
struct Run {
    std::vector<MapPoint> pts;
    bool left_whole = true;
    bool right_whole = true;
};

/// The columns of `l`'s floor walked to from its start without coming further
/// out of the box than the start is -- walk_stays_in's rule, as jump_plan's
/// own_columns has it.
Run run_of(const HuntZone& z, const ZoneLegs& l) {
    Run r;
    const int now = zone_outside_px(z, l.left.pts.front().x, l.left.pts.front().y - 1);
    for (const MapPoint& q : l.left.pts) {
        if (zone_outside_px(z, q.x, q.y - 1) > now) {
            r.left_whole = false;
            break;
        }
        r.pts.push_back(q);
    }
    std::reverse(r.pts.begin(), r.pts.end());
    for (std::size_t i = 1; i < l.right.pts.size(); ++i) {
        const MapPoint& q = l.right.pts[i];
        if (zone_outside_px(z, q.x, q.y - 1) > now) {
            r.right_whole = false;
            break;
        }
        r.pts.push_back(q);
    }
    return r;
}

/// A floor's usable columns, worked out once per floor (by its two ends).
struct Usable {
    MapPoint l_end, r_end;
    int      first_x = 0;           ///< pts' first column: one floor from two entries is cut alike inside the box
    std::vector<MapPoint> pts;
    std::vector<char>     ok;
};

std::vector<char> usable_of(const GameMapBook::Page& p, const HuntZone& z, const std::vector<MapPoint>& pts) {
    std::vector<char> ok(pts.size(), 0);
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const MapPoint s{ pts[i].x, pts[i].y - 1 };
        if (zone_outside_px(z, s.x, s.y) != 0) continue;
        ok[i] = walk_stays_in(p, z, s, -1, kSeekEdgeInsetPx).ok && walk_stays_in(p, z, s, 1, kSeekEdgeInsetPx).ok;
    }
    return ok;
}

/// What is still to be gone on from: a standing point, at what cost, by
/// which legs.
struct Todo {
    int      cost = 0;
    MapPoint at;
    int      falls = 0;
    int      jumps = 0;
    std::vector<SeekLeg> legs;
};

SeekTier tier_of(const SeekPiece& s) {
    return s.jumps > 0 ? SeekTier::Above : s.falls > 0 ? SeekTier::Below : SeekTier::OwnFloor;
}

} // namespace

const char* seek_leg_letter(SeekLeg::Kind k) {
    switch (k) {
        case SeekLeg::Kind::Walk: return "W";
        case SeekLeg::Kind::Fall: return "F";
        case SeekLeg::Kind::Jump: return "J";
    }
    return "?";
}

const char* seek_no_name(SeekNo w) {
    switch (w) {
        case SeekNo::None:          return "none";
        case SeekNo::Outside:       return "outside the hunting area";
        case SeekNo::NoFloor:       return "on no floor of the map";
        case SeekNo::NoAttackPoint: return "no place inside the area to hit it from";
        case SeekNo::NeedsJumpOff:  return "only a jump up gets there, and jump-ups are off";
        case SeekNo::Passed:        return "passed over a while";
        case SeekNo::Others:        return "another player is hitting it";
        case SeekNo::JumpsUsed:     return "only a jump up gets there, and its jumps up are used up";
    }
    return "?";
}

const char* seek_tier_name(SeekTier t) {
    switch (t) {
        case SeekTier::OwnFloor: return "your floor";
        case SeekTier::Below:    return "below";
        case SeekTier::Above:    return "above";
    }
    return "?";
}

SeekMap seek_map(const GameMapBook::Page& p, const HuntZone& z, MapPoint from, const SeekRules& rules) {
    SeekMap m;
    m.from = from;
    m.jumps = rules.jumps != nullptr;
    std::vector<Todo> todo;
    todo.push_back(Todo{ 0, from, 0, 0, {} });
    std::vector<Usable> floors;
    // Each piece kept improves on those of its floor somewhere; a bound on
    // the work all the same, whatever a map holds.
    for (std::size_t popped = 0; !todo.empty() && popped < 4 * kSeekPiecesMax && m.pieces.size() < kSeekPiecesMax;
         ++popped) {
        // The cheapest first: a floor is had by its cheapest way before any other.
        const auto it = std::min_element(todo.begin(), todo.end(),
                                         [](const Todo& a, const Todo& b) { return a.cost < b.cost; });
        const Todo t = std::move(*it);
        todo.erase(it);
        ZoneLegs legs = zone_legs_from(p, z, t.at);
        if (legs.left.pts.empty() || legs.right.pts.empty()) continue;
        const Run run = run_of(z, legs);
        if (run.pts.empty()) continue;
        const MapPoint le = legs.left.pts.back(), re = legs.right.pts.back();
        Usable* u = nullptr;
        for (Usable& f : floors)
            if (f.l_end.x == le.x && f.l_end.y == le.y && f.r_end.x == re.x && f.r_end.y == re.y &&
                f.first_x == run.pts.front().x && f.pts.size() == run.pts.size())
                u = &f;
        if (u == nullptr) {
            floors.push_back(Usable{ le, re, run.pts.front().x, run.pts, usable_of(p, z, run.pts) });
            u = &floors.back();
        }
        // Kept only where it is cheaper than every way to the same floor so far.
        bool better = false;
        for (std::size_t i = 0; i < run.pts.size() && !better; ++i) {
            const int x = run.pts[i].x;
            const int c = t.cost + std::abs(x - t.at.x);
            bool beaten = false;
            for (const SeekPiece& s : m.pieces)
                if (s.floor_l == run.pts.front().x && s.floor_r == run.pts.back().x && s.pts.size() == run.pts.size() &&
                    s.cost_at(x) <= c)
                    beaten = true;
            better = !beaten;
        }
        if (!better) continue;

        SeekPiece s;
        s.floor_l = run.pts.front().x;
        s.floor_r = run.pts.back().x;
        s.pts = run.pts;
        s.usable = u->ok;
        for (std::size_t i = 0; i < s.pts.size(); ++i) {
            if (!s.usable[i]) continue;
            if (s.use_l < 0) s.use_l = s.pts[i].x;
            s.use_r = s.pts[i].x;
        }
        s.entry_x = t.at.x;
        s.entry_cost = t.cost;
        s.falls = t.falls;
        s.jumps = t.jumps;
        s.legs = t.legs;
        m.pieces.push_back(s);

        // Off its ends, onto a floor inside the box -- an end the walk gets
        // to, the box not cut before it.
        if (t.falls < rules.falls_max) {
            for (const ZoneLegs::Fall& f : legs.falls) {
                if (f.dir < 0 ? !run.left_whole : !run.right_whole) continue;
                Todo next;
                next.cost = t.cost + std::abs(f.end.x - t.at.x) + kSeekFallCostPx + std::abs(f.land.x - f.end.x);
                next.at = f.land;
                next.falls = t.falls + 1;
                next.jumps = t.jumps;
                next.legs = t.legs;
                SeekLeg leg;
                leg.kind = SeekLeg::Kind::Fall;
                leg.from = { f.end.x, f.end.y - 1 };
                leg.to = f.land;
                leg.walk_x = f.end.x + f.dir * kZoneWalkOffPastPx;
                next.legs.push_back(leg);
                todo.push_back(std::move(next));
            }
        }
        // Straight up off it, from a usable column -- a walk to the take-off
        // is never refused.
        if (rules.jumps != nullptr && t.jumps < rules.jumps_max) {
            for (const JumpPlan& j : jump_legs_from(p, z, t.at, *rules.jumps)) {
                const auto at = std::find_if(s.pts.begin(), s.pts.end(), [&](const MapPoint& q) { return q.x == j.x; });
                if (at == s.pts.end() || !s.usable[static_cast<std::size_t>(at - s.pts.begin())]) continue;
                Todo next;
                next.cost = t.cost + j.walk_px + kSeekJumpCostPx;
                next.at = j.land();
                next.falls = t.falls;
                next.jumps = t.jumps + 1;
                next.legs = t.legs;
                SeekLeg leg;
                leg.kind = SeekLeg::Kind::Jump;
                leg.from = j.take();
                leg.to = j.land();
                leg.walk_x = j.x;
                leg.jump = j;
                next.legs.push_back(leg);
                todo.push_back(std::move(next));
            }
        }
    }
    return m;
}

std::optional<SeekPlan> seek_attack(const SeekMap& m, const HuntZone& z, const SeekMob& mob, const SeekRules& rules,
                                    SeekNo* why, std::optional<SeekPlan>* would) {
    const auto none = [&](SeekNo w) -> std::optional<SeekPlan> {
        if (why != nullptr) *why = w;
        return std::nullopt;
    };
    if (why != nullptr) *why = SeekNo::None;
    if (would != nullptr) would->reset();
    if (!kSeekOutsideHit && !z.contains(mob.at.x, mob.at.y)) return none(SeekNo::Outside);
    if (mob.floor_y < 0) return none(SeekNo::NoFloor);
    if (mob.hurt_by_other) return none(SeekNo::Others);
    const int reach = rules.reach - rules.reach_margin;
    const int up = rules.up - rules.band_margin;
    const int down = rules.down - rules.band_margin;
    // Best first: the tier (kSeekTiered), then the cost, then the nearer the
    // monster across.
    using Rank = std::array<int, 3>;
    std::optional<SeekPlan> best, best_any;
    Rank rank{}, rank_any{};
    const auto in_reach = [&](MapPoint at) {
        const int dx = mob.at.x - at.x;
        const int dy = mob.at.y - at.y + rules.dy_px;
        return std::abs(dx) <= reach && dy >= -up && dy <= down;
    };
    for (std::size_t k = 0; k < m.pieces.size(); ++k) {
        const SeekPiece& s = m.pieces[k];
        const SeekTier tier = tier_of(s);
        const bool allowed = s.jumps == 0 || rules.jumps_go;
        for (std::size_t i = 0; i < s.pts.size(); ++i) {
            if (!s.usable[i]) continue;
            const MapPoint at{ s.pts[i].x, s.pts[i].y - 1 };
            const int dx = mob.at.x - at.x;
            if (!in_reach(at)) continue;
            // The turn to it walks the character toward it: in reach from
            // there too, on this floor -- one ending first, never.
            if (rules.turn_px > 0 && std::abs(dx) > rules.turn_px) {
                const int tx = at.x + (dx < 0 ? -rules.turn_px : rules.turn_px);
                const auto t = std::lower_bound(s.pts.begin(), s.pts.end(), tx,
                                                [](const MapPoint& q, int x) { return q.x < x; });
                if (t == s.pts.end() || std::abs(t->x - tx) > GameMapBook::kFollowHolePx ||
                    !in_reach({ t->x, t->y - 1 }))
                    continue;
            }
            const int cost = s.cost_at(at.x);
            const Rank r{ kSeekTiered ? static_cast<int>(tier) : 0, cost, std::abs(dx) };
            const auto plan = [&] {
                SeekPlan p;
                p.attack = at;
                p.tier = tier;
                p.cost = cost;
                p.piece = static_cast<int>(k);
                p.legs = s.legs;
                if (!s.legs.empty()) {
                    p.first = s.legs.front();
                } else {
                    p.first.kind = SeekLeg::Kind::Walk;
                    p.first.from = m.from;
                    p.first.to = at;
                    p.first.walk_x = at.x;
                }
                return p;
            };
            if (!best_any || r < rank_any) {
                best_any = plan();
                rank_any = r;
            }
            if (allowed && (!best || r < rank)) {
                best = plan();
                rank = r;
            }
        }
    }
    if (best) return best;
    if (best_any) {
        if (would != nullptr) *would = best_any;
        return none(SeekNo::NeedsJumpOff);
    }
    return none(SeekNo::NoAttackPoint);
}

} // namespace gobot
