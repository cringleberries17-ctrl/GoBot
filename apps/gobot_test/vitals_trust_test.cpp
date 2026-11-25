// gobot_test -- your HP and MP from the packets (the packets plan, item D):
// when a bar's packet value goes (vitals_usable) -- since 2026.11.17 whenever
// the packets have it, no check against the bars first -- the bars' figures
// set beside it for the log (BarCompare), when the HP/MP reader may rest, how
// long HP may go unread while it does (PeriodWindow, world_window_ms), the
// hold while a map loads (map_load_holds), the reader's figures under the EXP
// bar's glowing tip -- and where GoBot's window wires them in, read off its
// source.
//
// Pure: every time is the test's own, on a clock that starts at kT0; nothing
// here goes near the game, a window, the network or the registry -- the HP/MP
// reader's thread, where it runs, reads frames the test draws (set_grab). HP
// 9232 and MP 8902 are the P0-2 recording's; HP 10084, MP 15280 the farm
// log's of 2026-11-03; MP 16813 and HP 15625 the owner's of 2026-11-17; every
// other number is made up.

#include "gobot/config.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/macro.hpp"
#include "gobot/template_matcher.hpp"
#include "gobot/vitals_reader.hpp"
#include "gobot/vitals_trust.hpp"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

std::string num(long long v) { return std::to_string(v); }

constexpr std::uint64_t kT0 = 7'000'000;     // where the test's clock starts

// ------------------------------------------------ the bars beside the packets

using Verdict = BarCompare::Verdict;

/// A read with its figures, at `at`.
BarCompare::Read figures_at(std::uint64_t at, int figure, int max) {
    BarCompare::Read r;
    r.read_at_ms = at;
    r.figure = figure;
    r.figure_max = max;
    return r;
}

void test_compare_one() {
    section("Vitals log: one packet value against one read's figures -- same, short of digits, or other");
    const BarCompare::Point p{ kT0, 15542, 16813, true, 1 };
    const BarCompare::Point unknown{ kT0, 15542, 16813, false, 1 };
    check(BarCompare::compare(p, figures_at(kT0, 15542, 16813)) == Verdict::Same &&
              BarCompare::compare(unknown, figures_at(kT0, 15542, 99999)) == Verdict::Same,
          "the figures the packet value and its maximum: same; a maximum the packets do not know is not held "
          "against them");
    check(BarCompare::compare(p, figures_at(kT0, 542, 16813)) == Verdict::Short &&
              BarCompare::compare(p, figures_at(kT0, 5542, 16813)) == Verdict::Short &&
              BarCompare::compare(p, figures_at(kT0, 1542, 16813)) == Verdict::Short &&
              BarCompare::compare(p, figures_at(kT0, 15542, 6813)) == Verdict::Short,
          "542 and 5542 for 15542 (its first digits under the EXP bar's tip), 1542 (one lost), 6813 for the "
          "maximum: short -- the reader's miss");
    check(BarCompare::compare(p, figures_at(kT0, 4215, 16813)) == Verdict::Other &&
              BarCompare::compare(p, figures_at(kT0, 15542, 16000)) == Verdict::Other &&
              BarCompare::compare(p, figures_at(kT0, 543, 16000)) == Verdict::Other,
          "4215 (the digit beside the tip misread too), another maximum: other");
}

void test_compare_window() {
    section("Vitals log: a read set beside every value around it -- the HUD ahead of 0x0051 -- of this epoch");
    BarCompare b;
    // The skills in turn: MP every 300 ms, the HUD 100 ms ahead of 0x0051 --
    // every read the same as a value around it, MP never still.
    const std::uint32_t max = 16813;
    const auto value_at = [](std::uint64_t at) {
        return 16813u - static_cast<std::uint32_t>(((at - kT0) / 300) % 8) * 437u;
    };
    std::uint64_t t = kT0;
    for (const std::uint64_t until = t + 10'000; t < until; t += 50) {
        if ((t - kT0) % 300 == 0) b.packet(BarCompare::Point{ t, value_at(t), max, true, 1 });
        if ((t - kT0) % 150 == 0) b.screen(figures_at(t, static_cast<int>(value_at(t + 100)), static_cast<int>(max)));
        b.settle(t, 1);
    }
    b.settle(t + BarCompare::kLeadMs + BarCompare::kJudgeSlackMs, 1);
    const BarCompare::Counts c = b.take_counts();
    check(c.same >= 60 && c.short_digits == 0 && c.other == 0 && b.take_odd().empty(),
          "MP moving every 300 ms, the bars read every 150 ms: every read the same as a value around it -- same " +
              num(static_cast<long long>(c.same)));

    // The owner's run: the HUD's 16811 read 811 under the tip, 0x0051 still
    // 16813, 0x02F0 16811 -- now a point of its own.
    {
        BarCompare m;
        m.packet(BarCompare::Point{ kT0, 16813, max, true, 1 });
        m.packet(BarCompare::Point{ kT0 + 800, 16811, max, true, 1 });
        m.screen(figures_at(kT0 + 1000, 811, static_cast<int>(max)));
        m.screen(figures_at(kT0 + 1001, 4215, static_cast<int>(max)));
        m.settle(kT0 + 1001 + BarCompare::kLeadMs, 1);
        const BarCompare::Counts early = m.take_counts();
        m.settle(kT0 + 1001 + BarCompare::kLeadMs + BarCompare::kJudgeSlackMs, 1);
        const BarCompare::Counts due = m.take_counts();
        const std::vector<BarCompare::Odd> odd = m.take_odd();
        check(early.same + early.short_digits + early.other == 0 && due.short_digits == 1 && due.other == 1 &&
                  odd.size() == 2 && odd[0].verdict == Verdict::Short && odd[0].packet.value == 16811 &&
                  odd[1].verdict == Verdict::Other && odd[1].read.figure == 4215,
              "811 against 16811 (short, the reader's miss), 4215 (other) -- once kLeadMs and its slack are past, "
              "not before; each kept for the log with the value it came nearest");
    }
    // A value of another epoch, or none at all: nothing to set it beside.
    {
        BarCompare m;
        m.packet(BarCompare::Point{ kT0, 9000, 9232, true, 1 });
        m.screen(figures_at(kT0 + 1000, 9000, 9232));
        m.screen(BarCompare::Read{ kT0 + 1100, -1, -1 });
        m.settle(kT0 + 5000, 2);
        BarCompare none;
        none.screen(figures_at(kT0 + 1000, 9000, 9232));
        none.settle(kT0 + 5000, 1);
        const BarCompare::Counts c2 = m.take_counts();
        const BarCompare::Counts c3 = none.take_counts();
        check(c2.no_packet == 1 && c2.same == 0 && c3.no_packet == 1,
              "a value from before a gap, or no value at all: not compared; a read without figures not kept");
    }
}

// ------------------------------------------------------------ the caption's shadow

void test_caption_shadow() {
    section("Vitals reader: no figures in the caption's shadow (54% and up) -- the kept ones say how much more");
    const float in = VitalsReader::shadowed_percent(56.0f, 12643, 15280);
    check(in > 82.0f && in < 83.0f && VitalsReader::shadowed_percent(40.0f, 12643, 15280) == 40.0f &&
              VitalsReader::shadowed_percent(56.0f, -1, -1) == 56.0f &&
              VitalsReader::shadowed_percent(60.0f, 8982, 15280) == 60.0f,
          "the macro gets 82.7%, not 56% -- no MP potion under 59%; 40% stands; no figures kept, or lower ones: the "
          "measure stands");
    check(VitalsReader::shadowed_percent(-1.0f, 12643, 15280) < 0.0f &&
              VitalsReader::shadowed_percent(53.9f, 12643, 15280) == 53.9f &&
              VitalsReader::shadowed_percent(54.0f, 12643, 0) == 54.0f &&
              VitalsReader::shadowed_percent(54.0f, 12643, 15280) > 82.0f,
          "no bar stays no bar; 53.9% stands, 54% is in the shadow; no maximum kept: the measure");
}

/// A frame of the test's own, `w` x `h`, all one colour.
Frame flat_frame(int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    Frame f;
    f.width = w;
    f.height = h;
    f.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    for (std::size_t i = 0; i < f.pixels.size(); i += 4) {
        f.pixels[i] = b;
        f.pixels[i + 1] = g;
        f.pixels[i + 2] = r;
        f.pixels[i + 3] = 255;
    }
    return f;
}

