// gobot_test -- the jumps' keys (2026.11.05b, the jump-up plan's §4): Ctrl +
// ↑, the high jump, as the drop's chord with ↑ its second key
// (InputManager::chord_ctrl, high_jump) -- its exact events, never sent as the
// drop's chord, other threads' keys refused meanwhile, its owed ups ↑ first;
// ↑↑ (jump_twice) -- the second ↑ timed from the first, sent only in time and
// with go_on's yes, never ↓ or Ctrl; ↑ alone (jump_once), extended; the
// window's "Thử" of each kind through JumpCheck::press_with; a stray ↑ let go
// of at BẮT ĐẦU; and the drop's Ctrl + ↓ the same as ever, event for event,
// through chord_ctrl too.
//
// Nothing here reaches the desktop. Every array goes to a pretend Windows
// (InputManager::PretendSender) that writes it down and never calls
// SendInput, listened to only while the "no real input" switch is on, as in
// drop_key_test.cpp; the window aimed at is only a number.

#include "gobot/common.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/jump_check.hpp"
#include "gobot/jump_plan.hpp"
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
using Chord = InputManager::ChordKey;
using JSent = InputManager::JumpSent;

constexpr std::uint16_t kCtrl = 0x11;
constexpr std::uint16_t kCtrlSc = 0x1D;
constexpr std::uint16_t kUpVk = 0x26;
constexpr std::uint16_t kUpSc = 0x48;
constexpr std::uint16_t kDown = 0x28;
constexpr std::uint16_t kDownSc = 0x50;
constexpr std::uint32_t kExt = KEYEVENTF_EXTENDEDKEY;
constexpr std::uint32_t kUpFlag = KEYEVENTF_KEYUP;

Ev key(std::uint16_t vk, std::uint16_t scan, std::uint32_t flags) {
    Ev e;
    e.vk = vk;
    e.scan = scan;
    e.flags = flags;
    return e;
}

const Ev kCtrlDown = key(kCtrl, kCtrlSc, 0);
const Ev kCtrlUp = key(kCtrl, kCtrlSc, kUpFlag);
const Ev kUpDown = key(kUpVk, kUpSc, kExt);
const Ev kUpUp = key(kUpVk, kUpSc, kExt | kUpFlag);
const Ev kADown = key('A', 0x1E, 0);
const Ev kAUp = key('A', 0x1E, kUpFlag);

std::vector<Ev> events_of(Chord k, Step s) {
    std::vector<Ev> out;
    for (const InputManager::KeyEvent& e : InputManager::chord_batch(k, s)) out.push_back(key(e.vk, e.scan, e.flags));
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

/// Whether any event is ↓ to Windows: its key code, or its scan code 0x50.
bool has_down(const std::vector<Ev>& events) {
    for (const Ev& e : events)
        if (!e.mouse && ((e.vk & 0xFFu) == kDown || e.scan == kDownSc)) return true;
    return false;
}

/// An array as a few letters: C Ctrl, U ↑, D ↓, A the A key; + down, - up.
std::string text_of(const std::vector<Ev>& events) {
    std::string s = "[";
    for (std::size_t i = 0; i < events.size(); ++i) {
        const Ev& e = events[i];
        if (i > 0) s += ' ';
        const unsigned vk = e.vk & 0xFFu;
        s += vk == kUpVk ? "U" : vk == kCtrl ? "C" : vk == kDown ? "D" : vk == 'A' ? "A" : "?";
        s += (e.flags & kUpFlag) != 0 ? "-" : "+";
    }
    return s + "]";
}

/// A pretend Windows for one check, as drop_key_test.cpp's: every array that
/// reaches it written down with its thread, priority, time and the chord flag
/// it came with; as many of each taken as `takes` says; the game in front and
/// what Windows reads down as the check sets -- ↑ among those.
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
    std::function<unsigned(std::size_t n, const std::vector<Ev>&)> takes;
    std::function<void(const std::vector<Ev>&)> on_send;
    std::atomic<bool> front{ true };
    std::atomic<bool> down_held{ false };
    std::atomic<bool> up_held{ false };
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
            return vk == VK_DOWN ? down_held.load() : vk == VK_UP ? up_held.load()
                                                  : vk == VK_CONTROL ? ctrl_held.load() : false;
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
    /// Whether the arrays went out as these steps of `k`, in order, and nothing else.
    bool went_as(Chord k, std::initializer_list<Step> steps) {
        const std::vector<Got> g = log();
        if (g.size() != steps.size()) return false;
        std::size_t i = 0;
        for (const Step s : steps)
            if (!same(g[i++].events, events_of(k, s))) return false;
        return true;
    }
    bool went(std::initializer_list<std::vector<Ev>> arrays) {
        const std::vector<Got> g = log();
        if (g.size() != arrays.size()) return false;
        std::size_t i = 0;
        for (const std::vector<Ev>& a : arrays)
            if (!same(g[i++].events, a)) return false;
        return true;
    }
    std::string said() {
        std::string s;
        for (const Got& g : log()) {
            s += text_of(g.events);
            if (g.chord) s += "(chord)";
            if (g.took < g.events.size()) s += "(took " + std::to_string(g.took) + ")";
            s += " ";
        }
        return s.empty() ? "nothing" : s;
    }
    bool any_chord() {
        for (const Got& g : log())
            if (g.chord) return true;
        return false;
    }
    bool any_down() {
        for (const Got& g : log())
            if (has_down(g.events)) return true;
        return false;
    }
    bool any_up_key() {
        for (const Got& g : log())
            for (const Ev& e : g.events)
                if ((e.vk & 0xFFu) == kUpVk) return true;
        return false;
    }
};

