// gobot_test -- jumping straight up onto a floor above (2026.11.05b, "Nhảy lên
// bục trên"): the planner alone, jump_plan.hpp -- the model's peaks, the
// column rule, the take-off chosen, the runs over the owner's boxes -- and the
// way back with jumps (return_point's `up`), on made-up floors and on copies
// of the game's own t52_s2 and t53_s2 in build\handoff\drop\testdata, never
// the game's install, skipped when they are not there. The numbers are the
// prototypes' (plan05b.py, runs05b.py), ±4 px where the plan allows it.
// Nothing here presses a key.

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
#include <functional>
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
    char buf[640];
    std::snprintf(buf, sizeof buf, f, args...);
    return buf;
}

/// The game's files copied for the tests, or "" -- never the game's install.
std::string game_data() {
#ifdef GOBOT_SOURCE_DIR
    const std::string dir = std::string(GOBOT_SOURCE_DIR) + "\\build\\handoff\\drop\\testdata\\data";
    if (path_exists(dir + "\\Map\\t52_s2.map") && path_exists(dir + "\\Map\\t53_s2.map")) return dir;
#endif
    return {};
}

/// A map's floors made column by column, as parse_map would leave them.
struct Floors {
    int w_cells;
    int h_cells;
    std::vector<std::vector<MapFloor>> cols;

    Floors(int wc, int hc) : w_cells(wc), h_cells(hc), cols(static_cast<std::size_t>(wc) * 32) {}
    void run(int x0, int x1, const std::function<int(int)>& y_at, std::uint8_t attr) {
        for (int x = x0; x <= x1; ++x) {
            MapFloor f;
            f.y = y_at(x);
            f.attr = attr;
            cols[static_cast<std::size_t>(x)].push_back(f);
        }
    }
    void flat(int x0, int x1, int y, std::uint8_t attr) {
        run(x0, x1, [y](int) { return y; }, attr);
    }
    /// Takes column x's floor at y out: a hole.
    void hole(int x0, int x1, int y) {
        for (int x = x0; x <= x1; ++x) {
            std::vector<MapFloor>& c = cols[static_cast<std::size_t>(x)];
            c.erase(std::remove_if(c.begin(), c.end(), [y](const MapFloor& f) { return f.y == y; }), c.end());
        }
    }
    GameMapBook::Page page(const char* stem) const {
        GameMapBook::Page p;
        p.stem = stem;
        p.floors.w_cells = w_cells;
        p.floors.h_cells = h_cells;
        for (std::vector<MapFloor> c : cols) {
            p.floors.start.push_back(static_cast<std::uint32_t>(p.floors.floors.size()));
            std::sort(c.begin(), c.end(), [](const MapFloor& a, const MapFloor& b) { return a.y < b.y; });
            p.floors.floors.insert(p.floors.floors.end(), c.begin(), c.end());
        }
        p.floors.start.push_back(static_cast<std::uint32_t>(p.floors.floors.size()));
        p.prj_read = true;
        return p;
    }
};

HuntZone box_of(int home_x, int home_y, int left, int top, int right, int bottom) {
    HuntZone z;
    z.home_x = home_x;
    z.home_y = home_y;
    z.left = left;
    z.top = top;
    z.right = right;
    z.bottom = bottom;
    return z;
}

MapGate gate_at(int x, int y) {
    MapGate g;
    g.x = x;
    g.y = y;
    g.kind = 3900000;
    return g;
}

/// The standing point on the floor of column x nearest y: its top - 1.
int stand_y(const GameMapBook::Page& p, int x, int y) {
    int best = y, off = INT_MAX;
    for (const MapFloor& f : p.floors_at(x))
        if (std::abs(f.y - 1 - y) < off) {
            off = std::abs(f.y - 1 - y);
            best = f.y - 1;
        }
    return best;
}

bool near(int a, int b, int tol) { return std::abs(a - b) <= tol; }

JumpRules kinds_only(std::uint8_t kinds) {
    JumpRules r;
    r.kinds = kinds;
    return r;
}

constexpr std::uint8_t kUp = 1u << 0;
constexpr std::uint8_t kUpUp = 1u << 1;
constexpr std::uint8_t kCtrlUp = 1u << 2;

/// "Ctrl+Up 663..916 (254, take-offs 695..884); ..." -- usable runs, or every one.
std::string runs_text(const std::vector<JumpRun>& rs, bool all) {
    std::string s;
    for (const JumpRun& r : rs) {
        if (!all && !r.usable()) continue;
        if (!s.empty()) s += "; ";
        s += fmt("%s %d..%d (%d, floor %d..%d onto %d..%d", jump_kind_name(r.kind), r.x0, r.x1, r.width(), r.take_y0,
                 r.take_y1, r.land_y0, r.land_y1);
        s += r.usable() ? fmt(", take-offs %d..%d)", r.take_l(), r.take_r()) : std::string(", narrow)");
    }
    return s.empty() ? std::string("none") : s;
}

std::vector<JumpRun> usable_runs(const std::vector<JumpRun>& rs) {
    std::vector<JumpRun> out;
    for (const JumpRun& r : rs)
        if (r.usable()) out.push_back(r);
    return out;
}

// --------------------------------------------------------------- the model

void test_model() {
    section("Jump-up plan: each key's peak, what it lands on for sure, the second Up's window, g from a flight");

    // 2026.11.12c: gravity 1900..2400 -- both readings of the live flights of
    // 2026-11-12 (g≈1905 at face value; GoC's 2400 with a late landing
    // report). Re-derived by wbx_check5.py's bracket: Up 133.3..168.4, Up,Up
    // 250.3..336.8, Ctrl+Up 432.0..545.7 (until 11.12b: 121..143, 213..286,
    // 392..463 at 2240..2640).
    const JumpPeak up = jump_peak(JumpKind::Up), uu = jump_peak(JumpKind::UpUp), hi = jump_peak(JumpKind::CtrlUp);
    check(up.lo == 133 && up.hi == 169 && uu.lo == 250 && uu.hi == 337 && hi.lo == 432 && hi.hi == 546,
          fmt("peaks above the feet: Up %d..%d, Up,Up %d..%d, Ctrl+Up %d..%d (133.3..168.4, 250.3..336.8, "
              "432.0..545.7: lowest down, highest up)",
              up.lo, up.hi, uu.lo, uu.hi, hi.lo, hi.hi));
    check(up.usable() == 121 && up.beyond() == 181 && uu.usable() == 238 && uu.beyond() == 349 &&
              hi.usable() == 420 && hi.beyond() == 558,
          fmt("landed on for sure up to %d / %d / %d px, the next floor above more than %d / %d / %d -- or a "
              "platform inside the box (2026.11.12c's band)",
              up.usable(), uu.usable(), hi.usable(), up.beyond(), uu.beyond(), hi.beyond()));
    // The owner's "Theo GoC": nominal peaks, inside the ranges.
    check(133 >= up.lo && 133 <= up.hi && 280 >= uu.lo && 280 <= uu.hi && 432 >= hi.lo && 432 <= hi.hi &&
              up.usable() < 133 && uu.usable() < 280 && hi.usable() < 432,
          "the owner's 133 / 280 / 432 px (GoC's) lie in the ranges: upper bounds, never planned to");
    check(std::lround(jump_shortest_arc_s(JumpKind::Up) * 1000.0) == 667 &&
              std::lround(jump_shortest_arc_s(JumpKind::CtrlUp) * 1000.0) == 1200,
          fmt("the shortest full arcs: Up %.0f ms, Ctrl+Up %.0f ms (2v/g at 2400)",
              jump_shortest_arc_s(JumpKind::Up) * 1000.0, jump_shortest_arc_s(JumpKind::CtrlUp) * 1000.0));
    check(kJumpUpSecondAtMs - kJumpUpSecondEarlyMs == 280 && kJumpUpSecondAtMs + kJumpUpSecondLateMs == 450 &&
              kJumpUpCeilingPx == 650 && kJumpUpMarginPx == 24 && kJumpUpRunMinPx == 64 && kJumpUpRunInsetPx == 32 &&
              !kJumpUpGateBand,
          "the second Up modelled 0.28..0.45 s after the take-off, from the one press constant (340 ms); the "
          "650 px ceiling; no gate band (the owner's answer 2)");
    check(std::string(jump_kind_name(JumpKind::Up)) == "Up" && std::string(jump_kind_name(JumpKind::UpUp)) == "Up,Up" &&
              std::string(jump_kind_name(JumpKind::CtrlUp)) == "Ctrl+Up" &&
              std::string(jump_kind_key(JumpKind::UpUp)) == "↑↑" &&
              std::string(jump_kind_key(JumpKind::CtrlUp)) == "Ctrl+↑",
          "the keys' names: Up, Up,Up, Ctrl+Up in the log; ↑↑, Ctrl+↑ on the card");

    // A clean Ctrl + ↑ of 0.845 s up 360 px at -1440 px/s: g about 2400.
    const std::optional<double> g = gravity_from(0.0, -1440.0, -360.0, 0.845);
    check(g && std::abs(*g - 2400.0) <= 0.02 * 2400.0 && !gravity_from(0.0, -800.0, 0.0, 0.0),
          fmt("g from a flight of 0.845 s, 360 px up at -1440: %.0f px/s² (within 2%% of 2400); none for no time",
              g ? *g : -1.0));
}

