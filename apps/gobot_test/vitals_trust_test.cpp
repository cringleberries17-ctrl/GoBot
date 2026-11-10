// gobot_test -- your HP and MP from the packets, checked against the bars on
// the screen before anything goes by them (the packets plan, item D): the gate
// (BarCheck), the read it hands over as it stops trusting them (HandOver), when
// a bar may be gone by (vitals_usable), when the HP/MP reader may rest, how
// long HP may go unread while it does (PeriodWindow, world_window_ms), the
// hold while a map loads (map_load_holds) -- and where GoBot's window wires
// them in, read off its source.
//
// Pure: every time is the test's own, on a clock that starts at kT0; nothing
// here goes near the game, a window, the network or the registry -- the HP/MP
// reader's thread, where it runs, reads frames the test draws (set_grab). HP
// 9232 and MP 8902 are the P0-2 recording's; HP 10084, MP 15280 and the
// replays' figures and times the farm log's of 2026-11-03 (22:36-22:54); every
// other number is made up.

#include "gobot/glyph_font.hpp"
#include "gobot/macro.hpp"
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

using State = BarCheck::State;

/// A bar and its clock: each step puts a packet value in at `t`, the bar's
/// read of it half a second on, and judges it once it is due -- kLeadMs on, as
/// a read of a trusted bar that does not agree waits that long -- then moves
/// the clock two seconds on, clear of the window either side of the read.
struct Bar {
    BarCheck      check;
    std::uint64_t t = kT0;
    std::uint32_t epoch = 1;
    std::uint32_t max = 9232;

    explicit Bar(bool hp) : check(hp) {}

    /// The bar's figures read as `figure` (the packets saying `value`), or
    /// with `figures` false only its length, `pct`.
    void step(std::uint32_t value, int figure, bool figures = true, float pct = -1.0f) {
        check.packet(BarCheck::Point{ t, value, max, true, epoch });
        BarCheck::Read r;
        r.read_at_ms = t + 500;
        r.pct = pct >= 0.0f ? pct : BarCheck::percent(static_cast<std::uint32_t>(figure), max);
        if (figures) {
            r.figure = figure;
            r.figure_max = static_cast<int>(max);
        }
        check.screen(r);
        check.settle(t + 500 + BarCheck::kLeadMs + BarCheck::kJudgeSlackMs, epoch);
        t += 2000;
    }
    void agree(std::uint32_t value) { step(value, static_cast<int>(value)); }
    /// A quarter of the bar off: beyond either bar's slack.
    void disagree(std::uint32_t value) { step(value, static_cast<int>(value) - static_cast<int>(max / 4)); }
    /// `n` agreements over three values, the lowest `low` -- under 98% or not.
    void agree_n(int n, std::uint32_t low) {
        const std::uint32_t values[] = { low, low + 1, low + 2 };
        for (int i = 0; i < n; ++i) agree(values[i % 3]);
    }
};

// ------------------------------------------------------------ the gate

void test_bar_trust() {
    section("Vitals gate: trusted after 20 agreements over 3 values; HP needs one under 98%");

    // MP: no low value needed.
    {
        Bar mp(false);
        mp.max = 8902;
        mp.agree_n(19, 8000);
        const State after19 = mp.check.state();
        mp.agree(8000);
        const auto changes = mp.check.take_changes();
        check(after19 == State::Learning && mp.check.state() == State::Trusted && changes.size() == 1 &&
                  changes[0].from == State::Learning && changes[0].to == State::Trusted &&
                  changes[0].agreements == 20 && changes[0].values == 3 && changes[0].exact == 20,
              "MP: learning after 19 agreements, trusted at the 20th, over 3 values, all exact");
    }
    {
        Bar mp(false);
        mp.max = 8902;
        for (int i = 0; i < 30; ++i) mp.agree(i % 2 == 0 ? 8000u : 8001u);
        check(mp.check.state() == State::Learning && mp.check.progress().values == 2,
              "30 agreements over 2 values: still learning -- 3 values are needed");
    }
    // HP: twenty agreements at full health are not enough.
    {
        Bar hp(true);
        hp.agree_n(24, 9230);       // 99.98% and up
        const State full = hp.check.state();
        const bool low_seen = hp.check.progress().low_seen;
        hp.agree(6100);             // 66%
        check(full == State::Learning && !low_seen && hp.check.state() == State::Trusted,
              "HP: 24 agreements all at 99% and more still learn; one at 66% trusts it");
    }
    // The figures must be the packets' value and maximum exactly.
    {
        BarCheck::Point p{ kT0, 7344, 9232, true, 1 };
        BarCheck::Read r{ kT0, 79.55f, 7344, 9232 };
        bool judged = false;
        const BarCheck::Verdict same = BarCheck::judge(p, r, judged);
        r.figure = 7343;
        const BarCheck::Verdict one_off = BarCheck::judge(p, r, judged);
        r.figure = 7344;
        r.figure_max = 9233;
        const BarCheck::Verdict max_off = BarCheck::judge(p, r, judged);
        p.max_known = false;
        p.max = 7400;
        const BarCheck::Verdict no_max = BarCheck::judge(p, r, judged);
        check(same == BarCheck::Verdict::Agree && one_off == BarCheck::Verdict::Disagree &&
                  max_off == BarCheck::Verdict::Disagree && no_max == BarCheck::Verdict::Agree,
              "by the figures: the same value and maximum agree; one off in either disagrees; a maximum the packets "
              "do not know is not held against them");
        // With a trusted bar's slack: one off is between, either way.
        const BarCheck::Point q{ kT0, 7344, 9232, true, 1 };
        const BarCheck::Read under{ kT0, BarCheck::percent(7343, 9232), 7343, 9232 };
        const BarCheck::Read over{ kT0, BarCheck::percent(7345, 9232), 7345, 9232 };
        check(BarCheck::judge(q, under, judged, BarCheck::kFigureSlackHpPct) == BarCheck::Verdict::Between &&
                  BarCheck::judge(q, over, judged, BarCheck::kFigureSlackHpPct) == BarCheck::Verdict::Between &&
                  BarCheck::judge(q, under, judged) == BarCheck::Verdict::Disagree,
              "with the HP slack, one off below or above is between; with none -- learning -- a disagreement");
    }
    // Without figures, by the bar: within 2 points, beyond 5, and between.
    {
        const BarCheck::Point p{ kT0, 5000, 10000, true, 1 };     // 50%
        const auto by_bar = [&p](float pct, bool& judged) {
            BarCheck::Read r;
            r.read_at_ms = kT0;
            r.pct = pct;
            return BarCheck::judge(p, r, judged);
        };
        bool j1 = false, j2 = false, j3 = false, j4 = false, j5 = false;
        const auto at2 = by_bar(52.0f, j1);
        const auto at3 = by_bar(47.0f, j2);
        const auto at5 = by_bar(55.0f, j3);
        const auto at55 = by_bar(44.5f, j4);
        BarCheck::Point unknown = p;
        unknown.max_known = false;
        BarCheck::Read r;
        r.read_at_ms = kT0;
        r.pct = 50.0f;
        BarCheck::judge(unknown, r, j5);
        check(at2 == BarCheck::Verdict::Agree && at3 == BarCheck::Verdict::Between &&
                  at5 == BarCheck::Verdict::Between && at55 == BarCheck::Verdict::Disagree && j1 && j2 && j3 && j4 &&
                  !j5,
              "by the bar: 2 points apart agrees, 3 and 5 are between, 5.5 disagrees; with no maximum nothing is "
              "judged");
    }
    // Between counts for nothing: no step towards trust.
    {
        Bar mp(false);
        mp.max = 10000;
        for (int i = 0; i < 25; ++i) mp.step(5000u + static_cast<std::uint32_t>(i % 3), -1, false, 46.5f);
        const BarCheck::Counts c = mp.check.take_counts();
        check(mp.check.state() == State::Learning && mp.check.progress().agreements == 0 && c.between == 25 &&
                  c.agree == 0 && c.disagree == 0,
              "25 reads 3.5 points off: all between, no agreement counted, still learning");
    }
}

