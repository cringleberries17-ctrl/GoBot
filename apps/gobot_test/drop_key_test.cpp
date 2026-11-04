// gobot_test -- the drop's key, Ctrl + ↓ (InputManager::drop_ctrl_down; the
// drop plan's §5): the one way ↓ ever goes down, always inside Ctrl, and
// inject() refusing it any other way; the exact events and their order; a
// drop cut short before ↓, and the ups it still owes after one is cut;
// release_all -- F12, DỪNG, HUNT OFF, the game losing focus -- letting go of
// Ctrl; and the coordinator's token taken only from nobody.
//
// Nothing here reaches the desktop. Every array goes to a pretend Windows
// (InputManager::PretendSender) that writes it down and never calls
// SendInput, listened to only while the "no real input" switch is on; the
// window aimed at is only a number. The guard's own checks -- ↓ refused
// before the switch is asked, the drop refused by it -- are in main.cpp's
// test_no_real_input, the one test whose refusals are expected.

#include "gobot/common.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/macro.hpp"
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
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

using Ev = InputManager::SentEvent;
using Step = InputManager::DropStep;
using Sent = InputManager::DropSent;

constexpr std::uint16_t kCtrl = 0x11;
constexpr std::uint16_t kCtrlSc = 0x1D;
constexpr std::uint16_t kDown = 0x28;
constexpr std::uint16_t kDownSc = 0x50;
constexpr std::uint32_t kExt = KEYEVENTF_EXTENDEDKEY;
constexpr std::uint32_t kUp = KEYEVENTF_KEYUP;
constexpr std::uint32_t kSc = KEYEVENTF_SCANCODE;

Ev key(std::uint16_t vk, std::uint16_t scan, std::uint32_t flags) {
    Ev e;
    e.vk = vk;
    e.scan = scan;
    e.flags = flags;
    return e;
}

Ev mouse(std::uint32_t flags) {
    Ev e;
    e.mouse = true;
    e.flags = flags;
    return e;
}

const Ev kCtrlDown = key(kCtrl, kCtrlSc, 0);
const Ev kCtrlUp = key(kCtrl, kCtrlSc, kUp);
const Ev kDownDown = key(kDown, kDownSc, kExt);
const Ev kDownUp = key(kDown, kDownSc, kExt | kUp);
const Ev kADown = key('A', 0x1E, 0);
const Ev kAUp = key('A', 0x1E, kUp);

/// A drop step's events, as the guards read them.
std::vector<Ev> events_of(Step s) {
    std::vector<Ev> out;
    for (const InputManager::KeyEvent& k : InputManager::drop_batch(s)) out.push_back(key(k.vk, k.scan, k.flags));
    return out;
}

bool same(const Ev& a, const Ev& b) {
    return a.mouse == b.mouse && a.vk == b.vk && a.scan == b.scan && a.flags == b.flags;
}

bool same(const std::vector<Ev>& a, const std::vector<Ev>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!same(a[i], b[i])) return false;
    return true;
}

bool refused(std::initializer_list<Ev> events, bool chord) {
    const std::vector<Ev> v(events);
    return InputManager::bare_down_in(v.data(), v.size(), chord);
}

/// An array as a few letters, for the checks' words: C Ctrl, D ↓, A the A
/// key, M a mouse event; + down, - up. "[C+ D+]" is the drop's ↓.
std::string text_of(const std::vector<Ev>& events) {
    std::string s = "[";
    for (std::size_t i = 0; i < events.size(); ++i) {
        const Ev& e = events[i];
        if (i > 0) s += ' ';
        if (e.mouse) {
            s += (e.flags & (MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_RIGHTDOWN)) != 0 ? "M+" : "M";
            continue;
        }
        const unsigned vk = e.vk & 0xFFu;
        s += vk == static_cast<unsigned>(kDown)   ? "D"
             : vk == static_cast<unsigned>(kCtrl) ? "C"
             : vk == 'A'                          ? "A"
                                                  : "?";
        s += (e.flags & kUp) != 0 ? "-" : "+";
    }
    return s + "]";
}

/// A pretend Windows for one check: every array that reaches it, written
/// down with the thread and its priority, the time on now_ms()'s clock and
/// how many events it took; as many taken as `takes` says (all, unless it
/// says); the game in front, and what Windows reads down, as the check sets.
struct Pretend {
    struct Got {
        std::vector<Ev> events;
        bool            chord = false;
        std::uint64_t   at_ms = 0;
        DWORD           thread = 0;
        int             priority = 0;
        unsigned        took = 0;
    };
    std::mutex       mutex;
    std::vector<Got> got;
    /// How many events of the n-th array (from 0) are taken.
    std::function<unsigned(std::size_t n, const std::vector<Ev>&)> takes;
    /// Asked first, for each array: may throw, as Windows never does.
    std::function<void(const std::vector<Ev>&)> on_send;
    std::atomic<bool> front{ true };
    std::atomic<bool> down_held{ false };
    std::atomic<bool> ctrl_held{ false };
    InputManager::PretendSender sender;

    Pretend() {
        sender.send = [this](const std::vector<Ev>& events, bool chord) -> unsigned {
            if (on_send) on_send(events);
            std::lock_guard<std::mutex> lock(mutex);
            Got g;
            g.events = events;
            g.chord = chord;
            g.at_ms = now_ms();
            g.thread = GetCurrentThreadId();
            g.priority = GetThreadPriority(GetCurrentThread());
            const auto all = static_cast<unsigned>(events.size());
            g.took = takes ? std::min(all, takes(got.size(), events)) : all;
            got.push_back(g);
            return g.took;
        };
        sender.focused = [this] { return front.load(); };
        sender.key_held = [this](int vk) {
            return vk == VK_DOWN ? down_held.load() : vk == VK_CONTROL ? ctrl_held.load() : false;
        };
        InputManager::set_pretend_sender(&sender);
    }
    ~Pretend() { InputManager::set_pretend_sender(nullptr); }
    Pretend(const Pretend&) = delete;
    Pretend& operator=(const Pretend&) = delete;

