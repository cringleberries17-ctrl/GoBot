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
#include "gobot/mob_detector.hpp"
#include "gobot/monster_list.hpp"
#include "gobot/screen_capture.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
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
    /// character, in pixels. 0: it stays where it is.
    int step_px = 0;

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
    int                        releases = 0;    ///< release_all() calls

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

    explicit HuntRig(const HunterConfig& cfg) {
        Hunter::Eyes eyes;
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
            if (w.step_px > 0 && hold >= 100) {
                if (vk == VK_LEFT)  w.me_x -= w.step_px;
                if (vk == VK_RIGHT) w.me_x += w.step_px;
            }
            return true;
        };
        hands.key_up = [this](std::uint32_t vk) {
            w.ups.push_back(vk);
            w.held.erase(vk);
        };
        hands.held = [this](std::uint32_t vk) { return w.held.count(vk) != 0; };
        hands.release_all = [this] {
            ++w.releases;
            w.held.clear();
        };
        hunter.set_hands(hands);

        Hunter::Clock clock;
        clock.now_ms = [this] { return w.t; };
        hunter.set_clock(clock);

        hunter.set_config(cfg);
        hunter.start_stepped();
    }
    ~HuntRig() { hunter.stop(); }

    /// One pass, then 60 ms on: the thread's own rhythm.
    void pass() {
        hunter.step();
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

void test_hunter_seam() {
    section("Hunter: one pass at a time, on scripted plates (today's rules)");

    const HunterConfig cfg;     // reach 150, height 60, a 180-ms step, a swing every 700 ms
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

        // Now within reach.
        r.w.presses.clear();
        r.w.mobs = { plate(700, 400, "TrungXanh") };
        const std::uint64_t t0 = r.w.t;
        r.pass();
        check(r.w.presses.size() == 2 && r.w.presses[0].vk == VK_RIGHT &&
                  r.w.presses[0].hold == 30 && r.w.presses[1].vk == 'Z' &&
                  r.w.presses[1].hold == 60,
              "60 px away: a 30-ms tap to face it, then the attack key");
        check(r.hunter.status().state == Hunter::State::Attacking, "and it says it is attacking");

        r.run(2040);    // passes up to 2040 ms after the first swing
        const auto swings = r.w.of('Z');
        bool spaced = swings.size() == 3;
        for (std::size_t i = 1; i < swings.size(); ++i)
            if (swings[i].at - swings[i - 1].at < 700) spaced = false;
        check(spaced && swings[0].at == t0,
              "the attack key once per attack_interval_ms: 3 swings in 2.04 s, 700+ ms apart -- " +
                  std::to_string(swings.size()));
        // As before: a tap toward it on every pass while it is more than 12 px
        // off, which is what creeps the character in.
        check(r.w.of(VK_RIGHT).size() == 35 && r.hunter.status().attacks == 3,
              "and, as before, a face tap on every one of the 35 passes");

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

/// The player's own frame: the monster on it is "NgạQuỷ", and the glyph sets
/// have no ỷ.
void test_monster_on_frame() {
    section("Monster lists: a name read off the player's own frame");

    Frame shot;
    if (!load_png(exe_relative("frame.png"), shot) || !shot.valid()) {
        std::printf("  [skip] no frame.png here\n");
        return;
    }
    GlyphFont font;
    const std::string dir = GlyphFont::default_dir();
    if (dir.empty() || !font.load(dir)) {
        std::printf("  [skip] no glyph set installed -- nothing to read the names with\n");
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

} // namespace

void test_hunter_fight(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_hunter_seam();
    test_monster_names();
    test_monster_lists_hunting();
    test_monster_on_frame();
}