/// A window that is only a number: nothing is ever sent to it.
void* const kWindow = reinterpret_cast<void*>(std::uintptr_t{ 0x3000 });

bool wait_for(const std::function<bool()>& cond) {
    for (int i = 0; i < 2000 && !cond(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return cond();
}

bool is_up_down_array(const std::vector<Ev>& events) {
    return events.size() == 2 && same(events[0], kCtrlDown) && same(events[1], kUpDown);
}

/// One manager aimed at a window that is only a number, made after its
/// Pretend so that whatever it lets go of on its way out reaches it.
struct Keys {
    InputManager im;
    explicit Keys(Pretend&) { im.set_target_window(kWindow); }
};

// ------------------------------------------------------------ the events

void test_chord_events() {
    section("Jump keys: Ctrl + ↑'s fixed events -- the drop's steps with ↑ {0x26, 0x48, extended}");

    check(same(events_of(Chord::Up, Step::Lead), { kCtrlDown }) &&
              same(events_of(Chord::Up, Step::Down), { kCtrlDown, kUpDown }) &&
              same(events_of(Chord::Up, Step::DownUp), { kUpUp }) &&
              same(events_of(Chord::Up, Step::CtrlUp), { kCtrlUp }) &&
              same(events_of(Chord::Up, Step::BothUp), { kUpUp, kCtrlUp }),
          "Ctrl + ↑: [Ctrl↓], [Ctrl↓, ↑↓ E0 48], [↑↑], [Ctrl↑]; BothUp [↑↑, Ctrl↑] -- ↑ before Ctrl");
    bool drop_same = true, no_down = true, passes = true;
    for (const Step s : { Step::Lead, Step::Down, Step::DownUp, Step::CtrlUp, Step::BothUp }) {
        std::vector<Ev> d;
        for (const InputManager::KeyEvent& e : InputManager::drop_batch(s)) d.push_back(key(e.vk, e.scan, e.flags));
        drop_same = drop_same && same(d, events_of(Chord::Down, s));
        const std::vector<Ev> u = events_of(Chord::Up, s);
        no_down = no_down && !has_down(u);
        passes = passes && !InputManager::bare_down_in(u.data(), u.size(), false);
    }
    check(drop_same, "chord_batch(Down, ...) is drop_batch, step for step: the drop's events as ever");
    check(no_down && passes,
          "no ↓ in any of Ctrl + ↑'s steps -- neither 0x28 nor scan 0x50 -- and each passes inject()'s ↓ guard sent "
          "as any array is, with no chord flag");
    const InputManager::KeyEvent typed = InputManager::typed_key_event(VK_UP, true, InputManager::KeyInject::Vk);
    check(InputManager::scan_code(VK_UP) == kUpSc && typed.vk == kUpVk && typed.scan == kUpSc &&
              (typed.flags & kExt) != 0,
          "↑ pressed as any key is -- the hunter's own press, send_key -- is extended, E0 48 too: never numpad 8");
    check(InputManager::kJumpHoldMinMs == 80 && InputManager::kJumpHoldMaxMs == 110 &&
              InputManager::kJumpSecondJitterMs == 15 && InputManager::kJumpSecondSendLateMs == 60 &&
              InputManager::kJumpHeldLongMs == 250 && kJumpUpSecondAtMs == 340,
          "↑ held 80-110 ms; the second of ↑↑ 340 ±15 ms after the first, never past 400; a jump key held past "
          "250 ms is HeldLong");
    InputManager::JumpPress p;
    p.held1 = 250;
    p.held2 = 90;
    const bool at = p.held_long();
    p.held2 = 251;
    check(!at && p.held_long(), "held_long: 250 ms is not, 251 is -- either key");
}

// ------------------------------------------------------------ Ctrl + ↑

void test_high_jump() {
    section("Jump keys: Ctrl + ↑ pressed -- Ctrl, [Ctrl↓, ↑↓], ↑ up, Ctrl up; never as the drop's chord");

    const auto yes = [] { return true; };

    // ---- a press ----
    {
        Pretend p;
        Keys k(p);
        const int priority_before = GetThreadPriority(GetCurrentThread());
        const InputManager::DropPress r = k.im.high_jump(yes);
        const std::vector<Pretend::Got> g = p.log();
        const bool four = p.went_as(Chord::Up, { Step::Lead, Step::Down, Step::DownUp, Step::CtrlUp });
        bool urgent = four;
        for (std::size_t i = 0; four && i < g.size(); ++i)
            urgent = urgent && g[i].priority == THREAD_PRIORITY_TIME_CRITICAL;
        check(four && !p.any_chord() && !p.any_down() && r.sent == Sent::Sent && !k.im.keys_owed(),
              "Sent: [Ctrl↓], [Ctrl↓, ↑↓], [↑↑], [Ctrl↑] -- none as the drop's chord, no ↓ anywhere, nothing owed "
              "-- " + p.said());
        if (four) {
            const std::uint64_t lead = g[1].at_ms - g[0].at_ms, held = g[2].at_ms - g[1].at_ms,
                                tail = g[3].at_ms - g[2].at_ms, all = g[3].at_ms - g[0].at_ms;
            check(lead >= 40 && held >= 80 && tail >= 40 && r.held_ms >= 80 &&
                      r.held_ms < InputManager::kJumpHeldLongMs && r.lead_ms >= 40 && r.tail_ms >= 40 &&
                      r.down_ms >= g[1].at_ms && r.down_ms <= g[2].at_ms && all < 1000,
                  "Ctrl alone " + std::to_string(lead) + " ms (40-60; said " + std::to_string(r.lead_ms) +
                      "), ↑ held " + std::to_string(held) + " ms (80-110; said " + std::to_string(r.held_ms) +
                      "), Ctrl " + std::to_string(tail) + " ms after ↑'s up (said " + std::to_string(r.tail_ms) +
                      "): " + std::to_string(all) + " ms in all");
        } else {
            check(false, "the high jump's timings: no press to time");
        }
        check(urgent && GetThreadPriority(GetCurrentThread()) == priority_before,
              "at TIME_CRITICAL, the thread put back after");
        const InputManager::DropPress again = k.im.chord_ctrl(Chord::Up, yes);
        check(again.sent == Sent::Sent && p.log().size() == 8, "chord_ctrl(Up) is the same press");
    }
    // ---- go_on says no at the look before ↑ ----
    {
        Pretend p;
        Keys k(p);
        int asked = 0;
        const InputManager::DropPress r = k.im.high_jump([&] { return ++asked == 1; });
        check(p.went_as(Chord::Up, { Step::Lead, Step::CtrlUp }) && !p.any_up_key() && r.sent == Sent::Aborted &&
                  r.down_ms == 0 && !k.im.keys_owed(),
              "go_on false at the look before ↑: [Ctrl↓], [Ctrl↑] -- the pick-up alone, no ↑ -- Aborted -- " + p.said());
    }
    // ---- nothing down at all ----
    {
        Pretend p;
        InputManager none;
        const Sent no_window = none.high_jump(yes).sent;
        Keys blocked(p);
        blocked.im.set_block_movement(true);
        const Sent b = blocked.im.high_jump(yes).sent;
        Keys behind(p);
        p.front = false;
        const Sent f = behind.im.high_jump(yes).sent;
        p.front = true;
        Keys refused(p);
        const Sent n = refused.im.high_jump([] { return false; }).sent;
        check(no_window == Sent::NotSent && b == Sent::NotSent && f == Sent::NotSent && n == Sent::NotSent &&
                  p.log().empty(),
              "NotSent, nothing sent: no window, movement blocked, the game behind, go_on false");
    }
    // ---- other threads while ↑ is held ----
    {
        Pretend p;
        Keys k(p);
        std::atomic<bool> down{ false }, tried{ false };
        p.on_send = [&down](const std::vector<Ev>& events) {
            if (is_up_down_array(events)) down = true;
        };
        unsigned key_down = 9, key_up = 0;
        std::thread other([&] {
            wait_for([&] { return down.load(); });
            key_down = InputManager::pretend_inject({ kADown });
            key_up = InputManager::pretend_inject({ kAUp });
            tried = true;
        });
        const InputManager::DropPress r = k.im.high_jump([&] {
            if (down.load()) wait_for([&] { return tried.load(); });
            return true;
        });
        other.join();
        unsigned after = 0;
        std::thread later([&] { after = InputManager::pretend_inject({ kADown }); });
        later.join();
        check(down && key_down == 0 && key_up == 1 && r.sent == Sent::Sent && after == 1,
              "another thread while ↑ is held inside Ctrl: its key-down refused -- it would be Ctrl + that key -- its "
              "key-up let through, and its key-down goes once the jump is over -- " + p.said());
    }
    // ---- ↑'s up not taken, nor [↑↑, Ctrl↑]: owed, ↑ first ----
    {
        Pretend p;
        Keys k(p);
        p.takes = [](std::size_t n, const std::vector<Ev>& events) {
            return n == 2 || n == 3 ? 0u : static_cast<unsigned>(events.size());
        };
        const InputManager::DropPress r = k.im.high_jump(yes);
        const bool owed = p.went_as(Chord::Up, { Step::Lead, Step::Down, Step::DownUp, Step::BothUp }) &&
                          k.im.keys_owed() && r.sent == Sent::Sent;
        const unsigned blocked = InputManager::pretend_inject({ kADown });
        InputManager second;
        second.set_target_window(kWindow);
        const Sent drop = second.drop_ctrl_down(yes).sent;
        const JSent jump = k.im.jump_twice(VK_UP, kJumpUpSecondAtMs, yes).sent;
        check(owed && blocked == 0 && drop == Sent::NotSent && jump == JSent::NotSent,
              "↑'s up not taken twice: both owed, Ctrl↑ never sent alone -- and meanwhile no key goes down, no drop "
              "starts, no ↑↑ either -- " + p.said());
        p.takes = nullptr;
        const std::size_t at = p.log().size();
        const bool settled = k.im.settle_owed();
        const std::vector<Pretend::Got> g = p.log();
        check(settled && g.size() == at + 1 && same(g.back().events, { kUpUp, kCtrlUp }) && !k.im.keys_owed(),
              "settle_owed: [↑↑, Ctrl↑] in one array, ↑'s up first");
    }
    // ---- owed, then release_all: F12, DỪNG, HUNT OFF ----
    {
        Pretend p;
        Keys k(p);
        p.takes = [](std::size_t n, const std::vector<Ev>& events) {
            return n == 2 || n == 3 ? 0u : static_cast<unsigned>(events.size());
        };
        k.im.high_jump(yes);
        const bool owed = k.im.keys_owed();
        p.takes = nullptr;
        const std::size_t at = p.log().size();
        k.im.release_all();
        const std::vector<Pretend::Got> g = p.log();
        check(owed && g.size() == at + 1 && same(g.back().events, { kUpUp, kCtrlUp }) && !k.im.keys_owed(),
              "release_all sends what the high jump owes in the same order: [↑↑, Ctrl↑]");
    }
    // ---- release_all from another thread in the hold ----
    {
        Pretend p;
        Keys k(p);
        std::atomic<bool> down{ false }, released{ false }, running{ true };
        p.on_send = [&down](const std::vector<Ev>& events) {
            if (is_up_down_array(events)) down = true;
        };
        DWORD stopper = 0;
        std::thread stop([&] {
            stopper = GetCurrentThreadId();
            wait_for([&] { return down.load(); });
            running = false;
            k.im.release_all();
            released = true;
        });
        const InputManager::DropPress r = k.im.high_jump([&] {
            if (down.load()) wait_for([&] { return released.load(); });
            return running.load();
        });
        stop.join();
        const std::vector<Pretend::Got> g = p.log();
        check(p.went_as(Chord::Up, { Step::Lead, Step::Down, Step::BothUp }) && g.size() == 3 &&
                  g[2].thread == stopper && r.down_ms != 0 && !k.im.keys_owed(),
              "release_all from another thread while ↑ is held: [↑↑, Ctrl↑] from it, nothing after from the jump -- " +
                  p.said());
    }
    // ---- ↑ still read down after ----
    {
        Pretend p;
        Keys k(p);
        p.up_held = true;
        k.im.high_jump(yes);
        check(p.went_as(Chord::Up, { Step::Lead, Step::Down, Step::DownUp, Step::CtrlUp, Step::BothUp }) &&
                  !k.im.keys_owed(),
              "Windows still reading ↑ down after the jump: [↑↑, Ctrl↑] once more -- " + p.said());
    }
}

/// chord_ctrl(Down) is drop_ctrl_down: the same arrays, the same flag on the
/// one that puts ↓ down, and nothing else of the chord's changed by ↑.
void test_drop_as_ever() {
    section("Jump keys: the drop's Ctrl + ↓ as ever -- drop_ctrl_down and chord_ctrl(Down) event for event");

    const auto yes = [] { return true; };
    Pretend p;
    Keys a(p);
    const InputManager::DropPress d = a.im.drop_ctrl_down(yes);
    const std::vector<Pretend::Got> one = p.log();
    Keys b(p);
    const InputManager::DropPress c = b.im.chord_ctrl(Chord::Down, yes);
    const std::vector<Pretend::Got> all = p.log();
    bool same_arrays = one.size() == 4 && all.size() == 8;
    for (std::size_t i = 0; same_arrays && i < 4; ++i)
        same_arrays = same(one[i].events, all[i + 4].events) && one[i].chord == all[i + 4].chord &&
                      one[i].chord == (i == 1);
    check(same_arrays && d.sent == Sent::Sent && c.sent == Sent::Sent &&
              same(one[1].events, events_of(Chord::Down, Step::Down)) && d.held_ms >= 70 && c.held_ms >= 70,
          "drop_ctrl_down and chord_ctrl(Down): [Ctrl↓], [Ctrl↓, ↓↓] -- the one array sent as the drop's chord -- "
          "[↓↑], [Ctrl↑], ↓ held its 70-110 ms -- " + p.said());
}

// ------------------------------------------------------------ ↑↑ and ↑

void test_jump_twice() {
    section("Jump keys: ↑↑ -- the second ↑ 340 ±15 ms after the first, in time or not at all; ↑ alone, extended");

    const auto yes = [] { return true; };
    const std::vector<Ev> up_down{ kUpDown }, up_up{ kUpUp };

    // ---- both ----
    {
        Pretend p;
        Keys k(p);
        const InputManager::JumpPress r = k.im.jump_twice(VK_UP, kJumpUpSecondAtMs, yes);
        const std::vector<Pretend::Got> g = p.log();
        const bool four = p.went({ up_down, up_up, up_down, up_up });
        bool plain = four;
        for (std::size_t i = 0; four && i < g.size(); ++i)
            plain = plain && !g[i].chord && g[i].priority == THREAD_PRIORITY_TIME_CRITICAL;
        check(four && plain && r.sent == JSent::Both && !p.any_down() && !k.im.is_held(VK_UP),
              "Both: [↑↓], [↑↑], [↑↓], [↑↑], each E0 48, none as a chord, at TIME_CRITICAL; nothing held after -- " +
                  p.said());
        if (four) {
            const std::uint64_t gap = g[2].at_ms - g[0].at_ms, held1 = g[1].at_ms - g[0].at_ms,
                                held2 = g[3].at_ms - g[2].at_ms;
            const std::uint64_t said_gap = r.second_down_ms - r.first_down_ms;
            check(gap >= 325 && gap <= 362 && said_gap >= 325 && said_gap <= 362 && held1 >= 80 && held2 >= 80 &&
                      r.held1 >= 80 && r.held2 >= 80 && !r.held_long(),
                  "the second ↑ " + std::to_string(gap) + " ms after the first (340 ±15; said " +
                      std::to_string(said_gap) + "), held " + std::to_string(held1) + " and " +
                      std::to_string(held2) + " ms (80-110; said " + std::to_string(r.held1) + ", " +
                      std::to_string(r.held2) + ")");
        } else {
            check(false, "↑↑'s timings: no press to time");
        }
    }
    // ---- go_on says no in the wait: the first only ----
    {
        Pretend p;
        Keys k(p);
        std::atomic<bool> first_up{ false };
        p.on_send = [&first_up](const std::vector<Ev>& events) {
            if (same(events, { kUpUp })) first_up = true;
        };
        const InputManager::JumpPress r = k.im.jump_twice(VK_UP, kJumpUpSecondAtMs, [&] { return !first_up.load(); });
        check(p.went({ up_down, up_up }) && r.sent == JSent::First && r.second_down_ms == 0 && r.held1 >= 80,
              "go_on no after the first ↑ -- a hold, the game behind: no second, First, the first held its time -- " +
                  p.said());
    }
    // ---- go_on says no in the first hold ----
    {
        Pretend p;
        Keys k(p);
        std::atomic<bool> down{ false };
        p.on_send = [&down](const std::vector<Ev>& events) {
            if (same(events, { kUpDown })) down = true;
        };
        const InputManager::JumpPress r = k.im.jump_twice(VK_UP, kJumpUpSecondAtMs, [&] { return !down.load(); });
        check(p.went({ up_down, up_up }) && r.sent == JSent::First && r.held1 < InputManager::kJumpHoldMinMs &&
                  !k.im.is_held(VK_UP),
              "go_on no in the first hold: ↑ up at once (" + std::to_string(r.held1) + " ms), no second -- " +
                  p.said());
    }
    // ---- too late for a second ----
    {
        Pretend p;
        Keys k(p);
        std::atomic<bool> first_up{ false }, slept{ false };
        p.on_send = [&first_up](const std::vector<Ev>& events) {
            if (same(events, { kUpUp })) first_up = true;
        };
        const InputManager::JumpPress r = k.im.jump_twice(VK_UP, kJumpUpSecondAtMs, [&] {
            // A thread put off past the second's time: 400 ms gone at once.
            if (first_up.load() && !slept.exchange(true)) std::this_thread::sleep_for(std::chrono::milliseconds(400));
            return true;
        });
        check(slept && p.went({ up_down, up_up }) && r.sent == JSent::First,
              "put off past 340 + 60 ms after the first: no second at all, rather than one too late -- " + p.said());
    }
    // ---- the game behind before the second ----
    {
        Pretend p;
        Keys k(p);
        p.on_send = [&p](const std::vector<Ev>& events) {
            if (same(events, { kUpUp })) p.front = false;
        };
        const InputManager::JumpPress r = k.im.jump_twice(VK_UP, kJumpUpSecondAtMs, yes);
        check(p.went({ up_down, up_up }) && r.sent == JSent::First,
              "the game no longer in front for the second: First -- " + p.said());
    }
    // ---- release_all from another thread in the first hold ----
    {
        Pretend p;
        Keys k(p);
        std::atomic<bool> down{ false };
        p.on_send = [&down](const std::vector<Ev>& events) {
            if (same(events, { kUpDown })) down = true;
        };
        DWORD stopper = 0;
        std::thread stop([&] {
            stopper = GetCurrentThreadId();
            wait_for([&] { return down.load(); });
            k.im.release_all();
        });
        const InputManager::JumpPress r = k.im.jump_twice(VK_UP, kJumpUpSecondAtMs, yes);
        stop.join();
        const std::vector<Pretend::Got> g = p.log();
        check(g.size() >= 2 && same(g[0].events, up_down) && same(g[1].events, up_up) && g[1].thread == stopper &&
                  r.sent == JSent::First && g.size() <= 3 &&
                  std::none_of(g.begin() + 1, g.end(),
                               [&](const Pretend::Got& x) { return same(x.events, up_down); }) &&
                  !k.im.is_held(VK_UP),
              "release_all from another thread in the first hold: ↑ let go of by it -- ↑ is held as any key "
              "pressed here -- and no second ↑ -- " + p.said());
    }
    // ---- never ↓, never Ctrl ----
    {
        Pretend p;
        Keys k(p);
        const JSent down = k.im.jump_twice(VK_DOWN, kJumpUpSecondAtMs, yes).sent;
        const JSent down_hi = k.im.jump_once(0x10028u, yes).sent;
        const JSent ctrl = k.im.jump_twice(VK_CONTROL, kJumpUpSecondAtMs, yes).sent;
        const JSent lctrl = k.im.jump_once(VK_LCONTROL, yes).sent;
        const JSent rctrl = k.im.jump_once(VK_RCONTROL, yes).sent;
        const JSent none = k.im.jump_once(0, yes).sent;
        check(down == JSent::NotSent && down_hi == JSent::NotSent && ctrl == JSent::NotSent &&
                  lctrl == JSent::NotSent && rctrl == JSent::NotSent && none == JSent::NotSent && p.log().empty(),
              "↓ (0x28, 0x10028), Ctrl (either one) or no key: NotSent, nothing sent");
        InputManager no_window;
        Keys blocked(p);
        blocked.im.set_block_movement(true);
        Keys behind(p);
        p.front = false;
        const JSent b = behind.im.jump_once(VK_UP, yes).sent;
        p.front = true;
        check(no_window.jump_once(VK_UP, yes).sent == JSent::NotSent &&
                  blocked.im.jump_twice(VK_UP, kJumpUpSecondAtMs, yes).sent == JSent::NotSent && b == JSent::NotSent &&
                  k.im.jump_once(VK_UP, [] { return false; }).sent == JSent::NotSent && p.log().empty(),
              "no window, movement blocked, the game behind, go_on false: NotSent, nothing sent");
    }
    // ---- ↑ alone ----
    {
        Pretend p;
        Keys k(p);
        const InputManager::JumpPress r = k.im.jump_once(VK_UP, yes);
        const std::vector<Pretend::Got> g = p.log();
        check(p.went({ up_down, up_up }) && r.sent == JSent::First && r.first_down_ms != 0 && r.second_down_ms == 0 &&
                  r.held1 >= 80 && r.held1 < InputManager::kJumpHeldLongMs && g.size() == 2 &&
                  g[1].at_ms - g[0].at_ms >= 80 && !g[0].chord && !k.im.is_held(VK_UP),
              "jump_once: [↑↓ E0 48], [↑↑ E0 48] held " + std::to_string(r.held1) + " ms -- " + p.said());
    }
}

void test_press_with() {
    section("Jump keys: \"Thử ↑\", \"Thử ↑↑\", \"Thử Ctrl + ↑\" press through JumpCheck::press_with -- always ↑, "
            "never ↓, never the drop's chord");

    const auto yes = [] { return true; };
    const std::vector<Ev> up_down{ kUpDown }, up_up{ kUpUp };
    {
        Pretend p;
        Keys k(p);
        const JumpCheck::Press r = JumpCheck::press_with(k.im, JumpKind::Up, yes);
        check(p.went({ up_down, up_up }) && r.went && r.whole && r.down_ms != 0 && r.held_ms >= 80 &&
                  !p.any_chord() && !p.any_down(),
              "↑: [↑↓ E0 48], [↑↑], held " + std::to_string(r.held_ms) + " ms -- " + p.said());
    }
    {
        Pretend p;
        Keys k(p);
        const JumpCheck::Press r = JumpCheck::press_with(k.im, JumpKind::UpUp, yes);
        check(p.went({ up_down, up_up, up_down, up_up }) && r.went && r.whole && r.held_ms >= 80 && !p.any_chord() &&
                  !p.any_down(),
              "↑↑: two ↑ of jump_twice, both whole -- " + p.said());
    }
    {
        Pretend p;
        Keys k(p);
        const JumpCheck::Press r = JumpCheck::press_with(k.im, JumpKind::CtrlUp, yes);
        check(p.went_as(Chord::Up, { Step::Lead, Step::Down, Step::DownUp, Step::CtrlUp }) && r.went && r.whole &&
                  r.held_ms >= 80 && !p.any_chord() && !p.any_down() && !k.im.keys_owed(),
              "Ctrl + ↑: the high jump's [Ctrl↓], [Ctrl↓, ↑↓], [↑↑], [Ctrl↑] -- " + p.said());
    }
    {
        Pretend p;
        Keys k(p);
        const JumpCheck::Press r = JumpCheck::press_with(k.im, JumpKind::CtrlUp, [] { return false; });
        check(!r.went && !r.whole && p.log().empty(), "go_on no from the start: nothing went, not Ctrl either -- " + p.said());
    }
}

void test_stray_up() {
    section("Jump keys: a ↑ left down in Windows let go of at BẮT ĐẦU, after ↓ and before Ctrl");

    Pretend p;
    InputManager im;
    p.down_held = true;
    p.up_held = true;
    p.ctrl_held = true;
    const int all = im.release_stray_drop_keys();
    const bool all_went = p.went({ { key(kDown, kDownSc, kExt | kUpFlag) }, { kUpUp }, { kCtrlUp } });
    Pretend q;
    q.up_held = true;
    const int up = im.release_stray_drop_keys();
    check(all == 3 && all_went && up == 1 && q.went({ { kUpUp } }),
          "↓, ↑ and Ctrl read down: [↓↑], [↑↑], [Ctrl↑], ups only; ↑ alone: [↑↑] -- " + p.said());
}

// ------------------------------------------------------------ the source

#ifdef GOBOT_SOURCE_DIR
std::string source(const char* rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

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

bool before(const std::string& text, const std::string& a, const std::string& b) {
    const std::size_t i = text.find(a), j = text.find(b);
    return i != std::string::npos && j != std::string::npos && i < j;
}
#endif

void test_jump_key_sources() {
    section("Jump keys, read off the source: ↑'s chord never the drop's, ↑↑ never ↓ or Ctrl, the old jumps unused");

#ifdef GOBOT_SOURCE_DIR
    const std::string im = source("/src/input_manager.cpp");
    const std::string hunter = source("/src/hunter.cpp") + source("/src/hunter_drop.cpp");
    if (im.empty() || hunter.empty()) {
        skip("the source tree is not here\n");
        return;
    }
    const std::string chord = body(im, "void InputManager::drop_chord(");
    check(chord.find("high ? inject(b, n, \"press Ctrl + Up\") : inject(b, n, \"press Ctrl + Down\", kChord)") !=
                  std::string::npos &&
              count(im, "kChord") == 2 && count(im, "key(kUpVk, kUpScan") == 2 &&
              body(im, "UINT drop_events(").find("key(kUpVk, kUpScan") != std::string::npos,
          "drop_chord: ↑'s [Ctrl↓, ↑↓] sent as any array, the chord flag for the drop's ↓ alone; ↑'s chord events "
          "built only in drop_events");
    const std::string keys = body(im, "void InputManager::jump_keys(");
    check(before(keys, "if (never_pressed(vk) || is_ctrl_key(vk)) return;", "refuse_real_input(\"press Up to jump\")") &&
              before(keys, "refuse_real_input(\"press Up to jump\")", "if (!asks_yes(go_on)) return;") &&
              before(keys, "g_chord_busy.load() > 0", "Urgent urgent;") && keys.find("kChord") == std::string::npos &&
              keys.find("key_jump_") == std::string::npos,
          "jump_keys: ↓ and Ctrl refused first, then the switch, then go_on; nothing while a chord runs or owes; no "
          "chord flag, no key_jump_");
    const std::string key_one = body(im, "bool InputManager::jump_key(");
    check(before(key_one, "held_keys_[vk] = true;", "inject(&in, 1, \"press Up to jump\")") &&
              before(key_one, "const OnExit guard(", "// ---- the hold") &&
              key_one.find("in.ki.dwFlags = is_extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0;") != std::string::npos,
          "jump_key: held before it goes down, its up guarded, extended as send_key makes a key");
    check(body(im, "InputManager::JumpPress InputManager::jump_twice(").find("catch (...)") != std::string::npos &&
              body(im, "InputManager::JumpPress InputManager::jump_once(").find("catch (...)") != std::string::npos &&
              body(im, "InputManager::DropPress InputManager::chord_ctrl(").find("catch (...)") != std::string::npos &&
              body(im, "InputManager::DropPress InputManager::chord_ctrl(")
                      .find("if (key == ChordKey::Down) return drop_ctrl_down(go_on);") != std::string::npos,
          "jump_twice, jump_once and chord_ctrl catch what their steps throw; chord_ctrl(Down) is drop_ctrl_down");
    check(body(im, "bool InputManager::jump()").find("return key_press(key_jump_);") != std::string::npos &&
              body(im, "bool InputManager::double_jump()").find("jitter(180, 240)") != std::string::npos &&
              hunter.find("input_.jump(") == std::string::npos && hunter.find("double_jump(") == std::string::npos &&
              hunter.find("key_jump") == std::string::npos,
          "InputManager::jump() and double_jump() as they were -- Ctrl's key_jump_ -- and the hunter calls neither");
    const std::string stray = body(im, "int InputManager::release_stray_drop_keys()");
    check(before(stray, "reads_down(fake, VK_DOWN)", "reads_down(fake, VK_UP)") &&
              before(stray, "reads_down(fake, VK_UP)", "reads_down(fake, VK_CONTROL)"),
          "release_stray_drop_keys: ↓, then ↑, then Ctrl");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_jump_key(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_chord_events();
    test_high_jump();
    test_drop_as_ever();
    test_jump_twice();
    test_press_with();
    test_stray_up();
    test_jump_key_sources();
}
