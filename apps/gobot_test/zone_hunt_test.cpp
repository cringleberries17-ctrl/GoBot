// gobot_test -- the hunter in the map tab's hunting area against the game's
// own floors (2026.11.04d), replayed on t52_s2 from the owner's first live
// hunt with a zone, 2026-11-04 20:39-20:44: a walk kept in the box (the
// owner's Q1), the way back into it (Q2), monsters above passed over (Q3),
// the stop above a target below (Q6), and the drop on t52_s2's slopes.
//
// Over copies of the game's own map files in build\handoff\drop\testdata --
// never the game's install, never the registry -- and skipped when they are
// not there. The hunter on a pretend game laid one to one over the packets'
// world pixels: the character walks along the real floors, falls off their
// ends, and is reported where the client reports it. Nothing here presses a
// real key: every key goes to the pretend game and is written down.

#include "gobot/common.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/hunter.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/map_book.hpp"
#include "gobot/mob_detector.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
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

using Why = Hunter::DropWhy;

/// The game's files copied for the tests, or "" -- never the game's install.
std::string game_data() {
#ifdef GOBOT_SOURCE_DIR
    const std::string dir = std::string(GOBOT_SOURCE_DIR) + "\\build\\handoff\\drop\\testdata\\data";
    if (path_exists(dir + "\\Map\\t52_s2.map") && path_exists(dir + "\\Project\\t52_s2.prj")) return dir;
#endif
    return {};
}

std::vector<std::string> info_lines(const std::function<void()>& run) {
    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_ring_capacity(4000);
    log.clear_ring();
    log.set_level(LogLevel::Info);
    run();
    std::vector<std::string> out;
    for (const LogLine& l : log.ring()) out.push_back(l.text);
    log.set_level(was);
    log.set_ring_capacity(0);
    return out;
}

int lines_with(const std::vector<std::string>& all, const char* a, const char* b = "") {
    int n = 0;
    for (const std::string& l : all)
        if (l.find(a) != std::string::npos && l.find(b) != std::string::npos) ++n;
    return n;
}

std::string line_with(const std::vector<std::string>& all, const char* a) {
    for (const std::string& l : all)
        if (l.find(a) != std::string::npos) return l;
    return {};
}

