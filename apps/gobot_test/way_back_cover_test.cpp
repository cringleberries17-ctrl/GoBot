// gobot_test -- the way back into the owner's five hunting areas from
// everywhere outside them (2026.11.12c, the owner's "phần tự quay về vùng đánh
// quái khi ở ngoài hoạt động chưa ổn đâu"): every floor every 32 px across, from
// 400 px left of each box to 400 px right of it and from its top to 500 px under
// its bottom, whose standing point is outside the box -- asked as the hunter
// asks (Hunter::track_zone_way: in the area's legs, then over the map without
// jumps, then with them), then walked as the hunter walks it, leg by leg, each
// jump landing where a straight jump really comes down at g 2400 and at g 1905
// (the two readings of the live flights of 2026-11-12). A sample with no way is
// explained by a looser search over the map's floors -- every walk, walk-off,
// drop and straight Ctrl+Up the game might allow: out of the game's jumps' reach,
// or only by a drop by a gate. None may be left unexplained.
//
// What "walked in" shows (the 2026.11.12c review): GoBot's plan reaches the area
// given its own model -- only the straight jumps are simulated afresh; a
// walk-off lands by the planner's own walk_off_landing, a drop and a wall-foot
// jump where planned. Not that the bot gets back in the game: at 23:47:45 of
// the owner's log a walk off t23_s14's small 1425 platform at x 1728 fell past
// the 1465 platform the file has at x 1707, which this model counts as landed.
//
// The areas are the owner's of configs\spots.ini -- boxes and homes, nothing
// personal; hunt.ini is never read. The game's map files are READ, never
// written: a copy named by GOBOT_TEST_GAME_DATA, the tests' own copies in
// build\handoff\drop\testdata, or the game's install where it usually is --
// looked up by its folder alone, never the registry. With none of them here it
// is skipped. Nothing here presses a key.

#include "gobot/common.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/jump_plan.hpp"
#include "gobot/map_book.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

template <typename... Args>
std::string fmt(const char* f, Args... args) {
    char buf[1024];
    std::snprintf(buf, sizeof buf, f, args...);
    return buf;
}

void info(const std::string& s) {
    std::printf("  [info] %s\n", s.c_str());
    std::fflush(stdout);
}

/// One of the owner's areas, and the share of its samples with no way back
/// before 2026.11.12c (11.12b's rules, measured): never more now.
struct Area {
    const char* stem;
    int         home_x, home_y, left, top, right, bottom;
    double      before_none;
};
constexpr Area kAreas[] = {
    { "t1901_s2", 495, 632, 339, 532, 705, 700, 0.0 },
    { "t23_s14", 1593, 1262, 740, 1162, 2858, 1586, 1.0 },
    { "t52_s2", 880, 274, 265, 174, 1399, 694, 0.0 },
    { "t53_s2", 2635, 1188, 2335, 1088, 2935, 1288, 0.0 },
    { "t49_s5", 2836, 1702, 1985, 1455, 3720, 1846, 0.0 },
};

/// The first folder with all five maps' files: a copy named by
/// GOBOT_TEST_GAME_DATA, the tests' own copies, the game's install where it
/// usually is. Only whether the files are there is looked at; "" for none.
std::string cover_data() {
    std::vector<std::string> dirs;
    if (const char* env = std::getenv("GOBOT_TEST_GAME_DATA"); env != nullptr && env[0] != '\0') dirs.push_back(env);
#ifdef GOBOT_SOURCE_DIR
    dirs.push_back(std::string(GOBOT_SOURCE_DIR) + "\\build\\handoff\\drop\\testdata\\data");
#endif
    dirs.push_back("D:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\data");
    dirs.push_back("C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\data");
    for (const std::string& d : dirs) {
        bool all = true;
        for (const Area& a : kAreas) all = all && path_exists(d + "\\Map\\" + a.stem + ".map");
        if (all) return d;
    }
    return {};
}

HuntZone box_of(const Area& a) {
    HuntZone z;
    z.home_x = a.home_x;
    z.home_y = a.home_y;
    z.left = a.left;
    z.top = a.top;
    z.right = a.right;
    z.bottom = a.bottom;
    return z;
}

/// The highest floor a jump may come down on and still be the planned one's
/// band (JumpPlan::band_top, 2026.11.12c); -1 none.
int plan_band_top(const JumpPlan& j) { return j.band_top; }

