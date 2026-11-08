#include "gobot/jump_plan.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/hunter.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace gobot {
namespace {

int index_of(JumpKind k) { return static_cast<int>(k); }

/// One key's peak at one gravity, lowest and highest: ↑↑'s second push
/// anywhere in its window -- a peak is lowest at an end of it, highest where
/// the first push tops out, or the end nearest that.
std::pair<double, double> peaks_at(JumpKind k, double g) {
    if (k == JumpKind::Up) {
        const double h = kJumpUpV * kJumpUpV / (2.0 * g);
        return { h, h };
    }
    if (k == JumpKind::CtrlUp) {
        const double h = kJumpUpVHigh * kJumpUpVHigh / (2.0 * g);
        return { h, h };
    }
    const double early = (kJumpUpSecondAtMs - kJumpUpSecondEarlyMs) / 1000.0;
    const double late = (kJumpUpSecondAtMs + kJumpUpSecondLateMs) / 1000.0;
    const auto at = [&](double t2) { return kJumpUpV * t2 - g * t2 * t2 / 2.0 + kJumpUpV * kJumpUpV / (2.0 * g); };
    const double top = std::clamp(kJumpUpV / g, early, late);
    return { std::min(at(early), at(late)), at(top) };
}

std::array<JumpPeak, kJumpKinds> make_peaks() {
    std::array<JumpPeak, kJumpKinds> out{};
    for (int k = 0; k < kJumpKinds; ++k) {
        double lo = 1e9, hi = 0.0;
        for (const double g : kJumpUpGravity) {
            const std::pair<double, double> p = peaks_at(static_cast<JumpKind>(k), g);
            lo = std::min(lo, p.first);
            hi = std::max(hi, p.second);
        }
        out[static_cast<std::size_t>(k)] = { static_cast<int>(std::floor(lo)), static_cast<int>(std::ceil(hi)) };
    }
    return out;
}

/// The landing in column x for a take-off floor whose top is `top`: the top of
/// the highest floor more than kJumpUpFeetPx above it and no more than
/// `usable`; -1 none.
int landing_in(const GameMapBook::Page& p, int x, int top, int usable) {
    int best = -1;
    for (const MapFloor& f : p.floors_at(x)) {
        const int rise = top - f.y;
        if (rise <= kJumpUpFeetPx || rise > usable) continue;
        if (best < 0 || f.y < best) best = f.y;
    }
    return best;
}

/// The same, across a hole of the landing's floor as Page::follow walks over
/// one -- t52_s2's spawn ledge has none at x 711: a column with no landing
/// whose nearest either way with one are kFollowHolePx apart at most, their
/// floors linked, lands on the left one's. `at`: the column it is found in.
int landing_over_hole(const GameMapBook::Page& p, int x, int top, int usable, int& at) {
    at = x;
    const int here = landing_in(p, x, top, usable);
    if (here >= 0) return here;
    int l = -1, lx = x, r = -1, rx = x;
    for (int d = 1; d <= GameMapBook::kFollowHolePx && l < 0; ++d) l = landing_in(p, lx = x - d, top, usable);
    for (int d = 1; d <= GameMapBook::kFollowHolePx && r < 0; ++d) r = landing_in(p, rx = x + d, top, usable);
    if (l < 0 || r < 0 || rx - lx - 1 > GameMapBook::kFollowHolePx || std::abs(l - r) > GameMapBook::kFollowLinkPx)
        return -1;
    at = lx;
    return l;
}

/// The first floor that refused a column one way, by the order they are said in.
struct Bad {
    bool hit = false;
    int  x = -1;
    int  top = -1;

