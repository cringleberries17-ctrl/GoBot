// gobot_test -- the hunter's jump up onto a floor above (2026.11.05b, the
// jump-up plan's §6-§9 and §12), over the game's own t52_s2 and a made-up
// staircase: the way back's Ctrl + ↑ from the ground to the home ledge
// (04d's walk back from 1342,772, replayed), the monster on the ledge seen
// from 600,579, falls back and what they teach, a key the game did not take
// as its jump, the latches, a map change after the key, ↑↑ where it is sure,
// the rule's refusals one at a time, and with jump_up off nothing pressed
// that 05 did not press -- one "would jump" line instead.
//
// Over copies of the game's map files in build\handoff\drop\testdata --
// never the game's install, never the registry -- and skipped when they are
// not there; the staircase is written to this run's temp folder. The hunter
// runs on a pretend game laid one to one over the packets' world: the
// character walks along the real floors, and a jump's key, through the
// hunter's Hands::jump, has it rise and come down as the model says --
// reported as the client reports it, by kind (0x002A states 1, 6, 2 and 0).
// Nothing here presses a real key: every key goes to the pretend game and is
// written down.

#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/hunter.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/jump_plan.hpp"
#include "gobot/logger.hpp"
#include "gobot/map_book.hpp"
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

using Why = Hunter::JumpUpWhy;
using Verdict = Hunter::JumpUpVerdict;

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
    log.set_ring_capacity(6000);
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

// ------------------------------------------------------------- the staircase