/// A straight jump's peak above the feet at gravity g: ↑ and Ctrl + ↑ from
/// their take-off speeds, ↑↑ with its second push kJumpUpSecondAtMs after the
/// take-off.
double real_peak(JumpKind k, double g) {
    if (k == JumpKind::Up) return kJumpUpV * kJumpUpV / (2.0 * g);
    if (k == JumpKind::CtrlUp) return kJumpUpVHigh * kJumpUpVHigh / (2.0 * g);
    const double t2 = kJumpUpSecondAtMs / 1000.0;
    return kJumpUpV * t2 - g * t2 * t2 / 2.0 + kJumpUpV * kJumpUpV / (2.0 * g);
}

/// Where a straight jump from a floor whose top is `top` at column x really
/// comes down at gravity g: up through every platform under its peak, its head
/// stopped by any other floor within kJumpUpHeadPx over it, and down on the
/// highest platform it rose past -- or back on its own floor. A floor's top;
/// a 1-2 px hole of the landing's floor is landed over, as the planner does.
int real_landing(const GameMapBook::Page& p, int x, int top, JumpKind kind, double g) {
    const double peak = real_peak(kind, g);
    const auto in = [&](int cx) -> int {
        int best = -1;
        for (const MapFloor& f : p.floors_at(cx)) {
            const int rise = top - f.y;
            if (rise <= kJumpUpFeetPx) continue;
            if (!f.platform() && rise <= peak + kJumpUpHeadPx) return -2;  // the head in it
            if (rise < peak && (best < 0 || f.y < best)) best = f.y;
        }
        return best;
    };
    const int here = in(x);
    if (here == -2) return top;
    if (here >= 0) return here;
    for (int d = 1; d <= GameMapBook::kFollowHolePx; ++d)
        for (const int cx : { x - d, x + d }) {
            const int there = in(cx);
            if (there >= 0) return there;
        }
    return top;
}

/// The way asked as Hunter::track_zone_way asks it: in the area's legs with
/// jumps up, then -- outside the box with none -- over the whole map, walking,
/// falling and dropping first, then with jumps up as well. Each standing point
/// asked once.
struct Asker {
    const GameMapBook::Page& p;
    HuntZone                 box;
    HuntZone                 whole;
    JumpRules                all;
    ZoneDropRules            down;
    std::map<std::pair<int, int>, std::optional<ZoneReturn>> memo;
    long long                slowest_ms = 0;
    int                      floors_most = 0;
    int                      capped = 0;        ///< asks a travel's floor budget cut short
    int                      capped_none = 0;   ///< ... of them with no way found

    std::optional<ZoneReturn> ask(MapPoint at) {
        const auto key = std::make_pair(at.x, at.y);
        if (const auto it = memo.find(key); it != memo.end()) return it->second;
        const auto t0 = std::chrono::steady_clock::now();
        std::optional<ZoneReturn> w = return_point(p, box, at, &all);
        bool cut = false;
        if ((!w || w->as_before) && zone_outside_px(box, at.x, at.y) > 0) {
            ZoneReturnStats st;
            w = return_point(p, box, at, nullptr, &whole, &down, &st);
            floors_most = std::max(floors_most, st.floors);
            cut = st.capped;
            if (!w) {
                w = return_point(p, box, at, &all, &whole, &down, &st);
                floors_most = std::max(floors_most, st.floors);
                cut = cut || st.capped;
            }
        }
        if (cut) ++capped;
        if (cut && !w) ++capped_none;
        const long long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        slowest_ms = std::max(slowest_ms, ms);
        memo.emplace(key, w);
        return w;
    }
};

/// What came of walking one sample's way at one g.
struct Walked {
    bool        in = false;
    bool        ctrl_first = false;    ///< its first jump a Ctrl+Up
    bool        up_first = false;      ///< its first jump a plain Up
    int         planned = 0, band = 0, elsewhere = 0;
    std::string why;                   ///< not in: where it stuck
};

