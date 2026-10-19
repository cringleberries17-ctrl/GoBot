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
            engine.set_vitals(hp, mp);
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

// ---------------------------------------------------- the sequence, threaded

/// The sequence on its own thread, on the real clock, with a pretend
/// keyboard and a pretend hunter.
struct SeqRig {
    InputManager         input;
    ExecutionCoordinator coord;
    MacroEngine          engine{ input, coord };
    std::mutex           m;
    std::vector<std::uint32_t> pressed;
    std::atomic<int>     mode{ static_cast<int>(Mode::Idle) };

    SeqRig(bool follow, bool with_hunter) {
        MacroEngine::Hands h;
        h.focused = [] { return true; };
        h.key = [this](std::uint32_t vk, int) {
            std::lock_guard<std::mutex> lock(m);
            pressed.push_back(vk);
            return true;
        };
        engine.set_hands(h);
        if (with_hunter)
            engine.set_fight([this] {
                MacroEngine::Fight f;
                f.mode = static_cast<Mode>(mode.load());
                f.dx = 80;
                f.attack_key = VK_SPACE;
                return f;
            });
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys.clear();
        cfg.sequence = { MacroStep{ 'X', 0, 100, "X" }, MacroStep{ 'C', 0, 100, "C" } };
        cfg.sequence_enabled = true;
        cfg.skills_follow_fight = follow;
        engine.set_config(cfg);
    }
    ~SeqRig() { engine.stop(); }

    std::vector<std::uint32_t> keys() {
        std::lock_guard<std::mutex> lock(m);
        return pressed;
    }
    void wait(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
};

void test_sequence_follows() {
    section("Skills follow the fight: the key sequence is held with them");

    {
        SeqRig rig(false, true);        // the tick off: the sequence as ever
        rig.engine.start();
        rig.wait(700);
        rig.engine.stop();
        check(rig.keys().size() >= 4,
              "the tick off, the hunter not fighting: the sequence presses as it always has -- " +
                  std::to_string(rig.keys().size()) + " steps in 0.7 s");
    }
    {
        SeqRig rig(true, false);        // the tick on, no hunter
        rig.engine.start();
        rig.wait(700);
        rig.engine.stop();
        check(rig.keys().size() >= 4, "the tick on but no hunter: as it always has too");
    }
    {
        SeqRig rig(true, true);         // the tick on, not fighting
        rig.engine.start();
        rig.wait(700);
        const bool none = rig.keys().empty() && rig.engine.sequence_position() == 0;
        // A fight: the sequence goes, from where it was.
        rig.mode = static_cast<int>(Mode::Fighting);
        std::vector<std::uint32_t> got;
        for (int i = 0; i < 100 && got.empty(); ++i) {
            rig.wait(5);
            got = rig.keys();
        }
        rig.mode = static_cast<int>(Mode::Idle);
        check(none && !got.empty() && got[0] == 'X',
              "the tick on, not fighting: no step for 0.7 s and the sequence where it was; a fight, "
              "and it starts from there");
        // Stopped mid-way: X out, C waiting.
        rig.wait(400);
        const std::size_t at = rig.engine.sequence_position();
        const std::size_t before = rig.keys().size();
        rig.wait(500);
        const bool waited = rig.keys().size() == before;
        rig.mode = static_cast<int>(Mode::Fighting);
        std::vector<std::uint32_t> after;
        for (int i = 0; i < 100 && after.size() <= before; ++i) {
            rig.wait(5);
            after = rig.keys();
        }
        rig.engine.stop();
        const std::uint32_t next = at == 1 ? 'C' : 'X';
        check(waited && after.size() > before && after[before] == next &&
                  rig.engine.stats().held_not_fighting > 0,
              "held again: nothing for 0.5 s, and the next step is the one it stopped at");
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
    test_skills_config();
    test_sequence_follows();
}