// ----------------------------------------------------------- made-up floors

void test_columns() {
    section("Jump-up plan: the column rule on made-up floors -- blocked, uncertain, the weaker jump, gates, holes, "
            "edges, what was learned");

    // The ground at 1000 (attr 2); a platform 100 px up over 200..1000; one
    // 180 px over that over 500..700.
    Floors stair(64, 40);
    stair.flat(0, 2047, 1000, 2);
    stair.flat(200, 1000, 900, 1);
    stair.flat(500, 700, 720, 1);
    const GameMapBook::Page sp = stair.page("t90_s1");
    const HuntZone sz = box_of(600, 719, 0, 600, 1500, 1100);
    const JumpRules all;

    JumpColumn c = judge_column(sp, sz, { 250, 999 }, JumpKind::Up, all);
    check(c.ok() && c.land_top == 900 && c.rise == 100 && c.headroom == 33 && c.take_top == 1000,
          fmt("Up from the ground at 250: onto the platform 100 px up, headroom 33 (%s, %d)", jump_no_name(c.why),
              c.land_top));
    c = judge_column(sp, sz, { 600, 899 }, JumpKind::UpUp, all);
    check(c.ok() && c.land_top == 720 && c.rise == 180,
          fmt("Up,Up from the platform at 600: onto the one 180 px above (%s)", jump_no_name(c.why)));
    c = judge_column(sp, sz, { 600, 899 }, JumpKind::Up, all);
    check(c.why == JumpNo::Short, "a plain Up there reaches nothing: Short");
    c = judge_column(sp, sz, { 600, 999 }, JumpKind::UpUp, all);
    check(c.ok() && c.land_top == 900 && c.band_top == 720,
          fmt("Up,Up from the ground under both: the top one 280 px up is in its uncertain band 239..349, a platform "
              "inside the box -- accepted (2026.11.12c): planned onto 900, a landing up to 720 works (%s, %d, band "
              "%d)",
              jump_no_name(c.why), c.land_top, c.band_top));
    JumpRules no_top;
    no_top.ceiling_px = 0;
    c = judge_column(sp, box_of(600, 899, 0, 800, 1500, 1100), { 600, 999 }, JumpKind::UpUp, no_top);
    check(c.why == JumpNo::Ambiguous && c.bad_top == 720,
          fmt("the same with the box's top under it (no ceiling guard, a test): a band floor outside the box -- "
              "Ambiguous (%s, %d)",
              jump_no_name(c.why), c.bad_top));
    c = judge_column(sp, sz, { 600, 999 }, JumpKind::CtrlUp, all);
    check(c.ok() && c.land_top == 720 && c.rise == 280, "Ctrl+Up from there: through the platform onto the top one");
    c = judge_column(sp, sz, { 600, 940 }, JumpKind::Up, all);
    check(c.why == JumpNo::OffFloor, "a point on no floor: OffFloor");

    // A floor that is not a platform in the way: blocked, whatever is above.
    Floors solid(64, 40);
    solid.flat(0, 2047, 1000, 2);
    solid.flat(300, 700, 900, 2);
    solid.flat(300, 700, 800, 1);
    const GameMapBook::Page bp = solid.page("t90_s2");
    c = judge_column(bp, sz, { 500, 999 }, JumpKind::Up, all);
    check(c.why == JumpNo::Blocked && c.bad_top == 900, "an attr-2 floor 100 px up: Up is Blocked");
    c = judge_column(bp, sz, { 500, 999 }, JumpKind::CtrlUp, all);
    check(c.why == JumpNo::Blocked, "and Ctrl+Up too, the platform above it or not");
    Floors wall(64, 40);
    wall.flat(0, 2047, 1000, 2);
    wall.flat(300, 700, 740, 4);
    c = judge_column(wall.page("t90_s3"), sz, { 500, 999 }, JumpKind::CtrlUp, all);
    check(c.why == JumpNo::Blocked, "a wall (attr 4) 260 px up: Ctrl+Up is Blocked -- unknown attributes block");

    // A floor in Up's uncertain band, 130 px up: Up refused, Up,Up lands on it.
    Floors band(64, 40);
    band.flat(0, 2047, 1000, 2);
    band.flat(300, 700, 870, 1);
    const GameMapBook::Page ap = band.page("t90_s4");
    c = judge_column(ap, sz, { 500, 999 }, JumpKind::Up, all);
    check(c.why == JumpNo::Short,
          fmt("a floor 130 px up, in Up's band 122..181 and inside the box: accepted, but nothing under it reached "
              "for sure -- Short (%s)",
              jump_no_name(c.why)));
    c = judge_column(ap, sz, { 500, 999 }, JumpKind::UpUp, all);
    check(c.ok() && c.land_top == 870, "Up,Up lands on it for sure");

    // The weaker jump: from below the box (Closer), Up,Up onto a floor in it;
    // a plain Up -- the second key lost -- onto the floor between, outside.
    Floors weak(64, 40);
    weak.flat(0, 2047, 900, 1);
    weak.flat(300, 700, 800, 1);
    weak.flat(300, 700, 720, 1);
    const GameMapBook::Page wp = weak.page("t90_s5");
    const HuntZone wz = box_of(500, 719, 0, 600, 1500, 790);
    JumpRules no_ceiling;
    no_ceiling.ceiling_px = 0;
    c = judge_column(wp, wz, { 500, 899 }, JumpKind::UpUp, no_ceiling, 109);
    check(c.why == JumpNo::Weaker && c.bad_top == 800 && c.land_top == 720,
          fmt("Up,Up from 109 px below the box onto a floor in it, the plain Up landing on one outside: Weaker "
              "(%s, %d)",
              jump_no_name(c.why), c.bad_top));
    c = judge_column(wp, wz, { 500, 899 }, JumpKind::UpUp, all, 109);
    check(c.why == JumpNo::Ceiling && c.bad_top == 800, "with the ceiling guard -- always but in a test -- Ceiling");
    c = judge_column(wp, wz, { 500, 899 }, JumpKind::UpUp, all);
    check(c.why == JumpNo::Outside, "and no take-off outside the box but coming back into it (Closer)");

    // The judges' gate band, for Ctrl + ↑ only, only when given one.
    Floors gated(64, 40);
    gated.flat(0, 2047, 1000, 2);
    gated.flat(100, 900, 920, 1);
    const GameMapBook::Page gp = gated.page("t90_s6");
    const std::vector<MapGate> gates{ gate_at(500, 990) };
    JumpRules banded;
    banded.gates = gates;
    banded.gate_px = 150;
    c = judge_column(gp, sz, { 560, 999 }, JumpKind::CtrlUp, banded);
    check(c.why == JumpNo::Gate && c.bad_x == 500, "a band of 150 px round a gate: Ctrl+Up 60 px from it -- Gate");
    check(judge_column(gp, sz, { 560, 999 }, JumpKind::Up, banded).ok() &&
              judge_column(gp, sz, { 560, 999 }, JumpKind::CtrlUp, all).ok(),
          "Up is never banded; and with no band -- the owner's answer 2 -- Ctrl+Up goes");

    // Holes: the game walks over 1-2 px; a jump lands over them too.
    Floors holed(64, 40);
    holed.flat(0, 2047, 1000, 2);
    holed.flat(100, 900, 700, 1);
    holed.hole(600, 600, 700);
    const GameMapBook::Page hp = holed.page("t90_s7");
    c = judge_column(hp, sz, { 600, 999 }, JumpKind::CtrlUp, all);
    check(c.ok() && c.land_top == 700 && judge_column(hp, sz, { 592, 999 }, JumpKind::CtrlUp, all).ok(),
          "a 1-px hole in the floor above, at the take-off or a margin's column: landed over, as walked over");
    check(c.land_x == 599, fmt("the landing over it on the column before the hole, 599 (%d)", c.land_x));
    Floors four = holed;
    four.hole(597, 600, 700);
    c = judge_column(four.page("t90_s7"), sz, { 600, 999 }, JumpKind::CtrlUp, all);
    check(c.why == JumpNo::Short, fmt("a 4-px one, more than a walk crosses: Short (%s)", jump_no_name(c.why)));
    // A floor's real end is not a hole: Up from the ground 24 px in from the
    // platform's end at 200, not 22.
    check(judge_column(sp, sz, { 224, 999 }, JumpKind::Up, all).ok() &&
              judge_column(sp, sz, { 223, 999 }, JumpKind::Up, all).why == JumpNo::Short,
          "a floor's end is no hole: the margin's column must be on it -- 224 goes, 223 falls short");

    // The landing's floor changing within the margin: Edge.
    Floors step(64, 40);
    step.flat(0, 2047, 1000, 2);
    step.flat(100, 500, 700, 1);
    step.flat(501, 900, 690, 1);
    const GameMapBook::Page ep = step.page("t90_s8");
    c = judge_column(ep, sz, { 500, 999 }, JumpKind::CtrlUp, all);
    check(c.why == JumpNo::Edge, fmt("floors 700 and 690 meeting at 500: Edge (%s)", jump_no_name(c.why)));
    check(judge_column(ep, sz, { 470, 999 }, JumpKind::CtrlUp, all).ok() &&
              judge_column(ep, sz, { 530, 999 }, JumpKind::CtrlUp, all).ok(),
          "24 px or more from it either way: fine");

    // What the hunter learned comes back through refused.
    JumpRules learned;
    learned.refused = [](MapPoint take, int land_top, JumpKind) {
        if (std::abs(take.x - 250) <= 48 && std::abs(take.y - 999) <= 24) return JumpNo::Learned;
        if (land_top == 720) return JumpNo::Solid;
        return JumpNo::None;
    };
    check(judge_column(sp, sz, { 260, 999 }, JumpKind::Up, learned).why == JumpNo::Learned &&
              judge_column(sp, sz, { 600, 899 }, JumpKind::UpUp, learned).why == JumpNo::Solid &&
              judge_column(sp, sz, { 320, 999 }, JumpKind::Up, learned).ok(),
          "a learned no-jump point refuses within 48 px (Learned); a floor solid from below refuses landing on it "
          "(Solid); elsewhere it goes");
}