std::string temp_path(const char* name) {
    char dir[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return name;
    return std::string(dir) + "gobot_test_" + std::to_string(GetCurrentProcessId()) + "\\" + name;
}

bool write_bytes(const std::string& path, const Bytes& bytes) {
    std::FILE* f = open_file(path, "wb");
    if (f == nullptr) return false;
    const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

/// A planar .map of w x h cells, every record empty as the game's are.
struct MapMaker {
    int   w;
    int   h;
    Bytes b;

    MapMaker(int w_cells, int h_cells)
        : w(w_cells), h(h_cells),
          b(8 + static_cast<std::size_t>(w_cells) * static_cast<std::size_t>(h_cells) * 72, 0xFF) {
        std::memcpy(b.data(), &w, 4);
        std::memcpy(b.data() + 4, &h, 4);
        for (std::size_t r = 8; r < b.size(); r += 36) b[r + 35] = 0;
    }
    void flat(int layer, int x0, int x1, int y, std::uint8_t attr) {
        for (int x = x0; x <= x1; ++x) {
            const std::size_t r = 8 + static_cast<std::size_t>(layer) * static_cast<std::size_t>(w) *
                                          static_cast<std::size_t>(h) * 36 +
                                  (static_cast<std::size_t>(x / 32) * static_cast<std::size_t>(h) +
                                   static_cast<std::size_t>(y / 32)) * 36;
            b[r + 2] = attr;
            b[r + 3 + static_cast<std::size_t>(x % 32)] = static_cast<std::uint8_t>(y % 32);
        }
    }
};

/// A .prj as the game's: a gate kind, and one placing of it.
struct PrjMaker {
    Bytes b;
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void text(const char* s) {
        for (const char* p = s; *p; ++p) b.push_back(static_cast<std::uint8_t>(*p));
        b.push_back(0);
    }
    void kind(std::int32_t id, const char* path) {
        u32(4);
        u32(static_cast<std::uint32_t>(id));
        u32(2);
        text(path);
    }
    void place(std::int32_t kind, std::int32_t x, std::int32_t y) {
        for (int i = 0; i < 20; ++i) b.push_back(i < 6 ? static_cast<std::uint8_t>(0xC0 + i) : 0);
        u32(static_cast<std::uint32_t>(kind));
        b.push_back(0);
        float one = 1.0f;
        std::uint32_t u = 0;
        std::memcpy(&u, &one, 4);
        u32(u);
        u32(static_cast<std::uint32_t>(x));
        u32(static_cast<std::uint32_t>(y));
    }
};

/// t98_s1, made up: solid ground at 1000 and one platform 180 px over it --
/// higher than ↑ lands for sure (109), within ↑↑'s 201 and under its
/// uncertain band -- with a gate by the entry's spawn. Written once a run.
std::string staircase_folder() {
    static std::string dir;
    if (!dir.empty()) return dir;
    dir = temp_path("jump_hunt");
    make_folder(dir);
    make_folder(dir + "\\Map");
    make_folder(dir + "\\Project");
    MapMaker map(80, 40);
    map.flat(0, 0, 2559, 1000, 2);      // the ground, solid
    map.flat(1, 600, 1500, 820, 1);     // the platform
    PrjMaker prj;
    prj.kind(3900000, "\\Data\\OBJ\\Object\\warpzone.spr");
    prj.place(3900000, 2000, 992);
    const bool ok = write_bytes(dir + "\\Map\\t98_s1.map", map.b) && write_bytes(dir + "\\Project\\t98_s1.prj", prj.b);
    if (!ok) dir.clear();
    return dir;
}

/// t97_s1, made up: solid ground at 1000 and one platform 100 px over it --
/// within one jump (Hunter::kOneJumpPx) and a plain ↑'s sure 109 -- with a
/// gate by the entry's spawn. Written once a run.
std::string hop_floor_folder() {
    static std::string dir;
    if (!dir.empty()) return dir;
    dir = temp_path("jump_hop");
    make_folder(dir);
    make_folder(dir + "\\Map");
    make_folder(dir + "\\Project");
    MapMaker map(80, 40);
    map.flat(0, 0, 2559, 1000, 2);      // the ground, solid
    map.flat(1, 600, 1500, 900, 1);     // the platform
    PrjMaker prj;
    prj.kind(3900000, "\\Data\\OBJ\\Object\\warpzone.spr");
    prj.place(3900000, 2000, 992);
    const bool ok = write_bytes(dir + "\\Map\\t97_s1.map", map.b) && write_bytes(dir + "\\Project\\t97_s1.prj", prj.b);
    if (!ok) dir.clear();
    return dir;
}

// ------------------------------------------------------------- the rig

/// What the pretend game makes of the next jump's key.
enum class Answer : std::uint8_t {
    Physics,    ///< up at the key's speed, down onto the highest floor its peak passed
    FallBack,   ///< up and bumped back onto where it stood, 0.5 s on
    Plain,      ///< a plain jump (state 1) whatever the key -- back where it stood
    Nothing,    ///< no take-off at all
    Outside,    ///< comes down at land_x, land_y wherever that is
    MapChange,  ///< a map entry 0.9 s after the key
};

/// The pretend game over a map's own floors, in the packets' world pixels:
/// the plates laid one to one over them.
struct JumpWorld {
    std::uint64_t t = 5'000'000;
    bool          focused = true;
    const GameMapBook::Page* page = nullptr;
    int           me_x = 880, me_y = 273;
    std::vector<Nameplate> mobs;
    int           step_px = 20;
    int           max_x = 0, min_x = INT_MAX;
    std::uint16_t map = 52, area = 2;
    float         entry_x = 880.0f, entry_y = 274.0f;

    std::set<std::uint32_t> held;
    struct Key {
        std::uint64_t at;
        std::uint32_t vk;
        int           hold;     ///< -1: put down and left down
    };
    std::vector<Key> keys;

    float         report_x = 0.0f, report_y = 0.0f;
    std::uint64_t report_ms = 0;
    std::uint32_t epoch = 7;

    // what the packets count and keep of your reports
    PacketWorld::AirCounts air;
    std::vector<PacketWorld::OwnTrace> trace;

    // the jumps
    struct Jump {
        std::uint64_t at;
        JumpKind      kind;
        int           x, y;
    };
    std::vector<Jump> jumps;
    Answer        answer = Answer::Physics;
    std::vector<Answer> answers;    ///< taken one a jump, first, before `answer`
    double        gravity = 2400.0;
    bool          second_cut = false;   ///< ↑↑'s second ↑ not sent
    int           force_x = 0, force_y = 0;
    bool          flying = false;
    std::uint64_t fly_from = 0, land_at = 0, entry_at = 0;
    int           fly_x = 0, fly_y0 = 0, land_y = 0;
    double        fly_v = 0.0;
    bool          cast_in_air = false;  ///< a cast's report mid-flight: settled, at the height it is
    std::vector<std::pair<std::uint64_t, std::uint64_t>> flights;

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
    /// Arrows pressed while a jump was in the air.
    int arrows_in_air() const {
        int n = 0;
        for (const Key& k : keys)
            for (const auto& f : flights)
                if ((k.vk == VK_LEFT || k.vk == VK_RIGHT) && k.at > f.first && k.at < f.second) ++n;
        return n;
    }
    /// Along the floor it stands on -- its slope, and across a hole -- or
    /// off the floor's end onto whatever is under it.
    void walk_on() {
        if (!walking() || flying || page == nullptr) return;
        move(held.count(VK_LEFT) != 0 ? -1 : 1, step_px);
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
                report();
            }
        }
        note();
    }
    /// The highest floor top in column x above the feet at y no higher than
    /// `peak` over them: where a jump straight up comes down.
    int landing(int x, int y, double peak) const {
        int best = y;
        for (const MapFloor& f : page->floors_at(x)) {
            const int rise = y + 1 - f.y;
            if (rise <= 3 || rise > peak) continue;
            if (f.y - 1 < best) best = f.y - 1;
        }
        return best;
    }
    /// The key: the take-off, by kind, reported as the client reports it.
    Hunter::JumpKeys jump(JumpKind kind, const std::function<bool()>& go_on) {
        Hunter::JumpKeys k;
        if (!go_on()) return k;
        Answer a = answer;
        if (!answers.empty()) {
            a = answers.front();
            answers.erase(answers.begin());
        }
        if (kind == JumpKind::CtrlUp) t += 50;     // Ctrl alone
        k.went = true;
        k.whole = !(kind == JumpKind::UpUp && second_cut);
        k.down_ms = k.last_ms = t;
        k.held_ms = 94;
        k.lead_ms = kind == JumpKind::CtrlUp ? 50 : 0;
        k.tail_ms = kind == JumpKind::CtrlUp ? 47 : 0;
        jumps.push_back({ t, kind, me_x, me_y });
        const std::uint64_t key = t;
        t += 94;
        if (kind == JumpKind::UpUp && k.whole) {
            k.last_ms = key + 340;
            t = key + 340 + 94;
        }
        if (kind == JumpKind::CtrlUp) t += 47;
        if (a == Answer::Nothing) return k;
        // Up and away, 30 ms after the key.
        const bool high = kind == JumpKind::CtrlUp && a != Answer::Plain;
        const double v = high ? kJumpUpVHigh : kJumpUpV;
        const std::uint64_t off = key + 30;
        own(OwnReport::Air, high ? 2u : 1u, me_x, me_y, high ? -36.0f : -20.0f, off);
        if (high) ++air.high;
        else      ++air.jumps;
        flying = true;
        fly_from = off;
        fly_x = me_x;
        fly_y0 = me_y;
        fly_v = v;
        report_ms = off;
        double up_s = v / gravity, peak = v * v / (2.0 * gravity);
        if (kind == JumpKind::UpUp && k.whole && a == Answer::Physics) {
            // The second push 340 ms after the first key: -800 again.
            const double t2 = 0.31;
            const double h2 = kJumpUpV * t2 - gravity * t2 * t2 / 2.0;
            own(OwnReport::Air, 6u, me_x, me_y - static_cast<int>(h2), -20.0f, off + 310);
            ++air.second;
            peak = h2 + kJumpUpV * kJumpUpV / (2.0 * gravity);
            up_s = t2 + kJumpUpV / gravity;
        }
        int land = me_y;
        double flight = 0.0;
        if (a == Answer::Physics) {
            land = landing(me_x, me_y, peak);
            flight = up_s + std::sqrt(2.0 * (peak - (me_y - land)) / gravity);
        } else if (a == Answer::FallBack) {
            flight = 0.5;
        } else if (a == Answer::Plain) {
            flight = 2.0 * kJumpUpV / gravity;
        } else if (a == Answer::Outside) {
            flight = 0.9;
        } else if (a == Answer::MapChange) {
            flight = 2.0 * up_s;
            entry_at = key + 900;
        }
        land_y = land;
        if (a == Answer::Outside) land_y = force_y;
        land_at = off + static_cast<std::uint64_t>(flight * 1000.0);
        flights.push_back({ off, land_at });
        if (a == Answer::Outside) force_x = force_x != 0 ? force_x : me_x;
        else                      force_x = 0;
        return k;
    }
    /// One pass of the game: the flight's height, its landing, a map entry.
    void tick() {
        if (entry_at != 0 && t >= entry_at) {
            entry_at = 0;
            ++epoch;
            map = 53;
            area = 1;
            entry_x = 400.0f;
            entry_y = 500.0f;
            flying = false;
            place(400, 500);
        }
        if (!flying) return;
        const double s = static_cast<double>(t - fly_from) / 1000.0;
        const int y = fly_y0 - static_cast<int>(std::lround(fly_v * s - gravity * s * s / 2.0));
        me_y = std::min(y, std::max(fly_y0, land_y));
        if (cast_in_air) {
            own(OwnReport::Cast, 0u, me_x, me_y, 0.0f, t);
            report();
        }
        if (t >= land_at) {
            flying = false;
            if (force_x != 0) me_x = force_x;
            me_y = land_y;
            own(OwnReport::Air, 0u, me_x, me_y, 0.0f, t);
            ++air.landed;
            report();
            note();
        }
    }
};