/// The way from `from` gone as the hunter goes it, a leg at a time, each jump
/// landing where a straight jump at g really does, and asked again from there.
Walked walk_way(Asker& a, MapPoint from, double g) {
    Walked out;
    bool jumped = false;
    MapPoint at = from;
    for (int leg = 0; leg < 12; ++leg) {
        if (a.box.contains(at.x, at.y)) {
            out.in = true;
            return out;
        }
        const std::optional<ZoneReturn> w = a.ask(at);
        if (!w) {
            out.why = fmt("stranded on the way at %d,%d", at.x, at.y);
            return out;
        }
        if (w->own_floor()) {
            out.in = a.box.contains(w->at.x, w->at.y);
            if (!out.in) out.why = fmt("a walk to %d,%d outside the box", w->at.x, w->at.y);
            return out;
        }
        if (w->jump && w->jump->wall) {
            // A wall-foot jump: onto the step's top, the owner's word.
            if (!jumped) out.ctrl_first = w->jump->kind == JumpKind::CtrlUp;
            jumped = true;
            ++out.planned;
            at = w->jump->land();
            continue;
        }
        if (w->jump) {
            const JumpPlan& j = *w->jump;
            if (!jumped) {
                out.ctrl_first = j.kind == JumpKind::CtrlUp;
                out.up_first = j.kind == JumpKind::Up;
                jumped = true;
            }
            const int top = j.feet_y + 1;
            const int land = real_landing(a.p, j.x, top, j.kind, g);
            const std::optional<int> planned = a.p.follow_to(j.land_x, j.land_top - 1, j.x);
            const int want = planned ? *planned : j.land_top;
            const int band = plan_band_top(j);
            if (std::abs(land - want) <= GameMapBook::kFollowLinkPx)
                ++out.planned;
            else if (land < want && band >= 0 && land >= band - GameMapBook::kFollowLinkPx)
                ++out.band;
            else
                ++out.elsewhere;
            at = { j.x, land - 1 };
            continue;
        }
        if (w->drop) {
            at = w->drop->land;
            continue;
        }
        // Off its floor's end, at a walk's full speed.
        const int dir = w->walk_x < at.x ? -1 : 1;
        const MapTrail t = a.p.follow(at.x, at.y, dir);
        if (t.pts.empty() || !t.ledge) {
            out.why = fmt("no end to walk off from %d,%d", at.x, at.y);
            return out;
        }
        const std::optional<MapPoint> land = walk_off_landing(a.p, t.pts.back(), dir, kZoneWalkPxPerS);
        if (!land) {
            out.why = fmt("a fall onto nothing off %d,%d", t.pts.back().x, t.pts.back().y);
            return out;
        }
        at = *land;
    }
    out.why = fmt("12 legs and still out, at %d,%d", at.x, at.y);
    return out;
}

/// The looser search a sample with no way is explained by: over the map's
/// floors, 512 at most, from where each is come onto and every 16th column
/// along it -- walks, walk-offs at any share, Ctrl + ↓ through any platform
/// (by a gate too with `by_gates`), and a straight jump onto any platform up
/// to Ctrl + ↑'s highest peak less kJumpUpClearPx, nothing in the way
/// counted. Whether a standing point in the box is reached.
bool loose_reach(const GameMapBook::Page& p, const HuntZone& box, MapPoint from, const std::vector<MapGate>& gates,
                 int gate_px, bool by_gates) {
    const int peak = jump_peak(JumpKind::CtrlUp).hi - kJumpUpClearPx;
    std::vector<MapPoint> todo{ from };
    std::vector<std::pair<MapPoint, MapPoint>> seen;
    for (std::size_t i = 0; i < todo.size() && seen.size() < 512; ++i) {
        const MapPoint at = todo[i];
        const MapTrail l = p.follow(at.x, at.y, -1), r = p.follow(at.x, at.y, 1);
        if (l.pts.empty() || r.pts.empty()) continue;
        const MapPoint le = l.pts.back(), re = r.pts.back();
        if (std::any_of(seen.begin(), seen.end(), [&](const std::pair<MapPoint, MapPoint>& s) {
                return s.first.x == le.x && s.first.y == le.y && s.second.x == re.x && s.second.y == re.y;
            }))
            continue;
        seen.emplace_back(le, re);
        std::vector<MapPoint> looks;
        for (const MapTrail* t : { &l, &r }) {
            for (std::size_t k = 0; k < t->pts.size(); ++k) {
                const MapPoint q = t->pts[k];
                if (box.contains(q.x, q.y - 1)) return true;
                if (k % 16 == 0 || k + 1 == t->pts.size()) looks.push_back(q);
            }
            if (t->step) todo.push_back({ t->step_x, t->step_top - 1 });   // a wall-foot climb
            if (t->ledge) {
                const int dir = t == &l ? -1 : 1;
                for (const double share : kZoneWalkOffShares)
                    if (const std::optional<MapPoint> land =
                            walk_off_landing(p, t->pts.back(), dir, share * kZoneWalkPxPerS))
                        todo.push_back(*land);
            }
        }
        for (const MapPoint& q : looks) {
            const std::optional<MapFloor> own = p.floor_near(q.x, q.y - 1);
            if (own && own->platform()) {
                const std::optional<MapFloor> below = p.floor_below(q.x, q.y - 1, 8);
                if (below && !below->wall()) {
                    bool gated = false;
                    for (const MapGate& g : gates)
                        if (std::abs(g.x - q.x) <= gate_px && g.y >= q.y - 1 - 60 && g.y <= below->y + 60)
                            gated = true;
                    if (!gated || by_gates) todo.push_back({ q.x, below->y - 1 });
                }
            }
            for (const MapFloor& f : p.floors_at(q.x)) {
                const int rise = q.y - f.y;
                if (f.platform() && rise > kJumpUpFeetPx && rise <= peak) todo.push_back({ q.x, f.y - 1 });
            }
        }
    }
    return false;
}