void test_bar_filter() {
    section("Vitals gate: a read judged only with the packet value still around it, of the current epoch");
    // The value moved 350 ms after the read: nothing said either way.
    {
        BarCheck b(false);
        b.packet(BarCheck::Point{ kT0, 8000, 8902, true, 1 });
        b.packet(BarCheck::Point{ kT0 + 1350, 7000, 8902, true, 1 });
        b.screen(BarCheck::Read{ kT0 + 1000, BarCheck::percent(7000, 8902), 7000, 8902 });
        b.settle(kT0 + 1000 + BarCheck::kHalfWindowMs, 1);
        const BarCheck::Counts early = b.take_counts();
        b.settle(kT0 + 1000 + BarCheck::kHalfWindowMs + BarCheck::kJudgeSlackMs, 1);
        const BarCheck::Counts due = b.take_counts();
        check(early.moving == 0 && early.agree + early.disagree == 0 && due.moving == 1 && due.agree == 0 &&
                  due.disagree == 0,
              "a value that changed 350 ms after the read: not judged at 400 ms (frames still on their way), "
              "and not judged at all once due -- it moved");
    }
    // The value from before a gap: stale.
    {
        BarCheck b(false);
        b.packet(BarCheck::Point{ kT0, 8000, 8902, true, 1 });
        b.screen(BarCheck::Read{ kT0 + 2000, BarCheck::percent(8000, 8902), 8000, 8902 });
        b.settle(kT0 + 3000, 2);
        const BarCheck::Counts c = b.take_counts();
        check(c.stale == 1 && c.agree == 0, "a value of an older epoch, the packets now in the next: not judged");
    }
    // No value before the read's window.
    {
        BarCheck b(false);
        b.packet(BarCheck::Point{ kT0 + 900, 8000, 8902, true, 1 });
        b.screen(BarCheck::Read{ kT0 + 1000, BarCheck::percent(8000, 8902), 8000, 8902 });
        b.settle(kT0 + 2000, 1);
        const BarCheck::Counts c = b.take_counts();
        check(c.no_packet == 1, "no value in force 400 ms before the read: not judged");
    }
    // A read with no figures while the font is there: not judged by the
    // bar's length where the caption paints its end out, which reads a full
    // bar short -- and judged under that, where it measures true: packets
    // stuck at full while the bar shows 40% are caught.
    {
        BarCheck b(false);
        b.set_figures_only(true);
        b.packet(BarCheck::Point{ kT0, 8902, 8902, true, 1 });
        BarCheck::Read r;
        r.read_at_ms = kT0 + 1000;
        r.pct = 66.0f;
        b.screen(r);
        b.settle(kT0 + 2000, 1);
        const BarCheck::Counts full = b.take_counts();
        r.read_at_ms = kT0 + 3000;
        r.pct = 40.0f;
        b.screen(r);
        b.settle(kT0 + 4000, 1);
        const BarCheck::Counts low = b.take_counts();
        check(full.no_figures == 1 && full.disagree == 0 && low.no_figures == 0 && low.disagree == 1,
              "figures only: the bar's length alone, 66% of a full bar, is not held against the packets; 40% of it, "
              "left of the caption, is");
    }
    // Two values in one millisecond -- one segment of the capture: the later
    // is the one in force, as it is the potions'.
    {
        BarCheck later(false), earlier(false);
        for (BarCheck* b : { &later, &earlier }) {
            b->packet(BarCheck::Point{ kT0, 8000, 8902, true, 1 });
            b->packet(BarCheck::Point{ kT0, 5000, 8902, true, 1 });
        }
        later.screen(BarCheck::Read{ kT0 + 1000, BarCheck::percent(5000, 8902), 5000, 8902 });
        earlier.screen(BarCheck::Read{ kT0 + 1000, BarCheck::percent(8000, 8902), 8000, 8902 });
        const std::uint64_t due = kT0 + 1000 + BarCheck::kLeadMs + BarCheck::kJudgeSlackMs;
        later.settle(due, 1);
        earlier.settle(due, 1);
        const BarCheck::Counts a = later.take_counts();
        const BarCheck::Counts d = earlier.take_counts();
        check(a.agree == 1 && a.disagree == 0 && d.agree == 0 && d.disagree == 1,
              "8000 then 5000 in the same millisecond: a read of 5000 agrees, one of 8000 disagrees -- the later of "
              "the two is the one in force");
    }
}

/// A bar trusted by twenty-one agreements over three values, one under 98%.
Bar trusted_hp() {
    Bar hp(true);
    hp.agree_n(20, 9100);
    hp.agree(6100);
    hp.check.take_changes();
    hp.check.take_counts();
    return hp;
}

void test_bar_doubt() {
    section("Vitals gate: one disagreement doubts it, 5 agreements in a row clear it, 2 in a row distrust it");
    {
        Bar hp = trusted_hp();
        const bool was = hp.check.trusted();
        hp.disagree(7344);
        const auto doubted = hp.check.take_changes();
        for (int i = 0; i < 4; ++i) hp.agree(7344);
        const State after4 = hp.check.state();
        hp.agree(7344);
        const auto cleared = hp.check.take_changes();
        check(was && doubted.size() == 1 && doubted[0].from == State::Trusted && doubted[0].to == State::Doubt &&
                  doubted[0].read.figure == 7344 - static_cast<int>(hp.max / 4) && doubted[0].packet.value == 7344 &&
                  after4 == State::Doubt &&
                  hp.check.state() == State::Trusted && cleared.size() == 1 && cleared[0].to == State::Trusted,
              "trusted, one disagreement: in doubt, the read it disagreed with kept; 4 agreements still in doubt, "
              "the 5th trusts it again");
    }
    {
        Bar hp = trusted_hp();
        hp.disagree(7344);
        hp.agree(7344);
        hp.agree(7344);
        hp.agree(7344);
        hp.disagree(7344);       // 2 among the last 20, not in a row: still in doubt, the row begun again
        const State twice = hp.check.state();
        const int row = hp.check.progress().doubt_agreements;
        hp.agree(7344);
        hp.disagree(7344);       // the third among the last 20
        const auto changes = hp.check.take_changes();
        check(twice == State::Doubt && row == 0 && hp.check.state() == State::Learning && !changes.empty() &&
                  changes.back().to == State::Learning && !changes.back().in_a_row &&
                  hp.check.progress().needed == BarCheck::kRelearnAgreements && hp.check.progress().distrusts == 1,
              "a disagreement in doubt, not in a row, keeps it in doubt and starts its 5 again; the third among "
              "the last 20 distrusts it: learning, 50 needed");
    }
}