/// A hunter on the pretend game, over a map's files, started without a
/// thread: every seam its own. `wired`: the air counts and your reports
/// handed in -- without them, the command line's hunter: no jump up ever.
struct JumpRig {
    InputManager         input;
    ExecutionCoordinator coord;
    MobDetector          detector;
    Hunter               hunter{ input, coord, detector };
    GameMapBook          book;
    JumpWorld            w;
    const char*          veto = nullptr;

    JumpRig(const HunterConfig& cfg, int x, int y, bool wired = true, const std::string& data = game_data(),
            const char* stem = "t52_s2")
        : book(data) {
        const std::shared_ptr<const GameMapBook::Page> page = book.get(stem);
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
            f.airborne = w.flying && !w.cast_in_air;
            f.moving = w.walking();
            f.map_known = true;
            f.map = w.map;
            f.area = w.area;
            f.epoch = w.epoch;
            f.entry_x = w.entry_x;
            f.entry_y = w.entry_y;
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
        eyes.takeoffs = [this]() -> std::optional<std::uint64_t> { return w.air.takeoffs(); };
        eyes.stands = [this] {
            std::vector<MapTrust::Mob> out;
            for (const Nameplate& n : w.mobs)
                if (n.slot >= 0)
                    out.push_back(MapTrust::Mob{ MapPoint{ n.x(), n.y() }, static_cast<std::uint32_t>(n.slot),
                                                 w.t - 400, n.gen });
            return out;
        };
        if (wired) {
            eyes.air = [this]() -> std::optional<PacketWorld::AirCounts> { return w.air; };
            eyes.own_since = [this](std::uint64_t ms) {
                std::vector<PacketWorld::OwnTrace> out;
                for (const PacketWorld::OwnTrace& r : w.trace)
                    if (r.ms >= ms) out.push_back(r);
                return out;
            };
        }
        hunter.set_eyes(eyes);

        Hunter::Hands hands;
        hands.focused = [this] { return w.focused; };
        hands.key = [this](std::uint32_t vk, int hold) {
            w.keys.push_back({ w.t, vk, hold });
            // An arrow tapped walks the character as far as it is held.
            if ((vk == VK_LEFT || vk == VK_RIGHT) && !w.flying && w.page != nullptr) {
                w.move(vk == VK_LEFT ? -1 : 1, static_cast<int>(std::lround(hold * kZoneWalkPxPerS / 1000.0)));
                w.report();
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
        hands.settle_owed = [] { return true; };
        hands.drop = [](const std::function<bool()>&) { return InputManager::DropPress{}; };
        hands.jump = [this](JumpKind kind, const std::function<bool()>& go_on) { return w.jump(kind, go_on); };
        hunter.set_hands(hands);

        Hunter::Clock clock;
        clock.now_ms = [this] { return w.t; };
        clock.sleep_ms = [this](std::uint32_t ms) { w.t += ms; };
        hunter.set_clock(clock);

        hunter.set_map_book(&book);
        hunter.set_drop_veto([this] { return veto; });
        hunter.set_config(cfg);
        hunter.start_stepped();
    }
    ~JumpRig() { hunter.stop(); }

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

/// The owner's hunt.ini of 2026-11-04/05: ↑ the jump key, packets on, the
/// map tab's area, jump_up as it ships -- on.
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

Hunter::Zone zone_box(const char* stem, int home_x, int home_y, int left, int top, int right, int bottom) {
    Hunter::Zone z;
    z.stem = stem;
    z.epoch = 7;
    z.home_x = static_cast<float>(home_x);
    z.home_y = static_cast<float>(home_y);
    z.left = static_cast<float>(left);
    z.top = static_cast<float>(top);
    z.right = static_cast<float>(right);
    z.bottom = static_cast<float>(bottom);
    return z;
}
/// spots.ini's [t52_s2] as the owner keeps it: home 880,274 on the spawn ledge.
Hunter::Zone owner_zone() { return zone_box("t52_s2", 880, 274, 265, 174, 1399, 694); }

bool from_map(const JumpRig& r) { return r.hunter.spot().set && r.hunter.spot().from_map; }

/// The keys a rig pressed, as one text: what two hunters did, compared.
std::string key_log(const JumpWorld& w) {
    std::string s;
    for (const JumpWorld::Key& k : w.keys) s += fmt("%llu:%u:%d ", static_cast<unsigned long long>(k.at), k.vk, k.hold);
    return s;
}

// ------------------------------------------------------------- the rule

void test_jump_rule() {
    section("Hunter::jump_up_rule: each refusal in the plan's order, over t52_s2 -- the owner's box, the ground "
            "under the home ledge");
    const std::string data = game_data();
    if (data.empty()) {
        skip("no copies of the game's map files here (build\\handoff\\drop\\testdata): the jump-up rule\n");
        return;
    }
    GameMapBook book(data);
    const std::shared_ptr<const GameMapBook::Page> page = book.get("t52_s2");
    check(page != nullptr, "t52_s2 read");
    if (!page) return;
    HuntZone box;
    box.home_x = 880;
    box.home_y = 274;
    box.left = 265;
    box.top = 174;
    box.right = 1399;
    box.bottom = 694;

    HunterConfig cfg = owner_config();
    // A monster on the ledge at 760,273, the character on the ground at 600,579.
    const auto facts = [&](bool press, int x) {
        Hunter::JumpUpFacts f;
        f.purpose = Hunter::JumpUpFor::Target;
        f.press = press;
        f.now = 100'000;
        f.area = true;
        f.trusted = true;
        f.wired = true;
        f.packet_target = true;
        f.slot = 11;
        f.gen = 1;
        f.dy = 273 - 579;
        f.target_wx = 760.0f;
        f.target_wy = 273.0f;
        f.fix.state = WorldFix::State::Known;
        f.fix.x = f.fix.rx = static_cast<float>(x);
        f.fix.y = f.fix.ry = static_cast<float>(stand_y(*page, x, 579));
        f.fix.report_ms = 99'960;
        f.entry_ms = 10'000;
        f.settle_from_ms = 99'940;
        f.page = page;
        f.box = box;
        f.stand = MapPoint{ x, stand_y(*page, x, 579) };
        f.rules.reach_px = 450;
        return f;
    };

    // It goes: Ctrl + ↑ from about 695 onto the ledge's floor, the monster's.
    const Hunter::JumpUpCall go = Hunter::jump_up_rule(cfg, facts(false, 600));
    check(go.ok() && go.plan && go.plan->kind == JumpKind::CtrlUp && std::abs(go.plan->x - 696) <= 4 &&
              std::abs(go.plan->land_top - 275) <= 2 && go.placed && go.target.y == stand_y(*page, 760, 273),
          fmt("from 600,579 to the monster at 760,273: Ctrl+Up at x %d onto floor %d, the monster's floor %d at x %d "
              "(%s)",
              go.plan ? go.plan->x : 0, go.plan ? go.plan->land_top : 0, go.placed ? go.target.y + 1 : -1, go.target.x,
              Hunter::jump_why_name(go.why)));
    // At the press, stood on the take-off: its column judged afresh.
    const Hunter::JumpUpCall at = Hunter::jump_up_rule(cfg, facts(true, 700));
    check(at.ok() && at.plan && at.plan->x == 700 && at.column.ok() && std::abs(at.column.land_top - 275) <= 2,
          fmt("settled at 700: the column passes, the jump goes from there (%s)", Hunter::jump_why_name(at.why)));
    // Stopped under ledge A (600): refused at the press -- the ceiling.
    const Hunter::JumpUpCall under = Hunter::jump_up_rule(cfg, facts(true, 600));
    check(under.why == Why::OffColumn && under.no == JumpNo::Ceiling,
          fmt("settled at 600, under ledge A: OffColumn (%s) -- never pressed there", jump_no_name(under.no)));

    struct Case {
        const char* what;
        std::function<void(HunterConfig&, Hunter::JumpUpFacts&)> set;
        Why why;
    };
    const Case cases[] = {
        { "jump_up off", [](HunterConfig& c, Hunter::JumpUpFacts&) { c.jump_up = false; }, Why::Off },
        { "packets Watch", [](HunterConfig& c, Hunter::JumpUpFacts&) { c.packet_positions = PacketPositions::Watch; },
          Why::NoPackets },
        { "stay_put", [](HunterConfig& c, Hunter::JumpUpFacts&) { c.stay_put = true; }, Why::StayPut },
        { "no area", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.area = false; }, Why::NoArea },
        { "latched", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.latched = true; }, Why::Latched },
        { "keys owed", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.owed = true; }, Why::KeysOwed },
        { "jump key Space", [](HunterConfig& c, Hunter::JumpUpFacts&) { c.jump_key = 32; }, Why::JumpKey },
        { "not wired", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.wired = false; }, Why::NotWired },
        { "a screen plate", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.packet_target = false; },
          Why::NotPacketTarget },
        { "a monster on the level", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.level_monster = true; },
          Why::LevelMonster },
        { "not above", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.dy = -40; }, Why::NotAbove },
        { "the monster outside", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.target_wx = 1500.0f; },
          Why::OutsideArea },
        { "not known", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.fix.state = WorldFix::State::Lost; },
          Why::NotKnown },
        { "after an entry", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.entry_ms = 98'000; }, Why::AfterEntry },
        { "in the air", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.in_air = true; }, Why::InAir },
        { "too soon", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.last_press_ms = 99'000; }, Why::TooSoon },
        { "after a drop", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.last_drop_worked_ms = 95'000; },
          Why::AfterDrop },
        { "a step back", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.step_judge = true; }, Why::StepJudge },
        { "the minute's", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.presses_minute = 4; }, Why::MinuteCap },
        { "the monster's", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.presses_target = 2; }, Why::TargetCap },
        { "its settles", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.settles_target = 2; }, Why::TargetCap },
        { "a hold", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.held = true; }, Why::Held },
        { "the game behind", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.focused = false; }, Why::Unfocused },
        { "pick-ups", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.loot_owed = true; }, Why::Loot },
        { "the veto (HP)", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.veto = "HP is low"; }, Why::Veto },
        { "the map not trusted", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.trusted = false; }, Why::MapUnknown },
        { "off every floor", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.stand.reset(); }, Why::OffFloor },
        { "the attack key Ctrl", [](HunterConfig& c, Hunter::JumpUpFacts&) { c.attack_key = 0x11; }, Why::Plan },
        { "Ctrl+Up off for the run", [](HunterConfig&, Hunter::JumpUpFacts& f) { f.kinds_off = 4; }, Why::Plan },
    };
    for (const Case& k : cases) {
        HunterConfig c = cfg;
        Hunter::JumpUpFacts f = facts(false, 600);
        k.set(c, f);
        const Hunter::JumpUpCall call = Hunter::jump_up_rule(c, f);
        check(call.why == k.why, fmt("%s: %s (got %s)", k.what, Hunter::jump_why_name(k.why),
                                     Hunter::jump_why_name(call.why)));
    }
    // At the press only: not settled, the stop not reported, an arrow down.
    {
        Hunter::JumpUpFacts f = facts(true, 700);
        f.fix.moving = true;
        Hunter::JumpUpFacts g = facts(true, 700);
        g.walk_held = true;
        g.fix.report_ms = 99'900;
        Hunter::JumpUpFacts h = facts(true, 700);
        h.left_held = true;
        Hunter::JumpUpFacts a = facts(false, 600);
        a.fix.moving = true;
        a.left_held = true;
        check(Hunter::jump_up_rule(cfg, f).why == Why::NotSettled &&
                  Hunter::jump_up_rule(cfg, g).why == Why::NotSettled &&
                  Hunter::jump_up_rule(cfg, h).why == Why::Walking && Hunter::jump_up_rule(cfg, a).ok(),
              "at the press: walking, or its stop not reported since the settle began, or an arrow down -- refused; "
              "walked to, none of them");
    }
    // On its own floor: walked to. A monster up the ground's slope from
    // 987,683 (700,579).
    {
        Hunter::JumpUpFacts f = facts(false, 987);
        f.stand = MapPoint{ 987, stand_y(*page, 987, 683) };
        f.fix.x = f.fix.rx = 987.0f;
        f.fix.y = f.fix.ry = static_cast<float>(f.stand->y);
        f.dy = 579 - f.stand->y;
        f.target_wx = 700.0f;
        f.target_wy = 579.0f;
        check(Hunter::jump_up_rule(cfg, f).why == Why::OnOwnFloor, "a monster up the character's own slope: walked to");
    }
    // The way back's leg: as given, the target's rules not asked; AfterDrop
    // not either. Seek: the monster's caps.
    {
        Hunter::JumpUpFacts f = facts(false, 880);
        f.purpose = Hunter::JumpUpFor::Home;
        f.packet_target = false;
        f.slot = -1;
        f.last_drop_worked_ms = 95'000;
        f.stand = MapPoint{ 880, stand_y(*page, 880, 634) };
        JumpRules up;
        up.reach_px = 450;
        f.plan = plan_jump_to(*page, box, *f.stand, MapPoint{ 880, 273 }, up);
        const Hunter::JumpUpCall home = Hunter::jump_up_rule(cfg, f);
        Hunter::JumpUpFacts s = f;
        s.purpose = Hunter::JumpUpFor::Seek;
        s.packet_target = true;
        s.slot = 4;
        s.target_wx = 880.0f;
        s.target_wy = 273.0f;
        check(home.ok() && home.plan && home.plan->kind == JumpKind::CtrlUp &&
                  Hunter::jump_up_rule(cfg, s).why == Why::AfterDrop,
              fmt("Home: the leg as given, Ctrl+Up at %d, a drop a moment ago not in its way; Seek: AfterDrop",
                  home.plan ? home.plan->x : 0));
        Hunter::JumpUpFacts none = f;
        none.plan.reset();
        check(Hunter::jump_up_rule(cfg, none).why == Why::Plan, "Home with no leg: Plan");
    }

    // The drop's side: no drop for kJumpUpStayMs after a jump up toward a
    // target that worked (DropWhy::AfterJump, appended).
    {
        Hunter::DropFacts d;
        d.now = 100'000;
        d.wired = true;
        d.packet_target = true;
        d.dy = 300;
        d.fix.state = WorldFix::State::Known;
        d.entry_ms = 10'000;
        d.last_jump_worked_ms = 95'000;
        HunterConfig dc = cfg;
        dc.drop_down = true;
        dc.spot = false;
        const Hunter::DropCall call = Hunter::drop_rule(dc, d);
        check(call.why == Hunter::DropWhy::AfterJump &&
                  static_cast<int>(Hunter::DropWhy::AfterJump) == static_cast<int>(Hunter::DropWhy::NoDropHere) + 1 &&
                  std::string(Hunter::drop_why_name(Hunter::DropWhy::AfterJump)).find("jump up") != std::string::npos,
              fmt("a drop 5 s after a jump up that worked: AfterJump, appended after NoDropHere (%s)",
                  Hunter::drop_why_name(call.why)));
    }
}