void put(Frame& f, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (x < 0 || y < 0 || x >= f.width || y >= f.height) return;
    const std::size_t i =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(f.width) + static_cast<std::size_t>(x)) * 4;
    f.pixels[i] = b;
    f.pixels[i + 1] = g;
    f.pixels[i + 2] = r;
}

/// `bar` painted into `f`, as test_glyph_font paints one: `filled` columns of
/// its fill colour, a dark trough after them, three rows over and under; and
/// `caption` -- "" none -- right-aligned across it, as the game prints it.
void paint(Frame& f, const BarSpec& bar, int filled, const GlyphFont& font, const char* caption) {
    for (int y = bar.y - 3; y < bar.y + bar.height + 3; ++y)
        for (int x = bar.x; x < bar.x + bar.width; ++x) {
            if (x < bar.x + filled)
                put(f, x, y, bar.r, bar.g, bar.b);
            else
                put(f, x, y, 18, 0, 18);
        }
    Frame drawn;
    if (*caption == '\0' || !font.render_line(caption, drawn)) return;
    const ScreenRect box = VitalsReader::caption_box(bar);
    const int left = bar.right() - 7 - drawn.width;
    const int top = box.y + (box.h - drawn.height) / 2;
    for (int y = 0; y < drawn.height; ++y)
        for (int x = 0; x < drawn.width; ++x) {
            std::uint8_t r, g, b;
            drawn.get(x, y, r, g, b);
            put(f, left + x, top + y, r, g, b);
        }
}

