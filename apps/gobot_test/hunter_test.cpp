// gobot_test -- the hunter on scripted plates: its seam, which keys it lets go
// of, and the monster lists ("Lọc quái").
//
// Nothing here goes near the game. The hunter is driven through its seams:
// plates the test puts in front of it, a pretend keyboard that writes down
// every key, how long it was held and every key it let go of, and a clock
// the test winds on -- one pass at a time, as its thread runs one every
// 60 ms.

#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/hunter.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/monster_list.hpp"
#include "gobot/screen_capture.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

/// A file in this run's own temp folder, in UTF-8, for the tests that need a
/// real one. main.cpp makes the folder and removes it: see temp_file there.
std::string temp_path(const char* name) {
    wchar_t dir[MAX_PATH] = {};
    const DWORD n = GetTempPathW(MAX_PATH, dir);
    std::wstring w;
    if (n > 0 && n <= MAX_PATH)
        w = std::wstring(dir) + L"gobot_test_" + std::to_wstring(GetCurrentProcessId()) + L"\\";
    w += std::wstring(name, name + std::strlen(name));
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len > 0 ? len - 1 : 0), '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

void write_file(const std::string& path, const std::string& text) {
    std::ofstream out(wide(path), std::ios::binary | std::ios::trunc);
    out << text;
}

void remove_file(const std::string& path) { DeleteFileW(wide(path).c_str()); }

/// A monster's plate standing at `x`, `y`: `x` the middle of its name, `y` its
/// feet, where the health bar is -- the two things the hunter goes by.
Nameplate plate(int x, int y, const std::string& name = {}, float hp = 1.0f) {
    Nameplate n;
    n.bar = ScreenRect{ x - 20, y, 40, 3 };
    n.plate = ScreenRect{ x - 30, y - 14, 60, 12 };
    n.name = name;
    n.name_confidence = name.empty() ? 0.0f : 0.9f;
    n.hp = hp;
    return n;
}

// ------------------------------------------------------------------ the rig

/// The pretend game: where the character stands, the monsters round it, a
/// clock, every key that reached it and every key let go of.
struct World {
    std::uint64_t t = 5'000'000;
    bool focused = true;
    bool see_me = true;
    int  me_x = 640, me_y = 400;
    std::vector<Nameplate> mobs;

    /// How far a walking step -- a direction held 100 ms or more -- moves the
    /// character, in pixels; and a direction held down (hold_walk), at each
    /// pass it is down for. 0: it stays where it is.
    int step_px = 0;

    /// A Press's hold for a key put down and left down (Hands::key_down).
    static constexpr int kHeld = 1 << 19;

    /// The next `refuse` presses of `refuse_vk` do not go out (Hands::key
    /// says false) -- the game behind another window for a moment. Written
    /// down all the same.
    int           refuse = 0;
    std::uint32_t refuse_vk = 0;

    /// Keys the pretend keyboard says are down -- as InputManager says of a
    /// key whose key-up did not go out, or one another thread is pressing.
    std::set<std::uint32_t> held;

    struct Press {
        std::uint64_t at;
        std::uint32_t vk;
        int           hold;
    };
    std::vector<Press>         presses;
    std::vector<std::uint32_t> ups;
    std::vector<std::uint64_t> up_at;           ///< when each of `ups` went
    int                        releases = 0;    ///< release_all() calls

    /// The frame of reference the plates are in (Hunter::Eyes::view), for a
    /// rig made with one: the screen's, say, and then the packets' world's.
    std::uint32_t view = 0;

    /// Where the packets put the character (Hunter::Eyes::where), for a rig
    /// made with it: the screen's place moved world_dx, world_dy -- the
    /// monsters' too -- in the state `fix_state` says. With `reports`, the
    /// client reports at every look; without, only as a walk starts and
    /// stops, and the packets work a held walk out at reckon_px a look the
    /// screen need not show: a walk into a wall.
    WorldFix::State fix_state = WorldFix::State::Known;
    int           world_dx = 4000, world_dy = 700;
    bool          airborne = false;
    bool          uncertain = false;
    std::uint32_t epoch = 1;
    float         entry_x = 0.0f, entry_y = 0.0f;
    /// The place the packets say the character is on, when they know it.
    bool          map_known = true;
    std::uint16_t map = 11, area = 1;
    bool          reports = true;
    int           reckon_px = 20;
    int           lead = 0;             ///< how far the working-out has run past the character
    float         report_x = 0.0f, report_y = 0.0f;
    std::uint64_t report_ms = 0;
    /// Which way the packets say the character faces (WorldFix::facing), and
    /// when a report said so -- set by the test: 0, as here unless set, the
    /// packets do not know, and nothing of the hunter's turning changes.
    /// With `turn_reports`, a direction key that goes out sets them too, as
    /// the client reports the walk it starts at once: a game that takes every
    /// key. Without, the keys say nothing: a game that took none of them, as
    /// far as the packets show.
    int           facing = 0;
    std::uint64_t facing_ms = 0;
    bool          turn_reports = false;
    int           wheres = 0;           ///< how often the hunter asked
    /// A report of where the character is: what the client sends as a walk
    /// starts or stops.
    void report() {
        lead = 0;
        report_x = static_cast<float>(me_x + world_dx);
        report_y = static_cast<float>(me_y + world_dy);
        report_ms = t;
    }

    std::vector<Press> of(std::uint32_t vk) const {
        std::vector<Press> out;
        for (const Press& p : presses)
            if (p.vk == vk) out.push_back(p);
        return out;
    }
    std::size_t ups_of(std::uint32_t vk) const {
        return static_cast<std::size_t>(std::count(ups.begin(), ups.end(), vk));
    }
    Nameplate* mob(const std::string& name) {
        for (Nameplate& n : mobs)
            if (n.name == name) return &n;
        return nullptr;
    }
};

/// A hunter on the pretend game, started without a thread.
struct HuntRig {
    InputManager         input;
    ExecutionCoordinator coord;
    MobDetector          detector;
    Hunter               hunter{ input, coord, detector };
    World                w;

    /// `with_view`: the eyes say which frame of reference the plates are in
    /// (World::view). Without, as every test before it: they never say.
    /// `with_where`: they say where the packets put the character (World's
    /// fix); without, there is no capture to say.
    explicit HuntRig(const HunterConfig& cfg, bool with_view = false, bool with_where = false) {
        Hunter::Eyes eyes;
        if (with_view) eyes.view = [this] { return w.view; };
        if (with_where) {
            eyes.where = [this](WorldFix& f) {
                ++w.wheres;
                if (w.reports) w.report();
                const bool walking = w.held.count(VK_LEFT) != 0 || w.held.count(VK_RIGHT) != 0;
                f = WorldFix{};
                f.state = w.fix_state;
                f.x = static_cast<float>(w.me_x + w.world_dx + w.lead);
                f.y = static_cast<float>(w.me_y + w.world_dy);
                f.rx = w.report_x;
                f.ry = w.report_y;
                f.report_ms = w.report_ms;
                f.facing = w.facing;
                f.facing_ms = w.facing_ms;
                f.airborne = w.airborne;
                f.moving = walking || w.lead != 0;
                f.uncertain = w.uncertain;
                f.map_known = w.map_known;
                f.map = w.map;
                f.area = w.area;
                f.epoch = w.epoch;
                f.entry_x = w.entry_x;
                f.entry_y = w.entry_y;
                return true;
            };
        }
        eyes.player = [this](Nameplate& out, bool* fresh) {
            if (fresh) *fresh = w.see_me;
            if (!w.see_me) return false;
            out = plate(w.me_x, w.me_y, "Me");
            out.is_player = true;
            out.is_self = true;
            return true;
        };
        // Nearest first, the way MobDetector::mobs() sorts them: a level
        // further up or down counts three times its height.
        eyes.mobs = [this] {
            std::vector<Nameplate> out = w.mobs;
            const int px = w.me_x, py = w.me_y;
            const auto cost = [px, py](const Nameplate& n) {
                const int dx = n.x() - px, dy = (n.y() - py) * 3;
                return dx * dx + dy * dy;
            };
            std::stable_sort(out.begin(), out.end(), [&](const Nameplate& a, const Nameplate& b) {
                return cost(a) < cost(b);
            });
            return out;
        };
        hunter.set_eyes(eyes);

        Hunter::Hands hands;
        hands.focused = [this] { return w.focused; };
        hands.key = [this](std::uint32_t vk, int hold) {
            w.presses.push_back(World::Press{ w.t, vk, hold });
            if (w.refuse > 0 && vk == w.refuse_vk) {
                --w.refuse;
                return false;
            }
            if (w.step_px > 0 && hold >= 100) {
                if (vk == VK_LEFT)  w.me_x -= w.step_px;
                if (vk == VK_RIGHT) w.me_x += w.step_px;
            }
            if (w.turn_reports && (vk == VK_LEFT || vk == VK_RIGHT)) {
                w.facing = vk == VK_LEFT ? -1 : 1;
                w.facing_ms = w.t;
            }
            return true;
        };
        // A walk's start and stop are reported (World::report), as the client
        // reports them.
        hands.key_down = [this](std::uint32_t vk) {
            w.presses.push_back(World::Press{ w.t, vk, World::kHeld });
            w.held.insert(vk);
            if (vk == VK_LEFT || vk == VK_RIGHT) w.report();
            if (w.turn_reports && (vk == VK_LEFT || vk == VK_RIGHT)) {
                w.facing = vk == VK_LEFT ? -1 : 1;
                w.facing_ms = w.t;
            }
            return true;
        };
        hands.key_up = [this](std::uint32_t vk) {
            w.ups.push_back(vk);
            w.up_at.push_back(w.t);
            if (w.held.erase(vk) != 0 && (vk == VK_LEFT || vk == VK_RIGHT)) w.report();
        };
        hands.held = [this](std::uint32_t vk) { return w.held.count(vk) != 0; };
        hands.release_all = [this] {
            ++w.releases;
            if (w.held.count(VK_LEFT) != 0 || w.held.count(VK_RIGHT) != 0) w.report();
            w.held.clear();
        };
        hunter.set_hands(hands);

        Hunter::Clock clock;
        clock.now_ms = [this] { return w.t; };
        clock.sleep_ms = [this](std::uint32_t ms) { w.t += ms; };
        hunter.set_clock(clock);

        hunter.set_config(cfg);
        hunter.start_stepped();
    }
    ~HuntRig() { hunter.stop(); }

    /// One pass, then 60 ms on: the thread's own rhythm -- the character
    /// walking meanwhile, a step_px, with a direction held down.
    void pass() {
        hunter.step();
        if (w.step_px > 0) {
            if (w.held.count(VK_LEFT)) w.me_x -= w.step_px;
            if (w.held.count(VK_RIGHT)) w.me_x += w.step_px;
        }
        // No reports between a walk's start and stop: the packets work it
        // out, the character moving or not.
        if (!w.reports) {
            if (w.held.count(VK_LEFT)) w.lead -= w.reckon_px;
            if (w.held.count(VK_RIGHT)) w.lead += w.reckon_px;
        }
        w.t += 60;
    }
    /// `ms` of passes, calling `each` after every one.
    void run(std::uint64_t ms, const std::function<void()>& each = nullptr) {
        for (const std::uint64_t until = w.t + ms; w.t < until;) {
            pass();
            if (each) each();
        }
    }
    /// The name of what it is going for or hitting right now, or "" when it
    /// is doing neither. Not Status::target alone: that keeps the last target
    /// after it has gone.
    std::string target() const {
        const Hunter::Status st = hunter.status();
        const bool after = st.state == Hunter::State::Approaching ||
                           st.state == Hunter::State::Attacking;
        return after && st.has_target ? st.target.name : std::string();
    }
};

/// Move a plate along the ground, keeping its name and health.
void move_to(Nameplate* n, int x) {
    if (n) *n = plate(x, n->y(), n->name, n->hp);
}

HunterConfig lists_config() {
    HunterConfig cfg;
    cfg.skip_names = { "NgaQuy" };
    cfg.first_names = { "HoLy" };
    return cfg;
}

// --------------------------------------------------------------- the seam

/// The hunter walking as it always did, a step of step_ms a look: hold_walk
/// off. The held walk, on by default, has test_hunter_held_walk.
HunterConfig stepping() {
    HunterConfig cfg;
    cfg.hold_walk = false;
    return cfg;
}

void test_hunter_seam() {
    section("Hunter: one pass at a time, on scripted plates (today's rules, stepping)");

    const HunterConfig cfg = stepping();    // reach 150, height 60, a 180-ms step, a swing every 700 ms
    {
        HuntRig r(cfg);
        r.w.mobs = { plate(900, 400, "TrungXanh") };
        r.pass();
        const Hunter::Status st = r.hunter.status();
        check(r.w.presses.size() == 1 && r.w.presses[0].vk == VK_RIGHT &&
                  r.w.presses[0].hold == 180,
              "a monster 260 px to the right: one step right, held for step_ms (180 ms)");
        check(r.w.ups.size() == 1 && r.w.ups[0] == VK_LEFT,
              "with the other direction let go of first");
        check(st.state == Hunter::State::Approaching && st.steps == 1 && st.has_target &&
                  st.dx == 260 && st.dy == 0,
              "walking to it, one step counted, 260 px off");

        // Now within reach -- and facing it already, from the step right.
        r.w.presses.clear();
        r.w.mobs = { plate(700, 400, "TrungXanh") };
        const std::uint64_t t0 = r.w.t;
        r.pass();
        check(r.w.presses.size() == 1 && r.w.presses[0].vk == 'Z' && r.w.presses[0].hold == 60,
              "60 px away, the step having turned the character right: no tap to face it, only the attack key");
        check(r.hunter.status().state == Hunter::State::Attacking, "and it says it is attacking");

        r.run(2040);    // passes up to 2040 ms after the first swing
        const auto swings = r.w.of('Z');
        bool spaced = swings.size() == 3;
        for (std::size_t i = 1; i < swings.size(); ++i)
            if (swings[i].at - swings[i - 1].at < 700) spaced = false;
        check(spaced && swings[0].at == t0,
              "the attack key once per attack_interval_ms: 3 swings in 2.04 s, 700+ ms apart -- " +
                  std::to_string(swings.size()));
        // No tap toward it on every pass while it is more than 12 px off, as
        // there was without a style: that crept the character in on it.
        check(r.w.of(VK_RIGHT).empty() && r.hunter.status().attacks == 3,
              "and no face tap at any of the 35 passes: it faces the target already");

        // The keys it lets go of, from here on: only a direction that is
        // down. release_all() here once let go of whatever another thread
        // was in the middle of pressing -- a potion among them.
        r.run(15 * 60);
        check(r.w.releases == 0 && r.w.ups.size() == 1,
              "over 50 passes of attacking: release_all never, and no key let go of -- nothing "
              "was down");
        r.w.held.insert(VK_LEFT);
        r.pass();
        r.pass();
        check(r.w.ups.size() == 2 && r.w.ups_of(VK_LEFT) == 2 && r.w.ups_of(VK_RIGHT) == 0 &&
                  r.w.releases == 0,
              "a left arrow left down: let go of once, and the right arrow, not down, left alone");
        r.w.held.insert(VK_RIGHT);
        r.w.held.insert('1');       // a potion the macro is pressing just now
        r.pass();
        check(r.w.ups_of(VK_RIGHT) == 1 && r.w.ups_of('1') == 0 && r.w.held.count('1') == 1 &&
                  r.w.releases == 0,
              "a key that is not a direction -- a potion being pressed -- is never let go of");
    }
    {
        // Not in front: nothing pressed, everything held let go, and it says why.
        HuntRig r(cfg);
        r.w.focused = false;
        r.w.mobs = { plate(700, 400, "TrungXanh") };
        r.run(600);
        check(r.w.presses.empty() && r.w.releases == 10 &&
                  r.hunter.status().state == Hunter::State::Unfocused,
              "with the game not in front: no key, every key let go each pass (release_all), "
              "Unfocused");
    }
    {
        HuntRig r(cfg);
        r.w.see_me = false;
        r.w.mobs = { plate(700, 400, "TrungXanh") };
        r.run(300);
        const bool quiet = r.w.presses.empty() && r.w.ups.empty() && r.w.releases == 0;
        r.w.held.insert(VK_RIGHT);
        r.pass();
        check(quiet && r.w.ups_of(VK_RIGHT) == 1 && r.w.releases == 0 &&
                  r.hunter.status().state == Hunter::State::NoPlayer,
              "without the character: no key, NoPlayer, and only a direction that is down let "
              "go of");
    }
    {
        HuntRig r(cfg);
        const std::uint64_t t0 = r.w.t;
        r.run(600);
        Hunter::Status st = r.hunter.status();
        check(r.w.presses.empty() && r.w.releases == 0 && st.state == Hunter::State::NoTargets &&
                  st.no_target_since_ms == t0 && !st.only_skipped,
              "with no monsters: no key, nothing released, NoTargets, and when that began");
        r.w.mobs = { plate(700, 400, "TrungXanh") };
        r.pass();
        st = r.hunter.status();
        check(st.no_target_since_ms == 0 && st.state == Hunter::State::Attacking,
              "a monster in reach ends it");
    }
    {
        // Up on a ledge: the hop goes with the step, as before.
        HuntRig r(cfg);
        r.w.mobs = { plate(900, 300, "TrungXanh") };
        r.pass();
        check(r.w.presses.size() == 2 && r.w.presses[0].vk == VK_RIGHT &&
                  r.w.presses[1].vk == VK_SPACE && r.w.presses[1].hold == 50,
              "a monster 100 px up: a step towards it and a 50-ms hop on the jump key");
    }
    {
        // Picking up after a kill, through the same hands.
        HunterConfig looting = cfg;
        looting.loot = true;
        looting.loot_key = 'A';
        HuntRig r(looting);
        r.w.mobs = { plate(700, 400, "TrungXanh") };
        r.pass();
        const std::size_t fighting = r.w.of('A').size();
        r.w.mobs.clear();
        r.run(1200);
        check(fighting == 1 && r.w.of('A').size() == fighting + 3 &&
                  r.hunter.status().loots == 4,
              "picking up through the same hands: once while fighting, then three times after "
              "the kill -- " + std::to_string(r.w.of('A').size()));
    }
    {
        // Standing still, nothing within reach: no key, and nothing let go of.
        HunterConfig still = cfg;
        still.stay_put = true;
        HuntRig r(still);
        r.w.mobs = { plate(1000, 400, "TrungXanh") };
        r.run(600);
        check(r.w.presses.empty() && r.w.releases == 0 &&
                  r.hunter.status().state == Hunter::State::Waiting,
              "standing still with nothing in reach: Waiting, no key, release_all never");
    }
    {
        // Stopped: one last let-go of everything, and nothing more from step().
        HuntRig r(cfg);
        r.w.mobs = { plate(700, 400, "TrungXanh") };
        r.pass();
        const int before = r.w.releases;
        r.hunter.stop();
        const std::size_t pressed = r.w.presses.size();
        r.pass();
        check(before == 0 && r.w.releases == 1 && r.w.presses.size() == pressed &&
                  r.hunter.status().state == Hunter::State::Idle && !r.hunter.running(),
              "stop(): every key let go once (release_all), Idle, and a step after it does nothing");
    }

    // The helpers, from plates alone.
    check(Hunter::in_reach(cfg, 150, 60) && Hunter::in_reach(cfg, -150, -60) &&
              !Hunter::in_reach(cfg, 151, 0) && !Hunter::in_reach(cfg, 0, -61),
          "in_reach: up to reach (150) along the ground and height_tolerance (60) up or down");
    check(Hunter::level_with(cfg, 60) && Hunter::level_with(cfg, -60) && !Hunter::level_with(cfg, 61),
          "level_with: the same height test");
    {
        const Nameplate me = plate(640, 400);
        const std::vector<Nameplate> mobs = { plate(700, 400, "A"), plate(760, 400, "B", 0.4f),
                                              plate(660, 300, "C"), plate(900, 400, "D", 0.02f) };
        const std::vector<Hunter::Listed> none(mobs.size(), Hunter::Listed::Free);
        check(Hunter::choose_target(cfg, mobs, &me, none) == 1,
              "choose_target: of those on the level, the most wounded (finish_wounded)");
        HunterConfig nearest = cfg;
        nearest.finish_wounded = false;
        check(Hunter::choose_target(nearest, mobs, &me, none) == 0,
              "or the nearest without finish_wounded");
        check(Hunter::choose_target(cfg, mobs, nullptr, none) == 0,
              "not knowing where the character is: the first with health");
        std::vector<Hunter::Listed> listed = none;
        listed[1] = Hunter::Listed::Skip;
        check(Hunter::choose_target(cfg, mobs, &me, listed) == 0, "never one the lists refuse");
        listed[2] = Hunter::Listed::First;
        check(Hunter::choose_target(cfg, mobs, &me, listed) == 0,
              "and one on Đánh trước on another level does not jump the queue");
        listed[2] = Hunter::Listed::Free;
        listed[0] = Hunter::Listed::NearSkip;
        listed[1] = Hunter::Listed::NotFirst;
        check(Hunter::choose_target(cfg, mobs, &me, listed) == 2,
              "every one on the level refused: the nearest with health elsewhere");
        check(Hunter::choose_in_reach(cfg, mobs, me, none) == 0 &&
                  Hunter::choose_in_reach(cfg, mobs, me,
                                          { Hunter::Listed::Skip, Hunter::Listed::First,
                                            Hunter::Listed::Free, Hunter::Listed::Free }) == 1,
              "choose_in_reach: the nearest within reach, or the nearest on Đánh trước");
    }
}