void test_plans() {
    section("Jump-up plan: the take-off chosen, why none, and the way back with jumps on made-up floors");

    Floors stair(64, 40);
    stair.flat(0, 2047, 1000, 2);
    stair.flat(200, 1000, 900, 1);
    stair.flat(500, 700, 720, 1);
    const GameMapBook::Page sp = stair.page("t90_s1");
    const HuntZone sz = box_of(600, 719, 0, 600, 1500, 1100);
    const JumpRules all;
    const JumpRules no_ctrl = kinds_only(kUp | kUpUp);

    // A staircase: Up onto the platform, then Up,Up onto the top -- two legs
    // with Ctrl + ↑ off; one Ctrl + ↑ with it.
    std::optional<ZoneReturn> r = return_point(sp, sz, { 250, 999 }, &no_ctrl);
    check(r && r->home && r->jumps == 2 && r->falls == 0 && r->jump && r->jump->kind == JumpKind::Up &&
              r->jump->x == 256 && r->walk_x == 256 && !r->own_floor() && !r->as_before,
          fmt("Ctrl+Up off, from the ground at 250: home by two jumps, the first Up at x %d (256: the run's inset "
              "from 224), walked to %d",
              r && r->jump ? r->jump->x : -1, r ? r->walk_x : -1));
    const std::vector<JumpPlan> legs = jump_legs_from(sp, sz, { 256, 899 }, no_ctrl);
    check(legs.size() == 1 && legs[0].kind == JumpKind::UpUp && legs[0].land_top == 720 && legs[0].x == 556 &&
              legs[0].run_l == 524 && legs[0].run_r == 676,
          fmt("its second leg: Up,Up at 556 from the run 524..676 onto 720 (%zu legs)", legs.size()));
    r = return_point(sp, sz, { 250, 999 }, &all);
    check(r && r->home && r->jumps == 1 && r->jump && r->jump->kind == JumpKind::CtrlUp && r->jump->x == 556,
          fmt("Ctrl+Up on: home by one jump, at 556 (%s at %d)", r && r->jump ? jump_kind_name(r->jump->kind) : "-",
              r && r->jump ? r->jump->x : -1));
    r = return_point(sp, sz, { 250, 999 });
    check(r && !r->home && r->jumps == 0 && !r->jump && r->at.x == 600 && r->at.y == 999 && !r->as_before,
          "without rules: 05's way -- the ground under the home, never a jump");

    // A monster on the top floor from the ground: one Ctrl + ↑, not the stairs.
    JumpNo why = JumpNo::None;
    int px = 0;
    std::optional<JumpPlan> j = plan_jump_to(sp, sz, { 250, 999 }, { 600, 719 }, all, &why, &px);
    check(j && j->kind == JumpKind::CtrlUp && j->x == 556 && j->land_top == 720 && j->walk_px == 306,
          fmt("a monster on the top floor: Ctrl+Up at %d, never two jumps for a target", j ? j->x : -1));
    j = plan_jump_to(sp, sz, { 250, 999 }, { 600, 719 }, no_ctrl, &why, &px);
    check(!j && why == JumpNo::Short && px == 280,
          fmt("Ctrl+Up off: none -- %s, 280 px up (%d)", jump_no_name(why), px));
    j = plan_jump_to(sp, sz, { 250, 999 }, { 300, 899 }, all, &why, &px);
    check(j && j->kind == JumpKind::Up && j->x == 256 && j->rise == 100,
          "a monster on the platform: a plain Up, the cheapest key");

    // Outside the box; above every usable rise.
    const HuntZone low_top = box_of(600, 899, 0, 800, 1500, 1100);
    j = plan_jump_to(sp, low_top, { 250, 999 }, { 600, 719 }, all, &why, &px);
    check(!j && why == JumpNo::Outside, "the monster above the box's top: Outside");
    Floors high(64, 40);
    high.flat(0, 2047, 1000, 2);
    high.flat(0, 2047, 400, 1);
    j = plan_jump_to(high.page("t90_s9"), box_of(500, 999, 0, 300, 2000, 1100), { 500, 999 }, { 500, 399 }, all, &why,
                     &px);
    check(!j && why == JumpNo::Short && px == 600, fmt("a floor 600 px up: Short, %d px", px));
    // A sloped floor in reach at its near end only: the monster 460 px up at
    // its far end -- Ctrl + Up onto the low end, then the walk up it (2026.11.05b:
    // the early Short measured the rise at the monster's column alone).
    Floors ramp(64, 40);
    ramp.flat(0, 2047, 1000, 2);
    ramp.run(200, 1200, [](int x) { return 640 - (x - 200) / 10; }, 1);
    const GameMapBook::Page rp = ramp.page("t90_s19");
    j = plan_jump_to(rp, box_of(500, 999, 0, 300, 2000, 1100), { 500, 999 }, { 1200, 539 }, all, &why, &px);
    check(j && j->kind == JumpKind::CtrlUp && j->rise <= jump_peak(JumpKind::CtrlUp).usable() &&
              rp.follow_to(j->land_x, j->land_top - 1, 1200).value_or(-1) == 540,
          fmt("a ramp 360..460 px up: Ctrl+Up at %d onto its low end, not Short (%s, %d)", j ? j->x : -1,
              jump_no_name(why), px));

    // Landing on another floor than the monster's: a ground piece wholly under
    // the top floor, the monster on the platform between.
    Floors under(64, 40);
    under.flat(520, 680, 1000, 1);
    under.flat(0, 2047, 1100, 2);
    under.flat(200, 1000, 900, 1);
    under.flat(500, 700, 720, 1);
    j = plan_jump_to(under.page("t90_s10"), box_of(600, 719, 0, 600, 1500, 1200), { 600, 999 }, { 600, 899 },
                     kinds_only(kCtrlUp), &why, &px);
    check(!j && why == JumpNo::NotTargetFloor && px == 100,
          fmt("Ctrl+Up alone under the top floor, the monster on the platform: NotTargetFloor (%s)",
              jump_no_name(why)));

    // Far: the landing's floor humps out of the box's top on its way to the
    // monster, and the jumps are all on the far side of it.
    Floors hump(64, 40);
    hump.flat(0, 450, 1000, 1);
    hump.flat(451, 2047, 1060, 2);
    hump.flat(100, 600, 700, 1);
    hump.run(601, 700, [](int x) { return 700 - (x - 600) * 11 / 10; }, 1);
    hump.flat(701, 800, 590, 1);
    hump.run(801, 900, [](int x) { return 590 + (x - 800) * 11 / 10; }, 1);
    hump.flat(901, 1300, 700, 1);
    const GameMapBook::Page fp = hump.page("t90_s11");
    const HuntZone fz = box_of(1000, 699, 0, 600, 1100, 1100);
    j = plan_jump_to(fp, fz, { 300, 999 }, { 1000, 699 }, all, &why, &px);
    check(!j && why == JumpNo::Far,
          fmt("the walk from the landing to it leaves the box's top: Far (%s)", jump_no_name(why)));
    JumpRules reach = all;
    reach.reach_px = 600;
    j = plan_jump_to(fp, fz, { 300, 999 }, { 1000, 699 }, reach, &why, &px);
    check(j && j->kind == JumpKind::CtrlUp && j->x == 400,
          fmt("within 600 px of reach across: Ctrl+Up at 400, the nearest in reach (%d)", j ? j->x : -1));
}