    std::vector<Got> log() {
        std::lock_guard<std::mutex> lock(mutex);
        return got;
    }
    /// Whether the arrays went out as these steps, in this order, and nothing else.
    bool went_as(std::initializer_list<Step> steps) {
        const std::vector<Got> g = log();
        if (g.size() != steps.size()) return false;
        std::size_t i = 0;
        for (const Step s : steps)
            if (!same(g[i++].events, events_of(s))) return false;
        return true;
    }
    std::string said() {
        std::string s;
        for (const Got& g : log()) {
            s += text_of(g.events);
            if (g.took < g.events.size()) s += "(took " + std::to_string(g.took) + ")";
            s += " ";
        }
        return s.empty() ? "nothing" : s;
    }
    /// Whether ↓'s key code is anywhere at all, down or up.
    bool any_down_key() {
        for (const Got& g : log())
            for (const Ev& e : g.events)
                if (!e.mouse && (e.vk & 0xFFu) == static_cast<unsigned>(kDown)) return true;
        return false;
    }
};

/// A window that is only a number: nothing is ever sent to it.
void* const kWindow = reinterpret_cast<void*>(std::uintptr_t{ 0x3000 });

/// Waits for `cond`, 2 s at most.
bool wait_for(const std::function<bool()>& cond) {
    for (int i = 0; i < 2000 && !cond(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return cond();
}

bool is_down_array(const std::vector<Ev>& events) {
    return events.size() == 2 && same(events[0], kCtrlDown) && same(events[1], kDownDown);
}

// ------------------------------------------------------------ the events

void test_drop_events() {
    section("The drop's keys: Ctrl + ↓, its fixed events and their order");

    check(same(events_of(Step::Lead), { kCtrlDown }) && same(events_of(Step::Down), { kCtrlDown, kDownDown }) &&
              same(events_of(Step::DownUp), { kDownUp }) && same(events_of(Step::CtrlUp), { kCtrlUp }) &&
              same(events_of(Step::BothUp), { kDownUp, kCtrlUp }),
          "the events: Ctrl {0x11, 0x1D, 0}; ↓ {0x28, 0x50, extended}; ↓-up {0x28, 0x50, extended|up}; Ctrl-up "
          "{0x11, 0x1D, up}; BothUp is [↓-up, Ctrl-up]");

    // Every way a drop goes, event by event: ↓ is never down without Ctrl.
    const auto walk = [](std::initializer_list<Step> steps, bool& in_ctrl, bool& ends_up) {
        bool ctrl = false, down = false;
        in_ctrl = true;
        for (const Step s : steps) {
            const std::vector<Ev> batch = events_of(s);
            bool ctrl_in_batch = false;
            for (const Ev& e : batch) {
                const bool up = (e.flags & kUp) != 0;
                if (e.vk == kCtrl) {
                    ctrl = !up;
                    if (!up) ctrl_in_batch = true;
                }
                if (e.vk == kDown) {
                    down = !up;
                    if (!up && !ctrl_in_batch) in_ctrl = false;     // ↓↓ with no Ctrl↓ before it in its array
                }
                if (down && !ctrl) in_ctrl = false;
            }
        }
        ends_up = !ctrl && !down;
    };
    bool in_ctrl = false, ends_up = false;
    walk({ Step::Lead, Step::Down, Step::DownUp, Step::CtrlUp }, in_ctrl, ends_up);
    bool in_ctrl2 = false, ends_up2 = false;
    walk({ Step::Lead, Step::Down, Step::BothUp }, in_ctrl2, ends_up2);
    bool in_ctrl3 = false, ends_up3 = false;
    walk({ Step::Lead, Step::CtrlUp }, in_ctrl3, ends_up3);
    check(in_ctrl && ends_up && in_ctrl2 && ends_up2 && in_ctrl3 && ends_up3,
          "a press, a press ended by [↓↑, Ctrl↑] and one cut before ↓: after every event ↓ down means Ctrl "
          "down, every ↓↓ has a Ctrl↓ before it in its own array, and both are up at the end");
    const std::vector<Ev> both = events_of(Step::BothUp);
    check(both.size() == 2 && both[0].vk == kDown && both[1].vk == kCtrl,
          "both up in one array: ↓'s up first, so Ctrl is still down as ↓ comes up");
    bool abort_has_down = false;
    for (const Step s : { Step::Lead, Step::CtrlUp })
        for (const Ev& e : events_of(s)) abort_has_down = abort_has_down || e.vk == kDown;
    check(!abort_has_down, "cut before ↓ ([Ctrl↓], [Ctrl↑]): no 0x28 anywhere");

    // Each step's array meets inject()'s guard: only the ↓ inside Ctrl needs
    // the drop's chord, and outside it that array is refused.
    bool steps_pass = true;
    for (const Step s : { Step::Lead, Step::Down, Step::DownUp, Step::CtrlUp, Step::BothUp }) {
        const std::vector<Ev> v = events_of(s);
        steps_pass = steps_pass && !InputManager::bare_down_in(v.data(), v.size(), true);
    }
    const std::vector<Ev> down = events_of(Step::Down);
    check(steps_pass && InputManager::bare_down_in(down.data(), down.size(), false),
          "every step passes the guard as the drop sends it; [Ctrl↓, ↓↓] sent as anything else is refused");

    check(InputManager::kDropLeadMinMs == 40 && InputManager::kDropLeadMaxMs == 60 &&
              InputManager::kDropHoldMinMs == 70 && InputManager::kDropHoldMaxMs == 110 &&
              InputManager::kDropTailMinMs == 40 && InputManager::kDropTailMaxMs == 60 &&
              InputManager::kDropCutMs == 60 && InputManager::kOwedTries == 3,
          "Ctrl alone 40-60 ms, ↓ held 70-110, Ctrl 40-60 after ↓'s up; under 60 ms held is Cut; 3 tries");
    check(InputManager::kDropLeadMaxMs + InputManager::kDropHoldMaxMs + InputManager::kDropTailMaxMs <= 250,
          "at most 230 ms from Ctrl's down to its up: under the 300 ms the trash and charm holds wait for keys");
}

void test_bare_down_table() {
    section("inject() refuses ↓ down outside the drop's Ctrl (InputManager::bare_down_in)");

    check(refused({ kDownDown }, false) && refused({ kDownDown }, true),
          "[↓↓] alone: refused, chord or not");
    check(refused({ kDownDown, kCtrlDown }, true), "[↓↓, Ctrl↓]: Ctrl after ↓ is no use, refused");
    check(refused({ kCtrlDown, kDownDown }, false), "[Ctrl↓, ↓↓] not sent as the drop's chord: refused");
    check(refused({ kCtrlUp, kDownDown }, true) && refused({ kADown, kDownDown }, true) &&
              refused({ mouse(MOUSEEVENTF_LEFTDOWN), kDownDown }, true),
          "a Ctrl-up, another key or a click before it is not a Ctrl-down: refused");
    check(refused({ key(0, kDownSc, kSc) }, false) && refused({ key(0, kDownSc, kSc | kExt) }, false) &&
              refused({ key('A', kDownSc, kSc) }, false),
          "↓ sent as its scan code, 0x50, extended or not, whatever key code rides with it: refused");
    check(refused({ key(0x0128, 0, 0) }, false) && refused({ key(0xFF28, 0x50, kExt) }, false),
          "a key code whose low byte is 0x28: refused, as never_pressed reads it");
    check(refused({ key(kCtrl, 0x2A, kSc), kDownDown }, true),
          "a 'Ctrl' sent by a scan code that is not Ctrl's is not Ctrl: refused");

    check(!refused({ kCtrlDown, kDownDown }, true) && !refused({ key(0xA2, kCtrlSc, 0), kDownDown }, true) &&
              !refused({ key(0, kCtrlSc, kSc), key(0, kDownSc, kSc | kExt) }, true),
          "the drop's own [Ctrl↓, ↓↓]: let through -- left Ctrl too, and both as scan codes");
    check(!refused({ kDownUp }, false) && !refused({ kDownUp, kCtrlUp }, false) && !refused({ kCtrlDown }, false) &&
              !refused({ kADown, kAUp }, false) && !refused({ key(0, 0x50, KEYEVENTF_UNICODE) }, false) &&
              !refused({ mouse(MOUSEEVENTF_LEFTDOWN) }, false),
          "↓'s up, Ctrl alone, other keys, a typed 'P' (Unicode 0x50 is a character, not a key) and a click: let "
          "through");
    check(!InputManager::bare_down_in(nullptr, 0, false) && !refused({}, false), "nothing at all: nothing refused");

    // In inject() itself, before the pretend Windows sees anything.
    Pretend p;
    const unsigned bare = InputManager::pretend_inject({ kDownDown });
    const unsigned with_ctrl = InputManager::pretend_inject({ kCtrlDown, kDownDown });
    const unsigned by_scan = InputManager::pretend_inject({ key(0, kDownSc, kSc | kExt) });
    const unsigned up = InputManager::pretend_inject({ kDownUp });
    const unsigned other = InputManager::pretend_inject({ kADown, kAUp });
    const std::vector<Pretend::Got> got = p.log();
    check(bare == 0 && with_ctrl == 0 && by_scan == 0 && up == 1 && other == 2 && got.size() == 2 &&
              same(got[0].events, { kDownUp }) && same(got[1].events, { kADown, kAUp }) && !got[0].chord &&
              !got[1].chord,
          "through inject(): [↓↓], [Ctrl↓, ↓↓] sent as any key is and ↓'s scan code never reach Windows; ↓'s up "
          "and other keys do -- " + p.said());
}

// ------------------------------------------------------------ the press

/// One drop, on a manager aimed at a window that is only a number.
/// Made after its Pretend, so it goes first: whatever it still lets go of
/// reaches the pretend Windows.
struct DropRun {
    InputManager             im;
    InputManager::DropPress  r;
    explicit DropRun(Pretend&) { im.set_target_window(kWindow); }
    void go(const std::function<bool()>& go_on) { r = im.drop_ctrl_down(go_on); }
};

void test_drop_press() {
    section("Ctrl + ↓ pressed: Ctrl, then [Ctrl↓, ↓↓] in one array, ↓ up, then Ctrl up");

    const auto yes = [] { return true; };

    // ---- a press ----
    {
        Pretend p;
        DropRun d(p);
        const int priority_before = GetThreadPriority(GetCurrentThread());
        d.go(yes);
        const std::vector<Pretend::Got> g = p.log();
        const bool four = p.went_as({ Step::Lead, Step::Down, Step::DownUp, Step::CtrlUp });
        bool chord_only_down = four;
        bool ours = four;
        for (std::size_t i = 0; four && i < g.size(); ++i) {
            chord_only_down = chord_only_down && g[i].chord == (i == 1);
            ours = ours && g[i].thread == GetCurrentThreadId() && g[i].priority == THREAD_PRIORITY_TIME_CRITICAL;
        }
        check(four && chord_only_down && d.r.sent == Sent::Sent && !d.im.keys_owed(),
              "Sent: [Ctrl↓], [Ctrl↓, ↓↓], [↓↑], [Ctrl↑] -- only the ↓ inside Ctrl as the drop's chord, nothing "
              "owed after -- " + p.said());
        if (four) {
            const std::uint64_t lead = g[1].at_ms - g[0].at_ms, held = g[2].at_ms - g[1].at_ms,
                                tail = g[3].at_ms - g[2].at_ms, all = g[3].at_ms - g[0].at_ms;
            check(lead >= 40 && held >= 70 && tail >= 40 && d.r.held_ms >= 70 && d.r.down_ms >= g[1].at_ms &&
                      d.r.down_ms <= g[2].at_ms && all < 1000,
                  "Ctrl alone " + std::to_string(lead) + " ms (40-60), ↓ held " + std::to_string(held) +
                      " ms (70-110; reported " + std::to_string(d.r.held_ms) + "), Ctrl " + std::to_string(tail) +
                      " ms after ↓'s up (40-60): " + std::to_string(all) + " ms from Ctrl's down to its up; down_ms "
                      "the moment ↓ went");
        } else {
            check(false, "the press's timings: no press to time");
        }
        check(ours && GetThreadPriority(GetCurrentThread()) == priority_before,
              "every array from the caller's thread at TIME_CRITICAL, and the thread put back as it was after");
    }

    // ---- go_on says no at the last look ----
    {
        Pretend p;
        DropRun d(p);
        int asked = 0;
        d.go([&] { return ++asked == 1; });
        check(p.went_as({ Step::Lead, Step::CtrlUp }) && !p.any_down_key() && d.r.sent == Sent::Aborted &&
                  d.r.down_ms == 0 && !d.im.keys_owed(),
              "go_on false at the look before ↓: [Ctrl↓], [Ctrl↑], no 0x28 anywhere, Aborted -- " + p.said());
    }
    // ---- the game behind at the last look ----
    {
        Pretend p;
        DropRun d(p);
        p.on_send = [&p](const std::vector<Ev>& events) {
            if (same(events, { kCtrlDown })) p.front = false;
        };
        d.go(yes);
        check(p.went_as({ Step::Lead, Step::CtrlUp }) && !p.any_down_key() && d.r.sent == Sent::Aborted,
              "the game no longer in front at the look before ↓: Ctrl let go of, no ↓ -- " + p.said());
    }
    // ---- go_on throws ----
    {
        Pretend p;
        DropRun d(p);
        d.go([]() -> bool { throw std::runtime_error("a test"); });
        const bool at_start = p.log().empty() && d.r.sent == Sent::NotSent;
        Pretend q;
        DropRun e(q);
        int asked = 0;
        e.go([&]() -> bool {
            if (++asked > 1) throw std::runtime_error("a test");
            return true;
        });
        const bool at_look = q.went_as({ Step::Lead, Step::CtrlUp }) && e.r.sent == Sent::Aborted;
        Pretend r;
        DropRun f(r);
        int asked_r = 0;
        f.go([&]() -> bool {
            if (++asked_r > 2) throw std::runtime_error("a test");
            return true;
        });
        const std::vector<Pretend::Got> g = r.log();
        const bool in_hold = r.went_as({ Step::Lead, Step::Down, Step::DownUp, Step::CtrlUp }) &&
                             f.r.sent == Sent::Cut && f.r.held_ms < InputManager::kDropCutMs && !f.im.keys_owed() &&
                             g[3].at_ms - g[2].at_ms < static_cast<std::uint64_t>(InputManager::kDropTailMinMs);
        check(at_start && at_look && in_hold,
              "go_on throwing is a no: nothing at the start; Ctrl let go of at the look before ↓; in the hold, ↓ "
              "up at once, then Ctrl with no wait -- Cut -- " + r.said());
    }
    // ---- the hold cut at its first look ----
    {
        Pretend p;
        DropRun d(p);
        int asked = 0;
        d.go([&] { return ++asked <= 2; });
        check(p.went_as({ Step::Lead, Step::Down, Step::DownUp, Step::CtrlUp }) && d.r.sent == Sent::Cut &&
                  d.r.down_ms != 0 && d.r.held_ms < InputManager::kDropCutMs,
              "go_on false from the hold's first look: ↓ up at once, then Ctrl: Cut, held " +
                  std::to_string(d.r.held_ms) + " ms -- " + p.said());
    }
    // ---- nothing down at all ----
    {
        Pretend p;
        InputManager none;      // no window
        const Sent no_window = none.drop_ctrl_down(yes).sent;
        DropRun blocked(p);
        blocked.im.set_block_movement(true);
        blocked.go(yes);
        DropRun behind(p);
        p.front = false;
        behind.go(yes);
        p.front = true;
        DropRun refused_go(p);
        refused_go.go([] { return false; });
        DropRun empty(p);
        empty.go({});
        check(no_window == Sent::NotSent && blocked.r.sent == Sent::NotSent && behind.r.sent == Sent::NotSent &&
                  refused_go.r.sent == Sent::NotSent && empty.r.sent == Sent::NotSent && p.log().empty(),
              "NotSent, nothing sent: no window, movement blocked, the game behind, go_on false, no go_on");
    }
}

void test_drop_cut_and_owed() {
    section("Ctrl + ↓ cut short, the ups still owed, and nothing else down meanwhile");

    const auto yes = [] { return true; };

    // ---- release_all at the look before ↓, as F12 or DỪNG from inside it ----
    {
        Pretend p;
        DropRun d(p);
        int asked = 0;
        d.go([&] {
            if (++asked == 2) d.im.release_all();
            return true;
        });
        check(p.went_as({ Step::Lead, Step::CtrlUp }) && !p.any_down_key() && d.r.sent == Sent::Aborted &&
                  !d.im.keys_owed(),
              "release_all at the look before ↓: its [Ctrl↑], then nothing -- no ↓↓ after the release -- " +
                  p.said());
    }
    // ---- release_all from another thread in the hold: F12, HUNT OFF ----
    {
        Pretend p;
        DropRun d(p);
        std::atomic<bool> down{ false }, released{ false }, running{ true };
        p.on_send = [&down](const std::vector<Ev>& events) {
            if (is_down_array(events)) down = true;
        };
        DWORD stopper = 0;
        std::thread stop([&] {
            stopper = GetCurrentThreadId();
            wait_for([&] { return down.load(); });
            running = false;            // as Hunter::stop does before its release_all
            d.im.release_all();
            released = true;
        });
        d.go([&] {
            if (down.load()) wait_for([&] { return released.load(); });
            return running.load();
        });
        stop.join();
        const std::vector<Pretend::Got> g = p.log();
        check(p.went_as({ Step::Lead, Step::Down, Step::BothUp }) && g.size() == 3 && g[2].thread == stopper &&
                  (d.r.sent == Sent::Cut || d.r.sent == Sent::Sent) && d.r.down_ms != 0 && !d.im.keys_owed(),
              "release_all from another thread while ↓ is held: [↓↑, Ctrl↑] from it, and the drop sends nothing "
              "after -- no ↓↓ after the release, no Ctrl↑ of its own -- " + p.said());
    }
    // ---- [Ctrl↓, ↓↓] not taken whole ----
    {
        Pretend p;
        DropRun d(p);
        p.takes = [](std::size_t n, const std::vector<Ev>&) { return n == 1 ? 1u : 2u; };
        d.go(yes);
        Pretend q;
        DropRun e(q);
        q.takes = [](std::size_t n, const std::vector<Ev>&) { return n == 1 ? 0u : 2u; };
        e.go(yes);
        check(p.went_as({ Step::Lead, Step::Down, Step::BothUp }) && d.r.sent == Sent::Aborted && d.r.down_ms == 0 &&
                  !d.im.keys_owed() && q.went_as({ Step::Lead, Step::Down, Step::BothUp }) &&
                  e.r.sent == Sent::Aborted && !e.im.keys_owed(),
              "Windows took 1 or 0 of [Ctrl↓, ↓↓]: [↓↑, Ctrl↑] at once, Aborted -- " + p.said());
    }
    // ---- ↓'s up not taken, nor the second try: both owed ----
    {
        Pretend p;
        DropRun d(p);
        p.takes = [](std::size_t n, const std::vector<Ev>& events) {
            return n == 2 || n == 3 ? 0u : static_cast<unsigned>(events.size());
        };
        d.go(yes);
        const bool owed = p.went_as({ Step::Lead, Step::Down, Step::DownUp, Step::BothUp }) && d.im.keys_owed() &&
                          d.r.sent == Sent::Sent && d.im.owed_tries() == 0;
        check(owed, "↓'s up not taken, nor [↓↑, Ctrl↑] after it: both owed, and Ctrl↑ never sent alone -- " +
                        p.said());

        // Meanwhile nothing goes down, from any thread or manager.
        const std::size_t before = p.log().size();
        const unsigned own_down = InputManager::pretend_inject({ kADown });
        unsigned other_down = 9, other_click = 9, other_up = 0, other_move = 0;
        std::thread other([&] {
            other_down = InputManager::pretend_inject({ kADown });
            other_click = InputManager::pretend_inject({ mouse(MOUSEEVENTF_LEFTDOWN) });
            other_up = InputManager::pretend_inject({ kAUp });
            other_move = InputManager::pretend_inject({ mouse(MOUSEEVENTF_MOVE) });
        });
        other.join();
        InputManager second;
        second.set_target_window(kWindow);
        const Sent second_drop = second.drop_ctrl_down(yes).sent;
        const Sent again = d.im.drop_ctrl_down(yes).sent;
        const std::vector<Pretend::Got> g = p.log();
        check(own_down == 0 && other_down == 0 && other_click == 0 && other_up == 1 && other_move == 1 &&
                  g.size() == before + 2 && second_drop == Sent::NotSent && again == Sent::NotSent,
              "while they are owed: no key or button goes down, this thread's or another's; ups and moves go; "
              "no drop starts, on this manager or another");

        // Sent again until they go: three tries that do not are kOwedTries.
        p.takes = [](std::size_t, const std::vector<Ev>&) { return 0u; };
        const bool t1 = d.im.settle_owed(), t2 = d.im.settle_owed(), t3 = d.im.settle_owed();
        const int tries = d.im.owed_tries();
        p.takes = nullptr;
        const std::size_t at = p.log().size();
        const bool settled = d.im.settle_owed();
        const std::vector<Pretend::Got> h = p.log();
        check(!t1 && !t2 && !t3 && tries == InputManager::kOwedTries && settled && h.size() == at + 1 &&
                  same(h.back().events, { kDownUp, kCtrlUp }) && !d.im.keys_owed() && d.im.owed_tries() == 0,
              "settle_owed: [↓↑, Ctrl↑] each time; 3 not taken count 3 tries; taken, nothing owed and the count "
              "back to 0");
        check(InputManager::pretend_inject({ kADown }) == 1, "and keys go down again once nothing is owed");
    }
    // ---- owed, then release_all: F12, DỪNG, HUNT OFF ----
    {
        Pretend p;
        DropRun d(p);
        p.takes = [](std::size_t n, const std::vector<Ev>& events) {
            return n == 2 || n == 3 ? 0u : static_cast<unsigned>(events.size());
        };
        d.go(yes);
        const bool owed = d.im.keys_owed();
        const std::size_t at = p.log().size();
        d.im.release_all();
        const std::vector<Pretend::Got> g = p.log();
        check(owed && g.size() == at + 1 && same(g.back().events, { kDownUp, kCtrlUp }) && !d.im.keys_owed(),
              "release_all sends what a drop owes, ↓'s up first in one array with Ctrl's");
    }
    // ---- ↓'s up taken on the second try, part way ----
    {
        Pretend p;
        DropRun d(p);
        p.takes = [](std::size_t n, const std::vector<Ev>& events) {
            return n == 2 ? 0u : n == 3 ? 1u : static_cast<unsigned>(events.size());
        };
        d.go(yes);
        check(p.went_as({ Step::Lead, Step::Down, Step::DownUp, Step::BothUp, Step::CtrlUp }) && !d.im.keys_owed() &&
                  d.r.sent == Sent::Sent,
              "↓'s up not taken, [↓↑, Ctrl↑] only its first: Ctrl's up after, alone, now that ↓ is up -- " + p.said());
    }
    // ---- Ctrl's up not taken ----
    {
        Pretend p;
        DropRun d(p);
        p.takes = [](std::size_t n, const std::vector<Ev>& events) {
            return n == 3 ? 0u : static_cast<unsigned>(events.size());
        };
        d.go(yes);
        const bool retried = p.went_as({ Step::Lead, Step::Down, Step::DownUp, Step::CtrlUp, Step::CtrlUp }) &&
                             !d.im.keys_owed();
        Pretend q;
        DropRun e(q);
        q.takes = [](std::size_t n, const std::vector<Ev>& events) {
            return n == 3 || n == 4 ? 0u : static_cast<unsigned>(events.size());
        };
        e.go(yes);
        const bool owed = e.im.keys_owed();
        const std::size_t at = q.log().size();
        const bool settled = e.im.settle_owed();
        const std::vector<Pretend::Got> g = q.log();
        check(retried && owed && settled && g.size() == at + 1 && same(g.back().events, { kCtrlUp }) &&
                  !e.im.keys_owed(),
              "Ctrl's up tried again once at once; not taken twice, owed, and sent alone -- ↓ is up -- by "
              "settle_owed");
    }
    // ---- something throws in the middle: the guard ----
    {
        Pretend p;
        DropRun d(p);
        bool thrown = false;
        p.on_send = [&thrown](const std::vector<Ev>& events) {
            if (!thrown && same(events, { kDownUp })) {
                thrown = true;
                throw std::runtime_error("a test");
            }
        };
        d.go(yes);
        check(thrown && p.went_as({ Step::Lead, Step::Down, Step::BothUp }) && d.r.sent == Sent::Cut &&
                  d.r.down_ms != 0 && !d.im.keys_owed(),
              "an exception as ↓'s up goes out: the guard sends [↓↑, Ctrl↑], and the call returns Cut -- " +
                  p.said());
    }
    // ---- ↓ still read down by Windows after the drop ----
    {
        Pretend p;
        DropRun d(p);
        p.down_held = true;
        d.go(yes);
        Pretend q;
        DropRun e(q);
        e.go(yes);
        check(p.went_as({ Step::Lead, Step::Down, Step::DownUp, Step::CtrlUp, Step::BothUp }) && !d.im.keys_owed() &&
                  q.went_as({ Step::Lead, Step::Down, Step::DownUp, Step::CtrlUp }),
              "Windows still reading ↓ down after the drop: [↓↑, Ctrl↑] once more; reading it up, nothing more -- " +
                  p.said());
    }
    // ---- other threads while ↓ is held ----
    {
        Pretend p;
        DropRun d(p);
        std::atomic<bool> down{ false }, tried{ false };
        p.on_send = [&down](const std::vector<Ev>& events) {
            if (is_down_array(events)) down = true;
        };
        unsigned key_down = 9, click = 9, key_up = 0, move = 0;
        std::thread other([&] {
            wait_for([&] { return down.load(); });
            key_down = InputManager::pretend_inject({ kADown });
            click = InputManager::pretend_inject({ mouse(MOUSEEVENTF_RIGHTDOWN) });
            key_up = InputManager::pretend_inject({ kAUp });
            move = InputManager::pretend_inject({ mouse(MOUSEEVENTF_MOVE) });
            tried = true;
        });
        d.go([&] {
            if (down.load()) wait_for([&] { return tried.load(); });
            return true;
        });
        other.join();
        unsigned after = 0;
        std::thread later([&] { after = InputManager::pretend_inject({ kADown }); });
        later.join();
        check(down && key_down == 0 && click == 0 && key_up == 1 && move == 1 && d.r.sent == Sent::Sent && after == 1,
              "another thread while ↓ is held: its key-down and its click refused, its key-up and move let through; "
              "its key-down goes once the drop is over -- " + p.said());
    }
}

void test_release_stray() {
    section("A ↓ or Ctrl left down in Windows let go of: ↓ first, ups only");

    Pretend p;
    InputManager im;
    p.down_held = true;
    p.ctrl_held = true;
    const int both = im.release_stray_drop_keys();
    const bool both_went = p.went_as({ Step::DownUp, Step::CtrlUp });
    Pretend q;
    q.ctrl_held = true;
    const int ctrl = im.release_stray_drop_keys();
    const bool ctrl_went = q.went_as({ Step::CtrlUp });
    Pretend r;
    r.down_held = true;
    const int down = im.release_stray_drop_keys();
    const bool down_went = r.went_as({ Step::DownUp });
    Pretend s;
    const int none = im.release_stray_drop_keys();
    check(both == 2 && both_went && ctrl == 1 && ctrl_went && down == 1 && down_went && none == 0 && s.log().empty(),
          "both read down: [↓↑] then [Ctrl↑]; one: its up alone; none: nothing -- no window needed, nothing down");
}

// ------------------------------------------------------------ the token

/// The macro on a clock of the test's own, pressing through pretend hands,
/// with the coordinator the drop takes its token from.
struct MacroRig {
    InputManager         input;
    ExecutionCoordinator coord;
    MacroEngine          engine{ input, coord };
    std::uint64_t        t = 9'000'000;
    std::vector<std::uint32_t> pressed;

    MacroRig() {
        MacroEngine::Clock c;
        c.now_ms = [this] { return t; };
        c.sleep_ms = [this](std::uint32_t ms) { t += ms; };
        engine.set_clock(c);
        MacroEngine::Hands h;
        h.focused = [] { return true; };
        h.key = [this](std::uint32_t vk, int) {
            pressed.push_back(vk);
            return true;
        };
        h.held = [](std::uint32_t) { return false; };
        h.release_all = [] {};
        engine.set_hands(h);
        MacroKey k;
        k.keycode = 'Z';
        k.label = "Skill";
        k.enabled = true;
        k.interval_ms = 1000;
        k.jitter_pct = 0;
        MacroConfig cfg;
        cfg.apply_defaults();
        cfg.keys = { k };
        cfg.sequence_enabled = false;
        cfg.require_focus = true;
        cfg.global_jitter_pct = 0;
        engine.set_config(cfg);
        engine.start_stepped();
    }
    ~MacroRig() { engine.stop(); }
    MacroRig(const MacroRig&) = delete;
    MacroRig& operator=(const MacroRig&) = delete;

    void run(std::uint64_t ms) {
        for (const std::uint64_t until = t + ms; t < until; t += 10) {
            engine.set_vitals(100.0f, 100.0f);
            engine.step();
        }
    }
};

void test_drop_token() {
    section("The drop's token: taken only from nobody, held at Combat");

    {
        ExecutionCoordinator c;
        bool held_while = false;
        {
            auto drop = c.acquire_free("hunt-drop", Priority::Combat);
            held_while = drop.ok() && c.held() && c.current_owner() == "hunt-drop" &&
                         c.current_priority() == Priority::Combat && !c.request("macro", Priority::Combat) &&
                         !c.request("hunt-move", Priority::Movement) && !c.request("loot", Priority::Loot) &&
                         !c.request_free("anyone", Priority::Heal) && !c.request_free("anyone", Priority::Critical);
        }
        const bool free_after = !c.held();
        check(held_while && free_after,
              "taken free at Combat: the macro's Combat, a walk's Movement and the loot are refused, and so is any "
              "request_free; let go of with its token");
        // Only a Critical hold takes it over, and go_on stops the drop for that.
        auto drop = c.acquire_free("hunt-drop", Priority::Combat);
        const bool critical = drop.ok() && c.request("revive", Priority::Critical) &&
                              c.current_owner() == "revive";
        c.release("revive");
        check(critical, "a Critical hold still takes it over");
    }
    {
        ExecutionCoordinator c;
        const bool walk = c.request("hunt-move", Priority::Movement);
        const bool over_walk = c.acquire_free("hunt-drop", Priority::Combat).ok();
        const bool over_walk_high = c.request_free("hunt-drop", Priority::Heal);
        const bool request_takes = c.request("macro", Priority::Combat);     // as ever: a lower one taken over
        c.release("macro");
        c.engage_critical("a test");
        const bool in_critical = c.request_free("hunt-drop", Priority::Combat);
        const bool critical_own = c.request_free("revive", Priority::Critical);
        c.release("revive");
        c.disengage_critical();
        check(walk && !over_walk && !over_walk_high && request_takes && !in_critical && critical_own,
              "never taken over a holder, even a walk's Movement, at any priority -- request() still takes over "
              "a lower one; refused below Critical in critical mode");
    }
    {
        MacroRig rig;
        rig.run(1500);
        const bool pressing = !rig.pressed.empty();
        rig.pressed.clear();
        const std::uint64_t busy_before = rig.engine.stats().skipped_busy;
        bool held_through = false;
        {
            auto drop = rig.coord.acquire_free("hunt-drop", Priority::Combat);
            rig.run(3000);
            held_through = drop.ok() && rig.coord.current_owner() == "hunt-drop";
        }
        const bool none_pressed = rig.pressed.empty() && rig.engine.stats().skipped_busy > busy_before;
        rig.run(2000);
        check(pressing && held_through && none_pressed && !rig.pressed.empty(),
              "the macro's presses, refused as Busy while the drop holds the token -- no skill or potion between "
              "Ctrl's down and its up -- and going out again after");
    }
}

// ------------------------------------------------------------ the source

#ifdef GOBOT_SOURCE_DIR
std::string source(const char* rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// A function's definition, from `head` to the brace that closes it at the
/// start of a line; "" when there is none.
std::string body(const std::string& src, const std::string& head) {
    const std::size_t from = src.find(head);
    if (from == std::string::npos) return {};
    const std::size_t end = src.find("\n}", from);
    return end == std::string::npos ? std::string() : src.substr(from, end + 2 - from);
}

int count(const std::string& text, const std::string& part) {
    int n = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
    return n;
}

/// Whether `a` comes before `b` in `text`, both there.
bool before(const std::string& text, const std::string& a, const std::string& b) {
    const std::size_t i = text.find(a), j = text.find(b);
    return i != std::string::npos && j != std::string::npos && i < j;
}
#endif

void test_drop_sources() {
    section("The drop's key, read off the source: ↓ only inside Ctrl, the guard first");

#ifdef GOBOT_SOURCE_DIR
    const std::string im = source("/src/input_manager.cpp");
    const std::string hpp = source("/include/gobot/input_manager.hpp");
    if (im.empty() || hpp.empty()) {
        skip("the source tree is not here\n");
        return;
    }

    // inject(): the ↓ guard first, then other threads, then the pretend
    // Windows, the switch and Windows itself -- the one SendInput there is.
    const std::string inject = body(im, "UINT inject(INPUT* in, UINT count, const char* what, bool chord = false)");
    check(!inject.empty() && before(inject, "bare_down_among(count, chord, at)", "g_chord_busy.load() > 0") &&
              before(inject, "g_chord_owner.load() != GetCurrentThreadId()", "pretend()") &&
              before(inject, "pretend()", "refuse_real_input(what)") &&
              before(inject, "refuse_real_input(what)", "SendInput(count, in, sizeof(INPUT))") &&
              count(im, "SendInput(") == 1,
          "inject(): bare ↓ refused first, then other threads' downs during a drop, then the pretend sender, the "
          "switch and SendInput -- the file's only SendInput");

    // Only the drop's ↓ is sent as a chord, and ↓'s events are built in one place.
    const std::string chord = body(im, "void InputManager::drop_chord(");
    check(!chord.empty() && count(im, "kChord") == 2 &&
              chord.find("inject(b, n, \"press Ctrl + Down\", kChord)") != std::string::npos &&
              count(im, "key(kDownVk, kDownScan") == 2 &&
              body(im, "UINT drop_events(").find("key(kDownVk, kDownScan") != std::string::npos,
          "`chord` passed once, for [Ctrl↓, ↓↓] in drop_chord; ↓'s events built only in drop_events");
    bool down_sent_elsewhere = false;
    for (std::size_t at = 0; at < im.size();) {
        const std::size_t end = std::min(im.find('\n', at), im.size());
        const std::string line = im.substr(at, end - at);
        if (line.find("VK_DOWN") != std::string::npos &&
            (line.find("wVk") != std::string::npos || line.find("send_key(") != std::string::npos ||
             line.find("key_down(") != std::string::npos || line.find("key_press(") != std::string::npos ||
             line.find("type_key(") != std::string::npos || line.find("inject(") != std::string::npos))
            down_sent_elsewhere = true;
        at = end + 1;
    }
    check(!down_sent_elsewhere, "VK_DOWN is never put into an event or handed to a key call anywhere in the file");

    // The drop: the switch asked before the window, the game in front or
    // go_on; ↓ only after the look under the lock; the ups not gated on focus.
    const std::size_t ups = chord.find("// ---- ↓ up");
    check(before(chord, "refuse_real_input(\"press Ctrl + Down", "if (!in_front()) return;") &&
              before(chord, "if (!in_front()) return;", "if (!asks_yes(go_on)) return;") &&
              before(chord, "if (block_movement_.load()) return;", "if (!in_front()) return;") &&
              before(chord, "if (!go || chord_cancel_.load() || !in_front())", "\"press Ctrl + Down\", kChord") &&
              ups != std::string::npos && chord.find("in_front()", ups) == std::string::npos &&
              chord.find("window_focused", ups) == std::string::npos &&
              before(chord, "const OnExit guard(", "// ---- Ctrl alone ----"),
          "drop_chord: the switch, movement and the game in front before go_on; ↓ only after the look; the "
          "guard before Ctrl goes down; ↓'s and Ctrl's ups whatever is in front");
    check(hpp.find("DropPress drop_ctrl_down(const std::function<bool()>& go_on) noexcept;") != std::string::npos &&
              body(im, "InputManager::DropPress InputManager::drop_ctrl_down(").find("catch (...)") !=
                  std::string::npos,
          "drop_ctrl_down is noexcept, and catches what its steps throw");

    // What stays as it was: never_pressed, the old jump_down.
    check(hpp.find("static bool never_pressed(std::uint32_t vk) { return (vk & 0xFFu) == 0x28u; }") !=
                  std::string::npos &&
              body(im, "bool InputManager::jump_down()").find("if (!key_down(key_down_)) return false;") !=
                  std::string::npos,
          "never_pressed unchanged; jump_down still refused at its ↓, as ever");

    // release_all lets go of the drop's keys first, in their order; F12,
    // DỪNG and HUNT OFF all come through it.
    const std::string release = body(im, "void InputManager::release_all()");
    check(before(release, "chord_cancel_ = true;", "held_keys_.clear();") &&
              before(release, "send_owed();", "held_keys_.clear();"),
          "release_all: the drop cut short and its ups sent before the held keys go");
    const std::string hunter = source("/src/hunter.cpp");
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string stop = body(hunter, "void Hunter::stop()");
    const std::string jobs = body(gui, "bool GuiApp::stop_jobs(bool with_bot)");
    const std::string tick = body(gui, "void GuiApp::tick()");
    check(before(stop, "running_.exchange(false)", "release_movement();") &&
              body(hunter, "void Hunter::release_movement()").find("input_.release_all();") != std::string::npos &&
              jobs.find("input_.release_all();") != std::string::npos &&
              before(tick, "if (keys.panic) {", "stop_jobs(true);"),
          "HUNT OFF (Hunter::stop: go_on false, then release_all), F12 and DỪNG (stop_jobs) let go of Ctrl");
    check(before(tick, "if (input_.keys_owed() && now_ms() - owed_look_ms_ >= 100)", "input_.settle_owed()") &&
              before(tick, "input_.owed_tries() >= InputManager::kOwedTries && hunting_", "stop_hunt();"),
          "GoBot's window sends owed ups again every 100 ms, and stops the hunt after 3 tries that did not go");
    check(before(tick, "input_.owed_tries() >= InputManager::kOwedTries && hunting_", "drop_stopped_vi_ =") &&
              before(tick, "drop_stopped_vi_ =", "simple_note(drop_stopped_vi_, 15000);") &&
              before(tick, "simple_note(drop_stopped_vi_, 15000);", "drop_tick();"),
          "and says so in the red line under BẮT ĐẦU, as drop_tick would: the hunt it stopped there is no longer "
          "drop_tick's to say");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_drop_key(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_drop_events();
    test_bare_down_table();
    test_drop_press();
    test_drop_cut_and_owed();
    test_release_stray();
    test_drop_token();
    test_drop_sources();
}