// ------------------------------------------------------- which one, and when not

void test_hunter_choosing() {
    section("Hunter: the most wounded only within reach; one out of reach passed over a while");

    const HunterConfig cfg;     // no style: reach 150, height 60, give_up_ms 4000
    {
        // A far one at 60% -- or a bar misread at 0.97 -- beat a whole one in
        // reach, and the character turned its back on what was hitting it.
        const Nameplate me = plate(640, 400);
        const std::vector<Nameplate> mobs = { plate(700, 400, "Near"), plate(1000, 400, "Far", 0.6f),
                                              plate(1100, 400, "Misread", 0.97f) };
        const std::vector<Hunter::Listed> none(mobs.size(), Hunter::Listed::Free);
        check(Hunter::choose_target(cfg, mobs, &me, none) == 0,
              "a whole one 60 px off beats one at 60% 360 px off, and one read at 97% further: the "
              "wounded are finished only within reach");
        const std::vector<Nameplate> firsts = { plate(700, 400, "A"), plate(760, 400, "B"),
                                                plate(1000, 400, "C", 0.3f) };
        check(Hunter::choose_target(cfg, firsts, &me,
                                    { Hunter::Listed::Free, Hunter::Listed::First, Hunter::Listed::First }) ==
                  1,
              "the same among those on Đánh trước: the nearest of them, not the wounded one out of reach");
        const std::vector<Nameplate> close = { plate(700, 400, "A"), plate(760, 400, "B", 0.4f) };
        check(Hunter::choose_target(cfg, close, &me, { Hunter::Listed::Free, Hunter::Listed::Free }) == 1,
              "and one within reach still goes first when hurt");
    }

    // The character walks nowhere here (step_px 0): every monster not in
    // reach where it stands is behind a wall.
    const auto lines = [](const std::function<void()>& run) {
        Logger& log = Logger::instance();
        const LogLevel was = log.level();
        log.set_ring_capacity(2000);
        log.clear_ring();
        log.set_level(LogLevel::Info);
        run();
        std::vector<std::string> out;
        for (const LogLine& l : log.ring()) out.push_back(l.text);
        log.set_level(was);
        log.set_ring_capacity(0);
        return out;
    };
    const auto said = [](const std::vector<std::string>& all, const char* a, const char* b) {
        for (const std::string& l : all)
            if (l.find(a) != std::string::npos && l.find(b) != std::string::npos) return true;
        return false;
    };
    {
        std::string first, next, later;
        std::uint64_t dropped = 0;
        const std::vector<std::string> log = lines([&] {
            HuntRig r(cfg);
            r.w.mobs = { plate(900, 400, "Wall"), plate(300, 400, "Other") };
            r.pass();
            first = r.target();
            r.run(4200);
            next = r.target();
            dropped = r.hunter.status().targets_dropped;
            r.run(3000);
            later = r.target();
        });
        check(first == "Wall" && next == "Other" && later == "Other" && dropped == 1,
              "the nearest, behind a wall, given up on after 4 s: the next one is gone for, not the "
              "same one again -- " + first + ", then " + next + ", then " + later);
        check(said(log, "Hunter: gave up on Wall out of reach after 4 s: 260 px right, 0 px down",
                   "passed over for 8 s"),
              "and the log says where it was: 260 px right, 0 px down, passed over for 8 s");
    }
    {
        // On "Đánh trước", up where it cannot be got to: let go of all the
        // same, or the bot stands under it for good.
        HunterConfig firsts = cfg;
        firsts.first_names = { "Boss" };
        HuntRig r(firsts);
        r.w.mobs = { plate(900, 400, "Boss"), plate(300, 400, "Moc") };
        r.pass();
        const std::string first = r.target();
        r.run(4200);
        check(first == "Boss" && r.target() == "Moc",
              "one on Đánh trước it cannot reach: given up on too, and an unlisted one gone for -- " +
                  first + ", then " + r.target());
        // The other one gone: the one on Đánh trước is still passed over.
        r.w.mobs = { plate(900, 400, "Boss") };
        r.run(2000);
        const Hunter::Status st = r.hunter.status();
        check(r.target().empty() && st.state == Hunter::State::NoTargets && st.passed_over && !st.only_skipped,
              "and with it alone in view, not gone for again within twice give_up_ms: NoTargets, said as "
              "passed over, not as nothing in view");
    }
    {
        // The only one there, out of reach: the bot stands for twice
        // give_up_ms instead of walking at the wall, then tries again.
        HuntRig r(cfg);
        r.w.mobs = { plate(900, 400, "Wall") };
        r.run(4200);
        const Hunter::State standing = r.hunter.status().state;
        const bool passed_over = r.hunter.status().passed_over;
        const std::size_t keys = r.w.presses.size();
        r.run(7000);
        const bool still = r.w.presses.size() == keys;
        r.run(1500);
        check(standing == Hunter::State::NoTargets && passed_over && still && r.target() == "Wall" &&
                  !r.hunter.status().passed_over,
              "the only monster, out of reach: given up on, the bot standing with no key for 8 s, then "
              "going for it again");
    }
    {
        // Without a style every fight is given up on every 4 s, in reach or
        // not, and picked straight back: one being fought in reach is never
        // passed over for it.
        std::vector<std::string> log;
        std::size_t swings = 0;
        std::string target;
        log = lines([&] {
            HuntRig r(cfg);
            r.w.mobs = { plate(700, 400, "Moc") };
            r.run(10'000);
            swings = r.w.of('Z').size();
            target = r.target();
        });
        check(swings >= 13 && target == "Moc" && !said(log, "Hunter: gave up on", "out of reach"),
              "no style, fought in reach for 10 s: given up on and picked back every 4 s as before, "
              "never passed over -- " + std::to_string(swings) + " swings");
    }
    {
        // Fought in reach, and a step out of it just as the 4 s run out:
        // out of reach at that one look, but not for the 4 s. Taken for one
        // behind a wall, it was dropped half dead and passed over for 8 s.
        std::string after;
        const std::vector<std::string> log = lines([&] {
            HuntRig r(cfg);
            r.w.mobs = { plate(700, 400, "Moc") };
            r.run(3900);
            move_to(r.w.mob("Moc"), 810);   // 170 px: out of reach, and still the same monster
            r.run(300);
            after = r.target();
        });
        check(after == "Moc" && !said(log, "Hunter: gave up on", "out of reach"),
              "no style, a monster fought in reach that steps out of it as the 4 s run out: gone after "
              "again, not passed over -- " + (after.empty() ? std::string("none") : after));
    }
    {
        // Walking to it, then a hold -- a trash pass -- or the game out of
        // focus for longer than give_up_ms: no time spent failing to reach
        // it, so it is gone after again once that is over.
        for (const bool hold : { true, false }) {
            HuntRig r(cfg);
            r.w.mobs = { plate(900, 400, "Xa") };
            r.run(500);
            if (hold) r.coord.engage_critical("trash cleaning");
            else      r.w.focused = false;
            r.run(5000);
            if (hold) r.coord.disengage_critical();
            else      r.w.focused = true;
            r.run(300);
            check(r.target() == "Xa" && !r.hunter.status().passed_over,
                  std::string(hold ? "a 5-s hold" : "the game out of focus for 5 s") +
                      " while walking to the only monster: gone after again once it is over, not passed over");
        }
    }
}

// ---------------------------------------------------------- monster lists

void test_monster_names() {
    section("Monster lists: names matched loosely");

    check(monster_name_matches("Thỏ", "Tho") && monster_name_matches("Tho", "Thỏ"),
          "\"Thỏ\" is \"Tho\": accents do not count");
    check(!monster_name_matches("NgaQuyVuong", "NgaQuy") &&
              !monster_name_matches("NgaQuy", "NgaQuyVuong"),
          "\"NgaQuy\" is not \"NgaQuyVuong\"");
    check(!monster_name_matches("", "NgaQuy") && !monster_name_matches("NgaQuy", "") &&
              !monster_name_matches("", ""),
          "an empty name matches nothing, not even another empty one");
    for (const char* read : { "NgaQuv", "NgaQu", "NGAQUY", "Nga Quỷ", "NgạQuỷ" })
        check(monster_name_matches(read, "NgaQuy"),
              std::string("\"") + read + "\" is \"NgaQuy\": one letter wrong or missing, case, "
                                         "spaces and accents");
    check(!monster_name_matches("Nga", "NgaQuy"), "a 3-letter \"Nga\" is not \"NgaQuy\"");
    check(monster_name_matches("holy", "HoLy") && !monster_name_matches("HoLyX", "HoLy") &&
              !monster_name_matches("HoLyy", "HoLy") && !monster_name_matches("HoL", "HoLy") &&
              !monster_name_matches("Tho", "Thoo") && monster_name_matches("Thoo", "Thooo"),
          "four letters or fewer on the list must match exactly (\"HoLy\" is not \"HoLyX\"); five "
          "may lose one");
    check(monster_on_list({ "Hồ Ly", "NgaQuỷ" }, "NgaQuy") && !monster_on_list({}, "NgaQuy") &&
              !monster_on_list({ "NgaQuỷ" }, ""),
          "on a list, or not");

    const auto names = split_monster_names("Nga Quỷ, Hồ Ly;Thỏ\nRồng ,  , ");
    check(names.size() == 4 && names[0] == "Nga Quỷ" && names[1] == "Hồ Ly" && names[2] == "Thỏ" &&
              names[3] == "Rồng",
          "a list splits at commas, semicolons and line breaks, trimmed, accents kept");
    std::string many;
    for (int i = 0; i < 25; ++i) many += "Quai" + std::to_string(i) + ",";
    check(split_monster_names(many).size() == kMaxMonsterNames && kMaxMonsterNames == 20,
          "and keeps the first 20");
    check(join_monster_names({ "Nga Quỷ", "Hồ Ly" }) == "Nga Quỷ, Hồ Ly" &&
              join_monster_names({}).empty(),
          "joined with commas to be written back");

    // hunt.ini.
    const std::string path = temp_path("gobot_hunter_lists.ini");
    {
        HunterConfig cfg;
        cfg.skip_names = split_monster_names("Nga Quỷ, Hồ Ly");
        cfg.first_names = { "Boss Rồng" };
        cfg.first_only = true;
        HunterConfig back;
        IniFile raw;
        const bool ok = cfg.save(path) && back.load(path) && raw.load(path);
        check(ok && back.skip_names == cfg.skip_names && back.first_names == cfg.first_names &&
                  back.first_only,
              "hunt.ini: 'Nga Quỷ, Hồ Ly', a first list and first_only read back unchanged");
        check(raw.get("Hunt", "skip_monsters") == "Nga Quỷ, Hồ Ly" &&
                  raw.get("Hunt", "first_monsters") == "Boss Rồng" &&
                  raw.get("Hunt", "first_only") == "true",
              "written as skip_monsters, first_monsters and first_only, in UTF-8 as typed");
    }
    {
        write_file(path, "# an older GoBot's\n[Hunt]\nreach=150\n");
        HunterConfig old;
        old.skip_names = { "x" };
        old.first_names = { "y" };
        old.first_only = true;
        check(old.load(path) && old.skip_names.empty() && old.first_names.empty() &&
                  !old.first_only && old.reach == 150,
              "an older hunt.ini without them loads empty lists and first_only off");
    }
    {
        write_file(path, "[Hunt]\nskip_monsters=Nga Quỷ; Hồ Ly\n");
        HunterConfig typed;
        check(typed.load(path) && typed.skip_names.size() == 2 && typed.skip_names[1] == "Hồ Ly",
              "and one typed by hand with semicolons reads too");
    }
    remove_file(path);
}

void test_monster_lists_hunting() {
    section("Monster lists: Không đánh is never attacked, Đánh trước goes first");

    const HunterConfig cfg = lists_config();
    {
        // A, on the skip list, is nearest; C beyond it. The character walks.
        HuntRig r(cfg);
        r.w.step_px = 40;
        r.w.mobs = { plate(700, 400, "NgaQuy"), plate(900, 400, "TrungXanh") };
        bool never_a = true, went_c = false;
        r.run(30'000, [&] {
            const std::string t = r.target();
            if (t == "NgaQuy") never_a = false;
            if (t == "TrungXanh") went_c = true;
        });
        const Hunter::Status st = r.hunter.status();
        check(never_a && went_c, "A (Không đánh) nearest, C beyond it: C is the target");
        // A and C are both to the right, so a key toward one is a key toward
        // the other: never_a above is what says A was never gone for. Here,
        // that nothing turned back toward the left and C was hit.
        check(r.w.of(VK_LEFT).empty() && !r.w.of('Z').empty(),
              "over 30 s it never turns or walks left, back from C, and C is attacked -- " +
                  std::to_string(r.w.of('Z').size()) + " swings");
        check(st.skipped_by_list == 1, "A counted once as kept off by the list, not once a frame");
    }
    {
        HuntRig r(cfg);
        r.w.mobs = { plate(700, 400, "NgaQuy") };
        r.run(2000);
        const Hunter::Status st = r.hunter.status();
        check(st.state == Hunter::State::NoTargets && st.only_skipped && !st.has_target &&
                  r.w.presses.empty() && r.w.ups.empty(),
              "A alone: NoTargets, only_skipped, and not one key sent");
    }
    for (const char* read : { "NgaQuv", "NgaQu", "NGAQUY", "Nga Quỷ" }) {
        HuntRig r(cfg);
        r.w.mobs = { plate(700, 400, read) };
        r.run(600);
        check(r.w.presses.empty() && r.hunter.status().only_skipped,
              std::string("read as \"") + read + "\", A is skipped all the same");
    }
    {
        HuntRig r(cfg);
        r.w.mobs = { plate(700, 400, "Nga") };
        r.run(600);
        check(r.target() == "Nga" && !r.w.of('Z').empty(), "a 3-letter \"Nga\" is not: attacked");
    }
    {
        // Both within reach, B (Đánh trước) the further.
        HuntRig r(cfg);
        r.w.mobs = { plate(700, 400, "TrungXanh"), plate(780, 400, "HoLy") };
        r.pass();
        check(r.target() == "HoLy", "B (Đánh trước) further than C, both reachable: B");
    }
    {
        HuntRig r(cfg);
        r.w.mobs = { plate(700, 300, "HoLy"), plate(780, 400, "TrungXanh") };
        r.pass();
        check(r.target() == "TrungXanh", "B on another level, C reachable: C");
    }
    {
        // Fighting C when B turns up and walks into reach.
        HuntRig r(cfg);
        r.w.mobs = { plate(700, 400, "TrungXanh") };
        r.run(600);
        r.w.mobs.push_back(plate(1000, 400, "HoLy"));
        r.pass();
        check(r.target() == "TrungXanh", "fighting C when B turns up out of reach: still C");
        move_to(r.w.mob("HoLy"), 760);
        r.pass();
        check(r.target() == "HoLy", "B comes within reach: B takes over within one pass");
        bool stayed = true;
        r.run(5000, [&] {
            if (r.target() != "HoLy") stayed = false;
        });
        check(stayed, "and it does not go back to C over the next 5 s, nearer as C is");
    }
    {
        // A second B within two seconds of a switch waits for them to pass.
        HuntRig r(cfg);
        r.w.mobs = { plate(700, 400, "TrungXanh") };
        r.run(300);
        r.w.mobs.push_back(plate(760, 400, "HoLy"));
        const std::uint64_t at = r.w.t;
        r.pass();
        check(r.target() == "HoLy", "a switch to B");
        r.w.mob("HoLy")->hp = 0.0f;
        r.pass();
        r.w.mobs.pop_back();
        r.pass();
        check(r.target() == "TrungXanh", "B finished: C again");
        r.w.mobs.push_back(plate(780, 400, "HoLy"));
        bool waited = true;
        while (r.w.t < at + 2000) {
            r.pass();
            if (r.target() == "HoLy") waited = false;
        }
        r.run(120);
        check(waited && r.target() == "HoLy",
              "another B in reach within 2 s of the last switch waits until the 2 s are up");
    }
    {
        // A's name drops out for a while, where A stood.
        HuntRig r(cfg);
        r.w.mobs = { plate(700, 400, "NgaQuy") };
        r.pass();
        const std::uint64_t read_at = r.w.t - 60;
        r.w.mobs[0].name.clear();
        bool kept_off = true;
        // Nameless, it wanders 5 px a pass: still known after it has gone 80 px.
        while (r.w.t < read_at + 1000) {
            r.w.mobs[0].bar.x += 5;
            r.w.mobs[0].plate.x += 5;
            r.pass();
            if (!r.w.presses.empty() || r.hunter.status().has_target) kept_off = false;
        }
        check(kept_off && r.hunter.status().only_skipped,
              "A nameless 1 s after it was last read, 80 px on: not targeted");
        while (r.w.t < read_at + 1980) r.pass();
        check(r.w.presses.empty(), "nor just under 2 s after");
        r.run(120);
        check(!r.w.presses.empty() && r.hunter.status().has_target,
              "2 s after, with no skip read meanwhile: a nameless plate there is fair game");
    }
    {
        // The same where it stands: A's name gone at the very spot it was
        // read at, 1 s later and 2 s later.
        HuntRig r(cfg);
        r.w.mobs = { plate(700, 400, "NgaQuy") };
        r.pass();
        const std::uint64_t read_at = r.w.t - 60;
        r.w.mobs[0].name.clear();
        bool kept_off = true;
        while (r.w.t < read_at + 1000) {
            r.pass();
            if (!r.w.presses.empty() || r.hunter.status().has_target) kept_off = false;
        }
        check(kept_off && r.hunter.status().only_skipped,
              "A nameless at the same spot 1 s after it was last read: not targeted");
        while (r.w.t < read_at + 1980) r.pass();
        const bool still = r.w.presses.empty();
        r.run(120);
        check(still && !r.w.presses.empty() && r.hunter.status().has_target,
              "and fair game there 2 s after, with no skip read meanwhile");
    }
    {
        HunterConfig only = cfg;
        only.first_only = true;
        HuntRig r(only);
        r.w.step_px = 40;
        r.w.mobs = { plate(700, 400, "NgaQuy"), plate(760, 400, "TrungXanh"),
                     plate(1000, 400, "HoLy") };
        bool only_b = true;
        r.run(10'000, [&] {
            const std::string t = r.target();
            if (!t.empty() && t != "HoLy") only_b = false;
        });
        check(only_b && !r.w.of('Z').empty(), "first_only: only B is ever targeted, and attacked");
    }
    {
        HunterConfig only = cfg;
        only.first_only = true;
        HuntRig r(only);
        r.w.mobs = { plate(700, 400, "NgaQuy"), plate(760, 400, "TrungXanh") };
        r.run(1000);
        check(r.hunter.status().state == Hunter::State::NoTargets && r.w.presses.empty(),
              "first_only with only A and C in view: NoTargets, nothing pressed");
    }
    {
        // Standing still: A and C within reach, B on the level but out of it.
        HunterConfig still = cfg;
        still.stay_put = true;
        HuntRig r(still);
        r.w.mobs = { plate(720, 400, "NgaQuy"), plate(760, 400, "TrungXanh"),
                     plate(1000, 400, "HoLy") };
        bool never_a = true;
        r.run(3000, [&] {
            if (r.target() == "NgaQuy") never_a = false;
        });
        check(never_a && r.target() == "TrungXanh" && !r.w.of('Z').empty(),
              "standing still: of those within reach C is taken, never A");
        move_to(r.w.mob("HoLy"), 780);
        r.pass();
        check(r.target() == "HoLy", "and B, once within reach, takes over");
    }
    {
        HunterConfig still = cfg;
        still.stay_put = true;
        HuntRig r(still);
        r.w.mobs = { plate(720, 400, "NgaQuy"), plate(1000, 400, "HoLy") };
        r.run(2000);
        check(r.w.of('Z').empty() && r.w.of(VK_LEFT).empty() && r.w.of(VK_RIGHT).empty(),
              "standing still with only A within reach: nothing attacked, no step taken");
    }
    {
        // With no listed monster in view the lists change nothing: the same
        // keys, at the same moments, as with no lists at all.
        HuntRig a(HunterConfig{});
        HuntRig b(lists_config());
        for (HuntRig* r : { &a, &b }) {
            r->w.step_px = 40;
            r->w.mobs = { plate(700, 400, "Moc", 0.6f), plate(980, 400, "Da"),
                          plate(660, 330, "Cao") };
        }
        a.run(8000);
        b.run(8000);
        bool same = a.w.presses.size() == b.w.presses.size() && a.w.ups == b.w.ups &&
                    a.w.releases == b.w.releases;
        for (std::size_t i = 0; same && i < a.w.presses.size(); ++i)
            same = a.w.presses[i].vk == b.w.presses[i].vk &&
                   a.w.presses[i].hold == b.w.presses[i].hold &&
                   a.w.presses[i].at == b.w.presses[i].at;
        check(same && !a.w.presses.empty(),
              "with no listed monster in view the hunter presses exactly what it did without lists");
    }
}