// ------------------------------------------------------------------ t52_s2

void test_t52s2() {
    section("Jump-up plan on t52_s2's own floors: the owner's box (home 880,274), the bottom at 810, the ways "
            "back (copies in build\\handoff\\drop\\testdata)");

    const std::string data = game_data();
    if (data.empty()) {
        skip("no copies of the game's map files here (build\\handoff\\drop\\testdata): jumps on t52_s2\n");
        return;
    }
    GameMapBook book(data);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t52_s2");
    check(page != nullptr, "t52_s2 reads");
    if (!page) return;
    const GameMapBook::Page& p = *page;
    const HuntZone owner = box_of(880, 274, 265, 174, 1399, 694);
    const HuntZone b810 = box_of(880, 274, 265, 174, 1399, 810);
    const JumpRules all;

    // The runs, against runs05b.py's -- 2026.11.12c's bracket (1900..2400)
    // and its column rule (the take-off's own slope no floor above, platforms
    // in the band inside the box accepted), re-derived by wbx_check5.py's
    // rules: Ctrl+Up from the ground 663..1034 (take-offs 695..1002; 663..916
    // until 11.12b, the ramp's own slope past 916 taken for a floor above)
    // and 992..1080 (1024..1048); Up,Up 1016..1034 and Ctrl+Up 496..504 narrow.
    const std::vector<JumpRun> runs = jump_runs(p, owner, all);
    const std::vector<JumpRun> use = usable_runs(runs);
    check(use.size() == 2 && use[0].kind == JumpKind::CtrlUp && use[1].kind == JumpKind::CtrlUp &&
              near(use[0].x0, 663, 4) && near(use[0].x1, 1034, 4) && near(use[0].take_l(), 695, 4) &&
              near(use[0].take_r(), 1002, 4) && near(use[1].x0, 992, 4) && near(use[1].x1, 1080, 4) &&
              near(use[1].take_l(), 1024, 4) && near(use[1].take_r(), 1048, 4) && use[0].land_y0 >= 272 &&
              use[0].land_y0 <= 276 && use[1].land_y0 >= 272 && use[1].land_y0 <= 276,
          "the owner's box: Ctrl+Up from the ground 663..1034 (take-offs 695..1002) and from the ramp's foot "
          "992..1080 (1024..1048), both onto the home ledge -- and nothing else usable: " +
              runs_text(runs, false));
    check(std::none_of(use.begin(), use.end(), [](const JumpRun& r) { return r.x0 < 659; }) &&
              std::any_of(runs.begin(), runs.end(),
                          [](const JumpRun& r) {
                              return r.kind == JumpKind::UpUp && !r.usable() && r.x0 <= 1034 && r.x1 >= 1016;
                          }),
          "nothing left of 663; Up,Up ground -> ramp foot about 1016..1034 too narrow -- every run: " +
              runs_text(runs, true));
    // 505..638: under ledges A and B, outside the box above: the ceiling guard
    // -- or ledge A's one column of attr 255, at 586,164, in the way up first.
    // Since 2026.11.12c the guard reaches only as high as each key does (Up
    // 245, Up,Up 413): a plain Up or Up,Up there falls short of everything.
    int judged = 0, ceiling = 0, blocked = 0, short_of = 0;
    std::string other;
    for (int x = 505; x <= 638; ++x)
        for (const MapFloor& f : p.floors_at(x)) {
            if (f.wall() || !owner.contains(x, f.y - 1)) continue;
            for (const JumpKind k : { JumpKind::Up, JumpKind::UpUp, JumpKind::CtrlUp }) {
                ++judged;
                const JumpColumn jc = judge_column(p, owner, { x, f.y - 1 }, k, all);
                if (jc.why == JumpNo::Ceiling) {
                    ++ceiling;
                } else if (jc.why == JumpNo::Blocked && jc.bad_x == 586 && jc.bad_top == 164) {
                    ++blocked;
                } else if (jc.why == JumpNo::Short && k != JumpKind::CtrlUp) {
                    ++short_of;
                } else if (other.size() < 200) {
                    other += fmt(" %s at %d,%d: %s;", jump_kind_name(k), x, f.y - 1, jump_no_name(jc.why));
                }
            }
        }
    check(judged > 0 && ceiling + blocked + short_of == judged && blocked == 28,
          fmt("no take-off of any key at 505..638, on the ground or the ledge: of %d, %d refused by the ceiling "
              "guard, %d by ledge A's attr-255 column at 586 (Blocked, first), %d Up or Up,Up short of everything",
              judged, ceiling, blocked, short_of) +
              other);
    // The ledge's 1-px hole at 711: a take-off under it lands on the column before.
    const JumpColumn under = judge_column(p, owner, { 711, stand_y(p, 711, 579) }, JumpKind::CtrlUp, all);
    check(under.ok() && under.land_x == 710 && p.on_floor({ under.land_x, under.land_top - 1 }),
          fmt("Ctrl+Up under the ledge's hole at 711: onto the ledge, at 710 (%s, %d)", jump_no_name(under.why),
              under.land_x));

    // Single columns.
    JumpColumn c = judge_column(p, owner, { 880, 634 }, JumpKind::CtrlUp, all);
    check(judge_column(p, owner, { 880, 634 }, JumpKind::Up, all).why == JumpNo::Short &&
              judge_column(p, owner, { 880, 634 }, JumpKind::UpUp, all).why == JumpNo::Short && c.ok() &&
              c.land_top == 275 && c.rise == 360 && c.headroom == 72,
          fmt("880,634 under the home: Up and Up,Up fall short; Ctrl+Up lands on floor %d, 360 up, headroom %d",
              c.land_top, c.headroom));
    c = judge_column(p, owner, { 600, 579 }, JumpKind::CtrlUp, all);
    check(c.why == JumpNo::Ceiling && c.bad_top < 174,
          fmt("600,579: Ctrl+Up refused by the ceiling -- floor %d at x %d, outside the box", c.bad_top, c.bad_x));

    // The take-off chosen (plan05b.py: 810 / 696 / -- its trail stops at the
    // ramp's 1-px hole at 1056, Page::follow walks over it). Since 2026.11.12c
    // Ctrl+Up's lowest peak is 432: 72 px of headroom under the ledge, no
    // headroom short of kJumpUpHeadroomWantPx -- where it stands.
    JumpNo why = JumpNo::None;
    int px = 0;
    const MapPoint home{ 880, 274 };
    std::optional<JumpPlan> j = plan_jump_to(p, owner, { 880, 634 }, home, all, &why, &px);
    check(j && j->kind == JumpKind::CtrlUp && j->x == 880 && j->land_top >= 272 && j->land_top <= 276 &&
              near(j->run_l, 663, 4) && near(j->run_r, 1034, 4) && j->headroom >= kJumpUpHeadroomWantPx,
          fmt("from 880,634 to the home: Ctrl+Up at %d, where it stands (headroom %d), feet %d, rise %d, run %d..%d",
              j ? j->x : -1, j ? j->headroom : -1, j ? j->feet_y : -1, j ? j->rise : -1, j ? j->run_l : -1,
              j ? j->run_r : -1));
    JumpRules flat_score;
    flat_score.headroom_weight = 0;
    j = plan_jump_to(p, owner, { 880, 634 }, home, flat_score);
    check(j && j->x == 880 && j->headroom >= kJumpUpHeadroomWantPx,
          fmt("headroom not weighed: 880 itself as well (%d, headroom %d) -- nothing short to weigh", j ? j->x : -1,
              j ? j->headroom : -1));
    const MapPoint ledge_mob{ 700, stand_y(p, 700, 273) };
    j = plan_jump_to(p, owner, { 600, 579 }, ledge_mob, all, &why, &px);
    check(j && j->kind == JumpKind::CtrlUp && near(j->x, 696, 4) && j->walk_px == j->x - 600,
          fmt("from 600,579 to a monster at 700,%d on the ledge: Ctrl+Up at %d (prototype 696), walking right",
              ledge_mob.y, j ? j->x : -1));
    j = plan_jump_to(p, owner, { 1030, 563 }, home, all);
    check(j && j->kind == JumpKind::CtrlUp && j->x == 1030 && near(j->run_l, 992, 4) && near(j->run_r, 1080, 4),
          fmt("from the ramp's foot at 1030,563: Ctrl+Up where it stands (%d, headroom %d)", j ? j->x : -1,
              j ? j->headroom : -1));

    // Learned, and a gate band brought back.
    JumpRules learned;
    learned.refused = [](MapPoint take, int, JumpKind) {
        return std::abs(take.x - 810) <= 48 && std::abs(take.y - 603) <= 24 ? JumpNo::Learned : JumpNo::None;
    };
    j = plan_jump_to(p, owner, { 880, 634 }, home, learned);
    check(j && std::abs(j->x - 810) >= 48,
          fmt("a fall-back learned at 810,603: the take-off %d px away, at %d", j ? std::abs(j->x - 810) : -1,
              j ? j->x : -1));
    const std::vector<MapGate> gate{ gate_at(810, 610) };
    JumpRules banded;
    banded.gates = gate;
    banded.gate_px = 150;
    check(judge_column(p, owner, { 810, 603 }, JumpKind::CtrlUp, banded).why == JumpNo::Gate &&
              judge_column(p, owner, { 810, 603 }, JumpKind::CtrlUp, all).ok() &&
              judge_column(p, owner, { 810, 603 }, JumpKind::Up, banded).why != JumpNo::Gate,
          "a gate at 810,610 with the band brought back: Ctrl+Up there refused (Gate); without it, as the owner "
          "answered, it goes; Up is never gate-checked");

    // A monster on the ramp's foot from the ground under it: Up,Up's run too narrow.
    const MapPoint ground{ 1024, stand_y(p, 1024, 690) };
    const MapPoint foot{ 1025, stand_y(p, 1025, 565) };
    j = plan_jump_to(p, owner, ground, foot, all, &why, &px);
    check(!j && why == JumpNo::Narrow,
          fmt("from the ground at %d,%d to the ramp's foot at %d,%d: none -- %s, %d px up", ground.x, ground.y, foot.x,
              foot.y, jump_no_name(why), px));

    // The way back with jumps.
    std::optional<ZoneReturn> r = return_point(p, owner, { 880, 634 }, &all);
    check(r && r->home && r->jumps == 1 && r->falls == 0 && r->jump && r->jump->kind == JumpKind::CtrlUp &&
              r->jump->x == 880 && r->walk_x == r->jump->x && !r->own_floor(),
          fmt("from 880,634 (04d's walk back ended there): home by one Ctrl+Up at %d, where it stands",
              r && r->jump ? r->jump->x : -1));
    const auto t0 = std::chrono::steady_clock::now();
    r = return_point(p, owner, { 1402, 802 }, &all);
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    // The nearest take-off of the run 695..1002 the walk back into the box
    // gets to (Closer): about 895 (2026.11.12c; 810 until 11.12b).
    check(r && r->home && r->jumps == 1 && r->jump && r->jump->kind == JumpKind::CtrlUp && near(r->jump->x, 895, 12) &&
              r->walk_x == r->jump->x && r->jump->walk_px == 1402 - r->jump->x,
          fmt("from 1402,802, 108 px below the box: left along the ground, then Ctrl+Up at %d (about 895)",
              r && r->jump ? r->jump->x : -1));
    check(ms < 1500, fmt("that way worked out in %lld ms", ms));
    r = return_point(p, owner, { 300, 531 }, &all);
    check(r && r->home && r->jumps == 1 && r->jump && r->jump->kind == JumpKind::CtrlUp && near(r->jump->x, 696, 4),
          fmt("from 300,531: Ctrl+Up at %d (prototype 696)", r && r->jump ? r->jump->x : -1));
    r = return_point(p, owner, { 1030, 563 }, &all);
    check(r && r->home && r->jumps == 1 && r->jump && r->jump->x == 1030 && r->walk_x == 1030,
          "from the ramp's foot at 1030,563: Ctrl+Up where it stands");
    r = return_point(p, owner, { 880, 634 });
    check(r && !r->home && r->at.x == 880 && r->at.y == 634 && r->own_floor() && r->jumps == 0 && !r->jump,
          "without rules, 04d's: 880,634 on its own floor");
    r = return_point(p, owner, { 1030, 563 });
    check(r && !r->home && r->at.x == 992 && r->at.y == 587 && r->own_floor(),
          "and from 1030,563 the ramp's foot, 992,587");

    // The bottom moved to 810: ground -> ramp as well -- since 2026.11.12c by
    // Up,Up from 1016..1118 (the ramp's own slope no floor above) and by
    // Ctrl+Up from 1038..1375 (1129..1320 until 11.12b).
    const std::vector<JumpRun> runs810 = jump_runs(p, b810, all);
    const std::vector<JumpRun> use810 = usable_runs(runs810);
    check(use810.size() == 4 && use810[0].kind == JumpKind::UpUp && near(use810[0].x0, 1016, 4) &&
              near(use810[0].x1, 1118, 4) && use810[3].kind == JumpKind::CtrlUp && near(use810[1].x0, 663, 4) &&
              near(use810[2].x0, 992, 4) && near(use810[3].x0, 1038, 4) && near(use810[3].x1, 1375, 4) &&
              near(use810[3].take_l(), 1070, 4) && near(use810[3].take_r(), 1343, 4),
          "the bottom at 810: also Up,Up ground -> ramp 1016..1118 and Ctrl+Up ground -> ramp 1038..1375 "
          "(take-offs 1070..1343): " +
              runs_text(runs810, false));
    check(std::count_if(use810.begin(), use810.end(), [](const JumpRun& x) { return x.kind == JumpKind::UpUp; }) == 1 &&
              std::none_of(use810.begin(), use810.end(), [](const JumpRun& x) { return x.kind == JumpKind::Up; }),
          "one Up,Up run usable, no plain Up -- every run: " + runs_text(runs810, true));
}