    void note(int at_x, int at_top) {
        if (hit) return;
        hit = true;
        x = at_x;
        top = at_top;
    }
};

/// A column of the character's own floor: where a walk along it stands.
struct Col {
    int x = 0;
    int top = 0;
};

/// The character's floor from `stand`, in x order, kJumpUpSearchPx and
/// kJumpUpRunInsetPx more either way at most (Page::follow, holes and all),
/// cut before the first standing point further outside the box than the
/// stand's own -- walk_stays_in's rule, so a walk to any of them is one it
/// lets go. `now`: how far outside the stand is. Empty off every floor.
std::vector<Col> own_columns(const GameMapBook::Page& p, const HuntZone& z, MapPoint stand, int& now) {
    std::vector<Col> out;
    const int reach = kJumpUpSearchPx + kJumpUpRunInsetPx;
    const MapTrail left = p.follow(stand.x, stand.y, -1, reach);
    const MapTrail right = p.follow(stand.x, stand.y, 1, reach);
    if (left.pts.empty() || right.pts.empty()) return out;
    now = zone_outside_px(z, left.pts.front().x, left.pts.front().y - 1);
    for (const MapPoint& q : left.pts) {
        if (zone_outside_px(z, q.x, q.y - 1) > now) break;
        out.push_back({ q.x, q.y });
    }
    std::reverse(out.begin(), out.end());
    for (std::size_t i = 1; i < right.pts.size(); ++i) {
        const MapPoint& q = right.pts[i];
        if (zone_outside_px(z, q.x, q.y - 1) > now) break;
        out.push_back({ q.x, q.y });
    }
    return out;
}

/// A run of passing columns, by index into the columns: first and last.
struct Run {
    std::size_t a = 0;
    std::size_t b = 0;
};

/// The columns judged for one key, and their runs: neighbours that both
/// pass, their landing floors linked as Page::follow links them.
std::vector<Run> runs_of(const GameMapBook::Page& p, const std::vector<Col>& cols, const std::vector<JumpColumn>& js) {
    std::vector<Run> out;
    for (std::size_t i = 0; i < cols.size(); ++i) {
        if (!js[i].ok()) continue;
        if (!out.empty() && out.back().b + 1 == i) {
            const std::optional<int> to = p.follow_to(js[i - 1].land_x, js[i - 1].land_top - 1, js[i].land_x);
            if (to && std::abs(*to - js[i].land_top) <= GameMapBook::kFollowLinkPx) {
                out.back().b = i;
                continue;
            }
        }
        out.push_back({ i, i });
    }
    return out;
}

int run_width(const std::vector<Col>& cols, const Run& r) { return cols[r.b].x - cols[r.a].x + 1; }

/// The plan for a take-off at column i of run r.
JumpPlan plan_at(JumpKind kind, const std::vector<Col>& cols, const std::vector<JumpColumn>& js, const Run& r,
                 std::size_t i, MapPoint stand, const JumpRules& rules) {
    JumpPlan j;
    j.kind = kind;
    j.x = cols[i].x;
    j.feet_y = cols[i].top - 1;
    j.land_x = js[i].land_x;
    j.land_top = js[i].land_top;
    j.rise = js[i].rise;
    j.headroom = js[i].headroom;
    j.run_l = cols[r.a].x;
    j.run_r = cols[r.b].x;
    j.walk_px = std::abs(j.x - stand.x);
    j.score = index_of(kind) * kJumpUpKindCostPx + j.walk_px +
              rules.headroom_weight * std::max(0, kJumpUpHeadroomWantPx - j.headroom);
    return j;
}

/// Column i of run r is one to walk to: kJumpUpRunInsetPx inside the run's
/// ends, kJumpUpSearchPx across at most.
bool take_off_at(const std::vector<Col>& cols, const Run& r, std::size_t i, MapPoint stand) {
    const int x = cols[i].x;
    return x >= cols[r.a].x + kJumpUpRunInsetPx && x <= cols[r.b].x - kJumpUpRunInsetPx &&
           std::abs(x - stand.x) <= kJumpUpSearchPx;
}

} // namespace

const char* jump_kind_name(JumpKind k) {
    switch (k) {
        case JumpKind::Up:     return "Up";
        case JumpKind::UpUp:   return "Up,Up";
        case JumpKind::CtrlUp: return "Ctrl+Up";
    }
    return "?";
}

const char* jump_kind_key(JumpKind k) {
    switch (k) {
        case JumpKind::Up:     return "↑";
        case JumpKind::UpUp:   return "↑↑";
        case JumpKind::CtrlUp: return "Ctrl+↑";
    }
    return "?";
}