// ------------------------------------------- another frame of reference

/// The packets take over from the screen, or the screen from them, while a
/// monster is being fought (Hunter::Eyes::view): from that pass on every plate
/// is in other coordinates. The target, followed by its spot, read as gone --
/// a kill, three presses of the pick-up key -- at every switch. It is let go
/// of instead, as no kill, and picked again where it now is.
void test_hunter_view_change() {
    section("Hunter: the plates in another frame of reference (Eyes::view)");

    HunterConfig cfg;
    cfg.loot = true;
    cfg.loot_key = 'A';
    // One fight, in the screen's pixels and then in the packets' world's: you
    // and the monster 60 px apart in both, 4000 px on and 700 px down.
    const auto change = [](HuntRig& r) {
        r.w.view = 1;
        r.w.me_x = 4640;
        r.w.me_y = 1100;
        r.w.mobs = { plate(4700, 1100, "TrungXanh") };
    };
    {
        HuntRig r(cfg, true);
        r.w.mobs = { plate(700, 400, "TrungXanh") };
        r.pass();
        const bool fighting = r.target() == "TrungXanh" && r.hunter.status().state == Hunter::State::Attacking;
        const std::size_t loot_before = r.w.of('A').size();
        const std::uint64_t finished_before = r.hunter.status().targets_finished;
        // The first pass swung already: what counts is a swing after the change.
        const std::size_t swings_before = r.w.of('Z').size();
        change(r);
        r.run(1200);
        const Hunter::Status st = r.hunter.status();
        const std::size_t looted = r.w.of('A').size() - loot_before;
        const std::size_t swings = r.w.of('Z').size() - swings_before;
        check(fighting && looted == 0 && st.targets_finished == finished_before,
              "a new frame of reference mid-fight: no loot owed for it and nothing counted finished -- " +
                  std::to_string(looted) + " pick-up presses in 1.2 s");
        check(st.state == Hunter::State::Attacking && st.has_target && st.target.x() == 4700 &&
                  st.target.y() == 1100 && st.dx == 60 && swings >= 1,
              "and the monster is picked again where it now is, and fought there -- " + std::to_string(swings) +
                  " swing(s) since");
    }
    {
        // The same with no view, as every hunt went before: read as a kill.
        HuntRig r(cfg);
        r.w.mobs = { plate(700, 400, "TrungXanh") };
        r.pass();
        const std::size_t loot_before = r.w.of('A').size();
        change(r);
        r.run(1200);
        const std::size_t looted = r.w.of('A').size() - loot_before;
        check(looted == 3 && r.hunter.status().state == Hunter::State::Attacking,
              "without it, as before: the target reads as gone, and three pick-up presses go out for "
              "it -- " + std::to_string(looted));
    }
    {
        // A view that stays where it is changes nothing: the same keys at the
        // same moments as with none.
        HuntRig a(cfg);
        HuntRig b(cfg, true);
        for (HuntRig* r : { &a, &b }) {
            r->w.step_px = 40;
            r->w.mobs = { plate(700, 400, "Moc", 0.6f), plate(980, 400, "Da"), plate(660, 330, "Cao") };
        }
        a.run(6000);
        b.run(6000);
        bool same = a.w.presses.size() == b.w.presses.size() && a.w.ups == b.w.ups;
        for (std::size_t i = 0; same && i < a.w.presses.size(); ++i)
            same = a.w.presses[i].vk == b.w.presses[i].vk && a.w.presses[i].hold == b.w.presses[i].hold &&
                   a.w.presses[i].at == b.w.presses[i].at;
        check(same && !a.w.presses.empty(), "a view that never changes: exactly the keys of a hunter with none");
    }
}

// ------------------------------------------------------- the hunting spot

/// "Chỗ đánh quái" on: the spot taken where the character stands. On the
/// rig's fix the screen's 640,400 is the world's 4640,1100, and the area of
/// +-300 / +-100 runs from 340 to 940 across the screen, 300 to 500 down it.
HunterConfig spotted() {
    HunterConfig cfg;
    cfg.spot = true;
    return cfg;
}

/// What the log says at Info and up while `run` runs.
std::vector<std::string> info_lines(const std::function<void()>& run) {
    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_ring_capacity(2000);
    log.clear_ring();
    log.set_level(LogLevel::Info);
    run();
    std::vector<std::string> out;
    for (const LogLine& l : log.ring()) out.push_back(l.text);
    log.set_level(was);
    log.set_ring_capacity(0);
    return out;
}

/// How many of `all` have both `a` and `b` in them.
int lines_with(const std::vector<std::string>& all, const char* a, const char* b = "") {
    int n = 0;
    for (const std::string& l : all)
        if (l.find(a) != std::string::npos && l.find(b) != std::string::npos) ++n;
    return n;
}

/// Whether two pretend games had the same keys pressed at the same moments,
/// held as long, and the same let go of.
bool same_keys(const World& a, const World& b) {
    bool same = a.presses.size() == b.presses.size() && a.ups == b.ups && a.releases == b.releases;
    for (std::size_t i = 0; same && i < a.presses.size(); ++i)
        same = a.presses[i].vk == b.presses[i].vk && a.presses[i].hold == b.presses[i].hold &&
               a.presses[i].at == b.presses[i].at;
    return same;
}

/// The walks taken `vk`'s way: the key put down and left down.
std::size_t walks_of(const World& w, std::uint32_t vk) {
    std::size_t n = 0;
    for (const World::Press& p : w.presses)
        if (p.vk == vk && p.hold == World::kHeld) ++n;
    return n;
}

/// Off, nothing of it changes anything: not one key, not one moment.
void test_spot_off() {
    section("Chỗ đánh quái: off, exactly the keys of before; on, at its spot with all in the area, too");

    {
        HuntRig a(HunterConfig{});
        HuntRig b(HunterConfig{}, false, true);
        for (HuntRig* r : { &a, &b }) {
            r->w.step_px = 40;
            r->w.mobs = { plate(700, 400, "Moc", 0.6f), plate(980, 400, "Da"), plate(660, 330, "Cao") };
        }
        a.run(8000);
        b.run(8000);
        // Watched only (packet_positions=watch, as hunt.ini ships): not even
        // for which way the character faces.
        check(same_keys(a.w, b.w) && !a.w.presses.empty() && b.w.wheres == 0 && !b.hunter.spot().set,
              "spot=false, the packets' fix there to be had: every key, held as long, at the same moment as a "
              "hunter with none -- and the fix never asked for");
    }
    {
        // Stepping, a kill and its pick-ups, a monster up on a ledge (a hop)
        // and nothing in view a while.
        HunterConfig steps;
        steps.hold_walk = false;
        steps.loot = true;
        steps.loot_key = 'A';
        HuntRig a(steps);
        HuntRig b(steps, false, true);
        for (HuntRig* r : { &a, &b }) {
            r->w.step_px = 40;
            r->w.mobs = { plate(900, 400, "Xa"), plate(500, 300, "Cao") };
            r->run(3000);
            r->w.mobs[0].hp = 0.0f;
            r->pass();
            r->w.mobs.erase(r->w.mobs.begin());
            r->run(3000);
            r->w.mobs.clear();
            r->w.me_x += 300;
            r->run(3000);
        }
        check(same_keys(a.w, b.w) && !a.w.of('A').empty() && !a.w.of(VK_SPACE).empty() && b.w.wheres == 0,
              "spot=false, stepping, a kill, a hop and an empty screen: the same keys at the same moments");
    }
    {
        // On, at its spot: every monster inside the area and on the level,
        // and always one to fight -- nothing for the spot to do.
        HuntRig a(HunterConfig{});
        HuntRig b(spotted(), false, true);
        for (HuntRig* r : { &a, &b }) {
            r->w.step_px = 40;
            r->w.mobs = { plate(700, 400, "Moc", 0.6f), plate(900, 400, "Da") };
            r->run(3000);
            r->w.mobs[0].hp = 0.0f;
            r->pass();
            r->w.mobs.erase(r->w.mobs.begin());
            r->run(5000);
        }
        check(same_keys(a.w, b.w) && !a.w.of('Z').empty() && b.hunter.spot().set && b.w.wheres > 0 &&
                  b.hunter.status().returns == 0,
              "spot=true at its spot, the monsters all inside the area: the same keys as with it off");
    }
}

void test_spot_taken() {
    section("Chỗ đánh quái: the spot is where the character stands, settled, at the first look that knows it");

    bool none_before = false, set = false, stays = false;
    const std::vector<std::string> log = info_lines([&] {
        HuntRig r(spotted(), false, true);
        r.w.airborne = true;
        r.pass();
        const bool in_air = !r.hunter.spot().set && r.hunter.status().spot_why == Hunter::SpotWhy::Settling;
        r.w.airborne = false;
        r.w.uncertain = true;
        r.pass();
        const bool uncertain = !r.hunter.spot().set;
        r.w.uncertain = false;
        r.w.fix_state = WorldFix::State::Lost;
        r.pass();
        const bool lost = !r.hunter.spot().set && r.hunter.status().spot_why == Hunter::SpotWhy::Paused;
        r.w.fix_state = WorldFix::State::FirstReport;
        r.pass();
        const bool first = !r.hunter.spot().set && r.hunter.status().spot_why == Hunter::SpotWhy::WaitingFirst;
        none_before = in_air && uncertain && lost && first;
        r.w.fix_state = WorldFix::State::Known;
        r.w.me_x = 700;
        r.pass();
        const Hunter::Spot s = r.hunter.spot();
        const Hunter::Status st = r.hunter.status();
        set = s.set && s.x == 4700.0f && s.y == 1100.0f && s.half_w == 300 && s.half_h == 100 && s.epoch == 1 &&
              !s.suspended && st.spot_set && st.spot_x == 4700.0f && st.spot_half_w == 300 &&
              st.spot_why == Hunter::SpotWhy::None;
        r.w.me_x = 900;
        r.pass();
        stays = r.hunter.spot().x == 4700.0f && std::abs(r.hunter.status().spot_dx + 200.0f) < 0.5f;
    });
    check(none_before, "in mid-jump, worked out past a report, lost, or before any report of yours: no spot yet");
    check(set, "standing on the ground, known: the spot is there, 4700,1100, +-300 / +-100, and in the status");
    check(stays, "it stays where it was set as the character goes: 200 px to its left now");
    check(lines_with(log, "Hunter: hunting spot set where the character stands: 4700,1100 (+-300 / +-100)") == 1,
          "and the log says where, once");
    {
        HuntRig r(spotted(), false, true);
        r.pass();
        r.hunter.stop();
        r.w.me_x = 900;
        r.hunter.start_stepped();
        r.pass();
        check(r.hunter.spot().x == 4900.0f, "a new start: the spot taken afresh where the character stands then");
    }
}