void test_reader_caption_shadow() {
    section("Vitals reader: a read that misses MP's figures in the caption's shadow goes by the kept ones, and says "
            "they are not its own");
    GlyphFont font;
    const std::string dir = GlyphFont::default_dir();
    if (dir.empty() || !font.load(dir)) {
        skip("no glyph set installed -- the reader cannot read figures\n");
        return;
    }
    BarSpec hp, mp;
    hp.x = mp.x = 83;
    hp.y = 8;
    mp.y = 40;                          // apart: neither caption's box reaches the other's
    hp.width = mp.width = 159;
    hp.height = mp.height = 5;
    hp.r = 239; hp.g = 0; hp.b = 0; hp.tolerance = 80;
    mp.r = 0; mp.g = 49; mp.b = 206; mp.tolerance = 80;
    // HP 8064/10084 throughout; MP 12643/15280 printed, then no figures with
    // the fill at 56% -- where the caption begins -- and at 40%.
    const int low_fill = 159 * 40 / 100;
    Frame frames[3];
    for (Frame& f : frames) {
        f = flat_frame(400, 64, 24, 24, 28);
        paint(f, hp, 159 * 80 / 100, font, "8064 10084");
    }
    paint(frames[0], mp, 159 * 83 / 100, font, "12643 15280");
    paint(frames[1], mp, 159 * 56 / 100, font, "");
    paint(frames[2], mp, low_fill, font, "");
    const float shadow_measure = VitalsReader::measure(frames[1], mp);

    std::atomic<int> which{ 0 };
    VitalsReader vr;
    vr.set_grab([&frames, &which](void*, Frame& out) {
        out = frames[which.load()];
        return true;
    });
    vr.set_auto_relocate(false);
    vr.set_hp_bar(hp);
    vr.set_mp_bar(mp);
    vr.set_target_window(reinterpret_cast<void*>(std::uintptr_t{ 0x5000 }));
    vr.set_period_ms(VitalsReader::kMinPeriodMs);
    vr.start();
    // `n` more reads, the first of which may have been grabbed already.
    const auto reads = [&vr](std::uint64_t n) {
        const std::uint64_t from = vr.stats().reads;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (vr.stats().reads < from + n && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return vr.stats().reads >= from + n;
    };
    const bool read_a = reads(4);
    const VitalsReader::Vitals a = vr.values();
    const float pct_a = vr.mp_percent();
    which.store(1);
    const bool read_b = reads(2);
    const VitalsReader::Vitals b = vr.values();
    const float pct_b = vr.mp_percent();
    which.store(2);
    const bool read_c = reads(2);
    const VitalsReader::Vitals c = vr.values();
    const float pct_c = vr.mp_percent();
    vr.stop();

    const float kept = 12643.0f * 100.0f / 15280.0f;
    check(read_a && a.mp == 12643 && a.mp_max == 15280 && a.mp_now && a.hp == 8064 && a.hp_now &&
              std::fabs(pct_a - kept) < 0.05f,
          "the figures printed: MP 12643/15280, made out on the read itself -- " + std::to_string(a.mp) + "/" +
              std::to_string(a.mp_max) + ", " + std::to_string(pct_a) + "%");
    check(shadow_measure >= VitalsReader::kCaptionShadowPct && shadow_measure < 59.0f && read_b && !b.mp_now &&
              b.mp == 12643 && b.hp_now && std::fabs(pct_b - kept) < 0.05f,
          "no figures, the fill measuring " + std::to_string(shadow_measure) +
              "% -- the caption's shadow: MP goes as the kept 82.7%, not under the mark of 59, and the figures are "
              "not this read's own -- " + std::to_string(pct_b) + "%");
    check(read_c && !c.mp_now && std::fabs(pct_c - static_cast<float>(low_fill) * 100.0f / 159.0f) < 1.5f &&
              c.hp_now,
          "no figures, the fill at 40%, left of any caption: the measure stands -- " + std::to_string(pct_c) + "%");
}

// ------------------------------------------------------------ usable

void test_usable() {
    section("Vitals: a bar's packet value gone by whenever it is current, live and with a maximum -- no check "
            "against the bars (2026.11.17)");
    const std::uint64_t now = kT0 + 100'000;
    VitalsUsable ok;
    ok.use_on = true;
    ok.capturing = true;
    ok.on_window = true;
    ok.self_starts = 3;
    ok.capture_starts = 3;
    ok.value_epoch = 7;
    ok.epoch = 7;
    ok.value_ms = now - 30'000;
    ok.last_server_ms = now - 5000;
    ok.max_known = true;
    const bool all = vitals_usable(ok, now) && *vitals_unusable_why(ok, now) == '\0';

    std::string failed;
    const auto without = [&](const char* what, VitalsUsable in) {
        if (vitals_usable(in, now) || *vitals_unusable_why(in, now) == '\0') failed += std::string(" ") + what;
    };
    VitalsUsable in = ok;
    in.use_on = false;
    without("[Packets] off", in);
    in = ok;
    in.capturing = false;
    without("no capture", in);
    in = ok;
    in.on_window = false;
    without("a capture not the game window's own", in);
    const std::string foreign = vitals_unusable_why(in, now);
    in = ok;
    in.self_starts = 2;
    without("an old start count", in);
    in = ok;
    in.value_epoch = 6;
    without("a stale epoch", in);
    in = ok;
    in.value_ms = 0;
    without("no value", in);
    in = ok;
    in.last_server_ms = now - 5001;
    without("5 s of quiet", in);
    in = ok;
    in.last_server_ms = 0;
    without("no server frame", in);
    in = ok;
    in.max_known = false;
    without("no maximum", in);
    check(all && failed.empty(),
          "all of it: usable, a value 30 s old included -- one comes only when it changes; any one missing (a stale "
          "epoch, an old start count, 5 s of quiet...) is not, with the reason said" +
              (failed.empty() ? std::string() : " -- usable all the same:" + failed));
    check(foreign == "the capture is not the game window's own",
          "a capture of every client's traffic, or one left on another client's process: not usable, and said so "
          "-- \"" + foreign + "\"");

    // A gap, then the first value after it (2026.11.17): the screen until it
    // comes, the packets from the moment it does -- no agreements to earn.
    VitalsUsable gap = ok;
    gap.epoch = 8;
    const std::string gap_why = vitals_unusable_why(gap, now);
    VitalsUsable first = gap;
    first.value_epoch = 8;
    first.value_ms = now;
    check(!vitals_usable(gap, now) && gap_why == "no value since the capture started or since a gap in it" &&
              vitals_usable(first, now),
          "after a gap in the capture: the screen, said why; the first value of the new epoch: the packets at "
          "once");
}

// ------------------------------------------------------------ resting

void test_rest() {
    section("Vitals: the HP/MP reader rests only with both bars usable for 10 s, and none of the reasons not to");
    const std::uint64_t now = kT0 + 500'000;
    VitalsRest ok;
    ok.hp_usable = true;
    ok.mp_usable = true;
    ok.usable_since_ms = now - kRestAfterUsableMs;
    std::string failed;
    const auto without = [&](const char* what, VitalsRest in) {
        if (vitals_rest_allowed(in, now)) failed += std::string(" ") + what;
    };
    VitalsRest in = ok;
    in.usable_since_ms = now - kRestAfterUsableMs + 1;
    without("9.999 s usable", in);
    in = ok;
    in.usable_since_ms = 0;
    without("not usable now", in);
    in = ok;
    in.mp_usable = false;
    without("MP not usable", in);
    in = ok;
    in.critical = true;
    without("a critical hold", in);
    in = ok;
    in.finding_bars = true;
    without("Tự tìm", in);
    in = ok;
    in.hp_zero_since_ms = now - 100;
    without("HP at 0", in);
    in = ok;
    in.death_seen_ms = now - kRestAfterDeathMs + 1;
    without("a death panel 59.999 s ago", in);
    in = ok;
    in.death_seen_ms = now - kRestAfterDeathMs;
    const bool after_death = vitals_rest_allowed(in, now);
    check(vitals_rest_allowed(ok, now) && after_death && failed.empty(),
          "rests after 10 s usable, and a minute after a death panel; not in a critical hold, during Tự tìm, while "
          "HP reads 0" +
              (failed.empty() ? std::string() : " -- rested all the same:" + failed));
    check(vitals_rest_period_ms(150, 1000) == 1000 && vitals_rest_period_ms(250, 1000) == 1000 &&
              vitals_rest_period_ms(150, 5000) == 1000 && vitals_rest_period_ms(150, 150) == 150 &&
              vitals_rest_period_ms(400, 300) == 400,
          "the resting pace: vitals_idle_ms, at most a second, never quicker than the run's");

    // 2026.11.16: a pet potion key rests too, twice a second -- the pet's HP
    // is only on the screen, and the macro drinks on a read 1.5 s old at most.
    VitalsRest pet = ok;
    pet.pet_key = true;
    VitalsRest pet_critical = pet;
    pet_critical.critical = true;
    VitalsRest pet_early = pet;
    pet_early.usable_since_ms = now - kRestAfterUsableMs + 1;
    check(vitals_rest_allowed(pet, now) && !vitals_rest_allowed(pet_critical, now) &&
              !vitals_rest_allowed(pet_early, now),
          "with a pet potion key: rests, as long as nothing else says no (a critical hold, under 10 s usable)");
    check(kRestPetMs == 500 && vitals_rest_period_ms(150, 1000, true) == 500 &&
              vitals_rest_period_ms(250, 1000, true) == 500 && vitals_rest_period_ms(150, 5000, true) == 500 &&
              vitals_rest_period_ms(150, 300, true) == 300 && vitals_rest_period_ms(600, 1000, true) == 600 &&
              vitals_rest_period_ms(150, 1000, false) == 1000,
          "the resting pace with a pet potion key: 500 ms (vitals_idle_ms when quicker, the run's when slower); "
          "a second without -- " + num(static_cast<long long>(vitals_rest_period_ms(150, 1000, true))));
    check(static_cast<std::uint64_t>(kRestPetMs) + kWorldWindowWorkMs < MacroEngine::kVitalsStaleMs &&
              world_window_ms(kRestPetMs, MacroEngine::kOutOfWorldAfterMs) == 1250,
          "a read at the pet's rest, with its work, is never too old for the macro (1500 ms); HP may go unread "
          "1.25 s before the macro is held");
}

void test_world_window() {
    section("Vitals: how long HP may go unread before the macro is held -- as before at the run's pace");
    const std::uint64_t least = MacroEngine::kOutOfWorldAfterMs;
    check(world_window_ms(150, least) == 1000 && world_window_ms(250, least) == 1000 &&
              world_window_ms(1000, least) == 2250,
          "150 ms: 1000, as always; 250: 1000; resting at a second: 2250 -- " +
              num(static_cast<long long>(world_window_ms(1000, least))));
    PeriodWindow w;
    w.note(kT0, 150);
    const int run = w.longest(kT0 + 60'000);
    w.note(kT0 + 60'000, 1000);
    const int resting = w.longest(kT0 + 70'000);
    w.note(kT0 + 70'000, 150);
    const int at_cut = w.longest(kT0 + 70'000);
    const int later = w.longest(kT0 + 72'999);
    const int after = w.longest(kT0 + 73'000);
    check(run == 150 && resting == 1000 && at_cut == 1000 && later == 1000 && after == 150 &&
              world_window_ms(later, least) == 2250 && world_window_ms(after, least) == 1000,
          "the pace cut from a second to 150 ms: still wide for 3 s, the reads before the cut that far apart; "
          "1000 again after");
    PeriodWindow none;
    check(none.longest(kT0) == 0, "nothing noted: 0, and the floor");
}

void test_map_loading() {
    section("Vitals: a map loading by the packets holds the macro, only on the game's own capture");
    const std::uint64_t now = kT0 + 900'000;
    MapLoad in;
    in.on_window = true;
    in.last_server_ms = now - 100;
    in.entry_ms = now - 1000;
    in.last_own_ms = now - 4000;
    const bool loading = map_load_holds(in, now);
    MapLoad reported = in;
    reported.last_own_ms = in.entry_ms + 200;
    MapLoad late = in;
    late.entry_ms = now - kLoadHoldMs;
    MapLoad nearly = in;
    nearly.entry_ms = now - kLoadHoldMs + 1;
    MapLoad other = in;
    other.on_window = false;
    MapLoad quiet = in;
    quiet.last_server_ms = now - 5001;
    MapLoad never = in;
    never.entry_ms = 0;
    check(loading && !map_load_holds(reported, now) && !map_load_holds(late, now) && map_load_holds(nearly, now) &&
              !map_load_holds(other, now) && !map_load_holds(quiet, now) && !map_load_holds(never, now),
          "within 3 s of a map entry with no report of yours: loading; not after a report, not at 3 s, not with "
          "no capture on the window's process, not with the server quiet, not with no entry");
}

// ------------------------------------- the MP caption's leading 1 (2026.11.08)

void test_lost_leading_one() {
    section("The MP figure short of its leading 1 -- 4798 for 14798 -- told, for the caption's picture (2026.11.08)");

    check(figure_lost_leading_one(4798, 14798) && figure_lost_leading_one(6209, 16209) &&
              figure_lost_leading_one(798, 1798) && figure_lost_leading_one(500, 10500),
          "the packets' value with its leading 1 gone: 4798 for 14798, 6209 for 16209, 798 for 1798, 500 for "
          "10500");
    check(!figure_lost_leading_one(4798, 24798) && !figure_lost_leading_one(14798, 14798) &&
              !figure_lost_leading_one(14788, 14798) && !figure_lost_leading_one(1479, 14798) &&
              !figure_lost_leading_one(142, 14236) && !figure_lost_leading_one(-1, 14798) &&
              !figure_lost_leading_one(4, 14) && !figure_lost_leading_one(8305, 8307),
          "never another digit lost, a trailing one (1479, 142 of 14236), a value off by ten or two, no figure, "
          "or a value under 100");
}

// ---------------------------------- a digit of the bars' figures lost (2026.11.13)

void test_lost_a_digit() {
    section("A bar's figure short of the packets' by one digit is the reader's miss, not the packets wrong "
            "(2026.11.13)");

    check(figure_lost_a_digit(1552, 11552) && figure_lost_a_digit(1152, 11552) && figure_lost_a_digit(1155, 11552) &&
              figure_lost_a_digit(1859, 18590) && figure_lost_a_digit(4798, 14798) && figure_lost_a_digit(500, 10500) &&
              figure_lost_a_digit(1479, 14798),
          "any one digit gone: 1552, 1152, 1155 for 11552; 1859 for 18590; 4798 for 14798; 500 for 10500; 1479 for "
          "14798");
    check(!figure_lost_a_digit(11552, 11552) && !figure_lost_a_digit(1, 11552) && !figure_lost_a_digit(552, 11552) &&
              !figure_lost_a_digit(8305, 8307) && !figure_lost_a_digit(7778, 11552) && !figure_lost_a_digit(-1, 11552) &&
              !figure_lost_a_digit(4, 14) && !figure_lost_a_digit(115520, 11552),
          "never the same figure, two digits gone (1/552's 1, 552), another value, no figure, a value under 100 or a "
          "longer figure");

    // Set beside the packets for the log: short -- the reader's miss -- not
    // other.
    BarCompare::Point p{ 1000, 11552, 18590, true, 1 };
    check(BarCompare::compare(p, figures_at(1000, 1552, 18590)) == Verdict::Short,
          "1552/18590 against the packets' 11552: short, not other");
    p.value = 18590;
    check(BarCompare::compare(p, figures_at(1000, 1859, 18590)) == Verdict::Short &&
              BarCompare::compare(p, figures_at(1000, 1859, 1859)) == Verdict::Short,
          "1859/18590 against the packets' 18590 (the owner's MP of 2026-11-13), and a maximum short by its last "
          "digit (1859 for 18590): short");
    p.value = 11552;
    check(BarCompare::compare(p, figures_at(1000, 7778, 18590)) == Verdict::Other &&
              BarCompare::compare(p, figures_at(1000, 11552, 18000)) == Verdict::Other,
          "a figure that is another value, or a maximum that is another value (18000 against 18590): other");
}

// ------------------------- the bars' figures short of their first digits (2026.11.16)

void test_lost_leading_digits() {
    section("A bar's figure short of the packets' first two or three digits is the reader's miss too: 462 for "
            "16462 (2026.11.16)");

    check(figure_lost_leading_digits(462, 16462) && figure_lost_leading_digits(297, 17297) &&
              figure_lost_leading_digits(850, 11850) && figure_lost_leading_digits(59, 9859) &&
              figure_lost_leading_digits(62, 16462) && figure_lost_leading_digits(6462, 16462) &&
              figure_lost_leading_digits(500, 10500) && figure_lost_leading_digits(7616, 17616) &&
              figure_lost_leading_digits(616, 17616) && figure_lost_leading_digits(2, 16462),
          "two lost (462 for 16462, 297 for 17297, 850 for 11850, 59 for 9859, 616 for 17616), three (62), one "
          "(6462, 7616), four (2), and 500 for 10500");
    check(!figure_lost_leading_digits(16462, 16462) && !figure_lost_leading_digits(164, 16462) &&
              !figure_lost_leading_digits(463, 16462) && !figure_lost_leading_digits(0, 10000) &&
              !figure_lost_leading_digits(-1, 16462) && !figure_lost_leading_digits(4, 14) &&
              !figure_lost_leading_digits(164620, 16462) && !figure_lost_leading_digits(1462, 16462),
          "never the whole value, its first digits kept (164), another value (463, 1462), 0 -- HP at 0 is the "
          "screen's -- no figure, a value under 100, or a longer figure");

    // Set beside the packets for the log: short where it is the packet value
    // or maximum less its first digits, other where it is not.
    BarCompare::Point p{ 1000, 16462, 17616, true, 1 };
    check(BarCompare::compare(p, figures_at(1000, 462, 17616)) == Verdict::Short &&
              BarCompare::compare(p, figures_at(1000, 462, 616)) == Verdict::Short &&
              BarCompare::compare(p, figures_at(1000, 462, 7616)) == Verdict::Short,
          "462/17616 against the packets' 16462 (a user's MP, 13:47:18), and maxima short of their first digits "
          "(616, 7616 for 17616): short");
    p.value = 9859;
    check(BarCompare::compare(p, figures_at(1000, 59, 17616)) == Verdict::Short,
          "59 for 9859, three digits lost: short");
    p.value = 16462;
    BarCompare::Point other_max = p;
    other_max.max = 17000;
    check(BarCompare::compare(p, figures_at(1000, 463, 17616)) == Verdict::Other &&
              BarCompare::compare(other_max, figures_at(1000, 462, 616)) == Verdict::Other,
          "463 is another value, and 616 is not 17000 less its first digits: other");
}

// ------------------------------------------------ the EXP bar's tip (2026.11.17)

// The owner's run of 2026-11-17: MP 15542/16813 and HP 14513/15625, the EXP
// bar's glowing tip -- it rides along the bar under MP's as EXP grows -- over
// the first two digits of MP's caption. The bars read MP 542/16813 (3%), the
// packets 92-100%; MP was never trusted while the skills ran in turn, and
// its potion went 60 times a minute.

void test_figure_behind_fill() {
    section("Vitals reader: figures short of the bar's own fill are refused (2026.11.17)");
    struct Case {
        float measured;
        int cur, max;
        bool refused;
        const char* what;
    };
    const Case cases[] = {
        { 57.3f, 542, 16813, true, "542/16813 (3.2%) on a bar filled to 57%: the tip over \"15542\"" },
        { 57.0f, 811, 16813, true, "811/16813 (4.8%): \"16811\" covered" },
        { 57.0f, 4215, 16813, true, "4215/16813 (25%): the digit beside the tip misread too" },
        { 57.0f, 4958, 16813, true, "4958/16813 (29%)" },
        { 57.0f, 4991, 16813, true, "4991/16813 (30%)" },
        { 57.0f, 509, 16813, true, "509/16813 (3%)" },
        { 57.0f, 6813, 16813, true, "6813/16813 (40.5%): a full MP short of its leading 1" },
        { 57.0f, 5542, 16813, true, "5542/16813 (33%): \"15542\" short of its 1" },
        { 57.0f, 9999, 19999, true, "9999/19999 (50%): \"19999\" short of its 1, the largest such maximum" },
        { 56.7f, 14513, 15625, false, "14513/15625 on a fill measured short by its caption: as many digits" },
        { 3.1f, 542, 16813, false, "542/16813 on a bar filled to 3%: an honest low MP" },
        { 8.0f, 542, 16813, false, "542/16813 on 8%: within a digit cell and a pixel" },
        { 53.5f, 9000, 16813, false, "9000/16813 on 53.5%" },
        { 72.7f, 6710, 9232, false, "6710/9232: as many digits as its maximum" },
        { -1.0f, 542, 16813, false, "no measure: nothing to hold them against" },
        { 57.0f, 0, 16813, true, "0/16813 on a bar filled to 57%" },
        { 0.0f, 0, 15625, false, "0/15625 on an empty bar: HP at 0, as the game prints it" },
    };
    std::string wrong;
    for (const Case& k : cases)
        if (VitalsReader::figure_behind_fill(k.measured, k.cur, k.max) != k.refused) wrong += std::string(" ") + k.what + ";";
    check(wrong.empty(), "refused where the fill runs past the figures, kept where it does not" +
                             (wrong.empty() ? std::string() : " -- wrong:" + wrong));
}

/// `cells` printed across `bar` as the game prints its caption: digits on a
/// 6 px pitch, the slash 3 px (left empty: the set has no glyph for it),
/// right-aligned 7 px in from the bar's right edge, each in white on a black
/// outline over the bar; a space is a digit's cell left empty. False with no
/// glyph for one of the digits.
bool print_pitched(Frame& f, const BarSpec& bar, const GlyphFont& font, const std::string& cells) {
    const int cell = 6, slash = 3;
    int width = 0;
    for (const char ch : cells) width += ch == '/' ? slash : cell;
    const ScreenRect box = VitalsReader::caption_box(bar);
    int pen = bar.right() - 7 - width;
    const int top = box.y + 5;
    std::vector<std::pair<int, int>> ink;
    for (const char ch : cells) {
        const int at = pen;
        pen += ch == '/' ? slash : cell;
        if (ch < '0' || ch > '9') continue;
        Frame digit;
        if (!font.render_line(std::string(1, ch), digit)) return false;
        for (int y = 0; y < digit.height; ++y)
            for (int x = 0; x < digit.width; ++x) {
                std::uint8_t r, g, b;
                digit.get(x, y, r, g, b);
                if (r < 60 && g < 60 && b < 60) continue;   // keep the ground
                ink.emplace_back(at + x, top + y);
            }
    }
    // The outline first, then the ink over it.
    for (const auto& [x, y] : ink)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) put(f, x + dx, y + dy, 0, 0, 0);
    for (const auto& [x, y] : ink) put(f, x, y, 255, 255, 255);
    return true;
}

/// Where the ink of the first cell of `cells` begins, as print_pitched lays
/// them: the set renders a glyph with a margin round its ink.
int pitched_left(const GlyphFont& font, const BarSpec& bar, const std::string& cells) {
    int width = 0;
    for (const char ch : cells) width += ch == '/' ? 3 : 6;
    int margin = 0;
    Frame eight;
    if (font.render_line("8", eight)) {
        margin = eight.width;
        for (int y = 0; y < eight.height; ++y)
            for (int x = 0; x < eight.width; ++x) {
                std::uint8_t r, g, b;
                eight.get(x, y, r, g, b);
                if (!(r < 60 && g < 60 && b < 60)) margin = std::min(margin, x);
            }
    }
    return bar.right() - 7 - width + margin;
}

/// The EXP bar's glowing tip as on the owner's frame: white at its heart,
/// pink round it (ffffff, ff63ff, ff00ff), an ellipse `rx` by `ry` about
/// cx,cy.
void tip(Frame& f, int cx, int cy, int rx, int ry) {
    for (int y = cy - ry; y <= cy + ry; ++y)
        for (int x = cx - rx; x <= cx + rx; ++x) {
            const double dx = static_cast<double>(x - cx) / rx, dy = static_cast<double>(y - cy) / ry;
            const double d = dx * dx + dy * dy;
            if (d > 1.0) continue;
            if (d < 0.2)      put(f, x, y, 255, 255, 255);
            else if (d < 0.5) put(f, x, y, 255, 99, 255);
            else              put(f, x, y, 255, 0, 255);
        }
}

/// The owner's HUD drawn: HP and MP bars of 159 at 83,8 and 83,22 in the
/// game's colours, filled `hp_fill` and `mp_fill` of 159, and their captions
/// ("" none).
Frame hud(const GlyphFont& font, BarSpec& hp, BarSpec& mp, int hp_fill, const char* hp_cells, int mp_fill,
          const char* mp_cells, bool& drawn) {
    hp.x = mp.x = 83;
    hp.y = 8;
    mp.y = 22;
    hp.width = mp.width = 159;
    hp.height = mp.height = 5;
    hp.r = 239; hp.g = 0; hp.b = 0; hp.tolerance = 80;
    mp.r = 0; mp.g = 49; mp.b = 206; mp.tolerance = 80;
    Frame f = flat_frame(320, 48, 24, 24, 28);
    paint(f, hp, hp_fill, font, "");
    paint(f, mp, mp_fill, font, "");
    drawn = print_pitched(f, hp, font, hp_cells) && print_pitched(f, mp, font, mp_cells);
    return f;
}

void test_reader_exp_tip() {
    section("Vitals reader: MP's caption under the EXP bar's tip reads as nothing, never as 3% (2026.11.17)");
    GlyphFont font;
    const std::string dir = GlyphFont::default_dir();
    if (dir.empty() || !font.load(dir)) {
        skip("no glyph set installed -- the reader cannot read figures\n");
        return;
    }
    BarSpec hp, mp;
    bool drawn = true, ok = true;
    const int full_mp = 159 * 92 / 100, full_hp = 159 * 93 / 100;
    const std::string mp_cells = "15543/16813";
    // The tip over the first two digits; over the first alone.
    Frame two = hud(font, hp, mp, full_hp, "14513/15625", full_mp, mp_cells.c_str(), ok);
    drawn = drawn && ok;
    const int left = pitched_left(font, mp, mp_cells);
    tip(two, left + 5, mp.y + 4, 6, 8);
    Frame one = hud(font, hp, mp, full_hp, "14513/15625", full_mp, mp_cells.c_str(), ok);
    drawn = drawn && ok;
    tip(one, left + 1, mp.y + 4, 4, 8);
    Frame full = hud(font, hp, mp, full_hp, "14513/15625", 159, "16813/16813", ok);
    drawn = drawn && ok;
    tip(full, pitched_left(font, mp, "16813/16813") + 1, mp.y + 4, 4, 8);
    // Honest short figures: 542 on a bar filled to 3%, the tip over the
    // empty cells left of it; 9000 on 53.5%; 6710/9232 on 72.7%.
    Frame low = hud(font, hp, mp, full_hp, "14513/15625", 5, "542/16813", ok);
    drawn = drawn && ok;
    tip(low, pitched_left(font, mp, "542/16813") - 7, mp.y + 4, 6, 8);
    Frame mid = hud(font, hp, mp, full_hp, "14513/15625", 159 * 535 / 1000, "9000/16813", ok);
    drawn = drawn && ok;
    Frame same = hud(font, hp, mp, full_hp, "14513/15625", 159 * 727 / 1000, "6710/9232", ok);
    drawn = drawn && ok;
    if (!drawn) {
        skip("no glyph for a digit of the pitched captions\n");
        return;
    }

    struct Case {
        const Frame* f;
        int want_cur, want_max;     // read_numbers' figures; -1: none
        bool refused;               // figure_behind_fill on the bar's measure
        const char* what;
    };
    const Case cases[] = {
        { &two, 543, 16813, true, "\"15543/16813\" under the tip reads 543/16813 with no hole to show it -- refused" },
        { &one, 5543, 16813, true, "\"15543/16813\" short of its 1 reads 5543/16813 (33%) -- refused" },
        { &full, 6813, 16813, true, "\"16813/16813\" short of its 1 reads 6813/16813 (40.5%) -- refused" },
        { &low, 542, 16813, false, "542/16813 on a bar filled to 3%, the tip left of it -- kept" },
        { &mid, 9000, 16813, false, "9000/16813 on 53.5% -- kept" },
        { &same, 6710, 9232, false, "6710/9232 on 72.7% -- kept" },
    };
    for (const Case& k : cases) {
        int cur = -1, max = -1;
        const bool read = VitalsReader::read_numbers(*k.f, mp, font, cur, max);
        const float measured = VitalsReader::measure(*k.f, mp);
        const bool refused = read && VitalsReader::figure_behind_fill(measured, cur, max);
        const bool ok_case = read && cur == k.want_cur && max == k.want_max && refused == k.refused;
        check(ok_case, std::string(k.what) + " -- read " + num(cur) + "/" + num(max) + ", the bar measuring " +
                           std::to_string(measured) + "%" +
                           (ok_case ? std::string()
                                    : ", the line \"" + font.read_line(*k.f, VitalsReader::caption_box(mp)) + "\""));
    }
    int hc = -1, hm = -1;
    check(VitalsReader::read_numbers(two, hp, font, hc, hm) && hc == 14513 && hm == 15625 &&
              !VitalsReader::figure_behind_fill(VitalsReader::measure(two, hp), hc, hm),
          "HP's caption, out of the tip's reach, reads 14513/15625 and is kept");

    // Through the reader: the honest frame, then the tip over MP's caption --
    // the kept figures first, then nothing -- then MP at 40% with its first
    // digit covered ("6725/16813" read 725), and HP's caption gone, its bar
    // at 80%.
    Frame low_mp = hud(font, hp, mp, 159 * 80 / 100, "", 159 * 40 / 100, "6725/16813", ok);
    // Wider than a digit, as the tip is: one the size of a digit, on the dark
    // of an empty bar, the set can take for one.
    tip(low_mp, pitched_left(font, mp, "6725/16813") - 1, mp.y + 4, 6, 8);
    Frame plain = hud(font, hp, mp, full_hp, "14513/15625", full_mp, mp_cells.c_str(), ok);
    {
        int c = -1, m = -1;
        const bool r = VitalsReader::read_numbers(low_mp, mp, font, c, m);
        int pc = -1, pm = -1;
        const bool pr = VitalsReader::read_numbers(plain, mp, font, pc, pm);
        check(r && c == 725 && m == 16813 && pr && pc == 15543 && pm == 16813,
              "the frames for the reader: MP 40% under the tip reads 725/16813, the plain one 15543/16813 -- " +
                  num(c) + "/" + num(m) + " \"" + font.read_line(low_mp, VitalsReader::caption_box(mp)) + "\", " +
                  num(pc) + "/" + num(pm) + " \"" + font.read_line(plain, VitalsReader::caption_box(mp)) + "\"");
    }
    const Frame* frames[] = { &plain, &two, &low_mp };
    std::atomic<int> which{ 0 };
    VitalsReader vr;
    vr.load_font(dir);
    vr.set_grab([&frames, &which](void*, Frame& out) {
        out = *frames[which.load()];
        return true;
    });
    vr.set_auto_relocate(false);
    vr.set_hp_bar(hp);
    vr.set_mp_bar(mp);
    vr.set_target_window(reinterpret_cast<void*>(std::uintptr_t{ 0x5000 }));
    vr.set_period_ms(VitalsReader::kMinPeriodMs);
    const int forget = VitalsReader::forget_after_misses(VitalsReader::kMinPeriodMs);
    vr.start();
    const auto reads = [&vr](std::uint64_t n) {
        const std::uint64_t from = vr.stats().reads;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (vr.stats().reads < from + n && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return vr.stats().reads >= from + n;
    };
    const bool a_read = reads(4);
    const float pct_a = vr.mp_percent();
    which.store(1);
    // Every read while the kept figures last: never 3%.
    bool never_low = reads(1);
    float lowest = 100.0f;
    for (int i = 0; i < forget - 4 && never_low; ++i) {
        never_low = reads(1);
        const float p = vr.mp_percent();
        if (p >= 0.0f) lowest = std::min(lowest, p);
    }
    const bool b_read = reads(static_cast<std::uint64_t>(forget) + 4);
    const float pct_b = vr.mp_percent();
    const bool covered_b = vr.mp_covered();
    const float hp_b = vr.hp_percent();
    const VitalsReader::Vitals vb = vr.values();
    which.store(2);
    const bool c_read = reads(static_cast<std::uint64_t>(forget) + 4);
    const float pct_c = vr.mp_percent();
    const bool covered_c = vr.mp_covered();
    const float hp_c = vr.hp_percent();
    const VitalsReader::Stats st = vr.stats();
    vr.stop();

    const float kept = 15543.0f * 100.0f / 16813.0f;
    check(a_read && std::fabs(pct_a - kept) < 0.05f, "MP 15543/16813 read off the plain frame: " + std::to_string(pct_a) + "%");
    check(never_low && lowest > 90.0f,
          "the tip over MP's caption: the kept 92.4% stands while kept, never 3% -- lowest " + std::to_string(lowest) +
              "%");
    check(b_read && pct_b < 0.0f && covered_b && !vb.mp_now && vb.hp_now && std::fabs(hp_b - 92.88f) < 0.1f,
          "and once they are forgotten, MP reads as nothing (its caption covered), not the bar's measure; HP "
          "reads on -- MP " + std::to_string(pct_b) + ", HP " + std::to_string(hp_b));
    check(c_read && std::fabs(pct_c - 40.0f) < 1.5f && !covered_c && hp_c > 78.0f && hp_c < 81.0f &&
              st.behind_fill_mp > 0 && st.behind_fill_hp == 0,
          "MP at 40% with its first digit covered (725/16813 refused): the measure, left of any caption, stands; "
          "HP's caption gone, its bar's measure stands -- MP " + std::to_string(pct_c) + ", HP " +
              std::to_string(hp_c) + ", refused MP " + num(static_cast<long long>(st.behind_fill_mp)));
}

/// FNV-1a over a file's bytes, as known_live_frame hashes the live frames.
std::uint64_t file_hash(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (std::istreambuf_iterator<char> it(in), end; it != end; ++it) {
        h ^= static_cast<unsigned char>(*it);
        h *= 0x100000001b3ull;
    }
    return h;
}

void test_reader_exp_tip_picture() {
    section("Vitals reader: the owner's frame of 2026-11-17 00:23:27 -- MP under the EXP bar's tip, at every game size");
#ifdef GOBOT_SOURCE_DIR
    // The HUD's corner of checkin_20261117_002327_00_start15.png (300x48),
    // as it was cut: build\handoff\vitals\testdata.
    const std::string path =
        std::string(GOBOT_SOURCE_DIR) + "/build/handoff/vitals/testdata/mp_spark_20261117_002327.png";
    Frame shot;
    if (file_hash(path) != 0x140f4bf9afc293e2ull || !load_png(path, shot) || shot.width != 300 || shot.height != 48) {
        skip("no cut of the owner's frame of 2026-11-17 here (build\\handoff\\vitals\\testdata)\n");
        return;
    }
    GlyphFont font;
    const std::string dir = GlyphFont::default_dir();
    if (dir.empty() || !font.load(dir)) {
        skip("no glyph set installed -- the reader cannot read figures\n");
        return;
    }
    // Where the landmark at 44,4 puts them (HudLayout).
    const VitalsReader::HudLayout layout;
    BarSpec hp, mp;
    hp.x = mp.x = 44 + layout.hp_dx;
    hp.y = 4 + layout.hp_dy;
    mp.y = 4 + layout.mp_dy;
    hp.width = mp.width = layout.bar_w;
    hp.height = mp.height = layout.bar_h;
    hp.r = 239; hp.g = 0; hp.b = 0; hp.tolerance = 80;
    mp.r = 0; mp.g = 49; mp.b = 206; mp.tolerance = 80;
    int hc = -1, hm = -1, mc = -1, mm = -1;
    const bool hp_read = VitalsReader::read_numbers(shot, hp, font, hc, hm);
    const bool mp_read = VitalsReader::read_numbers(shot, mp, font, mc, mm);
    const float hp_measure = VitalsReader::measure(shot, hp);
    const float mp_measure = VitalsReader::measure(shot, mp);
    check(hp_read && hc == 14513 && hm == 15625 && !VitalsReader::figure_behind_fill(hp_measure, hc, hm),
          "HP 14513/15625 read and kept (the bar measuring " + std::to_string(hp_measure) + "%)");
    check(mp_read && mc == 542 && mm == 16813 && mp_measure > 54.0f && mp_measure < 60.0f &&
              VitalsReader::figure_behind_fill(mp_measure, mc, mm),
          "MP read 542/16813 -- \"15542\" under the tip, no hole to show it -- and refused: the bar measures " +
              std::to_string(mp_measure) + "%, past 3.2%");

    // The HUD sits in the window's top-left corner at every size the game
    // runs at, found by its landmark: the same verdict at each.
    Frame landmark;
    if (!TemplateStore::load_one("hpmp_indicator2", exe_relative("configs/templates.ini"), landmark)) {
        skip("no HUD landmark beside this test: the owner's frame at 1024x768, 1280x720 and 1366x768\n");
        return;
    }
    const std::pair<int, int> sizes[] = { { 1024, 768 }, { 1280, 720 }, { 1366, 768 } };
    for (const auto& [w, h] : sizes) {
        Frame f = flat_frame(w, h, 40, 20, 20);
        for (int y = 0; y < shot.height; ++y)
            for (int x = 0; x < shot.width; ++x) {
                std::uint8_t r, g, b;
                shot.get(x, y, r, g, b);
                put(f, x, y, r, g, b);
            }
        VitalsReader vr;
        vr.load_font(dir);
        vr.set_anchor(landmark);
        vr.set_grab([&f](void*, Frame& out) {
            out = f;
            return true;
        });
        vr.set_auto_relocate(false);
        vr.set_target_window(reinterpret_cast<void*>(std::uintptr_t{ 0x5000 }));
        vr.set_period_ms(VitalsReader::kMinPeriodMs);
        vr.start();
        float lowest = 101.0f;
        bool low = false;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        const int forget = VitalsReader::forget_after_misses(VitalsReader::kMinPeriodMs);
        std::uint64_t seen = 0;
        while (vr.stats().reads < static_cast<std::uint64_t>(forget) + 6 && std::chrono::steady_clock::now() < until) {
            const std::uint64_t n = vr.stats().reads;
            if (n != seen) {
                seen = n;
                const float p = vr.mp_percent();
                if (p >= 0.0f && p < 50.0f) low = true;
                if (p >= 0.0f) lowest = std::min(lowest, p);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const float mp_now = vr.mp_percent();
        const float hp_now = vr.hp_percent();
        const bool covered = vr.mp_covered();
        const ScreenRect at = vr.anchor_at();
        const VitalsReader::Stats st = vr.stats();
        vr.stop();
        check(at.x == 44 && at.y == 4 && !low && mp_now < 0.0f && covered && std::fabs(hp_now - 92.88f) < 0.1f &&
                  st.behind_fill_mp > 0,
              std::to_string(w) + "x" + std::to_string(h) + ": the landmark at " + num(at.x) + "," + num(at.y) +
                  "; MP never under 50% (lowest " + (lowest > 100.0f ? std::string("none") : std::to_string(lowest)) +
                  "), nothing now -- " + std::to_string(mp_now) + "; HP " + std::to_string(hp_now) + "%");
    }
#else
    skip("no source folder\n");
#endif
}

// ------------------------------------------------------------ the wiring

#ifdef GOBOT_SOURCE_DIR
std::string source(const std::string& rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string body(const std::string& src, const std::string& head) {
    const std::size_t from = src.find(head);
    if (from == std::string::npos) return {};
    const std::size_t end = src.find("\n}", from);
    return end == std::string::npos ? std::string() : src.substr(from, end + 2 - from);
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

/// Whether `first` comes before `then` in `text`, both there.
bool before(const std::string& text, const std::string& first, const std::string& then) {
    const std::size_t a = text.find(first);
    const std::size_t b = text.find(then);
    return a != std::string::npos && b != std::string::npos && a < b;
}
#endif

void test_vitals_wiring() {
    section("Vitals from the packets wired in: the potions, the jobs, the pace, the in-world hold (their source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string part = source("/apps/gobot_gui/packets_vitals.cpp");
    if (gui.empty() || part.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::string tick = body(gui, "void GuiApp::tick() {");

    // The packets' side before the in-world block; HP-back on the screen, as it was.
    check(before(tick, "vitals_.set_period_ms(vitals_pace());", "vitals_check_tick(now_ms());") &&
              before(tick, "vitals_check_tick(now_ms());", "const std::uint64_t frame_at = vitals_.last_read_ms();") &&
              before(tick, "const bool loading = packet_map_loading(now_ms());",
                     "VitalsReader::in_world(vitals_.hp_seen_ms(), frame_at, in_world_window_ms()) && !loading") &&
              has(tick, "SelfVitals packets = gate_vitals(now_ms());") &&
              has(tick, "const float hp = vitals_running_ ? vitals_.hp_percent() : -1.0f;"),
          "tick(): the pace, then the packets' side, then the in-world hold (the packets only adding a map load), "
          "and the gate's HP and MP; HP back from 0 as it was");

    // Usable asked afresh, with every part of it -- and nothing of the bars
    // (2026.11.17): the packets whenever they have the value.
    const std::string usable =
        body(part, "VitalsUsable usable_in(const GuiApp& app, bool hp_bar, const SelfNow& s) const {");
    const std::string hp_now = body(part, "float GuiApp::hp_percent_now() const {");
    const std::string mp_now = body(part, "float GuiApp::mp_percent_now() const {");
    const std::string gate = body(part, "SelfVitals GuiApp::gate_vitals(std::uint64_t now) {");
    check(has(usable, "app.packets_settings_.vitals != PacketUse::Off") && has(usable, "app.capture_.running()") &&
              has(usable, "app.capture_.start_count()") && has(usable, "s.v.hp_epoch") &&
              has(usable, "s.last_server_ms") && has(usable, "max_known(s.v)") && !has(usable, "trusted") &&
              !has(usable, "vitals_.running()") && has(hp_now, "vitals_usable(") &&
              has(hp_now, "const float screen = vitals_.hp_percent();") && has(mp_now, "vitals_usable(") &&
              has(mp_now, "const float screen = vitals_.mp_percent();") && has(gate, "vitals_usable(") &&
              has(gate, "if (!vitals_.running()) return out;") && !has(gate, "hand") &&
              !has(gate, "vitals_.mp_percent()") && !has(gate, "vitals_.hp_percent()"),
          "packets_vitals.cpp: usable from [Packets], the capture and its start count, the epoch, the server and a "
          "maximum, asked at each call -- no check against the bars, no hand-over of a read; the bars' HP and MP "
          "only otherwise");
    check(!has(part, "BarCheck") && !has(part, "HandOver") && !has(part, "mp_stands_in") &&
              !has(source("/include/gobot/vitals_trust.hpp"), "class BarCheck") &&
              !has(source("/include/gobot/vitals_trust.hpp"), "trusted"),
          "the gate of 2026.11.16b and its hand-over are gone, not merely bypassed");

    // Never another client's HP: usable only on the capture started for the
    // positions on the game window's own process, and nothing at all with
    // packet_positions=off -- whoever captures.
    const std::string checking = body(part, "void GuiApp::vitals_check_tick(std::uint64_t now) {");
    check(has(usable, "in.on_window = on_window.load();") && has(usable, "!app.packets_off_.load()") &&
              has(checking, "part.on_window.store(capture_.running() && position_capture_") &&
              has(checking, "position_link_.pid == pid_for_window(input_.target_window())") &&
              has(body(gui, "void GuiApp::positions_tick() {"),
                  "packets_off_.store(hunt_config_.packet_positions == PacketPositions::Off);"),
          "usable needs the positions' capture on the window's own process, worked out once a tick on GoBot's "
          "thread for any thread to read, and packet_positions not off");

    // To the screen and back said once a minute each way at most, the rest
    // counted: a gap in the capture flips a still HP every 10 s or so.
    check(has(part, "constexpr std::uint64_t kSwitchGapMs = 60'000;") &&
              has(checking, "now - said_ms[way] < kSwitchGapMs") && has(checking, "++unsaid[way];") &&
              has(checking, "\"Vitals: %s from the screen until the packets have it again -- %s%s\"") &&
              has(checking, "vitals_unusable_why(in, now)") &&
              has(checking, "\"Vitals: %s from the packets -- the potions, the jobs' HP guards and the pace%s\""),
          "a bar to the screen with the reason, to the packets at the first usable value: each way said once a "
          "minute at most, the switches counted");

    // Every HP/MP point reaches the log's comparison -- told apart by its
    // seq, not its time -- with the bars' own figures.
    const std::string minute = body(part, "void GuiApp::note_vitals_minute(std::uint64_t now) {");
    check(has(checking, "if (p.seq <= part.fed_seq) continue;") && has(checking, "if (v.points > part.fed_seq) {") &&
              has(checking, "part.hp_cmp.screen(hp_read);") && has(checking, "part.mp_cmp.settle(now, s.epoch);") &&
              has(checking, "++part.same_ms;") && has(minute, "part->same_ms") && has(minute, "counts(mc)") &&
              has(minute, "odd_text(\"MP\", part->mp_odd)") && has(minute, "rs.behind_fill_mp"),
          "vitals_check_tick sets every point since the last, by its seq, beside the bars' own figures; the minute "
          "line says what came of it, the last read not the same, and the figures refused short of the fill");

    // 2026.11.08: the MP caption read short of its leading 1, kept once a
    // session as a picture -- named apart from vitals_debug's own frames,
    // whose rotation never takes it -- and cut and written off this thread.
    const std::string keep = body(part, "    void keep_mp_caption(");
    const std::size_t async = keep.find("std::async(std::launch::async,");
    const auto in_async = [&keep, async](const char* part_of) {
        const std::size_t at = keep.find(part_of);
        return async != std::string::npos && at != std::string::npos && at > async;
    };
    check(has(checking, "if (!part.caption_kept && figure_lost_leading_one(o.read.figure, o.packet.value))") &&
              has(checking, "part.keep_mp_caption(vitals_, o.read.figure, o.packet.value, o.read.read_at_ms);") &&
              has(keep, "caption_kept = true;") && in_async("exe_relative(\"vitals_debug\")") &&
              in_async("save_png(cut,") && in_async("make_folder(dir)") && in_async("Frame cut;") &&
              has(keep, "\"mp_caption_%Y%m%d_%H%M%S.png\"") &&
              !has(keep, "vitals_%") && !has(keep, "remove_file(") &&
              has(keep, "VitalsReader::caption_box(bar)"),
          "packets_vitals.cpp: the MP bar and its caption kept once, off GoBot's thread, as "
          "vitals_debug\\mp_caption_*.png, never in the rotation of vitals_*.png");

    // Only figures a read made out itself are set beside the packets: one in
    // the caption's shadow goes by the kept ones (shadowed_percent), at their
    // percentage exactly, which alone would pass them off as its own.
    const std::string reader = source("/src/vitals_reader.cpp");
    const std::string loop = body(reader, "void VitalsReader::loop() {");
    check(has(loop, "bar.percent = shadowed_percent(bar.percent, bar.value, bar.max);") &&
              before(loop, "bar.now = false;", "if (++bar.misses >= forget_at) {") &&
              before(loop, "bar.misses = 0;", "bar.now = true;") && has(loop, "hp_pct, values_.hp_now, 0 }") &&
              has(loop, "mp_pct, values_.mp_now, 1 }") &&
              has(checking, "bar_read(before, hp_pct, figures.hp, figures.hp_max, figures.hp_now);") &&
              has(checking, "bar_read(before, mp_pct, figures.mp, figures.mp_max, figures.mp_now);") &&
              has(part, "if (now && pct >= 0.0f && figure_percent(value, max) == pct) {"),
          "the reader's loop: a miss is not this read's own, and in the caption's shadow goes by the kept figures; "
          "vitals_check_tick hands the comparison figures only with the read's own flag");
    // 2026.11.17: figures short of their fill refused as a hole is, before
    // the hole's own handling; MP's measure in its caption -- no figures
    // kept -- nothing, never HP's.
    check(before(loop, "if (read && figure_behind_fill(measured, cur, max)) {", "if (!read) {") &&
              before(loop, "if (read && !max_believed(max, bar.max, bar.pending, bar.seen)) {",
                     "if (read && figure_behind_fill(measured, cur, max)) {") &&
              has(loop, "if (bar.which == 1 && bar.percent >= kCaptionShadowPct) {") &&
              has(loop, "} else if (bar.which == 1 && bar.value < 0 && bar.percent >= kCaptionShadowPct) {") &&
              has(loop, "mp_covered_.store(covered);"),
          "the reader's loop: figures behind their fill refused like a hole; MP's caption covered with none kept "
          "reads as nothing, HP never so");

    // HP -1 -- not in the world -- stays -1 for the jobs' gates, whatever the
    // packets say; MP by HP's sight of the world: MP unread alone is its
    // caption covered, and the packets' MP goes over it.
    check(before(hp_now, "if (screen < 0.0f) return screen;", "vitals_usable(") &&
              before(mp_now, "if (vitals_.hp_percent() < 0.0f) return screen;", "vitals_usable(") &&
              has(hp_now, "    return screen;") && has(mp_now, "    return screen;"),
          "hp_percent_now gives the packets' value only while the bars' last read saw HP, mp_percent_now likewise: "
          "the quest's, the charms' and the shop's in-world gates as before");

    // The run's pace asked again the moment a bar goes back to the screen
    // while the reader rests.
    check(before(tick, "SelfVitals packets = gate_vitals(now_ms());",
                 "vitals_.period_ms() > vitals_run_period()") &&
              before(tick, "vitals_.period_ms() > vitals_run_period()",
                     "const std::uint64_t read_at = last >= vitals_.paced_from_ms() ? last : 0, at = now_ms();"),
          "a bar back on the screen while the reader rests cuts the pace before the bars' read is judged, so an "
          "old read goes as no reading");

    // 0x02F0's MP is your MP too, the newer going (2026.11.17).
    const std::string self = source("/src/packet_self.cpp");
    const std::size_t udp = self.find("case 0x02F0:", self.find("void PacketSelf::take("));
    const std::string udp_case = udp == std::string::npos ? std::string() : self.substr(udp, self.find("break;", udp) - udp);
    check(has(udp_case, "s.mp = *mp;") && has(udp_case, "s.mp_epoch = epoch_;") && has(udp_case, "if (s.hp_ms != 0) {"),
          "packet_self.cpp: 0x02F0's MP taken as your MP once a frame has given HP, in the epoch of now");

    // The resting pace: its reasons not to, from the window's own state.
    const std::string pace = body(part, "int GuiApp::vitals_pace() const {");
    const std::string rest = body(part, "VitalsRest rest_in(const GuiApp& app, std::uint64_t now) const {");
    check(has(pace, "vitals_rest_allowed(") &&
              has(pace, "vitals_rest_period_ms(vitals_run_period(), performance_.vitals_idle_ms, in.pet_key)") &&
              has(rest, "MacroKeyKind::PetPotion") && has(rest, "app.macro_->running()") && has(rest, "critical_active()") &&
              has(rest, "app.simple_find_bars_ || app.full_find_bars_") && has(rest, "app.hp_zero_since_ms_") &&
              has(rest, "app.death_seen_ms_.load()") && !has(part, "if (in.pet_key) return"),
          "vitals_pace rests only as vitals_rest_allowed says, with a critical hold, the bars looked for, HP at 0 "
          "and a death panel from GoBot's own state -- and with a pet potion key at its own quicker pace");

    // 2026.11.16: the landmark looked for once a second while lost; the
    // unreadable frames kept 20 at first, then one in 10 minutes.
    const std::string reader_hpp = source("/include/gobot/vitals_reader.hpp");
    const std::string keep_frame = body(reader, "void VitalsReader::keep_unreadable_frame(");
    check(has(loop, "if (lost && due) {") && has(loop, "anchor_looked_ms_ = look_at;") &&
              before(loop, "anchor_looked_ms_ = look_at;", "load_meters().anchor_searches.fetch_add(") &&
              has(reader_hpp, "static constexpr std::uint64_t kAnchorLookGapMs = 1000;") &&
              has(body(reader, "void VitalsReader::set_hp_bar(const BarSpec& s) {"), "anchor_looked_ms_ = 0;") &&
              has(body(reader, "void VitalsReader::set_anchor(const Frame& anchor) {"), "anchor_looked_ms_ = 0;"),
          "vitals_reader.cpp: the landmark looked for once a second at most while lost; at once after a bar or "
          "the landmark is given");
    check(has(keep_frame, "kept_frames_ < kFramesAtFirst ? kKeepFrameGapMs : kKeepFrameLaterGapMs") &&
              has(keep_frame, "++kept_frames_;") &&
              has(reader_hpp, "static constexpr std::size_t   kFramesAtFirst = 20;") &&
              has(reader_hpp, "static constexpr std::uint64_t kKeepFrameLaterGapMs = 600'000;"),
          "vitals_reader.cpp: frames HP was unreadable on, the first 20 of a run one in 10 s, then one in 10 "
          "minutes");

    // The reviver stamps the death panel, and says what the packets said.
    const std::size_t died = gui.find("reviver.on_death([this] {");
    const std::size_t died_end = died == std::string::npos ? std::string::npos : gui.find("});", died);
    const std::string on_death = died_end == std::string::npos ? std::string() : gui.substr(died, died_end - died);
    const std::size_t up = gui.find("reviver.on_revived([this] {");
    const std::size_t up_end = up == std::string::npos ? std::string::npos : gui.find("});", up);
    const std::string on_revived = up_end == std::string::npos ? std::string() : gui.substr(up, up_end - up);
    check(has(on_death, "death_seen_ms_ = now_ms();") && has(on_death, "packet_vitals_text()") &&
              has(on_death, "alert(\"death\", alert_text::death(true));") && has(on_revived, "++revive_count_;") &&
              has(on_revived, "death_seen_ms_ = 0;") &&
              has(gui, "reviver.set_hp([this] { return vitals_.hp_percent(); });"),
          "revive_loop: the death panel stamped and the packets' HP said beside it; a revive clears it; the "
          "reviver's HP still the bars'");
    const std::string alerts = source("/apps/gobot_gui/simple_alerts.cpp");
    check(has(body(alerts, "void GuiApp::log_black_box() {"), "packet_vitals_text()") &&
              has(body(gui, "void GuiApp::draw_vitals_panel() {"), "packet_vitals_text()"),
          "the black box and the HP / MP tab say what the packets say");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_vitals_trust(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_compare_one();
    test_compare_window();
    test_caption_shadow();
    test_reader_caption_shadow();
    test_usable();
    test_rest();
    test_world_window();
    test_map_loading();
    test_lost_leading_one();
    test_lost_a_digit();
    test_lost_leading_digits();
    test_figure_behind_fill();
    test_reader_exp_tip();
    test_reader_exp_tip_picture();
    test_vitals_wiring();
}