JumpPeak jump_peak(JumpKind k) {
    static const std::array<JumpPeak, kJumpKinds> peaks = make_peaks();
    return peaks[static_cast<std::size_t>(index_of(k))];
}

const char* jump_no_name(JumpNo w) {
    switch (w) {
        case JumpNo::None:           return "none";
        case JumpNo::OffFloor:       return "not standing on a floor of the map";
        case JumpNo::Blocked:        return "a floor not jumped through in the way up";
        case JumpNo::Short:          return "no floor reached for sure";
        case JumpNo::Ambiguous:      return "a floor in the key's uncertain band";
        case JumpNo::Ceiling:        return "a floor outside the area within 650 px above";
        case JumpNo::Edge:           return "the landing floor ends or changes within 24 px";
        case JumpNo::Outside:        return "outside the area";
        case JumpNo::Weaker:         return "a lost key's plain Up would land outside the area";
        case JumpNo::Learned:        return "a jump failed there before this session";
        case JumpNo::Solid:          return "its floor is solid from below this session";
        case JumpNo::Gate:           return "by a gate";
        case JumpNo::Narrow:         return "no place wide enough to take off from";
        case JumpNo::Far:            return "out of reach where it lands, and the walk to it leaves the area";
        case JumpNo::NotTargetFloor: return "lands on another floor";
    }
    return "?";
}

JumpColumn judge_column(const GameMapBook::Page& p, const HuntZone& z, MapPoint take, JumpKind kind,
                        const JumpRules& rules, int take_out_px) {
    JumpColumn c;
    const std::optional<MapFloor> own = p.floor_near(take.x, take.y);
    if (!own || own->wall()) {
        c.why = JumpNo::OffFloor;
        return c;
    }
    const int top = own->y;
    c.take_top = top;
    if (zone_outside_px(z, take.x, top - 1) > std::max(0, take_out_px)) {
        c.why = JumpNo::Outside;
        return c;
    }
    const JumpPeak pk = jump_peak(kind);
    Bad blocked, ceiling, ambiguous, short_of, edge;
    // The landing at the column before, for the link: its column and top.
    int prev_x = -1, prev_land = -1;
    for (int dx = -kJumpUpMarginPx; dx <= kJumpUpMarginPx; dx += kJumpUpMarginStepPx) {
        const int cx = take.x + dx;
        for (const MapFloor& f : p.floors_at(cx)) {
            const int rise = top - f.y;
            if (rise <= kJumpUpFeetPx) continue;
            if (!f.platform() && rise <= pk.hi + kJumpUpHeadPx) blocked.note(cx, f.y);
            if (rules.ceiling_px > 0 && rise <= rules.ceiling_px && !z.contains(cx, f.y - 1)) ceiling.note(cx, f.y);
            if (rise > pk.usable() && rise <= pk.beyond()) ambiguous.note(cx, f.y);
        }
        // Its landing, across a hole as a walk goes over one.
        int lx = cx;
        const int land = landing_over_hole(p, cx, top, pk.usable(), lx);
        if (land < 0) {
            short_of.note(cx, -1);
        } else if (prev_land >= 0) {
            const std::optional<int> to = p.follow_to(prev_x, prev_land - 1, lx);
            if (!to || std::abs(*to - land) > GameMapBook::kFollowLinkPx) edge.note(lx, land);
        }
        prev_x = lx;
        prev_land = land;
        if (dx == 0 && land >= 0) {
            c.land_x = lx;
            c.land_top = land;
            c.rise = top - land;
            c.headroom = pk.lo - c.rise;
        }
    }
    const auto refuse = [&c](JumpNo w, const Bad* b) {
        c.why = w;
        if (b != nullptr) {
            c.bad_x = b->x;
            c.bad_top = b->top;
        }
        return c;
    };
    if (blocked.hit) return refuse(JumpNo::Blocked, &blocked);
    if (ceiling.hit) return refuse(JumpNo::Ceiling, &ceiling);
    if (ambiguous.hit) return refuse(JumpNo::Ambiguous, &ambiguous);
    if (short_of.hit) return refuse(JumpNo::Short, &short_of);
    if (edge.hit) return refuse(JumpNo::Edge, &edge);
    if (!z.contains(take.x, c.land_top - 1) || !z.contains(c.land_x, c.land_top - 1))
        return refuse(JumpNo::Outside, nullptr);

    // The jump a lost key leaves -- the second ↑ not sent, Ctrl not read --
    // is a plain ↑: back onto the take-off floor, or onto any floor it may
    // come down on, in its uncertain band too, inside the box; unblocked.
    if (kind != JumpKind::Up) {
        const JumpPeak weak = jump_peak(JumpKind::Up);
        Bad weaker;
        for (int dx = -kJumpUpMarginPx; dx <= kJumpUpMarginPx; dx += kJumpUpMarginStepPx) {
            const int cx = take.x + dx;
            for (const MapFloor& f : p.floors_at(cx)) {
                const int rise = top - f.y;
                if (rise <= kJumpUpFeetPx) continue;
                if (!f.platform() && rise <= weak.hi + kJumpUpHeadPx) weaker.note(cx, f.y);
                if (rise <= weak.beyond() && !z.contains(cx, f.y - 1)) weaker.note(cx, f.y);
            }
        }
        if (weaker.hit) return refuse(JumpNo::Weaker, &weaker);
    }

    // The judges' band round a gate, for Ctrl + ↑ alone: only when given one
    // (kJumpUpGateBand) -- the owner's Q2 has none.
    if (kind == JumpKind::CtrlUp && rules.gate_px > 0) {
        for (const MapGate& g : rules.gates) {
            if (std::abs(g.x - take.x) > rules.gate_px || std::abs(g.y - (top - 1)) > Hunter::kDropGateSpanPx) continue;
            Bad at;
            at.note(g.x, g.y);
            return refuse(JumpNo::Gate, &at);
        }
    }
    if (rules.refused) {
        const JumpNo w = rules.refused({ take.x, top - 1 }, c.land_top, kind);
        if (w != JumpNo::None) return refuse(w, nullptr);
    }
    return c;
}

