// Your HP and MP from the packets (the packets plan, item D): when they go,
// and the bars' figures set beside them for the log. See gobot/vitals_trust.hpp.
// Pure: GoBot's window feeds it and asks it (apps/gobot_gui/packets_vitals.cpp).

#include "gobot/vitals_trust.hpp"

#include <algorithm>
#include <string>

namespace gobot {
namespace {

/// A figure the reader made short of the packets' value: one digit lost
/// anywhere, or any number of them off the front.
bool lost_digits(int figure, std::uint32_t packet) {
    return figure_lost_a_digit(figure, packet) || figure_lost_leading_digits(figure, packet);
}

int rank(BarCompare::Verdict v) {
    switch (v) {
        case BarCompare::Verdict::Same:  return 0;
        case BarCompare::Verdict::Short: return 1;
        case BarCompare::Verdict::Other: return 2;
    }
    return 2;
}

} // namespace

float vitals_percent(std::uint32_t value, std::uint32_t max) {
    if (max == 0) return -1.0f;
    const float pct = static_cast<float>(value) * 100.0f / static_cast<float>(max);
    return std::clamp(pct, 0.0f, 100.0f);
}

// ---- BarCompare ----

BarCompare::Verdict BarCompare::compare(const Point& p, const Read& r) {
    // The figures printed on the bar are the game's own numbers: the packet
    // value is them or it is not. A maximum the packets do not know yet (the
    // highest value seen) is not held against them.
    const std::uint32_t fmax = static_cast<std::uint32_t>(std::max(0, r.figure_max));
    const bool max_same = !p.max_known || p.max == fmax;
    const bool max_short = !max_same && lost_digits(r.figure_max, p.max);
    if (!max_same && !max_short) return Verdict::Other;
    if (r.figure >= 0 && p.value == static_cast<std::uint32_t>(r.figure))
        return max_same ? Verdict::Same : Verdict::Short;
    return lost_digits(r.figure, p.value) ? Verdict::Short : Verdict::Other;
}

void BarCompare::packet(const Point& p) {
    // In the order they came; one out of order -- never, from one capture --
    // is put where its time says.
    auto at = points_.end();
    while (at != points_.begin() && (at - 1)->at_ms > p.at_ms) --at;
    points_.insert(at, p);
    if (points_.size() > kPoints) points_.erase(points_.begin(), points_.begin() + (points_.size() - kPoints));
}

void BarCompare::max_changed() {
    for (Point& p : points_) p.max_known = false;
}

void BarCompare::screen(const Read& r) {
    if (r.read_at_ms == 0 || r.figure < 0 || r.figure_max <= 0) return;
    pending_.push_back(r);
    if (pending_.size() > kPending) pending_.erase(pending_.begin(), pending_.begin() + (pending_.size() - kPending));
}

void BarCompare::settle(std::uint64_t now_ms, std::uint32_t epoch) {
    std::size_t done = 0;
    for (; done < pending_.size(); ++done) {
        const Read r = pending_[done];
        if (r.read_at_ms + kLeadMs + kJudgeSlackMs > now_ms) break;
        const std::uint64_t from = r.read_at_ms > kBeforeMs ? r.read_at_ms - kBeforeMs : 0;
        const std::uint64_t to = r.read_at_ms + kLeadMs;
        // The value in force as the window opened, and every one that came
        // inside it, of this epoch: the best verdict among them, and for one
        // not Same the value nearest the figure.
        bool any = false;
        Verdict best = Verdict::Other;
        Point at;
        std::uint32_t off = 0;
        const auto consider = [&](const Point& p) {
            if (p.epoch != epoch) return;
            const Verdict v = compare(p, r);
            const std::uint32_t fig = static_cast<std::uint32_t>(r.figure);
            const std::uint32_t d = p.value > fig ? p.value - fig : fig - p.value;
            if (!any || rank(v) < rank(best) || (v == best && v == Verdict::Other && d < off)) {
                best = v;
                at = p;
                off = d;
            }
            any = true;
        };
        const Point* base = nullptr;
        for (const Point& p : points_) {
            if (p.at_ms <= from) {
                base = &p;
                continue;
            }
            if (p.at_ms > to) break;
            consider(p);
        }
        if (base != nullptr) consider(*base);
        if (!any) {
            ++counts_.no_packet;
            continue;
        }
        switch (best) {
            case Verdict::Same:  ++counts_.same; break;
            case Verdict::Short: ++counts_.short_digits; break;
            case Verdict::Other: ++counts_.other; break;
        }
        if (best != Verdict::Same) {
            odd_.push_back(Odd{ best, r, at });
            if (odd_.size() > kOddKept) odd_.erase(odd_.begin());
        }
    }
    pending_.erase(pending_.begin(), pending_.begin() + done);
}

BarCompare::Counts BarCompare::take_counts() {
    const Counts out = counts_;
    counts_ = Counts{};
    return out;
}

std::vector<BarCompare::Odd> BarCompare::take_odd() {
    std::vector<Odd> out;
    out.swap(odd_);
    return out;
}

// ---- PeriodWindow ----

void PeriodWindow::note(std::uint64_t now_ms, int period_ms) {
    if (n_ > 0 && spans_[n_ - 1].period_ms == period_ms) return;
    if (n_ == kKept) {
        std::copy(spans_.begin() + 1, spans_.end(), spans_.begin());
        --n_;
    }
    spans_[n_++] = Span{ now_ms, period_ms };
}

int PeriodWindow::longest(std::uint64_t now_ms) const {
    const std::uint64_t from = now_ms > kSpanMs ? now_ms - kSpanMs : 0;
    int out = 0;
    for (std::size_t i = 0; i < n_; ++i) {
        // In use from its start until the next one's: within the span when
        // it lasted past its beginning.
        const bool last = i + 1 == n_;
        if (last || spans_[i + 1].from_ms > from) out = std::max(out, spans_[i].period_ms);
    }
    return out;
}

std::uint64_t world_window_ms(int longest_period_ms, std::uint64_t floor_ms) {
    const std::uint64_t p = longest_period_ms > 0 ? static_cast<std::uint64_t>(longest_period_ms) : 0;
    return std::max(floor_ms, 2 * p + kWorldWindowWorkMs);
}

// ---- usable, resting, a map loading ----

const char* vitals_unusable_why(const VitalsUsable& in, std::uint64_t now_ms) {
    if (!in.use_on) return "[Packets] vitals=off";
    if (!in.capturing) return "no capture";
    if (!in.on_window) return "the capture is not the game window's own";
    if (in.self_starts != in.capture_starts) return "the capture started again, and nothing has come since";
    if (in.value_ms == 0 || in.value_epoch != in.epoch)
        return "no value since the capture started or since a gap in it";
    if (in.last_server_ms == 0 || (now_ms > in.last_server_ms && now_ms - in.last_server_ms > kLinkQuietMs))
        return "the server quiet for 5 s";
    if (!in.max_known) return "no maximum yet";
    return "";
}

bool vitals_usable(const VitalsUsable& in, std::uint64_t now_ms) { return *vitals_unusable_why(in, now_ms) == '\0'; }

bool vitals_rest_allowed(const VitalsRest& in, std::uint64_t now_ms) {
    if (!in.hp_usable || !in.mp_usable || in.usable_since_ms == 0) return false;
    if (now_ms < in.usable_since_ms || now_ms - in.usable_since_ms < kRestAfterUsableMs) return false;
    // A pet potion key is no reason to keep the run's pace: it rests quicker
    // (vitals_rest_period_ms), as the pet's HP is only on the screen.
    if (in.critical || in.finding_bars || in.hp_zero_since_ms != 0) return false;
    if (in.death_seen_ms != 0 && (now_ms < in.death_seen_ms || now_ms - in.death_seen_ms < kRestAfterDeathMs))
        return false;
    return true;
}

int vitals_rest_period_ms(int run_ms, int idle_ms, bool pet_key) {
    return std::max(run_ms, std::min(idle_ms, pet_key ? kRestPetMs : kRestMostMs));
}

bool map_load_holds(const MapLoad& in, std::uint64_t now_ms) {
    if (!in.on_window || in.entry_ms == 0 || in.last_server_ms == 0) return false;
    if (now_ms > in.last_server_ms && now_ms - in.last_server_ms > kLinkQuietMs) return false;
    if (now_ms > in.entry_ms && now_ms - in.entry_ms >= kLoadHoldMs) return false;
    return in.last_own_ms < in.entry_ms;
}

bool figure_lost_leading_one(int figure, std::uint32_t packet) {
    if (figure < 0 || packet < 100) return false;
    // The leading digit's place: 10000 for 14798.
    std::uint32_t place = 1;
    while (packet / place >= 10) place *= 10;
    return packet / place == 1 && static_cast<std::uint32_t>(figure) == packet - place;
}

bool figure_lost_a_digit(int figure, std::uint32_t packet) {
    if (figure < 0 || packet < 100) return false;
    const std::string whole = std::to_string(packet);
    const std::string fig = std::to_string(figure);
    if (fig.size() >= whole.size()) return false;
    for (std::size_t i = 0; i < whole.size(); ++i) {
        std::string less = whole;
        less.erase(i, 1);
        // As the reader gives it: "0500" (10500 less its 1) is 500.
        const std::size_t lead = std::min(less.find_first_not_of('0'), less.size() - 1);
        if (less.compare(lead, std::string::npos, fig) == 0) return true;
    }
    return false;
}

bool figure_lost_leading_digits(int figure, std::uint32_t packet) {
    // 0 is HP at 0 -- a death is the screen's to say -- never digits lost.
    if (figure <= 0 || packet < 100) return false;
    const std::uint64_t whole = packet;
    // Each place under the packet value's first digit: what is left once the
    // digits from there up are gone. "0500" (10500 less its 1) reads as 500.
    for (std::uint64_t place = 10; place <= whole; place *= 10)
        if (whole % place == static_cast<std::uint64_t>(figure)) return true;
    return false;
}

} // namespace gobot