/// A plate from the packets standing at x, y.
Nameplate mob_at(int x, int y, int slot) {
    Nameplate n;
    n.bar = ScreenRect{ x - 20, y, 40, 3 };
    n.plate = ScreenRect{ x - 30, y - 14, 60, 12 };
    n.hp = 1.0f;
    n.slot = slot;
    n.gen = 1;
    n.actor_checked = true;
    return n;
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

// ------------------------------------------------------------- the rig

/// The pretend game over t52_s2's own floors, in the packets' world pixels:
/// the plates laid one to one over them.
struct ZoneWorld {
    std::uint64_t t = 5'000'000;
    bool          focused = true;
    const GameMapBook::Page* page = nullptr;
    int           me_x = 880, me_y = 273;   ///< a standing point: its floor's top - 1
    std::vector<Nameplate> mobs;
    int           step_px = 20;             ///< walked along the floor per pass with a direction held
    int           falls = 0;                ///< off a floor's end, onto another
    int           max_x = 0, min_x = INT_MAX, max_y = 0;
    bool          wall_left = false;        ///< a wall: ← held walks nowhere
    /// The game's floor going on where the file's ends: walked on past the
    /// end at its height, no fall and no report.
    bool          floor_goes_on = false;

    std::set<std::uint32_t> held;
    struct Key {
        std::uint64_t at;
        std::uint32_t vk;
        int           hold;                 ///< -1: put down and left down
    };
    std::vector<Key> keys;

    // what the packets say: a walk's worked-out x moves, its height stays the
    // last report's, as PacketWorld follows one
    float         report_x = 0.0f, report_y = 0.0f;
    std::uint64_t report_ms = 0;
    std::uint32_t epoch = 7;
    std::uint64_t takeoffs = 40;

    // the drop's key
    int           drop_calls = 0;
    std::vector<std::uint64_t> drops;
    int           land_dx = 0;              ///< where a drop comes down, across from where it fell
    bool          falling = false;
    std::uint64_t land_at = 0;
    int           land_x = 0, land_y = 0;
    std::uint64_t landed_at = 0;

    void report() {
        report_x = static_cast<float>(me_x);
        report_y = static_cast<float>(me_y);
        report_ms = t;
    }
    void place(int x, int y) {
        me_x = x;
        me_y = y;
        report();
        note();
    }
    void note() {
        max_x = std::max(max_x, me_x);
        min_x = std::min(min_x, me_x);
        max_y = std::max(max_y, me_y);
    }
    bool walking() const { return held.count(VK_LEFT) != 0 || held.count(VK_RIGHT) != 0; }
    int downs_of(std::uint32_t vk, std::uint64_t from = 0, std::uint64_t to = UINT64_MAX) const {
        int n = 0;
        for (const Key& k : keys)
            if (k.vk == vk && k.at >= from && k.at <= to) ++n;
        return n;
    }
    /// One pass's walk: along the floor it stands on -- its slope, and across
    /// a hole -- or off the floor's end, falling at a walk's speed onto
    /// whatever is under it, a report as it lands.
    void walk_on() {
        if (!walking() || falling || page == nullptr) return;
        const int dir = held.count(VK_LEFT) != 0 ? -1 : 1;
        if (dir < 0 && wall_left) return;
        const int to = me_x + dir * step_px;
        if (const std::optional<int> top = page->follow_to(me_x, me_y, to)) {
            me_x = to;
            me_y = *top - 1;
        } else if (floor_goes_on) {
            me_x = to;
        } else {
            const MapTrail tr = page->follow(me_x, me_y, dir, step_px);
            if (tr.pts.empty()) return;
            const MapPoint end = tr.pts.back();
            if (tr.stop) {
                me_x = end.x;
                me_y = end.y - 1;
            } else if (const std::optional<MapPoint> land = walk_off_landing(*page, end, dir, kZoneWalkPxPerS)) {
                me_x = land->x;
                me_y = land->y;
                ++falls;
                report();
            }
        }
        note();
    }
};

/// A hunter on the pretend game, over the copies of the game's files,
/// started without a thread: every seam its own.
struct ZoneRig {
    InputManager         input;
    ExecutionCoordinator coord;
    MobDetector          detector;
    Hunter               hunter{ input, coord, detector };
    GameMapBook          book;
    ZoneWorld            w;

    ZoneRig(const HunterConfig& cfg, int x, int y) : book(game_data()) {
        const std::shared_ptr<const GameMapBook::Page> page = book.get("t52_s2");
        w.page = page.get();
        w.place(x, y);
        Hunter::Eyes eyes;
        eyes.view = [] { return 1u; };
        eyes.where = [this](WorldFix& f) {
            f = WorldFix{};
            f.state = WorldFix::State::Known;
            f.x = static_cast<float>(w.me_x);
            f.y = w.walking() ? w.report_y : static_cast<float>(w.me_y);
            f.rx = w.report_x;
            f.ry = w.report_y;
            f.report_ms = w.report_ms;
            f.airborne = w.falling;
            f.moving = w.walking();
            f.map_known = true;
            f.map = 52;
            f.area = 2;
            f.epoch = w.epoch;
            f.entry_x = 880.0f;     // the map entry at the 피스존 gate's spawn, as at 20:39:04
            f.entry_y = 274.0f;
            return true;
        };
        eyes.player = [this](Nameplate& out, bool* fresh) {
            if (fresh) *fresh = true;
            out = mob_at(w.me_x, w.me_y, -1);
            out.is_player = true;
            out.is_self = true;
            return true;
        };
        eyes.mobs = [this] {
            std::vector<Nameplate> out = w.mobs;
            MobDetector::nearest_first(out, w.me_x, w.me_y);
            return out;
        };
        eyes.takeoffs = [this]() -> std::optional<std::uint64_t> { return w.takeoffs; };
        // The whole map's list the seek goes by (2026.11.05b): each plate's
        // point, on the floor under it -- what PacketEyes::world lays out.
        eyes.world = [this](std::vector<SeekMob>& out) {
            out.clear();
            for (const Nameplate& n : w.mobs) {
                SeekMob s;
                s.slot = n.slot;
                s.gen = n.gen;
                s.at = { n.x(), n.y() };
                const std::optional<MapFloor> on = w.page->floor_near(n.x(), n.y());
                const std::optional<int> top =
                    on && !on->wall() ? std::optional<int>(on->y) : Hunter::monster_floor(*w.page, n.x(), n.y());
                s.floor_y = top ? *top : -1;
                out.push_back(s);
            }
            return true;
        };
        eyes.stands = [this] {
            std::vector<MapTrust::Mob> out;
            for (const Nameplate& n : w.mobs)
                if (n.slot >= 0)
                    out.push_back(MapTrust::Mob{ MapPoint{ n.x(), n.y() }, static_cast<std::uint32_t>(n.slot),
                                                 w.t - 400, n.gen });
            return out;
        };
        hunter.set_eyes(eyes);

        Hunter::Hands hands;
        hands.focused = [this] { return w.focused; };
        hands.key = [this](std::uint32_t vk, int hold) {
            w.keys.push_back({ w.t, vk, hold });
            return true;
        };
        hands.key_down = [this](std::uint32_t vk) {
            w.keys.push_back({ w.t, vk, -1 });
            w.held.insert(vk);
            if (vk == VK_LEFT || vk == VK_RIGHT) w.report();
            return true;
        };
        hands.key_up = [this](std::uint32_t vk) {
            if (w.held.erase(vk) != 0 && (vk == VK_LEFT || vk == VK_RIGHT)) w.report();
        };
        hands.held = [this](std::uint32_t vk) { return w.held.count(vk) != 0; };
        hands.release_all = [this] {
            if (w.walking()) w.report();
            w.held.clear();
        };
        hands.settle_owed = [] { return true; };
        // The pretend Ctrl + ↓: down through the platform onto the first floor
        // under it, coming down land_dx across on that floor.
        hands.drop = [this](const std::function<bool()>& go_on) {
            InputManager::DropPress p;
            ++w.drop_calls;
            if (!go_on()) return p;
            w.t += 50;
            p.down_ms = w.t;
            w.drops.push_back(w.t);
            w.t += 90;
            p.held_ms = 90;
            w.t += 50;
            p.sent = InputManager::DropSent::Sent;
            if (const std::optional<MapFloor> below = w.page->floor_below(w.me_x, w.me_y, 8)) {
                ++w.takeoffs;
                w.falling = true;
                w.report_ms = p.down_ms + 30;
                w.land_x = w.me_x + w.land_dx;
                const std::optional<int> top = w.page->follow_to(w.me_x, below->y - 1, w.land_x);
                w.land_y = (top ? *top : below->y) - 1;
                const double d = std::max(1, w.land_y - w.me_y);
                w.land_at = p.down_ms + static_cast<std::uint64_t>(std::sqrt(d / 1200.0) * 1000.0);
            }
            return p;
        };
        hunter.set_hands(hands);

        Hunter::Clock clock;
        clock.now_ms = [this] { return w.t; };
        clock.sleep_ms = [this](std::uint32_t ms) { w.t += ms; };
        hunter.set_clock(clock);

        hunter.set_map_book(&book);
        hunter.set_drop_veto([] { return static_cast<const char*>(nullptr); });
        hunter.set_config(cfg);
        hunter.start_stepped();
    }
    ~ZoneRig() { hunter.stop(); }

    void pass() {
        hunter.step();
        w.walk_on();
        w.t += 60;
        if (w.falling && w.t >= w.land_at) {
            w.falling = false;
            w.me_x = w.land_x;
            w.me_y = w.land_y;
            w.landed_at = w.t;
            w.report();
            w.note();
        }
    }
    void run(std::uint64_t ms, const std::function<void()>& each = nullptr) {
        for (const std::uint64_t until = w.t + ms; w.t < until;) {
            pass();
            if (each) each();
        }
    }
    bool until(const std::function<bool()>& done, std::uint64_t ms) {
        for (const std::uint64_t end = w.t + ms; w.t < end;) {
            pass();
            if (done()) return true;
        }
        return false;
    }
    Hunter::Status st() const { return hunter.status(); }
};

/// The owner's hunt.ini of 2026-11-04, drops off unless a test turns them on
/// -- and the seek of 2026.11.05b off: these are 04d's walks back, which the
/// seek, standing inside the box, does not make (its twins: test_zone_seek).
HunterConfig owner_config() {
    HunterConfig c;
    c.seek_in_zone = false;
    c.reach = 450;
    c.height_tolerance = 60;
    c.jump_key = 38;            // ↑
    c.jump_to_reach = true;
    c.packet_positions = PacketPositions::On;
    c.spot = true;
    c.spot_half_w = 300;
    c.spot_half_h = 100;
    c.spot_tolerance = 48;
    c.spot_return_delay_ms = 1500;
    c.drop_down = false;
    c.drop_gate_px = 150;
    c.hold_walk = true;
    c.give_up_ms = 4000;
    return c;
}

Hunter::Zone zone_box(int home_x, int home_y, int left, int top, int right, int bottom) {
    Hunter::Zone z;
    z.stem = "t52_s2";
    z.epoch = 7;
    z.home_x = static_cast<float>(home_x);
    z.home_y = static_cast<float>(home_y);
    z.left = static_cast<float>(left);
    z.top = static_cast<float>(top);
    z.right = static_cast<float>(right);
    z.bottom = static_cast<float>(bottom);
    return z;
}
/// spots.ini's [t52_s2] as the owner left it at 20:39:10: home 880,274 on the
/// spawn ledge, 7 px under the 피스존 gate, the box dragged down to 694.
Hunter::Zone owner_zone() { return zone_box(880, 274, 265, 174, 1399, 694); }
/// The box the 04d notes advise: the home low on the ground, the box up from it.
Hunter::Zone home_low_zone() { return zone_box(1100, 714, 265, 174, 1399, 810); }

bool from_map(const ZoneRig& r) { return r.hunter.spot().set && r.hunter.spot().from_map; }

// ------------------------------------------------------------- the rules

void test_zone_rules() {
    section("Hunter 04d on t52_s2's own floors: the monster's floor, the drop's slopes, the four refusals of 20:39-20:40");
    const std::string data = game_data();
    if (data.empty()) {
        skip("no copies of the game's map files here (build\\handoff\\drop\\testdata): the hunter on t52_s2\n");
        return;
    }
    GameMapBook book(data);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t52_s2");
    const std::vector<MapGate> gates = book.gates("t52_s2");
    check(page && gates.size() == 2, "t52_s2 and its two gates read");
    if (!page) return;
    const GameMapBook::Page& p = *page;

    // The monster's floor (F7): its point is often over it.
    check(Hunter::monster_floor(p, 1088, 515) == std::optional<int>(517) &&
              Hunter::monster_floor(p, 1370, 613) == std::optional<int>(701) &&
              Hunter::monster_floor(p, 1399, 631) == std::optional<int>(694) && !Hunter::monster_floor(p, 1282, 466),
          "the monster's floor: 517 under 1088,515 on the ramp; 701 under 1370,613, 88 px over the small floor; 694 "
          "under 1399,631; none within 100 px under 1282,466");

    // The facts at a press over t52_s2: the owner's settings, the map
    // trusted, the area the owner's box unless a case changes it.
    HunterConfig cfg;
    cfg.drop_down = true;
    cfg.packet_positions = PacketPositions::On;
    cfg.reach = 450;
    cfg.jump_key = 38;
    cfg.drop_gate_px = 150;
    const auto facts = [&](int x, int y, int tx, int ty, bool area) {
        Hunter::DropFacts f;
        f.press = true;
        f.now = 100'000;
        f.wired = true;
        f.packet_target = true;
        f.dx = tx - x;
        f.dy = ty - y;
        f.target_wx = static_cast<float>(tx);
        f.target_wy = static_cast<float>(ty);
        f.fix.state = WorldFix::State::Known;
        f.fix.x = f.fix.rx = static_cast<float>(x);
        f.fix.y = f.fix.ry = static_cast<float>(y);
        f.fix.report_ms = 99'960;
        f.entry_ms = 10'000;
        f.settle_from_ms = 99'940;
        f.page = page;
        f.gates = gates;
        if (area) {
            f.spot_use = true;
            f.area.set = true;
            f.area.from_map = true;
            f.area.stem = "t52_s2";
            f.area.x = 880.0f;
            f.area.y = 274.0f;
            f.area.left = 265.0f;
            f.area.top = 174.0f;
            f.area.right = 1399.0f;
            f.area.bottom = 694.0f;
        }
        return f;
    };
    HunterConfig spot_cfg = cfg;
    spot_cfg.spot = true;

    // A, 20:39:43: from the ledge at 968 the fall lands on the ground at 675,
    // 158 px under the ramp the monster stood on -- refused, as live, and the
    // 피스존 gate 87 px across would refuse it too.
    Hunter::DropCall a = Hunter::drop_rule(spot_cfg, facts(968, 274, 1088, 515, true));
    check(a.why == Why::PastMonster && a.land_y == 675 && a.target_floor == 517 && a.px == 158 && a.placed,
          fmt("A, 20:39:43, 968,274 to 1088,515: past the monster still -- floor 675, %d px under its floor %d (%s)",
              a.px, a.target_floor, Hunter::drop_why_name(a.why)));
    // ... and the drop it could have had: above the monster, the ramp under
    // the ledge's end, a slope of 0.74 -- refused at every column in 04c.
    a = Hunter::drop_rule(spot_cfg, facts(1094, 274, 1088, 515, true));
    check(a.ok() && a.direct && a.land_y == 513 && a.stand_y == 274,
          fmt("from 1094,274 over it: it goes -- onto the ramp at 513, the monster's floor, on a slope (%s)",
              Hunter::drop_why_name(a.why)));
    Hunter::DropFacts walking = facts(1080, 274, 1088, 515, true);
    walking.press = false;
    walking.fix.moving = true;
    walking.fix.rx = 1050.0f;
    a = Hunter::drop_rule(spot_cfg, walking);
    check(a.ok() && a.direct && a.land_y == 524 && a.y0 == 273,
          fmt("walking at 1080, 24 px either side: the ramp's 1-px hole at 1056 walked over -- it goes (%s)",
              Hunter::drop_why_name(a.why)));

    // B, 20:40:01: the fix worked out up the ramp to 1282, its height the
    // report's at 1159 -- 70 px under the ramp. Judged from the ramp now.
    Hunter::DropFacts b = facts(1282, 466, 1399, 631, true);
    b.press = false;
    b.fix.moving = true;
    b.fix.rx = 1159.0f;
    b.fix.ry = 464.0f;
    const Hunter::DropCall bc = Hunter::drop_rule(spot_cfg, b);
    check(bc.why == Why::PastMonster && bc.y0 == 396 && bc.stand_y == 397 && bc.land_y == 746 &&
              bc.target_floor == 694 && bc.px == 52,
          fmt("B, 20:40:01: judged from the ramp at 1282,%d (floor %d), not 466 -- the ground at %d, %d px under the "
              "monster's floor %d: past it (%s)",
              bc.y0, bc.stand_y, bc.land_y, bc.px, bc.target_floor, Hunter::drop_why_name(bc.why)));

    // C and D, 20:40:02: the ramp's top over the small floor, the monster's
    // point 78-88 px over it. Its floor now, and direct -- refused only by the
    // box's bottom at 694, the landing at 701-702.
    const Hunter::DropCall c = Hunter::drop_rule(spot_cfg, facts(1362, 380, 1370, 613, true));
    const Hunter::DropCall d = Hunter::drop_rule(spot_cfg, facts(1366, 380, 1350, 627, true));
    check(c.why == Why::OutsideArea && c.direct && c.land_y == 702 && c.target_floor == 701 &&
              d.why == Why::OutsideArea && d.direct && d.land_y == 701 && d.target_floor == 702,
          fmt("C and D, 20:40:02: the small floor is the monsters' -- landings %d and %d direct, outside the box "
              "(%s, %s)",
              c.land_y, d.land_y, Hunter::drop_why_name(c.why), Hunter::drop_why_name(d.why)));
    Hunter::DropFacts deeper = facts(1362, 380, 1370, 613, true);
    deeper.area.bottom = 720.0f;
    Hunter::DropFacts deeper_d = facts(1366, 380, 1350, 627, true);
    deeper_d.area.bottom = 720.0f;
    check(Hunter::drop_rule(spot_cfg, deeper).ok() && Hunter::drop_rule(spot_cfg, deeper_d).ok(),
          "the box's bottom dragged to 720: both go");

    // From a slope: the ramp's foot at 1040 to the ground under it -- its own
    // floor falls 16 px within 24, which 04c read as a floor between.
    Hunter::DropFacts slope = facts(1040, 556, 1020, 690, false);
    slope.press = false;
    slope.fix.moving = true;
    const Hunter::DropCall s = Hunter::drop_rule(cfg, slope);
    check(s.ok() && s.direct && s.land_y == 697 && s.stand_y == 557,
          fmt("from the ramp's slope at 1040,556, walking: the ground at 697 -- the monster's, followed -- goes (%s)",
              Hunter::drop_why_name(s.why)));
    // A floor end within the margin still refuses: the small floor starts at
    // 1337, 3 px left of 1340.
    const Hunter::DropCall edge = Hunter::drop_rule(cfg, facts(1340, 382, 1360, 620, false));
    check(edge.why == Why::NoLanding && edge.px == -Hunter::kDropMarginPressPx,
          fmt("from 1340: the small floor ends 3 px to the left -- no landing, as ever (%s)", Hunter::drop_why_name(edge.why)));
    // A refusal before the landing is looked at says the monster's floor all
    // the same: off every floor at the press, the monster over the ground --
    // not "no floor within 100 px under it".
    const Hunter::DropCall off = Hunter::drop_rule(cfg, facts(1300, 600, 1310, 752, false));
    check(off.why == Why::OffFloor && off.placed && off.target_floor >= 752 && off.target_floor <= 760,
          fmt("from 1300,600, on no floor at the press: refused (%s), the monster's floor %d still looked up",
              Hunter::drop_why_name(off.why), off.target_floor));

    // The gate rule, unchanged: 119 px from the 피스존 gate, refused; 151 px, not.
    const Hunter::DropCall gate = Hunter::drop_rule(spot_cfg, facts(1000, 274, 1010, 576, true));
    check(gate.why == Why::Gate && gate.px == 119 && gate.direct,
          fmt("from the ledge at 1000 onto the ramp's foot: the 피스존 gate %d px across -- refused (%s)", gate.px,
              Hunter::drop_why_name(gate.why)));
    check(Hunter::drop_rule(spot_cfg, facts(1032, 274, 1040, 556, true)).ok(),
          "from 1032, the gate 151 px across: it goes");

    // The monster's last standing point over its point: it stood on the
    // ramp's top at 1300,389 and walked down to 1094 -- a walk moves its x
    // alone, its point stayed at 390, 123 px over the ramp there. Its floor,
    // followed down the ramp: the drop goes; by its point alone, past it.
    Hunter::DropFacts stood = facts(1094, 274, 1094, 390, true);
    stood.target_stood = true;
    stood.target_stand = { 1300, 389 };
    const Hunter::DropCall st = Hunter::drop_rule(spot_cfg, stood);
    Hunter::DropFacts unstood = stood;
    unstood.target_stood = false;
    check(st.ok() && st.direct && st.target_floor == 513 && Hunter::drop_rule(spot_cfg, unstood).why == Why::PastMonster,
          fmt("a monster that stood on the ramp at 1300,389 and walked down it, its point still at 390: the ramp at "
              "1094 is its floor (%d) -- it goes; by its point alone, past it",
              st.target_floor));

    // The owner's answers in one place each.
    HunterConfig hop_cfg;
    hop_cfg.jump_key = 38;
    HunterConfig no_hop = hop_cfg;
    no_hop.spot = true;
    HunterConfig down_key = hop_cfg;
    down_key.jump_key = 0x28;
    check(Hunter::kSkipAboveUnreached && Hunter::kDropStopAbovePx == 24 && Hunter::kDropMonsterFloorPx == 100 &&
              Hunter::hops_to_targets(hop_cfg) && !Hunter::hops_to_targets(no_hop) &&
              !Hunter::hops_to_targets(down_key) && Hunter::kZoneWalkSlackPx + Hunter::kZoneWalkSlackPx >= 96,
          "Q3 (a): skip above with no hop to follow -- the spot on, or a jump key that never goes out; Q6 (a): 24 px");
}

// ------------------------------------------------------------- the hunt

void test_zone_walk_kept() {
    section("Hunter 04d: a walk kept in the box (Q1) -- 987,683 never walks right past x 1030; a refused drop's walk "
            "off the ledge kept too (F6)");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the walk kept in the box\n");
        return;
    }

    // 20:43:31, 987,683 on the ground, inside the box: a monster on the small
    // floor's end inside it, 412 px right, level -- out of reach at 150. The
    // ground leaves the box's bottom at 1035.
    {
        HunterConfig c = owner_config();
        c.reach = 150;
        std::vector<std::string> log;
        int max_x = 0, rights = 0;
        Hunter::Status st;
        bool used = false;
        std::string minute;
        {
            GameMapBook probe(game_data());
            const GameMapBook::Page& p = *probe.get("t52_s2");
            ZoneRig r(c, 987, stand_y(p, 987, 683));
            r.hunter.set_zone(owner_zone());
            r.pass();
            used = from_map(r);
            r.w.mobs = { mob_at(1399, stand_y(p, 1399, 693), 3) };
            log = info_lines([&] { r.run(61'000); });
            max_x = r.w.max_x;
            rights = r.w.downs_of(VK_RIGHT);
            st = r.st();
            minute = line_with(log, " fights, ");
            check(r.w.downs_of(VK_DOWN) == 0 && r.w.downs_of(38) == 0, "no Down, no jump key");
        }
        check(used && max_x <= 1030,
              fmt("the owner's zone in use; the character never right of x 1030 in 61 s (furthest %d; %d right keys)",
                  max_x, rights));
        check(lines_with(log, "Hunter: not walking right toward a monster", "it leaves the area along its floor at "
                                                                           "1035,695 (1 px outside the hunting area)") >=
                  1,
              "said: \"" + line_with(log, "not walking right") + "\"");
        check(minute.find("walks stopped at the hunting area's edge ") != std::string::npos,
              "and counted in the minute line: \"" + minute.substr(minute.find(';') == std::string::npos ? 0 : minute.find(';')) +
                  "\"");
        check(st.spot_why == Hunter::SpotWhy::None && st.spot_outside_px == 0,
              fmt("in the box, a way back the map shows: nothing to say (outside %d px)", st.spot_outside_px));
    }

    // This run's spot -- no saved zone -- with the map book there: the walk
    // goes on as before 04d, nothing judged by the map.
    {
        HunterConfig c = owner_config();
        c.reach = 150;
        GameMapBook probe(game_data());
        const GameMapBook::Page& p = *probe.get("t52_s2");
        ZoneRig r(c, 987, stand_y(p, 987, 683));
        r.pass();
        r.w.mobs = { mob_at(1200, stand_y(p, 1200, 719), 3) };
        const std::vector<std::string> log = info_lines([&] { r.run(3000); });
        check(!from_map(r) && r.hunter.spot().set && r.w.max_x > 1035 && lines_with(log, "not walking") == 0,
              fmt("this run's spot round 987,680: walked right toward the monster at 1200 as ever, to %d -- no walk "
                  "judged by the map",
                  r.w.max_x));
    }

    // F6: a drop refused -- the 피스존 gate within drop_gate_px 400 -- became a
    // walk off the ledge's end. In a box whose right edge is 1199 the fall at
    // a walk's speed lands at 1231: never walked.
    {
        HunterConfig c = owner_config();
        c.drop_down = true;
        c.drop_gate_px = 400;
        c.spot_return_delay_ms = 60'000;
        GameMapBook probe(game_data());
        const GameMapBook::Page& p = *probe.get("t52_s2");
        ZoneRig r(c, 1060, stand_y(p, 1060, 274));
        r.hunter.set_zone(zone_box(880, 274, 600, 174, 1199, 560));
        r.run(Hunter::kDropAfterEntryMs + 300);
        const bool used = from_map(r);
        r.w.mobs = { mob_at(1150, stand_y(p, 1150, 471), 4) };
        const std::vector<std::string> log = info_lines([&] { r.run(6000); });
        check(used && r.w.max_x <= 1104 && r.w.max_y <= 275 && r.w.falls == 0 && r.w.drop_calls == 0,
              fmt("from 1060 on the ledge, the monster on the ramp below at 1150: no drop (the gate), and no walk off "
                  "the ledge's end at 1104 (furthest %d, lowest %d)",
                  r.w.max_x, r.w.max_y));
        check(lines_with(log, "a gate is near (179 px across; drop_gate_px is 400); you at 1060,274") == 1 &&
                  lines_with(log, "it walks off its floor's end, out of the area at 1104,273, landing at 1231,421") >= 1,
              "said: \"" + line_with(log, "not dropping") + "\" and \"" + line_with(log, "not walking") + "\"");
        check(r.w.downs_of(VK_DOWN) == 0, "no Down");
    }

    // "Giữ khoảng cách": a step back is looked along the floor as a walk is.
    // From 987,680 a monster 30 px left: the step right, about 80 px, ends at
    // 1068,706 -- 12 px under the box, its x room 412 px all the same -- none,
    // it fights where it stands. The monster 30 px right: the step left, up
    // the slope inside the box, goes.
    {
        HunterConfig c = owner_config();
        c.keep_distance = true;
        GameMapBook probe(game_data());
        const GameMapBook::Page& p = *probe.get("t52_s2");
        const auto steps = [](const ZoneWorld& w, std::uint32_t vk) {
            int n = 0;
            for (const ZoneWorld::Key& k : w.keys)
                if (k.vk == vk && k.hold > Hunter::kTurnTapMs) ++n;
            return n;
        };
        ZoneRig r(c, 987, stand_y(p, 987, 683));
        r.hunter.set_zone(owner_zone());
        r.pass();
        const bool used = from_map(r);
        r.w.mobs = { mob_at(957, stand_y(p, 957, 671), 4) };
        r.run(2000);
        ZoneRig l(c, 987, stand_y(p, 987, 683));
        l.hunter.set_zone(owner_zone());
        l.pass();
        l.w.mobs = { mob_at(1017, stand_y(p, 1017, 689), 4) };
        l.run(2000);
        check(used && from_map(l) && steps(r.w, VK_RIGHT) == 0 && r.w.downs_of('Z') > 0 && steps(l.w, VK_LEFT) >= 1,
              fmt("keep_distance, 30 px from a monster: no step right out of the box (%d), attacked from where it "
                  "stands (%d); the step left inside it taken (%d)",
                  steps(r.w, VK_RIGHT), r.w.downs_of('Z'), steps(l.w, VK_LEFT)));
        check(r.w.downs_of(VK_DOWN) == 0 && l.w.downs_of(VK_DOWN) == 0 && r.w.downs_of(38) == 0, "no Down, no jump key");
    }
}

void test_zone_walk_back() {
    section("Hunter 04d: the way back (Q2) -- from 1402,802 left along the ground to 880,634 with no jump key; no way "
            "back, it fights; 882 under the home is not home");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the way back\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");

    // 20:43:51-20:44:41, 1402,802: 108 px under the box, a monster in reach
    // out there, one above on the ramp. It walks back first: left along the
    // ground it stands on, up 168 px of slope, to 880 -- no hop.
    {
        ZoneRig r(owner_config(), 1402, 802);
        r.hunter.set_zone(owner_zone());
        r.w.mobs = { mob_at(1450, stand_y(p, 1450, 818), 1), mob_at(1300, stand_y(p, 1300, 389), 2) };
        r.pass();
        const Hunter::Status first = r.st();
        std::vector<std::string> log = info_lines([&] {
            r.until([&] { return r.st().returns > 0 && !r.w.walking() && std::abs(r.w.me_x - 880) <= 48; }, 8000);
            r.run(2000);
        });
        const Hunter::Status st = r.st();
        check(from_map(r) && first.spot_outside_px == 108 && first.spot_back_first && first.spot_way &&
                  first.spot_way_x == 880 && first.spot_way_y == 634,
              fmt("taken 108 px outside the box (%d): the way back the map shows ends at %d,%d, and comes first",
                  first.spot_outside_px, first.spot_way_x, first.spot_way_y));
        check(std::abs(r.w.me_x - 880) <= 48 && r.w.me_y == stand_y(p, r.w.me_x, 640) && r.w.me_y < 694 &&
                  !r.w.walking() &&
                  r.w.downs_of(38) == 0 && r.w.downs_of(VK_RIGHT) == 0 && r.w.downs_of(VK_LEFT) >= 1 &&
                  r.w.downs_of('Z') == 0 && r.w.falls == 0,
              fmt("walked left along the ground to %d,%d and stood: no jump key (%d), no right, no attack on the "
                  "monster in reach out there (%d)",
                  r.w.me_x, r.w.me_y, r.w.downs_of(38), r.w.downs_of('Z')));
        check(st.spot_why == Hunter::SpotWhy::None && st.spot_outside_px == 0 && st.returns == 1 &&
                  !st.spot_back_first && !st.spot_home,
              fmt("in the box, at the way back's point -- not home, 360 px under it: nothing more to walk (returns "
                  "%llu)",
                  static_cast<unsigned long long>(st.returns)));
        check(lines_with(log, "Hunter: walking back into the hunting area from 1402,802 (108 px outside it): to "
                              "880,634, its floor's point nearest the home -- 522 px left, 168 px up, along the floor "
                              "it stands on, no hop") == 1,
              "said once: \"" + line_with(log, "walking back into") + "\"");
        check(r.w.downs_of(VK_DOWN) == 0, "no Down");
    }

    // The walk back from there getting nowhere -- a wall: the left arrow held
    // walks nothing. Tried again a while later, and given up on after
    // kSpotMaxTries: while it waits, and for good after, the monster in reach
    // out there is fought as in 04c -- the walk back comes first only while
    // one is to be tried, or the character stands out there and never
    // attacks.
    {
        HunterConfig c = owner_config();
        c.spot_retry_ms = 3000;
        ZoneRig r(c, 1402, 802);
        r.w.wall_left = true;
        r.hunter.set_zone(owner_zone());
        r.w.mobs = { mob_at(1450, stand_y(p, 1450, 818), 1) };
        r.pass();
        const bool first = from_map(r) && r.st().spot_back_first;
        int retry_z = -1, gave_z = -1;
        bool retry_first = true, gave_first = true;
        const std::vector<std::string> log = info_lines([&] {
            if (r.until([&] { return r.st().spot_why == Hunter::SpotWhy::Retrying; }, 20'000)) {
                const int z0 = r.w.downs_of('Z');
                r.run(1500);
                retry_z = r.w.downs_of('Z') - z0;
                retry_first = r.st().spot_back_first || r.st().spot_why != Hunter::SpotWhy::Retrying;
            }
            if (r.until([&] { return r.st().spot_why == Hunter::SpotWhy::GaveUp; }, 60'000)) {
                const int z0 = r.w.downs_of('Z');
                r.run(3000);
                gave_z = r.w.downs_of('Z') - z0;
                gave_first = r.st().spot_back_first || r.st().spot_why != Hunter::SpotWhy::GaveUp;
            }
        });
        check(first && retry_z > 0 && !retry_first,
              fmt("the walk back against a wall, to be tried again: the monster in reach outside fought meanwhile "
                  "(%d attacks in 1.5 s), the walk back not first",
                  retry_z));
        check(gave_z > 0 && !gave_first && r.st().return_give_ups == static_cast<std::uint64_t>(Hunter::kSpotMaxTries) &&
                  lines_with(log, "Hunter: the walk back into the hunting area got nowhere", "no more tries") == 1,
              fmt("given up on after %d tries: fought for good (%d attacks in 3 s), the walk back not first -- \"%s\"",
                  Hunter::kSpotMaxTries, gave_z, line_with(log, "no more tries").c_str()));
        check(r.w.downs_of(38) == 0 && r.w.downs_of(VK_DOWN) == 0, "no jump key, no Down");
    }

    // A way back worked out there, then a report on none of the file's
    // floors, standing still 86 px under the box: no way carried to it -- a
    // character standing sends no report, so it would be for good. The
    // monster in reach out there is fought, as in 04c.
    {
        HunterConfig c = owner_config();
        c.spot_return_delay_ms = 60'000;
        ZoneRig r(c, 1402, 802);
        r.hunter.set_zone(owner_zone());
        r.pass();
        const Hunter::Status first = r.st();
        r.w.place(1402, 780);
        r.w.mobs = { mob_at(1450, 780, 1) };
        r.run(2000);
        const Hunter::Status st = r.st();
        check(from_map(r) && first.spot_back_first && first.spot_way && !st.spot_back_first && !st.spot_way &&
                  st.spot_outside_px == 86 && r.w.downs_of('Z') > 0 && r.w.downs_of(VK_LEFT) == 0,
              fmt("from 1402,802 the way worked out; then at 1402,780 on no floor, %d px outside: no way carried, the "
                  "monster in reach fought (%d attacks)",
                  st.spot_outside_px, r.w.downs_of('Z')));
    }

    // Past the file's end, the game's floor going on: the walk back off the
    // ramp's foot (992) into the low box is held, with no report coming, only
    // as far as where it was to fall from -- let go within a pass of it, not
    // walked blind until the packets lose the character.
    {
        ZoneRig r(owner_config(), 1102, stand_y(p, 1102, 505));
        r.w.floor_goes_on = true;
        r.hunter.set_zone(home_low_zone());
        HuntZone low;
        low.home_x = 1100;
        low.home_y = 714;
        low.left = 265;
        low.top = 174;
        low.right = 1399;
        low.bottom = 810;
        const std::optional<ZoneReturn> way = return_point(p, low, MapPoint{ r.w.me_x, r.w.me_y });
        const int walk_x = way ? way->walk_x : 0;
        r.until([&] { return r.w.downs_of(VK_LEFT) > 0 && !r.w.walking(); }, 6000);
        check(from_map(r) && way && way->falls > 0 && walk_x < 992 && r.w.falls == 0 && !r.w.walking() &&
                  r.w.min_x <= walk_x && r.w.min_x > walk_x - r.w.step_px && r.w.downs_of(38) == 0,
              fmt("from the ramp at 1102, the way off its foot to walk to %d: walked past the file's end on the "
                  "game's floor to %d and let go there, no jump key (%d)",
                  walk_x, r.w.min_x, r.w.downs_of(38)));
    }

    // With no way back -- the floor under the ground, 889..914, whose ends
    // both fall onto the bottom outside the box -- it fights where it stands.
    {
        ZoneRig r(owner_config(), 1300, stand_y(p, 1300, 752));
        r.hunter.set_zone(owner_zone());
        r.pass();
        const bool used = from_map(r);
        r.w.place(1300, stand_y(p, 1300, 893));
        r.w.mobs = { mob_at(1350, stand_y(p, 1350, 890), 5) };
        r.run(3000);
        const Hunter::Status st = r.st();
        check(used && st.spot_why == Hunter::SpotWhy::NoReturn && st.spot_outside_px == 199 && !st.spot_way &&
                  r.w.downs_of('Z') > 0 && r.w.downs_of(VK_LEFT) == 0 && r.w.downs_of(VK_RIGHT) <= 1,
              fmt("from 1300,893, 199 px under the box with no way back: it fights the monster in reach (%d attacks), "
                  "NoReturn said every pass",
                  r.w.downs_of('Z')));
        r.w.mobs.clear();
        const std::vector<std::string> log = info_lines([&] { r.run(3000); });
        check(lines_with(log, "Hunter: no way back into the hunting area from 1300,893 (199 px outside it)") == 1 &&
                  r.w.downs_of(VK_LEFT) == 0 && r.st().returns == 0 && r.w.downs_of(38) == 0,
              "nothing left: said once, no walk -- \"" + line_with(log, "no way back") + "\"");
        // A jump attack out there: NoReturn in the air too, as judged where it
        // stood -- not None at every jump, the card flickering "đi bộ về".
        r.w.mobs = { mob_at(1350, stand_y(p, 1350, 890), 5) };
        r.w.falling = true;
        r.w.land_at = r.w.t + 60'000;
        r.w.land_x = r.w.me_x;
        r.w.land_y = r.w.me_y;
        int passes = 0, kept = 0;
        r.run(900, [&] {
            ++passes;
            if (r.st().spot_why == Hunter::SpotWhy::NoReturn) ++kept;
        });
        r.w.falling = false;
        check(passes > 0 && kept == passes,
              fmt("in the air over its floor, fighting: NoReturn at %d of %d passes", kept, passes));
    }

    // The 04c way back, for a report on none of the file's floors, says the
    // home 506 px up is too high; landed where the map shows no way back, it
    // is NoReturn from then on -- fighting too, not TooHigh for good.
    {
        ZoneRig r(owner_config(), 1402, 780);
        r.hunter.set_zone(owner_zone());
        const std::vector<std::string> log = info_lines([&] { r.run(600); });
        const Hunter::SpotWhy high = r.st().spot_why;
        r.w.place(1300, stand_y(p, 1300, 893));
        r.run(600);
        r.w.mobs = { mob_at(1350, stand_y(p, 1350, 890), 5) };
        r.run(1500);
        const Hunter::Status st = r.st();
        check(from_map(r) && high == Hunter::SpotWhy::TooHigh &&
                  lines_with(log, "Hunter: the hunting spot is 506 px above -- higher than one jump") == 1 &&
                  st.spot_why == Hunter::SpotWhy::NoReturn && r.w.downs_of('Z') > 0,
              fmt("on no floor at 1402,780: TooHigh, as 04c (%d); then on 1300,893 with no way back: NoReturn while "
                  "it fights (%d, %d attacks)",
                  static_cast<int>(high), static_cast<int>(st.spot_why), r.w.downs_of('Z')));
    }

    // Home by height: 882 on the ground, "3 px off" at 20:41:11 by x alone,
    // is not home; nor 882,616 in the air; the ledge at 885 is.
    {
        ZoneRig r(owner_config(), 885, stand_y(p, 885, 273));
        r.hunter.set_zone(owner_zone());
        r.pass();
        const bool at_home = r.st().spot_home;
        r.w.place(882, stand_y(p, 882, 635));
        r.pass();
        const Hunter::Status ground = r.st();
        r.w.place(882, 616);
        r.pass();
        const bool air = r.st().spot_home;
        check(from_map(r) && at_home && !ground.spot_home && ground.spot_way && ground.spot_way_x == 880 && !air,
              "the ledge at 885: home; the ground at 882 under it: not home -- its way back is 880,634; 882,616 in "
              "the air: not home");
    }

    // The home low on the ground, 1100,714, the character over it on the ramp
    // at 1102: by x alone home, 208 px over it. It walks: down the ramp,
    // off its foot at 992, and back along the ground to the home -- no hop.
    {
        ZoneRig r(owner_config(), 1102, stand_y(p, 1102, 505));
        r.hunter.set_zone(home_low_zone());
        r.pass();
        const bool used = from_map(r) && !r.st().spot_home;
        r.until([&] { return r.w.falls > 0 && !r.w.walking() && std::abs(r.w.me_x - 1100) <= 48; }, 15'000);
        r.pass();
        check(used && r.w.falls == 1 && std::abs(r.w.me_x - 1100) <= 48 && r.w.me_y >= 690 && r.w.me_y <= 716 &&
                  r.w.downs_of(38) == 0 && r.st().spot_home,
              fmt("from the ramp over the home: off the ramp's foot (%d fall), then along the ground to %d,%d -- home, "
                  "no jump key (%d)",
                  r.w.falls, r.w.me_x, r.w.me_y, r.w.downs_of(38)));
    }
}

void test_zone_above() {
    section("Hunter 04d: monsters above that no hop gets to are passed over (Q3); one up the character's own slope "
            "is walked to");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: monsters above\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");

    // 20:43:32's: from 987,683 one on the small ramp 104 px up -- the nearest
    // by the plates' measure -- and one up the ground's own slope, 287 px
    // left and 101 up. The ground's is the target; the ramp's never.
    ZoneRig r(owner_config(), 987, stand_y(p, 987, 683));
    r.hunter.set_zone(owner_zone());
    r.w.mobs = { mob_at(1010, stand_y(p, 1010, 576), 6), mob_at(700, stand_y(p, 700, 579), 7) };
    bool ramp = false;
    r.run(1500, [&] {
        const Hunter::Status st = r.st();
        if (st.has_target && st.target.slot == 6) ramp = true;
    });
    const Hunter::Status st = r.st();
    check(from_map(r) && st.has_target && st.target.slot == 7 && !ramp && r.w.downs_of(VK_LEFT) >= 1 &&
              r.w.downs_of(38) == 0,
          fmt("the one up the ground's slope is the target (slot %d), walked to the left, never the ramp's; no jump "
              "key",
              st.has_target ? st.target.slot : -1));
    // With no map to say -- this run's spot, drops off -- any height, as ever:
    // the nearest, its area 200 px up and down.
    HunterConfig c = owner_config();
    c.spot_half_h = 200;
    ZoneRig s(c, 987, stand_y(p, 987, 683));
    s.w.mobs = { mob_at(1010, stand_y(p, 1010, 576), 6), mob_at(700, stand_y(p, 700, 579), 7) };
    s.run(300);
    check(s.st().has_target && s.st().target.slot == 6, "this run's spot, no map gone by: the nearest above, as before");

    // This run's spot with drops on, t52_s2 trusted by them at once (the
    // entry by the 피스존 gate): still as before 04d -- the ramp's 107 px up
    // is the target, its area 200 px up and down; never a jump key.
    {
        HunterConfig d = owner_config();
        d.spot_half_h = 200;
        d.drop_down = true;
        ZoneRig t(d, 987, stand_y(p, 987, 683));
        t.run(Hunter::kDropAfterEntryMs + 300);
        const bool trusted = t.st().drop_map.how != MapTrust::How::None;
        t.w.mobs = { mob_at(1010, stand_y(p, 1010, 576), 6) };
        bool picked = false;
        t.run(1500, [&] {
            if (t.st().has_target && t.st().target.slot == 6) picked = true;
        });
        check(trusted && !from_map(t) && t.hunter.spot().set && picked && t.w.downs_of(38) == 0,
              "this run's spot, drops on, t52_s2 trusted: the ramp's 107 px up is the target as before 04d, no jump "
              "key");
    }
    // No spot, a hop to follow: the ramp's picked, as before.
    {
        HunterConfig d = owner_config();
        d.spot = false;
        d.drop_down = true;
        ZoneRig t(d, 987, stand_y(p, 987, 683));
        t.run(Hunter::kDropAfterEntryMs + 300);
        t.w.mobs = { mob_at(1010, stand_y(p, 1010, 576), 6) };
        t.run(300);
        check(t.st().has_target && t.st().target.slot == 6, "no spot, a hop to follow: the ramp's picked, as before");
    }
    // No spot and no hop to follow, drops on: the map they trust says the
    // ramp is not the character's own floor -- Q3, passed over.
    {
        HunterConfig d = owner_config();
        d.spot = false;
        d.jump_to_reach = false;
        d.drop_down = true;
        ZoneRig t(d, 987, stand_y(p, 987, 683));
        t.run(Hunter::kDropAfterEntryMs + 300);
        const bool trusted = t.st().drop_map.how != MapTrust::How::None;
        t.w.mobs = { mob_at(1010, stand_y(p, 1010, 576), 6) };
        bool picked = false;
        t.run(1500, [&] {
            if (t.st().has_target && t.st().target.slot == 6) picked = true;
        });
        check(trusted && !picked && t.w.downs_of(38) == 0,
              "no spot, no hop, drops on and t52_s2 trusted: the ramp's 107 px up never the target (Q3), no jump key");
    }
}

void test_zone_drops() {
    section("Hunter 04d: the drop on t52_s2 -- stood above the monster (Q6), landed on a slope and judged there");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the drop on t52_s2\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");

    // From the ledge at 1094, the monster on the ramp under it: stood above
    // it, pressed, and came down 20 px further on the ramp, 14 px higher than
    // where the file had the fall land -- the floor the file said all the
    // same: it worked. 04c called it off the map, drops off, the file not trusted.
    {
        HunterConfig c = owner_config();
        c.drop_down = true;
        c.spot_return_delay_ms = 60'000;
        ZoneRig r(c, 1094, stand_y(p, 1094, 273));
        r.w.land_dx = 20;
        r.hunter.set_zone(owner_zone());
        r.run(Hunter::kDropAfterEntryMs + 300);
        r.w.mobs = { mob_at(1088, stand_y(p, 1088, 516), 8) };
        const std::vector<std::string> log = info_lines([&] { r.until([&] { return r.w.landed_at != 0; }, 4000); r.run(300); });
        const Hunter::Status st = r.st();
        check(r.w.drops.size() == 1 && r.w.me_x == 1114 && r.w.me_y == 497 && st.drops_worked == 1 &&
                  st.drop_off == Hunter::DropOff::None && !r.book.distrusted("t52_s2"),
              fmt("one Ctrl + Down from 1094,273, down at 1114,497 on the ramp: worked (%llu), drops still on, the file "
                  "still trusted",
                  static_cast<unsigned long long>(st.drops_worked)));
        check(lines_with(log, "Hunter: drop (Ctrl+Down) toward the monster: 6 px left, 243 px down -- you at 1094,273 "
                              "on floor 274 of t52_s2") == 1 &&
                  lines_with(log, "Hunter: drop worked: landed at 1114,497") == 1,
              "said: \"" + line_with(log, "drop (Ctrl+Down)") + "\"");
        check(r.w.downs_of(VK_DOWN) == 0 && r.w.landed_at != 0 && r.w.downs_of(VK_RIGHT, 0, r.w.landed_at - 1) == 0 &&
                  r.w.downs_of(VK_LEFT, 0, r.w.landed_at - 1) == 0,
              fmt("no Down, and no walk before the landing: it stood above the monster (%d left, %d right)",
                  r.w.downs_of(VK_LEFT, 0, r.w.landed_at - 1), r.w.downs_of(VK_RIGHT, 0, r.w.landed_at - 1)));
    }

    // From the ledge at 1000, the monster on the ramp's foot 10 px right,
    // 302 px down: stood above it -- not walked to and fro over it -- and the
    // drop refused, the 피스존 gate 119 px across, with where it was judged.
    {
        HunterConfig c = owner_config();
        c.drop_down = true;
        c.spot_return_delay_ms = 60'000;
        ZoneRig r(c, 1000, stand_y(p, 1000, 274));
        r.hunter.set_zone(owner_zone());
        r.run(Hunter::kDropAfterEntryMs + 300);
        r.w.mobs = { mob_at(1010, stand_y(p, 1010, 576), 9) };
        bool stood = false;
        Why wait = Why::None;
        const std::vector<std::string> log = info_lines([&] {
            r.run(3000);
            stood = r.w.drop_calls == 0 && r.w.downs_of(VK_LEFT) == 0 && r.w.downs_of(VK_RIGHT) == 0 &&
                    r.w.max_x == 1000;
            wait = r.st().drop_wait;
            r.run(58'000);
        });
        check(stood && wait == Why::Gate,
              fmt("stood above it for 3 s: no walk, no press (%s)", Hunter::drop_why_name(wait)));
        check(lines_with(log, "not dropping to the monster 302 px below -- a gate is near (119 px across; "
                              "drop_gate_px is 150); you at 1000,274 (floor 275), the monster at 1010,576 (its floor "
                              "577), 10 px right") == 1,
              "said once, with where: \"" + line_with(log, "not dropping") + "\"");
        const std::string m = line_with(log, " fights, ");
        check(m.find("; drop looks ") != std::string::npos && m.find("refused: gate ") != std::string::npos,
              "the minute line counts the looks and what refused them: \"" +
                  (m.find("; drop looks") == std::string::npos ? m : m.substr(m.find("; drop looks"))) + "\"");
        check(r.w.downs_of(VK_DOWN) == 0, "no Down");
    }
}

// ------------------------------------------------------------- the source

#ifdef GOBOT_SOURCE_DIR
std::string source(const char* rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int count_of(const std::string& text, const char* what) {
    int n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}
#endif

void test_zone_seek() {
    section("Hunter 2026.11.05b: 04d's walks back inside the box, with the seek on -- stood for where it is (the "
            "owner's Q1 (a)); from outside, walked back in and stood just inside");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the seek's twins of the walks back\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    HunterConfig on = owner_config();
    on.seek_in_zone = true;

    // On the ramp over the low home, nothing to fight: 04d walked down the
    // ramp, off its foot and home; the seek stands where it is.
    {
        ZoneRig r(on, 1102, stand_y(p, 1102, 505));
        r.hunter.set_zone(home_low_zone());
        std::vector<std::string> log = info_lines([&] { r.run(8000); });
        check(from_map(r) && r.w.keys.empty() && r.w.falls == 0 && r.st().returns == 0 && r.st().seek_none,
              fmt("from the ramp at 1102 inside the low box: no key (%zu), no walk back, nothing to seek",
                  r.w.keys.size()));
        check(lines_with(log, "Hunter: nothing to seek for 0.0 s at 1102,") == 1,
              "said: \"" + line_with(log, "nothing to seek") + "\"");
    }
    // 108 px under the box at 1402,802: walked back first, as 04d -- and once
    // inside the box, nothing to fight, stood there: not on to 880,634.
    {
        ZoneRig r(on, 1402, 802);
        r.hunter.set_zone(owner_zone());
        r.run(12'000);
        const Hunter::Status st = r.st();
        check(from_map(r) && st.returns >= 1 && st.spot_outside_px == 0 && !r.w.walking() && r.w.me_x > 960 &&
                  r.w.me_x < 1040 && r.w.downs_of(VK_RIGHT) == 0 && r.w.downs_of(38) == 0 && st.seek_none,
              fmt("walked back into the box, stood just inside at %d,%d (%llu walk back), nothing to seek", r.w.me_x,
                  r.w.me_y, static_cast<unsigned long long>(st.returns)));
    }
    // The ground under the home ledge, a monster on the left ground out of
    // reach: picked by the seek (its INFO line; seek_on while Approaching)
    // and walked left until it is in reach, about 740 -- the hunter's in-reach
    // check comes before its attack point at about 726. 04d walked there the
    // same way (the monster stands on the character's own floor), so the
    // seek's own line and status are what tell the two apart.
    {
        ZoneRig r(on, 880, stand_y(p, 880, 634));
        r.hunter.set_zone(owner_zone());
        r.run(600);
        bool sought = false;
        std::vector<std::string> log = info_lines([&] {
            r.w.mobs = { mob_at(300, stand_y(p, 300, 532), 21) };
            r.until(
                [&] {
                    const Hunter::Status s = r.st();
                    if (s.seek_on && s.seek_slot == 21 && s.state == Hunter::State::Approaching) sought = true;
                    return s.state == Hunter::State::Attacking;
                },
                8000);
        });
        check(sought && r.st().state == Hunter::State::Attacking && r.w.me_x < 760 && r.w.me_x > 680 &&
                  r.w.downs_of(VK_RIGHT) == 0,
              fmt("the left ground's monster at 300,532: sought, walked left to %d,%d and attacks", r.w.me_x,
                  r.w.me_y));
        check(lines_with(log, "Hunter: seeking slot 21 gen 1 at 300,") == 1,
              "said: \"" + line_with(log, "Hunter: seeking slot 21") + "\"");
    }
}

void test_zone_source() {
    section("Hunter 04d: what it leaves as it was -- which way the character faces, the drop's key (its source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string hunter = source("/src/hunter.cpp");
    const std::string drop = source("/src/hunter_drop.cpp");
    const std::string spot = source("/apps/gobot_gui/simple_spot.cpp");
    if (hunter.empty() || drop.empty()) {
        skip("the hunter's source is not here to read\n");
        return;
    }
    check(count_of(hunter, "facing_key_ms_ = ") == 5 && count_of(hunter, "facing_key_way_ = ") == 5 &&
              count_of(hunter, "facing_ = ") == 9 && count_of(drop, "facing_") == 1 &&
              count_of(hunter, "const bool missed = !after_key && !as_key && clock_now() > facing_key_ms_ + "
                               "kFacingMissedMs;") == 1 &&
              Hunter::kTurnTapMs == 60 && Hunter::kFacingMissedMs == 500 && Hunter::kFacingTrustMs == 150,
          "which way it faces: the same writes as 04c, none in the drop's file (its comment aside), the turn's tap "
          "and its timings unchanged");
    check(count_of(drop, "drop_ctrl_down(go_on)") == 1 && count_of(hunter + drop, "VK_DOWN") == 0 &&
              count_of(hunter + drop, "0x28") == 0,
          "the drop's key only through drop_ctrl_down; no Down named anywhere else in the hunter");
    check(spot.find("out << \", outside the area \" << hs.spot_outside_px << \" px\";") != std::string::npos,
          "the Positions line says how far outside the area the character is");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_zone_hunt(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_zone_rules();
    test_zone_walk_kept();
    test_zone_walk_back();
    test_zone_seek();
    test_zone_above();
    test_zone_drops();
    test_zone_source();
}