void test_spot_area() {
    section("Chỗ đánh quái: only monsters inside the area are walked to; one within reach is fought anywhere");

    {
        HuntRig r(spotted(), false, true);
        r.pass();
        r.w.me_x = 800;
        r.w.mobs = { plate(1100, 400, "Ngoai"), plate(450, 400, "Trong") };
        bool inner = true;
        r.run(3000, [&] {
            if (r.target() != "Trong") inner = false;
        });
        check(inner && walks_of(r.w, VK_LEFT) >= 1 && r.w.of(VK_RIGHT).empty(),
              "one outside the area 300 px off and one inside 350 px off: the one inside walked to, the other never");
    }
    {
        HuntRig r(spotted(), false, true);
        r.pass();
        r.w.me_x = 800;
        r.w.mobs = { plate(1100, 400, "Ngoai") };
        r.pass();
        const Hunter::Status st = r.hunter.status();
        const bool said = st.state == Hunter::State::NoTargets && st.outside_area && !st.only_skipped &&
                          !st.passed_over && r.target().empty();
        r.run(3000);
        check(said && r.w.of(VK_RIGHT).empty() && r.hunter.status().outside_area,
              "only one outside the area in view: NoTargets, said as outside the area and not as the lists' doing, "
              "and never a key toward it");
    }
    {
        HuntRig r(spotted(), false, true);
        r.pass();
        r.w.me_x = 900;
        r.w.mobs = { plate(1010, 400, "Gan") };     // 70 px outside the area, 110 px off
        r.run(1000);
        check(!r.w.of('Z').empty() && walks_of(r.w, VK_RIGHT) == 0 && walks_of(r.w, VK_LEFT) == 0 &&
                  r.hunter.status().state == Hunter::State::Attacking,
              "one 70 px outside the area but within reach: attacked where it stands, no walk");
    }
    {
        HuntRig r(spotted(), false, true);
        r.pass();
        r.w.mobs = { plate(900, 280, "Cao") };      // 120 px above the spot
        r.run(1000);
        check(r.w.presses.empty() && r.hunter.status().outside_area,
              "one 120 px above the spot and out of reach: outside the area's +-100, not walked to");
    }
}

void test_spot_target_slack() {
    section("Chỗ đánh quái: the target is let go of 60 px past the area's edge, as no kill");

    bool walking = false, kept = false, let_go = false, counted = false, no_loot = false;
    const std::vector<std::string> log = info_lines([&] {
        HunterConfig cfg = spotted();
        cfg.loot = true;
        cfg.loot_key = 'A';
        HuntRig r(cfg, false, true);
        r.pass();
        r.w.me_x = 700;
        r.w.mobs = { plate(930, 400, "Chay") };    // inside, 230 px off
        r.pass();
        walking = r.target() == "Chay" && r.w.held.count(VK_RIGHT) == 1;
        move_to(r.w.mob("Chay"), 990);            // 50 px past the edge
        r.pass();
        kept = r.target() == "Chay" && r.w.held.count(VK_RIGHT) == 1;
        move_to(r.w.mob("Chay"), 1010);           // 70 px past it
        r.pass();
        const Hunter::Status st = r.hunter.status();
        let_go = r.target().empty() && r.w.held.empty() && st.outside_area;
        counted = st.targets_finished == 0 && st.targets_dropped == 0;
        r.run(1000);
        no_loot = r.w.of('A').empty();
    });
    check(walking && kept, "walked to inside the area, and still once it is 50 px past the edge");
    check(let_go && counted && no_loot,
          "70 px past it: let go of, and the walk with it -- no kill, no give-up counted, nothing picked up");
    check(lines_with(log, "Hunter: letting go of Chay", "outside the hunting area") == 1, "and the log says why");
}

void test_spot_walk_back() {
    section("Chỗ đánh quái: nothing to fight away from the spot -- walked back after a moment, let go of there");

    {
        HuntRig r(spotted(), false, true);
        r.w.step_px = 20;
        const std::uint64_t t0 = r.w.t;
        r.pass();
        r.w.me_x = 1100;                        // pushed 460 px to the right
        while (r.w.t < t0 + 1500) r.pass();
        const bool waited = r.w.presses.empty() && r.hunter.status().state == Hunter::State::NoTargets;
        r.pass();
        const Hunter::Status going = r.hunter.status();
        const bool went = r.w.held.count(VK_LEFT) == 1 && going.state == Hunter::State::Returning &&
                          going.returns == 1 && std::abs(going.spot_dx + 460.0f) < 0.5f;
        r.run(2000);
        const Hunter::Status st = r.hunter.status();
        check(waited, "nothing for spot_return_delay_ms, 1.5 s");
        check(went, "then the left arrow held: Returning, one walk back, the spot 460 px to the left");
        check(r.w.held.empty() && std::abs(r.w.me_x - 640) <= 48 && walks_of(r.w, VK_LEFT) == 1 &&
                  r.w.of(VK_RIGHT).empty() && st.state == Hunter::State::NoTargets && st.returns == 1 &&
                  st.return_give_ups == 0,
              "let go of within 48 px of it, in one walk -- " + std::to_string(r.w.me_x - 640) + " px off");
        const std::size_t keys = r.w.presses.size();
        r.run(3000);
        check(r.w.presses.size() == keys, "and there it stays");
        r.hunter.stop();
        r.hunter.start_stepped();
        check(r.hunter.status().returns == 0 && r.hunter.status().return_give_ups == 0,
              "stopped and started again: the walks back counted afresh for the new run's spot");
    }
    {
        HuntRig r(spotted(), false, true);
        r.w.step_px = 20;
        const std::uint64_t t0 = r.w.t;
        r.pass();
        r.w.me_x = 1100;
        while (r.w.held.empty() && r.w.t < t0 + 3000) r.pass();
        r.pass();
        const bool walking = r.w.held.count(VK_LEFT) == 1;
        r.w.mobs = { plate(r.w.me_x - 100, 400, "Gan") };
        r.pass();
        check(walking && r.w.held.empty() && !r.w.of('Z').empty() &&
                  r.hunter.status().state == Hunter::State::Attacking,
              "walking back, a monster comes within reach: the walk let go of and the monster attacked in that pass");
    }
    {
        HuntRig r(spotted(), false, true);
        r.w.step_px = 20;
        const std::uint64_t t0 = r.w.t;
        r.pass();
        r.w.me_x = 760;
        while (r.w.held.empty() && r.w.t < t0 + 3000) r.pass();
        const bool walking = r.w.held.count(VK_LEFT) == 1;
        r.w.mobs = { plate(r.w.me_x + 180, 400, "Trong") };   // inside the area, out of reach, behind it
        r.pass();
        check(walking && r.w.held.count(VK_RIGHT) == 1 && r.w.held.count(VK_LEFT) == 0 && r.target() == "Trong",
              "walking back, one comes up inside the area the other way: gone for at once");
    }
    {
        // A kill owes its pick-ups first, stood for; the walk back after them
        // and after the delay.
        HunterConfig cfg = spotted();
        cfg.loot = true;
        cfg.loot_key = 'A';
        HuntRig r(cfg, false, true);
        r.w.step_px = 20;
        r.pass();
        r.w.me_x = 900;
        r.w.mobs = { plate(960, 400, "Moc") };
        r.run(600);
        const std::uint64_t killed = r.w.t;
        r.w.mobs.clear();
        r.run(3000);
        const std::vector<World::Press> loots = r.w.of('A');
        std::uint64_t walked = 0;
        for (const World::Press& p : r.w.presses)
            if (walked == 0 && p.vk == VK_LEFT && p.hold == World::kHeld) walked = p.at;
        check(loots.size() >= 4 && walked != 0 && walked > loots.back().at && walked >= killed + 1400,
              "a kill: the three pick-ups pressed standing, and only then, after the delay, the walk back");
    }
    {
        // Standing still (Đứng yên) with a spot: a monster out of reach is
        // not walked to; pushed off, walked back.
        HunterConfig cfg = spotted();
        cfg.stay_put = true;
        HuntRig r(cfg, false, true);
        r.w.step_px = 20;
        r.pass();
        r.w.me_x = 440;                         // pushed 200 px to the left
        r.w.mobs = { plate(900, 400, "Xa") };   // inside the area, out of reach
        r.run(4000);
        const Hunter::Status st = r.hunter.status();
        check(std::abs(r.w.me_x - 640) <= 48 && r.w.held.empty() && walks_of(r.w, VK_RIGHT) == 1 &&
                  st.state == Hunter::State::Waiting && st.returns == 1,
              "standing still with a spot, pushed 200 px off: walked back and let go of at the spot, never on to "
              "the monster out of reach -- at " + std::to_string(r.w.me_x - 640) + " px");
        r.w.mobs = { plate(r.w.me_x + 100, 400, "Gan") };
        r.run(600);
        check(!r.w.of('Z').empty() && walks_of(r.w, VK_RIGHT) == 1, "and one within reach attacked where it stands");
    }
}

void test_spot_give_up() {
    section("Chỗ đánh quái: a walk back the reports say gets nowhere is given up on, tried again, and stopped");

    {
        // A wall between: the character does not move (step_px 0), and the
        // packets, with no report between a walk's start and stop, work it
        // out to the spot -- let go of there, and the stop's report puts it
        // back where it is. Again and again, until 4 s by the reports.
        HuntRig r(spotted(), false, true);
        r.pass();
        r.w.me_x = 1100;
        r.pass();
        r.w.reports = false;
        std::vector<std::uint64_t> tries, gave;
        bool worked_out_home = false;
        const std::vector<std::string> log = info_lines([&] {
            r.run(125'000, [&] {
                const Hunter::Status st = r.hunter.status();
                if (st.returns > tries.size()) tries.push_back(r.w.t - 60);
                if (st.return_give_ups > gave.size()) gave.push_back(r.w.t - 60);
                if (gave.empty() && !tries.empty() && walks_of(r.w, VK_LEFT) > 1) worked_out_home = true;
            });
        });
        check(lines_with(log, "the walk back to the hunting spot got nowhere in 4 s", "trying again in 30 s") == 2 &&
                  lines_with(log, "got nowhere in 4 s, 3 times in a row -- no more tries") == 1 &&
                  lines_with(log, "walks back to the spot", "of them given up") >= 1,
              "the log says each give-up, the last as the last, and the minute's line counts the walks back");
        const auto apart = [](std::uint64_t a, std::uint64_t b, std::uint64_t lo, std::uint64_t hi) {
            return b >= a + lo && b <= a + hi;
        };
        check(tries.size() == 3 && gave.size() == 3 && worked_out_home,
              "three walks back, each given up on, the working-out reaching the spot over and over meanwhile -- " +
                  std::to_string(tries.size()) + " and " + std::to_string(gave.size()));
        check(tries.size() == 3 && gave.size() == 3 && apart(tries[0], gave[0], 4000, 4100) &&
                  apart(gave[0], tries[1], 30'000, 30'060) && apart(tries[1], gave[1], 4000, 4100) &&
                  apart(gave[1], tries[2], 30'000, 30'060) && apart(tries[2], gave[2], 4000, 4100),
              "each given up on 4 s in, by the reports, and tried again 30 s after");
        const Hunter::Status st = r.hunter.status();
        check(r.w.held.empty() && st.spot_why == Hunter::SpotWhy::GaveUp && st.returns == 3 && st.return_give_ups == 3,
              "after the third, no more tries: GaveUp, nothing held, for the 40 s since");
        // A new start counts this run's walks back, and give-ups, from none.
        {
            HuntRig again(spotted(), false, true);
            again.w.reports = false;
            again.w.me_x = 1100;
            again.w.step_px = 0;
            again.run(1000);                    // the spot is where it stands
            again.w.me_x = 1500;                // pushed off; a wall in the way
            again.run(8000);
            const bool had = again.hunter.status().return_give_ups >= 1;
            again.hunter.stop();
            again.hunter.start_stepped();
            check(had && again.hunter.status().return_give_ups == 0 && again.hunter.status().returns == 0,
                  "a run's give-ups (" + std::string(had ? "one or more" : "none") +
                      ") are not the next run's: a new start shows none");
        }
        // Back at the spot by itself: tried again when pushed off again.
        r.w.reports = true;
        r.w.me_x = 640;
        r.pass();
        r.w.me_x = 1100;
        r.w.step_px = 20;
        r.run(600);
        check(r.w.held.count(VK_LEFT) == 1 && r.hunter.status().returns == 4,
              "back at the spot by itself, then pushed off again: walked back again");
    }
    for (const bool hold : { true, false }) {
        HuntRig r(spotted(), false, true);
        r.pass();
        r.w.me_x = 1100;
        r.pass();
        r.w.reports = false;
        for (int i = 0; i < 60 && walks_of(r.w, VK_LEFT) == 0; ++i) r.pass();
        r.run(2000);
        if (hold) r.coord.engage_critical("trash cleaning");
        else      r.w.focused = false;
        const std::size_t before = walks_of(r.w, VK_LEFT);
        r.run(5000);
        const bool still = walks_of(r.w, VK_LEFT) == before && r.w.held.empty();
        if (hold) r.coord.disengage_critical();
        else      r.w.focused = true;
        const std::uint64_t back = r.w.t;
        while (r.w.t < back + 1500) r.pass();
        const bool not_yet = r.hunter.status().return_give_ups == 0;
        while (r.w.t < back + 2700) r.pass();
        check(still && not_yet && r.hunter.status().return_give_ups == 1,
              std::string(hold ? "a 5-s hold" : "the game out of focus 5 s") +
                  " 2 s into a walk back against a wall: nothing meanwhile, and not counted -- given up on 2 s after "
                  "it, not at once");
    }
}

void test_spot_heights() {
    section("Chỗ đánh quái: a spot one jump up is hopped to, a higher one not walked to, a lower one walked toward");

    {
        std::size_t keys = 1;
        Hunter::SpotWhy why = Hunter::SpotWhy::None;
        const std::vector<std::string> log = info_lines([&] {
            HuntRig r(spotted(), false, true);
            r.pass();
            r.w.me_x = 1100;
            r.w.me_y = 600;                     // the spot 460 px left and 200 px above
            r.run(4000);
            keys = r.w.presses.size();
            why = r.hunter.status().spot_why;
        });
        check(keys == 0 && why == Hunter::SpotWhy::TooHigh && lines_with(log, "higher than one jump") == 1,
              "200 px above, past one jump: no key at all, TooHigh, said once");
    }
    {
        HuntRig r(spotted(), false, true);      // jump key Space, as a file without one has
        r.w.step_px = 20;
        r.pass();
        r.w.me_x = 1100;
        r.w.me_y = 300;                         // the spot 100 px below
        r.run(4000);
        check(walks_of(r.w, VK_LEFT) >= 1 && r.w.of(VK_SPACE).empty() && r.w.of(VK_DOWN).empty() &&
                  std::abs(r.w.me_x - 640) <= 48,
              "100 px below: walked toward along the ground -- no jump, never Down -- to within 48 px of its x");
    }
    {
        HunterConfig cfg = spotted();
        cfg.jump_key = VK_UP;
        HuntRig r(cfg, false, true);
        r.w.step_px = 20;
        r.pass();
        r.w.me_x = 1400;
        r.w.me_y = 480;                         // 760 px right of it, 80 px below
        r.run(4000);
        const std::vector<World::Press> hops = r.w.of(VK_UP);
        bool apart = true;
        for (std::size_t i = 1; i < hops.size(); ++i)
            if (hops[i].at - hops[i - 1].at < Hunter::kJumpAirMs) apart = false;
        check(hops.size() >= 2 && apart && walks_of(r.w, VK_LEFT) >= 1 && std::abs(r.w.me_x - 640) <= 48,
              "80 px above: walked back with a hop on the jump key, each a whole jump after the last -- " +
                  std::to_string(hops.size()) + " hops");
    }
    {
        // The edges: kSpotFloorPx (31) of rise is the same floor -- a slope,
        // walked up with no hop, as a hop may land the character on the
        // platform above -- and kOneJumpPx (120) the most one jump reaches.
        HunterConfig cfg = spotted();
        cfg.jump_key = VK_UP;
        HuntRig slope(cfg, false, true);
        slope.w.step_px = 20;
        slope.pass();
        slope.w.me_x = 1100;
        slope.w.me_y = 420;                     // the spot 20 px above
        slope.run(4000);
        check(walks_of(slope.w, VK_LEFT) >= 1 && slope.w.of(VK_UP).empty() && std::abs(slope.w.me_x - 640) <= 48,
              "20 px above, up a slope: walked back to within 48 px with no hop -- at " +
                  std::to_string(slope.w.me_x - 640) + " px");
        HuntRig high(cfg, false, true);
        high.w.step_px = 20;
        high.pass();
        high.w.me_x = 1100;
        high.w.me_y = 525;                      // 125 px above
        high.run(3000);
        check(high.w.presses.empty() && high.hunter.status().spot_why == Hunter::SpotWhy::TooHigh,
              "125 px above, past one jump: no key at all, TooHigh");
        HuntRig jump(cfg, false, true);
        jump.w.step_px = 20;
        jump.pass();
        jump.w.me_x = 1100;
        jump.w.me_y = 515;                      // 115 px above
        jump.run(4000);
        check(!jump.w.of(VK_UP).empty() && walks_of(jump.w, VK_LEFT) >= 1 && std::abs(jump.w.me_x - 640) <= 48,
              "115 px above, within one jump: walked back with hops on the jump key");
    }
    {
        HunterConfig cfg = spotted();
        cfg.jump_key = VK_DOWN;
        HuntRig r(cfg, false, true);
        r.w.step_px = 20;
        r.pass();
        r.w.me_x = 1400;
        r.w.me_y = 480;
        r.run(4000);
        const bool walked_back = walks_of(r.w, VK_LEFT) >= 1;
        r.w.mobs = { plate(r.w.me_x + 220, 300, "Cao") };     // inside the area, 180 px up
        r.run(2000);
        check(walked_back && walks_of(r.w, VK_RIGHT) >= 1 && r.w.of(VK_DOWN).empty(),
              "the jump key set to Down: walked back to the spot above and to a monster up high, and Down never "
              "pressed");
    }
    {
        // With the spot on, never a hop toward a monster: it may leave the
        // character up on the platform above, off its spot.
        HunterConfig on = spotted(), off;
        on.jump_key = off.jump_key = VK_UP;
        HuntRig a(off);
        HuntRig b(on, false, true);
        for (HuntRig* r : { &a, &b }) {
            r->w.mobs = { plate(800, 300, "Cao") };
            r->run(2000);
        }
        check(!a.w.of(VK_UP).empty() && b.w.of(VK_UP).empty() && walks_of(b.w, VK_RIGHT) >= 1,
              "a monster 100 px up inside the area: walked to, but hopped toward only with the spot off");
    }
}

