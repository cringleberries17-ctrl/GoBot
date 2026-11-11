// gobot_test -- the skills paced on your casts (MacroConfig::cast_paced,
// 2026.11.08, W1): one round in the order of the keys, each skill pressed only
// once the game has cast the one before and its lock is over; a press the game
// dropped pressed once more, a key on its cooldown skipped and learned; the
// timers when your casts stop coming, and the round again when they come back;
// potions first and buffs between; and with the switch off, the keys exactly as
// before.
//
// Nothing here goes near the game, the network or the desktop. The engine is
// driven a step at a time on a clock of the test's own, with a pretend keyboard
// and a pretend game: a key-down it takes -- outside the last cast's lock and
// the key's own cooldown -- is a cast, stamped latency_ms after the key went
// down and handed to the engine delivery_ms after that, as the capture thread
// hands your casts in (GuiApp::more_on_packets). The lock and the delays are of
// the size fitted to the 2026-11-08 log; every other number is made up. One
// check runs the engine's own threads on the real clock, with the pretend
// keyboard, for a fraction of a second.

#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/macro.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
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

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

using Mode = MacroEngine::PaceStatus::Mode;
using Turn = MacroEngine::PaceStatus::Turn;

std::string temp_path(const char* name) {
    char dir[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return name;
    return std::string(dir) + "gobot_test_" + std::to_string(GetCurrentProcessId()) + "\\" + name;
}

MacroKey skill(std::uint32_t vk, std::uint32_t interval_ms) {
    MacroKey k;
    k.keycode = vk;
    k.label = "Skill";
    k.enabled = true;
    k.interval_ms = interval_ms;
    k.jitter_pct = 0;
    return k;
}

MacroKey buff(std::uint32_t vk, std::uint32_t interval_ms, std::uint32_t start_delay_ms) {
    MacroKey k = skill(vk, interval_ms);
    k.label = "Buff";
    k.buff = true;
    k.start_delay_ms = start_delay_ms;
    return k;
}

MacroKey potion(MacroTrigger trigger, std::uint32_t vk, int pct) {
    MacroKey k = skill(vk, 3000);
    k.label = "Potion";
    k.trigger = trigger;
    k.threshold_pct = pct;
    return k;
}

/// The owner's eight skill keys of 2026-11-08 22:40, in their order and at
/// their intervals: Z 0.5 s; X, C, V, B, Space 1 s; S, A 3 s.
std::vector<MacroKey> owner_keys() {
    return { skill('Z', 500),  skill('X', 1000),      skill('C', 1000), skill('V', 1000),
             skill('B', 1000), skill(VK_SPACE, 1000), skill('S', 3000), skill('A', 3000) };
}

/// A key as one letter of the tests' strings: Space as '_'.
char letter(std::uint32_t vk) { return vk == VK_SPACE ? '_' : static_cast<char>(vk); }

/// The pretend game: a key-down outside the last cast's lock and the key's own
/// cooldown is cast; any other it drops, as the game drops it.
struct Game {
    std::uint64_t lock_ms = 700;                        ///< from the key-down it took
    std::function<std::uint64_t()> lock_of;             ///< when set: each cast's lock, asked at it
    std::map<std::uint32_t, std::uint64_t> lock_after;  ///< a key whose cast locks this long instead
    std::uint64_t latency_ms = 40;                      ///< key-down to the cast's stamp
    std::uint64_t delivery_ms = 30;                     ///< the stamp to the engine
    std::map<std::uint32_t, std::uint64_t> cooldown;    ///< a key's own, ms
    std::map<std::uint32_t, int> drop;                  ///< the next so many presses of a key dropped, whatever
    bool casting = true;                                ///< false: nothing cast -- dead, in a town
    bool seen = true;                                   ///< false: casts made, none handed in -- no capture
    std::uint64_t lock_end = 0;
    std::map<std::uint32_t, std::uint64_t> ready_at;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> casts;     ///< key, its cast's stamp
    struct Due {
        std::uint64_t arrive = 0;
        std::uint64_t stamp = 0;
        std::uint32_t skill = 0;
    };
    std::vector<Due> inbox;

    void key_down(std::uint32_t vk, std::uint64_t t) {
        const auto d = drop.find(vk);
        if (d != drop.end() && d->second > 0) {
            --d->second;
            return;
        }
        if (!casting || t < lock_end || t < ready_at[vk]) return;
        const auto own = lock_after.find(vk);
        lock_end = t + (own != lock_after.end() ? own->second : lock_of ? lock_of() : lock_ms);
        const auto cd = cooldown.find(vk);
        ready_at[vk] = t + (cd == cooldown.end() ? 0 : cd->second);
        const std::uint64_t stamp = t + latency_ms;
        casts.emplace_back(vk, stamp);
        if (seen) inbox.push_back(Due{ stamp + delivery_ms, stamp, 50'000 + vk });
    }
    std::size_t cast_count(std::uint32_t vk) const {
        return static_cast<std::size_t>(std::count_if(casts.begin(), casts.end(),
                                                      [vk](const auto& c) { return c.first == vk; }));
    }
};

/// The engine on a clock of the test's own, a pretend keyboard -- each key held
/// 80 ms of that clock -- and the pretend game, its casts handed in before each
/// step as the capture would. With `seen_before`, a cast of yours came 5 s
/// before the start: the packets are there, and the round starts at once.
struct Rig {
    InputManager         input;
    ExecutionCoordinator coord;
    MacroEngine          engine{ input, coord };
    std::uint64_t        t = 31'000'000;
    float                hp = 100.0f, mp = 100.0f;
    bool                 focused = true;
    Game                 game;
    std::set<std::uint32_t> casters;    ///< the keys whose press the game casts: all but the potions
    std::set<std::uint32_t> fails;      ///< keys that never go out, as Windows refusing them: pressed is false
    std::vector<std::pair<std::uint32_t, std::uint64_t>> pressed;   ///< the presses that went out
    std::vector<std::pair<std::uint32_t, std::uint64_t>> failed;    ///< those of `fails` tried

    explicit Rig(std::vector<MacroKey> keys, bool paced = true, bool seen_before = true,
                 const std::function<void(MacroConfig&)>& tune = {},
                 std::function<MacroEngine::Fight()> fight = {}) {
        MacroEngine::Clock c;
        c.now_ms = [this] { return t; };
        c.sleep_ms = [this](std::uint32_t ms) { t += ms; };
        engine.set_clock(c);
        MacroEngine::Hands h;
        h.focused = [this] { return focused; };
        h.key = [this](std::uint32_t vk, int hold) {
            if (fails.count(vk) != 0) {
                failed.emplace_back(vk, t);
                return false;
            }
            pressed.emplace_back(vk, t);
            if (casters.count(vk) != 0) game.key_down(vk, t);
            if (hold > 0) t += static_cast<std::uint64_t>(hold);
            return true;
        };
        engine.set_hands(h);
        if (fight) engine.set_fight(std::move(fight));
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys = std::move(keys);
        cfg.sequence_enabled = false;
        cfg.global_jitter_pct = 0;
        cfg.skills_follow_fight = false;
        cfg.key_hold_min_ms = 80;
        cfg.key_hold_max_ms = 80;
        cfg.cast_paced = paced;
        if (tune) tune(cfg);
        for (const MacroKey& k : cfg.keys)
            if (!k.conditional()) casters.insert(k.keycode);
        engine.set_config(cfg);
        if (seen_before) engine.note_own_cast(1, t - 5000);
        engine.start_stepped();
    }
    ~Rig() { engine.stop(); }

    /// One step of 10 ms: the casts come in, the bars are read, a pass.
    void step() {
        std::vector<Game::Due> keep;
        for (const Game::Due& d : game.inbox) {
            if (d.arrive <= t) engine.note_own_cast(d.skill, d.stamp);
            else keep.push_back(d);
        }
        game.inbox.swap(keep);
        engine.set_vitals(hp, mp);
        engine.step();
    }
    void run(std::uint64_t ms) {
        for (const std::uint64_t until = t + ms; t < until; t += 10) step();
    }
    /// Steps until `done` says so, `ms` at most.
    void run_until(const std::function<bool()>& done, std::uint64_t ms) {
        for (const std::uint64_t until = t + ms; t < until && !done(); t += 10) step();
    }
    /// The keys pressed from the `from`-th press on, the potions' too, a letter each.
    std::string text(std::size_t from = 0) const {
        std::string s;
        for (std::size_t i = from; i < pressed.size(); ++i) s += letter(pressed[i].first);
        return s;
    }
    std::vector<std::uint64_t> when(std::uint32_t vk) const {
        std::vector<std::uint64_t> out;
        for (const auto& p : pressed)
            if (p.first == vk) out.push_back(p.second);
        return out;
    }
};

/// The first line the log had, while `run` ran at Info, with every one of
/// `words`; "" for none.
std::string logged_text(const std::function<void()>& run, std::initializer_list<const char*> words) {
    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_ring_capacity(4000);
    log.clear_ring();
    log.set_level(LogLevel::Info);
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

/// The longest gap between two presses of `vk`, from its first.
std::uint64_t longest_gap(const std::vector<std::uint64_t>& at) {
    std::uint64_t worst = 0;
    for (std::size_t i = 1; i < at.size(); ++i) worst = std::max(worst, at[i] - at[i - 1]);
    return worst;
}

// ------------------------------------------------------------ the round

void test_pace_order() {
    section("Paced on your casts: one round in the order of the keys, each once, the next after the cast");

    // The owner's eight keys and intervals; a game that takes a key 700 ms
    // after the last one it took. A minute.
    Rig rig(owner_keys());
    rig.run(60'000);
    const std::string got = rig.text();
    const std::string round = "ZXCVB_SA";
    bool strict = got.size() >= 70;
    for (std::size_t i = 0; strict && i < got.size(); ++i) strict = got[i] == round[i % round.size()];
    std::uint64_t worst = 0;
    for (const std::uint32_t vk : { std::uint32_t{ 'Z' }, std::uint32_t{ 'S' }, std::uint32_t{ 'A' } })
        worst = std::max(worst, longest_gap(rig.when(vk)));
    const MacroEngine::PaceStatus st = rig.engine.pace_status();
    bool all_cast = st.last_round.size() == 8;
    for (std::size_t i = 0; all_cast && i < st.last_round.size(); ++i)
        all_cast = st.last_round[i].turn == Turn::Cast && letter(st.last_round[i].keycode) == round[i];
    check(strict && st.mode == Mode::Paced && st.rounds >= 9 && all_cast,
          "Z X C V B Space S A, round after round, in the order of the keys whatever their intervals -- " +
              got.substr(0, 26) + "..., " + std::to_string(st.rounds) + " rounds in a minute");
    check(rig.game.casts.size() == rig.pressed.size() && rig.pressed.size() >= 75 && rig.pressed.size() <= 90 &&
              worst <= 6'500,
          "every press cast -- " + std::to_string(rig.pressed.size()) + " presses, " +
              std::to_string(rig.game.casts.size()) + " casts in the minute, not some 390 presses for 77 casts -- "
              "and every key again within " + std::to_string(worst) + " ms");
    // Each key goes down the lock after the cast before it, at its guess.
    bool spaced = true;
    for (std::size_t i = 1; spaced && i < rig.pressed.size(); ++i) {
        const std::uint64_t gap = rig.pressed[i].second - rig.pressed[i - 1].second;
        spaced = gap >= 700 && gap <= 760;
    }
    check(spaced && st.lock_ms <= 700 && st.lock_ms >= 680,
          "each key 0.70-0.76 s after the one before -- the cast's stamp and the lock -- and the lock learned a "
          "little shorter after its clean runs: " + std::to_string(st.lock_ms) + " ms");
}

void test_pace_repress() {
    section("Paced on your casts: a press the game dropped, once more -- the lock 20 ms longer once two are");

    Rig rig(owner_keys());
    rig.game.drop['C'] = 1;
    rig.run(6'500);
    const std::string got = rig.text();
    const auto c = rig.when('C');
    const auto v = rig.when('V');
    const MacroEngine::PaceStatus st = rig.engine.pace_status();
    check(got.rfind("ZXCCVB_SA", 0) == 0 && c.size() == 2 && c[1] - c[0] >= MacroEngine::kPaceRepressMs &&
              c[1] - c[0] <= MacroEngine::kPaceRepressMs + 10 && rig.game.cast_count('C') == 1,
          "C's first press dropped: the same key again " + std::to_string(c.size() == 2 ? c[1] - c[0] : 0) +
              " ms after it, cast, and the round goes on with V -- " + got);
    check(st.lock_ms == MacroEngine::kPaceLockStartMs && st.last_round.size() == 8 &&
              st.last_round[2].turn == Turn::CastOnRepress && st.last_round[3].turn == Turn::Cast,
          "one dropped alone is no lock to learn -- a stun, a hitch: still " + std::to_string(st.lock_ms) +
              " ms; C's turn: " + (st.last_round.size() > 2 ? MacroEngine::pace_turn_name(st.last_round[2].turn) : "?"));
    check(c.size() >= 2 && !v.empty() && v[0] >= c[1] + rig.game.latency_ms + st.lock_ms,
          "V goes the lock after C's cast");

    // Two keys in a row, each dropped once and cast on its re-press: the lock.
    Rig two(owner_keys());
    two.game.drop['C'] = 1;
    two.game.drop['V'] = 1;
    two.run(6'500);
    check(two.text().rfind("ZXCCVVB_SA", 0) == 0 &&
              two.engine.pace_status().lock_ms == MacroEngine::kPaceLockStartMs + MacroEngine::kPaceLockUpMs,
          "C and V each dropped once, one after the other: the lock guessed 20 ms longer (" +
              std::to_string(two.engine.pace_status().lock_ms) + " ms) -- " + two.text());
}

void test_pace_skip() {
    section("Paced on your casts: two presses dropped -- the key skipped this round, the next one at once");

    Rig rig(owner_keys());
    rig.game.drop['C'] = 2;
    rig.run(12'500);
    const std::string got = rig.text();
    const auto c = rig.when('C');
    const auto v = rig.when('V');
    const MacroEngine::PaceStatus st = rig.engine.pace_status();
    check(got.rfind("ZXCCVB_SAZXCVB_SA", 0) == 0 && c.size() >= 3 && !v.empty() &&
              v[0] >= c[1] + MacroEngine::kPaceConfirmMs && v[0] <= c[1] + MacroEngine::kPaceConfirmMs + 10 &&
              rig.game.cast_count('C') == c.size() - 2,
          "C pressed twice with no cast: skipped, and V " +
              std::to_string(v.empty() || c.size() < 2 ? 0 : v[0] - c[1]) +
              " ms after C's second press; next round C in its place again, and cast -- " + got.substr(0, 17));
    check(st.lock_ms == MacroEngine::kPaceLockStartMs,
          "two dropped are no lock to learn: still " + std::to_string(st.lock_ms) + " ms");
    // Never a third: a key the game will not take costs two presses a round.
    Rig stuck(owner_keys());
    stuck.game.drop['C'] = 1000;
    stuck.run(30'000);
    const std::size_t rounds = stuck.engine.pace_status().rounds;
    check(stuck.when('C').size() <= 2 * (rounds + 1) && stuck.when('Z').size() >= rounds &&
              stuck.engine.pace_status().mode == Mode::Paced,
          "a key never cast: two presses a round at most, every other key on -- " +
              std::to_string(stuck.when('C').size()) + " presses of C in " + std::to_string(rounds) + " rounds");
}

// ------------------------------------------------------------ no casts

void test_pace_fallback() {
    section("Paced on your casts: none for 2 s while keys go out -- the timers, then the round again");

    std::size_t back_at = 0, z_timers = 0;
    std::uint64_t fell = 0, stopped = 0;
    MacroEngine::PaceStatus during, after;
    std::string round_after;
    const std::string line = logged_text([&] {
        Rig rig(owner_keys());
        rig.run(3'000);
        // The capture stops: the game goes on casting, and none comes in.
        rig.game.seen = false;
        stopped = rig.t;
        rig.run_until([&] { return rig.engine.pace_status().mode == Mode::Fallback; }, 5'000);
        fell = rig.t - stopped;
        const std::size_t timers_from = rig.pressed.size();
        rig.run(5'000);
        during = rig.engine.pace_status();
        for (std::size_t i = timers_from; i < rig.pressed.size(); ++i) z_timers += rig.pressed[i].first == 'Z';
        // It comes back: the next cast hands the skills back to the round.
        rig.game.seen = true;
        rig.run_until([&] { return rig.engine.pace_status().mode == Mode::Paced; }, 2'000);
        back_at = rig.pressed.size();
        rig.run(13'000);
        after = rig.engine.pace_status();
        round_after = rig.text(back_at);
    }, { "Paced: no cast of yours for " });
    check(has(line, " key(s) went out -- the skills on their own timers until your next cast") &&
              during.mode == Mode::Fallback && fell >= MacroEngine::kPaceStaleMs && fell <= 3'500,
          "no cast seen from the first key out: on the timers " + std::to_string(fell) + " ms after the casts "
              "stopped coming -- " + line);
    check(z_timers >= 7,
          "on the timers every key as before: Z, at 0.5 s, " + std::to_string(z_timers) + " times in 5 s");
    bool no_cooling = after.mode == Mode::Paced && after.last_round.size() == 8;
    for (const auto& s : after.last_round) no_cooling = no_cooling && s.turn == Turn::Cast;
    check(no_cooling && round_after.size() >= 16,
          "the casts come again: the round again, nothing learned of the keys from the stop -- every key cast in "
          "its turn: " + round_after.substr(0, 24));

    // The line when it comes back, and when it is first switched on with no
    // cast of yours yet: the timers until the first.
    const std::string came = logged_text([] {
        Rig rig(owner_keys(), true, false);
        rig.run(2'000);
    }, { "Paced: your cast came after " });
    const std::string first = logged_text([] {
        Rig rig(owner_keys(), true, false);
        rig.run(200);
    }, { "Paced: on, but no cast of yours in the packets yet" });
    check(!came.empty() && !first.empty(),
          "switched on with none of your casts yet: on the timers, then the round from the first -- " + first +
              " / " + came);
}

void test_pace_refused() {
    section("Paced on your casts: a press refused is no try -- the same key when it can go");

    Rig rig(owner_keys());
    rig.run(100);                           // Z out, cast
    rig.focused = false;                    // GoBot's own window clicked
    const std::size_t had = rig.pressed.size();
    rig.run(1'500);
    const bool none = rig.pressed.size() == had;
    const std::uint64_t refused = rig.engine.stats().skills_unfocused;
    rig.focused = true;
    rig.run(900);
    const std::string got = rig.text();
    const MacroEngine::PaceStatus st = rig.engine.pace_status();
    check(none && refused >= 8 && got.rfind("ZXC", 0) == 0 && rig.when('X').size() == 1 &&
              rig.game.cast_count('X') == 1 && st.mode == Mode::Paced,
          "the game behind another window for 1.5 s: nothing pressed, X refused " + std::to_string(refused) +
              " times (counted on the Skills line), then pressed once, its first press, and cast -- " + got);
}

void test_pace_potion_first() {
    section("Paced on your casts: a potion due goes first, the skill right after it");

    Rig rig({ skill('Z', 500), skill('X', 1000), potion(MacroTrigger::HpBelow, '1', 50) });
    rig.run(100);
    // Up to the step at which X goes, then HP under the mark: both due in it.
    rig.run_until([&] {
        const MacroEngine::PaceStatus st = rig.engine.pace_status();
        return rig.t >= st.last_cast_ms + st.lock_ms;
    }, 2'000);
    rig.hp = 40.0f;
    const std::size_t had = rig.pressed.size();
    rig.run(100);
    const bool first = rig.pressed.size() >= had + 2 && rig.pressed[had].first == '1' && rig.pressed[had + 1].first == 'X' &&
                       rig.pressed[had + 1].second == rig.pressed[had].second + 80;
    check(first, "HP read under the mark as X was due: the potion's key first, X right after its key-up -- " +
                     rig.text(had));
}

void test_pace_lock_bounds() {
    section("Paced on your casts: the lock as learned stays 550-900 ms");

    {
        // A game whose lock is always 100 ms longer than the guess: every
        // first press dropped, every re-press cast -- two keys in a row, the
        // lock, each time.
        Rig rig(owner_keys());
        rig.game.lock_of = [&rig] { return rig.engine.pace_status().lock_ms + rig.game.latency_ms + 100; };
        std::uint32_t most = 0;
        for (int i = 0; i < 3000; ++i) {
            rig.step();
            rig.t += 10;
            most = std::max(most, rig.engine.pace_status().lock_ms);
        }
        check(rig.engine.pace_status().lock_ms == MacroEngine::kPaceLockMaxMs && most == MacroEngine::kPaceLockMaxMs,
              "a lock longer than any guess: up to 900 ms and no further -- " + std::to_string(most) + " ms");
    }
    {
        // A game with a lock of 300 ms: every first press cast, the guess a
        // little shorter after each run of them -- down to 550 and no further.
        Rig rig(owner_keys());
        rig.game.lock_ms = 300;
        std::uint32_t least = MacroEngine::kPaceLockMaxMs;
        for (int i = 0; i < 46'000; ++i) {
            rig.step();
            rig.t += 10;
            least = std::min(least, rig.engine.pace_status().lock_ms);
        }
        check(rig.engine.pace_status().lock_ms == MacroEngine::kPaceLockMinMs && least == MacroEngine::kPaceLockMinMs,
              "a lock shorter than any guess: down to 550 ms and no further -- " + std::to_string(least) + " ms");
    }
}

void test_pace_lone_drops() {
    section("Paced on your casts: a first press dropped now and then, not by the lock -- the lock as it was");

    // The game's lock 700 ms from the cast's stamp, the guess itself; one
    // first press in 50 dropped all the same -- a stun, a frame hitch, a cast
    // seen late -- each cast on its re-press. 500 casts.
    Rig rig(owner_keys());
    rig.game.lock_ms = MacroEngine::kPaceLockStartMs + rig.game.latency_ms;
    const std::vector<std::uint32_t> order = { 'Z', 'X', 'C', 'V', 'B', VK_SPACE, 'S', 'A' };
    const std::uint64_t from = rig.t;
    std::uint32_t least = MacroEngine::kPaceLockMaxMs, most = 0;
    std::size_t next = 50, k = 0;
    while (rig.game.casts.size() < 500 && rig.t < from + 600'000) {
        rig.step();
        rig.t += 10;
        if (rig.game.casts.size() >= next) {
            rig.game.drop[order[(3 * k++) % order.size()]] = 1;
            next += 50;
        }
        least = std::min(least, rig.engine.pace_status().lock_ms);
        most = std::max(most, rig.engine.pace_status().lock_ms);
    }
    check(rig.game.casts.size() >= 500 && rig.pressed.size() >= rig.game.casts.size() + 9 &&
              most <= MacroEngine::kPaceLockStartMs + 40 && least + 40 >= MacroEngine::kPaceLockStartMs,
          "one dropped in 50, each cast on its re-press: the lock stays " + std::to_string(least) + ".." +
              std::to_string(most) + " ms, not ratcheted up to " + std::to_string(MacroEngine::kPaceLockMaxMs));
}

void test_pace_min_gap() {
    section("Paced on your casts: a key's interval only its least time -- passed over until then, in its place");

    // X at 5 s between Z and C at 0.5 s: X goes in its place each time 5 s
    // have passed since its cast, passed over in between.
    Rig rig({ skill('Z', 500), skill('X', 5000), skill('C', 500) });
    bool passed_seen = false;
    for (int i = 0; i < 3000; ++i) {
        rig.step();
        rig.t += 10;
        for (const auto& s : rig.engine.pace_status().last_round)
            passed_seen = passed_seen || (s.keycode == 'X' && s.turn == Turn::NotDue);
    }
    const std::string got = rig.text();
    const auto x = rig.when('X');
    bool spaced = x.size() >= 4;
    for (std::size_t i = 1; spaced && i < x.size(); ++i) spaced = x[i] - x[i - 1] >= 5000;
    bool placed = true;
    for (std::size_t i = 0; placed && i < got.size(); ++i)
        if (got[i] == 'X') placed = i > 0 && got[i - 1] == 'Z';
    check(spaced && placed && passed_seen && rig.game.casts.size() == rig.pressed.size(),
          "X every 5 s or more, always right after Z, passed over (not due) in between -- " + got.substr(0, 30));

    // None due: a wait, not a press. Z alone at 2 s.
    Rig alone({ skill('Z', 2000) });
    alone.run(10'000);
    const auto z = alone.when('Z');
    bool waited = z.size() >= 5;
    for (std::size_t i = 1; waited && i < z.size(); ++i) waited = z[i] - z[i - 1] >= 2000 && z[i] - z[i - 1] <= 2100;
    check(waited, "a key alone at 2 s: once each 2 s from its cast, " + std::to_string(z.size()) + " in 10 s");
}

void test_pace_cooldown() {
    section("Paced on your casts: a key on the game's cooldown skipped, then passed over without a press");

    // S on an 8-s cooldown in the game: a round is some 6 s, so every other
    // round S is not ready. The first two times it costs two presses each --
    // a cooldown is taken on its second sighting (kPaceSeenMs); from then on
    // it is passed over, learned.
    std::size_t s_pressed = 0, s_cast = 0, others_skipped = 0;
    const std::string line = logged_text([&] {
        Rig rig(owner_keys());
        rig.game.cooldown['S'] = 8000;
        rig.run(90'000);
        s_pressed = rig.when('S').size();
        s_cast = rig.game.cast_count('S');
        for (const std::uint32_t vk : { std::uint32_t{ 'Z' }, std::uint32_t{ 'X' }, std::uint32_t{ 'A' } })
            others_skipped += rig.when(vk).size() - rig.game.cast_count(vk);
    }, { "Paced: ", " rounds; " });
    check(s_cast >= 5 && s_pressed == s_cast + 4 && others_skipped == 0,
          "S cast " + std::to_string(s_cast) + " times in 90 s with " + std::to_string(s_pressed) +
              " presses: four for the two rounds it was found cooling before its floor was taken, none since; the "
              "others never pressed in vain");
    check(has(line, "skipped (cooldown): S ") && has(line, " learned, floor ") && has(line, "; lock_est ") &&
              has(line, " casts confirmed (") && has(line, "; fallback 0.0 s;") && has(line, "; now paced"),
          "the minute's line: rounds, casts confirmed, re-presses, each key skipped and its floor, the time on the "
          "timers, the lock -- " + line);
}

void test_pace_short_stop() {
    section("Paced on your casts: the game stopped for a second -- no cooldown learned for keys with none");

    // No key on a cooldown; the game casts nothing for 1 s -- a stun, a frame
    // hitch, the capture late -- from four moments of the round: the keys
    // skipped in it, and the next one cast within kPaceFloorTakenMs. Seen once
    // only, no floor: no key passed over as cooling after, and the minute's
    // line names no floor.
    for (const std::uint64_t at : { std::uint64_t{ 10'000 }, std::uint64_t{ 11'500 }, std::uint64_t{ 13'000 },
                                    std::uint64_t{ 14'000 } }) {
        bool cooling = false;
        std::size_t skipped = 0;
        const std::string line = logged_text([&] {
            Rig rig(owner_keys());
            const std::uint64_t from = rig.t;
            rig.run(at);
            rig.game.casting = false;
            const std::uint64_t back = rig.t + 1'000;
            std::uint64_t rounds = rig.engine.pace_status().rounds;
            while (rig.t < from + 62'000) {
                if (rig.t >= back) rig.game.casting = true;
                rig.step();
                rig.t += 10;
                const MacroEngine::PaceStatus st = rig.engine.pace_status();
                for (const auto& s : st.last_round) cooling = cooling || s.turn == Turn::Cooling;
                if (st.rounds != rounds) {
                    rounds = st.rounds;
                    for (const auto& s : st.last_round) skipped += s.turn == Turn::NoCast;
                }
            }
        }, { "Paced: ", " rounds; " });
        check(!cooling && skipped >= 1 && !line.empty() && !has(line, ", floor "),
              "casts stopped for 1 s from " + std::to_string(at) + " ms, no cooldowns: " + std::to_string(skipped) +
                  " key(s) skipped in it, none passed over as cooling after, no floor -- " + line);
    }
}

void test_pace_failed_key() {
    section("Paced on your casts: a key that never goes out -- its turn passes, the round goes on");

    // C's press fails every time, as one Windows will not take: no cast, no
    // re-press, and the rest of the round in its order, every key cast.
    Rig rig(owner_keys());
    rig.fails = { 'C' };
    rig.run(60'000);
    const std::string got = rig.text();
    const std::string round = "ZXVB_SA";
    bool strict = got.size() >= 60;
    for (std::size_t i = 0; strict && i < got.size(); ++i) strict = got[i] == round[i % round.size()];
    const MacroEngine::PaceStatus st = rig.engine.pace_status();
    check(strict && st.mode == Mode::Paced && st.rounds >= 9 && rig.game.casts.size() == rig.pressed.size() &&
              rig.failed.size() <= st.rounds + 1,
          "C tried " + std::to_string(rig.failed.size()) + " times in " + std::to_string(st.rounds) +
              " rounds, once a round, never out; the others round after round in order, each cast -- " +
              got.substr(0, 21));
}

void test_pace_long_lock() {
    section("Paced on your casts: a skill whose lock is longer than the guess -- the key after it not starved");

    // X locks the character 1 s from its key-down, the rest 700 ms. C, right
    // after X, goes down inside X's lock twice and is skipped -- once: from
    // then on its re-press waits out the longest lock, and it is cast every
    // round.
    Rig rig(owner_keys());
    rig.game.lock_after['X'] = 1000;
    std::uint64_t rounds = 0;
    int no_cast = 0, in_a_row = 0, worst_row = 0;
    for (int i = 0; i < 6'000; ++i) {
        rig.step();
        rig.t += 10;
        const MacroEngine::PaceStatus st = rig.engine.pace_status();
        if (st.rounds == rounds) continue;
        rounds = st.rounds;
        for (const auto& s : st.last_round) {
            if (s.keycode != 'C') continue;
            if (s.turn == Turn::NoCast) {
                ++no_cast;
                worst_row = std::max(worst_row, ++in_a_row);
            } else {
                in_a_row = 0;
            }
        }
    }
    // Each of C's casts after its first skip: its re-press the longest lock
    // after X's cast.
    std::uint64_t x_cast = 0;
    bool waited = true;
    std::size_t c_casts = 0;
    for (const auto& c : rig.game.casts) {
        if (c.first == 'X') x_cast = c.second;
        if (c.first != 'C') continue;
        ++c_casts;
        waited = waited && x_cast != 0 && c.second >= x_cast + MacroEngine::kPaceLockMaxMs + MacroEngine::kPaceRepressMs;
    }
    check(rounds >= 8 && no_cast <= 1 && worst_row <= 1 && c_casts + 1 >= rounds && waited,
          "C skipped " + std::to_string(no_cast) + " time(s), cast " + std::to_string(c_casts) + " times in " +
              std::to_string(rounds) + " rounds, its re-press after X's lock at the longest");
}

void test_pace_buff() {
    section("Paced on your casts: a buff after the skill's cast and its lock, the skills after the buff's");

    // B, a buff, falls due while Z's press waits for its cast (100 ms on the
    // way): it waits for the cast and its lock, then goes; its cast's four
    // presses go uninterrupted, and X waits for the buff's own cast and lock.
    Rig rig({ skill('Z', 1000), skill('X', 1000), buff('B', 60'000, 100) }, true, true, [](MacroConfig& cfg) {
        cfg.buff_presses = 4;
        cfg.buff_press_gap_ms = 200;
    });
    rig.game.delivery_ms = 100;
    rig.run(3'000);
    const auto z = rig.when('Z');
    const auto b = rig.when('B');
    const auto x = rig.when('X');
    bool burst_alone = b.size() == 4;
    for (const auto& p : rig.pressed)
        if (p.first != 'B' && b.size() == 4 && p.second > b.front() && p.second < b.back()) burst_alone = false;
    std::uint64_t b_cast = 0;
    for (const auto& c : rig.game.casts)
        if (c.first == 'B') b_cast = c.second;
    check(!z.empty() && burst_alone && b.front() >= z.front() + rig.game.latency_ms + rig.game.delivery_ms &&
              b.front() >= z.front() + rig.game.latency_ms + MacroEngine::kPaceLockStartMs &&
              rig.game.cast_count('B') == 1,
          "the buff due while Z waited for its cast went once Z's cast and its lock were over, " +
              std::to_string(b.empty() || z.empty() ? 0 : b.front() - z.front()) +
              " ms after Z, cast, and its four presses with no skill between -- " + rig.text());
    check(b_cast != 0 && !x.empty() && x[0] >= b_cast + MacroEngine::kPaceLockStartMs && rig.game.cast_count('X') >= 1 &&
              rig.when('X').size() == rig.game.cast_count('X'),
          "X after the buff's own cast and its lock, and cast on its first press");

    // Z's cast handed in 30 ms on, as most are: B due as Z waits for it goes
    // after Z's lock, not inside it -- where all four of its presses would be
    // dropped, and the buff lost for its whole interval.
    Rig soon({ skill('Z', 1000), skill('X', 1000), buff('B', 60'000, 50) }, true, true, [](MacroConfig& cfg) {
        cfg.buff_presses = 4;
        cfg.buff_press_gap_ms = 200;
    });
    soon.run(3'000);
    const auto sz = soon.when('Z');
    const auto sb = soon.when('B');
    check(!sz.empty() && sb.size() == 4 &&
              sb.front() >= sz.front() + soon.game.latency_ms + MacroEngine::kPaceLockStartMs &&
              soon.game.cast_count('B') == 1,
          "a buff due as Z's cast came in at once: after Z's lock, " +
              std::to_string(sb.empty() || sz.empty() ? 0 : sb.front() - sz.front()) + " ms after Z, and cast -- " +
              soon.text());

    // One press a buff: that press is the whole buff -- never inside a lock.
    Rig once({ skill('Z', 1000), skill('X', 1000), buff('B', 60'000, 100) }, true, true,
             [](MacroConfig& cfg) { cfg.buff_presses = 1; });
    once.run(3'000);
    const auto ob = once.when('B');
    check(ob.size() == 1 && once.game.cast_count('B') == 1,
          "a buff of one press, due inside Z's lock: pressed once the lock is over, and cast -- " + once.text());
}

void test_pace_buff_after_potion() {
    section("Paced on your casts: a potion before a buff in one pass -- the skill still waits for the buff's cast");

    // A slow game: a cast stamped 80 ms after its key and handed in 70 ms on.
    // HP falls under the potion's mark as X's lock ends, the buff due: the
    // potion, then the buff, in one pass -- the buff's wait for its cast from
    // its own key-down, not the pass's start, so C waits for the cast and its
    // lock instead of going inside it.
    Rig rig({ skill('Z', 1000), skill('X', 1000), skill('C', 1000), buff('B', 60'000, 900),
              potion(MacroTrigger::HpBelow, '1', 50) },
            true, true, [](MacroConfig& cfg) { cfg.buff_presses = 1; });
    rig.game.latency_ms = 80;
    rig.game.delivery_ms = 70;
    rig.run_until([&] {
        const auto x = rig.when('X');
        const MacroEngine::PaceStatus st = rig.engine.pace_status();
        return !x.empty() && st.last_cast_ms > x[0] && rig.t >= st.last_cast_ms + st.lock_ms;
    }, 3'000);
    rig.hp = 40.0f;
    const std::size_t had = rig.pressed.size();
    rig.step();
    rig.hp = 100.0f;
    rig.run(1'500);
    const auto b = rig.when('B');
    const auto c = rig.when('C');
    std::uint64_t b_cast = 0;
    for (const auto& cast : rig.game.casts)
        if (cast.first == 'B') b_cast = cast.second;
    const bool together =
        rig.pressed.size() >= had + 2 && rig.pressed[had].first == '1' && rig.pressed[had + 1].first == 'B';
    check(together && b.size() == 1 && b_cast != 0 && c.size() == 1 &&
              c[0] >= b_cast + MacroEngine::kPaceLockStartMs && rig.game.cast_count('C') == 1,
          "the potion and the buff in one pass, C the lock after the buff's cast -- " +
              std::to_string(c.empty() || b_cast == 0 ? 0 : c[0] - b_cast) + " ms -- and cast on its first press: " +
              rig.text(had));
}

void test_pace_others() {
    section("Paced on your casts: off while the hunter or the key sequence presses keys too");

    // The hunter running, the skills not following the fight: its swing is
    // cast too, and would answer the key waited on. Off: every key on its
    // timer, and the status says why.
    const auto hunting = [] {
        MacroEngine::Fight f;
        f.mode = MacroEngine::Fight::Mode::Fighting;
        f.dx = 50;
        return f;
    };
    Rig hunter(owner_keys(), true, true, {}, hunting);
    hunter.run(3'000);
    const MacroEngine::PaceStatus hs = hunter.engine.pace_status();
    check(hs.mode == Mode::Off && hs.others && hunter.when('Z').size() >= 5,
          "the hunter running: off, said so, Z on its 0.5-s timer (" + std::to_string(hunter.when('Z').size()) +
              " presses in 3 s) -- \"" + MacroEngine::pace_status_vi(hs) + "\"");
    Rig none(owner_keys(), true, true, {}, [] { return MacroEngine::Fight{}; });
    none.run(3'000);
    check(none.engine.pace_status().mode == Mode::Paced && !none.engine.pace_status().others,
          "no hunter: paced");

    // The key sequence on with a key in it: off. A sequence of pauses alone
    // presses nothing: paced.
    Rig sequence(owner_keys(), true, true, [](MacroConfig& cfg) {
        cfg.sequence_enabled = true;
        MacroStep s;
        s.keycode = 'Q';
        s.delay_after_ms = 1000;
        cfg.sequence = { s };
    });
    sequence.run(3'000);
    Rig pauses(owner_keys(), true, true, [](MacroConfig& cfg) {
        cfg.sequence_enabled = true;
        MacroStep s;
        s.keycode = 0;
        s.delay_after_ms = 1000;
        cfg.sequence = { s, s };
    });
    pauses.run(3'000);
    check(sequence.engine.pace_status().mode == Mode::Off && sequence.engine.pace_status().others &&
              pauses.engine.pace_status().mode == Mode::Paced && !pauses.engine.pace_status().others,
          "the key sequence on with a key: off; with pauses alone: paced");
}

// ------------------------------------------------------------ off: as before

void test_pace_off_same() {
    section("Paced off: every key exactly as before, your casts or none");

    // The scenario of skills_test's "as before": skills, a buff by its
    // interval, an HP potion under its mark, half a minute -- with your casts
    // handed in, and with none: the same keys at the same moments.
    const std::vector<MacroKey> keys = { skill('Z', 1200), skill('X', 3000), skill('C', 6000), skill('3', 60'000),
                                         potion(MacroTrigger::HpBelow, '1', 50), skill('V', 9000) };
    Rig fed(keys, false, true);
    Rig none(keys, false, false);
    none.game.seen = false;
    for (Rig* r : { &fed, &none }) {
        r->hp = 45.0f;
        r->run(30'000);
    }
    check(!fed.pressed.empty() && fed.pressed == none.pressed && !fed.game.casts.empty() &&
              fed.engine.pace_status().mode == Mode::Off && fed.when('Z').size() >= 24 && fed.when('Z').size() <= 26,
          "cast_paced off: " + std::to_string(fed.pressed.size()) + " presses, the same with " +
              std::to_string(fed.game.casts.size()) + " casts handed in as with none; Z every 1.2 s as ever");
    // And no line of it.
    const std::string said = logged_text([&keys] {
        Rig r(keys, false, true);
        r.run(61'000);
    }, { "Paced:" });
    check(said.empty(), "and nothing of it in the log");

    // On, but the skills follow the fight with a hunter fighting: off, theirs.
    Rig follow(owner_keys(), true, true, [](MacroConfig& cfg) { cfg.skills_follow_fight = true; }, [] {
        MacroEngine::Fight f;
        f.mode = MacroEngine::Fight::Mode::Fighting;
        f.dx = 50;
        return f;
    });
    follow.run(3'000);
    check(follow.engine.pace_status().mode == Mode::Off && follow.when('Z').size() >= 4,
          "on, but following the fight with a hunter: off -- the skills follow the fight as before");
}

// ------------------------------------------------------------ macro.ini

void test_pace_config() {
    section("Paced on your casts: macro.ini's cast_paced, off unless it says so");

    const std::string path = temp_path("gobot_pace.ini");
    MacroConfig cfg;
    cfg.apply_defaults();
    check(!cfg.cast_paced, "off in a config built in code");
    cfg.cast_paced = true;
    MacroConfig back;
    IniFile raw;
    check(cfg.save(path) && back.load(path) && back.cast_paced && raw.load(path) &&
              raw.get("Macro", "cast_paced", "") == "true",
          "cast_paced=true saved and read back");
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f) {
            std::fputs("[Macro]\ntimers_enabled=true\n[Keys]\nkey_0_keycode=90\nkey_0_enabled=true\n"
                       "key_0_interval_ms=1200\n",
                       f);
            std::fclose(f);
        }
        MacroConfig old;
        check(old.load(path) && !old.cast_paced, "a macro.ini without it: off");
    }
    std::remove(path.c_str());
#ifdef GOBOT_SOURCE_DIR
    const std::string shipped = std::string(GOBOT_SOURCE_DIR) + "/configs/macro.ini";
    MacroConfig ships;
    IniFile ships_raw;
    if (ships.load(shipped) && ships_raw.load(shipped))
        check(!ships.cast_paced && ships_raw.get("Macro", "cast_paced", "") == "false",
              "the shipped macro.ini says cast_paced=false");
    else
        skip("the source tree's configs/macro.ini is not here\n");
#endif
}

// ------------------------------------------------------------ threads

void test_pace_threads() {
    section("Paced on your casts: casts handed in from another thread while the macro's own run");

    // More casts between two passes than the ring holds: the newest is the
    // lock's start, the rest let go of.
    {
        Rig rig(owner_keys());
        rig.run(100);
        for (std::uint64_t i = 0; i < 3 * MacroEngine::kCastRing; ++i) rig.engine.note_own_cast(9, rig.t - 60 + i);
        rig.step();
        check(rig.engine.pace_status().last_cast_ms == rig.t - 60 + 3 * MacroEngine::kCastRing - 1,
              "48 casts between two passes, the ring 16: the newest is where the lock starts");
    }

    // The engine's threads on the real clock, with a pretend keyboard, and a
    // thread standing in for the capture's: a cast handed in a moment after
    // each key goes down, as the game's would, while the status is read from
    // this one. Nothing waits on anything.
    InputManager input;
    ExecutionCoordinator coord;
    MacroEngine engine(input, coord);
    std::atomic<int> presses{ 0 };
    std::mutex keys_mutex;
    std::string keys;                       // the keys out, a letter each
    MacroEngine::Hands h;
    h.focused = [] { return true; };
    h.key = [&presses, &keys_mutex, &keys](std::uint32_t vk, int) {
        {
            std::lock_guard<std::mutex> lock(keys_mutex);
            keys += letter(vk);
        }
        ++presses;
        return true;
    };
    engine.set_hands(h);
    MacroConfig cfg;
    cfg.apply_defaults();
    cfg.keys = owner_keys();
    cfg.sequence_enabled = false;
    cfg.cast_paced = true;
    engine.set_config(cfg);
    // The seed is taken by start()'s arm(), on this thread. From then on only
    // the timer thread's pass takes casts from the ring.
    const std::uint64_t seed = now_ms() - 5000;
    engine.note_own_cast(1, seed);
    engine.start();
    std::atomic<bool> stop{ false };
    std::atomic<int> noted{ 0 };
    std::thread capture([&] {
        int answered = 0;
        while (!stop.load()) {
            if (presses.load() > answered) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                engine.note_own_cast(7, now_ms());
                ++noted;
                answered = presses.load();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    MacroEngine::PaceStatus st;
    for (int i = 0; i < 1000 && presses.load() < 3; ++i) {
        st = engine.pace_status();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stop.store(true);
    capture.join();
    st = engine.pace_status();
    engine.stop();
    std::string out;
    {
        std::lock_guard<std::mutex> lock(keys_mutex);
        out = keys;
    }
    // A last cast later than the seed is one the stand-in handed in, taken by
    // the timer thread. With no handoff the keys go Z, Z again, X (a skip),
    // the seed stays the last cast, and the check fails.
    check(presses.load() >= 3 && noted.load() >= 2 && st.mode == Mode::Paced && st.last_cast_ms > seed,
          "the rotation on its own thread, the casts handed in from another and taken there: " + out + ", " +
              std::to_string(presses.load()) + " key(s) out, " + std::to_string(noted.load()) + " casts in, paced");
}

// ------------------------------------------------------------ the wiring

#ifdef GOBOT_SOURCE_DIR
std::string source(const std::string& rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// A function's definition in `src`, from `head` to the brace that closes it
/// -- the first at the start of a line -- or "" when there is none.
std::string body(const std::string& src, const std::string& head) {
    const std::size_t from = src.find(head);
    if (from == std::string::npos) return {};
    const std::size_t end = src.find("\n}", from);
    return end == std::string::npos ? std::string() : src.substr(from, end + 2 - from);
}

int count_in(const std::string& text, const std::string& part) {
    int n = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
    return n;
}
#endif

void test_pace_wiring() {
    section("Paced on your casts: wired in -- your own casts only, no lock taken on the capture thread (source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string more = source("/apps/gobot_gui/packets_more.cpp");
    const std::string macro = source("/src/macro.cpp");
    const std::string pace = source("/src/macro_pace.cpp");
    const std::string world = source("/src/packet_world.cpp");
    if (more.empty() || macro.empty() || pace.empty() || world.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::string fed = body(more, "void GuiApp::more_on_packets(");
    const std::size_t buffs = fed.find("part.buffs.on_packets(all);");
    const std::size_t me = fed.find("const std::optional<std::uint32_t> me = packet_world_.own_id();");
    check(buffs != std::string::npos && me != std::string::npos && buffs < me &&
              has(fed, "const std::optional<CastFrame> c = read_cast(p);") &&
              has(fed, "if (c && c->id == *me) macro_->note_own_cast(c->skill, p.timestamp_ms);") &&
              has(body(world, "std::optional<std::uint32_t> PacketWorld::own_id() const {"), "if (!fix_.id_locked)"),
          "packets_more.cpp: your client's own 0x0031 over UDP (read_cast), under your id as the world has it "
          "locked, at the frame's own time, to the macro");
    const std::string noted = body(pace, "void MacroEngine::note_own_cast(");
    check(!noted.empty() && !has(noted, "lock_guard") && !has(noted, "unique_lock") && !has(noted, "mutex") &&
              !has(noted, "GB_") &&
              has(noted, "casts_noted_.store(n + 1, std::memory_order_release);"),
          "note_own_cast: atomics only -- no lock of the engine's, no log, on the capture thread");
    const std::string timers = body(macro, "void MacroEngine::run_timers(");
    // The fight is read at every pass since 2026.11.10 (Fight::Mode::Returning
    // holds the skills while the hunter walks back); with the switch off
    // pace_pass is off before it looks at whether others press keys.
    const std::string pass_body = body(pace, "bool MacroEngine::pace_pass(");
    check(has(timers, "const bool paced = pace_pass(cfg, follow, others, now);") &&
              has(timers, "const Fight fight = fight_now();") &&
              has(pass_body, "const bool off_others = cfg.cast_paced && !follow && others;") &&
              has(pass_body, "if (!cfg.cast_paced || follow || others) {") &&
              has(timers, "if (paced && macro_key_kind(k) == MacroKeyKind::Skill) continue;") &&
              has(timers, "else if (paced) run_paced(cfg, clock_now());") &&
              has(timers, "if (cfg.cast_paced) log_paced(clock_now());"),
          "run_timers: the skill keys the rotation's only while paced; the timers' otherwise, as before; the fight "
          "read at every pass, the rotation off with its switch whatever it says");
    // A buff paced: held while a skill's cast is waited for or inside the
    // last cast's lock; its wait counted from its own key-down.
    const std::string repeats = body(macro, "void MacroEngine::run_buff_repeats(");
    check(has(timers, "if (buff && paced && (pace_press_.active || now < pace_last_cast_ms_ + pace_lock_ms_)) "
                      "continue;") &&
              has(timers, "if (paced) pace_buff_ms_ = key_down;") &&
              has(repeats, "if (pace_mode_ == PaceStatus::Mode::Paced) pace_buff_ms_ = key_down;") &&
              !has(timers, "pace_buff_ms_ = now;") && !has(repeats, "pace_buff_ms_ = now;"),
          "a buff due inside a lock waits it out; its wait stamped at its key-down, not the pass's start");
    check(count_in(pace, "press(k.keycode, k.hold_ms, \"macro-paced\", &why)") == 2 && !has(pace, "key_press(") &&
              !has(pace, "SendInput") && !has(pace, "send_key(") && count_in(pace, "potions_first(cfg, clock_now())") == 2,
          "macro_pace.cpp presses only through press(), the potions looked at before each of its two presses");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_cast_pace(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_pace_order();
    test_pace_repress();
    test_pace_skip();
    test_pace_fallback();
    test_pace_refused();
    test_pace_potion_first();
    test_pace_lock_bounds();
    test_pace_lone_drops();
    test_pace_min_gap();
    test_pace_cooldown();
    test_pace_short_stop();
    test_pace_failed_key();
    test_pace_long_lock();
    test_pace_buff();
    test_pace_buff_after_potion();
    test_pace_others();
    test_pace_off_same();
    test_pace_config();
    test_pace_threads();
    test_pace_wiring();
}