// ------------------------------------------------------------- the way back

void test_jump_way_back() {
    section("Hunter 05b: the way back by Ctrl + Up -- 04d's walk back from 1342,772 replayed: along the ground, "
            "settled near 810, one high jump, onto the ledge, home");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the way back by a jump up\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");

    JumpRig r(owner_config(), 1342, stand_y(p, 1342, 772));
    r.hunter.set_zone(owner_zone());
    bool used = false;
    std::uint64_t first_key = 0;
    // What the card says of the way back on its way there: a jump up, the
    // one it starts with Ctrl + Up near 810 (2026.11.05b's GUI).
    bool said_jump = false;
    // The first look too: the walk back first goes at once (2026.11.10).
    std::vector<std::string> log = info_lines([&] {
        r.pass();
        used = from_map(r);
        r.until([&] {
            const Hunter::Status s = r.st();
            said_jump = said_jump || (s.spot_way_by_jumps && s.spot_way_jumps == 1 && s.spot_way_jump_first &&
                                      s.spot_way_jump_kind == JumpKind::CtrlUp && s.spot_way_jump_x >= 778 &&
                                      s.spot_way_jump_x <= 830);
            return !r.w.jumps.empty();
        }, 12'000);
        if (!r.w.jumps.empty()) first_key = r.w.jumps.front().at;
        r.until([&] { return r.st().spot_home && !r.w.walking(); }, 8'000);
        r.run(1500);
    });
    const Hunter::Status st = r.st();
    const bool one = r.w.jumps.size() == 1 && r.w.jumps[0].kind == JumpKind::CtrlUp;
    const int jx = one ? r.w.jumps[0].x : 0;
    check(used && one && jx >= 790 && jx <= 830 && r.w.jumps[0].y == stand_y(p, jx, 604),
          fmt("one Ctrl+Up, from %d,%d on the ground (about 810)", jx, one ? r.w.jumps[0].y : 0));
    check(st.spot_home && std::abs(r.w.me_x - 880) <= 48 && r.w.me_y == stand_y(p, r.w.me_x, 273) && st.jumps == 1 &&
              st.jumps_worked == 1,
          fmt("landed on the ledge and walked home: %d,%d, home %d (%llu worked)", r.w.me_x, r.w.me_y,
              st.spot_home ? 1 : 0, static_cast<unsigned long long>(st.jumps_worked)));
    check(r.w.arrows_in_air() == 0 && r.w.downs_of(38) == 0 && r.w.downs_of(VK_DOWN) == 0 &&
              r.w.downs_of(VK_LEFT, 0, first_key) >= 1 && r.w.downs_of(VK_RIGHT, 0, first_key) == 0,
          fmt("walked left to it, no hop (%d), no Down, no arrow in the air (%d)", r.w.downs_of(38),
              r.w.arrows_in_air()));
    check(st.return_give_ups == 0 && lines_with(log, "got nowhere") == 0,
          "the give-up clock stood while it waited for the map entry's 5 s, settled and flew: no walk back given up");
    check(said_jump, "the status on the way: the way back worked out with jumps, one, starting with Ctrl+Up near 810 "
                     "-- the card's \"Về: nhảy Ctrl+↑ ở x ...\"");
    // The walk back first goes at the first look (2026.11.10), before the
    // map is trusted for jumps: it starts along the ground, and the way is
    // asked again with the jump once it is -- one walk back, one start line.
    check(lines_with(log, "Hunter: walking back into the hunting area from 1342,") == 1 &&
              lines_with(log, "Hunter: jump up (Ctrl+Up) on the way home: from ", "onto floor 27") == 1 &&
              lines_with(log, "Hunter: jump up worked: landed at ", "on floor 27") == 1 &&
              lines_with(log, "came up short") == 0 && lines_with(log, "bumped from below") == 0,
          "said: \"" + line_with(log, "Hunter: jump up (Ctrl+Up)") + "\" -- \"" + line_with(log, "jump up worked") +
              "\"");

    // From 880,634 under the home: the same jump, 70-odd px left.
    JumpRig u(owner_config(), 880, stand_y(p, 880, 634));
    u.hunter.set_zone(owner_zone());
    u.until([&] { return u.st().spot_home && !u.w.walking() && u.w.me_y < 300; }, 12'000);
    check(u.w.jumps.size() == 1 && u.w.jumps[0].x >= 778 && u.w.jumps[0].x <= 830 && u.st().spot_home,
          fmt("from 880,634: one Ctrl+Up at %d (the take-off about 802, stopped within %d of it), home",
              u.w.jumps.empty() ? 0 : u.w.jumps[0].x, Hunter::kJumpUpLeadPx));
}

// ------------------------------------------------------------- a target above

void test_jump_target() {
    section("Hunter 05b: a monster on the ledge seen from 600,579 -- walked right to about 695, never pressed from "
            "505..638, attacked in the air with no turn and no step back, then fought on the ledge");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: a monster above\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");

    HunterConfig c = owner_config();
    c.spot_return_delay_ms = 60'000;
    c.keep_distance = true;
    JumpRig r(c, 600, stand_y(p, 600, 579));
    r.hunter.set_zone(owner_zone());
    r.run(Hunter::kDropAfterEntryMs + 300);
    const bool used = from_map(r) && r.w.jumps.empty();
    r.w.mobs = { mob_at(560, 273, 11) };
    std::uint64_t key = 0, land = 0;
    int air_attacks = 0;
    std::vector<std::string> log = info_lines([&] {
        r.until([&] { return !r.w.jumps.empty(); }, 6000);
        if (r.w.jumps.empty()) return;
        key = r.w.jumps.front().at;
        r.until([&] { return !r.w.flying; }, 3000);
        land = r.w.t;
        air_attacks = r.w.downs_of('Z', key, land);
        r.run(3000);
    });
    const bool one = r.w.jumps.size() == 1;
    const int jx = one ? r.w.jumps[0].x : 0;
    check(used && one && r.w.jumps[0].kind == JumpKind::CtrlUp && jx >= 663 && jx <= 740 && (jx < 505 || jx > 638),
          fmt("one Ctrl+Up from x %d (about 695; never 505..638)", jx));
    check(air_attacks >= 1 && r.w.arrows_in_air() == 0,
          fmt("attacked in the air (%d) with no arrow -- no turn's tap, no step back", air_attacks));
    check(r.w.me_y == stand_y(p, r.w.me_x, 273) && r.st().jumps_worked == 1 && r.w.downs_of('Z', land) >= 2 &&
              r.w.downs_of(VK_LEFT, land) >= 1,
          fmt("landed on the ledge at %d,%d and fought there: turned left after the landing, %d attacks", r.w.me_x,
              r.w.me_y, r.w.downs_of('Z', land)));
    check(r.w.downs_of(38) == 0 && r.w.downs_of(VK_DOWN) == 0, "no hop, no Down");
    check(lines_with(log, "Hunter: jump up (Ctrl+Up) toward the monster: from ") == 1,
          "said: \"" + line_with(log, "Hunter: jump up (Ctrl+Up)") + "\"");
    // The minute line: the looks, the press by key, what came of it, g≈.
    const std::vector<std::string> minute = info_lines([&] { r.run(60'000); });
    const std::string m = line_with(minute, " fights, ");
    const std::size_t part = m.find("; jump-ups");
    check(part != std::string::npos &&
              m.find("pressed 1 (Up 0, Up,Up 0, Ctrl+Up 1), worked 1, fell back 0") != std::string::npos &&
              m.find("; g≈") != std::string::npos,
          "the minute line: \"" + m.substr(part == std::string::npos ? 0 : part) + "\"");
}

// ------------------------------------------------------------- what goes wrong

void test_jump_fails() {
    section("Hunter 05b: falls back and what they teach, a key not taken as its jump, Nothing three times, Physics, "
            "a map change after the key");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: the jumps that go wrong\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");
    const int ground = stand_y(p, 880, 634);

    // Fall back twice from under the home: the point learned and the next
    // press 48 px away at least, the gap kept; the second onto the same floor
    // makes it solid -- no third press.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::FallBack;
        r.hunter.set_zone(owner_zone());
        std::vector<std::string> log = info_lines([&] { r.run(25'000); });
        const auto& j = r.w.jumps;
        check(j.size() == 2 && std::abs(j[1].x - j[0].x) >= GameMapBook::kNoJumpXPx &&
                  j[1].at >= j[0].at + Hunter::kDropGapMs,
              fmt("two presses: at %d, then %d -- %d px apart, %llu ms apart", j.empty() ? 0 : j[0].x,
                  j.size() > 1 ? j[1].x : 0, j.size() > 1 ? std::abs(j[1].x - j[0].x) : 0,
                  j.size() > 1 ? static_cast<unsigned long long>(j[1].at - j[0].at) : 0ull));
        check(r.book.no_jumps("t52_s2").size() == 2 && r.book.solid_floors("t52_s2").size() == 1 &&
                  r.st().jumps_fell_back == 2,
              fmt("learned: %zu no-jump points, %zu floor solid from below", r.book.no_jumps("t52_s2").size(),
                  r.book.solid_floors("t52_s2").size()));
        check(lines_with(log, "Hunter: jump up FELL BACK to ", "bumped from below) -- no Ctrl+Up to floor 27") == 2 &&
                  lines_with(log, "Hunter: floor 27", "taken as solid from below this session") == 1 &&
                  r.st().jump_off == Hunter::JumpUpOff::None,
              "said: \"" + line_with(log, "FELL BACK") + "\" -- \"" + line_with(log, "solid from below") +
                  "\"; a bump's g not counted against the model");
        check(r.w.downs_of(VK_DOWN) == 0 && r.w.arrows_in_air() == 0, "no Down, no arrow in the air");
    }
    // Nothing three times: a fail each, not learned -- off until the next start.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::Nothing;
        r.hunter.set_zone(owner_zone());
        std::vector<std::string> log = info_lines([&] { r.run(25'000); });
        check(r.w.jumps.size() == 3 && r.st().jump_off == Hunter::JumpUpOff::Fails && r.book.no_jumps("t52_s2").empty(),
              fmt("%zu presses that did nothing: jumps up off (%s), nothing learned", r.w.jumps.size(),
                  Hunter::jump_off_name(r.st().jump_off)));
        check(lines_with(log, "did nothing in 900 ms") == 3 &&
                  lines_with(log, "3 jumps up in a row did not get there -- jumps up OFF until the next BẮT ĐẦU") == 1,
              "said: \"" + line_with(log, "did not get there") + "\"");
        // A new start: on again.
        r.hunter.stop();
        r.hunter.start_stepped();
        check(r.st().jump_off == Hunter::JumpUpOff::None, "BẮT ĐẦU again: on");
    }
    // Ctrl + ↑ taken as a plain jump twice: Ctrl + ↑ off for the run.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::Plain;
        r.hunter.set_zone(owner_zone());
        std::vector<std::string> log = info_lines([&] { r.run(25'000); });
        check(r.w.jumps.size() == 2 && (r.st().jump_kinds_off & 4) != 0 && r.book.no_jumps("t52_s2").empty(),
              fmt("%zu Ctrl+Up the game took as a plain jump: Ctrl+Up off (%u), nothing learned", r.w.jumps.size(),
                  static_cast<unsigned>(r.st().jump_kinds_off)));
        check(lines_with(log, "Ctrl+Up came out as a plain jump (state 1, no state 2) -- 1 of 2") == 1 &&
                  lines_with(log, "Ctrl+Up OFF until the next BẮT ĐẦU") == 1,
              "said: \"" + line_with(log, "came out as") + "\"");
    }
    // Landed outside the area: Physics, off.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::Outside;
        r.w.force_x = 586;
        r.w.force_y = 163;
        r.hunter.set_zone(owner_zone());
        r.run(12'000);
        check(r.w.jumps.size() == 1 && r.st().jump_off == Hunter::JumpUpOff::Physics,
              fmt("landed on ledge A, outside the box: jumps up off -- %s", Hunter::jump_off_name(r.st().jump_off)));
    }
    // g twice out of 2100..2800: Physics.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.gravity = 1800.0;
        r.w.answer = Answer::Physics;
        r.hunter.set_zone(owner_zone());
        std::vector<std::string> log = info_lines([&] { r.run(9000); }, LogLevel::Info);
        const bool one = r.w.jumps.size() == 1;
        check(one && r.st().jump_off == Hunter::JumpUpOff::None && lines_with(log, "jump up worked", "g≈1") == 1,
              "a flight under g=1800: measured, said (\"" + line_with(log, "g≈") + "\"), not yet off");
        // A second such flight in the run: off -- the jumps are not what
        // the model says.
        r.hunter.set_zone(owner_zone());
        r.w.place(880, ground);
        r.run(12'000);
        check(r.w.jumps.size() == 2 && r.st().jump_off == Hunter::JumpUpOff::Physics,
              fmt("a second flight under g=1800: Physics (%s, %zu jumps)", Hunter::jump_off_name(r.st().jump_off),
                  r.w.jumps.size()));
    }
    // A map change 0.9 s after the key: halted, the hunt to stop and
    // jump_up to be unticked; the take-off learned as a gate.
    {
        JumpRig r(owner_config(), 880, ground);
        r.w.answer = Answer::MapChange;
        r.hunter.set_zone(owner_zone());
        std::vector<std::string> log = info_lines([&] { r.run(12'000); });
        const std::vector<MapGate> learned = r.book.learned_gates("t52_s2");
        const bool gate = !r.w.jumps.empty() && std::any_of(learned.begin(), learned.end(), [&](const MapGate& g) {
            return g.x == r.w.jumps[0].x && g.y == r.w.jumps[0].y;
        });
        check(r.w.jumps.size() == 1 && r.hunter.drop_stop() == Hunter::DropStop::JumpMapChanged &&
                  r.st().jump_map_changed && r.st().jump_off == Hunter::JumpUpOff::MapChanged && gate &&
                  r.book.learned_gates("t53_s1").size() == 1,
              fmt("drop_stop() JumpMapChanged, jumps off, the take-off %d learned as a gate, and the entry it led to",
                  r.w.jumps.empty() ? 0 : r.w.jumps[0].x));
        check(lines_with(log, "Hunter: MAP CHANGED ", "s after Ctrl+Up at ") == 1 &&
                  lines_with(log, "Hunter: MAP CHANGED ", "Nhảy lên to be unticked") == 1,
              "said: \"" + line_with(log, "MAP CHANGED") + "\"");
        const std::size_t keys = r.w.keys.size();
        r.run(2000);
        check(r.w.keys.size() == keys, "halted: no key after it");
    }
}

// ------------------------------------------------------------- the staircase

void test_jump_twice() {
    section("Hunter 05b: Up,Up where it is sure -- a made-up platform 180 px over solid ground; its second Up not "
            "sent, judged as a plain Up");
    const std::string dir = staircase_folder();
    check(!dir.empty(), "the made-up t98_s1 written to this run's temp folder");
    if (dir.empty()) return;
    HunterConfig c = owner_config();
    const auto rig = [&](JumpRig& r) {
        r.w.map = 98;
        r.w.area = 1;
        r.w.entry_x = 2000.0f;
        r.w.entry_y = 999.0f;
        r.hunter.set_zone(zone_box("t98_s1", 1000, 819, 200, 400, 2400, 1050));
    };
    {
        JumpRig r(c, 1000, 999, true, dir, "t98_s1");
        rig(r);
        r.until([&] { return r.st().spot_home && r.w.me_y == 819 && !r.w.walking(); }, 14'000);
        check(from_map(r) && r.w.jumps.size() == 1 && r.w.jumps[0].kind == JumpKind::UpUp && r.st().jumps_worked == 1 &&
                  r.st().spot_home,
              fmt("from the ground at 1000: one Up,Up (%s), onto the platform, home",
                  r.w.jumps.empty() ? "none" : jump_kind_name(r.w.jumps[0].kind)));
    }
    {
        JumpRig r(c, 1000, 999, true, dir, "t98_s1");
        rig(r);
        r.w.second_cut = true;
        std::vector<std::string> log = info_lines([&] {
            r.until([&] { return !r.w.jumps.empty() && !r.w.flying; }, 12'000);
            r.run(600);
        });
        check(r.w.jumps.size() == 1 && r.st().jumps_fell_back == 0 && r.book.no_jumps("t98_s1").empty() &&
                  r.st().jump_off == Hunter::JumpUpOff::None &&
                  lines_with(log, "second Up not sent (a hold, or too late) -- judged as a plain Up") == 1,
              "the second Up not sent: a plain Up, back where it stood -- no fall back counted, nothing learned (\"" +
                  line_with(log, "second Up not sent") + "\")");
    }
}

// ------------------------------------------------------------- off, and the rest

void test_jump_off() {
    section("Hunter 05b: jump_up off -- the same keys as the hunter without jumps, one \"would jump\" line; the "
            "config; the map book's learning; MapTrust in the air");
    if (game_data().empty()) {
        skip("no copies of the game's map files here: jump_up off\n");
        return;
    }
    GameMapBook probe(game_data());
    const GameMapBook::Page& p = *probe.get("t52_s2");

    // T-J8: jump_up off, wired, against the hunter with no air counts at all
    // (the command line's): the same keys at the same times.
    for (const int start : { 0, 1 }) {
        HunterConfig off = owner_config();
        off.jump_up = false;
        const int x = start == 0 ? 1342 : 600, y = start == 0 ? 772 : 579;
        JumpRig a(off, x, stand_y(p, x, y));
        JumpRig b(owner_config(), x, stand_y(p, x, y), false);
        for (JumpRig* r : { &a, &b }) {
            r->hunter.set_zone(owner_zone());
            if (start == 1) r->w.mobs = { mob_at(560, 273, 11), mob_at(300, stand_y(p, 300, 531), 12) };
        }
        std::vector<std::string> log = info_lines([&] {
            a.run(20'000);
            b.run(20'000);
        });
        check(!a.w.keys.empty() && key_log(a.w) == key_log(b.w) && a.w.jumps.empty() && b.w.jumps.empty(),
              fmt("from %d,%d: %zu keys, the same with jump_up off as with no jumps wired; no jump", x, y,
                  a.w.keys.size()));
        check(lines_with(log, "Hunter: a jump up would go here (jump_up is off): Ctrl+Up at x ") == 1 &&
                  a.st().jumps_would >= 1 && b.st().jumps_would == 0,
              "said once: \"" + line_with(log, "would go here") + "\"");
        // And kept for the map tab's dashed arrow, as worked out at the last pass.
        check(a.st().jump_would_ms != 0 && a.st().jump_would_plan.kind == JumpKind::CtrlUp &&
                  a.st().jump_would_plan.land_top < a.st().jump_would_plan.feet_y && b.st().jump_would_ms == 0,
              fmt("the jump that would have gone kept for the map tab: Ctrl+Up at x %d onto floor %d",
                  a.st().jump_would_plan.x, a.st().jump_would_plan.land_top));
    }

    // The config: saved and read back; a file without it reads on; the
    // shipped hunt.ini says jump_up=true.
    {
        const std::string path = temp_path("jump_up.ini");
        HunterConfig c;
        c.jump_up = false;
        HunterConfig back;
        const bool saved = c.save(path) && back.load(path);
        std::ofstream(path) << "[Hunt]\njump_key=38\n";
        HunterConfig bare;
        bare.jump_up = false;
        check(HunterConfig{}.jump_up && saved && !back.jump_up && bare.load(path) && bare.jump_up,
              "jump_up: on by default, saved off and read back off, missing from a file -- on");
        DeleteFileA(path.c_str());
#ifdef GOBOT_SOURCE_DIR
        IniFile ini;
        HunterConfig ship;
        const std::string shipped = std::string(GOBOT_SOURCE_DIR) + "\\configs\\hunt.ini";
        check(ini.load(shipped) && ini.has_key("Hunt", "jump_up") && ship.load(shipped) && ship.jump_up,
              "the shipped hunt.ini: jump_up=true (the owner's answer 5)");
#endif
    }

    // The map book: no-jump points and floors solid from below, learned and
    // taken back.
    {
        GameMapBook book{ std::string() };     // no data folder, never looked up
        book.learn_no_jump("t52_s2", GameMapBook::NoJump{ MapPoint{ 810, 603 }, MapPoint{ 810, 273 }, 2 });
        book.learn_solid("t52_s2", MapPoint{ 810, 273 });
        book.learn_solid("t52_s2", MapPoint{ 810, 273 });
        const bool had = book.no_jumps("t52_s2").size() == 1 && book.solid_floors("t52_s2").size() == 1 &&
                         book.no_jumps("t52_s1").empty();
        check(had && book.unlearn_no_jump("t52_s2", MapPoint{ 810, 603 }) &&
                  !book.unlearn_no_jump("t52_s2", MapPoint{ 810, 603 }) &&
                  book.unlearn_solid("t52_s2", MapPoint{ 810, 273 }) &&
                  book.no_jumps("t52_s2").empty() && book.solid_floors("t52_s2").empty(),
              "GameMapBook: a no-jump point and a solid floor, once each, per map; taken back by their points");
    }

    // A cast in the air reports you settled at the height you are: never a
    // standing point for MapTrust -- the area's file stays trusted.
    {
        JumpRig r(owner_config(), 880, stand_y(p, 880, 634));
        r.w.cast_in_air = true;
        r.hunter.set_zone(owner_zone());
        r.until([&] { return !r.w.jumps.empty(); }, 12'000);
        r.run(1200);
        const MapTrust::Report t = r.st().drop_map;
        check(!r.w.jumps.empty() && t.why == MapTrust::Why::Trusted && !t.own_off && !r.book.distrusted("t52_s2"),
              fmt("casts reported mid-flight: the file still trusted (%s)", MapTrust::why_text(t.why)));
    }
}

#ifdef GOBOT_SOURCE_DIR
std::string source(const char* rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
#endif

void test_jump_hop() {
    section("Hunter 05b: the old hop on a walk back the map has no jump in (as_before) -- kept where jumps up cannot "
            "be planned (another jump key, Watch), as 2026.11.05; off with jump_up where they can");
    const std::string dir = hop_floor_folder();
    check(!dir.empty(), "the made-up t97_s1 written to this run's temp folder");
    if (dir.empty()) return;
    // The home on the platform 100 px over the ground, the box round the
    // platform alone: from the ground at 400, 94 px under the box, no walk or
    // fall gets in -- the home, one jump above, walked toward as before.
    struct Case {
        const char*     what;
        std::uint32_t   key;
        PacketPositions pos;
        bool            jump_up;
        bool            hops;
    };
    const Case cases[] = {
        { "jump key C, packets on, jump_up on (no jump up can be planned)", 'C', PacketPositions::On, true, true },
        { "Watch, jump key Up, jump_up on (no jump up can be planned)", 38, PacketPositions::Watch, true, true },
        { "jump_up off, jump key Up, packets on (2026.11.05's walk back)", 38, PacketPositions::On, false, true },
        { "jump_up on, jump key Up, packets on (jumps up planned)", 38, PacketPositions::On, true, false },
    };
    for (const Case& k : cases) {
        HunterConfig c = owner_config();
        c.jump_key = k.key;
        c.packet_positions = k.pos;
        c.jump_up = k.jump_up;
        JumpRig r(c, 400, 999, true, dir, "t97_s1");
        r.w.map = 97;
        r.w.area = 1;
        r.w.entry_x = 2000.0f;
        r.w.entry_y = 999.0f;
        r.hunter.set_zone(zone_box("t97_s1", 1000, 899, 200, 850, 2400, 905));
        r.run(8000);
        const int hops = r.w.downs_of(k.key);
        check(from_map(r) && (k.hops ? hops >= 1 : hops == 0 && !r.w.jumps.empty()),
              fmt("%s: %d hop(s) toward the home, %zu planned jump(s)", k.what, hops, r.w.jumps.size()));
    }
}

void test_jump_source() {
    section("Hunter 05b: what the jump up leaves alone -- no Down, no chord flag, no old jump, nothing of the facing");
#ifdef GOBOT_SOURCE_DIR
    const std::string jump = source("/src/hunter_jump.cpp");
    if (jump.empty()) {
        skip("the hunter's source is not here to read\n");
        return;
    }
    const auto has = [&](const char* what) { return jump.find(what) != std::string::npos; };
    check(!has("0x28") && !has("VK_DOWN") && !has("kChord") && !has("drop_ctrl_down") && !has("input_.jump(") &&
              !has("double_jump(") && !has("key_jump") && !has("facing_"),
          "hunter_jump.cpp: no Down, no chord flag, no InputManager::jump()/double_jump(), no key_jump, no facing");
    check(has("input_.jump_once(kUpVk, go_on)") && has("input_.jump_twice(kUpVk, kJumpUpSecondAtMs, go_on)") &&
              has("input_.high_jump(go_on)") && has("constexpr std::uint32_t kUpVk = 0x26;"),
          "its keys: always Up (0x26) -- jump_once, jump_twice at kJumpUpSecondAtMs, high_jump");
    // jump_go: only its own settle goes straight to jump_at; one under way for
    // another purpose or monster is let go of first, and this jump walked to
    // its own take-off -- never begun where the other one stopped.
    check(has("const bool mine = jump_.phase == JumpUpPhase::Settle && jump_.purpose == purpose && jump_.slot == slot "
              "&&") &&
              has("if (jump_.phase == JumpUpPhase::Settle && !mine) jump_settle_end(\"another jump\", false, now);") &&
              has("if (mine || std::abs(stand->x - plan.x) <= kJumpUpLeadPx)") &&
              !has("if (jump_.phase == JumpUpPhase::Settle || std::abs(stand->x - plan.x) <= kJumpUpLeadPx)"),
          "jump_go: another's settle let go of, the take-off walked to -- not jumped from where that one stood");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_jump_hunt(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_jump_rule();
    test_jump_way_back();
    test_jump_target();
    test_jump_fails();
    test_jump_twice();
    test_jump_hop();
    test_jump_off();
    test_jump_source();
}