/// Why each key does not go from a standing point, judged in the whole map's
/// box: "Up no floor reached for sure; ...".
std::string keys_here(const GameMapBook::Page& p, const HuntZone& whole, MapPoint at) {
    std::string s;
    for (const JumpKind k : { JumpKind::Up, JumpKind::UpUp, JumpKind::CtrlUp }) {
        const JumpColumn c = judge_column(p, whole, at, k, JumpRules{});
        if (!s.empty()) s += "; ";
        s += fmt("%s %s", jump_kind_name(k), c.ok() ? "goes" : jump_no_name(c.why));
        if (c.bad_top >= 0) s += fmt(" at %d", c.bad_top);
    }
    return s;
}

/// A standing point near x, y: on its column's floor there, or the first
/// below it.
std::optional<MapPoint> stand_near(const GameMapBook::Page& p, int x, int y) {
    if (const std::optional<MapFloor> f = p.floor_near(x, y)) return MapPoint{ x, f->y - 1 };
    if (const std::optional<MapFloor> f = p.floor_below(x, y, 0)) return MapPoint{ x, f->y - 1 };
    return std::nullopt;
}

void test_area(GameMapBook& book, const Area& a) {
    const std::shared_ptr<const GameMapBook::Page> page = book.get(a.stem);
    check(page != nullptr, fmt("%s reads", a.stem));
    if (!page) return;
    const GameMapBook::Page& p = *page;
    const HuntZone box = box_of(a);
    Asker ask{ p, box, zone_whole_map(p, box), JumpRules{}, ZoneDropRules{ book.gates(a.stem), 150, {} } };

    // The samples.
    std::vector<MapPoint> samples;
    const int x0 = std::max(0, box.left - 400), x1 = std::min(p.floors.w_px() - 1, box.right + 400);
    for (int x = x0; x <= x1; x += 32)
        for (const MapFloor& f : p.floors_at(x))
            if (!f.wall() && f.y >= box.top && f.y <= box.bottom + 500 && !box.contains(x, f.y - 1))
                samples.push_back({ x, f.y - 1 });

    const auto t0 = std::chrono::steady_clock::now();
    int found = 0, inbox = 0, travel = 0, none = 0, beyond = 0, gate = 0, odd = 0;
    std::string odd_text;
    std::vector<MapPoint> ways;
    for (const MapPoint& s : samples) {
        const std::optional<ZoneReturn> w = ask.ask(s);
        if (w) {
            ++found;
            (w->travel ? travel : inbox) += 1;
            ways.push_back(s);
            continue;
        }
        ++none;
        if (!loose_reach(p, box, s, ask.down.gates, ask.down.gate_px, true)) {
            ++beyond;
        } else if (!loose_reach(p, box, s, ask.down.gates, ask.down.gate_px, false)) {
            ++gate;
        } else {
            ++odd;
            if (odd_text.size() < 600) odd_text += fmt(" %d,%d (%s);", s.x, s.y, keys_here(p, ask.whole, s).c_str());
        }
    }
    struct AtG {
        double g;
        int    in = 0, planned = 0, band = 0, elsewhere = 0;
        std::string stuck;
    };
    AtG at_g[] = { { 2400.0 }, { 1905.0 } };
    for (AtG& r : at_g) {
        for (const MapPoint& s : ways) {
            const Walked w = walk_way(ask, s, r.g);
            if (w.in) ++r.in;
            else if (r.stuck.size() < 400) r.stuck += fmt(" from %d,%d: %s;", s.x, s.y, w.why.c_str());
            r.planned += w.planned;
            r.band += w.band;
            r.elsewhere += w.elsewhere;
        }
    }
    const long long total_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    const int n = static_cast<int>(samples.size());
    info(fmt("way back from outside %s (%d..%d x %d..%d): sampled %d; a way %d (in the area's legs %d, over the map "
             "%d); no way %d (beyond the game's jumps %d, gate %d, UNEXPLAINED %d); walked in at g 2400 %d of %d, at "
             "g 1905 %d of %d; jumps as planned %d/%d, on a band floor %d/%d, elsewhere %d/%d (g 2400/1905); floors "
             "looked at up to %d, budget used up %d times (%d with no way found); slowest ask %lld ms; %lld ms in all",
             a.stem, box.left, box.right, box.top, box.bottom, n, found, inbox, travel, none, beyond, gate, odd,
             at_g[0].in, found, at_g[1].in, found, at_g[0].planned, at_g[1].planned, at_g[0].band, at_g[1].band,
             at_g[0].elsewhere, at_g[1].elsewhere, ask.floors_most, ask.capped, ask.capped_none, ask.slowest_ms,
             total_ms));

    check(odd == 0, fmt("%s: every sample with no way back is explained -- out of the game's jumps' reach (%d) or only "
                        "by a drop by a gate (%d); unexplained %d:%s",
                        a.stem, beyond, gate, odd, odd_text.c_str()));
    const double share = n > 0 ? static_cast<double>(none) / n : 0.0;
    check(share <= a.before_none + 1e-9,
          fmt("%s: no more samples with no way than before 2026.11.12c (%d of %d, %.0f%%; before %.0f%%)", a.stem, none,
              n, share * 100.0, a.before_none * 100.0));
    if (std::string(a.stem) == "t23_s14")
        check(share <= 0.05, fmt("t23_s14: a way back from 95%% of the places outside its area at least (%d of %d with "
                                 "none)",
                                 none, n));
    for (const AtG& r : at_g)
        check(r.in == found && r.elsewhere == 0,
              fmt("%s at g %.0f: every way walked in (%d of %d), no jump landing on a floor it did not plan for (%d)%s",
                  a.stem, r.g, r.in, found, r.elsewhere, r.stuck.c_str()));
    check(ask.capped_none == 0, fmt("%s: no way back lost to the travel's floor budget (cut short %d times, %d with no "
                                    "way found)",
                                    a.stem, ask.capped, ask.capped_none));

    // The places the logs of 2026-11-12 put the character at, under t23_s14's
    // area: the bottom floor and the ground under it.
    if (std::string(a.stem) == "t23_s14") {
        struct Logged {
            int  x, y;
            bool ctrl_first;
        };
        const Logged logged[] = { { 1826, 1806, true }, { 1757, 1810, true }, { 1707, 1802, true },
                                  { 1826, 1863, false }, { 1707, 1883, false } };
        for (const Logged& l : logged) {
            const std::optional<MapPoint> s = stand_near(p, l.x, l.y);
            const std::optional<ZoneReturn> w = s ? ask.ask(*s) : std::nullopt;
            const Walked hi = s ? walk_way(ask, *s, 2400.0) : Walked{};
            const Walked lo = s ? walk_way(ask, *s, 1905.0) : Walked{};
            check(s && w && hi.in && lo.in && (l.ctrl_first ? hi.ctrl_first : hi.up_first) &&
                      (l.ctrl_first ? lo.ctrl_first : lo.up_first),
                  fmt("t23_s14 from %d,%d (logged %d,%d): a way back%s, walked in at g 2400 and at g 1905 (%s / %s)",
                      s ? s->x : -1, s ? s->y : -1, l.x, l.y,
                      l.ctrl_first ? " whose first jump is Ctrl+Up" : " whose first jump is one Up back onto the bottom",
                      hi.in ? "in" : hi.why.c_str(), lo.in ? "in" : lo.why.c_str()));
        }
    }
}

