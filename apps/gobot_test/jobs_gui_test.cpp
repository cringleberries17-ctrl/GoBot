// gobot_test -- the jobs and GoBot's window in 2026.11.08: the potion shop's and
// the quick quest's quiet second spoiled, waited out longer each time (R4) and
// said in the log by what the pointer, its buttons and the window in front did;
// the charms' and the shop's clocks kept across a MACRO OFF and ON (R5); a key on
// a buff row and a skill row at once, warned of and never refused (R7); the
// "Ra chiêu theo vòng" tick and its status (W1's switch); the words that said
// "Dùng skill tuần tự" works without Tự đánh quái, or that GoC waits for the
// server (R8); and the release's notes -- DOC-TRUOC.txt, README.md,
// START-HERE (package.ps1) and macro.ini -- against the numbers and the words
// the code has. And 2026.11.09's notes the same way: the potion shop bought
// by the slot, and the check-in board closed under "Tự đóng thông báo".
//
// Nothing here goes near the desktop, the pointer, the keyboard's state or the
// game. The glances at the pointer are a pretend desktop's (InputGlancer's
// parts set by the test), the quest's rounds are made up, the macro runs a step
// at a time on a clock of the test's own with a pretend keyboard -- or once on
// its own threads, the keyboard still pretend -- and where only GoBot's window
// does a thing, its source is read.

#include "gobot/bag.hpp"
#include "gobot/cast_match.hpp"
#include "gobot/checkin_board.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/macro.hpp"
#include "gobot/popup_closer.hpp"
#include "gobot/quick_quest.hpp"
#include "gobot/quiet_second.hpp"
#include "gobot/vitals_trust.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

int count_in(const std::string& text, const std::string& part) {
    int n = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
    return n;
}

std::string ms_list(const std::vector<std::uint64_t>& v) {
    std::string out;
    for (const std::uint64_t ms : v) out += (out.empty() ? "" : ", ") + std::to_string(ms / 1000) + " s";
    return out;
}

// ------------------------------------------------------------ the back-off