std::optional<JumpPlan> plan_jump_to(const GameMapBook::Page& p, const HuntZone& z, MapPoint stand, MapPoint target,
                                     const JumpRules& rules, JumpNo* why_not, int* why_px) {
    const auto none = [&](JumpNo w, int px) -> std::optional<JumpPlan> {
        if (why_not != nullptr) *why_not = w;
        if (why_px != nullptr) *why_px = px;
        return std::nullopt;
    };
    int now = 0;
    const std::vector<Col> cols = own_columns(p, z, stand, now);
    if (cols.empty()) return none(JumpNo::OffFloor, 0);
    const int target_top = target.y + 1;
    // Its floor's rise above the character's at the column nearest it, and
    // the least anywhere along the character's floor.
    const Col& near = *std::min_element(cols.begin(), cols.end(), [&](const Col& a, const Col& b) {
        return std::abs(a.x - target.x) < std::abs(b.x - target.x);
    });
    const int rise_near = near.top - target_top;
    if (!z.contains(target.x, target.y)) return none(JumpNo::Outside, rise_near);

    // The monster's floor either way of it. Inside the box, a landing within
    // walk_l..walk_r walks to it without leaving the box. Over the character's
    // columns (in x order), its top there: a sloped one may be in reach at its
    // near end only.
    const int lo_x = cols.front().x, hi_x = cols.back().x;
    std::vector<int> its_top(static_cast<std::size_t>(std::max(0, hi_x - lo_x) + 1), target_top);
    int walk_l = target.x, walk_r = target.x;
    for (const int dir : { -1, 1 }) {
        bool in = true;
        const MapTrail t = p.follow(target.x, target.y, dir);
        for (const MapPoint& q : t.pts) {
            if (q.x >= lo_x && q.x <= hi_x) its_top[static_cast<std::size_t>(q.x - lo_x)] = q.y;
            in = in && z.contains(q.x, q.y - 1);
            if (in) (dir < 0 ? walk_l : walk_r) = q.x;
        }
    }
    // Its least rise anywhere along the character's floor, where it lies over
    // it; elsewhere its rise at its own column, as before.
    int least = INT_MAX, usable = INT_MIN;
    for (const Col& c : cols) {
        const int top = c.x >= lo_x && c.x <= hi_x ? its_top[static_cast<std::size_t>(c.x - lo_x)] : target_top;
        least = std::min(least, c.top - top);
    }
    for (int k = 0; k < kJumpKinds; ++k) {
        const JumpKind kind = static_cast<JumpKind>(k);
        if (rules.allows(kind)) usable = std::max(usable, jump_peak(kind).usable());
    }
    if (least > usable) return none(JumpNo::Short, rise_near);

    std::optional<JumpPlan> best;
    bool onto_its_floor = false, far = false;
    for (int k = 0; k < kJumpKinds; ++k) {
        const JumpKind kind = static_cast<JumpKind>(k);
        if (!rules.allows(kind)) continue;
        std::vector<JumpColumn> js;
        js.reserve(cols.size());
        for (const Col& c : cols) js.push_back(judge_column(p, z, { c.x, c.top - 1 }, kind, rules, 0));
        for (const Run& r : runs_of(p, cols, js)) {
            // Its landing floor followed to the monster's column: the same floor.
            const std::optional<int> there = p.follow_to(js[r.a].land_x, js[r.a].land_top - 1, target.x);
            if (!there || std::abs(*there - target_top) > Hunter::kDropSameFloorPx) continue;
            onto_its_floor = true;
            if (run_width(cols, r) < kJumpUpRunMinPx) continue;
            for (std::size_t i = r.a; i <= r.b; ++i) {
                if (!take_off_at(cols, r, i, stand)) continue;
                const int x = cols[i].x;
                if (std::abs(target.x - x) > rules.reach_px && (x < walk_l || x > walk_r)) {
                    far = true;
                    continue;
                }
                const JumpPlan j = plan_at(kind, cols, js, r, i, stand, rules);
                if (!best || j.score < best->score) best = j;
            }
        }
    }
    if (best) return best;
    if (onto_its_floor) return none(far ? JumpNo::Far : JumpNo::Narrow, rise_near);
    // Why not, where it would be nearest: the column under it, for the
    // cheapest key that would reach that high -- or the strongest.
    JumpKind fit = JumpKind::Up;
    for (int k = 0; k < kJumpKinds; ++k) {
        const JumpKind kind = static_cast<JumpKind>(k);
        if (!rules.allows(kind)) continue;
        fit = kind;
        if (jump_peak(kind).usable() >= rise_near) break;
    }
    const JumpColumn c = judge_column(p, z, { near.x, near.top - 1 }, fit, rules, 0);
    return none(c.ok() ? JumpNo::NotTargetFloor : c.why, rise_near);
}

