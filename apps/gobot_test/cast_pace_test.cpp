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
#include "gobot/hunter.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/macro.hpp"
#include "gobot/mob_detector.hpp"
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
#include <memory>
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

/// The hunter as the macro sees it (2026.11.16b): the fight the test sets, and
/// a pretend swing of its attack key every `every_ms` while Fighting -- left to
/// the rotation when the engine says so (swing_yields), as Hunter's is, unless
/// `obey` is false. A swing that goes out goes straight to the game, as the
/// hunter's own key does, and its key-down is the fight's swing_ms.
struct Hunt {
    MacroEngine::Fight         f;
    std::uint32_t              swing_vk = 0;    ///< the key it swings; 0 none
    std::uint64_t              every_ms = 700;
    std::uint64_t              last_ms = 0;
    bool                       obey = true;
    std::vector<std::uint64_t> swings;          ///< the swings that went out
    std::uint64_t              yielded = 0;     ///< swings due and left to the rotation
};

std::shared_ptr<Hunt> fighting(std::uint32_t attack_vk) {
    auto h = std::make_shared<Hunt>();
    h->f.mode = MacroEngine::Fight::Mode::Fighting;
    h->f.dx = 50;
    h->f.attack_key = attack_vk;
    h->swing_vk = attack_vk;
    return h;
}

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
    float                pet = -1.0f;   ///< the pet's HP as read; -1 none
    std::shared_ptr<Hunt> hunt;         ///< the hunter, when one runs (with_hunter)
    bool                 packets = false;   ///< the packets coming, told every step (note_packets, 2026.11.17)

    explicit Rig(std::vector<MacroKey> keys, bool paced = true, bool seen_before = true,
                 const std::function<void(MacroConfig&)>& tune = {},
                 std::function<MacroEngine::Fight()> fight = {}, std::shared_ptr<Hunt> hunter = {})
        : hunt(std::move(hunter)) {
        if (hunt && !fight) {
            const std::shared_ptr<Hunt> h = hunt;
            fight = [h] { return h->f; };
        }
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
        if (packets) engine.note_packets(t);
        engine.set_vitals(hp, mp, pet);
        engine.step();
        // The hunter's own swing, on its own beat, as its thread would -- on
        // what the pass just done says: a swing a pass behind the macro, the
        // race a thread of its own can lose, is test_pace_swing_not_ours'.
        if (hunt && hunt->swing_vk != 0 && hunt->f.mode == MacroEngine::Fight::Mode::Fighting &&
            t >= hunt->last_ms + hunt->every_ms) {
            if (hunt->obey && engine.swing_yields(hunt->swing_vk)) {
                ++hunt->yielded;
            } else {
                hunt->last_ms = t;
                swing_now(hunt->swing_vk);
            }
        }
    }
    /// A swing of the hunter's going out now, whatever the engine says: to the
    /// game, its key-down the fight's swing_ms.
    void swing_now(std::uint32_t vk) {
        game.key_down(vk, t);
        if (hunt) {
            hunt->swings.push_back(t);
            hunt->f.swing_ms = t;
        }
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

    std::size_t back_at = 0, z_timers = 0, timer_presses = 0, keys_seen = 0;
    std::uint64_t fell = 0, stopped = 0, least_gap = ~std::uint64_t{ 0 };
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
        std::set<std::uint32_t> seen;
        for (std::size_t i = timers_from; i < rig.pressed.size(); ++i) {
            z_timers += rig.pressed[i].first == 'Z';
            seen.insert(rig.pressed[i].first);
            if (i > timers_from) least_gap = std::min(least_gap, rig.pressed[i].second - rig.pressed[i - 1].second);
        }
        timer_presses = rig.pressed.size() - timers_from;
        keys_seen = seen.size();
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
    // 2026.11.17: fallen back after casts were seen, no flood -- a key a lock
    // at most, each key in turn, not Z at its 0.5 s ten times in 5 s.
    check(timer_presses >= 5 && timer_presses <= 8 && least_gap >= MacroEngine::kPaceLockStartMs - 20 &&
              keys_seen == timer_presses && z_timers <= 1,
          "on the timers, your casts seen before: " + std::to_string(timer_presses) + " skill presses in 5 s, " +
              std::to_string(least_gap) + " ms apart at least, " + std::to_string(keys_seen) +
              " keys in turn, Z " + std::to_string(z_timers) + " time(s) -- the lock's pace, no flood");
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
    // Since 2026.11.17 a floor is checked after 3 turns passed over, then 6,
    // 12 and 20: a check or two not cast in 90 s.
    check(s_cast >= 5 && s_pressed >= s_cast + 4 && s_pressed <= s_cast + 6 && others_skipped == 0,
          "S cast " + std::to_string(s_cast) + " times in 90 s with " + std::to_string(s_pressed) +
              " presses: four for the two rounds it was found cooling before its floor was taken, a check or two "
              "since; the others never pressed in vain");
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
    // Since 2026.11.17 its cast seen ends its presses: seen 140 ms after the
    // first, before the second was due -- one press, one cast.
    bool burst_alone = b.size() == 1;
    std::uint64_t b_cast = 0;
    for (const auto& c : rig.game.casts)
        if (c.first == 'B') b_cast = c.second;
    check(!z.empty() && burst_alone && b.front() >= z.front() + rig.game.latency_ms + rig.game.delivery_ms &&
              b.front() >= z.front() + rig.game.latency_ms + MacroEngine::kPaceLockStartMs &&
              rig.game.cast_count('B') == 1,
          "the buff due while Z waited for its cast went once Z's cast and its lock were over, " +
              std::to_string(b.empty() || z.empty() ? 0 : b.front() - z.front()) +
              " ms after Z, cast, and its other three presses spared, its cast seen -- " + rig.text());
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
    check(!sz.empty() && sb.size() == 1 &&
              sb.front() >= sz.front() + soon.game.latency_ms + MacroEngine::kPaceLockStartMs &&
              soon.game.cast_count('B') == 1,
          "a buff due as Z's cast came in at once: after Z's lock, " +
              std::to_string(sb.empty() || sz.empty() ? 0 : sb.front() - sz.front()) + " ms after Z, and cast -- " +
              soon.text());

    // 2026.11.17: a press really lost -- the game drops the first, or the
    // cast comes late -- and the burst goes on until the cast shows, as
    // buff_presses means it to; never more than its four.
    for (const int lost : { 1, 2, 3 }) {
        Rig drop({ skill('Z', 1000), skill('X', 1000), buff('B', 60'000, 50) }, true, true, [](MacroConfig& cfg) {
            cfg.buff_presses = 4;
            cfg.buff_press_gap_ms = 200;
        });
        drop.game.drop['B'] = lost;
        drop.run(3'000);
        const auto db = drop.when('B');
        check(db.size() == static_cast<std::size_t>(lost) + 1 && drop.game.cast_count('B') == 1,
              "the buff's first " + std::to_string(lost) + " press(es) dropped: pressed " + std::to_string(db.size()) +
                  " times, until its cast, and cast once -- " + drop.text());
    }
    Rig slow({ skill('Z', 1000), skill('X', 1000), buff('B', 60'000, 50) }, true, true, [](MacroConfig& cfg) {
        cfg.buff_presses = 4;
        cfg.buff_press_gap_ms = 200;
    });
    slow.game.delivery_ms = 250;            // the cast seen after the second press
    slow.run(3'000);
    check(slow.when('B').size() == 2 && slow.game.cast_count('B') == 1,
          "the buff's cast seen 290 ms after its first press: pressed twice, the last two spared -- " + slow.text());
    // On the timers (not paced), every press of the burst as before.
    Rig timers({ skill('Z', 1000), buff('B', 60'000, 50) }, false, true, [](MacroConfig& cfg) {
        cfg.buff_presses = 4;
        cfg.buff_press_gap_ms = 200;
    });
    timers.run(2'000);
    check(timers.when('B').size() == 4, "paced off: the buff's four presses as before -- " + timers.text());

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
    section("Paced on your casts: with the hunter too (2026.11.16b); off only while the key sequence presses keys");

    // The hunter running, the skills not following the fight: paced all the
    // same since 2026.11.16b, and the status says it is with the hunter. Its
    // swing -- on Q, no key of the rotation's -- is left to the rotation
    // while a key of it is ready, which with these keys is always.
    Rig hunter(owner_keys(), true, true, {}, {}, fighting('Q'));
    hunter.run(6'000);
    const MacroEngine::PaceStatus hs = hunter.engine.pace_status();
    check(hs.mode == Mode::Paced && hs.hunting && !hs.others && hunter.text().rfind("ZXCVB_SAZ", 0) == 0 &&
              has(MacroEngine::pace_status_vi(hs), "Đang theo vòng cùng Tự đánh quái — khóa chiêu ước "),
          "the hunter running: paced, in the order of the keys, said so -- " + hunter.text() + " -- \"" +
              MacroEngine::pace_status_vi(hs) + "\"");
    Rig none(owner_keys(), true, true, {}, [] { return MacroEngine::Fight{}; });
    none.run(3'000);
    check(none.engine.pace_status().mode == Mode::Paced && !none.engine.pace_status().others &&
              !none.engine.pace_status().hunting,
          "no hunter: paced, alone");

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

// ------------------------------------------------------------ with the hunter (2026.11.16b)

/// Every whole round's turns, gathered as the rounds end: per key, its turns
/// and those cast -- on the first press or the re-press.
struct Rounds {
    std::uint64_t                         seen = 0;
    std::map<std::uint32_t, std::size_t>  turns;
    std::map<std::uint32_t, std::size_t>  cast;
    std::map<std::uint32_t, std::size_t>  skipped;      ///< turns with no cast from the key's presses
    void look(const MacroEngine& e) {
        const MacroEngine::PaceStatus st = e.pace_status();
        if (st.rounds == seen) return;
        seen = st.rounds;
        for (const auto& s : st.last_round) {
            ++turns[s.keycode];
            if (s.turn == Turn::Cast || s.turn == Turn::CastOnRepress) ++cast[s.keycode];
            if (s.turn == Turn::NoCast) ++skipped[s.keycode];
        }
    }
};

const std::vector<std::uint32_t> kOwnerOrder = { 'Z', 'X', 'C', 'V', 'B', VK_SPACE, 'S', 'A' };

/// Whether `got` is `round` over and over from its first letter.
bool strictly(const std::string& got, const std::string& round) {
    for (std::size_t i = 0; i < got.size(); ++i)
        if (got[i] != round[i % round.size()]) return false;
    return !got.empty();
}

/// The longest time from one cast of the game's to the next, from the `from`-th.
std::uint64_t longest_cast_gap(const Game& g, std::size_t from = 0) {
    std::uint64_t worst = 0;
    for (std::size_t i = from + 1; i < g.casts.size(); ++i)
        worst = std::max(worst, g.casts[i].second - g.casts[i - 1].second);
    return worst;
}

void test_pace_hunting() {
    section("Paced with the hunter (2026.11.16b): the round goes on while it fights, its swing left to it");

    // The owner's eight keys, the hunter fighting all the while and swinging
    // Q -- no key of the rotation's -- every 700 ms when it may. A minute.
    auto h = fighting('Q');
    Rig rig(owner_keys(), true, true, {}, {}, h);
    rig.run(60'000);
    const std::string got = rig.text();
    const MacroEngine::PaceStatus st = rig.engine.pace_status();
    std::size_t least = 1000, most = 0;
    for (const std::uint32_t vk : kOwnerOrder) {
        least = std::min(least, rig.game.cast_count(vk));
        most = std::max(most, rig.game.cast_count(vk));
    }
    check(strictly(got, "ZXCVB_SA") && got.size() >= 70 && st.mode == Mode::Paced && st.hunting &&
              rig.game.casts.size() == rig.pressed.size() && most - least <= 1 && least >= 9,
          "Z X C V B Space S A round after round while the hunter fights, every press cast, each key " +
              std::to_string(least) + "-" + std::to_string(most) + " casts in the minute -- " + got.substr(0, 26));
    check(h->swings.empty() && h->yielded > 0 && rig.engine.swing_yields('Q'),
          "a key of the rotation's always ready: the hunter's swing left to it, " + std::to_string(h->yielded) +
              " times, none pressed on top");

    // Keys with long waits: between their casts nothing of the rotation's is
    // ready, and the swing fills the gap -- each skill still cast at its time,
    // and every turn said cast was the key's own cast.
    auto g = fighting('Q');
    Rig gaps({ skill('Z', 3000), skill('X', 3000) }, true, true, {}, {}, g);
    Rounds r;
    for (int i = 0; i < 3000; ++i) {
        gaps.step();
        gaps.t += 10;
        r.look(gaps.engine);
    }
    const auto z = gaps.when('Z');
    bool timely = gaps.game.cast_count('Z') >= 8 && gaps.game.cast_count('X') >= 8;
    std::uint64_t z_cast = 0;
    for (const auto& c : gaps.game.casts) {
        if (c.first != 'Z') continue;
        if (z_cast != 0) timely = timely && c.second - z_cast >= 3000 && c.second - z_cast <= 3000 + 800;
        z_cast = c.second;
    }
    const bool own = r.cast['Z'] <= gaps.game.cast_count('Z') && r.cast['Z'] + 1 >= gaps.game.cast_count('Z') &&
                     r.cast['X'] <= gaps.game.cast_count('X') && r.cast['X'] + 1 >= gaps.game.cast_count('X');
    check(g->swings.size() >= 10 && timely && own && r.skipped['Z'] == 0 && r.skipped['X'] == 0,
          "Z and X at 3 s: the swing in the gaps (" + std::to_string(g->swings.size()) + " swings, " +
              std::to_string(gaps.game.cast_count('Q')) + " cast), Z every 3.0-3.8 s from its cast (" +
              std::to_string(gaps.game.cast_count('Z')) + " casts in 30 s), every turn said cast the key's own, "
              "none skipped for a swing's lock");

    // The casts handed in 100 ms after their stamp: a swing just before a key
    // falls due is cast, the key pressed inside its lock before its cast is
    // seen -- stamped before the key went down. Given back, not re-pressed
    // inside the lock and skipped.
    auto slow = fighting('Q');
    Rig late({ skill('Z', 3000), skill('X', 3000) }, true, true, {}, {}, slow);
    late.game.delivery_ms = 100;
    Rounds lr;
    for (int i = 0; i < 6000; ++i) {
        late.step();
        late.t += 10;
        lr.look(late.engine);
    }
    check(slow->swings.size() >= 20 && lr.skipped['Z'] == 0 && lr.skipped['X'] == 0 &&
              late.game.cast_count('Z') >= 16 && late.game.cast_count('X') >= 16,
          "the casts seen 100 ms late: Z and X never skipped for a swing's lock (" +
              std::to_string(late.game.cast_count('Z')) + " and " + std::to_string(late.game.cast_count('X')) +
              " casts in a minute, " + std::to_string(slow->swings.size()) + " swings)");
}

void test_pace_attack_key_both() {
    section("Paced with the hunter (2026.11.16b): its attack key a key of the rotation's -- the rotation's");

    // The owner's hunt.ini of 2026-11-15: attack_key=Z, and Z a skill at
    // 0.5 s. Pressed by the hunter every 700 ms on top of the rotation, Z was
    // the one skill cast all the time. Now the rotation has it in its turn.
    auto h = fighting('Z');
    Rig rig(owner_keys(), true, true, {}, {}, h);
    rig.run(60'000);
    const std::size_t z = rig.game.cast_count('Z'), x = rig.game.cast_count('X'), a = rig.game.cast_count('A');
    check(h->swings.empty() && h->yielded > 0 && strictly(rig.text(), "ZXCVB_SA") && z <= x + 1 && x <= z + 1 &&
              a + 1 >= z,
          "attack key Z, a skill too: the hunter left it to the rotation (" + std::to_string(h->yielded) +
              " swings let be), Z cast " + std::to_string(z) + " times, X " + std::to_string(x) + ", A " +
              std::to_string(a) + " -- in turn");
    // Held for a drop, a jump's flight -- the hunter swings in it -- or
    // walking back, the swing is the hunter's again (2026.11.16b review:
    // under a turn or nothing to fight the hand-off is kept, the hunter having
    // no swing to make then -- test_pace_swing_after_hold); and on the
    // timers, with no casts.
    bool held_own = true, kept = true;
    {
        using FM = MacroEngine::Fight::Mode;
        using FH = MacroEngine::Fight::Hold;
        const std::pair<FM, FH> own[] = { { FM::Held, FH::Drop },    { FM::Held, FH::Jump },
                                          { FM::Held, FH::SeekLeg }, { FM::Held, FH::Outside },
                                          { FM::Returning, FH::None } };
        for (const auto& o : own) {
            h->f.mode = o.first;
            h->f.hold = o.second;
            rig.run(20);
            held_own = held_own && !rig.engine.swing_yields('Z');
        }
        for (const FH k : { FH::Turning, FH::NoTarget }) {
            h->f.mode = FM::Held;
            h->f.hold = k;
            rig.run(20);
            kept = kept && rig.engine.swing_yields('Z');
        }
    }
    auto f = fighting('Z');
    Rig timers(owner_keys(), true, false, {}, {}, f);
    timers.game.seen = false;
    timers.run(5'000);
    check(held_own && kept && !timers.engine.swing_yields('Z') && f->swings.size() >= 5 &&
              timers.engine.pace_status().mode == Mode::Fallback,
          "held for a drop, a jump, a seek leg, outside the area or walking back, or on the timers with no cast of "
          "yours: the hunter swings Z itself, as before (" +
              std::to_string(f->swings.size()) + " swings in 5 s); under a turn or nothing to fight Z stays the "
              "rotation's");
}

void test_pace_swing_after_hold() {
    section("Paced with the hunter (2026.11.16b review): the swing as a turn ends or a fight begins -- the slot "
            "the rotation's");

    // The hunter ends a hold -- a turn confirmed, a target engaged -- and
    // swings in that same pass of its own, before the macro's next pass has
    // seen the change: stepped here hunter first. The hand-off cleared under
    // the hold, that swing went out every time, and the attack key Z -- a
    // skill too -- was cast once more per fight on top of its turns. A hold
    // of 300 ms every 4 s, a minute.
    using FM = MacroEngine::Fight::Mode;
    using FH = MacroEngine::Fight::Hold;
    const struct {
        bool        follow;
        FM          mode;
        FH          hold;
        const char* what;
    } kCases[] = {
        { false, FM::Held, FH::Turning, "a turn confirmed" },
        { false, FM::Held, FH::NoTarget, "a target after nothing to fight" },
        { true, FM::Idle, FH::None, "following the fight, a fight begun" },
    };
    for (const auto& c : kCases) {
        auto h = fighting('Z');
        const bool follow = c.follow;
        Rig rig(owner_keys(), true, true, [follow](MacroConfig& cfg) { cfg.skills_follow_fight = follow; }, {}, h);
        std::size_t starts = 0, first = 0;
        for (int i = 0; i < 15; ++i) {
            rig.run(3'700);
            h->f.mode = c.mode;
            h->f.hold = c.hold;
            rig.run(300);
            h->f.mode = FM::Fighting;
            h->f.hold = FH::None;
            // The hunter's pass that ends the hold, before the macro's.
            if (rig.t >= h->last_ms + h->every_ms) {
                ++starts;
                if (rig.engine.swing_yields('Z')) {
                    ++h->yielded;
                } else {
                    h->last_ms = rig.t;
                    rig.swing_now('Z');
                    ++first;
                }
            }
        }
        rig.run(1'000);
        const std::size_t z = rig.game.cast_count('Z'), x = rig.game.cast_count('X');
        check(starts == 15 && first == 0 && h->swings.empty() && z <= x + 1 && x <= z + 1,
              std::string(c.what) + ", 15 times: the hunter's first swing left to the rotation each time (" +
                  std::to_string(first) + " went out); Z cast " + std::to_string(z) + " times, X " +
                  std::to_string(x));
    }

    // The holds where the hunter's swing is wanted: a jump's flight swings,
    // as before -- the hand-off not kept there.
    auto j = fighting('Z');
    Rig jump(owner_keys(), true, true, {}, {}, j);
    jump.run(2'000);
    j->f.mode = FM::Held;
    j->f.hold = FH::Jump;
    jump.run(30);
    check(!jump.engine.swing_yields('Z'), "a jump up's flight: the hunter's own swing, as before");
}

void test_pace_attack_far() {
    section("Paced with the hunter (2026.11.16b review): following the fight, the attack key past the hunter's "
            "reach passes its turn");

    // Z the hunter's attack key and a skill, its range_px 0; the target
    // 300 px off, past the reach the hunter lets its own swing of Z go
    // (Fight::attack_far, F3) -- the hunter holds its swing there, and so
    // the rotation must: before 2026.11.16b only the hunter pressed Z then.
    const auto keys = [] { return std::vector<MacroKey>{ skill('Z', 500), skill('X', 1000), skill('C', 1000) }; };
    auto h = fighting('Z');
    h->f.dx = 300;
    h->f.attack_far = true;
    h->swing_vk = 0;            // the hunter's swing held for the reach
    Rig rig(keys(), true, true, [](MacroConfig& cfg) { cfg.skills_follow_fight = true; }, {}, h);
    rig.run(5'000);
    const std::string past = rig.text();
    const std::uint64_t range = rig.engine.stats().held_range;
    const std::uint64_t worst = longest_cast_gap(rig.game);
    // In reach again: Z in its turn.
    h->f.dx = 60;
    h->f.attack_far = false;
    h->swing_vk = 'Z';
    const std::size_t from = rig.pressed.size();
    rig.run(5'000);
    const std::string in_reach = rig.text(from);
    check(past.find('Z') == std::string::npos && strictly(past, "XC") && past.size() >= 6 && range >= 1 &&
              worst <= 800 && in_reach.find('Z') != std::string::npos && h->swings.empty(),
          "the target past Z's reach: Z passes its turn (" + std::to_string(range) + " counted held for range), X "
          "and C on without a stall (" + std::to_string(worst) + " ms at most) -- " + past.substr(0, 12) +
              "; in reach, Z in its turn again -- " + in_reach.substr(0, 12));
    // On "Luôn bấm theo giờ" (follow off) it is pressed at any distance, as
    // the timers pressed it before.
    auto o = fighting('Z');
    o->f.dx = 300;
    o->f.attack_far = true;
    o->swing_vk = 0;
    Rig own(keys(), true, true, {}, {}, o);
    own.run(5'000);
    check(strictly(own.text(), "ZXC") && own.when('Z').size() >= 2,
          "the skills not following the fight: Z in its turn at any distance, as before -- " + own.text().substr(0, 12));
}

void test_pace_own_timer_pair() {
    section("Paced, following the fight (2026.11.16b review): keys on their own timers and a buff due together -- "
            "a lock apart");

    // T and U on their own timers (always_timer), beside Z and X in the
    // rotation; walking back holds them both, so they fall due together.
    // Back to back, U went inside T's lock, was dropped, and waited a whole
    // interval.
    MacroKey t = skill('T', 5'000);
    t.always_timer = true;
    MacroKey u = skill('U', 5'000);
    u.always_timer = true;
    auto h = fighting('Q');
    h->swing_vk = 0;
    Rig rig({ skill('Z', 1000), skill('X', 1000), t, u }, true, true,
            [](MacroConfig& cfg) { cfg.skills_follow_fight = true; }, {}, h);
    // The casts seen 100 ms after their stamp: the second key is free to go
    // (skill_gap_ms after the first's key-up) before the first's cast is in.
    rig.game.delivery_ms = 100;
    rig.run(2'000);
    h->f.mode = MacroEngine::Fight::Mode::Returning;
    rig.run(6'000);
    const std::size_t from = rig.pressed.size();
    h->f.mode = MacroEngine::Fight::Mode::Fighting;
    rig.run(3'000);
    std::uint64_t tu = 0, uu = 0;
    for (std::size_t i = from; i < rig.pressed.size(); ++i) {
        if (rig.pressed[i].first == 'T' && tu == 0) tu = rig.pressed[i].second;
        if (rig.pressed[i].first == 'U' && uu == 0) uu = rig.pressed[i].second;
    }
    const std::uint64_t apart = tu != 0 && uu != 0 ? (tu > uu ? tu - uu : uu - tu) : 0;
    check(rig.game.casts.size() == rig.pressed.size() && apart >= 690 && rig.game.cast_count('T') >= 2 &&
              rig.game.cast_count('U') >= 2,
          "T and U due together after walking back: " + std::to_string(apart) + " ms apart, the second after the "
          "first's cast and lock; every press cast -- " + rig.text(from));

    // A buff and such a key due in the same pass, either order: both cast.
    for (int order = 0; order < 2; ++order) {
        MacroKey b = buff('6', 60'000, 3'000);
        MacroKey v = skill('T', 20'000);       // a skill: under kBuffMinIntervalMs
        v.always_timer = true;
        v.start_delay_ms = 3'000;
        std::vector<MacroKey> keys = { skill('Z', 1000) };
        if (order == 0) {
            keys.push_back(b);
            keys.push_back(v);
        } else {
            keys.push_back(v);
            keys.push_back(b);
        }
        auto g = fighting('Q');
        g->swing_vk = 0;
        Rig pair(keys, true, true, [](MacroConfig& cfg) {
            cfg.skills_follow_fight = true;
            cfg.buff_presses = 1;
        }, {}, g);
        pair.run(6'000);
        const auto bw = pair.when('6'), vw = pair.when('T');
        const std::uint64_t gap = !bw.empty() && !vw.empty() ? (bw[0] > vw[0] ? bw[0] - vw[0] : vw[0] - bw[0]) : 0;
        check(pair.game.cast_count('6') == 1 && pair.game.cast_count('T') == 1 && bw.size() == 1 && vw.size() == 1 &&
                  gap >= 690,
              std::string(order == 0 ? "a buff, then" : "a key on its own timer, then a buff") +
                  (order == 0 ? " a key on its own timer" : "") + ", due together: " + std::to_string(gap) +
                  " ms apart, both cast once");
    }

    // The first one's cast never comes, and no key in the rotation to clear
    // the wait: the second goes kPaceConfirmMs after it, never stuck. (Under
    // kBuffMinIntervalMs: a key that long is a buff, kept apart by buff_spacing.)
    MacroKey t2 = skill('T', 20'000);
    t2.always_timer = true;
    MacroKey u2 = skill('U', 20'000);
    u2.always_timer = true;
    auto s = fighting('Q');
    s->swing_vk = 0;
    Rig lone({ t2, u2 }, true, true, [](MacroConfig& cfg) { cfg.skills_follow_fight = true; }, {}, s);
    lone.game.drop['T'] = 1;
    lone.run(2'000);
    const auto tw = lone.when('T'), uw = lone.when('U');
    const std::uint64_t wait = !tw.empty() && !uw.empty() && uw[0] > tw[0] ? uw[0] - tw[0] : 0;
    check(macro_key_kind(t2) == MacroKeyKind::Skill && lone.engine.pace_status().mode == Mode::Paced &&
              wait >= MacroEngine::kPaceConfirmMs &&
              wait <= MacroEngine::kPaceConfirmMs + 60 && lone.game.cast_count('U') == 1,
          "T's press dropped, no key in the rotation: U " + std::to_string(wait) + " ms after it -- the wait for "
          "T's cast ends on its own");
}

void test_pace_swing_not_ours() {
    section("Paced with the hunter (2026.11.16b): a swing's cast -- or one by hand -- not taken for the key's");

    // A swing of Q goes out right as the lock ends, before the rotation's
    // next key -- the hunter's thread a moment ahead -- every third lock: the
    // game casts Q, drops the key pressed inside Q's lock, and Q's cast comes
    // while the key waits. Taken for the key's, the key's turn went with the
    // key never cast. Given back, the key goes after Q's lock and is cast.
    std::map<std::uint32_t, std::size_t> got_cast, said_cast;
    std::size_t swings = 0, least = 1000;
    const std::string line = logged_text([&] {
        auto h = fighting('Q');
        Rig rig(owner_keys(), true, true, {}, {}, h);
        Rounds r;
        std::uint64_t seen_end = 0;
        int ends = 0;
        for (int i = 0; i < 6'100; ++i) {
            if (rig.game.lock_end == rig.t && rig.t != seen_end) {
                seen_end = rig.t;
                if (++ends % 3 == 0) rig.swing_now('Q');
            }
            rig.step();
            rig.t += 10;
            r.look(rig.engine);
        }
        swings = h->swings.size();
        for (const std::uint32_t vk : kOwnerOrder) {
            got_cast[vk] = rig.game.cast_count(vk);
            said_cast[vk] = r.cast[vk];
            least = std::min(least, rig.game.cast_count(vk));
        }
    }, { "Paced: ", " rounds; " });
    bool own = true;
    for (const std::uint32_t vk : kOwnerOrder) own = own && said_cast[vk] <= got_cast[vk] && said_cast[vk] + 1 >= got_cast[vk];
    check(swings >= 15 && own && least >= 6 && has(line, "; given back ") && !has(line, "; given back 0;") &&
              has(line, "; now paced with the hunter"),
          std::to_string(swings) + " swings of Q ahead of the rotation's key: every turn said cast was the key's own "
          "cast, each key cast " + std::to_string(least) + " times or more -- " + line);

    // By hand -- no swing of the hunter's near it -- once the keys' skills are
    // learned: a cast of another skill answers no key.
    std::size_t hand = 0;
    bool by_hand_own = true;
    {
        // Three rounds and more: each key's skill learned, kPaceSkillLearn
        // casts of it in a row.
        Rig rig(owner_keys());
        rig.run(30'000);
        std::map<std::uint32_t, std::size_t> before;
        for (const std::uint32_t vk : kOwnerOrder) before[vk] = rig.game.cast_count(vk);
        std::map<std::uint32_t, std::size_t> said;
        std::uint64_t rounds = rig.engine.pace_status().rounds;
        std::uint64_t seen_end = 0;
        int ends = 0;
        for (int i = 0; i < 4'000; ++i) {
            if (rig.game.lock_end == rig.t && rig.t != seen_end) {
                seen_end = rig.t;
                if (++ends % 3 == 0) {
                    rig.game.key_down('Q', rig.t);
                    ++hand;
                }
            }
            rig.step();
            rig.t += 10;
            const MacroEngine::PaceStatus st = rig.engine.pace_status();
            if (st.rounds != rounds) {
                rounds = st.rounds;
                for (const auto& s : st.last_round)
                    if (s.turn == Turn::Cast || s.turn == Turn::CastOnRepress) ++said[s.keycode];
            }
        }
        for (const std::uint32_t vk : kOwnerOrder) {
            const std::size_t real = rig.game.cast_count(vk) - before[vk];
            by_hand_own = by_hand_own && said[vk] <= real + 1 && said[vk] + 2 >= real;
        }
    }
    check(hand >= 15 && by_hand_own,
          std::to_string(hand) + " casts of Q by hand, the keys' skills learned: none taken for a key's");
}

void test_pace_hunt_holds() {
    section("Paced with the hunter (2026.11.16b): its holds -- the round waits at its key, then on, no burst");

    using FM = MacroEngine::Fight::Mode;
    using FH = MacroEngine::Fight::Hold;
    const struct {
        FM          mode;
        FH          hold;
        const char* what;
    } kHolds[] = {
        { FM::Held, FH::Turning, "turning" },
        { FM::Held, FH::Drop, "a drop settling" },
        { FM::Held, FH::NoTarget, "nothing to fight in the area" },
        { FM::Held, FH::Outside, "outside the area" },
        { FM::Held, FH::SeekLeg, "a seek leg" },
        { FM::Held, FH::Jump, "a jump up" },
        { FM::Returning, FH::None, "walking back" },
    };
    for (const auto& hold : kHolds) {
        auto h = fighting('Q');
        Rig rig(owner_keys(), true, true, {}, {}, h);
        rig.run(5'030);
        // The key whose turn it is: the one after the last pressed, whose
        // cast has come (every press here is cast).
        rig.run_until([&rig] { return rig.game.inbox.empty(); }, 500);
        const std::size_t before = rig.pressed.size();
        const std::uint32_t last = rig.pressed.back().first;
        std::size_t place = 0;
        while (place < kOwnerOrder.size() && kOwnerOrder[place] != last) ++place;
        const std::uint32_t next = kOwnerOrder[(place + 1) % kOwnerOrder.size()];
        h->f.mode = hold.mode;
        h->f.hold = hold.hold;
        rig.run(3'000);
        const bool none = rig.pressed.size() == before;
        const MacroEngine::Stats s = rig.engine.stats();
        h->f.mode = FM::Fighting;
        h->f.hold = FH::None;
        rig.run(4'000);
        bool spaced = rig.pressed.size() > before;
        for (std::size_t i = before + 1; spaced && i < rig.pressed.size(); ++i)
            spaced = rig.pressed[i].second - rig.pressed[i - 1].second >= 690;
        const std::uint64_t counted = hold.mode == FM::Returning ? s.held_returning : s.held_hunter;
        check(none && spaced && rig.pressed.size() > before && rig.pressed[before].first == next && counted >= 25 &&
                  rig.game.casts.size() == rig.pressed.size(),
              std::string("held 3 s for ") + hold.what + ": nothing pressed, counted " + std::to_string(counted) +
                  " times; then on from " + std::string(1, letter(next)) + ", one key a lock -- " +
                  rig.text(before));
    }
}

void test_pace_long_keys() {
    section("Paced with the hunter (2026.11.16b): no key starved -- the owner's set, and keys of 10 and 30 s");

    // Z 0.5 s; X, C 1 s; L 10 s; M 30 s. Two minutes: L in its place each
    // time its 10 s are over, M each 30 s -- never later than one round more.
    auto h = fighting('Q');
    Rig rig({ skill('Z', 500), skill('X', 1000), skill('C', 1000), skill('L', 10'000), skill('M', 30'000) }, true,
            true, {}, {}, h);
    rig.run(120'000);
    const auto gaps_of = [&rig](std::uint32_t vk) {
        std::vector<std::uint64_t> at;
        for (const auto& c : rig.game.casts)
            if (c.first == vk) at.push_back(c.second);
        std::uint64_t least = ~std::uint64_t{ 0 }, most = 0;
        for (std::size_t i = 1; i < at.size(); ++i) {
            least = std::min(least, at[i] - at[i - 1]);
            most = std::max(most, at[i] - at[i - 1]);
        }
        return std::make_pair(least, most);
    };
    const auto l = gaps_of('L');
    const auto m = gaps_of('M');
    const std::size_t lc = rig.game.cast_count('L'), mc = rig.game.cast_count('M');
    // Due just after its turn went by, a key waits one round more: the three
    // short keys and the other long one, four turns of a lock each.
    const std::uint64_t round = 4 * 760 + 100;
    check(lc >= 10 && mc >= 4 && l.first >= 10'000 && l.second <= 10'000 + round && m.first >= 30'000 &&
              m.second <= 30'000 + round && rig.game.cast_count('Z') >= 40,
          "L cast " + std::to_string(lc) + " times, every " + std::to_string(l.first) + "-" + std::to_string(l.second) +
              " ms; M " + std::to_string(mc) + " times, every " + std::to_string(m.first) + "-" +
              std::to_string(m.second) + " ms; the short ones between");
}

void test_pace_lost_press_gap() {
    section("Paced with the hunter (2026.11.16b): the worst gap after a lost press");

    auto h = fighting('Q');
    Rig rig(owner_keys(), true, true, {}, {}, h);
    rig.run(3'000);
    const std::size_t from = rig.game.casts.size();
    const std::uint64_t clean = longest_cast_gap(rig.game);
    rig.game.drop['C'] = 1;     // its first press lost: cast on the re-press
    rig.run(6'000);
    const std::uint64_t one = longest_cast_gap(rig.game, from);
    const std::size_t mid = rig.game.casts.size();
    rig.game.drop['V'] = 2;     // both lost: skipped, the next key at once
    rig.run(6'000);
    const std::uint64_t two = longest_cast_gap(rig.game, mid);
    check(clean <= 760 && one <= 950 && two <= 1'150,
          "from one cast to the next: " + std::to_string(clean) + " ms at most with none lost, " + std::to_string(one) +
              " ms with a first press lost, " + std::to_string(two) + " ms with both of a key's lost -- never the "
              "1-2 s of 2026-11-15");
}

void test_pace_follow_fight() {
    section("Paced with the hunter (2026.11.16b): the skills following the fight -- its gates held to");

    // Z X C in the rotation; T on its own timer (always_timer), as a buff on
    // the character: fight or none.
    MacroKey t = skill('T', 2000);
    t.always_timer = true;
    auto h = fighting('Q');
    Rig rig({ skill('Z', 1000), skill('X', 1000), skill('C', 1000), t }, true, true,
            [](MacroConfig& cfg) { cfg.skills_follow_fight = true; }, {}, h);
    rig.run(4'000);
    std::string zxc;
    for (const char c : rig.text())
        if (c != 'T') zxc += c;
    // T's casts take their slots too: some four of the rotation's in 4 s.
    const bool fought = strictly(zxc, "ZXC") && zxc.size() >= 4 && rig.engine.pace_status().mode == Mode::Paced &&
                        !rig.when('T').empty();
    // No fight: the rotation waits; T goes on its timer.
    h->f.mode = MacroEngine::Fight::Mode::Idle;
    std::size_t from = rig.pressed.size();
    rig.run(3'000);
    std::size_t rotation_idle = 0, t_idle = 0;
    for (std::size_t i = from; i < rig.pressed.size(); ++i) (rig.pressed[i].first == 'T' ? t_idle : rotation_idle) += 1;
    const std::uint64_t not_fighting = rig.engine.stats().held_not_fighting;
    // Fighting with MP under the floor: the rotation waits.
    h->f.mode = MacroEngine::Fight::Mode::Fighting;
    rig.mp = 5.0f;
    from = rig.pressed.size();
    rig.run(2'000);
    std::size_t rotation_low = 0;
    for (std::size_t i = from; i < rig.pressed.size(); ++i) rotation_low += rig.pressed[i].first != 'T';
    const std::uint64_t low = rig.engine.stats().held_low_mp;
    rig.mp = 100.0f;
    from = rig.pressed.size();
    rig.run(3'000);
    std::size_t rotation_back = 0;
    for (std::size_t i = from; i < rig.pressed.size(); ++i) rotation_back += rig.pressed[i].first != 'T';
    check(fought && rotation_idle == 0 && t_idle >= 1 && not_fighting >= 20 && rotation_low == 0 && low >= 15 &&
              rotation_back >= 3,
          "following the fight: Z X C in turn while it fights; no fight, none of them (" +
              std::to_string(not_fighting) + " looks held) and T on its own timer " + std::to_string(t_idle) +
              " time(s); MP under the floor, none (" + std::to_string(low) + "); back, on again -- " + rig.text());
}

void test_pace_hunt_fallback() {
    section("Paced with the hunter (2026.11.16b): no casts of yours -- the timers as before, the swing the hunter's");

    auto h = fighting('Q');
    Rig rig(owner_keys(), true, false, {}, {}, h);
    rig.game.seen = false;
    rig.run(5'000);
    check(rig.engine.pace_status().mode == Mode::Fallback && rig.when('Z').size() >= 8 &&
              !rig.engine.swing_yields('Q') && h->swings.size() >= 5,
          "no packets: on the timers, Z " + std::to_string(rig.when('Z').size()) + " times in 5 s at 0.5 s, the "
          "hunter swinging its own key (" + std::to_string(h->swings.size()) + ")");
}

void test_pace_hunt_potions() {
    section("Paced with the hunter (2026.11.16b): potions, the pet's and buffs not held back by the rotation");

    MacroKey pet = potion(MacroTrigger::PetHpBelow, '4', 50);
    std::vector<MacroKey> keys = owner_keys();
    keys.push_back(potion(MacroTrigger::HpBelow, '1', 50));
    keys.push_back(pet);
    keys.push_back(buff('6', 60'000, 4'000));
    auto h = fighting('Q');
    Rig rig(keys, true, true, [](MacroConfig& cfg) { cfg.buff_presses = 1; }, {}, h);
    const std::uint64_t start = rig.t;
    rig.pet = 100.0f;
    rig.run(1'500);
    // HP under its mark as a skill is due: the potion's key first, the skill
    // right after its key-up. Up to the next key's press, its cast in, and
    // the step at which its lock ends.
    const std::size_t n0 = rig.pressed.size();
    rig.run_until([&] { return rig.pressed.size() > n0; }, 2'000);
    rig.run_until([&] { return rig.game.inbox.empty(); }, 500);
    rig.run_until([&] {
        const MacroEngine::PaceStatus st = rig.engine.pace_status();
        return rig.t >= st.last_cast_ms + st.lock_ms;
    }, 2'000);
    rig.hp = 40.0f;
    std::size_t had = rig.pressed.size();
    rig.run(100);
    const bool hp_first = rig.pressed.size() >= had + 2 && rig.pressed[had].first == '1' &&
                          rig.pressed[had + 1].first != '1' &&
                          rig.pressed[had + 1].second == rig.pressed[had].second + 80;
    const std::string hp_text = rig.text(had);
    rig.hp = 100.0f;
    // The pet under its mark: its potion within a look.
    rig.run(1'000);
    rig.pet = 30.0f;
    const std::uint64_t pet_at = rig.t;
    rig.run(300);
    const auto p = rig.when('4');
    const bool pet_soon = !p.empty() && p.back() >= pet_at && p.front() <= pet_at + 100;
    rig.pet = 100.0f;
    rig.run(3'000);
    // The buff, due 4 s from the start: at most the lock after it.
    const auto b = rig.when('6');
    check(hp_first && pet_soon && !b.empty() && b[0] >= start + 4'000 &&
              b[0] <= start + 4'000 + MacroEngine::kPaceLockMaxMs + MacroEngine::kPaceConfirmMs &&
              rig.game.cast_count('6') == 1,
          "the hunter fighting: HP's potion first, the skill right after it (" + hp_text + "); the pet's within " +
              std::to_string(p.empty() ? 0 : p.front() - pet_at) + " ms; the buff " +
              std::to_string(b.empty() ? 0 : b[0] - start - 4'000) + " ms after it fell due -- the lock's rest -- "
              "and cast");
}

// ------------------------------------------------------------ even while hunting (2026.11.17)

/// The owner's skills of 2026-11-17: Z X C V B A D, each every 500 ms.
std::vector<MacroKey> owner_keys_1117() {
    return { skill('Z', 500), skill('X', 500), skill('C', 500), skill('V', 500),
             skill('B', 500), skill('A', 500), skill('D', 500) };
}
const std::vector<std::uint32_t> kOwner1117 = { 'Z', 'X', 'C', 'V', 'B', 'A', 'D' };

/// The longest cooldown floor any key has as learned now; 0 none.
std::uint64_t most_floor(const MacroEngine& e) {
    std::uint64_t most = 0;
    for (const auto& f : e.pace_status().floors) most = std::max(most, f.floor_ms);
    return most;
}

const MacroEngine::PaceStatus::Floor* floor_of(const MacroEngine::PaceStatus& st, std::uint32_t vk) {
    for (const auto& f : st.floors)
        if (f.keycode == vk) return &f;
    return nullptr;
}

/// Every line the log had while `run` ran at Info with every one of `words`.
std::vector<std::string> logged_lines(const std::function<void()>& run, std::initializer_list<const char*> words) {
    Logger& log = Logger::instance();
    const LogLevel was = log.level();
    log.set_ring_capacity(8000);
    log.clear_ring();
    log.set_level(LogLevel::Info);
    run();
    std::vector<std::string> found;
    for (const LogLine& l : log.ring()) {
        bool all = true;
        for (const char* w : words) all = all && l.text.find(w) != std::string::npos;
        if (all) found.push_back(l.text);
    }
    log.set_level(was);
    log.set_ring_capacity(0);
    return found;
}

void test_pace_hand_stamps() {
    section("Paced (2026.11.17): a key's skill cast by hand starts its cooldown -- no 24-70 s floor learned from it");

    // 2026-11-17 23:53: X's and Z's skills cast by hand, with no key of
    // GoBot's, between the rotation's own presses of them. Each refusal that
    // followed was measured from the rotation's own last cast, 20-70 s
    // before, and learned as the key's cooldown: Z cast every 24 s, X every
    // 30 for minutes. Here every skill has a 3-s cooldown in the game, and X's
    // or Z's is cast by hand at a lock's end every 4 s, for two minutes.
    Rig rig(owner_keys_1117());
    for (const std::uint32_t vk : kOwner1117) rig.game.cooldown[vk] = 3000;
    rig.run(30'000);
    std::map<std::uint32_t, std::size_t> before, by_hand;
    for (const std::uint32_t vk : kOwner1117) before[vk] = rig.game.cast_count(vk);
    std::uint64_t most = 0, next_hand = rig.t;
    std::size_t hand = 0;
    const std::size_t presses_from = rig.pressed.size();
    for (int i = 0; i < 12'000; ++i) {
        if (rig.t >= next_hand && rig.game.lock_end == rig.t) {
            const std::uint32_t vk = hand % 2 == 0 ? 'X' : 'Z';
            const std::size_t had = rig.game.casts.size();
            rig.game.key_down(vk, rig.t);
            if (rig.game.casts.size() > had) {
                ++hand;
                ++by_hand[vk];
                next_hand = rig.t + 4000;
            }
        }
        rig.step();
        rig.t += 10;
        most = std::max(most, most_floor(rig.engine));
    }
    // Every key's casts, the rotation's and by hand: X's and Z's by hand are
    // casts of theirs all the same.
    std::size_t least = 1000;
    std::string each;
    for (const std::uint32_t vk : kOwner1117) {
        const std::size_t n = rig.game.cast_count(vk) - before[vk];
        least = std::min(least, n);
        each += std::string(each.empty() ? "" : ", ") + letter(vk) + " " + std::to_string(n) +
                (by_hand[vk] != 0 ? " (" + std::to_string(by_hand[vk]) + " by hand)" : "");
    }
    std::vector<std::uint64_t> z, x;
    for (std::size_t i = presses_from; i < rig.pressed.size(); ++i) {
        if (rig.pressed[i].first == 'Z') z.push_back(rig.pressed[i].second);
        if (rig.pressed[i].first == 'X') x.push_back(rig.pressed[i].second);
    }
    const std::uint64_t worst = std::max(longest_gap(z), longest_gap(x));
    // A turn that comes inside the game's cooldown from a cast by hand is
    // passed over, rightly: the key goes a round later -- two rounds, about
    // 10.4 s, at the most.
    check(hand >= 20 && most <= 3'100 && least >= 18 && worst <= 11'000,
          std::to_string(hand) + " casts of X's and Z's skills by hand in 2 minutes: the longest floor learned " +
              std::to_string(most) + " ms (the game's cooldown 3000); casts " + each + "; X and Z pressed again "
              "within " + std::to_string(worst) + " ms (two rounds) -- not passed over for 24-70 s");
}

void test_pace_floor_bounded() {
    section("Paced (2026.11.17): a key the game refuses for 25 s -- its floor no longer than its skill's re-cast");

    // Z X C, a round of some 2.2 s, Z on a 3-s cooldown in the game: its floor
    // learned near its cooldown. Then the game takes no Z for 25 s -- a pile
    // of monsters, 00:21:37-00:22:40 on 2026-11-17, where X's and D's floors
    // went to 70 s at once, each skip measured from a cast long gone. A skip
    // longer since its last cast than its skill was ever cast again after
    // teaches nothing: the floor stays under that re-cast, and Z goes again
    // in its next turn once the game takes it.
    std::uint64_t learned = 0, most = 0, recast = 0, back_ms = 0;
    const std::string line = logged_text([&] {
        Rig rig({ skill('Z', 500), skill('X', 500), skill('C', 500) });
        rig.game.cooldown['Z'] = 3000;
        rig.run(30'000);
        learned = most_floor(rig.engine);
        rig.game.drop['Z'] = 1000;
        for (int i = 0; i < 2'500; ++i) {
            rig.step();
            rig.t += 10;
            most = std::max(most, most_floor(rig.engine));
        }
        const MacroEngine::PaceStatus::Floor* z = floor_of(rig.engine.pace_status(), 'Z');
        recast = z != nullptr ? z->min_recast_ms : 0;
        rig.game.drop['Z'] = 0;
        const std::size_t had = rig.game.cast_count('Z');
        const std::uint64_t on = rig.t;
        rig.run_until([&] { return rig.game.cast_count('Z') > had; }, 10'000);
        back_ms = rig.t - on;
        rig.run(10'000);
    }, { "Paced: ", " rounds; " });
    check(learned >= 1'500 && learned <= 3'000 && most <= recast && most <= 4'500 && back_ms <= 3'000 &&
              has(line, " longer than its re-cast") && !has(line, "; 0 longer than its re-cast"),
          "Z's floor learned at " + std::to_string(learned) + " ms; refused 25 s: the longest floor " +
              std::to_string(most) + " ms, under its shortest re-cast " + std::to_string(recast) + " ms; cast " +
              std::to_string(back_ms) + " ms after the game takes it again -- " + line.substr(line.find("; skips")));
}

void test_pace_no_floor_after_hold() {
    section("Paced (2026.11.17): refusals as a hold ends -- a landing, a turn, a drop -- teach no cooldown");

    // Every 8 s the hunter holds the skills 1.2 s -- a jump's flight, a turn,
    // a drop -- and for 500 ms after it the game takes no key (the character
    // landing, the turn's animation). The keys refused then had been cast a
    // round or so before: their wait was taken as their cooldown, a floor on
    // its second sighting.
    using FH = MacroEngine::Fight::Hold;
    for (const FH hold : { FH::Jump, FH::Turning, FH::Drop }) {
        auto h = fighting('Q');
        h->swing_vk = 0;
        std::uint64_t most = 0;
        std::size_t cooling = 0;
        const std::string line = logged_text([&] {
            Rig rig(owner_keys_1117(), true, true, {}, {}, h);
            for (const std::uint32_t vk : kOwner1117) rig.game.cooldown[vk] = 3000;
            rig.run(5'000);
            Rounds r;
            for (int i = 0; i < 8; ++i) {
                h->f.mode = MacroEngine::Fight::Mode::Held;
                h->f.hold = hold;
                rig.run(1'200);
                h->f.mode = MacroEngine::Fight::Mode::Fighting;
                h->f.hold = FH::None;
                rig.game.casting = false;
                rig.run(500);
                rig.game.casting = true;
                for (int s = 0; s < 630; ++s) {
                    rig.step();
                    rig.t += 10;
                    most = std::max(most, most_floor(rig.engine));
                    r.look(rig.engine);
                }
            }
            for (const std::uint32_t vk : kOwner1117) cooling += r.turns[vk] - r.cast[vk] - r.skipped[vk];
            rig.run(1'000);
        }, { "Paced: ", " rounds; " });
        check(most == 0 && has(line, "; skips taught 0, not taught: ") && !has(line, "not taught: 0 after a hold"),
              std::string(hold == FH::Jump ? "a jump's flight" : hold == FH::Turning ? "a turn" : "a drop") +
                  " 8 times, the game taking no key 500 ms after each: no floor learned (the longest " +
                  std::to_string(most) + " ms) -- " + line.substr(line.find("; skips taught")));
    }
}

void test_pace_owner_hunt() {
    section("Paced with the hunter (2026.11.17): the owner's set, the dash not learned, near an edge -- every key "
            "9-12 casts a minute, C and V too");

    // The owner's seven keys every 500 ms, the hunter's attack key Z one of
    // them, the character near the area's edge all the while and the dash
    // not learned: since 2026.11.17 no key is held for it (C and V got about
    // one cast a minute so). Every skill on a 3-s cooldown in the game. The
    // bad minute's pattern on top: X, Z, C, X and V cast by hand at 18.9,
    // 27.9, 28.3, 29.0 and 29.5 s; a walk back of 0.5 s after each of D's
    // casts (its 300-px throw out of the box); and from 35 to 45 s a pile of
    // monsters, the game's lock 1 s long and the game taking no key for
    // 300 ms every 2 s.
    // The keys held as the dash are a real hunter's with none learned, as
    // GoBot's window hands them to the macro (Fight::dash_keys).
    std::array<std::uint32_t, 3> unlearned{};
    {
        InputManager input;
        ExecutionCoordinator coord;
        MobDetector detector;
        Hunter hunter(input, coord, detector);
        unlearned = hunter.dash_keys();
    }
    auto h = fighting('Z');
    h->f.dash_edge = true;
    h->f.dash_keys = unlearned;
    Rig rig(owner_keys_1117(), true, true, {}, {}, h);
    for (const std::uint32_t vk : kOwner1117) rig.game.cooldown[vk] = 3000;
    rig.packets = true;
    rig.run(30'000);
    std::map<std::uint32_t, std::size_t> before, by_hand;
    for (const std::uint32_t vk : kOwner1117) before[vk] = rig.game.cast_count(vk);
    const std::uint64_t m0 = rig.t;
    const struct {
        std::uint64_t at;
        std::uint32_t vk;
    } kHands[] = { { 18'900, 'X' }, { 27'900, 'Z' }, { 28'300, 'C' }, { 29'000, 'X' }, { 29'500, 'V' } };
    std::size_t hi = 0, d_seen = rig.game.cast_count('D');
    std::uint64_t back_until = 0, most = 0;
    const std::size_t casts_from = rig.game.casts.size();
    while (rig.t < m0 + 60'000) {
        const std::uint64_t in = rig.t - m0;
        if (hi < std::size(kHands) && in >= kHands[hi].at) {
            const std::size_t had = rig.game.casts.size();
            rig.game.key_down(kHands[hi].vk, rig.t);
            if (rig.game.casts.size() > had) ++by_hand[kHands[hi].vk];
            ++hi;
        }
        const bool pile = in >= 35'000 && in < 45'000;
        if (pile) rig.game.lock_of = [] { return std::uint64_t{ 1000 }; };
        else rig.game.lock_of = nullptr;
        rig.game.casting = !(pile && in % 2'000 < 300);
        if (rig.game.cast_count('D') > d_seen) {
            d_seen = rig.game.cast_count('D');
            h->f.mode = MacroEngine::Fight::Mode::Returning;
            back_until = rig.t + 500;
        }
        if (back_until != 0 && rig.t >= back_until) {
            h->f.mode = MacroEngine::Fight::Mode::Fighting;
            back_until = 0;
        }
        rig.step();
        rig.t += 10;
        most = std::max(most, most_floor(rig.engine));
    }
    std::size_t least = 1000, most_casts = 0;
    std::string each;
    for (const std::uint32_t vk : kOwner1117) {
        const std::size_t n = rig.game.cast_count(vk) - before[vk] - by_hand[vk];
        least = std::min(least, n);
        most_casts = std::max(most_casts, n);
        each += std::string(each.empty() ? "" : ", ") + letter(vk) + " " + std::to_string(n);
    }
    // From one cast of the game's to the next, outside the walks back.
    std::vector<std::uint64_t> gaps;
    for (std::size_t i = casts_from + 1; i < rig.game.casts.size(); ++i)
        gaps.push_back(rig.game.casts[i].second - rig.game.casts[i - 1].second);
    std::sort(gaps.begin(), gaps.end());
    const std::uint64_t p50 = gaps.empty() ? 0 : gaps[gaps.size() / 2];
    check(unlearned == std::array<std::uint32_t, 3>{ 0, 0, 0 } && least >= 9 && most_casts <= 13 && most <= 3'100 &&
              h->swings.empty() && p50 <= 800 && rig.engine.stats().held_dash == 0,
          "a minute of the owner's set hunting, near an edge with no dash learned (no key held for it): " + each + " casts by the rotation (" + std::to_string(least) + "-" +
              std::to_string(most_casts) + "), no floor over " + std::to_string(most) + " ms, the gaps' middle " +
              std::to_string(p50) + " ms, the attack key left to the rotation");

    // The dash learned as D, the character at the edge: D alone passes its
    // turn; the rest go on.
    auto e = fighting('Q');
    e->swing_vk = 0;
    e->f.dash_edge = true;
    e->f.dash_keys = { 'D', 0, 0 };
    Rig edge(owner_keys_1117(), true, true, {}, {}, e);
    edge.run(20'000);
    bool others = true;
    for (const std::uint32_t vk : kOwner1117)
        if (vk != 'D') others = others && edge.game.cast_count(vk) >= 4;
    check(edge.when('D').empty() && others && edge.engine.stats().held_dash > 0,
          "the dash learned as D, at the edge: D passed over (" + std::to_string(edge.engine.stats().held_dash) +
              " counted), every other key cast in turn -- " + edge.text().substr(0, 18));
}

void test_pace_no_flood() {
    section("Paced (2026.11.17): no casts while the packets come -- a key a second, no flood; the character "
            "down -- no skill; the capture gone -- the timers at the lock's pace");

    // 00:07:19 on 2026-11-17: the character dead, the packets' HP stale at
    // 27%, no cast for 2 s -- the timers took over and pressed 220 keys a
    // minute. The packets still coming, the game taking no key: the rotation
    // stays and probes, a key a second, learning nothing; the first cast
    // brings the round back.
    std::size_t probe_presses = 0, all_presses = 0;
    bool stayed = true, said = false;
    std::uint64_t back_ms = 0, most = 0;
    std::size_t after = 0;
    const std::string line = logged_text([&] {
        auto h = fighting('Q');
        h->swing_vk = 0;
        Rig rig(owner_keys_1117(), true, true, {}, {}, h);
        rig.packets = true;
        rig.run(10'000);
        rig.game.casting = false;
        const std::size_t from = rig.pressed.size();
        std::size_t probe_from = 0;
        for (int i = 0; i < 3'000; ++i) {
            rig.step();
            rig.t += 10;
            stayed = stayed && rig.engine.pace_status().mode == Mode::Paced;
            most = std::max(most, most_floor(rig.engine));
            if (probe_from == 0 && rig.engine.pace_status().probing) probe_from = rig.pressed.size();
        }
        said = rig.engine.pace_status().probing;
        all_presses = rig.pressed.size() - from;
        probe_presses = probe_from == 0 ? all_presses : rig.pressed.size() - probe_from;
        rig.game.casting = true;
        const std::uint64_t on = rig.t;
        rig.run_until([&] { return !rig.engine.pace_status().probing; }, 3'000);
        back_ms = rig.t - on;
        const std::size_t back_from = rig.pressed.size();
        rig.run(10'000);
        after = rig.pressed.size() - back_from;
    }, { "Paced: no cast of yours for ", "the packets coming" });
    // The steps' 30 s and each press's 80 ms hold: some 32 s in all.
    check(stayed && said && probe_presses <= 33 && probe_presses >= 26 && all_presses <= 45 && most == 0 &&
              back_ms <= 1'300 && after >= 12 && has(line, "one key a second until your next cast"),
          "30 s of no cast, the packets coming: paced all along, " + std::to_string(all_presses) + " presses, " +
              std::to_string(probe_presses) + " of them a key a second once 2 s had shown none, nothing learned; "
              "the game taking keys again, the round back in " +
              std::to_string(back_ms) + " ms, " + std::to_string(after) + " presses in the next 10 s -- " + line);

    // The character down -- HP read at 0 -- the skills not following the
    // fight: no skill key, in the rotation or on its timer; the potion drinks.
    {
        Rig rig({ skill('Z', 500), skill('X', 500), skill('C', 500), potion(MacroTrigger::HpBelow, '1', 50) });
        rig.packets = true;
        rig.run(5'000);
        rig.hp = 0.3f;
        rig.run(1'000);
        const std::size_t from = rig.pressed.size();
        rig.run(30'000);
        std::size_t skills = 0, drinks = 0;
        for (std::size_t i = from; i < rig.pressed.size(); ++i) (rig.pressed[i].first == '1' ? drinks : skills) += 1;
        check(skills == 0 && drinks >= 3 && rig.engine.stats().held_dead > 0,
              "HP read at 0, not following the fight: no skill pressed in 30 s, the potion " + std::to_string(drinks) +
                  " times, held as down " + std::to_string(rig.engine.stats().held_dead) + " times");
        // ... and on the timers too, with no casts ever seen.
        Rig timers({ skill('Z', 500), skill('X', 500), potion(MacroTrigger::HpBelow, '1', 50) }, true, false);
        timers.game.seen = false;
        timers.run(2'000);
        timers.hp = 0.3f;
        timers.run(1'000);
        const std::size_t t_from = timers.pressed.size();
        timers.run(10'000);
        std::size_t t_skills = 0, t_drinks = 0;
        for (std::size_t i = t_from; i < timers.pressed.size(); ++i)
            (timers.pressed[i].first == '1' ? t_drinks : t_skills) += 1;
        check(timers.engine.pace_status().mode == Mode::Fallback && t_skills == 0 && t_drinks >= 1,
              "on the timers, HP read at 0: no skill pressed in 10 s, the potion " + std::to_string(t_drinks) +
                  " times");
    }

    // The capture gone -- no packets, no casts -- after casts were seen: the
    // timers, a key a lock at most, each in turn: the standing rate, not 230
    // a minute.
    {
        auto h = fighting('Q');
        h->swing_vk = 0;
        Rig rig(owner_keys_1117(), true, true, {}, {}, h);
        rig.run(5'000);
        rig.game.seen = false;
        rig.run_until([&] { return rig.engine.pace_status().mode == Mode::Fallback; }, 5'000);
        const std::size_t from = rig.pressed.size();
        rig.run(60'000);
        const std::size_t n = rig.pressed.size() - from;
        std::uint64_t least = ~std::uint64_t{ 0 };
        for (std::size_t i = from + 1; i < rig.pressed.size(); ++i)
            least = std::min(least, rig.pressed[i].second - rig.pressed[i - 1].second);
        check(rig.engine.pace_status().mode == Mode::Fallback && n >= 70 && n <= 86 && least >= 680,
              "the capture gone: on the timers, " + std::to_string(n) + " skill presses in a minute, " +
                  std::to_string(least) + " ms apart at least -- not 220-230");
    }
}

void test_pace_mismatch_owner() {
    section("Paced (2026.11.17): another key's skill cast by hand while X waits -- X keeps its skill");

    // 23:46:56 on 2026-11-17: X waited while Z's 51205 came by hand, three
    // times, no swing near -- "key X cast skill 51205 3 times in a row, not
    // the 51203 learned for it -- learned afresh". A skill another key is
    // known to cast is that key's: never counted against X.
    const auto run = [](std::uint32_t foreign) {
        return logged_text([foreign] {
            Rig rig(owner_keys_1117());
            rig.run(30'000);
            rig.game.drop['X'] = 3;
            std::size_t seen = rig.pressed.size();
            int fed = 0;
            for (int i = 0; i < 2'000 && fed < 3; ++i) {
                rig.step();
                for (; seen < rig.pressed.size(); ++seen) {
                    if (rig.pressed[seen].first != 'X') continue;
                    const std::uint64_t at = rig.pressed[seen].second + 30;
                    rig.game.inbox.push_back(Game::Due{ at + 10, at, foreign });
                    ++fed;
                }
                rig.t += 10;
            }
            rig.run(5'000);
        }, { "Paced: key X cast skill " });
    };
    const std::string z = run(50'000 + 'Z');
    const std::string unknown = run(99'999);
    check(z.empty() && has(unknown, "Paced: key X cast skill 99999 3 times in a row, not the ") &&
              has(unknown, " learned for it -- learned afresh"),
          "Z's skill three times while X waited: X keeps its own (no line); a skill no key casts, three times: "
          "learned afresh as before -- " + unknown);
}

void test_pace_arm_clears() {
    section("Paced (2026.11.17): a stop and a start -- the cooldowns learned afresh, not measured from before");

    // S on an 8-s cooldown: its floor learned in a minute and a half. Stopped
    // for 70 s and started again, its first press dropped twice: before, the
    // floor kept over the stop was raised to the 70 s since S's last cast.
    Rig rig(owner_keys());
    rig.game.cooldown['S'] = 8000;
    rig.run(90'000);
    const MacroEngine::PaceStatus::Floor* learned = floor_of(rig.engine.pace_status(), 'S');
    const std::uint64_t had = learned != nullptr ? learned->floor_ms : 0;
    rig.engine.stop();
    rig.t += 70'000;
    rig.engine.start_stepped();
    rig.game.drop['S'] = 2;
    std::uint64_t most = 0;
    for (int i = 0; i < 3'000; ++i) {
        rig.step();
        rig.t += 10;
        most = std::max(most, most_floor(rig.engine));
    }
    const MacroEngine::PaceStatus st = rig.engine.pace_status();
    const MacroEngine::PaceStatus::Floor* s = floor_of(st, 'S');
    check(had >= 5'000 && had <= 8'000 && most <= MacroEngine::kPaceFloorMaxMs && s != nullptr && s->known_skill != 0 &&
              st.mode == Mode::Paced,
          "S's floor " + std::to_string(had) + " ms before the stop; after it, S's first press dropped twice: the "
          "longest floor " + std::to_string(most) + " ms, not the 70 s since its cast; its skill still known");
}

void test_pace_check_backoff() {
    section("Paced (2026.11.17 review): a check under its floor not cast -- the next waits twice the turns, up to "
            "kPaceFloorUses; never 3 turns every time");

    // S on a 10-s cooldown beside Z alone: while S cools the round is Z, some
    // 0.7 s, so 3 turns fit in S's floor three times over. Before the fix a
    // check refused taught a candidate under the floor, and the next cast's
    // raise -- the floor unchanged -- put the gap back to 3 turns: S checked
    // every ~2 s for its whole cooldown, 3-4 wasted presses each cycle.
    Rig rig({ skill('Z', 500), skill('S', 500) });
    rig.game.cooldown['S'] = 10'000;
    rig.run(60'000);
    const std::size_t pressed_from = rig.when('S').size(), cast_from = rig.game.cast_count('S');
    rig.run(60'000);
    const std::size_t pressed = rig.when('S').size() - pressed_from;
    const std::size_t cast = rig.game.cast_count('S') - cast_from;
    const std::size_t refused = pressed - cast;
    const MacroEngine::PaceStatus::Floor* s = floor_of(rig.engine.pace_status(), 'S');
    check(cast >= 5 && refused <= 8 && s != nullptr && s->floor_ms != 0,
          "the second minute: S cast " + std::to_string(cast) + " times with " + std::to_string(refused) +
              " presses not cast (3 turns every time made some 18), its floor " +
              std::to_string(s != nullptr ? s->floor_ms : 0) + " ms");
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

    // On, and the skills follow the fight with a hunter fighting: paced since
    // 2026.11.16b -- the rotation has them while it fights.
    Rig follow(owner_keys(), true, true, [](MacroConfig& cfg) { cfg.skills_follow_fight = true; }, [] {
        MacroEngine::Fight f;
        f.mode = MacroEngine::Fight::Mode::Fighting;
        f.dx = 50;
        return f;
    });
    follow.run(3'000);
    check(follow.engine.pace_status().mode == Mode::Paced && follow.text().rfind("ZXCV", 0) == 0 &&
              follow.game.casts.size() == follow.pressed.size(),
          "on, and following the fight with a hunter fighting: paced, every press cast -- " + follow.text());
}

// ------------------------------------------------------------ macro.ini

void test_pace_config() {
    section("Paced on your casts: on by default since 2026.11.16b -- an old file's false once, an untick kept");

    const std::string path = temp_path("gobot_pace.ini");
    const auto write = [&path](const char* text) {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f) {
            std::fputs(text, f);
            std::fclose(f);
        }
    };
    MacroConfig cfg;
    cfg.apply_defaults();
    check(cfg.cast_paced && MacroConfig{}.cast_paced && std::string(MacroConfig::kCastPacedKey) == "cast_paced_v2",
          "on in a config built in code");
    MacroConfig back;
    IniFile raw;
    check(cfg.save(path) && back.load(path) && back.cast_paced && raw.load(path) &&
              raw.get("Macro", "cast_paced_v2", "") == "true" && raw.get("Macro", "cast_paced", "") == "true",
          "saved as cast_paced_v2=true, the old cast_paced beside it the same, and read back on");

    // The owner's own file of 2026-11-15: cast_paced=false, written back by
    // every save since 2026.11.08 shipped it off, and no cast_paced_v2 --
    // read on once; the next save makes it the player's.
    write("[Macro]\ntimers_enabled=true\ncast_paced=false\n[Keys]\nkey_0_keycode=90\nkey_0_enabled=true\n"
          "key_0_interval_ms=1200\n");
    MacroConfig old;
    MacroConfig resaved;
    IniFile old_raw;
    check(old.load(path) && old.cast_paced && old.save(path) && old_raw.load(path) &&
              old_raw.get("Macro", "cast_paced_v2", "") == "true" && resaved.load(path) && resaved.cast_paced,
          "a macro.ini from before 2026.11.16b saying cast_paced=false: on, and saved on with cast_paced_v2=true");
    // An untick from now on: kept across restarts, whatever the old key says.
    MacroConfig unticked = resaved;
    unticked.cast_paced = false;
    MacroConfig restart;
    MacroConfig again;
    check(unticked.save(path) && restart.load(path) && !restart.cast_paced && restart.save(path) && again.load(path) &&
              !again.cast_paced,
          "unticked in 2026.11.16b: saved as cast_paced_v2=false and kept off over two restarts");
    write("[Macro]\ncast_paced=true\ncast_paced_v2=false\n[Keys]\nkey_0_keycode=90\nkey_0_enabled=true\n");
    MacroConfig hand;
    check(hand.load(path) && !hand.cast_paced, "cast_paced_v2=false wins over an old cast_paced=true");
    write("[Macro]\ntimers_enabled=true\n[Keys]\nkey_0_keycode=90\nkey_0_enabled=true\nkey_0_interval_ms=1200\n");
    MacroConfig without;
    check(without.load(path) && without.cast_paced, "a macro.ini with neither key: on");
    std::remove(path.c_str());
#ifdef GOBOT_SOURCE_DIR
    const std::string shipped = std::string(GOBOT_SOURCE_DIR) + "/configs/macro.ini";
    MacroConfig ships;
    IniFile ships_raw;
    if (ships.load(shipped) && ships_raw.load(shipped))
        check(ships.cast_paced && ships_raw.get("Macro", "cast_paced_v2", "") == "true" &&
                  ships_raw.get("Macro", "cast_paced", "") == "true",
              "the shipped macro.ini says cast_paced_v2=true, and cast_paced=true beside it");
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

/// Whether each of `parts` is in `text`, each after the one before.
bool in_order(const std::string& text, std::initializer_list<const char*> parts) {
    std::size_t at = 0;
    for (const char* p : parts) {
        at = text.find(p, at);
        if (at == std::string::npos) return false;
        at += std::char_traits<char>::length(p);
    }
    return true;
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
    // Since 2026.11.16b with the hunter running and with the skills following
    // the fight too: only the key sequence keeps it off, and run_paced has the
    // skills before run_skills.
    check(has(timers, "const bool paced = pace_pass(cfg, fight, sequence, now);") &&
              has(timers, "const Fight fight = fight_now();") &&
              has(pass_body, "const bool off_sequence = cfg.cast_paced && sequence;") &&
              has(pass_body, "if (!cfg.cast_paced || sequence) {") && !has(pass_body, "follow ||") &&
              !has(timers, "fight.mode != Fight::Mode::NoHunter ||") &&
              has(timers, "if (paced && macro_key_kind(k) == MacroKeyKind::Skill && !own_timer) continue;") &&
              in_order(timers, { "if (paced) run_paced(cfg, follow, clock_now());",
                                 "else if (follow) run_skills(cfg, clock_now());" }) &&
              has(timers, "if (cfg.cast_paced) log_paced(clock_now());"),
          "run_timers: the skill keys the rotation's while paced, the hunter running or not, following the fight or "
          "not; the timers' or run_skills' otherwise, as before; the rotation off with its switch or the key "
          "sequence whatever the rest says");
    // A buff paced: held while a skill's cast is waited for or inside the
    // last cast's lock; its wait counted from its own key-down. So too a skill
    // on its own timer beside the rotation while the skills follow the fight.
    const std::string repeats = body(macro, "void MacroEngine::run_buff_repeats(");
    // And (2026.11.16b review) each waits for the cast of the one of them
    // pressed just before -- until it shows, or kPaceConfirmMs on its own.
    check(has(timers, "const bool lock_waits = buff || (own_timer && macro_key_kind(k) == MacroKeyKind::Skill);") &&
              in_order(timers, { "if (lock_waits && paced &&",
                                 "(pace_press_.active || now < pace_last_cast_ms_ + pace_lock_ms_ ||",
                                 "(pace_buff_ms_ != 0 && now < pace_buff_ms_ + kPaceConfirmMs)))", "continue;",
                                 "if (buff) {" }) &&
              has(timers, "if (lock_waits && paced) pace_buff_ms_ = key_down;") &&
              has(repeats, "if (pace_mode_ == PaceStatus::Mode::Paced) pace_buff_ms_ = key_down;") &&
              !has(timers, "pace_buff_ms_ = now;") && !has(repeats, "pace_buff_ms_ = now;"),
          "a buff due inside a lock waits it out; its wait stamped at its key-down, not the pass's start");
    check(count_in(pace, "press(k.keycode, k.hold_ms, \"macro-paced\", &why)") == 2 && !has(pace, "key_press(") &&
              !has(pace, "SendInput") && !has(pace, "send_key(") && count_in(pace, "potions_first(cfg, clock_now())") == 2,
          "macro_pace.cpp presses only through press(), the potions looked at before each of its two presses");
    // 2026.11.16b: the hunter's swing told apart, and left to the rotation.
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string hunter = source("/src/hunter.cpp");
    const std::string jump = source("/src/hunter_jump.cpp");
    const std::string cli_src = source("/apps/gobot_cli/main.cpp");
    check(has(gui, "hunter_->set_swing_yield([this](std::uint32_t vk) { return macro_ && macro_->swing_yields(vk); });") &&
              has(gui, "f.swing_ms = std::max(swing_ms_.load(), hunter_->last_swing_ms());") &&
              has(cli_src, "f.swing_ms = hunter.last_swing_ms();") &&
              // 2026.11.17: stamped before the press, so a swing's cast is
              // never taken for a key's own before its time is known.
              in_order(hunter, { "swing_down_ms_.store(down_ms);",
                                 "if (press(cfg.attack_key, 60) && on_attack_) on_attack_(cfg.attack_key, down_ms);" }) &&
              in_order(jump, { "swing_down_ms_.store(down_ms);",
                               "if (press(cfg.attack_key, 60) && on_attack_) on_attack_(cfg.attack_key, down_ms);" }) &&
              has(body(more, "void GuiApp::more_note_attack("), "swing_ms_.store(at_ms);") &&
              has(hunter, "} else if (now - last_attack_ms_ >= cfg.attack_interval_ms && swing_yields(cfg.attack_key)) {") &&
              has(jump, "if (now - last_attack_ms_ >= cfg.attack_interval_ms && !swing_yields(cfg.attack_key)) {") &&
              has(pace, "swing_yield_vk_.store(yield);"),
          "the hunter's swing: its key-down to the macro's fight, and left to the rotation where swing_yields says "
          "so -- both of the hunter's swings, in reach and in a jump's flight");
    // The review of 2026.11.16b: the attack key's reach (F3) to the macro's
    // fight, said before the fight is; and kept by the rotation, following it.
    const std::string cli = source("/apps/gobot_cli/main.cpp");
    const std::string fought = body(hunter, "void Hunter::disengage() {");
    check(has(gui, "f.attack_far = hunter_->attack_far();") && has(cli, "f.attack_far = hunter.attack_far();") &&
              in_order(hunter, { "const bool too_far = limit > 0 && world > static_cast<double>(limit);",
                                 "if (facing && !held && focused()) {", "attack_far_now_.store(too_far);",
                                 "engage(adx, now);" }) &&
              has(fought, "attack_far_now_.store(false);") &&
              in_order(jump, { "attack_far_now_.store(false);", "engage(std::abs(dx), now);" }) &&
              has(pace, "return follow && fight.mode == Fight::Mode::Fighting && fight.attack_far && "
                        "fight.attack_key != 0 &&") &&
              has(pace, "if (waits(k)) return Turn::Waiting;") &&
              has(pace, "} else if (held || waits(cfg.keys[rot[at].i])) {"),
          "the attack key past the hunter's reach: said to the macro by the GUI and the CLI, set before engage(), "
          "cleared on disengage() and in a jump's flight; the rotation passes its turn, following the fight");
    check(has(pace, "const bool yield_kept = !returning && !dead && (hunter_hold ? fight.hold == Fight::Hold::Turning ||") &&
              has(pace, "fight.hold == Fight::Hold::NoTarget") &&
              has(pace, ": gate == SkillGate::Pass || gate == SkillGate::NotFighting);") &&
              has(pace, "if (yield_kept && fight.attack_key != 0) {"),
          "the swing's hand-off kept under a turn, nothing to fight and no fight -- cleared under every other hold");
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
    test_pace_hunting();
    test_pace_attack_key_both();
    test_pace_swing_after_hold();
    test_pace_attack_far();
    test_pace_own_timer_pair();
    test_pace_swing_not_ours();
    test_pace_hunt_holds();
    test_pace_long_keys();
    test_pace_lost_press_gap();
    test_pace_follow_fight();
    test_pace_hunt_fallback();
    test_pace_hunt_potions();
    test_pace_hand_stamps();
    test_pace_floor_bounded();
    test_pace_no_floor_after_hold();
    test_pace_owner_hunt();
    test_pace_no_flood();
    test_pace_mismatch_owner();
    test_pace_arm_clears();
    test_pace_check_backoff();
    test_pace_off_same();
    test_pace_config();
    test_pace_threads();
    test_pace_wiring();
}