void test_bar_distrust() {
    section("Vitals gate: distrusted, relearnt over 50 agreements; off after 3 distrusts");
    // Each pair two packet values: two HUD moments (test_bar_episode).
    Bar hp = trusted_hp();
    hp.disagree(7344);
    hp.disagree(7300);
    const auto first = hp.check.take_changes();
    const bool learning = hp.check.state() == State::Learning;
    hp.agree_n(48, 9100);
    hp.agree(6100);
    const State after49 = hp.check.state();
    hp.agree(6100);
    check(first.size() == 2 && first[1].in_a_row && first[1].distrusts == 1 && learning && after49 == State::Learning &&
              hp.check.state() == State::Trusted,
          "2 disagreements in a row: learning again; 49 agreements still learn, the 50th trusts it");
    hp.check.take_changes();

    hp.disagree(7344);
    hp.disagree(7300);           // the second distrust
    hp.agree_n(49, 9100);
    hp.agree(6100);
    const bool back = hp.check.trusted();
    hp.disagree(7344);
    hp.disagree(7300);           // the third
    const auto last = hp.check.take_changes();
    hp.agree_n(60, 9100);
    hp.agree(6100);
    check(back && hp.check.state() == State::Off && !last.empty() && last.back().to == State::Off &&
              last.back().distrusts == BarCheck::kDistrustsOff,
          "relearnt after the second distrust; off at the third, and 61 more agreements leave it off for the session");
}

void test_bar_over_max() {
    section("Vitals gate: HP above the stat sheet's maximum is a disagreement");
    Bar hp = trusted_hp();
    hp.check.over_max(hp.t);
    const auto changes = hp.check.take_changes();
    const BarCheck::Counts c = hp.check.take_counts();
    check(hp.check.state() == State::Doubt && changes.size() == 1 && changes[0].over_max &&
              changes[0].read.pct < 0.0f && c.over_max == 1 && c.disagree == 1,
          "trusted HP, then HP above the maximum: in doubt, with no read of the bars to hand over");
}

void test_bar_empty() {
    section("Vitals gate: with the font, an empty bar with no figures -- the HUD dimmed -- is not judged");
    // Two empty reads in a row, no figures, as the fade into a map load.
    {
        Bar hp = trusted_hp();
        hp.check.set_figures_only(true);
        hp.step(6100, -1, false, 0.0f);
        hp.step(6100, -1, false, 0.0f);
        const auto changes = hp.check.take_changes();
        const BarCheck::Counts c = hp.check.take_counts();
        check(hp.check.state() == State::Trusted && changes.empty() && c.empty == 2 && c.disagree == 0 &&
                  c.agree == 0,
              "with the font: two empty reads with no figures are not judged -- HP stays trusted, counted as empty");
    }
    // No font: the empty bar is the only word the screen has, and is judged --
    // the two reads against one packet value as one HUD moment, the next
    // value's the second disagreement in a row.
    {
        Bar hp = trusted_hp();
        hp.check.set_figures_only(false);
        hp.step(6100, -1, false, 0.0f);
        hp.step(6100, -1, false, 0.0f);
        const State one = hp.check.state();
        hp.step(6050, -1, false, 0.0f);
        const auto changes = hp.check.take_changes();
        const BarCheck::Counts c = hp.check.take_counts();
        check(one == State::Doubt && hp.check.state() == State::Learning && !changes.empty() &&
                  changes.back().in_a_row && c.empty == 0 && c.disagree == 3 && c.same_episode == 1,
              "with no font: all judged -- two empty reads against one packet value one disagreement (in doubt), the "
              "next value's the second in a row: distrusted, learning again");
    }
    // An empty bar that carries its figures, 0/9232: judged exactly.
    {
        Bar hp = trusted_hp();
        hp.check.set_figures_only(true);
        hp.step(6100, 0, true);
        const auto changes = hp.check.take_changes();
        const BarCheck::Counts c = hp.check.take_counts();
        check(hp.check.state() == State::Doubt && changes.size() == 1 && changes[0].to == State::Doubt &&
                  c.empty == 0 && c.disagree == 1,
              "the figures 0/9232 against the packets' 6100: a disagreement, trusted -> in doubt");
    }
    // A sliver, 3%, with no figures: still judged by the bar's length.
    {
        Bar hp = trusted_hp();
        hp.check.set_figures_only(true);
        hp.step(6100, -1, false, 3.0f);
        const auto changes = hp.check.take_changes();
        const BarCheck::Counts c = hp.check.take_counts();
        check(hp.check.state() == State::Doubt && changes.size() == 1 && c.empty == 0 && c.disagree == 1,
              "3% with no figures against the packets' 66%: judged, a disagreement, trusted -> in doubt");
    }
    // MP the same.
    {
        Bar mp(false);
        mp.max = 8902;
        mp.agree_n(20, 8000);
        const bool trusted = mp.check.trusted();
        mp.check.take_changes();
        mp.check.take_counts();
        mp.check.set_figures_only(true);
        mp.step(8000, -1, false, 0.0f);
        mp.step(8000, -1, false, 0.0f);
        const BarCheck::Counts c = mp.check.take_counts();
        check(trusted && mp.check.trusted() && mp.check.take_changes().empty() && c.empty == 2 && c.disagree == 0,
              "MP with the font: two empty reads with no figures not judged, still trusted");
    }
}

void test_bar_disagreements() {
    section("Vitals gate: each disagreement kept for the log, with the state it was judged in");
    {
        BarCheck b(false);
        b.packet(BarCheck::Point{ kT0, 8000, 8902, true, 1 });
        b.screen(BarCheck::Read{ kT0 + 1000, BarCheck::percent(6000, 8902), 6000, 8902 });
        b.settle(kT0 + 1000 + BarCheck::kLeadMs + BarCheck::kJudgeSlackMs, 1);
        const std::vector<BarCheck::Disagreement> first = b.take_disagreements();
        const std::vector<BarCheck::Disagreement> again = b.take_disagreements();
        check(first.size() == 1 && first[0].read.figure == 6000 && first[0].packet.value == 8000 &&
                  first[0].state == State::Learning && again.empty(),
              "a read of 6000 against the packets' 8000, learning: kept with both and the state, taken once");
    }
    {
        Bar mp(false);
        mp.max = 8902;
        mp.agree(8000);
        check(mp.check.take_disagreements().empty(), "an agreement keeps none");
    }
    {
        Bar mp(false);
        mp.max = 8902;
        for (int i = 0; i < 10; ++i) mp.disagree(8000u + static_cast<std::uint32_t>(i));
        const std::vector<BarCheck::Disagreement> kept = mp.check.take_disagreements();
        check(kept.size() == BarCheck::kDisagreementsKept && kept.front().packet.value == 8002 &&
                  kept.back().packet.value == 8009,
              "10 disagreements, each against a value still around its read: the last " +
                  num(static_cast<long long>(BarCheck::kDisagreementsKept)) + " kept, the newest last");
    }
    {
        BarCheck hp(true);
        hp.over_max(kT0);
        check(hp.take_disagreements().empty() && hp.take_counts().disagree == 1,
              "HP above the stat sheet's maximum: a disagreement with no read, so none kept for the log");
    }
}

