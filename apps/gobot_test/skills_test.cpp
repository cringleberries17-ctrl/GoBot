// gobot_test -- skills that follow the fight ("Chỉ khi đang đánh quái"):
// GoC's two ways of throwing them, each skill's own cooldown and range, the
// MP floor, nothing at a death panel, potions first, the sequence held with
// them -- and every key on its timer, as before, with the tick off.
//
// Nothing here goes near the game. The engine is driven a step at a time on a
// clock of the test's own, with a pretend keyboard and a pretend hunter that
// says whether it is fighting -- except for the sequence, whose thread runs on
// the real clock, for a second or two, with the same pretend keyboard.

#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/hunter.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/macro.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

std::string temp_path(const char* name) {
    char dir[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return name;
    return std::string(dir) + "gobot_test_" + std::to_string(GetCurrentProcessId()) + "\\" + name;
}

MacroKey skill(std::uint32_t vk, std::uint32_t interval_ms, int range_px = 0, bool own_timer = false) {
    MacroKey k;
    k.keycode = vk;
    k.label = "Skill";
    k.enabled = true;
    k.interval_ms = interval_ms;
    k.jitter_pct = 0;
    k.range_px = range_px;
    k.always_timer = own_timer;
    return k;
}

MacroKey buff(std::uint32_t vk, std::uint32_t interval_ms) {
    MacroKey k = skill(vk, interval_ms);
    k.label = "Buff";
    return k;
}

MacroKey potion(MacroTrigger trigger, std::uint32_t vk, int pct) {
    MacroKey k = skill(vk, 3000);
    k.label = "Potion";
    k.trigger = trigger;
    k.threshold_pct = pct;
    return k;
}

using Mode = MacroEngine::Fight::Mode;

/// The engine on a clock of the test's own, with a pretend keyboard, the
/// vitals pushed every 10 ms, and a pretend hunter: what it says is `mode`,
/// `dx` and `attack_key`, which a test changes as it goes.
struct Rig {
    InputManager         input;
    ExecutionCoordinator coord;
    MacroEngine          engine{ input, coord };
    std::uint64_t        t = 11'000'000;
    float                hp = 100.0f, mp = 100.0f;
    float                pet = -1.0f;    ///< the pet's HP: none read unless a test says
    Mode                 mode = Mode::Fighting;
    int                  dx = 80;
    std::uint32_t        attack_key = VK_SPACE;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> pressed;
    /// Called as each key goes out -- what the hunter does meanwhile. A key
    /// with a hold of its own takes that long on the clock; the rest none.
    std::function<void(std::uint32_t)> on_press;

    Rig(std::vector<MacroKey> keys, bool follow = true, bool with_hunter = true,
        bool sequential = true) {
        MacroEngine::Clock c;
        c.now_ms = [this] { return t; };
        c.sleep_ms = [this](std::uint32_t ms) { t += ms; };
        engine.set_clock(c);
        MacroEngine::Hands h;
        h.focused = [] { return true; };
        h.key = [this](std::uint32_t vk, int hold) {
            pressed.emplace_back(vk, t);
            if (hold > 0) t += static_cast<std::uint64_t>(hold);
            if (on_press) on_press(vk);
            return true;
        };
        engine.set_hands(h);
        if (with_hunter)
            engine.set_fight([this] {
                MacroEngine::Fight f;
                f.mode = mode;
                f.dx = dx;
                f.attack_key = attack_key;
                return f;
            });
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys = std::move(keys);
        cfg.sequence_enabled = false;
        cfg.global_jitter_pct = 0;
        cfg.skills_follow_fight = follow;
        cfg.sequential_skills = sequential;
        engine.set_config(cfg);
        engine.start_stepped();
    }
    ~Rig() { engine.stop(); }

    void run(std::uint64_t ms) {
        for (const std::uint64_t until = t + ms; t < until; t += 10) {
            engine.set_vitals(hp, mp, pet);
            engine.step();
        }
    }
    std::vector<std::uint64_t> when(std::uint32_t vk, std::uint64_t from = 0) const {
        std::vector<std::uint64_t> out;
        for (const auto& p : pressed)
            if (p.first == vk && p.second >= from) out.push_back(p.second);
        return out;
    }
    std::size_t count(std::uint32_t vk, std::uint64_t from = 0) const { return when(vk, from).size(); }
};

std::string ms_list(const std::vector<std::uint64_t>& at, std::uint64_t from) {
    std::string s;
    for (const std::uint64_t v : at) s += (s.empty() ? "" : ", ") + std::to_string(v - from);
    return s.empty() ? "none" : s;
}

/// Whether the log, while `run` ran at `level` and up, had a line with every
/// one of `words`.
bool logged_line(LogLevel level, const std::function<void()>& run,
                 std::initializer_list<const char*> words) {
    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_ring_capacity(2000);
    log.clear_ring();
    log.set_level(level);
    run();
    bool found = false;
    for (const LogLine& l : log.ring()) {
        bool all = true;
        for (const char* w : words) all = all && l.text.find(w) != std::string::npos;
        found = found || all;
    }
    log.set_level(was);
    log.set_ring_capacity(0);
    return found;
}

/// As logged_line, the first such line itself; "" for none.
std::string logged_text(LogLevel level, const std::function<void()>& run, std::initializer_list<const char*> words) {
    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_ring_capacity(2000);
    log.clear_ring();
    log.set_level(level);
    run();
    std::string found;
    for (const LogLine& l : log.ring()) {
        bool all = true;
        for (const char* w : words) all = all && l.text.find(w) != std::string::npos;
        if (all && found.empty()) found = l.text;
    }
    log.set_level(was);
    log.set_ring_capacity(0);
    return found;
}

/// The number after `after` in `text`; -1 for none.
long long number_after(const std::string& text, const std::string& after) {
    const std::size_t at = text.find(after);
    if (at == std::string::npos) return -1;
    const std::size_t from = at + after.size();
    std::size_t to = from;
    while (to < text.size() && text[to] >= '0' && text[to] <= '9') ++to;
    return to == from ? -1 : std::stoll(text.substr(from, to - from));
}

// ------------------------------------------------------------------ gates

void test_skills_idle() {
    section("Skills follow the fight: nothing while there is no fight, the rest on time");

    // Z and X follow the fight; V keeps its own timer ("Luôn bấm theo giờ");
    // a buff and an HP potion as ever.
    Rig rig({ skill('Z', 1000), skill('X', 3000), skill('V', 2000, 0, true), buff('3', 60'000),
              potion(MacroTrigger::HpBelow, '1', 50) });
    rig.mode = Mode::Idle;
    rig.hp = 40.0f;
    const std::uint64_t t0 = rig.t;
    rig.run(60'000);
    const auto st = rig.engine.stats();
    check(rig.count('Z') == 0 && rig.count('X') == 0 && st.held_not_fighting > 0,
          "60 s with the hunter not fighting: the two skills never pressed, held as not fighting");
    check(rig.count('V') == 30 && rig.count('3') == 1 && rig.count('1') == 20,
          "while V on its own timer pressed every 2 s (" + std::to_string(rig.count('V')) +
              "), the buff once (" + std::to_string(rig.count('3')) + ") and the potion every 3 s (" +
              std::to_string(rig.count('1')) + ")");

    // A fight begins: the two that were waiting go straight away.
    rig.mode = Mode::Fighting;
    const std::uint64_t t1 = rig.t;
    rig.run(200);
    const auto z = rig.when('Z', t1);
    const auto x = rig.when('X', t1);
    check(!z.empty() && !x.empty() && z[0] - t1 <= 110 && x[0] > z[0],
          "a fight begins: the skills held ready go at once, Z then X -- at +" + ms_list(z, t1) +
              " and +" + ms_list(x, t1) + " ms");
    (void)t0;
}

void test_skills_sequential() {
    section("Skills follow the fight: in order, 50 ms apart, each on its own cooldown");

    Rig rig({ skill('Z', 1000), skill('X', 3000), skill('C', 6000) });
    const std::uint64_t t0 = rig.t;
    rig.run(400);
    const bool order = rig.pressed.size() == 3 && rig.pressed[0].first == 'Z' &&
                       rig.pressed[1].first == 'X' && rig.pressed[2].first == 'C';
    const bool gaps = order && rig.pressed[1].second - rig.pressed[0].second >= 50 &&
                      rig.pressed[2].second - rig.pressed[1].second >= 50;
    check(order && gaps && rig.pressed[0].second == t0,
          "all three ready: Z, X, C in that order, at least 50 ms apart -- at +" +
              ms_list({ rig.pressed.empty() ? t0 : rig.pressed[0].second,
                        rig.pressed.size() > 1 ? rig.pressed[1].second : t0,
                        rig.pressed.size() > 2 ? rig.pressed[2].second : t0 },
                      t0) +
              " ms");
    rig.run(12'000);
    const auto z = rig.when('Z');
    const auto x = rig.when('X');
    const auto c = rig.when('C');
    bool cooled = true;
    for (const auto* list : { &z, &x, &c }) {
        const std::uint64_t cd = list == &z ? 1000 : list == &x ? 3000 : 6000;
        for (std::size_t i = 1; i < list->size(); ++i)
            if ((*list)[i] - (*list)[i - 1] < cd) cooled = false;
    }
    check(cooled && z.size() >= 11 && x.size() >= 4 && c.size() >= 2,
          "afterwards none goes before its own cooldown: Z every 1 s, X every 3, C every 6 -- " +
              std::to_string(z.size()) + ", " + std::to_string(x.size()) + ", " +
              std::to_string(c.size()) + " presses in 12.4 s");
}

void test_skills_one_at_a_time() {
    section("Skills follow the fight: sequential off, the first ready one, 300 ms apart");

    Rig rig({ skill('Z', 1000), skill('X', 3000), skill('C', 6000) }, true, true, false);
    rig.run(10'000);
    bool spaced = rig.pressed.size() >= 2;
    for (std::size_t i = 1; i < rig.pressed.size(); ++i)
        if (rig.pressed[i].second - rig.pressed[i - 1].second < MacroEngine::kSkillChanceMs)
            spaced = false;
    // The first three: Z, X, C, each the first one ready at its chance.
    const bool first = rig.pressed.size() >= 3 && rig.pressed[0].first == 'Z' &&
                       rig.pressed[1].first == 'X' && rig.pressed[2].first == 'C';
    check(spaced && first,
          "one press at a time, never two within 300 ms, the first ready key each time -- " +
              std::to_string(rig.pressed.size()) + " presses in 10 s");
}

void test_skills_mp_floor() {
    section("Skills follow the fight: none under the MP floor, the swing carries on");

    {
        Rig rig({ skill('X', 1000) });
        rig.mp = 9.0f;
        rig.run(3000);
        const bool none = rig.count('X') == 0 && rig.engine.stats().held_low_mp > 0;
        rig.mp = 10.0f;
        rig.run(1500);
        check(none && rig.count('X') >= 1,
              "MP 9% with the floor at 10: no skill, counted as held for MP; at 10% they go");
    }
    {
        Rig rig({ skill('X', 1000) });
        MacroConfig cfg = rig.engine.config();
        cfg.skill_mp_floor_pct = 0;
        rig.engine.set_config(cfg);
        rig.mp = 1.0f;
        rig.run(3000);
        check(rig.count('X') == 3, "a floor of 0: never held for MP");
    }
    {
        Rig rig({ skill('X', 1000) });
        rig.mp = -1.0f;         // no reading
        rig.run(3000);
        check(rig.count('X') == 3 && rig.engine.stats().held_low_mp == 0,
              "no MP reading: no MP hold");
    }
}

void test_skills_range_and_dead() {
    section("Skills follow the fight: each within its range; none with the character down");

    {
        Rig rig({ skill('X', 1000, 100) });
        rig.dx = 150;
        rig.run(2000);
        const bool held = rig.count('X') == 0 && rig.engine.stats().held_range > 0;
        rig.dx = 90;
        rig.run(1000);
        check(held && rig.count('X') >= 1,
              "a skill reaching 100 px: held with the target 150 px off, pressed at 90");
    }
    {
        Rig rig({ skill('X', 1000), potion(MacroTrigger::HpBelow, '1', 50) });
        rig.hp = 0.3f;
        rig.run(4000);
        check(rig.count('X') == 0 && rig.engine.stats().held_dead > 0 && rig.count('1') >= 1,
              "HP read at 0.3%: no skill (held as down), while the potion due at 0.3% still drinks");
    }
    {
        Rig rig({ skill('Z', 1000), skill('X', 1000) });
        rig.attack_key = 'Z';
        rig.run(5000);
        rig.mode = Mode::Idle;
        rig.run(2000);
        check(rig.count('Z') == 0 && rig.count('X') >= 5,
              "a skill on the hunter's own attack key is never pressed by the rotation");
    }
}

void test_skills_refused() {
    section("Skills follow the fight: a press that does not go out keeps its turn");

    {
        Rig rig({ skill('X', 1000) });
        rig.coord.request("other", Priority::Combat);
        rig.run(2000);
        const bool none = rig.pressed.empty();
        rig.coord.release("other");
        const std::uint64_t freed = rig.t;
        rig.run(1500);
        const auto x = rig.when('X');
        check(none && x.size() == 2 && x[0] - freed <= 30 && x[1] - x[0] == 1000,
              "the keys held by another job for 2 s: the skill goes within 30 ms of their release "
              "and its cooldown counts from that press -- at +" + ms_list(x, freed) + " ms");
    }
    {
        Rig rig({ skill('X', 1000), potion(MacroTrigger::HpBelow, '1', 50) });
        rig.hp = 40.0f;
        rig.run(10);
        check(rig.pressed.size() == 2 && rig.pressed[0].first == '1' && rig.pressed[1].first == 'X',
              "a potion and a skill due together: the potion first");
    }
}

void test_skills_fight_read_late() {
    section("Skills follow the fight: the fight asked about just before the skill, not before the buff");

    // A buff held 120 ms goes first in the pass; while it is down the hunter
    // stops fighting -- a step back begins. The skill due in the same pass
    // goes by the fight as it is then, not as it was when the pass began.
    MacroKey b = buff('B', 120'000);
    b.hold_ms = 120;
    Rig rig({ b, skill('Z', 1000) });
    std::uint64_t idle_at = 0;
    rig.on_press = [&](std::uint32_t vk) {
        if (vk != 'B' || idle_at != 0) return;
        rig.mode = Mode::Idle;
        idle_at = rig.t;
    };
    rig.run(10);
    const bool held = rig.count('B') == 1 && rig.count('Z') == 0 &&
                      rig.engine.stats().held_not_fighting > 0;
    rig.run(500);
    const bool still = rig.count('Z') == 0;
    rig.mode = Mode::Fighting;
    rig.run(200);
    check(held && still && rig.count('Z') == 1,
          "the hunter stopped fighting while a buff was down: the skill due in that pass waits, and "
          "goes once the fight is back");
}

/// A key on its own timer ("Luôn bấm theo giờ") keeps it whatever the gates
/// hold the others for: no fight, MP under the floor, the character down.
void test_skills_own_timer_gates() {
    section("Skills follow the fight: a key on its own timer ignores the MP floor and the death panel");

    {
        Rig rig({ skill('Z', 1000), skill('V', 2000, 0, true) });
        rig.mp = 5.0f;
        rig.run(10'000);
        check(rig.count('Z') == 0 && rig.engine.stats().held_low_mp > 0 && rig.count('V') == 5,
              "MP 5% under the floor of 10, fighting: Z held for MP, V on its own timer every 2 s -- " +
                  std::to_string(rig.count('V')) + " presses in 10 s");
    }
    {
        Rig rig({ skill('Z', 1000), skill('V', 2000, 0, true) });
        rig.hp = 0.3f;
        rig.run(10'000);
        check(rig.count('Z') == 0 && rig.engine.stats().held_dead > 0 && rig.count('V') == 5,
              "HP read at 0.3%, fighting: Z held as down, V on its own timer still -- " +
                  std::to_string(rig.count('V')) + " presses in 10 s");
    }
}

/// A key taken out while a skill that follows the fight is being pressed:
/// the skill's cooldown is written on it, wherever it stands now -- not on
/// the key that has come up into its old place.
void test_skills_keys_change_mid_press() {
    section("Skills follow the fight: a key taken out while a skill is down");

    Rig rig({ buff('B', 300'000), skill('Z', 1200), skill('X', 3000) });
    rig.run(100);
    MacroConfig gone = rig.engine.config();
    gone.keys.erase(gone.keys.begin());
    bool done = false;
    rig.on_press = [&](std::uint32_t vk) {
        if (vk != 'Z' || done) return;
        done = true;
        rig.engine.set_config(gone);
    };
    rig.run(12'000);
    const auto z = rig.when('Z');
    const auto x = rig.when('X');
    bool spaced = z.size() >= 10 && x.size() >= 4;
    for (std::size_t i = 1; spaced && i < z.size(); ++i) spaced = z[i] - z[i - 1] >= 1200;
    for (std::size_t i = 1; spaced && i < x.size(); ++i) spaced = x[i] - x[i - 1] >= 3000;
    check(done && spaced && rig.count('B') == 1,
          "buff B, in front, taken out while Z is down: Z every 1.2 s and X every 3 s after it, each on "
          "its own cooldown -- " + std::to_string(z.size()) + " and " + std::to_string(x.size()) +
              " presses in 12 s");
}

void test_skills_log() {
    section("Skills follow the fight: what they did, in the log");

    // Half a minute fighting, half a minute not: the minute's line.
    const bool minute = logged_line(LogLevel::Info, [] {
        Rig rig({ skill('Z', 1000), skill('X', 3000) });
        rig.run(30'000);
        rig.mode = Mode::Idle;
        rig.run(31'000);
    }, { "Skills: ", " pressed; held ", " not fighting, ", " MP under 10%, ", " dead, ", " range" });
    check(minute, "once a minute: the skills pressed, and those held -- not fighting, MP, down, range");

    const bool each = logged_line(LogLevel::Debug, [] {
        Rig rig({ skill('Z', 1000) });
        rig.run(500);
    }, { "Skills: Z, the target 80 px to the side" });
    check(each, "and each press, with how far to the side the target was (debug)");

    // 2026.11.08: the skills refused with the game behind another window --
    // GoBot's own, clicked mid-run -- on their timers or following the fight,
    // counted on the minute's line. Nothing else changes: none goes out.
    for (const bool follow : { false, true }) {
        std::uint64_t counted = 0;
        bool none_out = false;
        const std::string line = logged_text(LogLevel::Info, [&] {
            Rig rig({ skill('Z', 1000), skill('X', 3000) }, follow, follow);
            MacroEngine::Hands h;
            h.focused = [] { return false; };
            h.key = [&rig](std::uint32_t vk, int) {
                rig.pressed.emplace_back(vk, rig.t);
                return true;
            };
            rig.engine.set_hands(h);
            rig.attack_key = 0;
            rig.run(61'000);
            counted = rig.engine.stats().skills_unfocused;
            none_out = rig.pressed.empty();
        }, { "Skills: 0 pressed; held 0 not fighting, ", "; refused: game not in front " });
        const long long said = number_after(line, "; refused: game not in front ");
        // The line counts its minute; the count runs on a second past it.
        check(none_out && said > 0 && counted >= static_cast<std::uint64_t>(said) &&
                  static_cast<std::uint64_t>(said) * 10 >= counted * 9,
              std::string(follow ? "following the fight" : "on their timers") +
                  ", the game not in front for a minute: none pressed, the refusals counted on the Skills line -- " +
                  line + " (" + std::to_string(counted) + " in all)");
    }
    const std::string in_front = logged_text(LogLevel::Info, [] {
        Rig rig({ skill('Z', 1000), skill('X', 3000) }, false, false);
        rig.run(61'000);
    }, { "Skills: ", " pressed; held " });
    check(number_after(in_front, "; refused: game not in front ") == 0,
          "and the game in front: refused 0 -- " + in_front);
}

// --------------------------------------------------- the tick off: as ever

void test_skills_off_as_before() {
    section("Skills follow the fight off, or no hunter: every key on its timer, as before");

    const std::vector<MacroKey> keys = { skill('Z', 1200), skill('X', 3000), skill('C', 6000),
                                         buff('3', 60'000), potion(MacroTrigger::HpBelow, '1', 50),
                                         skill('V', 9000, 50) };
    Rig plain(keys, false, false);           // no hunter, nothing to follow
    Rig off(keys, false, true);              // a hunter, the tick off
    Rig no_hunter(keys, true, true);         // the tick on, no hunter running
    no_hunter.mode = Mode::NoHunter;
    off.mode = Mode::Idle;
    for (Rig* r : { &plain, &off, &no_hunter }) {
        r->hp = 45.0f;
        r->run(30'000);
    }
    const auto same = [](const Rig& a, const Rig& b) {
        if (a.pressed.size() != b.pressed.size()) return false;
        for (std::size_t i = 0; i < a.pressed.size(); ++i)
            if (a.pressed[i] != b.pressed[i]) return false;
        return true;
    };
    check(!plain.pressed.empty() && same(plain, off) && same(plain, no_hunter) &&
              plain.count('Z') == 25,
          "the same keys at the same moments with no hunter, with the tick off, and with the "
          "tick on but no hunter running -- Z every 1.2 s, " +
              std::to_string(plain.count('Z')) + " times in 30 s, never Idle-held");
    check(off.engine.stats().held_not_fighting == 0 && no_hunter.engine.stats().held_not_fighting == 0,
          "and nothing counted as held for want of a fight");
}

void test_skills_timed_gap() {
    section("Skills on their own timers: two due together go skill_gap_ms apart (Tốc độ bấm kỹ năng)");

    // Z and X due at the same moment, no fight to follow: X waits the gap
    // after Z's key came up, never pressed with it -- at each step offered.
    bool all = true;
    std::string seen;
    for (const std::uint32_t gap : MacroConfig::kSkillGapSteps) {
        Rig rig({ skill('Z', 1000), skill('X', 1000) }, false, false);
        MacroConfig cfg = rig.engine.config();
        cfg.skill_gap_ms = gap;
        rig.engine.set_config(cfg);
        rig.run(600);
        const auto z = rig.when('Z');
        const auto x = rig.when('X');
        // The rig takes no time for a key with no hold of its own, so Z's
        // key-up is its press: X the gap after it, within a step of 10 ms.
        const bool spaced = !z.empty() && !x.empty() && x[0] >= z[0] + gap && x[0] <= z[0] + gap + 10;
        all = all && spaced;
        seen += (seen.empty() ? "" : ", ") + std::to_string(gap) + ": X +" +
                std::to_string(!z.empty() && !x.empty() ? static_cast<long long>(x[0] - z[0]) : -1);
    }
    check(all, "the second skill the gap after the first's key-up, at 50, 60, 80, 100 and 150 ms -- " + seen);

    // And each still on its own cooldown afterwards.
    Rig rig({ skill('Z', 1000), skill('X', 1000) }, false, false);
    rig.run(10'000);
    check(rig.count('Z') >= 9 && rig.count('X') >= 9, "each still pressed about every second: " +
                                                          std::to_string(rig.count('Z')) + " and " +
                                                          std::to_string(rig.count('X')) + " in 10 s");
}

// ------------------------ walking back to the hunting area (2026.11.10)

void test_skills_returning() {
    section("Skills held while the hunter walks back to its area (Fight::Mode::Returning, 2026.11.10): on their own "
            "timers or following the fight -- potions, the pet and a buff as ever");

    for (const bool follow : { false, true }) {
        // The owner's way: Z and X on their own timers ("Luôn bấm theo giờ"),
        // C following the fight, a buff on 6, the HP potion on G, the pet's
        // on Q.
        Rig rig({ skill('Z', 500, 0, true), skill('X', 500, 0, true), skill('C', 1000), buff('6', 60'000),
                  potion(MacroTrigger::HpBelow, 'G', 50), potion(MacroTrigger::PetHpBelow, 'Q', 50) },
                follow);
        rig.mode = Mode::Returning;
        rig.hp = 40.0f;
        rig.pet = 40.0f;
        const std::uint64_t t0 = rig.t;
        rig.run(5000);
        const auto st = rig.engine.stats();
        const bool held = rig.count('Z', t0) == 0 && rig.count('X', t0) == 0 && rig.count('C', t0) == 0 &&
                          st.held_returning > 0;
        const bool rest = rig.count('G', t0) == 2 && rig.count('6', t0) == 1 && rig.count('Q', t0) >= 1;
        // The walk over: what waited goes at once.
        rig.mode = follow ? Mode::Fighting : Mode::Idle;
        const std::uint64_t t1 = rig.t;
        rig.run(300);
        const auto z = rig.when('Z', t1);
        check(held && rest && !z.empty() && z[0] - t1 <= 110 && rig.count('X', t1) >= 1,
              std::string(follow ? "following the fight" : "on their own timers") +
                  ": 5 s walking back -- Z, X and C never pressed (" + std::to_string(st.held_returning) +
                  " held), the potion every 3 s (" + std::to_string(rig.count('G', t0)) + "), the pet (" +
                  std::to_string(rig.count('Q', t0)) + "), the buff once (" + std::to_string(rig.count('6', t0)) +
                  "); the walk over, Z at +" + ms_list(z, t1) + " ms");
    }
    // The minute's Skills line says them.
    Rig rig({ skill('Z', 500, 0, true) }, false);
    rig.mode = Mode::Returning;
    check(logged_line(LogLevel::Info, [&] { rig.run(61'000); }, { "Skills: 0 pressed; held ", " walking back; refused" }),
          "the Skills line counts them: \"... N walking back; refused ...\"");
    // A hunter's own pass: returning() is what the macro is told, and is
    // false once hunting stops (source: the GUI's and the command line's
    // set_fight).
#ifdef GOBOT_SOURCE_DIR
    const auto read = [](const char* rel) {
        std::FILE* f = nullptr;
        std::string out;
        if (fopen_s(&f, (std::string(GOBOT_SOURCE_DIR) + rel).c_str(), "rb") != 0 || f == nullptr) return out;
        char buf[4096];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
        std::fclose(f);
        return out;
    };
    const std::string gui = read("/apps/gobot_gui/gui_app.cpp");
    const std::string cli = read("/apps/gobot_cli/main.cpp");
    check(gui.find(": hunter_->returning() ? MacroEngine::Fight::Mode::Returning") != std::string::npos &&
              cli.find(": hunter.returning() ? MacroEngine::Fight::Mode::Returning") != std::string::npos,
          "the GUI and the command line tell the macro Returning from the hunter's own returning()");
#endif
}

// ------------------------------------ the pet pressed over and over (11.10)

void test_pet_takes_turns() {
    section("The pet pressed over and over (pet_every_ms) takes at most every other press while a skill is "
            "ready (2026.11.10)");

    {
        // Z always due, free skill_gap_ms (250) after each press; the pet
        // under its mark, every 200 ms. A pet press straight after a pet press
        // only while Z was not free to go -- with Z free, Z goes first.
        Rig rig({ skill('Z', 100), potion(MacroTrigger::PetHpBelow, 'Q', 50) }, false, false);
        MacroConfig cfg = rig.engine.config();
        cfg.pet_every_ms = 200;
        cfg.skill_gap_ms = 250;
        rig.engine.set_config(cfg);
        rig.pet = 40.0f;
        const std::uint64_t from = rig.t;
        // The first time both are due with the pet's the last press: at 1 s,
        // where the black box still holds it.
        rig.run(1100);
        std::string box;
        for (const std::string& l : rig.engine.black_box_lines())
            if (l.rfind("Pet potion key Q", 0) == 0) box = l;
        rig.run(8900);
        std::uint64_t last_z = 0;
        bool have_z = false, pet_last = false;
        int both = 0;
        std::string where;
        for (const auto& p : rig.pressed) {
            if (p.first == 'Z') {
                last_z = p.second;
                have_z = true;
                pet_last = false;
                continue;
            }
            if (p.first != 'Q') continue;
            if (pet_last && have_z && p.second >= last_z + 250) {
                ++both;
                if (where.empty()) where = " -- first at +" + std::to_string(p.second - from) + " ms";
            }
            pet_last = true;
        }
        const std::size_t pets = rig.count('Q', from), zs = rig.count('Z', from);
        check(both == 0, "no pet press straight after another while Z was free to go: " + std::to_string(both) +
                             where);
        check(pets >= 45 && zs >= 38 && box.find("a skill's turn") != std::string::npos,
              "the pet still about every 200 ms (" + std::to_string(pets) + " in 10 s), Z every 250 (" +
                  std::to_string(zs) + "), \"a skill's turn\" in the black box -- " + box);
    }
    {
        // Following the fight with no fight: the skills are held, so none is
        // ready, and the pet goes at its own pace.
        Rig rig({ skill('Z', 100), potion(MacroTrigger::PetHpBelow, 'Q', 50) }, true, true);
        rig.mode = Mode::Idle;
        MacroConfig cfg = rig.engine.config();
        cfg.pet_every_ms = 200;
        rig.engine.set_config(cfg);
        rig.pet = 40.0f;
        rig.run(2000);
        const auto at = rig.when('Q');
        bool even = at.size() == 10;
        for (std::size_t i = 1; even && i < at.size(); ++i) even = at[i] - at[i - 1] == 200;
        check(even && rig.count('Z') == 0,
              "skills following the fight, no fight: the pet every 200 ms, nothing waited for -- at +" +
                  ms_list(at, at.empty() ? 0 : at[0]) + " ms");
    }
    {
        // HP's potion keeps going first, ahead of the pet and the skill.
        Rig rig({ skill('Z', 100), potion(MacroTrigger::PetHpBelow, 'Q', 50),
                  potion(MacroTrigger::HpBelow, '1', 50) },
                false, false);
        MacroConfig cfg = rig.engine.config();
        cfg.pet_every_ms = 200;
        rig.engine.set_config(cfg);
        rig.pet = 40.0f;
        rig.run(1000);
        rig.hp = 30.0f;
        const std::uint64_t low = rig.t;
        rig.run(20);
        const auto hp = rig.when('1', low);
        bool first = !hp.empty();
        for (const auto& p : rig.pressed)
            if (first && p.second >= low && p.second < hp[0]) first = false;
        check(first && hp[0] - low <= 10, "HP under its mark: its potion the first key out, as ever -- at +" +
                                              ms_list(hp, low) + " ms");
    }
}

// ------------------------------------------------------------ macro.ini

void test_skills_config() {
    section("Skills follow the fight: macro.ini");

    const std::string path = temp_path("gobot_skills.ini");
    MacroConfig cfg;
    cfg.apply_defaults();
    cfg.skills_follow_fight = true;
    cfg.sequential_skills = false;
    cfg.skill_mp_floor_pct = 25;
    cfg.skill_gap_ms = 120;
    cfg.keys[0].always_timer = true;
    cfg.keys[1].range_px = 300;
    cfg.keys[2].interval_ms = clamp_skill_ms(500);
    cfg.keys[3].interval_ms = clamp_skill_ms(59'000);
    MacroConfig back;
    IniFile raw;
    check(cfg.save(path) && back.load(path) && raw.load(path) && back.skills_follow_fight &&
              !back.sequential_skills && back.skill_mp_floor_pct == 25 && back.skill_gap_ms == 120 &&
              back.keys[0].always_timer && !back.keys[1].always_timer && back.keys[1].range_px == 300 &&
              back.keys[0].range_px == 0 && back.keys[2].interval_ms == 500 &&
              back.keys[3].interval_ms == 59'000 && raw.has_key("Keys", "key_0_always_timer") &&
              raw.has_key("Keys", "key_1_range_px"),
          "skills_follow_fight, sequential_skills, the MP floor, the gap, key_N_always_timer, "
          "key_N_range_px and cooldowns of 0.5 and 59 s read back");
    check(macro_key_kind(back.keys[3]) == MacroKeyKind::Skill && clamp_skill_ms(60'000) == 59'000 &&
              clamp_skill_ms(100) == 500 && clamp_skill_ms(1234) == 1200,
          "a skill's cooldown stays 0.5-59 s, in tenths: 60 s cannot be set, it would be a buff");

    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f) {
            std::fputs("[Macro]\ntimers_enabled=true\n[Keys]\nkey_0_keycode=90\nkey_0_enabled=true\n"
                       "key_0_interval_ms=1200\n", f);
            std::fclose(f);
        }
        MacroConfig old;
        check(old.load(path) && !old.skills_follow_fight && old.sequential_skills &&
                  old.skill_mp_floor_pct == 10 && old.skill_gap_ms == 50 && old.keys.size() == 1 &&
                  !old.keys[0].always_timer && old.keys[0].range_px == 0,
              "an old macro.ini: the tick off, sequential on, floor 10, gap 50, no own timers, any "
              "range");
    }
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f) {
            std::fputs("[Macro]\nskill_mp_floor_pct=90\nskill_gap_ms=5\n[Keys]\nkey_0_keycode=90\n"
                       "key_0_range_px=5000\n", f);
            std::fclose(f);
        }
        MacroConfig wild;
        check(wild.load(path) && wild.skill_mp_floor_pct == 50 && wild.skill_gap_ms == 30 &&
                  wild.keys[0].range_px == 1000,
              "out of range in the file: the floor at most 50, the gap at least 30 ms, range at most "
              "1000 px");
    }
    std::remove(path.c_str());
}

void test_settings_changes() {
    section("Settings changed during a run: one line of what changed (2026.11.08)");

    // 2026-11-08 at 22:14:20: the MP potion's key from H to F, mid-run.
    MacroConfig was;
    was.apply_defaults();
    was.keys[7].keycode = 'H';
    MacroConfig now = was;
    now.keys[7].keycode = 'F';
    now.keys[0].interval_ms = 500;
    HunterConfig hunt_was, hunt_now;
    hunt_was.enabled = false;
    hunt_now.enabled = true;
    IniFile a, b;
    was.store(a);
    hunt_was.store(a);
    now.store(b);
    hunt_now.store(b);
    const std::string line = settings_changes(a, b);
    check(line.find("Keys.key_7_keycode 72 (H) -> 70 (F)") != std::string::npos &&
              line.find("Keys.key_0_interval_ms 1200 -> 500") != std::string::npos &&
              line.find("Hunt.enabled false -> true") != std::string::npos &&
              std::count(line.begin(), line.end(), ';') == 2,
          "the MP key H to F, Z's interval and Tự đánh quái, each as the file has it, a key's code with its name: " +
              line);
    check(settings_changes(a, a).empty() && settings_changes(a, b, 1).find("; and 2 more") != std::string::npos,
          "nothing changed: no line; past `most`: how many more -- " + settings_changes(a, b, 1));
    // A row taken out, and one put in.
    MacroConfig fewer = was;
    fewer.keys.pop_back();
    IniFile c;
    fewer.store(c);
    hunt_was.store(c);
    const std::string gone = settings_changes(a, c, 20);
    check(gone.find("Keys.key_7_keycode 72 (H) -> (none)") != std::string::npos &&
              settings_changes(c, a, 20).find("Keys.key_7_keycode (none) -> 72 (H)") != std::string::npos,
          "a row taken out, or put in: said as none -- " + gone);

    // What save() writes is what store() puts in, every key of it.
    const std::string path = temp_path("gobot_store.ini");
    IniFile saved, stored;
    now.store(stored);
    bool same = now.save(path) && saved.load(path);
    for (const std::string& s : stored.sections())
        for (const std::string& k : stored.keys(s)) same = same && saved.get(s, k, "?") == stored.get(s, k);
    for (const std::string& s : saved.sections()) same = same && saved.keys(s).size() == stored.keys(s).size();
    std::remove(path.c_str());
    check(same && settings_changes(stored, saved).empty(), "macro.ini as saved holds what store() gives, no more");
}

// ------------------------------------------------------------ the sequence

/// The sequence on a clock of the test's own, with a pretend keyboard and a
/// pretend hunter: each turn its thread would take, taken by step() when it
/// is due by that clock, a step every 10 ms. What the checks count is the
/// test's time, never how much of a loaded CPU a thread got.
struct SeqRig {
    InputManager         input;
    ExecutionCoordinator coord;
    MacroEngine          engine{ input, coord };
    std::uint64_t        t = 21'000'000;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> pressed;
    Mode                 mode = Mode::Idle;

    SeqRig(bool follow, bool with_hunter) {
        MacroEngine::Clock c;
        c.now_ms = [this] { return t; };
        c.sleep_ms = [this](std::uint32_t ms) { t += ms; };
        engine.set_clock(c);
        MacroEngine::Hands h;
        h.focused = [] { return true; };
        h.key = [this](std::uint32_t vk, int) {
            pressed.emplace_back(vk, t);
            return true;
        };
        engine.set_hands(h);
        if (with_hunter)
            engine.set_fight([this] {
                MacroEngine::Fight f;
                f.mode = mode;
                f.dx = 80;
                f.attack_key = VK_SPACE;
                return f;
            });
        engine.set_config(config(follow));
        engine.start_stepped();
    }
    ~SeqRig() { engine.stop(); }

    /// X then C, each with 100 ms after it -- 92 to 108 with the step's own
    /// jitter -- looping.
    static MacroConfig config(bool follow) {
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys.clear();
        cfg.sequence = { MacroStep{ 'X', 0, 100, "X" }, MacroStep{ 'C', 0, 100, "C" } };
        cfg.sequence_enabled = true;
        cfg.global_jitter_pct = 0;
        cfg.skills_follow_fight = follow;
        return cfg;
    }

    /// `ms` of the test's time: a step every 10 ms, as the timer thread beats.
    void run(std::uint64_t ms) {
        for (const std::uint64_t until = t + ms; t < until; t += 10) engine.step();
    }
    /// Up to `ms` of it, until one more key has gone: when it went, or 0.
    std::uint64_t run_until_key(std::uint64_t ms) {
        const std::size_t had = pressed.size();
        for (const std::uint64_t until = t + ms; t < until && pressed.size() == had; t += 10) engine.step();
        return pressed.size() > had ? pressed.back().second : 0;
    }
    std::string keys(std::size_t from = 0) const {
        std::string s;
        for (std::size_t i = from; i < pressed.size(); ++i) s += static_cast<char>(pressed[i].first);
        return s;
    }
};

void test_sequence_follows() {
    section("Skills follow the fight: the key sequence is held with them");

    // A step every 100 ms, give or take its 8%, on a clock that a step of
    // the test winds on 10 ms: each step 100 or 110 ms after the last, so
    // 0.7 s holds exactly seven of them -- X first, and none held.
    {
        SeqRig rig(false, true);        // the tick off: the sequence as ever
        const std::uint64_t from = rig.t;
        rig.run(700);
        bool paced = rig.pressed.size() == 7 && rig.pressed.front().second == from;
        for (std::size_t i = 1; i < rig.pressed.size(); ++i) {
            const std::uint64_t gap = rig.pressed[i].second - rig.pressed[i - 1].second;
            paced = paced && (gap == 100 || gap == 110);
        }
        check(paced && rig.keys() == "XCXCXCX" && rig.engine.stats().held_not_fighting == 0 &&
                  rig.engine.stats().sequence_presses == 7,
              "the tick off, the hunter not fighting: the sequence presses as it always has -- " +
                  std::to_string(rig.pressed.size()) + " steps in 0.7 s of the test's clock (" + rig.keys() +
                  "), each 100 or 110 ms after the last, none held");
    }
    {
        SeqRig rig(true, false);        // the tick on, no hunter
        rig.run(700);
        check(rig.keys() == "XCXCXCX" && rig.engine.stats().held_not_fighting == 0,
              "the tick on but no hunter: as it always has too -- " + rig.keys() + " in 0.7 s");
    }
    {
        SeqRig rig(true, true);         // the tick on, not fighting
        rig.run(700);
        const bool none = rig.pressed.empty() && rig.engine.sequence_position() == 0 &&
                          rig.engine.stats().held_not_fighting > 0;
        // A fight: the sequence goes, from where it was -- at its next look,
        // 50 ms at most.
        rig.mode = Mode::Fighting;
        const std::uint64_t fought = rig.t;
        const std::uint64_t x_at = rig.run_until_key(200);
        rig.mode = Mode::Idle;
        check(none && x_at != 0 && x_at - fought <= 50 && rig.keys() == "X",
              "the tick on, not fighting: no step for 0.7 s and the sequence where it was; a fight, and it "
              "starts from there -- " + rig.keys() + ", " + std::to_string(x_at - fought) + " ms after the fight");
        // Stopped mid-way: X out, C waiting where it is, held, for as long
        // as there is no fight; then C, the step it stopped at.
        const std::uint64_t held_before = rig.engine.stats().held_not_fighting;
        rig.run(900);
        const bool waited = rig.keys() == "X" && rig.engine.sequence_position() == 1 &&
                            rig.engine.stats().held_not_fighting > held_before;
        rig.mode = Mode::Fighting;
        const std::uint64_t again = rig.t;
        const std::uint64_t c_at = rig.run_until_key(200);
        check(waited && c_at != 0 && c_at - again <= 50 && rig.keys() == "XC",
              "held again: nothing for 0.9 s, and the next step is the one it stopped at -- " + rig.keys());
    }
    {
        // The thread takes the same turns on the real clock: started, a step
        // goes out -- waited for as long as a loaded machine needs.
        InputManager input;
        ExecutionCoordinator coord;
        MacroEngine engine(input, coord);
        std::atomic<int> pressed{ 0 };
        MacroEngine::Hands h;
        h.focused = [] { return true; };
        h.key = [&pressed](std::uint32_t, int) {
            ++pressed;
            return true;
        };
        engine.set_hands(h);
        engine.set_config(SeqRig::config(false));
        engine.start();
        for (int i = 0; i < 1000 && pressed.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        engine.stop();
        check(pressed.load() > 0, "and on its own thread, on the real clock, the sequence presses too");
    }
}

} // namespace

void test_skills(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_skills_idle();
    test_skills_sequential();
    test_skills_one_at_a_time();
    test_skills_mp_floor();
    test_skills_range_and_dead();
    test_skills_refused();
    test_skills_fight_read_late();
    test_skills_own_timer_gates();
    test_skills_keys_change_mid_press();
    test_skills_log();
    test_skills_off_as_before();
    test_skills_timed_gap();
    test_skills_returning();
    test_pet_takes_turns();
    test_skills_config();
    test_settings_changes();
    test_sequence_follows();
}