void test_back_off() {
    section("The quiet second spoiled: 30 s, then 2, 5 and 10 minutes, never past the job's interval (R4)");

    const auto run = [](std::uint64_t cap_ms, int n) {
        QuietBackOff b;
        std::vector<std::uint64_t> out;
        for (int i = 0; i < n; ++i) out.push_back(b.next_ms(cap_ms));
        return out;
    };
    const std::vector<std::uint64_t> two_hours = run(120ull * 60'000, 6);
    check(two_hours == std::vector<std::uint64_t>{ 30'000, 120'000, 300'000, 600'000, 600'000, 600'000 },
          "the shop at 120 minutes: " + ms_list(two_hours));
    const std::vector<std::uint64_t> fifteen = run(15ull * 60'000, 4);
    check(fifteen == std::vector<std::uint64_t>{ 30'000, 120'000, 300'000, 600'000 },
          "at 15 minutes, the shop's shortest: " + ms_list(fifteen));
    const std::vector<std::uint64_t> three = run(3ull * 60'000, 4);
    check(three == std::vector<std::uint64_t>{ 30'000, 120'000, 180'000, 180'000 },
          "capped at a 3-minute interval: " + ms_list(three));
    const std::vector<std::uint64_t> one = run(60'000, 3);
    check(one == std::vector<std::uint64_t>{ 30'000, 60'000, 60'000 }, "the quest at 1 minute: " + ms_list(one));
    const std::vector<std::uint64_t> tiny = run(10'000, 3);
    check(tiny == std::vector<std::uint64_t>{ 30'000, 30'000, 30'000 },
          "an interval under 30 s never makes the wait shorter than before: " + ms_list(tiny));

    QuietBackOff b;
    b.next_ms(3'600'000);
    b.next_ms(3'600'000);
    const std::uint64_t peeked = b.peek_ms(3'600'000);
    check(peeked == 300'000 && b.peek_ms(3'600'000) == 300'000 && b.level() == 2,
          "peek_ms says the next wait (5 min) and moves nothing");
    b.reset();
    check(b.level() == 0 && b.next_ms(3'600'000) == 30'000, "reset: 30 s again");
    b.set_level(9);
    const int high = b.level();
    b.set_level(-3);
    check(high == QuietBackOff::kSteps - 1 && b.level() == 0, "a kept level is taken within 0 to 3");
}

// ------------------------------------------------------------ the glances

/// A pretend desktop: where the pointer is, the buttons down, the program in
/// front -- and which buttons were asked about.
struct Desk {
    bool read = true;
    long x = 0, y = 0;
    std::set<int> down;
    std::string front;
    std::vector<int> asked;

    InputGlancer glancer() {
        InputGlancer g;
        g.cursor = [this](long& gx, long& gy) {
            if (!read) return false;
            gx = x;
            gy = y;
            return true;
        };
        g.button_down = [this](int vk) {
            asked.push_back(vk);
            return down.count(vk) != 0;
        };
        g.front_program = [this] { return front; };
        return g;
    }
};

void test_glances() {
    section("The quiet second spoiled, said in the log: the pointer, its buttons, the program in front (R4)");

    Desk d;
    d.x = 812;
    d.y = 440;
    d.front = "chrome.exe";
    const InputGlancer g = d.glancer();
    const InputGlance before = g.glance();
    d.x = 845;
    d.y = 452;
    const InputGlance after = g.glance();
    check(before.cursor && before.x == 812 && after.x == 845 && after.y == 452 && after.front == "chrome.exe",
          "a glance reads the pointer and the program in front through the desktop it is given");
    check(d.asked == std::vector<int>{ VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2,
                                       VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 },
          "and asks of the five mouse buttons only, no other key");
    const std::string moved = quiet_spoiled_text(before, after);
    check(moved == "the pointer moved 35 px (812,440 -> 845,452); no mouse button held; in front: chrome.exe",
          "moved: \"" + moved + "\"");

    const std::string still = quiet_spoiled_text(after, after);
    check(still == "the pointer still at 845,452 (a key, the wheel, or a move there and back); no mouse button "
                   "held; in front: chrome.exe",
          "still, no button: \"" + still + "\"");

    d.down = { VK_LBUTTON, VK_XBUTTON1 };
    d.front = "Game.exe";
    const InputGlance held = g.glance();
    const std::string clicked = quiet_spoiled_text(after, held);
    check(clicked == "the pointer still at 845,452; buttons held: left, X1; in front: chrome.exe, then Game.exe",
          "a button held, and the window in front changed: \"" + clicked + "\"");

    d.read = false;
    d.front.clear();
    const InputGlance unread = g.glance();
    const std::string lost = quiet_spoiled_text(held, unread);
    check(!unread.cursor && lost == "the pointer not read; buttons held: left, X1; in front: Game.exe, then ?",
          "the pointer not read, the program not known: \"" + lost + "\"");

    const InputGlance none = InputGlancer{}.glance();
    check(!none.cursor && none.buttons == 0 && none.front.empty(), "a glancer with no parts reads nothing");
}

// ------------------------------------------------------------ the clocks kept

void test_resumed_clock() {
    section("The charms' and the shop's clocks kept across a MACRO OFF and ON, never sooner than a start's (R5)");

    const std::uint64_t now = 5'000'000;
    check(resumed_clock(0, now, 8'000) == now + 8'000 && resumed_clock(0, now, 45'000) == now + 45'000,
          "none kept: the charms 8 s on and the shop 45 s on, as every start had them");
    check(resumed_clock(now - 60'000, now, 45'000) == now + 45'000,
          "a clock already past -- due while the macro was off -- waits a start's 45 s still");
    check(resumed_clock(now + 600'000, now, 8'000) == now + 600'000,
          "a later one -- ten minutes after a tab not read -- is kept, not brought to 8 s");
}

// ------------------------------------------------------------ the quest

/// QuestRun with rounds made up: each says what `next` says.
struct QuestRig {
    QuestRun run{ 11 };
    QuestResult next;
    std::vector<std::string> said;
    int rounds = 0;
    std::uint64_t now = 2'000'000;

    explicit QuestRig(int interval_min = 5) {
        QuestRun::Hooks h;
        h.gates = [] {
            QuestGateInput in;
            in.window = in.focused = true;
            in.hp_pct = 100.0f;
            return in;
        };
        h.round = [this](const QuestCall&) {
            ++rounds;
            return next;
        };
        h.say = [this](const std::string& s) { said.push_back(s); };
        run.set_hooks(h);
        QuestSettings s;
        s.enabled = true;
        s.first_delay_s = 0;
        s.interval_min = interval_min;
        run.start(now, s);
        touched();
    }
    void touched() {
        next = QuestResult{};
        next.outcome = QuestOutcome::NotNow;
        next.why = "bạn đang dùng chuột hoặc bàn phím";
        next.touched = true;
    }
    /// One round now; how long until the next is due.
    std::uint64_t round() {
        run.step(now);
        const std::uint64_t at = run.next_at();
        const std::uint64_t wait = at > now ? at - now : 0;
        now = std::max(now, at);
        return wait;
    }
};

void test_quest_back_off() {
    section("Quick quest: a timed round's quiet second spoiled, waited out longer each time; reset (R4)");

    {
        QuestRig q;
        std::vector<std::uint64_t> waits;
        for (int i = 0; i < 5; ++i) waits.push_back(q.round());
        check(q.rounds == 5 && waits == std::vector<std::uint64_t>{ 30'000, 120'000, 300'000, 300'000, 300'000 },
              "5-minute rounds, spoiled five times: " + ms_list(waits) + " -- capped at the interval");
        check(!q.said.empty() && q.said.back() == "Chờ: bạn đang dùng chuột hoặc bàn phím; thử lại sau 5 phút.",
              "and the tab is told when: \"" + (q.said.empty() ? std::string() : q.said.back()) + "\"");

        // Another not-now after its quiet second had passed: from 30 s again.
        q.next = QuestResult{};
        q.next.outcome = QuestOutcome::NotNow;
        q.next.why = "game không ở trên cùng";
        q.next.quiet = true;
        const std::uint64_t gone_behind = q.round();
        q.touched();
        const std::uint64_t after_quiet = q.round();
        check(gone_behind == 30'000 && after_quiet == 30'000 && q.run.quiet_level() == 1,
              "a not-now after a quiet second that passed: 30 s, and the next spoiled one 30 s again");

        // A not-now before the hold -- its second never looked at -- moves nothing.
        q.round();
        q.next = QuestResult{};
        q.next.outcome = QuestOutcome::NotNow;
        q.next.why = "đang dọn rác";
        const int level = q.run.quiet_level();
        const std::uint64_t trash = q.round();
        check(trash == 30'000 && level == 2 && q.run.quiet_level() == 2,
              "a not-now before the hold (the trash cleaner): 30 s as ever, the back-off kept");

        // A round that ran: its quiet second passed.
        q.next = QuestResult{};
        q.next.outcome = QuestOutcome::AlreadyOn;
        q.round();
        check(q.run.quiet_level() == 0, "a round that ran: the back-off from its first step");
    }
    {
        // Làm ngay 1 lần: the back-off from the first step, and a round asked
        // for that is spoiled looks again a second later, moving nothing.
        QuestRig q;
        q.round();
        q.round();
        const int before = q.run.quiet_level();
        q.run.ask_once(q.now);
        q.run.step(q.now);
        const int asked = q.run.quiet_level();
        const int rounds = q.rounds;
        q.now += 1000;
        q.run.step(q.now);
        check(before == 2 && asked == 0 && q.rounds == rounds + 1,
              "Làm ngay 1 lần: the back-off reset, and its round spoiled is asked again a second later");
    }
    {
        // Ticked off and on again in a run (QuestRun::update), and a new run.
        QuestRig q;
        q.round();
        q.round();
        QuestSettings off;
        off.enabled = false;
        q.run.update(off);
        const int kept = q.run.quiet_level();
        QuestSettings on;
        on.enabled = true;
        q.run.update(on);
        const int ticked = q.run.quiet_level();
        q.round();
        q.round();
        QuestSettings again;
        again.enabled = true;
        again.first_delay_s = 0;
        q.run.start(q.now, again);
        check(kept == 2 && ticked == 0 && q.run.quiet_level() == 0,
              "the box ticked on again: from 30 s; and a new run (BẮT ĐẦU) starts there too");
    }
    {
        // Every other not-now as before: 30 s, whatever came before.
        QuestRig q(1);
        q.next = QuestResult{};
        q.next.outcome = QuestOutcome::NotNow;
        q.next.why = "bot đang bấm phím";
        const std::uint64_t a = q.round();
        const std::uint64_t b = q.round();
        check(a == 30'000 && b == 30'000 && q.run.quiet_level() == 0,
              "\"bot đang bấm phím\" and the like: 30 s each time, as before");
    }
    {
        // A round that ran, its quiet second passed, and was then stopped --
        // the hold taken over, the keeper crossing: from 30 s again.
        QuestRig q;
        q.round();
        q.round();
        const int before = q.run.quiet_level();
        q.next = QuestResult{};
        q.next.outcome = QuestOutcome::Cancelled;
        q.next.quiet = true;
        q.round();
        const int stopped = q.run.quiet_level();
        q.touched();
        const std::uint64_t next = q.round();
        check(before == 2 && stopped == 0 && next == 30'000,
              "a round stopped after its quiet second passed: the back-off from its first step, the next spoiled "
              "one 30 s");
    }
    {
        // ... or halted in it, low HP or the game gone behind: the same.
        QuestRig q;
        q.round();
        q.round();
        q.next = QuestResult{};
        q.next.outcome = QuestOutcome::NotNow;
        q.next.why = "máu thấp";
        q.next.quiet = true;
        q.round();
        const int halted = q.run.quiet_level();
        q.touched();
        const std::uint64_t next = q.round();
        check(halted == 0 && next == 30'000,
              "a round halted for low HP after its quiet second passed: from 30 s again");
    }
    {
        // A stop during the quiet second -- it never passed: the back-off kept.
        QuestRig q;
        q.round();
        q.round();
        q.next = QuestResult{};
        q.next.outcome = QuestOutcome::Cancelled;
        q.round();
        check(q.run.quiet_level() == 2, "a stop before the quiet second passed: the back-off kept");
    }
}

// ------------------------------------------------------------ a buff and a skill

MacroKey key(std::uint32_t vk, std::uint32_t interval_ms, bool on, bool buff) {
    MacroKey k;
    k.keycode = vk;
    k.enabled = on;
    k.interval_ms = interval_ms;
    k.buff = buff;
    k.jitter_pct = 0;
    k.label = buff ? "Buff" : "Skill";
    return k;
}

/// The owner's rows of 2026-11-08 22:13-22:15: Z X and the rest as skills, a
/// 60-s and a 300-s buff on A and S, and A and S as 3-s attack skills too.
std::vector<MacroKey> owner_rows(bool buffs_on) {
    MacroKey hp = key('F', 3000, true, false);
    hp.trigger = MacroTrigger::MpBelow;
    hp.threshold_pct = 59;
    return { key('Z', 500, true, false),   key('X', 1000, true, false), key('C', 1000, true, false),
             key('V', 1000, true, false),  key('A', 60'000, buffs_on, true), key('S', 300'000, buffs_on, true),
             hp,                           key('S', 3000, true, false),     key('A', 3000, true, false) };
}

void test_buff_skill_clash() {
    section("A key on a buff row and a skill row, both on: warned of in the window and the log, never refused (R7)");

    const std::vector<MacroKey> both = owner_rows(true);
    check(macro_buff_skill_clashes(both) == std::vector<std::uint32_t>{ 'A', 'S' },
          "the owner's rows of 22:13: A and S, in the buff rows' order");
    check(macro_buff_skill_clashes(owner_rows(false)).empty() && macro_clash_note_vi(owner_rows(false)).empty() &&
              macro_clash_line(owner_rows(false)).empty(),
          "the buff rows switched off (22:15 on): nothing to say");
    {
        std::vector<MacroKey> keys = both;
        keys[7].enabled = false;        // S's skill row off
        keys[8].enabled = false;        // A's
        check(macro_buff_skill_clashes(keys).empty(), "the skill rows switched off: nothing");
        keys = both;
        keys.push_back(key('A', 120'000, true, true));      // A a buff twice over
        keys.push_back(key('F', 60'000, true, true));       // F a buff, and the MP potion's key
        check(macro_buff_skill_clashes(keys) == std::vector<std::uint32_t>{ 'A', 'S' },
              "a key on two buff rows said once; a buff on a potion's key is no skill");
        keys = both;
        keys[8].interval_ms = 0;        // a skill at 0 is never pressed
        check(macro_buff_skill_clashes(keys) == std::vector<std::uint32_t>{ 'S' },
              "a skill row the timers never press (0 ms) is none");
    }
    const std::string note = macro_clash_note_vi(both);
    check(note == "Phím A, S đang bật ở cả buff lẫn kỹ năng: GoBot bấm các phím đó theo cả hai giờ, và ghi lại như "
                  "buff. Là chiêu đánh thì tắt hoặc xóa dòng buff; là buff thì xóa nó khỏi kỹ năng.",
          "the window's words: \"" + note + "\"");
    const std::string line = macro_clash_line(both);
    check(line == "Macro: A, S are on a buff row and a skill row, both on -- pressed on both clocks, and logged as "
                  "a buff (a warning only: nothing is changed)",
          "the log's: \"" + line + "\"");

    // The macro presses it on both clocks, as before: warned, never refused.
    const auto presses_of_a = [](const std::vector<MacroKey>& keys) {
        InputManager input;
        ExecutionCoordinator coord;
        MacroEngine engine(input, coord);
        std::uint64_t t = 40'000'000;
        int a = 0;
        MacroEngine::Clock c;
        c.now_ms = [&t] { return t; };
        c.sleep_ms = [&t](std::uint32_t ms) { t += ms; };
        engine.set_clock(c);
        MacroEngine::Hands h;
        h.focused = [] { return true; };
        h.key = [&t, &a](std::uint32_t vk, int hold) {
            if (vk == 'A') ++a;
            if (hold > 0) t += static_cast<std::uint64_t>(hold);
            return true;
        };
        engine.set_hands(h);
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys = keys;
        cfg.sequence_enabled = false;
        cfg.global_jitter_pct = 0;
        cfg.key_hold_min_ms = cfg.key_hold_max_ms = 80;
        engine.set_config(cfg);
        engine.start_stepped();
        for (const std::uint64_t until = t + 10'000; t < until; t += 10) {
            engine.set_vitals(100.0f, 100.0f);
            engine.step();
        }
        engine.stop();
        return a;
    };
    std::vector<MacroKey> skill_only = both;
    skill_only[4].enabled = false;
    skill_only[5].enabled = false;
    const int with_buff = presses_of_a(both), alone = presses_of_a(skill_only);
    check(alone >= 3 && with_buff > alone,
          "the macro still presses A as a buff and as a skill (" + std::to_string(with_buff) + " presses of A in "
              "10 s, " + std::to_string(alone) + " as a skill alone)");

    // Said once as the macro comes on, from its own thread -- the pretend
    // keyboard still, for a moment of the real clock.
    {
        Logger& log = Logger::instance();
        const LogLevel was = log.level();
        log.set_ring_capacity(4000);
        log.clear_ring();
        log.set_level(LogLevel::Info);
        InputManager input;
        ExecutionCoordinator coord;
        MacroEngine engine(input, coord);
        MacroEngine::Hands h;
        h.focused = [] { return false; };      // nothing goes out: the game "behind"
        h.key = [](std::uint32_t, int) { return true; };
        engine.set_hands(h);
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys = both;
        cfg.sequence_enabled = false;
        engine.set_config(cfg);
        engine.start();
        engine.stop();
        int said = 0;
        for (const LogLine& l : log.ring())
            if (has(l.text, "are on a buff row and a skill row, both on")) ++said;
        // "Repeating keys" off: neither row is pressed, so nothing is said.
        log.clear_ring();
        cfg.timers_enabled = false;
        engine.set_config(cfg);
        engine.start();
        engine.stop();
        int said_off = 0;
        for (const LogLine& l : log.ring())
            if (has(l.text, "are on a buff row and a skill row, both on")) ++said_off;
        log.set_level(was);
        log.set_ring_capacity(0);
        check(said == 1, "MACRO ON: the log line once (" + std::to_string(said) + ")");
        check(said_off == 0, "MACRO ON with \"Repeating keys\" off: no line (" + std::to_string(said_off) + ")");
    }
}

// ------------------------------------------------------------ the rotation's status

void test_pace_status_text() {
    section("\"Ra chiêu theo vòng\": its status in a few words, off as shipped (W1's switch)");

    MacroConfig shipped_default;
    shipped_default.apply_defaults();
    check(!MacroConfig{}.cast_paced && !shipped_default.cast_paced, "off in a config built in code");

    MacroEngine::PaceStatus s;
    s.mode = MacroEngine::PaceStatus::Mode::Paced;
    s.lock_ms = 680;
    s.rounds = 12;
    check(MacroEngine::pace_status_vi(s) == "Đang theo vòng — khóa chiêu ước 680 ms, 12 vòng.",
          "paced: \"" + MacroEngine::pace_status_vi(s) + "\"");
    s.mode = MacroEngine::PaceStatus::Mode::Fallback;
    s.last_cast_ms = 0;
    check(MacroEngine::pace_status_vi(s) == "Tạm theo giờ: chưa thấy gói tin ra chiêu nào của bạn.",
          "no cast of yours yet: \"" + MacroEngine::pace_status_vi(s) + "\"");
    s.last_cast_ms = 7'000'000;
    check(MacroEngine::pace_status_vi(s) ==
              "Tạm theo giờ vì không thấy gói tin ra chiêu — khóa chiêu ước 680 ms; thấy lại thì theo vòng tiếp.",
          "fallen back: \"" + MacroEngine::pace_status_vi(s) + "\"");
    s.mode = MacroEngine::PaceStatus::Mode::Off;
    check(MacroEngine::pace_status_vi(s) == "Kỹ năng đang theo trận đánh (Tự đánh quái), nên không theo vòng.",
          "off with the switch on -- the skills following the fight: \"" + MacroEngine::pace_status_vi(s) + "\"");
    s.others = true;
    check(MacroEngine::pace_status_vi(s) ==
              "Tự đánh quái hoặc chuỗi phím cũng đang bấm phím, nên không theo vòng: mỗi phím bấm theo giờ như thường.",
          "off with the switch on -- the hunter or the sequence pressing keys too: \"" +
              MacroEngine::pace_status_vi(s) + "\"");
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

/// `src` from `from` up to `to`, or "" when either is missing.
std::string slice(const std::string& src, const std::string& from, const std::string& to) {
    const std::size_t a = src.find(from);
    const std::size_t b = a == std::string::npos ? std::string::npos : src.find(to, a);
    return a == std::string::npos || b == std::string::npos ? std::string() : src.substr(a, b - a);
}

/// Whether `first` comes before `second` in `text`, both there.
bool in_order(const std::string& text, std::initializer_list<const char*> parts) {
    std::size_t at = 0;
    for (const char* p : parts) {
        const std::size_t found = text.find(p, at);
        if (found == std::string::npos) return false;
        at = found + 1;
    }
    return true;
}

/// `text` with every run of spaces and line ends one space: a phrase of the
/// notes found however they wrap it. `ini` drops a comment's leading '#' too.
std::string flat(const std::string& text, bool ini = false) {
    std::string out;
    out.reserve(text.size());
    bool space = false, line_start = true;
    for (const char c : text) {
        if (c == '\r' || c == '\n') {
            space = line_start = true;
            continue;
        }
        if (c == ' ' || c == '\t') {
            space = true;
            continue;
        }
        if (ini && line_start && c == '#') {
            line_start = false;
            space = true;
            continue;
        }
        line_start = false;
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

/// The N of `src`'s "constexpr std::uint64_t `name` = N;" -- a file's own
/// constant the tests cannot name -- or 0 when it is not there.
std::uint64_t source_ms(const std::string& src, const std::string& name) {
    const std::string head = "constexpr std::uint64_t " + name + " = ";
    const std::size_t at = src.find(head);
    if (at == std::string::npos) return 0;
    std::uint64_t v = 0;
    for (std::size_t i = at + head.size(); i < src.size() && src[i] != ';'; ++i) {
        if (src[i] == '\'') continue;
        if (src[i] < '0' || src[i] > '9') return 0;
        v = v * 10 + static_cast<std::uint64_t>(src[i] - '0');
    }
    return v;
}

/// The string literal of `src` that `head` opens, up to its closing quote; ""
/// when there is none.
std::string literal_after(const std::string& src, const std::string& head) {
    const std::size_t at = src.find(head);
    if (at == std::string::npos) return {};
    const std::size_t from = at + head.size();
    const std::size_t to = src.find('"', from);
    return to == std::string::npos ? std::string() : src.substr(from, to - from);
}
#endif

void test_jobs_wiring() {
    section("The shop's and the quest's waits, the clocks kept, the tick and the notes: wired in (their source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string hpp = source("/apps/gobot_gui/gui_app.hpp");
    const std::string quest = source("/apps/gobot_gui/simple_quest.cpp");
    const std::string shop_card = source("/apps/gobot_gui/simple_shop.cpp");
    const std::string use = source("/apps/gobot_gui/simple_trash_use.cpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    const std::string skills = source("/apps/gobot_gui/simple_skills.cpp");
    const std::string macro = source("/src/macro.cpp");
    const std::string macro_h = source("/include/gobot/macro.hpp");
    const std::string quiet = source("/src/quiet_second.cpp");
    const std::string goc = source("/docs/goc-reference.md");
    const std::string plan = source("/docs/goc-parity-plan.md");
    if (gui.empty() || hpp.empty() || quest.empty() || shop_card.empty() || use.empty() || view.empty() ||
        skills.empty() || macro.empty() || macro_h.empty() || quiet.empty() || goc.empty() || plan.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }

    // ---- R4: the shop ----
    const std::string loop = body(gui, "void GuiApp::trash_loop() {");
    const std::string shop = slice(gui, "const auto run_shop = [&](bool forced, bool dry) {", "while (!trash_stop_) {");
    check(in_order(shop, { "if (forced || dry) shop_quiet.reset();", "take_hold(\"trash cleaning\")",
                           "input_.release_all();", "seen_before = glancer.glance();",
                           "const DWORD quiet = last_input();", "touched = !trash_stop_ && !lost && last_input() != quiet;",
                           "quiet_spoiled_text(seen_before, glancer.glance())", "passed = true;", "shop_quiet.reset();",
                           "r = cleaner.run();" }),
          "run_shop: a button resets the back-off; the glance before the quiet second and after it is spoiled, under "
          "the hold; a quiet second that passed resets it");
    check(has(shop, "if (forced || dry) {") && count_in(shop, "shop_quiet.next_ms(") == 1 &&
              has(shop, "the next look in %llu s"),
          "and only a timed look moves the back-off, said in the log with its wait");
    check(count_in(gui, "InputGlancer::real()") == 1 && has(loop, "InputGlancer::real()") &&
              count_in(quest, "InputGlancer::real()") == 1 &&
              has(body(quest, "void GuiApp::quest_loop() {"), "InputGlancer::real()"),
          "the desktop glanced at only on the trash and quest workers' threads, never the window's");
    const std::string real = body(quiet, "InputGlancer InputGlancer::real() {");
    check(count_in(quiet, "GetCursorPos(") == 1 && has(real, "GetCursorPos(") && count_in(quiet, "GetAsyncKeyState(") == 1 &&
              has(real, "GetAsyncKeyState(") && count_in(quiet, "GetForegroundWindow(") == 1 &&
              has(real, "GetForegroundWindow(") && !has(quiet, "SendInput") && !has(quiet, "SetCursorPos"),
          "quiet_second.cpp: the pointer, the buttons and the window in front read only in InputGlancer::real(), "
          "the seam a test replaces; nothing sent");

    // ---- R4: the quest ----
    const std::string round = slice(quest, "const auto one_round = [&](const QuestCall& call) -> QuestResult {",
                                    "QuestResult r = round.run();");
    check(in_order(round, { "if (!take_hold(kQuestHold)) {", "input_.release_all();",
                            "const InputGlance seen_before = glancer.glance();", "const DWORD quiet = last_input();",
                            "if (last_input() != quiet) {", "not_now.touched = true;",
                            "quiet_spoiled_text(seen_before, glancer.glance())",
                            "return held_not_now(\"bạn đang dùng chuột hoặc bàn phím\");", "not_now.quiet = true;" }) &&
              in_order(quest, { "QuestResult r = round.run();", "r.quiet = true;" }),
          "simple_quest.cpp: one_round marks a spoiled quiet second as touched and says what spoiled it; one that "
          "passed as quiet, and the round run after it too, however it ended");

    // ---- R5: the clocks kept ----
    // 2026.11.09: with no clock of the last worker's, the shop's first look is
    // what is left of the plan time saved in shop_clock.txt, clamped to 45 s
    // and one interval (bag::shop_first_wait_s) -- never 45 s whatever was
    // saved, which bought a plan again at every BẮT ĐẦU.
    check(has(gui, "constexpr std::uint64_t kCharmFirstMs = 8'000;") &&
              has(gui, "constexpr std::uint64_t kShopFirstMs = 45'000;") &&
              in_order(loop, { "std::lock_guard<std::mutex> lock(trash_clocks_mutex_);", "kept = trash_clocks_;",
                               "resumed_clock(kept.charm_check_ms, now_ms(), kCharmFirstMs)",
                               "CharmClock shop_clock(shop_clock_path());", "shop_clock.load();",
                               "bag::shop_first_wait_s(",
                               "kept.shop_due_ms != 0 ? resumed_clock(kept.shop_due_ms, now_ms(), kShopFirstMs) : "
                               "shop_first_due();",
                               "owed.hp = kept.owed_hp;", "std::uint64_t plan_due_ms = kept.plan_due_ms;",
                               "shop_quiet.set_level(kept.shop_quiet_level);" }) &&
              !has(loop, "now_ms() + 8'000;") && !has(loop, "now_ms() + 45'000;") &&
              !has(loop, "now_ms() + kShopFirstMs;"),
          "trash_loop: the charms' and the shop's clocks resumed from the last worker's, never sooner than 8 s and "
          "45 s; with none kept, the shop waits for the plan time saved, clamped to 45 s and one interval");
    const std::size_t last_loop = loop.rfind("while (!trash_stop_) {");
    const std::size_t kept_at = loop.rfind("trash_clocks_.shop_due_ms = shop_due_ms;");
    const std::size_t ends = loop.rfind("trash_running_ = false;");
    check(last_loop != std::string::npos && kept_at != std::string::npos && ends != std::string::npos &&
              last_loop < kept_at && kept_at < ends && has(loop, "trash_clocks_.charm_check_ms = charm_check_ms;") &&
              has(loop, "trash_clocks_.shop_quiet_level = shop_quiet.level();") &&
              in_order(loop.substr(kept_at == std::string::npos ? 0 : kept_at),
                       { "trash_clocks_.owed_hp = owed.hp;", "trash_clocks_.plan_due_ms = plan_due_ms;",
                         "trash_clocks_.shop_troubles = owed.troubles;", "shop_look_at_s_ = 0;",
                         "shop_owed_ = owed.hp + owed.mp;", "trash_running_ = false;" }),
          "and kept for the next as the worker ends, whichever way its loop is left -- the shop's plan with them, "
          "and the card told no worker buys it now");
    check(in_order(loop, { "if (shop_ticked_.load() != shop_ticks) {", "owed = bag::ShopOwed{};",
                           "shop_due_ms = shop_first_due();", "shop_quiet.reset();",
                           "if (charm_ticked_.load() != charm_ticks) {", "charm_check_ms = now_ms() + kCharmFirstMs;" }) &&
              count_in(loop, "shop_due_ms = shop_first_due();") == 2 && !has(gui, "shop_blocked") &&
              has(loop, "if (kept.charm_ticks_seen != charm_ticks) kept.charm_check_ms = 0;") &&
              in_order(loop, { "if (kept.shop_ticks_seen != shop_ticks) {", "kept.owed_hp = kept.owed_mp = 0;",
                               "kept.plan_due_ms = 0;" }) &&
              in_order(flat(shop_card), { "if (ImGui::Checkbox(\"Tự mua bình\", &simple_clean_.buy_potions)) {",
                                          "if (simple_clean_.buy_potions) { ++shop_ticked_; shop_owed_ = 0; }",
                                          "if (simple_clean_.buy_potions && simple_running()) start_trash();" }) &&
              count_in(shop_card, "++shop_ticked_;") == 1 &&
              has(use, "if (simple_clean_.use_charms) ++charm_ticked_;") &&
              has(hpp, "std::atomic<unsigned>    charm_ticked_{0};"),
          "\"Dùng bùa\" and \"Tự mua bình\" ticked on again: that job's clock, and the shop's back-off, afresh -- the "
          "shop owing nothing, its first look as at a start (the plan time saved, not 45 s); the worker running or "
          "the next; the window's thread moves an atomic only; no shop_blocked anywhere");
    const std::string shop_job = slice(gui, "const auto run_shop = [&](bool forced, bool dry) {", "while (!trash_stop_) {");
    const std::string charm_job = slice(gui, "const auto run_charms = [&](bool forced) {", "// ---- the potion shop ----");
    check(in_order(shop_job, { "if (dry) {", "shop_due_ms = due_before;", "const bool next_part = owed.hp + owed.mp > 0;",
                               "if (!next_part) shop_due_ms = plan_due_ms;",
                               "else if (trash_stop_) shop_due_ms = due_before;",
                               "else if (r.board_up) shop_due_ms = now_ms() + BagCleaner::kBoardWaitMs;",
                               "else if (end == bag::ShopEnd::Paused) shop_due_ms = now_ms() + "
                               "BagCleaner::kShopPartGapMs;",
                               "else if (end == bag::ShopEnd::Trouble) shop_due_ms = minutes(10);",
                               "else shop_due_ms = now_ms() + BagCleaner::kShopPartGapMs;" }) &&
              has(charm_job, "if (!forced && !trash_stop_ && ((!r.ran && (!only_message || !r.trouble.empty())) ||") &&
              has(charm_job, "if (!forced && r.tab_unsure) charm_check_ms = now_ms() + kCharmTabRetryMs;"),
          "a shop run cut by DỪNG/F10 owing the rest leaves its clock as it was (the next worker resumes from 45 s), a "
          "charm run's too (8 s) -- but a plan bought in full waits for the next plan whatever ended its round");

    // ---- 2026.11.09: the shop's plan ----
    const std::size_t plan_at = shop_job.find("owed = bag::shop_new_plan(settings.buy_hp_slots, settings.buy_mp_slots);");
    const std::size_t clock_at = shop_job.find("shop_clock.set_next(kShopClockId,");
    const std::size_t hold_at = shop_job.find("take_hold(\"trash cleaning\")");
    const std::size_t run_at = shop_job.find("r = cleaner.run();");
    check(clock_at != std::string::npos && plan_at != std::string::npos && hold_at != std::string::npos &&
              run_at != std::string::npos && clock_at < plan_at && plan_at < hold_at && hold_at < run_at &&
              count_in(gui, "shop_clock.set_next(") == 1 &&
              in_order(shop_job, { "if (!dry) {", "owed.hp = std::min(owed.hp, settings.buy_hp_slots);",
                                   "if (forced || now_ms() >= plan_due_ms) {",
                                   "if (!shop_clock.set_next(kShopClockId,", "owed = bag::ShopOwed{};",
                                   "shop_due_ms = forced ? due_before : minutes(5);", "return;",
                                   "owed = bag::shop_new_plan(", "if (owed.hp + owed.mp == 0) {",
                                   "shop_due_ms = plan_due_ms;", "return;", "ScreenCapture shop_capture;" }) &&
              !has(shop_job, "|| owed.hp + owed.mp == 0) {"),
          "run_shop: a new plan's time is written to shop_clock.txt before anything is clicked -- never by a trial "
          "(\"if (!dry)\") -- and one not written buys nothing; the numbers set bound what is owed; nothing owed "
          "before the plan's time waits for it, never a new plan early");
    check(in_order(shop_job, { "return not_now(\"Máu đang thấp, mua bình sau.\", 15'000);",
                               "const bag::ShopOwed owed_before = owed;", "owed = bag::shop_new_plan(",
                               "ScreenCapture shop_capture;", "if (!passed) {", "if (forced) {",
                               "owed = owed_before;", "plan_due_ms = plan_before;" }) &&
              count_in(shop_job, "owed = bag::shop_new_plan(") == 1,
          "run_shop: a plan begins only once the gates let the round run, and a \"Mua ngay\" that does not get to "
          "run leaves what is owed and the plan time as they were");
    check(has(shop_job, "bag::shop_order(owed.hp, owed.mp, forced ? BagCleaner::kShopMaxBuys : "
                        "BagCleaner::kShopBuysPerHold);") &&
              count_in(gui, "BagCleaner::kShopBuysPerHold") ==
                  count_in(gui, "forced ? BagCleaner::kShopMaxBuys : BagCleaner::kShopBuysPerHold") &&
              has(shop_job, "if (!dry) cleaner.set_shop_plan(part_hp, static_cast<int>(part.size()) - part_hp);") &&
              has(shop_job, "cleaner.set_deadline_ms(BagCleaner::shop_deadline_ms(dry ? 2 : static_cast<int>(part.size())));") &&
              !has(gui, "set_deadline_ms(240'000)"),
          "run_shop: kShopBuysPerHold a round only for a timed one, \"Mua ngay\" all of it (kShopMaxBuys); the "
          "deadline 30 s and 6 s a purchase of the round");
    {
        // The window never touches the file: the card's line comes from the
        // worker's atomics.
        const std::string card = body(shop_card, "void GuiApp::simple_shop_card(");
        check(!card.empty() && !has(shop_card, "shop_clock") && !has(shop_card, "CharmClock") &&
                  !has(shop_card, "IniFile") && has(card, "shop_plan_at_s_.load()") &&
                  has(card, "shop_look_at_s_.load()") && has(card, "shop_owed_.load()") &&
                  count_in(gui, "shop_clock_path()") == 3 && count_in(gui, "CharmClock shop_clock(") == 1,
              "simple_shop.cpp: the card reads no clock file -- its line is the worker's atomics; shop_clock.txt is "
              "opened by the trash worker alone");
        const std::string cpp = source("/src/bag.cpp");
        const std::string left_up = body(cpp, "bool shop_left_up(const Frame& f, const TemplateStore& store) {");
        check(!left_up.empty() && !has(left_up, "shop_full_up("),
              "bag.cpp: shop_left_up holds for the shop and its question, never for the full-bag message alone");
    }

    // ---- R7: the notes and the line ----
    const std::string timer = body(macro, "void MacroEngine::timer_loop() {");
    check(in_order(timer, { "clash = macro_clash_line(config_.keys);", "GB_WARN(\"%s\", clash.c_str());",
                            "while (running_.load()) {" }) &&
              !has(body(macro, "void MacroEngine::start() {"), "clash") &&
              !has(body(macro, "void MacroEngine::run_timers("), "clash"),
          "macro.cpp: the clash said once from the timer thread as it starts -- not from start(), on the window's "
          "thread -- and nothing pressed or held by it");
    check(has(view, "simple_note(macro_clash_note_vi(keys), 10000);") &&
              in_order(view, { "// ---- buffs ----", "simple_clash_note();", "// ---- dying ----" }) &&
              in_order(body(skills, "void GuiApp::simple_skills_line() {"), { "simple_clash_note();" }) &&
              has(body(gui, "void GuiApp::draw_macro_panel() {"), "macro_clash_note_vi(macro_config_.keys)"),
          "the simple screen says it under the skills and buffs rows, and as a key box makes it; the full screen "
          "under its table");
    check(has(timer, "if (config_.timers_enabled) clash = macro_clash_line(config_.keys);") &&
              has(body(skills, "void GuiApp::simple_clash_note() {"), "if (!macro_config_.timers_enabled) return;") &&
              has(body(gui, "void GuiApp::draw_macro_panel() {"),
                  "macro_config_.timers_enabled ? macro_clash_note_vi(macro_config_.keys)") &&
              has(view, "if (macro_config_.timers_enabled && k.keycode != 0 && !k.conditional() &&"),
          "\"Repeating keys\" off, no row pressed: the clash said nowhere -- the log, the two screens, the key box");

    // ---- W1: the tick ----
    const std::string popup = body(skills, "void GuiApp::simple_skills_popup() {");
    check(has(skills, "const char* const kCastPacedLabel = \"Ra chiêu theo vòng (chờ game ra chiêu rồi mới bấm chiêu kế)\";") &&
              in_order(popup, { "ImGui::Checkbox(\"Dùng skill tuần tự\", &cfg.sequential_skills);",
                                "changed |= skill_gap_combo(cfg);",
                                "if (ImGui::Checkbox(kCastPacedLabel, &cfg.cast_paced)) simple_changed();",
                                "ImGui::SetItemTooltip(\"%s\", cast_paced_tip_vi());", "if (changed) simple_changed();" }) &&
              has(body(gui, "void GuiApp::draw_macro_panel() {"),
                  "ImGui::Checkbox(\"Ra chiêu theo vòng (chờ game ra chiêu rồi mới bấm chiêu kế)##macro\", "
                  "&macro_config_.cast_paced);"),
          "\"Ra chiêu theo vòng\": a tick beside \"Dùng skill tuần tự\" and \"Tốc độ bấm kỹ năng\", saved as the "
          "window saves the rest; and on the full screen");
    const std::string tip = body(skills, "const char* GuiApp::cast_paced_tip_vi() {");
    const std::string status = body(skills, "std::string GuiApp::cast_paced_status_vi() const {");
    check(has(tip, "gói tin") && has(tip, "2 giây") && has(tip, "tạm bấm theo giờ") && has(tip, "Tự đánh quái tắt") &&
              has(tip, "khoảng chờ ngắn nhất") && has(tip, "Bỏ dấu (mặc định)") &&
              has(status, "MacroEngine::pace_status_vi(st)") && has(status, "!follows && !st.others") &&
              !has(status, "save(") && !has(status, "load(") &&
              !has(status, "exe_relative") && !has(skills, "macro_config_.save("),
          "its hover says what it does, that it needs the packets and falls back after 2 s, the macro alone, and the "
          "interval as the least wait; its status reads no file and writes none");

    // ---- R8: the words ----
    check(!has(skills, "Như GoC") &&
              has(skills, "Chỉ có tác dụng khi Tự đánh quái đang bật và \\\"Chỉ khi đang đánh quái\\\" có dấu") &&
              has(skills, "Chỉ khi Tự đánh quái đang bật: chỉ bấm kỹ năng khi bot đang đánh") &&
              has(macro_h, "GoC's \"Dùng skill tuần tự\". Only while the skills follow the fight") &&
              has(macro_h, "which its client never starts (2026-11-08)"),
          "\"Dùng skill tuần tự\" and \"Chỉ khi đang đánh quái\" say they work only with Tự đánh quái; no \"Như GoC\"");
    check(!has(goc, "**GoC waits for server confirmation before advancing an action.**") &&
              has(goc, "**GoC does not wait for server confirmation before advancing an action**") &&
              !has(plan, "and each press waits for the server's confirmation") &&
              !has(plan, "and waits for confirmation") && !has(plan, "counts only when the server confirms it") &&
              !has(plan, "counts a cast only when the server confirms it"),
          "docs: GoC no longer said to wait for the server's confirmation");
#else
    skip("no source folder\n");
#endif
}

// ------------------------------------------------------------ the notes

/// 2026.11.08's notes against the code: DOC-TRUOC.txt's entry and its live
/// test, README.md's release and macro sections, START-HERE (package.ps1) and
/// macro.ini -- the back-off's steps, the clocks' first waits, the rotation's
/// numbers and its switch off as shipped, the advice the window can take, and
/// every word of the screen and the log they quote, as the code has them.
void test_notes_1108() {
    section("The 2026.11.08 notes against the code: the back-off, the rotation and its switch, the words quoted");
#ifdef GOBOT_SOURCE_DIR
    const std::string doc = flat(source("/DOC-TRUOC.txt"));
    const std::string readme = flat(source("/README.md"));
    const std::string pack = flat(source("/package.ps1"));
    const std::string ini = flat(source("/configs/macro.ini"), true);
    const std::string goc = flat(source("/docs/goc-reference.md"));
    const std::string plan = flat(source("/docs/goc-parity-plan.md"));
    const std::string skills = source("/apps/gobot_gui/simple_skills.cpp");
    const std::string shop = source("/apps/gobot_gui/simple_shop.cpp");
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    const std::string quest = source("/apps/gobot_gui/simple_quest.cpp");
    const std::string vitals = source("/apps/gobot_gui/packets_vitals.cpp");
    const std::string more = source("/apps/gobot_gui/packets_more.cpp");
    const std::string casts = source("/src/cast_match.cpp");
    const std::string pace = source("/src/macro_pace.cpp");
    const std::string macro = source("/src/macro.cpp");
    if (doc.empty() || readme.empty() || pack.empty() || ini.empty() || goc.empty() || plan.empty() ||
        skills.empty() || shop.empty() || gui.empty() || view.empty() || quest.empty() || vitals.empty() ||
        more.empty() || casts.empty() || pace.empty() || macro.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto n = [](auto v) { return std::to_string(v); };
    // 1200 ms as the notes write an interval, "1,2" (and 1000 "1,0"); and as
    // README writes a time, "1.2" (and 2000 "2").
    const auto vi_s = [](std::uint64_t ms) {
        return std::to_string(ms / 1000) + "," + std::to_string(ms % 1000 / 100);
    };
    const auto en_s = [](std::uint64_t ms) {
        std::string s = std::to_string(ms / 1000);
        if (ms % 1000 != 0) s += "." + std::to_string(ms % 1000 / 100);
        return s;
    };
    using E = MacroEngine;

    // ---- where: on top of 05b's entry; its live test step 11 of section 6 ----
    const std::size_t e08 = doc.find("- Mới trong bản 2026.11.08 (");
    const std::size_t e05b = doc.find("- Mới trong bản 2026.11.05b (");
    const std::size_t sec6 = doc.find("6. THỬ TỪNG PHẦN CHO AN TOÀN");
    const std::size_t step10 = doc.find("10. Nhảy lên bục trên và Tự tìm quái trong vùng (bản 2026.11.05b");
    const std::size_t step11 = doc.find("11. Kỹ năng ở chiến trường (bản 2026.11.08, chưa thử trên game thật;");
    const std::size_t sec7 = doc.find("7. KHI GẶP LỖI");
    check(e08 != npos && e05b != npos && e08 < e05b && sec6 != npos && step10 != npos && step11 != npos &&
              sec7 != npos && sec6 < step10 && step10 < step11 && step11 < sec7 &&
              has(doc, "2026.11.08 (kỹ năng ra không mượt, theo điều bạn báo sau lần đánh ở chiến trường tối 08/11; "
                       "chưa thử trên game thật)") &&
              has(doc, "Thử như bước 11 ở mục 6.") && has(readme, "### Skills on the battlefield (2026.11.08)") &&
              has(readme, "Since 2026.11.08 (not yet run on the game).") &&
              has(pack, "Skills that come out unevenly (2026.11.08, not yet tried on the game)."),
          "DOC-TRUOC.txt: the 2026.11.08 entry on top of 05b's, not yet tried on the game, its live test step 11 "
          "of section 6; README and START-HERE (package.ps1) say it too");

    // ---- R4: the back-off's steps, as QuietBackOff has them ----
    const auto& st = QuietBackOff::kStepsMs;
    const bool in_minutes = st[1] % 60'000 == 0 && st[2] % 60'000 == 0 && st[3] % 60'000 == 0;
    const std::string vi_steps = n(st[0] / 1000) + " giây, rồi " + n(st[1] / 60'000) + ", " + n(st[2] / 60'000) +
                                 " và " + n(st[3] / 60'000) + " phút";
    const std::string en_steps = n(st[0] / 1000) + " s, then " + n(st[1] / 60'000) + ", " + n(st[2] / 60'000) +
                                 " and " + n(st[3] / 60'000) + " minutes";
    check(QuietBackOff::kSteps == 4 && in_minutes && has(doc, "lần sau chờ " + vi_steps + " (không lâu hơn") &&
              has(doc, "nhưng không dưới " + n(st[0] / 1000) + " giây") &&
              has(doc, "(Sửa ở bản 2026.11.08: lần theo giờ nay chờ " + vi_steps) &&
              has(readme, "look again after " + en_steps + ", one spoiled look after another (`QuietBackOff::kStepsMs`)") &&
              has(readme, "since 2026.11.08 after " + en_steps) && has(pack, "now wait " + en_steps),
          "the back-off, " + en_steps + ": the 2026.11.08 entry, 2026.11.04's shop item put right, README's shop "
          "paragraph and release section, START-HERE -- as QuietBackOff::kStepsMs has it");

    // Its words: the cap's label, the buttons that reset it, the quest's tab
    // and the log, each as the code says it.
    QuestRig q;
    q.round();
    q.round();
    const std::string tab = q.said.empty() ? std::string() : q.said.back();
    Desk d;
    d.x = 812;
    d.y = 440;
    d.front = "chrome.exe";
    const InputGlancer glancer = d.glancer();
    const InputGlance before = glancer.glance();
    d.x = 845;
    d.y = 452;
    const std::string moved = quiet_spoiled_text(before, glancer.glance());
    const std::string shop_line = "Shop: not now -- someone is using the mouse or keyboard (held 1012 ms): " + moved +
                                  "; the next look in " + n(st[1] / 1000) + " s";
    check(tab == "Chờ: bạn đang dùng chuột hoặc bàn phím; thử lại sau " + n(st[1] / 60'000) + " phút." &&
              has(doc, "\"" + tab + "\"") && has(readme, "*" + tab + "*") &&
              // The label is "mua mỗi" since 2026.11.09 (a plan is bought, not a
              // bag looked at); the 2026.11.08 entry quotes it as it was then.
              has(shop, "label(\"mua mỗi\");") && !has(shop, "label(\"kiểm tra mỗi\");") &&
              has(doc, "(không lâu hơn \"kiểm tra mỗi\" của Tự mua bình") &&
              has(shop, "ImGui::Button(\"Mua ngay\")") && has(shop, "ImGui::Button(\"Thử mua (không mua gì)\")") &&
              has(quest, "ImGui::Button(\"Làm ngay 1 lần\")") &&
              has(doc, "khi bấm \"Mua ngay\", \"Thử mua\" hay \"Làm ngay 1 lần\", khi đánh dấu lại ô đó, và (với nhiệm "
                       "vụ) mỗi lần BẮT ĐẦU") &&
              has(gui, "\"Shop: not now -- someone is using the mouse or keyboard (held %llu ms): %s; \"") &&
              has(gui, "\"the next look in %llu s\"") && has(doc, "\"" + shop_line + "\"") &&
              has(readme, "`" + shop_line + "`") && has(quest, "\"Quest: the quiet second was not quiet -- %s\"") &&
              has(doc, "\"Quest: the quiet second was not quiet -- ...\"") &&
              has(readme, "`Quest: the quiet second was not quiet -- …`") &&
              has(doc, "\"the pointer still at ...\" là chuột không dời"),
          "the back-off's words: the interval's label (\"mua mỗi\", \"kiểm tra mỗi\" in 2026.11.08's entry), the "
          "buttons, the quest's tab (\"" + tab + "\"), the shop's and the quest's log lines -- as the GUI and "
          "quiet_spoiled_text say them");

    // ---- R5: the clocks' first waits, gui_app.cpp's own ----
    const std::uint64_t charm_first = source_ms(gui, "kCharmFirstMs"), shop_first = source_ms(gui, "kShopFirstMs");
    check(charm_first != 0 && shop_first != 0 && charm_first % 1000 == 0 && shop_first % 1000 == 0 &&
              has(doc, "GoBot xem bùa sau " + n(charm_first / 1000) + " giây và xem cửa hàng sau " +
                           n(shop_first / 1000) + " giây") &&
              has(doc, "vẫn không sớm hơn " + n(charm_first / 1000) + " giây (bùa) hay " + n(shop_first / 1000) +
                           " giây (cửa hàng) sau BẮT ĐẦU") &&
              has(readme, "every BẮT ĐẦU used to look for a charm due " + n(charm_first / 1000) + " s on and at the "
                          "shop " + n(shop_first / 1000) + " s on"),
          "the charms' and the shop's first waits after a BẮT ĐẦU, " + n(charm_first) + " and " + n(shop_first) +
              " ms -- as kCharmFirstMs and kShopFirstMs have them");

    // ---- W1: the switch, off as shipped, and where it is ----
    const std::string label = literal_after(skills, "const char* const kCastPacedLabel = \"");
    MacroConfig built;
    built.apply_defaults();
    MacroConfig ships;
    const bool shipped = ships.load(std::string(GOBOT_SOURCE_DIR) + "/configs/macro.ini");
    check(!label.empty() && !MacroConfig{}.cast_paced && !built.cast_paced && shipped && !ships.cast_paced &&
              has(ini, "cast_paced=false") &&
              has(ini, "Off, and in a file without it: every key on its own timer, as always.") &&
              has(ini, "the tick \"" + label + "\" in the \"Kỹ năng\" window (Chi tiết… on the \"Tự dùng kỹ năng\" "
                       "row), and in the full screen's Timed keys.") &&
              has(ini, "(VI) Bấm kỹ năng theo nhịp chiêu ra: lần lượt từng chiêu theo thứ tự phím, chỉ bấm chiêu sau "
                       "khi game đã ra chiêu trước (xem qua gói tin). Mặc định tắt.") &&
              has(doc, "Ô mới \"" + label + "\" (thử nghiệm, mặc định KHÔNG có dấu)") &&
              has(doc, "macro.ini không có dòng cast_paced thì coi như ô này không có dấu") &&
              has(readme, "*" + label + "*") &&
              has(readme, "Off as shipped, and a `macro.ini` without `cast_paced` reads false") &&
              has(pack, "OFF as shipped, cast_paced in configs\\macro.ini"),
          "\"" + label + "\": off in code, in the shipped macro.ini (named by its tick) and in one without it, as "
          "every note says");
    check(has(gui, "draw_feature(\"Timed keys\", &macro_on,") &&
              has(gui, "ImGui::Checkbox(\"" + label + "##macro\", &macro_config_.cast_paced);") &&
              has(doc, "sau \"Dùng skill tuần tự\" và \"Tốc độ bấm kỹ năng\". Màn hình Nâng cao, phần \"Timed keys\", có "
                       "ô y như vậy (ở đó phải bấm Apply now hay Save macro)") &&
              has(readme, "in the full screen's **Timed keys** panel, which hands it over on Apply now or Save macro") &&
              has(doc, "Cách bật: thẻ Đánh quái, dòng Tự dùng kỹ năng, nút \"Chi tiết…\", đánh dấu ô đó"),
          "where the tick is, and how it is turned on, as the two screens have it");

    // Its status under the tick, as pace_status_vi words it.
    E::PaceStatus ps;
    ps.mode = E::PaceStatus::Mode::Paced;
    ps.lock_ms = 680;
    ps.rounds = 12;
    const std::string paced = E::pace_status_vi(ps);
    ps.mode = E::PaceStatus::Mode::Fallback;
    ps.last_cast_ms = 0;
    const std::string none_yet = E::pace_status_vi(ps);
    check(has(doc, "\"" + paced + "\"") && has(doc, "\"" + none_yet + "\"") && has(readme, "*" + paced + "*"),
          "the status the notes quote: \"" + paced + "\", \"" + none_yet + "\"");

    // ---- W1: its numbers, macro.hpp's ----
    check(has(doc, "bắt đầu " + n(E::kPaceLockStartMs) + " ms, rồi tự chỉnh trong khoảng " + n(E::kPaceLockMinMs) +
                       "–" + n(E::kPaceLockMaxMs) + " ms") &&
              has(doc, "Phím bấm mà sau " + n(E::kPaceRepressMs) + " ms chưa thấy chiêu ra") &&
              has(doc, "vẫn không ra trong " + n(E::kPaceConfirmMs) + " ms thì vòng đó bỏ qua phím đó") &&
              has(doc, "Không thấy gói tin ra chiêu nào trong " + n(E::kPaceStaleMs / 1000) +
                           " giây trong khi vẫn bấm phím") &&
              E::kPaceStaleMs % 1000 == 0 &&
              has(readme, "`kPaceLockStartMs` (" + n(E::kPaceLockStartMs) + ")") &&
              has(readme, "`kPaceLockMinMs`..`kPaceLockMaxMs` (" + n(E::kPaceLockMinMs) + ".." + n(E::kPaceLockMaxMs) +
                              ")") &&
              has(readme, "`kPaceLockUpMs` (" + n(E::kPaceLockUpMs) + ")") &&
              has(readme, "`kPaceLockDownMs` (" + n(E::kPaceLockDownMs) + ")") &&
              has(readme, "`kPaceLockDownAfter` (" + n(E::kPaceLockDownAfter) + ")") &&
              has(readme, "`kPaceAtLockMs` (" + n(E::kPaceAtLockMs) + ")") &&
              has(readme, "`kPaceRepressMs` (" + n(E::kPaceRepressMs) + " ms)") &&
              has(readme, "`kPaceConfirmMs` (" + n(E::kPaceConfirmMs) + " ms)") &&
              has(readme, "`kPaceFloorTakenMs` (" + en_s(E::kPaceFloorTakenMs) + " s)") &&
              has(readme, "`kPaceFloorUses` (" + n(E::kPaceFloorUses) + ")") &&
              has(readme, "`kPaceSeenMs` (" + en_s(E::kPaceSeenMs) + " s)") &&
              has(readme, "`kPaceDropsToRaise` (" + n(E::kPaceDropsToRaise) + ") of the last `kPaceDropWindow` (" +
                              n(E::kPaceDropWindow) + ") first presses") &&
              has(readme, "`CastMatch::kLoneNearMs` (" + en_s(CastMatch::kLoneNearMs) + " s)") &&
              has(readme, "`kPaceStaleMs` (" + en_s(E::kPaceStaleMs) + " s)") &&
              has(readme, "`kCastRing` (" + n(E::kCastRing) + ")") &&
              has(ini, "With no cast of yours for " + en_s(E::kPaceStaleMs) + " s while keys go out") &&
              has(pack, "goes back to the timers after " + en_s(E::kPaceStaleMs) + " s with no cast seen"),
          "the rotation's lock " + n(E::kPaceLockStartMs) + " ms within " + n(E::kPaceLockMinMs) + ".." +
              n(E::kPaceLockMaxMs) + ", the re-press at " + n(E::kPaceRepressMs) + " ms, the skip at " +
              n(E::kPaceConfirmMs) + " ms, the fallback after " + n(E::kPaceStaleMs) + " ms -- as macro.hpp has them");

    // Its log lines, as macro_pace.cpp writes them.
    check(has(pace, "GB_INFO(\"Paced: on -- the skills one after another") && has(pace, "GB_INFO(\"Paced: off -- %s\"") &&
              has(pace, "\"Paced: no cast of yours for %s while %llu key(s) went out -- the skills on their own timers \"") &&
              has(pace, "\"until your next cast\"") &&
              has(pace, "\"Paced: your cast came after %s on the timers -- the skills one after another again\"") &&
              has(pace, "\"Paced: %llu rounds; %llu casts confirmed (%llu on a re-press); %llu re-presses; skipped "
                        "(cooldown): \"") &&
              has(pace, "\"%s; fallback %s; lock_est %u ms; other casts %llu; now %s\"") &&
              has(pace, "case Mode::Paced:    return \"paced\";") &&
              has(pace, "part += \" (\" + std::to_string(k.skipped_learned) + \" learned\";") &&
              has(doc, "\"Paced: on -- ...\", \"Paced: no cast of yours for ... -- the skills on their own timers until "
                       "your next cast\", \"Paced: your cast came after ... on the timers -- the skills one after "
                       "another again\", \"Paced: off -- ...\"") &&
              has(doc, "\"Paced: N rounds; N casts confirmed (N on a re-press); N re-presses; skipped (cooldown): S 5 (4 "
                       "learned, floor 6.0 s); fallback 0.0 s; lock_est 680 ms; other casts 0; now paced\"") &&
              has(readme, "`Paced: N rounds; N casts confirmed (N on a re-press); N re-presses; skipped (cooldown): S 5 "
                          "(4 learned, floor 6.0 s); fallback 0.0 s; lock_est 680 ms; other casts 0; now paced`"),
          "the rotation's log lines -- on, off, falling back, resuming and the minute's -- as macro_pace.cpp writes "
          "them");

    // ---- R1, R2, R3, R6, R9: the new log lines, as the code writes them ----
    check(has(casts, "out << \"Cast gaps (packets): \";") && has(casts, "<< \" -- p50 \"") && has(casts, "<< \", p90 \"") &&
              has(casts, "<< \", longest \"") && has(casts, "<< \" ending at \"") &&
              has(casts, "<< \"; over 1.0 s \" << over(1000) << \", over 1.5 s \" << over(1500);") &&
              has(casts, "out << \"; newest press to cast \";") && has(casts, "out << \"p10 \"") &&
              has(casts, "<< \" ms, p50 \"") && has(casts, "<< \" ms, p90 \"") &&
              has(casts, "<< \" with no press of GoBot's in the \" << CastMatch::kPairMs << \" ms before\";") &&
              has(casts, "out << \"; by skill\";") && has(casts, "<< \", shortest re-cast \"") &&
              has(more, "cast_gaps_line(casts, &cast_clock_text)") &&
              has(doc, "ngay sau dòng \"Casts (packets)\", một dòng \"Cast gaps (packets): N gaps -- p50 ..., p90 ..., "
                       "longest ... ending at ...; over 1.0 s N, over 1.5 s N; newest press to cast p10 ... ms, p50 ... "
                       "ms, p90 ... ms (N casts); by skill ... N casts, shortest re-cast ... s / ...\"") &&
              has(readme, "N with no press of GoBot's in the " + n(CastMatch::kPairMs) + " ms before") &&
              has(readme, "`CastMatch::kPairMs` (" + n(CastMatch::kPairMs) + " ms)"),
          "R1: \"Cast gaps (packets): ...\" as cast_gaps_line writes it, and the press's window, CastMatch::kPairMs");
    check(has(casts, "out << \"Cast (packets) with no GoBot key at \" << time_text(clock, c.at_ms) << \": skill \"") &&
              has(casts, "<< \", kind \"") && has(casts, "<< \"; GoBot's last press \";") &&
              has(casts, "<< \" before it (\" << key_text(c.before_vk) << \")\";") &&
              has(casts, "out << \", the first after it \";") &&
              has(casts, "<< \" after (\" << key_text(c.after_vk) << \")\";") &&
              has(casts, "(c.held ? \"; the keys held for a job then\" : \"; no hold then\")") &&
              has(more, "\" more this minute, not described\"") &&
              has(doc, "tối đa " + n(CastMatch::kLoneSaid) + " dòng mỗi phút \"Cast (packets) with no GoBot key at ...: "
                       "skill ..., kind ...; GoBot's last press ... before it (key Z), the first after it ... after (key "
                       "X); no hold then\" (hay \"; the keys held for a job then\"), rồi \"Cast (packets) with no GoBot "
                       "key: N more this minute, not described\"") &&
              has(readme, "at most `CastMatch::kLoneSaid` (" + n(CastMatch::kLoneSaid) + ") a minute, the rest counted") &&
              has(more, "\" s either side and no hold (by hand or another tool), not described\"") &&
              has(doc, "\"Cast (packets) with no GoBot key: N with no GoBot press within " +
                           n(CastMatch::kLoneNearMs / 1000) +
                           " s either side and no hold (by hand or another tool), not described\"") &&
              has(readme, "`Cast (packets) with no GoBot key: N with no GoBot press within " +
                              n(CastMatch::kLoneNearMs / 1000) +
                              " s either side and no hold (by hand or another tool), not described`") &&
              has(casts, "<< \" across a stop and start or a capture restart, left out\";") &&
              has(doc, "\"; N gaps across a stop and start or a capture restart, left out\"") &&
              has(readme, "`; N gaps across a stop and start or a capture restart, left out`"),
          "R2: a cast no GoBot key made, " + n(CastMatch::kLoneSaid) + " a minute at most, as lone_cast_line writes it; "
          "one by hand only counted; R1's gaps across a stop and start left out");
    // The line as the GUI makes it: both configs through MacroConfig::store,
    // as simple_settings_ini does -- a key row's code is in [Keys].
    MacroConfig was;
    was.apply_defaults();
    was.keys[7].keycode = 'H';
    MacroConfig now = was;
    now.keys[7].keycode = 'F';
    IniFile was_ini, now_ini;
    was.store(was_ini);
    now.store(now_ini);
    const std::string change = settings_changes(was_ini, now_ini);
    check(change == "Keys.key_7_keycode 72 (H) -> 70 (F)" &&
              has(view, "log_later({ \"Simple screen: start asked by \" + simple_run_from(from) });") &&
              has(view, "log_later({ \"Simple screen: stop asked by \" + simple_run_from(from) });") &&
              has(view, "return \"the big button\";") &&
              has(view, "return \"the toggle key \" + InputManager::key_name(macro_config_.toggle_key);") &&
              InputManager::key_name(MacroConfig{}.toggle_key) == "F10" &&
              has(view, "log_later({ \"Settings changed during the run: \" + changes });") &&
              has(view, "log_later({ \"Settings changed since the last run: \" + changes });") &&
              has(doc, "lần BẮT ĐẦU sau ghi \"Settings changed since the last run: ...\"") &&
              has(readme, "`Settings changed since the last run: Keys.key_4_keycode 65 (A) -> 51 (3)`") &&
              has(macro, "\"%llu range; refused: game not in front %llu\"") &&
              has(doc, "\"Simple screen: start asked by the big button\" (hay \"the toggle key F10\", hay \"stop asked by "
                       "...\")") &&
              has(doc, "\"Settings changed during the run: " + change + "\"") &&
              has(doc, "Dòng \"Skills:\" mỗi phút thêm \"; refused: game not in front N\"") &&
              has(readme, "`Simple screen: start asked by the toggle key F10`") &&
              has(readme, "`Settings changed during the run: " + change + "`") &&
              has(readme, "`; refused: game not in front N`"),
          "R6: who started or stopped the run, the settings changed during it (\"" + change + "\"), the skills refused "
          "-- as simple_view.cpp, settings_changes and the Skills line say them");
    check(macro_buff_keycodes(owner_rows(true)) == std::vector<std::uint32_t>{ 'A', 'S' } &&
              macro_buff_keycodes(owner_rows(false)).empty() &&
              has(casts, "maybe not cast") && has(casts, "out << \"Buff (packets): \" << key_text(b.vk) << \" -- \";") &&
              has(doc, "Không còn \"(buff)\" và \"Buff (packets): key A -- ... maybe not cast\" từ dòng buff đã tắt") &&
              has(readme, "only for keys that a buff row switched on holds (`macro_buff_keycodes`, `macro_buff_on`)"),
          "R3: a buff key only on a buff row switched on -- the owner's rows of 22:15 make none -- as the notes say");
    check(figure_lost_leading_one(4798, 14798) && !figure_lost_leading_one(14798, 14798) &&
              has(vitals, "\"mp_caption_%Y%m%d_%H%M%S.png\"") &&
              has(vitals, "\"Vitals: MP read %d where the packets say %u -- its leading 1 lost (the read at %s); kept the "
                          "MP \"") &&
              has(vitals, "\"bar and its caption, of the frame read at %s, as vitals_debug\\\\%s\"") &&
              has(doc, "\"Vitals: MP read 4798 where the packets say 14798 -- its leading 1 lost (the read at ...); kept "
                       "the MP bar and its caption, of the frame read at ..., as vitals_debug\\mp_caption_....png\"") &&
              has(readme, "`vitals_debug\\mp_caption_YYYYMMDD_HHMMSS.png`") &&
              has(readme, "`Vitals: MP read 4798 where the packets say 14798 -- its leading 1 lost …`"),
          "R9: 4798 for 14798 a lost leading 1, and the MP caption's picture and its line as packets_vitals.cpp names "
          "and writes them");

    // ---- R7: a key on a buff row and a skill row, the words quoted ----
    const std::string note = macro_clash_note_vi(owner_rows(true));
    const std::string clash = macro_clash_line(owner_rows(true));
    const std::size_t dash = clash.find(" -- ");
    check(!note.empty() && dash != npos && has(doc, "ví dụ \"" + note + "\"") &&
              has(doc, "\"" + clash.substr(0, dash + 4) + "...\" mỗi lần macro bật") && has(readme, "`" + clash + "`"),
          "R7: the window's note and the log's line, as macro_clash_note_vi and macro_clash_line word them");

    // ---- R8: "Dùng skill tuần tự" with Tự đánh quái only; GoC presses blind ----
    check(has(doc, "\"Dùng skill tuần tự\" không làm gì trong lần đó: ô này, và \"Chỉ khi đang đánh quái\", chỉ có tác "
                   "dụng khi Tự đánh quái đang chạy") &&
              has(readme, "*Chỉ khi đang đánh quái*, and with it *Dùng skill tuần tự*, the ranges and the MP floor, act "
                          "only while **Tự đánh quái** runs") &&
              has(pack, "it counts only with Chi khi dang danh quai ticked and Tu danh quai on: with the macro alone "
                        "every skill keeps its own timer.") &&
              !has(pack, "ticked as shipped, as in GoC") && !has(skills, "Như GoC") &&
              has(goc, "`MAX_SKILL_RETRIES` (never used)") &&
              has(goc, "but the client never starts this thread (corrected 2026-11-08)") &&
              has(plan, "no press waits for the server's confirmation, whose flags nothing reads (corrected 2026-11-08)") &&
              has(plan, "the confirm flags are written and never read, MAX_SKILL_RETRIES is never used, and AttackThread "
                        "is never started, so GoC presses blind") &&
              has(readme, "`MAX_SKILL_RETRIES` is never used and `AttackThread` is never started -- GoC presses blind "
                          "too"),
          "R8: \"Dùng skill tuần tự\" with Tự đánh quái alone, in every note; GoC's confirmations never read, its "
          "retries unused, its attack thread never started -- docs/goc-reference.md and docs/goc-parity-plan.md");

    // ---- the advice the notes give: what the window takes, as it is ----
    const MacroConfig def;
    const bool gap_150 = std::find(std::begin(MacroConfig::kSkillGapSteps), std::end(MacroConfig::kSkillGapSteps),
                                   150u) != std::end(MacroConfig::kSkillGapSteps);
    check(clamp_skill_ms(1200) == 1200 && clamp_skill_ms(700) == 700 && clamp_skill_ms(1400) == 1400 &&
              clamp_skill_ms(1000) == 1000 && def.skill_gap_ms == 50 && gap_150 &&
              has(skills, "ImGui::TableSetupColumn(\"Hồi chiêu (giây)\");") &&
              has(skills, "label(\"Tốc độ bấm kỹ năng\");") &&
              has(doc, "đặt cột \"Hồi chiêu (giây)\" của cả 8 phím kỹ năng (cửa sổ Kỹ năng, nút \"Chi tiết…\") là " +
                           vi_s(1200)) &&
              has(doc, "Z " + vi_s(700) + " và bảy phím kia " + vi_s(1400)) &&
              has(doc, "chỉ đổi Z từ " + vi_s(500) + " lên " + vi_s(1000)) &&
              has(doc, "Để \"Tốc độ bấm kỹ năng\" ở " + n(def.skill_gap_ms) + " ms: số lớn hơn làm tệ hơn (150 ms:") &&
              has(readme, "1.2 s each, or for Q1 (b) Z 0.7 s and the rest 1.4 s") &&
              has(readme, "*Tốc độ bấm kỹ năng* left at " + n(def.skill_gap_ms) + " ms") &&
              has(pack, "(" + en_s(1200) + " s each, in Chi tiet...)"),
          "the intervals advised (1.2 s; 0.7 and 1.4 s; Z 1.0 s) go into \"Hồi chiêu (giây)\" as they are, and "
          "\"Tốc độ bấm kỹ năng\" ships at 50 ms with 150 among its steps");

    // ---- the owner's ten questions, each with its (a) ----
    check(has(doc, "Các câu hỏi dưới đây chưa có câu trả lời, nên bản này làm theo cách (a) ở chỗ cần; báo lại nếu "
                   "muốn khác:") &&
              has(doc, "1. Thứ tự chiêu, khi mọi chiêu đều sẵn: (a) một vòng cố định theo thứ tự phím, mỗi chiêu một "
                       "lần") &&
              has(doc, "Ô \"Ra chiêu theo vòng\" làm theo (a).") &&
              has(doc, "2. Phím nào là đánh thường của game (Đánh thường / Tấn công)? (a) Space") &&
              has(doc, "3. Hồi chiêu trong game: có chiêu nào hồi lâu hơn khoảng 1–2 giây không? (a) để GoBot tự đo") &&
              has(doc, "4. Lúc \"khựng\" trông thế nào, và lúc nào? (a) nhân vật đứng yên") &&
              has(doc, "5. Tự mua bình, dọn túi khi đang đánh: (a) để tắt khi đánh chiến trường") &&
              has(doc, "6. Nguồn \"bạn đang dùng chuột hoặc bàn phím\" lúc 22:17–22:20: (a) chạy chương trình theo dõi") &&
              has(doc, "7. Bình MP sau khi sửa cách đọc MP: (a) nâng mốc MP từ 59% lên 75%") &&
              has(doc, "Bản này chưa sửa cách đọc MP và không đổi mốc nào: chờ câu trả lời.") &&
              has(doc, "8. Bình thú cưng (Q) ở mức nào? (a) 70%") &&
              has(doc, "9. Một phím vừa là buff vừa là kỹ năng: (a) GoBot nhắc (bản này làm vậy)") &&
              has(doc, "10. Lúc 22:12–22:16, 22:20:28–43 và 22:39–22:42") &&
              has(doc, "đừng gõ mật khẩu khi nó đang chạy"),
          "the ten questions with their (a), \"báo lại nếu muốn khác\"; the input watcher's warning about passwords");
#else
    skip("no source folder\n");
#endif
}

/// 2026.11.09's notes against the code: DOC-TRUOC.txt's entry and its live
/// test step 12, README.md's release section, START-HERE (package.ps1), the
/// comments of trash.ini and app.ini, docs/goc-comparison.md and
/// docs/protocol-findings.md -- the shop's numbers and the check-in board's,
/// the owner's answers, and the words of the screen and the log they quote, as
/// the code has them.
void test_notes_1109() {
    section("The 2026.11.09 notes against the code: buying by the slot, the check-in board, the words quoted");
#ifdef GOBOT_SOURCE_DIR
    const std::string doc = flat(source("/DOC-TRUOC.txt"));
    const std::string readme = flat(source("/README.md"));
    const std::string pack = flat(source("/package.ps1"));
    const std::string trash_ini = flat(source("/configs/trash.ini"), true);
    const std::string app_ini = flat(source("/configs/app.ini"), true);
    const std::string goc = flat(source("/docs/goc-comparison.md"));
    const std::string proto = flat(source("/docs/protocol-findings.md"));
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string shop = source("/apps/gobot_gui/simple_shop.cpp");
    const std::string popups = source("/apps/gobot_gui/simple_popups.cpp");
    const std::string quest = source("/apps/gobot_gui/simple_quest.cpp");
    const std::string bag_cpp = source("/src/bag.cpp");
    const std::string bag_hpp = source("/include/gobot/bag.hpp");
    const std::string closer = source("/src/popup_closer.cpp");
    const std::string board = source("/src/checkin_board.cpp");
    if (doc.empty() || readme.empty() || pack.empty() || trash_ini.empty() || app_ini.empty() || goc.empty() ||
        proto.empty() || gui.empty() || shop.empty() || popups.empty() || quest.empty() || bag_cpp.empty() ||
        bag_hpp.empty() || closer.empty() || board.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto n = [](auto v) { return std::to_string(v); };
    // 300 ms as the notes write a time, "0,3"; and as README does, "1.2".
    const auto vi_s = [](std::uint64_t ms) {
        return std::to_string(ms / 1000) + "," + std::to_string(ms % 1000 / 100);
    };
    const auto en_s = [](std::uint64_t ms) {
        std::string s = std::to_string(ms / 1000);
        if (ms % 1000 != 0) s += "." + std::to_string(ms % 1000 / 100);
        return s;
    };
    // A private constant of BagCleaner's, as bag.hpp writes it; -1 without it.
    const auto hpp_int = [&](const std::string& name) {
        const std::string head = "static constexpr int " + name + " = ";
        const std::size_t at = bag_hpp.find(head);
        if (at == npos) return -1;
        int v = 0;
        for (std::size_t i = at + head.size(); i < bag_hpp.size() && bag_hpp[i] != ';'; ++i) {
            if (bag_hpp[i] == '\'') continue;
            if (bag_hpp[i] < '0' || bag_hpp[i] > '9') return -1;
            v = v * 10 + (bag_hpp[i] - '0');
        }
        return v;
    };
    using B = BagCleaner;
    using P = PopupCloser;
    using S = CheckinShots;

    // ---- where: on top of 2026.11.08's entry; its live test step 12 ----
    const std::size_t e09 = doc.find("- Mới trong bản 2026.11.09 (");
    const std::size_t e08 = doc.find("- Mới trong bản 2026.11.08 (");
    const std::size_t step11 = doc.find("11. Kỹ năng ở chiến trường (bản 2026.11.08,");
    const std::size_t step12 = doc.find("12. Bảng điểm danh và Tự mua bình (bản 2026.11.09, chưa thử trên game thật):");
    const std::size_t sec7 = doc.find("7. KHI GẶP LỖI");
    check(e09 != npos && e08 != npos && e09 < e08 && step11 != npos && step12 != npos && sec7 != npos &&
              step11 < step12 && step12 < sec7 &&
              has(doc, "2026.11.09 (Tự mua bình không đếm bình nữa, và tự tắt bảng Sự kiện và điểm danh, theo điều "
                       "bạn báo ngày 09/11; chưa thử trên game thật)") &&
              has(doc, "Thử như bước 12 ở mục 6.") &&
              has(readme, "### Buying by the slot, and the check-in board (2026.11.09)") &&
              has(readme, "Since 2026.11.09 (not yet run on the game).") &&
              has(pack, "Buying by the slot and the check-in board (2026.11.09, not yet tried on the game)."),
          "DOC-TRUOC.txt: the 2026.11.09 entry on top of 2026.11.08's, not yet tried on the game, its live test "
          "step 12 of section 6; README and START-HERE (package.ps1) say it too");

    // ---- the owner's two requests, word for word, and the answers ----
    check(has(doc, "\"cần sửa 1 chút phần tự mua bình, không cần phải đếm có bao nhiêu bình nữa, tới giờ sẽ tự mua "
                   "số slot đã chỉ định, khi có thông báo túi đồ đã đầy thì dừng lại và tắt\"") &&
              has(doc, "\"Khi login bị hiển thị bảng daily checkin, cần ấn esc để tắt bảng này. Bảng này sẽ hiện mỗi "
                       "khi login và khi qua 00:00h\"") &&
              has(doc, "Câu trả lời của bạn ngày 09/11, và hai câu chưa hỏi mà bản này làm theo cách (a); báo lại "
                       "nếu muốn khác:") &&
              has(doc, "1. Túi đầy: (a) đóng thông báo và cửa hàng, dừng lượt đó; Tự mua bình vẫn có dấu, tới giờ "
                       "sau mua lại.") &&
              has(doc, "2. Số ô mỗi lần: \"mua theo setting\"") &&
              has(doc, "3. Phím G và F uống Thịt bò khô và Rượu lá tre: (a)") &&
              has(doc, "4. Tắt bảng điểm danh: (c) gộp vào ô \"Tự đóng thông báo\".") &&
              has(doc, "5. (Chưa hỏi) Bot đang dừng mà vào game") && has(doc, "6. (Chưa hỏi) Bảng được nhận ra") &&
              has(readme, "Q1 (a), a full bag ends that time's round and the box stays ticked") &&
              has(readme, "Q4 (c), the board folded into **Tự đóng thông báo**, with no tick of its own") &&
              has(readme, "Q5, the board closed only while the bot runs; Q6, the board told by its title and closed "
                          "by ESC alone."),
          "the owner's two requests quoted as written, Q1 (a), Q2 by the setting, Q3 (a), Q4 (c), and Q5 and Q6 "
          "built as (a) with \"báo lại nếu muốn khác\"");
    // Q4 (c): no tick of its own -- the one to tick is "Tự đóng thông báo",
    // off as shipped, which the notes say first.
    check(!has(app_ini, "close_checkin") && !has(pack, "close_checkin") && has(app_ini, "close_popups=false") &&
              has(doc, "Việc cần làm trước: muốn GoBot tự tắt bảng điểm danh thì đánh dấu ô \"Tự đóng thông báo\" "
                       "(dòng cuối của \"Bot sẽ làm gì\", thẻ Đánh quái) MỘT lần.") &&
              has(pack, "Tu dong thong bao (OFF as shipped; tick it once) now also closes the Su kien va diem danh "
                        "board") &&
              has(popups, "if (ImGui::Checkbox(\"Tự đóng thông báo\", &ui.enabled)) {"),
          "Q4 (c): the board under \"Tự đóng thông báo\", no close_checkin anywhere; the notes say to tick it first");

    // ---- the slots: the card's own numbers, the old buy_*_to read till saved ----
    const int most = CleanSettings::kBuySlotsPerRunMost, three = CleanSettings::kBuySlotsDefault;
    check(has(bag_cpp, "slots(\"buy_hp_slots\", \"buy_hp_to\", \"buy_hp_below\")") &&
              has(bag_cpp, "slots(\"buy_mp_slots\", \"buy_mp_to\", \"buy_mp_below\")") &&
              has(shop, "label(\"mỗi lần mua\");") && has(shop, "slots(\"Thịt bò khô (HP):\", \"hp\"") &&
              has(shop, "slots(\"Rượu lá tre (MP):\", \"mp\"") &&
              has(shop, "n = std::clamp(n, 0, CleanSettings::kBuySlotsPerRunMost);") &&
              has(doc, "\"Thịt bò khô (HP): mỗi lần mua [N] ô\" và \"Rượu lá tre (MP): mỗi lần mua [N] ô\", từ 0 tới " +
                           n(most)) &&
              has(doc, "trash.ini của bản zip đặt " + n(three) + " và " + n(three) + ".") &&
              has(doc, "bản này lấy số \"tới\" làm số ô mua mỗi lần, cho tới khi bạn đổi số đó trên thẻ") &&
              has(readme, "0 to " + n(most) + " each (`CleanSettings::kBuySlotsPerRunMost`), " + n(three) + " and " +
                              n(three) + " as shipped (`kBuySlotsDefault`)") &&
              has(readme, "A file without them reads its old `buy_hp_to` and `buy_mp_to` -- the owner's 10 and 10 -- "
                          "until the card saves the new key") &&
              has(trash_ini, "a slot of the bag each: 0 to " + n(most) + ". 0 leaves that potion alone.") &&
              has(trash_ini, "until that number is changed on the card, which writes the new key; with neither, " +
                                 n(three) + ".") &&
              has(trash_ini, "cho tới khi bạn đổi số đó trên thẻ (GoBot ghi dòng mới); không có cả hai thì là " +
                                 n(three) + ".") &&
              has(pack, "(moi lan mua N o, 0 to " + n(most) + ", " + n(three) + " and " + n(three) + " as shipped;"),
          "the slots, 0 to " + n(most) + " and " + n(three) + " as shipped, a file's old buy_*_to read until the "
          "card saves -- as CleanSettings::load and the card have them");

    // ---- the check-in board over the shop and its quantity box (the review) ----
    const std::string clear = slice(gui, "const auto look_for_clear = [&] {", "const bool was_held = held;");
    const std::string cancel =
        slice(bag_cpp, "bool BagCleaner::shop_cancel(int tries) {", "bool BagCleaner::board_over_question(");
    const std::string over = body(bag_cpp, "bool BagCleaner::board_over_question(const Frame& f) const {");
    const std::string close_shop = slice(bag_cpp, "const auto close_shop = [&] {", "shop_up = !closed;");
    check(has(gui, "#include \"gobot/checkin_board.hpp\"") &&
              has(clear, "if (shop_held && (bag::shop_left_up(f, store) || checkin_board_title(f, store))) return;") &&
              count_in(cancel, "board_over_question(") == 3 &&
              has(cancel, "if (!seen_at(shop_q_) || board_over_question(f)) return false;") &&
              has(cancel, "if (shop_q_.valid() && (!seen_at(shop_q_) || board_over_question(f))) return false;") &&
              has(over, "checkin_board_title(f, store_, &title)") && has(over, "kCheckinBoardFromTitle") &&
              has(over, "board.overlaps(bag::message_box(shop_q_))") &&
              has(bag_cpp, "((shop_q_.valid() || (res.board_up && res.shop_opened)) && checkin_board_title(g, store_))"),
          "a quantity box under the check-in board is never taken for gone (shop_cancel), the run's end holds for it "
          "and for a shop opened under the board, and the trash worker keeps a shop's hold while the board is up");
    check(in_order(close_shop, { "if (!open.found) {", "if (bag::shop_row_up(g, store_)) {",
                                 "if (!late && checkin_board_title(g, store_)) {", "res.board_up = true;",
                                 "if (shop_q_.valid()) {", "break;", "if (message_left()) break;",
                                 "closed = shop_close(open, res);", "break;", "closed = true;",
                                 "if (message_left()) break;", "closed = shop_close(open, res);" }) &&
              count_in(close_shop, "closed = shop_close(open, res);") == 2 &&
              has(bag_cpp, "if (!shop_now(f).found && !checkin_board_title(f, store_)) return true;"),
          "close_shop: a shop hidden by the board gets the shop's one ESC to the board and then Thoát -- never "
          "with the quantity box taken to be up under it, nor over a message GoBot does not know -- and a shop "
          "still under the board after the ESC is not taken for gone");

    // ---- a hold's purchases, the rest a minute on, "Mua ngay" all of them ----
    const int per_hold = B::kShopBuysPerHold, all = B::kShopMaxBuys;
    const int gap_s = B::kShopPartGapMs / 1000, gap_min = B::kShopPartGapMs / 60'000;
    check(B::kShopPartGapMs % 60'000 == 0 && has(doc, "Mỗi lượt theo giờ giữ phím mua tối đa " + n(per_hold) + " lần") &&
              has(doc, "phần còn lại mua tiếp sau " + n(gap_min) + " phút") &&
              has(doc, "Mỗi lượt theo giờ mua tối đa " + n(per_hold) + " lần, phần còn lại sau " + n(gap_min) +
                           " phút") &&
              has(doc, "\"Mua ngay\": mua ngay đủ số ô đã đặt trong MỘT lượt (tối đa " + n(all) + " lần mua)") &&
              has(readme, "makes at most `kShopBuysPerHold` (" + n(per_hold) + ") purchases in one hold, the rest "
                          "`kShopPartGapMs` (" + n(gap_s) + " s) later") &&
              has(readme, "`kShopMaxBuys` (" + n(all) + ") at most") &&
              has(trash_ini, "so a run on the clock makes " + n(per_hold) + " purchases at the most and the rest a "
                             "minute later; \"Mua ngay\" buys them all at once (" + n(all) + " at the most).") &&
              has(pack, n(per_hold) + " purchases a hold and the rest a minute later"),
          "a timed round " + n(per_hold) + " purchases, the rest " + n(gap_s) + " s later, \"Mua ngay\" " + n(all) +
              " at most -- as BagCleaner has them");

    // ---- a round's deadline ----
    const int dl0 = B::shop_deadline_ms(0), per = B::shop_deadline_ms(1) - dl0, cap = B::kShopDeadlineMostMs;
    check(dl0 % 1000 == 0 && per % 1000 == 0 && cap % 1000 == 0 && B::shop_deadline_ms(1000) == cap &&
              has(doc, "Mỗi lượt có hạn " + n(dl0 / 1000) + " giây cộng " + n(per / 1000) + " giây mỗi lần mua (tối "
                       "đa " + n(cap / 1000) + " giây; trước là 240 giây)") &&
              has(doc, "mỗi lượt tối đa " + n(dl0 / 1000) + " giây cộng " + n(per / 1000) + " giây mỗi lần mua, không "
                       "quá " + n(cap / 1000) + " giây") &&
              has(readme, "A round's deadline is " + n(dl0 / 1000) + " s and " + n(per / 1000) + " s a purchase, "
                          "`kShopDeadlineMostMs` (" + n(cap / 1000) + " s) at most, where it was 240 s."),
          "a round's deadline, " + n(dl0) + " ms and " + n(per) + " ms a purchase, " + n(cap) + " ms at most -- as "
          "BagCleaner::shop_deadline_ms has it");

    // ---- the plan's clock on disk ----
    const std::string clock_file = shop_clock_path();
    const std::string tail = "\\shop_clock.txt";
    const std::int64_t now_s = 1'000'000;
    check(clock_file.size() > tail.size() &&
              clock_file.compare(clock_file.size() - tail.size(), tail.size(), tail) == 0 &&
              bag::shop_first_wait_s(0, now_s, 60) == bag::kShopFirstWaitS &&
              bag::shop_first_wait_s(now_s + 600, now_s, 60) == 600 &&
              bag::shop_first_wait_s(now_s + 99'999, now_s, 60) == 3600 &&
              has(doc, "Giờ mua được ghi vào %LOCALAPPDATA%\\GoBot\\shop_clock.txt TRƯỚC khi bấm gì") &&
              has(doc, "(ít nhất " + n(bag::kShopFirstWaitS) + " giây, nhiều nhất một khoảng \"mua mỗi\")") &&
              has(readme, "`%LOCALAPPDATA%\\GoBot\\shop_clock.txt`") &&
              has(readme, "`kShopFirstWaitS` (" + n(bag::kShopFirstWaitS) + " s) at least and one interval at most "
                          "(`bag::shop_first_wait_s`)") &&
              has(trash_ini, "%LOCALAPPDATA%\\GoBot\\shop_clock.txt before anything is clicked") &&
              has(trash_ini, n(bag::kShopFirstWaitS) + " seconds at least, one interval at most") &&
              has(pack, "%LOCALAPPDATA%\\GoBot\\shop_clock.txt, so a restart never buys early"),
          "the plan's time in shop_clock.txt, a restart waiting what is left, " + n(bag::kShopFirstWaitS) +
              " s at least and an interval at most -- as bag::shop_first_wait_s has it");

    // ---- the bag full, a message, trouble ----
    const int full_gap = hpp_int("kShopFullGapMs"), full_late = hpp_int("kShopFullLateMs");
    check(full_gap > 0 && full_late == 1000 && bag::kShopTroublesToDrop == 2 &&
              has(doc, "chỉ coi là đã đóng khi hai lần nhìn cách nhau " + vi_s(static_cast<std::uint64_t>(full_gap)) +
                           " giây đều không còn thấy") &&
              has(readme, "closed only when two looks " + n(full_gap) + " ms apart no longer see it; never an ESC while "
                          "it is up") &&
              has(readme, "for a second after Xác Nhận (`kShopFullLateMs`)") &&
              has(doc, "hai lượt liền có lỗi thì GoBot bỏ phần còn lại") &&
              has(readme, "the second in a row (`kShopTroublesToDrop`) drops what is left of the plan") &&
              has(doc, "Ô \"Tự mua bình\" VẪN CÓ DẤU: tới giờ sau GoBot mua lại.") &&
              has(doc, "giữ ảnh shop_message trong trash_debug") && has(bag_cpp, "keep(f, \"shop_message\");") &&
              has(doc, "giữ ảnh shop_full_stuck") && has(bag_cpp, "keep(g, \"shop_full_stuck\");"),
          "the full message closed when two looks " + n(full_gap) + " ms apart miss it, looked for a second after "
          "Xác Nhận, never ESC; two troubles in a row drop the plan -- as bag.hpp has them");

    // ---- the check-in board: how it is told and closed ----
    check(P::kSteadyMs % 1000 == 0 && P::kHoldMs % 1000 == 0 && P::kTriesWindowMs % 60'000 == 0 && P::kTries == 2 &&
              has(doc, "bảng hiện (lúc vào game, lúc qua 00:00) và đứng yên " + n(P::kSteadyMs / 1000) +
                           " giây thì GoBot bấm ESC một lần") &&
              has(doc, "các phím khác chờ tối đa " + n(P::kHoldMs / 1000) + " giây") &&
              has(doc, "Bấm ESC " + n(P::kTries) + " lần trong " + n(P::kTriesWindowMs / 60'000) +
                           " phút mà bảng vẫn còn") &&
              has(readme, "presses ESC once the board has stayed put " + n(P::kSteadyMs / 1000) + " s") &&
              has(readme, "a hold of at most " + n(P::kHoldMs / 1000) + " s, a fresh look, ESC by " +
                              en_s(P::kEscPressByMs) + " s") &&
              has(readme, "Two tries in " + n(P::kTriesWindowMs / 60'000) + " minutes") &&
              has(readme, "within " + n(kCheckinRelookSlack) + " px of where it was") &&
              kCheckinBoardScore > 0.899f && kCheckinBoardScore < 0.901f && has(readme, "`kCheckinBoardScore` 0.90") &&
              has(readme, "(\"Sự kiện và điểm danh\", " + n(kCheckinBoardFromTitle.w) + "x" +
                              n(kCheckinBoardFromTitle.h) + ", with its *Điểm danh* calendar)") &&
              has(app_ini, "each once it has stayed in one place for " + n(P::kSteadyMs / 1000) + " seconds") &&
              has(app_ini, "bảng Sự kiện và điểm danh (lúc vào game và lúc qua 00:00, bấm ESC sau khi bảng đứng yên " +
                               n(P::kSteadyMs / 1000) + " giây)") &&
              has(app_ini, "(ESC, never its X, and only with nothing else of these up: the notice goes first)") &&
              std::string(popup_kind_name(PopupKind::CheckinBoard)) == "the check-in board",
          "the board ESC'd after " + n(P::kSteadyMs) + " ms still, under a hold of " + n(P::kHoldMs) + " ms, " +
              n(P::kTries) + " tries in " + n(P::kTriesWindowMs) + " ms, re-looked within " + n(kCheckinRelookSlack) +
              " px -- as PopupCloser and checkin_board.hpp have them");

    // ---- the jobs put off while it is up; the pictures kept ----
    check(B::kBoardWaitMs == 60'000 && QuestRun::kBoardMs == static_cast<std::uint64_t>(B::kBoardWaitMs) &&
              has(doc, "không bấm gì, chờ " + n(B::kBoardWaitMs / 60'000) + " phút rồi xem lại (không tính là lỗi)") &&
              has(doc, "\"Đang có bảng Sự kiện và điểm danh: việc túi đồ chờ " + n(B::kBoardWaitMs / 60'000) +
                           " phút. Bật \"Tự đóng thông báo\" (thẻ Đánh quái) để GoBot tự tắt bảng điểm danh.\"") &&
              has(gui, "tell(\"Đang có bảng Sự kiện và điểm danh: việc túi đồ chờ \" + "
                       "std::to_string(BagCleaner::kBoardWaitMs / 60'000) +") &&
              has(readme, "looks again a minute later (`BagCleaner::kBoardWaitMs`, `QuestRun::kBoardMs`)") &&
              has(app_ini, "the bag jobs and the quick quest wait a minute at a time while it is up"),
          "a job that finds the board up waits " + n(B::kBoardWaitMs) + " ms, the quest the same -- the notes "
          "quote the tab's line as the worker says it");
    char midnight[16];
    std::snprintf(midnight, sizeof midnight, "00:00:%02d", S::kMidnightS);
    check(S::kAfterMs % 1000 == 0 &&
              has(doc, "_in15 (" + n(S::kAfterMs / 1000) + " giây sau khi GoBot vào lại game), _start15 (" +
                           n(S::kAfterMs / 1000) + " giây sau BẮT ĐẦU) và _midnight (lần nhìn đầu tiên từ " +
                           midnight + "), giữ " + n(S::kKeep) + " ảnh mới nhất.") &&
              has(readme, "keeps `checkin_<time>_<nn>_<why>.png`, the newest " + n(S::kKeep) + ":") &&
              has(readme, "`in15` (" + n(S::kAfterMs / 1000) + " s after the keeper got back in), `start15` (" +
                              n(S::kAfterMs / 1000) + " s after BẮT ĐẦU) and `midnight` (the first look from " +
                              midnight + ")") &&
              has(app_ini, "-- the newest " + n(S::kKeep) + ".") &&
              has(closer, "const std::string head = probe ? \"probe_\" : \"checkin_\";") &&
              has(popups, "const char* const kPopupDebug = \"popup_debug\";"),
          "the pictures: in15 and start15 " + n(S::kAfterMs) + " ms on, midnight from " + midnight + ", the newest " +
              n(S::kKeep) + " in popup_debug -- as CheckinShots and keep_popup_frame have them");

    // ---- the screen's words the notes quote ----
    const std::string bought = "Đã mua 2 lần " + n(CleanSettings::kBuyQuantity) + ": 1 ô Thịt bò khô, 1 ô Rượu lá tre.";
    check(has(gui, "line = \"Đã mua \" + std::to_string(r.shop_bought) + \" lần \" + "
                   "std::to_string(CleanSettings::kBuyQuantity) +") &&
              has(gui, "\": \" + std::to_string(r.shop_bought_hp) + \" ô Thịt bò khô, \" + "
                       "std::to_string(r.shop_bought_mp) +") &&
              has(gui, "\" ô Rượu lá tre.\";") && has(doc, "thẻ ghi \"" + bought + "\"") &&
              has(gui, "tell(\"Cả hai số ô đều là 0: không có gì để mua.\");") &&
              has(doc, "\"Cả hai số ô đều là 0: không có gì để mua.\"") &&
              has(gui, "std::string(\"Thử mua: game báo túi đồ [Tiêu thụ] đã đầy.\")") &&
              has(doc, "\"Thử mua: game báo túi đồ [Tiêu thụ] đã đầy.\"") &&
              has(closer, "\"Bảng Sự kiện và điểm danh vẫn còn sau 2 lần bấm ESC: GoBot để yên, bạn tắt giúp.\"") &&
              has(doc, "\"Bảng Sự kiện và điểm danh vẫn còn sau 2 lần bấm ESC: GoBot để yên, bạn tắt giúp.\"") &&
              has(popups, "return \"Tự đóng thông báo: đang bật\" + (n > 0 ? \", đã đóng \" + std::to_string(n)") &&
              has(popups, "(boards > 0 ? \" (bảng điểm danh: \" + std::to_string(boards) + \")\"") &&
              has(doc, "\"Tự đóng thông báo: đang bật, đã đóng N (bảng điểm danh: M).\"") &&
              !has(doc, "\"Tự đóng thông báo: đang bật, đã đóng N.\"") &&
              has(doc, "\"Thử xem\" phải nói thấy \"bảng Sự kiện và điểm danh ở x,y\"") &&
              has(popups, "out += \" ở \" + std::to_string(up[i].at.x) + \",\" + std::to_string(up[i].at.y) +") &&
              has(doc, "thấy \"" + std::string(popup_kind_vi(PopupKind::CheckinBoard)) + " ở x,y\""),
          "the card's \"" + bought + "\", the both-0 and trial lines, the board left alone, the line under BẮT ĐẦU "
          "and Thử xem -- as the GUI and the closer word them");

    // ---- the log's words the notes quote ----
    char plan[160], buy[96], done[160];
    std::snprintf(plan, sizeof plan,
                  "Trash: shop plan HP %d + MP %d (owed %d+%d, at most %d a hold%s); clock saved, next plan %s", 10,
                  10, 10, 10, per_hold, "", "23:15");
    std::snprintf(buy, sizeof buy, "Trash: shop purchase %d/%d %s -- bought (%d read back)", 1, 10, "HP",
                  CleanSettings::kBuyQuantity);
    std::snprintf(done, sizeof done, "Trash: shop done -- bought %d+%d, unsure %d, full %s%s; owed %d+%d; %s %s", 5, 5,
                  0, "no", "", 5, 5, "next part", (n(gap_s) + " s on").c_str());
    const std::string troubles =
        "Trash: shop -- " + n(bag::kShopTroublesToDrop) + " troubles in a row: owed dropped until the next plan ";
    check(has(gui, "\"Trash: shop plan HP %d + MP %d (owed %d+%d, at most %d a hold%s); clock saved, next plan %s\"") &&
              has(bag_cpp, "\"Trash: shop purchase %d/%d %s -- bought (%d read back)\"") &&
              has(gui, "\"Trash: shop done -- bought %d+%d, unsure %d, full %s%s; owed %d+%d; %s %s\"") &&
              has(gui, "\" s on\"") && has(gui, "next_part ? \"next part\" : \"next plan\"") &&
              has(gui, "\"Trash: shop -- %d troubles in a row: owed dropped until the next plan %s\"") &&
              has(doc, "\"" + std::string(plan) + "\"") && has(readme, "`" + std::string(plan) + "`") &&
              has(doc, "\"" + std::string(buy) + "\"") && has(readme, "`" + std::string(buy) + "`") &&
              has(doc, "\"" + std::string(done) + "\"") && has(readme, "`" + std::string(done) + "`") &&
              has(doc, "\"" + troubles + "...\"") && has(readme, "`" + troubles + "…`"),
          "the shop's plan, purchase, done and troubles lines -- \"" + std::string(plan) + "\" -- as the worker and "
          "the cleaner write them");
    check(has(bag_cpp, "\"Trash: shop full message at %d,%d closed by %s (seen gone twice, %d click(s))\"") &&
              has(bag_cpp, "by = \"its label\";") && has(doc, "closed by its label (seen gone twice, 1 click(s))") &&
              has(readme, "closed by its label (seen gone twice, 1 click(s))`") &&
              has(bag_cpp, "-- NOT closed after %d click(s), no ESC; \"") &&
              has(bag_cpp, "\"the keys are not held for it\",") &&
              has(doc, "NOT closed after 2 click(s), no ESC; the keys are not held for it") &&
              has(readme, "NOT closed after 2 click(s), no ESC; the keys are not held for it`") &&
              has(bag_cpp, "\"Trash: shop lone message at %d,%d %s (full picture here: whole %.2f at %d,%d, words %.2f) "
                           "-- kept \"") &&
              has(readme, "-- kept shop_message, closing it; nothing more bought this plan`") &&
              has(bag_cpp, "\"shop_message, closing it; nothing more bought this plan\",") &&
              has(gui, "GB_INFO(\"Bag (packets): before the shop %s\"") &&
              has(gui, "GB_INFO(\"Bag (packets): after the shop %s\"") &&
              has(gui, "\"Bag (packets): the game said full with %d Tiêu Dùng slots used\"") &&
              has(doc, "\"Bag (packets): before the shop ...\" và \"Bag (packets): after the shop ...\"") &&
              has(readme, "`… the game said full with N Tiêu Dùng slots used`"),
          "the full message's, the lone message's and the packets' lines, as the cleaner and the worker write them");
    check(has(gui, "\"Trash: the check-in board is up -- %s put off %d s%s\"") &&
              has(gui, "board_put_off(look_only ? \"the look\" : \"the pass\", request != 0);") &&
              has(doc, "\"Trash: the check-in board is up -- the pass put off " + n(B::kBoardWaitMs / 1000) + " s\"") &&
              has(readme, "`Trash: the check-in board is up -- the pass put off " + n(B::kBoardWaitMs / 1000) + " s`") &&
              has(quest, "\"Quest: the check-in board is up at %d,%d (title %.2f) -- quest put off %d s%s\"") &&
              has(readme, "`Quest: the check-in board is up at x,y (title 0.97) -- quest put off " +
                              n(QuestRun::kBoardMs / 1000) + " s`") &&
              has(closer, "say(\"the check-in board (Sự kiện và điểm danh) is up at \"") &&
              has(doc, "\"Popups: the check-in board (Sự kiện và điểm danh) is up at ...\"") &&
              has(closer, "say(std::string(\"closed \") + popup_kind_name(kind) + \" at \"") &&
              has(closer, "std::string(\"with ESC\")") &&
              has(readme, "`Popups: closed the check-in board at x,y (title 0.97) with ESC`") &&
              has(popups, "GB_INFO(\"Popups: kept %s (board %s)\"") &&
              has(board, "\"Checkin: a title search on %dx%d took %lld ms\"") &&
              has(doc, "\"Checkin: a title search on ... took N ms\"") &&
              has(readme, "`Checkin: a title search on WxH took N ms`"),
          "the board's lines -- the bag jobs', the quest's, the closer's and the title search's -- as the code writes "
          "them");

    // ---- the docs beside the notes ----
    check(has(goc, "Since 2026.11.09 it buys a fixed number each time, as GoC's `[Shopping]` (`buy_hp_quantity`, "
                   "`buy_mp_quantity`, \"Mode Exact\") does, and counts nothing") &&
              has(goc, "the *Sự kiện và điểm danh* board the client opens at every login and after 00:00, by ESC alone") &&
              has(proto, "**Not known (2026-11-09): no attendance packet**, nor one for the shop's full bag.") &&
              has(app_ini, "since 2026.11.09 the \"Sự kiện và điểm danh\" board the game opens at every login and as "
                           "the clock passes 00:00"),
          "docs/goc-comparison.md (a fixed number each time, as GoC's [Shopping]; the board by ESC), "
          "docs/protocol-findings.md (no attendance packet), app.ini's close_popups naming the board");

    // ---- what is no longer so is no longer said ----
    check(!has(readme, "a due *Tự mua bình* look may be skipped") &&
              !has(readme, "when Tiêu Dùng holds fewer slots of them than you set") &&
              !has(doc, "Bảng sự kiện điểm danh cũng không đóng") && !has(doc, "Món nào có ít ô hơn mức \"dưới\"") &&
              !has(doc, "Sau đó đặt \"dưới\" và \"tới\"") && !has(doc, "Mỗi \"kiểm tra mỗi N phút\"") &&
              !has(doc, "GoBot đếm bình, mở cửa hàng") &&
              !has(goc, "A purchase that adds no slot is not repeated") &&
              !has(pack, "Tu mua binh counts the two potions") && !has(trash_ini, "counts the slots") &&
              !has(app_ini, "Tự đóng thông báo: chỉ đóng thông báo nhận quà điểm danh, Yêu Quái Bách Khoa và menu ESC "
                            "do GoBot mở."),
          "the counting, the thresholds, the packets' skip and \"the board is not closed\" gone from every note");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_jobs_gui(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_back_off();
    test_glances();
    test_resumed_clock();
    test_quest_back_off();
    test_buff_skill_clash();
    test_pace_status_text();
    test_jobs_wiring();
    test_notes_1108();
    test_notes_1109();
}
