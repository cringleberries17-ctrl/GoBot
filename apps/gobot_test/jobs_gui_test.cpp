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
// shop right after the pass, HP during the bag jobs, the six keys, the digits;
// and 2026.11.12's -- the turns, the holds, the dash, the area, the way back,
// the pit, the two bars and the bag jobs counted from the start.
//
// Nothing here goes near the desktop, the pointer, the keyboard's state or the
// game. The glances at the pointer are a pretend desktop's (InputGlancer's
// parts set by the test), the quest's rounds are made up, the macro runs a step
// at a time on a clock of the test's own with a pretend keyboard -- or once on
// its own threads, the keyboard still pretend -- and where only GoBot's window
// does a thing, its source is read.

#include "gobot/alerts.hpp"
#include "gobot/auto_chat.hpp"
#include "gobot/bag.hpp"
#include "gobot/cast_match.hpp"
#include "gobot/checkin_board.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/crash_watch.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/hunter.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/machine_info.hpp"
#include "gobot/macro.hpp"
#include "gobot/owed_start.hpp"
#include "gobot/packet_eyes.hpp"
#include "gobot/packet_world.hpp"
#include "gobot/party_invite.hpp"
#include "gobot/popup_closer.hpp"
#include "gobot/quick_quest.hpp"
#include "gobot/quiet_second.hpp"
#include "gobot/react_wait.hpp"
#include "gobot/reviver.hpp"
#include "gobot/vitals_reader.hpp"
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
          "none kept: the charms 8 s on and the shop 45 s on, as every start had them (the shop until 2026.11.12: "
          "a whole interval since, bag::start_due_ms)");
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
    section("\"Ra chiêu theo vòng\": its status in a few words, on as shipped since 2026.11.16b (W1's switch)");

    MacroConfig shipped_default;
    shipped_default.apply_defaults();
    check(MacroConfig{}.cast_paced && shipped_default.cast_paced, "on in a config built in code (2026.11.16b)");

    MacroEngine::PaceStatus s;
    s.mode = MacroEngine::PaceStatus::Mode::Paced;
    s.lock_ms = 680;
    s.rounds = 12;
    check(MacroEngine::pace_status_vi(s) == "Đang theo vòng — khóa chiêu ước 680 ms, 12 vòng.",
          "paced: \"" + MacroEngine::pace_status_vi(s) + "\"");
    s.hunting = true;
    check(MacroEngine::pace_status_vi(s) == "Đang theo vòng cùng Tự đánh quái — khóa chiêu ước 680 ms, 12 vòng.",
          "paced with the hunter (2026.11.16b): \"" + MacroEngine::pace_status_vi(s) + "\"");
    s.hunting = false;
    s.mode = MacroEngine::PaceStatus::Mode::Fallback;
    s.last_cast_ms = 0;
    check(MacroEngine::pace_status_vi(s) == "Tạm theo giờ: chưa thấy gói tin ra chiêu nào của bạn.",
          "no cast of yours yet: \"" + MacroEngine::pace_status_vi(s) + "\"");
    s.last_cast_ms = 7'000'000;
    check(MacroEngine::pace_status_vi(s) ==
              "Tạm theo giờ vì không thấy gói tin ra chiêu — khóa chiêu ước 680 ms; thấy lại thì theo vòng tiếp.",
          "fallen back: \"" + MacroEngine::pace_status_vi(s) + "\"");
    // Off with the switch on: since 2026.11.16b neither the hunter nor the
    // skills following the fight keep it so -- only the key sequence, or the
    // moment before the first pass.
    s.mode = MacroEngine::PaceStatus::Mode::Off;
    check(MacroEngine::pace_status_vi(s) == "Chưa theo vòng: mỗi phím bấm theo giờ riêng.",
          "off with the switch on, nothing else on: \"" + MacroEngine::pace_status_vi(s) + "\"");
    s.others = true;
    check(MacroEngine::pace_status_vi(s) ==
              "Chuỗi phím (Sequence) đang bật, nên không theo vòng: mỗi phím bấm theo giờ như thường.",
          "off with the switch on -- the key sequence pressing keys too: \"" + MacroEngine::pace_status_vi(s) + "\"");
    s.hunting = true;
    const std::string with_hunter = MacroEngine::pace_status_vi(s);
    check(with_hunter.find("Tự đánh quái") == std::string::npos && with_hunter.find("Kỹ năng đang theo trận đánh") ==
                                                                         std::string::npos,
          "no status says the hunter or the fight keeps it off any more: \"" + with_hunter + "\"");
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
    // 2026.11.12 (the owner: "khi bật thì bắt đầu đếm thời gian. Lúc nào tới
    // thời gian thì mới bắt đầu thực hiện"): the timed pass and the shop count
    // a whole interval from a start (bag::start_due_ms) -- no longer 15 s and
    // 45 s, nor the plan time saved in shop_clock.txt, which 2026.11.09 waited
    // for -- but for a clock the last worker left still counting, kept as the
    // charms' is, never sooner than 15 s and 45 s. The charms as they were.
    check(has(gui, "constexpr std::uint64_t kCharmFirstMs = 8'000;") &&
              has(gui, "constexpr std::uint64_t kPassFirstMs = 15'000;") &&
              has(gui, "constexpr std::uint64_t kShopFirstMs = 45'000;") &&
              in_order(loop, { "std::lock_guard<std::mutex> lock(trash_clocks_mutex_);", "kept = trash_clocks_;",
                               "if (kept.pass_ticks_seen != pass_ticks) kept.pass_due_ms = 0;",
                               "resumed_clock(kept.charm_check_ms, now_ms(), kCharmFirstMs)",
                               "std::uint64_t next = 0;", "std::uint64_t shop_due_ms = 0;", "bool counting = false;",
                               "CharmClock shop_clock(shop_clock_path());", "owed.hp = kept.owed_hp;",
                               "std::uint64_t plan_due_ms = kept.plan_due_ms;",
                               "shop_quiet.set_level(kept.shop_quiet_level);", "const auto start_counting = [&] {",
                               "counting = true;",
                               "next = bag::start_due_ms(kept.pass_due_ms, now, settings.interval_minutes, "
                               "kPassFirstMs);",
                               "shop_due_ms = bag::start_due_ms(kept.shop_due_ms, now, settings.buy_every_minutes, "
                               "kShopFirstMs);",
                               "GB_INFO(\"Trash: counting from the start -- the first timed pass at %s, the shop's "
                               "first look at %s\",",
                               "while (!trash_stop_) {", "if (!counting && !trash_oneshot_) start_counting();",
                               "const int request = trash_request_.exchange(0);" }) &&
              count_in(gui, "start_counting()") == 1 && count_in(gui, "bag::start_due_ms(") == 4 &&
              !has(gui, "shop_clock.load()") && !has(gui, "shop_first_wait_s") && !has(gui, "shop_first_due") &&
              !has(loop, "now_ms() + 15'000;") && !has(loop, "now_ms() + 8'000;") &&
              !has(loop, "now_ms() + 45'000;") && !has(loop, "now_ms() + kShopFirstMs;"),
          "trash_loop: the pass's and the shop's clocks count once the worker is here for a run -- from the start, or "
          "a button's worker become one -- a whole interval on, or the last worker's still counting, 15 s and 45 s "
          "at least; the charms' resumed as before, 8 s at least; shop_clock.txt no longer read");
    const std::size_t last_loop = loop.rfind("while (!trash_stop_) {");
    const std::size_t kept_at = loop.rfind("trash_clocks_.shop_due_ms = shop_due_ms;");
    const std::size_t ends = loop.rfind("trash_running_ = false;");
    check(last_loop != std::string::npos && kept_at != std::string::npos && ends != std::string::npos &&
              last_loop < kept_at && kept_at < ends && has(loop, "trash_clocks_.charm_check_ms = charm_check_ms;") &&
              in_order(loop.substr(last_loop == std::string::npos ? 0 : last_loop),
                       { "trash_clocks_.charm_check_ms = charm_check_ms;", "if (counting) {",
                         "trash_clocks_.pass_due_ms = next;", "trash_clocks_.pass_ticks_seen = pass_ticks;",
                         "trash_clocks_.shop_due_ms = shop_due_ms;", "trash_clocks_.shop_ticks_seen = shop_ticks;",
                         "}", "trash_clocks_.shop_quiet_level = shop_quiet.level();",
                         "trash_clocks_.charm_ticks_seen = charm_ticks;" }) &&
              count_in(loop, "trash_clocks_.pass_due_ms = ") == 1 &&
              count_in(loop, "trash_clocks_.shop_due_ms = ") == 1 &&
              in_order(loop.substr(kept_at == std::string::npos ? 0 : kept_at),
                       { "trash_clocks_.owed_hp = owed.hp;", "trash_clocks_.plan_due_ms = plan_due_ms;",
                         "trash_clocks_.shop_troubles = owed.troubles;", "shop_look_at_s_ = 0;",
                         "shop_owed_ = owed.hp + owed.mp;", "trash_running_ = false;" }),
          "and kept for the next as the worker ends, whichever way its loop is left -- the pass's and the shop's "
          "only by a worker that counted them, never one here for a button alone -- the shop's plan with them, "
          "and the card told no worker buys it now");
    check(in_order(loop, { "if (settings.enabled != pass_was_on) {", "if (pass_was_on) pass_afresh();",
                           "if (pass_ticked_.load() != pass_ticks) {", "pass_afresh();",
                           "if (settings.buy_potions != shop_was_on) {", "shop_afresh();",
                           "if (shop_ticked_.load() != shop_ticks) {", "owed = bag::ShopOwed{};", "shop_afresh();",
                           "shop_quiet.reset();", "if (charm_ticked_.load() != charm_ticks) {",
                           "charm_check_ms = now_ms() + kCharmFirstMs;" }) &&
              in_order(loop, { "const auto pass_afresh = [&] {",
                               "if (counting) next = bag::start_due_ms(0, now_ms(), settings.interval_minutes, "
                               "kPassFirstMs);",
                               "else kept.pass_due_ms = 0;", "const auto shop_afresh = [&] {",
                               "if (counting) shop_due_ms = bag::start_due_ms(0, now_ms(), settings.buy_every_minutes, "
                               "kShopFirstMs);",
                               "else kept.shop_due_ms = 0;" }) &&
              // And the third (2026.11.13b): "Mua ngay sau khi dọn rác" gone,
              // the clock buying again.
              count_in(loop, "shop_afresh();") == 3 && count_in(loop, "pass_afresh();") == 2 &&
              has(loop, "if (!after_now && settings.buy_potions && owed.hp + owed.mp == 0) shop_afresh();") &&
              !has(gui, "shop_blocked") &&
              has(loop, "if (kept.charm_ticks_seen != charm_ticks) kept.charm_check_ms = 0;") &&
              in_order(loop, { "if (kept.shop_ticks_seen != shop_ticks) {", "kept.owed_hp = kept.owed_mp = 0;",
                               "kept.plan_due_ms = 0;" }) &&
              in_order(flat(shop_card), { "if (ImGui::Checkbox(\"Tự mua bình\", &simple_clean_.buy_potions)) {",
                                          "if (simple_clean_.buy_potions) { ++shop_ticked_; shop_owed_ = 0; }",
                                          "if (simple_clean_.buy_potions && simple_running()) start_trash();" }) &&
              count_in(shop_card, "++shop_ticked_;") == 1 &&
              in_order(flat(view), { "if (ImGui::Checkbox(\"Tự dọn rác\", &simple_clean_.enabled)) {",
                                     "if (simple_clean_.enabled) ++pass_ticked_;",
                                     "if (simple_clean_.enabled && simple_running()) start_trash();" }) &&
              count_in(view, "++pass_ticked_;") == 1 &&
              has(use, "if (simple_clean_.use_charms) ++charm_ticked_;") &&
              has(hpp, "std::atomic<unsigned>    charm_ticked_{0};") &&
              has(hpp, "std::atomic<unsigned>    pass_ticked_{0};"),
          "\"Tự dọn rác\", \"Tự mua bình\" and \"Dùng bùa\" ticked on again: that job's clock afresh -- the pass and "
          "the shop a whole interval from now (in a worker not counting, from the next start), the shop owing "
          "nothing with its back-off reset; the worker running or the next; the window's thread moves an atomic "
          "only; no shop_blocked anywhere");
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
                               "else if (end == bag::ShopEnd::Trouble) shop_due_ms = now_ms() + "
                               "bag::kShopTroubleAgainMs;",
                               "else shop_due_ms = now_ms() + BagCleaner::kShopPartGapMs;" }) &&
              has(charm_job, "if (!forced && !trash_stop_ && ((!r.ran && (!only_message || !r.trouble.empty())) ||") &&
              has(charm_job, "if (!forced && r.tab_unsure) charm_check_ms = now_ms() + kCharmTabRetryMs;"),
          "a shop run cut by DỪNG/F10 owing the rest leaves its clock as it was (due already: the next worker counts "
          "a whole interval from its start since 2026.11.12, 45 s before), a charm run's too (8 s) -- but a plan "
          "bought in full waits for the next plan whatever ended its round; one its guard stopped for HP is looked "
          "at again in 15 s (2026.11.11)");
    // 2026.11.24 (the owner: "Kiểm tra các phần khác xem có phần nào không làm
    // được thì dừng không, cần thay cơ chế này bằng 1 cơ chế khác"): a shop
    // trouble never drops the plan -- every one tried again 30 s on, said on
    // the card from the second in a row -- and a charm's message two clicks
    // did not close is clicked again by the next charm run, a minute on.
    check(!has(shop_job, "bool dropped") && !has(gui, "owed.dropped") && !has(gui, "kShopTroublesToDrop") &&
              !has(shop_job, "if (dropped)") && !has(shop_job, "add(\"Hai lần mua liền bị lỗi") &&
              in_order(shop_job, { "const bag::ShopEnd end = bag::shop_end(seen);",
                                   "else if (end == bag::ShopEnd::Trouble) shop_due_ms = now_ms() + "
                                   "bag::kShopTroubleAgainMs;",
                                   "if (end == bag::ShopEnd::Trouble && next_part) {",
                                   "\"Trash: shop -- %d trouble(s) in a row: owed %d+%d kept, tried again %llu s on\"",
                                   "troubled = owed.troubles >= 2 && !trash_oneshot_ && !trash_stop_;",
                                   "if (!r.trouble.empty() && !troubled) line +=", "if (troubled) {",
                                   "add(\"Mua bình bị lỗi \" + std::to_string(owed.troubles) + \" lần liền\" +",
                                   "\": GoBot thử lại sau \" + in_said + \".\");",
                                   "} else if (!dry && left > 0 && (!r.board_up || trash_oneshot_) && !trash_stop_) {",
                                   "\", mua tiếp sau \" + in_said + \".\"" }) &&
              has(shop_job, "in_s < 60 ? std::to_string(in_s) + \" giây\" : std::to_string((in_ms + 59'999) / "
                            "60'000) + \" phút\";") &&
              in_order(charm_job, { "} else if (!r.notice_carried || charm_notice_ms == 0 || r.notice_clicked) {",
                                    "if (r.yielded && !trash_stop_) charm_check_ms = now_ms() + bag::kHpStoppedAgainMs;",
                                    "if (r.notice_left && r.notice_clicked && !trash_stop_) charm_check_ms = now_ms() "
                                    "+ kCharmNoticeAgainMs;" }) &&
              has(gui, "constexpr std::uint64_t kCharmNoticeAgainMs = 60'000;") &&
              has(gui, "constexpr std::uint64_t kCharmTabRetryMs = 60'000;"),
          "2026.11.24: a shop trouble keeps what is owed and is tried again 30 s on without limit -- logged with its "
          "count, the card saying \"Mua bình bị lỗi N lần liền (...): GoBot thử lại sau 30 giây.\" from the second "
          "in a row, a part's wait in seconds under a minute; a charm message clicked and still up keeps its two "
          "minutes from that run and is clicked again a minute on; the charm tab's retry a minute");

    // ---- 2026.11.09: the shop's plan ----
    const std::size_t plan_at = shop_job.find("owed = bag::shop_new_plan(settings.buy_hp_slots, settings.buy_mp_slots);");
    const std::size_t clock_at = shop_job.find("shop_clock.set_next(kShopClockId,");
    const std::size_t hold_at = shop_job.find("take_hold(\"trash cleaning\")");
    const std::size_t run_at = shop_job.find("r = cleaner.run();");
    check(clock_at != std::string::npos && plan_at != std::string::npos && hold_at != std::string::npos &&
              run_at != std::string::npos && clock_at < plan_at && plan_at < hold_at && hold_at < run_at &&
              count_in(gui, "shop_clock.set_next(") == 1 &&
              in_order(shop_job, { "if (!dry) {", "owed.hp = std::min(owed.hp, settings.buy_hp_slots);",
                                   "if (bag::shop_plan_begins(forced, after_pass_buys, look_owed, now_ms() >= "
                                   "plan_due_ms)) {",
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
    // 2026.11.16b: on the "Tự dùng kỹ năng" row itself too, the same switch,
    // saved and handed over at once, with a short hover.
    const std::string line = body(skills, "void GuiApp::simple_skills_line() {");
    check(has(skills, "const char* const kCastPacedShort = \"Ra chiêu theo vòng\";") &&
              in_order(line, { "if (skill_gap_combo(cfg)) simple_changed();",
                               "if (ImGui::Checkbox(kCastPacedShort, &cfg.cast_paced)) simple_changed();",
                               "ImGui::SetItemTooltip(\"Có dấu (mặc định): kỹ năng ra lần lượt theo thứ tự phím",
                               "Chạy cả khi Tự đánh quái bật.\\nBỏ dấu: mỗi phím bấm theo giờ riêng như trước.\");",
                               "if (cfg.cast_paced) hint(cast_paced_status_vi().c_str());" }),
          "\"Ra chiêu theo vòng\" on the \"Tự dùng kỹ năng\" row (2026.11.16b): after \"Tốc độ bấm kỹ năng\", saved at "
          "once, its short hover saying it is on by default and runs with Tự đánh quái, its status under the row");
    const std::string tip = body(skills, "const char* GuiApp::cast_paced_tip_vi() {");
    const std::string status = body(skills, "std::string GuiApp::cast_paced_status_vi() const {");
    check(has(tip, "gói tin") && has(tip, "2 giây") && has(tip, "tạm bấm theo giờ") &&
              has(tip, "Chạy cả khi Tự đánh quái bật") && !has(tip, "Tự đánh quái tắt") &&
              has(tip, "khoảng chờ ngắn nhất") && has(tip, "không phím nào bị bỏ đói") &&
              has(tip, "Có dấu (mặc định)") && !has(tip, "Bỏ dấu (mặc định)") &&
              has(tip, "Phím đánh mà cũng là kỹ năng thì ra theo lượt của nó, bot không bấm thêm") &&
              has(tip, "Chuỗi phím (Sequence) bật thì ô này tạm không có tác dụng") &&
              has(status, "MacroEngine::pace_status_vi(st)") &&
              has(status, "st.mode == MacroEngine::PaceStatus::Mode::Off && !st.others") &&
              !has(status, "follows") && !has(status, "chỉ dành cho macro chạy một mình") &&
              !has(status, "save(") && !has(status, "load(") &&
              !has(status, "exe_relative") && !has(skills, "macro_config_.save("),
          "its hover says what it does, on by default, with Tự đánh quái too, that it needs the packets and falls "
          "back after 2 s, the sequence alone keeping it off, and the interval as the least wait; its status reads "
          "no file and writes none");

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

    // ---- W1: the switch -- on as shipped since 2026.11.16b -- and where it is ----
    const std::string label = literal_after(skills, "const char* const kCastPacedLabel = \"");
    const std::string short_label = literal_after(skills, "const char* const kCastPacedShort = \"");
    MacroConfig built;
    built.apply_defaults();
    MacroConfig ships;
    const bool shipped = ships.load(std::string(GOBOT_SOURCE_DIR) + "/configs/macro.ini");
    check(!label.empty() && short_label == "Ra chiêu theo vòng" && MacroConfig{}.cast_paced && built.cast_paced &&
              shipped && ships.cast_paced && std::string(MacroConfig::kCastPacedKey) == "cast_paced_v2" &&
              has(ini, "cast_paced=true") && has(ini, "cast_paced_v2=true") && !has(ini, "cast_paced=false") &&
              !has(ini, "Off, and in a file without it: every key on its own timer, as always.") &&
              has(ini, "the tick \"" + short_label + "\" on the \"Tự dùng kỹ năng\" row, \"" + label +
                           "\" in the \"Kỹ năng\" window (Chi tiết… on that row), and in the full screen's Timed keys "
                           "-- one switch.") &&
              has(ini, "Only the key sequence on, with a key in it, keeps it off.") &&
              has(ini, "cast_paced_v2 is the switch read since 2026.11.16b; a file without it was written before and "
                       "is read as on, whatever its cast_paced says (that false was the old default).") &&
              has(ini, "To switch it off by hand: cast_paced_v2=false, with GoBot closed.") &&
              has(ini, "(VI) Bấm kỹ năng theo nhịp chiêu ra: lần lượt từng chiêu theo thứ tự phím, chỉ bấm chiêu sau "
                       "khi game đã ra chiêu trước (xem qua gói tin), cả khi Tự đánh quái chạy. Mặc định bật từ bản "
                       "2026.11.16b; muốn tắt thì bỏ dấu ô \"Ra chiêu theo vòng\" ở dòng Tự dùng kỹ năng.") &&
              !has(ini, "Mặc định tắt.") &&
              has(doc, "Ô mới \"" + label + "\" (thử nghiệm; trong bản này mặc định KHÔNG có dấu, từ bản 2026.11.16b "
                       "mặc định CÓ dấu và chạy cả khi Tự đánh quái chạy)") &&
              has(doc, "Trong bản này, macro.ini không có dòng cast_paced thì coi như ô này không có dấu. Từ bản "
                       "2026.11.16b ô nằm ngay ở dòng Tự dùng kỹ năng, mặc định có dấu, và sửa tay là dòng "
                       "cast_paced_v2 trong phần [Macro], lúc GoBot đã tắt.") &&
              !has(doc, "sửa tay thì là cast_paced=true") &&
              has(readme, "*" + label + "*") &&
              has(readme, "On as shipped since 2026.11.16b, and a `macro.ini` without `cast_paced_v2` reads on, "
                          "whatever its old `cast_paced` says") &&
              !has(readme, "Off as shipped, and a `macro.ini` without `cast_paced` reads false") &&
              has(pack, "OFF as shipped in this version, ON and with Tu danh quai too since 2026.11.16b, below") &&
              !has(pack, "OFF as shipped, cast_paced in configs\\macro.ini"),
          "\"" + label + "\": on in code, in the shipped macro.ini (named by its ticks) and in one without "
          "cast_paced_v2 -- 2026.11.08's notes say it was off then and is on since 2026.11.16b");
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
              has(clear, "const bool board = held && checkin_board_title(f, store);") &&
              has(clear, "if (shop_held && (bag::shop_left_up(f, store) || board)) return;") &&
              count_in(cancel, "board_over_question(") == 3 &&
              has(cancel, "if (!seen_at(shop_q_) || board_over_question(f)) return false;") &&
              has(cancel, "if (shop_q_.valid() && (!seen_at(shop_q_) || board_over_question(f))) return false;") &&
              has(over, "checkin_board_title(f, store_, &title)") && has(over, "kCheckinBoardFromTitle") &&
              has(over, "board.overlaps(bag::message_box(shop_q_))") &&
              has(bag_cpp, "((shop_q_.valid() || (res.board_up && res.shop_opened)) && checkin_board_title(h, store_))"),
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
    // Still written as a plan begins; since 2026.11.12 a start counts a whole
    // interval and no longer waits by it (bag::start_due_ms). The 2026.11.09
    // entries say the rule of then, 45 s to an interval, as it was.
    const std::string clock_file = shop_clock_path();
    const std::string tail = "\\shop_clock.txt";
    const std::uint64_t start = 7'000'000;
    check(clock_file.size() > tail.size() &&
              clock_file.compare(clock_file.size() - tail.size(), tail.size(), tail) == 0 &&
              bag::start_due_ms(0, start, 60, 45'000) == start + 3'600'000 &&
              bag::start_due_ms(start + 600'000, start, 60, 45'000) == start + 600'000 &&
              has(doc, "Giờ mua được ghi vào %LOCALAPPDATA%\\GoBot\\shop_clock.txt TRƯỚC khi bấm gì") &&
              has(doc, "(ít nhất 45 giây, nhiều nhất một khoảng \"mua mỗi\")") &&
              has(readme, "`%LOCALAPPDATA%\\GoBot\\shop_clock.txt`") &&
              has(readme, "`kShopFirstWaitS` (45 s) at least and one interval at most (`bag::shop_first_wait_s`)") &&
              has(trash_ini, "%LOCALAPPDATA%\\GoBot\\shop_clock.txt before anything is clicked") &&
              has(trash_ini, "Since 2026.11.12 the first time comes buy_every_minutes after BẮT ĐẦU (F10), "
                                         "or after \"Tự mua bình\" is ticked on -- nothing is bought at the start "
                                         "itself (\"Mua ngay\" is for that); a DỪNG and BẮT ĐẦU again before it is "
                                         "due keeps the time still counting.") &&
              has(trash_ini, "GoBot started again counts a whole interval from its start, so it never "
                                         "buys early (until 2026.11.12 it waited what was left -- 45 seconds at "
                                         "least, one interval at most).") &&
              has(trash_ini, "Since 2026.11.12 the first timed pass comes interval_minutes after BẮT ĐẦU "
                                         "(F10), or after the box is ticked on -- none at the start itself (\"Dọn "
                                         "ngay\" is for that)") &&
              has(pack, "%LOCALAPPDATA%\\GoBot\\shop_clock.txt, so a restart never buys early"),
          "the plan's time in shop_clock.txt, still written; a start's first look a whole interval on, a clock still "
          "counting kept -- as bag::start_due_ms has it -- trash.ini saying so for the pass and the shop, and the "
          "2026.11.09 entries the rule of then");

    // ---- the bag full, a message, trouble ----
    const int full_gap = hpp_int("kShopFullGapMs"), full_late = hpp_int("kShopFullLateMs");
    // Troubles in a row (2026.11.24, the owner: "Kiểm tra các phần khác xem
    // có phần nào không làm được thì dừng không, cần thay cơ chế này bằng 1 cơ
    // chế khác"): never the plan dropped -- every one tried again 30 s on --
    // where the second in a row dropped it until then (kShopTroublesToDrop,
    // gone). The bag full and a message still end it.
    bag::ShopOwed rows = bag::shop_new_plan(2, 2);
    for (int i = 0; i < 4; ++i) rows = bag::shop_owed_after(rows, 0, 0, 0, 0, bag::ShopEnd::Trouble);
    check(full_gap > 0 && full_late == 1000 && rows.hp == 2 && rows.mp == 2 && rows.troubles == 4 &&
              has(doc, "chỉ coi là đã đóng khi hai lần nhìn cách nhau " + vi_s(static_cast<std::uint64_t>(full_gap)) +
                           " giây đều không còn thấy") &&
              has(readme, "closed only when two looks " + n(full_gap) + " ms apart no longer see it; never an ESC while "
                          "it is up") &&
              has(readme, "for a second after Xác Nhận (`kShopFullLateMs`)") &&
              has(doc, "Lượt có lỗi thì chờ 30 giây (trước bản 2026.11.15b là 10 phút) rồi mua tiếp phần còn lại, "
                       "lượt nào có lỗi cũng vậy, không giới hạn (từ bản 2026.11.24; trước đó hai lượt liền có lỗi "
                       "thì GoBot bỏ phần còn lại, tới giờ sau mới mua lại).") &&
              has(readme, "and so does every one after it, without limit: since 2026.11.24 troubles in a row never "
                          "drop the plan (until then the second in a row, `kShopTroublesToDrop`, dropped what was "
                          "left of it until the next)") &&
              !has(readme, "the second in a row (`kShopTroublesToDrop`) drops what is left of the plan") &&
              has(doc, "Ô \"Tự mua bình\" VẪN CÓ DẤU: tới giờ sau GoBot mua lại.") &&
              has(doc, "giữ ảnh shop_message trong trash_debug") && has(bag_cpp, "keep(f, \"shop_message\");") &&
              has(doc, "giữ ảnh shop_full_stuck") && has(bag_cpp, "keep(g, \"shop_full_stuck\");"),
          "the full message closed when two looks " + n(full_gap) + " ms apart miss it, looked for a second after "
          "Xác Nhận, never ESC; troubles in a row never drop the plan since 2026.11.24 (four in a row: 2 + 2 still "
          "owed) -- as bag.hpp has them");

    // ---- the check-in board: how it is told and closed ----
    // Since 2026.11.15d (the owner: "chờ 30 giây rồi mới ấn ESC ... qua ngày
    // thì chờ tới 00:03"): the ESC kCheckinBoardLoginWaitMs after the arrival
    // or the first sight, still for kSteadyMs, never in the night hold. Since
    // 2026.11.24 (the owner: "Kiểm tra các phần khác xem có phần nào không làm
    // được thì dừng không, cần thay cơ chế này bằng 1 cơ chế khác") an ESC that
    // left it up is followed by another kBackoffMs on, as often as it takes --
    // no more kTries (2) in kTriesWindowMs (10 minutes), then left alone.
    const std::string board_wait_s = n(kCheckinBoardLoginWaitMs / 1000);
    const std::string again_s = n(P::kBackoffMs / 1000);
    check(P::kSteadyMs % 1000 == 0 && P::kHoldMs % 1000 == 0 && P::kBackoffMs == 30'000 &&
              kCheckinBoardLoginWaitMs % 1000 == 0 && kCheckinNightFromS == 23 * 3600 + 55 * 60 &&
              kCheckinNightToS == 3 * 60 &&
              has(doc, "bảng hiện (lúc vào game, lúc qua 00:00) thì GoBot bấm ESC một lần " + board_wait_s +
                           " giây sau khi nhân vật vào game (GoBot tự vào: tính từ lúc vào; vào bằng tay: tính từ "
                           "lúc thấy bảng), bảng phải đứng yên ít nhất " + n(P::kSteadyMs / 1000) +
                           " giây; từ 23:55 tới 00:03 theo đồng hồ máy thì không bấm, qua ngày GoBot chờ tới 00:03 "
                           "mới bấm ESC") &&
              !has(doc, "bảng hiện (lúc vào game, lúc qua 00:00) và đứng yên " + n(P::kSteadyMs / 1000) +
                            " giây thì GoBot bấm ESC một lần") &&
              has(doc, "các phím khác chờ tối đa " + n(P::kHoldMs / 1000) + " giây") &&
              has(doc, "Bấm ESC mà bảng vẫn còn thì " + again_s + " giây sau GoBot bấm lại, cứ thế tới khi bảng tắt") &&
              has(doc, "(từ bản 2026.11.24; trước đó bấm ESC 2 lần trong 10 phút mà bảng vẫn còn thì GoBot để yên "
                       "cho bạn tự tắt)") &&
              has(readme, "presses ESC once the board is due with nothing else of its list up") &&
              has(readme, "`kCheckinBoardLoginWaitMs` (" + board_wait_s + " s) after the later of the character's "
                          "arrival in the world (the keeper's crossing) and the board's first sight, having stayed "
                          "put `kSteadyMs` (" + n(P::kSteadyMs / 1000) + " s), and never while the PC's clock reads "
                          "23:55:00 to 00:03:00 (`checkin_night_hold`)") &&
              !has(readme, "presses ESC once the board has stayed put " + n(P::kSteadyMs / 1000) + " s") &&
              has(readme, "a hold of at most " + n(P::kHoldMs / 1000) + " s, a fresh look, ESC by " +
                              en_s(P::kEscPressByMs) + " s") &&
              has(readme, "Since 2026.11.24 an ESC that left it up is followed by another `kBackoffMs` (" + again_s +
                              " s) later, as often as it takes") &&
              has(readme, "until then two tries in 10 minutes, then it was left alone and the tab said so") &&
              !has(readme, "Two tries in 10 minutes, then it is left alone") &&
              has(readme, "within " + n(kCheckinRelookSlack) + " px of where it was") &&
              kCheckinBoardScore > 0.899f && kCheckinBoardScore < 0.901f && has(readme, "`kCheckinBoardScore` 0.90") &&
              has(readme, "(\"Sự kiện và điểm danh\", " + n(kCheckinBoardFromTitle.w) + "x" +
                              n(kCheckinBoardFromTitle.h) + ", with its *Điểm danh* calendar)") &&
              has(app_ini, "each once it has stayed in one place for " + n(P::kSteadyMs / 1000) + " seconds") &&
              has(app_ini, "-- since 2026.11.15d only " + board_wait_s + " seconds after the character comes into "
                           "the game (or the board is first seen), and never from 23:55 to 00:03 by the PC's clock: "
                           "at the day's turn it waits until 00:03;") &&
              has(app_ini, "bảng Sự kiện và điểm danh (lúc vào game và lúc qua 00:00, bấm ESC " + board_wait_s +
                               " giây sau khi vào game, qua ngày thì chờ tới 00:03)") &&
              !has(app_ini, "bấm ESC sau khi bảng đứng yên") &&
              has(app_ini, "(ESC, never its X, and only with nothing else of these up: the notice goes first)") &&
              std::string(popup_kind_name(PopupKind::CheckinBoard)) == "the check-in board",
          "the board ESC'd " + n(kCheckinBoardLoginWaitMs) + " ms (kCheckinBoardLoginWaitMs) after the arrival or "
              "its first sight, " + n(P::kSteadyMs) + " ms still, never in the night hold (checkin_night_hold, "
              "23:55 to 00:03), under a hold of " + n(P::kHoldMs) + " ms, tried again " + n(P::kBackoffMs) +
              " ms after an ESC that left it up, without end, re-looked within " + n(kCheckinRelookSlack) +
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
              // The board still up after its ESC (2026.11.24): tried again,
              // never "GoBot để yên, bạn tắt giúp" any more.
              has(closer, "last_vi_ = board ? \"Đã bấm ESC mà bảng Sự kiện và điểm danh vẫn còn: thử lại sau \" +") &&
              has(closer, ": std::string(\"Đã thử đóng \") + popup_kind_vi(kind) + \" mà vẫn còn: thử lại sau \" +") &&
              has(doc, "\"Đã bấm ESC mà bảng Sự kiện và điểm danh vẫn còn: thử lại sau " + n(P::kBackoffMs / 1000) +
                           " giây.\"") &&
              !has(closer, "bạn tắt giúp") && !has(doc, "\"Bảng Sự kiện và điểm danh vẫn còn sau 2 lần bấm ESC") &&
              has(popups, "return \"Tự đóng thông báo: đang bật\" + (n > 0 ? \", đã đóng \" + std::to_string(n)") &&
              has(popups, "(boards > 0 ? \" (bảng điểm danh: \" + std::to_string(boards) + \")\"") &&
              has(doc, "\"Tự đóng thông báo: đang bật, đã đóng N (bảng điểm danh: M).\"") &&
              !has(doc, "\"Tự đóng thông báo: đang bật, đã đóng N.\"") &&
              has(doc, "\"Thử xem\" phải nói thấy \"bảng Sự kiện và điểm danh ở x,y\"") &&
              has(popups, "out += \" ở \" + std::to_string(up[i].at.x) + \",\" + std::to_string(up[i].at.y) +") &&
              has(doc, "thấy \"" + std::string(popup_kind_vi(PopupKind::CheckinBoard)) + " ở x,y\""),
          "the card's \"" + bought + "\", the both-0 and trial lines, the board tried again (never left alone), the "
          "line under BẮT ĐẦU and Thử xem -- as the GUI and the closer word them");

    // ---- the log's words the notes quote ----
    char plan[160], buy[96], done[160];
    std::snprintf(plan, sizeof plan,
                  "Trash: shop plan HP %d + MP %d (owed %d+%d, at most %d a hold%s); clock saved, next plan %s", 10,
                  10, 10, 10, per_hold, "", "23:15");
    std::snprintf(buy, sizeof buy, "Trash: shop purchase %d/%d %s -- bought (%d read back)", 1, 10, "HP",
                  CleanSettings::kBuyQuantity);
    std::snprintf(done, sizeof done, "Trash: shop done -- bought %d+%d, unsure %d, full %s%s; owed %d+%d; %s %s", 5, 5,
                  0, "no", "", 5, 5, "next part", (n(gap_s) + " s on").c_str());
    // Troubles in a row since 2026.11.24: the rest kept and tried again 30 s
    // on, said with their count -- no longer "owed dropped until the next
    // plan", which the second in a row was until then.
    char troubles[160];
    std::snprintf(troubles, sizeof troubles,
                  "Trash: shop -- %d trouble(s) in a row: owed %d+%d kept, tried again %llu s on", 2, 5, 5,
                  static_cast<unsigned long long>(bag::kShopTroubleAgainMs / 1000));
    const std::string dropped_line = "Trash: shop -- 2 troubles in a row: owed dropped until the next plan ";
    check(has(gui, "\"Trash: shop plan HP %d + MP %d (owed %d+%d, at most %d a hold%s); clock saved, next plan %s\"") &&
              has(bag_cpp, "\"Trash: shop purchase %d/%d %s -- bought (%d read back)\"") &&
              has(gui, "\"Trash: shop done -- bought %d+%d, unsure %d, full %s%s; owed %d+%d; %s %s\"") &&
              has(gui, "\" s on\"") && has(gui, "next_part ? \"next part\" : \"next plan\"") &&
              has(gui, "\"Trash: shop -- %d trouble(s) in a row: owed %d+%d kept, tried again %llu s on\"") &&
              !has(gui, "owed dropped until the next plan") &&
              has(doc, "\"" + std::string(plan) + "\"") && has(readme, "`" + std::string(plan) + "`") &&
              has(doc, "\"" + std::string(buy) + "\"") && has(readme, "`" + std::string(buy) + "`") &&
              has(doc, "\"" + std::string(done) + "\"") && has(readme, "`" + std::string(done) + "`") &&
              has(doc, "\"" + std::string(troubles) + "\" (từ bản 2026.11.24; trước đó \"" + dropped_line +
                           "...\")") &&
              has(readme, "`" + std::string(troubles) + "` (since 2026.11.24; until then `" + dropped_line + "…`)"),
          "the shop's plan, purchase, done and troubles lines -- \"" + std::string(plan) + "\", \"" +
              std::string(troubles) + "\" -- as the worker and the cleaner write them");
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
    // 2026.11.11b's entry on top of it since, its step 15 after step 14,
    // 2026.11.11c's (the notice closed by GoBot) on top of that, and
    // 2026.11.12's (the hunt: turns, the area, falls, the bars) on top of all
    // -- and 2026.11.12b's (the charms and more at any HP) on top of it,
    // 2026.11.12c's on that, 2026.11.13's (the bars' figures) on it,
    // 2026.11.13b's (the shop after the pass, the login's message) on that and
    // 2026.11.13c's (the three game sizes) on it, 2026.11.14's (the game in
    // the corner after a login) on that, and 2026.11.14b's (the shop after a
    // pass, ESC after 10 s, the message after an invite) -- and 2026.11.15's
    // (one lost key press is no evidence) before that, and 2026.11.15b's (the
    // shop and the party again after 30 s) first of those, and 2026.11.15c's
    // (a report in mid-air is no landing) on them, and 2026.11.15d's (thirty
    // seconds for the game to react, the board's ESC after a login) on all of
    // those, and 2026.11.16's (lighter on weak PCs, old features removed)
    // on them, and 2026.11.16b's (the skills in turn while hunting too) on
    // those, and 2026.11.17's (the MP bar, HP/MP by the packets alone, the
    // skills even while hunting) first of all.
    const std::size_t e17 = doc.find("- Mới trong bản 2026.11.17 (");
    const std::size_t e16b = doc.find("- Mới trong bản 2026.11.16b (");
    const std::size_t e16 = doc.find("- Mới trong bản 2026.11.16 (");
    const std::size_t e15d = doc.find("- Mới trong bản 2026.11.15d (");
    const std::size_t e15c = doc.find("- Mới trong bản 2026.11.15c (");
    const std::size_t e15b = doc.find("- Mới trong bản 2026.11.15b (");
    const std::size_t e15 = doc.find("- Mới trong bản 2026.11.15 (");
    const std::size_t e14b = doc.find("- Mới trong bản 2026.11.14b (");
    const std::size_t e14 = doc.find("- Mới trong bản 2026.11.14 (");
    const std::size_t e13c = doc.find("- Mới trong bản 2026.11.13c (");
    const std::size_t e13b = doc.find("- Mới trong bản 2026.11.13b (");
    const std::size_t e13 = doc.find("- Mới trong bản 2026.11.13 (");
    const std::size_t e12c = doc.find("- Mới trong bản 2026.11.12c (");
    const std::size_t e12b = doc.find("- Mới trong bản 2026.11.12b (");
    const std::size_t e12 = doc.find("- Mới trong bản 2026.11.12 (");
    const std::size_t e11c = doc.find("- Mới trong bản 2026.11.11c (tự tắt thông báo lạ khi mua bình;");
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
    check(sec5 != npos && e14b != npos && e14 != npos && e13c != npos && e13b != npos && e13 != npos &&
              e12c != npos && e12b != npos && e12 != npos && e11c != npos && e11b != npos && e11 != npos &&
              e10 != npos && e17 != npos && e16b != npos && e16 != npos && e15d != npos && e15c != npos &&
              e15b != npos && e15 != npos && doc.find("- Mới trong bản", sec5) == doc.find("- Mới trong bản 2026.11.24 (") && doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.24 (") + 1) == doc.find("- Mới trong bản 2026.11.17c (") && doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.17c (") + 1) == doc.find("- Mới trong bản 2026.11.17b (") &&
              doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.17b (") + 1) == e17 &&
              doc.find("- Mới trong bản", e17 + 1) == e16b &&
              doc.find("- Mới trong bản", e16b + 1) == e16 && doc.find("- Mới trong bản", e16 + 1) == e15d &&
              doc.find("- Mới trong bản", e15d + 1) == e15c &&
              doc.find("- Mới trong bản", e15c + 1) == e15b && doc.find("- Mới trong bản", e15b + 1) == e15 &&
              doc.find("- Mới trong bản", e15 + 1) == e14b &&
              doc.find("- Mới trong bản", e14b + 1) == e14 && doc.find("- Mới trong bản", e14 + 1) == e13c &&
              doc.find("- Mới trong bản", e13c + 1) == e13b && doc.find("- Mới trong bản", e13b + 1) == e13 &&
              doc.find("- Mới trong bản", e13 + 1) == e12c && doc.find("- Mới trong bản", e12c + 1) == e12b &&
              doc.find("- Mới trong bản", e12b + 1) == e12 &&
              e12 < e11c && doc.find("- Mới trong bản", e12 + 1) == e11c &&
              e11c < e11b && doc.find("- Mới trong bản", e11c + 1) == e11b &&
              e11b < e11 && doc.find("- Mới trong bản", e11b + 1) == e11 && e11 < e10 &&
              try14 != npos && e11 < try14 && try14 < e10 && step13 != npos && step14 != npos && revive != npos &&
              step15 != npos && step13 < step14 && step14 < step15 && step15 < revive && rkeys != npos &&
              r11 != npos && rmacro != npos && rkeys < r11 &&
              r11 < rmacro && has(readme, "Since 2026.11.11 (not yet run on the game).") &&
              has(pack, "The shop right after the pass, potions during the bag jobs, and six more keys (2026.11.11, "
                        "not yet tried on the game)."),
          "DOC-TRUOC.txt: the 2026.11.11 entry on top of 2026.11.10's -- under 11.13's, 11.12c's, 11.12b's, 11.12's, 11.11c's and "
          "11.11b's, in that order -- not yet tried on the game, its live test step 14 after step 13; README's section after "
          "the key window's, and START-HERE, say it too");

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
              has(shop_card, "if (simple_clean_.buy_potions && simple_clean_.enabled && !after_pass) {") &&
              has(doc, "thẻ Tự mua bình ghi \"" + ride_hint + "\"") && has(readme, "*" + ride_hint + "*") &&
              has(doc, "Vừa dọn xong chưa tới " + mins + " phút thì không dọn lại") &&
              has(readme, "`bag::kRideFreshMs` (" + n(fresh / 1000) + " s)") &&
              has(pack, "after a pass that ended less than " + mins + " minute before, none is made again") &&
              has(trash_ini, "a pass ended less than a minute before is not made again."),
          "a pass ended less than " + n(fresh) + " ms ago pulls none, an older one or none yet pulls one, Dọn rác "
          "off never -- as bag::ride_turn has it -- and the card's hint quoted as simple_shop.cpp builds it");
    check(has(gui, "ride = bag::ride_turn(was, ride_due, settings.enabled, cannot_pull, now_ms(), last_pass_end_ms);") &&
              has(gui, "const bool ride_due = shop_due && owed.troubles == 0;") &&
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
              has(gui, "pulled ? \"the look that pulled it\"") &&
              has(gui, ": look_owed ? \"right after each timed pass, buy_after_trash\"") &&
              has(gui, ": \"come due while it ran\");") &&
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
              // The pet's Thịt bò named apart since 2026.11.24.
              has(bag_cpp, "const std::string name = pet ? std::string(CleanSettings::kBuyPetShown) + \" (HP pet)\"") &&
              has(bag_cpp, ": hp ? \"Thịt bò khô\" : \"Rượu lá tre\";") &&
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
    // (The 2026.11.11 entry's first waits, 15 s and 45 s, as they were then:
    // a whole interval since 2026.11.12.)
    check(has(doc, "lần xem mua đầu 45 giây sau, rồi mỗi đồng hồ tính từ lượt của chính "
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

// ------------------------------------------------ 2026.11.12: the notes

/// 2026.11.12's notes against the code: DOC-TRUOC.txt's entry -- on top of
/// 11.11c's -- and its live test step 16, README.md's release section,
/// START-HERE (package.ps1), the comments of hunt.ini and macro.ini, and
/// docs/goc-comparison.md: the owner's report and eight answers as written,
/// and the numbers, the card's words and the log lines they quote as the code
/// has them -- the confirmed turns, the attack's reach, the skill holds, the
/// dash, the area kept, the way back, the pit, the two bars, the minimums gone
/// and the bag jobs counted from the start.
void test_notes_1112() {
    section("The 2026.11.12 notes against the code: turns, holds, the dash, the area, the way back, the pit, the two "
            "bars, the start count, the words quoted");
#ifdef GOBOT_SOURCE_DIR
    const std::string doc = flat(source("/DOC-TRUOC.txt"));
    const std::string readme = flat(source("/README.md"));
    const std::string pack = flat(source("/package.ps1"));
    const std::string hunt_raw = source("/configs/hunt.ini");
    const std::string hunt_ini = flat(hunt_raw, true);
    const std::string macro_ini = flat(source("/configs/macro.ini"), true);
    const std::string goc = flat(source("/docs/goc-comparison.md"));
    const std::string hunter = source("/src/hunter.cpp");
    const std::string drop = source("/src/hunter_drop.cpp");
    const std::string macro = source("/src/macro.cpp");
    const std::string world = source("/src/packet_world.cpp");
    const std::string more = source("/apps/gobot_gui/packets_more.cpp");
    const std::string spot = source("/apps/gobot_gui/simple_spot.cpp");
    const std::string map = source("/apps/gobot_gui/simple_map.cpp");
    const std::string map_view = source("/src/map_view.cpp") + source("/include/gobot/map_view.hpp");
    const std::string style = source("/apps/gobot_gui/simple_style.cpp");
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    const std::string shop_card = source("/apps/gobot_gui/simple_shop.cpp");
    if (doc.empty() || readme.empty() || pack.empty() || hunt_raw.empty() || macro_ini.empty() || goc.empty() ||
        hunter.empty() || drop.empty() || macro.empty() || world.empty() || more.empty() || spot.empty() ||
        map.empty() || map_view.empty() || style.empty() || gui.empty() || view.empty() || shop_card.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto n = [](auto v) { return std::to_string(v); };
    // A time as the notes write it: 300 ms "0,3", 150 "0,15", 1000 "1".
    const auto vi_s = [](std::uint64_t ms) {
        std::string s = std::to_string(ms / 1000);
        if (ms % 1000 != 0) {
            std::string frac = std::to_string(1000 + ms % 1000).substr(1);
            while (!frac.empty() && frac.back() == '0') frac.pop_back();
            s += "," + frac;
        }
        return s;
    };

    // ---- where: in section 5 under 11.12c's and 11.12b's, on 11.11c's; step 16 after step 15 ----
    const std::size_t sec5 = doc.find("5. CÓ GÌ MỚI, PHẦN NÀO LÀ THỬ NGHIỆM");
    const std::size_t e17 = doc.find("- Mới trong bản 2026.11.17 (");
    const std::size_t e16b = doc.find("- Mới trong bản 2026.11.16b (");
    const std::size_t e16 = doc.find("- Mới trong bản 2026.11.16 (");
    const std::size_t e15d = doc.find("- Mới trong bản 2026.11.15d (");
    const std::size_t e15c = doc.find("- Mới trong bản 2026.11.15c (");
    const std::size_t e15b = doc.find("- Mới trong bản 2026.11.15b (");
    const std::size_t e15 = doc.find("- Mới trong bản 2026.11.15 (");
    const std::size_t e14b = doc.find("- Mới trong bản 2026.11.14b (");
    const std::size_t e14 = doc.find("- Mới trong bản 2026.11.14 (");
    const std::size_t e13c = doc.find("- Mới trong bản 2026.11.13c (");
    const std::size_t e13b = doc.find("- Mới trong bản 2026.11.13b (");
    const std::size_t e13 = doc.find("- Mới trong bản 2026.11.13 (");
    const std::size_t e12c = doc.find("- Mới trong bản 2026.11.12c (");
    const std::size_t e12b = doc.find("- Mới trong bản 2026.11.12b (");
    const std::size_t e12 = doc.find("- Mới trong bản 2026.11.12 (đánh quái và di chuyển: quay mặt, rơi xuống, đi "
                                     "lạc, hai vạch trên tab Bản đồ; và Dọn rác, Tự mua bình đếm giờ từ lúc bật; theo "
                                     "điều bạn báo tối 11/11 và câu trả lời của bạn; chưa thử trên game thật):");
    const std::size_t e11c = doc.find("- Mới trong bản 2026.11.11c (");
    const std::size_t try16 = doc.find("Thử như bước 16 ở mục 6.");
    const std::size_t step15 = doc.find("15. Mua bình và dọn rác ở mọi mức máu, GoBot tự bấm bình (bản 2026.11.11b,");
    const std::size_t step16 = doc.find("16. Quay mặt, chỉ đánh trong vùng, rơi xuống và hai vạch (bản 2026.11.12, chưa "
                                        "thử trên game thật; cần Npcap và Vị trí từ gói tin có dấu; chỉ thử khi ngồi "
                                        "xem, tay gần F12):");
    const std::size_t revive = doc.find("- Tự hồi sinh: thử ở bản đồ dễ");
    const std::size_t r11 =
        readme.find("### The shop after the pass, HP during the bag jobs, and six more keys (2026.11.11)");
    const std::size_t r12 = readme.find("### Turns, the area, falls and two bars (2026.11.12)");
    const std::size_t rmacro = readme.find("## Keyboard macro");
    check(sec5 != npos && e17 != npos && e16b != npos && e16 != npos && e15d != npos && e15c != npos &&
              e15b != npos && e15 != npos && e14b != npos && e14 != npos && e13c != npos && e13b != npos &&
              e13 != npos && e12c != npos && e12b != npos && e12 != npos && e11c != npos &&
              doc.find("- Mới trong bản", sec5) == doc.find("- Mới trong bản 2026.11.24 (") && doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.24 (") + 1) == doc.find("- Mới trong bản 2026.11.17c (") && doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.17c (") + 1) == doc.find("- Mới trong bản 2026.11.17b (") &&
              doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.17b (") + 1) == e17 && doc.find("- Mới trong bản", e17 + 1) == e16b &&
              doc.find("- Mới trong bản", e16b + 1) == e16 &&
              doc.find("- Mới trong bản", e16 + 1) == e15d &&
              doc.find("- Mới trong bản", e15d + 1) == e15c && doc.find("- Mới trong bản", e15c + 1) == e15b &&
              doc.find("- Mới trong bản", e15b + 1) == e15 && doc.find("- Mới trong bản", e15 + 1) == e14b &&
              doc.find("- Mới trong bản", e14b + 1) == e14 &&
              doc.find("- Mới trong bản", e14 + 1) == e13c && doc.find("- Mới trong bản", e13c + 1) == e13b &&
              doc.find("- Mới trong bản", e13b + 1) == e13 && doc.find("- Mới trong bản", e13 + 1) == e12c &&
              doc.find("- Mới trong bản", e12c + 1) == e12b &&
              doc.find("- Mới trong bản", e12b + 1) == e12 &&
              doc.find("- Mới trong bản", e12 + 1) == e11c && try16 != npos && e12 < try16 && try16 < e11c &&
              step15 != npos && step16 != npos && revive != npos && step15 < step16 && step16 < revive &&
              r11 != npos && r12 != npos && rmacro != npos && r11 < r12 && r12 < rmacro &&
              has(readme, "Since 2026.11.12 (not yet run on the game).") &&
              has(pack, "Turns, the area, falls and two bars (2026.11.12, not yet tried on the game)."),
          "DOC-TRUOC.txt: the 2026.11.12 entry in section 5 under 11.12b's alone, on top of 11.11c's, not yet tried "
          "on the game, its live test step 16 after step 15; README's section after 11.11's, and START-HERE, say it "
          "too");
    // 2026.11.12b's (the charms, drops, the quick quest and popups at any HP):
    // the owner's words, not yet tried on the game, in all three.
    check(has(doc, "- Mới trong bản 2026.11.12b (bùa, nhảy xuống/lên, Nhiệm vụ nhanh và Tự đóng") &&
              has(doc, "bạn; chưa thử trên game thật). Bạn báo: \"hiện tại còn lỗi máu thấp thì") &&
              has(readme, "### Charms, drops, the quick quest and popups at any HP (2026.11.12b)") &&
              has(readme, "Since 2026.11.12b (not yet run on the game).") &&
              readme.find("### Charms, drops, the quick quest and popups at any HP (2026.11.12b)") <
                  readme.find("### The bag jobs at any HP, and their own potions (2026.11.11b)") &&
              has(pack, "Charms, drops, the quick quest and popups at any HP (2026.11.12b, not yet"),
          "the 2026.11.12b entry, README's section on top of 11.11b's and START-HERE: the owner's words, not yet "
          "tried on the game");
    // 2026.11.12c's (the jumps past the two bars, the wall foot, the way back):
    // first in section 5, its step 17 after step 16, README's section after
    // 11.12's, START-HERE, the owner's words and the eight questions.
    {
        const std::size_t step17 = doc.find("17. Nhảy lên/xuống theo hai vạch và đường về vùng (bản 2026.11.12c, chưa "
                                            "thử trên game thật; cần Npcap và Vị trí từ gói tin có dấu; chỉ thử khi "
                                            "ngồi xem, tay gần F12):");
        const std::size_t try17 = doc.find("Thử như bước 17 ở mục 6.");
        const std::size_t r12c = readme.find("### Jumps past the two bars, the wall foot, and the way back (2026.11.12c)");
        const char* const questions[] = {
            "1. Quái ngang tầm và quái cao hơn vạch trên cùng trong tầm: đánh con gần hơn (mặc định) hay con ngang tầm "
            "trước?",
            "2. Nhảy thường đánh quái trên cao tối đa mấy lần/phút (mặc định 10)?",
            "3. Ở t53_s2, chỗ nào nhảy ↑ có thể rơi lên trên vùng thì không nhảy (mặc định) hay kéo vạch trên của vùng "
            "lên?",
            "4. Ctrl+↓ rơi thấp hơn quái nhưng còn trong một lần ↑: cho (mặc định) hay chỉ khi rơi đúng tầng quái?",
            "5. Đăng nhập lại hay rơi xuống dưới vùng (tới 600 px): bot tự nhận vùng và tìm đường về (mặc định)?",
            "6. Chưa tin bản đồ mà kẹt ngoài vùng: cho bot đi vài bước 80 px trên sàn đang đứng để tin bản đồ (mặc "
            "định)?",
            "7. Giữ skill macro khi đi tới chỗ rơi/nhảy của đường tìm quái và lúc đứng chờ nhảy (mặc định)?",
            "8. Ctrl+↑ ở chân bậc t49_s5: lên bục 1639 bên cạnh, hay lên tấm ván 1287 phía trên?",
            // The review of 2026.11.12c: the 650 px rule per key, asked.
            "9. Luật 650 px: giữ như bản này (↑ xét 245 px, ↑↑ 413 px, Ctrl+↑ 650 px; mặc định) hay quay lại 650 px "
            "cho mọi kiểu nhảy (ít chỗ nhảy hơn)?",
        };
        bool asked = true;
        for (const char* q : questions) asked = asked && doc.find(q, e12c) != npos && doc.find(q, e12c) < e12b;
        check(has(doc, "- Mới trong bản 2026.11.12c (nhảy lên/xuống theo hai vạch, nhảy Ctrl+↑ ở chân bậc, tự về "
                       "vùng đánh quái chắc hơn; theo điều bạn báo tối 12/11 và câu trả lời của bạn; chưa thử trên "
                       "game thật).") &&
                  has(doc, "\"cũng được nhưng mình thấy phần tự quay về vùng đánh quái khi ở ngoài hoạt động chưa ổn "
                           "đâu\"") &&
                  has(doc, "\"Chỉ Ctrl+↑ tại chỗ\"") && has(doc, "hoặc ngay dưới vùng (tới 600 px)") && asked &&
                  try17 != npos && e12c < try17 && try17 < e12b && step17 != npos && step16 < step17 &&
                  step17 < revive && r12c != npos && r12 < r12c && r12c < rmacro &&
                  has(readme, "Since 2026.11.12c (not yet run on the game).") &&
                  has(pack, "Jumps past the two bars, the wall foot and the way back (2026.11.12c, not yet tried on "
                            "the game)."),
              "the 2026.11.12c entry under 11.13's in section 5 with the owner's words and the nine questions, its step "
              "17 after step 16, README's section after 11.12's and START-HERE's paragraph -- not yet tried on the game");
    }
    // 2026.11.13's (a digit of the bars' figures missed): first in section 5,
    // README's section after 11.12c's, START-HERE -- not yet tried on the game.
    {
        const std::size_t r12c = readme.find("### Jumps past the two bars, the wall foot, and the way back (2026.11.12c)");
        const std::size_t r13 = readme.find("### HP and MP figures with a digit missed (2026.11.13)");
        check(has(doc, "- Mới trong bản 2026.11.13 (sửa lỗi đọc sót chữ số máu/mana trên thanh; theo") &&
                  has(doc, "chưa thử trên game thật). + Lỗi: khi số máu hay mana có hai chữ \"1\" liền nhau") &&
                  e13 < e12c && r12c != npos && r13 != npos && r12c < r13 && r13 < rmacro &&
                  has(readme, "Since 2026.11.13 (not yet run on the game).") &&
                  has(pack, "HP and MP figures with a digit missed (2026.11.13, not yet tried on the"),
              "the 2026.11.13 entry first in section 5, README's section after 11.12c's and START-HERE's paragraph -- "
              "not yet tried on the game");
    }
    // 2026.11.13b's (the shop right after the pass, the login's message):
    // first in section 5 with the users' words and the owner's answers,
    // README's section after 11.13's, START-HERE -- not yet tried on the game.
    {
        const std::size_t r13 = readme.find("### HP and MP figures with a digit missed (2026.11.13)");
        const std::size_t r13b = readme.find("### Buy right after the pass, and the game's message at login (2026.11.13b)");
        check(has(doc, "- Mới trong bản 2026.11.13b (mua bình ngay sau khi dọn rác, và tự đăng nhập lại khi game báo") &&
                  has(doc, "\"Thêm 1 checkbox \"Mua ngay sau khi dọn rác\": Bằng checkbox này thì không cần thiết định "
                           "thời gian mua HP/MP nữa, sẽ ngay lập tức thực hiện mua sau khi dọn rác xong\"") &&
                  has(doc, "\"khi auto login xong thì có thông báo dialog \"Tài khoản đã bị chặn\", cần ấn nút xác nhận "
                           "ở dialog này rồi thực hiện login lại\"") &&
                  has(doc, "\"Thử lại ngay, không giới hạn\"") && e13b < e13 && r13 != npos && r13b != npos &&
                  r13 < r13b && r13b < rmacro && has(readme, "Since 2026.11.13b (not yet run on the game).") &&
                  has(pack, "Buy right after the pass, and the game's message at login (2026.11.13b, not yet tried on "
                            "the game)."),
              "the 2026.11.13b entry first in section 5 with the users' words and the owner's answer, README's section "
              "after 11.13's and START-HERE's paragraph -- not yet tried on the game");
    }
    // 2026.11.13c's (the game at 1024x768, 1280x720 and 1366x768): first in
    // section 5 with the owner's words, README's section after 11.13b's,
    // START-HERE after 11.13b's paragraph -- not yet tried on the game.
    {
        const std::size_t r13b = readme.find("### Buy right after the pass, and the game's message at login (2026.11.13b)");
        const std::size_t r13c = readme.find("### The game at 1024x768, 1280x720 and 1366x768 (2026.11.13c)");
        const std::size_t p13b = pack.find("Buy right after the pass, and the game's message at login (2026.11.13b,");
        const std::size_t p13c = pack.find("The game at 1024x768, 1280x720 and 1366x768 (2026.11.13c, not yet tried on "
                                           "the game at 1280x720 and 1366x768).");
        check(has(doc, "- Mới trong bản 2026.11.13c (game cỡ 1024x768, 1280x720 và 1366x768; theo yêu cầu của bạn "
                       "ngày 13/11; chưa thử trên game thật ở 1280x720 và 1366x768).") &&
                  has(doc, "\"nhớ là cần làm cho đối ứng các độ phân giải: 1080 x 768, 1280 x 720, 1366 x 768\"") &&
                  has(doc, "\"Game không nằm trọn trên màn hình\"") && e13c < e13b && r13b != npos && r13c != npos &&
                  r13b < r13c && r13c < rmacro && has(readme, "Since 2026.11.13c (not yet run on the game).") &&
                  p13b != npos && p13c != npos && p13b < p13c,
              "the 2026.11.13c entry under 11.14's in section 5 with the owner's words, README's section after "
              "11.13b's and START-HERE's paragraph after 11.13b's -- not yet tried on the game at 1280x720 and "
              "1366x768");
    }
    // 2026.11.14's (the game's window in the corner after a login): first in
    // section 5 with the owner's two answers, README's section after 11.13c's,
    // START-HERE after 11.13c's paragraph, login.ini's key -- not yet tried.
    {
        const std::size_t r13c = readme.find("### The game at 1024x768, 1280x720 and 1366x768 (2026.11.13c)");
        const std::size_t r14 = readme.find("### The game's window in the corner after a login (2026.11.14)");
        const std::size_t p13c = pack.find("The game at 1024x768, 1280x720 and 1366x768 (2026.11.13c,");
        const std::size_t p14 = pack.find("The game's window in the corner after a login (2026.11.14, not yet tried on "
                                          "the game).");
        check(has(doc, "- Mới trong bản 2026.11.14 (tự kéo game lên góc trái trên sau khi tự đăng nhập; theo câu trả "
                       "lời của bạn ngày 14/11; chưa thử trên game thật).") &&
                  has(doc, "\"Mặc định sẽ kéo lên góc trên bên trái đi\"") &&
                  has(doc, "\"chỉ kéo lên sau khi auto login thôi\"") && has(doc, "move_game_to_corner") &&
                  e14 < e13c && r13c != npos && r14 != npos && r13c < r14 && r14 < rmacro &&
                  has(readme, "Since 2026.11.14 (not yet run on the game).") && p13c != npos && p14 != npos &&
                  p13c < p14,
              "the 2026.11.14 entry first in section 5 with the owner's answers, README's section after 11.13c's and "
              "START-HERE's paragraph after 11.13c's -- not yet tried on the game");
    }
    // 2026.11.14b's (the shop after a pass, ESC after 10 s stuck, the game's
    // message after an invite): first in section 5 with the owner's three
    // reports and his word on 00, README's section after 11.14's, START-HERE
    // after 11.14's paragraph -- not yet tried on the game.
    {
        const std::size_t r14 = readme.find("### The game's window in the corner after a login (2026.11.14)");
        const std::size_t r14b = readme.find("### The shop after a pass, ESC after 10 s stuck, and the game's message "
                                             "after an invite (2026.11.14b)");
        const std::size_t p14 = pack.find("The game's window in the corner after a login (2026.11.14,");
        const std::size_t p14b = pack.find("The shop after a pass, ESC after 10 s stuck, and the game's message after "
                                           "an invite (2026.11.14b, not yet tried on the game).");
        const char* const said[] = {
            "Khi dọn rác xong thì tắt luôn túi đồ khiến xử lý mua HPMP bị dừng => trước khi thực hiện xử lý mua HPMP "
            "cần check xem túi đồ có đang mở không, nếu có thì thực hiện luôn, còn không thì mở túi đồ lên và thao tác "
            "mua",
            "Khi thao tác mua nếu không may click vào chỗ khác sẽ khiến không ấn được số 00 để mua. Trường hợp bị dừng "
            "quá 10s thì tắt toàn bộ hiển thị shop, túi đồ trên màn hình bằng phím ESC. Xử lý này thực hiện cho cả "
            "khi hộp nhập số lượng không biến mất",
            // As sent ("Về 2 : hắc chắn ..."), its first letter given back
            // and shown so (the review of 2026.11.14b).
            "[c]hắc chắn khi dialog mua HPMP hiện ra và không click vào đâu nữa thì sẽ có thể ấn được 00 luôn",
            "Khi thực hiện auto pt, nếu chẳng may pt trúng người đã vào pt thì sẽ bị hiện dialog mà không tự động tắt "
            "=> chỉ cần ấn enter hoặc click xác nhận trong dialog",
        };
        bool quoted = true;
        for (const char* s : said)
            quoted = quoted && has(doc, "\"" + std::string(s) + "\"") && has(readme, "*\"" + std::string(s) + "\"*");
        check(has(doc, "- Mới trong bản 2026.11.14b (mua bình ngay sau khi dọn rác, kẹt quá 10 giây thì bấm ESC, "
                       "và thông báo của game khi mời tổ đội; theo ba lỗi bạn báo ngày 14/11; chưa thử trên game "
                       "thật).") &&
                  quoted && has(doc, "Dùng bùa không bao giờ bấm ESC hay Enter.") &&
                  has(doc, "GoBot bấm Xác Nhận để tắt (không bấm Enter") && e14b < e14 && r14 != npos &&
                  r14b != npos && r14 < r14b && r14b < rmacro &&
                  has(readme, "Since 2026.11.14b (not yet run on the game).") && p14 != npos && p14b != npos &&
                  p14 < p14b,
              "the 2026.11.14b entry first in section 5 with the owner's three reports and his word on 00, README's "
              "section after 11.14's and START-HERE's paragraph after 11.14's -- not yet tried on the game");
        // What its review changed (2026-11-15): ESC on the bin's question
        // said to be expected to delete nothing, not known; the owner asked
        // to try it; 21:37's pictures asked for; the party's minute's pause
        // for a click refused too; no ESC with the character down; and the
        // living reference's Khác clicks.
        check(has(doc, "ESC được cho là không xoá đồ (chỉ Xác Nhận mới xoá), nhưng điều này chưa thấy trên game "
                       "thật: bạn thử tay giúp một lần") &&
                  !has(doc, "ESC không xoá đồ") &&
                  has(doc, "*_not_in_bin.png và *_tab_unsure_shop_other_tab.png") &&
                  has(doc, "hoặc không bấm được vì một việc khác của GoBot đang giữ game") &&
                  has(doc, "GoBot bấm Xác Nhận tắt nó trước rồi mới mời") && has(doc, "Nhân vật đã chết thì không bấm ESC.") &&
                  has(readme, "ESC is taken to delete nothing (only Xác Nhận deletes) -- not yet seen on the live client") &&
                  !has(readme, "ESC deletes nothing") && !has(readme, "The pass worker sets `set_hold_check` too") &&
                  has(readme, "clicked once more (the shop's Khác up to four times in all, since 2026.11.14b)."),
              "the 2026.11.14b entry after its review: ESC on the bin's question expected, not known, to delete "
              "nothing, the owner asked to try it; 21:37's pictures asked for; the party's pause for a refused click; "
              "no ESC with the character down; Getting started's Khác clicks");
    }
    // 2026.11.15b's (the owner, 2026-11-15: the shop and the party again after
    // 30 s, not 10 minutes): the two waits, the lines that use them, and the
    // entry first in section 5, README's section after 11.15's, START-HERE's
    // paragraph after 11.15's -- not yet tried on the game.
    {
        const std::size_t r15 = readme.find("### One lost key press, pauses instead of \"off until the next start\", "
                                            "and the way back asked sooner (2026.11.15)");
        const std::size_t r15b = readme.find("### The shop and the party again after 30 s (2026.11.15b)");
        const std::size_t p15 =
            pack.find("Walls, a lost key press and the way back (2026.11.15, not yet tried on the game).");
        const std::size_t p15b =
            pack.find("The shop and the party again after 30 s (2026.11.15b, not yet tried on the game).");
        const std::string said = "về việc thực hiện lại xử lý thì giờ mua HPMP nếu bị kẹt sẽ thực hiện lại sau 10 "
                                 "phút tương tự với việc tổ đội. Cần thực hiện lại sau 30s chứ không phải 10 phút";
        // The party's 30 s gave way to InviteTries' minute -- ten after two in
        // a row -- in 2026.11.17b, the owner's later choice (A).
        // The second trouble in a row dropped the plan until 2026.11.24; since,
        // every one is tried again 30 s on (kShopTroublesToDrop gone).
        check(bag::kShopTroubleAgainMs == 30'000 && InviteTries::kFailHoldMs == 60'000 &&
                  InviteReply::kStuckMs == 60'000 &&
                  has(readme, "`kShopTroublesToDrop`-th trouble in a row dropped what was left of the plan until "
                              "the next until 2026.11.24") &&
                  has(gui, "else if (end == bag::ShopEnd::Trouble) shop_due_ms = now_ms() + bag::kShopTroubleAgainMs;") &&
                  !has(gui, "shop_due_ms = minutes(10);") && !has(gui, "InviteReply::kHoldMs") &&
                  has(gui, "\"Party: invite to %s did not lead to a join (%s) -- %d in a row; not invited again for %d s\",") &&
                  has(gui, "minutes_vi(o.hold_ms) + \" nữa mới mời lại\"") &&
                  !has(gui, "10 phút nữa mới mời lại") && e15b != npos && e15 != npos && e15b < e15 &&
                  has(doc, "- Mới trong bản 2026.11.15b (mua bình và mời tổ đội thử lại sau 30 giây, không phải 10 "
                           "phút; theo lời bạn ngày 15/11; chưa thử trên game thật).") &&
                  has(doc, "\"" + said + "\"") && has(readme, "*\"" + said + "\"*") &&
                  has(doc, "Lượt có lỗi thì chờ 30 giây (trước bản 2026.11.15b là 10 phút)") &&
                  r15 != npos && r15b != npos && rmacro != npos && r15 < r15b && r15b < rmacro &&
                  has(readme, "Since 2026.11.15b (not yet run on the game).") && p15 != npos && p15b != npos &&
                  p15 < p15b,
              "2026.11.15b: a shop round in trouble tried again 30 s on, and a player whose invite the game answered "
              "held off 30 s -- not 10 minutes, the owner's word (a minute since 2026.11.17b, ten after two in a row, "
              "the owner's later choice); the second trouble in a row dropped the plan until 2026.11.24, said so; "
              "the entry first in section 5, README's section after 11.15's, START-HERE's paragraph after 11.15's");
    }

    // ---- the owner's report and the eight answers, as written ----
    const char* const report[] = { "Quay mặt sai, đánh hụt", "Rơi xuống, không về chỗ", "Đi lung tung, đi lạc xa" };
    const char* const answers[] = {
        "C, V hoặc Space",
        "GoBot giữ skill lúc nguy hiểm",
        "Chỉ đánh quái trong vùng",
        "Tìm cách quay lại chỗ cũ, khi bị rơi xuống thì tìm platform có thể nhảy lên theo map rồi thực hiện nhảy "
        "cao nếu cần",
        "không, nhân vật bị rơi xuống vực nên bị di chuyển lên trên đỉnh",
        "nó vẫn thuộc vùng đánh quái nhưng bot không biết cần cho nhảy xuống. Nên thể hiện 2 thanh trên và dưới "
        "trên bản đồ để có thể thiết định xem bot sẽ nhảy xuống ở mức bao nhiêu pixel.",
        "Ngoài ra hiện tại đang giới hạn không cho bot nhảy xuống hoặc nhảy lên nếu không quá 30 pixel, cần bỏ giới "
        "hạn này",
        "vậy thêm 1 chỉnh sửa vào lần fix lần này, đó là khi bật thì bắt đầu đếm thời gian. Lúc nào tới thời gian "
        "thì mới bắt đầu thực hiện",
    };
    bool quoted = has(doc, "Bạn báo tối 11/11, về đánh quái và di chuyển: \"Quay mặt sai, đánh hụt\", \"Rơi xuống, "
                           "không về chỗ\", \"Đi lung tung, đi lạc xa\".");
    for (const char* r : report) quoted = quoted && has(readme, "*\"" + std::string(r) + "\"*");
    for (const char* a : answers)
        quoted = quoted && has(doc, "\"" + std::string(a) + "\"") && has(readme, "*\"" + std::string(a) + "\"*");
    check(quoted && has(doc, "1. Phím lướt: \"C, V hoặc Space\".") &&
              has(doc, "2. Kỹ năng: \"GoBot giữ skill lúc nguy hiểm\".") &&
              has(doc, "3. Quái ngoài vùng mà trong tầm: \"Chỉ đánh quái trong vùng\".") &&
              has(doc, "5. Lần nhân vật lên đỉnh ĐồiĐen[5] lúc 21:50:37: \"không, nhân vật bị rơi xuống vực") &&
              has(doc, "6. Vùng hai tầng ở PhòngSinhTử[4]: \"nó vẫn thuộc vùng đánh quái") &&
              has(doc, "8. Dọn rác và mua bình khi bấm BẮT ĐẦU: \"vậy thêm 1 chỉnh sửa"),
          "the owner's three symptoms and eight answers of 2026-11-11 and -12, quoted as written in DOC-TRUOC.txt "
          "and README");

    // ---- the turns: confirmed by the packets, and no back and forth ----
    check(Hunter::kTurnTapMs == 80 && Hunter::kFacingMissedMs == 300 && Hunter::kTurnQuietMs == 150 &&
              Hunter::kTurnCapMs == 1000 && Hunter::kTurnSlackPx == 25 && Hunter::kTurnStayMs == 250 &&
              has(doc, "bot bấm quay lại sau " + vi_s(Hunter::kFacingMissedMs) + " giây (trước là 0,5 giây), khi đã " +
                           vi_s(Hunter::kTurnQuietMs) + " giây không có chiêu nào của bạn ra; quá " +
                           vi_s(Hunter::kTurnCapMs) + " giây thì coi như đã quay. Phím quay giữ " +
                           n(Hunter::kTurnTapMs) + " ms (trước là 60 ms).") &&
              has(doc, "thì chỉ quay khi nó ở phía sau quá \"Hạn chế quay người\" (facing_tolerance, của bạn 12 px) "
                       "thêm " + n(Hunter::kTurnSlackPx) + " px suốt " + vi_s(Hunter::kTurnStayMs) + " giây") &&
              // The band a wait, not a facing (the review of 2026.11.12).
              Hunter::kTurnBandStayMs == 600 &&
              has(doc, "hoặc ở phía sau quá \"Hạn chế quay người\" suốt " + vi_s(Hunter::kTurnBandStayMs) +
                           " giây (con quái đứng sau lưng để đánh") &&
              has(readme, "or past `facing_tolerance` at all for `kTurnBandStayMs` (" + n(Hunter::kTurnBandStayMs) +
                              " ms)") &&
              has(hunt_ini, "or past it at all for " + vi_s(Hunter::kTurnBandStayMs).replace(1, 1, ".") +
                                " s (one that stays behind to hit)") &&
              has(goc, "or past `facing_tolerance` for " + vi_s(Hunter::kTurnBandStayMs).replace(1, 1, ".") +
                           " s (one that stays behind)") &&
              has(readme, "`Hunter::kFacingMissedMs` (" + n(Hunter::kFacingMissedMs) + " ms; 500 before)") &&
              has(readme, "`kTurnQuietMs` (" + n(Hunter::kTurnQuietMs) + " ms)") &&
              has(readme, "`kTurnCapMs` (" + n(Hunter::kTurnCapMs) + " ms). `kTurnTapMs` is " + n(Hunter::kTurnTapMs) +
                              " ms (60 before)") &&
              has(readme, "`facing_tolerance` + `kTurnSlackPx` (" + n(Hunter::kTurnSlackPx) + ") for `kTurnStayMs` (" +
                              n(Hunter::kTurnStayMs) + " ms)") &&
              has(pack, "(" + n(Hunter::kTurnCapMs / 1000) + " s at most; the tap is " + n(Hunter::kTurnTapMs) +
                            " ms)") &&
              has(hunt_ini, "the same one only once it has stayed the other side more than " +
                                n(Hunter::kTurnSlackPx) + " px past it for a quarter of a second") &&
              has(goc, "the swing waits for the game's report of the turn (" + n(Hunter::kTurnCapMs / 1000) +
                           " s at most, the tap tried again after 0.3 s)") &&
              has(hunter, "\"%d turns (%d taps; %d confirmed by the packets%s; %d not confirmed in %llu ms; %d facings "
                          "put right \"") &&
              has(hunter, "\"by the packets; keys the game missed: %d turn taps, %d walk holds)\"") &&
              has(hunter, "\", the report %llu ms after its tap on average, %llu at most\"") &&
              has(doc, "\"N turns (T taps; C confirmed by the packets, the report A ms after its tap on average, M at "
                       "most; K not confirmed in " + n(Hunter::kTurnCapMs) + " ms; P facings put right by the packets; "
                       "keys the game missed: X turn taps, Y walk holds)\"") &&
              has(readme, "`N turns (T taps; C confirmed by the packets, the report A ms after its tap on average, M at "
                          "most; K not confirmed in 1000 ms; P facings put right by the packets; keys the game missed: "
                          "X turn taps, Y walk holds)`"),
          "the turn: a tap of " + n(Hunter::kTurnTapMs) + " ms, tapped again " + n(Hunter::kFacingMissedMs) +
              " ms on after " + n(Hunter::kTurnQuietMs) + " ms with no cast, taken as made at " +
              n(Hunter::kTurnCapMs) + " ms; a target behind past facing_tolerance + " + n(Hunter::kTurnSlackPx) +
              " px for " + n(Hunter::kTurnStayMs) + " ms -- the notes, hunt.ini and GoC's row, and the minute line "
              "as hunter.cpp writes it");

    // ---- the attack's reach, and a skill's range on its own timer ----
    HunterConfig shipped_hunt;
    const bool hunt_loaded = shipped_hunt.load(std::string(GOBOT_SOURCE_DIR) + "/configs/hunt.ini");
    check(Hunter::kAttackReachSlackPx == 24 && CastMatch::kReachCasts == 5 && HunterConfig{}.attack_range_px == 0 &&
              hunt_loaded && shipped_hunt.attack_range_px == 0 && has(hunt_raw, "\nattack_range_px=0") &&
              has(doc, "(sau " + n(CastMatch::kReachCasts) + " lần ra chiêu có tên con quái) cộng " +
                           n(Hunter::kAttackReachSlackPx) + " px, hay số attack_range_px mới trong hunt.ini (0 = theo "
                           "tầm đo được)") &&
              has(readme, "`CastMatch::kReachCasts` (" + n(CastMatch::kReachCasts) + ")") &&
              has(readme, "`kAttackReachSlackPx` (" + n(Hunter::kAttackReachSlackPx) + ")") &&
              has(hunt_ini, "plus " + n(Hunter::kAttackReachSlackPx) + " px -- and at any distance until " +
                                n(CastMatch::kReachCasts) + " of them have been seen") &&
              has(hunter, "\"; own attack key let be %d times beyond its reach (%d px), %d at an edge as the "
                                 "dash's\"") &&
              has(doc, "\"; own attack key let be N times beyond its reach (L px), M at an edge as the dash's\"") &&
              has(readme, "`; own attack key let be N times beyond its reach (L px), M at an edge as the dash's`") &&
              has(doc, "\"Tầm (px)\" của từng kỹ năng (cửa sổ Kỹ năng, nút \"Chi tiết…\") nay có tác dụng cả khi \"Chỉ "
                       "khi đang đánh quái\" không có dấu, lúc bot đang đánh một con quái") &&
              has(macro_ini, "Since 2026.11.12 it counts on the key's own timer too -- always_timer, or "
                             "skills_follow_fight off -- while the hunter fights a monster"),
          "the hunter's own attack key within attack_range_px (0 as shipped) or the reach learned from " +
              n(CastMatch::kReachCasts) + " named casts plus " + n(Hunter::kAttackReachSlackPx) + " px, and range_px "
              "on a key's own timer -- as the notes and the ini comments say");

    // ---- the holds, and the dash ----
    check(kZoneOutsideBackPx == 60 &&
              has(macro, "\"Skills: %llu held for the hunter -- %llu turning, %llu a drop settling or in its "
                                "fall, \"") &&
              has(macro, "\"%llu nothing to fight in the area, %llu outside the area or stranded, %llu a leg of the "
                         "seek's \"") &&
              has(doc, "\"Skills: N held for the hunter -- T turning, D a drop settling or in its fall, I nothing to "
                       "fight in the area, O outside the area or stranded, L a leg of the seek's way, J a jump up; K of "
                       "the dash's key near an edge\"") &&
              has(readme, "`Skills: N held for the hunter -- T turning, D a drop settling or in its fall, I nothing to "
                          "fight in the area, O outside the area or stranded, L a leg of the seek's way, J a jump up; K "
                          "of the dash's key near an edge`") &&
              has(doc, "ở ngoài vùng quá " + n(kZoneOutsideBackPx) + " px mà không đi về được") &&
              has(doc, "macro giữ phím kỹ năng, cả phím \"Luôn bấm theo giờ\" (bình và buff vẫn bấm)") &&
              has(macro_ini, "Since 2026.11.12 (the owner's \"GoBot giữ skill lúc nguy hiểm\"), with the hunter "
                             "running, on or off, the skill keys -- always_timer and the [Sequence] steps too, never "
                             "potions or buffs -- are held") &&
              has(hunt_ini, "Since 2026.11.12 (the owner's Q2) the macro also holds its skill keys -- never potions or "
                            "buffs --"),
          "the skill keys held -- turning, a drop, nothing to fight, outside -- never potions or buffs, and the "
          "minute's Skills line, as macro.cpp writes it");
    check(PacketWorld::kNoWalkMovePx == 100 && PacketWorld::kDashLearnMoves == 3 && PacketWorld::kDashLearnPct == 70 &&
              PacketWorld::kMoveLinesPerMinute == 30 && Hunter::kDashGuessPx == 320 &&
              has(world, "\"Positions: moved %+d px with no walk, %d,%d -> %d,%d by 0x%04X, facing %s; %s\"") &&
              has(world, "\"your cast of skill %u %llu ms before\"") &&
              has(world, "\"Positions: the dash is skill %u, about %d px -- %d of the last %d moves of yours with no "
                         "walk \"") &&
              has(world, "\"Dash (packets): %d move%s of yours with no walk this minute (%d px\"") &&
              has(world, "\"); the dash is skill %u, %s, about %d px (%d of %d)\"") &&
              // 2026.11.17: no key guessed while none is learned. The notes'
              // own words below are the docs' to change with it.
              has(world, "\"); the dash not learned yet -- no key held for it\"") &&
              has(world, "\"after its casts; no key is held for a dash until one is learned\"") &&
              has(more, "out = vk != 0 ? \"its key \" + InputManager::key_name(vk)") &&
              has(more, "dash_vk != 0 ? \"key \" + InputManager::key_name(dash_vk)") &&
              has(doc, "Mỗi lần nhân vật dời ít nhất " + n(PacketWorld::kNoWalkMovePx) + " px mà không đi bộ") &&
              has(doc, "(tối đa " + n(PacketWorld::kMoveLinesPerMinute) + " dòng mỗi phút)") &&
              has(doc, "Khi ít nhất " + n(PacketWorld::kDashLearnMoves) + " lần như vậy, và từ " +
                           n(PacketWorld::kDashLearnPct) + "% số lần trở lên") &&
              // 2026.11.12's words kept as that release's, with 2026.11.17's
              // correction beside them: no key guessed since.
              has(doc, "Chưa học được thì C, V và Space đều coi là phím lướt, dài " + n(Hunter::kDashGuessPx) +
                           " px. (Từ bản 2026.11.17: chưa học được thì không phím nào bị giữ vì lướt; " +
                           n(Hunter::kDashGuessPx) + " px chỉ còn là độ dài của phím lướt đã học mà chưa đo được.)") &&
              has(doc, "\"Positions: moved +214 px with no walk, A,B -> C,D by 0x0027, facing right; your cast of "
                       "skill S M ms before; its key C; GoBot: ...\"") &&
              has(doc, "\"Positions: the dash is skill S, about 214 px -- ...\"") &&
              has(doc, "\"Dash (packets): N moves of yours with no walk this minute (N px); the dash is skill S, key C, "
                       "about 214 px (A of B)\"") &&
              has(readme, "`PacketWorld::kNoWalkMovePx` (" + n(PacketWorld::kNoWalkMovePx) + ")") &&
              has(readme, "`kDashLearnMoves` (" + n(PacketWorld::kDashLearnMoves) + ")") &&
              has(readme, "`kDashLearnPct` (" + n(PacketWorld::kDashLearnPct) + "%)") &&
              has(readme, "`Hunter::kDashGuessPx` (" + n(Hunter::kDashGuessPx) + ")") &&
              has(readme, "`Positions: moved +214 px with no walk, A,B -> C,D by 0x0027, facing right; your cast of "
                          "skill S M ms before; its key C; GoBot: …`") &&
              has(readme, "`Dash (packets): N moves of yours with no walk this minute (N px); the dash is skill S, key "
                          "C, about 214 px (A of B)`") &&
              has(pack, "until then C, V and Space -- since 2026.11.17 no key until one is learned)") &&
              has(readme, "since 2026.11.17 no key until one is learned, `kDashGuessPx` only the length of a "
                          "learned dash with none measured") &&
              has(hunt_ini, "chưa học được thì C, V và Space; từ bản 2026.11.17 chưa học được thì không phím nào)") &&
              has(macro_ini, "C, V and Space until then; since 2026.11.17 no key until one is learned --"),
          "the dash: a move of " + n(PacketWorld::kNoWalkMovePx) + " px or more with no walk logged, learned from " +
              n(PacketWorld::kDashLearnMoves) + " votes and " + n(PacketWorld::kDashLearnPct) + "%, C, V and Space "
              "at " + n(Hunter::kDashGuessPx) + " px until then (no key since 2026.11.17, the notes say) -- and its "
              "lines as packet_world.cpp writes them");

    // ---- the area kept, the way back, standing stranded ----
    check(Hunter::kSpotKeepPx == 60 && kZoneTravelJumpsMax == 4 &&
              has(doc, "(con đang đánh vẫn được giữ tới khi nó ra ngoài vùng quá " + n(Hunter::kSpotKeepPx) + " px)") &&
              has(spot, "\" — bot vẫn đánh quái trong vùng tới trong tầm\" + reach + \"; hết quái thì đi bộ về \" + way + "
                        "\".\";") &&
              has(doc, "\"... bot vẫn đánh quái trong vùng tới trong tầm (tầm đánh N px); hết quái thì đi bộ về X, "
                       "Y.\"") &&
              has(doc, "và nhảy lên ↑ (khoảng 133 px), ↑↑ (khoảng 280 px), Ctrl+↑ (khoảng 432 px) \"Theo GoC\", từng "
                       "tầng một, tối đa " + n(kZoneTravelJumpsMax) + " lần nhảy, tới bục của chỗ về.") &&
              // Ctrl+Down on the way back, Nhảy xuống ticked (the review of
              // 2026.11.12): the drop's own rules, kZoneTravelDropsMax.
              kZoneTravelDropsMax == 8 && has(doc, "Đường về không bao giờ đi qua cửa.") &&
              has(doc, "thì đường về còn nhảy xuống Ctrl+↓ qua bục, theo luật của Nhảy xuống: không gần cửa, không "
                       "xuống vực, không ở chỗ Ctrl+↓ đã không xuống được, tối đa " + n(kZoneTravelDropsMax) +
                           " lần một đường về") &&
              has(drop, "\"Hunter: drop (Ctrl+Down) on the way back: %d px down -- you at %d,%d on floor %d of %s "
                        "(trusted: %s), \"") &&
              has(doc, "\"Hunter: drop (Ctrl+Down) on the way back: N px down -- ...\"") &&
              has(hunter, "\"no way back the map shows but by Ctrl+Down, and Nhảy xuống is off\"") &&
              has(doc, "\"... no way back the map shows but by Ctrl+Down, and Nhảy xuống is off: standing where it is "
                       "...\"") &&
              has(readme, "`kZoneTravelDropsMax` (" + n(kZoneTravelDropsMax) + ")") &&
              has(readme, "`kZoneTravelJumpsMax`, " + n(kZoneTravelJumpsMax)) &&
              has(hunt_ini, "and with jump_up up to " + n(kZoneTravelJumpsMax) + " jumps up; with drop_down, Ctrl + "
                            "Down through platforms by the drop's own rules (no gate near, never into a pit), " +
                                n(kZoneTravelDropsMax) + " at most -- never through a gate") &&
              has(hunt_ini, "a monster outside the area is never faced, fought or walked to, in reach or not.") &&
              has(hunt_ini, "Từ bản 2026.11.12 chỉ đánh quái trong vùng: quái ở ngoài vùng thì không đánh, cả khi ở "
                            "trong tầm đánh") &&
              has(spot, "\" — không có đường về vùng: bot đứng yên ở đây, không đánh quái ngoài vùng, kỹ năng tạm dừng "
                        "\"") &&
              has(spot, "\"(bình và buff vẫn bấm), tới khi có đường về.\"") &&
              has(doc, "\"... không có đường về vùng: bot đứng yên ở đây, không đánh quái ngoài vùng, kỹ năng tạm dừng "
                       "(bình và buff vẫn bấm), tới khi có đường về.\"") &&
              has(spot, "\" px, quá một lần nhảy: bot không đi về được, đứng yên ở đây. Tự đưa nhân vật về, \"") &&
              has(doc, "(\"... quá một lần nhảy: bot không đi về được, đứng yên ở đây.\")") &&
              has(spot, "\" không có đường đi bộ, rơi xuống hay nhảy lên nào về vùng: bot đứng yên ở đây. Tự \"") &&
              has(doc, "\"... không có đường đi bộ, rơi xuống hay nhảy lên nào về vùng: bot đứng yên ở đây. ...\"") &&
              // 2026.11.12c: each key's reason at the stand, " (here: ...)", after the why.
              has(hunter, "\"Hunter: %d px outside the hunting area at %d,%d -- %s%s%s%s: standing where it is until a "
                          "way back \"") &&
              has(hunter, "\"shows (no walk, nothing outside the area fought, the macro's skills held)\"") &&
              has(doc, "\"Hunter: N px outside the hunting area at X,Y -- ...: standing where it is until a way back "
                       "shows (no walk, nothing outside the area fought, the macro's skills held)\"") &&
              has(readme, "`Hunter: N px outside the hunting area at X,Y -- <why>: standing where it is until a way "
                          "back shows (no walk, nothing outside the area fought, the macro's skills held)`") &&
              has(goc, "since 2026.11.12, with the map's file trusted, over the map's other floors too -- walking, off "
                       "floors' ends, and jumping up -- never through a gate"),
          "only monsters in the area, the target kept " + n(Hunter::kSpotKeepPx) + " px past its edge; the way back "
          "over other floors with " + n(kZoneTravelJumpsMax) + " jumps up and, Nhảy xuống ticked, " +
              n(kZoneTravelDropsMax) + " drops at most, never a gate; no way, standing -- the card's and the log's "
              "words as simple_spot.cpp, hunter.cpp and hunter_drop.cpp say them");

    // ---- the pit: the put-back, the walks stopped, the gap left ----
    check(Hunter::kPutBackPx == 1000 && Hunter::kPutBackAgreePx == 400 && Hunter::kPutBackWaitMs == 3000 &&
              has(doc, "gói tin báo nhân vật cao hơn lần trước quá " + n(Hunter::kPutBackPx) + " px mà không đi, không "
                       "nhảy, không qua cửa") &&
              has(doc, "(trong " + n(Hunter::kPutBackAgreePx) + " px) hay " + n(Hunter::kPutBackWaitMs / 1000) +
                           " giây") &&
              has(spot, "\"Chỗ đánh quái: gói tin vừa báo nhân vật ở rất xa chỗ cũ mà không đi, không nhảy, không \"") &&
              has(spot, "\"qua cửa (rơi xuống vực thì game đưa lên trên) — bot đứng yên, chờ gói tin sau báo đúng \"") &&
              has(spot, "\"chỗ rồi mới tính đường về.\"") &&
              has(doc, "(thẻ: \"Chỗ đánh quái: gói tin vừa báo nhân vật ở rất xa chỗ cũ mà không đi, không nhảy, không "
                       "qua cửa (rơi xuống vực thì game đưa lên trên) — bot đứng yên, chờ gói tin sau báo đúng chỗ rồi "
                       "mới tính đường về.\")") &&
              has(hunter, "\"Hunter: the game put you at %d,%d, %d px from %d,%d (%d px %s) with no walk, jump or map "
                          "entry \"") &&
              has(doc, "\"Hunter: the game put you at X,Y, N px from A,B (N px up) ...\"") &&
              has(readme, "`Hunter::kPutBackPx` (" + n(Hunter::kPutBackPx) + ")") &&
              has(readme, "`kPutBackAgreePx` (" + n(Hunter::kPutBackAgreePx) + ")") &&
              has(readme, "`kPutBackWaitMs` (" + n(Hunter::kPutBackWaitMs / 1000) + " s)") &&
              has(hunt_ini, "A rise of more than " + n(Hunter::kPutBackPx) + " px with no walk, jump or map entry is a "
                            "fall into a pit the game put back up top") &&
              has(pack, "A rise of more than " + n(Hunter::kPutBackPx) + " px with no walk, jump or door is a fall into "
                        "a pit put back up top") &&
              has(hunter, "\"Hunter: not walking %s -- the floor ends at %d,%d with nothing of the map file \"") &&
              has(hunter, "\"; walks stopped at a pit's edge %d\"") &&
              has(doc, "\"Hunter: not walking left -- the floor ends at X,Y with nothing of the map file under it (a "
                       "pit ...)\"") &&
              has(doc, "\"; walks stopped at a pit's edge N\"") &&
              has(doc, "Đỉnh ĐồiĐen[5] (t62_s5): sau khi game đưa nhân vật lên đỉnh (3166,723), mọi đường đi bộ và rơi "
                       "xuống đều dừng ở một bục dài ngang cả bản đồ") &&
              !has(doc, "Chưa làm được: đường về không dùng Ctrl+↓.") &&
              has(readme, "From the top of `t62_s5` (3166,723) every walk and fall ends on the platform spanning the "
                          "map at about y 3000: with *Nhảy xuống* ticked the way back drops through it") &&
              !has(readme, "Not done: no Ctrl+Down leg on a way back"),
          "a rise past " + n(Hunter::kPutBackPx) + " px with nothing to make it: a pit's put-back, agreed within " +
              n(Hunter::kPutBackAgreePx) + " px or after " + n(Hunter::kPutBackWaitMs) + " ms; walks stopped at a "
              "pit's edge; the top of t62_s5 left by Ctrl+Down with Nhảy xuống ticked -- the words as the code has "
              "them");

    // ---- the review of 2026.11.12: the long wait (since 2026.11.24 spot_retry_ms and a new round), the far
    // below, y 0 kept, the spawn, the turn's pit ----
    check(Hunter::kSpotMaxTries == 3 && Hunter::kSpotFarBelowPx == 461 &&
              has(hunter, "\"Hunter: walks back given up on %d times in a row -- the next round of tries in %llu s, "
                          "or sooner \"") &&
              has(hunter, "\"Hunter: trying the walk back again -- %s (at %d,%d): a new round of %d tries\"") &&
              !has(hunter, "kSpotGaveUpRetryMinMs") &&
              has(doc, "\"Hunter: walks back given up on 3 times in a row -- the next round of tries in N s\"") &&
              has(readme, "the round's wait is `spot_retry_ms` (30 s), then a new round of `Hunter::kSpotMaxTries` "
                          "(3) tries") &&
              has(hunter, "\"more than a view above it, no map to go by\"") &&
              has(doc, "(\"... more than a view above it, no map to go by\")") &&
              has(readme, "`kSpotFarBelowPx` (" + n(Hunter::kSpotFarBelowPx) + ", a view)") &&
              has(world, "\"Positions: your report at %d,0 (opcode 0x%04X) -- y 0 is no place: height kept at %d%s\"") &&
              has(doc, "\"Positions: your report at X,0 (opcode ...) -- y 0 is no place: height kept at Y\"") &&
              has(hunter, "\"no trusted map file to say; taken at your next cast or walk report\"") &&
              has(hunter, "\"Hunter: not turning %s toward %s -- the floor ends at %d,%d with nothing of the map file \"") &&
              has(doc, "\"Hunter: not turning right toward ... (a pit), and a turn's tap walks the character; passed "
                       "over for N s\"") &&
              has(doc, "(trừ packet_dy, như vòng quanh con quái đang đánh; của bạn là 0)") &&
              has(map_view, "double band_feet_y(double you_y, int packet_dy, double scale)"),
          "the review of 2026.11.12 and 2026.11.24: " + n(Hunter::kSpotMaxTries) + " walks back given up on wait "
              "spot_retry_ms and try again, a new round of " + n(Hunter::kSpotMaxTries) + "; a spot more than " +
              n(Hunter::kSpotFarBelowPx) + " px below with no map not walked to blind; y 0 kept at the last height; "
              "the spawn's 0x0028 alone refused; no turn's tap off a pit; the bars less packet_dy -- as the code and "
              "the notes say them");

    // ---- the trust: the drop's landings, the points mid-fall, the spot ----
    check(GameMapBook::kNoDropXPx == 64 && MapTrust::kMobStandMs == 300 &&
              has(drop, "\"Hunter: drop landed at %d,%d, %d px down, %s -- not floor %d the map file gave from %d,%d; "
                        "\"") &&
              has(drop, "\"arrow held: something else moved you; nothing learned, %s's file still gone by\"") &&
              has(drop, "\"another floor, %d map changes, %d unknown)\"") &&
              has(doc, "\"Hunter: drop landed at X,Y, N px down, on floor F (attr A) -- not floor L the map file gave "
                       "...\"") &&
              has(doc, "thêm \"N on another floor\"") &&
              has(doc, "bị dời xa hơn " + n(GameMapBook::kNoDropXPx) + " px (một kỹ năng dời nhân vật)") &&
              has(doc, "(\"... something else moved you\")") &&
              has(doc, "khi có báo vị trí sau đó ít nhất " + vi_s(MapTrust::kMobStandMs) + " giây ở cùng độ cao") &&
              has(readme, "`GameMapBook::kNoDropXPx` (" + n(GameMapBook::kNoDropXPx) + ")") &&
              has(readme, "`MapTrust::kMobStandMs` (" + n(MapTrust::kMobStandMs) + " ms)") &&
              has(hunter, "\" -- on the floor under %d,%d, by the map file\"") &&
              has(doc, "(log thêm \"-- on the floor under X,Y, by the map file\")") &&
              has(hunter, "\"Hunter: no hunting spot at the map entry's spawn %d,%d -- perhaps in the air above its "
                          "floor, and \"") &&
              has(doc, "(\"Hunter: no hunting spot at the map entry's spawn X,Y -- ...\")"),
          "a landing on another floor of the file kept, one " + n(GameMapBook::kNoDropXPx) + " px along the take-off "
          "floor not a fail, a point off the floors confirmed " + n(MapTrust::kMobStandMs) + " ms on, the spot on "
          "its floor or not at the spawn -- the lines as the code writes them");

    // ---- the two bars ----
    check(kBandMaxPx == 1000 &&
              has(map_view, "inline constexpr int kBandStepPx = 5;") &&
              has(map_view, "return (bar == BandBar::Up ? \"+\" : \"-\") + std::to_string(px) + \" px\";") &&
              has(map, "\"Quái thấp hơn vạch dưới: bot nhảy xuống; cao hơn vạch trên: bot nhảy lên; giữa hai vạch: "
                       "đánh tại chỗ.\\n\"") &&
              has(map, "box(\"Vạch trên\", \"##band_up\", up);") &&
              has(map, "box(\"Vạch dưới\", \"##band_down\", down);") &&
              has(map, "{ Sym::Band, \"vạch trên, dưới\" }") &&
              has(map, "\"Không lưu được hai vạch vào configs\\\\hunt.ini — GoBot thử lại sau ít giây.\"") &&
              has(style, "\"Nhảy xuống (Ctrl + ↓) khi quái thấp hơn vạch dưới (\" + std::to_string(c.down()) +") &&
              has(style, "number(\"Cao nhất (px)\", \"Vị trí cao nhất có thể tấn công (vạch trên ở tab Bản đồ).\", up, 0, "
                         "kBandMaxPx, 1);") &&
              has(doc, "vạch trên ghi \"+10 px\", vạch dưới \"-20 px\" (số của bạn)") &&
              has(doc, "(mỗi nấc 5 px; lúc kéo, bản đồ đứng yên)") &&
              has(doc, "\"Vạch trên [..] px Vạch dưới [..] px\"") && has(doc, "(0 tới " + n(kBandMaxPx) + ")") &&
              has(doc, "\"Không lưu được hai vạch vào configs\\hunt.ini — GoBot thử lại sau ít giây.\"") &&
              has(doc, "(nay tới " + n(kBandMaxPx) + " px, trước là 400)") &&
              has(doc, "\"Quái thấp hơn vạch dưới: bot nhảy xuống; cao hơn vạch trên: bot nhảy lên; giữa hai vạch: đánh "
                       "tại chỗ.\"") &&
              count_in(doc, "\"Nhảy xuống (Ctrl + ↓) khi quái thấp hơn vạch dưới (N px), ...\"") == 2 &&
              !has(doc, "Dưới hàng Tự đánh quái phải có \"Nhảy xuống (Ctrl + ↓) khi quái thấp hơn quá 100 px") &&
              has(readme, "`kBandStepPx` (5)") && has(readme, "0-`kBandMaxPx`, 1000") &&
              has(readme, "*Quái thấp hơn vạch dưới: bot nhảy xuống; cao hơn vạch trên: bot nhảy lên; giữa hai vạch: "
                          "đánh tại chỗ.*") &&
              has(pack, "(0 to " + n(kBandMaxPx) + " px; up_reach and down_reach in hunt.ini, also Cao nhat and Thap "
                        "nhat)") &&
              has(hunt_ini, "Since 2026.11.12 up_reach and down_reach are the two bars drawn on the \"Bản đồ\" tab's "
                            "map") &&
              has(goc, "Since 2026.11.12 drawn as two bars on **Bản đồ**, dragged or typed (0-" + n(kBandMaxPx) +
                           " px), and the only heights a drop or a jump up goes by"),
          "the two bars: labelled +N and -N px from the feet, steps of 5, 0 to " + n(kBandMaxPx) + ", the boxes, the "
          "tip, the red line and the drop's hint quoted as simple_map.cpp and simple_style.cpp say them");

    // ---- the minimums gone, and those kept ----
    HunterConfig band;
    band.up_reach = 10;
    band.down_reach = 20;
    band.up_no_jump = 30;
    band.down_no_jump = 30;
    check(Hunter::kDropMinFallPx == Hunter::kDropLandPx + 1 && Hunter::kDropMinFallPx == 25 && band.up_nj() == 10 &&
              band.down_nj() == 20 && !has(hunt_raw, "\ndrop_min_px=") &&
              has(doc, "drop_min_px (100 px: nhảy xuống chỉ khi quái thấp hơn quá số lớn hơn giữa \"Thấp nhất\" và 100): "
                       "bỏ. hunt.ini cũ còn dòng đó thì GoBot không đọc nữa.") &&
              has(doc, "Lần rơi ngắn nhất GoBot nhận là nhảy xuống: 60 px xuống " + n(Hunter::kDropMinFallPx) +
                                 " px.") &&
              has(doc, "(vạch 10 và 20 thì hiện 10 và 20)") &&
              has(doc, "từ bản 2026.11.12 chỉ theo \"Thấp nhất\", tức vạch dưới ở tab Bản đồ: drop_min_px không còn "
                       "được đọc") &&
              has(doc, "Từ bản 2026.11.12 hunt.ini không còn dòng drop_min_px (hunt.ini cũ còn dòng đó thì GoBot không "
                       "đọc)") &&
              has(doc, "Giữ nguyên, vì là luật an toàn hay không phải mức độ cao: không nhảy gần cửa "
                                 "(drop_gate_px 150)") &&
              has(readme, "`Hunter::kDropMinFallPx` from 60 to `kDropLandPx` + 1 (" + n(Hunter::kDropMinFallPx) + ")") &&
              has(readme, "an older file's `drop_min_px` is no longer read") &&
              !has(readme, "with `drop_min_px` 100, `drop_side_px` 120") &&
              has(style, "hint(\"Hai số này không lớn hơn Cao nhất và Thấp nhất. Nhảy lên, nhảy xuống chỉ theo Cao nhất và "
                         "Thấp nhất.\");") &&
              has(pack, "drop_min_px (100) and the other hidden minimums are gone") &&
              has(pack, "A hunt.ini from before 2026.11.12 may hold drop_min_px: it is no longer read"),
          "drop_min_px gone from the code and the shipped hunt.ini, the least fall " + n(Hunter::kDropMinFallPx) +
              " px, the no-jump pair under bars of 10 and 20 read 10 and 20 -- each said, and what is kept");

    // ---- the start count ----
    const std::uint64_t pass_least = source_ms(gui, "kPassFirstMs"), shop_least = source_ms(gui, "kShopFirstMs");
    const std::uint64_t t0 = 50'000'000;
    CleanSettings ships;
    const bool trash_loaded = ships.load(std::string(GOBOT_SOURCE_DIR) + "/configs/trash.ini");
    check(pass_least == 15'000 && shop_least == 45'000 &&
              bag::start_due_ms(0, t0, 10, pass_least) == t0 + 600'000 &&
              bag::start_due_ms(t0 + 5'000, t0, 10, pass_least) == t0 + pass_least &&
              bag::start_due_ms(t0 + 300'000, t0, 10, pass_least) == t0 + 300'000 &&
              bag::start_due_ms(t0 - 1, t0, 60, shop_least) == t0 + 3'600'000 && trash_loaded &&
              ships.interval_minutes == 10 && ships.buy_every_minutes == 60 &&
              has(gui, "GB_INFO(\"Trash: counting from the start -- the first timed pass at %s, the shop's first look at "
                       "%s\",") &&
              has(gui, "std::string(\" (kept from the last run)\")") && has(gui, "\" min from the start)\"") &&
              has(gui, "\"Trash: Tự dọn rác ticked on -- the first timed pass at %s, %d min from now\"") &&
              has(gui, "\"Trash: Tự mua bình ticked on -- the first look at %s, %d min from now, owing nothing\"") &&
              has(doc, "\"Trash: counting from the start -- the first timed pass at HH:MM (" +
                           n(ships.interval_minutes) + " min from the start), the shop's first look at HH:MM (" +
                           n(ships.buy_every_minutes) + " min from the start)\" (hay \"(kept from the last run)\")") &&
              has(doc, "\"Trash: Tự dọn rác ticked on -- the first timed pass at HH:MM, N min from now\"") &&
              has(doc, "\"Trash: Tự mua bình ticked on -- the first look at HH:MM, N min from now, owing nothing\"") &&
              has(readme, "`Trash: counting from the start -- the first timed pass at HH:MM (10 min from the start), "
                          "the shop's first look at HH:MM (60 min from the start)`") &&
              has(doc, "lượt dọn rác theo giờ đầu tiên tới sau cả một khoảng \"dọn mỗi\" (trước đây " +
                           n(pass_least / 1000) + " giây sau)") &&
              has(doc, "(nhưng không sớm hơn " + n(pass_least / 1000) + " giây cho dọn rác, " + n(shop_least / 1000) +
                           " giây cho mua bình)") &&
              has(readme, "`kPassFirstMs` (" + n(pass_least / 1000) + " s)") &&
              has(readme, "`kShopFirstMs` (" + n(shop_least / 1000) + " s)") &&
              has(doc, "từ bản 2026.11.12 lần đầu là cả một khoảng \"mua mỗi\" sau khi bấm BẮT ĐẦU hay đánh dấu "
                                 "ô này") &&
              !has(doc, "lần đầu không sớm hơn 45 giây sau khi bấm BẮT ĐẦU)") &&
              has(view, "\"lại theo giờ ở trên, đếm từ lúc bấm BẮT ĐẦU (hay lúc đánh dấu Tự dọn rác): lần đầu là \"") &&
              has(shop_card, "Giờ mua đếm từ lúc bấm BẮT ĐẦU (hay lúc đánh dấu ô này): lần mua đầu là một khoảng ") &&
              has(pack, "the first timed pass comes interval_minutes after BAT DAU, the first shop look "
                        "buy_every_minutes after it") &&
              has(goc, "the first a whole interval after START since 2026.11.12") &&
              has(goc, "since 2026.11.12 the first a whole interval after the bot starts, or what is left of the last "
                       "one after a quick stop and start"),
          "the start count: a whole interval from BẮT ĐẦU, a clock still counting kept at " + n(pass_least) + " / " +
              n(shop_least) + " ms at least, one run out counted afresh -- as bag::start_due_ms has it, and the log's "
              "lines as trash_loop writes them");
#else
    skip("no source folder\n");
#endif
}

// ------------------------------------------------------------ 2026.11.15d

/// 2026.11.15d's notes against the code: the owner's 30 s for the game to
/// react (react_wait.hpp) and the check-in board's ESC 30 s after a login, never
/// from 23:55 to 00:03 (checkin_board.hpp) -- DOC-TRUOC.txt's entry in
/// section 5 under 2026.11.16's alone, README's section after 2026.11.15c's,
/// START-HERE's paragraph after 15c's, the ini comments and gobot_cli's words,
/// each number as the constants have it.
void test_notes_1115d() {
    section("The 2026.11.15d notes against the code: 30 s for the game to react, the board's ESC after a login");
#ifdef GOBOT_SOURCE_DIR
    const std::string doc = flat(source("/DOC-TRUOC.txt"));
    const std::string readme = flat(source("/README.md"));
    const std::string pack = flat(source("/package.ps1"));
    const std::string trash_ini = flat(source("/configs/trash.ini"), true);
    const std::string quest_ini = flat(source("/configs/quest.ini"), true);
    const std::string chat_ini = flat(source("/configs/chat.ini"), true);
    const std::string party_ini = flat(source("/configs/party.ini"), true);
    const std::string app_ini = flat(source("/configs/app.ini"), true);
    const std::string login_ini = flat(source("/configs/login.ini"), true);
    const std::string cli = source("/apps/gobot_cli/main.cpp");
    const std::string closer = source("/src/popup_closer.cpp");
    const std::string chat_cpp = source("/src/auto_chat.cpp");
    const std::string chat_hpp = flat(source("/include/gobot/auto_chat.hpp"));
    const std::string reviver_hpp = flat(source("/include/gobot/reviver.hpp"));
    const std::string login_hpp = flat(source("/include/gobot/auto_login.hpp"));
    const std::string login_cpp = flat(source("/src/auto_login.cpp"));
    if (doc.empty() || readme.empty() || pack.empty() || trash_ini.empty() || quest_ini.empty() || chat_ini.empty() ||
        party_ini.empty() || app_ini.empty() || login_ini.empty() || cli.empty() || closer.empty() ||
        chat_cpp.empty() || chat_hpp.empty() || reviver_hpp.empty() || login_hpp.empty() || login_cpp.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto n = [](auto v) { return std::to_string(v); };
    // A second of the day as the notes write it: 23:55, 00:03.
    const auto hhmm = [](int s) {
        char out[32];
        std::snprintf(out, sizeof out, "%02d:%02d", s / 3600, s / 60 % 60);
        return std::string(out);
    };
    using P = PopupCloser;
    const std::string react_s = n(kReactMs / 1000);
    const std::string slow_s = n(kReactSlowMs / 1000);
    const std::string board_s = n(kCheckinBoardLoginWaitMs / 1000);
    const std::string night_from = hhmm(kCheckinNightFromS), night_to = hhmm(kCheckinNightToS);
    const std::string asked_react = "khi GoBot thực hiện thao tác, sẽ có thêm 30 giây chờ phản ứng. Nếu màn hình phản "
                                    "ứng luôn thì thực hiện tiếp bước tiếp theo, còn nếu màn hình không phản ứng gì "
                                    "thì sẽ dừng xử lý.";
    const std::string asked_board = "Sau khi chọn nhân vật xong và vào tới màn hình game thì sẽ chờ 30 giây rồi mới ấn "
                                    "ESC để tắt thông báo. Trường hợp qua ngày thì chờ tới 00:03 mới ấn ESC.";

    // ---- the constants the notes quote ----
    check(kReactMs == 30'000 && kReactSlowMs == 2'000 && BagCleaner::kStuckMs == kReactMs &&
              P::kCheckMs == kReactMs && P::kAfterMenuMs == 1500 && P::kHoldMs == 2000 && P::kSteadyMs == 3000 &&
              InviteReply::kFirstGoneMs == kReactMs && InviteReply::kGoneMs == 2500 &&
              InviteReply::kWatchMs == 2500 && kCheckinBoardLoginWaitMs == 30'000 &&
              night_from == "23:55" && night_to == "00:03" && kCheckinNightFromS % 60 == 0 &&
              kCheckinNightToS % 60 == 0 && checkin_night_hold(kCheckinNightFromS) &&
              checkin_night_hold(kCheckinNightToS - 1) && !checkin_night_hold(kCheckinNightToS) &&
              !checkin_night_hold(kCheckinNightFromS - 1) &&
              QuestSettings{}.round_limit_ms == 10000 &&
              has(reviver_hpp, "static constexpr int kQuestionWaitMs = kReactMs;") &&
              has(reviver_hpp, "static constexpr int kReviveWaitMs = kReactMs;") &&
              has(chat_hpp, "int max_enters = 2;") &&
              has(login_hpp, "static constexpr int kMessageClicks = 3;") &&
              has(login_hpp, "static constexpr int kMessageGoneMs = 2500;") &&
              has(login_cpp, "constexpr int kChannelTookMs = 2000;") && has(login_cpp, "constexpr int kChannelTries = 3;"),
          "kReactMs " + n(kReactMs) + " ms (kStuckMs, kCheckMs, kFirstGoneMs and the reviver's waits with it), "
              "kReactSlowMs " + n(kReactSlowMs) + " ms, the board's " + n(kCheckinBoardLoginWaitMs) +
              " ms and its night hold " + night_from + " to " + night_to + ", two Enters, three clicks at login");

    // ---- DOC-TRUOC.txt: the entry under 2026.11.16's alone, the owner's words ----
    const std::size_t sec5 = doc.find("5. CÓ GÌ MỚI, PHẦN NÀO LÀ THỬ NGHIỆM");
    const std::size_t e17 = doc.find("- Mới trong bản 2026.11.17 (");
    const std::size_t e16b = doc.find("- Mới trong bản 2026.11.16b (");
    const std::size_t e16 = doc.find("- Mới trong bản 2026.11.16 (");
    const std::size_t e15d = doc.find("- Mới trong bản 2026.11.15d (");
    const std::size_t e15c = doc.find("- Mới trong bản 2026.11.15c (");
    check(sec5 != npos && e17 != npos && e16b != npos && e16 != npos && e15d != npos && e15c != npos &&
              doc.find("- Mới trong bản", sec5) == doc.find("- Mới trong bản 2026.11.24 (") && doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.24 (") + 1) == doc.find("- Mới trong bản 2026.11.17c (") && doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.17c (") + 1) == doc.find("- Mới trong bản 2026.11.17b (") &&
              doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.17b (") + 1) == e17 && doc.find("- Mới trong bản", e17 + 1) == e16b &&
              doc.find("- Mới trong bản", e16b + 1) == e16 &&
              doc.find("- Mới trong bản", e16 + 1) == e15d && doc.find("- Mới trong bản", e15d + 1) == e15c &&
              has(doc, "- Mới trong bản 2026.11.15d (máy chậm: mỗi thao tác của GoBot chờ game phản ứng tới " +
                           react_s + " giây; bảng điểm danh tắt " + board_s + " giây sau khi vào game, qua ngày "
                           "thì chờ tới " + night_to + "; theo lời bạn ngày 15/11; chưa thử trên game thật).") &&
              has(doc, "\"" + asked_react + "\"") && has(doc, "\"" + asked_board + "\"") &&
              has(readme, "*\"" + asked_react + "\"*") && has(readme, "*\"" + asked_board + "\"*"),
          "DOC-TRUOC.txt: the 2026.11.15d entry in section 5 under 2026.11.17's, 16b's and 16's, over 15c's, with the "
          "owner's "
          "two requests as written -- quoted in README too");
    check(has(doc, "Hết " + react_s + " giây mà không thấy gì thì bỏ bước đó như trước") &&
              has(doc, "Luật \"kẹt quá 10 giây thì bấm ESC\" (bản 2026.11.14b) nay là kẹt quá " +
                           n(BagCleaner::kStuckMs / 1000) + " giây, tính từ thao tác cuối của GoBot trong bước đó.") &&
              has(doc, "được cộng thêm phần game chậm quá " + slow_s + " giây ở mỗi bước") &&
              has(doc, "(round_limit_ms) nay không tính thời gian chờ game.") &&
              has(doc, "lần bấm Xác Nhận đầu chờ tới " + n(InviteReply::kFirstGoneMs / 1000) + " giây, lần hai 2,5 "
                       "giây.") &&
              has(doc, "\"Không mở được ô chat (đã bấm Enter 2 lần, mỗi lần chờ " + react_s +
                           " giây) — bỏ qua câu này.\"") &&
              has(chat_cpp, "(enters > 1 ? \"mỗi lần chờ \" : \"chờ \")") &&
              has(chat_cpp, "\" giây) — bỏ qua câu này.\";") &&
              has(doc, "GoBot bấm ESC " + board_s + " giây sau khi nhân vật vào game (GoBot tự vào: tính từ lúc vào; "
                       "vào bằng tay: tính từ lúc thấy bảng), bảng phải đứng yên ít nhất " +
                       n(P::kSteadyMs / 1000) + " giây. Từ " + night_from + " tới " + night_to +
                       " theo đồng hồ máy thì không bấm: qua ngày GoBot chờ tới " + night_to + " mới bấm ESC") &&
              has(doc, "\"Thấy bảng Sự kiện và điểm danh: chờ " + board_s +
                           " giây sau khi vào game rồi mới bấm ESC.\"") &&
              has(closer, "\"Thấy bảng Sự kiện và điểm danh: chờ 30 giây sau khi vào game rồi mới bấm ESC.\"") &&
              has(doc, "\"Qua ngày: chờ tới " + night_to + " rồi mới bấm ESC tắt bảng Sự kiện và điểm danh.\"") &&
              has(closer, "\"Qua ngày: chờ tới 00:03 rồi mới bấm ESC tắt bảng Sự kiện và điểm danh.\"") &&
              has(doc, "game bỏ sót một phím thì nay GoBot chờ tới " + react_s + " giây mới bỏ bước đó") &&
              has(doc, "\"the game took N ms to show ...\"") && has(doc, "\"nothing after " + react_s + " s ...\"") &&
              has(doc, "\"the check-in board waits for its ESC until ...\"") &&
              has(closer, "\"the check-in board waits for its ESC until \""),
          "DOC-TRUOC.txt's 2026.11.15d entry: " + react_s + " s a reaction, the stuck rule from the last input, "
          "the deadlines moved on past " + slow_s + " s, the chat's two Enters, the party's first click, the board's " +
          board_s + " s and " + night_from + "-" + night_to + ", the tab's words and the log lines to send -- as the "
          "code says them");

    // ---- README: the section after 2026.11.15c's, before the macro's ----
    const std::size_t r15c = readme.find("### A report in mid-air is no landing, whatever its kind (2026.11.15c)");
    const std::size_t r15d =
        readme.find("### Thirty seconds for the game to react, and the board's ESC after a login (2026.11.15d)");
    const std::size_t rmacro = readme.find("## Keyboard macro");
    check(r15c != npos && r15d != npos && rmacro != npos && r15c < r15d && r15d < rmacro &&
              has(readme, "Since 2026.11.15d (not yet run on the game).") &&
              has(readme, "(`gobot/react_wait.hpp`, `kReactMs` = " + react_s + " s)") &&
              has(readme, "A reaction later than `kReactSlowMs` (" + slow_s + " s) is said in the log") &&
              has(readme, "`BagCleaner::kStuckMs` is `kReactMs`, counted from the step's last input of GoBot's own") &&
              has(readme, "up to " + react_s + " s (`InviteReply::kFirstGoneMs`), the second `InviteReply::kGoneMs` "
                          "(2.5 s)") &&
              has(readme, "`max_enters` (" + n(ChatSettings{}.max_enters) + " shipped)") &&
              has(readme, "(`Reviver::kQuestionWaitMs`, `kReviveWaitMs`)") &&
              has(readme, "`kChannelTries` / `kMessageClicks` (3) clicks spread over " + react_s + " s from the first") &&
              has(readme, "`PopupCloser::kCheckMs` (= `kReactMs`)") &&
              has(readme, "watched only `kAfterMenuMs` (" + n(P::kAfterMenuMs / 1000) + "." +
                              n(P::kAfterMenuMs % 1000 / 100) + " s) longer") &&
              has(readme, "`kCheckinBoardLoginWaitMs` (" + board_s + " s) after the later of the character's arrival in "
                          "the world") &&
              has(readme, "from `kCheckinNightFromS` (" + night_from + ":00) to `kCheckinNightToS` (" + night_to +
                              ":00) (`checkin_night_hold`)") &&
              has(readme, "a key press the game loses now costs up to " + react_s + " s before its step is given up") &&
              has(readme, "Since 2026.11.15d the board waits longer: its ESC goes " + board_s + " s after the "
                          "character's arrival in the game"),
          "README: the 2026.11.15d section after 15c's and before the macro's -- kReactMs, kReactSlowMs, kStuckMs, "
          "the party's, the chat's, the reviver's, auto login's and the popup closer's waits, the board's "
          "kCheckinBoardLoginWaitMs and night hold, and the cost -- and Getting started's popups line");

    // ---- START-HERE (package.ps1): the paragraph after 15c's ----
    const std::size_t p15c = pack.find("A report in mid-air is no landing (2026.11.15c, not yet tried on the game).");
    const std::size_t p15d = pack.find("Thirty seconds for the game to react, and the board's ESC after a login "
                                       "(2026.11.15d, not yet tried on the game).");
    check(p15c != npos && p15d != npos && p15c < p15d &&
              has(pack, "GoBot now waits up to " + react_s + " s for the screen to show what it did") &&
              has(pack, "a step stuck " + n(BagCleaner::kStuckMs / 1000) + " s, not 10 s, gets the ESCs") &&
              has(pack, "ESC " + board_s + " s after the character comes into the game, and never from " +
                            night_from + " to " + night_to + " by the PC's clock") &&
              has(pack, "A key press the game loses now costs up to " + react_s + " s."),
          "START-HERE: the 2026.11.15d paragraph after 15c's, with the " + react_s + " s and the board's " + board_s +
              " s and " + night_from + " to " + night_to);

    // ---- the ini comments and gobot_cli's words ----
    check(has(trash_ini, "a step stuck more than 10 s (the quantity box, a message that will not close; " +
                             n(BagCleaner::kStuckMs / 1000) + " s since GoBot's last input for it, since "
                             "2026.11.15d) is shut with ESC") &&
              has(trash_ini, "Từ bản 2026.11.15d: mỗi bước chờ game phản ứng tới " + react_s +
                                 " giây, và một bước mua kẹt quá " + n(BagCleaner::kStuckMs / 1000) +
                                 " giây (không phải 10) mới bấm ESC.") &&
              has(trash_ini, "Since 2026.11.15d the question is waited for up to " + react_s + " s") &&
              has(quest_ini, "tối đa " + react_s + " giây tính từ phím hay cú bấm;") &&
              has(quest_ini, "chỉ tính thời gian của chính lần làm, không tính lúc chờ game phản ứng.") &&
              has(chat_ini, "the Enter is watched until the line shows open, up to " + react_s + " s") &&
              has(chat_ini, "Since 2026.11.15d each Enter is watched up to " + react_s +
                                " s and the next goes only after that whole wait, on a look that still shows the "
                                "line shut;") &&
              has(party_ini, "Req Party is clicked once two looks in a row show it -- up to " + react_s +
                                 " s, or this if it is longer, from the row's click; never blind.") &&
              has(login_ini, "since 2026.11.15d spread over " + react_s + " s from the first click") &&
              has(app_ini, "-- since 2026.11.15d only " + board_s + " seconds after the character comes into the game") &&
              has(cli, "only 30 s after its first sight, and never from 23:55 to 00:03 by the PC's clock.") &&
              !has(cli, "a popup is closed once it has stayed 3 s.\\n\"") &&
              has(cli, " ms and up to 30 s, up to ") &&
              has(cli, " Enters (each after the one before's whole 30 s), "),
          "trash.ini, quest.ini, chat.ini, party.ini, login.ini and app.ini say the " + react_s + " s and the board's " +
              board_s + " s; gobot_cli's popup and chat lines say them too");
#else
    skip("no source folder\n");
#endif
}

// ------------------------------------------------------------ 2026.11.16

/// 2026.11.16's notes against the code: lighter on a weak PC -- the popups'
/// idle look, the name-plate reader's rest, HP and MP with the pet's key,
/// light mode by CPUs and RAM -- and the old features gone. DOC-TRUOC.txt's
/// entry first in section 5, README's section after 2026.11.15c's and before
/// 15d's, START-HERE's paragraph after 15d's, app.ini's comments; and the
/// deleted features no longer described as there.
void test_notes_1116() {
    section("The 2026.11.16 notes against the code: lighter on a weak PC, and the old features gone");
#ifdef GOBOT_SOURCE_DIR
    const std::string doc = flat(source("/DOC-TRUOC.txt"));
    const std::string readme = flat(source("/README.md"));
    const std::string pack = flat(source("/package.ps1"));
    const std::string app_ini = flat(source("/configs/app.ini"), true);
    if (doc.empty() || readme.empty() || pack.empty() || app_ini.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto n = [](auto v) { return std::to_string(v); };
    using P = PopupCloser;
    const std::string idle_s = n(P::kIdleLookMs / 1000), idle_light_s = n(P::kIdleLookLightMs / 1000);
    const std::string rest_s = n(DetectorPace::kRestMs / 1000);

    // ---- the numbers the notes quote ----
    check(P::kIdleLookMs == 2000 && P::kIdleLookLightMs == 3000 && DetectorPace::kRestMs == 2000 &&
              kRestPetMs == 500 && kWeakCpus == 2 && kWeakRamMb == 4096,
          "the popups' idle look " + n(P::kIdleLookMs) + " ms (" + n(P::kIdleLookLightMs) +
              " ms light), the detector's rest " + n(DetectorPace::kRestMs) + " ms, the bars' rest with a pet key " +
              n(kRestPetMs) + " ms, light mode at " + n(kWeakCpus) + " CPUs or under " + n(kWeakRamMb) + " MB");

    // ---- DOC-TRUOC.txt: the entry first in section 5, over 15d's ----
    const std::size_t sec5 = doc.find("5. CÓ GÌ MỚI, PHẦN NÀO LÀ THỬ NGHIỆM");
    const std::size_t e16 = doc.find("- Mới trong bản 2026.11.16 (GoBot nhẹ máy hơn cho máy yếu, và bỏ các phần cũ "
                                     "không còn ai dùng; theo lời bạn ngày 16/11; chưa thử trên game thật).");
    const std::size_t e15d = doc.find("- Mới trong bản 2026.11.15d (");
    // 2026.11.16b's entry on top of it since, and 2026.11.17's on that.
    const std::size_t e16b = doc.find("- Mới trong bản 2026.11.16b (");
    const std::size_t e17 = doc.find("- Mới trong bản 2026.11.17 (");
    check(sec5 != npos && e17 != npos && e16b != npos && e16 != npos && e15d != npos &&
              doc.find("- Mới trong bản", sec5) == doc.find("- Mới trong bản 2026.11.24 (") && doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.24 (") + 1) == doc.find("- Mới trong bản 2026.11.17c (") && doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.17c (") + 1) == doc.find("- Mới trong bản 2026.11.17b (") &&
              doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.17b (") + 1) == e17 && doc.find("- Mới trong bản", e17 + 1) == e16b &&
              doc.find("- Mới trong bản", e16b + 1) == e16 && doc.find("- Mới trong bản", e16 + 1) == e15d &&
              has(doc, "lần nhìn không thấy gì thì " + idle_s + " giây sau mới nhìn lại (" + idle_light_s +
                           " giây khi bật Nhẹ máy), thay vì 1 giây") &&
              has(doc, "Đọc tên quái lúc bot đánh theo gói tin: mỗi " + rest_s + " giây thay vì 0,6 giây.") &&
              has(doc, "GoBot đọc mỗi 0,5 giây (máu thú cưng chỉ đọc được trên màn hình)") &&
              has(doc, "chỉ có 1 hoặc 2 nhân, hoặc RAM dưới 4 GB") &&
              has(doc, "(16462 đọc thành 462) không còn bị coi là lệch với gói tin") &&
              has(doc, "khoảng 11,5 giây CPU mỗi phút") && has(doc, "Đó chỉ là ước tính, chưa đo") &&
              has(doc, "đây là chỗ duy nhất màn hình đơn giản đổi"),
          "DOC-TRUOC.txt: the 2026.11.16 entry in section 5 under 17's and 16b's, over 15d's, with the popups' " + idle_s +
              " s (" +
              idle_light_s + " s), the detector's " + rest_s + " s, the pet key's 0.5 s, light mode by cores and "
              "RAM, the MP fix, the measured 11.5 s and the estimate said to be one");

    // ---- README: the section after 2026.11.15c's, before 15d's ----
    const std::size_t r15c = readme.find("### A report in mid-air is no landing, whatever its kind (2026.11.15c)");
    const std::size_t r16 = readme.find("### Lighter on weak PCs, and old features removed (2026.11.16)");
    const std::size_t r15d =
        readme.find("### Thirty seconds for the game to react, and the board's ESC after a login (2026.11.15d)");
    check(r15c != npos && r16 != npos && r15d != npos && r15c < r16 && r16 < r15d &&
              has(readme, "Since 2026.11.16 (not yet run on the game).") &&
              has(readme, "the next comes " + idle_s + " s later (" + idle_light_s + " s in light mode,") &&
              has(readme, "(`DetectorPace::kRestMs`): " + n(DetectorPace::kRestMs) + " ms instead of 600") &&
              has(readme, "the bars are read every " + n(kRestPetMs) + " ms (`kRestPetMs`)") &&
              has(readme, "`auto` now also counts two or fewer logical CPUs, or under " + n(kWeakRamMb) +
                              " MB of RAM") &&
              has(readme, "-- an estimate, not a measurement,"),
          "README: the 2026.11.16 section between 15c's and 15d's, with the constants it names");

    // ---- START-HERE (package.ps1): the paragraph after 15d's ----
    const std::size_t p15d = pack.find("Thirty seconds for the game to react, and the board's ESC after a login "
                                       "(2026.11.15d, not yet tried on the game).");
    const std::size_t p16 = pack.find("Lighter on weak PCs, and old features removed (2026.11.16, not yet tried "
                                      "on the game).");
    check(p15d != npos && p16 != npos && p15d < p16 &&
              has(pack, "Tu dong thong bao looks every " + idle_s + " s (" + idle_light_s + " s with Nhe may)") &&
              has(pack, "the monsters' names are read every " + rest_s + " s, not 0.6 s") &&
              has(pack, "an estimate only, not measured"),
          "START-HERE: the 2026.11.16 paragraph after 15d's, with the " + idle_s + " s and the " + rest_s + " s");

    // ---- app.ini's comments ----
    check(has(app_ini, "or since 2026.11.16 a PC with " + n(kWeakCpus) + " logical CPUs or fewer or under " +
                           n(kWeakRamMb) + " MB of RAM") &&
              has(app_ini, "máy chỉ có 1-2 nhân hoặc RAM dưới 4 GB") &&
              has(app_ini, "every " + rest_s + " s (0.6 s before 2026.11.16) instead of every 0.14 s."),
          "app.ini: light mode by CPUs and RAM, and the name plates' rest of " + rest_s + " s");

    // ---- what went is no longer described as there ----
    check(!has(readme, "## Packet-capture bot") && !has(readme, "gobot_cli --status") &&
              !has(readme, "| `configs/bot_default.ini` |") && !has(readme, "**Dashboard → \"Set up automatically\"**") &&
              !has(readme, "a walk to a saved point") && !has(readme, "`gobot_gui` and `gobot_cli`'s packet bot") &&
              !has(doc, "\"Map changed -- minimap\"") && !has(doc, "mở thẻ Radar ở màn hình Nâng cao") &&
              !has(doc, "\"Detector: every 600 ms\";") && !has(pack, "Phim nhay khi di toi diem da luu la Space: se") &&
              !has(app_ini, "bot_config=") && !has(app_ini, "autostart="),
          "README, DOC-TRUOC.txt, START-HERE and app.ini no longer describe the packet-only bot, the Radar, the "
          "minimap's map changes or bot_default.ini as there");
#else
    skip("no source folder\n");
#endif
}

// ------------------------------------------------------------ 2026.11.16b

/// 2026.11.16b's notes against the code: the skills in turn while hunting
/// too, on by default -- DOC-TRUOC.txt's entry first in section 5, README's
/// section right before 2026.11.16's, START-HERE's paragraph after 16's,
/// macro.ini's comment; each number and log line as the code has it.
void test_notes_1116b() {
    section("The 2026.11.16b notes against the code: the skills in turn while hunting too, on by default");
#ifdef GOBOT_SOURCE_DIR
    const std::string doc = flat(source("/DOC-TRUOC.txt"));
    const std::string readme = flat(source("/README.md"));
    const std::string pack = flat(source("/package.ps1"));
    const std::string ini = flat(source("/configs/macro.ini"), true);
    const std::string pace = source("/src/macro_pace.cpp");
    const std::string skills = source("/apps/gobot_gui/simple_skills.cpp");
    if (doc.empty() || readme.empty() || pack.empty() || ini.empty() || pace.empty() || skills.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto n = [](auto v) { return std::to_string(v); };
    using E = MacroEngine;
    const std::string asked = "Hiện tại dùng skill đang không được đều, cần chỉnh lại";

    // ---- the numbers the notes quote ----
    check(E::kPaceRepressMs == 160 && E::kPaceConfirmMs == 220 && E::kSkillRetryMs == 30 &&
              E::kPaceSkillLearn == 3 && E::kPaceSkillUnlearn == 3 && E::kPaceGiveBackMost == 2 &&
              MacroConfig{}.cast_paced && std::string(MacroConfig::kCastPacedKey) == "cast_paced_v2",
          "the re-press at " + n(E::kPaceRepressMs) + " ms, the skip at " + n(E::kPaceConfirmMs) + " ms, a busy key "
          "again after " + n(E::kSkillRetryMs) + " ms, a key's skill after " + n(E::kPaceSkillLearn) +
          " casts, a press given back " + n(E::kPaceGiveBackMost) + " times a turn; on in code, read from "
          "cast_paced_v2");

    // ---- DOC-TRUOC.txt: the entry in section 5 under 2026.11.17's alone, over 16's ----
    const std::size_t sec5 = doc.find("5. CÓ GÌ MỚI, PHẦN NÀO LÀ THỬ NGHIỆM");
    const std::size_t e16b = doc.find("- Mới trong bản 2026.11.16b (kỹ năng ra đều, theo vòng cả khi đánh quái; theo "
                                      "lời bạn ngày 16/11; chưa thử trên game thật).");
    const std::size_t e16 = doc.find("- Mới trong bản 2026.11.16 (");
    const std::size_t e17 = doc.find("- Mới trong bản 2026.11.17 (");
    check(sec5 != npos && e17 != npos && e16b != npos && e16 != npos && doc.find("- Mới trong bản", sec5) == doc.find("- Mới trong bản 2026.11.24 (") && doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.24 (") + 1) == doc.find("- Mới trong bản 2026.11.17c (") && doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.17c (") + 1) == doc.find("- Mới trong bản 2026.11.17b (") &&
              doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.17b (") + 1) == e17 &&
              doc.find("- Mới trong bản", e17 + 1) == e16b &&
              doc.find("- Mới trong bản", e16b + 1) == e16 && has(doc, "Bạn báo: \"" + asked + "\"") &&
              has(doc, "Bạn chọn \"Ra chiêu theo vòng cả khi đánh quái\", bật sẵn.") &&
              has(doc, "Ô \"Ra chiêu theo vòng\" nay mặc định CÓ dấu, kể cả với macro.ini cũ của bạn (dòng "
                       "cast_paced=false trong đó là mặc định cũ, không phải bạn chọn).") &&
              has(doc, "Ô nằm ngay ở dòng Tự dùng kỹ năng (thẻ Đánh quái), ngay dưới \"Tốc độ bấm kỹ năng\".") &&
              has(doc, "phím chưa tới giờ thì nhường lượt ngay, tới giờ thì ra ở lượt kế của nó, nên không phím nào "
                       "bị bỏ đói") &&
              has(doc, "Nay chạy cả khi Tự đánh quái chạy, có hay không \"Chỉ khi đang đánh quái\".") &&
              has(doc, "vòng đứng chờ ở phím đang tới lượt, hết giữ thì đi tiếp từ phím đó, mỗi lần một phím, không "
                       "bấm dồn") &&
              has(doc, "lúc theo vòng, Z ra theo lượt của nó, bot không bấm thêm Z mỗi 0,7 giây nữa") &&
              has(doc, "bấm lại sau " + n(E::kPaceRepressMs) + " ms, vẫn không ra trong " + n(E::kPaceConfirmMs) +
                           " ms thì vòng qua phím kế ngay") &&
              has(doc, "thì thử lại sau " + n(E::kSkillRetryMs) + " ms thay vì 100 ms") &&
              has(doc, "Muốn tắt: thẻ Đánh quái, dòng Tự dùng kỹ năng, bỏ dấu ô \"Ra chiêu theo vòng\" (lưu ngay; mở "
                       "lại GoBot vẫn tắt). Sửa tay thì là dòng cast_paced_v2=false trong phần [Macro] của "
                       "configs\\macro.ini, lúc GoBot đã tắt.") &&
              has(doc, "\"Đang theo vòng cùng Tự đánh quái — khóa chiêu ước 680 ms, 12 vòng.\"") &&
              has(doc, "\"now paced with the hunter\"") && has(doc, "\"given back N\"") && has(doc, "\"held N s\"") &&
              has(doc, "(\"per key pressed/cast: Z 9/9, X 9/8, ...\")"),
          "DOC-TRUOC.txt: the 2026.11.16b entry in section 5 under 17's alone, over 16's, with the owner's words, on by "
          "default for old files too, the tick on the row, no key starved, the holds, the attack key, the " +
              n(E::kPaceRepressMs) + "/" + n(E::kPaceConfirmMs) + " ms, how to untick, and the log's new words");

    // ---- the review of 2026.11.16b: what the notes say against what the code does ----
    // The tick is drawn on a line of its own, under the combo: no SameLine
    // between the two.
    const std::string row = body(skills, "void GuiApp::simple_skills_line() {");
    const std::size_t combo = row.find("if (skill_gap_combo(cfg)) simple_changed();");
    const std::size_t tick = row.find("if (ImGui::Checkbox(kCastPacedShort, &cfg.cast_paced)) simple_changed();");
    // A key out of reach, or the dash's near an edge, passes its turn
    // (turn_of's Turn::Waiting) -- never among the holds that stop the round.
    // A cast by hand is told apart only once the key's skill is learned.
    // "Dùng skill tuần tự" does nothing while the rotation has the skills.
    check(combo != npos && tick != npos && combo < tick &&
              row.substr(combo, tick - combo).find("SameLine") == npos &&
              has(readme, "on a line of its own right below *Tốc độ bấm kỹ năng*") && !has(doc, "cạnh \"Tốc độ bấm") &&
              has(pace, "if (waits(k)) return Turn::Waiting;") &&
              !has(doc, "phím lướt gần mép, quái xa quá tầm của phím") &&
              has(doc, "Còn phím mà quái xa quá tầm của nó, hay phím lướt lúc gần mép, thì chỉ nhường lượt: vòng "
                       "đi tiếp phím kế, các phím khác vẫn ra.") &&
              has(doc, "phím đánh mà cũng là kỹ năng cũng nhường lượt khi quái xa quá tầm đánh của bot") &&
              !has(pack, "a jump, out of range;") &&
              has(pack, "A key whose monster is out of its reach, or the dash key near an edge, only passes its "
                        "turn: the round goes on with the next key.") &&
              !has(ini, "outside the area, out of range,") &&
              has(ini, "a key whose monster is out of its reach, or the dash key near an edge, only passes its turn "
                       "and the round goes on with the next key.") &&
              !has(doc, "hay do bạn tự bấm không bị tính nhầm") &&
              has(doc, "Chiêu bạn tự bấm cũng vậy, khi GoBot đã nhớ phím đó ra chiêu nào (sau " +
                           n(E::kPaceSkillLearn) + " lần ra chiêu đầu của phím đó, mỗi lần mở GoBot)") &&
              has(skills, "\"Khi \\\"Ra chiêu theo vòng\\\" có dấu và GoBot thấy gói tin ra chiêu, kỹ năng ra theo "
                          "vòng \"") &&
              has(skills, "\"và ô này không có tác dụng; nó chỉ dùng lúc tạm bấm theo giờ (không có gói tin) hoặc "
                          "\"") &&
              has(readme, "`Hunter::attack_far`: `attack_limit_px`, F3") &&
              has(readme, "Under a turn not yet confirmed, nothing to fight and, following the"),
          "the review's notes: the tick under \"Tốc độ bấm kỹ năng\", a key out of reach or the dash's near an edge "
          "passing its turn (not a hold), a cast by hand told apart once learned, \"Dùng skill tuần tự\" idle while "
          "paced, and the attack key's reach and the swing's hand-off in the README");
    // Its status and the log's words as the code has them.
    E::PaceStatus ps;
    ps.mode = E::PaceStatus::Mode::Paced;
    ps.lock_ms = 680;
    ps.rounds = 12;
    ps.hunting = true;
    check(has(doc, "\"" + E::pace_status_vi(ps) + "\"") && has(readme, "*" + E::pace_status_vi(ps) + "*") &&
              has(pace, "if (m == Mode::Paced && hunting) return \"paced with the hunter\";") &&
              in_order(pace, { "\"%s; fallback %s; lock_est %u ms; other casts %llu; now %s\"",
                               "\"; given back %llu; held %.1f s; per key pressed/cast: %s%s\"," }) &&
              has(pace, "GB_INFO(\"Paced: on, with the hunter -- the skills one after another in the order of the "
                        "keys (%s), \"") &&
              has(pace, "\"Paced: %s\", hunting ? \"the hunter runs -- the rotation goes on beside it, under its "
                        "holds\"") &&
              has(readme, "`Paced: on, with the hunter -- …` and `Paced: the hunter runs -- the rotation goes on "
                          "beside it, under its holds`") &&
              has(readme, "`…; other casts 0; now paced with the hunter; given back N; held N.N s; per key "
                          "pressed/cast: Z 9/9, X 9/8, …`"),
          "the status with the hunter, \"" + E::pace_status_vi(ps) + "\", and the minute's new words -- as "
          "macro_pace.cpp writes them and the notes quote them");

    // ---- README: the section right before 2026.11.16's ----
    const std::size_t r15c = readme.find("### A report in mid-air is no landing, whatever its kind (2026.11.15c)");
    const std::size_t r16b = readme.find("### Skills in turn while hunting too (2026.11.16b)");
    const std::size_t r16 = readme.find("### Lighter on weak PCs, and old features removed (2026.11.16)");
    check(r15c != npos && r16b != npos && r16 != npos && r15c < r16b && r16b < r16 &&
              readme.find("### ", r16b + 1) == r16 && has(readme, "Since 2026.11.16b (not yet run on the game).") &&
              has(readme, "*\"" + asked + "\"*") &&
              has(readme, "the switch is read from `cast_paced_v2` (`MacroConfig::kCastPacedKey`)") &&
              has(readme, "`kPaceSkillLearn` (" + n(E::kPaceSkillLearn) + ")") &&
              has(readme, "`kPaceSkillUnlearn` (" + n(E::kPaceSkillUnlearn) + ")") &&
              has(readme, "`kPaceGiveBackMost` (" + n(E::kPaceGiveBackMost) + ")") &&
              has(readme, "`kSkillRetryMs` (" + n(E::kSkillRetryMs) + " ms), not 100") &&
              has(readme, "`MacroEngine::swing_yields` tells the hunter (`Hunter::set_swing_yield`)") &&
              has(readme, "This was chosen over leaving the attack key out of the rotation"),
          "README: the 2026.11.16b section right before 16's, with the constants it names and the R2 choice");

    // ---- START-HERE (package.ps1): the paragraph after 16's ----
    const std::size_t p16 = pack.find("Lighter on weak PCs, and old features removed (2026.11.16, not yet tried "
                                      "on the game).");
    const std::size_t p16b = pack.find("Skills in turn while hunting too (2026.11.16b, not yet tried on the game).");
    check(p16 != npos && p16b != npos && p16 < p16b &&
              has(pack, "The tick Ra chieu theo vong is now ON by default, also for a macro.ini saved before") &&
              has(pack, "the round presses it in its turn and the hunter does not press it on top") &&
              has(pack, "To switch it off: untick Ra chieu theo vong on the Tu dung ky nang row (kept after a "
                        "restart; by hand, cast_paced_v2=false in configs\\macro.ini with GoBot closed)."),
          "START-HERE: the 2026.11.16b paragraph after 16's, on by default, the attack key, and how to untick");

    // ---- no note says it is off while hunting, or off as shipped, as of now ----
    check(!has(skills, "chỉ dành cho macro chạy một mình") && !has(skills, "Bỏ dấu (mặc định)") &&
              !has(readme, "The hunter's `run_skills` is not paced in this release") &&
              has(readme, "Since 2026.11.16b it paces with the hunter running too") &&
              !has(pack, "Only with Tu danh quai off.") && has(pack, "In this version only with Tu danh quai off.") &&
              !has(ini, "no hunter running, no key sequence") &&
              has(doc, "Trong bản này (từ bản 2026.11.16b thì khác: chạy cả khi Tự đánh quái chạy, xem bản đó) ô chỉ "
                       "dùng khi Tự đánh quái tắt"),
          "the old \"only with Tự đánh quái off\" and \"off as shipped\" words gone or said to be 2026.11.08's");
#else
    skip("no source folder\n");
#endif
}

// ------------------------------------------------------------ 2026.11.17

/// 2026.11.17's notes against the code: the MP bar the EXP bar's tip covered,
/// HP and MP by the packets alone (the owner's "chỉ dùng gói tin"), the
/// potions' brake, and the skills even while hunting -- DOC-TRUOC.txt's entry
/// first in section 5, README's section right before 2026.11.16b's,
/// START-HERE's paragraph after 16b's, macro.ini's and app.ini's comments;
/// each number and log line as the code has it, and no note left saying the
/// old check decides HP and MP.
void test_notes_1117() {
    section("The 2026.11.17 notes against the code: the MP bar, HP/MP by the packets alone, the skills while hunting");
#ifdef GOBOT_SOURCE_DIR
    const std::string doc = flat(source("/DOC-TRUOC.txt"));
    const std::string readme = flat(source("/README.md"));
    const std::string pack = flat(source("/package.ps1"));
    const std::string ini = flat(source("/configs/macro.ini"), true);
    const std::string app_ini = flat(source("/configs/app.ini"), true);
    const std::string trust_hpp = source("/include/gobot/vitals_trust.hpp");
    const std::string macro_hpp = source("/include/gobot/macro.hpp");
    const std::string hunter_hpp = source("/include/gobot/hunter.hpp");
    const std::string vitals_gui = source("/apps/gobot_gui/packets_vitals.cpp");
    const std::string reader = source("/src/vitals_reader.cpp");
    const std::string macro = source("/src/macro.cpp");
    const std::string pace = source("/src/macro_pace.cpp");
    const std::string world = source("/src/packet_world.cpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    if (doc.empty() || readme.empty() || pack.empty() || ini.empty() || app_ini.empty() || trust_hpp.empty() ||
        macro_hpp.empty() || hunter_hpp.empty() || vitals_gui.empty() || reader.empty() || macro.empty() ||
        pace.empty() || world.empty() || view.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto n = [](auto v) { return std::to_string(v); };
    using E = MacroEngine;
    using R = VitalsReader;
    const std::string asked = "check lại log mới nhất nhé, tìm thanh MP lỗi, skill ra không đồng đều";
    const std::string chose = "không mình muốn đổi sang theo phương án chỉ dùng gói tin";
    // The tenths the Vietnamese notes write with a comma: 0,5% and 0,8 giây.
    const std::string down_pct_vi = n(static_cast<int>(E::kDownHpPct)) + "," +
                                    n(static_cast<int>(E::kDownHpPct * 10.0f) % 10);
    const std::string down_s_vi = n(E::kSkillsDownMs / 1000) + "," + n(E::kSkillsDownMs % 1000 / 100);
    const int shadow = static_cast<int>(R::kCaptionShadowPct);
    const int behind = static_cast<int>(R::kBehindFillPct);

    // ---- the numbers the notes quote ----
    check(BarCompare::kBeforeMs == 400 && BarCompare::kLeadMs == 900 && kLinkQuietMs == 5000 &&
              R::kCaptionShadowPct == 54.0f && R::kBehindFillPct == 5.0f &&
              has(macro_hpp, "static constexpr int kBrakeAfterChecks = 3;") &&
              has(vitals_gui, "constexpr std::uint64_t kSwitchGapMs = 60'000;") && E::kPaceFloorMaxMs == 12'000 &&
              E::kPaceSettleMs == 1'000 && E::kPaceFloorCheckTurns == 3 && E::kPaceFloorUses == 20 &&
              E::kPaceProbeMs == 1'000 && E::kSkillsDownMs == 800 && E::kDownHpPct == 0.5f &&
              E::kPaceStaleMs == 2000 && Hunter::kDashGuessPx == 320,
          "the compare's " + n(BarCompare::kBeforeMs) + "/" + n(BarCompare::kLeadMs) + " ms, the server quiet " +
              n(kLinkQuietMs) + " ms, the fill " + n(behind) + " points and the shadow " + n(shadow) +
              "%, the brake after 3, a switch said once a minute; floors at most " + n(E::kPaceFloorMaxMs) +
              " ms, settling " + n(E::kPaceSettleMs) + " ms, checked after " + n(E::kPaceFloorCheckTurns) +
              " turns up to " + n(E::kPaceFloorUses) + ", a probe every " + n(E::kPaceProbeMs) + " ms, down after " +
              n(E::kSkillsDownMs) + " ms under " + down_pct_vi + "%");

    // ---- the code the notes describe: no gate, its lines, the brake, the probe, no guessed dash ----
    check(!has(trust_hpp, "class BarCheck") && !has(trust_hpp, "HandOver") && !has(trust_hpp, "MpStandIn") &&
              has(trust_hpp, "class BarCompare") && has(trust_hpp, "bool vitals_usable(const VitalsUsable& in") &&
              has(vitals_gui, "\"Vitals: %s from the packets -- the potions, the jobs' HP guards and the pace%s\"") &&
              has(vitals_gui, "\"Vitals: %s from the screen until the packets have it again -- %s%s\"") &&
              has(vitals_gui, "line << \"Vitals: from the packets HP \"") &&
              has(vitals_gui, "\" of the minute (to and from the screen: HP \"") &&
              has(vitals_gui, "\"); the bars' figures beside them, for the log: HP \"") &&
              !has(vitals_gui, "Vitals check:") &&
              has(reader, "\"Vitals: %s figures %d/%d (%.0f%%) refused -- the bar is filled to %.0f%%: its \"") &&
              has(reader, "\"first digits covered (the EXP bar's tip?)%s\"") &&
              has(macro, "\"Potion check: %s key %s pressed %d times and the reading never rose (%.0f%%) -- the \"") &&
              has(macro, "\"Potion check: %s key %s -- the reading %s (%.0f%%): its quicker presses back\"") &&
              has(pace, "\"Paced: no cast of yours for %s while %llu key(s) went out, the packets coming -- the game \"") &&
              has(pace, "\"takes no key: one key a second until your next cast\"") &&
              has(pace, "\"Paced: your cast came after %s of the game taking no key -- the skills one after another "
                        "again\"") &&
              has(pace, "\"; skips taught %llu, not taught: %llu after a hold or wait, %llu beside another's cast, "
                        "%llu \"") &&
              has(pace, "\"longer than its re-cast, %llu probing; floors capped %llu; probing %s; held dead %.1f s; "
                        "buff \"") &&
              has(pace, "if (k.min_recast_ms != 0) part += \", re-cast \" + pace_seconds(k.min_recast_ms);") &&
              has(world, "\"); the dash not learned yet -- no key held for it\"") && !has(hunter_hpp, "kDashGuessKeys") &&
              has(view, "label(\"Đọc được HP; MP theo gói tin (số MP trên thanh đang bị che)\");") &&
              has(view, "label(\"Đọc được HP; số MP đang bị che (đầu thanh EXP) -- chờ\");"),
          "the code as the notes say: no BarCheck, HandOver or stand-in; the switch, minute, refusal, brake, probe "
          "and Paced lines; the per-key re-cast; no dash key guessed; the checklist's two covered-MP lines");

    // ---- DOC-TRUOC.txt: the entry first in section 5, over 16b's ----
    const std::size_t sec5 = doc.find("5. CÓ GÌ MỚI, PHẦN NÀO LÀ THỬ NGHIỆM");
    const std::size_t e17 = doc.find("- Mới trong bản 2026.11.17 (sửa thanh MP đọc sai làm bình MP bị bấm liên tục, "
                                     "HP/MP nay chỉ theo gói tin, và kỹ năng ra không đều khi đánh quái; theo log của "
                                     "bạn đêm 16/11 sang 17/11; chưa thử trên game thật).");
    const std::size_t e16b = doc.find("- Mới trong bản 2026.11.16b (");
    check(sec5 != npos && e17 != npos && e16b != npos && doc.find("- Mới trong bản", sec5) == doc.find("- Mới trong bản 2026.11.24 (") && doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.24 (") + 1) == doc.find("- Mới trong bản 2026.11.17c (") && doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.17c (") + 1) == doc.find("- Mới trong bản 2026.11.17b (") &&
              doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.17b (") + 1) == e17 &&
              doc.find("- Mới trong bản", e17 + 1) == e16b && has(doc, "Bạn nhờ: \"" + asked + "\".") &&
              has(doc, "\"15542/16813\" bị đọc thành \"542/16813\" (3%)") &&
              has(doc, "Bạn chọn \"chỉ dùng gói tin\": HP và MP nay theo gói tin, không so với màn hình nữa. Có "
                       "gói tin là dùng ngay; thanh HP/MP trên màn hình chỉ được dùng khi không có gói tin:") &&
              has(doc, "hay game im hơn " + n(kLinkQuietMs / 1000) + " giây. Có gói tin lại là dùng gói tin ngay.") &&
              has(doc, "Không còn \"20 lần khớp\", \"đang nghi\" hay \"thôi tin\".") &&
              has(doc, "Đổi lại: nếu một ngày game đổi cách gửi HP/MP thì GoBot không tự biết.") &&
              has(doc, "nhỏ hơn hẳn (quá " + n(behind) + " điểm %) phần thanh đang có màu thì bị coi là bị che") &&
              has(doc, "MP đo được từ " + n(shadow) + "% trở lên thì GoBot coi là chưa đọc được MP") &&
              has(doc, "\"Đọc được HP; MP theo gói tin (số MP trên thanh đang bị che)\"") &&
              has(doc, "\"Đọc được HP; số MP đang bị che (đầu thanh EXP) -- chờ\"") &&
              has(doc, "Phanh bình: một phím bình bấm 3 lần liền mà số đọc được không tăng") &&
              has(doc, "\"Potion check: MP key G pressed 3 times and the reading never rose (3%) -- ...\"") &&
              has(doc, "Nay chưa học được thì không phím nào bị coi là phím lướt") &&
              has(doc, "và không quá " + n(E::kPaceFloorMaxMs / 1000) + " giây") &&
              has(doc, "lần game không nhận phím trong " + n(E::kPaceSettleMs / 1000) + " giây sau lúc giữ kỹ năng") &&
              has(doc, "được bấm thử lại sau " + n(E::kPaceFloorCheckTurns) + " lượt (trước đây " +
                           n(E::kPaceFloorUses) + " lượt)") &&
              has(doc, "không thấy chiêu nào ra trong " + n(E::kPaceStaleMs / 1000) + " giây") &&
              has(doc, "chỉ bấm thử một phím mỗi giây tới khi chiêu ra lại") &&
              has(doc, "HP dưới " + down_pct_vi + "% quá " + down_s_vi + " giây thì không bấm phím kỹ năng nào") &&
              has(doc, "thấy chiêu buff đã ra thì không bấm nốt các lần còn lại") &&
              has(doc, "\"Vitals: MP from the screen until the packets have it again -- ...\"") &&
              has(doc, "\"Vitals: MP figures ... refused\"") && has(doc, "\"skips taught N, not taught: ...\"") &&
              has(doc, "\"Paced: no cast of yours for ...\"") && has(doc, "\"Dash (packets):\""),
          "DOC-TRUOC.txt: the 2026.11.17 entry first in section 5, over 16b's, with the owner's words and choice, "
          "the packets whenever they have it, the covered figures, the brake, the four skill causes and the log "
          "lines to send");

    // ---- the old check no longer said to decide HP and MP, in the notes still read as now ----
    check(has(doc, "(Bỏ ở bản 2026.11.17, theo lựa chọn của bạn: HP/MP có gói tin là dùng ngay, không so với màn "
                   "hình nữa; thanh trên màn hình chỉ dùng khi không có gói tin. Xem bản 2026.11.17.)") &&
              has(doc, "vitals=auto (HP/MP; từ bản 2026.11.17 auto là có gói tin là dùng, không so trước)") &&
              !has(readme, "| `vitals` | `auto` | your HP and MP from the packets once each bar's check trusts them:") &&
              has(readme, "| `vitals` | `auto` | your HP and MP from the packets whenever they have them, with no "
                          "check first since 2026.11.17") &&
              has(readme, "go by the packets whenever the packets have them, since 2026.11.17 with no check against "
                          "the bars on the screen first") &&
              has(readme, "Until 2026.11.16b they went through a check per bar against the bars on the screen, as "
                          "follows; none of it decides anything now.") &&
              !has(readme, "A bar is trusted after 20 agreements") &&
              has(readme, "your HP and MP whenever they have them (unchecked since 2026.11.17)") &&
              !has(readme, "your HP and MP, the bag, **Thử** and map changes still use the packets once checked") &&
              has(app_ini, "Not checked first (2026.11.17): auto takes them from the packets whenever the packets "
                           "have them, and reads the bars on the screen only when they do not.") &&
              has(app_ini, "all but vitals, which since 2026.11.17 is not checked first") &&
              has(ini, "Since 2026.11.17 HP and MP go by the game's packets whenever GoBot has them ([Packets] vitals "
                       "in app.ini), with no check against these bars first"),
          "README, DOC-TRUOC.txt, app.ini and macro.ini: HP and MP by the packets whenever they have them, the old "
          "trust check said to be gone where the notes read as now");

    // ---- the 2026.11.17 review: the notes as the code and the log have it ----
    // The intro and the diagram no longer say HP and MP are checked against
    // the screen; a map load is no switch to the screen (vitals_unusable_why
    // has no such rule: the last values carry across it); the skills' flood
    // of 169-230 a minute is the death of 00:07-00:09 alone, the 00:21-00:22
    // minute being re-presses against wrong floors; and the death hold goes by
    // the HP GoBot goes by, which the packets kept at 27% through that death.
    check(!has(readme, "your HP and MP, each checked against the screen") &&
              has(readme, "your HP and MP -- since 2026.11.17 taken from the packets whenever they have them, the "
                          "bars read only without them") &&
              !has(readme, "the monsters, your place, HP, MP and bag, each") &&
              has(readme, "HP and MP straight from the packets (2026.11.17)") &&
              !has(readme, "before the first value after a start or a map load") &&
              has(readme, "A map load does not switch to the screen") &&
              !has(readme, "or stunned (00:07:19, 00:21:37-00:22:40)") && !has(readme, "169-230") &&
              has(readme, "220-230 presses a minute. The 169 of the minute to 00:22:55 were") &&
              has(readme, "In the 00:07 death the packets kept HP at 27% while the screen read 0") &&
              !has(doc, "vừa mở GoBot hay vừa qua cửa mà game") &&
              has(doc, "Qua cửa thì không chuyển sang màn hình") && !has(doc, "Bị choáng hay đã chết") &&
              !has(doc, "169-230") && has(doc, "3. Nhân vật đã chết (HP 0 trên thanh lúc 00:07:17") &&
              has(doc, "lần chết lúc 00:07 gói tin vẫn báo") &&
              !has(pack, "just after a start or a door") &&!has(pack, "With the character down or stunned") &&
              has(pack, "With the character dead (00:07-00:09 that night)"),
          "README, DOC-TRUOC.txt and START-HERE after the 2026.11.17 review: no HP/MP check in the intro or the "
          "diagram, no screen after a map load, the flood the death's alone, the death hold by the HP gone by");

    // ---- README: the section right before 2026.11.16b's ----
    const std::size_t r17 = readme.find("### MP bar and skills while hunting (2026.11.17)");
    const std::size_t r16b = readme.find("### Skills in turn while hunting too (2026.11.16b)");
    check(r17 != npos && r16b != npos && r17 < r16b && readme.find("### ", r17 + 1) == r16b &&
              has(readme, "Since 2026.11.17 (not yet run on the game).") && has(readme, "*\"" + asked + "\"*") &&
              has(readme, "*\"" + chose + "\"*") &&
              has(readme, "within `kLinkQuietMs` (" + n(kLinkQuietMs / 1000) + " s)") &&
              has(readme, "`BarCheck`, `HandOver` and the 20 agreements are deleted") &&
              has(readme, "`kBeforeMs` (" + n(BarCompare::kBeforeMs) + " ms)") &&
              has(readme, "`kLeadMs` (" + n(BarCompare::kLeadMs) + " ms)") &&
              has(readme, "`kBehindFillPct` (" + n(behind) + ")") &&
              has(readme, "`kCaptionShadowPct` (" + n(shadow) + "%)") && has(readme, "`kBrakeAfterChecks`, 3") &&
              has(readme, "`kPaceFloorMaxMs` (" + n(E::kPaceFloorMaxMs / 1000) + " s)") &&
              has(readme, "`kPaceSettleMs` (" + n(E::kPaceSettleMs / 1000) + " s)") &&
              has(readme, "`kPaceFloorCheckTurns` (" + n(E::kPaceFloorCheckTurns) + ")") &&
              has(readme, "`kPaceProbeMs` (" + n(E::kPaceProbeMs / 1000) + " s)") &&
              has(readme, "`kSkillsDownMs` (" + n(E::kSkillsDownMs) + " ms)") &&
              has(readme, "`Vitals: MP from the screen until the packets have it again -- <why>`") &&
              has(readme, "`Vitals: MP from the packets -- the potions, the jobs' HP guards and the pace`") &&
              has(readme, "`Vitals: MP figures 542/16813 (3%) refused -- the bar is filled to 57%: its first digits "
                          "covered (the EXP bar's tip?)`") &&
              has(readme, "`Paced: no cast of yours for … the packets coming -- the game takes no key: one key a "
                          "second until your next cast`") &&
              has(readme, "`; skips taught N, not taught: N after a hold or wait, N beside another's cast, N longer "
                          "than its re-cast, N probing; floors capped N; probing N.N s; held dead N.N s; buff presses "
                          "spared N`") &&
              has(readme, "`Dash (packets): … the dash not learned yet -- no key held for it`"),
          "README: the 2026.11.17 section right before 16b's, with the owner's words and choice, the constants it "
          "names and the log lines as the code writes them");

    // ---- START-HERE (package.ps1): the paragraph after 16b's ----
    const std::size_t p16b = pack.find("Skills in turn while hunting too (2026.11.16b, not yet tried on the game).");
    const std::size_t p17 = pack.find("MP bar and skills while hunting (2026.11.17, not yet tried on the game).");
    const std::size_t walk = pack.find("Walking to a monster, the hunter holds the arrow key down");
    check(p16b != npos && p17 != npos && walk != npos && p16b < p17 && p17 < walk &&
              has(pack, "HP and MP now go by the game's packets whenever GoBot has them, with no check against the "
                        "screen first (the owner's choice)") &&
              has(pack, "the game quiet for " + n(kLinkQuietMs / 1000) + " s") &&
              has(pack, "now no key is held for a dash until one is learned") &&
              has(pack, "and at " + n(E::kPaceFloorMaxMs / 1000) + " s.") &&
              has(pack, "A potion pressed 3 times in a row with no rise goes at its own cooldown"),
          "START-HERE: the 2026.11.17 paragraph after 16b's: the packets with no check, the bars without them, no "
          "dash guessed, the floors' cap and the brake");

    // ---- macro.ini: what the settings now do ----
    check(has(ini, "Since 2026.11.17, 3 presses in a row with no rise (HP not falling further either) stop it, and "
                   "potion_urgent_ms below, until the reading rises") &&
              has(ini, "a learned cooldown is never longer than the shortest gap the skill was ever cast again after, "
                       "nor " + n(E::kPaceFloorMaxMs / 1000) + " s, and is checked after " +
                           n(E::kPaceFloorCheckTurns) + " turns") &&
              has(ini, "the buff's cast seen ends the rest of its presses: buff_presses is the most"),
          "macro.ini: the potions' brake, the floors' cap and the buff's presses ended by its cast");
#else
    skip("no source folder\n");
#endif
}

// ------------------------------------------------------------ 2026.11.17b

/// 2026.11.17b's notes against the code: inviting somebody already in a party
/// (the owner's "cứ mở khung pt", and "ok làm A B C đi") -- an invite that
/// did not lead to a join held off, the notice clicked with the mouse held
/// 2.5 s at most, the player list shut after every turn. DOC-TRUOC.txt's
/// entry first in section 5, README's section right before 2026.11.17's,
/// START-HERE's paragraph after 17's; each number and log line as the code
/// has it.
void test_notes_1117b() {
    section("The 2026.11.17b notes against the code: invites that came to nothing held off, the notice closed with "
            "a short hold, the player list shut");
#ifdef GOBOT_SOURCE_DIR
    const std::string doc = flat(source("/DOC-TRUOC.txt"));
    const std::string readme = flat(source("/README.md"));
    const std::string pack = flat(source("/package.ps1"));
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string list_cpp = source("/src/player_list.cpp");
    const std::string list_hpp = source("/include/gobot/player_list.hpp");
    if (doc.empty() || readme.empty() || pack.empty() || gui.empty() || list_cpp.empty() || list_hpp.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto n = [](auto v) { return std::to_string(v); };
    using T = InviteTries;
    using R = InviteReply;
    const std::string asked = "Hiện tại có bug khi pt người đã có pt thì hiển thị thông báo sau đó bot không phản ứng "
                              "gì nữa và cứ mở khung pt.";
    const std::string chose = "ok làm A B C đi";

    // ---- the numbers the notes quote ----
    check(T::kJoinWaitMs == 15'000 && T::kFailHoldMs == 60'000 && T::kFailHoldLongMs == 600'000 &&
              T::kFailsForLong == 2 && T::kStaleWaitMs == 10'000 && R::kCloseHoldMs == 2500 && R::kCloseTries == 3 &&
              R::kRetryGapMs == 1'000 && R::kStuckMs == 60'000 && has(list_hpp, "static constexpr int kCloseWaitMs = 3000;") &&
              has(list_hpp, "static constexpr int kCloseSettleMs = 300;"),
          "judged " + n(T::kJoinWaitMs / 1000) + " s on, held " + n(T::kFailHoldMs / 1000) + " s, " +
              n(T::kFailHoldLongMs / 60'000) + " min from the " + n(T::kFailsForLong) + "nd in a row; the notice's "
              "click held " + n(R::kCloseHoldMs) + " ms, " + n(R::kCloseTries) + " clicks; the list given 3 s");

    // ---- the log lines the notes quote, as the code writes them ----
    check(has(gui, "\"Party: invite to %s did not lead to a join (%s) -- %d in a row; not invited again for %d s\",") &&
              has(gui, "GB_INFO(\"Party: %s joined after the invite -- %s\", o.who.c_str(), o.why.c_str());") &&
              has(gui, "\"Party: %s asked; held off %d s more after an invite that did not lead to a join -- \"") &&
              has(gui, "\"Party: the game's message after the invite to %s at %d,%d, %s: \\\"%s\\\" -- closed with its Xác \"") &&
              has(gui, "\"Party: the game's message is still up after %d clicks on its Xác Nhận -- no invite for %d s\",") &&
              has(gui, "GB_INFO(\"Party: the player list did not shut after %s's turn -- %s; left as it is\",") &&
              has(list_cpp, "bool PlayerList::close(") && !has(gui, "VK_ESCAPE"),
          "gui_app.cpp: the Party: lines README and DOC-TRUOC.txt quote, and no ESC");

    // ---- DOC-TRUOC.txt: the entry first in section 5, over 17's ----
    const std::size_t sec5 = doc.find("5. CÓ GÌ MỚI, PHẦN NÀO LÀ THỬ NGHIỆM");
    const std::size_t e17b = doc.find("- Mới trong bản 2026.11.17b (mời tổ đội người đã có tổ đội: không còn mở danh "
                                      "sách người chơi liên tục, không còn giữ chuột lâu; theo lời bạn ngày 17/11; "
                                      "chưa thử trên game thật).");
    const std::size_t e17 = doc.find("- Mới trong bản 2026.11.17 (");
    check(sec5 != npos && e17b != npos && e17 != npos && doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.17c (") + 1) == e17b &&
              doc.find("- Mới trong bản", e17b + 1) == e17 && has(doc, "Bạn báo: \"" + asked + "\"") &&
              has(doc, "Bạn chọn: \"" + chose + "\".") &&
              has(doc, n(T::kJoinWaitMs / 1000) + " giây sau lời mời, GoBot xem khung tổ đội của game") &&
              has(doc, "Không vào thì " + n(T::kFailHoldMs / 60'000) + " phút nữa mới mời lại người đó; không vào " +
                           n(T::kFailsForLong) + " lần liền thì " + n(T::kFailHoldLongMs / 60'000) + " phút.") &&
              has(doc, "Người đang bị tạm ngưng có chat từ khoá thì GoBot không mở danh sách người chơi.") &&
              has(doc, "GoBot chỉ giữ chuột chờ hộp tắt tối đa 2,5 giây (trước đây tới " + n(kReactMs / 1000) +
                           " giây") &&
              has(doc, "tối đa " + n(R::kCloseTries) + " lần; vẫn không tắt thì ngưng mời " + n(R::kStuckMs / 60'000) +
                           " phút") &&
              has(doc, "dùng được ở 1024x768, 1280x720 và 1366x768") &&
              has(doc, "Bấm một lần, chờ tối đa 3 giây; không đóng được thì ghi log một lần và để vậy.") &&
              has(doc, "1 phút, 10 phút nếu 2 lần liền không vào"),
          "DOC-TRUOC.txt: the 2026.11.17b entry in section 5 right after 17c's, over 17's, with the owner's words and choice, "
          "the holds, the short click, the list shut at the three sizes; 15b's entry says what replaced its 30 s");

    // ---- README: the section right before 2026.11.17's ----
    const std::size_t r17b = readme.find("### Party: inviting somebody already in a party (2026.11.17b)");
    const std::size_t r17 = readme.find("### MP bar and skills while hunting (2026.11.17)");
    check(r17b != npos && r17 != npos && r17b < r17 && readme.find("### ", r17b + 1) == r17 &&
              has(readme, "Since 2026.11.17b (not yet run on the game).") && has(readme, "*\"" + chose + "\"*") &&
              has(readme, "`kJoinWaitMs` (" + n(T::kJoinWaitMs / 1000) + " s)") &&
              has(readme, "`kStaleWaitMs` (" + n(T::kStaleWaitMs / 1000) + " s)") &&
              has(readme, "`kFailHoldMs` (" + n(T::kFailHoldMs / 60'000) + " min)") &&
              has(readme, "`kFailHoldLongMs` (" + n(T::kFailHoldLongMs / 60'000) + " min)") &&
              has(readme, "`kFailsForLong`-th (" + n(T::kFailsForLong) + ")") &&
              has(readme, "`kCloseHoldMs` (" + n(R::kCloseHoldMs / 1000) + ".5 s)") &&
              has(readme, "`kRetryGapMs` (" + n(R::kRetryGapMs / 1000) + " s)") &&
              has(readme, "`kCloseTries` (" + n(R::kCloseTries) + ")") &&
              has(readme, "`kStuckMs` (" + n(R::kStuckMs / 1000) + " s)") &&
              has(readme, "`kCloseSettleMs` (300 ms)") && has(readme, "`kCloseWaitMs` (3 s)") &&
              has(readme, "`Party: the player list did not shut after <who>'s turn -- <why>; left as it is`") &&
              has(readme, "`Party: invite to <who> did not lead to a join (<why>) -- N in a row; not invited again "
                          "for N s`"),
          "README: the 2026.11.17b section right before 17's, with the owner's words and choice, the constants it "
          "names and the log lines as the code writes them");

    // ---- START-HERE (package.ps1): the paragraph after 17's ----
    const std::size_t p17 = pack.find("MP bar and skills while hunting (2026.11.17, not yet tried on the game).");
    const std::size_t p17b =
        pack.find("Party: inviting somebody already in a party (2026.11.17b, not yet tried on the game).");
    const std::size_t walk = pack.find("Walking to a monster, the hunter holds the arrow key down");
    check(p17 != npos && p17b != npos && walk != npos && p17 < p17b && p17b < walk &&
              has(pack, "is not invited again for 1 minute, 10 minutes after 2 in a row") &&
              has(pack, "with the game's mouse held 2.5 s at most, not 30 s") &&
              has(pack, "clicked again on a later pass, 3 clicks in all, then no invite for 1 minute") &&
              has(pack, "picture at 1024x768, 1280x720 and 1366x768"),
          "START-HERE: the 2026.11.17b paragraph after 17's: the holds, the short click, the list shut");

    // ---- the review's fixes, said as the code does them ----
    check(T::kNotListedHoldMs == 60'000 &&
              has(gui, "\"Party: %s asked but is not among the %d on the player list -- not asked again for %d s\",") &&
              has(readme, "`kNotListedHoldMs` (" + n(T::kNotListedHoldMs / 1000) + " s), the same every time, never "
                          "counted towards the 10 minutes") &&
              !has(readme, "An asker not on the list counts as a miss as well.") &&
              has(doc, "không tính là một lần không vào: GoBot chỉ bỏ qua từ khoá của người đó " +
                           n(T::kNotListedHoldMs / 60'000) + " phút") &&
              !has(doc, "cũng tính là một lần không vào") &&
              has(doc, "Hai người được mời gần nhau mà cả hai đều vào thì cả hai được tính là đã vào.") &&
              has(doc, "kể cả những người khác chat từ khoá cùng lúc (họ chờ lượt sau)."),
          "the review of 2026.11.17b: an asker not on the list held " + n(T::kNotListedHoldMs / 1000) +
              " s in the chat, never counted; two joins at once both credited; a message waiting holds the pass's "
              "other askers -- in the code, README and DOC-TRUOC.txt alike");
#else
    skip("no source folder\n");
#endif
}

// ------------------------------------------------------------ 2026.11.17c

/// 2026.11.17c's notes against the code: the check-in board that stayed up
/// (the owner's "có bảng điểm danh hiển thị mà không thấy tự tắt") -- a look
/// that misses it no longer starts its 30 s over, its title under the game's
/// lines of news is still told where it was, and every reason no ESC went is
/// said. DOC-TRUOC.txt's entry first in section 5, README's section right
/// before 2026.11.17b's, START-HERE's paragraph after 17b's; each number and
/// log line as the code has it.
void test_notes_1117c() {
    section("The 2026.11.17c notes against the code: the check-in board's first sight kept through a missed look, "
            "its title told under the game's news, every reason for no ESC said");
#ifdef GOBOT_SOURCE_DIR
    const std::string doc = flat(source("/DOC-TRUOC.txt"));
    const std::string readme = flat(source("/README.md"));
    const std::string pack = flat(source("/package.ps1"));
    const std::string closer = source("/src/popup_closer.cpp");
    const std::string popups = source("/apps/gobot_gui/simple_popups.cpp");
    if (doc.empty() || readme.empty() || pack.empty() || closer.empty() || popups.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto n = [](auto v) { return std::to_string(v); };
    using P = PopupCloser;
    const std::string asked = "check lại phần tắt thông báo, có bảng điểm danh hiển thị mà không thấy tự tắt";

    // ---- the numbers the notes quote ----
    check(P::kBoardGapMs == 15'000 && P::kBoardGraceMs == 60'000 && kCheckinBoardCoveredScore == 0.60f &&
              kCheckinBoardScore == 0.90f && P::kBlockedMs == 5'000 && P::kBackoffMs == 30'000 &&
              P::kLookMs == 1'000 && P::kSteadyMs == 3'000 && kCheckinBoardLoginWaitMs == 30'000 &&
              has(popups, "constexpr int kBoardUnfocusedSayMs = 10'000;"),
          "the board's sighting kept " + n(P::kBoardGapMs / 1000) + " s out of sight, its covered title at 0.60 " +
              n(P::kBoardGraceMs / 1000) + " s after a whole sight, a gate or a refused hold " +
              n(P::kBlockedMs / 1000) + " s, seen again " + n(P::kSteadyMs / 1000) +
              " s before its ESC, the game behind another window said after 10 s");

    // ---- the log lines the notes quote, as the code writes them ----
    check(has(closer, "\"the check-in board is up again at \"") &&
              has(closer, "\"): the same board -- its wait still counts from its first sight, \"") &&
              has(closer, "\"the check-in board's title at \"") &&
              has(closer, "\" is partly covered (\"") &&
              has(closer, "\", the game's lines of news over it?): still taken as up\"") &&
              has(closer, "\"the check-in board is due its ESC, but the look under the hold did not find it again round \"") &&
              has(closer, "\", whole or covered: nothing pressed; looked for again on the \"") &&
              has(closer, "\" is up too and an ESC would go to it first: no ESC while it is\"") &&
              has(closer, "\" now: not the moment\"") &&
              has(popups, "\"Popups: the check-in board is up and gets no ESC: the game has not been in front for %d s \"") &&
              has(popups, "\"-- nothing is looked at or pressed until it is\","),
          "popup_closer.cpp and simple_popups.cpp: the Popups: lines README quotes");

    // ---- DOC-TRUOC.txt: the entry first in section 5, over 17b's ----
    const std::size_t sec5 = doc.find("5. CÓ GÌ MỚI, PHẦN NÀO LÀ THỬ NGHIỆM");
    const std::size_t e17c = doc.find("- Mới trong bản 2026.11.17c (bảng Sự kiện và điểm danh: vẫn được tắt bằng ESC "
                                      "dù có lúc GoBot nhìn không thấy nó; theo lời bạn ngày 17/11; chưa thử trên game "
                                      "thật).");
    const std::size_t e17b = doc.find("- Mới trong bản 2026.11.17b (");
    // Under 2026.11.24's (the pet's HP food on reaching TổCổLong) since then.
    check(sec5 != npos && e17c != npos && e17b != npos &&
              doc.find("- Mới trong bản", sec5) == doc.find("- Mới trong bản 2026.11.24 (") &&
              doc.find("- Mới trong bản", doc.find("- Mới trong bản 2026.11.24 (") + 1) == e17c &&
              doc.find("- Mới trong bản", e17c + 1) == e17b && has(doc, "Bạn báo: \"" + asked + "\".") &&
              has(doc, "Nay bảng thấy lại ở đúng chỗ cũ trong vòng " + n(P::kBoardGapMs / 1000) +
                           " giây là cùng một bảng: vẫn bấm ESC " + n(kCheckinBoardLoginWaitMs / 1000) +
                           " giây sau khi vào game") &&
              has(doc, "hoặc không thấy quá " + n(P::kBoardGapMs / 1000) + " giây thì mới đếm lại từ đầu.") &&
              has(doc, "bạn tắt rồi mở lại bảng để điểm danh thì đó là bảng mới, vẫn có đủ " +
                           n(kCheckinBoardLoginWaitMs / 1000) + " giây để đọc.") &&
              has(doc, "cũng chờ thêm " + n(P::kSteadyMs / 1000) + " giây mới bấm ESC") &&
              has(doc, "hay bạn vừa chạm chuột, bàn phím thì") &&
              has(doc, "Qua ngày vẫn chờ tới 00:03 như cũ.") &&
              has(doc, "chỉ ở đúng chỗ bảng vừa được thấy rõ trong " + n(P::kBoardGraceMs / 1000) +
                           " giây trước, không bao giờ để nhận một bảng mới") &&
              has(doc, "game không ở trên cùng quá 10 giây") &&
              has(doc, "thì " + n(P::kBlockedMs / 1000) + " giây sau thử lại, không phải " +
                           n(P::kBackoffMs / 1000) + " giây.") &&
              has(doc, "không bao giờ bấm vào bảng."),
          "DOC-TRUOC.txt: the 2026.11.17c entry first in section 5, over 17b's, with the owner's words, the 15 s, "
          "the player's own board, the 3 s, the covered title's 60 s, the reasons said and the 5 s");

    // ---- README: the section right before 2026.11.17b's ----
    const std::size_t r17c = readme.find("### Check-in board closed even after a missed look (2026.11.17c)");
    const std::size_t r17b = readme.find("### Party: inviting somebody already in a party (2026.11.17b)");
    check(r17c != npos && r17b != npos && r17c < r17b && readme.find("### ", r17c + 1) == r17b &&
              has(readme, "Since 2026.11.17c (not yet run on the game).") && has(readme, "*\"" + asked + "\"*") &&
              has(readme, "`PopupCloser::kBoardGapMs` (" + n(P::kBoardGapMs / 1000) + " s)") &&
              has(readme, "one out of sight for over " + n(P::kBoardGapMs / 1000) + " s") &&
              has(readme, "been back `kSteadyMs` (" + n(P::kSteadyMs / 1000) + " s)") &&
              has(readme, "60 s (`PopupCloser::kBoardGraceMs`)") &&
              has(readme, "`Popups: not closing the check-in board now: not the moment`") &&
              has(readme, "`kCheckinBoardCoveredScore` (0.60)") && has(readme, "`kCheckinBoardScore` (0.90)") &&
              has(readme, "`kBlockedMs` (" + n(P::kBlockedMs / 1000) + " s), not `kBackoffMs` (" +
                              n(P::kBackoffMs / 1000) + " s)") &&
              has(readme, "`Popups: the check-in board is up again at X,Y after N ms out of sight (...): the same "
                          "board -- its wait still counts from its first sight, N s ago`") &&
              has(readme, "`Popups: the check-in board is due its ESC, but the look under the hold did not find it "
                          "again round X,Y, whole or covered: nothing pressed; looked for again on the next turn`") &&
              has(readme, "`Popups: the check-in board is up and gets no ESC: the game has not been in front for 10 s "
                          "-- nothing is looked at or pressed until it is`") &&
              has(readme, "`build\\handoff\\popups\\testdata\\checkin_live_1024x768.png`"),
          "README: the 2026.11.17c section right before 17b's, with the owner's words, the constants it names, the "
          "log lines as the code writes them and the saved frame the tests read");

    // ---- START-HERE (package.ps1): the paragraph after 17b's ----
    const std::size_t p17b =
        pack.find("Party: inviting somebody already in a party (2026.11.17b, not yet tried on the game).");
    const std::size_t p17c = pack.find("Check-in board closed even after a missed look (2026.11.17c, not yet tried on "
                                       "the game).");
    const std::size_t walk = pack.find("Walking to a monster, the hunter holds the arrow key down");
    check(p17b != npos && p17c != npos && walk != npos && p17b < p17c && p17c < walk &&
              has(pack, "a board seen again where it was within 15 s is the same board") &&
              has(pack, "one the player closes and opens again later is a new board") &&
              has(pack, "and seen again it gets no ESC for 3 s") &&
              has(pack, "another job's hold puts the board off 5 s, not 30 s"),
          "START-HERE: the 2026.11.17c paragraph after 17b's: the 15 s, the player's own board, the 3 s, the covered "
          "title, the 5 s");
#else
    skip("no source folder\n");
#endif
}

/// A start owed (2026.11.24, the owner: "Kiểm tra các phần khác xem có phần
/// nào không làm được thì dừng không, cần thay cơ chế này bằng 1 cơ chế
/// khác"): Tự đánh quái, the macro a resume or a tick asked for, and the cold
/// start's jobs, refused by something that passes -- Npcap's service up late,
/// a doubt seen over a moment later, the detector -- asked again every 30 s
/// while the run is on, and at once when a watcher sees its doubt over; never
/// after the player's own stop. The rule is pure (gobot/owed_start.hpp);
/// where GoBot's window asks by it is read off its source.
void test_owed_start() {
    section("A start owed (2026.11.24): Tự đánh quái, the macro and the cold start's jobs refused by something that "
            "passes, asked again every 30 s while the run is on -- never after the player's own stop");

    // ---- the rule ----
    check(kOwedAskMs == static_cast<std::uint64_t>(kReactMs) && kOwedAskMs == 30'000,
          "asked again every kReactMs: the owner's 30 s for laggy PCs");
    const std::uint64_t t = 5'000'000;
    check(!owed_due(t + 29'999, t, true, true, false) && owed_due(t + 30'000, t, true, true, false) &&
              owed_due(t + 95'000, t, true, true, false),
          "no sooner than 30 s after the last ask, and at any time after it");
    check(owed_due(t, 0, true, true, false) && owed_due(1, 0, true, true, false),
          "a last ask of 0 is at once -- a watcher saw its doubt over -- even on a clock less than 30 s old");
    check(!owed_due(t + 60'000, t, false, true, false) && !owed_due(t + 60'000, t, true, false, false) &&
              !owed_due(t + 60'000, t, true, true, true) && !owed_due(t, 0, false, true, false) &&
              !owed_due(t, 0, true, true, true),
          "never with the run over, the tick taken off or no key on, or the start made meanwhile -- at once or not");
    check(!owed_due(t, t + 1'000, true, true, false),
          "a last ask later than now is no ask due: the clocks' difference never wraps round");
    check(owed_dropped(false, true, false) && owed_dropped(true, false, false) && owed_dropped(true, true, true) &&
              !owed_dropped(true, true, false),
          "forgotten once the run is over, it is no longer wanted or it started; kept while it waits");
    {
        // Refused at 0 and refused at every ask: asked again at 30, 60, 90
        // and 120 s -- each ask moves the clock -- never in between.
        std::uint64_t asked = t;
        std::vector<std::uint64_t> at;
        for (std::uint64_t now = t; now <= t + 120'000; now += 100)
            if (owed_due(now, asked, true, true, false)) {
                at.push_back(now - t);
                asked = now;
            }
        check(at == std::vector<std::uint64_t>{ 30'000, 60'000, 90'000, 120'000 },
              "a start refused at 0 and at every ask after: asked at " + ms_list(at));
    }

#ifdef GOBOT_SOURCE_DIR
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string hpp = source("/apps/gobot_gui/gui_app.hpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    if (gui.empty() || hpp.empty() || view.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }

    // ---- tick() asks, after the doubts' watchers and before the cold start ----
    const std::string tick = body(gui, "void GuiApp::tick() {");
    check(in_order(tick, { "watch_left_held();", "watch_chat_doubt();", "owed_tick();",
                           "if (keeper_reattach_.exchange(false)) {" }),
          "tick(): what is owed asked after watch_left_held and watch_chat_doubt -- in the very tick either sees its "
          "doubt over -- and before a cold start's jobs");
    const std::string owed = body(view, "void GuiApp::owed_tick() {");
    check(in_order(owed, { "if (!hunt_owed_ && !macro_owed_ && !jobs_owed_) return;", "if (simple_cold_start_) return;",
                           "if (hunt_owed_ && owed_dropped(run, hunt_config_.enabled, hunting_)) hunt_owed_ = false;",
                           "if (macro_owed_ && owed_dropped(run, keys, macro_running_)) macro_owed_ = false;",
                           "if (jobs_owed_ && owed_dropped(run && keeper, true, false)) jobs_owed_ = false;",
                           "const bool jobs = jobs_owed_ && owed_due(now, owed_ms_, run && keeper, true, false);",
                           "const bool hunt = hunt_owed_ && owed_due(now, owed_ms_, run, hunt_config_.enabled, "
                           "hunting_);",
                           "const bool macro = macro_owed_ && owed_due(now, owed_ms_, run, keys, macro_running_);",
                           "if (trash_left_held_ || (chat_doubt_ && !chat_doubt_watched_) || (chat_running_ && "
                           "chat_quit_)) return;",
                           "if (!input_.window_valid()) return;", "owed_ms_ = now;", "owed_asking_ = true;",
                           "simple_start_jobs();", "if (macro) start_macro();", "if (hunt) start_hunt();",
                           "owed_asking_ = false;" }) &&
              has(owed, "const bool run = simple_running();") &&
              has(owed, "const bool keeper = keeper_running_ && !keeper_oneshot_ && !keeper_stop_;") &&
              has(owed, "const bool keys = any_key_on(macro_config_);"),
          "owed_tick: forgotten once not wanted; asked no sooner than kOwedAskMs after the last ask (owed_due), with "
          "the run on and no cold start under way; a doubt keys_may_start would look at, or no game window, waits; "
          "through simple_start_jobs, start_macro and start_hunt");
    check(!has(owed, "keys_may_start()") && !has(owed, "focus_game_to_start(") && !has(owed, "grab(") &&
              !has(owed, "bin_looks_clear(") && !has(owed, "chat_line_looks_shut(") && !has(owed, "hunter_->start()") &&
              !has(owed, "macro_->start()"),
          "owed_tick itself looks at nothing, brings nothing forward and starts no engine: each start's own checks do, "
          "as at BẮT ĐẦU");
    check(in_order(owed, { "GB_INFO(\"Owed: asked again -- %s started\",", "simple_note(note, 12000);" }) &&
              has(view, "\"GoBot thử lại: Tự đánh quái đã chạy.\""),
          "and what it started is said once, in the log and on the card");

    // ---- what becomes owed: only through owe_start, only with the run on ----
    const std::string owe = body(view, "void GuiApp::owe_start(std::atomic<bool>& owed, const char* what) {");
    check(in_order(owe, { "if (!simple_running() || simple_cold_start_) return;", "owed_ms_ = now_ms();",
                          "if (!owed.exchange(true))", "GB_INFO(" }) &&
              !has(gui + view, "_owed_ = true") && !has(gui + view, "_owed_.store(true"),
          "owe_start, the one way a start becomes owed: with the run on and no cold start under way, the refusal "
          "counted as the last ask, said in the log once as it becomes owed");
    const std::string resume = body(view, "int GuiApp::resume_after_bin() {");
    check(in_order(resume, { "out |= macro_running_ ? kResumedMacro : kResumeRefused;",
                             "if (!macro_running_) owe_start(macro_owed_, \"The macro\");",
                             "out |= hunting_ ? kResumedHunt : kResumeRefused;",
                             "if (!hunting_) owe_start(hunt_owed_, \"Hunting\");" }),
          "resume_after_bin: the macro or the hunter its start refused is owed -- no longer lost with the flags it "
          "cleared");
    const std::string changed = body(view, "void GuiApp::simple_changed() {");
    check(in_order(changed, { "if (hunt_config_.enabled && !hunting_) {", "start_hunt();",
                              "if (!hunting_) owe_start(hunt_owed_, \"Hunting\");",
                              "if (!hunt_config_.enabled) hunt_owed_ = false;",
                              "if (!hunt_config_.enabled && hunting_) stop_hunt();", "start_macro();",
                              "if (!macro_running_) owe_start(macro_owed_, \"The macro\");" }),
          "simple_changed: Tự đánh quái ticked on mid-run and refused is owed, a key's macro likewise; Tự đánh quái "
          "ticked off owes nothing");
    const std::string jobs = body(view, "void GuiApp::simple_start_jobs() {");
    check(in_order(jobs, { "left_held_resume_macro_ = false;", "hunt_owed_ = false;", "macro_owed_ = false;",
                           "owed_why_.clear();", "if (!keys_may_start()) {",
                           "if (keeper_running_ && !keeper_oneshot_ && !keeper_stop_) owe_start(jobs_owed_, "
                           "\"The run's jobs\");",
                           "return;", "jobs_owed_ = false;", "simple_track_run(true);", "start_hunt();",
                           "start_popups();",
                           "if (hunt_config_.enabled && !unnamed && !hunting_) owe_start(hunt_owed_, \"Hunting\");",
                           "spot_refused_vi_ + (hunt_owed_ ? owed_again_vi() : std::string())" }),
          "simple_start_jobs: BẮT ĐẦU forgets what was owed and says each refusal; its keys_may_start refusal "
          "owed only with the cold start's keeper running (BẮT ĐẦU with the game up is the player's own); Tự đánh "
          "quái refused owed after every other start, once whether the run is on is known");

    // ---- the doubts' watchers ask at once ----
    const std::string now_ask = body(view, "bool GuiApp::owed_now() {");
    const std::string watch = body(gui, "void GuiApp::watch_left_held() {");
    const std::string chat_watch = body(gui, "void GuiApp::watch_chat_doubt() {");
    check(in_order(now_ask, { "if (!hunt_owed_ && !macro_owed_ && !jobs_owed_) return false;", "owed_ms_ = 0;",
                              "return true;" }) &&
              in_order(watch, { "bin_seen_clear();", "const int resumed = resume_after_bin();",
                                "const bool owed = owed_now();",
                                "if (resumed < 0 || (resumed > 0 && (resumed & kResumeRefused))) return;",
                                "\"Thùng rác không còn giữ gì: GoBot thử bật lại những việc chưa bật được.\"",
                                "Bấm DỪNG LẠI rồi BẮT ĐẦU" }) &&
              has(watch, "const int resumed = resume_after_bin();\n        owed_now();") &&
              in_order(chat_watch, { "chat_seen_shut();", "const bool owed = owed_now();",
                                     "\"Ô chat trong game đã đóng: GoBot thử bật lại những việc chưa bật được.\"",
                                     "\"Ô chat trong game đã đóng. Bấm BẮT ĐẦU" }),
          "watch_left_held and watch_chat_doubt: the doubt over asks what is owed in this tick, and the note says "
          "GoBot tries rather than \"Bấm BẮT ĐẦU\"");

    // ---- forgotten at the player's own hand, kept through the bin's doubt ----
    check(in_order(slice(gui, "bool GuiApp::stop_jobs() {", "take_hold(kStopHold);"),
                   { "left_held_resume_macro_ = false;", "hunt_owed_ = false;", "macro_owed_ = false;",
                     "jobs_owed_ = false;" }),
          "stop_jobs -- DỪNG, F12, closing GoBot -- forgets all that is owed, beside the bin's resume");
    const std::string features = body(gui, "void GuiApp::draw_feature_list() {");
    // The 2026.11.24 review: the owed flag cleared on the stop branch only; a
    // start the player asks for that is refused (Npcap up late) is owed like
    // any other -- clearing it there ended the 30 s retry for good.
    check(in_order(body(gui, "void GuiApp::toggle_hunt() {"),
                   { "if (hunting_) {", "hunt_owed_ = false;", "stop_hunt();", "return;", "start_hunt();",
                     "if (!hunting_ && hunt_config_.enabled) owe_start(hunt_owed_, \"Hunting\");" }) &&
              in_order(features, { "if (attacking != hunting_) {", "if (attacking) {", "start_hunt();",
                                   "if (!hunting_ && hunt_config_.enabled) owe_start(hunt_owed_, \"Hunting\");",
                                   "} else {", "hunt_owed_ = false;", "stop_hunt();" }) &&
              in_order(features, { "if (macro_on != macro_running_) {", "if (macro_on) {", "start_macro();",
                                   "owe_start(macro_owed_, \"The macro\");", "} else {", "macro_owed_ = false;",
                                   "stop_macro();" }) &&
              in_order(body(gui, "void GuiApp::toggle_macro() {"),
                       { "if (macro_running_) {", "macro_owed_ = false;", "stop_macro();", "return;", "start_macro();",
                         "owe_start(macro_owed_, \"The macro\");" }) &&
              in_order(body(gui, "void GuiApp::draw_run_bar() {"),
                       { "if (ImGui::Button(running ? \"STOP\" : \"START\", ImVec2(110, 32))) {", "if (running) {",
                         "macro_owed_ = false;", "stop_macro();", "start_macro();",
                         "owe_start(macro_owed_, \"The macro\");" }),
          "F11, the full screen's Attack and Timed keys switches, its F10, START MACRO and START/STOP: the player's "
          "own stop -- nothing owed is asked afterwards; a start they ask for that is refused is owed");
    // The 2026.11.24 review: Tự chat stopped by the player with the line in
    // doubt stops the hunter and the macro -- owed now, started again once the
    // line is seen shut, as the bin's doubt resumes what it stopped.
    check(in_order(gui, { "if (chat_handover_.exchange(false)) {",
                          "const bool was_hunting = hunting_, was_macro = macro_running_;", "stop_hunt();",
                          "stop_macro();", "stopping = stopping_;",
                          "if (was_hunting && hunt_config_.enabled) owe_start(hunt_owed_, \"Hunting\");",
                          "if (was_macro) owe_start(macro_owed_, \"The macro\");",
                          "GoBot tự bật lại khi thấy ô chat " }) &&
              has(body(gui, "void GuiApp::watch_chat_doubt() {"), "Bấm DỪNG LẠI rồi BẮT ĐẦU"),
          "the chat hand-over: what it stopped owed, started again once the line is seen shut");
    check(!has(body(gui, "void GuiApp::stop_hunt() {"), "_owed_") &&
              !has(body(gui, "void GuiApp::stop_macro() {"), "_owed_") &&
              !has(slice(gui, "bool GuiApp::stop_trash(bool wait) {", "void GuiApp::bin_seen_clear() {"), "_owed_"),
          "stop_hunt, stop_macro and the bin's doubt (stop_trash) keep what is owed: a start the doubt refused is "
          "asked once the doubt is over");

    // ---- start_hunt says a refusal once a reason; owed no longer once it runs ----
    const std::string hunt = body(gui, "void GuiApp::start_hunt() {");
    check(has(hunt, "const bool again = owed_asking_ && owed_why_ == why;") && has(hunt, "owed_why_ = why;") &&
              in_order(hunt, { "if (!hunt_named()) {", "if (say(\"no character name typed\")) {", "simple_note(",
                               "GB_WARN(", "return;", "if (const char* why = spot_refusal(spot_refused_vi_)) {",
                               "if (say(why)) {", "simple_note(\"Chưa bật Tự đánh quái: \" + spot_refused_vi_, 15000);",
                               "GB_WARN(", "return;", "if (!keys_may_start()) return;",
                               "focus_game_to_start(\"hunting\")", "hunter_->start();", "hunting_ = true;",
                               "hunt_owed_ = false;", "owed_why_.clear();" }),
          "start_hunt: a refusal said -- the card, the status line, the log -- unless an owed ask meets the reason it "
          "said last; the game brought forward only once every check passed; owed no longer once it runs, and the "
          "next refusal said whatever its reason");
    check(in_order(body(gui, "void GuiApp::start_macro() {"),
                   { "if (!keys_may_start()) return;", "focus_game_to_start(\"the macro\")", "macro_->start();",
                     "macro_running_ = true;", "macro_owed_ = false;" }),
          "start_macro: owed no longer once it runs; the game brought forward only after keys_may_start");

    // ---- where it is kept, and what the card says ----
    check(has(hpp, "#include \"gobot/owed_start.hpp\"") && has(hpp, "std::atomic<bool> hunt_owed_{false};") &&
              has(hpp, "std::atomic<bool> macro_owed_{false};") && has(hpp, "std::atomic<bool> jobs_owed_{false};") &&
              has(hpp, "std::uint64_t     owed_ms_ = 0;") && has(hpp, "std::string       owed_why_;") &&
              has(hpp, "bool              owed_asking_ = false;"),
          "gui_app.hpp: the three starts owed, the last ask, the last reason said and the ask under way");
    check(has(view, "return \" GoBot tự thử lại mỗi \" + std::to_string(kOwedAskMs / 1000) + \" giây.\";") &&
              has(view, "(\"Tự đánh quái chưa chạy: \" + spot_refused_vi_ + (hunt_owed_ ? owed_again_vi() : "
                        "std::string()))") &&
              has(view, "coloured(kWait, (\"Tự đánh quái chưa chạy được.\" + owed_again_vi()).c_str());") &&
              has(view, "if (running && jobs_owed_)"),
          "the card under BẮT ĐẦU: Tự đánh quái refused and the cold start's jobs, owed, say GoBot asks again every "
          "30 s -- by the number the code asks by");
#else
    skip("no source folder\n");
#endif
}

/// 2026.11.24, the owner: "Kiểm tra các phần khác xem có phần nào không làm
/// được thì dừng không, cần thay cơ chế này bằng 1 cơ chế khác" -- with his
/// correction: retry at once or after a short gap, never a long pause. Tự
/// chat, Làm nhiệm vụ nhanh and Tự đóng thông báo no longer stop themselves:
/// their own next turn tries again, and the phone is told once. Against the
/// code that runs them (chat_loop needs the live game, so its words are read)
/// and the notes that say it.
void test_self_stops_1124() {
    section("2026.11.24: Tự chat, Làm nhiệm vụ nhanh and Tự đóng thông báo never stop themselves");
    {
        ChatSettings s;
        s.interval_s = 120;
        ChatSettings fast = s;
        fast.interval_s = 10;
        check(chat_retry_ms(false, s) == 30'000 && chat_retry_ms(true, s) == 120'000 &&
                  chat_retry_ms(true, fast) == 30'000 && ChatSettings{}.max_failures == 3 &&
                  QuestSettings{}.max_failures == 3 && PopupCloser::kBackoffMs == 30'000 &&
                  QuestRun::kPostponeMs == 30'000,
              "the gaps: a chat line 30 s, or max(30 s, the line interval) when GoBot could not judge or close it; "
              "the quest's missing pictures 30 s; a popup still up 30 s; the phone told at the 3rd in a row");
    }
#ifdef GOBOT_SOURCE_DIR
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string chat_cpp = source("/src/auto_chat.cpp");
    const std::string quest = source("/src/quick_quest.cpp");
    const std::string closer = source("/src/popup_closer.cpp");
    const std::string closer_h = source("/include/gobot/popup_closer.hpp");
    const std::string popups = source("/apps/gobot_gui/simple_popups.cpp");
    const std::string doc = flat(source("/DOC-TRUOC.txt"));
    const std::string readme = flat(source("/README.md"));
    const std::string chat_ini = flat(source("/configs/chat.ini"), true);
    const std::string quest_ini = flat(source("/configs/quest.ini"), true);
    if (gui.empty() || chat_cpp.empty() || quest.empty() || closer.empty() || closer_h.empty() || popups.empty() ||
        doc.empty() || readme.empty() || chat_ini.empty() || quest_ini.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }

    // ---- Tự chat ----
    const std::string loop = body(gui, "void GuiApp::chat_loop() {");
    check(!loop.empty() && !has(loop, "bool stopped_self") && !has(loop, "stopped_self =") &&
              !has(loop, "tell(\"Tự chat đã dừng.\");") && !has(loop, "— Tự chat đã dừng.\"") &&
              has(loop, "if (chat_stop_ || chat_oneshot_) break;"),
          "chat_loop: nothing of its own ends a run any more -- no stopped_self, no \"Tự chat đã dừng.\"; only "
          "DỪNG, F12, unticking, or a one-off's end");
    const std::string asked = slice(loop, "if (request == 2) {", "if (doubt.active()) {");
    // The 2026.11.24 review: its r.stop puts the next timed try a line
    // interval after the doubt (gap_after_doubt), as a timed line's does --
    // the turn and the failures untouched.
    check(has(asked, "const ChatSender::Result r = attempt(line, true);") && has(asked, "note_doubt(r);") &&
              has(asked, "if (r.stop) gap_after_doubt = std::max(gap_after_doubt, chat_retry_ms(true, settings));") &&
              !has(asked, "schedule."),
          "Gửi thử meeting a line it could not judge or close: its doubt noted as ever, the next timed try a line "
          "interval after it ends, the timed run's turn untouched -- never the run's end");
    check(in_order(loop, { "} else if (r.aborted && !r.stop) {", "} else if (chat_quit_ || chat_stop_) {",
                           "schedule.postpone(end, 3000);", "} else {", "schedule.failed(end, retry_ms);",
                           "alert(\"chat_stopped\", told);" }),
          "chat_loop: a line cut by DỪNG, F12 or an unticking is put off, never counted as a failure nor told to "
          "the phone");
    check(in_order(loop, { "if (doubt.active()) {", "doubt.step(*closer, now_ms());", "if (doubt.active()) continue;",
                           "if (gap_after_doubt != 0) {",
                           "if (schedule.due_at() < now + gap_after_doubt) schedule.postpone(now, gap_after_doubt);",
                           "gap_after_doubt = 0;", "if (chat_stop_ || chat_oneshot_) break;" }) &&
              in_order(loop, { "} else if (r.sent) {", "schedule.sent(end);",
                               "if (r.stop) gap_after_doubt = chat_retry_ms(true, settings);",
                               "} else if (r.aborted && !r.stop) {",
                               "const std::uint64_t retry_ms = chat_retry_ms(r.stop, settings);",
                               "schedule.failed(end, retry_ms);", "if (r.stop) gap_after_doubt = retry_ms;",
                               "if (schedule.failures() == settings.max_failures) {",
                               "const std::string told = alert_text::chat_stopped(schedule.failures());",
                               "tell(told);", "alert(\"chat_stopped\", told);" }) &&
              !has(loop, "schedule.failed(end, 30'000);") && !has(loop, "schedule.failures() >= settings.max_failures"),
          "chat_loop: a line that failed tried again chat_retry_ms on -- after its doubt, a whole line interval, for "
          "one it could not judge or close (r.stop), sent or not -- the phone told once, at max_failures");
    check(has(chat_cpp, "const char* const kNotClosed  = \"Không đóng được ô chat sau khi gửi — GoBot thử lại ở lượt "
                        "sau.\";") &&
              has(chat_cpp, "\"GoBot thử lại ở lượt sau. Ảnh màn hình lúc đó ở thư mục chat_debug.\";") &&
              !has(chat_cpp, "— Tự chat đã dừng.\"") && !has(chat_cpp, "\"Tự chat đã dừng. Ảnh") &&
              !has(chat_cpp, "Tu chat stops") &&
              has(chat_ini, "Since 2026.11.24 no longer a stop: the line keeps its turn and is tried again 30 s on"),
          "the sender's words and the trace: GoBot tries again at the next turn, Tự chat stops no more; chat.ini's "
          "max_failures says so");

    // ---- Làm nhiệm vụ nhanh ----
    const std::string step = body(quest, "void QuestRun::step(std::uint64_t now) {");
    check(in_order(step, { "if (r.outcome == QuestOutcome::Cancelled) {", "if (r.outcome == QuestOutcome::NoPictures) {",
                           "if (running_ && settings_.enabled) schedule_.postpone(now, kPostponeMs);",
                           "if (!no_pictures_said_ && hooks_.say)", "no_pictures_said_ = true;", "return;",
                           "no_pictures_said_ = false;", "schedule_.done(now, r);",
                           "if (r.outcome == QuestOutcome::English) {", "stop_itself(",
                           "if (running_ && r.failed() && schedule_.failures() == settings_.max_failures) {",
                           "GoBot vẫn thử lại mỗi lượt. Xem ảnh trong quest_debug.",
                           "hooks_.alert(\"quest_trouble\", alert_text::quest_trouble(n, alert_why));" }) &&
              count_in(step, "stop_itself(") == 1 && !has(step, "stop_itself(\"Đã tự tắt") &&
              !has(step, "stop_itself(\"Thiếu hình"),
          "QuestRun::step: the pictures missing put off 30 s and said once; failed rounds told once at "
          "max_failures, the next round at the interval; only the English client stops it");
    check(has(quest_ini, "Hỏng chừng này lần liền thì báo, một lần cho mỗi đợt (1-20). Từ bản 2026.11.24 không tự tắt "
                         "nữa: GoBot vẫn thử lại mỗi lượt"),
          "quest.ini's max_failures says it no longer stops it");

    // ---- Tự đóng thông báo ----
    check(!has(closer_h, "LeftAlone,") && !has(closer_h, "static constexpr int kTries") &&
              !has(closer_h, "kTriesWindowMs = ") && has(closer_h, "static constexpr std::uint64_t kBackoffMs = 30'000;") &&
              !has(closer, "case Turn::LeftAlone") && !has(closer, "Turn::LeftAlone;") &&
              !has(closer, "\"; leaving it alone\"") && !has(closer, "tr.tries.erase(") &&
              has(closer, "\"); trying again in \" + std::to_string(kBackoffMs / 1000) + \" s\"") &&
              !has(popups, "LeftAlone"),
          "the popup closer: no cap of tries and no Turn::LeftAlone -- a popup still up is tried again kBackoffMs "
          "later, as often as it takes");

    // ---- the notes ----
    check(has(doc, "Cũng trong bản này, cùng lời bạn ở trên, Tự chat, Làm nhiệm vụ nhanh và Tự đóng thông báo không "
                   "còn tự dừng (chưa thử trên game thật):") &&
              has(doc, "\"" + alert_text::chat_stopped(3) + "\"") &&
              has(doc, "\"Không làm được 3 lần liền (lý do) — GoBot vẫn thử lại mỗi lượt. Xem ảnh trong quest_debug.\"") &&
              has(doc, "\"Thiếu hình nhận diện cửa sổ Nhiệm Vụ — GoBot thử lại sau 30 giây.\"") &&
              has(doc, "log ghi \"Popups: ... is still up after closing it with ESC (try N); trying again in 30 s\"") &&
              has(doc, "3 lần liền thì GoBot báo một lần và vẫn thử lại (từ bản 2026.11.24; trước đó Tự chat tự dừng).") &&
              has(readme, "### Tự chat, the quick quest and the popups: never stopped by themselves (2026.11.24)") &&
              has(readme, "*" + alert_text::chat_stopped(3) + "*") &&
              has(readme, "Three failed rounds in a row are told on the tab and the phone") &&
              !has(readme, "Three failed rounds in a row stop it") &&
              has(readme, "`Popups: the check-in board is still up after closing it with ESC (try 3); trying again "
                          "in 30 s`"),
          "DOC-TRUOC.txt and README.md: the three no longer stop themselves, in the words the code says");
#else
    skip("no source folder\n");
#endif
}

/// 2026.11.24, the same request -- "Kiểm tra các phần khác xem có phần nào
/// không làm được thì dừng không, cần thay cơ chế này bằng 1 cơ chế khác" --
/// for Tự hồi sinh and Tự vào lại game: the revive out of tries no longer
/// stops the bot, max_restarts counts over an hour and is waited out, a
/// crossing not in is made again 30 s on, and a relogin's window not found is
/// looked for again. The Reviver and CrashWatch's hour are tried on their own
/// in main.cpp; here, what only GoBot's window does, by its source, and the
/// notes.
void test_revive_keeper_1124() {
    section("2026.11.24: Tự hồi sinh and Tự vào lại game never stop the bot or themselves");
    check(Reviver::kRoundGapMs == kReactMs && CrashWatch::kCrossAgainMs == 30'000 &&
              CrashWatch::kRestartWindowMs == 3'600'000,
          "the gaps: another round of revive tries 30 s on, a crossing not in made again 30 s on, max_restarts "
          "counted over an hour");
#ifdef GOBOT_SOURCE_DIR
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string hpp = source("/apps/gobot_gui/gui_app.hpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    const std::string keeper = source("/apps/gobot_gui/simple_keeper.cpp");
    const std::string login = source("/src/auto_login.cpp");
    const std::string doc = flat(source("/DOC-TRUOC.txt"));
    const std::string readme = flat(source("/README.md"));
    const std::string app_ini = flat(source("/configs/app.ini"), true);
    const std::string alerts_ini = flat(source("/configs/alerts.ini"), true);
    if (gui.empty() || hpp.empty() || view.empty() || keeper.empty() || login.empty() || doc.empty() ||
        readme.empty() || app_ini.empty() || alerts_ini.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }

    // ---- Tự hồi sinh ----
    check(!has(hpp, "simple_stopped_vi_") && !has(view, "simple_stopped_vi_") && !has(gui, "simple_stopped_vi_"),
          "nothing stops the bot by itself any more: the red \"why the bot stopped itself\" line is gone");
    const std::string card = slice(view, "if (revive_running_) {", "if (trash_running_ && !trash_oneshot_) {");
    check(in_order(card, { "const std::uint64_t round_at = revive_round_ms_.load();", "if (round_at != 0) {",
                           "coloured(kWait, (\"Tự hồi sinh: 3 lần chưa được — có thể đã hết giấy/bùa hồi sinh. Thử "
                           "lại sau \" +",
                           "\" giây; hồi sinh bằng tay thì bot tự chạy tiếp.\")", "} else {",
                           "hint((\"Tự hồi sinh: đang theo dõi\" +" }) &&
              has(gui, "revive_round_ms_ = next_round_ms;") && has(gui, "revive_round_ms_ = 0;"),
          "the card: dead with a round run out, in amber with the seconds to the next round; else as ever");
    check(has(view, "\"Khi nhân vật chết, bấm Sử dụng vật phẩm hồi sinh rồi Xác Nhận. Thử \"") &&
              has(view, "\"3 lần; chưa được thì 30 giây sau thử lại (không dừng bot). Không hồi \"") &&
              has(view, "\"sinh tại làng, không dùng Cash.\").c_str());") && !has(view, "không được thì dừng bot."),
          "Tự hồi sinh's tooltip: \"Thử 3 lần; chưa được thì 30 giây sau thử lại (không dừng bot). Không hồi sinh tại "
          "làng, không dùng Cash.\"");

    // ---- Tự vào lại game ----
    check(has(gui, "ImGui::Text(\"Restarts   at most %d an hour\", s.max_restarts);") &&
              !has(gui, "Gives up   after %d restart(s)"),
          "the full screen: \"Restarts at most %d an hour\", no longer \"Gives up after\"");
    check(has(view, "tối đa trong 1 giờ (max_restarts trong configs/app.ini, mặc định 5) thì GoBot không đóng game") &&
              has(view, "\"sau GoBot thử lại, không dừng.\");") && !has(view, "thì GoBot dừng và chờ bạn.\"") &&
              has(keeper, "như vậy được tính vào số lần vào lại tối đa trong 1 giờ.") &&
              has(view, "if (keeper_waiting_) coloured(kWait, line.c_str());") && has(view, "hint(line.c_str());") &&
              has(view, "coloured(keeper_waiting_ ? kWait : kGood,"),
          "the Tự đăng nhập tab and the run card: max_restarts in an hour, waited out; a crossing not in tried again "
          "30 s on; the keeper's line in amber while it waits");
    check(has(login, "nhập VTCZone (chọn ghi nhớ nếu có) — GoBot tự thử lại sau đó.\";") &&
              has(login, "xem và trả lời hộp đó — GoBot tự thử lại sau đó.\";") &&
              !has(login, "rồi bấm BẮT ĐẦU lại."),
          "the sign-in form's and the Security Alert's words: GoBot tries again by itself -- no longer \"rồi bấm BẮT "
          "ĐẦU lại\" (that button says DỪNG LẠI with the run on)");
    check(has(hpp, "std::vector<std::uint64_t> keeper_restart_times_;") &&
              in_order(body(gui, "void GuiApp::start_keeper(bool oneshot) {"),
                       { "join_pumping(keeper_thread_);", "keeper_restarts_ = 0;", "keeper_restart_times_.clear();",
                         "keeper_wait_until_ms_ = 0;", "keeper_wait_told_ = false;", "keeper_retry_told_ = false;",
                         "keeper_again_ = false;", "keeper_waiting_ = false;",
                         "keeper_thread_ = std::thread([this] { keeper_loop(); });" }),
          "start_keeper: the hour's restarts, a wait and the crossings owed counted from none, as the run's number "
          "was -- once the last keeper's thread is joined");
    check(in_order(body(keeper, "GuiApp::KeeperWake GuiApp::keeper_wake("),
                   { "if (!keeper_oneshot_) return KeeperWake::None;", "keeper_login_now_ = false;",
                     "return KeeperWake::Quit;" }),
          "a one-shot keeper ending on a game not running normally drops the key's request asked again");

    // ---- the game's window after a log in, looked for again ----
    const std::string tick = body(gui, "void GuiApp::tick() {");
    check(has(hpp, "static constexpr std::uint64_t kReattachAgainMs = 5'000;") &&
              in_order(tick, { "owed_tick();",
                               "if (reattach_missed_ && keeper_running_ && !keeper_stop_ && now_ms() - "
                               "reattach_missed_ms_ >= kReattachAgainMs &&",
                               "!keeper_crossing())", "keeper_reattach_ = true;",
                               "if (keeper_reattach_.exchange(false)) {",
                               "const bool again = reattach_missed_;", "const bool found = reattach_game();",
                               "if (found) {", "where_pending_ms_ = now_ms();",
                               "if (hunter_ && hunter_->running() && (found || !again)) hunter_->suspend_spot();",
                               "if (found) simple_map_lose();",
                               "if (found && macro_ && macro_->running()) macro_->recast_buffs(\"logging back in\");",
                               "simple_start_jobs();" }) &&
              in_order(body(gui, "bool GuiApp::reattach_game() {"),
                       { "reattach_missed_ = true;", "reattach_missed_ms_ = now_ms();", "return false;",
                         "reattach_missed_ = false;" }),
          "tick(): a relogin's window not found is looked for again every kReattachAgainMs while the keeper runs, "
          "and the look that finds it does every follow-up of a log in -- the spot set aside once, at the log in, "
          "and on the look that finds it");

    // ---- the notes ----
    check(has(readme, "### Tự hồi sinh and Tự vào lại game: never stopped by themselves (2026.11.24)") &&
              has(readme, "*" + Reviver::out_of_tries_vi() + "*") &&
              has(readme, "*" + alert_text::gave_up_restarts(5, "hh:mm:ss") + "*") &&
              has(readme, "`back_in` says *Đã vào lại game (lần N trong 1 giờ qua). Bot chạy tiếp.*") &&
              has(readme, "*Vào lại game chưa được (VTCZone đang hiện màn hình đăng nhập) — GoBot thử lại sau 30 "
                          "giây.*") &&
              has(readme, "`Revive: out of tries (round 1) -- nothing stopped: …`") &&
              has(readme, "`Revive: round 2 at this death: another 3 tries -- still never the village, never Cash`") &&
              has(readme, "At most `max_restarts` restarts in any hour") &&
              !has(readme, "It stops for a person after `max_restarts` restarts"),
          "README.md: the section of 2026.11.24, in the words and the log lines the code says, and Tự đăng nhập's "
          "summary");
    check(has(doc, "Cũng trong bản này, cùng lời bạn ở trên, Tự hồi sinh và Tự vào lại game không còn dừng bot hay tự "
                   "dừng (chưa thử trên game thật):") &&
              has(doc, "\"" + Reviver::out_of_tries_vi() + "\"") &&
              has(doc, "\"Đã vào lại game 5 lần trong 1 giờ — GoBot chờ tới hh:mm:ss rồi tự vào lại.\"") &&
              has(doc, "\"Tạm nghỉ tự vào lại\"") &&
              has(doc, "\"Đã vào lại game (lần N trong 1 giờ qua). Bot chạy tiếp.\"") &&
              has(doc, "\"Vào lại game chưa được (lý do) — GoBot thử lại sau 30 giây.\"") &&
              has(doc, "\"Tự hồi sinh: 3 lần chưa được — có thể đã hết giấy/bùa hồi sinh. Thử lại sau N giây; hồi sinh "
                       "bằng tay thì bot tự chạy tiếp.\""),
          "DOC-TRUOC.txt: the 2026.11.24 paragraph, in the words the screen and the phone say");
    check(has(app_ini, "At most this many restarts in any hour. One more waits -- nothing ended or clicked -- until the "
                       "oldest of them is an hour old") &&
              has(app_ini, "Counted in max_restarts' hour like a crash.") &&
              has(alerts_ini, "Tạm nghỉ tự vào lại: đã vào lại game max_restarts lần trong 1 giờ") &&
              has(alerts_ini, "Từ bản 2026.11.24 bot không dừng nữa: 30 giây sau GoBot thử thêm 3 lần"),
          "configs/app.ini's max_restarts and configs/alerts.ini's two ticks say it (the repo's templates)");
#else
    skip("no source folder\n");
#endif
}

/// 2026.11.24: Tổ đội, Tự hồi sinh, Dọn rác and Tự chat no longer end at their
/// start for missing pictures, nor Tổ đội for a party.ini that does not read
/// (the owner: "Kiểm tra các phần khác xem có phần nào không làm được thì dừng
/// không, cần thay cơ chế này bằng 1 cơ chế khác"). They wait, pressing
/// nothing, and look again -- every 30 s for the pictures, every 2 s for the
/// file -- their stop flags ending the wait at once. By their source: the
/// waits need the live program's threads.
void test_worker_waits_1124() {
    section("2026.11.24: workers short of pictures or of party.ini wait and look again, never end at their start");
#ifdef GOBOT_SOURCE_DIR
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string hpp = source("/apps/gobot_gui/gui_app.hpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    if (gui.empty() || hpp.empty() || view.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    // ---- the shared wait ----
    const std::string wait = body(gui, "bool wait_for_pictures(");
    check(has(gui, "constexpr int kPicturesAgainMs = 30'000;") &&
              in_order(wait, { "for (;;) {", "if (stopped()) return false;",
                               "const std::string what = pictures ? missing(*pictures) : std::string(\"hình giao diện\");",
                               "if (what.empty()) {", "return true;", "if (what != said) {", "say_waiting(what);",
                               "said = what;", "std::chrono::milliseconds(kPicturesAgainMs);",
                               "if (stopped()) return false;", "std::this_thread::sleep_for(std::chrono::milliseconds(50));",
                               "if (stopped()) return false;", "pictures = TemplateStore::shared_best(name_map_ini);" }) &&
              !has(wait, "key") && !has(wait, "click"),
          "wait_for_pictures: what is missing said once per thing, the stop asked every 50 ms, the program's one set "
          "asked for again every 30 s, true as soon as nothing is missing -- nothing pressed");
    check(has(gui, "return \"Thiếu hình giao diện của game (\" + what + \") — GoBot thử lại sau \" +") &&
              has(gui, "std::to_string(kPicturesAgainMs / 1000) + \" giây.\";"),
          "its line: \"Thiếu hình giao diện của game (<cái gì>) — GoBot thử lại sau 30 giây.\"");

    // ---- Tổ đội ----
    const std::string party = body(gui, "void GuiApp::party_loop() {");
    const std::string party_start = slice(party, "FileWatch rules_watch(2000);", "while (!party_stop_) {");
    check(!party_start.empty() &&
              in_order(party_start,
                       { "bool read = rules.load(config);", "if (!read) {",
                         "say_party(\"Chưa đọc được configs/party.ini — Tổ đội chờ, chưa làm gì; đọc lại mỗi 2 giây.\");",
                         "while (!read) {", "slept < 2000 && !party_stop_", "if (party_stop_) {",
                         "party_running_ = false;", "return;", "rules_watch.forget();", "rules_watch.due(config);",
                         "read = rules.load(config);", "IniFile ini;", "ini.load(config);" }) &&
              !has(party_start, "rồi bấm BẮT ĐẦU lại") && !has(party_start, "not started"),
          "party_loop: party.ini that does not read is read again every 2 s, nothing acted on meanwhile, never the "
          "code's defaults; only the stop ends the wait");
    check(in_order(party_start, { "std::shared_ptr<const TemplateStore> pictures =", "if (!wait_for_pictures(",
                                  "[this] { return party_stop_.load(); },", "party_running_ = false;", "return;",
                                  "const TemplateStore& store = *pictures;" }) &&
              !has(party_start, "No UI pictures") && !has(party_start, "if (store.size() == 0)"),
          "party_loop: no return on no pictures -- the 30 s wait, its stop ending it");

    // ---- Tự hồi sinh ----
    const std::string revive = body(gui, "void GuiApp::revive_loop() {");
    const std::string revive_start = slice(revive, "name_this_thread(\"revive\");", "reviver.set_stop(");
    check(in_order(revive_start,
                   { "std::shared_ptr<const TemplateStore> pictures =", "const bool have_pictures = wait_for_pictures(",
                     "if (s.size() == 0) return std::string(\"hình giao diện\");",
                     "return Reviver(input_, s, grab).ready(&why) ? std::string() : why;",
                     "[this] { return revive_stop_.load(); },", "revive_wait_vi_ = line;", "tell(line);",
                     "revive_wait_vi_.clear();", "if (!have_pictures) {", "revive_running_ = false;",
                     "const TemplateStore& store = *pictures;", "Reviver reviver(input_, store, grab);" }) &&
              !has(revive_start, "death panel cannot be recognised") && !has(revive_start, "if (store.size() == 0)"),
          "revive_loop: no pictures, or not the reviver's (ready() false), wait and are looked for again every 30 s; "
          "the stop ends the wait");
    check(has(hpp, "std::string              revive_wait_vi_;") &&
              in_order(slice(view, "if (revive_running_) {", "if (trash_running_ && !trash_oneshot_) {"),
                       { "if (!revive_wait.empty()) {", "coloured(kBad, (\"Tự hồi sinh chưa chạy: \" + revive_wait).c_str());",
                         "} else if (round_at != 0) {" }) &&
              has(view, "revive_wait = revive_wait_vi_;"),
          "the card: the revive's wait in red, as a death would not be seen");
    // The 2026.11.24 review: the HP figure's death alert stays on while the
    // reviver waits (it watches no death panel then).
    check(has(hpp, "std::atomic<bool>        revive_waiting_{false};") &&
              in_order(revive_start, { "revive_waiting_ = true;", "const bool have_pictures = wait_for_pictures(",
                                       "revive_waiting_ = false;", "if (!have_pictures) {" }) &&
              has(source("/apps/gobot_gui/simple_alerts.cpp"),
                  "const bool by_hp = running && vitals_running_ && (!revive_running_ || revive_waiting_.load());"),
          "revive_loop: waiting for its pictures, the death alert by the HP figure stays on");
    check(in_order(revive, { "if (turn == Reviver::Turn::OutOfTries) {", "if (!holding && !under_other && take_hold(\"reviving\")) {",
                             "holding = true;", "input_.release_all();" }),
          "revive_loop: out of tries with the hold let go by a flicker -- taken again, no key at the death panel");

    // ---- Dọn rác ----
    const std::string trash = body(gui, "void GuiApp::trash_loop() {");
    const std::string trash_start = slice(trash, "name_this_thread(\"trash\");", "CountReader counts;");
    check(in_order(trash_start, { "std::shared_ptr<const TemplateStore> pictures =",
                                  "if (pictures->size() == 0 && trash_oneshot_) {", "finish(", "return;",
                                  "if (!wait_for_pictures(", "if (trash_request_.exchange(0) != 0) tell(pictures_again_vi(trash_missing));", "return trash_stop_.load();",
                                  "trash_running_ = false;", "return;", "const TemplateStore& store = *pictures;" }),
          "trash_loop: a run waits for the pictures, 30 s a look, its stop ending it; a one-off button still says so "
          "at once");

    // ---- Tự chat ----
    const std::string chat = body(gui, "void GuiApp::chat_loop() {");
    const std::string chat_start = slice(chat, "name_this_thread(\"chat\");", "ChatDialogs dialogs;");
    check(in_order(chat_start,
                   { "std::shared_ptr<const TemplateStore> pictures =", "const auto missing = [&](const TemplateStore& s)",
                     "if (s.size() == 0) return \"hình giao diện\";",
                     "if (!can_look_for_chat(s, settings)) return \"hình ô chat \" + settings.open_picture;",
                     "if (!ChatDialogs::can_check(s)) return \"hình hộp thoại\";", "if (chat_oneshot_) {", "finish(",
                     "for (;;) {", "const bool have = wait_for_pictures(", "if (fresh.load(config)) settings = fresh;",
                     "if (chat_request_.exchange(0) != 0) tell(pictures_again_vi(chat_missing));", "return chat_stop_.load() || chat_quit_.load();", "if (have) break;",
                     "if (!stays()) return;", "chat_restart_ = false;", "const TemplateStore& store = *pictures;" }) &&
              !has(chat_start, "finish(\"Không có hình ô chat") && !has(chat_start, "finish(\"Không có hình hộp thoại") &&
              !has(chat_start, "if (store.size() == 0)"),
          "chat_loop: no pictures, no chat bar's, no dialogs': a run waits, 30 s a look, chat.ini read again too; "
          "unticking ends the wait, a run asked again keeps it; a one-off still says so at once");
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
    test_notes_1112();
    test_notes_1115d();
    test_notes_1116();
    test_notes_1116b();
    test_notes_1117();
    test_notes_1117b();
    test_notes_1117c();
    test_owed_start();
    test_self_stops_1124();
    test_revive_keeper_1124();
    test_worker_waits_1124();
}
