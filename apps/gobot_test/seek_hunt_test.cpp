// gobot_test -- the hunter seeking in the map tab's area, "Tự tìm quái trong
// vùng" (2026.11.05b), on the game's own t52_s2: the hunt of 2026-11-05
// 23:02-23:08 replayed -- the left ground fought from where it stands, a
// monster out of view walked to, the ramp's foot hit from up the slope
// instead of walked into the box's bottom edge, nothing to go to stood for,
// the ledge by a jump up (JumpUpFor::Seek) -- the pick kept against a
// like one either side, a long walk not given up, a monster not where the map
// says passed over, a drop to one below, and the setups the seek leaves as
// they were.
//
// Over copies of the game's map files in build\handoff\drop\testdata --
// never the game's install -- and skipped when they are not there. The
// hunter runs on a pretend game laid one to one over the packets' world: the
// character walks the real floors, falls off their ends, jumps up as the
// model says and drops through a platform; the monsters are its plates, cut
// to the box PacketEyes offers (±614 x ±461), and the whole map's list the
// seek goes by. Nothing here presses a real key.

#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/hunt_card.hpp"
#include "gobot/hunt_seek.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/hunter.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/jump_plan.hpp"
#include "gobot/logger.hpp"
#include "gobot/map_book.hpp"
#include "gobot/map_view.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/packet_world.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
    char buf[900];
    std::snprintf(buf, sizeof buf, f, args...);
    return buf;
}

/// The game's files copied for the tests, or "" -- never the game's install.
std::string game_data() {
#ifdef GOBOT_SOURCE_DIR
    const std::string dir = std::string(GOBOT_SOURCE_DIR) + "\\build\\handoff\\drop\\testdata\\data";
    if (path_exists(dir + "\\Map\\t52_s2.map") && path_exists(dir + "\\Project\\t52_s2.prj")) return dir;
#endif
    return {};
}