void test_spot_map_entry() {
    section("Chỗ đánh quái: a map entry suspends the spot; one back where the character left resumes it");

    HuntRig r(spotted(), false, true);
    r.w.step_px = 20;
    const std::uint64_t t0 = r.w.t;
    r.pass();
    r.w.me_x = 1100;
    while (r.w.held.empty() && r.w.t < t0 + 3000) r.pass();
    r.pass();
    const bool walking = r.w.held.count(VK_LEFT) == 1;
    // Through a door: another map's spawn, far off.
    r.w.epoch = 2;
    r.w.entry_x = 20000.0f;
    r.w.entry_y = 1100.0f;
    r.w.me_x = 16000;
    r.pass();
    Hunter::Spot s = r.hunter.spot();
    Hunter::Status st = r.hunter.status();
    check(walking && r.w.held.empty() && s.suspended && st.spot_suspended &&
              st.spot_why == Hunter::SpotWhy::Suspended && s.left_x > 4900.0f && s.left_x < 5100.0f,
          "a map entry while walking back: let go of in that very pass, the spot suspended, and where the "
          "character left from kept -- " + std::to_string(std::lround(s.left_x)));
    const float left = s.left_x;
    const std::size_t walks = walks_of(r.w, VK_LEFT) + walks_of(r.w, VK_RIGHT);
    r.w.mobs = { plate(16200, 400, "Xa") };
    r.run(2000);
    check(walks_of(r.w, VK_LEFT) + walks_of(r.w, VK_RIGHT) == walks &&
              r.hunter.status().state == Hunter::State::Waiting,
          "suspended: standing, a monster out of reach not walked to");
    r.w.mobs.clear();
    // Another entry, 400 px from where it left: still aside.
    r.w.epoch = 3;
    r.w.entry_x = left + 400.0f;
    r.w.me_x = static_cast<int>(left) + 400 - r.w.world_dx;
    r.run(600);
    check(r.hunter.spot().suspended && walks_of(r.w, VK_LEFT) + walks_of(r.w, VK_RIGHT) == walks,
          "an entry 400 px from where it left: still suspended, nothing walked");
    // Back through the door, 100 px from where it left: in use again.
    r.w.epoch = 4;
    r.w.entry_x = left + 100.0f;
    r.w.me_x = static_cast<int>(left) + 100 - r.w.world_dx;
    const std::size_t lefts = walks_of(r.w, VK_LEFT);
    r.pass();
    s = r.hunter.spot();
    st = r.hunter.status();
    r.run(3000);
    check(!s.suspended && s.epoch == 4 && s.x == 4640.0f && st.spot_why == Hunter::SpotWhy::None &&
              walks_of(r.w, VK_LEFT) > lefts && std::abs(r.w.me_x - 640) <= 48,
          "back 100 px from where it left: the spot in use again, and walked back to");
}

void test_spot_asked() {
    section("Chỗ đánh quái: logged back in (suspend_spot), and Đặt chỗ này (retake_spot)");

    HuntRig r(spotted(), false, true);
    r.w.step_px = 20;
    r.pass();
    r.hunter.suspend_spot();
    r.w.me_x = 1100;
    r.run(3000);
    Hunter::Spot s = r.hunter.spot();
    check(s.suspended && s.left_x == 4640.0f && r.w.presses.empty() &&
              r.hunter.status().spot_why == Hunter::SpotWhy::Suspended,
          "logged back in: the spot stands aside, with where the character was last known, and nothing walks "
          "back to it");
    r.w.mobs = { plate(1150, 400, "Gan") };
    r.run(600);
    const bool fought = !r.w.of('Z').empty();
    r.w.mobs.clear();
    r.hunter.retake_spot();
    r.pass();
    s = r.hunter.spot();
    const std::size_t keys = r.w.presses.size();
    r.run(3000);
    check(fought, "a monster within reach is fought all the same");
    check(s.set && !s.suspended && s.x == 5100.0f && r.hunter.status().spot_why == Hunter::SpotWhy::None &&
              r.w.presses.size() == keys,
          "Đặt chỗ này: the spot taken again where the character stands, in use, nothing to walk back to");
}

void test_spot_no_fix() {
    section("Chỗ đánh quái: lost or no capture -- standing, as Đứng yên; no report yet -- hunting as without it");

    for (const WorldFix::State state : { WorldFix::State::Lost, WorldFix::State::NoCapture }) {
        HuntRig r(spotted(), false, true);
        r.pass();
        r.w.fix_state = state;
        r.w.mobs = { plate(900, 400, "Xa") };
        r.run(2000);
        const Hunter::Status st = r.hunter.status();
        const bool stood = r.w.presses.empty() && st.state == Hunter::State::Waiting &&
                           st.spot_why == (state == WorldFix::State::Lost ? Hunter::SpotWhy::Paused
                                                                         : Hunter::SpotWhy::NoFix);
        r.w.mobs = { plate(740, 400, "Gan") };
        r.run(1000);
        check(stood && !r.w.of('Z').empty() && r.w.of(VK_RIGHT).size() == 1,
              std::string(state == WorldFix::State::Lost ? "the packets lost the character" : "no capture") +
                  ": one out of reach not walked to; one within reach turned to once and attacked");
    }
    {
        HuntRig r(spotted());       // no fix to be had at all
        r.w.mobs = { plate(900, 400, "Xa") };
        r.run(2000);
        check(r.w.presses.empty() && !r.hunter.spot().set && r.hunter.status().spot_why == Hunter::SpotWhy::NoFix &&
                  r.hunter.status().state == Hunter::State::Waiting,
              "no capture from the start: no spot ever, standing");
    }
    {
        HuntRig a(HunterConfig{});
        HuntRig b(spotted(), false, true);
        b.w.fix_state = WorldFix::State::FirstReport;
        for (HuntRig* r : { &a, &b }) {
            r->w.step_px = 20;
            r->w.mobs = { plate(1000, 400, "Xa") };
            r->run(3000);
        }
        check(same_keys(a.w, b.w) && walks_of(b.w, VK_RIGHT) >= 1 && !b.w.of('Z').empty() && !b.hunter.spot().set &&
                  b.hunter.status().spot_why == Hunter::SpotWhy::WaitingFirst,
              "no report of yours yet: walked to the monster and fought exactly as with the spot off");
    }
}

void test_spot_config() {
    section("Chỗ đánh quái: hunt.ini keys saved, read back, defaulted and kept within range");

    const HunterConfig none;
    check(!none.spot && none.spot_half_w == 300 && none.spot_half_h == 100 && none.spot_tolerance == 48 &&
              none.spot_return_delay_ms == 1500 && none.spot_give_up_ms == 4000 && none.spot_retry_ms == 30000,
          "off by default: +-300 / +-100, 48 px, 1.5 s, 4 s and 30 s");
    const std::string path = temp_path("gobot_hunter_spot.ini");
    HunterConfig set;
    set.spot = true;
    set.spot_half_w = 450;
    set.spot_half_h = 150;
    set.spot_tolerance = 80;
    set.spot_return_delay_ms = 2500;
    set.spot_give_up_ms = 6000;
    set.spot_retry_ms = 45000;
    HunterConfig back;
    IniFile raw;
    check(set.save(path) && back.load(path) && raw.load(path) && back.spot && back.spot_half_w == 450 &&
              back.spot_half_h == 150 && back.spot_tolerance == 80 && back.spot_return_delay_ms == 2500 &&
              back.spot_give_up_ms == 6000 && back.spot_retry_ms == 45000 && raw.get_bool("Hunt", "spot", false) &&
              raw.get_int("Hunt", "spot_half_w", 0) == 450,
          "spot and its six numbers written to [Hunt] and read back");
    write_file(path, "[Hunt]\nreach=150\n");
    HunterConfig old = set;
    check(old.load(path) && !old.spot && old.spot_half_w == 300 && old.spot_half_h == 100 && old.spot_tolerance == 48 &&
              old.spot_return_delay_ms == 1500 && old.spot_give_up_ms == 4000 && old.spot_retry_ms == 30000,
          "a hunt.ini without them: off, and the defaults");
    write_file(path, "[Hunt]\nspot_half_w=10\nspot_half_h=9999\nspot_tolerance=0\nspot_return_delay_ms=-5\n"
                     "spot_give_up_ms=10\nspot_retry_ms=99999999\n");
    HunterConfig wild;
    check(wild.load(path) && wild.spot_half_w == 50 && wild.spot_half_h == 500 && wild.spot_tolerance == 10 &&
              wild.spot_return_delay_ms == 0 && wild.spot_give_up_ms == 3000 && wild.spot_retry_ms == 600000,
          "out of range in the file: 50-2000, 20-500, 10-1000, 0-60000, 3000-60000 (past the walk's 2.5-s "
          "renewal, its first report on a clear floor) and 1000-600000");
    write_file(path, "[Hunt]\nspot_half_w=4000\nspot_half_h=1\nspot_tolerance=5000\nspot_return_delay_ms=999999\n"
                     "spot_give_up_ms=999999\nspot_retry_ms=1\n");
    HunterConfig wild2;
    check(wild2.load(path) && wild2.spot_half_w == 2000 && wild2.spot_half_h == 20 && wild2.spot_tolerance == 1000 &&
              wild2.spot_return_delay_ms == 60000 && wild2.spot_give_up_ms == 60000 && wild2.spot_retry_ms == 1000,
          "and from the other side");
    remove_file(path);
}

/// The usual start with the packets on: the capture begun, no report of
/// yours yet (FirstReport), so hunted as without a spot -- the first walk's
/// start report places the character. Known but moving, it is not settled:
/// the hunter stands and lets go of the walk, and the spot is taken where it
/// stopped, near where BẮT ĐẦU was pressed -- not at the first monster.
void test_spot_first_walk() {
    section("Chỗ đánh quái: the first walk's report places the character -- it stands, the spot taken there");

    HuntRig r(spotted(), false, true);
    r.w.fix_state = WorldFix::State::FirstReport;
    r.w.step_px = 20;
    r.w.mobs = { plate(1040, 400, "Xa") };      // 400 px off, out of reach
    r.pass();
    const bool walked =
        r.w.held.count(VK_RIGHT) == 1 && r.hunter.status().spot_why == Hunter::SpotWhy::WaitingFirst;
    // The walk's start report: the character known, and walking.
    r.w.fix_state = WorldFix::State::Known;
    r.pass();
    const Hunter::Status st = r.hunter.status();
    const bool stood = r.w.held.empty() && st.state == Hunter::State::Waiting &&
                       st.spot_why == Hunter::SpotWhy::Settling && !r.hunter.spot().set;
    r.pass();
    const Hunter::Spot s = r.hunter.spot();
    r.run(2000);
    check(walked, "no report of yours yet: the monster walked to, as with the spot off");
    check(stood, "the walk's report has the character known but walking: it stands, the walk let go of, no spot yet");
    check(s.set && s.x >= 4640.0f && s.x <= 4680.0f && walks_of(r.w, VK_RIGHT) == 1 && r.w.held.empty(),
          "stopped: the spot taken there, within two steps of the start -- " + std::to_string(std::lround(s.x)) +
              " -- and the monster, outside its area, never walked to again");
}

/// Logged back in (suspend_spot: the keeper, or a game opened again and found
/// with Tìm lại): the spot aside until the character stands, settled, within
/// kSpotBackPx of where it was -- by a look from after the log in, as the
/// new game's capture begins knowing nothing of the character.
void test_spot_login() {
    section("Chỗ đánh quái: logged back in -- in use again once the character stands within 150 px of where it was");

    // The log in: no capture a moment, then the new game's, with no report
    // of yours yet -- and the login's own map entry seen, its spawn far off.
    const auto logged_in = [](HuntRig& r) {
        r.hunter.suspend_spot();
        r.w.fix_state = WorldFix::State::NoCapture;
        r.run(600);
        r.w.fix_state = WorldFix::State::FirstReport;
        r.w.epoch = 2;
        r.w.entry_x = 9000.0f;
        r.w.entry_y = 1100.0f;
        r.run(600);
    };
    {
        bool aside = false, unsettled = false, back = false;
        Hunter::Spot s;
        HuntRig r(spotted(), false, true);
        r.w.step_px = 20;
        const std::vector<std::string> log = info_lines([&] {
            r.pass();
            logged_in(r);
            const Hunter::Status st = r.hunter.status();
            aside = r.hunter.spot().suspended && r.hunter.spot().login && r.hunter.spot().left_x == 4640.0f &&
                    st.spot_suspended && st.spot_login && st.spot_why == Hunter::SpotWhy::Suspended &&
                    r.w.presses.empty();
            // Where it was, 100 px off -- in mid-jump first.
            r.w.me_x = 740;
            r.w.fix_state = WorldFix::State::Known;
            r.w.airborne = true;
            r.run(600);
            unsettled = r.hunter.spot().suspended && r.w.presses.empty();
            r.w.airborne = false;
            r.pass();
            s = r.hunter.spot();
            back = !s.suspended && !s.login && s.epoch == 2 && s.x == 4640.0f &&
                   r.hunter.status().spot_why == Hunter::SpotWhy::None && !r.hunter.status().spot_login;
        });
        // In use: pushed off, walked back.
        r.w.me_x = 1000;
        r.run(4000);
        check(aside, "logged back in: the spot aside as a log in, where the character was kept (4640), nothing "
                     "pressed -- through the capture started again and the login's own far-off entry");
        check(unsettled, "100 px from where it was but in mid-jump: still aside");
        check(back && std::abs(r.w.me_x - 640) <= 48,
              "standing there, settled: in use again, with the map entries of now (2), and walked back to when "
              "pushed off");
        check(lines_with(log, "Hunter: logged back in, the character stands at 4740,1100, 100 px from where it was",
                         "in use again") == 1,
              "and the log says so, once");
    }
    {
        HuntRig r(spotted(), false, true);
        r.w.step_px = 20;
        r.pass();
        logged_in(r);
        r.w.me_x = 1040;                        // 400 px from where it was
        r.w.fix_state = WorldFix::State::Known;
        r.w.mobs = { plate(1240, 400, "Xa") };  // out of reach
        r.run(3000);
        check(r.hunter.spot().suspended && r.w.presses.empty() &&
                  r.hunter.status().spot_why == Hunter::SpotWhy::Suspended,
              "standing 400 px from where it was: still aside -- standing, a monster out of reach not walked to");
    }
    {
        // A fix still from before the log in -- the old game's capture, no
        // report since -- is not the character come back; the new game's
        // first look is.
        HuntRig r(spotted(), false, true);
        r.pass();
        r.w.reports = false;                    // standing: nothing sent
        r.w.me_x = 700;                         // 60 px off, settled, known all along
        r.hunter.suspend_spot();
        r.run(1000);
        const bool old = r.hunter.spot().suspended;
        r.w.fix_state = WorldFix::State::FirstReport;
        r.pass();
        r.w.fix_state = WorldFix::State::Known;
        r.pass();
        check(old && !r.hunter.spot().suspended,
              "known all along from before the log in, nothing reported since: still aside; once the packets have "
              "begun afresh, the same place puts it back in use");
    }
    {
        // The same game found again (Tìm lại on its own window): its capture
        // goes on, and never stops knowing the character. A report newer
        // than the last before the log in -- a step, a skill -- is the
        // character as it is now.
        HuntRig r(spotted(), false, true);
        r.pass();
        r.w.reports = false;
        r.w.me_x = 700;
        r.pass();
        r.hunter.suspend_spot();
        r.run(1000);
        const bool waiting = r.hunter.spot().suspended;
        r.w.report();                           // a skill cast: a report of now
        r.pass();
        check(waiting && !r.hunter.spot().suspended,
              "the same game's capture going on: aside until a report newer than the log in, then back in use");
    }
    {
        // Pushed off when the log in came, and back standing on the spot
        // itself, far from where it was last known: that is the place too.
        HuntRig r(spotted(), false, true);
        r.pass();
        r.w.me_x = 1000;                        // 360 px off the spot
        r.w.reports = false;
        r.pass();
        r.hunter.suspend_spot();
        r.pass();
        const float left = r.hunter.spot().left_x;
        r.w.fix_state = WorldFix::State::FirstReport;
        r.pass();
        r.w.fix_state = WorldFix::State::Known;
        r.w.me_x = 680;                         // 40 px from the spot, 320 from where it was
        r.pass();
        check(left == 5000.0f && !r.hunter.spot().suspended,
              "logged out 360 px off the spot, back standing 40 px from the spot itself: in use again");
    }
    {
        // Back near where it was -- but the packets know it is another place
        // than the spot's.
        HuntRig r(spotted(), false, true);
        r.pass();
        r.hunter.suspend_spot();
        r.w.fix_state = WorldFix::State::FirstReport;
        r.pass();
        r.w.fix_state = WorldFix::State::Known;
        r.w.map = 12;
        r.w.me_x = 700;
        r.run(1000);
        check(r.hunter.spot().suspended,
              "logged back in near where it was, on a place the packets know is another (12/1, the spot on 11/1): "
              "still aside");
    }
    {
        // Through a door first, then logged back in: the character was on
        // another map before the log in, so the entry's rule stands -- only
        // a door back near where it left from brings it back, and standing
        // near that point does not.
        HuntRig r(spotted(), false, true);
        r.pass();
        r.w.epoch = 2;
        r.w.entry_x = 20000.0f;
        r.w.entry_y = 1100.0f;
        r.w.map = 12;
        r.w.me_x = 16000;
        r.pass();
        r.hunter.suspend_spot();
        r.pass();
        const Hunter::Spot aside = r.hunter.spot();
        r.w.fix_state = WorldFix::State::FirstReport;
        r.w.map_known = false;
        r.pass();
        r.w.fix_state = WorldFix::State::Known;
        r.w.me_x = 760;                         // 120 px from where it left
        r.run(1000);
        const bool stays = r.hunter.spot().suspended;
        r.w.epoch = 3;                          // the door back, near where it left
        r.w.entry_x = 4700.0f;
        r.w.map_known = true;
        r.w.map = 11;
        r.pass();
        check(aside.suspended && !aside.login && aside.left_x == 4640.0f && stays && !r.hunter.spot().suspended,
              "a map entry, then a log in: still aside as the entry left it, not by standing near where it left "
              "from -- and back in use at the door back there");
    }
    {
        // Logged back in, then a map entry somewhere else -- the spot set
        // with the map not known, as before any door: no longer a log in --
        // standing near where it was, after that, may be another map's
        // pixels.
        HuntRig r(spotted(), false, true);
        r.w.map_known = false;
        r.pass();
        r.hunter.suspend_spot();
        r.w.fix_state = WorldFix::State::FirstReport;
        r.pass();
        r.w.epoch = 2;
        r.w.entry_x = 9000.0f;
        r.w.entry_y = 1100.0f;
        r.w.map_known = true;
        r.w.map = 12;
        r.pass();
        r.w.fix_state = WorldFix::State::Known;
        r.w.me_x = 700;
        r.run(1000);
        const Hunter::Spot s = r.hunter.spot();
        check(s.suspended && !s.login,
              "logged back in, then a map entry far from where it left: aside as that entry left it, not back by "
              "standing near the old place");
    }
}