// ------------------------------------------------------------------ t53_s2

void test_t53s2() {
    section("Jump-up plan on t53_s2's own floors: the owner's box -- no jump of any kind, the top at 1020 or not");

    const std::string data = game_data();
    if (data.empty()) {
        skip("no copies of the game's map files here (build\\handoff\\drop\\testdata): jumps on t53_s2\n");
        return;
    }
    GameMapBook book(data);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t53_s2");
    check(page != nullptr, "t53_s2 reads");
    if (!page) return;
    const GameMapBook::Page& p = *page;
    const HuntZone owner = box_of(2635, 1188, 2335, 1088, 2935, 1288);
    const HuntZone raised = box_of(2635, 1188, 2335, 1020, 2935, 1288);
    const JumpRules all;
    const std::vector<JumpRun> runs = jump_runs(p, owner, all);
    const std::vector<JumpRun> raised_runs = jump_runs(p, raised, all);
    // 2026.11.12c: in the owner's box still none (the owner's Q3 (a): an Up
    // that could land above the box's top is refused); with the top raised to
    // 1020, a plain Up 2359..2441 onto the floor at about 1150 (Up's lowest
    // peak 133 since 11.12c).
    const std::vector<JumpRun> raised_use = usable_runs(raised_runs);
    check(usable_runs(runs).empty() && raised_use.size() == 1 && raised_use[0].kind == JumpKind::Up &&
              near(raised_use[0].x0, 2359, 4) && near(raised_use[0].x1, 2441, 4),
          "no usable run of any key in the owner's box: " + runs_text(runs, true) +
              " / top at 1020, one Up: " + runs_text(raised_runs, true));
    // The way back is 04d's wherever it is the map's.
    bool same = true;
    std::string seen;
    for (const MapPoint at : { MapPoint{ 2635, 1188 }, MapPoint{ 2400, stand_y(p, 2400, 1188) },
                               MapPoint{ 2900, stand_y(p, 2900, 1188) }, MapPoint{ 2400, stand_y(p, 2400, 1150) } }) {
        const std::optional<ZoneReturn> a = return_point(p, owner, at);
        const std::optional<ZoneReturn> b = return_point(p, owner, at, &all);
        const bool one = a && b && !a->as_before && a->at.x == b->at.x && a->at.y == b->at.y && a->home == b->home &&
                         a->falls == b->falls && a->walk_x == b->walk_x && b->jumps == 0 && !b->jump;
        same = same && one;
        seen += fmt(" %d,%d -> %d,%d%s;", at.x, at.y, b ? b->at.x : -1, b ? b->at.y : -1, b && b->home ? " home" : "");
    }
    check(same, "with jumps the way back is 04d's:" + seen);
}