/// The first folder with t60_s1's map (the walls' test below), looked up as
/// cover_data() does; "" for none.
std::string pocket_data() {
    std::vector<std::string> dirs;
    if (const char* env = std::getenv("GOBOT_TEST_GAME_DATA"); env != nullptr && env[0] != '\0') dirs.push_back(env);
#ifdef GOBOT_SOURCE_DIR
    dirs.push_back(std::string(GOBOT_SOURCE_DIR) + "\\build\\handoff\\drop\\testdata\\data");
#endif
    dirs.push_back("D:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\data");
    dirs.push_back("C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\data");
    for (const std::string& d : dirs)
        if (path_exists(d + "\\Map\\t60_s1.map")) return d;
    return {};
}

/// t60_s1's pocket (2026.11.15, the owner's log of 00:18): a ledge at 1035
/// over x 4480..4599 whose left is a block of the file -- attr 6, no surface,
/// x 4448..4479, y 960..1055 -- that the walk left pressed against for 12 s
/// while the plan called the end a fall. The file's walls now stop a walk:
/// left is the wall, right is the only end, and from outside a box that stops
/// short of the ledge the way back goes the other way.
void test_pocket(const std::string& data) {
    GameMapBook book(data);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t60_s1");
    check(page != nullptr, "t60_s1 reads");
    if (!page) return;
    const GameMapBook::Page& p = *page;
    check(!p.floors.blocks.empty() && p.floors.block_in(4460, 1000, 1034) == 6 && p.floors.block_in(4400, 930, 950) == 6 &&
              p.floors.block_in(4500, 1000, 1034) == 0,
          "t60_s1's blocks are read: the attr-6 wall of the pocket (x 4448..4479) and the one under the step");
    // The notes' figures (README, DOC-TRUOC; the 2026.11.15 review caught t53_s2
    // as 10, with 3 of its 10 attr-4 cells holding a surface): the blocks read
    // on the owner's maps. And the ground's fill -- a layer-1 record in a cell
    // the ground is in -- is no wall: t100_s3's ground runs to its end.
    const auto count_blocks = [](const GameMapBook::Page& pg) {
        int n = 0;
        for (const std::uint8_t a : pg.floors.blocks)
            if (a != 0) ++n;
        return n;
    };
    check(count_blocks(p) == 47, fmt("t60_s1 reads 47 blocks, as the notes say (%d)", count_blocks(p)));
    for (const char* stem : { "t49_s5", "t53_s2" }) {
        if (!path_exists(data + "\\Map\\" + stem + ".map")) continue;
        const int want = std::string(stem) == "t49_s5" ? 30 : 7;
        const std::shared_ptr<const GameMapBook::Page> pg = book.get(stem);
        check(pg && count_blocks(*pg) == want,
              fmt("%s reads %d blocks, as the notes say (%d)", stem, want, pg ? count_blocks(*pg) : -1));
    }
    if (path_exists(data + "\\Map\\t100_s3.map")) {
        const std::shared_ptr<const GameMapBook::Page> pg = book.get("t100_s3");
        const MapTrail fill = pg ? pg->follow(1300, 3605, 1) : MapTrail{};
        check(pg && fill.ledge && !fill.wall && !fill.pts.empty() && fill.pts.back().x == 1503,
              fmt("t100_s3's ground at 1300,3605 walked right: its end at 1503, the fill under it no wall (ledge %d, "
                  "wall %d, last %d)",
                  fill.ledge ? 1 : 0, fill.wall ? 1 : 0, fill.pts.empty() ? -1 : fill.pts.back().x));
    }

    const MapTrail left = p.follow(4533, 1034, -1);
    check(left.wall && left.stop && !left.ledge && left.wall_x == 4479 && left.pts.back().x == 4480,
          fmt("t60_s1 4533,1034 walking left: the wall at x %d, the walk stops at %d -- no floor's end to walk off",
              left.wall_x, left.pts.empty() ? -1 : left.pts.back().x));
    const MapTrail right = p.follow(4533, 1034, 1);
    check(right.ledge && !right.wall && !right.pts.empty() && right.pts.back().x >= 4590 && right.pts.back().x <= 4600,
          fmt("and right: the ledge's own end (at %d), where a walk off lands on the ground",
              right.pts.empty() ? -1 : right.pts.back().x));
    if (!right.pts.empty()) {
        const std::optional<MapPoint> land = walk_off_landing(p, right.pts.back(), 1, kZoneWalkPxPerS);
        check(land.has_value() && land->y > 1200, "a walk off that end lands on the ground (1237)");
    }
    const MapTrail step = p.follow(4440, 954, -1);
    check(step.wall && step.stop && !step.ledge && step.pts.back().x == 4416,
          "the floor at 955 left of the pocket stops at the wall under the middle floor's end, x 4416");

    // 2026-11-15 00:05:56 (the owner's log): a dash threw the character off the
    // top floor's left end onto the hillside ledge at ~1006, 153 px outside
    // the box then (2398..4421 x 365..1337, home 2617,518). A way back from
    // where it landed, 2245,1006 -- and from 2273,1005, where it stood 8 s
    // later: both, by the map, with jumps or without.
    {
        HuntZone e1;
        e1.home_x = 2617;
        e1.home_y = 518;
        e1.left = 2398;
        e1.top = 365;
        e1.right = 4421;
        e1.bottom = 1337;
        Asker a1{ *page, e1, zone_whole_map(*page, e1), JumpRules{}, ZoneDropRules{ book.gates("t60_s1"), 150, {} } };
        for (const int x : { 2245, 2273, 2160, 2330 }) {
            const std::optional<MapPoint> s = stand_near(*page, x, 1006);
            const std::optional<ZoneReturn> w = s ? a1.ask(*s) : std::nullopt;
            info(fmt("t60_s1 E1 from %d,%d: %s", s ? s->x : -1, s ? s->y : -1,
                     !w ? "NO WAY"
                     : w->jump ? "a jump first"
                     : w->drop ? "a drop first"
                     : w->own_floor() ? "walks along its floor"
                                      : fmt("walk to %d, falls %d, jumps %d", w->walk_x, w->falls, w->jumps).c_str()));
            check(w.has_value(), fmt("t60_s1 (E1) a way back from the hillside ledge at %d,1006", x));
        }
    }

    // From outside a box that stops short of the ledge (its columns start at
    // 4480): the way is not left, into the wall.
    HuntZone box;
    box.home_x = 2617;
    box.home_y = 518;
    box.left = 2042;
    box.top = 365;
    box.right = 4470;
    box.bottom = 1367;
    Asker ask{ *page, box, zone_whole_map(*page, box), JumpRules{}, ZoneDropRules{ book.gates("t60_s1"), 150, {} } };
    for (const int x : { 4510, 4533, 4560, 4590 }) {
        const std::optional<ZoneReturn> w = ask.ask(MapPoint{ x, 1034 });
        const bool away = w && !w->own_floor() && (w->drop || w->walk_x > x || w->jump);
        check(w.has_value() && away,
              fmt("t60_s1 from the pocket ledge at %d,1034, a box ending at 4470: a way back that is a drop, a jump or a "
                  "walk off its right end -- never along it to the wall (%s)",
                  x, !w ? "none" : w->own_floor() ? "walks along its floor" : w->drop ? "a drop" : w->jump ? "a jump"
                                                                                                           : "off an end"));
        const Walked in = walk_way(ask, MapPoint{ x, 1034 }, 2400.0);
        check(in.in, fmt("... and it walks in at g 2400 (%s)", in.in ? "in" : in.why.c_str()));
    }
}

} // namespace

void test_way_back_cover(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    section("The way back into the owner's five hunting areas from everywhere outside them, walked at g 2400 and "
            "1905 (2026.11.12c)");
    const std::string data = cover_data();
    if (data.empty()) {
        skip("no copies or install of the game's map files here: the way back from outside the owner's five areas\n");
    } else {
        info("map files read from " + data);
        GameMapBook book(data);
        for (const Area& a : kAreas) test_area(book, a);
    }
    section("t60_s1's pocket: the file's walls stop a walk (2026.11.15)");
    const std::string pocket = pocket_data();
    if (pocket.empty()) {
        skip("no copy or install of t60_s1's map file here: the pocket's wall\n");
        return;
    }
    test_pocket(pocket);
}
