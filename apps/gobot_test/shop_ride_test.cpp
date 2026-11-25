// gobot_test -- the potion shop right after the Dọn rác pass (2026.11.11, the
// owner: "Tới giờ mua: dọn rồi mua"). A timed look of the shop come due with
// Tự dọn rác on pulls the pass to now, and its round rides that pass under the
// same hold and the same hand on the game's mouse, with no quiet second and no
// HP start gate of its own; a look come due while a timed pass ran rides it the
// same way. The rules -- bag::ride_turn and bag::ride_refused -- tried alone and
// stepped through as the worker's loop steps them; and where GoBot's window
// wires them in, read from its source: what is carried across and never let go
// in between, the buttons and a worker here for one left as they were, the
// order of the jobs, the log's lines and the card's words. And 2026.11.12's
// start count (bag::start_due_ms): the pass and the shop a whole interval from
// a start, a quick DỪNG and BẮT ĐẦU keeping the wait left, a box ticked on
// counting afresh, the buttons at once -- stepped the same way.
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
#include <vector>

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
    std::vector<std::uint64_t> pass_at, look_at;   // when each pass began, and each look on its own or ridden went

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
            look_at.push_back(now);
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
        pass_at.push_back(now);
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
            look_at.push_back(now);
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
    // A fresh start since 2026.11.12 -- each counted a whole interval from it
    // (bag::start_due_ms), the pass 10 minutes on and the shop's first look an
    // hour on: the first plan comes due with the hour's pass, which it pulls
    // to that moment and rides; the next comes due while its hour's pass runs,
    // and rides that one. (The pass at 15 s and the look at 45 s before.)
    {
        Loop l;
        const std::uint64_t t0 = l.now;
        l.next = bag::start_due_ms(0, t0, 10, 15'000);
        l.shop_due_ms = bag::start_due_ms(0, t0, 60, 45'000);
        l.run(2 * 3'600'000ull + 60'000);
        check(l.pulls == 1 && l.ridden == 2 && l.alone == 0 && l.passes == 12 && l.pass_at.front() == t0 + 600'000 &&
                  l.look_at.size() == 2 && l.look_at.front() == t0 + 3'600'000 + l.pass_ms,
              "two hours at 10/60 from a fresh start: the first pass 10 min on, each plan riding its hour's pass, "
              "the first right after the pass at 60 min (ridden " + std::to_string(l.ridden) + ", alone " +
                  std::to_string(l.alone) + ", pulls " + std::to_string(l.pulls) + ", passes " +
                  std::to_string(l.passes) + ")");
    }
}

// ------------------------------------------------------- the start count

/// The trash worker's two timed clocks from one start to the next
/// (2026.11.12, the owner: "khi bật thì bắt đầu đếm thời gian. Lúc nào tới
/// thời gian thì mới bắt đầu thực hiện"), stepped as trash_loop steps them: a
/// worker here for a run counts -- bag::start_due_ms from the clocks the last
/// one left (GuiApp::TrashClocks): a whole interval when none still counts,
/// what is left of one that does, 15 s and 45 s at the least -- and keeps them
/// as it ends; one here for a button alone counts nothing -- its button goes
/// at once -- and leaves the clocks as it found them; a box ticked on counts
/// its job afresh, from now, or with no worker counting from the next start.
/// The timed jobs themselves go by Loop, the ride included.
struct Starts {
    static constexpr std::uint64_t kPassFirst = 15'000, kShopFirst = 45'000;   // gui_app.cpp's, pinned below
    Loop          l;
    int           pass_every = 10, buy_every = 60;
    std::uint64_t kept_pass = 0, kept_shop = 0;    // TrashClocks
    bool          running = false, counting = false;
    std::vector<std::uint64_t> asked_pass, asked_look;   // when a button's pass and plan went

    void count() {
        counting = true;
        l.next = bag::start_due_ms(kept_pass, l.now, pass_every, kPassFirst);
        l.shop_due_ms = bag::start_due_ms(kept_shop, l.now, buy_every, kShopFirst);
    }
    /// A new worker: BẮT ĐẦU or F10 -- or, `for_button`, one here for a
    /// button alone, which counts nothing.
    void start(bool for_button = false) {
        running = true;
        counting = false;
        l.next = l.shop_due_ms = 0;
        l.ride = ShopRide::None;
        l.last_pass_end_ms = l.no_pull_before_ms = 0;
        if (!for_button) count();
    }
    /// BẮT ĐẦU while a button's worker works: it becomes one for a run.
    void become_run() {
        if (!counting) count();
    }
    /// DỪNG / F10, or a button's worker done: the two clocks kept only by a
    /// worker that counted them.
    void stop() {
        if (counting) {
            kept_pass = l.next;
            kept_shop = l.shop_due_ms;
        }
        running = counting = false;
    }
    void tick_pass() {
        if (running && counting) l.next = bag::start_due_ms(0, l.now, pass_every, kPassFirst);
        else kept_pass = 0;
    }
    void tick_shop() {
        if (running && counting) l.shop_due_ms = bag::start_due_ms(0, l.now, buy_every, kShopFirst);
        else kept_shop = 0;
    }
    /// "Dọn ngay": the pass now, and the timed pass an interval on from it, as
    /// every pass sets it; with the bot stopped, in a worker of its own.
    void don_ngay() {
        const bool alone = !running;
        if (alone) start(true);
        asked_pass.push_back(l.now);
        l.next = l.now + l.interval_ms;
        if (alone) stop();
    }
    /// "Mua ngay": a plan now, its next look an hour on.
    void mua_ngay() {
        const bool alone = !running;
        if (alone) start(true);
        asked_look.push_back(l.now);
        l.shop_due_ms = l.now + 3'600'000;
        if (alone) stop();
    }
    void run(std::uint64_t ms) {
        if (running && counting) l.run(ms);
        else l.now += ms;
    }
};

void test_start_count() {
    section("Dọn rác and Tự mua bình count from the start: nothing at BẮT ĐẦU, a quick DỪNG and BẮT ĐẦU keeps the "
            "time (2026.11.12)");
    constexpr std::uint64_t kMin = 60'000;
    const auto mins = [](std::uint64_t ms) {
        return std::to_string(ms / 60'000) + " min " + std::to_string(ms / 1000 % 60) + " s";
    };
    // A fresh worker at 10 and 60: no pass before 10 minutes, no shop look
    // before 60 -- the first plan riding the pass of its hour.
    {
        Starts s;
        const std::uint64_t t0 = s.l.now;
        s.start();
        s.run(61 * kMin);
        const std::vector<std::uint64_t>& p = s.l.pass_at;
        const std::vector<std::uint64_t>& k = s.l.look_at;
        check(p.size() == 6 && p.front() == t0 + 10 * kMin,
              "a fresh worker at 10 minutes: no pass before 10 minutes, the first at " +
                  (p.empty() ? std::string("none") : mins(p.front() - t0)) + ", 6 in 61 minutes");
        check(k.size() == 1 && k.front() >= t0 + 60 * kMin && k.front() <= t0 + 60 * kMin + s.l.pass_ms,
              "and at 60 minutes no shop look before 60 minutes: the first at " +
                  (k.empty() ? std::string("none") : mins(k.front() - t0)) + ", right after the hour's pass");
    }
    // Tự mua bình ticked on again at t: its first look at t + 60 minutes, the
    // one the start had due at 60 gone; Tự dọn rác likewise, t + 10.
    {
        Starts s;
        const std::uint64_t t0 = s.l.now;
        s.start();
        s.run(50 * kMin);
        const std::uint64_t t = s.l.now;
        s.tick_shop();
        s.run(80 * kMin);
        const std::vector<std::uint64_t>& k = s.l.look_at;
        check(!k.empty() && k.front() >= t + 60 * kMin && k.front() <= t + 60 * kMin + 10'000,
              "Tự mua bình ticked on again 50 minutes in: no look at 60, the first at " +
                  (k.empty() ? std::string("none") : mins(k.front() - t0)) + " -- 60 minutes after the tick");
        Starts d;
        d.start();
        d.run(4 * kMin);
        const std::uint64_t td = d.l.now;
        d.tick_pass();
        d.run(15 * kMin);
        check(!d.l.pass_at.empty() && d.l.pass_at.front() == td + 10 * kMin,
              "Tự dọn rác ticked on again 4 minutes in: the first pass 10 minutes after the tick, none at 10");
        // Ticked on with the bot stopped: from the next start.
        Starts e;
        const std::uint64_t e0 = e.l.now;
        e.start();
        e.run(5 * kMin);
        e.stop();
        e.tick_shop();
        e.run(kMin);
        const std::uint64_t te = e.l.now;
        e.start();
        e.run(61 * kMin);
        check(!e.l.look_at.empty() && e.l.look_at.front() >= te + 60 * kMin && !e.l.pass_at.empty() &&
                  e.l.pass_at.front() == e0 + 10 * kMin,
              "ticked on with the bot stopped: the first look 60 minutes after the next start; the pass's clock, not "
              "ticked, kept");
    }
    // A quick DỪNG and BẮT ĐẦU keeps what is left; a long one counts afresh;
    // what is left under 15 s (45 s) waits that long.
    {
        Starts s;
        const std::uint64_t t0 = s.l.now;
        s.start();
        s.run(4 * kMin);
        s.stop();
        s.run(kMin);
        s.start();
        s.run(57 * kMin);
        const std::vector<std::uint64_t>& k = s.l.look_at;
        check(!s.l.pass_at.empty() && s.l.pass_at.front() == t0 + 10 * kMin && !k.empty() &&
                  k.front() >= t0 + 60 * kMin && k.front() <= t0 + 60 * kMin + s.l.pass_ms,
              "DỪNG 4 minutes in and BẮT ĐẦU a minute later: the pass still at 10 and the shop at 60 from the first "
              "start -- the wait left kept, not 15 and 65");
        Starts g;
        g.start();
        g.run(4 * kMin);
        g.stop();
        g.run(66 * kMin);
        const std::uint64_t t1 = g.l.now;
        g.start();
        g.run(61 * kMin);
        check(!g.l.pass_at.empty() && g.l.pass_at.front() == t1 + 10 * kMin && !g.l.look_at.empty() &&
                  g.l.look_at.front() >= t1 + 60 * kMin,
              "stopped 66 minutes, both clocks run out meanwhile: counted afresh from the second start, 10 and 60 "
              "minutes -- nothing at the start");
        Starts h;
        const std::uint64_t h0 = h.l.now;
        h.start();
        h.run(10 * kMin - 5'000);
        h.stop();
        const std::uint64_t t2 = h.l.now;
        h.start();
        h.run(kMin);
        check(h.l.pass_at.size() == 1 && h.l.pass_at.front() == t2 + Starts::kPassFirst &&
                  bag::start_due_ms(h0 + 60 * kMin, h0 + 60 * kMin - 5'000, 60, Starts::kShopFirst) ==
                      h0 + 60 * kMin - 5'000 + Starts::kShopFirst,
              "5 s left when it starts again: the pass 15 s on (the shop's look 45 s), never at once");
    }
    // Mua ngay and Dọn ngay go at once, the bot running or not; a button's
    // worker leaves the clocks for the next start, which counts from itself;
    // one become a run counts from then.
    {
        Starts s;
        const std::uint64_t t0 = s.l.now;
        s.start();
        s.run(kMin);
        const std::uint64_t t = s.l.now;
        s.don_ngay();
        s.mua_ngay();
        s.run(11 * kMin);
        check(s.asked_pass == std::vector<std::uint64_t>{ t } && s.asked_look == std::vector<std::uint64_t>{ t } &&
                  !s.l.pass_at.empty() && s.l.pass_at.front() == t + 10 * kMin && t < t0 + 10 * kMin,
              "running, a minute in: Dọn ngay and Mua ngay at once, and the timed pass 10 minutes after the Dọn ngay");
        Starts b;
        const std::uint64_t b0 = b.l.now;
        b.don_ngay();
        b.mua_ngay();
        b.run(3 * kMin);
        const std::uint64_t t1 = b.l.now;
        b.start();
        b.run(61 * kMin);
        check(b.asked_pass == std::vector<std::uint64_t>{ b0 } && b.asked_look == std::vector<std::uint64_t>{ b0 } &&
                  !b.l.pass_at.empty() && b.l.pass_at.front() == t1 + 10 * kMin && !b.l.look_at.empty() &&
                  b.l.look_at.front() >= t1 + 60 * kMin,
              "the bot stopped: Dọn ngay and Mua ngay at once, each in a worker of its own; BẮT ĐẦU 3 minutes on "
              "counts from itself, 10 and 60 minutes");
        Starts c;
        c.start(true);
        c.don_ngay();
        c.run(5'000);
        const std::uint64_t t2 = c.l.now;
        c.become_run();
        c.run(11 * kMin);
        check(!c.l.pass_at.empty() && c.l.pass_at.front() == t2 + 10 * kMin,
              "BẮT ĐẦU while a Dọn ngay's worker works: it counts from then, the timed pass 10 minutes on");
    }
}

void test_start_count_sources() {
    section("The start count: where trash_loop counts, and what a button's job asks of it (source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string gui = read_file(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/gui_app.cpp");
    if (gui.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto has = [&](const std::string& part) { return gui.find(part) != npos; };
    const std::size_t a = gui.find("const bool timed = request == 0 && !trash_oneshot_;");
    const std::size_t b = a == npos ? npos : gui.find("const bool look_only = request == 2;", a);
    const std::string turn = b == npos ? std::string() : gui.substr(a, b - a);
    check(has("constexpr std::uint64_t kPassFirstMs = " + std::string("15'000;")) &&
              has("constexpr std::uint64_t kShopFirstMs = " + std::string("45'000;")) &&
              Starts::kPassFirst == 15'000 && Starts::kShopFirst == 45'000,
          "the least a kept clock waits, 15 s and 45 s, as gui_app.cpp has them");
    check(!turn.empty() && turn.find("counting") == npos && turn.find("request == 4 || request == 5 ||") != npos &&
              turn.find("if (request == 3 || (charm_due && !pass_due)) {") != npos &&
              turn.find("if (!settings.enabled || now_ms() < next) continue;") != npos,
          "the order of the jobs asks nothing of the count: Mua ngay, Thử mua, Dùng bùa ngay, Dọn ngay and Thử tìm "
          "go at once, only a timed pass waits for its clock");
    // What the bin may hold as the count starts holds every key off -- the
    // fighting and the potions too -- until a pass gives it back: that pass
    // comes 15 s on, as every start had it, not an interval.
    const std::size_t c = gui.find("const auto start_counting = [&] {");
    const std::size_t d = c == npos ? npos : gui.find("\n    };", c);
    const std::string counts = d == npos ? std::string() : gui.substr(c, d - c);
    const std::size_t g =
        counts.find("const bool give_back = (held || trash_left_held_) && next > now + kPassFirstMs;");
    check(g != npos && counts.find("if (give_back) next = now + kPassFirstMs;", g) != npos &&
              counts.find("next = bag::start_due_ms(kept.pass_due_ms,") < g &&
              counts.find("shop_due_ms = bag::start_due_ms(kept.shop_due_ms,") < g,
          "start_counting: with something the bin may hold, its pass 15 s on rather than an interval; the shop's "
          "look as counted");
#else
    skip("no source folder\n");
#endif
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
    check(in_order(loop, { "const bool shop_due = timed && !held && settings.buy_potions && now_ms() >= shop_due_ms &&",
                           "if (!settings.enabled) no_pull_before_ms = 0;", "const auto no_pull = [&] {",
                           "if (chat_doubt_ || keeper_cold_start_ || now_ms() < no_pull_before_ms || "
                           "!input_.window_valid() ||",
                           "!client_on_screen(input_.target_window()) || hp_percent_now() < 0.0f)",
                           "return true;", "std::lock_guard<std::mutex> lock(keeper_flow_mutex_);",
                           "return keeper_flow_ != nullptr;",
                           "const bool cannot_pull = shop_due && settings.enabled && was == bag::ShopRide::None && "
                           "no_pull();",
                           "const bool ride_due = shop_due && owed.troubles == 0;",
                           "ride = bag::ride_turn(was, ride_due, settings.enabled, cannot_pull, now_ms(), "
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
                           "ridden.shop_due = settings.buy_potions && now_ms() >= shop_due_ms &&",
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
        // A ride (`after_pass`, run_shop's own) begins no plan; buying after
        // each pass (`after_pass_buys`, 2026.11.13b) the pass's look owed does.
        check(!plan.empty() && !has(plan, "after_pass)") && !has(plan, "(after_pass") &&
                  !has(plan, "after_pass ") &&
                  has(plan, "if (bag::shop_plan_begins(forced, after_pass_buys, look_owed, now_ms() >= plan_due_ms)) "
                            "{") &&
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
              // A job that runs forgets them: the pass, the potion round and,
              // since 2026.11.24, the pet's round on reaching TổCổLong.
              has(gui, "if (why == put_off_why) return;") && count_in(gui, "put_off_why.clear();") == 3 &&
              // The pull said once until a job runs or a new reason puts a
              // look off -- not dropped for any reason still stored (R4): its
              // declaration and four resets.
              has(gui, "bool pull_said = false;") && count_in(gui, "pull_said = false;") == 5 &&
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
    check(in_order(card, { "if (simple_clean_.buy_potions && simple_clean_.enabled && !after_pass) {",
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

// ------------------------------- "Mua ngay sau khi dọn rác" (2026.11.13b)

void test_after_trash() {
    section("\"Mua ngay sau khi dọn rác\": a plan right after each timed pass, the clock beginning none "
            "(2026.11.13b)");
    CleanSettings s;
    s.buy_potions = true;
    s.enabled = true;
    s.buy_after_trash = true;
    check(bag::buys_after_pass(s), "the box, Tự mua bình and Tự dọn rác on: buys after each pass");
    s.enabled = false;
    check(!bag::buys_after_pass(s), "Tự dọn rác off: the clock buys, as before");
    s.enabled = true;
    s.buy_potions = false;
    check(!bag::buys_after_pass(s), "Tự mua bình off: nothing");
    s.buy_potions = true;
    s.buy_after_trash = false;
    check(!bag::buys_after_pass(s), "the box off: the clock");

    // The clock's own looks: buying after each pass, only what a pass or a
    // plan still owes.
    check(!bag::shop_clock_looks(true, false, 0) && bag::shop_clock_looks(true, true, 0) &&
              bag::shop_clock_looks(true, false, 3) && bag::shop_clock_looks(false, false, 0),
          "after each pass: the clock looks for a pass's owed look or a plan's part, never a plan of its own; "
          "otherwise always");
    // A plan begins: Mua ngay always; after each pass the pass's look and no
    // plan time; otherwise the plan time.
    check(bag::shop_plan_begins(true, true, false, false) && bag::shop_plan_begins(true, false, false, false) &&
              bag::shop_plan_begins(false, true, true, false) && !bag::shop_plan_begins(false, true, false, true) &&
              bag::shop_plan_begins(false, false, false, true) && !bag::shop_plan_begins(false, false, true, false),
          "a plan: Mua ngay always; after each pass the pass's look, never the plan's time; otherwise the plan's "
          "time alone");

    // Stepped as the worker steps it: three passes 10 min apart, a plan of 2
    // HP + 2 MP bought 2 a hold; the clock's 60 min never begins one.
    {
        const bool after = true;
        bool look_owed = false;
        int owed = 0, plans = 0, rounds = 0;
        std::uint64_t plan_due = 0;
        for (int pass = 0; pass < 3; ++pass) {
            const std::uint64_t now = 600'000ull * static_cast<std::uint64_t>(pass + 1);
            look_owed = true;                                   // the pass ran, timed
            if (bag::shop_clock_looks(after, look_owed, owed)) {
                ++rounds;
                if (bag::shop_plan_begins(false, after, look_owed, now >= plan_due)) {
                    look_owed = false;
                    ++plans;
                    owed = 4;
                    plan_due = now + 3'600'000ull;
                }
                owed -= std::min(owed, 2);                      // kShopBuysPerHold's worth
            }
            // The part's gap on: the clock looks for the rest.
            if (bag::shop_clock_looks(after, look_owed, owed)) {
                ++rounds;
                owed -= std::min(owed, 2);
            }
            check(owed == 0 && !look_owed && !bag::shop_clock_looks(after, look_owed, owed),
                  "pass " + std::to_string(pass + 1) + ": the plan bought, its part a gap on, and then no look of the "
                  "clock's");
        }
        check(plans == 3 && rounds == 6, "three passes, three plans, six rounds -- got " + std::to_string(plans) +
                                             " plans, " + std::to_string(rounds) + " rounds");
    }
    // The review of 2026.11.13b: a plan of 20, BagCleaner::kShopBuysPerHold
    // (10) a hold, its part's look kShopPartGapMs after the round -- never
    // fresh by kRideFreshMs, so bag::ride_turn pulls a pass for it. That pass
    // owes no look while the part is owed (pass_owes_look): the part is
    // bought, the plan not begun afresh -- before, every part's pass began it
    // again, and it bought until the bag was full.
    {
        const bool after = true;
        bool look_owed = false;
        int owed = 0, plans = 0, bought = 0;
        std::uint64_t now = 600'000;
        std::uint64_t last_pass_end = 0;
        // The first timed pass, and the round that rides it.
        last_pass_end = now;
        if (bag::pass_owes_look(after, owed)) look_owed = true;
        if (bag::shop_plan_begins(false, after, look_owed, false)) {
            look_owed = false;
            ++plans;
            owed = 20;
        }
        const int first = std::min(owed, BagCleaner::kShopBuysPerHold);
        owed -= first;
        bought += first;
        // The part's look, a part's gap after the round (taken as ending at
        // the pass's end, the freshest it could be).
        now = last_pass_end + BagCleaner::kShopPartGapMs;
        const ShopRide turn =
            bag::ride_turn(ShopRide::None, bag::shop_clock_looks(after, look_owed, owed), true, false, now,
                           last_pass_end);
        check(turn == ShopRide::Pending,
              "the part's look comes kShopPartGapMs after its round, never fresh: it pulls a pass -- " +
                  std::string(said(turn)));
        // That pass, and the round that rides it.
        last_pass_end = now;
        if (bag::pass_owes_look(after, owed)) look_owed = true;
        if (bag::shop_plan_begins(false, after, look_owed, false)) {
            look_owed = false;
            ++plans;
            owed = 20;
        }
        const int second = std::min(owed, BagCleaner::kShopBuysPerHold);
        owed -= second;
        bought += second;
        check(plans == 1 && bought == 20 && owed == 0 && !look_owed &&
                  !bag::shop_clock_looks(after, look_owed, owed),
              "a plan of 20: its part bought on the pass it pulled, the plan not begun afresh -- " +
                  std::to_string(plans) + " plan(s), " + std::to_string(bought) + " bought");
        check(!bag::pass_owes_look(after, 10) && bag::pass_owes_look(after, 0) && !bag::pass_owes_look(false, 0),
              "a pass owes a look only buying after each pass, with nothing of a plan owed");
    }

#ifdef GOBOT_SOURCE_DIR
    const std::string root = GOBOT_SOURCE_DIR;
    const std::string gui = read_file(root + "/apps/gobot_gui/gui_app.cpp");
    const std::string card = read_file(root + "/apps/gobot_gui/simple_shop.cpp");
    const std::string ini = read_file(root + "/configs/trash.ini");
    if (gui.empty() || card.empty() || ini.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const auto has = [](const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; };
    check(has(gui, "bag::shop_clock_looks(bag::buys_after_pass(settings), look_owed, owed.hp + owed.mp);") &&
              has(gui, "if (after_pass_buys && ridden.timed && !look_only && !trash_stop_) {") &&
              has(gui, "if (bag::pass_owes_look(after_pass_buys, owed.hp + owed.mp)) look_owed = true;") &&
              // A timed pass with nothing it could do owes it too (the review).
              has(gui, "if (request != 0 || !bag::pass_owes_look(bag::buys_after_pass(settings), owed.hp + owed.mp)) "
                       "return;") &&
              [&] {
                  int n = 0;
                  for (std::size_t at = gui.find("owe_look_unrun();"); at != std::string::npos;
                       at = gui.find("owe_look_unrun();", at + 1))
                      ++n;
                  return n == 2;
              }() &&
              // Kept across workers, and given back when a Mua ngay did not run.
              has(gui, "bool look_owed = kept.look_owed;") && has(gui, "trash_clocks_.look_owed = look_owed;") &&
              has(gui, "look_owed = look_before;") &&
              has(gui, "shop_due_ms = std::min(shop_due_ms, now_ms());") &&
              has(gui, "bag::shop_clock_looks(after_pass_buys, look_owed, owed.hp + owed.mp);") &&
              has(gui, "if (bag::shop_plan_begins(forced, after_pass_buys, look_owed, now_ms() >= plan_due_ms)) {") &&
              has(gui, "if (!after_now && settings.buy_potions && owed.hp + owed.mp == 0) shop_afresh();") &&
              has(gui, "\"right after each timed pass, buy_after_trash\""),
          "gui_app.cpp: the clock's looks and the ride by shop_clock_looks, every timed pass owing a look, a plan by "
          "shop_plan_begins, the clock afresh as the box goes off, the ride said in the log");
    check(has(card, "ImGui::Checkbox(\"Mua ngay sau khi dọn rác\", &simple_clean_.buy_after_trash)") &&
              has(card, "put(\"buy_after_trash\", simple_clean_.buy_after_trash ? \"true\" : \"false\");") &&
              has(card, "ImGui::BeginDisabled(after_pass);") &&
              has(card, "const bool after_pass = simple_clean_.buy_after_trash && simple_clean_.enabled;") &&
              has(card, "\"Mua bình ngay sau mỗi lần Dọn rác theo giờ.\""),
          "simple_shop.cpp: the box saved as buy_after_trash, \"mua mỗi\" greyed with it and Dọn rác on, the plan's "
          "line saying so");
    check(has(ini, "\nbuy_after_trash=false\n"), "configs/trash.ini ships buy_after_trash=false");
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
    test_start_count();
    test_start_count_sources();
    test_ride_refused();
    test_ride_sources();
    test_after_trash();
}