// ------------------------------------------------------------ the HUD ahead of 0x0051

/// A bar of the farm log's maxima -- HP 10084, MP 15280 -- trusted by 20
/// agreements at half the bar.
Bar trusted_bar(bool is_hp, std::uint32_t max) {
    Bar b(is_hp);
    b.max = max;
    b.agree_n(20, max / 2);
    b.check.take_changes();
    b.check.take_counts();
    return b;
}

void test_bar_hud_leads() {
    section("Vitals gate: the HUD ahead of 0x0051 by a hit or a cast's cost is between, never a disagreement");
    {   // the log's HP pairs: 5343/5342, 8328/8327 x3, 6802/6525, 8064/7757 x2
        Bar hp = trusted_bar(true, 10084);
        const bool was = hp.check.trusted();
        const std::uint32_t pk[] = { 5343, 8328, 8328, 8328, 6802, 8064, 8064 };
        const int bars[] = { 5342, 8327, 8327, 8327, 6525, 7757, 7757 };
        for (int i = 0; i < 7; ++i) hp.step(pk[i], bars[i]);
        const BarCheck::Counts c = hp.check.take_counts();
        check(was && hp.check.trusted() && hp.check.take_changes().empty() && c.between == 7 && c.disagree == 0,
              "HP 10084: one point and one hit (2.75%, 3.04%) off are between -- trusted throughout");
    }
    {
        Bar hp = trusted_bar(true, 10084);
        hp.step(8064, 8064 - 600);   // 5.95%: two hits
        check(hp.check.state() == State::Doubt, "HP 600 of 10084 off: a disagreement, trusted -> in doubt");
    }
    {   // the log's MP pairs: 857, 1576, 857, 857, 1612, 1612, 573 of 15280
        Bar mp = trusted_bar(false, 15280);
        const bool was = mp.check.trusted();
        const std::uint32_t pk[] = { 15280, 8691, 9839, 10244, 8657, 12643, 9294 };
        const int bars[] = { 14423, 7115, 8982, 9387, 7045, 11031, 8721 };
        for (int i = 0; i < 7; ++i) mp.step(pk[i], bars[i]);
        const BarCheck::Counts c = mp.check.take_counts();
        check(was && mp.check.trusted() && mp.check.take_changes().empty() && c.between == 7 && c.disagree == 0,
              "MP 15280: a cast or two off (up to 10.55%) is between -- not off for the session as at 22:44:29");
    }
    {
        Bar mp = trusted_bar(false, 15280);
        mp.step(12643, 12643 - 2000);   // 13.1%
        check(mp.check.state() == State::Doubt, "MP 2000 of 15280 off: a disagreement");
    }
    {
        BarCheck::Point p{ kT0, 6705, 10084, true, 1 };
        BarCheck::Read r{ kT0, BarCheck::percent(6704, 10084), 6704, 10085 };
        bool judged = false;
        check(BarCheck::judge(p, r, judged, BarCheck::kFigureSlackHpPct) == BarCheck::Verdict::Disagree,
              "one point off with another maximum: a disagreement, whatever the slack");
    }
    // In doubt the lead is between too: it neither steps the bar back nor
    // breaks the agreements in a row that clear the doubt.
    {
        Bar mp = trusted_bar(false, 15280);
        mp.disagree(12000);
        const State doubted = mp.check.state();
        mp.agree(12001);
        mp.agree(12002);
        mp.step(12643, 11031);       // 1612: a cast's cost
        mp.agree(12003);
        mp.agree(12004);
        const State after4 = mp.check.state();
        mp.step(9294, 8721);         // 573
        mp.agree(12005);
        const BarCheck::Counts c = mp.check.take_counts();
        check(doubted == State::Doubt && after4 == State::Doubt && mp.check.trusted() && c.between == 2 &&
                  c.disagree == 1,
              "in doubt, reads a cast's cost off are between: 4 agreements and two of them still in doubt, the 5th "
              "agreement in a row trusts it again");
    }
}

void test_bar_caught_up() {
    section("Vitals gate: figures 0x0051 reaches within kLeadMs after the read agree");
    {   // 22:49:20: bars 6704; 0x0051 6705 0.9 s before the read, 6704 0.5 s after
        Bar hp = trusted_bar(true, 10084);
        const std::uint64_t t = hp.t;
        hp.check.packet(BarCheck::Point{ t, 6705, 10084, true, 1 });
        hp.check.screen(BarCheck::Read{ t + 900, BarCheck::percent(6704, 10084), 6704, 10084 });
        hp.check.settle(t + 900 + BarCheck::kHalfWindowMs + BarCheck::kJudgeSlackMs, 1);
        const BarCheck::Counts early = hp.check.take_counts();
        hp.check.packet(BarCheck::Point{ t + 1400, 6704, 10084, true, 1 });
        hp.check.settle(t + 900 + BarCheck::kLeadMs + BarCheck::kJudgeSlackMs, 1);
        const BarCheck::Counts late = hp.check.take_counts();
        check(early.agree + early.disagree + early.between == 0 && late.agree == 1 && late.exact == 1 &&
                  late.caught_up == 1 && hp.check.trusted(),
              "a read 1 under the packets waits kLeadMs; 0x0051 reaches it 0.5 s later: an exact agreement, caught up");
    }
    {   // two hits off (6.0%), reached 0.5 s after the read
        Bar hp = trusted_bar(true, 10084);
        const std::uint64_t t = hp.t;
        hp.check.packet(BarCheck::Point{ t, 8064, 10084, true, 1 });
        hp.check.screen(BarCheck::Read{ t + 600, BarCheck::percent(7457, 10084), 7457, 10084 });
        hp.check.packet(BarCheck::Point{ t + 1100, 7457, 10084, true, 1 });
        hp.check.settle(t + 600 + BarCheck::kLeadMs + BarCheck::kJudgeSlackMs, 1);
        const BarCheck::Counts c = hp.check.take_counts();
        check(c.agree == 1 && c.caught_up == 1 && c.disagree == 0 && hp.check.trusted(),
              "6% off, reached 0.5 s after the read: agrees");
    }
    {   // the same, reached only 1.2 s after the read: judged 1.0 s after it, not 0.5
        Bar hp = trusted_bar(true, 10084);
        const std::uint64_t t = hp.t;
        hp.check.packet(BarCheck::Point{ t, 8064, 10084, true, 1 });
        hp.check.screen(BarCheck::Read{ t + 600, BarCheck::percent(7457, 10084), 7457, 10084 });
        hp.check.settle(t + 600 + BarCheck::kHalfWindowMs + BarCheck::kJudgeSlackMs, 1);
        const State waiting = hp.check.state();
        const BarCheck::Counts early = hp.check.take_counts();
        hp.check.packet(BarCheck::Point{ t + 1800, 7457, 10084, true, 1 });
        hp.check.settle(t + 1900, 1);
        const BarCheck::Counts c = hp.check.take_counts();
        check(waiting == State::Trusted && early.disagree == 0 && c.disagree == 1 && c.caught_up == 0 &&
                  hp.check.state() == State::Doubt,
              "reached 1.2 s after the read: too late, a disagreement -- said 1.0 s after the read, not 0.5");
    }
    {   // the next 0x0051 jumps past the bars (22:51:07.909: bars 5644, 7256 then 5071): no agreement
        Bar mp = trusted_bar(false, 15280);
        const std::uint64_t t = mp.t;
        mp.check.packet(BarCheck::Point{ t, 7256, 15280, true, 1 });
        mp.check.screen(BarCheck::Read{ t + 800, BarCheck::percent(5644, 15280), 5644, 15280 });
        mp.check.packet(BarCheck::Point{ t + 1300, 5071, 15280, true, 1 });
        mp.check.settle(t + 800 + BarCheck::kLeadMs + BarCheck::kJudgeSlackMs, 1);
        const BarCheck::Counts c = mp.check.take_counts();
        check(c.between == 1 && c.agree == 0 && c.caught_up == 0 && mp.check.trusted(),
              "0x0051 overshooting the bars' 5644 to 5071: not caught up, but 1612 off is between -- still trusted");
    }
    {   // another epoch's value, or another maximum, reaching the figures is no agreement
        Bar hp = trusted_bar(true, 10084);
        const std::uint64_t t = hp.t;
        hp.check.packet(BarCheck::Point{ t, 8064, 10084, true, 1 });
        hp.check.screen(BarCheck::Read{ t + 600, BarCheck::percent(7457, 10084), 7457, 10084 });
        hp.check.packet(BarCheck::Point{ t + 1100, 7457, 10084, true, 2 });
        hp.check.packet(BarCheck::Point{ t + 1200, 7457, 11000, true, 1 });
        hp.check.settle(t + 600 + BarCheck::kLeadMs + BarCheck::kJudgeSlackMs, 1);
        const BarCheck::Counts c = hp.check.take_counts();
        check(c.disagree == 1 && c.caught_up == 0 && hp.check.state() == State::Doubt,
              "the figures reached by a value of another epoch, or of another maximum: not caught up");
    }
}

