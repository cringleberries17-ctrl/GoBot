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
// by the slot, and the check-in board closed under "Tự đóng thông báo"; and
// 2026.11.10's purchase count with no cap of GoBot's own, and that release's
// notes -- the pet, the walk back, the bag pass, the key window, the death the
// screen alone read -- against the code the same way; and 2026.11.11's -- the
// shop right after the pass, HP during the bag jobs, the six keys, the digits.
//
// Nothing here goes near the desktop, the pointer, the keyboard's state or the
// game. The glances at the pointer are a pretend desktop's (InputGlancer's
// parts set by the test), the quest's rounds are made up, the macro runs a step
// at a time on a clock of the test's own with a pretend keyboard -- or once on
// its own threads, the keyboard still pretend -- and where only GoBot's window
// does a thing, its source is read.

#include "gobot/alerts.hpp"
#include "gobot/bag.hpp"
#include "gobot/cast_match.hpp"
#include "gobot/checkin_board.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/hunter.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/macro.hpp"
#include "gobot/packet_world.hpp"
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
    const std::string shop =
        slice(gui, "const auto run_shop = [&](bool forced, bool dry, bool after_pass) {", "while (!trash_stop_) {");
    check(in_order(shop, { "if (forced || dry) shop_quiet.reset();", "take_hold(\"trash cleaning\")",
                           "input_.release_all();", "seen_before = glancer.glance();",
                           "const DWORD quiet = last_input();",
                           "touched = !trash_stop_ && !lost && said == QuietSaid::Touched;",
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
    const std::string shop_job =
        slice(gui, "const auto run_shop = [&](bool forced, bool dry, bool after_pass) {", "while (!trash_stop_) {");
    const std::string charm_job = slice(gui, "const auto run_charms = [&](bool forced) {", "// ---- the potion shop ----");
    check(in_order(shop_job, { "if (dry) {", "shop_due_ms = due_before;", "const bool next_part = owed.hp + owed.mp > 0;",
                               "if (!next_part) shop_due_ms = plan_due_ms;",
                               "else if (trash_stop_) shop_due_ms = due_before;",
                               "else if (r.board_up) shop_due_ms = now_ms() + BagCleaner::kBoardWaitMs;",
                               "else if (hp_stop) shop_due_ms = now_ms() + bag::kHpStoppedAgainMs;",
                               "else if (end == bag::ShopEnd::Paused) shop_due_ms = now_ms() + "
                               "BagCleaner::kShopPartGapMs;",
                               "else if (end == bag::ShopEnd::Trouble) shop_due_ms = minutes(10);",
                               "else shop_due_ms = now_ms() + BagCleaner::kShopPartGapMs;" }) &&
              has(charm_job, "if (!forced && !trash_stop_ && ((!r.ran && (!only_message || !r.trouble.empty())) ||") &&
              has(charm_job, "if (!forced && r.tab_unsure) charm_check_ms = now_ms() + kCharmTabRetryMs;"),
          "a shop run cut by DỪNG/F10 owing the rest leaves its clock as it was (the next worker resumes from 45 s), a "
          "charm run's too (8 s) -- but a plan bought in full waits for the next plan whatever ended its round; one "
          "its guard stopped for HP is looked at again in 15 s (2026.11.11)");

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
    check(in_order(shop_job, { "return not_now(\"Máu đang thấp, mua bình sau.\", bag::kHpLookAgainMs);",
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
              has(macro, "\"%llu range, %llu walking back; refused: game not in front %llu\"") &&
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
    // 2026.11.09 had them 0 to 20, as its entry still says; since 2026.11.10
    // they go to kBuySlotsPerRunMost (test_buy_count_1110), an old key still
    // to kBuySlotsFromOldMost.
    const int most = CleanSettings::kBuySlotsPerRunMost, three = CleanSettings::kBuySlotsDefault;
    const int old_most = CleanSettings::kBuySlotsFromOldMost, then = 20;
    check(has(bag_cpp, "slots(\"buy_hp_slots\", \"buy_hp_to\", \"buy_hp_below\")") &&
              has(bag_cpp, "slots(\"buy_mp_slots\", \"buy_mp_to\", \"buy_mp_below\")") &&
              has(shop, "label(\"mỗi lần mua\");") && has(shop, "slots(\"Thịt bò khô (HP):\", \"hp\"") &&
              has(shop, "slots(\"Rượu lá tre (MP):\", \"mp\"") &&
              has(shop, "n = std::clamp(n, 0, CleanSettings::kBuySlotsPerRunMost);") &&
              has(doc, "\"Thịt bò khô (HP): mỗi lần mua [N] ô\" và \"Rượu lá tre (MP): mỗi lần mua [N] ô\", từ 0 tới " +
                           n(then) + " (0 là không mua loại đó; từ bản 2026.11.10 không còn giới hạn " + n(then) +
                           ").") &&
              has(doc, "trash.ini của bản zip đặt " + n(three) + " và " + n(three) + ".") &&
              has(doc, "bản này lấy số \"tới\" làm số ô mua mỗi lần, cho tới khi bạn đổi số đó trên thẻ") &&
              has(readme, "`buy_hp_slots` and `buy_mp_slots` in `trash.ini`: 0 to " + n(then) + " each in 2026.11.09, "
                          "and since 2026.11.10, the owner's \"Không giới hạn\", 0 to " + n(most) +
                              " each (`CleanSettings::kBuySlotsPerRunMost`, a bound of the number alone: the "
                              "bag-full message ends the round), " + n(three) + " and " + n(three) +
                              " as shipped (`kBuySlotsDefault`)") &&
              has(readme, "A file without them reads its old `buy_hp_to` and `buy_mp_to` -- the owner's 10 and 10 -- "
                          "until the card saves the new key, " + n(old_most) + " at most (`kBuySlotsFromOldMost`)") &&
              has(trash_ini, "a slot of the bag each: 0 to " + n(most) + ". 0 leaves that potion alone.") &&
              has(trash_ini, "(buy_hp_to, buy_mp_to; " + n(old_most) + " at the most), until that number is changed "
                             "on the card, which writes the new key; with neither, " + n(three) + ".") &&
              has(trash_ini, "(buy_hp_to, buy_mp_to; tối đa " + n(old_most) + ") cho tới khi bạn đổi số đó trên thẻ "
                             "(GoBot ghi dòng mới); không có cả hai thì là " + n(three) + ".") &&
              has(pack, "(moi lan mua N o, 0 to " + n(then) + " then and 0 to " + n(most) + " since 2026.11.10, " +
                            n(three) + " and " + n(three) + " as shipped;") &&
              has(pack, "reads its old buy_hp_to and buy_mp_to, " + n(old_most) + " at most, until the card is "
                        "changed)"),
          "the slots, 0 to " + n(most) + " (" + n(then) + " in 2026.11.09) and " + n(three) + " as shipped, a file's "
          "old buy_*_to read until the card saves, " + n(old_most) + " at most -- as CleanSettings::load and the card "
          "have them");

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
                             "minute later; \"Mua ngay\" buys up to " + n(all) + " at once, and the rest the same "
                             "way, " + n(per_hold) + " a minute.") &&
              has(pack, n(per_hold) + " purchases a hold and the rest a minute later; Mua ngay buys up to " + n(all) +
                            " at once.") &&
              gap_min == 1 &&
              has(doc, "nút \"Mua ngay\" mua ngay số ô đã đặt, không chờ giờ: tối đa " + n(all) + " lần trong một "
                       "lượt, phần còn lại mua tiếp như lượt theo giờ (" + n(per_hold) + " lần, sau " + n(gap_min) +
                       " phút).") &&
              has(goc, n(per_hold) + " purchases a hold and the rest a minute later (**Mua ngay**: up to " + n(all) +
                           " in one hold)"),
          "a timed round " + n(per_hold) + " purchases, the rest " + n(gap_s) + " s later, \"Mua ngay\" " + n(all) +
              " at most and the rest in timed parts -- as BagCleaner has them");

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

// ------------------------------------------- 2026.11.10: the purchase count

/// The owner on 2026-11-10: "Mua HP/MP cần để người chơi tự setting bao nhiêu
/// lần chứ không phải là max 20", answered "Không giới hạn". The card's box,
/// its words and the notes against the constants; what a hold buys as it was.
void test_buy_count_1110() {
    section("2026.11.10: the potion shop's purchase count with no cap of GoBot's own -- the card, its words, the "
            "notes");
#ifdef GOBOT_SOURCE_DIR
    const std::string doc = flat(source("/DOC-TRUOC.txt"));
    const std::string readme = flat(source("/README.md"));
    const std::string pack = flat(source("/package.ps1"));
    const std::string trash_ini = flat(source("/configs/trash.ini"), true);
    const std::string goc = flat(source("/docs/goc-comparison.md"));
    const std::string shop = source("/apps/gobot_gui/simple_shop.cpp");
    const std::string bag_cpp = source("/src/bag.cpp");
    const std::string bag_hpp = source("/include/gobot/bag.hpp");
    if (doc.empty() || readme.empty() || pack.empty() || trash_ini.empty() || goc.empty() || shop.empty() ||
        bag_cpp.empty() || bag_hpp.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const auto n = [](auto v) { return std::to_string(v); };
    using B = BagCleaner;
    using C = CleanSettings;

    // ---- the numbers: a bound of the box only; an old key as 2026.11.09 read it ----
    check(C::kBuySlotsPerRunMost == 999 && C::kBuySlotsFromOldMost == 20 && B::kShopMaxBuys == 40 &&
              B::kShopBuysPerHold == 10 && B::kShopPartGapMs == 60'000 &&
              has(bag_hpp, "static constexpr int kBuySlotsPerRunMost = 999;") &&
              has(bag_hpp, "static constexpr int kBuySlotsFromOldMost = 20;") &&
              has(bag_cpp,
                  "return std::clamp(ini.get_int(\"Trash\", key, kBuySlotsDefault), 0, kBuySlotsPerRunMost);") &&
              has(bag_cpp,
                  "return std::clamp(ini.get_int(\"Trash\", before, kBuySlotsDefault), 0, kBuySlotsFromOldMost);"),
          "the slots 0 to 999 as set, an old buy_*_to 20 at most; a hold's 40 and 10 and a part's minute as they "
          "were -- CleanSettings::load");

    // ---- the card: 0 to the bound, steps of 1 and 10, words from the constants ----
    check(has(shop, "ImGui::InputInt(\"##slots\", &n, 1, 10);") &&
              has(shop, "n = std::clamp(n, 0, CleanSettings::kBuySlotsPerRunMost);") &&
              has(shop, "std::to_string(CleanSettings::kBuySlotsPerRunMost)") &&
              has(shop, "ImGui::SetItemTooltip(\"%s\", slots_tip.c_str());") &&
              has(shop, "GoBot không giới hạn số này: HP và MP chung ngăn Tiêu thụ, game báo túi đồ đã \"") &&
              has(shop, "\"đầy thì GoBot dừng lượt đó.\"") && !has(shop, "0–20") && !has(shop, "(0-20)"),
          "the card's box: 0 to kBuySlotsPerRunMost, steps 1 and 10, its tooltip from the constants and saying a "
          "full bag ends the round; no \"0–20\" left");
    check(in_order(shop, { "const int planned = simple_clean_.buy_hp_slots + simple_clean_.buy_mp_slots;",
                           "if (simple_clean_.buy_potions && planned <= 0)",
                           "if (planned > BagCleaner::kShopMaxBuys) {",
                           "std::to_string(BagCleaner::kShopBuysPerHold) + \" lần (\\\"Mua ngay\\\": \" +",
                           "std::to_string(BagCleaner::kShopMaxBuys) + \" lần), phần còn lại mua tiếp, mỗi \" +",
                           "std::to_string(BagCleaner::kShopPartGapMs / 60'000) +", "hint(over.c_str());" }) &&
              in_order(shop, { "static const std::string buy_now_tip =",
                               "std::to_string(BagCleaner::kShopMaxBuys) + \" lần; phần còn lại mua tiếp, mỗi \" +",
                               "std::to_string(BagCleaner::kShopPartGapMs / 60'000) + \" phút một lượt \" +",
                               "std::to_string(BagCleaner::kShopBuysPerHold) +",
                               "Game báo túi đồ đã đầy thì dừng lượt đó.",
                               "ImGui::SetItemTooltip(\"%s\", simple_clean_.buy_potions ? buy_now_tip.c_str() : "
                               "\"Bật ô \\\"Tự mua bình\\\" trước.\");" }) &&
              !has(shop, "Mua ngay bây giờ đủ số ô đã đặt trong một lượt"),
          "a hint only over kShopMaxBuys in all, and \"Mua ngay\"'s tooltip: " + n(B::kShopMaxBuys) + " in its hold, "
          "the rest " + n(B::kShopBuysPerHold) + " a part every " + n(B::kShopPartGapMs / 60'000) +
          " minute, a full bag ending it -- from the constants");

    // ---- the notes: what is no longer so is no longer said ----
    check(has(readme, "100 a slot, 0 to " + n(C::kBuySlotsPerRunMost) + " each (no cap of GoBot's own since "
                      "2026.11.10; 20 before), 3 and 3 as shipped") &&
              has(readme, "`buy_hp_slots` and `buy_mp_slots`, 0 to " + n(C::kBuySlotsPerRunMost) +
                              " since 2026.11.10 (20 before), 3 as shipped -- a file without them reads its old "
                              "`buy_hp_to` and `buy_mp_to`, " + n(C::kBuySlotsFromOldMost) + " at most,") &&
              has(doc, "(\"mỗi lần mua N ô\", 0 tới " + n(C::kBuySlotsPerRunMost) + ", mặc định " +
                           n(C::kBuySlotsDefault) + "; từ bản 2026.11.10 GoBot không giới hạn số này, trước là 20: game "
                           "báo túi đầy thì dừng lượt đó)") &&
              has(trash_ini, "từ 0 tới " + n(C::kBuySlotsPerRunMost) + "; 0 là không mua loại đó. GoBot không giới hạn "
                             "số này (trước bản 2026.11.10 là 20): game báo túi đồ đã đầy thì dừng lượt đó.") &&
              has(trash_ini, "GoBot sets no cap of its own (20 until 2026.11.10): the game's message that the bag is "
                             "full ends the round.") &&
              has(goc, "0 to " + n(C::kBuySlotsPerRunMost) + " each (no cap of GoBot's own since 2026.11.10, 20 "
                       "before: the bag-full message ends the round)") &&
              !has(readme, "100 a slot, 0 to 20 each") && !has(readme, "`buy_mp_slots`, 0 to 20, 3 as shipped") &&
              !has(doc, "(\"mỗi lần mua N ô\", 0 tới 20, mặc định") && !has(doc, "nút \"Mua ngay\" mua ngay đủ số ô") &&
              !has(trash_ini, "a slot of the bag each: 0 to 20.") && !has(trash_ini, "buys them all at once") &&
              !has(pack, "Mua ngay buys them all at once") && !has(goc, "0 to 20 each, 3 and 3 as shipped") &&
              !has(goc, "(**Mua ngay**: all in one hold)"),
          "README, DOC-TRUOC.txt, trash.ini and docs/goc-comparison.md: 0 to " + n(C::kBuySlotsPerRunMost) +
              " with no cap of GoBot's own, a full bag ending the round, \"Mua ngay\" " + n(B::kShopMaxBuys) +
              " at most -- no \"0 to 20\" left but as 2026.11.09 had it");
#else
    skip("no source folder\n");
#endif
}

// ------------------------------------------------ 2026.11.10: the notes

/// 2026.11.10's notes against the code: DOC-TRUOC.txt's entry and its live
/// test step 13, README.md's release section, START-HERE (package.ps1), the
/// comments of trash.ini, hunt.ini and alerts.ini, and docs/goc-comparison.md
/// -- the owner's words and answers as written, and the numbers and the words
/// of the screen and the log they quote, as the code has them: the pet pressed
/// over and over, the walk back first, the bag pass, the key window, the
/// purchases and the death the screen alone read.
void test_notes_1110() {
    section("The 2026.11.10 notes against the code: the pet, the walk back, the bag pass, the key window, the "
            "purchases, the death alert, the words quoted");
#ifdef GOBOT_SOURCE_DIR
    const std::string doc = flat(source("/DOC-TRUOC.txt"));
    const std::string readme = flat(source("/README.md"));
    const std::string pack = flat(source("/package.ps1"));
    const std::string trash_ini = flat(source("/configs/trash.ini"), true);
    const std::string hunt_ini = flat(source("/configs/hunt.ini"), true);
    const std::string alerts_ini = flat(source("/configs/alerts.ini"), true);
    const std::string goc = flat(source("/docs/goc-comparison.md"));
    const std::string pet_cpp = source("/src/macro_pet.cpp");
    const std::string macro_cpp = source("/src/macro.cpp");
    const std::string hunter_cpp = source("/src/hunter.cpp");
    const std::string bag_cpp = source("/src/bag.cpp");
    const std::string bag_hpp = source("/include/gobot/bag.hpp");
    const std::string coord_cpp = source("/src/execution_coordinator.cpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    const std::string potions = source("/apps/gobot_gui/simple_potions.cpp");
    const std::string spot = source("/apps/gobot_gui/simple_spot.cpp");
    const std::string map = source("/apps/gobot_gui/simple_map.cpp");
    const std::string alerts = source("/apps/gobot_gui/simple_alerts.cpp");
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    if (doc.empty() || readme.empty() || pack.empty() || trash_ini.empty() || hunt_ini.empty() ||
        alerts_ini.empty() || goc.empty() || pet_cpp.empty() || macro_cpp.empty() || hunter_cpp.empty() ||
        bag_cpp.empty() || bag_hpp.empty() || coord_cpp.empty() || view.empty() || potions.empty() ||
        spot.empty() || map.empty() || alerts.empty() || gui.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto n = [](auto v) { return std::to_string(v); };
    // A time as the notes write it: 500 ms "0,5", 3000 "3".
    const auto vi_s = [](std::uint64_t ms) {
        std::string s = std::to_string(ms / 1000);
        if (ms % 1000 != 0) s += "," + std::to_string(ms % 1000 / 100);
        return s;
    };
    using MC = MacroConfig;
    using E = MacroEngine;
    using B = BagCleaner;
    using C = CleanSettings;

    // ---- where: on top of 2026.11.09's entry; its live test step 13 ----
    const std::size_t e10 = doc.find("- Mới trong bản 2026.11.10 (");
    const std::size_t e09 = doc.find("- Mới trong bản 2026.11.09 (");
    const std::size_t step12 = doc.find("12. Bảng điểm danh và Tự mua bình (bản 2026.11.09, chưa thử trên game thật):");
    const std::size_t step13 = doc.find("13. Bơm máu pet, về vùng đánh quái, dọn rác và mua bình (bản 2026.11.10, chưa "
                                        "thử trên game thật;");
    const std::size_t revive = doc.find("- Tự hồi sinh: thử ở bản đồ dễ");
    const std::size_t sec7 = doc.find("7. KHI GẶP LỖI");
    const std::size_t r09 = readme.find("### Buying by the slot, and the check-in board (2026.11.09)");
    const std::size_t r10 = readme.find("### The pet, the walk back, the bag and the shop (2026.11.10)");
    const std::size_t rkeys = readme.find("### The hold's key window (2026.11.10)");
    check(e10 != npos && e09 != npos && e10 < e09 && step12 != npos && step13 != npos && revive != npos &&
              sec7 != npos && step12 < step13 && step13 < revive && revive < sec7 &&
              has(doc, "2026.11.10 (bơm máu pet liên tục, về vùng đánh quái trước, dọn rác nhanh hơn, kỹ năng khi dọn "
                       "rác và mua bình, mua bình không giới hạn số lần, theo điều bạn báo ngày 10/11; chưa thử trên "
                       "game thật)") &&
              has(doc, "Thử như bước 13 ở mục 6.") && r09 != npos && r10 != npos && rkeys != npos && r09 < r10 &&
              r10 < rkeys && has(readme, "Since 2026.11.10 (not yet run on the game).") &&
              has(pack, "The pet, walking back, the bag and the shop (2026.11.10, not yet tried on the game)."),
          "DOC-TRUOC.txt: the 2026.11.10 entry on top of 2026.11.09's, not yet tried on the game, its live test "
          "step 13 of section 6; README's release section before the key window's, and START-HERE, say it too");

    // ---- the owner's words and answers, as written ----
    const char* const owner[] = {
        "\"Bơm máu pet chậm\"",
        "\"Thao tác dọn rác chậm\"",
        "\"dọn rác chậm là từ user feedback\"",
        "\"Không tự di chuyển quay về vùng đánh quái\"",
        "\"Khi dọn rác thì cần ra các skill thuộc phím chữ cái (z,x,c,v, v.v.)\"",
        "\"cả khi mua HP và MP cũng cần ra skill nhé\"",
        "\"có tính phím space, vì phím space không ảnh hưởng\"",
        "\"lúc dùng bùa thì không cần ra skill\"",
        "\"Mua HP/MP cần để người chơi tự setting bao nhiêu lần chứ không phải là max 20\"",
        "\"Liên tục như GoC (0,2–0,5 s)\"",
        "(a) \"Ngưng đánh, đi về trước\"",
        "\"Bấm cả bình\"",
        "\"Không giới hạn\"",
    };
    bool quoted = true;
    for (const char* w : owner) quoted = quoted && has(doc, w);
    const char* const owner_en[] = {
        "*\"Bơm máu pet chậm\"*",
        "*\"Thao tác dọn rác chậm\"*",
        "*\"dọn rác chậm là từ user feedback\"*",
        "*\"Không tự di chuyển quay về vùng đánh quái\"*",
        "*\"Mua HP/MP cần để người chơi tự setting bao nhiêu lần chứ không phải là max 20\"*",
        "*\"Liên tục như GoC (0,2–0,5 s)\"*",
        "(a) *\"Ngưng đánh, đi về trước\"*",
        "*\"Bấm cả bình\"*",
        "*\"Không giới hạn\"*",
        "*\"Khi dọn rác thì cần ra các skill thuộc phím chữ cái (z,x,c,v, v.v.)\"*",
        "*\"cả khi mua HP và MP cũng cần ra skill nhé\"*",
        "*\"có tính phím space, vì phím space không ảnh hưởng\"*",
        "*\"lúc dùng bùa thì không cần ra skill\"*",
    };
    for (const char* w : owner_en) quoted = quoted && has(readme, w);
    check(quoted && has(doc, "Câu trả lời của bạn ngày 10/11:"),
          "the owner's five requests and four answers of 2026-11-10 quoted as written, and \"dọn rác chậm là từ "
          "user feedback\", in DOC-TRUOC.txt and README");

    // ---- the pet: the box's numbers, the turn a skill gets ----
    const std::string least = n(MC::kPetEveryLeastMs), most = n(MC::kPetEveryMostMs), def = n(MC::kDefaultPetEveryMs);
    check(MC::kPetEveryLeastMs == 200 && MC::kPetEveryMostMs == 3000 && MC::kDefaultPetEveryMs == 300 &&
              has(view, "label(\"Bơm liên tục, mỗi\");") &&
              has(doc, "có ô mới \"Bơm liên tục, mỗi [N] ms\", từ " + least + " tới " + most + " ms, mặc định " + def +
                           " ms, một số cho mọi phím pet") &&
              has(doc, "macro.ini chưa có dòng pet_every_ms (như của bạn: Q đặt 3000 ms ở cả hai dòng) thì đọc là " +
                           def + " ms.") &&
              has(doc, "bản này bấm Q mỗi " + def + " ms khi máu pet dưới mức") &&
              has(doc, "macro.ini của bản trước 2026.11.10 không có dòng pet_every_ms: bản này đọc là " + def + " ms") &&
              has(readme, "(`MacroConfig::pet_every_ms`, `kPetEveryLeastMs`-`kPetEveryMostMs`, " + least + "-" + most +
                              " ms; `kDefaultPetEveryMs`, " + def + ", in a file without it") &&
              has(readme, "`pet_every_ms`, how often the pet's potions go while the pet is under its mark (" + least +
                              "-" + most + " ms, " + def + " as shipped and in a file without it)") &&
              has(pack, "(" + least + " to " + most + " ms; " + def + " as shipped and in a macro.ini without "
                        "pet_every_ms, where it was about every 3 s)") &&
              has(pack, "A macro.ini from before 2026.11.10 has no pet_every_ms: its pet keys are then pressed every " +
                            def + " ms") &&
              has(doc, "phím đó liên tục, mỗi \"Bơm liên tục, mỗi N ms\" một lần (mặc định " + def +
                           " ms; trước bản 2026.11.10 là khoảng 3 giây)"),
          "the pet's box " + least + " to " + most + " ms, " + def + " in a file without it -- as MacroConfig has it "
          "-- in the entry, section 6's pet step, the update notes, README's table and START-HERE");
    check(E::kPetYieldMostMs == 500 && has(doc, "(pet chờ nhiều nhất " + vi_s(E::kPetYieldMostMs) + " giây)") &&
              has(readme, "`kPetYieldMostMs` " + n(E::kPetYieldMostMs) + " at most, *" +
                              E::potion_why_name(E::PotionWhy::SkillTurn) + "*") &&
              std::string(E::potion_why_name(E::PotionWhy::SkillTurn)) == "a skill's turn" &&
              MC::kRecheckPeakMs == 700 &&
              has(doc, "theo lần đọc cao nhất trong " + vi_s(MC::kRecheckPeakMs) + " giây sau khi bấm") &&
              has(readme, "`MacroConfig::kRecheckPeakMs` (" + n(MC::kRecheckPeakMs) + " ms)") &&
              has(view, "\"Lv2 cùng phím không bấm nhanh hơn: Lv1 và Lv2 trên một phím là một bình, \"") &&
              has(doc, "\"Lv2 cùng phím không bấm nhanh hơn: ...\""),
          "a skill's turn after the pet's press, " + n(E::kPetYieldMostMs) + " ms of waiting at most; the peak "
          "within " + n(MC::kRecheckPeakMs) + " ms; the Lv2 note -- as the engine and the row have them");

    // ---- the pet's log: the minute's line, a still run, the plateau's picture ----
    char still_head[256];
    std::snprintf(still_head, sizeof still_head,
                  "Potion check: Pet key %s pressed at %d%%, the bar within %.0f%% of it for %llu ms -- no rise; "
                  "pressed on every %u ms all the same",
                  "Q", 71, static_cast<double>(PetHealWatch::kStaticPct),
                  static_cast<unsigned long long>(PetHealWatch::kJudgeMs), MC::kDefaultPetEveryMs);
    const std::string still = std::string(still_head) + " (more often than the " + n(MC::kDefaultRecheckMs) +
                              "-ms check)";
    const std::string minute = "Pet heals: key Q, N pressed; of N judged, N rose (median +N% after N ms; two that "
                               "rose N s apart at the closest), N static -- the bar still -- in N runs (N at 71%, ";
    check(has(pet_cpp, "\"Pet heals: key %s, %llu pressed; of %llu judged, %llu rose\"") &&
              has(pet_cpp, "\" (median +%d%% after %llu ms\"") &&
              has(pet_cpp, "\"; two that rose %.1f s apart at the closest\"") &&
              has(pet_cpp, "\", %llu static -- the bar still -- in %zu run%s (\"") &&
              has(pet_cpp, "\" no rise with the bar moving\"") && has(pet_cpp, "\" with no reading\"") &&
              has(doc, "\"" + minute + "...), N no rise with the bar moving, N with no reading\"") &&
              has(readme, "`" + minute + "…), N no rise with the bar moving, N with no reading`") &&
              PetHealWatch::kJudgeMs == 700 &&
              has(readme, "`PetHealWatch::kJudgeMs` (" + n(PetHealWatch::kJudgeMs) + " ms)"),
          "the minute's \"Pet heals\" line, as PetHealWatch::minute_line writes it");
    check(has(pet_cpp, "GB_INFO(\"Potion check: Pet key %s pressed at %d%%, the bar within %.0f%% of it for %llu ms -- "
                       "no rise; pressed \"") &&
              has(pet_cpp, "\"on every %u ms all the same (more often than the %u-ms check), the minute's") &&
              has(doc, "\"" + still + ", ...\"") && has(readme, "`" + std::string(still_head) + " …`"),
          "a still run's \"" + still + "\" -- as pet_outcomes writes it");
    check(E::kPlateauLowPct == 70 && E::kPlateauHighPct == 75 &&
              has(potions, "\"Pet heals: the pet's bar still at \" + std::to_string(plateau) +") &&
              has(potions, "\"% through a press of its potion\";") &&
              has(potions, "stamp() + \"_pet_\" + std::to_string(plateau) + \".png\";") &&
              has(potions, "return text + \"; kept a picture of the pet's slot as potion_debug\\\\\" + name;") &&
              has(doc, "\"Pet heals: the pet's bar still at 71% through a press of its potion; kept a picture of the "
                       "pet's slot as potion_debug\\...\"") &&
              has(doc, "Lần đầu thanh đứng yên ở " + n(E::kPlateauLowPct) + "–" + n(E::kPlateauHighPct) + "%") &&
              has(readme, "`MacroEngine::kPlateauLowPct`-`kPlateauHighPct` (" + n(E::kPlateauLowPct) + "-" +
                              n(E::kPlateauHighPct) + "%;") &&
              has(readme, "`potion_debug\\<time>_pet_<pct>.png`"),
          "the plateau's picture, " + n(E::kPlateauLowPct) + "-" + n(E::kPlateauHighPct) + "%, and its line -- as "
          "GoBot's window keeps it");

    // ---- the walk back first: the edge, the waits, the card ----
    HunterConfig hc;
    check(kZoneOutsideBackPx == 60 &&
              has(doc, "ra ngoài vùng quá " + n(kZoneOutsideBackPx) + " px (vùng có đường về, hay chỗ đánh quái của "
                       "lần chạy này không cao hơn một lần nhảy) thì bot ngưng đánh ngay") &&
              has(doc, "(không chờ " + vi_s(hc.spot_return_delay_ms) + " giây)") &&
              has(doc, "Đi về mà " + vi_s(hc.spot_give_up_ms) + " giây không tới gần hơn (vướng tường) thì bot thôi, "
                       "đánh ở chỗ đang đứng, " + vi_s(hc.spot_retry_ms) + " giây sau thử lại") &&
              has(readme, "more than `kZoneOutsideBackPx` (" + n(kZoneOutsideBackPx) + ") out, nothing is fought until "
                          "the character is inside the box") &&
              has(pack, "More than " + n(kZoneOutsideBackPx) + " px outside the hunting area the bot stops fighting") &&
              has(spot, "if (back) return out + \" — bot ngưng đánh, về vùng trước: đi bộ về \" + way + \", vào tới "
                        "trong vùng rồi mới đánh tiếp.\";") &&
              has(doc, "\"Nhân vật đang ở ngoài vùng N px — bot ngưng đánh, về vùng trước: đi bộ về X, Y, vào tới "
                       "trong vùng rồi mới đánh tiếp.\""),
          "the walk back first past " + n(kZoneOutsideBackPx) + " px, no " + n(hc.spot_return_delay_ms) + " ms wait, "
          "a wall given up after " + n(hc.spot_give_up_ms) + " ms -- as hunt_zone.hpp and HunterConfig have them -- "
          "and the card's words");
    const std::string loading = "Vừa qua cửa về lại bản đồ này: chờ bản đồ tải xong rồi dùng lại vùng đã lưu và đi bộ "
                                "về vùng.";
    check(Hunter::kDoorLoadMs == 3000 && has(spot, "hint(\"" + loading + "\");") &&
              has(doc, "\"" + loading + "\"") && has(readme, "*" + loading + "*") &&
              has(doc, "(gói tin báo chỗ nhân vật, hay sau " + vi_s(Hunter::kDoorLoadMs) + " giây)") &&
              has(readme, "`Hunter::kDoorLoadMs` (" + n(Hunter::kDoorLoadMs / 1000) + " s)") &&
              has(hunt_ini, "Back on an area's map through any door, the area is taken again once the map has loaded") &&
              has(hunt_ini, "qua cửa nào cũng được về lại bản đồ có vùng thì bot dùng lại vùng khi bản đồ tải xong"),
          "a door back to the area's map: the area taken again once loaded, " + n(Hunter::kDoorLoadMs) + " ms at "
          "most -- as Hunter::kDoorLoadMs and the card have it");
    check(Hunter::kOneJumpPx == 120 && Hunter::kSpotOddSayMs == 60'000 &&
              has(hunter_cpp, "GB_INFO(\"Hunter: a report of yours at %d,%d not taken for the hunting spot -- %s "
                              "(opcode 0x%04X)%s\",") &&
              has(hunter_cpp, "\"y 0 is no place to stand\"") &&
              has(doc, "\"Hunter: a report of yours at 999,0 not taken for the hunting spot -- y 0 is no place to "
                       "stand (opcode 0x....)\"") &&
              has(readme, "`Hunter::kOneJumpPx` (" + n(Hunter::kOneJumpPx) + ")") &&
              has(readme, "`Hunter: a report of yours at x,y not taken for the hunting spot -- …`") &&
              has(map, "const std::string label = \"Dùng vùng \" + pick + \"##map_choose\";") &&
              has(doc, "nút \"Dùng vùng <tên file>\" (ví dụ \"Dùng vùng t1901_s2\")") &&
              has(readme, "*Dùng vùng <stem>*"),
          "no spot at y 0 or a jump up with no jump, said as the hunter says it; the Bản đồ card's \"Dùng vùng\"");

    // ---- the walk's log, the own walk's line made by the code ----
    PacketWorld::OwnWalk w;
    w.from_x = 25.0f;
    w.from_y = 632.0f;
    w.to_x = 363.0f;
    w.to_y = 632.0f;
    w.from_ms = 10'000;
    w.to_ms = 10'940;
    w.ended_by = 0x0027;
    w.casts = 2;
    std::vector<InputManager::KeyMark> marks(2);
    marks[0].vk = 'Z';
    marks[0].down = true;
    marks[0].at_ms = w.from_ms - 85;
    marks[1].vk = 'Z';
    marks[1].at_ms = w.from_ms - 5;
    const std::string walk =
        PacketWorld::own_walk_line(w, InputManager::walk_keys_text(marks, VK_LEFT, VK_RIGHT, w.from_ms, w.to_ms));
    check(walk == "Positions: own walk 25,632 -> 363,632 (+338 px) in 0.94 s, stopped by 0x0027; 2 casts of yours in "
                  "it; GoBot: no arrow within " + n(InputManager::kWalkKeyMs) + " ms; last key Z 85 ms before" &&
              has(doc, "\"" + walk + "\"") && has(readme, "`" + walk + "`") &&
              has(doc, "trong " + n(InputManager::kWalkKeyMs) + " ms quanh đó") &&
              has(readme, "`kWalkKeyMs` " + n(InputManager::kWalkKeyMs)),
          "the own walk's line the notes quote, as own_walk_line and walk_keys_text write it -- " + walk);
    check(has(hunter_cpp, "GB_INFO(\"Hunter: %d px outside the hunting area -- no fight until the character is back "
                          "inside it: walking back \"") &&
              has(hunter_cpp, "\"first, %s (reach %d px)\",") &&
              has(hunter_cpp, "GB_INFO(\"Hunter: the walk back first is over after %.1f s, from %d px outside -- %s\",") &&
              has(hunter_cpp, "back_walk_end(\"inside the hunting area: fighting again\");") &&
              has(hunter_cpp, "GB_INFO(\"Hunter: walk back over after %.1f s -- %s\",") &&
              has(hunter_cpp, "\"; outside the area %.1f s, walking back %.1f s\"") &&
              has(macro_cpp, "\"%llu range, %llu walking back; refused: game not in front %llu\"") &&
              has(doc, "\"Hunter: N px outside the hunting area -- no fight until the character is back inside it: "
                       "walking back first, to x,y (reach 450 px)\"") &&
              has(doc, "\"Hunter: the walk back first is over after N s, from N px outside -- inside the hunting area: "
                       "fighting again\"") &&
              has(doc, "\"Hunter: walk back over after N s -- ...\"") &&
              has(doc, "\"; outside the area N s, walking back N s\"") &&
              has(doc, "dòng \"Skills:\" thêm \", N walking back\"") &&
              has(readme, "`Hunter: N px outside the hunting area -- no fight until the character is back inside it: "
                          "walking back first, to x,y (reach N px)`") &&
              has(readme, "`Hunter: the walk back first is over after N s, from N px outside -- …`") &&
              has(readme, "`Hunter: walk back over after N s -- …`") &&
              has(readme, "`; outside the area N s, walking back N s`") && has(readme, "`, N walking back`"),
          "the walk back's lines, the minute's time outside and the Skills line's walking back -- as the hunter and "
          "the macro write them");

    // ---- the bag pass: nothing to look through, Tiêu Dùng alone, the steps ----
    const std::string spend_hint = "Mọi món trong danh sách đều nằm ở ngăn Tiêu Dùng, nên khi dọn GoBot chỉ tìm ở "
                                   "ngăn đó.";
    check(has(doc, "\"" + std::string(bag::kNoTabVi) + ".\"") && has(doc, "\"" + std::string(bag::kNoUseTabVi) + ".\"") &&
              has(view, "coloured(kWait, (std::string(looked.none_said()) + \".\").c_str());") &&
              has(view, "hint(\"" + spend_hint + "\");") && has(doc, "\"" + spend_hint + "\"") &&
              has(readme, "*" + std::string(bag::kNoTabVi) + "*") &&
              has(bag_cpp, "GB_INFO(\"Trash: every item on the list is a spend/ icon -- only Tiêu Dùng looked through, "
                           "not %s\",") &&
              has(doc, "\"Trash: every item on the list is a spend/ icon -- only Tiêu Dùng looked through, not ...\"") &&
              has(readme, "`Trash: every item on the list is a spend/ icon -- only Tiêu Dùng looked through, not …`"),
          "no tab ticked, or Tiêu Dùng alone and not ticked: the card's words as bag::pass_tabs says them; the "
          "spend-only line as the pass writes it");
    const std::string item = "Trash: item at Tiêu Dùng, hàng 1 ô 1 (giống 97%) deleted in N ms -- drag N ms, in the "
                             "bin after N ms (N looks), its dialog N ms after the click, the box read on N looks in N "
                             "ms, closed N ms after Xác Nhận (1 question), then N ms till the item was seen gone, last "
                             "look N ms";
    const std::string steps = "Trash: pass steps -- bag opened in N ms (N looks); Tiêu Dùng: tab N ms, top N ms, N "
                              "pages (scrolling N ms), N items in N ms; bag shut in N ms; N ms in all";
    check(has(bag_cpp, "return \"item at \" + where + (gone ? \" deleted in \" : \" not deleted, after \") + "
                       "ms(t.total) +") &&
              has(bag_cpp, "step(\"in the bin after \" + ms(t.in_bin) + \" (\"") &&
              has(bag_cpp, "step(\"its dialog \" + ms(t.dialog) + \" after the click\");") &&
              has(bag_cpp, "step(\"closed \" + ms(t.closed) + \" after Xác Nhận (\"") &&
              has(bag_cpp, "(t.early ? \" till the item was seen gone\" : \" watching for another question\")") &&
              has(bag_cpp, "GB_INFO(\"Trash: pass steps -- %s; %d ms in all\",") &&
              has(bag_cpp, "\": tab \" + std::to_string(t.select) + \" ms, top \"") &&
              has(bag_cpp, "line += \"; bag shut in \"") && has(doc, "\"" + item + "\"") &&
              has(readme, "`" + item + "`") && has(doc, "\"" + steps + "\"") && has(readme, "`" + steps + "`") &&
              has(bag_cpp, "constexpr int kQuickLookMs = 80;") && has(bag_cpp, "constexpr int kDoneApartMs = 150;") &&
              has(doc, "GoBot nhìn lại mỗi 80 ms (trước là 150 ms)") &&
              has(doc, "hai lần nhìn cách nhau 0,15 giây"),
          "each item's and each pass's timing line, as the cleaner writes them; the 80-ms looks and the two looks "
          "150 ms apart as bag.cpp has them");

    // ---- the key window: what goes through, the waits, the words ----
    check(pass_key_allowed('Z') && pass_key_allowed('G') && pass_key_allowed(0x20) && !pass_key_allowed('6') &&
              pass_key_allowed(VK_HOME) && pass_key_allowed(VK_END) && pass_key_allowed(VK_PRIOR) &&
              pass_key_allowed(VK_NEXT) && pass_key_allowed(VK_INSERT) && pass_key_allowed(VK_DELETE) &&
              !pass_key_allowed(VK_DOWN) && !pass_key_allowed(VK_NUMPAD2) && !pass_key_allowed('1') &&
              !pass_key_allowed(VK_RETURN) && !pass_key_allowed(VK_ESCAPE) && !pass_key_allowed(VK_LEFT) &&
              !pass_key_allowed(VK_CONTROL) && !pass_key_allowed(VK_F5) && !pass_key_allowed(VK_OEM_3) &&
              has(doc, "các phím của macro ở chữ cái A–Z hay Space (kỹ năng, buff và bình: của bạn là Z X C V Space và "
                       "G, F, Q)") &&
              !has(doc, "của bạn là Z X C V B Space A S") &&
              has(doc, "Không bao giờ để qua: phím số (như buff 6 của bạn: chờ xong lượt), Enter, Esc, phím mũi tên, "
                       "Ctrl, Shift, Alt, phím F, các phím dấu") &&
              has(doc, "Dùng bùa thì vẫn dừng hết mọi phím như trước.") &&
              has(pack, "the macro's keys on a letter or Space -- skills, buffs and potions -- go on between GoBot's own "
                        "clicks") &&
              has(pack, "digit keys wait, and Dung bua still holds every key."),
          "letters, Space and Home, End, PgUp, PgDn, Insert, Delete through (2026.11.11); digits, the number pad, "
          "Enter, Esc, arrows, modifiers, F and OEM keys never -- as pass_key_allowed has it -- and the charms' full "
          "hold, in the notes");
    check(B::kKeysUpWaitMs == 600 && B::kPotionSettleMs == 500 &&
              has(doc, "chờ các phím đó nhả ra, nhiều nhất " + vi_s(B::kKeysUpWaitMs) + " giây") &&
              has(doc, "sau nửa giây kể từ lần bấm bình gần nhất") &&
              has(bag_cpp, "const char* const kKeysLateVi = \"một phím của macro chưa nhả kịp trước khi GoBot bấm chuột "
                           "hay bấm phím, nên dừng\";") &&
              has(doc, "\"một phím của macro chưa nhả kịp trước khi GoBot bấm chuột hay bấm phím, nên dừng\"") &&
              has(bag_hpp, "static constexpr int kShopFullLateMs = 1000;") &&
              has(doc, "đã chờ " + vi_s(1000) + " giây xem game có báo túi đầy không") &&
              has(gui, "constexpr int kShopOwnRelooks = 2;") && has(doc, "đo lại, nhiều nhất 2 lần") &&
              has(gui, "not_now(\"Chờ: GoBot vừa tự bấm phím hay chuột trong lúc chờ yên, mua bình sau.\", 15'000);") &&
              has(doc, "\"Chờ: GoBot vừa tự bấm phím hay chuột trong lúc chờ yên, mua bình sau.\" và thử lại sau 15 "
                       "giây") &&
              has(coord_cpp, "GB_INFO(\"Trash: %s's key window: %s%s\", job_,") &&
              has(coord_cpp, "\" through, \" + std::to_string(pass_openings_) +") &&
              has(coord_cpp, "(pass_openings_ == 1 ? \" time open (\" : \" times open (\")") &&
              has(gui, "const PassKeysScope keys(coord_, pass_keys, \"pass\");") &&
              has(gui, "const PassKeysScope keys(coord_, pass_keys, \"shop\");") &&
              has(doc, "\"Trash: pass's key window: Z 5, X 4, G 1 through, 3 times open (1840 ms)\"") &&
              has(doc, "\"Trash: shop's key window: ...\""),
          "the keys seen up within " + n(B::kKeysUpWaitMs) + " ms, a slot judged " + n(B::kPotionSettleMs) +
              " ms after a potion, the shop's second for the full message, GoBot's own input measured again twice "
              "then 15 s, the window's line -- as the cleaner, the worker and the coordinator have them");

    // ---- the purchases ----
    const std::string over = "\"Tổng N lần: một lượt giữ phím mua tối đa " + n(B::kShopBuysPerHold) +
                             " lần (\"Mua ngay\": " + n(B::kShopMaxBuys) + " lần), phần còn lại mua tiếp, mỗi " +
                             n(B::kShopPartGapMs / 60'000) + " phút một lượt, khi bot chạy, cho tới khi đủ số hoặc "
                             "game báo túi đồ đã đầy.\"";
    check(has(doc, over) && has(doc, "nay nhận từ 0 tới " + n(C::kBuySlotsPerRunMost)) &&
              has(doc, "\"Mua ngay\" mua tối đa " + n(B::kShopMaxBuys) + " lần trong một lượt") &&
              has(doc, "số \"mua tới\" cũ vẫn chỉ tới " + n(C::kBuySlotsFromOldMost)) &&
              has(pack, "Tu mua binh buys 0 to " + n(C::kBuySlotsPerRunMost) + " of each.") &&
              has(readme, "*mỗi lần mua [N] ô* from 0 to " + n(C::kBuySlotsPerRunMost) +
                              ", a hint when the two together pass `kShopMaxBuys` (" + n(B::kShopMaxBuys) + ")"),
          "the purchases 0 to " + n(C::kBuySlotsPerRunMost) + ", \"Mua ngay\" " + n(B::kShopMaxBuys) + ", the card's "
          "hint quoted as simple_shop.cpp builds it (test_buy_count_1110)");

    // ---- the death the screen alone read ----
    DeathByHp death;
    const bool at_once = death.step(true, 0, 100, 1'000);
    const bool short_of = death.step(true, 0, 100, 3'999);
    const bool after = death.step(true, 0, 100, 4'000);
    check(kLinkQuietMs == 5000 && kDeathQuietMostMs == 60'000 && !at_once && !short_of && after &&
              has(alerts, "log_later({ \"Alerts: HP reads 0/\" + std::to_string(v.hp_max) + \" on the screen -- not "
                          "taken for a death: \" +") &&
              !has(alerts, "GB_INFO(\"Alerts: HP reads") &&
              has(doc, "\"Alerts: HP reads 0/N on the screen -- not taken for a death: ...\"") &&
              has(readme, "`Alerts: HP reads 0/N on the screen -- not taken for a death: …`") &&
              has(doc, "im lặng quá " + n(kLinkQuietMs / 1000) + " giây (tới " + n(kDeathQuietMostMs / 1000) +
                           " giây") &&
              has(readme, "`kLinkQuietMs` (" + n(kLinkQuietMs / 1000) + " s) and up to `kDeathQuietMostMs` (" +
                              n(kDeathQuietMostMs / 1000) + " s)") &&
              has(doc, "Chết thật vẫn được báo, 3 giây sau khi không còn lý do nào ở trên.") &&
              has(readme, "a real death alerts 3 s after nothing doubts it") &&
              has(alerts_ini, "Từ bản 2026.11.10, HP đọc ra 0 trên màn hình không tính là chết khi game không ở trên "
                              "cùng"),
          "no death from the screen's HP 0 with the game behind, live packets above 0 or the server silent past " +
              n(kLinkQuietMs) + " ms up to " + n(kDeathQuietMostMs) + " ms; a real one after DeathByHp's 3 s");
    // The pet's plateau lines and the misread frames' lines, written off the
    // GUI thread as the doubt's is (log_later): no file on the GUI's.
    check(has(potions, "log_later({ text + \" -- no picture of the pet's slot to keep\" });") &&
              has(potions, "saved.push_back(it->get());") && has(potions, "log_later(std::move(saved));") &&
              !has(potions, "GB_INFO(\"%s\", it->get().c_str());") &&
              !has(potions, "GB_INFO(\"%s -- no picture of the pet's slot to keep\""),
          "simple_potions.cpp: the plateau's line and the saved frames' lines through log_later, never GB_INFO on the "
          "GUI thread");

    // ---- the pass's figures: the narrowing to Tiêu Dùng said with them ----
    check(has(readme, "With the zip's four tabs ticked and every item on the list a spend/ icon, so that only Tiêu "
                      "Dùng is looked through: a pass with two stacks 12.6 to 6.8 s, one with nothing to delete 7.4 "
                      "to 2.1 s.") &&
              has(readme, "all four tabs are still looked through: 12.6 to 12.0 s and 7.4 to 7.3 s") &&
              !has(readme, "a pass of the zip's four tabs with two spend stacks 12.6 to 6.8 s") &&
              has(doc, "khi mọi món trong danh sách đều là hình của game ở nhóm đồ tiêu dùng (GoBot chỉ tìm ở ngăn "
                       "Tiêu Dùng): một lượt có 2 chồng đồ khoảng 12,6 xuống 6,8 giây") &&
              has(doc, "thì GoBot vẫn tìm cả 4 ngăn: khoảng 12,6 xuống 12,0 giây và 7,4 xuống 7,3 giây") &&
              !has(doc, "một lượt với 4 ngăn của bản zip và 2 chồng đồ tiêu dùng: khoảng 12,6 xuống 6,8 giây"),
          "the pass's estimates: 6.8 s and 2.1 s only with a list of spend/ icons alone; with any other item, all "
          "four tabs, 12.0 s and 7.3 s");

    // ---- the comments beside, and what is no longer so ----
    check(has(trash_ini, "since 2026.11.10 those on a letter or Space -- skills, buffs and potions -- go on between "
                         "its own clicks") &&
              has(trash_ini, "since 2026.11.10 those on a letter or Space -- skills, buffs and potions -- go on between "
                             "purchases") &&
              has(goc, "since 2026.11.10 those on a letter or Space -- skills, potions and buffs -- go on between "
                       "purchases") &&
              !has(trash_ini, "Fighting, potions and buffs wait while it does.") &&
              !has(trash_ini, "While it runs nothing else of GoBot presses a key") &&
              !has(goc, "Everything else that presses keys -- skills, potions and buffs -- is held for the length of "
                        "the run") &&
              !has(pack, "presses it about every 3 seconds while the pet's HP") &&
              !has(doc, "bấm BẮT ĐẦU và xem GoBot bấm phím đó, cách nhau khoảng 3 giây.") &&
              has(doc, "ghi số thức ăn pet đang có và giờ") &&
              has(doc, "tự bấm Z, X, C, V và Space, mỗi phím 5 lần") &&
              has(doc, "ở \"Tìm trong:\" chỉ đánh dấu Tiêu Dùng") &&
              has(doc, "bấm \"Thử tìm (không xoá)\" và đọc danh sách sẽ bỏ") &&
              has(doc, "bấm \"Mua ngay\" một lần và ngồi xem"),
          "trash.ini and docs/goc-comparison.md: letters and Space between the pass's clicks and the purchases; the "
          "old \"every key waits\" and \"about every 3 seconds\" gone; step 13's pet, hand test, pass and purchase");
#else
    skip("no source folder\n");
#endif
}

// ------------------------------------------------ 2026.11.11: the notes

/// 2026.11.11's notes against the code: DOC-TRUOC.txt's entry and its live
/// test step 14, README.md's release section, START-HERE (package.ps1), the
/// comments of trash.ini and macro.ini, and docs/goc-comparison.md -- the
/// users' words and the owner's four answers as written, and the numbers and
/// the words of the card and the log they quote, as the code has them: the
/// shop right after the pass, HP and MP during the bag jobs, the digits kept
/// shut and the six keys let through.
void test_notes_1111() {
    section("The 2026.11.11 notes against the code: the shop after the pass, HP during the bag jobs, the six keys, "
            "the digits, the words quoted");
#ifdef GOBOT_SOURCE_DIR
    const std::string doc = flat(source("/DOC-TRUOC.txt"));
    const std::string readme = flat(source("/README.md"));
    const std::string pack = flat(source("/package.ps1"));
    const std::string trash_ini = flat(source("/configs/trash.ini"), true);
    const std::string macro_ini = flat(source("/configs/macro.ini"), true);
    const std::string goc = flat(source("/docs/goc-comparison.md"));
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string shop_card = source("/apps/gobot_gui/simple_shop.cpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    const std::string bag_cpp = source("/src/bag.cpp");
    const std::string coord_cpp = source("/src/execution_coordinator.cpp");
    if (doc.empty() || readme.empty() || pack.empty() || trash_ini.empty() || macro_ini.empty() || goc.empty() ||
        gui.empty() || shop_card.empty() || view.empty() || bag_cpp.empty() || coord_cpp.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto n = [](auto v) { return std::to_string(v); };
    // A time as the notes write it: 400 ms "0,4" in Vietnamese, "0.4" in
    // English; 15000 "15".
    const auto vi_s = [](std::uint64_t ms) {
        std::string s = std::to_string(ms / 1000);
        if (ms % 1000 != 0) s += "," + std::to_string(ms % 1000 / 100);
        return s;
    };
    const auto en_s = [](std::uint64_t ms) {
        std::string s = std::to_string(ms / 1000);
        if (ms % 1000 != 0) s += "." + std::to_string(ms % 1000 / 100);
        return s;
    };
    using B = BagCleaner;

    // ---- where: on top of 2026.11.10's entry; its live test step 14 ----
    const std::size_t sec5 = doc.find("5. CÓ GÌ MỚI, PHẦN NÀO LÀ THỬ NGHIỆM");
    const std::size_t e11 = doc.find("- Mới trong bản 2026.11.11 (mua bình ngay sau khi dọn rác, bình HP và MP được "
                                     "bấm trong lúc dọn rác và mua bình, và các phím Home, End, PgUp, PgDn, Insert, "
                                     "Delete, theo điều người dùng báo và câu trả lời của bạn ngày 11/11; chưa thử trên "
                                     "game thật):");
    const std::size_t e10 = doc.find("- Mới trong bản 2026.11.10 (");
    // 2026.11.11b's entry on top of it since, its step 15 after step 14.
    const std::size_t e11b = doc.find("- Mới trong bản 2026.11.11b (mua bình và dọn rác ở mọi mức máu, GoBot tự");
    const std::size_t step15 = doc.find("15. Mua bình và dọn rác ở mọi mức máu, GoBot tự bấm bình (bản 2026.11.11b,");
    const std::size_t try14 = doc.find("Thử như bước 14 ở mục 6.");
    const std::size_t step13 = doc.find("13. Bơm máu pet, về vùng đánh quái, dọn rác và mua bình (bản 2026.11.10,");
    const std::size_t step14 = doc.find("14. Mua bình ngay sau khi dọn rác, bình trong lúc mua, và sáu phím mới (bản "
                                        "2026.11.11, chưa thử trên game thật; chỉ thử khi ngồi xem, tay gần F12):");
    const std::size_t revive = doc.find("- Tự hồi sinh: thử ở bản đồ dễ");
    const std::size_t rkeys = readme.find("### The hold's key window (2026.11.10)");
    const std::size_t r11 =
        readme.find("### The shop after the pass, HP during the bag jobs, and six more keys (2026.11.11)");
    const std::size_t rmacro = readme.find("## Keyboard macro");
    check(sec5 != npos && e11b != npos && e11 != npos && e10 != npos && doc.find("- Mới trong bản", sec5) == e11b &&
              e11b < e11 && doc.find("- Mới trong bản", e11b + 1) == e11 && e11 < e10 &&
              try14 != npos && e11 < try14 && try14 < e10 && step13 != npos && step14 != npos && revive != npos &&
              step15 != npos && step13 < step14 && step14 < step15 && step15 < revive && rkeys != npos &&
              r11 != npos && rmacro != npos && rkeys < r11 &&
              r11 < rmacro && has(readme, "Since 2026.11.11 (not yet run on the game).") &&
              has(pack, "The shop right after the pass, potions during the bag jobs, and six more keys (2026.11.11, "
                        "not yet tried on the game)."),
          "DOC-TRUOC.txt: the 2026.11.11 entry first in section 5, on top of 2026.11.10's, not yet tried on the game, "
          "its live test step 14 after step 13; README's section after the key window's, and START-HERE, say it too");

    // ---- the users' words and the owner's answers, as written ----
    const std::string feedback =
        "- Sau khi dọn rác thì không lập tức mua bình ngay mà bị delay tới tận vài phút. Việc này dẫn tới khi bắt "
        "đầu thực hiện mua bình thì rác đã đầy túi đồ => Cần cho thực hiện mua bình ngay sau khi dọn rác xong. - Làm "
        "cho có thể bơm HP và MP khi đang dọn rác và mua bình. => Hiện tại khi HP thấp thì sẽ ưu tiên bơm xong mới "
        "mua bình dẫn tới việc mua bình bị delay. Cần sửa vấn đề này. Ngoài ra làm cho các phím như home, pgup, "
        "pgdown, end, delete có thể sử dụng được";
    const char* const answers[] = {
        "Tới giờ mua: dọn rồi mua",
        "Mua luôn, bơm xen",
        "Không cho phím số",
        "Home End PgUp PgDn Ins Del",
    };
    bool quoted = has(doc, "Người dùng báo (bạn chuyển lại ngày 11/11): \"" + feedback + "\"") &&
                  has(readme, "*\"" + feedback + "\"*") && has(doc, "Câu trả lời của bạn ngày 11/11:");
    for (const char* a : answers)
        quoted = quoted && has(doc, "\"" + std::string(a) + "\"") && has(readme, "*\"" + std::string(a) + "\"*");
    check(quoted &&
              has(doc, "1. Mua bình sau khi dọn rác: \"Tới giờ mua: dọn rồi mua\" (\"Giữ ô mua mỗi N phút. Tới giờ "
                       "mua, GoBot dọn rác trước (nếu Dọn rác bật) rồi mua liền trong cùng một lần dừng phím, không chờ "
                       "thêm. Các lần dọn khác vẫn chạy theo giờ dọn.\").") &&
              has(doc, "2. HP và MP trong lúc dọn rác và mua bình: \"Mua luôn, bơm xen\" (\"Không chờ máu đầy. Bình "
                       "HP/MP bấm xen giữa các lần mua và giữa các món dọn rác. Chỉ dừng lượt mua khi máu xuống dưới "
                       "nửa mức bình (sắp chết), rồi tới giờ sau mua tiếp.\").") &&
              has(doc, "3. Phím số trong lúc dọn rác và mua bình: \"Không cho phím số\".") &&
              has(doc, "4. Các phím mới: \"Home End PgUp PgDn Ins Del\"."),
          "the users' words of 2026-11-11 and the owner's four answers, with the two options' words, quoted as "
          "written in DOC-TRUOC.txt and README");

    // ---- the shop right after the pass: a minute fresh, the card's words ----
    const std::uint64_t fresh = bag::kRideFreshMs;
    const std::uint64_t t0 = 10'000'000;
    using R = bag::ShopRide;
    const bool fresh_alone = bag::ride_turn(R::None, true, true, false, t0, t0 - fresh + 1) == R::Alone;
    const bool stale_pulls = bag::ride_turn(R::None, true, true, false, t0, t0 - fresh) == R::Pending;
    const bool none_pulls = bag::ride_turn(R::None, true, true, false, t0, 0) == R::Pending;
    const bool trash_off = bag::ride_turn(R::None, true, false, false, t0, 0) == R::None;
    const std::string mins = n(fresh / 60'000);
    const std::string ride_hint = "Dọn rác đang bật: tới giờ mua, GoBot dọn rác trước rồi mua bình liền sau đó, trong "
                                  "cùng một lần dừng phím, không chờ thêm. Vừa dọn xong chưa tới " + mins +
                                  " phút thì mua luôn, không dọn lại.";
    check(fresh == 60'000 && fresh_alone && stale_pulls && none_pulls && trash_off &&
              has(shop_card, "\"Dọn rác đang bật: tới giờ mua, GoBot dọn rác trước rồi mua bình liền sau đó, trong "
                             "cùng một lần dừng \"") &&
              has(shop_card, "\"phím, không chờ thêm. Vừa dọn xong chưa tới \" + std::to_string(bag::kRideFreshMs / "
                             "60'000) +") &&
              has(shop_card, "\" phút thì mua luôn, không dọn lại.\";") &&
              has(shop_card, "if (simple_clean_.buy_potions && simple_clean_.enabled) {") &&
              has(doc, "thẻ Tự mua bình ghi \"" + ride_hint + "\"") && has(readme, "*" + ride_hint + "*") &&
              has(doc, "Vừa dọn xong chưa tới " + mins + " phút thì không dọn lại") &&
              has(readme, "`bag::kRideFreshMs` (" + n(fresh / 1000) + " s)") &&
              has(pack, "after a pass that ended less than " + mins + " minute before, none is made again") &&
              has(trash_ini, "a pass ended less than a minute before is not made again."),
          "a pass ended less than " + n(fresh) + " ms ago pulls none, an older one or none yet pulls one, Dọn rác "
          "off never -- as bag::ride_turn has it -- and the card's hint quoted as simple_shop.cpp builds it");
    check(has(gui, "ride = bag::ride_turn(was, shop_due, settings.enabled, cannot_pull, now_ms(), last_pass_end_ms);") &&
              in_order(gui, { "const bool pulled = ride == bag::ShopRide::Pending && request == 0;",
                              "if (rides) {", "run_shop(false, false, true);" }) &&
              count_in(gui, "run_shop(false, false, true);") == 1 &&
              has(gui, "run_shop(request == 4, request == 5, false);") &&
              B::kShopBuysPerHold == 10 && B::kShopPartGapMs == 60'000 && B::kShopMaxBuys == 40 &&
              has(doc, "mỗi lượt theo giờ mua tối đa " + n(B::kShopBuysPerHold) + " lần và phần còn lại sau " +
                           n(B::kShopPartGapMs / 60'000) + " phút, \"Mua ngay\" tối đa " + n(B::kShopMaxBuys) +
                           " lần, túi đầy thì dừng lượt đó.") &&
              has(readme, "`kShopBuysPerHold` (" + n(B::kShopBuysPerHold) + ") a hold and the rest `kShopPartGapMs` (" +
                              n(B::kShopPartGapMs / 1000) + " s) on, **Mua ngay**'s `kShopMaxBuys` (" +
                              n(B::kShopMaxBuys) + ")") &&
              has(doc, "\"Dọn ngay\" không bao giờ mua bình.") && has(readme, "a **Dọn ngay** never buys.") &&
              has(pack, "Don ngay never buys.") &&
              has(doc, "riêng lượt mua đi liền sau lượt dọn thì đi trước Dùng bùa (bùa tới giờ cùng lúc thì dùng sau "
                       "lượt mua).") &&
              has(readme, "A ridden round comes before a charm run due at the same moment") &&
              has(readme, "a potion round that rides the pass comes right after it, before the charms."),
          "one round that rides the pass, after it, never for a button; the plan's rules kept -- " +
              n(B::kShopBuysPerHold) + " a hold, the rest " + n(B::kShopPartGapMs) + " ms on, Mua ngay " +
              n(B::kShopMaxBuys) + " -- and the charms after it, as the notes say");

    // ---- the ride's log ----
    const std::string due_line =
        "Trash: shop due -- a pass first, then the round under the same hold (the last pass ended N s ago)";
    const std::string rides_line = "Trash: shop rides the pass -- the same hold, no quiet second (the look that "
                                   "pulled it)";
    const std::string no_ride = "Trash: shop -- no ride on the pass it pulled (";
    check(has(gui, "GB_INFO(\"Trash: shop due -- a pass first, then the round under the same hold (the last pass \"") &&
              has(gui, "\"ended %s)\", since_pass().c_str());") &&
              has(gui, ": std::to_string((now_ms() - last_pass_end_ms) / 1000) + \" s ago\";") &&
              has(gui, "GB_INFO(\"Trash: shop rides the pass -- the same hold, no quiet second (%s)\",") &&
              has(gui, "pulled ? \"the look that pulled it\" : \"come due while it ran\");") &&
              has(gui, "GB_INFO(\"Trash: shop -- no ride on the pass it pulled (%s): it goes on its own\", no_ride);") &&
              has(gui, "\"%d unsure, full %s; %s, the last pass ended %s\",") &&
              has(gui, "after_pass ? \"right after the pass\" : forced ? \"Mua ngay\" : dry ? \"Thử mua\" : \"on its "
                       "own\",") &&
              has(doc, "\"" + due_line + "\"") && has(readme, "`" + due_line + "`") &&
              has(doc, "\"" + rides_line + "\" (hay \"(come due while it ran)\")") &&
              has(readme, "`" + rides_line + "` or `(come due while it ran)`") &&
              has(doc, "\"" + no_ride + "...): it goes on its own\"") &&
              has(readme, "`" + no_ride + "…): it goes on its own`") &&
              has(doc, "\"; right after the pass, the last pass ended N s ago\" (hay \"on its own\", \"Mua ngay\", "
                       "\"Thử mua\")") &&
              has(readme, "`; right after the pass, the last pass ended N s ago` (or `on its own`, `Mua ngay`, `Thử "
                          "mua`)"),
          "the pull, the ride, no ride and the shop job's end -- \"" + due_line + "\" -- as the worker writes them");

    // ---- HP and the bag jobs: the gates, the waits, the example ----
    // 2026.11.11's numbers, as its entries tell them -- history since
    // 2026.11.11b, which runs the bag jobs at any HP (bag::kJobDownHpPct).
    const bag::HpGates drinks{ true, 80, 40 };
    const bag::HpGates waits{ false, 90, 80 };
    const bag::HpGates now_drinks = bag::hp_gates(true, 80, 80);
    const bag::HpGates now_waits = bag::hp_gates(false, 80, 80);
    const std::string look = n(bag::kHpLookAgainMs / 1000), again = n(bag::kHpStoppedAgainMs / 1000);
    check(now_drinks.drinks && now_drinks.start == bag::kJobDownHpPct && now_drinks.stop == bag::kJobDownHpPct &&
              !now_waits.drinks && now_waits.start == bag::kJobDownHpPct && now_waits.stop == bag::kJobDownHpPct &&
              bag::kHpLookAgainMs == 5'000 && bag::kHpStoppedAgainMs == 15'000 &&
              has(gui, "return bag::hp_gates(keys.hp_all_through, keys.hp_mark, chat_hp_floor_.load());") &&
              has(doc, "Ví dụ bình HP 80%: trước đây bắt đầu từ " + n(waits.start) + "% và dừng dưới " +
                           n(waits.stop) + "%; nay bắt đầu từ " + n(drinks.start) + "% và chỉ dừng dưới " +
                           n(drinks.stop) + "%.") &&
              has(readme, "a mark of 80 started at " + n(waits.start) + " and stopped under " + n(waits.stop) +
                              ", and now starts at " + n(drinks.start) + " and stops under " + n(drinks.stop)) &&
              has(pack, "(80 gives " + n(drinks.stop) + ")") &&
              has(doc, "không cộng 10% nữa; chưa tới thì " + look + " giây sau xem lại;") &&
              has(doc, "chỉ khác là xem lại sau " + look + " giây thay cho 15 giây") &&
              has(doc, "phím cho bình; " + again + " giây sau thử lại (lượt dọn cũng vậy, không chờ cả khoảng \"dọn "
                       "mỗi\").") &&
              has(readme, "`bag::kHpLookAgainMs` (" + look + " s)") &&
              has(readme, "`bag::kHpStoppedAgainMs` (" + again + " s)") &&
              has(pack, "tried again " + again + " s later and never counted as trouble") &&
              has(trash_ini, "tried again " + again + " s on, never as trouble.") &&
              has(shop_card, "std::to_string(bag::kHpStoppedAgainMs / 1000) +"),
          "2026.11.11's entries: the HP potion drunk through the job's window started at its mark, stopped under "
          "half of it (80: " + n(drinks.start) + " and " + n(drinks.stop) + ", where it was " + n(waits.start) +
              " and " + n(waits.stop) + "), " + n(bag::kHpLookAgainMs) + " ms to look again, " +
              n(bag::kHpStoppedAgainMs) + " ms after a stop; since 2026.11.11b at any HP, only the character down "
              "under " + n(bag::kJobDownHpPct) + "%");
    bag::ShopRoundSeen hp_seen;
    hp_seen.hp_cut = true;
    hp_seen.trouble = true;
    bag::ShopRoundSeen late_seen;
    late_seen.keys_late = true;
    late_seen.trouble = true;
    check(bag::shop_end(hp_seen) == bag::ShopEnd::Paused && bag::shop_end(late_seen) == bag::ShopEnd::Paused &&
              has(doc, "Lượt mua bị dừng vì máu không tính là lỗi, phần còn lại vẫn mua.") &&
              has(doc, "nay không tính là lỗi: phần còn lại mua sau " + n(B::kShopPartGapMs / 60'000) + " phút.") &&
              has(bag_cpp, "const char* const kKeysLateVi = \"một phím của macro chưa nhả kịp trước khi GoBot bấm chuột "
                           "hay bấm phím, nên dừng\";") &&
              has(doc, "(\"một phím của macro chưa nhả kịp trước khi GoBot bấm chuột hay bấm phím, nên dừng\")") &&
              has(readme, "a key of the macro's not seen up in time (`Result::keys_late`) is Paused too") &&
              has(doc, "Thông báo túi đầy nay được đóng cả khi máu thấp"),
          "a round stopped for HP, or by a key not up in time, is put off, never trouble -- as bag::shop_end has it");

    // ---- the potion breaks ----
    char breaks[160];
    std::snprintf(breaks, sizeof breaks, "Trash: potion break %s -- the key window open %s ms for an HP or MP potion "
                  "due%s", "before the double-click", "N", "");
    check(B::kPotionBreakMs == 400 &&
              has(bag_cpp, "GB_INFO(\"Trash: potion break %s -- the key window open %d ms for an HP or MP potion due%s\", "
                           "where, waited,") &&
              has(bag_cpp, "waited >= kPotionBreakMs ? \", one still due\" : \"\");") &&
              has(bag_cpp, "potion_break(f0, BagView{}, \"before the double-click\", nullptr, -1, true);") &&
              has(bag_cpp, "potion_break(g, v, \"as the shop is seen\", nullptr, -1, false, true);") &&
              has(bag_cpp, "potion_break(f, v, \"between two items\")") &&
              has(doc, "\"" + std::string(breaks) + "\" (hay \"as the shop is seen\", \"between two items\"; thêm \", "
                       "one still due\" khi đã hết " + vi_s(B::kPotionBreakMs) + " giây mà bình vẫn chờ)") &&
              has(readme, "`" + std::string(breaks) + "` (or `as the shop is seen`, `between two items`; `, one still "
                          "due` after the whole " + n(B::kPotionBreakMs) + " ms)") &&
              has(doc, "GoBot mở lối phím cho bình tới " + vi_s(B::kPotionBreakMs) + " giây ngay trước mỗi lần bấm đúp") &&
              has(readme, "`BagCleaner::kPotionBreakMs` (" + n(B::kPotionBreakMs) + " ms)") &&
              has(pack, "(up to " + en_s(B::kPotionBreakMs) + " s each time)") &&
              has(trash_ini, "is given up to " + en_s(B::kPotionBreakMs) + " s between two items") &&
              has(trash_ini, "is given up to " + en_s(B::kPotionBreakMs) + " s before each double-click") &&
              has(goc, "is given up to " + en_s(B::kPotionBreakMs) + " s between two items") &&
              has(goc, "is given up to " + en_s(B::kPotionBreakMs) + " s before each double-click"),
          "a potion break of " + n(B::kPotionBreakMs) + " ms at most, at its three places, and its line -- \"" +
              std::string(breaks) + "\" -- as the cleaner has them");

    // ---- the stops and the put-offs, said ----
    char hp_said[96], put_off[160], stopped[160];
    std::snprintf(hp_said, sizeof hp_said, "HP %.0f%% under %s %d%%", 62.0, "the HP potion's mark", drinks.start);
    std::snprintf(put_off, sizeof put_off, "Trash: timed %s put off -- %s", "shop", hp_said);
    std::snprintf(hp_said, sizeof hp_said, "HP %.0f%% under %s %d%%", 38.0, "the level", drinks.stop);
    std::snprintf(stopped, sizeof stopped, "Trash: %s stopped by HP -- %s", "pass", hp_said);
    check(has(gui, "GB_INFO(\"Trash: timed %s put off -- %s\", job, said.c_str());") &&
              has(gui, "std::snprintf(buf, sizeof buf, \"HP %.0f%% under %s %d%%\", static_cast<double>(hp), what, "
                       "level);") &&
              has(gui, "hp_said(hp, hp_gate.start, hp_gate.drinks ? \"the HP potion's mark\" : \"the start gate\")") &&
              has(gui, "GB_INFO(\"Trash: %s stopped by HP -- %s\", job, hp_said(seen.hp, seen.level, \"the level\")"
                       ".c_str());") &&
              has(gui, "GB_INFO(\"Trash: %s stopped by the hold -- %s's now; HP %.0f%%, the level %d%%\", job, "
                       "whose.c_str(),") &&
              has(gui, "say_stopped(\"pass\", hp_cut, r.trouble);") &&
              has(gui, "say_stopped(\"shop round\", hp_cut, r.trouble);") &&
              has(gui, "\"someone is using the mouse or keyboard (the quiet second)\"") &&
              has(gui, "\"GoBot's own input in each quiet second\"") &&
              has(gui, "\"the check-in board is up\"") &&
              has(doc, "\"" + std::string(put_off) + "\"") && has(readme, "`" + std::string(put_off) + "`") &&
              has(doc, "\"" + std::string(stopped) + "\"") && has(readme, "`" + std::string(stopped) + "`") &&
              has(doc, "\"Trash: shop round stopped by the hold -- ...'s now; HP N%, the level N%\"") &&
              has(readme, "`Trash: shop round stopped by the hold -- …'s now; HP N%, the level N%`") &&
              has(doc, "\"someone is using the mouse or keyboard (the quiet second)\", \"GoBot's own input in each "
                       "quiet second\", \"the check-in board is up\"") &&
              has(doc, "\"Trash: shop round stopped by HP -- HP N% under the level N%\""),
          "the put-off's \"" + std::string(put_off) + "\" and the stop's \"" + std::string(stopped) + "\" -- as the "
          "worker writes them");

    // ---- the six keys, exactly; the digits, the number pad and F shut ----
    const std::uint32_t six[] = { VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_INSERT, VK_DELETE };
    const char* const named[] = { "Home", "End", "PgUp", "PgDn", "Insert", "Delete" };
    bool six_through = true;
    for (std::size_t i = 0; i < 6; ++i)
        six_through = six_through && pass_key_allowed(six[i]) && pass_key_nav(six[i]) &&
                      InputManager::key_name(six[i]) == named[i] && InputManager::parse_key(named[i]) == six[i];
    int admitted = 0;
    bool shut = true;
    for (std::uint32_t vk = 0; vk < 256; ++vk) {
        if (pass_key_allowed(vk)) ++admitted;
        const bool digit = vk >= '0' && vk <= '9';
        const bool pad = vk >= VK_NUMPAD0 && vk <= VK_DIVIDE;
        const bool fkey = vk >= VK_F1 && vk <= VK_F24;
        const bool between = vk >= VK_LEFT && vk <= VK_SNAPSHOT;
        if ((digit || pad || fkey || between) && pass_key_allowed(vk)) shut = false;
    }
    check(six_through && admitted == 26 + 1 + 6 && shut && InputManager::parse_key("pgup") == VK_PRIOR &&
              InputManager::parse_key("pgdn") == VK_NEXT && InputManager::parse_key("del") == VK_DELETE &&
              InputManager::parse_key("ins") == VK_INSERT &&
              has(coord_cpp, "keys += (keys.empty() ? \"\" : \", \") + InputManager::key_name(t.first) + \" \" + "
                             "std::to_string(t.second);") &&
              has(doc, "\"Trash: pass's key window: Z 1, Home 2, PgDn 1, Delete 1 through, ...\"") &&
              has(readme, "`Trash: pass's key window: Z 1, Home 2, PgDn 1, Delete 1 through, …`") &&
              has(doc, "gõ pgup, pgdn, del hay ins vào ô phím nay là PgUp, PgDn, Delete, Insert (trước đây thành phím "
                       "P, P, D, I)") &&
              has(readme, "`parse_key` also takes `pgup`, `pgdn`, `del` and `ins`, which set P, P, D and I before") &&
              has(readme, "before 2026.11.11 `pgup`, `pgdn`, `del` and `ins` set P, P, D and I.") &&
              has(readme, "exactly VK 0x21, 0x22, 0x23, 0x24, 0x2D and 0x2E go through the key window") &&
              has(doc, "Chỉ đúng sáu phím đó: các phím mũi tên (cả ↓) và các phím khác vẫn không.") &&
              has(macro_ini, "since 2026.11.11 those on PgUp " + n(VK_PRIOR) + ", PgDn " + n(VK_NEXT) + ", End " +
                                 n(VK_END) + ", Home " + n(VK_HOME) + ", Insert " + n(VK_INSERT) + " and Delete " +
                                 n(VK_DELETE) + " too; a key on a digit, the number pad or an F-key waits for the "
                                 "job's end.") &&
              has(doc, "phím 0–9 không bao giờ được bấm trong lúc dọn rác và mua bình (gõ vào hộp số lượng sẽ thành "
                       "số), cả phím số bên phải bàn phím và phím F."),
          "Home, End, PgUp, PgDn, Insert and Delete through, " + n(admitted) + " keys in all, by those names and "
          "typed short; digits, the number pad, F-keys and the arrows never -- as pass_key_allowed, key_name and "
          "parse_key have them");

    // ---- the six keys' settle, the shop's row, the trouble said ----
    const std::string row_line = "Trash: shop row not matched before the double-click on Thịt bò khô -- its buttons "
                                 "alone at x,y (row 0.00, buttons 0.97), look 1 of " + n(B::kShopRowRelooks + 1);
    const std::string row_trouble = "không thấy hàng Thịt bò khô và Rượu lá tre trong cửa hàng trước khi chọn Thịt bò "
                                    "khô (danh sách có thể đã bị cuộn), nên không bấm";
    check(B::kNavSettleMs == 200 && B::kShopRowRelooks == 3 &&
              has(bag_cpp, "if (ago >= 0 && ago < kNavSettleMs) pause_ms(kNavSettleMs - ago);") &&
              has(bag_cpp, "if (s.row_score > 0.0f) return -1;") &&
              has(bag_cpp, "GB_INFO(\"Trash: shop row not matched before the double-click on %s -- its buttons alone "
                           "at %d,%d (row \"") &&
              has(bag_cpp, "\"%.2f, buttons %.2f), look %d of %d\",") &&
              has(bag_cpp, "static_cast<double>(s.buttons_score), again + 1, kShopRowRelooks + 1);") &&
              has(bag_cpp, "const std::string name = hp ? \"Thịt bò khô\" : \"Rượu lá tre\";") &&
              has(bag_cpp, "res.trouble = \"không thấy hàng Thịt bò khô và Rượu lá tre trong cửa hàng trước khi chọn "
                           "\" + name +") &&
              has(bag_cpp, "\" (danh sách có thể đã bị cuộn), nên không bấm\";") &&
              has(bag_cpp, "keep(f0, \"shop_row_unseen\");") &&
              has(doc, "GoBot chờ " + vi_s(B::kNavSettleMs) + " giây rồi mới tự bấm chuột, bấm phím, hay nhìn màn hình "
                       "để quyết định.") &&
              has(doc, "không thấy thì nhìn lại tối đa " + n(B::kShopRowRelooks) + " lần nữa, rồi dừng lượt mua, không "
                       "bấm gì.") &&
              has(doc, "\"" + row_trouble + "\"") && has(doc, "ảnh giữ là shop_row_unseen trong trash_debug") &&
              has(doc, "\"" + row_line + "\"") && has(readme, "`" + row_line + "`") &&
              has(readme, "`BagCleaner::kNavSettleMs` (" + n(B::kNavSettleMs) + " ms)") &&
              has(readme, "`BagCleaner::kShopRowRelooks` (" + n(B::kShopRowRelooks) + ") times") &&
              has(pack, "GoBot waits " + en_s(B::kNavSettleMs) + " s after one, and double-clicks a potion only when "
                        "it sees the potions' row itself.") &&
              has(doc, "các ảnh trash_debug\\*shop_row_unseen* nếu có."),
          "a settle of " + n(B::kNavSettleMs) + " ms after one of the six, the shop's row looked for " +
              n(B::kShopRowRelooks) + " times more, the trouble and the line -- as the cleaner has them");

    // ---- the simple screen: Delete a key, Backspace to clear ----
    check(has(view, "{ ImGuiKey_Delete, VK_DELETE },") &&
              has(view, "(can_clear ? \"Nhấn phím muốn dùng. Esc để huỷ, Backspace để bỏ phím này.\"") &&
              has(view, "help += \"\\nMuốn bỏ phím này: bấm vào ô rồi nhấn Backspace.\";") &&
              !has(view, "Delete để bỏ phím này") &&
              has(doc, "\"Nhấn phím muốn dùng. Esc để huỷ, Backspace để bỏ phím này.\"") &&
              has(readme, "*Nhấn phím muốn dùng. Esc để huỷ, Backspace để bỏ phím này.*") &&
              has(doc, "muốn bỏ phím của một ô thì bấm vào ô rồi nhấn Backspace (trước là Delete)") &&
              has(pack, "On the simple screen Delete is now a key like any other, and Backspace clears a key box."),
          "Delete a key on the simple screen, Backspace clearing a box -- its help quoted as simple_view.cpp has it");

    // ---- the digits: the zip's potions on 1 and 2, as the screen fills in ----
    MacroConfig ships;
    const bool shipped = ships.load(std::string(GOBOT_SOURCE_DIR) + "/configs/macro.ini");
    std::uint32_t hp_vk = 0, mp_vk = 0;
    for (const MacroKey& k : ships.keys) {
        if (hp_vk == 0 && macro_key_kind(k) == MacroKeyKind::HpPotion) hp_vk = k.keycode;
        if (mp_vk == 0 && macro_key_kind(k) == MacroKeyKind::MpPotion) mp_vk = k.keycode;
    }
    check(shipped && hp_vk == '1' && mp_vk == '2' && !pass_key_allowed(hp_vk) && !pass_key_allowed(mp_vk) &&
              !bag::hp_gates(pass_key_allowed(hp_vk), 50, 50).drinks &&
              has(view, "{ MacroKeyKind::HpPotion, MacroTrigger::HpBelow, '1', 50, \"HP potion\" },") &&
              has(view, "{ MacroKeyKind::MpPotion, MacroTrigger::MpBelow, '2', 30, \"MP potion\" },") &&
              has(doc, "mà màn hình đơn giản tự điền sẵn phím 1 và 2 cho hai dòng bình (macro.ini của bản zip cũng để "
                       "1 và 2).") &&
              has(doc, "hãy nhắc người dùng đổi bình sang phím chữ cái hay một trong sáu phím trên.") &&
              has(doc, "Bình HP, MP ở phím số (macro.ini của bản zip và màn hình đơn giản để sẵn phím 1 và 2) vẫn chờ "
                       "trong lúc dọn rác và mua bình") &&
              has(readme, "the simple screen fills in 1 and 2, and so does the zip's `macro.ini` -- still waits through "
                          "a pass and a round") &&
              has(pack, "the simple screen fills in 1 and 2 for the HP and MP potions (so does the zip's macro.ini), "
                        "so tell testers to move them to a letter or one of the six") &&
              has(pack, "An HP or MP potion on a digit (the zip's, 1 and 2) still waits through a bag pass and a "
                        "purchase round in 2026.11.11") &&
              has(macro_ini, "ship on " + n(hp_vk) + " and " + n(mp_vk) + " (1 and 2), as the simple screen fills "
                             "them in: move them to a letter or one of the six") &&
              has(macro_ini, "(VI) Trong lúc dọn rác và mua bình chỉ phím chữ cái, Space, Home, End, PgUp, PgDn, "
                             "Insert, Delete được bấm; phím số chờ xong việc."),
          "the zip's HP and MP potions on 1 and 2, as the simple screen fills them in, wait through the bag jobs "
          "(the owner's \"Không cho phím số\") -- said in the entry, its update notes, README, START-HERE and "
          "macro.ini");

    // ---- the comments beside, and the step's words ----
    check(has(trash_ini, "Since 2026.11.11 those on Home, End, PgUp, PgDn, Insert and Delete go on too (never a "
                         "digit)") &&
              has(trash_ini, "With buy_potions below on too, a pass is also made the moment the shop comes due "
                             "(2026.11.11), and the next comes interval_minutes after that one.") &&
              has(trash_ini, "With enabled=true above, a timed round comes right after a pass: when it is due GoBot "
                             "cleans the bag first and buys at once, in the same stop of the keys, with no quiet "
                             "second;") &&
              has(trash_ini, "Since 2026.11.11b a round runs at any HP: only the character down holds it back or stops "
                             "it, tried again 15 s on, never as trouble.") &&
              has(trash_ini, "a potion on a digit is never pressed during a round, so move it to one of those keys.") &&
              !has(trash_ini, "It does not run with HP near the HP potion's mark (never counted over 95%), nor with") &&
              has(trash_ini, "Từ bản 2026.11.11: Dọn rác bật thì tới giờ mua GoBot dọn rác trước rồi mua liền sau đó. "
                             "Từ bản 2026.11.11b: mua và dọn ở mọi mức máu") &&
              has(trash_ini, "since 2026.11.10 those on a letter or Space -- skills, buffs and potions -- go on between "
                             "purchases") &&
              has(goc, "Since 2026.11.11, with **Dọn rác** on, a timed round comes right after a bag pass, made the "
                       "moment the round is due, under the same hold and with no quiet second.") &&
              has(goc, "It does not start with HP near the potion's mark -- since 2026.11.11 only with the HP potion "
                       "on a digit") &&
              has(goc, "since 2026.11.10 those on a letter or Space -- skills, potions and buffs -- go on between "
                       "purchases") &&
              // Step 14a sets both clocks (the review's DOC-RIDE-STEP14A): with
              // "dọn mỗi" 10 the plan 15 minutes on comes 5 minutes after a
              // pass, and pulls one; 5 or 15 would have it go on its own.
              has(doc, "đặt \"dọn mỗi\" 10 phút và \"mua mỗi\" 15 phút, đặt 1 ô HP và 1 ô MP, rồi mới đánh dấu Tự "
                       "mua bình") &&
              has(doc, "Với \"dọn mỗi\" 5 hay 15 phút thì lần mua rơi vào trong 1 phút sau lượt dọn của nó và đi "
                       "riêng, log ghi \"on its own, the last pass ended N s ago\": đó là đúng, không phải lỗi.") &&
              has(view, "static const int kMinutes[] = { 5, 10, 15, 30, 60 };") &&
              has(shop_card, "static const int kMinutes[] = { 15, 30, 60, 90, 120, 180 };") &&
              has(doc, "và khoảng " + again + " giây sau (máu đã lên tới mức bình) GoBot mua tiếp phần còn lại;") &&
              has(doc, "lúc túi đồ đang mở và lúc cửa hàng đang mở") &&
              has(doc, "Rồi làm lại với End, PgUp, PgDn, Insert và Delete.") &&
              has(doc, "Chưa biết sáu phím mới làm gì trong game khi túi đồ hay cửa hàng đang mở.") &&
              has(readme, "Still not known: what the six keys do in the game with the bag or the shop open"),
          "trash.ini, macro.ini and docs/goc-comparison.md say the ride, the HP gates and the six keys, 2026.11.10's "
          "words kept; step 14's buy every 15 minutes (a choice on the card), its HP stop and the six keys by hand");

    // ---- the review's fixes, said where the ride and the HP gates are ----
    // A round due within the fresh minute of its pass goes on its own -- every
    // timed one with the shipped 10 and 60 -- and a pass is pulled only longer
    // after one (DOC-RIDE-STEP14A); none where the shop would wait, nor again
    // after one with nothing to do (R1, R2); HP unread waits a ridden round
    // too (R3); the H gates only with every HP line from half the mark up let
    // through (HP-1); a lone message closed with HP low (HP-2).
    check(has(doc, "lần xem mua đầu " + n(bag::kShopFirstWaitS) + " giây sau, rồi mỗi đồng hồ tính từ lượt của chính "
                   "nó. Nên khi \"mua mỗi\" chia hết cho \"dọn mỗi\" (như 10 và 60 của bản zip, hay 5 hoặc 15 với "
                   "15), mỗi lần mua theo giờ thường tới trong vòng " + mins + " phút sau một lượt dọn theo giờ") &&
              has(readme, "so when `buy_every_minutes` is a multiple of `interval_minutes` -- the shipped 10 and 60, or "
                          "5 or 15 with 15 -- each timed round usually comes within that minute of a timed pass and is "
                          "bought on its own right after it.") &&
              has(doc, "GoBot cũng không kéo lượt dọn lên khi chính lượt mua sẽ phải chờ: chưa thấy cửa sổ game hay "
                       "game nằm ngoài màn hình, chưa đọc được máu") &&
              has(doc, "Lượt mua liền sau lượt dọn vẫn chờ khi chưa đọc được máu (thử lại sau 15 giây).") &&
              has(readme, "nor for an interval after a pulled pass found an empty list or no tab ticked "
                          "(`no_pull_before_ms`)") &&
              has(readme, "a ridden round still waits while HP is not read.") &&
              has(trash_ini, "nor again before interval_minutes after one with nothing to do.") &&
              has(pack, "No pass is made for the shop while the game is not in the world (HP not read) or not found.") &&
              has(gui, "if (hp < 0.0f) return not_now(\"Chưa đọc được máu của nhân vật, mua bình sau.\", 15'000);\n"
                       "        if (!after_pass) {") &&
              has(doc, "và có hai dòng bình HP (Lv1, Lv2) thì mọi dòng có mức từ nửa mức cao nhất trở lên đều ở phím "
                       "như vậy") &&
              has(readme, "`PassKeys::hp_all_through`") &&
              has(pack, "With two HP potion lines (Lv1, Lv2), each one marked at half the highest or over must be on "
                        "such a key too, or the job goes as before.") &&
              has(trash_ini, "Since 2026.11.11b a pass runs at any HP -- only the character down holds it back or "
                             "stops it, tried again 15 s on --") &&
              has(doc, "Một thông báo lạ hiện ra thay cho hộp số lượng cũng vậy.") &&
              has(readme, "The bag-full message, or any lone message in the quantity box's place, is closed whatever "
                          "HP is (`set_hold_check`: a stop or the hold lost only).") &&
              has(bag_cpp, "(mode_ == Mode::ShopBuy && hold_check_ ? !hold_check_() : guard_ && !guard_());") &&
              has(goc, "A pass that ended under a minute before is not made again: the round then goes on its own right "
                       "after it, as with Dọn rác every 10 minutes and buying every 60."),
          "the review's fixes said: a round within the fresh minute of its pass goes on its own -- the shipped 10 and "
          "60 every time -- none pulled where the shop would wait or after one with nothing to do, HP unread waits a "
          "ridden round, the H gates only with every HP line from half the mark up let through, a lone message closed "
          "with HP low");
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
    test_buy_count_1110();
    test_notes_1110();
    test_notes_1111();
}
