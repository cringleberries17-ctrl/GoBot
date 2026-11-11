// gobot_test -- the hold's key window (2026.11.10, the owner: "Khi dọn rác thì
// cần ra các skill thuộc phím chữ cái", "cả khi mua HP và MP cũng cần ra
// skill", "có tính phím space", "lúc dùng bùa thì không cần ra skill"): which
// keys may ever go through, the coordinator's window -- armed, opened, shut and
// disarmed, beside the hold and never keyed on its reason -- the macro's keys
// through it on a clock of the test's own, the quiet second, and where GoBot's
// window wires it in, read from its source.
//
// Nothing here goes near the game: a pretend keyboard, the engine stepped on a
// clock of the test's own, and the coordinator's own waits on the real clock
// for a few hundred milliseconds at most. What a bag job does with the window
// on a pretend game is in use_test.cpp (test_key_window).

#include "gobot/bag.hpp"
#include "gobot/common.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/macro.hpp"
#include "gobot/quiet_second.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
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

constexpr const char* kTrash = "trash cleaning";

MacroKey key(std::uint32_t vk, std::uint32_t interval_ms, bool always = true) {
    MacroKey k;
    k.keycode = vk;
    k.label = "Skill";
    k.enabled = true;
    k.interval_ms = interval_ms;
    k.jitter_pct = 0;
    k.always_timer = always;
    return k;
}

MacroKey potion(MacroTrigger trigger, std::uint32_t vk, int pct) {
    MacroKey k = key(vk, 3000, false);
    k.label = "Potion";
    k.trigger = trigger;
    k.threshold_pct = pct;
    return k;
}

MacroKey buff(std::uint32_t vk, std::uint32_t interval_ms) {
    MacroKey k = key(vk, interval_ms, false);
    k.label = "Buff";
    k.buff = true;
    return k;
}

/// The engine on a clock of the test's own, with a pretend keyboard and the
/// vitals pushed every 10 ms; no hunter. The coordinator is the real one.
struct Rig {
    InputManager         input;
    ExecutionCoordinator coord;
    MacroEngine          engine{ input, coord };
    std::uint64_t        t = 21'000'000;
    float                hp = 100.0f, mp = 100.0f, pet = -1.0f;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> pressed;

    explicit Rig(std::vector<MacroKey> keys, std::vector<MacroStep> steps = {}) {
        MacroEngine::Clock c;
        c.now_ms = [this] { return t; };
        c.sleep_ms = [this](std::uint32_t ms) { t += ms; };
        engine.set_clock(c);
        MacroEngine::Hands h;
        h.focused = [] { return true; };
        h.key = [this](std::uint32_t vk, int hold) {
            pressed.emplace_back(vk, t);
            if (hold > 0) t += static_cast<std::uint64_t>(hold);
            return true;
        };
        engine.set_hands(h);
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys = std::move(keys);
        cfg.sequence = std::move(steps);
        cfg.sequence_enabled = !cfg.sequence.empty();
        cfg.sequence_loop = true;
        cfg.global_jitter_pct = 0;
        cfg.skills_follow_fight = false;
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
    /// The hold of a bag job, its window armed with `keys` (potions among
    /// them `potions`) and open.
    void bag_job(std::vector<std::uint32_t> keys, std::vector<std::uint32_t> potions, bool open = true) {
        coord.engage_critical(kTrash);
        PassKeys p;
        p.keys = std::move(keys);
        p.potions = std::move(potions);
        coord.arm_pass_keys(p);
        if (open) coord.open_pass_keys();
    }
};

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ------------------------------------------------------------- the keys

void test_allowed() {
    section("Key window: only a letter A-Z, Space, or exactly Home, End, PgUp, PgDn, Insert and Delete may ever go "
            "through (2026.11.11) -- every one of the 256 codes swept");
    const std::uint32_t six[] = { VK_PRIOR, VK_NEXT, VK_END, VK_HOME, VK_INSERT, VK_DELETE };
    check(six[0] == 0x21 && six[1] == 0x22 && six[2] == 0x23 && six[3] == 0x24 && six[4] == 0x2D && six[5] == 0x2E,
          "the six by their codes: PgUp 0x21, PgDn 0x22, End 0x23, Home 0x24, Insert 0x2D, Delete 0x2E");
    std::string wrong, wrong_nav;
    int allowed = 0, nav = 0;
    for (std::uint32_t vk = 0; vk < 256; ++vk) {
        const bool is_six = std::find(std::begin(six), std::end(six), vk) != std::end(six);
        const bool want = (vk >= 'A' && vk <= 'Z') || vk == VK_SPACE || is_six;
        if (pass_key_allowed(vk) != want) wrong += " " + std::to_string(vk);
        if (pass_key_nav(vk) != is_six) wrong_nav += " " + std::to_string(vk);
        allowed += pass_key_allowed(vk) ? 1 : 0;
        nav += pass_key_nav(vk) ? 1 : 0;
    }
    check(wrong.empty() && allowed == 33, "26 letters, Space and the six -- 33, nothing else of 0-255 --" + wrong);
    check(wrong_nav.empty() && nav == 6, "pass_key_nav: the six alone, never a letter or Space --" + wrong_nav);
    // Never a range: what lies between the six, and every other class.
    bool between = true;
    for (std::uint32_t vk = 0x25; vk <= 0x2C; ++vk) between = between && !pass_key_allowed(vk);
    bool digits = true, pad = true, fkeys = true;
    for (std::uint32_t vk = '0'; vk <= '9'; ++vk) digits = digits && !pass_key_allowed(vk);
    for (std::uint32_t vk = VK_NUMPAD0; vk <= VK_DIVIDE; ++vk) pad = pad && !pass_key_allowed(vk);
    for (std::uint32_t vk = VK_F1; vk <= VK_F24; ++vk) fkeys = fkeys && !pass_key_allowed(vk);
    check(between && !pass_key_allowed(VK_LEFT) && !pass_key_allowed(VK_UP) && !pass_key_allowed(VK_RIGHT) &&
              !pass_key_allowed(VK_DOWN) && !pass_key_allowed(VK_SELECT) && !pass_key_allowed(VK_SNAPSHOT),
          "0x25-0x2C between them never: the arrows (0x28 is ↓), Select, Print, Execute, Print Screen");
    check(digits && pad && fkeys, "never a digit 0-9 (the owner: \"Không cho phím số\"), a number-pad key -- digits "
                                  "and . * + - / -- nor F1-F24");
    bool none_folded = true;
    for (const std::uint32_t vk : { 0x100u | 'Z', 0x10000u | 'Z', 0x15Au, 0x120u, 0xFFFFFFFFu, 0x124u, 0x10024u,
                                    0x12Eu })
        none_folded = none_folded && !pass_key_allowed(vk) && !pass_key_nav(vk);
    check(none_folded && !pass_key_allowed('0') && !pass_key_allowed('9') && !pass_key_allowed(VK_RETURN) &&
              !pass_key_allowed(VK_ESCAPE) && !pass_key_allowed(VK_LEFT) && !pass_key_allowed(VK_DOWN) &&
              !pass_key_allowed(VK_CONTROL) && !pass_key_allowed(VK_SHIFT) && !pass_key_allowed(VK_MENU) &&
              !pass_key_allowed(VK_LWIN) && !pass_key_allowed(VK_RWIN) && !pass_key_allowed(VK_LCONTROL) &&
              !pass_key_allowed(VK_F12) && !pass_key_allowed(VK_OEM_3) && !pass_key_allowed(VK_OEM_MINUS) &&
              !pass_key_allowed(VK_OEM_PERIOD) && !pass_key_allowed(VK_TAB) && !pass_key_allowed(VK_BACK) &&
              !pass_key_allowed(VK_NUMPAD1) && !pass_key_allowed(VK_NUMPAD2) && !pass_key_allowed(VK_CAPITAL),
          "never Enter, Esc, an arrow, a modifier, an OEM key, Tab, Backspace, Caps Lock, Num2 (↓'s scan code with "
          "NumLock off), or a key with anything folded into its code -- Home's 0x24 under 0x100 included");
}

void test_window() {
    section("Key window: armed for the hold on now, opened and shut by the job, never keyed on the hold's reason, "
            "gone with the hold");
    ExecutionCoordinator c;
    PassKeys keys;
    keys.keys = { 'Z', 'X', VK_SPACE, 'G', '2', VK_RETURN, 'Z' };
    keys.potions = { 'G', '2' };
    check(!c.arm_pass_keys(keys) && !c.pass_keys_armed() && !c.open_pass_keys(),
          "no hold on: nothing armed, nothing to open");
    check(c.request_key("macro-timer", 'Z') == ExecutionCoordinator::KeyWay::NoWindow,
          "and a key asks through no window");
    c.engage_critical(kTrash);
    check(c.request_key("macro-timer", 'Z') == ExecutionCoordinator::KeyWay::NoWindow && !c.lets_through('Z'),
          "a hold with no window armed: every key refused, as ever");
    check(c.arm_pass_keys(keys) && c.pass_keys_armed() && !c.pass_keys_open() && !c.lets_through('Z'),
          "armed under the hold, shut until opened");
    check(c.request_key("macro-timer", 'Z') == ExecutionCoordinator::KeyWay::Shut,
          "armed and shut: Shut, not NoWindow");
    check(c.open_pass_keys() && c.pass_keys_open() && c.lets_through('Z') && c.lets_through('X') &&
              c.lets_through(VK_SPACE) && c.lets_through('G') && !c.lets_through('2') && !c.lets_through(VK_RETURN) &&
              !c.lets_through('C'),
          "open: Z, X, Space and G through; the digit and Enter armed with them never, nor C, not in the set");
    // The token: one key at a time, through the window or not.
    check(c.request_key("macro-timer", 'Z') == ExecutionCoordinator::KeyWay::Taken && c.held() &&
              c.current_owner() == "macro-timer",
          "Z takes the token");
    check(c.request_key("macro-potion", 'G') == ExecutionCoordinator::KeyWay::Busy &&
              !c.request("hunter", Priority::Combat) && !c.request("hunter", Priority::Movement) &&
              !c.request_free("hunter-drop", Priority::Combat) && !c.request("macro-buff", Priority::Buff),
          "while it is down G waits (Busy), and request() under the hold refuses the hunter, a walk, the drop and a "
          "buff alike, as ever");
    c.release("macro-timer");
    check(!c.held() && c.pass_potion_ms() == 0, "let go; no potion through yet");
    check(c.request_key("macro-potion", 'G') == ExecutionCoordinator::KeyWay::Taken && c.pass_potion_ms() != 0,
          "G, a potion, goes through and its time is kept");
    c.release("macro-potion");
    check(c.request_key("macro-potion", '2') == ExecutionCoordinator::KeyWay::Shut &&
              c.request_key("macro-timer", 'C') == ExecutionCoordinator::KeyWay::Shut,
          "the digit and a key not in the set: Shut, never Taken");
    // Shut: nothing through from then on.
    check(c.close_pass_keys(0) && !c.pass_keys_open() && c.pass_keys_armed() && !c.lets_through('Z') &&
              c.request_key("macro-timer", 'Z') == ExecutionCoordinator::KeyWay::Shut,
          "shut, nothing down: at once, and no key through");
    check(c.open_pass_keys() && c.lets_through('Z'), "opened again");
    // The same hold taken again -- take_hold's engage with its own reason --
    // leaves it; another's hold, or the hold ended, takes it away.
    c.engage_critical(kTrash);
    check(c.pass_keys_armed() && c.pass_keys_open(), "the same hold engaged again: the window as it was");
    c.engage_critical("stopping");
    check(!c.pass_keys_armed() && !c.lets_through('Z') &&
              c.request_key("macro-timer", 'Z') == ExecutionCoordinator::KeyWay::NoWindow && !c.open_pass_keys(),
          "another's hold in its place (F12's \"stopping\"): disarmed, and it cannot be opened");
    c.engage_critical(kTrash);
    check(!c.pass_keys_armed(), "nor back with the trash hold again: a job arms its own");
    check(c.arm_pass_keys(keys) && c.open_pass_keys(), "armed again by a job");
    c.disengage_critical();
    check(!c.pass_keys_armed() && !c.lets_through('Z') && !c.critical_active(), "the hold ended: gone");
    c.engage_critical(kTrash);
    check(c.arm_pass_keys(keys) && c.open_pass_keys(), "and once more");
    c.disarm_pass_keys();
    check(!c.pass_keys_armed() && !c.open_pass_keys(), "disarmed (a stop): shut, and not to be opened");
    c.disengage_critical();
    PassKeys junk;
    junk.keys = { '1', VK_RETURN, VK_ESCAPE, VK_F5 };
    c.engage_critical(kTrash);
    check(!c.arm_pass_keys(junk) && !c.pass_keys_armed(), "a set of nothing it may let through arms nothing");
    c.disengage_critical();

    // Shut while a key let through is down: waited for, and only so long.
    c.engage_critical(kTrash);
    c.arm_pass_keys(keys);
    c.open_pass_keys();
    check(c.request_key("macro-timer", 'X') == ExecutionCoordinator::KeyWay::Taken, "X down");
    const auto from = std::chrono::steady_clock::now();
    const bool short_wait = c.close_pass_keys(60);
    const auto waited = std::chrono::steady_clock::now() - from;
    check(!short_wait && waited >= std::chrono::milliseconds(50) && !c.lets_through('X'),
          "shut with X still down: false after the wait, and no key through");
    std::thread up([&c] {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        c.release("macro-timer");
    });
    const auto from2 = std::chrono::steady_clock::now();
    const bool long_wait = c.close_pass_keys(2000);
    const auto waited2 = std::chrono::steady_clock::now() - from2;
    up.join();
    check(long_wait && waited2 >= std::chrono::milliseconds(80) && waited2 < std::chrono::milliseconds(1500),
          "and true once it is let go of, at once after it");
    const std::string line = c.pass_keys_line();
    check(line.find("X 1 through") != std::string::npos && line.find(" open (") != std::string::npos,
          "its line counts what went through and how often it was open -- " + line);
    c.disengage_critical();
}

void test_scope() {
    section("Key window: PassKeysScope -- armed for one job's hold, shut and disarmed however it ends");
    ExecutionCoordinator c;
    PassKeys keys;
    keys.keys = { 'Z', 'G' };
    keys.potions = { 'G' };
    {
        const PassKeysScope none(c, keys, "pass");
        check(!none.armed() && !c.pass_keys_armed(), "no hold: not armed");
    }
    c.engage_critical(kTrash);
    {
        const PassKeysScope empty(c, PassKeys{}, "pass");
        check(!empty.armed() && !c.pass_keys_armed(), "no keys (the macro off, a charm run): not armed");
    }
    const auto in_scope = [&](bool leave_early) {
        const PassKeysScope scope(c, keys, "shop");
        if (!scope.armed() || !c.open_pass_keys() || !c.lets_through('Z')) return false;
        if (leave_early) return true;       // returned with it open
        return c.pass_keys_open();
    };
    check(in_scope(true) && !c.pass_keys_armed() && !c.lets_through('Z'),
          "left with the window open: shut and disarmed on the way out");
    check(in_scope(false) && !c.pass_keys_armed(), "and as ever at its end");
    // A key down as the job ends is waited for.
    std::atomic<bool> released{ false };
    std::thread held;
    {
        const PassKeysScope scope(c, keys, "pass");
        c.open_pass_keys();
        check(c.request_key("macro-timer", 'Z') == ExecutionCoordinator::KeyWay::Taken, "Z down as the job ends");
        held = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            released = true;
            c.release("macro-timer");
        });
    }
    check(released.load() && !c.pass_keys_armed(), "the job's end waited for it to come up, then disarmed");
    held.join();
    c.disengage_critical();
}