std::vector<JumpPlan> jump_legs_from(const GameMapBook::Page& p, const HuntZone& z, MapPoint stand,
                                     const JumpRules& rules) {
    int now = 0;
    const std::vector<Col> cols = own_columns(p, z, stand, now);
    // The cheapest per landing floor, known by its ends.
    struct Best {
        MapPoint l_end, r_end;
        JumpPlan plan;
    };
    std::vector<Best> best;
    if (cols.empty()) return {};
    for (int k = 0; k < kJumpKinds; ++k) {
        const JumpKind kind = static_cast<JumpKind>(k);
        if (!rules.allows(kind)) continue;
        std::vector<JumpColumn> js;
        js.reserve(cols.size());
        for (const Col& c : cols) js.push_back(judge_column(p, z, { c.x, c.top - 1 }, kind, rules, now));
        for (const Run& r : runs_of(p, cols, js)) {
            if (run_width(cols, r) < kJumpUpRunMinPx) continue;
            std::optional<JumpPlan> mine;
            for (std::size_t i = r.a; i <= r.b; ++i) {
                if (!take_off_at(cols, r, i, stand)) continue;
                const JumpPlan j = plan_at(kind, cols, js, r, i, stand, rules);
                if (!mine || j.score < mine->score) mine = j;
            }
            if (!mine) continue;
            const MapTrail l = p.follow(js[r.a].land_x, js[r.a].land_top - 1, -1);
            const MapTrail rt = p.follow(js[r.a].land_x, js[r.a].land_top - 1, 1);
            if (l.pts.empty() || rt.pts.empty()) continue;
            const MapPoint le = l.pts.back(), re = rt.pts.back();
            const auto same = [&](const Best& b) {
                return b.l_end.x == le.x && b.l_end.y == le.y && b.r_end.x == re.x && b.r_end.y == re.y;
            };
            const auto it = std::find_if(best.begin(), best.end(), same);
            if (it == best.end()) {
                best.push_back({ le, re, *mine });
            } else if (mine->score < it->plan.score) {
                it->plan = *mine;
            }
        }
    }
    std::vector<JumpPlan> out;
    for (const Best& b : best) out.push_back(b.plan);
    std::stable_sort(out.begin(), out.end(), [](const JumpPlan& a, const JumpPlan& b) { return a.score < b.score; });
    return out;
}

