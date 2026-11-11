// gobot_test -- the load of watching the game: the HP/MP reader's pace
// (VitalsReader::set_period_ms) and what counted its reads where time was
// meant (forget_after_misses, potion_pair_gap_ms, [Performance]
// vitals_run_ms); the detector stopped at once, and with the hunt;
// PrintWindow judged once per window for the whole process
// (PrintWindowVerdicts, PrintWindowAsk), and in light mode never asked again
// of a window that refused it; the camera tracker fed only when used; and the
// frames grabbed into again rather than made anew.
//
// On frames, windows and clocks of the test's own: no window is made, nothing
// is grabbed off the screen -- the readers' threads are fed by set_grab --
// and the PrintWindow verdicts are given the time. What only GoBot's window
// does is read off its source.

#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/machine_info.hpp"
#include "gobot/macro.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/potion_stock.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/vitals_reader.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

std::string num(long long v) { return std::to_string(v); }

using Clock = std::chrono::steady_clock;

long long ms_since(Clock::time_point t) {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
}

// ---- HP and MP at 250 ms as at 150 ----

void test_counted_reads() {
    section("HP/MP read every 250 ms in light mode as every 150: what counted reads counts time");

    // The printed figures forgotten, and the HUD's landmark looked for again,
    // after about two seconds of reads without them: twelve at 150 ms, as
    // they always were.
    const int at150 = VitalsReader::forget_after_misses(150);
    const int at250 = VitalsReader::forget_after_misses(250);
    const int at1000 = VitalsReader::forget_after_misses(1000);
    check(at150 == 12 && at150 * 150 == VitalsReader::kForgetAfterMs,
          "at 150 ms the figures are forgotten after 12 reads without them, 1.8 s, as before -- " + num(at150));
    check(at250 == 8 && at250 * 250 >= VitalsReader::kForgetAfterMs &&
              (at250 - 1) * 250 < VitalsReader::kForgetAfterMs,
          "at 250 ms after 8: 2.0 s, the fewest reads that are not sooner than 1.8 s -- " + num(at250));
    check(at1000 == 2, "at a second, with nothing running, after 2: 2 s, not the 12 s that 12 reads would be -- " +
                           num(at1000));
    const int at100 = VitalsReader::forget_after_misses(100);
    const int at5000 = VitalsReader::forget_after_misses(5000);
    const int at0 = VitalsReader::forget_after_misses(0);
    check(at100 == 18 && at5000 == 2 && at0 >= 2,
          "18 at 100 ms; never after a single read -- one unreadable read is ordinary -- at any pace");

    // A look at the potion slots reads two of the reader's frames apart, and
    // they must be two frames.
    const int gap150 = potion_pair_gap_ms(150);
    const int gap250 = potion_pair_gap_ms(250);
    const int gap1000 = potion_pair_gap_ms(1000);
    check(gap150 == kPotionPairGapMs && gap250 == kPotionPairGapMs,
          "a potion look's second frame 400 ms after its first at 150 ms and at 250 ms alike -- " + num(gap150) +
              ", " + num(gap250));
    check(gap150 > 150 && gap250 > 250 && gap1000 > 1000 && gap1000 == 1000 + kPotionPairSlackMs,
          "always more than one read later: a pace set by hand to a second waits " + num(gap1000) +
              " ms, or both frames would be the same one and agree whatever they show");

    // Already in time, and as good at 250 as at 150: HP unread for a second
    // holds the macro -- several unreadable reads in a row at either pace,
    // never one -- and a reading is stale at 1.5 s, far past one read.
    const std::uint64_t seen = 1'000'000;
    const bool one_late150 = VitalsReader::in_world(seen, seen + 150 + 20, MacroEngine::kOutOfWorldAfterMs);
    const bool one_late250 = VitalsReader::in_world(seen, seen + 250 + 20, MacroEngine::kOutOfWorldAfterMs);
    check(one_late150 && one_late250 && MacroEngine::kOutOfWorldAfterMs / 250 >= 4 &&
              MacroEngine::kOutOfWorldAfterMs / 150 >= 6,
          "an unreadable read at either pace is not out of the world: 6 in a row at 150 ms, 4 at 250");
    const float fresh250 = VitalsReader::if_fresh(80.0f, seen, seen + 250 + 50, MacroEngine::kVitalsStaleMs);
    check(fresh250 == 80.0f && MacroEngine::kVitalsStaleMs > 2 * 250,
          "a reading 300 ms old -- one read at 250 ms and its work -- is fresh to the macro");
}

void test_run_pace() {
    section("[Performance] vitals_run_ms: HP/MP's pace while a job runs -- 150 ms, 250 in light mode");
    check(vitals_run_period(0, false) == kVitalsRunMs && vitals_run_period(0, true) == kVitalsRunLightMs &&
              kVitalsRunMs == 150 && kVitalsRunLightMs == 250,
          "0, automatic: 150 ms, and 250 in light mode");
    check(vitals_run_period(400, false) == 400 && vitals_run_period(400, true) == 400 &&
              vitals_run_period(20, false) == 100 && vitals_run_period(60000, true) == 1000,
          "any other value is the player's, light mode or not, held to 100..1000");

    const PerformanceSettings none = performance_settings(IniFile{});
    check(none.vitals_run_ms == 0, "not in app.ini: 0, automatic");
    const auto run_ms = [](const char* value) {
        IniFile ini;
        ini.set("Performance", "vitals_run_ms", value);
        return performance_settings(ini).vitals_run_ms;
    };
    const int zero = run_ms("0");
    const int set = run_ms("250");
    const int low = run_ms("20");
    const int high = run_ms("99999");
    const int negative = run_ms("-5");
    check(zero == 0 && set == 250 && low == 100 && high == 1000 && negative == 100,
          "read as written: 0 automatic, 250 as it is, 20 up to 100, 99999 down to 1000, -5 up to 100 -- " +
              num(zero) + ", " + num(set) + ", " + num(low) + ", " + num(high) + ", " + num(negative));
}