/// With only "Đánh trước" to be attacked, a target whose name does not read
/// this look is NotFirst, and kept as the one it was. Gone past the area's
/// edge and its slack, it is let go of all the same.
void test_spot_nameless_target() {
    section("Chỗ đánh quái: a target of only Đánh trước whose name does not read is let go of past the edge too");

    bool walking = false, kept = false, let_go = false, no_more = false;
    const std::vector<std::string> log = info_lines([&] {
        HunterConfig cfg = spotted();
        cfg.first_names = { "Chay" };
        cfg.first_only = true;
        HuntRig r(cfg, false, true);
        r.pass();
        r.w.me_x = 700;
        r.w.mobs = { plate(930, 400, "Chay") };    // inside, 230 px off
        r.pass();
        walking = r.target() == "Chay" && r.w.held.count(VK_RIGHT) == 1;
        r.w.mobs = { plate(990, 400) };            // its name not read; 50 px past the edge
        r.pass();
        kept = r.w.held.count(VK_RIGHT) == 1 && r.hunter.status().state == Hunter::State::Approaching;
        r.w.mobs = { plate(1010, 400) };           // nameless still, 70 px past it
        r.pass();
        const Hunter::Status st = r.hunter.status();
        let_go = r.w.held.empty() && st.state == Hunter::State::NoTargets && st.outside_area &&
                 st.targets_finished == 0 && st.targets_dropped == 0;
        r.run(1000);
        no_more = walks_of(r.w, VK_RIGHT) == 1;
    });
    check(walking && kept, "walked to by its name, and on with the name not read 50 px past the edge");
    check(let_go && no_more, "nameless 70 px past it: let go of as outside the area, no kill, no give-up, not "
                             "walked after again");
    check(lines_with(log, "Hunter: letting go of a monster", "outside the hunting area") == 1, "and the log says why");
}

/// Unticked while it hunts: the walk back stops at once, and the spot goes.
/// Ticked again where the player has walked the character, the spot is taken
/// there -- "bỏ dấu, đi tới chỗ mới, đánh dấu lại rồi bấm Đặt chỗ này".
void test_spot_switched() {
    section("Chỗ đánh quái: unticked mid-run -- the walk back stops, the spot goes; ticked again -- taken there");

    HuntRig r(spotted(), false, true);
    r.w.step_px = 20;
    const std::uint64_t t0 = r.w.t;
    r.pass();
    r.w.me_x = 1100;
    while (r.w.held.empty() && r.w.t < t0 + 3000) r.pass();
    const bool walking = r.w.held.count(VK_LEFT) == 1;
    HunterConfig off = spotted();
    off.spot = false;
    r.hunter.set_config(off);
    r.pass();
    const bool stopped = r.w.held.empty() && !r.hunter.spot().set;
    r.w.me_x = 900;                             // walked by hand to a new place
    r.run(2000);
    const std::size_t lefts = walks_of(r.w, VK_LEFT);
    r.hunter.set_config(spotted());
    r.pass();
    const Hunter::Spot s = r.hunter.spot();
    r.hunter.retake_spot();
    r.run(3000);
    check(walking && stopped, "unticked while walking back: the walk let go of in that very pass, the spot gone");
    check(s.set && s.x == 4900.0f && r.hunter.spot().x == 4900.0f && walks_of(r.w, VK_LEFT) == lefts &&
              r.w.held.empty() && r.hunter.status().spot_why == Hunter::SpotWhy::None,
          "ticked again at the new place, and Đặt chỗ này: the spot there (4900), and no walk back to the old one");
}