void test_bar_episode() {
    section("Vitals gate: reads of one HUD moment against one packet value are one disagreement");
    Bar mp = trusted_bar(false, 15280);
    const std::uint64_t t = mp.t;
    mp.check.packet(BarCheck::Point{ t, 12643, 15280, true, 1 });
    for (std::uint64_t i = 0; i < 3; ++i)
        mp.check.screen(BarCheck::Read{ t + 500 + 165 * i, BarCheck::percent(9000, 15280), 9000, 15280 });
    mp.check.settle(t + 830 + BarCheck::kLeadMs + BarCheck::kJudgeSlackMs, 1);
    const auto one = mp.check.take_changes();
    const BarCheck::Counts c = mp.check.take_counts();
    const State after = mp.check.state();
    mp.check.packet(BarCheck::Point{ t + 2000, 12000, 15280, true, 1 });
    mp.check.screen(BarCheck::Read{ t + 2500, BarCheck::percent(8357, 15280), 8357, 15280 });
    mp.check.settle(t + 2500 + BarCheck::kLeadMs + BarCheck::kJudgeSlackMs, 1);
    check(after == State::Doubt && one.size() == 1 && c.disagree == 3 && c.same_episode == 2 &&
              mp.check.state() == State::Learning && mp.check.progress().distrusts == 1,
          "three reads 165 ms apart, one figure, one packet value: one disagreement (in doubt); the next value's "
          "disagreement is the second in a row -- distrusted");

    // An agreement between ends the moment: the same again is a new one.
    Bar hp = trusted_bar(true, 10084);
    const std::uint64_t h = hp.t;
    hp.check.packet(BarCheck::Point{ h, 8000, 10084, true, 1 });
    hp.check.screen(BarCheck::Read{ h + 500, BarCheck::percent(6000, 10084), 6000, 10084 });
    hp.check.screen(BarCheck::Read{ h + 665, BarCheck::percent(8000, 10084), 8000, 10084 });
    hp.check.screen(BarCheck::Read{ h + 830, BarCheck::percent(6000, 10084), 6000, 10084 });
    hp.check.settle(h + 830 + BarCheck::kLeadMs + BarCheck::kJudgeSlackMs, 1);
    const BarCheck::Counts e = hp.check.take_counts();
    check(e.disagree == 2 && e.same_episode == 0 && e.agree == 1 && hp.check.state() == State::Doubt,
          "a disagreement, an agreement, the first disagreement's read again: two disagreements, not in a row -- "
          "still in doubt");

    // A 0x0051 that moved only HP gives MP a point too, its value the same at a
    // new time: the next read is judged against that point, and is the same
    // moment all the same.
    Bar m2 = trusted_bar(false, 15280);
    const std::uint64_t u = m2.t;
    m2.check.packet(BarCheck::Point{ u, 12643, 15280, true, 1 });
    m2.check.packet(BarCheck::Point{ u + 200, 12643, 15280, true, 1 });
    m2.check.screen(BarCheck::Read{ u + 500, BarCheck::percent(10643, 15280), 10643, 15280 });
    m2.check.screen(BarCheck::Read{ u + 665, BarCheck::percent(10643, 15280), 10643, 15280 });
    m2.check.settle(u + 665 + BarCheck::kLeadMs + BarCheck::kJudgeSlackMs, 1);
    const BarCheck::Counts f = m2.check.take_counts();
    check(f.disagree == 2 && f.same_episode == 1 && m2.check.state() == State::Doubt &&
              m2.check.progress().distrusts == 0,
          "two reads 165 ms apart against 12643, the second judged against a point a 0x0051 for HP alone gave MP: "
          "one disagreement -- in doubt, not distrusted");
}

void test_bar_learning_exact() {
    section("Vitals gate: trust earned as hard as ever -- while learning, no slack and no waiting for 0x0051");
    {
        Bar mp(false);
        mp.max = 15280;
        mp.agree_n(19, 8000);
        mp.step(8691, 8691 - 857);   // a cast's cost off, as the HUD leads
        const BarCheck::Counts c = mp.check.take_counts();
        mp.agree_n(19, 8000);
        const State after19 = mp.check.state();
        mp.agree(8000);
        check(c.disagree == 1 && c.between == 0 && after19 == State::Learning && mp.check.trusted(),
              "learning: 857 of 15280 off is a disagreement, as before -- 19 agreements after it still learn, the "
              "20th, with it out of the last 20, trusts it");
    }
    {
        Bar mp(false);
        mp.max = 15280;
        const std::uint64_t t = mp.t;
        mp.check.packet(BarCheck::Point{ t, 8691, 15280, true, 1 });
        mp.check.screen(BarCheck::Read{ t + 600, BarCheck::percent(7834, 15280), 7834, 15280 });
        mp.check.settle(t + 600 + BarCheck::kHalfWindowMs + BarCheck::kJudgeSlackMs, 1);
        const BarCheck::Counts at_once = mp.check.take_counts();
        mp.check.packet(BarCheck::Point{ t + 1100, 7834, 15280, true, 1 });
        mp.check.settle(t + 600 + BarCheck::kLeadMs + BarCheck::kJudgeSlackMs, 1);
        const BarCheck::Counts later = mp.check.take_counts();
        check(at_once.disagree == 1 && at_once.caught_up == 0 && later.agree + later.disagree + later.between == 0 &&
                  mp.check.progress().agreements == 0,
              "learning: judged 0.5 s after the read, as before; 0x0051 reaching its figures 0.5 s after it makes no "
              "agreement");
    }
}