// ------------------------------------------------------- Thử's floor above

/// What "Thử" of a jump's kind is refused by (JumpCheck::run_kind): any floor
/// the map shows within kJumpUpCeilingPx over the feet, at the column and 24
/// px either way -- never the character's own floor up a slope.
void test_floor_above() {
    section("Jump-up plan: the floor above a Thử (floor_above) -- 650 px up, 24 px either way, slopes not counted");

    Floors f(40, 40);
    f.flat(0, 1279, 1000, 2);                              // the ground
    f.flat(400, 500, 700, 1);                              // a ledge 300 up
    f.flat(800, 900, 300, 1);                              // one 700 up
    f.run(1001, 1200, [](int x) { return 2000 - x; }, 1);  // a ramp up off the ground: 1 px a column
    const GameMapBook::Page p = f.page("t_above");
    const std::optional<int> under = floor_above(p, { 450, 999 });
    const std::optional<int> edge = floor_above(p, { 377, 999 });
    const std::optional<int> clear = floor_above(p, { 375, 999 });
    const std::optional<int> high = floor_above(p, { 850, 999 });
    const std::optional<int> wider = floor_above(p, { 850, 999 }, 750);
    const std::optional<int> ramp = floor_above(p, { 1100, 899 });
    const std::optional<int> air = floor_above(p, { 300, 800 });
    check(under && *under == 300 && edge && *edge == 300 && !clear && !high && wider && *wider == 700 && !ramp && !air,
          fmt("under the ledge 300 (%d); 23 px from its end 300 (%d), 25 px from it none (%d); 700 up none, 700 "
              "within 750; up a ramp its own rise none; off every floor none",
              under ? *under : -1, edge ? *edge : -1, clear ? *clear : -1));

    const std::string data = game_data();
    if (data.empty()) {
        skip("no copies of the game's map files here (build\\handoff\\drop\\testdata): Thử's floor above on t52_s2\n");
        return;
    }
    GameMapBook book(data);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t52_s2");
    if (!page) {
        check(false, "t52_s2 reads");
        return;
    }
    const std::optional<int> below_home = floor_above(*page, { 880, 634 });
    const std::optional<int> home = floor_above(*page, { 880, 274 });
    const std::optional<int> left = floor_above(*page, { 40, stand_y(*page, 40, 571) });
    const std::optional<int> bottom = floor_above(*page, { 880, stand_y(*page, 880, 992) });
    check(below_home && *below_home >= 355 && *below_home <= 365 && !home && !left && bottom && *bottom < 400,
          fmt("t52_s2: under the home ledge at 880,634 a floor %d above (Thử refused there); on the ledge at the "
              "home, and on the far left platform at 40, none (Thử goes); on the bottom ground at 880, the slope "
              "%d above",
              below_home ? *below_home : -1, bottom ? *bottom : -1));
}

// ------------------------------------------------------------ 2026.11.12c

/// t23_s14's bottom stack, made up as the game's file has it at x 1500..1950:
/// the bottom floor 1811 at 1757 (a platform, rising 0.25 px a column to the
/// left), platforms at 1468, 1425, 1393 and 1263, the ground 1864 under it all.
Floors t23_stack() {
    Floors f(100, 70);
    f.flat(0, 3199, 1864, 2);
    f.run(1500, 1950, [](int x) { return 1811 - (1757 - x) / 4; }, 1);
    f.flat(1604, 1797, 1468, 1);
    f.flat(1650, 1900, 1425, 1);
    f.flat(826, 1900, 1393, 1);
    f.flat(1500, 1900, 1263, 1);
    return f;
}