// ---- the readers' threads ----

void test_vitals_pace() {
    section("HP/MP reader: the pace asked for, a shorter one at once, and stopped at once (set_period_ms)");
    {
        VitalsReader idle;
        const int first = idle.period_ms();
        idle.set_period_ms(1);
        const int low = idle.period_ms();
        idle.set_period_ms(10'000'000);
        const int high = idle.period_ms();
        check(first == VitalsReader::kReadGapMs && low == VitalsReader::kMinPeriodMs &&
                  high == VitalsReader::kMaxPeriodMs,
              "150 ms until asked otherwise, as it always was; held to 50..10000 ms");
    }
    {
        // A job starting cuts the idle pace to the run's: reads before that
        // are a second old at most, not a run's pace, and GoBot's window hands
        // the macro none of them (paced_from_ms). Never started: no thread,
        // nothing grabbed.
        VitalsReader r;
        r.set_period_ms(1000);
        const std::uint64_t idle = r.paced_from_ms();
        const std::uint64_t asked = now_ms();
        r.set_period_ms(150);
        const std::uint64_t cut = r.paced_from_ms();
        r.set_period_ms(150);
        const std::uint64_t again = r.paced_from_ms();
        r.set_period_ms(250);
        const std::uint64_t longer = r.paced_from_ms();
        r.set_period_ms(150);
        const std::uint64_t light_off = r.paced_from_ms();
        check(idle == 0 && cut != 0 && cut >= asked && again == cut && longer == cut && light_off == cut,
              "1000 to 150 -- a job starting -- marks the time; the same pace asked again, a longer one, and 250 to "
              "150 (light mode unticked mid-run) leave it -- " + num(static_cast<long long>(cut - asked)) + " ms");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        r.set_period_ms(1000);
        const std::uint64_t asked2 = now_ms();
        r.set_period_ms(150);
        const std::uint64_t next = r.paced_from_ms();
        r.set_period_ms(300);
        r.set_period_ms(150);
        const std::uint64_t halved = r.paced_from_ms();
        check(next >= asked2 && next > cut && halved >= next,
              "the next job's start moves it on; 300 to 150, half, counts as a cut too");
    }

    // The test's own frames, which are none: every grab fails, and is
    // counted. The window is only a number.
    void* const window = reinterpret_cast<void*>(std::uintptr_t{ 0x3000 });
    std::atomic<int> grabs{ 0 };
    VitalsReader vr;
    vr.set_grab([&grabs](void*, Frame&) {
        grabs.fetch_add(1);
        return false;
    });
    vr.set_target_window(window);
    vr.set_period_ms(5000);
    vr.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const int waiting = grabs.load();
    // GoBot's window asks for its pace on every pass of its loop.
    for (int i = 0; i < 10; ++i) {
        vr.set_period_ms(5000);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const int same = grabs.load();
    check(waiting == 0 && same == 0,
          "5 s between reads: nothing read in the first half second, and the same pace asked again every pass "
          "wakes nothing -- " + num(same) + " reads");

    const Clock::time_point asked = Clock::now();
    vr.set_period_ms(150);
    while (grabs.load() == 0 && ms_since(asked) < 3000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const long long took = ms_since(asked);
    const int woke = grabs.load();
    check(woke > 0 && took < 1000,
          "a shorter pace asked for -- a job starting -- reads at once, not when the 5 s wait would have ended: " +
              num(took) + " ms");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    const int later = grabs.load();
    check(later - woke >= 2, "and reads on at the new pace -- " + num(later - woke) + " reads in 0.7 s at 150 ms");

    vr.set_period_ms(5000);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const Clock::time_point stopping = Clock::now();
    vr.stop();
    const long long stop_ms = ms_since(stopping);
    check(!vr.running() && stop_ms < 1000,
          "stopped in the middle of a 5 s wait: back at once, not at its end -- " + num(stop_ms) + " ms");
}

void test_detector_stops_at_once() {
    section("Mob detector: stopped at once, its wait between looks cut short -- GoBot's own thread stops it at "
            "every hunt's end");
    void* const window = reinterpret_cast<void*>(std::uintptr_t{ 0x4000 });
    std::atomic<int> grabs{ 0 };
    MobDetector d;
    d.set_grab([&grabs](void*, Frame&) {
        grabs.fetch_add(1);
        return false;
    });
    d.set_target_window(window);
    long long longest = 0;
    const int nap_ms[] = { 400, 25, 60, 95, 130, 165 };
    for (const int nap : nap_ms) {
        d.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(nap));
        const Clock::time_point t = Clock::now();
        d.stop();
        longest = std::max(longest, ms_since(t));
    }
    check(!d.running() && grabs.load() > 0 && longest < 100,
          "six starts and stops, each stop back in " + num(longest) + " ms at most -- not up to " +
              num(MobDetector::kLookGapMs) + " ms of a sleep, on the thread that reads F12");
}

// ---- PrintWindow ----

using Window = PrintWindowVerdicts::Window;

Window window_at(std::uintptr_t handle, std::uint32_t process) {
    return Window{ reinterpret_cast<const void*>(handle), process };
}

/// One grab of `w` at `now` as ScreenCapture::grab drives its PrintWindowAsk:
/// whether PrintWindow was asked. When it was, its picture `drew`, or was
/// blank with the screen showing the window drawn (`drawn`) or not -- covered,
/// or one flat colour; `back` and `gave_up` get what grab() says in the log.
bool grab_at(PrintWindowAsk& a, const Window& w, std::uint64_t now, bool drew, bool drawn, bool* back = nullptr,
             bool* gave_up = nullptr) {
    a.attach(w, now);
    if (!a.ask()) return false;
    const bool came_back = a.printed(drew);
    if (back != nullptr) *back = came_back;
    if (!drew && a.trying()) {
        const bool given = a.screen(drawn);
        if (gave_up != nullptr) *gave_up = given;
    }
    return true;
}

void test_print_window_verdicts() {
    section("PrintWindow: one verdict per window for the whole process (PrintWindowVerdicts)");
    const Window game = window_at(0x1000, 42);
    const Window other_program = window_at(0x1000, 43);
    constexpr std::uint64_t t0 = 10'000'000;
    constexpr std::uint64_t kProbe = PrintWindowVerdicts::kReprobeMs;
    {
        PrintWindowVerdicts v;
        const PrintWindowVerdicts::Turn first = v.turn(game, t0);
        check(first.ask && !first.refused && !v.refused(game), "a window never judged is asked");
        const bool said = v.refuse(game, t0);
        const bool said_again = v.refuse(game, t0 + 10);
        check(said && !said_again && v.refused(game), "refused for the whole process, and said once");
        const PrintWindowVerdicts::Turn soon = v.turn(game, t0 + 1000);
        const PrintWindowVerdicts::Turn before = v.turn(game, t0 + kProbe - 1);
        const PrintWindowVerdicts::Turn due = v.turn(game, t0 + kProbe);
        const PrintWindowVerdicts::Turn same_time = v.turn(game, t0 + kProbe);
        const PrintWindowVerdicts::Turn next = v.turn(game, t0 + 2 * kProbe);
        check(!soon.ask && soon.refused && !before.ask && due.ask && due.refused && !same_time.ask && next.ask,
              "refused: asked again 5 minutes on, by one capture of them all, and 5 minutes after that");
        const PrintWindowVerdicts::Turn elsewhere = v.turn(other_program, t0 + 1);
        check(elsewhere.ask && !elsewhere.refused && !v.refused(other_program),
              "the same handle in another program is another window");
        const bool back = v.drew(game, t0 + 2 * kProbe + 1);
        const bool back_again = v.drew(game, t0 + 2 * kProbe + 2);
        const PrintWindowVerdicts::Turn tried = v.turn(game, t0 + 2 * kProbe + 5);
        check(back && !back_again && !v.refused(game) && tried.ask && !tried.refused,
              "a probe that draws: tried again everywhere, and said once");
    }
    {
        PrintWindowVerdicts v;
        v.set_light(true);
        v.refuse(game, t0);
        int probes = 0;
        for (std::uint64_t t = t0; t <= t0 + 3'600'000; t += 150)
            if (v.turn(game, t).ask) ++probes;
        const Window fresh = window_at(0x2000, 42);
        const bool fresh_asked = v.turn(fresh, t0).ask;
        check(probes == 0 && fresh_asked,
              "light mode: a window this session saw refuse it is never asked again -- " + num(probes) +
                  " times in an hour -- and one never judged is asked as ever");
    }
    {
        PrintWindowVerdicts v;
        const bool first = v.turn(game, t0).ask;
        v.unclear(game, t0);
        const bool soon = v.turn(game, t0 + PrintWindowVerdicts::kUnclearAskMs - 1).ask;
        const bool due = v.turn(game, t0 + PrintWindowVerdicts::kUnclearAskMs).ask;
        const bool same_time = v.turn(game, t0 + PrintWindowVerdicts::kUnclearAskMs).ask;
        v.judged(game);
        const bool after = v.turn(game, t0 + PrintWindowVerdicts::kUnclearAskMs + 1).ask;
        const bool next = v.turn(game, t0 + PrintWindowVerdicts::kUnclearAskMs + 2).ask;
        check(first && !soon && due && !same_time && after && next && !v.refused(game),
              "nothing to judge by -- covered, or one flat colour: asked again 3 s on, by one capture; a picture "
              "judged has every grab ask again");
    }
    {
        // A loading screen with the game covered: PrintWindow drew it a moment
        // ago, so its black frame is the game's own, and every grab asks --
        // the first after the game draws again has its picture, not the copy
        // of the window covering it.
        PrintWindowVerdicts v;
        v.turn(game, t0);
        v.drew(game, t0);
        v.unclear(game, t0 + 100);
        const bool next = v.turn(game, t0 + 200).ask;
        const bool again = v.turn(game, t0 + 201).ask;
        v.unclear(game, t0 + 202);
        const bool still = v.turn(game, t0 + PrintWindowVerdicts::kDrewTrustMs - 1).ask;
        check(next && again && still,
              "drawn by PrintWindow within the last 30 s: a blank picture with nothing to judge it by is asked again "
              "at every grab, as before the 3 s gate");
    }
    {
        PrintWindowVerdicts v;
        v.turn(game, t0);
        v.drew(game, t0);
        const std::uint64_t late = t0 + PrintWindowVerdicts::kDrewTrustMs + 100;
        v.unclear(game, late);
        const bool soon = v.turn(game, late + 1).ask;
        const bool due = v.turn(game, late + PrintWindowVerdicts::kUnclearAskMs).ask;
        check(!soon && due,
              "drawn longer than 30 s ago and blank and covered since: held to one ask in 3 s again");
    }
    {
        PrintWindowVerdicts v;
        const Window oldest = window_at(0x10000, 7);
        v.refuse(oldest, 1);
        for (std::uintptr_t i = 1; i <= PrintWindowVerdicts::kMaxWindows; ++i)
            v.turn(window_at(0x10000 + i * 16, 7), 1 + i);
        check(v.windows() == PrintWindowVerdicts::kMaxWindows && !v.refused(oldest),
              "64 windows kept at most: the one longest unasked forgotten -- " + num(static_cast<long long>(v.windows())));
    }
}

void test_print_window_ask() {
    section("PrintWindow, grab by grab: each capture's count inside the process's verdict (PrintWindowAsk)");
    const Window game = window_at(0x1000, 42);
    constexpr std::uint64_t t0 = 20'000'000;

    // Before any window is attached: PrintWindowRule, as it was.
    {
        PrintWindowAsk alone;
        bool gave[3] = {};
        for (bool& g : gave) {
            const bool asked = alone.ask();
            alone.printed(false);
            g = asked && alone.trying() && alone.screen(true);
        }
        int asked_at = 0;
        for (int i = 1; i <= PrintWindowRule::kProbeEvery + 1 && asked_at == 0; ++i)
            if (alone.ask()) asked_at = i;
        check(!gave[0] && !gave[1] && gave[2] && !alone.trying() && asked_at == PrintWindowRule::kProbeEvery,
              "with no window attached it is the rule alone: given up at the third blank picture, asked at the 60th "
              "grab");
    }

    // The vitals reader's capture gives it up; the detector's, and a shop's
    // made afterwards, go by that.
    PrintWindowVerdicts v;
    PrintWindowAsk vitals, detector, shop;
    vitals.set_verdicts(&v);
    detector.set_verdicts(&v);
    shop.set_verdicts(&v);
    bool gave_up[3] = {};
    for (int i = 0; i < 3; ++i) grab_at(vitals, game, t0 + 150 * static_cast<std::uint64_t>(i), false, true, nullptr,
                                        &gave_up[i]);
    check(!gave_up[0] && !gave_up[1] && gave_up[2] && v.refused(game),
          "three blank pictures of a window the screen shows drawn give PrintWindow up -- for the whole process, "
          "at the third");
    const bool detector_asked = grab_at(detector, game, t0 + 500, false, true);
    const bool shop_asked = grab_at(shop, game, t0 + 600, false, true);
    check(!detector_asked && !shop_asked && !shop.trying() && !detector.trying(),
          "every other capture of it goes by that, one made afterwards too: not asked, not tried");

    // An hour of the vitals reader's and the detector's grabs.
    const auto hour = [&](PrintWindowVerdicts& verdicts, PrintWindowAsk& a, PrintWindowAsk& b) {
        int asks = 0;
        for (std::uint64_t t = 1000; t <= 1000 + 3'600'000; t += 10) {
            if (t % 150 == 0 && grab_at(a, game, t0 + t, false, true)) ++asks;
            if (t % 140 == 0 && grab_at(b, game, t0 + t, false, true)) ++asks;
        }
        (void)verdicts;
        return asks;
    };
    const int probes = hour(v, vitals, detector);
    check(probes >= 11 && probes <= 12 && v.refused(game),
          "refused, an hour of grabs at 150 and 140 ms asks it " + num(probes) +
              " times -- once in 5 minutes for the whole process, not once in 60 grabs of each (about 830)");

    {
        PrintWindowVerdicts light;
        light.set_light(true);
        PrintWindowAsk a, b;
        a.set_verdicts(&light);
        b.set_verdicts(&light);
        for (int i = 0; i < 3; ++i) grab_at(a, game, t0 + 150 * static_cast<std::uint64_t>(i), false, true);
        const int asks = hour(light, a, b);
        check(light.refused(game) && asks == 0,
              "light mode: refused this session, it is not asked again -- " + num(asks) + " times in an hour");
    }

    // A probe that draws: every capture reads the window's own picture again.
    bool back = false;
    const std::uint64_t later = t0 + 5'000'000;
    const bool probed = grab_at(shop, game, later, true, false, &back);
    const bool detector_again = grab_at(detector, game, later + 140, false, true);
    const bool vitals_again = grab_at(vitals, game, later + 150, false, true);
    check(probed && back && !v.refused(game) && detector_again && vitals_again && vitals.trying(),
          "a probe that draws has it back for every capture, the one that gave it up included");

    // The game covered by another window: a minute of three captures' grabs.
    {
        PrintWindowVerdicts covered;
        PrintWindowAsk a, b, c;
        a.set_verdicts(&covered);
        b.set_verdicts(&covered);
        c.set_verdicts(&covered);
        int asks = 0, grabs = 0;
        for (std::uint64_t t = 0; t <= 60'000; t += 10) {
            if (t % 150 == 0) {
                ++grabs;
                if (grab_at(a, game, t0 + t, false, false)) ++asks;
            }
            if (t % 140 == 0) {
                ++grabs;
                if (grab_at(b, game, t0 + t, false, false)) ++asks;
            }
            if (t % 1000 == 0) {
                ++grabs;
                if (grab_at(c, game, t0 + t, false, false)) ++asks;
            }
        }
        check(asks >= 20 && asks <= 21 && !covered.refused(game),
              "the game covered for a minute, " + num(grabs) + " grabs by three captures: PrintWindow asked " +
                  num(asks) + " times, once in 3 s, not at every grab -- and not given up on nothing");
        // Seen drawn again: the next ask judges it, and every grab asks.
        bool gave = false;
        int asked = 0;
        for (std::uint64_t t = 63'000; t <= 64'000 && !gave; t += 10) {
            bool g = false;
            if (t % 150 == 0 && grab_at(a, game, t0 + t, false, true, nullptr, &g)) ++asked;
            gave = gave || g;
        }
        check(gave && asked == 3 && covered.refused(game),
              "seen drawn again, three grabs running ask it, and give it up -- " + num(asked));
    }

    // A loading screen with the game covered, PrintWindow drawing it before
    // and after: every grab asks through the black, and the first one after
    // has the game's own picture rather than the window covering it.
    {
        PrintWindowVerdicts loading;
        PrintWindowAsk a;
        a.set_verdicts(&loading);
        grab_at(a, game, t0, true, false);
        int asks = 0, grabs = 0;
        for (std::uint64_t t = 150; t <= 5000; t += 150) {
            ++grabs;
            if (grab_at(a, game, t0 + t, false, false)) ++asks;
        }
        const bool after = grab_at(a, game, t0 + 5100, true, false);
        check(asks == grabs && after && !loading.refused(game) && a.trying(),
              "a loading screen with the game covered, PrintWindow having drawn it: " + num(asks) + " of " +
                  num(grabs) + " grabs ask it, and the first grab after the black has the game's picture");
    }

    // One capture looking at two windows in turn counts each afresh.
    {
        PrintWindowVerdicts both;
        PrintWindowAsk login;
        login.set_verdicts(&both);
        const Window zone = window_at(0x5000, 77);
        const Window launcher = window_at(0x6000, 78);
        grab_at(login, zone, t0, false, true);
        grab_at(login, zone, t0 + 100, false, true);
        grab_at(login, launcher, t0 + 200, false, true);
        grab_at(login, zone, t0 + 300, false, true);
        check(!both.refused(zone) && !both.refused(launcher),
              "a capture that looks at another window counts that window's blank pictures from nothing: two, one "
              "and one give neither up");
    }
}

// ---- the wiring, read off the source ----

#ifdef GOBOT_SOURCE_DIR
std::string source(const std::string& rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// A function's definition in `src`, from `head` to the brace that closes it
/// -- the first at the start of a line -- or "": main.cpp's body_of.
std::string body(const std::string& src, const std::string& head) {
    const std::size_t from = src.find(head);
    if (from == std::string::npos) return {};
    const std::size_t end = src.find("\n}", from);
    return end == std::string::npos ? std::string() : src.substr(from, end + 2 - from);
}

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

/// Whether `first` comes before `then` in `text`, both there.
bool before(const std::string& text, const char* first, const char* then) {
    const std::size_t a = text.find(first);
    const std::size_t b = text.find(then);
    return a != std::string::npos && b != std::string::npos && a < b;
}
#endif

void test_capture_wiring() {
    section("Watching the game wired in: the readers' waits, the detector with the hunt, the pace before each "
            "start, PrintWindow's verdict, the camera, the frames kept (their source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string vitals = source("/src/vitals_reader.cpp");
    const std::string mobs = source("/src/mob_detector.cpp");
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    if (vitals.empty() || mobs.empty() || gui.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const auto npos = std::string::npos;

    // The readers wait on what stop() wakes, and publish their frame by a swap.
    const std::string vloop = body(vitals, "void VitalsReader::loop() {");
    const std::string vstop = body(vitals, "void VitalsReader::stop() {");
    const std::string vset = body(vitals, "void VitalsReader::set_period_ms(int ms) {");
    check(has(vloop, "wake_.wait_for(") && !has(vloop, "sleep_for") && has(vstop, "wake_.notify_all();") &&
              has(vset, "wake_.notify_all();") && has(vloop, "std::swap(frame_, spare);") &&
              !has(vloop, "frame_ = frame;") && has(vloop, ">= forget_at") && !has(vitals, "kForgetAfterMisses"),
          "vitals_reader.cpp: waits for its pace, woken by stop() and a shorter pace; its frame swapped in, not "
          "copied; the figures forgotten by forget_after_misses");
    const std::string mloop = body(mobs, "void MobDetector::loop() {");
    const std::string mstop = body(mobs, "void MobDetector::stop() {");
    check(has(mloop, "wake_.wait_for(") && !has(mloop, "sleep_for") && has(mstop, "wake_.notify_all();") &&
              has(mloop, "std::swap(frame_, frame);") && !has(mloop, "frame_ = frame;"),
          "mob_detector.cpp: waits between looks so that stop() wakes it; its frame swapped in, not copied");

    // The detector with the hunt, once, never through a call of itself.
    const std::string stop_hunt = body(gui, "void GuiApp::stop_hunt() {");
    const std::string stop_det = body(gui, "void GuiApp::stop_detector() {");
    const std::string release = body(gui, "void GuiApp::release_detector() {");
    const std::string wanted = body(gui, "bool GuiApp::detector_wanted() const {");
    const std::string tick = body(gui, "void GuiApp::tick() {");
    check(has(stop_hunt, "release_detector();") && !has(stop_hunt, "stop_position_capture();") &&
              before(stop_det, "detector_running_ = false;", "stop_hunt();") &&
              has(release, "if (!detector_running_ || detector_wanted()) return;") &&
              has(wanted, "hunting_") && has(wanted, "monster_look_running()") && has(wanted, "jump_test_running()") &&
              has(wanted, "navigator_->running()") && has(wanted, "detector_by_player_") &&
              has(wanted, "camera_panels_open()"),
          "gui_app.cpp: the hunt's end lets the detector go unless a look, a Thử, a walk or the Radar still uses it; "
          "stop_detector marks it stopped before it ends the hunt");
    const std::size_t ended = tick.find("if (hunting_ && hunter_ && !hunter_->running()) {");
    const std::size_t let_go = ended == npos ? npos : tick.find("release_detector();", ended);
    check(ended != npos && let_go != npos && let_go - ended < 300,
          "tick(): a hunter that ended by itself lets the detector go as well");
    const std::string style = source("/apps/gobot_gui/simple_style.cpp");
    check(has(body(style, "void GuiApp::simple_style_tick() {"), "release_detector();") &&
              has(style, "bool GuiApp::jump_test_running() const { return style_ui_ && style_ui_->jump.valid(); }"),
          "simple_style.cpp: a Thử under way keeps the detector, whoever started it, and lets it go when it ends");

    // The pace: every pass, and at the top of every start.
    const std::string pace = body(source("/apps/gobot_gui/packets_vitals.cpp"), "int GuiApp::vitals_pace() const {");
    check(has(tick, "vitals_.set_period_ms(vitals_pace());") && has(pace, "any_job_alive()") &&
              has(pace, "vitals_run_period()") && has(pace, "performance_.vitals_idle_ms"),
          "tick(): HP and MP at the run's pace while a job is alive, at vitals_idle_ms otherwise (vitals_pace)");
    check(before(tick, "const std::uint64_t read_at = last >= vitals_.paced_from_ms() ? last : 0, at = now_ms();",
                 "macro_->set_vitals(VitalsReader::if_fresh(vitals_.hp_percent(), read_at"),
          "tick(): a read taken at the idle pace before a job started is handed to the macro as no reading");
    const std::string quest = source("/apps/gobot_gui/simple_quest.cpp");
    const std::string popups = source("/apps/gobot_gui/simple_popups.cpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    const struct { const std::string* file; const char* head; const char* starts; } kStarts[] = {
        { &gui, "void GuiApp::start_macro() {", "macro_->start();" },
        { &gui, "void GuiApp::start_hunt() {", "hunter_->start();" },
        { &gui, "void GuiApp::start_party() {", "std::thread(" },
        { &gui, "void GuiApp::start_keeper(bool oneshot) {", "std::thread(" },
        { &gui, "void GuiApp::start_revive() {", "std::thread(" },
        { &gui, "void GuiApp::start_trash() {", "std::thread(" },
        { &gui, "void GuiApp::request_trash(bool look_only) {", "std::thread(" },
        { &gui, "void GuiApp::request_charms() {", "std::thread(" },
        { &gui, "void GuiApp::request_shop(bool dry) {", "std::thread(" },
        { &gui, "void GuiApp::start_chat() {", "std::thread(" },
        { &gui, "void GuiApp::request_chat(bool send_one) {", "std::thread(" },
        { &quest, "void GuiApp::start_quest() {", "std::thread(" },
        { &quest, "void GuiApp::request_quest(bool look_only) {", "std::thread(" },
        { &popups, "void GuiApp::start_popups() {", "std::thread(" },
        { &view, "void GuiApp::simple_start_jobs() {", "start_macro();" },
    };
    std::string late;
    for (const auto& s : kStarts)
        if (!before(body(*s.file, s.head), "vitals_for_job();", s.starts)) late += std::string(" ") + s.head;
    check(late.empty(), "every start asks for the run's pace before its thread or engine can press a key" +
                            (late.empty() ? std::string() : " -- not:" + late));
    const std::string any = body(gui, "bool GuiApp::any_job_alive() const {");
    bool all_jobs = !any.empty();
    for (const char* job : { "simple_running()", "macro_running_", "hunting_", "party_running_", "revive_running_",
                             "popups_running()", "keeper_running_", "trash_running_", "chat_running_",
                             "quest_running_", "simple_find_bars_", "full_find_bars_", "coord_.critical_active()",
                             "bot_running_",
                             "navigator_->running()", "jump_test_running()" })
        all_jobs = all_jobs && has(any, job);
    check(all_jobs, "any_job_alive: every job and one-shot pass, Tự tìm and a critical hold");
    const std::size_t click = view.find("if (ImGui::Button(simple_find_bars_ ? \"Đang tìm…\" : \"Tự tìm\")) {");
    const std::size_t paced = click == npos ? npos : view.find("vitals_for_job();", click);
    const std::size_t finding = click == npos ? npos : view.find("simple_find_bars_ = true;", click);
    check(paced != npos && finding != npos && paced < finding &&
              has(view, "vitals_.last_read_ms() >= simple_find_bars_since_ms_"),
          "simple_view.cpp: Tự tìm asks for a read at once, and searches a frame read after the click");
    const std::size_t full_click = gui.find("if (ImGui::Button(full_find_bars_ ? \"FINDING...\" : \"FIND THE BARS\"");
    const std::size_t full_paced = full_click == npos ? npos : gui.find("vitals_for_job();", full_click);
    const std::size_t full_finding = full_click == npos ? npos : gui.find("full_find_bars_ = true;", full_click);
    check(full_paced != npos && full_finding != npos && full_paced < full_finding &&
              has(body(gui, "void GuiApp::full_find_bars_tick() {"),
                  "vitals_.last_read_ms() >= full_find_bars_since_ms_") &&
              has(tick, "full_find_bars_tick();"),
          "gui_app.cpp: the full screen's FIND THE BARS likewise, searched from tick() so that it ends though the tab "
          "is left");
    check(has(source("/apps/gobot_gui/simple_potions.cpp"), "potion_pair_gap_ms(vitals_.period_ms())"),
          "simple_potions.cpp: a potion look's second frame one read later at whatever pace");

    // PrintWindow: the window attached before the pinned line, light mode handed on.
    const std::string cap = source("/src/screen_capture.cpp");
    const std::string grab = body(cap, "bool ScreenCapture::grab(void* hwnd_raw, Frame& out) {");
    check(before(grab, "print_rule_.attach(", "if (!screen_only_ && !PrintWindowPause::active() && print_rule_.ask()) {") &&
              grab.find("= 600;") == npos && has(cap, "if (covered_whole(root, origin, w, h)) return false;"),
          "screen_capture.cpp: each grab tells its rule which window it is before asking it; a client area covered "
          "whole by one window is said so with one look-up");
    check(has(body(gui, "void GuiApp::load_performance(const IniFile& app_ini) {"),
              "print_window_verdicts().set_light(light_.on);") &&
              has(body(view, "void GuiApp::set_light_mode(bool on) {"), "print_window_verdicts().set_light(light_.on);"),
          "light mode handed to the PrintWindow verdicts at the start and at every tick of its box");

    // The camera tracker only while something uses it.
    const std::string start_det = body(gui, "void GuiApp::start_detector() {");
    check(has(start_det, "detector_.set_camera(camera_fed_ ? &camera_ : nullptr);") &&
              !has(start_det, "detector_.set_camera(&camera_);") &&
              has(tick, "if (feed) camera_.reset();") && has(tick, "detector_.set_camera(feed ? &camera_ : nullptr);") &&
              has(body(gui, "bool GuiApp::camera_wanted() const {"), "navigator_->running()"),
          "gui_app.cpp: the camera fed only for a walk or the full screen's Radar and movement panels, afresh when "
          "it starts");

    // The frames grabbed into again.
    const std::string party = body(gui, "void GuiApp::party_loop() {");
    check(before(party, "PartyLook look;", "while (!party_stop_) {") &&
              party.find("PartyLook look;", party.find("while (!party_stop_) {")) == npos &&
              has(source("/src/party_hud.cpp"), "out.frame.pixels = std::move(buffer);"),
          "party_loop: one look kept across its passes, its buffer kept by take_party_look");
    const std::string dialog_wait = body(source("/src/bag.cpp"), "bag::Dialog BagCleaner::wait_for_dialog(");
    check(has(source("/src/reviver.cpp"), "Frame& frame = turn_frame_;") &&
              has(source("/src/popup_closer.cpp"), "Frame& f = turn_frame_;") &&
              before(dialog_wait, "Frame f;", "while (ms_between(from, now()) < ms) {") &&
              has(dialog_wait, "std::swap(*last, f);") &&
              !has(dialog_wait, "*last = std::move(f);"),
          "the revive's and the popups' turns, and the bin's wait for its dialog, grab into the same frame each time");
#else
    skip("no source folder\n");
#endif
}

// ---- the detector resting while the packets steer ----

void test_detector_pace() {
    section("Mob detector: the wait between looks asked for, a shorter one at once (set_period_ms) -- it rests "
            "while the packets steer");
    {
        MobDetector idle;
        const int first = idle.period_ms();
        idle.set_period_ms(1);
        const int low = idle.period_ms();
        idle.set_period_ms(10'000'000);
        const int high = idle.period_ms();
        check(first == MobDetector::kLookGapMs && low == MobDetector::kMinPeriodMs &&
                  high == MobDetector::kMaxPeriodMs,
              "140 ms until asked otherwise, as it always was; held to 50..5000 ms");
    }

    // The test's own frames, which are none: every look's grab fails, and is
    // counted. The window is only a number.
    void* const window = reinterpret_cast<void*>(std::uintptr_t{ 0x5000 });
    std::atomic<int> grabs{ 0 };
    MobDetector d;
    d.set_grab([&grabs](void*, Frame&) {
        grabs.fetch_add(1);
        return false;
    });
    d.set_target_window(window);
    d.set_period_ms(3000);
    d.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const int waiting = grabs.load();
    // GoBot's window asks for the pace on every pass of its loop.
    for (int i = 0; i < 10; ++i) {
        d.set_period_ms(3000);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const int same = grabs.load();
    check(waiting == 0 && same == 0,
          "3 s between looks: nothing looked at in the first half second, and the same wait asked again every pass "
          "wakes nothing -- " + num(same) + " looks");

    const Clock::time_point asked = Clock::now();
    d.set_period_ms(MobDetector::kLookGapMs);
    while (grabs.load() == 0 && ms_since(asked) < 3000) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const long long took = ms_since(asked);
    const int woke = grabs.load();
    check(woke > 0 && took < 1000,
          "its own 140 ms asked for -- the screen steering again -- looks at once, not when the 3 s wait would have "
          "ended: " + num(took) + " ms");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    const int later = grabs.load();
    check(later - woke >= 2, "and looks on at its own pace -- " + num(later - woke) + " looks in 0.7 s at 140 ms");

    d.set_period_ms(3000);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const Clock::time_point stopping = Clock::now();
    d.stop();
    const long long stop_ms = ms_since(stopping);
    check(!d.running() && stop_ms < 1000,
          "stopped in the middle of a 3 s wait: back at once, not at its end -- " + num(stop_ms) + " ms");
}

void test_detector_pace_wiring() {
    section("Detector rest wired in: decided at every tick, a shorter wait waking the detector, and nothing that "
            "waits on GoBot's thread");
#ifdef GOBOT_SOURCE_DIR
    const std::string mobs = source("/src/mob_detector.cpp");
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string paced = source("/apps/gobot_gui/packets_detector.cpp");
    const std::string eyes = source("/src/packet_eyes.cpp");
    if (mobs.empty() || gui.empty() || paced.empty() || eyes.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }

    // The detector: its loop waits for its period, woken by a shorter one.
    const std::string mloop = body(mobs, "void MobDetector::loop() {");
    const std::string mset = body(mobs, "void MobDetector::set_period_ms(int ms) {");
    check(has(mloop, "wake_.wait_for(") && has(mloop, "period_ms_.load()") && has(mloop, "wake_now_") &&
              !has(mloop, "sleep_for") && has(mset, "if (ms >= was) return;") && has(mset, "wake_now_ = true;") &&
              has(mset, "wake_.notify_all();"),
          "mob_detector.cpp: the loop waits period_ms_, cut short by stop() and by a shorter period asked for");

    // GoBot's window: every tick, from what the hunt and the eyes say.
    const std::string tick = body(gui, "void GuiApp::tick() {");
    const std::string pace = body(paced, "void GuiApp::detector_pace_tick(");
    bool all = !pace.empty();
    for (const char* what : { "detector_rest(", "detector_.set_period_ms(", "hunting_", "packet_eyes_->source()",
                              "packet_eyes_->why()", "jump_test_running()", "monster_look_running()",
                              "camera_panels_open()", "navigator_->running()", "packets_settings_.detector_rest",
                              "detector_running_", "Detector: every %d ms while the packets steer" })
        all = all && has(pace, what);
    check(has(tick, "detector_pace_tick(") && all,
          "tick(): the detector's pace decided at every tick, from the hunt, the eyes, Thử, Quái đang thấy, the "
          "panels, a walk and [Packets] detector_rest");
    check(!has(paced, "light_") && !has(paced, "sleep_for") && !has(paced, "Sleep(") && !has(paced, "capture_.") &&
              !has(paced, "wait_idle"),
          "packets_detector.cpp: nothing that waits, and nothing of the capture, on GoBot's thread");
    check(!has(body(gui, "void GuiApp::stop_hunt() {"), "set_period_ms") &&
              !has(body(gui, "void GuiApp::start_detector() {"), "set_period_ms"),
          "neither the hunt's end nor the detector's start sets its pace: the next tick does");

    // The hand-over in PacketEyes.
    const std::string frame = body(eyes, "std::uint64_t PacketEyes::frame() {");
    check(has(frame, "age(picture_new_ms_, now) > fresh") && has(frame, "picture_gap_ms_ > kScreenFreshMs") &&
              has(frame, "MobDetector::kLookGapMs") && has(frame, "handing_over_ = true;") &&
              has(frame, "pictures > handover_pictures_ + 1") && has(frame, "held >= kHandOverMs"),
          "packet_eyes.cpp: back to the screen with an old picture -- older than one at the detector's own pace "
          "while it rests -- the packets' plates kept until the detector's count is two on, or 300 ms");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_capture_load(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_counted_reads();
    test_run_pace();
    test_vitals_pace();
    test_detector_stops_at_once();
    test_print_window_verdicts();
    test_print_window_ask();
    test_capture_wiring();
    test_detector_pace();
    test_detector_pace_wiring();
}