/// What the walk back met -- given up on, too high -- is still said while a
/// monster is fought, where go_home is not reached to say it: the card and
/// the Positions line do not flip to "in use" with every fight.
void test_spot_why_in_fight() {
    section("Chỗ đánh quái: walks back given up on, or a spot too high, still said through a fight");

    {
        HuntRig r(spotted(), false, true);
        r.pass();
        r.w.me_x = 1100;
        r.pass();
        r.w.reports = false;
        r.run(80'000);
        const bool gave_up = r.hunter.status().spot_why == Hunter::SpotWhy::GaveUp;
        r.w.mobs = { plate(r.w.me_x + 100, 400, "Gan") };
        bool said = true;
        r.run(1000, [&] {
            if (r.hunter.status().spot_why != Hunter::SpotWhy::GaveUp) said = false;
        });
        check(gave_up && said && !r.w.of('Z').empty() && r.hunter.status().return_give_ups == 3,
              "three walks back given up on, then a fight: GaveUp at every look of it");
    }
    {
        HuntRig r(spotted(), false, true);
        r.pass();
        r.w.me_x = 1100;
        r.w.me_y = 600;                         // the spot 200 px above
        r.run(600);
        const bool high = r.hunter.status().spot_why == Hunter::SpotWhy::TooHigh;
        r.w.mobs = { plate(1200, 600, "Gan") };
        bool said = true;
        r.run(1000, [&] {
            if (r.hunter.status().spot_why != Hunter::SpotWhy::TooHigh) said = false;
        });
        check(high && said && !r.w.of('Z').empty(), "a spot past one jump above, then a fight: TooHigh at every look");
    }
}

/// A walk back that hops toward a spot above counts the steps back the other
/// way afresh, as one along the ground does: held through each jump, it is
/// walking that way at every look.
void test_spot_hop_regains() {
    section("Chỗ đánh quái: a walk back that hops counts the steps back the other way afresh");

    HunterConfig cfg = spotted();
    cfg.jump_key = VK_UP;
    cfg.keep_distance = true;
    cfg.keep_distance_steps = 1;
    HuntRig r(cfg, false, true);
    r.w.step_px = 20;
    r.pass();
    // Too close on the left: one step back to the right, the most there are.
    r.w.mobs = { plate(610, 400, "Gan") };
    r.run(600);
    const std::uint64_t first = r.hunter.status().backsteps;
    r.w.mobs.clear();
    // Pushed off, below the spot: walked back left, hopping all the way.
    r.w.me_x = 1400;
    r.w.me_y = 480;                             // 760 px right of it, 80 px below
    r.run(5000);
    const bool home = std::abs(r.w.me_x - 640) <= 48 && !r.w.of(VK_UP).empty();
    // Too close on the left again: a step back to the right once more.
    r.w.mobs = { plate(r.w.me_x - 30, 480, "Gan2") };
    r.run(600);
    check(first == 1 && home && r.hunter.status().backsteps == 2,
          "a step back right, then a walk back left with hops: the next monster on the left gets a step back right "
          "again -- " + std::to_string(r.hunter.status().backsteps - first) + " more");
}

void test_hunter_spot() {
    test_spot_off();
    test_spot_taken();
    test_spot_area();
    test_spot_target_slack();
    test_spot_walk_back();
    test_spot_give_up();
    test_spot_heights();
    test_spot_map_entry();
    test_spot_asked();
    test_spot_no_fix();
    test_spot_config();
    test_spot_first_walk();
    test_spot_login();
    test_spot_nameless_target();
    test_spot_switched();
    test_spot_why_in_fight();
    test_spot_hop_regains();
}

// ------------------------------------------------------------- turning

/// How a ranged character turns to its target -- no style, the walk held,
/// the packets steering, as the player hunts -- where it was seen "nhích từng
/// chút khi đang bắn": without a style, keep_distance or stay_put a tap
/// toward the target went out at every pass, and each walked it in a few
/// pixels. Now, for every setup, one tap of kTurnTapMs when the hunter does
/// not take it to be facing the target already; and where the packets steer
/// and know better which way it faces -- a report more than kFacingTrustMs
/// after the hunter's own last turning key, one that says the way the key
/// went, or a key kFacingMissedMs old the game never reported -- they put
/// the hunter's idea right.
void test_hunter_turns() {
    section("Hunter: a turn only when not facing the target -- every setup -- and the packets putting it right");

    HunterConfig ranged;        // no style, hold_walk, keep_distance and stay_put off
    ranged.reach = 450;         // half the 1024-px frame, as the player means to set it
    {
        HuntRig r(ranged);
        r.w.step_px = 20;
        r.w.mobs = { plate(640 + 400, 400, "Xa") };       // 400 px right: in reach
        r.pass();
        const bool first = r.w.presses.size() == 2 && r.w.presses[0].vk == VK_RIGHT &&
                           r.w.presses[0].hold == Hunter::kTurnTapMs && r.w.presses[1].vk == 'Z';
        r.run(100 * 60);
        check(Hunter::kTurnTapMs == 60 && first && r.w.of(VK_RIGHT).size() == 1 && r.w.of(VK_LEFT).empty() &&
                  r.w.of('Z').size() >= 8 && r.w.held.empty() && r.w.me_x == 640 && r.hunter.engaged(),
              "no style, a target 400 px right within a reach of 450, which way it faces not known: one 60-ms tap "
              "right, then the attack -- and over 100 passes no other tap and never a step, " +
                  std::to_string(r.w.of('Z').size()) + " attacks from where it stood");

        // The target the other side now: one tap left. Back on the right: one
        // tap right.
        r.w.mobs = { plate(640 - 300, 400, "Xa") };
        r.run(30 * 60);
        const auto lefts = r.w.of(VK_LEFT);
        const bool left_once = lefts.size() == 1 && lefts[0].hold == Hunter::kTurnTapMs;
        r.w.mobs = { plate(640 + 200, 400, "Xa") };
        r.run(30 * 60);
        const auto rights = r.w.of(VK_RIGHT);
        check(left_once && r.w.of(VK_LEFT).size() == 1 && rights.size() == 2 &&
                  rights[1].hold == Hunter::kTurnTapMs && r.w.held.empty() && r.w.me_x == 640,
              "a target on the other side: one tap left over 30 passes; back on the right, one tap right -- " +
                  std::to_string(r.w.of(VK_LEFT).size()) + " left, " + std::to_string(rights.size()) + " right");
    }

    HunterConfig steered = ranged;
    steered.packet_positions = PacketPositions::On;
    {
        // The packets' fix (Eyes::where), from a game that takes every key:
        // the client reports the hunter's tap as it goes out, and more
        // reports as the test sets them.
        HuntRig r(steered, false, true);
        r.w.turn_reports = true;
        r.w.mobs = { plate(640 + 300, 400, "Xa") };
        r.run(100 * 60);
        const auto taps = [&r] { return r.w.of(VK_RIGHT).size(); };
        check(taps() == 1 && r.w.facing == 1 && r.w.of(VK_LEFT).empty() && r.w.me_x == 640,
              "the packets steering, the game reporting the hunter's turn: one tap over 100 passes, the report of "
              "it taken for what it says -- " + std::to_string(taps()) + " taps");

        // Something else turned the character away -- a report says it faces
        // left, long after the hunter's turn: turned back at the next pass.
        r.w.facing = -1;
        r.w.facing_ms = r.w.t;
        r.pass();
        const bool at_once = taps() == 2;
        const std::uint64_t second = at_once ? r.w.of(VK_RIGHT)[1].at : 0;

        // Then one saying left again, no more than kFacingTrustMs after that
        // tap -- one that may have been sent before the tap reached the game:
        // not taken at once. With no report since for kFacingMissedMs, the
        // game never took the tap, and the character faces left: turned
        // again, once.
        r.w.facing = -1;
        r.w.facing_ms = r.w.t;
        const bool soon = r.w.facing_ms <= second + Hunter::kFacingTrustMs;
        bool waited = true;
        while (r.w.t <= second + Hunter::kFacingMissedMs) {
            r.pass();
            waited = waited && taps() == 2;
        }
        r.run(10 * 60);
        check(at_once && soon && waited && taps() == 3 && r.w.facing == 1,
              "a report long after the tap saying the character faces away: one tap back at the next pass; one "
              "within 150 ms of that tap saying the same: no tap for 500 ms, then one -- " +
                  std::to_string(taps()) + " taps");

        // The packets not knowing the character: whatever the fix says of
        // the facing changes nothing. Known again, it does.
        r.w.fix_state = WorldFix::State::Lost;
        r.w.facing = -1;
        r.w.facing_ms = r.w.t;
        r.run(10 * 60);
        const bool lost_ignored = taps() == 3;
        r.w.fix_state = WorldFix::State::Known;
        r.run(10 * 60);
        check(lost_ignored && taps() == 4 && r.w.of(VK_LEFT).empty(),
              "the fix Lost, saying the character faces away: no tap; Known again, saying the same: one -- " +
                  std::to_string(taps()) + " taps");
    }
    {
        // A tap the game does not take -- a press it read no frame of -- and
        // so never reports: the last report, from before it, still says the
        // character faces left. Tapped again once kFacingMissedMs show it,
        // not sooner; and once the game takes one, no more.
        HuntRig r(steered, false, true);
        r.w.facing = -1;
        r.w.facing_ms = r.w.t;
        r.w.mobs = { plate(640 + 300, 400, "Xa") };
        r.run(20 * 60);
        const std::vector<World::Press> missed = r.w.of(VK_RIGHT);
        bool spaced = missed.size() == 3;
        for (std::size_t i = 1; spaced && i < missed.size(); ++i)
            spaced = missed[i].at - missed[i - 1].at > Hunter::kFacingMissedMs &&
                     missed[i].at - missed[i - 1].at < Hunter::kFacingMissedMs + 120;
        r.w.turn_reports = true;
        r.run(30 * 60);
        check(spaced && r.w.of(VK_RIGHT).size() == missed.size() + 1 && r.w.of(VK_LEFT).empty() &&
                  r.w.facing == 1,
              "taps the game never reports: one, and another each 500 ms and a pass after the last; once one is "
              "reported, no more -- " + std::to_string(missed.size()) + " unreported, " +
                  std::to_string(r.w.of(VK_RIGHT).size()) + " in all");
    }
    {
        // A hold over: on the screen, which way the character faces is not
        // known, and it is turned once more. With the packets, the report of
        // the hunter's own tap -- sent with it, and the last there is, as
        // while the character stands and shoots -- says it faces the way the
        // tap went: no tap.
        std::size_t taps[2] = {};
        for (const bool packets : { false, true }) {
            HuntRig r(steered, false, packets);
            r.w.turn_reports = true;
            r.w.mobs = { plate(640 + 300, 400, "Xa") };
            r.run(10 * 60);
            r.coord.engage_critical("trash cleaning");
            r.run(300);
            r.coord.disengage_critical();
            r.run(10 * 60);
            taps[packets ? 1 : 0] = r.w.of(VK_RIGHT).size();
        }
        check(taps[0] == 2 && taps[1] == 1,
              "a hold over: turned once more on the screen alone; not with the packets, the report of the hunter's "
              "own tap the last saying it faces the target -- " + std::to_string(taps[0]) + " and " +
                  std::to_string(taps[1]) + " taps");
    }
    {
        // Watched only (packet_positions=watch, as hunt.ini ships): the
        // screen fights, and the packets have no say in the turning -- not
        // even asked, with the spot off.
        HuntRig r(ranged, false, true);
        r.w.mobs = { plate(640 + 300, 400, "Xa") };
        r.run(10 * 60);
        r.w.facing = -1;
        r.w.facing_ms = r.w.t;
        r.run(20 * 60);
        check(ranged.packet_positions == PacketPositions::Watch && r.w.of(VK_RIGHT).size() == 1 &&
                  r.w.wheres == 0,
              "packet_positions=watch, a report saying the character faces away: no tap for it, the fix never "
              "asked -- " + std::to_string(r.w.of(VK_RIGHT).size()) + " taps");
    }
    {
        // The spot on: the fix the pass took for it is the one gone by, not
        // asked again -- once a pass. Off: asked only at a pass with a target
        // in reach. Here it says from the start that the character faces the
        // target: never a tap.
        HunterConfig with_spot = steered;
        with_spot.spot = true;
        HuntRig on(with_spot, false, true), off(steered, false, true);
        for (HuntRig* r : { &on, &off }) {
            r->w.facing = 1;
            r->w.facing_ms = r->w.t;
            r->w.mobs = { plate(640 + 200, 400, "Xa") };
            r->run(20 * 60);
        }
        check(on.w.wheres == 20 && off.w.wheres == 20 && on.w.of(VK_RIGHT).empty() && off.w.of(VK_RIGHT).empty() &&
                  on.w.of('Z').size() >= 2 && off.w.of('Z').size() >= 2,
              "the spot on or off: the fix asked once a pass, and saying the character faces the target already, "
              "no tap at all -- asked " + std::to_string(on.w.wheres) + " and " + std::to_string(off.w.wheres) +
                  " times in 20 passes");
    }
    {
        // Once a minute: the turns tapped, the facings the packets put right
        // and, of those, the keys the game missed -- with a style, so that
        // nothing else (a fight ended, a give-up) has the line said, and a
        // minute with none of them says none. The packets filling in a facing
        // not known, at the start, is not counted.
        HunterConfig styled = steered;
        styled.style = "near_wide";
        const std::vector<std::string> log = info_lines([&] {
            HuntRig r(styled, false, true);
            r.w.facing = -1;                    // the character faces left, by a report from before
            r.w.facing_ms = r.w.t;
            r.w.mobs = { plate(640 + 300, 400, "Xa") };
            // Hurt a little at a time: never given up on for taking no
            // damage, never killed in the two minutes and more.
            const auto hurt = [&r] { r.w.mobs[0].hp = std::max(0.2f, r.w.mobs[0].hp - 0.0003f); };
            // A first tap the game misses, never reported; the one tried
            // again it takes.
            r.pass();
            r.w.turn_reports = true;
            r.run(10'000, hurt);
            // Then something else turns the character away.
            r.w.facing = -1;
            r.w.facing_ms = r.w.t;
            r.run(115'000, hurt);
        });
        check(lines_with(log, "Hunter: ", " turns (") == 1 &&
                  lines_with(log, "Hunter: 0 fights, ",
                             ", 3 turns (2 facings put right by the packets, 1 of them keys the game missed)") == 1,
              "the minute's line: 3 turns, 2 facings put right by the packets, 1 a key the game missed -- and the "
              "minute after, with none, no line");
    }
}

/// The minute lines of a run: those that say how the fights went.
std::vector<std::string> minute_lines(const std::vector<std::string>& all) {
    std::vector<std::string> out;
    for (const std::string& l : all)
        if (l.find("Hunter: ") != std::string::npos && l.find(" fights, ") != std::string::npos) out.push_back(l);
    return out;
}

/// The number after `after` in `line`, or -1 without one.
double number_after(const std::string& line, const char* after) {
    const std::size_t at = line.find(after);
    return at == std::string::npos ? -1.0 : std::atof(line.c_str() + at + std::strlen(after));
}

/// Seconds as the minute line writes them: one place.
std::string seconds(double s) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", s);
    return buf;
}

/// The minute line's counts, from the farm of 2026-11-03: the 4-s re-picks of
/// a fight in reach without a style said apart from what was given up on
/// (B5) -- 165 "given up" there, every one in reach -- and, for the drop-down
/// to come, the monsters off the character's level (H6): the picks whose
/// nearest monster stood below or above the band, and the time spent waiting
/// with one below in reach across.
void test_hunter_minute_counts() {
    section("Hunter: the minute line -- re-picks in reach apart from give-ups, and monsters off the level");

    HunterConfig owner;             // the owner's: Đứng yên, reach 450, no style
    owner.stay_put = true;
    owner.reach = 450;
    // Hurt a little at a time, never killed in the minute.
    const auto slow = [](HuntRig& r, const char* name) {
        return [&r, name] {
            if (Nameplate* m = r.w.mob(name)) m->hp = std::max(0.2f, m->hp - 0.0003f);
        };
    };
    {
        Hunter::Status st;
        const std::vector<std::string> log = minute_lines(info_lines([&] {
            HuntRig r(owner);
            r.w.mobs = { plate(640 + 300, 400, "Moc") };
            r.run(61'000, slow(r, "Moc"));
            st = r.hunter.status();
        }));
        const std::string line = log.empty() ? std::string() : log.front();
        unsigned long long n = 0, m = 0;
        unsigned every = 0;
        const std::size_t at = line.find(" back-steps, ");
        const bool read =
            at != std::string::npos &&
            std::sscanf(line.c_str() + at + 13, "%llu given up (%llu of them in reach: re-picked every %u s", &n, &m,
                        &every) == 3;
        check(log.size() == 1 && read && n >= 12 && m == n && every == 4 &&
                  line.find(" of them in reach: re-picked every 4 s, Như trước), ") != std::string::npos,
              "Đứng yên, no style, one monster fought in reach for a minute: \"" + std::to_string(n) + " given up (" +
                  std::to_string(m) + " of them in reach: re-picked every 4 s, Như trước)\"");
        check(st.targets_dropped >= 12 && st.targets_repicked == st.targets_dropped,
              "and the counts: every one given up re-picked -- the card's \"out of reach\" says " +
                  std::to_string(st.targets_dropped - st.targets_repicked));
        check(line.find("nearest off level") == std::string::npos &&
                  line.find("waiting while a monster below") == std::string::npos,
              "nothing off the level: neither count said");
    }
    {
        // Behind a wall, walked at: given up on for staying out of reach,
        // none of it a re-pick.
        Hunter::Status st;
        const std::vector<std::string> log = minute_lines(info_lines([&] {
            HuntRig r(stepping());
            r.w.mobs = { plate(900, 400, "Wall") };
            r.run(61'000);
            st = r.hunter.status();
        }));
        check(st.targets_dropped >= 4 && st.targets_repicked == 0 && log.size() == 1 &&
                  lines_with(log, " given up, ") == 1 && lines_with(log, " given up (") == 0,
              "one behind a wall: " + std::to_string(st.targets_dropped) +
                  " given up for staying out of reach, none re-picked, the minute line with no \"of them in reach\"");
    }
    {
        // A monster below nearer than the one fought on the level: at every
        // pick, the nearest stood below.
        const std::vector<std::string> log = minute_lines(info_lines([&] {
            HuntRig r(owner);
            r.w.mobs = { plate(640 + 300, 400, "Moc"), plate(640 + 40, 480, "Ha") };
            r.run(61'000, slow(r, "Moc"));
        }));
        const std::string line = log.empty() ? std::string() : log.front();
        int below = -1, above = -1, picks = -1;
        const std::size_t at = line.find("; nearest off level: below ");
        const bool read = at != std::string::npos &&
                          std::sscanf(line.c_str() + at + 27, "%d, above %d of %d picks", &below, &above, &picks) == 3;
        check(log.size() == 1 && read && picks >= 12 && below == picks && above == 0 &&
                  line.find("waiting while a monster below") == std::string::npos,
              "one fought on the level 300 px off, one 80 px below 40 px off: the nearest below at every pick -- "
              "below " + std::to_string(below) + ", above " + std::to_string(above) + " of " + std::to_string(picks));
    }
    {
        const std::vector<std::string> log = minute_lines(info_lines([&] {
            HuntRig r(owner);
            r.w.mobs = { plate(640 + 300, 400, "Moc"), plate(640 - 40, 320, "Tren") };
            r.run(61'000, slow(r, "Moc"));
        }));
        const std::string line = log.empty() ? std::string() : log.front();
        int below = -1, above = -1, picks = -1;
        const std::size_t at = line.find("; nearest off level: below ");
        const bool read = at != std::string::npos &&
                          std::sscanf(line.c_str() + at + 27, "%d, above %d of %d picks", &below, &above, &picks) == 3;
        check(log.size() == 1 && read && picks >= 12 && below == 0 && above == picks,
              "and one 80 px above instead: above " + std::to_string(above) + " of " + std::to_string(picks));
    }
    {
        // Standing still with only a monster below, within reach across:
        // nothing to attack, and the time said.
        const std::vector<std::string> log = minute_lines(info_lines([&] {
            HuntRig r(owner);
            r.w.mobs = { plate(640 + 100, 520, "Ha") };
            r.run(61'000);
        }));
        const std::string line = log.empty() ? std::string() : log.front();
        const double waited = number_after(line, "; waiting while a monster below was in reach: ");
        check(log.size() == 1 && line.find("Hunter: 0 fights, ") != std::string::npos && waited >= 59.0 &&
                  waited <= 60.0 && line.find("nearest off level") == std::string::npos,
              "Đứng yên, one monster 120 px below and 100 px across: \"waiting while a monster below was in reach: " +
                  seconds(waited) + " s\", and no pick counted at each pass it waits");
    }
    {
        // The one on the level out of reach, the one below in reach across:
        // picked afresh at every pass, and not a pick taken up -- waiting.
        const std::vector<std::string> log = minute_lines(info_lines([&] {
            HuntRig r(owner);
            r.w.mobs = { plate(640 + 500, 400, "Xa"), plate(640 + 100, 520, "Ha") };
            r.run(61'000);
        }));
        const std::string line = log.empty() ? std::string() : log.front();
        const double waited = number_after(line, "; waiting while a monster below was in reach: ");
        check(log.size() == 1 && waited >= 59.0 && line.find("nearest off level") == std::string::npos,
              "one on the level 500 px off, one below 100 px across: waiting " + seconds(waited) +
                  " s, not a thousand picks");
    }
    {
        // Below but out of reach across, or on "Không đánh": nothing to say.
        HunterConfig skipping = owner;
        skipping.skip_names = { "Ha" };
        const std::vector<std::string> across = minute_lines(info_lines([&] {
            HuntRig r(owner);
            r.w.mobs = { plate(640 + 500, 520, "Ha") };
            r.run(61'000);
        }));
        const std::vector<std::string> skipped = minute_lines(info_lines([&] {
            HuntRig r(skipping);
            r.w.mobs = { plate(640 + 100, 520, "Ha") };
            r.run(61'000);
        }));
        check(across.empty() && skipped.empty(),
              "one below 500 px across, or one below on Không đánh: no minute line at all");
    }
    {
        // Walking, a monster below given up on for staying out of reach:
        // the time it is passed over, nothing else to attack, is waiting.
        const std::vector<std::string> log = minute_lines(info_lines([&] {
            HuntRig r(stepping());
            r.w.mobs = { plate(640 + 50, 520, "Ha") };
            r.run(61'000);
        }));
        const std::string line = log.empty() ? std::string() : log.front();
        const double waited = number_after(line, "; waiting while a monster below was in reach: ");
        check(log.size() == 1 && waited >= 25.0 && waited <= 50.0,
              "walking, one 120 px below given up on and passed over: waiting " + seconds(waited) +
                  " s of the minute");
    }

    // The two rules on their own.
    {
        HunterConfig cfg;               // height 60 both ways, reach 150
        const Nameplate me = plate(640, 400, "Me");
        const std::vector<Hunter::Listed> none;
        Nameplate dead = plate(650, 500, "Chet", 0.0f);
        check(Hunter::nearest_off_level(cfg, { plate(650, 470) }, me, none) == 1 &&
                  Hunter::nearest_off_level(cfg, { plate(650, 330) }, me, none) == -1 &&
                  Hunter::nearest_off_level(cfg, { plate(650, 460), plate(700, 470) }, me, none) == 0 &&
                  Hunter::nearest_off_level(cfg, { plate(650, 340) }, me, none) == 0 &&
                  Hunter::nearest_off_level(cfg, {}, me, none) == 0 &&
                  Hunter::nearest_off_level(cfg, { dead, plate(700, 400) }, me, none) == 0,
              "nearest_off_level: 70 px below +1, 70 above -1, 60 either way level, none 0, a dead one passed by");
        check(Hunter::nearest_off_level(cfg, { plate(650, 470), plate(700, 400) }, me,
                                        { Hunter::Listed::GaveUp, Hunter::Listed::Free }) == 0 &&
                  Hunter::nearest_off_level(cfg, { plate(650, 470), plate(700, 400) }, me,
                                            { Hunter::Listed::Skip, Hunter::Listed::Free }) == 0,
              "and one refused -- given up on or on Không đánh -- is not the nearest");
        check(Hunter::below_in_reach(cfg, { plate(790, 470) }, me, none) &&
                  !Hunter::below_in_reach(cfg, { plate(791, 470) }, me, none) &&
                  !Hunter::below_in_reach(cfg, { plate(700, 460) }, me, none) &&
                  !Hunter::below_in_reach(cfg, { plate(700, 300) }, me, none) &&
                  !Hunter::below_in_reach(cfg, { dead }, me, none),
              "below_in_reach: 70 px below 150 across, yes; 151 across, 60 below, or above, no; a dead one no");
        check(Hunter::below_in_reach(cfg, { plate(700, 470) }, me, { Hunter::Listed::GaveUp }) &&
                  !Hunter::below_in_reach(cfg, { plate(700, 470) }, me, { Hunter::Listed::Skip }) &&
                  !Hunter::below_in_reach(cfg, { plate(700, 470) }, me, { Hunter::Listed::OutOfArea }),
              "and one given up on a moment ago counts; one on Không đánh or outside the area does not");
    }

#ifdef GOBOT_SOURCE_DIR
    // The card and the command line show the two apart.
    {
        const auto read = [](const char* rel) {
            std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        const std::string gui = read("/apps/gobot_gui/gui_app.cpp");
        const std::string cli = read("/apps/gobot_cli/main.cpp");
        check(gui.find("hs.targets_dropped - repicked") != std::string::npos &&
                  gui.find("\"%llu re-picked while in reach (every %u s, Như trước)\"") != std::string::npos &&
                  gui.find("given up on (out of reach)\"") == std::string::npos,
              "the card: given up on less the re-picks, and the re-picks on a line of their own");
        check(cli.find("hs.targets_dropped - repicked") != std::string::npos &&
                  cli.find("%llu fights re-picked while in reach (%s)") != std::string::npos &&
                  cli.find("%llu dropped, %llu re-picked") != std::string::npos,
              "the command line too, at its running line and at its end");
    }
#endif
}

/// The player's own frame, as it was saved: the monster on it is "NgạQuỷ",
/// and the glyph sets have no ỷ.
void test_monster_on_frame() {
    section("Monster lists: a name read off the player's own frame");

    Frame shot;
    if (!load_live_frame(exe_relative("frame.png"), shot) || !shot.valid()) {
        skip("no frame.png here, or not the one these checks were written against\n");
        return;
    }
    GlyphFont font;
    const std::string dir = GlyphFont::default_dir();
    if (dir.empty() || !font.load(dir)) {
        skip("no glyph set installed -- nothing to read the names with\n");
        return;
    }
    const std::vector<Nameplate> plates =
        MobDetector::detect(shot, DetectorTuning::defaults(), &font);
    const Nameplate* found = nullptr;
    for (const Nameplate& n : plates)
        if (!n.is_player && std::abs(n.x() - 325) <= 25 && std::abs(n.y() - 254) <= 12) found = &n;
    // What was read is not printed: the frame is the player's.
    check(found != nullptr, "frame.png: the monster plate near 325,246 is found");
    check(found && monster_on_list({ "NgaQuỷ" }, found->name),
          "and what is read of its name is on a list holding \"NgaQuỷ\", typed with its accent");
}

// ------------------------------------------------- the packets' plates

/// `n` as PacketEyes makes a monster's plate: with its slot on the map and
/// the slot's generation, which a respawn changes.
Nameplate slotted(Nameplate n, int slot, std::uint32_t gen = 1) {
    n.slot = slot;
    n.gen = gen;
    return n;
}

/// The packets' plates say which monster each is (Nameplate::slot, gen): the
/// target is held by them -- never swapped for another walking past where it
/// was, nor taken for the next monster spawned where it died -- and who
/// struck a monster counts for the wounded and for the no-damage watch.
void test_hunter_slots() {
    section("Hunter: the packets' plates -- the target held by its slot, and who hurt it");

    const HunterConfig cfg;     // no style: reach 150, height 60
    {
        // Slot 3 the target, 120 px to the right; slot 4 behind it. Then 3
        // walks on to 800 and 4 comes to 770: nearer where 3 was.
        HuntRig r(cfg);
        r.w.mobs = { slotted(plate(760, 400, "A"), 3), slotted(plate(840, 400, "B"), 4) };
        r.pass();
        const std::string first = r.target();
        r.w.mobs = { slotted(plate(800, 400, "A"), 3), slotted(plate(770, 400, "B"), 4) };
        r.pass();
        const Hunter::Status st = r.hunter.status();
        check(first == "A" && r.target() == "A" && st.target.slot == 3 && st.target.x() == 800,
              "two packet plates crossing: the target stays slot 3, though slot 4 now stands nearer where it was -- " +
                  first + ", then " + r.target());
    }
    {
        HuntRig r(cfg);
        r.w.mobs = { plate(760, 400, "A"), plate(840, 400, "B") };
        r.pass();
        const std::string first = r.target();
        r.w.mobs = { plate(800, 400, "A"), plate(770, 400, "B") };
        r.pass();
        check(first == "A" && r.target() == "B" && r.hunter.status().target.slot == -1,
              "the screen's plates (slot -1): the plate nearest where the target was, as always -- " + first +
                  ", then " + r.target());
    }
    {
        // The target's slot 10 px on, the same monster or the next one
        // spawned there: only the next one is a kill, with its pick-ups.
        HunterConfig looting = cfg;
        looting.loot = true;
        looting.loot_key = 'A';
        looting.loot_interval_ms = 600'000;     // only what a kill owes
        for (const bool respawn : { false, true }) {
            HuntRig r(looting);
            r.w.mobs = { slotted(plate(700, 400, "Moc"), 3, 1) };
            r.pass();
            const std::size_t looted = r.w.of('A').size();
            r.w.mobs = { slotted(plate(710, 400, "Moc"), 3, respawn ? 2u : 1u) };
            r.run(1500);
            const std::size_t after = r.w.of('A').size() - looted;
            check(r.target() == "Moc" && r.hunter.status().target.gen == (respawn ? 2u : 1u) &&
                      after == (respawn ? 3u : 0u),
                  respawn ? "its slot with a new generation, within 360 px: not the same monster -- the target gone, "
                            "three pick-up presses, and the new one fought -- " + std::to_string(after)
                          : "the same slot and generation 10 px on: the same monster, no pick-up owed -- " +
                                std::to_string(after));
        }
    }
    {
        // stay_put: slot 3 level and 160 px off, first in the list and
        // picked; slot 5 right below the character, in reach. Standing
        // still, the one in reach is taken -- by its slot too, or the next
        // look would go back to slot 3 and never attack.
        HunterConfig still = cfg;
        still.stay_put = true;
        HuntRig r(still);
        r.w.mobs = { slotted(plate(800, 400, "Xa"), 3), slotted(plate(640, 455, "Gan"), 5) };
        r.run(600);
        const Hunter::Status st = r.hunter.status();
        check(!r.w.of('Z').empty() && st.state == Hunter::State::Attacking && st.target.slot == 5,
              "stay_put: the monster in reach is attacked, and the hold does not go back to the slot out of reach -- " +
                  std::to_string(r.w.of('Z').size()) + " swing(s)");
    }
    {
        // The most wounded within reach goes first -- on a packet plate only
        // once your id is proven the hitter's, and never one another player
        // is hitting.
        const Nameplate me = plate(640, 400);
        std::vector<Nameplate> mobs = { slotted(plate(700, 400, "A"), 1), slotted(plate(760, 400, "B", 0.5f), 2) };
        const std::vector<Hunter::Listed> none(mobs.size(), Hunter::Listed::Free);
        bool picked = false;
        check(Hunter::choose_target(cfg, mobs, &me, none, &picked) == 0 && !picked,
              "before your id is proven the hitter's, a packet plate at 50% is no wounded one to finish: the nearest");
        for (Nameplate& n : mobs) n.actor_checked = true;
        check(Hunter::choose_target(cfg, mobs, &me, none, &picked) == 1 && picked,
              "after it, the one at 50% in reach is finished first, and said to be the wounded one picked");
        mobs[1].hurt_by_other = true;
        check(Hunter::choose_target(cfg, mobs, &me, none) == 0, "but not while another player is hitting it");
        std::vector<Nameplate> two = { slotted(plate(700, 400, "Theirs", 0.4f), 1),
                                       slotted(plate(760, 400, "Mine", 0.8f), 2) };
        for (Nameplate& n : two) n.actor_checked = true;
        two[0].hurt_by_other = true;
        two[1].hurt_ms = 4'990'000;
        check(Hunter::choose_target(cfg, two, &me, none) == 1,
              "a monster you hurt is finished before one another player hurt, nearer and more wounded as that is");
        const std::vector<Nameplate> screen = { plate(700, 400, "A"), plate(760, 400, "B", 0.5f) };
        check(Hunter::choose_target(cfg, screen, &me, none) == 1,
              "the screen's plates (slot -1): the wounded one in reach first, as before");
    }
    {
        // With a style, a monster whose bar never moves is given up on --
        // unless, on a packet plate, the packets say your blows land on it.
        HunterConfig styled = cfg;
        styled.style = "near_wide";
        styled.no_damage_ms = 3000;
        const char* const kWhat[] = { "a packet plate, no blow of yours said", "a packet plate, a blow of yours at "
                                      "every look", "a screen plate, the same said" };
        std::uint64_t dropped[3] = {};
        for (int c = 0; c < 3; ++c) {
            HuntRig r(styled);
            Nameplate n = plate(700, 400, "Moc", 0.9f);
            if (c < 2) n = slotted(n, 3);
            r.w.mobs = { n };
            r.run(5000, [&r, c] {
                if (c != 0) r.w.mobs[0].hurt_ms = r.w.t;
            });
            dropped[c] = r.hunter.status().targets_dropped;
        }
        check(dropped[0] >= 1 && dropped[1] == 0 && dropped[2] >= 1,
              std::string("no_damage: given up on with ") + kWhat[0] + " (" + std::to_string(dropped[0]) +
                  "); not with " + kWhat[1] + " (" + std::to_string(dropped[1]) + "); and with " + kWhat[2] +
                  ", whose bar alone counts (" + std::to_string(dropped[2]) + ")");
    }
}

} // namespace

/// hold_walk, as the hunter walks by default: the direction key put down at
/// the first look that walks, and left down from look to look until the walk
/// is over -- 933 walks in 4.5 minutes, each stopped within a second, was the
/// character's gait while each look pressed a step of its own.
void test_hunter_held_walk() {
    section("Hunter: a walk held from one look to the next (hold_walk)");

    const HunterConfig cfg;     // hold_walk on: the default
    check(cfg.hold_walk, "hold_walk is on by default");
    {
        // The character walks 20 px a look: about the game's 355 px a second.
        HuntRig r(cfg);
        r.w.step_px = 20;
        r.w.mobs = { plate(1500, 400, "TrungXanh") };
        r.pass();
        check(r.w.presses.size() == 1 && r.w.presses[0].vk == VK_RIGHT && r.w.presses[0].hold == World::kHeld &&
                  r.w.held.count(VK_RIGHT) == 1 && r.w.ups.size() == 1 && r.w.ups[0] == VK_LEFT,
              "a monster 860 px to the right: the left arrow let go of, the right one put down and left down");
        r.run(600);
        const Hunter::Status st = r.hunter.status();
        check(r.w.presses.size() == 1 && r.w.ups.size() == 1 && r.w.held.count(VK_RIGHT) == 1 &&
                  st.state == Hunter::State::Approaching && st.steps == 11,
              "ten looks on, coming closer and still out of reach: no key pressed and none let go of -- the walk "
              "goes on, a step counted a look");
        r.w.mobs = { plate(r.w.me_x + 60, 400, "TrungXanh") };
        r.pass();
        check(r.w.held.empty() && r.w.ups.size() == 2 && r.w.ups[1] == VK_RIGHT && r.w.presses.size() == 2 &&
                  r.w.presses[1].vk == 'Z',
              "within reach: the walk let go of, then the attack -- no tap to face it, the walk right having "
              "turned the character to it");
    }
    {
        // The character walks, a step_px a look, and stops where it can hit.
        HuntRig r(cfg);
        r.w.step_px = 20;
        r.w.mobs = { plate(1040, 400, "TrungXanh") };
        bool reached = false;
        r.run(3000, [&] {
            if (!reached && r.hunter.status().state == Hunter::State::Attacking) reached = true;
        });
        // One press of the right arrow: the walk, held. Turned right by it,
        // the character needs no tap to face the target once there.
        check(reached && r.w.of(VK_RIGHT).size() == 1 && r.w.of(VK_RIGHT)[0].hold == World::kHeld &&
                  r.w.held.empty() && std::abs(1040 - r.w.me_x) <= 150 && std::abs(1040 - r.w.me_x) >= 130,
              "400 px off: walked to it with the key held, and let go of within reach -- at " +
                  std::to_string(1040 - r.w.me_x) + " px -- with no tap to face it after");
    }
    {
        // It turns: the held key let go of at once, the other put down.
        HuntRig r(cfg);
        r.w.mobs = { plate(900, 400, "TrungXanh") };
        r.pass();
        r.w.mobs = { plate(300, 400, "TrungXanh") };    // gone right, one to the left
        r.pass();
        check(r.w.held.count(VK_LEFT) == 1 && r.w.held.count(VK_RIGHT) == 0 && r.w.ups_of(VK_RIGHT) >= 1 &&
                  r.w.presses.back().vk == VK_LEFT && r.w.presses.back().hold == World::kHeld,
              "the target the other way: the right arrow let go of, the left one put down");
    }
    {
        // With the packets to steer by, a walk held kWalkRenewMs is let go of
        // a moment and put down again: the game reports a stop and a start,
        // and the packets have a fresh place. Without them, never.
        HunterConfig steered = cfg;
        steered.packet_positions = PacketPositions::On;
        HuntRig r(steered);
        HuntRig plain(cfg);     // packet_positions watch, as hunt.ini ships
        for (HuntRig* rig : { &r, &plain }) {
            rig->w.step_px = 20;
            rig->w.mobs = { plate(3500, 400, "Xa") };   // far enough to walk 3 s toward
            rig->pass();
        }
        const std::uint64_t from = r.w.t - 60;
        while (r.w.t < from + Hunter::kWalkRenewMs) {
            r.pass();
            plain.pass();
        }
        const bool kept = r.w.of(VK_RIGHT).size() == 1 && r.w.ups_of(VK_RIGHT) == 0;
        r.pass();
        plain.run(1000);
        const auto downs = r.w.of(VK_RIGHT);
        const bool gap = !r.w.up_at.empty() && downs.size() == 2 &&
                         downs[1].at >= r.w.up_at.back() + Hunter::kWalkRenewGapMs;
        check(kept && downs.size() == 2 && downs[1].hold == World::kHeld && r.w.ups_of(VK_RIGHT) == 1 &&
                  r.w.held.count(VK_RIGHT) == 1 && downs[1].at >= from + Hunter::kWalkRenewMs && gap,
              "with the packets on: held 2.5 s, let go of once, put down again 40 ms later, and held on from there");
        check(plain.w.of(VK_RIGHT).size() == 1 && plain.w.ups_of(VK_RIGHT) == 0 && plain.w.held.count(VK_RIGHT) == 1,
              "with the packets only watched: held 3.5 s and never let go of");
    }
    {
        // Getting no closer -- a walk start the packets never heard leaves them
        // with you standing: let go of and put down again within 0.4 s, not at
        // 2.5 s.
        HuntRig r(cfg);
        r.w.mobs = { plate(1200, 400, "Xa") };      // and the character does not move
        r.pass();
        const std::uint64_t from = r.w.t - 60;
        while (r.w.t < from + Hunter::kWalkStuckMs) r.pass();
        const bool kept = r.w.of(VK_RIGHT).size() == 1;
        r.pass();
        const auto downs = r.w.of(VK_RIGHT);
        check(kept && downs.size() == 2 && r.w.ups_of(VK_RIGHT) == 1 && downs[1].at >= from + Hunter::kWalkStuckMs &&
                  downs[1].at < from + Hunter::kWalkStuckMs + 120,
              "a held walk the target comes no closer in for 0.4 s: let go of and put down again then");
    }
    {
        // Let go of by something else -- the macro letting go of every key as
        // the character left the world: put down again at the next look.
        HuntRig r(cfg);
        r.w.mobs = { plate(1200, 400, "Xa") };
        r.pass();
        r.w.held.clear();
        r.pass();
        check(r.w.of(VK_RIGHT).size() == 2 && r.w.held.count(VK_RIGHT) == 1,
              "the walk's key let go of by another thread: put down again at the next look");
    }
    {
        // A hold: the walk let go of at the next look, nothing while it is on,
        // the walk taken up again after.
        HuntRig r(cfg);
        r.w.mobs = { plate(1200, 400, "Xa") };
        r.pass();
        r.coord.engage_critical("trash cleaning");
        r.pass();
        const bool let_go = r.w.held.empty() && r.w.ups_of(VK_RIGHT) == 1;
        const std::size_t before = r.w.presses.size();
        r.run(600);
        const bool still = r.w.presses.size() == before && r.w.held.empty();
        r.coord.disengage_critical();
        r.pass();
        check(let_go && still && r.w.held.count(VK_RIGHT) == 1,
              "a hold: the walk let go of at the next look, no key while it is on, walking again after it");
    }
    {
        // A kill owes pick-ups: the walk is let go of first, or the character
        // would walk off what the monster dropped.
        HunterConfig looting = cfg;
        looting.loot = true;
        looting.loot_key = 'A';
        HuntRig r(looting);
        r.w.mobs = { plate(900, 400, "TrungXanh") };
        r.pass();
        r.w.mobs = { plate(1100, 400, "Da") };      // it is gone; another further on
        r.pass();
        const bool stood = r.w.held.empty() && r.w.ups_of(VK_RIGHT) == 1 && r.w.of(VK_RIGHT).size() == 1;
        r.run(600);
        const auto loots = r.w.of('A');
        const auto walks = r.w.of(VK_RIGHT);
        check(stood && loots.size() == 3 && walks.size() == 2 && walks[1].at > loots[2].at,
              "a kill while walking: the walk let go of at once, the three pick-ups pressed standing, and "
              "only then the walk to the next one -- " + std::to_string(loots.size()) + " pick-ups, " +
                  std::to_string(walks.size()) + " walks");
    }
    {
        // A pick-up key the input will not take: stood for 1.5 s at most.
        HunterConfig looting = cfg;
        looting.loot = true;
        looting.loot_key = 'A';
        HuntRig r(looting);
        r.w.mobs = { plate(900, 400, "TrungXanh") };
        r.pass();
        r.w.mobs = { plate(1100, 400, "Da") };
        r.pass();
        r.coord.request("test", Priority::Heal);    // the keys busy with something more urgent
        r.run(1600);
        const bool none = r.w.of('A').empty() && r.w.held.empty();
        r.coord.release("test");
        r.pass();
        check(none && r.w.held.count(VK_RIGHT) == 1,
              "pick-ups the input will not take: stood for 1.5 s, then the walk taken up again");
    }
    {
        // The game losing focus lets go of everything; back in front, the
        // walk is put down again.
        HuntRig r(cfg);
        r.w.mobs = { plate(1200, 400, "Xa") };
        r.pass();
        r.w.focused = false;
        r.pass();
        const bool released = r.w.releases == 1 && r.w.held.empty();
        r.w.focused = true;
        r.pass();
        check(released && r.w.of(VK_RIGHT).size() == 2 && r.w.held.count(VK_RIGHT) == 1,
              "focus lost: every key let go of; back in front, the walk put down again");
    }
    {
        // Out of sight, or nothing to go for: the walk let go of.
        HuntRig r(cfg);
        r.w.mobs = { plate(1200, 400, "Xa") };
        r.pass();
        r.w.mobs.clear();
        r.pass();
        check(r.w.held.empty() && r.w.ups_of(VK_RIGHT) == 1, "the monsters gone: the walk let go of");
    }
    {
        // A hop toward one above: never again while the last is in the air
        // (kJumpAirMs), walking held or stepping -- it came every 300 ms held
        // and every look stepping, a second jump in the air each time.
        HuntRig held(cfg);
        HuntRig steps(stepping());
        for (HuntRig* r : { &held, &steps }) {
            r->w.mobs = { plate(1200, 300, "Cao") };
            r->run(1700);
        }
        bool apart = true;
        for (const HuntRig* r : { &held, &steps }) {
            const auto hops = r->w.of(VK_SPACE);
            for (std::size_t i = 1; i < hops.size(); ++i)
                if (hops[i].at - hops[i - 1].at < Hunter::kJumpAirMs) apart = false;
        }
        check(held.w.of(VK_SPACE).size() == 3 && steps.w.of(VK_SPACE).size() == 3 && apart &&
                  Hunter::kJumpAirMs >= 667,
              "a hop toward one 100 px up: 3 in 1.7 s, walking held or stepping, each a whole jump's time after "
              "the last -- " + std::to_string(held.w.of(VK_SPACE).size()) + " and " +
                  std::to_string(steps.w.of(VK_SPACE).size()));
        // A hop that did not go out -- the game behind another window a
        // moment -- is no jump in the air: tried again at the next look, not
        // a whole jump's time on.
        HuntRig refused(cfg);
        refused.w.refuse = 1;
        refused.w.refuse_vk = VK_SPACE;
        refused.w.mobs = { plate(1200, 300, "Cao") };
        refused.run(200);
        const auto tries = refused.w.of(VK_SPACE);
        check(tries.size() == 2 && tries[1].at - tries[0].at == 60,
              "a hop the game did not take: tried again at the next look -- " + std::to_string(tries.size()) +
                  " tries in 200 ms");
        // The jump key set to Down -- which on a gate changes the map: never.
        HunterConfig down = cfg;
        down.jump_key = VK_DOWN;
        HuntRig r(down);
        r.w.mobs = { plate(1200, 300, "Cao") };
        r.run(1700);
        check(r.w.of(VK_DOWN).empty() && !r.w.of(VK_RIGHT).empty(),
              "the jump key set to Down: it walks to the monster above, and never presses Down");
    }
    {
        // Saved and read back; and a file from before it walks held.
        const std::string path = temp_path("gobot_hunter_hold_walk.ini");
        HunterConfig off;
        off.hold_walk = false;
        HunterConfig read;
        const bool saved = off.save(path) && read.load(path) && !read.hold_walk;
        write_file(path, "[Hunt]\nstep_ms=180\n");
        HunterConfig old;
        check(saved && old.load(path) && old.hold_walk,
              "hold_walk saved and read back; a hunt.ini without it walks held");
        DeleteFileW(wide(path).c_str());
    }
}

void test_hunter_fight(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_hunter_seam();
    test_hunter_choosing();
    test_hunter_held_walk();
    test_monster_names();
    test_monster_lists_hunting();
    test_hunter_view_change();
    test_hunter_spot();
    test_hunter_turns();
    test_hunter_minute_counts();
    test_monster_on_frame();
    test_hunter_slots();
}