/// One line of the farm log replayed: a packet value (0x0051) or a read of the
/// bars -- with its figures, or with `figure` -1 only its length -- `at` ms
/// after the bar's clock.
struct Logged {
    std::uint64_t at = 0;
    bool          packet = false;
    std::uint32_t value = 0;
    int           figure = -1;
    float         pct = -1.0f;
};
/// 0x0051 sent `value`; the bars shown with `figure` printed, or only `pct` long.
Logged sent(std::uint64_t at, std::uint32_t value) { return Logged{ at, true, value, -1, -1.0f }; }
Logged shown(std::uint64_t at, int figure) { return Logged{ at, false, 0, figure, -1.0f }; }
Logged shown_pct(std::uint64_t at, float pct) { return Logged{ at, false, 0, -1, pct }; }

/// `log` played into `b` as GoBot's window feeds it -- each line once its time
/// has come, the check settled every 30 ms -- until `until` ms after the bar's
/// clock; then the clock moved 2 s past it. What was judged.
BarCheck::Counts replay(Bar& b, const std::vector<Logged>& log, std::uint64_t until) {
    const std::uint64_t t0 = b.t;
    std::size_t i = 0;
    for (std::uint64_t now = t0; now <= t0 + until; now += 30) {
        for (; i < log.size() && t0 + log[i].at <= now; ++i) {
            const Logged& e = log[i];
            if (e.packet) {
                b.check.packet(BarCheck::Point{ t0 + e.at, e.value, b.max, true, b.epoch });
                continue;
            }
            BarCheck::Read r;
            r.read_at_ms = t0 + e.at;
            r.pct = e.figure >= 0 ? BarCheck::percent(static_cast<std::uint32_t>(e.figure), b.max) : e.pct;
            if (e.figure >= 0) {
                r.figure = e.figure;
                r.figure_max = static_cast<int>(b.max);
            }
            b.check.screen(r);
        }
        b.check.settle(now, b.epoch);
    }
    b.t = t0 + until + 2000;
    return b.check.take_counts();
}

void test_bar_farm_replay() {
    section("Vitals gate: the farm log's disagreements of 2026-11-03 replayed -- none now, and no distrust");
    // HP: all 8 written disagreements, 5 Doubts and 2 distrusts then, 32 s on
    // the screen. Times from the packet value each was judged against.
    {
        Bar hp = trusted_bar(true, 10084);
        hp.check.set_figures_only(true);
        // 22:38:16.800: 5342 against 5343, 0.8 s old.
        const BarCheck::Counts a = replay(hp, { sent(0, 5343), shown(800, 5342) }, 2500);
        // 22:42:23.108 / .272 / .438: 8327 against one 8328 -- a distrust then.
        const BarCheck::Counts b =
            replay(hp, { sent(0, 8328), shown(500, 8327), shown(664, 8327), shown(830, 8327) }, 2500);
        // 22:43:18.720: 6525 against 6802, one hit.
        const BarCheck::Counts c = replay(hp, { sent(0, 6802), shown(800, 6525) }, 2500);
        // 22:49:20.387: 6704 against 6705; 6704 came 0.5 s after the read.
        const BarCheck::Counts d = replay(hp, { sent(0, 6705), shown(900, 6704), sent(1400, 6704) }, 2500);
        // 22:51:07.745 / .909: 7757 against one 8064; 7757 came 0.5 s after the second -- a distrust then.
        const BarCheck::Counts e =
            replay(hp, { sent(0, 8064), shown(600, 7757), shown(764, 7757), sent(1264, 7757) }, 2500);
        check(a.between == 1 && b.between == 3 && c.between == 1 && d.agree == 1 && d.caught_up == 1 &&
                  e.agree == 2 && e.caught_up == 2 &&
                  a.disagree + b.disagree + c.disagree + d.disagree + e.disagree == 0 && hp.check.trusted() &&
                  hp.check.take_changes().empty(),
              "HP: 5 between, 3 caught up (22:49:20.387, 22:51:07.745 and .909), 0 disagreements -- trusted "
              "throughout, never on the screen");
    }
    // MP: Off at 22:44:29.222 then, the screen deciding 91% of the hunt.
    {
        Bar mp = trusted_bar(false, 15280);
        mp.check.set_figures_only(true);
        // 22:38:16.473 / .634 / .800: 7115 against one 8691 -- the first distrust.
        const BarCheck::Counts a =
            replay(mp, { sent(0, 8691), shown(500, 7115), shown(661, 7115), shown(827, 7115) }, 2500);
        // 22:40:59.388, then 22:41:02.342 / .503: 857 under two values -- the second.
        const BarCheck::Counts b = replay(
            mp, { sent(0, 9839), shown(600, 8982), sent(3054, 10244), shown(3554, 9387), shown(3715, 9387) }, 5000);
        // 22:44:28.552 / .715, 56% (no figures) at .881, 29.047: 11031 against
        // one 12643, sent again at 29.1 and 29.25 -- Off for the session.
        const BarCheck::Counts c = replay(mp,
                                          { sent(0, 12643), shown(500, 11031), shown(663, 11031),
                                            shown_pct(829, 56.0f), shown(995, 11031), sent(1063, 12643),
                                            sent(1195, 12643) },
                                          2500);
        // 22:48:27.218 / .382: 8691 against 9548; 8691 came 0.5 s after the second.
        const BarCheck::Counts d =
            replay(mp, { sent(0, 9548), shown(600, 8691), shown(764, 8691), sent(1264, 8691) }, 2500);
        // 22:51:07.909: 5644 against 7256; the next was 5071, past the bars.
        const BarCheck::Counts e = replay(mp, { sent(0, 7256), shown(800, 5644), sent(1300, 5071) }, 2500);
        // 22:52:04.875 / 05.040 / 05.205: 11358 against 12215; 11358 came 0.4 s after the last.
        const BarCheck::Counts f = replay(
            mp, { sent(0, 12215), shown(500, 11358), shown(665, 11358), shown(830, 11358), sent(1245, 11358) }, 2500);
        check(a.between == 3 && b.between == 3 && c.between == 3 && c.no_figures == 1 && d.agree == 2 &&
                  d.caught_up == 2 && e.between == 1 && f.agree == 3 && f.caught_up == 3 &&
                  a.disagree + b.disagree + c.disagree + d.disagree + e.disagree + f.disagree == 0 &&
                  mp.check.trusted() && mp.check.take_changes().empty(),
              "MP: 10 between, 5 caught up (22:48:27.218/.382, 22:52:04.875/05.040/05.205), the caption's 56% not "
              "judged, 0 disagreements -- trusted throughout, never off");
    }
}

// ------------------------------------------------------------ the caption's shadow

