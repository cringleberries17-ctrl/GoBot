// gobot_test -- the macro in a fight: the potion rows' second tier, the rule
// that only the more urgent tier drinks, potions before every other key, and
// "Quái đang thấy", the names the monster lists are picked from.
//
// Nothing here goes near the game. The engine is driven a step at a time on a
// clock of the test's own, with a pretend keyboard that writes down every key
// and when -- as the buff tests drive it; where the potions meet the key
// sequence, its two threads' turns are taken on that clock as well, each
// where its thread would be (Threads) -- except for a few checks of the
// engine's own threads, which run on the real clock with the same pretend
// keyboard and count only what a loaded machine cannot put out of order.

#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/macro.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/monster_list.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
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

/// A file in this run's own temp folder, which main.cpp makes and removes:
/// see temp_file there.
std::string temp_path(const char* name) {
    char dir[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return name;
    return std::string(dir) + "gobot_test_" + std::to_string(GetCurrentProcessId()) + "\\" + name;
}

MacroKey timer_key(std::uint32_t vk, std::uint32_t interval_ms, const char* label) {
    MacroKey k;
    k.keycode = vk;
    k.label = label;
    k.enabled = true;
    k.interval_ms = interval_ms;
    return k;
}

MacroKey potion_key(MacroTrigger trigger, std::uint32_t vk, int pct, const char* label,
                    int jitter = 15) {
    MacroKey k = timer_key(vk, 3000, label);
    k.trigger = trigger;
    k.threshold_pct = pct;
    k.jitter_pct = jitter;
    return k;
}

MacroKey hp_key(std::uint32_t vk, int pct, int jitter = 0) {
    return potion_key(MacroTrigger::HpBelow, vk, pct, "HP", jitter);
}
MacroKey mp_key(std::uint32_t vk, int pct, int jitter = 0) {
    return potion_key(MacroTrigger::MpBelow, vk, pct, "MP", jitter);
}

// ------------------------------------------------------- the rows' config

void test_potion_tiers_config() {
    section("Potions: a second tier on each row, in macro.ini");

    // A new Lv2, as filling its empty box on the simple screen makes it.
    {
        const MacroKey hp2 = macro_potion_key(MacroKeyKind::HpPotion, VK_F1, 2);
        check(hp2.keycode == VK_F1 && hp2.trigger == MacroTrigger::HpBelow &&
                  hp2.threshold_pct == 30 && hp2.enabled && hp2.interval_ms == 3000 &&
                  hp2.jitter_pct == 15 && hp2.label == "HP potion 2" &&
                  macro_key_kind(hp2) == MacroKeyKind::HpPotion,
              "a new Lv2 HP key: below 30%, every 3 s at most, jitter 15, on, 'HP potion 2'");
        const MacroKey mp2 = macro_potion_key(MacroKeyKind::MpPotion, VK_F2, 2);
        check(mp2.trigger == MacroTrigger::MpBelow && mp2.threshold_pct == 20 && mp2.enabled &&
                  mp2.interval_ms == 3000 && mp2.jitter_pct == 15 && mp2.label == "MP potion 2",
              "a new Lv2 MP key: below 20%, 'MP potion 2'");
        const MacroKey hp1 = macro_potion_key(MacroKeyKind::HpPotion, '1', 1);
        const MacroKey mp1 = macro_potion_key(MacroKeyKind::MpPotion, '2', 1);
        check(hp1.threshold_pct == 50 && hp1.label == "HP potion" && mp1.threshold_pct == 30 &&
                  mp1.label == "MP potion",
              "and a Lv1 as the simple screen has always put one back: HP 50%, MP 30%");
        check(macro_potion_key(MacroKeyKind::Skill, 'Z', 2).keycode == 0 &&
                  macro_potion_key(MacroKeyKind::Buff, '3', 2).keycode == 0,
              "a skill or a buff is not a potion: nothing made");
    }

    // The player's eleven keys, and a second HP and MP potion after them.
    std::vector<MacroKey> keys = {
        timer_key('Z', 1200, "Skill 1"),   timer_key('X', 3000, "Skill 2"),
        timer_key('C', 6000, "Skill 3"),   timer_key('V', 9000, "Skill 4"),
        timer_key(VK_SPACE, 3000, "Skill 5"), timer_key('N', 3000, "Skill 6"),
        timer_key('3', 1600000, "Buff 1"), timer_key('4', 1600000, "Buff 2"),
        potion_key(MacroTrigger::HpBelow, '1', 50, "HP potion"),
        potion_key(MacroTrigger::MpBelow, '2', 31, "MP potion"),
        timer_key('5', 60000, "Buff 3"),
    };
    keys.push_back(macro_potion_key(MacroKeyKind::HpPotion, VK_F1, 2));
    keys.push_back(macro_potion_key(MacroKeyKind::MpPotion, VK_F2, 2));
    keys.back().threshold_pct = 15;

    MacroConfig cfg;
    cfg.apply_defaults();
    cfg.keys = keys;
    const std::string path = temp_path("gobot_macro_fight.ini");
    MacroConfig back;
    check(MacroConfig::kMaxKeys == 16 && cfg.save(path) && back.load(path) &&
              back.keys.size() == 13,
          "the player's 11 keys and two Lv2 potions -- 13 -- save and load");
    bool same = back.keys.size() == keys.size();
    for (std::size_t i = 0; same && i < keys.size(); ++i)
        same = back.keys[i].keycode == keys[i].keycode &&
               back.keys[i].threshold_pct == keys[i].threshold_pct &&
               back.keys[i].trigger == keys[i].trigger && back.keys[i].label == keys[i].label;
    check(same, "all 13 in order");
    const auto hp = macro_keys_of_kind(back.keys, MacroKeyKind::HpPotion);
    const auto mp = macro_keys_of_kind(back.keys, MacroKeyKind::MpPotion);
    check(hp.size() == 2 && back.keys[hp[0]].keycode == '1' && back.keys[hp[1]].keycode == VK_F1 &&
              back.keys[hp[1]].threshold_pct == 30,
          "HP: Lv1 is '1', the first HP potion in key order; Lv2 is F1 at 30%");
    check(mp.size() == 2 && back.keys[mp[0]].keycode == '2' && back.keys[mp[1]].keycode == VK_F2 &&
              back.keys[mp[1]].threshold_pct == 15,
          "MP: Lv1 is '2'; Lv2 is F2 at 15%");
    check(macro_keys_of_kind(back.keys, MacroKeyKind::Skill).size() == 6 &&
              macro_keys_of_kind(back.keys, MacroKeyKind::Buff).size() == 3,
          "and the six skills and three buffs are where they were");

    // A changed Lv2 key and threshold round-trip.
    back.keys[hp[1]].keycode = VK_F3;
    back.keys[hp[1]].threshold_pct = 22;
    MacroConfig changed;
    check(back.save(path) && changed.load(path) && changed.keys.size() == 13 &&
              changed.keys[hp[1]].keycode == VK_F3 && changed.keys[hp[1]].threshold_pct == 22,
          "a Lv2 changed to F3 at 22% reads back so");

    // Lv2 emptied on the simple screen: gone, the rest kept.
    back.keys[hp[1]].keycode = 0;
    check(drop_empty_keys(back.keys) && back.keys.size() == 12 &&
              macro_keys_of_kind(back.keys, MacroKeyKind::HpPotion).size() == 1,
          "an emptied Lv2 is taken out");
    MacroConfig again;
    IniFile raw;
    check(back.save(path) && again.load(path) && raw.load(path) && again.keys.size() == 12 &&
              !raw.has_key("Keys", "key_12_keycode") &&
              std::none_of(again.keys.begin(), again.keys.end(),
                           [](const MacroKey& k) { return k.keycode == VK_F3; }),
          "and is gone from the saved file");
    {
        std::vector<MacroKey> some = { potion_key(MacroTrigger::HpBelow, 0, 50, "HP potion"),
                                       timer_key(0, 3000, "Skill"), timer_key('Z', 3000, "Z"),
                                       timer_key(0, 300000, "Buff"),
                                       potion_key(MacroTrigger::MpBelow, '2', 30, "MP potion"),
                                       potion_key(MacroTrigger::MpBelow, 0, 20, "MP potion 2") };
        check(drop_empty_keys(some) && some.size() == 3 && some[0].keycode == 0 &&
                  some[1].keycode == 'Z' && some[2].keycode == '2' && !drop_empty_keys(some),
              "an empty skill, buff and Lv2 go too; a row's Lv1 is never taken out, key or none");
    }

    check(clamp_potion_pct(2) == 5 && clamp_potion_pct(99) == 95 && clamp_potion_pct(30) == 30 &&
              kPotionMinPct == 5 && kPotionMaxPct == 95,
          "a potion's threshold on the simple screen stays within 5..95%");

    // Sixteen keys, not seventeen.
    {
        MacroConfig many;
        many.apply_defaults();
        many.keys.clear();
        for (int i = 0; i < 17; ++i)
            many.keys.push_back(timer_key(static_cast<std::uint32_t>('A' + i), 3000, "k"));
        MacroConfig read;
        IniFile file;
        check(many.save(path) && read.load(path) && file.load(path) && read.keys.size() == 16 &&
                  file.has_key("Keys", "key_15_keycode") && !file.has_key("Keys", "key_16_keycode"),
              "a 16th key is kept and a 17th is not");
        InputManager input;
        ExecutionCoordinator coord;
        MacroEngine engine(input, coord);
        engine.set_config(many);
        check(engine.config().keys.size() == 16, "and the engine takes 16");
    }
    std::remove(path.c_str());
}

// ------------------------------------------------------------- the engine

/// The engine on a clock of the test's own, with a pretend keyboard: every
/// key that reached it and when, the vitals pushed before every pass, and the
/// waits inside a pass -- the gap between an HP and an MP potion -- wound on
/// the same clock.
struct Rig {
    InputManager         input;
    ExecutionCoordinator coord;
    MacroEngine          engine{ input, coord };
    std::uint64_t        t = 7'000'000;
    const std::uint64_t  start = 7'000'000;
    float                hp = 100.0f, mp = 100.0f;
    bool                 push_vitals = true;
    bool                 focused = true;
    std::uint32_t        broken = 0;     ///< a key whose press never goes out
    /// A key that goes down and cannot come up: the game loses focus while
    /// it is held. It stays down until everything is let go of.
    std::uint32_t        sticks = 0;
    bool                 stuck = false;
    int                  releases = 0;
    /// How long a press takes on the clock: 0 unless a test says, so every
    /// key of a pass goes out at the same moment.
    std::uint64_t        press_takes = 0;
    std::function<void(std::uint32_t)> on_press;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> pressed;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> tried;

    explicit Rig(std::vector<MacroKey> keys) {
        MacroEngine::Clock c;
        c.now_ms = [this] { return t; };
        c.sleep_ms = [this](std::uint32_t ms) { t += ms; };
        engine.set_clock(c);
        MacroEngine::Hands h;
        h.focused = [this] { return focused; };
        h.key = [this](std::uint32_t vk, int hold) {
            tried.emplace_back(vk, t);
            if (vk == broken) return false;
            const std::uint64_t at = t;
            t += hold > 0 ? static_cast<std::uint64_t>(hold) : press_takes;
            if (vk == sticks) {
                stuck = true;
                return false;
            }
            pressed.emplace_back(vk, at);
            if (on_press) on_press(vk);
            return true;
        };
        h.held = [this](std::uint32_t vk) { return stuck && vk == sticks; };
        h.release_all = [this] {
            stuck = false;
            ++releases;
        };
        engine.set_hands(h);
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys = std::move(keys);
        cfg.sequence_enabled = false;
        cfg.require_focus = true;
        cfg.global_jitter_pct = 0;
        engine.set_config(cfg);
        engine.start_stepped();
    }
    ~Rig() { engine.stop(); }

    /// `ms` of pretend time: the vitals and a pass of the timers every 10 ms,
    /// as the GUI and the timer thread do.
    void run(std::uint64_t ms) {
        for (const std::uint64_t until = t + ms; t < until; t += 10) {
            if (push_vitals) engine.set_vitals(hp, mp);
            engine.step();
        }
    }
    std::vector<std::uint64_t> when(std::uint32_t vk) const {
        std::vector<std::uint64_t> out;
        for (const auto& p : pressed)
            if (p.first == vk) out.push_back(p.second);
        return out;
    }
    std::vector<std::uint64_t> tries(std::uint32_t vk) const {
        std::vector<std::uint64_t> out;
        for (const auto& p : tried)
            if (p.first == vk) out.push_back(p.second);
        return out;
    }
    /// Presses of `vk` from `from` on.
    std::size_t count(std::uint32_t vk, std::uint64_t from = 0) const {
        std::size_t n = 0;
        for (const auto& p : pressed)
            if (p.first == vk && p.second >= from) ++n;
        return n;
    }
    /// The first key pressed at `from` or after, and when; 0 for none.
    std::pair<std::uint32_t, std::uint64_t> first_from(std::uint64_t from) const {
        for (const auto& p : pressed)
            if (p.second >= from) return p;
        return { 0, 0 };
    }
};

std::string ms_list(const std::vector<std::uint64_t>& at, std::uint64_t from) {
    std::string s;
    for (const std::uint64_t t : at) s += (s.empty() ? "" : ", ") + std::to_string(t - from);
    return s.empty() ? "none" : s;
}

void test_potion_tiers_engine() {
    section("Potions: only the more urgent tier drinks (GoC), each on its own cooldown");

    {
        // HP Lv1 at 50%, Lv2 at 30%, 3-s cooldowns, no jitter.
        Rig rig({ hp_key('1', 50), hp_key(VK_F1, 30) });
        rig.run(1000);
        check(rig.pressed.empty(), "HP 100%: neither drinks");

        rig.hp = 40.0f;
        const std::uint64_t drop1 = rig.t;
        const std::uint64_t condition_before = rig.engine.stats().held_condition;
        rig.run(2500);
        const auto lv1 = rig.when('1');
        check(lv1.size() == 1 && lv1[0] - drop1 <= 100 && rig.when(VK_F1).empty(),
              "HP 40%: only Lv1 (50%) drinks, within 100 ms -- at +" + ms_list(lv1, drop1) + " ms");
        check(rig.engine.stats().held_condition - condition_before >= 15,
              "while Lv2 (30%) is held for its condition -- counted as held_condition");

        rig.hp = 25.0f;
        const std::uint64_t drop2 = rig.t;
        const std::uint64_t tier_before = rig.engine.stats().held_tier;
        rig.run(10'000);
        const auto lv2 = rig.when(VK_F1);
        check(rig.count('1', drop2) == 0,
              "HP 25%, under both: Lv1 never drinks, cooldown over or not -- " +
                  std::to_string(rig.count('1', drop2)) + " times");
        bool on_beat = lv2.size() == 4;
        for (std::size_t i = 0; on_beat && i < lv2.size(); ++i)
            on_beat = lv2[i] - drop2 == i * 3000;
        check(on_beat, "only Lv2 (30%) drinks, at 0, 3, 6 and 9 s on its own 3-s cooldown -- at +" +
                           ms_list(lv2, drop2) + " ms");
        check(rig.engine.stats().held_tier > tier_before,
              "and Lv1, low enough to drink, is counted as held by the tier rule (held_tier)");

        rig.hp = 45.0f;
        const std::uint64_t up = rig.t;
        rig.run(4000);
        const auto lv1b = rig.when('1');
        std::vector<std::uint64_t> after;
        for (const std::uint64_t at : lv1b)
            if (at >= up) after.push_back(at);
        check(after.size() == 2 && after[0] - up <= 100 && after[1] - after[0] == 3000 &&
                  rig.count(VK_F1, up) == 0,
              "HP back at 45%: Lv1 drinks again at once, then on its own cooldown, and Lv2 not -- "
              "at +" + ms_list(after, up) + " ms");
    }
    {
        // Lv2 set ABOVE Lv1: by threshold, not by row.
        Rig rig({ hp_key('1', 30), hp_key(VK_F1, 50) });
        rig.hp = 25.0f;
        rig.run(5000);
        rig.hp = 40.0f;
        const std::uint64_t mid = rig.t;
        rig.run(5000);
        check(rig.count(VK_F1, 0) == rig.count(VK_F1, mid) && rig.count('1', 0) == 2 &&
                  rig.count('1', mid) == 0 && rig.count(VK_F1, mid) == 2,
              "a Lv2 at 50% over a Lv1 at 30%: under both the 30% key drinks, between them the "
              "50% one -- by threshold, not by row");
    }
    {
        Rig rig({ hp_key('1', 50), hp_key(VK_F1, 50) });
        rig.hp = 40.0f;
        rig.run(10'000);
        check(rig.count('1') == 4 && rig.count(VK_F1) == 0,
              "two tiers at the same 50%: only Lv1, the first in key order, drinks");
    }
    {
        Rig rig({ hp_key('1', 50), hp_key('1', 30) });
        rig.hp = 25.0f;
        rig.run(10'000);
        check(rig.count('1') == 4,
              "Lv1 and Lv2 on the same key, under both: the key is pressed once per cooldown, not "
              "twice -- " + std::to_string(rig.count('1')) + " times in 10 s");
    }
    {
        MacroKey off = hp_key('1', 50);
        off.enabled = false;
        Rig a({ off, hp_key(VK_F1, 30) });
        a.hp = 25.0f;
        a.run(4000);
        Rig b({ hp_key(0, 50), hp_key(VK_F1, 30) });
        b.hp = 25.0f;
        b.run(4000);
        check(a.count(VK_F1) == 2 && a.count('1') == 0 && b.count(VK_F1) == 2,
              "a Lv1 switched off, or with no key, leaves Lv2 to drink on its own");
    }
    {
        Rig rig({ hp_key('1', 50), hp_key(VK_F1, 30), hp_key(VK_F2, 20) });
        rig.hp = 15.0f;
        rig.run(1000);
        const bool lowest = rig.count(VK_F2) == 1 && rig.count('1') == 0 && rig.count(VK_F1) == 0;
        rig.hp = 25.0f;
        const std::uint64_t mid = rig.t;
        rig.run(1000);
        check(lowest && rig.count(VK_F1, mid) == 1 && rig.count('1', mid) == 0,
              "a third HP key at 20%: chosen under 20%; at 25% the 30% key");
    }
    {
        // One key of each kind, as every setup before the tiers: what the
        // engine always did -- a press as soon as the bar is under the
        // threshold, then once per cooldown; no reading means no drink.
        Rig rig({ hp_key('1', 50), mp_key('2', 30) });
        rig.push_vitals = false;
        rig.run(1000);
        const auto st = rig.engine.stats();
        check(rig.pressed.empty() && st.held_no_reading > 0,
              "one key of each kind, no reading: nothing drinks, counted as held_no_reading");
        rig.push_vitals = true;
        rig.hp = 80.0f;
        rig.mp = 80.0f;
        rig.run(1000);
        check(rig.pressed.empty() && rig.engine.stats().held_condition > st.held_condition,
              "above the thresholds: held_condition, as before");
        rig.hp = 40.0f;
        const std::uint64_t drop = rig.t;
        rig.run(9500);
        const auto hp = rig.when('1');
        bool beat = hp.size() == 4;
        for (std::size_t i = 0; beat && i < hp.size(); ++i) beat = hp[i] - drop == i * 3000;
        check(beat && rig.when('2').empty() && rig.engine.stats().held_tier == 0,
              "HP under 50%: '1' at once and every 3 s, the MP key never, nothing held by tier "
              "-- at +" + ms_list(hp, drop) + " ms");
    }
}

void test_potions_first() {
    section("Potions: HP, then MP 20 ms after, before any other key");

    {
        // A skill first in key order, both potions due with it.
        Rig rig({ timer_key('Z', 1000, "Skill 1"), hp_key('1', 50), mp_key('2', 30) });
        rig.hp = 40.0f;
        rig.mp = 20.0f;
        rig.run(10);
        const bool order = rig.pressed.size() == 3 && rig.pressed[0].first == '1' &&
                           rig.pressed[1].first == '2' && rig.pressed[2].first == 'Z';
        check(order && rig.pressed[1].second - rig.pressed[0].second >= 20,
              "HP and MP due with a skill earlier in key order, in one pass: '1', then '2' " +
                  std::to_string(order ? rig.pressed[1].second - rig.pressed[0].second : 0) +
                  " ms after it came up, then the skill");
    }
    {
        Rig rig({ timer_key('Z', 1000, "Skill 1"), timer_key('3', 600000, "Buff"),
                  mp_key('2', 30) });
        rig.mp = 10.0f;
        rig.run(10);
        check(!rig.pressed.empty() && rig.pressed[0].first == '2',
              "an MP potion due with a skill and a buff: the potion first");
    }
    {
        // Another job's key -- the hunter's 30-ms face tap -- holds the keys
        // as the potion falls due.
        Rig rig({ hp_key('1', 50) });
        rig.run(1000);
        rig.coord.request("hunt-face", Priority::Combat);
        rig.hp = 40.0f;
        rig.run(40);
        const bool refused = rig.pressed.empty();
        rig.coord.release("hunt-face");
        const std::uint64_t freed = rig.t;
        rig.run(500);
        const auto at = rig.when('1');
        check(refused && at.size() == 1 && at[0] >= freed && at[0] - freed <= 100,
              "the keys held 40 ms by another job: the potion goes within 100 ms of their release, "
              "not 3 s later -- +" + ms_list(at, freed) + " ms");
        rig.run(3000);
        const auto all = rig.when('1');
        check(all.size() == 2 && all[1] - all[0] == 3000,
              "and its cooldown counts from that press");
    }
    {
        // A dialog's hold -- the popup closer's, 2 s.
        Rig rig({ hp_key('1', 50) });
        rig.run(1000);
        rig.coord.engage_critical("đóng thông báo");
        rig.hp = 40.0f;
        rig.run(2000);
        const bool held = rig.pressed.empty();
        rig.coord.disengage_critical();
        const std::uint64_t over = rig.t;
        rig.run(200);
        const auto at = rig.when('1');
        check(held && at.size() == 1 && at[0] - over <= 100,
              "held off 2 s by a hold: the potion goes within 100 ms of its end, its cooldown "
              "not started meanwhile -- +" + ms_list(at, over) + " ms");
    }
    {
        // HP falls at an odd moment.
        Rig rig({ hp_key('1', 50) });
        rig.run(1230);
        rig.hp = 30.0f;
        const std::uint64_t drop = rig.t;
        rig.run(300);
        const auto at = rig.when('1');
        check(at.size() == 1 && at[0] - drop <= 100,
              "an HP drop is acted on within 100 ms of the reading -- +" + ms_list(at, drop) + " ms");
    }
    {
        // A potion that cannot be pressed holds the rest up a moment, no more.
        Rig rig({ timer_key('Z', 1000, "Skill 1"), hp_key('1', 50) });
        rig.broken = '1';
        rig.hp = 40.0f;
        const std::uint64_t t0 = rig.t;
        rig.run(3000);
        const auto z = rig.when('Z');
        const auto tries = rig.tries('1');
        check(!z.empty() && z[0] - t0 >= MacroEngine::kPotionFirstMs && z[0] - t0 <= 320,
              "a potion due that does not go out: nothing else starts for 300 ms, then the skill "
              "does -- the skill at +" + (z.empty() ? std::string("never") : std::to_string(z[0] - t0)) +
                  " ms");
        check(z.size() >= 3 && tries.size() >= 25,
              "and after that the skill keeps its beat while the potion is tried every 100 ms -- " +
                  std::to_string(z.size()) + " skill presses, " + std::to_string(tries.size()) +
                  " potion tries");
    }
}

/// A skill on a 1230-ms timer that starts `delay` ms in: one of the phases a
/// potion falling due can meet it at.
MacroKey phase_skill(std::uint32_t delay) {
    MacroKey z = timer_key('Z', 1230, "Skill 1");
    z.jitter_pct = 0;
    z.start_delay_ms = delay;
    return z;
}

/// The moment a potion falls due -- a hold over, a reading come in, a
/// cooldown run out, the macro started -- it goes, before any other key,
/// whatever the phase of the skills around it. Each press takes 40 ms here,
/// as a real one does.
void test_potion_the_moment_due() {
    section("Potions: the moment one is due, before any skill, at every phase");

    {
        // A hold of a little over 2 s with HP under the mark, a skill at each
        // of 123 phases. Looked at only every 100 ms, the potion waited for
        // its next look and the skill went first in some phases: the wait
        // allowed a potion had run out during the hold.
        int first = 0, cases = 0;
        std::uint64_t worst = 0;
        std::string missed;
        for (std::uint32_t s = 0; s < 1230; s += 10) {
            Rig rig({ phase_skill(s), hp_key('1', 50) });
            rig.press_takes = 40;
            rig.run(1000);
            rig.hp = 30.0f;
            rig.coord.engage_critical("đóng thông báo");
            rig.run(2037);
            rig.coord.disengage_critical();
            const std::uint64_t over = rig.t;
            rig.run(400);
            ++cases;
            const auto f = rig.first_from(over);
            const auto potion = rig.when('1');
            if (f.first == '1') ++first;
            else if (missed.empty())
                missed = " (start delay " + std::to_string(s) + ": " + std::string(1, static_cast<char>(f.first)) +
                         " first)";
            worst = std::max<std::uint64_t>(worst, potion.empty() ? 9999 : potion[0] - over);
        }
        check(first == cases && worst <= 10,
              "a hold over with HP low: the potion is the first key after it in all " +
                  std::to_string(cases) + " phases of a skill -- " + std::to_string(first) +
                  ", at most " + std::to_string(worst) + " ms after it" + missed);
    }
    {
        // HP read low at each of 123 phases.
        int first = 0, cases = 0;
        std::uint64_t worst = 0;
        for (std::uint32_t s = 0; s < 1230; s += 10) {
            Rig rig({ phase_skill(s), hp_key('1', 50) });
            rig.press_takes = 40;
            rig.run(1503);
            rig.hp = 30.0f;
            const std::uint64_t low = rig.t;
            rig.run(400);
            ++cases;
            if (rig.first_from(low).first == '1') ++first;
            const auto potion = rig.when('1');
            worst = std::max<std::uint64_t>(worst, potion.empty() ? 9999 : potion[0] - low);
        }
        check(first == cases && worst <= 10,
              "HP read low: the potion is the next key, in all " + std::to_string(cases) +
                  " phases -- " + std::to_string(first) + ", at most " + std::to_string(worst) +
                  " ms after the reading");
    }
    {
        // The reading comes in while a key of the pass is down: the next key
        // of that pass waits for the potion.
        Rig rig({ timer_key('Z', 5000, "Skill 1"), timer_key('X', 5000, "Skill 2"), hp_key('1', 50) });
        rig.press_takes = 40;
        bool dropped = false;
        rig.on_press = [&](std::uint32_t vk) {
            if (vk != 'Z' || dropped) return;
            dropped = true;
            rig.hp = 30.0f;
            rig.engine.set_vitals(rig.hp, rig.mp);
        };
        rig.run(10);
        check(rig.pressed.size() == 3 && rig.pressed[0].first == 'Z' && rig.pressed[1].first == '1' &&
                  rig.pressed[2].first == 'X',
              "HP read low while Z is down: the potion goes next, before X of the same pass");
    }
    {
        // HP stays under the mark; the potion's cooldown, jittered, runs out
        // between the skills' presses. Nothing else may start between the
        // moment it is due and its press.
        std::vector<MacroKey> keys = { timer_key('Z', 1200, "Z"), timer_key('X', 3000, "X"),
                                       timer_key('C', 600, "C"), timer_key('V', 900, "V"),
                                       hp_key('1', 60, 15) };
        keys[1].start_delay_ms = 150;
        keys[2].start_delay_ms = 300;
        keys[3].start_delay_ms = 450;
        Rig rig(keys);
        rig.press_takes = 40;
        rig.hp = 40.0f;
        std::uint64_t due = 0;
        std::size_t seen = 0;
        int windows = 0, others = 0, early = 0;
        std::uint64_t worst = 0;
        for (const std::uint64_t until = rig.t + 60'000; rig.t < until; rig.t += 10) {
            rig.engine.set_vitals(rig.hp, rig.mp);
            rig.engine.step();
            for (; seen < rig.pressed.size(); ++seen) {
                const auto p = rig.pressed[seen];
                if (p.first != '1') continue;
                if (due != 0) {
                    ++windows;
                    if (p.second < due) {
                        ++early;
                    } else {
                        worst = std::max<std::uint64_t>(worst, p.second - due);
                        for (const auto& q : rig.pressed)
                            if (q.first != '1' && q.second >= due && q.second < p.second) ++others;
                    }
                }
                const auto next = rig.engine.next_due_ms();
                due = next.size() > 4 ? rig.t + next[4] : 0;
            }
        }
        check(windows >= 15 && others == 0 && early == 0 && worst <= 50,
              "its cooldown running out between the skills' presses: " + std::to_string(windows) +
                  " drinks, each at most " + std::to_string(worst) +
                  " ms after it was due -- a key down then -- and " + std::to_string(others) +
                  " keys started in between");
    }
}

/// BẮT ĐẦU with HP and MP already under their marks: the player's eleven
/// keys as the simple screen laid them out, each 150 ms after the last --
/// which put the potions 900 and 1050 ms in.
void test_potion_at_start() {
    section("Potions: under the mark at BẮT ĐẦU, first -- their start delay is not waited out");

    std::vector<MacroKey> keys = {
        timer_key('Z', 1200, "Skill 1"), timer_key('X', 3000, "Skill 2"),
        timer_key('C', 6000, "Skill 3"), timer_key('V', 9000, "Skill 4"),
        timer_key('A', 300'000, "Buff 1"), timer_key('S', 300'000, "Buff 2"),
        hp_key('1', 60), mp_key('2', 58),
        timer_key('B', 3000, "Skill 5"), timer_key('N', 3000, "Skill 6"),
        timer_key('F', 300'000, "Buff 3"),
    };
    const std::uint32_t delays[] = { 0, 150, 300, 450, 600, 750, 900, 1050, 1200, 1500, 1500 };
    for (std::size_t i = 0; i < keys.size(); ++i) {
        keys[i].start_delay_ms = delays[i];
        keys[i].jitter_pct = 0;
    }
    Rig rig(keys);
    rig.press_takes = 40;
    rig.hp = 40.0f;
    rig.mp = 40.0f;
    rig.run(1300);
    const bool order = rig.pressed.size() >= 3 && rig.pressed[0].first == '1' &&
                       rig.pressed[1].first == '2' && rig.pressed[2].first == 'Z';
    std::string got;
    for (std::size_t i = 0; i < rig.pressed.size() && i < 4; ++i)
        got += (got.empty() ? "" : ", ") + std::string(1, static_cast<char>(rig.pressed[i].first)) +
               "@" + std::to_string(rig.pressed[i].second - rig.start);
    check(order && rig.pressed[0].second == rig.start &&
              rig.pressed[1].second >= rig.pressed[0].second + 40 + MacroEngine::kPotionGapMs,
          "started at HP 40% and MP 40% (marks 60% and 58%): the HP potion first, at once, the MP "
          "potion 20 ms after it came up, then the skills -- " + got);

    // And on every cooldown after: the MP potion 20 ms after the HP one came
    // up -- or as soon after as the key of a buff or skill already down
    // allows -- not a whole 100-ms look behind it, as when both kinds were
    // looked at together only that often.
    rig.run(9000);
    const auto hp = rig.when('1');
    const auto mp = rig.when('2');
    bool in_step = hp.size() == 4 && mp.size() == 4;
    for (std::size_t i = 0; in_step && i < hp.size(); ++i)
        in_step = mp[i] >= hp[i] + 40 + MacroEngine::kPotionGapMs && mp[i] <= hp[i] + 40 + 70;
    check(in_step, "and every 3 s after it, the MP potion 20 ms after the HP one came up, a key "
                   "already down allowing -- HP at +" + ms_list(hp, rig.start) + ", MP at +" +
                       ms_list(mp, rig.start) + " ms");

    // With no potion due at BẮT ĐẦU, the same file starts as it always did:
    // each key at its start delay, the buffs kBuffGapMs apart (as the engine
    // before the tiers armed them), and no potion.
    Rig high(keys);
    high.run(3100);
    const auto first = [&](std::uint32_t vk) {
        const auto at = high.when(vk);
        return at.empty() ? std::uint64_t{ 99'999 } : at[0] - high.start;
    };
    const bool as_before = first('Z') == 0 && first('X') == 150 && first('C') == 300 &&
                           first('V') == 450 && first('A') == 600 && first('B') == 1200 &&
                           first('N') == 1500 && first('S') == 1800 && first('F') == 3000 &&
                           high.count('1') == 0 && high.count('2') == 0;
    check(as_before, "HP and MP high at BẮT ĐẦU: every key at its start delay as before -- Z +" +
                         std::to_string(first('Z')) + ", X +" + std::to_string(first('X')) + ", A +" +
                         std::to_string(first('A')) + ", S +" + std::to_string(first('S')) + ", F +" +
                         std::to_string(first('F')) + " ms -- and no potion");
}

/// The player's eleven keys, one HP and one MP potion among them, no jitter.
std::vector<MacroKey> player_keys() {
    std::vector<MacroKey> k = {
        timer_key('Z', 1200, "Skill 1"), timer_key('X', 3000, "Skill 2"),
        timer_key('C', 6000, "Skill 3"), timer_key('V', 9000, "Skill 4"),
        timer_key('A', 300'000, "Buff 1"), timer_key('S', 300'000, "Buff 2"),
        hp_key('1', 60), mp_key('2', 58),
        timer_key('B', 3000, "Skill 5"), timer_key('N', 3000, "Skill 6"),
        timer_key('F', 300'000, "Buff 3"),
    };
    for (MacroKey& m : k) m.jitter_pct = 0;
    return k;
}

void test_potion_keys_change() {
    section("Potions: keys taken out, added or shared while the macro runs");

    {
        // Z, ahead of the potions, cleared mid-run: everything after it moves
        // up one place. By place, the HP potion took on the buff's due time
        // and did not drink for minutes with HP low.
        Rig rig(player_keys());
        rig.hp = 40.0f;
        rig.run(10'000);
        const std::size_t before = rig.count('1');
        MacroConfig cfg = rig.engine.config();
        cfg.keys.erase(cfg.keys.begin());
        rig.engine.set_config(cfg);
        const std::uint64_t at = rig.t;
        rig.run(10'000);
        std::vector<std::uint64_t> after;
        for (const std::uint64_t w : rig.when('1'))
            if (w >= at) after.push_back(w);
        const auto due = rig.engine.next_due_ms();
        const auto st = rig.engine.stats();
        check(before == 4 && after.size() == 3 && after[0] - at <= 2010,
              "a skill in front of the HP potion taken out mid-run, HP low: the potion goes on "
              "drinking every 3 s -- at +" + ms_list(after, at) + " ms");
        check(due.size() == 10 && due[5] <= 3000 && st.key_presses.size() >= 6 &&
                  st.key_presses[5] == rig.count('1'),
              "its due time and its tally moved with it");
    }
    {
        // HP under both tiers, Lv2 at the end; skill B, in front of it, taken
        // out. By place, Lv2 took on a buff's due time, and Lv1 waited on it.
        std::vector<MacroKey> keys = player_keys();
        keys.push_back(hp_key(VK_F1, 30));
        keys.push_back(mp_key(VK_F2, 20));
        Rig rig(keys);
        rig.hp = 25.0f;
        rig.run(10'000);
        const std::size_t lv2_before = rig.count(VK_F1);
        MacroConfig cfg = rig.engine.config();
        cfg.keys.erase(cfg.keys.begin() + 8);
        rig.engine.set_config(cfg);
        const std::uint64_t at = rig.t;
        rig.run(10'000);
        check(lv2_before == 4 && rig.count(VK_F1, at) == 3 && rig.count('1') == 0,
              "with Lv2 at 30% and HP 25%: a skill in front of Lv2 taken out, Lv2 goes on every 3 s "
              "and Lv1 never -- " + std::to_string(rig.count(VK_F1, at)) + " drinks in 10 s");
    }
    {
        // A key added mid-run: a potion due at once, a skill at its start
        // delay.
        Rig rig({ timer_key('Z', 5000, "Skill 1") });
        rig.hp = 40.0f;
        rig.run(1000);
        MacroConfig cfg = rig.engine.config();
        MacroKey x = timer_key('X', 5000, "Skill 2");
        x.start_delay_ms = 300;
        cfg.keys.push_back(x);
        cfg.keys.push_back(hp_key('1', 50));
        rig.engine.set_config(cfg);
        const std::uint64_t at = rig.t;
        rig.run(1000);
        const auto x_at = rig.when('X');
        const auto p = rig.when('1');
        check(p.size() == 1 && p[0] == at && x_at.size() == 1 && x_at[0] - at >= 300 &&
                  x_at[0] - at <= 310,
              "keys added mid-run: the potion drinks at once, the skill at its start delay");
    }
    {
        // Lv1 and Lv2 on the same key, HP crossing between their marks: one
        // potion in the game, one cooldown. Each its own, Lv2's drink at 25%
        // left Lv1 free to press the key again 200 ms later at 40%.
        Rig rig({ hp_key('1', 50), hp_key('1', 30) });
        rig.run(500);
        rig.hp = 25.0f;
        const std::uint64_t low = rig.t;
        rig.run(200);
        rig.hp = 40.0f;
        rig.run(2700);
        const std::size_t within = rig.count('1', low);
        rig.run(400);
        std::vector<std::uint64_t> all;
        for (const std::uint64_t w : rig.when('1'))
            if (w >= low) all.push_back(w);
        check(within == 1 && all.size() == 2 && all[1] - all[0] == 3000,
              "Lv1 and Lv2 both on '1', HP 25% then 40%: the key once per cooldown -- at +" +
                  ms_list(all, low) + " ms");
    }
    {
        // A Lv2 put on Lv1's key just after Lv1 drank: the same potion, so it
        // waits out that cooldown too.
        Rig rig({ hp_key('1', 50) });
        rig.hp = 40.0f;
        rig.run(500);
        MacroConfig cfg = rig.engine.config();
        cfg.keys.push_back(hp_key('1', 30));
        rig.engine.set_config(cfg);
        rig.hp = 25.0f;
        rig.run(2000);
        const std::size_t soon = rig.count('1');
        rig.run(1000);
        check(soon == 1 && rig.count('1') == 2 && rig.when('1')[1] - rig.when('1')[0] == 3000,
              "a Lv2 added on Lv1's key just after Lv1 drank waits out its cooldown");
    }
    {
        // The game loses focus while the potion's key is down: the key-up is
        // refused and the key stays down. Let go of, and counted as drunk --
        // not drunk again the moment the game is back in front.
        Rig rig({ hp_key('1', 50) });
        rig.sticks = '1';
        rig.hp = 40.0f;
        rig.run(2000);
        const bool once = rig.tries('1').size() == 1 && rig.releases == 1 && !rig.stuck &&
                          rig.engine.stats().key_presses.size() == 1 &&
                          rig.engine.stats().key_presses[0] == 1;
        rig.sticks = 0;
        rig.run(1500);
        const auto tries = rig.tries('1');
        check(once && tries.size() == 2 && tries[1] - tries[0] == 3000,
              "a potion key left down by the game losing focus: let go of, taken as drunk, and "
              "pressed next on its cooldown, not at once");
    }
}

/// A key taken out while the timer pass is in the middle of pressing another:
/// the GUI calls set_config on its own thread, and the timer thread holds no
/// lock while a key is down -- which is where the pretend keyboard calls it
/// from here. The pass, working from the keys as they were when it began,
/// read and wrote each key's times by where it had stood: a buff read the HP
/// potion's due time and went again, and wrote its 300-s interval onto the
/// potion, which then did not drink for five minutes with HP low.
void test_keys_change_mid_press() {
    section("Keys taken out while another is being pressed: each keeps its own times");

    {
        // Z taken out during its own press.
        Rig rig({ timer_key('Z', 1000, "Skill 1"), timer_key('A', 300'000, "Buff 1"), hp_key('1', 50) });
        rig.press_takes = 40;
        rig.run(5000);
        const std::size_t buff_before = rig.count('A');
        MacroConfig gone = rig.engine.config();
        gone.keys.erase(gone.keys.begin());
        bool done = false;
        rig.on_press = [&](std::uint32_t vk) {
            if (vk != 'Z' || done) return;
            done = true;
            rig.engine.set_config(gone);
        };
        const std::uint64_t at = rig.t;
        rig.run(1200);
        const std::size_t buff_again = rig.count('A', at);
        rig.hp = 30.0f;
        const std::uint64_t low = rig.t;
        rig.run(20'000);
        const auto potion = rig.when('1');
        const auto due = rig.engine.next_due_ms();
        check(done && buff_before == 1 && buff_again == 0,
              "Z taken out while it is down: the buff is not pressed again -- " +
                  std::to_string(buff_again) + " more time(s)");
        check(!potion.empty() && potion[0] - low <= 100 && rig.count('1', low) == 7 && due.size() == 2 &&
                  due[1] <= 3000,
              "and with HP low after it the potion drinks at once and every 3 s -- " +
                  std::to_string(rig.count('1', low)) + " drinks in 20 s, the first at +" +
                  (potion.empty() ? std::string("never") : std::to_string(potion[0] - low)) + " ms");
    }
    {
        // The GUI's own order: "Xóa" on skill V empties its box and saves;
        // a frame later drop_empty_keys takes it out and saves again -- and
        // that lands while skill Z, or X, is being pressed.
        std::vector<MacroKey> keys = { timer_key('Z', 1200, "Z"), timer_key('X', 3000, "X"),
                                       timer_key('V', 9000, "V"), timer_key('A', 300'000, "A"),
                                       timer_key('S', 300'000, "S"), hp_key('1', 50), mp_key('2', 30) };
        const std::uint32_t delays[] = { 0, 150, 300, 600, 750, 0, 0 };
        for (std::size_t i = 0; i < keys.size(); ++i) {
            keys[i].start_delay_ms = delays[i];
            keys[i].jitter_pct = 0;
        }
        Rig rig(keys);
        rig.press_takes = 40;
        rig.run(10'000);
        MacroConfig emptied = rig.engine.config();
        emptied.keys[2].keycode = 0;
        emptied.keys[2].enabled = false;
        rig.engine.set_config(emptied);
        rig.run(20);
        MacroConfig dropped = emptied;
        dropped.keys.erase(dropped.keys.begin() + 2);
        bool done = false;
        rig.on_press = [&](std::uint32_t vk) {
            if ((vk != 'Z' && vk != 'X') || done) return;
            done = true;
            rig.engine.set_config(dropped);
        };
        const std::uint64_t from = rig.t;
        rig.run(3000);
        const std::size_t again = rig.count('A', from) + rig.count('S', from);
        rig.hp = 30.0f;
        rig.mp = 20.0f;
        const std::uint64_t low = rig.t;
        rig.run(20'000);
        const auto hp = rig.when('1');
        const auto mp = rig.when('2');
        check(done && again == 0, "V emptied, then taken out while Z is down: no buff pressed twice -- " +
                                      std::to_string(again) + " more");
        check(!hp.empty() && hp[0] - low <= 100 && rig.count('1', low) == 7 && !mp.empty() &&
                  mp[0] - low <= 200 && rig.count('2', low) == 7,
              "then HP and MP low: both drink at once and every 3 s -- HP " +
                  std::to_string(rig.count('1', low)) + ", MP " + std::to_string(rig.count('2', low)) +
                  " in 20 s");
        // Z and X kept their own cooldowns: none of them on another key.
        // A key's next time is armed from the start of the pass that pressed
        // it, and a potion or two pressed first in that pass makes it go up
        // to 100 ms late: 200 ms of room, against a cooldown on the wrong key
        // -- 1.2 s on X, or Z due again at once.
        bool spaced = true;
        std::string short_gap;
        for (const std::uint32_t vk : { std::uint32_t{ 'Z' }, std::uint32_t{ 'X' } }) {
            const auto at = rig.when(vk);
            const std::uint64_t cd = vk == 'Z' ? 1200 : 3000;
            for (std::size_t i = 1; i < at.size(); ++i)
                if (at[i] - at[i - 1] + 200 < cd) {
                    spaced = false;
                    if (short_gap.empty())
                        short_gap = std::string(" (") + static_cast<char>(vk) + " at +" +
                                    std::to_string(at[i - 1] - rig.start) + " and +" +
                                    std::to_string(at[i] - rig.start) + " ms)";
                }
        }
        check(spaced, "and the skills each on their own cooldown throughout: Z every 1.2 s, X every 3 s" +
                          short_gap);
    }
    {
        // A key in front taken out while the HP potion itself is down: the
        // potion's cooldown lands on the potion, and on no other key.
        Rig rig({ timer_key('Z', 5000, "Skill 1"), timer_key('A', 300'000, "Buff 1"), hp_key('1', 50) });
        rig.press_takes = 40;
        rig.run(1000);
        MacroConfig gone = rig.engine.config();
        gone.keys.erase(gone.keys.begin());
        bool done = false;
        rig.on_press = [&](std::uint32_t vk) {
            if (vk != '1' || done) return;
            done = true;
            rig.engine.set_config(gone);
        };
        rig.hp = 30.0f;
        const std::uint64_t low = rig.t;
        rig.run(10'000);
        const auto potion = rig.when('1');
        bool every_3s = potion.size() == 4;
        for (std::size_t i = 1; every_3s && i < potion.size(); ++i)
            every_3s = potion[i] - potion[i - 1] == 3000;
        check(done && every_3s && rig.count('A', low) == 0,
              "Z taken out while the HP potion is down: the potion drinks every 3 s after it, the buff "
              "not again -- at +" + ms_list(potion, low) + " ms");
    }
}

/// The macro's two threads, stood in for on a clock of the test's own, a
/// millisecond at a time -- where the timer thread and the sequence's thread
/// each wake, every turn of theirs is taken, by the engine's own code
/// (MacroEngine::step_timers, step_sequence): the timer thread 10 ms after
/// its last pass ended, as its loop sleeps first; the sequence's whenever its
/// own wait is over, from the moment the macro starts. While one of them is
/// inside a press -- the pretend keyboard holds each key for its hold, or for
/// `press_ms` when it has none -- or the timer pass waits between the HP and
/// the MP potion, the clock goes on and the other takes its turns as it would
/// on its own thread: a press of its own then finds the keys taken and is
/// refused, as on the real threads. Where both are due at one moment the
/// timers go first, which the real threads leave to chance. What the checks
/// count is the test's time, never how much of a loaded CPU a thread got --
/// the readings are handed over every millisecond.
struct Threads {
    InputManager         input;
    ExecutionCoordinator coord;
    MacroEngine          engine{ input, coord };
    std::uint64_t        t = 9'000'000;
    std::uint64_t        timer_wake = 0;
    bool                 in_timer = false, in_seq = false;
    float                hp = 100.0f, mp = 100.0f;
    std::uint64_t        press_ms = 40;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> got;

    explicit Threads(const MacroConfig& cfg) {
        MacroEngine::Clock c;
        c.now_ms = [this] { return t; };
        c.sleep_ms = [this](std::uint32_t ms) { pass(ms); };
        engine.set_clock(c);
        MacroEngine::Hands h;
        h.focused = [] { return true; };
        h.key = [this](std::uint32_t vk, int hold) {
            got.emplace_back(vk, t);
            pass(hold > 0 ? static_cast<std::uint64_t>(hold) : press_ms);
            return true;
        };
        engine.set_hands(h);
        engine.set_config(cfg);
    }
    ~Threads() { engine.stop(); }

    /// BẮT ĐẦU: the timer thread sleeps its first 10 ms, the sequence's takes
    /// its first turn at once.
    void start() {
        engine.set_vitals(hp, mp);
        engine.start_stepped();
        timer_wake = t + 10;
        wake();
    }
    /// Whichever of the two is due now and not inside a press of its own.
    void wake() {
        if (!in_timer && t >= timer_wake) {
            in_timer = true;
            engine.step_timers();
            in_timer = false;
            timer_wake = t + 10;
        }
        if (!in_seq) {
            in_seq = true;
            engine.step_sequence();
            in_seq = false;
        }
    }
    /// `ms` of the clock, a millisecond at a time, with the readings as they
    /// are now handed over at each.
    void pass(std::uint64_t ms) {
        for (const std::uint64_t until = t + ms; t < until;) {
            ++t;
            engine.set_vitals(hp, mp);
            wake();
        }
    }
    /// When `vk` first went down at `from` or after; 0 for never.
    std::uint64_t first(std::uint32_t vk, std::uint64_t from) const {
        for (const auto& p : got)
            if (p.first == vk && p.second >= from) return p.second;
        return 0;
    }
    /// Steps of the sequence -- any key but the potions -- that went down in
    /// [from, to).
    int steps_between(std::uint64_t from, std::uint64_t to) const {
        int n = 0;
        for (const auto& p : got)
            if (p.first != '1' && p.first != '2' && p.second >= from && p.second < to) ++n;
        return n;
    }
};

/// The key sequence through a hold, the two threads' turns on the test's
/// clock: the potion that came due under the hold goes first once it is over.
void test_potion_after_hold_threaded() {
    section("Potions: after a hold, before the key sequence's next step (the two threads' turns)");

    const int runs = 4;
    int good = 0;
    std::string bad;
    for (int run = 0; run < runs; ++run) {
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys = { hp_key('1', 50) };
        cfg.sequence = { MacroStep{ 'Z', 0, 120, "Z" }, MacroStep{ 'X', 0, 120, "X" },
                         MacroStep{ 'C', 0, 120, "C" } };
        cfg.sequence_enabled = true;
        cfg.require_focus = true;
        Threads th(cfg);
        th.press_ms = 25;
        th.hp = 90.0f;
        th.mp = 90.0f;
        th.start();
        th.pass(300);
        th.coord.engage_critical("đóng thông báo");
        const std::uint64_t held = th.t;
        th.hp = 30.0f;
        th.pass(1000 + static_cast<std::uint64_t>(run) * 37);
        // Taken before the hold ends: nothing goes out under it, so every key
        // from here on is after it.
        const std::uint64_t over = th.t;
        th.coord.disengage_critical();
        th.pass(400);

        std::uint32_t first = 0;
        std::uint64_t potion = 0;
        bool under = false;
        for (const auto& p : th.got) {
            if (p.second < over) {
                under = under || p.second >= held;
                continue;
            }
            if (first == 0) first = p.first;
            if (p.first == '1' && potion == 0) potion = p.second;
        }
        if (first == '1' && potion - over <= 100 && !under) {
            ++good;
        } else if (bad.empty()) {
            bad = std::string(" (first after the hold: ") +
                  (first ? std::string(1, static_cast<char>(first)) : std::string("none")) +
                  (under ? ", and a key under it" : "") + ")";
        }
    }
    check(good == runs, "a hold of a second over HP low, the sequence on: nothing under the hold, and the potion "
                        "is the first key after it, within 100 ms of the test's clock, in " +
                            std::to_string(good) + " of " + std::to_string(runs) + " runs" + bad);
}

/// The sequence on its own thread, on the real clock: a step that comes up
/// while a potion is due waits for it, a moment at most.
void test_sequence_waits_for_potion() {
    section("Potions: the key sequence waits for a potion that is due");

    InputManager input;
    ExecutionCoordinator coord;
    MacroEngine engine(input, coord);
    std::mutex m;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> tried;
    MacroEngine::Hands h;
    h.focused = [] { return true; };
    h.key = [&](std::uint32_t vk, int) {
        std::lock_guard<std::mutex> lock(m);
        tried.emplace_back(vk, now_ms());
        return vk != '1';       // the potion never goes out
    };
    engine.set_hands(h);

    MacroConfig cfg;
    cfg.apply_defaults();
    cfg.keys = { hp_key('1', 50) };
    cfg.sequence = { MacroStep{ 'Z', 0, 100, "Skill 1" } };
    cfg.sequence_enabled = false;
    cfg.require_focus = true;
    engine.set_config(cfg);
    engine.start();
    engine.set_vitals(40.0f, 80.0f);

    const auto first = [&](std::uint32_t vk) {
        std::lock_guard<std::mutex> lock(m);
        for (const auto& p : tried)
            if (p.first == vk) return p.second;
        return std::uint64_t{ 0 };
    };
    // The potion refused once, then the sequence switched on.
    std::uint64_t potion = 0;
    for (int i = 0; i < 200 && potion == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        potion = first('1');
    }
    cfg.sequence_enabled = true;
    engine.set_config(cfg);
    std::uint64_t z = 0;
    for (int i = 0; i < 300 && z == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        engine.set_vitals(40.0f, 80.0f);
        z = first('Z');
    }
    engine.stop();
    check(potion != 0 && z != 0 && z >= potion + MacroEngine::kPotionFirstMs - 20,
          "the sequence's step waits while the potion is due, then goes: " +
              (potion && z ? std::to_string(z - potion) : std::string("?")) +
              " ms after the potion was first tried");
}

/// The engine on its own two threads and the real clock, as BẮT ĐẦU runs it:
/// a pretend keyboard that holds each key for its hold -- 40 ms when it has
/// none -- and writes down what went down, and the readings handed over
/// every 5 ms from the test's thread, as the GUI hands them over each frame.
/// Only for whether the threads press at all: what goes before what is
/// counted on the test's clock (Threads), as a loaded CPU may hold either
/// thread back for a tenth of a second or more.
struct Live {
    InputManager         input;
    ExecutionCoordinator coord;
    MacroEngine          engine{ input, coord };
    std::mutex           m;
    std::vector<std::uint32_t> got;

    explicit Live(const MacroConfig& cfg) {
        MacroEngine::Hands h;
        h.focused = [] { return true; };
        h.key = [this](std::uint32_t vk, int hold) {
            {
                std::lock_guard<std::mutex> lock(m);
                got.push_back(vk);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(hold > 0 ? hold : 40));
            return true;
        };
        engine.set_hands(h);
        engine.set_config(cfg);
    }
    ~Live() { engine.stop(); }

    /// Whether `vk` has gone down.
    bool pressed(std::uint32_t vk) {
        std::lock_guard<std::mutex> lock(m);
        return std::find(got.begin(), got.end(), vk) != got.end();
    }
};

/// A key sequence that steps every few ms: a step pressed in any gap the
/// potions leave is all but sure to be seen.
std::vector<MacroStep> quick_sequence() {
    return { MacroStep{ 'Z', 5, 10, "Z" }, MacroStep{ 'X', 5, 10, "X" }, MacroStep{ 'C', 5, 10, "C" } };
}

/// The potions before the key sequence, the turns of the two threads BẮT ĐẦU
/// starts taken on the test's clock (Threads): at the start, between the HP
/// and the MP potion, and between a reading and the pass that acts on it --
/// and, on the real threads, that both press at all.
void test_potions_before_sequence_threads() {
    section("Potions: before the key sequence's steps, the two threads' turns on the test's clock");

    {
        // BẮT ĐẦU with HP and MP under their marks, on the player's layout --
        // every skill key off, the sequence Z, X, C on. The sequence's thread
        // starts with the timers' and pressed Z before the first look at the
        // potions. Each run with keys held down a different while.
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys = player_keys();
        const std::uint32_t delays[] = { 0, 150, 300, 450, 600, 750, 900, 1050, 1200, 1500, 1500 };
        for (std::size_t i = 0; i < cfg.keys.size(); ++i) {
            cfg.keys[i].start_delay_ms = delays[i];
            if (macro_key_kind(cfg.keys[i]) == MacroKeyKind::Skill) cfg.keys[i].enabled = false;
        }
        cfg.sequence = { MacroStep{ 'Z', 0, 250, "Skill 1" }, MacroStep{ 'X', 0, 400, "Skill 2" },
                         MacroStep{ 'C', 0, 600, "Skill 3" } };
        cfg.sequence_enabled = true;
        cfg.require_focus = true;
        cfg.global_jitter_pct = 0;
        const std::uint64_t holds[] = { 40, 25, 33, 50, 61 };
        int good = 0;
        std::string bad;
        for (const std::uint64_t hold : holds) {
            Threads th(cfg);
            th.press_ms = hold;
            th.hp = 40.0f;
            th.mp = 40.0f;
            const std::uint64_t t0 = th.t;
            th.start();
            th.pass(400);
            const auto& got = th.got;
            std::string order;
            for (std::size_t i = 0; i < got.size() && i < 4; ++i)
                order += (order.empty() ? "" : ", ") + std::string(1, static_cast<char>(got[i].first)) + "@" +
                         std::to_string(got[i].second - t0);
            const bool ok = got.size() >= 3 && got[0].first == '1' && got[1].first == '2' &&
                            got[2].first == 'Z' &&
                            got[1].second >= got[0].second + hold + MacroEngine::kPotionGapMs;
            if (ok) ++good;
            else if (bad.empty()) bad = " (held " + std::to_string(hold) + " ms: " + order + ")";
        }
        check(good == static_cast<int>(std::size(holds)),
              "BẮT ĐẦU at HP 40% and MP 40%, the sequence on: the HP potion, the MP potion 20 ms after it came "
              "up, then Z -- with keys held 25 to 61 ms, in " + std::to_string(good) + " of " +
                  std::to_string(std::size(holds)) + " runs" + bad);
    }
    {
        // HP and MP read low together while the sequence steps: nothing
        // between the HP potion and the MP one, which waits 20 ms after the
        // HP key comes up -- whatever step the sequence is at, whatever ms of
        // it the readings come in.
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys = { hp_key('1', 60), mp_key('2', 58) };
        cfg.sequence = quick_sequence();
        cfg.sequence_enabled = true;
        cfg.require_focus = true;
        const int runs = 12;
        int between = 0, missing = 0;
        std::string example;
        for (int run = 0; run < runs; ++run) {
            Threads th(cfg);
            th.hp = 90.0f;
            th.mp = 90.0f;
            th.start();
            th.pass(150 + static_cast<std::uint64_t>(run * 23 % 120));
            th.hp = 40.0f;
            th.mp = 40.0f;
            th.engine.set_vitals(th.hp, th.mp);
            const std::uint64_t low = th.t;
            th.pass(300);
            const std::uint64_t hp = th.first('1', low), mp = th.first('2', low);
            if (hp == 0 || mp == 0) {
                ++missing;
                continue;
            }
            const int n = th.steps_between(hp + 1, mp);
            if (n > 0) {
                ++between;
                if (example.empty())
                    example = " (e.g. HP at +" + std::to_string(hp - low) + ", MP at +" +
                              std::to_string(mp - low) + " ms, " + std::to_string(n) + " step(s) between)";
            }
        }
        check(missing == 0 && between == 0,
              "HP and MP low together, the sequence stepping every few ms: no step between the HP and the "
              "MP potion in " + std::to_string(runs - between - missing) + " of " + std::to_string(runs) +
                  " runs" + example);
    }
    {
        // HP read low at a moment of no one's choosing: the sequence steps
        // every few ms, and the timer pass comes within 10 ms. A step went in
        // between, the keys were held, and the potion waited for them.
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys = { hp_key('1', 60) };
        cfg.sequence = quick_sequence();
        cfg.sequence_enabled = true;
        cfg.require_focus = true;
        const int runs = 12;
        int before = 0, missing = 0;
        std::uint64_t worst = 0;
        std::string example;
        for (int run = 0; run < runs; ++run) {
            Threads th(cfg);
            th.hp = 90.0f;
            th.mp = 90.0f;
            th.start();
            th.pass(150 + static_cast<std::uint64_t>(run * 29 % 120));
            th.hp = 40.0f;
            th.engine.set_vitals(th.hp, th.mp);
            const std::uint64_t low = th.t;
            th.pass(300);
            const std::uint64_t hp = th.first('1', low);
            if (hp == 0) {
                ++missing;
                continue;
            }
            worst = std::max<std::uint64_t>(worst, hp - low);
            // From 2 ms on: a step whose thread looked at the potions just
            // before the reading came in may go down a moment after it.
            const int n = th.steps_between(low + 2, hp);
            if (n > 0) {
                ++before;
                if (example.empty())
                    example = " (e.g. the potion at +" + std::to_string(hp - low) + " ms, " +
                              std::to_string(n) + " step(s) before it)";
            }
        }
        check(missing == 0 && before == 0 && worst <= 100,
              "HP read low while the sequence steps: no step between the reading and the potion in " +
                  std::to_string(runs - before - missing) + " of " + std::to_string(runs) +
                  " runs, the potion at most " + std::to_string(worst) + " ms after the reading" + example);
    }
    {
        // And on the real threads, as BẮT ĐẦU starts them: HP and MP low, the
        // sequence on -- both potions and a step of the sequence go out,
        // waited for as long as a loaded machine needs.
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys = { hp_key('1', 60), mp_key('2', 58) };
        cfg.sequence = quick_sequence();
        cfg.sequence_enabled = true;
        cfg.require_focus = true;
        Live live(cfg);
        live.engine.set_vitals(40.0f, 40.0f);
        live.engine.start();
        bool all = false;
        for (int i = 0; i < 1000 && !all; ++i) {
            live.engine.set_vitals(40.0f, 40.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            all = live.pressed('1') && live.pressed('2') &&
                  (live.pressed('Z') || live.pressed('X') || live.pressed('C'));
        }
        live.engine.stop();
        check(all, "and on the macro's own threads, on the real clock, both potions and the sequence press");
    }
}

// ------------------------------------------------------ Quái đang thấy

void test_monster_sightings() {
    section("Monster lists: what has been seen lately (Quái đang thấy)");

    MonsterSightings seen;
    const std::uint64_t t0 = 1'000'000;
    seen.saw("NgaQuy", t0);
    seen.saw("NgaQuv", t0 + 700);
    seen.saw("NgaQuy", t0 + 1400);
    seen.saw("", t0 + 2100);
    seen.saw("HoLy", t0 + 2900);
    auto names = seen.names(t0 + 3000);
    check(names.size() == 2 && names[0] == "NgaQuy" && names[1] == "HoLy",
          "'NgaQuy', 'NgaQuv', 'NgaQuy', '' and 'HoLy' over 3 s: two names, NgaQuy first");
    names = seen.names(t0 + 10'000);
    check(names.size() == 2, "both still listed 10 s after the first sighting");
    names = seen.names(t0 + 1400 + 10'001);
    check(names.size() == 1 && names[0] == "HoLy", "NgaQuy gone once not seen for over 10 s");
    seen.saw("Moc", t0 + 20'000);
    names = seen.names(t0 + 20'000);
    check(names.size() == 1 && names[0] == "Moc",
          "and HoLy gone too, ten seconds after its last read: only Moc, just seen");

    // From plates: players and nameless plates do not count.
    MonsterSightings plates;
    const auto plate = [](int x, const std::string& name, bool player) {
        Nameplate n;
        n.bar = ScreenRect{ x - 20, 400, 40, 3 };
        n.plate = ScreenRect{ x - 30, 386, 60, 12 };
        n.name = name;
        n.is_player = player;
        return n;
    };
    plates.saw(std::vector<Nameplate>{ plate(640, "Me", true), plate(700, "", false),
                                       plate(760, "Moc", false) },
               t0);
    names = plates.names(t0);
    check(names.size() == 1 && names[0] == "Moc", "from a frame: monsters with a name only");

    // Spelt the way it was read most often.
    MonsterSightings spelt;
    spelt.saw("NgaQuv", t0);
    spelt.saw("NgaQuy", t0 + 100);
    spelt.saw("NgaQuy", t0 + 200);
    names = spelt.names(t0 + 300);
    check(names.size() == 1 && names[0] == "NgaQuy",
          "one monster read two ways is listed once, as it was read most often");

    // Twenty kinds, the n-th seen n times. Three letters each, so no two can
    // be taken for one another.
    MonsterSightings lots;
    for (int i = 0; i < 20; ++i) {
        const std::string name = "Q" + std::string(2, static_cast<char>('A' + i));
        for (int k = 0; k <= i; ++k) lots.saw(name, t0);
    }
    names = lots.names(t0);
    check(names.size() == MonsterSightings::kMaxShown && names[0] == "QTT" && names[11] == "QII",
          "at most 12, the most seen first");

    // A reader that misspells a name a new way every time.
    MonsterSightings messy;
    for (int i = 0; i < 70; ++i) messy.saw("Quai" + std::to_string(1000 + i * 37), t0 + i);
    check(messy.spellings() <= MonsterSightings::kMaxSpellings &&
              messy.names(t0 + 70).size() <= MonsterSightings::kMaxShown,
          "70 different misspellings: what is kept stays bounded -- " +
              std::to_string(messy.spellings()) + " spellings kept");
}

} // namespace

void test_macro_fight(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_potion_tiers_config();
    test_potion_tiers_engine();
    test_potions_first();
    test_potion_the_moment_due();
    test_potion_at_start();
    test_potion_keys_change();
    test_keys_change_mid_press();
    test_potion_after_hold_threaded();
    test_sequence_waits_for_potion();
    test_potions_before_sequence_threads();
    test_monster_sightings();
}