void test_nav_window() {
    section("Key window: Home, End, PgUp, PgDn, Insert and Delete through it (2026.11.11) -- when one came up kept "
            "for the job's settle, a letter's never, and each said by its name");
    ExecutionCoordinator c;
    PassKeys keys;
    keys.keys = { 'Z', VK_HOME, VK_NEXT, VK_DELETE, VK_INSERT, VK_END, VK_PRIOR, VK_DOWN, VK_LEFT, VK_SELECT, '5',
                  VK_NUMPAD7 };
    keys.potions = { VK_NEXT };
    c.engage_critical(kTrash);
    check(c.arm_pass_keys(keys) && c.open_pass_keys(), "armed and open");
    check(c.lets_through(VK_HOME) && c.lets_through(VK_NEXT) && c.lets_through(VK_DELETE) &&
              c.lets_through(VK_INSERT) && c.lets_through(VK_END) && c.lets_through(VK_PRIOR) &&
              !c.lets_through(VK_DOWN) && !c.lets_through(VK_LEFT) && !c.lets_through(VK_SELECT) &&
              !c.lets_through('5') && !c.lets_through(VK_NUMPAD7) && c.pass_key_armed(VK_HOME) &&
              !c.pass_key_armed(VK_DOWN),
          "the six through; ↓, ←, Select, the digit and Num7 (Home with NumLock off) armed with them never");
    check(c.request_key("macro-timer", VK_DOWN) == ExecutionCoordinator::KeyWay::Shut &&
              c.request_key("macro-timer", '5') == ExecutionCoordinator::KeyWay::Shut,
          "↓ and the digit ask: Shut, never Taken");
    // A letter: nothing kept for the settle.
    check(c.request_key("macro-timer", 'Z') == ExecutionCoordinator::KeyWay::Taken, "Z down");
    c.release("macro-timer");
    check(c.pass_nav_ms() == 0, "Z up: no settle -- a letter's way, as before");
    // One of the six: kept as it comes up, not as it goes down.
    check(c.request_key("macro-timer", VK_HOME) == ExecutionCoordinator::KeyWay::Taken && c.pass_nav_ms() == 0,
          "Home down: nothing kept yet");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::uint64_t before_up = now_ms();
    c.release("macro-timer");
    const std::uint64_t home_up = c.pass_nav_ms();
    check(home_up >= before_up && home_up <= now_ms(), "Home up: when, kept");
    check(c.request_key("macro-potion", VK_NEXT) == ExecutionCoordinator::KeyWay::Taken && c.pass_potion_ms() != 0,
          "PgDn, a potion's key: its key-down kept as a potion's, as a letter potion's is");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    c.release("macro-potion");
    const std::uint64_t pgdn_up = c.pass_nav_ms();
    check(pgdn_up > home_up, "and its key-up as one of the six");
    check(c.request_key("macro-timer", VK_DELETE) == ExecutionCoordinator::KeyWay::Taken, "Delete down");
    c.release("someone else");
    check(c.held() && c.pass_nav_ms() == pgdn_up, "a release not its own: Delete still down, nothing kept");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    // Taken over by a Critical hold's own press: its press over for the window.
    check(c.request("stopping", Priority::Critical) && c.pass_nav_ms() > pgdn_up, "Delete taken over: kept then");
    c.release("stopping");
    check(c.pass_nav_ms() > pgdn_up && !c.held(), "the Critical press let go");
    // The line: by name, never by the code's character ('$' is Home's).
    c.request_key("macro-timer", VK_HOME);
    c.release("macro-timer");
    const std::string line = c.pass_keys_line();
    check(line.find("Z 1, Home 2, PgDn 1, Delete 1 through, ") == 0 && line.find('$') == std::string::npos &&
              line.find('"') == std::string::npos && line.find('.') == std::string::npos,
          "the line names them: " + line);
    // Armed afresh: nothing kept.
    check(c.arm_pass_keys(keys) && c.pass_nav_ms() == 0, "armed for the next job: the settle's time forgotten");
    c.disengage_critical();
    // Space by its name as before.
    c.engage_critical(kTrash);
    PassKeys space;
    space.keys = { VK_SPACE };
    c.arm_pass_keys(space);
    c.open_pass_keys();
    c.request_key("macro-timer", VK_SPACE);
    c.release("macro-timer");
    check(c.pass_keys_line().find("Space 1 through") == 0 && c.pass_nav_ms() == 0,
          "Space: \"Space 1\", and no settle");
    c.disengage_critical();
}

