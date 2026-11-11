// gobot_test -- the potion shop right after the Dọn rác pass (2026.11.11, the
// owner: "Tới giờ mua: dọn rồi mua"). A timed look of the shop come due with
// Tự dọn rác on pulls the pass to now, and its round rides that pass under the
// same hold and the same hand on the game's mouse, with no quiet second and no
// HP start gate of its own; a look come due while a timed pass ran rides it the
// same way. The rules -- bag::ride_turn and bag::ride_refused -- tried alone and
// stepped through as the worker's loop steps them; and where GoBot's window
// wires them in, read from its source: what is carried across and never let go
// in between, the buttons and a worker here for one left as they were, the
// order of the jobs, the log's lines and the card's words.
//
// Nothing here goes near the desktop, the pointer, the keyboard or the game:
// the rules are pure, the loop is stepped on numbers of the test's own, and the
// window's part is its source.

#include "gobot/bag.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

using bag::ShopRide;

const char* said(ShopRide r) {
    switch (r) {
        case ShopRide::None:    return "None";
        case ShopRide::Pending: return "Pending";
        case ShopRide::Alone:   return "Alone";
    }
    return "?";
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ------------------------------------------------------------- the rules

void test_ride_turn() {
    section("Shop after the pass: a look come due pulls a pass, none within 60 s of the last one (2026.11.11)");
    constexpr std::uint64_t t = 50'000'000;
    const auto turn = [&](ShopRide was, bool due, bool trash_on, bool cannot_pull, std::uint64_t last_end) {
        return bag::ride_turn(was, due, trash_on, cannot_pull, t, last_end);
    };
    check(bag::kRideFreshMs == 60'000, "a pass ended less than 60 s ago is fresh (kRideFreshMs)");

    // A look just come due with Tự dọn rác on: a pass first -- none yet, or the
    // last long ago, or exactly a minute ago.
    check(turn(ShopRide::None, true, true, false, 0) == ShopRide::Pending,
          "no pass yet: the look pulls one (" + std::string(said(turn(ShopRide::None, true, true, false, 0))) + ")");
    check(turn(ShopRide::None, true, true, false, t - 600'000) == ShopRide::Pending,
          "the last pass ten minutes ago: the look pulls one");
    check(turn(ShopRide::None, true, true, false, t - bag::kRideFreshMs) == ShopRide::Pending,
          "the last pass ended 60 s ago to the millisecond: the look pulls one");

    // A pass less than a minute old: no extra pass, the shop on its own at once.
    check(turn(ShopRide::None, true, true, false, t - bag::kRideFreshMs + 1) == ShopRide::Alone &&
              turn(ShopRide::None, true, true, false, t - 20'000) == ShopRide::Alone &&
              turn(ShopRide::None, true, true, false, t) == ShopRide::Alone,
          "a pass ended 59.999 s, 20 s or 0 s ago: no pass pulled, the shop goes on its own");
    // No pass to be pulled now (the review's R1 and R2): where the shop's own
    // cheap gates would put the look off -- the chat line maybe open, no game
    // window, HP not read, a keeper's crossing -- or a pass found nothing it
    // could do a while ago.
    check(turn(ShopRide::None, true, true, true, 0) == ShopRide::Alone &&
              turn(ShopRide::None, true, true, true, t - 600'000) == ShopRide::Alone,
          "no pass to be pulled -- the chat line in doubt, no game window, HP not read, a keeper's crossing, or a "
          "pass a while ago that found nothing to do: none pulled, the shop waits on its own");

    // No ride with no look due, or Tự dọn rác off -- from whatever was decided.
    bool none = true;
    for (const ShopRide was : { ShopRide::None, ShopRide::Pending, ShopRide::Alone }) {
        none = none && turn(was, false, true, false, 0) == ShopRide::None &&
               turn(was, true, false, false, 0) == ShopRide::None && turn(was, false, false, true, 0) == ShopRide::None;
    }
    check(none, "no look due (Tự mua bình off, a button, a one-shot worker, the bin held) or Tự dọn rác off: no ride, "
                "whatever the last turn had");

    // Decided once for each look: kept as it was while the look stays due.
    check(turn(ShopRide::Pending, true, true, false, t) == ShopRide::Pending &&
              turn(ShopRide::Pending, true, true, true, t) == ShopRide::Pending,
          "a pass pulled stays pulled while the look is due -- a pass just ended or a chat line meanwhile");
    check(turn(ShopRide::Alone, true, true, false, 0) == ShopRide::Alone &&
              turn(ShopRide::Alone, true, true, false, t - 600'000) == ShopRide::Alone,
          "a look gone on its own stays so while it is due: its pass put off is not pulled again every turn");
}

/// The worker's loop, as far as the ride goes, a turn every 100 ms on numbers
/// of the test's own: the ride worked out (ride_turn) and the pass pulled --
/// none where the shop's cheap gates would put the look off (`cannot_pull`:
/// HP not read, a keeper's crossing, no game window), nor before
/// `no_pull_before_ms`; the shop on its own with no ride pending and no pass
/// due; the pass, which marks a pull taken (Alone) on its way in, then either
/// put off by its HP gate, finds nothing it could do (the list empty, no tab
/// ticked: no pull again before its own clock), or runs -- the round riding
/// it when it ran clean (ride_refused). What a gate or a round does to the
/// shop's clock is the shop's own rule, given here: a plan bought, the next
/// an hour on; put off, `shop_put_off_ms` -- by its HP gate,
/// bag::kHpLookAgainMs (5 s).
struct Loop {
    std::uint64_t now = 80'000'000;
    std::uint64_t next = 0;               // the pass's clock
    std::uint64_t shop_due_ms = 0;        // the shop's
    std::uint64_t last_pass_end_ms = 0;
    std::uint64_t no_pull_before_ms = 0;
    ShopRide      ride = ShopRide::None;
    bool          pass_hp_low = false;    // the pass's HP gate puts it off
    std::uint64_t shop_put_off_ms = 0;    // the shop on its own put off this long (0: it buys)
    bool          pass_clean = true;      // a pass that runs ends clean
    bool          pass_cannot_run = false;    // the list empty, or no tab ticked
    bool          cannot_pull = false;    // the shop's own cheap gates would put a look off
    std::uint64_t interval_ms = 600'000;  // Dọn rác every 10 minutes
    std::uint64_t pass_ms = 8'000;        // a pass takes this long
    int           pulls = 0, passes = 0, put_offs = 0, alone = 0, ridden = 0, idle = 0;

    void turn() {
        now += 100;
        const bool shop_due = now >= shop_due_ms;
        const ShopRide was = ride;
        const bool no_pull = shop_due && was == ShopRide::None && (cannot_pull || now < no_pull_before_ms);
        ride = bag::ride_turn(was, shop_due, true, no_pull, now, last_pass_end_ms);
        if (ride == ShopRide::Pending && was != ShopRide::Pending) {
            next = std::min(next, now);
            ++pulls;
        }
        const bool pass_due = now >= next;
        if (shop_due && ride != ShopRide::Pending && !pass_due) {
            ride = ShopRide::None;
            ++alone;
            shop_due_ms = now + (shop_put_off_ms > 0 ? shop_put_off_ms : 3'600'000);
            return;
        }
        if (!pass_due) return;
        const bool pulled = ride == ShopRide::Pending;
        if (pulled) ride = ShopRide::Alone;
        next = now + interval_ms;
        if (pass_hp_low) {
            ++put_offs;
            next = now + bag::kHpLookAgainMs;
            return;
        }
        if (pass_cannot_run) {
            ++idle;
            no_pull_before_ms = next;
            return;
        }
        now += pass_ms;
        last_pass_end_ms = now;
        ++passes;
        bag::RidePass p;
        p.ran = p.timed = p.bag_ran = p.hold_ours = true;
        p.trouble = !pass_clean;
        p.shop_due = now >= shop_due_ms;
        if (bag::ride_refused(p) == nullptr) {
            ride = ShopRide::None;
            ++ridden;
            shop_due_ms = now + 3'600'000;    // a plan bought: the next in an hour
        }
    }
    void run(std::uint64_t ms) {
        for (const std::uint64_t until = now + ms; now < until;) turn();
    }
};

void test_ride_stepped() {
    section("Shop after the pass: stepped as the worker's loop steps it -- one pull a look, the grid re-based");

    // Due 4 minutes after a pass: a pass pulled at once, and the round on it.
    {
        Loop l;
        l.last_pass_end_ms = l.now - 240'000;
        l.next = l.now + 360'000;
        l.shop_due_ms = l.now + 1;
        const std::uint64_t due = l.shop_due_ms;
        l.run(1'000);
        check(l.pulls == 1 && l.passes == 1 && l.ridden == 1 && l.alone == 0 && l.last_pass_end_ms - due < 10'000,
              "due 4 min after a pass: one pass pulled at once and the round rides it (pulls " +
                  std::to_string(l.pulls) + ", passes " + std::to_string(l.passes) + ", ridden " +
                  std::to_string(l.ridden) + ")");
        check(l.next == l.last_pass_end_ms - l.pass_ms + l.interval_ms,
              "and the pulled pass sets the pass clock an interval on from itself: the grid follows the shop");
    }
    // Due 20 s after a pass: no extra pass, the shop on its own at once.
    {
        Loop l;
        l.last_pass_end_ms = l.now - 20'000;
        l.next = l.now + 580'000;
        l.shop_due_ms = l.now + 1;
        l.run(1'000);
        check(l.pulls == 0 && l.passes == 0 && l.alone == 1,
              "due 20 s after a pass: no pass pulled, the shop on its own at once (alone " +
                  std::to_string(l.alone) + ")");
    }
    // The pulled pass put off by its HP gate, 30 s long, and the shop on its
    // own by its own: pulled once a look -- the shop's own wait between, every
    // bag::kHpLookAgainMs -- never every turn.
    {
        Loop l;
        l.last_pass_end_ms = l.now - 600'000;
        l.next = l.now + 300'000;
        l.shop_due_ms = l.now + 1;
        l.pass_hp_low = true;
        l.shop_put_off_ms = bag::kHpLookAgainMs;
        l.run(30'000);
        check(l.pulls >= 2 && l.pulls <= static_cast<int>(30'000 / bag::kHpLookAgainMs) + 1 && l.alone == l.pulls &&
                  l.passes == 0,
              "a pulled pass its HP gate puts off: one pull for each look of the shop, every bag::kHpLookAgainMs, "
              "which then goes on its own -- never every turn (pulls " + std::to_string(l.pulls) + ", alone " +
                  std::to_string(l.alone) + " in 30 s)");
    }
    // The shop's own cheap gates would put the look off -- HP not read (the
    // login or character screen, a map loading), a keeper's crossing, no
    // game window (the review's R1): no pass pulled for minutes, the shop
    // waiting on its own as before 2026.11.11; the first look after, one.
    {
        Loop l;
        l.last_pass_end_ms = l.now - 600'000;
        l.next = l.now + 300'000;
        l.shop_due_ms = l.now + 1;
        l.cannot_pull = true;
        l.shop_put_off_ms = 15'000;      // "Chưa đọc được máu của nhân vật, mua bình sau."
        l.run(180'000);
        const int alone_out = l.alone;
        check(l.pulls == 0 && l.passes == 0 && alone_out >= 10,
              "HP not read, a keeper's crossing or no game window for 3 minutes: no pass pulled, the shop on its own "
              "each look (alone " + std::to_string(alone_out) + ", pulls " + std::to_string(l.pulls) + ")");
        l.cannot_pull = false;
        l.shop_put_off_ms = 0;
        l.run(20'000);
        check(l.pulls == 1 && l.passes == 1 && l.ridden == 1 && l.alone == alone_out,
              "back in the world: the next look pulls one pass, and the round rides it (pulls " +
                  std::to_string(l.pulls) + ", ridden " + std::to_string(l.ridden) + ")");
    }
    // A pulled pass that finds nothing it could do -- the list empty, no tab
    // ticked (the review's R2): no pull again before its own clock, an
    // interval on, the shop put off every 5 s going on its own.
    {
        Loop l;
        l.last_pass_end_ms = l.now - 600'000;
        l.next = l.now + 300'000;
        l.shop_due_ms = l.now + 1;
        l.pass_cannot_run = true;
        l.shop_put_off_ms = bag::kHpLookAgainMs;
        l.run(60'000);
        check(l.pulls == 1 && l.idle == 1 && l.passes == 0 && l.alone >= 10 &&
                  l.no_pull_before_ms >= l.now + l.interval_ms - 61'000,
              "a pulled pass with nothing to do: pulled once in a minute, not at each of the shop's looks, which go "
              "on their own (pulls " + std::to_string(l.pulls) + ", alone " + std::to_string(l.alone) + ")");
    }
    // A pass that does not end clean: no ride, the shop on its own -- and no
    // pass pulled again, the last one under a minute old.
    {
        Loop l;
        l.last_pass_end_ms = l.now - 600'000;
        l.next = l.now + 300'000;
        l.shop_due_ms = l.now + 1;
        l.pass_clean = false;
        l.run(10'000);
        check(l.pulls == 1 && l.passes == 1 && l.ridden == 0 && l.alone == 1,
              "a pulled pass that stopped early: no ride, the shop on its own after it, and no second pass");
    }
    // Two hours at 10/60 after a start whose saved plan time comes 4 minutes
    // after a pass: the first plan pulls one, and from then on each plan comes
    // due as the pass of its hour ends, and rides it -- no pass more.
    {
        Loop l;
        l.last_pass_end_ms = l.now - 240'000;
        l.next = l.now + 360'000;
        l.shop_due_ms = l.now + 1;
        l.run(2 * 3'600'000ull + 60'000);
        check(l.pulls == 1 && l.ridden == 3 && l.alone == 0 && l.passes == 13,
              "two hours at 10/60, a plan due 4 min after a pass: one pass pulled, every plan's round riding a pass, "
              "13 passes (ridden " + std::to_string(l.ridden) + ", alone " + std::to_string(l.alone) + ", pulls " +
                  std::to_string(l.pulls) + ", passes " + std::to_string(l.passes) + ")");
    }
    // A fresh start -- the pass at 15 s, the shop's first look at 45 s: the pass
    // a moment old each time, so each plan is bought on its own right after it.
    {
        Loop l;
        l.next = l.now + 15'000;
        l.shop_due_ms = l.now + 45'000;
        l.run(2 * 3'600'000ull + 60'000);
        check(l.pulls == 0 && l.ridden == 0 && l.alone == 3 && l.passes == 13,
              "two hours at 10/60 from a fresh start: no pass pulled, each plan on its own 22 s after its pass "
              "(alone " + std::to_string(l.alone) + ", passes " + std::to_string(l.passes) + ")");
    }
}

void test_ride_refused() {
    section("Shop after the pass: the round rides only a timed pass that ran clean, the hold in hand");
    bag::RidePass clean;
    clean.ran = clean.timed = clean.bag_ran = clean.hold_ours = clean.shop_due = true;
    check(bag::ride_refused(clean) == nullptr, "a timed pass that ran clean, the hold still the job's, the shop due: "
                                               "the round rides it");
    using P = bag::RidePass;
    const struct {
        const char* what;
        void (*spoil)(P&);
    } kSpoiled[] = {
        { "a Dọn ngay or Thử tìm button's pass, or one of a worker here for a button",
          [](P& p) { p.timed = false; } },
        { "a pass that never ran (the mouse or the hold not got)", [](P& p) { p.ran = false; } },
        { "DỪNG / F12 / the worker told to stop", [](P& p) { p.stopped = true; } },
        { "a look only (Thử tìm)", [](P& p) { p.look_only = true; } },
        { "a pass begun to give back what may be left in the bin", [](P& p) { p.left_held = true; } },
        { "a pass that kept the hold (an item maybe left in the bin)", [](P& p) { p.keeps_hold = true; } },
        { "the check-in board up", [](P& p) { p.board_up = true; } },
        { "the bag not searched", [](P& p) { p.bag_ran = false; } },
        { "a pass stopped early -- trouble, HP under the level, the hold lost", [](P& p) { p.trouble = true; } },
        { "the hold no longer the job's", [](P& p) { p.hold_ours = false; } },
        { "the shop no longer due, or Tự mua bình turned off", [](P& p) { p.shop_due = false; } },
    };
    for (const auto& s : kSpoiled) {
        bag::RidePass p = clean;
        s.spoil(p);
        const char* why = bag::ride_refused(p);
        check(why != nullptr && *why != '\0',
              std::string("no ride: ") + s.what + (why != nullptr ? std::string(" (\"") + why + "\")" : std::string()));
    }
}

// ------------------------------------------------------- the wiring

void test_ride_sources() {
    section("Shop after the pass: where GoBot's window wires it in -- one hold, one mouse, no quiet second (source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string root = GOBOT_SOURCE_DIR;
    const std::string gui = read_file(root + "/apps/gobot_gui/gui_app.cpp");
    const std::string card = read_file(root + "/apps/gobot_gui/simple_shop.cpp");
    const std::string cli = read_file(root + "/apps/gobot_cli/main.cpp");
    if (gui.empty() || card.empty() || cli.empty()) {
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
    const auto in_order = [](const std::string& text, std::initializer_list<const char*> parts) {
        std::size_t at = 0;
        for (const char* p : parts) {
            const std::size_t found = text.find(p, at);
            if (found == npos) return false;
            at = found + 1;
        }
        return true;
    };
    const std::string worker = slice(gui, "void GuiApp::trash_loop() {", "---------------- auto chat");
    const std::string shop =
        slice(gui, "const auto run_shop = [&](bool forced, bool dry, bool after_pass) {", "while (!trash_stop_) {");
    const std::string loop = slice(worker, "while (!trash_stop_) {", "trash_clocks_.charm_check_ms = charm_check_ms;");
    const std::string pass =
        slice(loop, "const bool look_only = request == 2;", "if (trash_oneshot_ && !held) break;\n    }");
    if (worker.empty() || shop.empty() || loop.empty() || pass.empty()) {
        check(false, "gui_app.cpp: trash_loop, run_shop, the loop and the pass found");
        return;
    }

    // ---- the turn: the ride worked out, the pass pulled, before the order ----
    // None pulled where the shop's own cheap gates would put the look off,
    // nor for an interval after one found nothing it could do (the review's
    // R1 and R2), asked only as a look is decided; the pull said once until a
    // job runs (R4).
    check(in_order(loop, { "const bool shop_due = timed && !held && settings.buy_potions && now_ms() >= shop_due_ms;",
                           "if (!settings.enabled) no_pull_before_ms = 0;", "const auto no_pull = [&] {",
                           "if (chat_doubt_ || keeper_cold_start_ || now_ms() < no_pull_before_ms || "
                           "!input_.window_valid() ||",
                           "!client_on_screen(input_.target_window()) || hp_percent_now() < 0.0f)",
                           "return true;", "std::lock_guard<std::mutex> lock(keeper_flow_mutex_);",
                           "return keeper_flow_ != nullptr;",
                           "const bool cannot_pull = shop_due && settings.enabled && was == bag::ShopRide::None && "
                           "no_pull();",
                           "ride = bag::ride_turn(was, shop_due, settings.enabled, cannot_pull, now_ms(), "
                           "last_pass_end_ms);",
                           "if (ride == bag::ShopRide::Pending && was != bag::ShopRide::Pending) {",
                           "next = std::min(next, now_ms());", "if (!pull_said) {", "pull_said = true;",
                           "GB_INFO(\"Trash: shop due -- a pass first,",
                           "const bool pass_due = timed && settings.enabled && now_ms() >= next;",
                           "if ((pass_due || charm_due || shop_due) && chat_first()) continue;",
                           "(shop_due && ride != bag::ShopRide::Pending && !pass_due && !charm_due)",
                           "ride = bag::ShopRide::None;", "run_shop(request == 4, request == 5, false);",
                           "(charm_due && !pass_due)",
                           "const bool pulled = ride == bag::ShopRide::Pending && request == 0;",
                           "if (pulled) ride = bag::ShopRide::Alone;", "if (chat_doubt_) {", "if (held_by_other()) {",
                           "next = now_ms() + static_cast<std::uint64_t>(settings.interval_minutes) * 60'000ull;" }) &&
              count_in(gui, "bag::ride_turn(") == 1 && count_in(gui, "next = std::min(next, now_ms());") == 1 &&
              has(gui, "const bool timed = request == 0 && !trash_oneshot_;"),
          "trash_loop: the ride worked out as each turn begins -- a look of a timed worker, the pass pulled to now "
          "once -- before the pass's turn; the shop on its own only with none pending, before the charms, which "
          "wait for a pulled pass; the pass marks the pull taken before its gates, and is then timed from now");
    // A timed pass with nothing it could do -- the list empty, no tab ticked --
    // blocks pulls until its own clock (R2); not one with no game window,
    // which the pull asks live. Its HP gate before its list (PERF-PASS-HP-5S).
    check(in_order(loop, { "next = now_ms() + static_cast<std::uint64_t>(settings.interval_minutes) * 60'000ull;",
                           "const bool look_only = request == 2;", "if (request == 0 && !held) {",
                           "next = now_ms() + bag::kHpLookAgainMs;", "TrashList list;",
                           "tell(\"Danh sách đồ bỏ đi đang trống, nên không có gì để dọn.\");",
                           "if (request == 0) no_pull_before_ms = next;",
                           "tell(std::string(pass_tabs.none_said()) + \".\");",
                           "if (request == 0) no_pull_before_ms = next;", "if (!input_.window_valid()) {",
                           "tell(\"Chưa thấy cửa sổ game.\");", "if (!client_on_screen(input_.target_window())) {" }) &&
              count_in(gui, "no_pull_before_ms = next;") == 2 &&
              // Its declaration, and Tự dọn rác turned off.
              has(gui, "if (!settings.enabled) no_pull_before_ms = 0;") &&
              count_in(gui, "no_pull_before_ms = 0;") == 2,
          "the pass: one with nothing it could do keeps the shop from pulling another until its own clock, an "
          "interval on -- not one with no game window, which the pull asks live -- and its HP gate is asked before "
          "its list is built");

    // ---- the pass: the hold and the mouse carried into the round ----
    check(in_order(pass, { "std::unique_lock<std::mutex> mouse(game_mouse_mutex_, std::defer_lock);",
                           "while (!trash_stop_ && !mouse.try_lock())", "take_hold(\"trash cleaning\")",
                           "hp_cut = HpCut{};", "r = cleaner.run();", "if (!look_only) last_pass_end_ms = now_ms();",
                           "say_stopped(\"pass\", hp_cut, r.trouble);", "held = r.keeps_hold(held);",
                           "ridden.timed = request == 0 && !trash_oneshot_;", "ridden.stopped = trash_stop_;",
                           "ridden.look_only = look_only;", "ridden.left_held = left_held;",
                           "ridden.keeps_hold = held;", "ridden.bag_ran = r.ran;",
                           "ridden.trouble = !r.trouble.empty();", "ridden.board_up = r.board_up;",
                           "ridden.hold_ours = coord_.critical_active() && "
                           "coord_.critical_reason() == \"trash cleaning\";",
                           "ridden.shop_due = settings.buy_potions && now_ms() >= shop_due_ms;",
                           "rides = no_ride == nullptr;", "if (!held && !rides) end_hold(\"trash cleaning\");",
                           "if (!rides) trash_pass_ = false;", "if (!rides && mouse.owns_lock()) mouse.unlock();",
                           "if (rides) {", "ride = bag::ShopRide::None;", "run_shop(false, false, true);",
                           "if (!held) end_hold(\"trash cleaning\");", "trash_pass_ = false;",
                           "if (mouse.owns_lock()) mouse.unlock();" }),
          "the pass: the round rides it only by ride_refused, every part of it from the pass itself; the hold, the "
          "mouse and trash_pass_ let go as the pass ends only when nothing rides, else after the round");
    {
        // From the pass's end to the round: nothing let go, nothing taken,
        // nothing waited for.
        const std::size_t from = pass.find("rides = no_ride == nullptr;");
        const std::size_t to = pass.find("run_shop(false, false, true);");
        const std::string between =
            from == npos || to == npos || to < from ? std::string() : pass.substr(from, to - from);
        check(!between.empty() && count_in(between, "end_hold(") == 1 &&
                  has(between, "if (!held && !rides) end_hold(\"trash cleaning\");") &&
                  count_in(between, "take_hold(") == 0 && count_in(between, "mouse.unlock()") == 1 &&
                  has(between, "if (!rides && mouse.owns_lock()) mouse.unlock();") &&
                  count_in(between, "sleep_for(") == 0 && count_in(between, "disengage_critical") == 0 &&
                  count_in(between, "try_lock(") == 0,
              "between the pass's end and the round: no end_hold, no take_hold, the mouse kept, no wait -- each let "
              "go only when nothing rides");
    }
    check(count_in(gui, "run_shop(false, false, true);") == 1 && count_in(gui, "run_shop(") == 2 &&
              count_in(gui, "run_shop(request == 4, request == 5, false);") == 1,
          "run_shop with after_pass from the end of a pass alone; Mua ngay and Thử mua (4, 5) and a timed look on "
          "its own as before, never after_pass");

    // ---- the round that rides: no mouse asked for, no quiet second, no HP start gate ----
    // (2026.11.11, the owner's "Mua luôn, bơm xen": the gate the round's own,
    // by its key window -- bag::hp_gates -- and looked at again in 5 s. HP not
    // read -- not in the world -- waits for a ridden round too: the review's
    // R3.)
    check(in_order(shop, { "const bag::HpGates hp_gate = hp_gates_of(pass_keys);",
                           "const float hp = hp_percent_now();",
                           "if (hp < 0.0f) return not_now(\"Chưa đọc được máu của nhân vật, mua bình sau.\", 15'000);",
                           "if (!after_pass) {", "if (hp < static_cast<float>(hp_gate.start)) {",
                           "return not_now(\"Máu đang thấp, mua bình sau.\", bag::kHpLookAgainMs);",
                           "const bag::ShopOwed owed_before = owed;", "owed = bag::shop_new_plan(",
                           "ScreenCapture shop_capture;",
                           "while (!after_pass && !trash_stop_ && now_ms() < give_up && !mouse.try_lock())",
                           "if ((after_pass || mouse.owns_lock()) && !trash_stop_ && !chat_doubt_ && "
                           "take_hold(\"trash cleaning\")) {",
                           "const int tries = forced || dry ? kShopQuietTries : after_pass ? 0 : 1;",
                           "if (after_pass) lost = !still_ours();",
                           "if (tries > 0) std::this_thread::sleep_for(std::chrono::milliseconds(30));",
                           "for (int t = 0; t < tries && !trash_stop_ && !lost; ++t) {", "passed = true;",
                           "if (!after_pass) shop_quiet.reset();", "r = cleaner.run();",
                           "if (!held) end_hold(\"trash cleaning\");" }),
          "run_shop: a round that rides a pass waits with HP not read but skips the HP start gate, takes no mouse "
          "(the pass's is held) and has no quiet second -- the hold and HP looked at once -- and leaves the quiet "
          "second's back-off as it was");
    {
        // The HP start gate alone under `if (!after_pass)`, HP unread outside.
        const std::string gate = slice(shop, "if (!after_pass) {", "const std::vector<std::string> icons{");
        check(!gate.empty() && !has(gate, "hp < 0.0f") && has(gate, "hp < static_cast<float>(hp_gate.start)") &&
                  count_in(shop, "if (hp < 0.0f) return not_now(") == 1,
              "the not-in-world check asked of every round, the start gate only of one on its own");
    }
    {
        // The plan's rules are the same for a round that rides: never a plan
        // begun early, its clock written first, a timed part kShopBuysPerHold.
        const std::string plan = slice(shop, "const bag::ShopOwed owed_before = owed;", "ScreenCapture shop_capture;");
        check(!plan.empty() && !has(plan, "after_pass") && has(plan, "if (forced || now_ms() >= plan_due_ms) {") &&
                  has(shop, "bag::shop_order(owed.hp, owed.mp, forced ? BagCleaner::kShopMaxBuys : "
                            "BagCleaner::kShopBuysPerHold);"),
              "and the plan's rules as they were: a new plan only when due (or Mua ngay), never by a ride, a timed "
              "round of the ride kShopBuysPerHold at most");
    }

    // ---- the log ----
    check(has(gui, "\"Trash: shop due -- a pass first, then the round under the same hold (the last pass \"") &&
              has(gui, "\"ended %s)\", since_pass().c_str());") &&
              has(gui, "\"Trash: shop rides the pass -- the same hold, no quiet second (%s)\"") &&
              has(gui, "\"Trash: shop -- no ride on the pass it pulled (%s): it goes on its own\"") &&
              has(gui, "\"%d unsure, full %s; %s, the last pass ended %s\"") &&
              has(gui, "after_pass ? \"right after the pass\" : forced ? \"Mua ngay\" : dry ? \"Thử mua\" : "
                       "\"on its own\"") &&
              has(gui, "GB_INFO(\"Trash: timed %s put off -- %s\", job, said.c_str());") &&
              has(gui, "if (why == put_off_why) return;") && count_in(gui, "put_off_why.clear();") == 2 &&
              // The pull said once until a job runs or a new reason puts a
              // look off -- not dropped for any reason still stored (R4): its
              // declaration and three resets.
              has(gui, "bool pull_said = false;") && count_in(gui, "pull_said = false;") == 4 &&
              in_order(gui, { "put_off_why = why;", "pull_said = false;" }) &&
              in_order(gui, { "put_off_why.clear();", "pull_said = false;", "put_off_why.clear();",
                              "pull_said = false;" }) &&
              !has(gui, "if (put_off_why.empty())") &&
              has(gui, "put_off(\"pass\", \"hp\",\n                        hp_said(hp, hp_gate.start, hp_gate.drinks ? "
                       "\"the HP potion's mark\" : \"the start gate\"));") &&
              has(gui, "timed_put_off(\"hp\",\n                              hp_said(hp, hp_gate.start, hp_gate.drinks ? "
                       "\"the HP potion's mark\" : \"the start gate\"));") &&
              has(gui, "timed_put_off(\"touched\",") && has(gui, "timed_put_off(\"own input\",") &&
              has(gui, "timed_put_off(\"board\", \"the check-in board is up\");") &&
              has(gui, "put_off(\"pass\", \"board\", \"the check-in board is up\");") &&
              count_in(gui, "put_off(\"pass\", \"hold \" + coord_.critical_reason(),") == 2 &&
              has(gui, "if (!forced && !dry) put_off(\"shop\", why, said);") &&
              has(gui, "\"Trash: %s stopped by HP -- %s\"") &&
              has(gui, "\"Trash: %s stopped by the hold -- %s's now; HP %.0f%%, the level %d%%\"") &&
              has(gui, "say_stopped(\"shop round\", hp_cut, r.trouble);"),
          "the log: a pull with the time since the last pass, the ride, a pull with no ride, the time since the last "
          "pass on each shop job line, a timed put-off said once until its reason changes (HP and its gate, another "
          "hold, the quiet second, the board), and what stopped a pass or a round -- HP, or the hold");
    // The guards as they were in shape (packet_self_test counts three), noting
    // what HP they stopped at -- the job's own level since 2026.11.11
    // (bag::HpGates::stop).
    check(count_in(gui, "hp_cut = HpCut{ hp, level };") == 2 &&
              count_in(gui, "\"trash cleaning\")) return false;\n            const float hp = hp_percent_now();\n"
                            "            const int level = stop_level;") == 2,
          "the pass's guard and the shop's still_ours note the HP and the level they stopped at, nothing else changed");

    // ---- the card, and what is left alone ----
    check(in_order(card, { "if (simple_clean_.buy_potions && simple_clean_.enabled) {",
                           "\"Dọn rác đang bật: tới giờ mua, GoBot dọn rác trước rồi mua bình liền sau đó, trong cùng "
                           "một lần dừng \"",
                           "std::to_string(bag::kRideFreshMs / 60'000)", "hint(ride.c_str());" }) &&
              has(card, "Khi Dọn rác bật, tới giờ mua GoBot dọn rác trước rồi mua liền sau đó, trong cùng một lần "
                        "dừng \""),
          "simple_shop.cpp: with Dọn rác on the card says the round comes right after the pass, in the same stop of "
          "the keys, and none again within a minute -- the tooltip says it too");
    check(!has(cli, "ride_turn") && !has(cli, "ride_refused") && !has(cli, "after_pass"),
          "gobot_cli's --trash and its shop untouched: no ride");
#else
    skip("no source folder\n");
#endif
}

} // namespace

void test_shop_ride(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_ride_turn();
    test_ride_stepped();
    test_ride_refused();
    test_ride_sources();
}