std::vector<JumpRun> jump_runs(const GameMapBook::Page& p, const HuntZone& z, const JumpRules& rules) {
    std::vector<JumpRun> out;
    const int x0 = std::max(0, z.left), x1 = std::min(p.floors.w_px() - 1, z.right);
    for (int k = 0; k < kJumpKinds; ++k) {
        const JumpKind kind = static_cast<JumpKind>(k);
        if (!rules.allows(kind)) continue;
        const std::size_t first = out.size();
        // Column by column, so each run's last point is the one before.
        for (int x = x0; x <= x1; ++x) {
            for (const MapFloor& f : p.floors_at(x)) {
                if (f.wall() || !z.contains(x, f.y - 1)) continue;
                const JumpColumn c = judge_column(p, z, { x, f.y - 1 }, kind, rules, 0);
                if (!c.ok()) continue;
                JumpRun* joined = nullptr;
                for (std::size_t i = first; i < out.size() && joined == nullptr; ++i) {
                    JumpRun& r = out[i];
                    const int d = x - r.x1;
                    if (d > 0 && d <= GameMapBook::kFollowHolePx + 1 &&
                        std::abs(f.y - r.take_y1) <= GameMapBook::kFollowLinkPx * d &&
                        std::abs(c.land_top - r.land_y1) <= GameMapBook::kFollowLinkPx * d)
                        joined = &r;
                }
                if (joined == nullptr) {
                    JumpRun r;
                    r.kind = kind;
                    r.x0 = r.x1 = x;
                    r.take_y0 = r.take_y1 = f.y;
                    r.land_y0 = r.land_y1 = c.land_top;
                    r.head_lo = r.head_hi = c.headroom;
                    out.push_back(r);
                    continue;
                }
                joined->x1 = x;
                joined->take_y1 = f.y;
                joined->land_y1 = c.land_top;
                joined->head_lo = std::min(joined->head_lo, c.headroom);
                joined->head_hi = std::max(joined->head_hi, c.headroom);
            }
        }
    }
    return out;
}

std::optional<double> gravity_from(double y0, double vy0, double y1, double t_s) {
    if (!(t_s > 1e-6)) return std::nullopt;
    return 2.0 * ((y1 - y0) - vy0 * t_s) / (t_s * t_s);
}

std::optional<int> floor_above(const GameMapBook::Page& p, MapPoint stand, int within_px) {
    const std::optional<MapFloor> own = p.floor_near(stand.x, stand.y);
    if (!own) return std::nullopt;
    std::optional<int> best;
    for (int x = stand.x - kJumpUpMarginPx; x <= stand.x + kJumpUpMarginPx; ++x) {
        // Above the character's own floor there, so a slope's rise is not a
        // floor above; measured from the feet.
        const int own_here = p.follow_to(stand.x, stand.y, x).value_or(own->y);
        for (const MapFloor& f : p.floors_at(x)) {
            const int up = own->y - f.y;
            if (own_here - f.y <= kJumpUpFeetPx || up <= kJumpUpFeetPx || up > within_px) continue;
            if (!best || up < *best) best = up;
        }
    }
    return best;
}

} // namespace gobot