void test_set() {
    section("Key window: the set from the macro's keys in memory -- letters and Space, skills, buffs and potions; "
            "never a digit, the bag key, the quest key or GoBot's own switches");
    InputManager input;
    ExecutionCoordinator coord;
    MacroEngine engine(input, coord);
    MacroConfig cfg;
    cfg.apply_defaults();
    MacroKey off = key('V', 500);
    off.enabled = false;
    cfg.keys = { key('Z', 500),
                 key('X', 500),
                 key(VK_SPACE, 500),
                 potion(MacroTrigger::HpBelow, 'G', 60),
                 potion(MacroTrigger::MpBelow, 'F', 84),
                 potion(MacroTrigger::PetHpBelow, 'Q', 90),
                 buff('6', 300'000),
                 buff('B', 0),
                 key('I', 500),
                 key('T', 500),
                 key(VK_F5, 500),
                 key(VK_RETURN, 500),
                 key('3', 500),
                 off,
                 key('Z', 700),
                 key('K', 500) };
    cfg.toggle_key = 'K';       // a letter by hand: never through
    engine.set_config(cfg);
    const PassKeys p = engine.pass_keys({ 'I', 'T' });
    const std::vector<std::uint32_t> want{ 'Z', 'X', VK_SPACE, 'G', 'F', 'Q', 'B' };
    check(p.keys == want, "Z, X, Space, G, F, Q and the letter buff B, each once, in the keys' order -- " +
                              std::to_string(p.keys.size()) + " keys");
    check(p.potions == std::vector<std::uint32_t>({ 'G', 'F', 'Q' }), "the potions among them: G, F and Q");
    check(!p.has('6') && !p.has('3') && !p.has('I') && !p.has('T') && !p.has(VK_F5) && !p.has(VK_RETURN) &&
              !p.has('V') && !p.has('K'),
          "not the digit buff 6, the digit 3, the bag key I, the quest key T, F5, Enter, the key switched off, nor "
          "the toggle key");
    // The HP potion among them, and the HP potions' highest mark on whatever
    // key (2026.11.11): what the bag jobs' HP gates go by (bag::hp_gates).
    check(p.hp_potions == std::vector<std::uint32_t>({ 'G' }) && p.hp_mark == 60 && p.hp_all_through,
          "the HP potion among them: G, its mark 60%, the only one");
    // Two tiers on two keys (the review's HP-1): the macro presses only the
    // tier HP is under, so the job drinks only when each one it may choose
    // from the mark down to half of it -- where the job stops -- goes through.
    const auto tiered = [&](std::uint32_t lv1, int lv1_pct, std::uint32_t lv2, int lv2_pct) {
        MacroConfig tiers = cfg;
        tiers.keys = { key('Z', 500), potion(MacroTrigger::HpBelow, lv1, lv1_pct),
                       potion(MacroTrigger::HpBelow, lv2, lv2_pct) };
        engine.set_config(tiers);
        return engine.pass_keys({});
    };
    const auto gates = [](const PassKeys& k) {
        return bag::hp_gates(k.hp_all_through, k.hp_mark, std::max(50, k.hp_mark));
    };
    const PassKeys t = tiered('G', 50, '1', 80);
    const bag::HpGates t_gates = gates(t);
    // 2026.11.11b: the gates at any HP whatever drinks (bag::kJobDownHpPct).
    check(t.hp_potions == std::vector<std::uint32_t>({ 'G' }) && t.hp_mark == 80 && !t.hp_all_through &&
              !t_gates.drinks && t_gates.start == bag::kJobDownHpPct && t_gates.stop == bag::kJobDownHpPct,
          "Lv1 on G at 50%, Lv2 on the digit 1 at 80%: G let through, the mark 80% -- the highest, whatever its key "
          "-- but 1, the one pressed from 80% down to 50%, held: the gates as before, 90% and 80%");
    const PassKeys d80 = tiered('1', 80, 'G', 30);
    const PassKeys d50 = tiered('1', 50, 'G', 30);
    const bag::HpGates d50_gates = gates(d50);
    check(!d80.hp_all_through && !gates(d80).drinks && !d50.hp_all_through && !d50_gates.drinks &&
              d50_gates.start == bag::kJobDownHpPct && d50_gates.stop == bag::kJobDownHpPct,
          "Lv1 on the digit 1 at 80% or at 50% (the simple screen's), Lv2 on G at 30%: nothing drinks from the mark "
          "down to G's 30%, so no H gates -- with 50%, started at 60% and stopped under 50%, as before");
    const PassKeys edge = tiered('G', 81, '1', 40);
    const PassKeys under = tiered('G', 80, '1', 30);
    const bag::HpGates under_gates = gates(under);
    check(!edge.hp_all_through && under.hp_all_through && under_gates.drinks &&
              under_gates.start == bag::kJobDownHpPct && under_gates.stop == bag::kJobDownHpPct,
          "G at 81% and 1 at 40% -- half of 81 is 40, where the job still runs: held, no H gates; G at 80% and 1 at "
          "30%, under where the job stops: G drinks, started at 80%, stopped under 40%");
    MacroConfig digit = cfg;
    digit.keys = { key('Z', 500), potion(MacroTrigger::HpBelow, '1', 80), potion(MacroTrigger::MpBelow, '2', 70) };
    engine.set_config(digit);
    const PassKeys d = engine.pass_keys({});
    const bag::HpGates today = bag::hp_gates(d.hp_all_through, d.hp_mark, std::max(50, d.hp_mark));
    check(d.hp_potions.empty() && d.potions.empty() && d.hp_mark == 80 && !d.hp_all_through && !today.drinks &&
              today.start == bag::kJobDownHpPct && today.stop == bag::kJobDownHpPct,
          "HP on the digit 1 at 80% (the simple screen's default key): no HP potion let through, so the gates as "
          "before -- started at 90%, stopped under 80%");
    // Home, End, PgUp, PgDn, Insert and Delete (2026.11.11): in the set like
    // the letters, every kind of line; an HP potion on one drinks through the
    // window, so the job's gates go by its mark.
    MacroConfig nav = cfg;
    nav.keys = { key(VK_HOME, 500),
                 potion(MacroTrigger::HpBelow, VK_PRIOR, 80),
                 potion(MacroTrigger::MpBelow, VK_DELETE, 70),
                 potion(MacroTrigger::PetHpBelow, VK_NEXT, 90),
                 buff(VK_INSERT, 0),
                 key(VK_END, 500),
                 key(VK_LEFT, 500),
                 key(VK_SELECT, 500),
                 key(VK_NUMPAD7, 500),
                 key('5', 500) };
    nav.timers_enabled = true;
    engine.set_config(nav);
    const PassKeys n = engine.pass_keys({});
    const bag::HpGates by_mark = bag::hp_gates(n.hp_all_through, n.hp_mark, std::max(50, n.hp_mark));
    check(n.keys == std::vector<std::uint32_t>({ VK_HOME, VK_PRIOR, VK_DELETE, VK_NEXT, VK_INSERT, VK_END }) &&
              n.potions == std::vector<std::uint32_t>({ VK_PRIOR, VK_DELETE, VK_NEXT }),
          "a skill on Home and End, HP on PgUp, MP on Delete, the pet on PgDn, a buff on Insert: all six in the set, "
          "the three potions among them; ←, Select, Num7 and the digit 5 never -- " + std::to_string(n.keys.size()) +
              " keys");
    check(n.hp_potions == std::vector<std::uint32_t>({ VK_PRIOR }) && n.hp_mark == 80 && by_mark.drinks &&
              by_mark.start == bag::kJobDownHpPct && by_mark.stop == bag::kJobDownHpPct,
          "HP on PgUp at 80%: let through, so drunk during the job -- which since 2026.11.11b runs at any HP");
    nav.keys = { key(VK_HOME, 500), potion(MacroTrigger::HpBelow, VK_PRIOR, 80) };
    nav.toggle_key = VK_HOME;
    engine.set_config(nav);
    const PassKeys own = engine.pass_keys({ VK_PRIOR });
    check(!own.any() && own.hp_potions.empty() && own.hp_mark == 80,
          "Home the macro's own toggle key, PgUp the bag key: neither -- GoBot's own keys never go");
    cfg.timers_enabled = false;
    engine.set_config(cfg);
    const PassKeys none = engine.pass_keys({});
    check(!none.any() && none.hp_potions.empty() && none.hp_mark == 0, "the timers off: nothing");
}

// ------------------------------------------------------------ the macro

void test_macro_through() {
    section("Key window: the macro's letter and Space keys go on through a bag job's hold, a digit potion under "
            "its mark starves nothing, the sequence waits, and what waited goes as the window opens");
    // The owner's way: Z X Space on their own timers, G the HP potion, '2'
    // the MP potion on a digit, the pet on Q, a buff on 6 and one on B.
    const auto keys = [] {
        return std::vector<MacroKey>{ key('Z', 500),
                                      key('X', 500),
                                      key(VK_SPACE, 500),
                                      potion(MacroTrigger::HpBelow, 'G', 60),
                                      potion(MacroTrigger::MpBelow, '2', 84),
                                      buff('6', 300'000),
                                      buff('B', 0) };
    };
    {
        // Held, no window: nothing at all, as ever.
        Rig rig(keys());
        rig.mp = 50.0f;
        rig.coord.engage_critical(kTrash);
        const std::uint64_t t0 = rig.t;
        rig.run(3000);
        // Every key looked at, not Z alone: a slip letting one keycode or one
        // kind through an unarmed window would show here.
        std::string out;
        for (const auto& p : rig.pressed)
            if (p.second >= t0) out += " " + InputManager::key_name(p.first);
        const auto st = rig.engine.stats();
        check(out.empty(), "a hold with no window: no key at all -- not Z, X, Space, B, G nor the due digit MP "
                           "potion 2" + (out.empty() ? std::string() : " -- went out:" + out));
        check(st.skipped_busy > 0 && st.held_gated == 0,
              "refused by the hold itself, never as Gated with no window armed (" + std::to_string(st.skipped_busy) +
                  " refused, " + std::to_string(st.held_gated) + " gated)");
        rig.coord.disengage_critical();
    }
    {
        Rig rig(keys());
        rig.mp = 50.0f;                    // the digit MP potion due all along
        rig.bag_job({ 'Z', 'X', VK_SPACE, 'G', 'B' }, { 'G' });
        const std::uint64_t t0 = rig.t;
        rig.run(5000);
        const auto st = rig.engine.stats();
        check(rig.count('Z', t0) >= 8 && rig.count('X', t0) >= 8 && rig.count(VK_SPACE, t0) >= 8,
              "5 s of a bag job: Z, X and Space every half second all the same (" + std::to_string(rig.count('Z', t0)) +
                  ", " + std::to_string(rig.count('X', t0)) + ", " + std::to_string(rig.count(VK_SPACE, t0)) + ")");
        check(rig.count('2', t0) == 0 && rig.count('6', t0) == 0 && st.held_gated > 0,
              "the digit MP potion and the digit buff never, held by the window (" + std::to_string(st.held_gated) +
                  " held)");
        check(rig.count('B', t0) >= 1, "the letter buff cast");
        // HP falls: G first.
        rig.hp = 40.0f;
        const std::uint64_t t1 = rig.t;
        rig.run(300);
        const auto g = rig.when('G', t1);
        const auto z = rig.when('Z', t1);
        check(!g.empty() && (z.empty() || g[0] <= z[0]) && g[0] - t1 <= 30,
              "HP under the mark: the letter HP potion at once, before the next skill");
        rig.hp = 100.0f;
        // Shut for a click of the job's own: nothing -- three seconds and a
        // half, G's own cooldown over meanwhile; open again, Z within a
        // moment.
        rig.coord.close_pass_keys(0);
        const std::uint64_t t2 = rig.t;
        rig.run(3500);
        check(rig.count('Z', t2) + rig.count('X', t2) + rig.count(VK_SPACE, t2) + rig.count('G', t2) +
                      rig.count('B', t2) ==
                  0,
              "shut: not a key");
        rig.hp = 40.0f;                     // G due while it is shut
        rig.run(500);
        rig.coord.open_pass_keys();
        const std::uint64_t t3 = rig.t;
        rig.run(200);
        const auto g3 = rig.when('G', t3);
        const auto z3 = rig.when('Z', t3);
        check(!g3.empty() && !z3.empty() && g3[0] - t3 <= 20 && g3[0] <= z3[0] && z3[0] - t3 <= 120,
              "opened again: the potion that waited first, at once, then the skills -- G after " +
                  std::to_string(g3.empty() ? 0 : g3[0] - t3) + " ms, Z after " +
                  std::to_string(z3.empty() ? 0 : z3[0] - t3) + " ms");
        rig.coord.disengage_critical();
        // The job over: the digit potion and buff go as ever.
        rig.hp = 100.0f;
        const std::uint64_t t4 = rig.t;
        rig.run(1500);
        check(rig.count('2', t4) >= 1, "the hold over: the digit MP potion drinks");
    }
    {
        // A potion only the window holds never re-stamps its wait: the skills
        // are held kPotionFirstMs at most, not the whole job.
        Rig rig({ key('Z', 500), potion(MacroTrigger::MpBelow, '2', 84) });
        rig.bag_job({ 'Z' }, {});
        rig.mp = 50.0f;
        const std::uint64_t t0 = rig.t;
        rig.run(2000);
        const auto z = rig.when('Z', t0);
        check(z.size() >= 3 && z[0] - t0 <= MacroEngine::kPotionFirstMs + 50,
              "MP under its mark on a digit: Z first goes within " + std::to_string(MacroEngine::kPotionFirstMs) +
                  " ms and every half second after (" + std::to_string(z.size()) + " in 2 s)");
        rig.coord.disengage_critical();
    }
    {
        // The sequence never goes through it, its letter key in the set or not.
        MacroStep s;
        s.keycode = 'C';
        s.delay_after_ms = 200;
        Rig rig({ key('Z', 500) }, { s });
        rig.bag_job({ 'Z', 'C' }, {});
        const std::uint64_t t0 = rig.t;
        rig.run(2000);
        check(rig.count('C', t0) == 0 && rig.count('Z', t0) >= 3,
              "a sequence step on C, C in the window: never pressed; Z on its timer is");
        rig.coord.disengage_critical();
        const std::uint64_t t1 = rig.t;
        rig.run(1000);
        check(rig.count('C', t1) >= 1, "the hold over: the sequence goes on");
    }
    {
        // Home, End, PgUp, PgDn, Insert and Delete (2026.11.11): skills on
        // Home and End, the HP potion on PgUp, a buff on Insert -- through the
        // open window like the letters; ← and the digit 4 never.
        Rig rig({ key(VK_HOME, 500), key(VK_END, 500), potion(MacroTrigger::HpBelow, VK_PRIOR, 60),
                  buff(VK_INSERT, 0), key(VK_LEFT, 500), key('4', 500) });
        rig.bag_job({ VK_HOME, VK_END, VK_PRIOR, VK_INSERT, VK_LEFT, '4' }, { VK_PRIOR });
        const std::uint64_t t0 = rig.t;
        rig.run(3000);
        check(rig.count(VK_HOME, t0) >= 5 && rig.count(VK_END, t0) >= 5 && rig.count(VK_INSERT, t0) >= 1 &&
                  rig.count(VK_LEFT, t0) == 0 && rig.count('4', t0) == 0,
              "3 s of a bag job: Home and End every half second, the Insert buff cast; ← and 4 never (" +
                  std::to_string(rig.count(VK_HOME, t0)) + ", " + std::to_string(rig.count(VK_END, t0)) + ")");
        check(rig.coord.pass_nav_ms() != 0, "and when the last of them came up kept, for the job's settle");
        rig.hp = 40.0f;
        const std::uint64_t t1 = rig.t;
        rig.run(300);
        const auto g = rig.when(VK_PRIOR, t1);
        check(!g.empty() && g[0] - t1 <= 30, "HP under the mark: the potion on PgUp at once");
        rig.coord.close_pass_keys(0);
        const std::uint64_t t2 = rig.t;
        rig.run(1500);
        check(rig.count(VK_HOME, t2) + rig.count(VK_END, t2) + rig.count(VK_PRIOR, t2) == 0, "shut: none of them");
        rig.coord.disengage_critical();
    }
}

// ------------------------------------------- HP and the bag jobs (2026.11.11)

void test_vital_due() {
    section("Key window: the potion break's hook -- an HP or MP potion due whose key the window lets through, never "
            "the pet's (2026.11.11)");
    const auto all = [](std::uint32_t) { return true; };
    Rig rig({ key('Z', 500), potion(MacroTrigger::HpBelow, 'G', 80), potion(MacroTrigger::MpBelow, 'F', 84),
              potion(MacroTrigger::PetHpBelow, 'Q', 90), potion(MacroTrigger::HpBelow, '1', 30) });
    rig.engine.set_vitals(100.0f, 100.0f, 40.0f);
    check(!rig.engine.vital_potion_due(all) && !rig.engine.vital_potion_due({}),
          "only the pet's potion due (its HP 40% under 90%): no break");
    rig.engine.set_vitals(70.0f, 100.0f, 100.0f);
    check(rig.engine.vital_potion_due(all) && rig.engine.vital_potion_due({}) &&
              !rig.engine.vital_potion_due([](std::uint32_t vk) { return vk != 'G'; }),
          "HP 70% under G's 80%: due -- but not for a window that would not let G through");
    rig.engine.set_vitals(100.0f, 50.0f, 100.0f);
    check(rig.engine.vital_potion_due(all) && !rig.engine.vital_potion_due([](std::uint32_t vk) { return vk == 'G'; }),
          "MP 50% under F's 84%: due, by F's key");
    rig.engine.set_vitals(20.0f, 100.0f, 100.0f);
    check(!rig.engine.vital_potion_due([](std::uint32_t vk) { return vk == 'G'; }),
          "HP 20%: the more urgent tier, on the digit 1, is the one due -- G not the key asked about");
    // As GoBot's window asks it: the window armed for the hold, G and F in it,
    // the digit never -- open or shut.
    rig.coord.engage_critical(kTrash);
    PassKeys keys;
    keys.keys = { 'Z', 'G', 'F', 'Q', '1' };
    keys.potions = { 'G', 'F', 'Q', '1' };
    rig.coord.arm_pass_keys(keys);
    const auto armed = [&rig](std::uint32_t vk) { return rig.coord.pass_key_armed(vk); };
    check(rig.coord.pass_key_armed('G') && rig.coord.pass_key_armed('F') && !rig.coord.pass_key_armed('1') &&
              !rig.coord.pass_key_armed('C') && !rig.coord.pass_keys_open(),
          "pass_key_armed: G and F, shut or not; never the digit 1 armed with them, nor C, not in the set");
    rig.engine.set_vitals(70.0f, 100.0f, 100.0f);
    check(rig.engine.vital_potion_due(armed), "HP under G's mark, G armed: a break");
    rig.engine.set_vitals(20.0f, 100.0f, 100.0f);
    check(!rig.engine.vital_potion_due(armed), "HP under the digit's 30%: that tier never armed, no break for it");
    rig.engine.set_vitals(100.0f, 100.0f, 40.0f);
    check(!rig.engine.vital_potion_due(armed), "the pet's potion due, Q armed: no break all the same");
    // Drunk through the window: on its cooldown, no longer due.
    rig.coord.open_pass_keys();
    rig.hp = 70.0f;
    rig.pet = 100.0f;
    const std::uint64_t t0 = rig.t;
    rig.run(300);
    rig.engine.set_vitals(70.0f, 100.0f, 100.0f);
    check(rig.count('G', t0) == 1 && !rig.engine.vital_potion_due(armed),
          "G drunk with the window open: on its cooldown, the break over");
    rig.coord.disengage_critical();
    check(!rig.coord.pass_key_armed('G'), "the hold over: nothing armed");
    MacroConfig off = rig.engine.config();
    off.timers_enabled = false;
    rig.engine.set_config(off);
    rig.engine.set_vitals(30.0f, 30.0f, 30.0f);
    check(!rig.engine.vital_potion_due(all), "the timers off: never");

    // HP on Home, MP on Delete (2026.11.11): armed like a letter, so a break
    // for them.
    Rig nav({ key('Z', 500), potion(MacroTrigger::HpBelow, VK_HOME, 80),
              potion(MacroTrigger::MpBelow, VK_DELETE, 84) });
    nav.coord.engage_critical(kTrash);
    PassKeys six;
    six.keys = { 'Z', VK_HOME, VK_DELETE };
    six.potions = { VK_HOME, VK_DELETE };
    nav.coord.arm_pass_keys(six);
    const auto nav_armed = [&nav](std::uint32_t vk) { return nav.coord.pass_key_armed(vk); };
    nav.engine.set_vitals(70.0f, 100.0f, 100.0f);
    const bool hp_due = nav.engine.vital_potion_due(nav_armed);
    nav.engine.set_vitals(100.0f, 50.0f, 100.0f);
    const bool mp_due = nav.engine.vital_potion_due(nav_armed);
    check(nav.coord.pass_key_armed(VK_HOME) && nav.coord.pass_key_armed(VK_DELETE) && hp_due && mp_due,
          "HP under its mark on Home, MP under its on Delete: both armed, both a break");
    nav.coord.disengage_critical();
}

void test_own_due() {
    section("Own potions: the potion a bag job presses itself with the macro not running (2026.11.11b, the owner: "
            "\"GoBot tự bấm bình\", \"Giữ chặn phím số\") -- HP's tier first, never the pet's, never a digit");
    Rig rig({ key('Z', 500), potion(MacroTrigger::HpBelow, 'G', 80), potion(MacroTrigger::MpBelow, 'F', 84),
              potion(MacroTrigger::PetHpBelow, 'Q', 90), potion(MacroTrigger::HpBelow, '1', 30) });
    rig.engine.set_vitals(100.0f, 100.0f, 40.0f);
    const MacroEngine::OwnPotion pet = rig.engine.own_potion_due({});
    check(pet.vk == 0 && pet.refused == 0, "only the pet's potion due: none");
    rig.engine.set_vitals(39.0f, 50.0f, 100.0f);
    const MacroEngine::OwnPotion hp = rig.engine.own_potion_due({});
    check(hp.vk == 'G' && hp.hp && hp.mark == 80 && hp.cooldown_ms > 0 && hp.refused == 0,
          "HP 39% under G's 80%, MP due as well: G, HP's first");
    const MacroEngine::OwnPotion never = rig.engine.own_potion_due({ 'G' });
    check(never.vk == 'F' && !never.hp && never.refused == 'G',
          "G on the never-list (the bag key, say): not pressed, said, MP's F instead");
    rig.engine.set_vitals(20.0f, 100.0f, 100.0f);
    const MacroEngine::OwnPotion digit = rig.engine.own_potion_due({});
    check(digit.vk == 0 && digit.refused == '1',
          "HP 20%: the urgent tier is on the digit 1 -- never pressed by a bag job, and said");
    rig.engine.set_vitals(-1.0f, -1.0f, -1.0f);
    check(rig.engine.own_potion_due({}).vk == 0, "no reading: none");
    MacroConfig off = rig.engine.config();
    off.timers_enabled = false;
    rig.engine.set_config(off);
    rig.engine.set_vitals(30.0f, 30.0f, 30.0f);
    check(rig.engine.own_potion_due({}).vk == 0, "the timers off: none");
    Rig nav({ key('Z', 500), potion(MacroTrigger::HpBelow, VK_HOME, 80) });
    nav.engine.set_vitals(50.0f, 100.0f, 100.0f);
    check(nav.engine.own_potion_due({}).vk == VK_HOME, "HP on Home: pressed like a letter");
}

void test_hp_gates() {
    section("Key window: HP and the bag jobs (2026.11.11, the owner: \"Mua luôn, bơm xen\") -- the gates by whether "
            "the HP potion is let through, and how a shop round ended");
    struct Row {
        bool drinks;
        int  mark, floor, start, stop;
        const char* what;
    };
    const Row rows[] = {
        { true, 80, 80, 80, 40, "the owner's G at 80%, let through: started at 80%, stopped only under 40%" },
        { false, 80, 80, 90, 80, "the same on a digit (let through never): as before -- 90% and 80%" },
        { true, 45, 50, 45, 22, "G at 45%: half its own mark, never chat's floor of 50%" },
        { true, 98, 98, 95, 49, "G at 98%: started at 95% at most" },
        { false, 91, 91, 95, 91, "a digit at 91%: 95% at most, as before" },
        { false, 0, 50, 60, 50, "no HP potion: 60% and 50%, as before" },
        { true, 0, 50, 60, 50, "nothing to drink however it is said: as before" },
    };
    // 2026.11.11b (the owner: "mua bình ở bất cứ mức máu nào", "Cả dọn rác"):
    // whatever the row, at any HP -- only the character down (under
    // bag::kJobDownHpPct) holds a job back or stops it. The rows above are
    // what 2026.11.11 had; `drinks` is still told.
    for (const Row& r : rows) {
        const bag::HpGates g = bag::hp_gates(r.drinks, r.mark, r.floor);
        check(g.drinks == (r.drinks && r.mark > 0) && g.start == bag::kJobDownHpPct && g.stop == bag::kJobDownHpPct &&
                  bag::kJobDownHpPct == 1 && r.start > g.start && r.stop >= g.stop,
              std::string(r.what) + " -- 2026.11.11b: any HP, " + std::to_string(g.start) + "/" +
                  std::to_string(g.stop));
    }
    check(bag::kHpLookAgainMs == 5'000 && bag::kHpStoppedAgainMs == 15'000,
          "a look put off for HP again in 5 s; a job stopped for HP again in 15 s");

    // How a round ended: never trouble for what its guard stopped it for.
    using bag::ShopEnd;
    const auto end = [](std::initializer_list<bool bag::ShopRoundSeen::*> on) {
        bag::ShopRoundSeen s;
        for (const auto m : on) s.*m = true;
        return bag::shop_end(s);
    };
    using S = bag::ShopRoundSeen;
    check(end({}) == ShopEnd::Clean && end({ &S::trouble }) == ShopEnd::Trouble &&
              end({ &S::trouble, &S::hp_cut }) == ShopEnd::Paused &&
              end({ &S::trouble, &S::keys_late }) == ShopEnd::Paused &&
              end({ &S::trouble, &S::hold_lost }) == ShopEnd::Paused &&
              end({ &S::trouble, &S::stopped }) == ShopEnd::Paused &&
              end({ &S::trouble, &S::board_up }) == ShopEnd::Paused &&
              end({ &S::full, &S::hp_cut }) == ShopEnd::Full && end({ &S::message, &S::trouble }) == ShopEnd::Message,
          "bag::shop_end: HP its guard stopped it for, a key of the macro's late, the hold lost, a stop, the board "
          "-- put off; the full bag and a message as the game said them; the rest trouble");
    bag::ShopOwed o = bag::shop_new_plan(10, 10);
    o = bag::shop_owed_after(o, 2, 2, 0, 0, ShopEnd::Trouble);
    for (int i = 0; i < 3; ++i) o = bag::shop_owed_after(o, 1, 1, 0, 0, end({ &S::trouble, &S::hp_cut }));
    check(o.hp == 5 && o.mp == 5 && o.troubles == 1 && !o.dropped,
          "one trouble, then three rounds stopped for HP: the rest owed, still one trouble -- the plan never dropped "
          "for HP");
}

// ------------------------------------------------------- the quiet second

void test_quiet() {
    section("Key window: the shop's quiet second never takes GoBot's own input for the player's (quiet_said)");
    check(quiet_said(1000, 1000, 7, 7) == QuietSaid::Quiet && quiet_said(1000, 1000, 7, 9) == QuietSaid::Quiet,
          "no input at all: quiet, whatever GoBot sent that Windows did not count");
    check(quiet_said(1000, 1450, 7, 7) == QuietSaid::Touched, "input, none of it GoBot's: the player's");
    check(quiet_said(1000, 1450, 7, 8) == QuietSaid::Own, "input with one event of GoBot's meanwhile: not told, Own");

    // The count and the key marks as inject() keeps them, through a pretend
    // Windows (only while real input is forbidden): what the quiet second and
    // the own-walk line read.
    using Ev = InputManager::SentEvent;
    const auto key_ev = [](std::uint16_t vk, bool up, std::uint32_t more = 0) {
        Ev e;
        e.vk = vk;
        e.flags = (up ? static_cast<std::uint32_t>(KEYEVENTF_KEYUP) : 0u) | more;
        return e;
    };
    struct Sender {
        InputManager::PretendSender sender;
        unsigned                    takes = 1000;
        std::uint64_t               seen = 0;      ///< sent_count() as the events reached it
        Sender() {
            sender.send = [this](const std::vector<Ev>& events, bool) -> unsigned {
                seen = InputManager::sent_count();
                return std::min(takes, static_cast<unsigned>(events.size()));
            };
            sender.focused = [] { return true; };
            sender.key_held = [](int) { return false; };
            InputManager::set_pretend_sender(&sender);
        }
        ~Sender() { InputManager::set_pretend_sender(nullptr); }
        Sender(const Sender&) = delete;
        Sender& operator=(const Sender&) = delete;
    } s;
    const auto marks_text = [](const std::vector<InputManager::KeyMark>& marks) {
        std::string out;
        for (const auto& m : marks) out += " " + InputManager::key_name(m.vk) + (m.down ? "+" : "-");
        return out;
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const std::uint64_t t0 = now_ms();
    const std::uint64_t b = InputManager::sent_count();
    const unsigned went = InputManager::pretend_inject({ key_ev('Z', false), key_ev('Z', true) });
    const std::uint64_t t1 = now_ms();
    const std::vector<InputManager::KeyMark> z = InputManager::key_marks(t0);
    check(went == 2 && s.seen == b + 2 && InputManager::sent_count() == b + 4,
          "Z down and up: counted as they are handed over (" + std::to_string(s.seen - b) +
              " as Windows had them) and again once gone -- " + std::to_string(InputManager::sent_count() - b) +
              " in all");
    check(z.size() == 2 && z[0].vk == 'Z' && z[0].down && z[1].vk == 'Z' && !z[1].down && z[0].at_ms >= t0 &&
              z[1].at_ms <= t1,
          "and marked, down then up, when they went --" + marks_text(z));
    const std::uint64_t b2 = InputManager::sent_count();
    const unsigned bare = InputManager::pretend_inject({ key_ev(VK_DOWN, false, KEYEVENTF_EXTENDEDKEY) });
    check(bare == 0 && InputManager::sent_count() == b2 && InputManager::key_marks(t0).size() == 2,
          "a bare ↓, refused before Windows is asked: not counted, not marked");
    s.takes = 0;
    const unsigned none = InputManager::pretend_inject({ key_ev('X', false) });
    s.takes = 1000;
    check(none == 0 && InputManager::sent_count() == b2 + 2 && InputManager::key_marks(t0).size() == 2,
          "one Windows took none of: counted as handed over -- leaning towards GoBot's own -- and never marked");
    Ev unicode;
    unicode.scan = 'q';
    unicode.flags = KEYEVENTF_UNICODE;
    Ev move;
    move.mouse = true;
    move.flags = MOUSEEVENTF_MOVE;
    const std::uint64_t b3 = InputManager::sent_count();
    const unsigned typed = InputManager::pretend_inject({ unicode });
    const std::uint64_t b4 = InputManager::sent_count();
    const unsigned moved = InputManager::pretend_inject({ move });
    check(typed == 1 && moved == 1 && b4 == b3 + 2 && InputManager::sent_count() == b4 + 2 &&
              InputManager::key_marks(t0).size() == 2,
          "a Unicode character and a pointer's move: counted, never a key mark");

    // The own-walk line's reading of the marks: GoBot's arrow held as the
    // walk began, then one tapped inside it.
    InputManager::pretend_inject({ key_ev(VK_LEFT, false, KEYEVENTF_EXTENDEDKEY) });
    const std::uint64_t from = now_ms();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    InputManager::pretend_inject({ key_ev(VK_LEFT, true, KEYEVENTF_EXTENDEDKEY) });
    const std::uint64_t to = now_ms();
    const std::string held = InputManager::walk_keys_text(InputManager::key_marks(t0), VK_LEFT, VK_RIGHT, from, to);
    check(held.rfind("GoBot: Left held from ", 0) == 0, "Left down before the walk began: " + held);
    const std::uint64_t from2 = now_ms();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    InputManager::pretend_inject({ key_ev(VK_RIGHT, false, KEYEVENTF_EXTENDEDKEY) });
    InputManager::pretend_inject({ key_ev(VK_RIGHT, true, KEYEVENTF_EXTENDEDKEY) });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const std::uint64_t to2 = now_ms();
    const std::string tapped =
        InputManager::walk_keys_text(InputManager::key_marks(from2), VK_LEFT, VK_RIGHT, from2, to2);
    check(tapped.rfind("GoBot: Right tapped ", 0) == 0 && tapped.find(" after; ") != std::string::npos,
          "Right down and up inside the walk: " + tapped);
}

// ------------------------------------------------------- the wiring

void test_sources() {
    section("Key window: where GoBot's window, the cleaner and the macro wire it in (their source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string root = GOBOT_SOURCE_DIR;
    const std::string gui = read_file(root + "/apps/gobot_gui/gui_app.cpp");
    const std::string hotkeys = read_file(root + "/apps/gobot_gui/simple_hotkeys.cpp");
    const std::string bag = read_file(root + "/src/bag.cpp");
    const std::string macro = read_file(root + "/src/macro.cpp");
    const std::string pace = read_file(root + "/src/macro_pace.cpp");
    const std::string hunter_h = read_file(root + "/include/gobot/hunter.hpp");
    const std::string macro_h = read_file(root + "/include/gobot/macro.hpp");
    const std::string cli = read_file(root + "/apps/gobot_cli/main.cpp");
    if (gui.empty() || bag.empty() || macro.empty() || pace.empty() || hunter_h.empty() || macro_h.empty() ||
        hotkeys.empty() || cli.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto has = [](const std::string& text, const std::string& part) { return text.find(part) != npos; };
    const auto count_in = [](const std::string& text, const std::string& part) {
        int n = 0;
        for (std::size_t at = text.find(part); at != npos; at = text.find(part, at + 1)) ++n;
        return n;
    };
    const auto slice = [&](const std::string& text, const std::string& from, const std::string& to) {
        const std::size_t a = text.find(from);
        const std::size_t b = a == npos ? npos : text.find(to, a);
        return a == npos || b == npos ? std::string() : text.substr(a, b - a);
    };
    const auto body = [&](const std::string& text, const std::string& head) {
        const std::size_t a = text.find(head);
        const std::size_t b = a == npos ? npos : text.find("\n}", a);
        return a == npos || b == npos ? std::string() : text.substr(a, b + 2 - a);
    };
    const auto in_order = [](const std::string& text, std::initializer_list<const char*> parts) {
        std::size_t at = 0;
        for (const char* p : parts) {
            const std::size_t found = text.find(p, at);
            if (found == npos) return false;
            at = found + 1;
        }
        return true;
    };

    // F12, DỪNG and a stop of the trash worker shut it before anything else.
    const std::string jobs = body(gui, "bool GuiApp::stop_jobs(bool with_bot) {");
    const std::string stop_trash = body(gui, "bool GuiApp::stop_trash(bool wait) {");
    check(in_order(jobs, { "coord_.disarm_pass_keys();", "stopping_ = true;", "take_hold(kStopHold);",
                           "stop_trash(false);" }) &&
              in_order(stop_trash, { "coord_.disarm_pass_keys();", "trash_stop_ = true;", "trash_cleaner_->cancel();" }),
          "stop_jobs and stop_trash: the window disarmed first -- under the trash hold take_hold(kStopHold) does "
          "nothing");

    // The two jobs that arm it, each around its run alone; never the charms.
    const std::string shop =
        slice(gui, "const auto run_shop = [&](bool forced, bool dry, bool after_pass) {", "while (!trash_stop_) {");
    const std::string charms = slice(gui, "const auto run_charms = [&](bool forced) {", "// ---- the potion shop ----");
    const std::string pass = slice(gui, "const bool look_only = request == 2;", "trash_thrown_ += r.deleted");
    check(in_order(shop, { "const PassKeys pass_keys = held || trash_left_held_ ? PassKeys{} : pass_keys_now();",
                           "if (pass_keys.any()) cleaner.set_key_window(key_window());", "take_hold(\"trash cleaning\")",
                           "coord_.disarm_pass_keys();", "input_.release_all();",
                           "std::this_thread::sleep_for(std::chrono::milliseconds(30));",
                           "const std::uint64_t own_from = InputManager::sent_count();",
                           "const DWORD quiet = last_input();", "const DWORD after = last_input();",
                           "const std::uint64_t own_to = InputManager::sent_count();",
                           "const QuietSaid said = quiet_said(quiet, after, own_from, own_to);",
                           "touched = !trash_stop_ && !lost && said == QuietSaid::Touched;",
                           "if (touched || own || trash_stop_ || lost) {", "const PassKeysScope keys(coord_, pass_keys, "
                           "\"shop\");", "r = cleaner.run();", "held = r.keeps_hold(held);" }),
          "run_shop: the set left out when something may be left in the bin; no window through the quiet second, "
          "whose own-input verdict is GoBot's count; armed around the run alone, gone before the hold is kept or "
          "let go of");
    check(in_order(pass, { "const PassKeys pass_keys = left_held ? PassKeys{} : pass_keys_now();",
                           "if (pass_keys.any()) cleaner.set_key_window(key_window());", "take_hold(\"trash cleaning\")",
                           "const PassKeysScope keys(coord_, pass_keys, \"pass\");", "r = cleaner.run();",
                           "held = r.keeps_hold(held);" }),
          "the pass: likewise, none in a pass begun to give back what may be in the bin");
    check(!charms.empty() && !has(charms, "set_key_window") && !has(charms, "PassKeysScope") &&
              !has(charms, "pass_keys_now") && count_in(gui, "set_key_window(") == 2 &&
              count_in(gui, "PassKeysScope keys(") == 2 && !has(cli, "set_key_window(") &&
              !has(cli, "PassKeysScope"),
          "never a charm run's -- and gobot_cli's jobs keep the whole hold");
    // The never-list in its own lambda since 2026.11.11b: the job's own potion
    // presses go by it too.
    const std::string now = slice(gui, "const auto job_never = [&] {", "const auto key_window = [this] {");
    check(in_order(now, { "quest.load(exe_relative(\"configs/quest.ini\"));",
                          "std::vector<std::uint32_t> never{ settings.bag_key, quest.quest_key, own_trash_key_.load(),",
                          "own_login_key_.load() };", "never.push_back(hunter_->config().toggle_key);",
                          "const auto pass_keys_now = [&] {", "if (!macro_ || !macro_->running()) return PassKeys{};",
                          "return macro_->pass_keys(job_never());",
                          "o.due = [this, never = job_never(), refused_said, &own_potion_ms](std::string& line)",
                          "if (!macro_ || macro_->running()) return 0;",
                          "const MacroEngine::OwnPotion p = macro_->own_potion_due(never);" }) &&
              has(hotkeys, "own_trash_key_.store(hotkeys_.trash_now);") &&
              has(hotkeys, "own_login_key_.store(hotkeys_.login_now);"),
          "the set from the macro's keys in memory, never the bag key, the quest key, the trash-now and login-now "
          "keys (atomics the window's thread keeps) nor the hunt switch; the macro's own toggle and panic keys in "
          "pass_keys");

    // The cleaner: every input of its own through the six, each shutting it.
    bool all_shut = true;
    for (const char* head : { "bool BagCleaner::click(int x, int y, bool need_front) {",
                              "bool BagCleaner::drag(int fx, int fy, int tx, int ty) {",
                              "bool BagCleaner::press(std::uint32_t vk) {", "bool BagCleaner::press_scan(std::uint32_t vk) {",
                              "bool BagCleaner::right_click(int x, int y) {", "bool BagCleaner::double_click(int x, int y) {" }) {
        const std::string b = body(bag, head);
        const std::size_t first = b.find('{');
        all_shut = all_shut && first != npos && b.find("if (!keys_shut()) return false;") != npos &&
                   b.find("if (!keys_shut()) return false;") < b.find("hands_.");
    }
    check(all_shut && count_in(bag, "input_.key_press(") == 1 && count_in(bag, "input_.drag_on_game(") == 1 &&
              count_in(bag, "input_.type_key(") == 1 && count_in(bag, "input_.click_on_game(") == 2,
          "bag.cpp: click, drag, press, press_scan, right_click and double_click shut the window first, and no other "
          "input of the cleaner's goes out");
    // Its checks in keys_may_open since 2026.11.11 (the review's KEYS-1), which
    // a potion break asks of a window open already.
    const std::string may = body(bag, "bool BagCleaner::keys_may_open(const Frame& f, const BagView& v, const "
                                      "std::vector<bag::Dialog>* up, int lone,");
    const std::string open = body(bag, "void BagCleaner::keys_open(const Frame& f, const BagView& v, const "
                                       "std::vector<bag::Dialog>* up, int lone,");
    // Its checks moved to keys_clear in 2026.11.11b, which an own potion press
    // goes by as well; keys_may_open asks for the window first.
    const std::string clear = body(bag, "bool BagCleaner::keys_clear(const Frame& f, const BagView& v, const "
                                        "std::vector<bag::Dialog>* up, int lone,");
    check(in_order(may, { "if (!keys_.open) return false;", "return keys_clear(f, v, up, lone, full_asked);" }) &&
              in_order(clear, { "if (cancelled_ || keys_late_ || item_out_ || late_ask_ || money_ || "
                          "!f.valid()) return false;",
                          "if (mode_ != Mode::Pass && mode_ != Mode::ShopBuy) return false;",
                          "if (up == nullptr) {", "found = bag::find_dialogs(f, store_);",
                          "if (mode_ == Mode::Pass && !clean(v, *up)) return false;",
                          "if (!shop_filter(*up).empty() || shop_q_.valid() || question_still(f)) return false;",
                          "const bool message = bag::notice_up(f, store_, *up) ||",
                          "(lone < 0 ? bag::lone_ok(f, store_, ScreenRect{ 0, 0, f.width, f.height }, *up) : lone > 0);",
                          "return !(message || (!full_asked && bag::shop_full_up(f, store_)) || "
                          "checkin_board_title(f, store_));" }) &&
              !has(may, "keys_.open()") && !has(may, "keys_out_") &&
              in_order(open, { "if (keys_out_ || !keys_may_open(f, v, up, lone, full_asked)) return;",
                               "if (keys_.open()) keys_out_ = true;" }),
          "keys_open: never with something out, a question come late, a stop, a key late, in a purchase's money "
          "stretch, in a charm run, nor with a dialog, the quantity question, a message, the full-bag message or the "
          "board up -- the frame searched for dialogs once, by the caller when it has (keys_may_open)");
    const std::string buy = body(bag, "int BagCleaner::shop_purchase(bool hp, Result& res) {");
    const std::size_t money_on = buy.find("money_ = true;");
    const std::size_t dbl = buy.find("double_click(cx, cy)");
    const std::size_t open_at = buy.find("keys_open(tail_last, BagView{}, tail_seen ? &tail_up : nullptr,");
    const std::size_t money_off = open_at == npos ? npos : buy.rfind("money_ = false;", open_at);
    // 2026.11.11: or, an HP or MP potion due, for its break on the look judged
    // before the money stretch -- through keys_open, every check of it made,
    // or keys_may_open's of a window open already; the full-bag message judge
    // asked.
    const std::size_t brk = buy.find("potion_break(f0, BagView{}, \"before the double-click\", nullptr, -1, true);");
    check(money_on != npos && dbl != npos && open_at != npos && money_off != npos && brk != npos && brk < money_on &&
              money_on < dbl && dbl < money_off && open_at - money_off < 40 && count_in(buy, "keys_open(") == 1 &&
              count_in(buy, "potion_break(") == 1 &&
              in_order(buy, { "money_ = true;", "if (!keys_shut()) {", "res.trouble = kKeysLateVi;",
                              "double_click(cx, cy)" }) &&
              buy.find("ms_between(confirmed_at, now()) < kShopFullLateMs") < money_off,
          "shop_purchase: shut, its keys seen up, before the double-click; opened again only after the tail looks, "
          "at the one place -- or for a potion break before the money stretch begins");
    // The review's K1: the look judged with the window open is judged again
    // on one taken after the shut, and the double-click goes by that.
    check(in_order(buy, { "if (const int end = judge(f0, shop); end >= 0) return end;",
                          "potion_break(f0, BagView{}, \"before the double-click\", nullptr, -1, true);",
                          "const bool went = keys_out_ || own_pressed_;",
                          "money_ = true;", "if (!keys_shut()) {", "res.trouble = kKeysLateVi;", "if (went) {",
                          "if (!grab_ || !grab_(f1) || !f1.valid()) {",
                          "if (const int end = judge(f1, shop); end >= 0) return end;", "shop_buttons_ = shop.buttons;",
                          "const int cx = hp ? shop.hp_x() : shop.mp_x(), cy = shop.item_y();",
                          "double_click(cx, cy)" }),
          "shop_purchase: keys let through while a look was judged -- the look judged again after the shut, the "
          "double-click aimed by it");
    const std::string run_pass = body(bag, "BagCleaner::Result BagCleaner::run_pass() {");
    check(in_order(run_pass, { "if (!keys_settle()) break;", "use_sweep(tab, v, res, stop, used, no_use, uses, "
                                                              "registered);" }) &&
              in_order(run_pass, { "if (fresh && bin_clear(v)) up = bag::find_dialogs(f, store_);",
                                   "if (fresh && !clean(v, up)) {", "if (fresh && !relook) {", "keys_open(f, v, &up);",
                                   "if (keys_out_) f_shut = false;" }) &&
              in_order(run_pass, { "if (!drag_grade(f_at, f_shut)) {", "relook = true;",
                                   "trash_one(f, v, static_cast<int>(s), count, &handed_f, &handed_v, &handed);",
                                   "if (relook) {", "if (!keys_settle()) break;" }),
          "run_pass: the use sweep after the window shut and a potion waited out; a slot dragged only from a look "
          "taken with it shut and half a second after a potion, else looked at again");

    // The macro: Gated never refuses every key; the sequence never asks.
    check(has(macro_h, "return why == Refused::Held || why == Refused::Unfocused || why == Refused::Suspended;") &&
              has(macro, "press(step.keycode, step.hold_ms, \"macro-sequence\", nullptr, false)") &&
              has(macro, "case Refused::Gated:     return coord_.critical_active() && !coord_.lets_through(vk);") &&
              has(macro, "const bool over = (refuses_all(pk.refused) || pk.refused == Refused::Gated) &&") &&
              has(pace, "return why == Refused::Gated && coord_.pass_keys_open() && !coord_.lets_through(vk);") &&
              has(pace, "pace_retry_ms_ = now + (why == Refused::Gated ? kSkillRetryMs : kGatePollMs);") &&
              count_in(pace, "shut_out(why, k.keycode)") == 2,
          "macro.cpp: Gated is no refusal of every key, is over the moment the key goes through, and the sequence "
          "never asks; a paced key never let through is passed over");
    check(has(hunter_h, "return returning_now_.load() && running_.load() && !coord_.critical_active();"),
          "Hunter::returning is never true under a hold, so the window's skills are not held for a walk back");
    check(count_in(read_file(root + "/src/hunter.cpp"), "request_key(") == 0 &&
              count_in(read_file(root + "/src/hunter.cpp"), "acquire_key(") == 0 &&
              count_in(macro, "coord_.acquire_key(") == 1,
          "only the macro's press asks through the window; the hunter never does");
#else
    skip("no source folder\n");
#endif
}

void test_hp_sources() {
    section("Key window: HP and the bag jobs -- where GoBot's window and the cleaner wire it in (their source, "
            "2026.11.11)");
#ifdef GOBOT_SOURCE_DIR
    const std::string root = GOBOT_SOURCE_DIR;
    const std::string gui = read_file(root + "/apps/gobot_gui/gui_app.cpp");
    const std::string bag = read_file(root + "/src/bag.cpp");
    const std::string macro = read_file(root + "/src/macro.cpp");
    const std::string cli = read_file(root + "/apps/gobot_cli/main.cpp");
    const std::string card = read_file(root + "/apps/gobot_gui/simple_shop.cpp");
    if (gui.empty() || bag.empty() || macro.empty() || cli.empty() || card.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto has = [](const std::string& text, const std::string& part) { return text.find(part) != npos; };
    const auto count_in = [](const std::string& text, const std::string& part) {
        int n = 0;
        for (std::size_t at = text.find(part); at != npos; at = text.find(part, at + 1)) ++n;
        return n;
    };
    const auto slice = [&](const std::string& text, const std::string& from, const std::string& to) {
        const std::size_t a = text.find(from);
        const std::size_t b = a == npos ? npos : text.find(to, a);
        return a == npos || b == npos ? std::string() : text.substr(a, b - a);
    };
    const auto body = [&](const std::string& text, const std::string& head) {
        const std::size_t a = text.find(head);
        const std::size_t b = a == npos ? npos : text.find("\n}", a);
        return a == npos || b == npos ? std::string() : text.substr(a, b + 2 - a);
    };
    const auto in_order = [](const std::string& text, std::initializer_list<const char*> parts) {
        std::size_t at = 0;
        for (const char* p : parts) {
            const std::size_t found = text.find(p, at);
            if (found == npos) return false;
            at = found + 1;
        }
        return true;
    };
    const std::string shop =
        slice(gui, "const auto run_shop = [&](bool forced, bool dry, bool after_pass) {", "while (!trash_stop_) {");
    const std::string pass = slice(gui, "const bool look_only = request == 2;", "trash_thrown_ += r.deleted");
    const std::string charms = slice(gui, "const auto run_charms = [&](bool forced) {", "// ---- the potion shop ----");
    if (shop.empty() || pass.empty() || charms.empty()) {
        check(false, "gui_app.cpp: run_shop, the pass and the charms found");
        return;
    }

    // chat_hp_floor_ as it was: set on the window's thread alone, and what
    // the charms' gate and guard go by.
    check(count_in(gui, "chat_hp_floor_ = ") == 1 &&
              in_order(gui, { "int floor = 50;", "macro_key_kind(k) == MacroKeyKind::HpPotion)",
                              "floor = std::max(floor, k.threshold_pct);", "chat_hp_floor_ = floor;" }) &&
              has(charms, "if (hp < static_cast<float>(chat_hp_floor_.load() + 10)) return not_now(\"Máu đang thấp, "
                          "dùng bùa sau.\");") &&
              has(charms, "return !(hp >= 0.0f && hp < static_cast<float>(chat_hp_floor_.load()));") &&
              !has(charms, "hp_gate") && !has(charms, "potion_break"),
          "chat_hp_floor_ unchanged -- max(50, the HP potions' highest mark), set in one place -- and the charms' gate "
          "and guard by it as before");
    // The gates: one helper, both jobs, by the job's own window -- every HP
    // potion the macro may choose let through (the review's HP-1). HP not
    // read waits for a round that rides a pass too (R3).
    check(has(gui, "return bag::hp_gates(keys.hp_all_through, keys.hp_mark, chat_hp_floor_.load());") &&
              count_in(gui, "hp_gates_of(pass_keys)") == 2 && !has(gui, "start_gate") &&
              in_order(shop, { "const PassKeys pass_keys = held || trash_left_held_ ? PassKeys{} : pass_keys_now();",
                               "const bag::HpGates hp_gate = hp_gates_of(pass_keys);",
                               "const float hp = hp_percent_now();",
                               "if (hp < 0.0f) return not_now(\"Chưa đọc được máu của nhân vật, mua bình sau.\", "
                               "15'000);",
                               "if (!after_pass) {", "if (hp < static_cast<float>(hp_gate.start)) {",
                               "return not_now(\"Máu đang thấp, mua bình sau.\", bag::kHpLookAgainMs);",
                               "const auto still_ours = [this, &hp_cut, stop_level = hp_gate.stop] {",
                               "const auto hp_under = [this, stop_level = hp_gate.stop] {",
                               "cleaner.set_guard(still_ours);" }) &&
              in_order(pass, { "const PassKeys pass_keys = left_held ? PassKeys{} : pass_keys_now();",
                               "const bag::HpGates hp_gate = hp_gates_of(pass_keys);", "if (request == 0 && !held) {",
                               "if (hp >= 0.0f && hp < static_cast<float>(hp_gate.start)) {",
                               "next = now_ms() + bag::kHpLookAgainMs;",
                               "cleaner.set_guard([this, &hp_cut, stop_level = hp_gate.stop] {" }),
          "the shop's start and the timed pass's: the HP potion's mark when it is drunk through the window, else over "
          "it by a margin, looked at again in 5 s; the guards stop at the job's level -- the window's set built "
          "first, an empty one when something may be left in the bin");
    // The pass's HP gate before its list is built (the review's PERF-PASS-HP-
    // 5S): put off every 5 s, it builds nothing.
    check(in_order(pass, { "if (request == 0 && !held) {", "next = now_ms() + bag::kHpLookAgainMs;", "continue;",
                           "TrashList list;", "list.build(exe_relative(\"configs/trash\"), settings, catalog);",
                           "const bag::PassTabs pass_tabs = bag::pass_tabs(settings, list);" }) &&
              count_in(pass, "list.build(") == 1,
          "the timed pass's HP gate asked before its list is built: a pass put off for HP builds none");
    // A stop for HP: the pass again in 15 s, the round put off (bag::shop_end),
    // never trouble, again in 15 s.
    check(in_order(pass, { "say_stopped(\"pass\", hp_cut, r.trouble);",
                           "if (hp_cut.level > 0 && !trash_stop_) next = now_ms() + bag::kHpStoppedAgainMs;",
                           "held = r.keeps_hold(held);" }) &&
              in_order(gui, { "if (hp_cut.level > 0 && !trash_stop_) next = now_ms() + bag::kHpStoppedAgainMs;",
                              "if (held) {", "next = now_ms() + 60'000;" }),
          "a pass its guard stopped for HP is tried again in 15 s, not the interval on -- a hold kept still a minute");
    check(in_order(shop, { "say_stopped(\"shop round\", hp_cut, r.trouble);",
                           "hp_stop = !trash_stop_ && hp_cut.level > 0;",
                           "cut = !trash_stop_ && !(coord_.critical_active() && coord_.critical_reason() == \"trash "
                           "cleaning\");",
                           "seen.stopped = trash_stop_;", "seen.hp_cut = hp_stop;", "seen.hold_lost = cut;",
                           "seen.keys_late = r.keys_late;", "const bag::ShopEnd end = bag::shop_end(seen);",
                           "else if (r.board_up) shop_due_ms = now_ms() + BagCleaner::kBoardWaitMs;",
                           "else if (hp_stop) shop_due_ms = now_ms() + bag::kHpStoppedAgainMs;",
                           "else if (end == bag::ShopEnd::Paused) shop_due_ms = now_ms() + "
                           "BagCleaner::kShopPartGapMs;" }) &&
              !has(shop, "cut = !trash_stop_ && !still_ours();") && count_in(shop, "!still_ours();") == 2 &&
              has(shop, "not_now(\"Máu đang thấp, mua bình sau.\", bag::kHpStoppedAgainMs);") &&
              has(bag, "res.keys_late = keys_late_;"),
          "a round its guard stopped for HP, by the guard's own word and not a look at HP after it, put off and "
          "again in 15 s; a key of the macro's late put off as well");
    // The full-bag message closed with HP low, never under another's hold.
    const std::string full = body(bag, "bool BagCleaner::shop_full_close(const ScreenRect& at, Result& res) {");
    check(has(full, "return cancelled_.load() || (hold_check_ ? !hold_check_() : guard_ && !guard_());") &&
              count_in(full, "guard_()") == 1 &&
              in_order(shop, { "cleaner.set_guard(still_ours);", "cleaner.set_hold_check(",
                               "[this] { return coord_.critical_active() && coord_.critical_reason() == \"trash "
                               "cleaning\"; });" }) &&
              count_in(gui, "set_hold_check(") == 1 && !has(cli, "set_hold_check("),
          "shop_full_close asks the hold alone -- a stop or another job's hold stop it, HP low does not -- set for "
          "the shop round only");
    // And a lone message in the box's place the same (the review's HP-2):
    // close_notice asks the hold alone in a shop round; a charm run, and a run
    // with no hold_check, the guard as before.
    const std::string notice = body(bag, "BagCleaner::NoticeClose BagCleaner::close_notice(");
    const std::string message = body(bag, "int BagCleaner::shop_message(");
    check(has(notice, "(mode_ == Mode::ShopBuy && hold_check_ ? !hold_check_() : guard_ && !guard_());") &&
              count_in(notice, "guard_()") == 1 &&
              in_order(message, { "} else if (c == NoticeClose::Stopped) {", "if (hold_check_)",
                                  "cancelled_ ? \"dừng giữa chừng\"",
                                  "\"bot không còn giữ phím (có việc khác đã lấy lại), nên dừng mua bình\";",
                                  "else if (!charm_must_stop(res, stop))" }),
          "close_notice asks the hold alone in a shop round -- a lone message in the box's place closed with HP low, "
          "never under another job's hold -- and charm runs ask the guard as before");
    // The break's hook: HP's or MP's potion due whose key this hold's window
    // lets through; never the pet's, and the window's own checks every time.
    const std::string vital = body(macro, "bool MacroEngine::vital_potion_due(");
    const std::string brk = body(bag, "bool BagCleaner::potion_break(");
    const std::string run_pass = body(bag, "BagCleaner::Result BagCleaner::run_pass() {");
    const std::string open_shop = body(bag, "bool BagCleaner::shop_open(");
    check(has(gui, "w.potion_due = [this] {") &&
              has(gui, "macro_->vital_potion_due([this](std::uint32_t vk) { return coord_.pass_key_armed(vk); })") &&
              in_order(vital, { "for (std::size_t n = 0; n < 2; ++n) {", "const MacroKeyKind kind = kPotionKinds[n];",
                                "const float v = kind == MacroKeyKind::HpPotion ? hp : mp;" }) &&
              !has(vital, "PetPotion") && !has(vital, "pet ?"),
          "the hook: the HP or MP potion due -- the first two kinds, never the pet's -- its key one the window armed "
          "for this hold lets through");
    // A window open already -- since a purchase's tail -- kept open for a
    // break only by keys_may_open's word on the look (the review's KEYS-1),
    // and not shut there: the purchase's own shut and K1 look go on.
    check(in_order(brk, { "if (!keys_.potion_due || !keys_.potion_due()) return false;", "if (!asked) {",
                          "if (!keys_out_) keys_open(f, v, up, lone, full_asked);",
                          "else if (!keys_may_open(f, v, up, lone, full_asked)) return false;",
                          "if (!keys_out_) return false;",
                          "while (waited < kPotionBreakMs && !cancelled_ && keys_.potion_due()) {" }) &&
              !has(brk, "keys_.open()") && !has(brk, "keys_shut(") && count_in(bag, "keys_.open()") == 1 &&
              count_in(bag, "potion_break(") == 4 &&
              in_order(run_pass, { "if (fresh && !relook) {", "keys_open(f, v, &up);", "if (keys_out_) f_shut = false;",
                                   "} else if (!fresh && potion_break(f, v, \"between two items\")) {",
                                   "f_shut = false;", "if (!drag_grade(f_at, f_shut)) {" }) &&
              in_order(open_shop, { "say(\"bấm chuột phải vào món Người Giữ Sân\");", "if (shop.found) {",
                                    "keys_open(g, v);",
                                    "potion_break(g, v, \"as the shop is seen\", nullptr, -1, false, true);",
                                    "return true;" }) &&
              BagCleaner::kPotionBreakMs == 400,
          "the break: through keys_open alone, or keys_may_open's checks of a window open already, 400 ms at most -- "
          "between two items on the look handed over, the slot then looked at again shut; as the shop is seen, "
          "keys_open asked of that look once; and before each double-click");
    check(has(card, "GoBot mua ở mọi mức máu, không chờ máu lên: bình HP \"") &&
              has(card, "\"và MP bấm xen giữa các lần mua, kể cả lúc bot đang dừng (khi đó GoBot tự bấm bình theo các "
                        "dòng bình \"") &&
              has(card, "std::to_string(bag::kHpStoppedAgainMs / 1000)") && !has(card, "nửa mức"),
          "simple_shop.cpp: the tooltip says the round goes at any HP (2026.11.11b), the potions between purchases "
          "-- GoBot's own with the bot stopped -- and never on a digit");
    // The run's own potion press (2026.11.11b, the owner: "GoBot tự bấm bình",
    // "Giữ chặn phím số"): with no key window -- the macro not running -- at
    // the break's moments, on keys_clear's word (nothing up, never the money
    // stretch nor a charm run), through the run's own press, then the potion
    // waited out: a drag only from a look begun after it, a double-click only
    // from a look taken after it.
    const std::string own = body(bag, "bool BagCleaner::press_own_potion(");
    const std::string grade = body(bag, "bool BagCleaner::drag_grade(");
    const std::string own_gui = slice(gui, "const auto own_potions = [&] {", "const auto key_window = [this] {");
    check(in_order(brk, { "if (own_potions_.due) return press_own_potion(f, v, where, up, lone, full_asked);",
                          "if (!keys_.potion_due || !keys_.potion_due()) return false;" }) &&
              in_order(own, { "const std::uint32_t vk = own_potions_.due(line);",
                              "if (vk == 0 || !keys_clear(f, v, up, lone, full_asked)) return false;",
                              "if (!press(vk)) return false;", "own_potion_at_ = now();", "own_pressed_ = true;",
                              "sleep(kPotionSettleMs);" }) &&
              !has(own, "keys_.open") && !has(own, "input_.") && !has(own, "hands_.") &&
              in_order(grade, { "if (own_potion_any_ && at - own_potion_at_ < std::chrono::milliseconds(kPotionSettleMs)) "
                                "return false;",
                                "if (!keys_.open) return true;" }) &&
              in_order(body(bag, "int BagCleaner::shop_purchase(bool hp, Result& res) {"),
                       { "own_pressed_ = false;",
                         "potion_break(f0, BagView{}, \"before the double-click\", nullptr, -1, true);",
                         "const bool went = keys_out_ || own_pressed_;", "money_ = true;", "if (went) {" }) &&
              has(gui, "else if (!held && !trash_left_held_) cleaner.set_own_potions(own_potions());") &&
              has(gui, "else if (!left_held && !look_only) cleaner.set_own_potions(own_potions());") &&
              count_in(gui, "set_own_potions(") == 2 && !has(charms, "own_potions") && !has(cli, "set_own_potions") &&
              in_order(own_gui, { "if (!macro_ || macro_->running()) return 0;",
                                  "const MacroEngine::OwnPotion p = macro_->own_potion_due(never);",
                                  "if (p.refused != 0 && !*refused_said) {", "if (p.vk == 0) return 0;",
                                  "if (at != own_potion_ms.end() && now_ms() < at->second + p.cooldown_ms) return 0;" }),
          "the run's own potion press: only with no window, on keys_clear, through press(), the potion waited out "
          "before a drag or a double-click; set for a pass (not a look only) and a shop round, never a bin-held job, a "
          "charm run or gobot_cli; the macro running presses its own; each key's cooldown kept");
#else
    skip("no source folder\n");
#endif
}

void test_nav_sources() {
    section("Key window: Home, End, PgUp, PgDn, Insert and Delete -- where the coordinator, the cleaner, GoBot's "
            "window and the simple screen wire them in (their source, 2026.11.11)");
#ifdef GOBOT_SOURCE_DIR
    const std::string root = GOBOT_SOURCE_DIR;
    const std::string gui = read_file(root + "/apps/gobot_gui/gui_app.cpp");
    const std::string bag = read_file(root + "/src/bag.cpp");
    const std::string macro = read_file(root + "/src/macro.cpp");
    const std::string coord = read_file(root + "/src/execution_coordinator.cpp");
    const std::string view = read_file(root + "/apps/gobot_gui/simple_view.cpp");
    const std::string card = read_file(root + "/apps/gobot_gui/simple_shop.cpp");
    if (gui.empty() || bag.empty() || macro.empty() || coord.empty() || view.empty() || card.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto has = [](const std::string& text, const std::string& part) { return text.find(part) != npos; };
    const auto count_in = [](const std::string& text, const std::string& part) {
        int n = 0;
        for (std::size_t at = text.find(part); at != npos; at = text.find(part, at + 1)) ++n;
        return n;
    };
    const auto body = [&](const std::string& text, const std::string& head) {
        const std::size_t a = text.find(head);
        const std::size_t b = a == npos ? npos : text.find("\n}", a);
        return a == npos || b == npos ? std::string() : text.substr(a, b + 2 - a);
    };
    const auto in_order = [](const std::string& text, std::initializer_list<const char*> parts) {
        std::size_t at = 0;
        for (const char* p : parts) {
            const std::size_t found = text.find(p, at);
            if (found == npos) return false;
            at = found + 1;
        }
        return true;
    };

    // One predicate, asked everywhere a key may go through.
    check(has(coord, "return vk == 0x21 || vk == 0x22 || vk == 0x23 || vk == 0x24 || vk == 0x2D || vk == 0x2E;") &&
              has(coord, "return (vk >= 'A' && vk <= 'Z') || vk == 0x20 || pass_key_nav(vk);") &&
              has(coord, "if (!pass_open_ || !pass_keys_.has(vk) || !pass_key_allowed(vk)) return KeyWay::Shut;") &&
              has(coord, "if (!pass_key_allowed(vk) || kept.has(vk)) continue;") &&
              has(coord, "pass_armed_ && pass_open_ && pass_keys_.has(vk) && pass_key_allowed(vk);") &&
              has(macro, "if (!macro_timer_on(k) || !pass_key_allowed(k.keycode) || refused(k.keycode)) continue;"),
          "pass_key_allowed: the letters, Space and the six by their codes -- asked by arm, request_key, "
          "lets_through and the macro's set alike");
    const std::string release = body(coord, "void ExecutionCoordinator::release(const std::string& owner) {");
    check(in_order(release, { "if (!held_.load() || owner_ != owner) return;",
                              "if (through_ && pass_key_nav(through_vk_)) pass_nav_ms_ = "
                              "std::max<std::uint64_t>(now_ms(), 1);",
                              "through_ = false;" }) &&
              has(coord, "through_vk_ = vk;") && count_in(coord, "pass_nav_ms_ = 0;") == 1 &&
              has(body(coord, "std::string ExecutionCoordinator::pass_keys_line() const {"),
                  "InputManager::key_name(t.first)"),
          "the coordinator: when one of the six came up kept as its holder lets go, forgotten as a window is armed; "
          "the line by the keys' names");
    // The cleaner: the settle in the one place every input of its own passes.
    const std::string shut = body(bag, "bool BagCleaner::keys_shut() {");
    check(in_order(shut, { "if (!keys_.shut || keys_.shut(kKeysUpWaitMs)) {",
                           "const int ago = keys_.nav_ago_ms ? keys_.nav_ago_ms() : -1;",
                           "if (ago >= 0 && ago < kNavSettleMs) pause_ms(kNavSettleMs - ago);", "return true;" }) &&
              count_in(bag, "keys_.shut(") == 1 && count_in(bag, "keys_.nav_ago_ms()") == 1 &&
              BagCleaner::kNavSettleMs >= 150 && BagCleaner::kNavSettleMs <= 200,
          "keys_shut: after the keys are seen up, " + std::to_string(BagCleaner::kNavSettleMs) +
              " ms from the last of the six -- before the input it guards and any look after it");
    check(has(gui, "w.nav_ago_ms = [this] {") && has(gui, "const std::uint64_t at = coord_.pass_nav_ms();"),
          "GoBot's window: the hook by the coordinator's time");
    // The double-click only by a look with the row on it; shop_now's fallback
    // kept for the close paths and the run's end.
    const std::string buy = body(bag, "int BagCleaner::shop_purchase(bool hp, Result& res) {");
    const std::string now = body(bag, "bag::Shop BagCleaner::shop_now(const Frame& f) const {");
    check(in_order(buy, { "const auto judge = [&](Frame& f0, bag::Shop& s) -> int {", "for (int again = 0;; ++again) {",
                          "s = f0.valid() ? shop_now(f0) : bag::Shop{};", "if (board_up(f0, res)) {",
                          "if (!s.found) {", "if (lone_message(f0, &label, &shot)) {",
                          "if (s.row_score > 0.0f) return -1;", "if (!keys_shut()) {",
                          "GB_INFO(\"Trash: shop row not matched before the double-click on %s -- its buttons alone "
                          "at %d,%d (row \"",
                          "if (again >= kShopRowRelooks) {", "keep(f0, \"shop_row_unseen\");",
                          "return kShopStopped;", "if (!wait_ms(kShopLookMs)) return kShopStopped;",
                          "f0 = std::move(g);", "double_click(cx, cy)" }) &&
              count_in(buy, "return -1;") == 1 && !has(now, "row_score >") &&
              has(now, "s.row = ScreenRect{ m.at.x - bag::kShopButtonsDx, m.at.y - shop_row_dy_, 294, 21 };") &&
              BagCleaner::kShopRowRelooks == 3,
          "shop_purchase's judge: a look passes only with the potions' row matched; the buttons alone, the window "
          "shut, looked at again 3 times with the scores said, then stopped -- shop_now's fallback as it was");
    // The simple screen: Delete a key like any other, Backspace clears.
    check(has(view, "{ ImGuiKey_PageDown, VK_NEXT },     { ImGuiKey_Delete, VK_DELETE },") &&
              has(view, "} else if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {") &&
              !has(view, "ImGui::IsKeyPressed(ImGuiKey_Delete, false)") &&
              has(view, "\"Nhấn phím muốn dùng. Esc để huỷ, Backspace để bỏ phím này.\"") &&
              has(view, "\"\\nMuốn bỏ phím này: bấm vào ô rồi nhấn Backspace.\"") && !has(view, "Delete để bỏ") &&
              !has(view, "rồi nhấn Delete"),
          "the simple screen: Delete is set like any key, Backspace alone clears the box, and the help says so");
    check(has(view, "Home, End, PgUp, PgDn, Insert và Delete của kỹ năng, bình và buff vẫn bấm giữa các lần ") &&
              has(card, "Home, End, PgUp, PgDn, Insert và Delete \"") &&
              has(card, "\"phím chữ cái hoặc Home, End, PgUp, PgDn, Insert, Delete. Không mua khi đang chat, \""),
          "the cards: the six go as the letters do, and a digit potion waits -- moved to a letter or one of the six "
          "to go");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_pass_keys(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_allowed();
    test_window();
    test_scope();
    test_nav_window();
    test_set();
    test_macro_through();
    test_vital_due();
    test_own_due();
    test_hp_gates();
    test_quiet();
    test_sources();
    test_hp_sources();
    test_nav_sources();
}