void test_band() {
    section("Jump-up plan 2026.11.12c: the uncertain band accepted inside the box, the take-off's own slope no floor "
            "above, the ceiling per key -- t23_s14's bottom, made up");
    const GameMapBook::Page p = t23_stack().page("t87_s1");
    const HuntZone owner = box_of(1593, 1262, 740, 1162, 2858, 1586);
    const HuntZone whole = zone_whole_map(p, owner);
    const JumpRules all;

    JumpColumn c = judge_column(p, whole, { 1757, 1810 }, JumpKind::CtrlUp, all);
    check(c.ok() && c.land_top == 1393 && c.band_top == 1263,
          fmt("Ctrl+Up from the bottom at 1757,1810 over the map: onto 1393 for sure, 1263 (548 up) in its band and a "
              "platform inside -- accepted (%s, land %d, band %d)",
              jump_no_name(c.why), c.land_top, c.band_top));
    JumpRules no_top;
    no_top.ceiling_px = 0;
    c = judge_column(p, box_of(1593, 1400, 740, 1300, 2858, 1900), { 1757, 1810 }, JumpKind::CtrlUp, no_top, 0);
    check(c.why == JumpNo::Ambiguous && c.bad_top == 1263,
          fmt("the box's top under 1263 (no ceiling guard, a test): a band floor outside it -- Ambiguous (%s, %d)",
              jump_no_name(c.why), c.bad_top));
    Floors solid = t23_stack();
    solid.hole(1500, 1900, 1263);
    solid.flat(1500, 1900, 1263, 2);
    c = judge_column(solid.page("t87_s2"), whole, { 1757, 1810 }, JumpKind::CtrlUp, all);
    check(c.why == JumpNo::Blocked && c.bad_top == 1263, fmt("1263 solid ground: Blocked (%s)", jump_no_name(c.why)));
    c = judge_column(p, owner, { 1757, 1810 }, JumpKind::CtrlUp, all, 224);
    check(c.ok() && c.land_top == 1393,
          fmt("in the owner's box, 224 px under it: its own slope (1805 at x 1733, outside the box) no floor above -- "
              "not Ceiling (%s)",
              jump_no_name(c.why)));
    // The ceiling per key: a floor outside the box 300 px up is out of a plain
    // Up's reach; Ctrl+Up's 650 px guard keeps it.
    Floors high(64, 40);
    high.flat(0, 2047, 1000, 2);
    high.flat(300, 700, 900, 1);
    high.flat(300, 700, 700, 1);
    const GameMapBook::Page hp = high.page("t87_s3");
    const HuntZone low_top = box_of(500, 899, 0, 800, 1500, 1100);
    check(judge_column(hp, low_top, { 500, 999 }, JumpKind::Up, all).ok() &&
              judge_column(hp, low_top, { 500, 999 }, JumpKind::CtrlUp, all).why == JumpNo::Ceiling,
          "a floor outside the box 300 px up: Up goes (out of its reach, 245); Ctrl+Up refused by the ceiling");

    // The way back from the bottom: Ctrl+Up near where it stands onto 1393,
    // then Up,Up onto the home's floor -- what the owner did by hand.
    std::optional<ZoneReturn> r = return_point(p, owner, { 1757, 1810 }, &all);
    check(r && r->home && r->jumps == 2 && r->jump && r->jump->kind == JumpKind::CtrlUp && r->jump->x >= 1500 &&
              r->jump->x <= 1760 && near(r->jump->land_top, 1393, 4) && r->jump->band_top == 1263,
          fmt("from 1757,1810: home by %d jumps, the first %s at x %d onto %d (band to %d)", r ? r->jumps : -1,
              r && r->jump ? jump_kind_name(r->jump->kind) : "-", r && r->jump ? r->jump->x : -1,
              r && r->jump ? r->jump->land_top : -1, r && r->jump ? r->jump->band_top : -1));
    // The area taken when the character is under it (the owner's Q5 of
    // 2026.11.12c): the relog of 23:47:34 at 1757,1810 -- 224 px under the box.
    check(kZoneTakeFallen && kZoneTakeBelowPx == 600 &&
              zone_take(owner, 1757.0f, 1810.0f, false, false) == ZoneTake::Below &&
              zone_take(owner, 2300.0f, 2300.0f, false, false) == ZoneTake::Wait &&
              zone_take(box_of(495, 632, 339, 532, 705, 700), 64.0f, 620.0f, false, false) == ZoneTake::Wait &&
              std::string(zone_take_name(ZoneTake::Below)) == "under it",
          "zone_take: 224 px under t23_s14's box -- taken (\"under it\"); 714 px under, or 275 px beside t1901_s2's "
          "-- waits");
    const ZoneDropRules down{ {}, 150, {} };
    r = return_point(p, owner, { 1826, stand_y(p, 1826, 1863) }, &all);
    if (!r) r = return_point(p, owner, { 1826, stand_y(p, 1826, 1863) }, &all, &whole, &down);
    check(r && r->jump && r->jumps >= 2,
          fmt("from the ground under it at 1826: a way back, its first leg a jump (%s, %d jumps)",
              r && r->jump ? jump_kind_name(r->jump->kind) : "-", r ? r->jumps : -1));
}

/// t88_s1's floors, made up: a ledge of solid ground at 936 to x 1299, the
/// ground at 1000 from 1300 -- a step of 64 px, as t49_s5's at 2816 -- and a
/// platform at 584 over 1200..1500, outside the box (as t49_s5's 1287).
Floors t88_floors() {
    Floors f(80, 40);
    f.flat(0, 1299, 936, 2);
    f.flat(1300, 2559, 1000, 2);
    f.flat(1200, 1500, 584, 1);
    return f;
}