std::vector<std::string> info_lines(const std::function<void()>& run, LogLevel level = LogLevel::Info) {
    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_ring_capacity(8000);
    log.clear_ring();
    log.set_level(level);
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

std::string temp_path(const char* name) {
    char dir[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return name;
    return std::string(dir) + "gobot_test_" + std::to_string(GetCurrentProcessId()) + "_" + name;
}

// ------------------------------------------------------------- the rig

/// What the pretend game makes of the next jump's key.
enum class Answer : std::uint8_t {
    Physics,    ///< up at the key's speed, down onto the highest floor its peak passed
    FallBack,   ///< up and bumped back onto where it stood, 0.5 s on
    Nothing,    ///< the key went out and nothing came of it: no take-off, no report
};

/// The pretend game over t52_s2's own floors, in the packets' world pixels.
struct SeekWorld {
    std::uint64_t t = 5'000'000;
    bool          focused = true;
    const GameMapBook::Page* page = nullptr;
    int           me_x = 880, me_y = 273;
    std::vector<Nameplate> mobs;
    /// In the whole map's list only, never a plate: where the map has a
    /// monster the game does not show.
    std::vector<Nameplate> phantoms;
    bool          world_on = true;          ///< the packets steer: the list is had
    bool          cut = true;               ///< plates within ±614 x ±461 of you, as PacketEyes offers them
    int           kill_after = 0;           ///< attacks on a plate in reach that kill it; 0 never
    int           reach = 450;              ///< what counts as an attack landing, for kill_after
    int           step_px = 20;
    bool          wall_left = false;        ///< ← held walks nowhere
    int           max_x = 0, min_x = INT_MAX;
    int           falls = 0;                ///< off a floor's end by a walk
    std::uint32_t epoch = 7;
    bool          owed = false;             ///< a key-up owed that will not go out
    bool          up_owed = false;          ///< ... and it is a high jump's ↑ (InputManager::up_owed)
    std::set<int> others;                   ///< slots another player hurt a moment ago (SeekMob::hurt_by_other)

    std::set<std::uint32_t> held;
    struct Key {
        std::uint64_t at;
        std::uint32_t vk;
        int           hold;
    };
    std::vector<Key> keys;

    float         report_x = 0.0f, report_y = 0.0f;
    std::uint64_t report_ms = 0;
    PacketWorld::AirCounts air;
    std::vector<PacketWorld::OwnTrace> trace;

    struct Jump {
        std::uint64_t at;
        JumpKind      kind;
        int           x, y;
    };
    std::vector<Jump> jumps;
    Answer        answer = Answer::Physics;
    bool          flying = false;
    std::uint64_t fly_from = 0, land_at = 0;
    int           fly_y0 = 0, land_y = 0;
    double        fly_v = 0.0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> flights;

    // the drop's key
    std::vector<std::uint64_t> drops;
    bool          falling = false;
    std::uint64_t fall_land_at = 0;
    int           fall_x = 0, fall_y = 0;

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
    }
    void own(OwnReport kind, std::uint32_t state, int x, int y, float vy, std::uint64_t at) {
        PacketWorld::OwnTrace r;
        r.ms = at;
        r.kind = kind;
        r.state = state;
        r.x = static_cast<float>(x);
        r.y = static_cast<float>(y);
        r.vy = vy;
        trace.push_back(r);
    }
    bool walking() const { return held.count(VK_LEFT) != 0 || held.count(VK_RIGHT) != 0; }
    int downs_of(std::uint32_t vk, std::uint64_t from = 0, std::uint64_t to = UINT64_MAX) const {
        int n = 0;
        for (const Key& k : keys)
            if (k.vk == vk && k.at >= from && k.at <= to) ++n;
        return n;
    }
    int arrows_in_air() const {
        int n = 0;
        for (const Key& k : keys)
            for (const auto& f : flights)
                if ((k.vk == VK_LEFT || k.vk == VK_RIGHT) && k.at > f.first && k.at < f.second) ++n;
        return n;
    }
    /// Walks taken up, one way then the other: how often the walk turned round.
    int reversals(std::uint64_t until = UINT64_MAX) const {
        int n = 0;
        std::uint32_t last = 0;
        for (const Key& k : keys) {
            if (k.at > until || k.hold != -1 || (k.vk != VK_LEFT && k.vk != VK_RIGHT)) continue;
            if (last != 0 && k.vk != last) ++n;
            last = k.vk;
        }
        return n;
    }
    void walk_on() {
        if (!walking() || flying || falling || page == nullptr) return;
        const int dir = held.count(VK_LEFT) != 0 ? -1 : 1;
        if (dir < 0 && wall_left) return;
        move(dir, step_px);
    }
    void move(int dir, int px) {
        const int to = me_x + dir * px;
        if (const std::optional<int> top = page->follow_to(me_x, me_y, to)) {
            me_x = to;
            me_y = *top - 1;
        } else {
            const MapTrail tr = page->follow(me_x, me_y, dir, px);
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
    int landing(int x, int y, double peak) const {
        int best = y;
        for (const MapFloor& f : page->floors_at(x)) {
            const int rise = y + 1 - f.y;
            if (rise <= 3 || rise > peak) continue;
            if (f.y - 1 < best) best = f.y - 1;
        }
        return best;
    }
    Hunter::JumpKeys jump(JumpKind kind, const std::function<bool()>& go_on) {
        Hunter::JumpKeys k;
        if (!go_on()) return k;
        if (kind == JumpKind::CtrlUp) t += 50;
        k.went = true;
        k.whole = true;
        k.down_ms = k.last_ms = t;
        k.held_ms = 94;
        k.lead_ms = kind == JumpKind::CtrlUp ? 50 : 0;
        k.tail_ms = kind == JumpKind::CtrlUp ? 47 : 0;
        jumps.push_back({ t, kind, me_x, me_y });
        const std::uint64_t key = t;
        t += 94;
        if (kind == JumpKind::UpUp) {
            k.last_ms = key + 340;
            t = key + 340 + 94;
        }
        if (kind == JumpKind::CtrlUp) t += 47;
        if (answer == Answer::Nothing) return k;
        const bool high = kind == JumpKind::CtrlUp;
        const double v = high ? kJumpUpVHigh : kJumpUpV;
        const double g = 2400.0;
        const std::uint64_t off = key + 30;
        own(OwnReport::Air, high ? 2u : 1u, me_x, me_y, high ? -36.0f : -20.0f, off);
        if (high) ++air.high;
        else      ++air.jumps;
        flying = true;
        fly_from = off;
        fly_y0 = me_y;
        fly_v = v;
        report_ms = off;
        const double up_s = v / g, peak = v * v / (2.0 * g);
        int land = me_y;
        double flight = 0.5;
        if (answer == Answer::Physics) {
            land = landing(me_x, me_y, peak);
            flight = up_s + std::sqrt(2.0 * (peak - (me_y - land)) / g);
        }
        land_y = land;
        land_at = off + static_cast<std::uint64_t>(flight * 1000.0);
        flights.push_back({ off, land_at });
        return k;
    }
    void tick() {
        if (flying) {
            const double s = static_cast<double>(t - fly_from) / 1000.0;
            const int y = fly_y0 - static_cast<int>(std::lround(fly_v * s - 2400.0 * s * s / 2.0));
            me_y = std::min(y, std::max(fly_y0, land_y));
            if (t >= land_at) {
                flying = false;
                me_y = land_y;
                own(OwnReport::Air, 0u, me_x, me_y, 0.0f, t);
                ++air.landed;
                report();
                note();
            }
        }
        if (falling && t >= fall_land_at) {
            falling = false;
            me_x = fall_x;
            me_y = fall_y;
            report();
            note();
        }
    }
    /// The whole map's list, as PacketEyes::world lays it: each plate's
    /// point, on the floor under it (the hunter's own monster_floor).
    std::vector<SeekMob> world() const {
        std::vector<SeekMob> out;
        for (const std::vector<Nameplate>* list : { &mobs, &phantoms })
            for (const Nameplate& n : *list) {
                SeekMob s;
                s.slot = n.slot;
                s.gen = n.gen;
                s.at = { n.x(), n.y() };
                s.hp = n.hp;
                const std::optional<MapFloor> on = page->floor_near(n.x(), n.y());
                const std::optional<int> top =
                    on && !on->wall() ? std::optional<int>(on->y) : Hunter::monster_floor(*page, n.x(), n.y());
                s.floor_y = top ? *top : -1;
                s.hurt_by_other = others.count(n.slot) != 0;
                out.push_back(s);
            }
        return out;
    }
};

/// A hunter on the pretend game, over the copies of t52_s2's files, started
/// without a thread. `world`: the whole map's list handed in (PacketEyes's
/// own while the packets steer) -- without it, the hunter as before the seek.
struct SeekRig {
    InputManager         input;
    ExecutionCoordinator coord;
    MobDetector          detector;
    Hunter               hunter{ input, coord, detector };
    GameMapBook          book;
    SeekWorld            w;

    SeekRig(const HunterConfig& cfg, int x, int y, bool world = true) : book(game_data()) {
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
            f.airborne = w.flying || w.falling;
            f.moving = w.walking();
            f.map_known = true;
            f.map = 52;
            f.area = 2;
            f.epoch = w.epoch;
            f.entry_x = 880.0f;
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
            std::vector<Nameplate> out;
            for (const Nameplate& n : w.mobs)
                if (!w.cut || (std::abs(n.x() - w.me_x) <= 614 && std::abs(n.y() - w.me_y) <= 461)) out.push_back(n);
            MobDetector::nearest_first(out, w.me_x, w.me_y);
            return out;
        };
        eyes.takeoffs = [this]() -> std::optional<std::uint64_t> { return w.air.takeoffs() + w.drops.size(); };
        eyes.stands = [this] {
            std::vector<MapTrust::Mob> out;
            for (const Nameplate& n : w.mobs)
                if (n.slot >= 0)
                    out.push_back(MapTrust::Mob{ MapPoint{ n.x(), n.y() }, static_cast<std::uint32_t>(n.slot),
                                                 w.t - 400, n.gen });
            return out;
        };
        eyes.air = [this]() -> std::optional<PacketWorld::AirCounts> { return w.air; };
        eyes.own_since = [this](std::uint64_t ms) {
            std::vector<PacketWorld::OwnTrace> out;
            for (const PacketWorld::OwnTrace& r : w.trace)
                if (r.ms >= ms) out.push_back(r);
            return out;
        };
        if (world) {
            eyes.world = [this](std::vector<SeekMob>& out) {
                out.clear();
                if (!w.world_on) return false;
                out = w.world();
                return true;
            };
        }
        hunter.set_eyes(eyes);

        Hunter::Hands hands;
        hands.focused = [this] { return w.focused; };
        hands.key = [this](std::uint32_t vk, int hold) {
            w.keys.push_back({ w.t, vk, hold });
            if ((vk == VK_LEFT || vk == VK_RIGHT) && !w.flying && !w.falling && w.page != nullptr) {
                w.move(vk == VK_LEFT ? -1 : 1, static_cast<int>(std::lround(hold * kZoneWalkPxPerS / 1000.0)));
                w.report();
            }
            // An attack lands on the nearest plate in reach: kill_after of them kill it.
            if (vk == 'Z' && w.kill_after > 0) {
                Nameplate* hit = nullptr;
                for (Nameplate& n : w.mobs)
                    if (std::abs(n.x() - w.me_x) <= w.reach && std::abs(n.y() - w.me_y) <= 60 &&
                        (hit == nullptr || std::abs(n.x() - w.me_x) < std::abs(hit->x() - w.me_x)))
                        hit = &n;
                if (hit != nullptr) {
                    hit->hurt_ms = w.t;
                    hit->hp -= 1.0f / static_cast<float>(w.kill_after) + 0.001f;
                    if (hit->hp <= 0.0f) {
                        const int slot = hit->slot;
                        w.mobs.erase(std::remove_if(w.mobs.begin(), w.mobs.end(),
                                                    [slot](const Nameplate& n) { return n.slot == slot; }),
                                     w.mobs.end());
                    }
                }
            }
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
        hands.settle_owed = [this] { return !w.owed; };
        hands.up_owed = [this] { return w.up_owed; };
        // Ctrl + ↓: down through the platform onto the first floor under it.
        hands.drop = [this](const std::function<bool()>& go_on) {
            InputManager::DropPress p;
            if (!go_on()) return p;
            w.t += 50;
            p.down_ms = w.t;
            w.drops.push_back(w.t);
            w.t += 90;
            p.held_ms = 90;
            w.t += 50;
            p.sent = InputManager::DropSent::Sent;
            if (const std::optional<MapFloor> below = w.page->floor_below(w.me_x, w.me_y, 8)) {
                w.falling = true;
                w.report_ms = p.down_ms + 30;
                w.fall_x = w.me_x;
                w.fall_y = below->y - 1;
                const double d = std::max(1, w.fall_y - w.me_y);
                w.fall_land_at = p.down_ms + static_cast<std::uint64_t>(std::sqrt(d / 1200.0) * 1000.0);
            }
            return p;
        };
        hands.jump = [this](JumpKind kind, const std::function<bool()>& go_on) { return w.jump(kind, go_on); };
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
    ~SeekRig() { hunter.stop(); }

    void pass() {
        hunter.step();
        w.walk_on();
        w.t += 60;
        w.tick();
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

/// The owner's hunt.ini of 2026-11-05: reach 450, the band 60, ↑ the jump
/// key, packets on, the map tab's area, jump_up and the seek as they ship.
HunterConfig owner_config() {
    HunterConfig c;
    c.reach = 450;
    c.height_tolerance = 60;
    c.jump_key = 38;
    c.jump_to_reach = true;
    c.packet_positions = PacketPositions::On;
    c.spot = true;
    c.spot_tolerance = 48;
    c.spot_return_delay_ms = 1500;
    c.drop_down = false;
    c.hold_walk = true;
    c.give_up_ms = 4000;
    return c;
}

Hunter::Zone owner_zone() {
    Hunter::Zone z;
    z.stem = "t52_s2";
    z.epoch = 7;
    z.home_x = 880.0f;
    z.home_y = 274.0f;
    z.left = 265.0f;
    z.top = 174.0f;
    z.right = 1399.0f;
    z.bottom = 694.0f;
    return z;
}

std::string key_log(const SeekWorld& w) {
    std::string s;
    for (const SeekWorld::Key& k : w.keys) s += fmt("%llu:%u:%d ", static_cast<unsigned long long>(k.at), k.vk, k.hold);
    return s;
}

/// The rig with the zone taken and the map entry's 5 s gone by.
void settle(SeekRig& r) {
    r.hunter.set_zone(owner_zone());
    r.run(Hunter::kDropAfterEntryMs + 300);
}

// ------------------------------------------------------------- the scenarios

void test_left_ground() {
    section("Seek 1: at 709,579 on the left ground -- the monsters there fought from where it stands; no walk right, "
            "no walk back to the home's point, never past 313..986");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the seek on the left ground\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    HunterConfig c = owner_config();
    c.jump_up = false;
    SeekRig r(c, 709, stand_y(p, 709, 579));
    r.w.kill_after = 3;
    std::vector<std::string> log = info_lines([&] {
        settle(r);
        r.w.mobs = { mob_at(435, stand_y(p, 435, 547), 35), mob_at(502, stand_y(p, 502, 567), 16),
                     mob_at(769, 272, 11), mob_at(1122, stand_y(p, 1122, 716), 15) };
        r.run(12'000);
    });
    const Hunter::Status st = r.st();
    check(st.seek_active && r.w.downs_of(VK_RIGHT) == 0 && r.w.downs_of('Z') >= 4 && r.w.min_x >= 313 &&
              r.w.max_x <= 986,
          fmt("seek on, no Right, %d attacks, x stayed %d..%d", r.w.downs_of('Z'), r.w.min_x, r.w.max_x));
    check(lines_with(log, "walking back") == 0 && st.returns == 0,
          "no walk back toward the home's point (\"" + line_with(log, "walking back") + "\")");
    check(lines_with(log, "nothing to seek") >= 1 && st.seek_none && r.w.mobs.size() == 2,
          fmt("the two on the ground killed; the ledge's and the one under the box left: \"%s\"",
              line_with(log, "nothing to seek").c_str()));
}

void test_out_of_view() {
    section("Seek 2: a monster inside the box 660 px off, out of the plates' view -- walked to before its plate is "
            "seen, then fought");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: a monster out of view\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    SeekRig r(owner_config(), 950, stand_y(p, 950, 668));
    bool seen_out_of_view = false;
    std::vector<std::string> log = info_lines([&] {
        settle(r);
        r.w.mobs = { mob_at(290, stand_y(p, 290, 532), 21) };
        r.until(
            [&] {
                const Hunter::Status s = r.st();
                if (s.seek_on && !s.seek_in_view && s.state == Hunter::State::Approaching) seen_out_of_view = true;
                return s.state == Hunter::State::Attacking;
            },
            15'000);
    });
    check(seen_out_of_view && r.st().state == Hunter::State::Attacking && r.w.downs_of(VK_RIGHT) == 0,
          fmt("Approaching with the seek on, out of view; then Attacking from %d,%d", r.w.me_x, r.w.me_y));
    check(lines_with(log, "Hunter: seeking slot 21 gen 1 at 290,", "out of view") == 1,
          "said: \"" + line_with(log, "Hunter: seeking slot 21") + "\"");
}

void test_ramp_foot() {
    section("Seek 3: 23:04:42 replayed -- from 945,666 the ramp's foot monster at 1007,578 hit from up the slope "
            "(walked left to about 849, turned to it near 870), never walked right into the box's bottom edge; those "
            "off their floor or outside never walked to");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the ramp's foot\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    HunterConfig c = owner_config();
    c.jump_up = false;
    SeekRig r(c, 945, stand_y(p, 945, 666));
    std::vector<std::string> log = info_lines([&] {
        settle(r);
        r.w.mobs = { mob_at(1007, stand_y(p, 1007, 578), 18), mob_at(1132, 548, 33), mob_at(1237, 729, 22),
                     mob_at(1429, 589, 31), mob_at(1132, stand_y(p, 1132, 486), 34) };
        r.until([&] { return r.st().state == Hunter::State::Attacking; }, 10'000);
        r.run(2000);
    });
    // 23:05:25's target on the ramp, 1132,486: with jump-ups off, no way to it.
    check(lines_with(log, "Hunter: not going for slot 34 gen 1 at 1132,") == 1 &&
              lines_with(log, "Hunter: not going for slot 33 gen 1 at 1132,548", "on no floor") == 1,
          "the ramp's 1132,486 and the one off its floor never walked toward: \"" +
              line_with(log, "not going for slot 34") + "\"");
    check(r.st().state == Hunter::State::Attacking && r.w.downs_of(VK_RIGHT, 0, UINT64_MAX) <= 1 &&
              r.w.max_x <= 945 && std::abs(r.w.me_x - 873) <= 24 && r.w.min_x >= 840,
          fmt("walked left (to %d), turned once and attacks from %d,%d; never right of 945 (%d)", r.w.min_x, r.w.me_x,
              r.w.me_y, r.w.max_x));
    check(lines_with(log, "not walking right") == 0 && lines_with(log, "walks stopped at the hunting area's edge") == 0,
          "no walk refused at the box's edge");
    check(lines_with(log, "Hunter: seeking slot 18", "attack from 8") == 1 &&
              lines_with(log, "Hunter: seeking slot 18", " px left (your floor;") == 1,
          "said: \"" + line_with(log, "Hunter: seeking slot 18") + "\"");
}

void test_nothing() {
    section("Seek 4: only ledge monsters, jump-ups off -- from 961,672 no key at all, nothing to seek said once in "
            "10 s, stood for inside the box");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: nothing to seek\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    HunterConfig c = owner_config();
    c.jump_up = false;
    SeekRig r(c, 961, stand_y(p, 961, 672));
    // From the first look with the zone: nothing inside it at first, the
    // ledge's two from 3 s on -- one run of "nothing to seek" all along,
    // said as it began and again 10 s on, no sooner.
    std::vector<std::string> log = info_lines([&] {
        r.hunter.set_zone(owner_zone());
        r.run(3000);
        r.w.mobs = { mob_at(683, 273, 12), mob_at(1024, 274, 20) };
        r.run(9000);
    });
    const Hunter::Status st = r.st();
    check(r.w.keys.empty() && st.seek_none && st.state == Hunter::State::NoTargets,
          fmt("no key (%zu), seek_none, NoTargets", r.w.keys.size()));
    check(lines_with(log, "Hunter: nothing to seek for 0.0 s") == 1 &&
              lines_with(log, "Hunter: nothing to seek for 10.") == 1 && lines_with(log, "nothing to seek for") == 2 &&
              lines_with(log, "standing where it is") == 2,
          "said as it began and 10 s on, no more: \"" + line_with(log, "nothing to seek for 10.") + "\"");
    check(st.seek_above_no_way == 2 && st.seek_none_why == Hunter::SeekNone::AboveOff,
          fmt("the two above with no way up (jump-ups off): %d, why %s", st.seek_above_no_way,
              Hunter::seek_none_name(st.seek_none_why)));
    // What the window makes of it: the card's "bật Nhảy lên bục trên", the
    // map tab's two grey with why, and the Ctrl + Up that would get there
    // drawn dashed (2026.11.05b's GUI).
    int above = 0;
    for (const Hunter::Status::SeekGrey& g : st.seek_grey)
        if (g.why == SeekNo::NeedsJumpOff && (g.slot == 12 || g.slot == 20)) ++above;
    const std::optional<JumpArrow> arrow = jump_arrow(st, r.w.t);
    check(above == 2 && seek_above(c, st) == SeekAbove::TickOff && arrow && arrow->would &&
              arrow->kind == JumpKind::CtrlUp && arrow->land.y < arrow->take.y && std::abs(st.seek_you_x - r.w.me_x) <= 2,
          fmt("the card: tick Nhảy lên for them; the tab: both grey (%d), the would-be Ctrl+Up drawn from x %d, the way "
              "worked out from where it stands",
              above, arrow ? arrow->take.x : 0));
    // 05b's dry run with the seek on (the owner's answer 5): the jump it would
    // have gone for, standing with nothing else -- said once, counted, and the
    // card's grey line.
    check(st.jumps_would >= 1 && lines_with(log, "Hunter: a jump up would go here (jump_up is off): Ctrl+Up at x ") == 1 &&
              lines_with(log, "would go here", "(sought)") == 1 && jump_up_card(c, st, r.w.t).would,
          fmt("the dry run with the seek: counted %llu, said once: \"%s\"",
              static_cast<unsigned long long>(st.jumps_would), line_with(log, "would go here").c_str()));
}

void test_jump_seek() {
    section("Seek 5: the same scene with jump-ups on -- Ctrl + Up near 810 for the ledge (JumpUpFor::Seek), landed, "
            "fought on the ledge; no more than 2 presses for one monster");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the seek's jump up\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    {
        SeekRig r(owner_config(), 961, stand_y(p, 961, 672));
        std::uint64_t land = 0;
        std::vector<std::string> log = info_lines([&] {
            settle(r);
            r.w.mobs = { mob_at(683, 273, 12) };
            r.until([&] { return !r.w.jumps.empty(); }, 10'000);
            r.until([&] { return !r.w.flying; }, 3000);
            land = r.w.t;
            r.run(3000);
        });
        const bool one = r.w.jumps.size() == 1 && r.w.jumps[0].kind == JumpKind::CtrlUp;
        check(one && r.w.jumps[0].x >= 778 && r.w.jumps[0].x <= 830 && r.st().jumps_worked == 1,
              fmt("one Ctrl+Up from x %d (about 810), worked", one ? r.w.jumps[0].x : 0));
        check(r.w.me_y == stand_y(p, r.w.me_x, 273) && r.w.downs_of('Z', land) >= 1 && r.w.arrows_in_air() == 0,
              fmt("on the ledge at %d,%d, attacked there (%d), no arrow in the air", r.w.me_x, r.w.me_y,
                  r.w.downs_of('Z', land)));
        check(lines_with(log, "Hunter: jump up (Ctrl+Up) toward slot 12 (sought)") == 1 &&
                  lines_with(log, "Hunter: seeking slot 12", "Ctrl+Up at ") >= 1,
              "said: \"" + line_with(log, "Hunter: seeking slot 12") + "\" -- \"" +
                  line_with(log, "Hunter: jump up (Ctrl+Up)") + "\"");
    }
    {
        // Falls back every time: no more than kJumpUpsPerTarget for it.
        SeekRig r(owner_config(), 961, stand_y(p, 961, 672));
        r.w.answer = Answer::FallBack;
        settle(r);
        r.w.mobs = { mob_at(683, 273, 12) };
        r.run(30'000);
        check(!r.w.jumps.empty() && r.w.jumps.size() <= static_cast<std::size_t>(Hunter::kJumpUpsPerTarget),
              fmt("%zu presses for the one monster (kJumpUpsPerTarget %d)", r.w.jumps.size(), Hunter::kJumpUpsPerTarget));
    }
}

void test_ping_pong() {
    section("Seek 6: two monsters either side, a like way off, drifting 30 px -- the pick kept: at most one turn round");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the pick kept\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    HunterConfig c = owner_config();
    c.reach = 100;
    c.jump_up = false;
    SeekRig r(c, 660, stand_y(p, 660, 580));
    r.w.step_px = 6;
    const auto put = [&](int shift) {
        r.w.mobs = { mob_at(374 + shift, stand_y(p, 374 + shift, 550), 1), mob_at(946 - shift, stand_y(p, 946 - shift, 660), 2) };
    };
    settle(r);
    put(0);
    std::uint64_t first_attack = UINT64_MAX;
    int phase = 0;
    r.run(10'000, [&] {
        if (r.w.t / 500 % 2 != static_cast<std::uint64_t>(phase)) {
            phase = 1 - phase;
            put(phase == 0 ? 30 : -30);
        }
        if (first_attack == UINT64_MAX && r.w.downs_of('Z') > 0) first_attack = r.w.t;
    });
    check(r.w.reversals(first_attack) <= 1 && first_attack != UINT64_MAX,
          fmt("%d turns round before the first attack (at most 1); attacked", r.w.reversals(first_attack)));
}

void test_long_walk() {
    section("Seek 7: a long walk to a monster is not given up at 4 s; one that gets nowhere is passed over after "
            "kSeekStuckMs");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: a long walk\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    HunterConfig c = owner_config();
    c.reach = 150;
    c.jump_up = false;
    {
        SeekRig r(c, 980, stand_y(p, 980, 680));
        r.w.step_px = 3;
        std::vector<std::string> log = info_lines([&] {
            settle(r);
            r.w.mobs = { mob_at(300, stand_y(p, 300, 532), 21) };
            r.until([&] { return r.w.downs_of('Z') > 0; }, 25'000);
        });
        check(r.w.downs_of('Z') > 0 && lines_with(log, "gave up on") == 0 && lines_with(log, "passed over for") == 0,
              fmt("walked from 980 to %d -- 10 s and more at 50 px/s -- and attacked; not given up", r.w.me_x));
    }
    {
        SeekRig r(c, 700, stand_y(p, 700, 580));
        r.w.wall_left = true;
        std::vector<std::string> log = info_lines([&] {
            settle(r);
            r.w.mobs = { mob_at(330, stand_y(p, 330, 532), 21) };
            r.run(8000);
        });
        check(lines_with(log, "Hunter: slot 21 at ", "got no 40 px nearer") == 1 &&
                  lines_with(log, "passed over for 20 s") == 1,
              "a wall on the left: passed over -- \"" + line_with(log, "got no 40 px nearer") + "\"");
    }
}

void test_not_found() {
    section("Seek 8: a monster the map has and the game does not show -- got to, not found there in 1.5 s, passed over; "
            "the next one gone for");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: not found\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    HunterConfig c = owner_config();
    c.jump_up = false;
    SeekRig r(c, 900, stand_y(p, 900, 640));
    std::vector<std::string> log = info_lines([&] {
        settle(r);
        r.w.phantoms = { mob_at(330, stand_y(p, 330, 532), 9) };
        r.run(9000);
        r.w.mobs = { mob_at(950, stand_y(p, 950, 668), 4) };
        r.run(4000);
    });
    check(lines_with(log, "Hunter: slot 9 not found at 330,") == 1 && lines_with(log, "passed over for 20 s") == 1,
          "\"" + line_with(log, "not found at") + "\"");
    check(r.w.downs_of('Z') >= 1, fmt("then the next one, fought (%d attacks)", r.w.downs_of('Z')));
}

void test_drop_compose() {
    section("Seek 9: on the ledge with drops on -- a ground monster under it dropped to (Ctrl + Down), not walked off "
            "the ledge's end to");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: drops and the seek\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    HunterConfig c = owner_config();
    c.drop_down = true;
    c.jump_up = false;
    SeekRig r(c, 600, stand_y(p, 600, 273));
    settle(r);
    r.w.mobs = { mob_at(600, stand_y(p, 600, 578), 5) };
    r.run(8000);
    check(!r.w.drops.empty() && r.w.falls == 0 && r.w.me_y > 500,
          fmt("dropped (%zu), no walk off an end (%d), on the ground at %d,%d", r.w.drops.size(), r.w.falls, r.w.me_x,
              r.w.me_y));
}

void test_unchanged() {
    section("Seek 10: the seek off is the hunter of 05b, key for key; this run's spot, no spot, the screen's plates "
            "and Watch the same with it on");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the setups the seek leaves alone\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    struct Case {
        const char* what;
        int         x, y;
        bool        zone;
        bool        spot;
        PacketPositions pos;
        bool        seek;
        bool        world_on;
    };
    const Case cases[] = {
        { "seek_in_zone off, from 945,666", 945, 666, true, true, PacketPositions::On, false, true },
        { "seek_in_zone off, from 880,634", 880, 634, true, true, PacketPositions::On, false, true },
        { "seek_in_zone off, from 1342,772 (outside)", 1342, 772, true, true, PacketPositions::On, false, true },
        { "this run's spot (no zone), seek on", 709, 579, false, true, PacketPositions::On, true, true },
        { "no spot, seek on", 709, 579, false, false, PacketPositions::On, true, true },
        { "the screen's plates (no list), seek on", 945, 666, true, true, PacketPositions::On, true, false },
        { "Watch, seek on", 945, 666, true, true, PacketPositions::Watch, true, true },
    };
    for (const Case& k : cases) {
        HunterConfig c = owner_config();
        c.seek_in_zone = k.seek;
        c.spot = k.spot;
        c.packet_positions = k.pos;
        const int y = stand_y(p, k.x, k.y);
        SeekRig a(c, k.x, y, true);
        SeekRig b(c, k.x, y, false);
        for (SeekRig* r : { &a, &b }) {
            r->w.world_on = k.world_on;
            r->w.mobs = { mob_at(1007, stand_y(p, 1007, 578), 18), mob_at(435, stand_y(p, 435, 547), 35),
                          mob_at(683, 273, 12) };
            if (k.zone) r->hunter.set_zone(owner_zone());
        }
        a.run(15'000);
        b.run(15'000);
        check(!a.w.keys.empty() && key_log(a.w) == key_log(b.w) && !a.st().seek_active,
              fmt("%s: %zu keys, the same as with no world list; the seek not on", k.what, a.w.keys.size()));
    }
}

void test_seek_logs() {
    section("Seek 11: the monsters' snapshot every 30 s, the minute line's seek part, and the minute cut short said "
            "at the stop; the config");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the seek's log\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    HunterConfig c = owner_config();
    c.jump_up = false;
    SeekRig r(c, 709, stand_y(p, 709, 579));
    std::vector<std::string> log = info_lines([&] {
        settle(r);
        r.w.mobs = { mob_at(683, 273, 12), mob_at(1237, 729, 22) };
        r.run(65'000);
        r.hunter.stop();
    });
    check(lines_with(log, "Hunter: monsters at ") >= 2 && lines_with(log, "Hunter: monsters at ") <= 4 &&
              lines_with(log, "Hunter: monsters at ", "s12#1 683,273") >= 2,
          "a snapshot every 30 s: \"" + line_with(log, "Hunter: monsters at ") + "\"");
    const std::string minute = line_with(log, "; seek: goals ");
    check(minute.find("nothing to seek ") != std::string::npos && minute.find("; time: attacking ") != std::string::npos &&
              minute.find("screen's plates ") != std::string::npos,
          "the minute line: \"" + minute.substr(minute.empty() ? 0 : minute.find("; seek:")) + "\"");
    check(lines_with(log, "Hunter (last ", "hunt stopped): ") == 1 && lines_with(log, "hunt stopped", "; seek: ") == 1,
          "the stop: \"" + line_with(log, "hunt stopped") + "\"");

    // The config: on by default, saved and read back, missing reads on.
    const std::string path = temp_path("seek.ini");
    HunterConfig off;
    off.seek_in_zone = false;
    HunterConfig back;
    const bool saved = off.save(path) && back.load(path);
    std::ofstream(path) << "[Hunt]\njump_key=38\n";
    HunterConfig bare;
    bare.seek_in_zone = false;
    check(HunterConfig{}.seek_in_zone && saved && !back.seek_in_zone && bare.load(path) && bare.seek_in_zone,
          "seek_in_zone: on by default, saved off and read back off, missing from a file -- on");
    DeleteFileA(path.c_str());
}

void test_seek_hold() {
    section("Seek 12: a hold of 6 s during a walk to a monster out of view -- not counted toward stuck or not found, "
            "nothing pressed meanwhile; the walk taken up again and the monster fought");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the seek through a hold\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    SeekRig r(owner_config(), 950, stand_y(p, 950, 668));
    r.w.step_px = 3;
    int held_x = 0, after_x = 0;
    std::size_t keys_at = 0, keys_after = 0;
    std::vector<std::string> log = info_lines([&] {
        settle(r);
        r.w.mobs = { mob_at(290, stand_y(p, 290, 532), 21) };
        r.run(900);
        r.coord.engage_critical("trash cleaning");
        r.run(120);
        held_x = r.w.me_x;
        keys_at = r.w.keys.size();
        r.run(6000);
        after_x = r.w.me_x;
        keys_after = r.w.keys.size();
        r.coord.disengage_critical();
        r.until([&] { return r.st().state == Hunter::State::Attacking; }, 15'000);
    });
    check(held_x < 950 && after_x == held_x && keys_after == keys_at,
          fmt("walking before the hold (to %d), standing through it, no key (%zu)", held_x, keys_after - keys_at));
    check(r.st().state == Hunter::State::Attacking && lines_with(log, "got no 40 px nearer") == 0 &&
              lines_with(log, "not found at") == 0 && lines_with(log, "passed over for") == 0,
          fmt("walked on after it and attacks from %d,%d; never passed over (\"%s\")", r.w.me_x, r.w.me_y,
              line_with(log, "passed over for").c_str()));
}

void test_seek_caps() {
    section("Seek 13: a ledge monster whose jumps up are used up -- kJumpUpsPerTarget presses that did nothing, then "
            "no more walks to the take-off: JumpsUsed, grey, not \"jump-ups off\"");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the seek's jump caps\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    SeekRig r(owner_config(), 961, stand_y(p, 961, 672));
    r.w.answer = Answer::Nothing;
    std::size_t lefts_after = 0;
    std::vector<std::string> log = info_lines([&] {
        settle(r);
        r.w.mobs = { mob_at(683, 273, 12) };
        r.run(3 * 2 * 4000 + 4000);
        // Moved away along the ground: never walked back to the take-off.
        r.w.place(961, stand_y(p, 961, 672));
        const std::uint64_t from = r.w.t;
        r.run(10'000);
        lefts_after = static_cast<std::size_t>(r.w.downs_of(VK_LEFT, from));
    });
    const Hunter::Status st = r.st();
    bool used = false;
    for (const Hunter::Status::SeekGrey& g : st.seek_grey)
        if (g.slot == 12 && g.why == SeekNo::JumpsUsed) used = true;
    check(r.w.jumps.size() == static_cast<std::size_t>(Hunter::kJumpUpsPerTarget) && lefts_after == 0 &&
              lines_with(log, "-- no jump up to it for now") == 0,
          fmt("%zu presses (kJumpUpsPerTarget %d), no walk back toward the take-off after (%zu)", r.w.jumps.size(),
              Hunter::kJumpUpsPerTarget, lefts_after));
    check(used && st.seek_above_no_way == 0 && st.seek_none_why == Hunter::SeekNone::NoWay &&
              seek_above(owner_config(), st) == SeekAbove::None && st.jump_off == Hunter::JumpUpOff::None,
          fmt("grey for JumpsUsed, none counted as above with no way up (%d), nothing to seek: %s",
              st.seek_above_no_way, Hunter::seek_none_name(st.seek_none_why)));
}

void test_seek_arc() {
    section("Seek 14: Ctrl + Up off the ramp's foot peaks over the box's top (the owner's Q3) -- the walk back never "
            "comes first mid-flight, the goal kept through it");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the arc over the box's top\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    SeekRig r(owner_config(), 1200, stand_y(p, 1200, 437));
    bool flew = false, back_first = false, let_go = false;
    int top = INT_MAX;
    std::vector<std::string> log = info_lines([&] {
        settle(r);
        r.w.mobs = { mob_at(1024, 274, 20) };
        r.until([&] { return !r.w.jumps.empty(); }, 10'000);
        r.until(
            [&] {
                const Hunter::Status s = r.st();
                if (r.w.flying) {
                    flew = true;
                    top = std::min(top, r.w.me_y);
                    if (s.spot_back_first) back_first = true;
                    if (!s.seek_on || s.seek_slot != 20) let_go = true;
                }
                return !r.w.flying && flew;
            },
            3000);
        r.run(1500);
    });
    check(flew && r.w.jumps.size() == 1 && r.w.jumps[0].x > 1048 && top < 174 - kZoneOutsideBackPx,
          fmt("one Ctrl+Up from x %d, its peak at %d -- over %d px above the box's top (174)",
              r.w.jumps.empty() ? 0 : r.w.jumps[0].x, top, kZoneOutsideBackPx));
    check(!back_first && !let_go && lines_with(log, "Hunter: seeking slot 20") == 1 &&
              lines_with(log, "not seeking slot 20", "the seek is not on") == 0,
          "in the air: no walk back first, the goal kept, said once: \"" + line_with(log, "Hunter: seeking slot 20") +
              "\"");
}

void test_seek_no_drop() {
    section("Seek 15: on the home ledge with drops on, a ground monster right under it near the gate -- Ctrl + Down "
            "refused there for good (a gate is near), so its own way: off the ledge's end, then fought");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: no drop from above\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    HunterConfig c = owner_config();
    c.drop_down = true;
    c.jump_up = false;
    SeekRig r(c, 880, stand_y(p, 880, 273));
    std::vector<std::string> log = info_lines([&] {
        settle(r);
        r.w.mobs = { mob_at(880, stand_y(p, 880, 634), 6) };
        r.until([&] { return r.w.downs_of('Z') > 0; }, 14'000);
    });
    check(r.w.drops.empty() && r.w.falls >= 1 && r.w.me_y > 450 && r.w.downs_of('Z') >= 1,
          fmt("no drop (%zu), off an end (%d), on the ground at %d,%d and attacking", r.w.drops.size(), r.w.falls,
              r.w.me_x, r.w.me_y));
    check(lines_with(log, "Hunter: slot 6 -- no drop to it from above (a gate is near); its own way instead") == 1,
          "said once: \"" + line_with(log, "no drop to it from above") + "\"");
}

void test_seek_after_fight() {
    section("Seek 16: got to a monster's point before its plate came in reach, fought it over 1.5 s, then it steps "
            "10 px out of reach -- walked after, fought on; never \"not found\"");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: a fight after the walk\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    HunterConfig c = owner_config();
    c.jump_up = false;
    SeekRig r(c, 900, stand_y(p, 900, 640));
    int stood = 0, then_x = 0;
    std::uint64_t moved = 0;
    std::vector<std::string> log = info_lines([&] {
        settle(r);
        // In the map's list only at first: walked to, stood at for its plate.
        r.w.phantoms = { mob_at(330, stand_y(p, 330, 532), 9) };
        r.until([&] { return r.w.min_x < 880 && !r.w.walking(); }, 8000);
        stood = r.w.me_x;
        // Its plate in view, in reach: fought, for more than kSeekArriveMs.
        r.w.mobs = r.w.phantoms;
        r.w.phantoms.clear();
        r.run(3000);
        // A step 10 px out of reach.
        then_x = r.w.me_x;
        r.w.mobs = { mob_at(then_x - 460, stand_y(p, then_x - 460, 532), 9) };
        moved = r.w.t;
        r.run(3000);
    });
    check(r.w.downs_of('Z', 0, moved) >= 1 && r.w.downs_of('Z', moved) >= 1 && r.w.me_x < then_x &&
              then_x - r.w.me_x <= 60,
          fmt("stood at %d, fought, then walked %d px after it and fought on (%d attacks)", stood, then_x - r.w.me_x,
              r.w.downs_of('Z', moved)));
    check(lines_with(log, "not found at") == 0 && lines_with(log, "passed over for") == 0,
          "never passed over: \"" + line_with(log, "passed over for") + "\"");
}

void test_seek_others_in_reach() {
    section("Seek 17: walking away up the slope to the ramp foot's point, the monster in reach -- then another "
            "player hits it (no plan any more): fought where it is, not walked round");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: a plan lost in reach\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    HunterConfig c = owner_config();
    c.jump_up = false;
    SeekRig r(c, 945, stand_y(p, 945, 666));
    std::uint64_t marked = 0;
    std::vector<std::string> log = info_lines([&] {
        settle(r);
        r.w.mobs = { mob_at(1007, stand_y(p, 1007, 578), 18) };
        // Walking left, away from it, until its plate is in reach.
        r.until(
            [&] {
                const int dy = stand_y(p, 1007, 578) - r.w.me_y;
                return r.w.walking() && r.w.me_x < 945 && std::abs(1007 - r.w.me_x) <= 450 && dy >= -60 && dy <= 60;
            },
            6000);
        r.w.others.insert(18);
        marked = r.w.t;
        r.run(1500);
    });
    int walks_right = 0;
    for (const SeekWorld::Key& k : r.w.keys)
        if (k.at >= marked && k.vk == VK_RIGHT && k.hold == -1) ++walks_right;
    check(marked != 0 && r.w.downs_of('Z', marked) >= 1 && walks_right == 0,
          fmt("hit by another player at x %d: attacked (%d), no walk right toward it (%d)", r.w.me_x,
              r.w.downs_of('Z', marked), walks_right));
}

void test_seek_drop_kept() {
    section("Seek 18: on the ledge with drops on, two ground monsters below trading places as nearest -- the drop's "
            "target kept: dropped to, the goal never switched");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the drop's target kept\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    HunterConfig c = owner_config();
    c.drop_down = true;
    c.jump_up = false;
    // From x 1000 on the ledge: a walk of some 280 px left before a drop
    // goes (the gate's 150 px, the drop's 120 across), the two trading
    // places every other look all the way.
    SeekRig r(c, 1000, stand_y(p, 1000, 273));
    const auto put = [&](int shift) {
        r.w.mobs = { mob_at(590 - shift, stand_y(p, 590 - shift, 578), 1),
                     mob_at(630 - shift, stand_y(p, 630 - shift, 578), 2) };
    };
    settle(r);
    put(0);
    int first = -1, switches = 0, looks = 0, phase = 0, n = 0;
    r.until(
        [&] {
            if (++n % 2 == 0) {
                phase = 1 - phase;
                put(phase == 0 ? 0 : 20);
            }
            const Hunter::Status s = r.st();
            if (r.w.drops.empty() && s.seek_on) {
                ++looks;
                if (first < 0) first = s.seek_slot;
                else if (s.seek_slot != first) ++switches;
            }
            return !r.w.drops.empty();
        },
        8000);
    check(!r.w.drops.empty() && first >= 0 && looks >= 6 && switches == 0,
          fmt("dropped (%zu) to slot %d after %d looks with the order trading places, the goal switched %d times "
              "before",
              r.w.drops.size(), first, looks, switches));
}

void test_both_watched() {
    section("Seek 19: a map change with a jump up's and a drop's press both still watched -- both undone and learned, "
            "both switched off, one stop for the window (DropStop::BothMapChanged), whichever came first");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: two presses watched\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    HunterConfig c = owner_config();
    c.seek_in_zone = false;
    c.drop_down = true;
    const auto near_gate = [](const SeekRig& r, int x) {
        for (const MapGate& g : r.book.learned_gates("t52_s2"))
            if (std::abs(g.x - x) <= 30) return true;
        return false;
    };
    {
        // The way back's Ctrl + Up from the ground onto the home's ledge, then a
        // drop from the ledge to a monster under it at 600 -- then the map
        // changes.
        SeekRig r(c, 880, stand_y(p, 880, 634));
        int drop_x = 0;
        std::vector<std::string> log = info_lines([&] {
            settle(r);
            r.until([&] { return !r.w.jumps.empty(); }, 15'000);
            r.until([&] { return !r.w.flying; }, 3000);
            r.w.mobs = { mob_at(600, stand_y(p, 600, 578), 5) };
            r.until([&] { return !r.w.drops.empty(); }, 8000);
            drop_x = r.w.fall_x;
            r.run(300);
            ++r.w.epoch;
            r.run(600);
        });
        const Hunter::Status st = r.st();
        const bool watched = !r.w.jumps.empty() && !r.w.drops.empty() &&
                             r.w.drops[0] < r.w.jumps[0].at + Hunter::kPressWatchMs;
        check(watched && r.hunter.drop_stop() == Hunter::DropStop::BothMapChanged &&
                  st.drop_off == Hunter::DropOff::MapChanged && st.jump_off == Hunter::JumpUpOff::MapChanged &&
                  st.drop_map_changed && st.jump_map_changed,
              fmt("the jump up first (x %d), the drop %llu ms after it: both off, BothMapChanged",
                  r.w.jumps.empty() ? 0 : r.w.jumps[0].x,
                  watched ? static_cast<unsigned long long>(r.w.drops[0] - r.w.jumps[0].at) : 0ull));
        check(lines_with(log, "MAP CHANGED", "after Ctrl+Up at") == 1 && lines_with(log, "MAP CHANGED", "after Ctrl+Down at") == 1 &&
                  near_gate(r, r.w.jumps.empty() ? -999 : r.w.jumps[0].x) && near_gate(r, drop_x),
              "both said, both take-offs learned as gates: \"" + line_with(log, "after Ctrl+Up at") + "\"");
    }
    {
        // The other order: the drop from the ledge at 600 to a ground monster,
        // then -- the monster gone -- the way back's Ctrl + Up within 10 s.
        SeekRig r(c, 600, stand_y(p, 600, 273));
        std::vector<std::string> log = info_lines([&] {
            settle(r);
            r.w.mobs = { mob_at(600, stand_y(p, 600, 578), 5) };
            r.until([&] { return !r.w.drops.empty(); }, 8000);
            r.until([&] { return !r.w.falling; }, 3000);
            r.w.mobs.clear();
            r.until([&] { return !r.w.jumps.empty(); }, 8000);
            r.run(300);
            ++r.w.epoch;
            r.run(600);
        });
        const Hunter::Status st = r.st();
        const bool watched = !r.w.jumps.empty() && !r.w.drops.empty() &&
                             r.w.jumps[0].at < r.w.drops[0] + Hunter::kPressWatchMs;
        check(watched && r.hunter.drop_stop() == Hunter::DropStop::BothMapChanged &&
                  st.drop_off == Hunter::DropOff::MapChanged && st.jump_off == Hunter::JumpUpOff::MapChanged &&
                  lines_with(log, "MAP CHANGED") == 2,
              fmt("the drop first, the jump up %llu ms after it: both off, BothMapChanged",
                  watched ? static_cast<unsigned long long>(r.w.jumps[0].at - r.w.drops[0]) : 0ull));
    }
}

void test_jump_keys_owed() {
    section("Seek 20: a high jump's Up left owed -- the hunt halts with JumpKeysStuck, said as Up and Ctrl, not Down");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: a high jump's key owed\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    SeekRig r(owner_config(), 961, stand_y(p, 961, 672));
    std::vector<std::string> log = info_lines([&] {
        settle(r);
        r.w.owed = true;
        r.w.up_owed = true;
        r.run(1000);
    });
    check(r.hunter.drop_stop() == Hunter::DropStop::JumpKeysStuck && r.st().drop_keys_stuck &&
              lines_with(log, "Hunter: keys owed after a jump up (Up or Ctrl up not sent, ") == 1 &&
              lines_with(log, "keys owed after a drop") == 0,
          "halted, JumpKeysStuck: \"" + line_with(log, "keys owed after") + "\"");
}

void test_seek_source() {
    section("Seek: its source -- no Down, no old jump, no key of its own, nothing written to which way the character "
            "faces");
#ifdef GOBOT_SOURCE_DIR
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + "/src/hunter_seek.cpp", std::ios::binary);
    const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (src.empty()) {
        skip("the seek's source is not here to read\n");
        return;
    }
    bool clean = true;
    std::string named;
    for (const char* bad : { "VK_DOWN", "0x28", ".jump()", "double_jump", "key_jump", "facing_ =", "facing_key",
                             "press(", "hold_down(", "drop_ctrl_down", "kChord", "acquire" }) {
        if (src.find(bad) == std::string::npos) continue;
        clean = false;
        named += std::string(" ") + bad;
    }
    check(clean && src.find("walk(cfg, ") != std::string::npos &&
              src.find("jump_go(cfg, JumpUpFor::Seek, ") != std::string::npos,
          "hunter_seek.cpp walks through walk() and jumps through jump_go(Seek) alone; named:" +
              (named.empty() ? std::string(" none") : named));
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_seek_hunt(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_left_ground();
    test_out_of_view();
    test_ramp_foot();
    test_nothing();
    test_jump_seek();
    test_ping_pong();
    test_long_walk();
    test_not_found();
    test_drop_compose();
    test_unchanged();
    test_seek_logs();
    test_seek_hold();
    test_seek_caps();
    test_seek_arc();
    test_seek_no_drop();
    test_seek_after_fight();
    test_seek_others_in_reach();
    test_seek_drop_kept();
    test_both_watched();
    test_jump_keys_owed();
    test_seek_source();
}