void test_bar_caption_shadow() {
    section("Vitals gate and reader: no figures in the caption's shadow (54% and up)");
    BarCheck b(false);
    b.set_figures_only(true);
    b.packet(BarCheck::Point{ kT0, 12643, 15280, true, 1 });   // 83%
    BarCheck::Read r;
    r.read_at_ms = kT0 + 1000;
    r.pct = 56.0f;                                             // "12643/15280" begins 56.7% along
    b.screen(r);
    b.settle(kT0 + 2000, 1);
    const BarCheck::Counts shadow = b.take_counts();
    r.read_at_ms = kT0 + 3000;
    r.pct = 50.0f;
    b.screen(r);
    b.settle(kT0 + 4000, 1);
    const BarCheck::Counts left = b.take_counts();
    check(shadow.no_figures == 1 && shadow.disagree == 0 && left.disagree == 1,
          "56% with no figures against 83% (22:44:28.881 ...): not judged; 50%, left of any caption: judged");
    const float in = VitalsReader::shadowed_percent(56.0f, 12643, 15280);
    check(in > 82.0f && in < 83.0f && VitalsReader::shadowed_percent(40.0f, 12643, 15280) == 40.0f &&
              VitalsReader::shadowed_percent(56.0f, -1, -1) == 56.0f &&
              VitalsReader::shadowed_percent(60.0f, 8982, 15280) == 60.0f,
          "the macro gets 82.7%, not 56% -- no MP potion under 59%; 40% stands; no figures kept, or lower ones: the "
          "measure stands");
    check(VitalsReader::shadowed_percent(-1.0f, 12643, 15280) < 0.0f &&
              VitalsReader::shadowed_percent(53.9f, 12643, 15280) == 53.9f &&
              VitalsReader::shadowed_percent(54.0f, 12643, 0) == 54.0f &&
              VitalsReader::shadowed_percent(54.0f, 12643, 15280) > 82.0f &&
              BarCheck::kBarTrueBelowPct == VitalsReader::kCaptionShadowPct,
          "no bar stays no bar; 53.9% stands, 54% is in the shadow; no maximum kept: the measure; the gate's cut "
          "and the reader's the same");
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

// ------------------------------------------------------------ the hand-over

void test_hand_over() {
    section("Vitals gate: the read it disagreed with goes to the macro, once, while fresh");
    const std::uint64_t stale = MacroEngine::kVitalsStaleMs;
    {
        HandOver h;
        h.start(66.0f, kT0);
        // The pace cut back to the run's: the screen has no read to give yet.
        const float paced = h.give(kT0 + 120, 0, stale);
        // Its newest read older than the one handed over.
        const float older = h.give(kT0 + 200, kT0 - 1000, stale);
        // A read of the screen's own, newer: it decides from here.
        const float newer = h.give(kT0 + 300, kT0 + 250, stale);
        const float again = h.give(kT0 + 400, 0, stale);
        check(paced == 66.0f && older == 66.0f && newer < 0.0f && again < 0.0f && !h.live(),
              "given while the screen has none newer; then never again, though the screen has none for a moment "
              "after");
    }
    {
        HandOver h;
        h.start(66.0f, kT0);
        const float fresh = h.give(kT0 + stale, 0, stale);
        const float old = h.give(kT0 + stale + 1, 0, stale);
        // The screen's newest read is that very read, which the pace cut back
        // to the run's on the next tick takes away: still given, until a
        // newer one lands.
        HandOver same;
        same.start(40.0f, kT0);
        const float its_own = same.give(kT0 + 600, kT0, stale);
        const float cut_away = same.give(kT0 + 620, 0, stale);
        const float landed = same.give(kT0 + 660, kT0 + 640, stale);
        HandOver none;
        none.start(-1.0f, kT0);
        check(fresh == 66.0f && old < 0.0f && its_own == 40.0f && cut_away == 40.0f && landed < 0.0f &&
                  none.give(kT0 + 10, 0, stale) < 0.0f,
              "within kVitalsStaleMs of its read and not a moment after; given while the screen's newest is that "
              "very read, and through the pace's cut, until a newer one; never for no read");
    }
}

// ------------------------------------------------------------ usable

void test_usable() {
    section("Vitals: a bar's packet value gone by only when checked, current, live and with a maximum");
    const std::uint64_t now = kT0 + 100'000;
    VitalsUsable ok;
    ok.use_on = true;
    ok.trusted = true;
    ok.reading = true;
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
    in.trusted = false;
    without("not trusted", in);
    in = ok;
    in.reading = false;
    without("the bars not read", in);
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
          "a capture of the Protocol tab's or Autopilot's -- every client's traffic -- or one left on another "
          "client's process: not usable, and said so -- \"" + foreign + "\"");
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
    in.pet_key = true;
    without("a pet potion key", in);
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
          "rests after 10 s usable, and a minute after a death panel; not with a pet potion key, in a critical "
          "hold, during Tự tìm, while HP reads 0" +
              (failed.empty() ? std::string() : " -- rested all the same:" + failed));
    check(vitals_rest_period_ms(150, 1000) == 1000 && vitals_rest_period_ms(250, 1000) == 1000 &&
              vitals_rest_period_ms(150, 5000) == 1000 && vitals_rest_period_ms(150, 150) == 150 &&
              vitals_rest_period_ms(400, 300) == 400,
          "the resting pace: vitals_idle_ms, at most a second, never quicker than the run's");
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

    // The check before the in-world block; the gate before the bot's own path;
    // death and HP-back on the screen, as they were.
    check(before(tick, "vitals_.set_period_ms(vitals_pace());", "vitals_check_tick(now_ms());") &&
              before(tick, "vitals_check_tick(now_ms());", "const std::uint64_t frame_at = vitals_.last_read_ms();") &&
              before(tick, "const bool loading = packet_map_loading(now_ms());",
                     "VitalsReader::in_world(vitals_.hp_seen_ms(), frame_at, in_world_window_ms()) && !loading") &&
              before(tick, "SelfVitals packets = gate_vitals(now_ms());",
                     "if (packets.hp < 0.0f && packets.mp < 0.0f && bot_running_)") &&
              has(tick, "const bool down = bot_running_ && !vitals_running_ && (state_.self().dead || packets.hp == "
                        "0.0f);") &&
              has(tick, "const float hp = vitals_running_ ? vitals_.hp_percent() : -1.0f;"),
          "tick(): the pace, then the check, then the in-world hold (the packets only adding a map load), then the "
          "gate's HP and MP; the death hold and HP back from 0 as they were");

    // Usable asked afresh, with every part of it.
    const std::string usable =
        body(part, "VitalsUsable usable_in(const GuiApp& app, bool hp_bar, const SelfNow& s) const {");
    const std::string hp_now = body(part, "float GuiApp::hp_percent_now() const {");
    const std::string mp_now = body(part, "float GuiApp::mp_percent_now() const {");
    const std::string gate = body(part, "SelfVitals GuiApp::gate_vitals(std::uint64_t now) {");
    check(has(usable, "app.packets_settings_.vitals != PacketUse::Off") && has(usable, ".trusted()") &&
              has(usable, "app.capture_.running()") && has(usable, "app.capture_.start_count()") &&
              has(usable, "s.v.hp_epoch") && has(usable, "s.last_server_ms") && has(usable, "max_known(s.v)") &&
              has(hp_now, "vitals_usable(") && has(hp_now, "const float screen = vitals_.hp_percent();") &&
              has(mp_now, "vitals_usable(") && has(mp_now, "const float screen = vitals_.mp_percent();") &&
              has(gate, "vitals_usable(") && has(gate, "hp_hand.give(now, screen_at, MacroEngine::kVitalsStaleMs)") &&
              has(gate, "vitals_.paced_from_ms()") && has(gate, "if (!vitals_.running()) {"),
          "packets_vitals.cpp: usable from [Packets], the check, the capture and its start count, the epoch, the "
          "server and a maximum, asked at each call; the bars' HP and MP otherwise; the potions' hand-over while "
          "the screen has no fresh read");

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

    // Every HP/MP point the potions go by reaches the check -- told apart by
    // its seq, not its time -- and its disagreements are written out with
    // what else said that value around the read.
    const std::string minute = body(part, "void GuiApp::note_vitals_minute(std::uint64_t now) {");
    check(has(checking, "if (p.seq <= part.fed_seq) continue;") && has(checking, "if (v.points > part.fed_seq) {") &&
              !has(checking, "p.at_ms <= part.fed_ms"),
          "vitals_check_tick feeds the check every point since the last by its seq: two in one millisecond both go "
          "in, the later last, as the potions have it");
    check(has(checking, "take_disagreements()") && has(checking, "udp_mp") && has(checking, "presses_since(") &&
              has(checking, "++part.same_ms;") && has(minute, "c.empty") && has(minute, ", empty ") &&
              has(minute, "part->same_ms"),
          "each disagreement written out beside the newest packets, 0x02F0 and the potion key; the minute line "
          "counts the points of one millisecond and the empty reads not judged");
    check(has(minute, "c.caught_up") && has(minute, ", caught up ") && has(minute, "c.same_episode") &&
              has(minute, ", same moment "),
          "the minute line counts the agreements 0x0051 caught up with and the disagreements of one HUD moment again");

    // 2026.11.08: the MP caption read short of its leading 1, kept once a
    // session as a picture -- named apart from vitals_debug's own frames,
    // whose rotation never takes it -- and cut and written off this thread.
    const std::string keep = body(part, "    void keep_mp_caption(");
    const std::size_t async = keep.find("std::async(std::launch::async,");
    const auto in_async = [&keep, async](const char* part_of) {
        const std::size_t at = keep.find(part_of);
        return async != std::string::npos && at != std::string::npos && at > async;
    };
    check(has(checking, "if (!part.caption_kept)") &&
              has(checking, "figure_lost_leading_one(d.read.figure, d.packet.value)") &&
              has(checking, "part.keep_mp_caption(vitals_, d.read.figure, d.packet.value, d.read.read_at_ms);") &&
              has(keep, "caption_kept = true;") && in_async("exe_relative(\"vitals_debug\")") &&
              in_async("save_png(cut,") && in_async("make_folder(dir)") && in_async("Frame cut;") &&
              has(keep, "\"mp_caption_%Y%m%d_%H%M%S.png\"") &&
              !has(keep, "vitals_%") && !has(keep, "remove_file(") &&
              has(keep, "VitalsReader::caption_box(bar)"),
          "packets_vitals.cpp: the MP bar and its caption kept once, off GoBot's thread, as "
          "vitals_debug\\mp_caption_*.png, never in the rotation of vitals_*.png");

    // Only figures a read made out itself go to the check: one in the
    // caption's shadow goes by the kept ones (shadowed_percent), at their
    // percentage exactly, which alone would pass them off as its own.
    const std::string reader = source("/src/vitals_reader.cpp");
    const std::string loop = body(reader, "void VitalsReader::loop() {");
    check(has(loop, "bar.percent = shadowed_percent(bar.percent, bar.value, bar.max);") &&
              before(loop, "bar.now = false;", "if (++bar.misses >= forget_at) {") &&
              before(loop, "bar.misses = 0;", "bar.now = true;") && has(loop, "hp_pct, values_.hp_now }") &&
              has(loop, "mp_pct, values_.mp_now }") &&
              has(checking, "bar_read(before, hp_pct, figures.hp, figures.hp_max, figures.hp_now);") &&
              has(checking, "bar_read(before, mp_pct, figures.mp, figures.mp_max, figures.mp_now);") &&
              has(part, "if (now && pct >= 0.0f && figure_percent(value, max) == pct) {"),
          "the reader's loop: a miss is not this read's own, and in the caption's shadow goes by the kept figures; "
          "vitals_check_tick hands the check figures only with the read's own flag");

    // HP -1 -- not in the world -- stays -1 for the jobs' gates, whatever the
    // packets say.
    check(before(hp_now, "if (screen < 0.0f) return screen;", "vitals_usable(") &&
              before(mp_now, "if (screen < 0.0f) return screen;", "vitals_usable(") &&
              has(hp_now, "    return screen;") && has(mp_now, "    return screen;"),
          "hp_percent_now and mp_percent_now give the packets' value only while the bars' last read saw the bar: "
          "the quest's, the charms' and the shop's in-world gates as before");

    // A hand-over only to a macro running, and the run's pace asked again the
    // moment a bar goes back to the screen while the reader rests.
    check(has(gate, "if (!macro_ || !macro_->running()) {") &&
              has(checking, "c.read.pct >= 0.0f && macro_ && macro_->running()") &&
              has(checking, "c.read.read_at_ms >= vitals_.paced_from_ms())") &&
              before(tick, "SelfVitals packets = gate_vitals(now_ms());",
                     "vitals_.period_ms() > vitals_run_period()") &&
              before(tick, "vitals_.period_ms() > vitals_run_period()",
                     "const std::uint64_t read_at = last >= vitals_.paced_from_ms() ? last : 0, at = now_ms();"),
          "no hand-over across a BẮT ĐẦU, nor of a read from before the pace was cut; a bar back on the screen "
          "while the reader rests cuts the pace before "
          "the bars' read is judged, so an old read goes as no reading");

    // The resting pace: its reasons not to, from the window's own state.
    const std::string pace = body(part, "int GuiApp::vitals_pace() const {");
    const std::string rest = body(part, "VitalsRest rest_in(const GuiApp& app, std::uint64_t now) const {");
    check(has(pace, "vitals_rest_allowed(") && has(pace, "vitals_rest_period_ms(") &&
              has(rest, "MacroKeyKind::PetPotion") && has(rest, "app.macro_->running()") && has(rest, "critical_active()") &&
              has(rest, "app.simple_find_bars_ || app.full_find_bars_") && has(rest, "app.hp_zero_since_ms_") &&
              has(rest, "app.death_seen_ms_.load()"),
          "vitals_pace rests only as vitals_rest_allowed says, with the pet's key, a critical hold, the bars looked "
          "for, HP at 0 and a death panel from GoBot's own state");

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
    test_bar_trust();
    test_bar_filter();
    test_bar_empty();
    test_bar_doubt();
    test_bar_distrust();
    test_bar_over_max();
    test_bar_disagreements();
    test_bar_hud_leads();
    test_bar_caught_up();
    test_bar_episode();
    test_bar_learning_exact();
    test_bar_farm_replay();
    test_bar_caption_shadow();
    test_reader_caption_shadow();
    test_hand_over();
    test_usable();
    test_rest();
    test_world_window();
    test_map_loading();
    test_lost_leading_one();
    test_vitals_wiring();
}