void test_wall_foot() {
    section("Jump-up plan 2026.11.12c: a step of the ground is a wall, not a pit; Ctrl+Up at its foot onto the step "
            "(the owner's \"Chỉ Ctrl+↑ tại chỗ\"); a plain Up past the upper bar's landings");
    const GameMapBook::Page p = t88_floors().page("t88_s1");
    // The step: followed left from the ground it stops at the foot, never a pit.
    MapTrail t = p.follow(1500, 999, -1);
    check(t.step && t.stop && !t.ledge && !t.pts.empty() && t.pts.back().x == 1300 && t.step_x == 1299 &&
              t.step_top == 936,
          fmt("follow left from 1500,999: a step at %d (top %d), stopped at x %d -- not a pit", t.step_x, t.step_top,
              t.pts.empty() ? -1 : t.pts.back().x));
    t = p.follow(1200, 935, 1);
    check(t.ledge && !t.step, "the ledge followed right: an ordinary walk-off onto the ground");
    Floors plat = t88_floors();
    plat.hole(0, 1299, 936);
    plat.flat(0, 1299, 936, 1);
    Floors tall = t88_floors();
    tall.hole(0, 1299, 936);
    tall.flat(0, 1299, 700, 2);
    Floors block = t88_floors();
    block.hole(0, 1299, 936);
    block.flat(0, 1299, 936, 5);
    // A 2-px hole before the step -- no platform over it (the first column on
    // with any floor is what is looked at).
    Floors holed(80, 40);
    holed.flat(0, 1299, 936, 2);
    holed.flat(1302, 2559, 1000, 2);
    check(plat.page("t88_s2").follow(1500, 999, -1).ledge && tall.page("t88_s3").follow(1500, 999, -1).ledge &&
              block.page("t88_s4").follow(1500, 999, -1).ledge && holed.page("t88_s5").follow(1500, 999, -1).step,
          "a platform past the end, a step 300 px up, attr 5: a pit as before; a 2-px hole before the step: a step");
    check(p.on_step_lip({ 1301, 935 }) && !p.on_step_lip({ 1306, 935 }) && !p.on_step_lip({ 1301, 999 }),
          "the step's lip: 1301,935 on it, 1306,935 and 1301,999 not");

    const HuntZone box = box_of(1800, 999, 400, 650, 2400, 1050);
    const JumpRules all;
    std::vector<JumpPlan> w = wall_foot_jumps(p, box, { 1800, 999 }, all);
    check(w.size() == 1 && w[0].wall && w[0].kind == JumpKind::CtrlUp && w[0].x == 1300 && w[0].land_x == 1299 &&
              w[0].land_top == 936 && w[0].rise == 64,
          fmt("from 1800,999: one wall-foot Ctrl+Up at x %d onto %d/%d, rise %d", w.empty() ? -1 : w[0].x,
              w.empty() ? -1 : w[0].land_x, w.empty() ? -1 : w[0].land_top, w.empty() ? -1 : w[0].rise));
    const std::vector<JumpPlan> legs = jump_legs_from(p, box, { 1800, 999 }, all);
    check(std::any_of(legs.begin(), legs.end(), [](const JumpPlan& j) { return j.wall && j.land_top == 936; }) &&
              std::none_of(legs.begin(), legs.end(),
                           [](const JumpPlan& j) { return !j.wall && j.land_top == 936; }),
          "jump_legs_from has it -- and no Up or Up,Up onto the ledge");
    JumpRules reach = all;
    reach.reach_px = 400;
    const std::optional<JumpPlan> to = plan_jump_to(p, box, { 1800, 999 }, { 1100, 935 }, reach);
    check(to && to->wall && to->x == 1300, "a monster on the ledge at 1100: the wall-foot Ctrl+Up");
    check(wall_foot_jumps(p, box, { 1800, 999 }, kinds_only(kUp | kUpUp)).empty() &&
              judge_wall_foot(p, box_of(1800, 999, 1250, 650, 2400, 1050), { 1300, 999 }, -1, all).why == JumpNo::Edge &&
              judge_wall_foot(p, box_of(1800, 999, 400, 940, 2400, 1050), { 1300, 999 }, -1, all).why ==
                  JumpNo::Outside,
          "none without Ctrl+Up; the box's left at 1250 (the ledge runs out of it): Edge; its top at 940: Outside");
    JumpRules learned;
    learned.refused = [](MapPoint take, int, JumpKind) {
        return std::abs(take.x - 1301) <= 48 ? JumpNo::Learned : JumpNo::None;
    };
    check(wall_foot_jumps(p, box, { 1800, 999 }, learned).empty() &&
              judge_wall_foot(p, box, { 1301, 999 }, -1, all).ok() &&
              judge_wall_foot(p, box, { 1306, 999 }, -1, all).why == JumpNo::Foot,
          "learned at 1301: none; judged at 1301 it goes, at 1306 not at the foot (Foot)");
    Floors roof = t88_floors();
    roof.flat(1300, 1320, 940, 2);
    check(judge_wall_foot(roof.page("t88_s6"), box, { 1300, 999 }, -1, all).why == JumpNo::Blocked,
          "solid ground 60 px over the foot: Blocked");
    JumpRules no_wall = all;
    no_wall.wall_foot = false;
    check(wall_foot_jumps(p, box, { 1800, 999 }, no_wall).empty(), "wall-foot jumps off for the run: none");
    const HuntZone ledge_home = box_of(1100, 935, 400, 650, 2400, 1050);
    const std::optional<ZoneReturn> r = return_point(p, ledge_home, { 1800, 999 }, &all);
    check(r && r->home && r->jumps == 1 && r->jump && r->jump->wall && r->walk_x == 1300,
          fmt("the home on the ledge, from 1800,999: one wall-foot jump, walked to x %d", r ? r->walk_x : -1));

    // The plain Up past the upper bar: where it may come down.
    check(bar_up_landings(p, box, { 1310, 999 }, all).ok() && bar_up_landings(p, box, { 1301, 999 }, all).ok(),
          "Up at the step's foot: the ledge solid -- a bump, no landing -- ok");
    Floors plats(64, 40);
    plats.flat(0, 2047, 1000, 2);
    plats.flat(300, 700, 932, 1);
    plats.flat(300, 700, 876, 1);
    const GameMapBook::Page pp = plats.page("t87_s4");
    const HuntZone pbox = box_of(500, 999, 0, 800, 1500, 1100);
    const JumpColumn b1 = bar_up_landings(pp, pbox, { 500, 999 }, all);
    const JumpColumn b2 = bar_up_landings(pp, box_of(500, 999, 0, 900, 1500, 1100), { 500, 999 }, all);
    check(b1.ok() && b1.land_top == 1000 && b2.why == JumpNo::Outside && b2.bad_top == 876,
          fmt("platforms 68 and 124 px up inside the box: ok; the box's top under the upper one: Outside at %d",
              b2.bad_top));
    Floors head(64, 40);
    head.flat(0, 2047, 1000, 2);
    head.flat(300, 700, 950, 2);
    JumpRules up_learned;
    up_learned.refused = [](MapPoint take, int, JumpKind k) {
        return k == JumpKind::Up && std::abs(take.x - 500) <= 48 ? JumpNo::Learned : JumpNo::None;
    };
    check(bar_up_landings(head.page("t87_s5"), pbox, { 500, 999 }, all).ok() &&
              bar_up_landings(pp, pbox, { 520, 999 }, up_learned).why == JumpNo::Learned,
          "solid ground 50 px up: a bump, ok; a learned no-Up within 48 px: Learned");
}

/// t49_s5's own floors (copies in build\handoff\drop\testdata): its step at
/// 2816, and the wall-foot jump the owner makes there.
void test_t49s5() {
    section("Jump-up plan 2026.11.12c on t49_s5's own floors: the step at 2816 a wall, Ctrl+Up at its foot onto 1639");
    const std::string data = game_data();
    if (data.empty() || !path_exists(data + "\\Map\\t49_s5.map")) {
        skip("no copy of the game's t49_s5.map here (build\\handoff\\drop\\testdata): the step at 2816\n");
        return;
    }
    GameMapBook book(data);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t49_s5");
    check(page != nullptr, "t49_s5 reads");
    if (!page) return;
    const GameMapBook::Page& p = *page;
    const HuntZone owner = box_of(2836, 1702, 1985, 1455, 3720, 1846);
    const MapTrail t = p.follow(2836, 1702, -1);
    check(t.step && t.step_top == 1639 && !t.pts.empty() && t.pts.back().x == 2816,
          fmt("left from 2836,1702: a step at the foot x %d, its top %d", t.pts.empty() ? -1 : t.pts.back().x,
              t.step_top));
    check(walk_stays_in(p, owner, { 2836, 1702 }, -1, 96).ok && p.follow(2800, 1638, 1).ledge,
          "the walk left toward it stays in the area; the ledge followed right is a walk-off");
    const std::vector<JumpPlan> legs = jump_legs_from(p, owner, { 2836, 1702 }, JumpRules{});
    check(std::any_of(legs.begin(), legs.end(),
                      [](const JumpPlan& j) {
                          return j.wall && j.x == 2816 && j.land_x == 2815 && j.land_top == 1639;
                      }),
          fmt("from 2836,1702: the wall-foot Ctrl+Up at 2816 onto 1639 (%zu legs)", legs.size()));
}

} // namespace

void test_jump_plan(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_model();
    test_columns();
    test_plans();
    test_t52s2();
    test_t53s2();
    test_floor_above();
    test_band();
    test_wall_foot();
    test_t49s5();
}
