// The gate between your HP and MP from the packets and the bars on the
// screen (the packets plan, item D). See gobot/vitals_trust.hpp. Pure: GoBot's
// window feeds it and asks it (apps/gobot_gui/packets_vitals.cpp).

#include "gobot/vitals_trust.hpp"

#include <algorithm>
#include <cmath>

namespace gobot {
namespace {

bool contains(const std::vector<std::uint32_t>& values, std::uint32_t v) {
    return std::find(values.begin(), values.end(), v) != values.end();
}

/// `v` remembered among `values`, at most `most` of them.
void remember(std::vector<std::uint32_t>& values, std::uint32_t v, std::size_t most) {
    if (!contains(values, v) && values.size() < most) values.push_back(v);
}

} // namespace

// ---- BarCheck ----

BarCheck::BarCheck(bool hp) : hp_(hp) {}

const char* BarCheck::state_name(State s) {
    switch (s) {
        case State::Learning: return "learning";
        case State::Trusted:  return "trusted";
        case State::Doubt:    return "in doubt";
        case State::Off:      return "off";
    }
    return "?";
}

float BarCheck::percent(std::uint32_t value, std::uint32_t max) {
    if (max == 0) return -1.0f;
    const float pct = static_cast<float>(value) * 100.0f / static_cast<float>(max);
    return std::clamp(pct, 0.0f, 100.0f);
}

BarCheck::Verdict BarCheck::judge(const Point& got, const Read& read, bool& judged, float figure_slack_pct) {
    judged = true;
    // The figures printed on the bar are the game's own numbers: the packet
    // value is them or it is not. A maximum the packets do not know yet (the
    // highest value seen) is not held against it.
    if (read.figure >= 0 && read.figure_max > 0) {
        const std::uint32_t fmax = static_cast<std::uint32_t>(read.figure_max);
        if (got.max_known && got.max != fmax) return Verdict::Disagree;
        const std::uint32_t fig = static_cast<std::uint32_t>(read.figure);
        if (got.value == fig) return Verdict::Agree;
        // The HUD ahead of 0x0051 by a hit or a cast: between, no step either
        // way -- within the slack, which settle() gives only a trust earned.
        const std::uint32_t off = got.value > fig ? got.value - fig : fig - got.value;
        const float apart = static_cast<float>(off) * 100.0f / static_cast<float>(fmax);
        return apart <= figure_slack_pct ? Verdict::Between : Verdict::Disagree;
    }
    // The bar's length: near enough agrees, far enough disagrees, and the
    // band between says nothing.
    if (!got.max_known || got.max == 0) {
        judged = false;
        return Verdict::Between;
    }
    const float d = std::fabs(percent(got.value, got.max) - read.pct);
    if (d <= kAgreePoints) return Verdict::Agree;
    if (d > kDisagreePoints) return Verdict::Disagree;
    return Verdict::Between;
}

void BarCheck::packet(const Point& p) {
    // In the order they came; one out of order -- never, from one capture --
    // is put where its time says.
    auto at = points_.end();
    while (at != points_.begin() && (at - 1)->at_ms > p.at_ms) --at;
    points_.insert(at, p);
    if (points_.size() > kPoints) points_.erase(points_.begin(), points_.begin() + (points_.size() - kPoints));
}

void BarCheck::max_changed() {
    for (Point& p : points_) p.max_known = false;
}

void BarCheck::screen(const Read& r) {
    if (r.pct < 0.0f || r.read_at_ms == 0) return;
    pending_.push_back(r);
    if (pending_.size() > kPending) pending_.erase(pending_.begin(), pending_.begin() + (pending_.size() - kPending));
}

void BarCheck::over_max(std::uint64_t now_ms) {
    ++counts_.over_max;
    apply(Verdict::Disagree, Read{}, Point{}, -1.0f, true, now_ms);
}

void BarCheck::settle(std::uint64_t now_ms, std::uint32_t epoch) {
    std::size_t done = 0;
    for (; done < pending_.size(); ++done) {
        const Read r = pending_[done];
        if (r.read_at_ms + kHalfWindowMs + kJudgeSlackMs > now_ms) break;
        if (figures_only_ && (r.figure < 0 || r.figure_max <= 0) && r.pct >= kBarTrueBelowPct) {
            ++counts_.no_figures;
            continue;
        }
        if (figures_only_ && (r.figure < 0 || r.figure_max <= 0) && r.pct < kEmptyBelowPct) {
            ++counts_.empty;
            continue;
        }
        const std::uint64_t from = r.read_at_ms > kHalfWindowMs ? r.read_at_ms - kHalfWindowMs : 0;
        const std::uint64_t to = r.read_at_ms + kHalfWindowMs;
        // The value in force as the window opened, and every one that came
        // inside it: the same value throughout, or nothing is said.
        const Point* base = nullptr;
        bool moving = false;
        bool stale = false;
        for (const Point& p : points_) {
            if (p.at_ms <= from) {
                base = &p;
                continue;
            }
            if (p.at_ms > to) break;
            if (base == nullptr || p.value != base->value || p.max != base->max) moving = true;
            if (p.epoch != epoch) stale = true;
        }
        if (base == nullptr) {
            ++counts_.no_packet;
            continue;
        }
        if (moving) {
            ++counts_.moving;
            continue;
        }
        if (stale || base->epoch != epoch) {
            ++counts_.stale;
            continue;
        }
        // A trust earned allows for the HUD's lead over 0x0051; while learning
        // only the figures exactly, at the read, count -- as hard to earn as
        // ever.
        const bool keeping = state_ == State::Trusted || state_ == State::Doubt;
        bool judged = false;
        Verdict v = judge(*base, r, judged, keeping ? (hp_ ? kFigureSlackHpPct : kFigureSlackMpPct) : 0.0f);
        if (!judged) {
            ++counts_.no_maximum;
            continue;
        }
        // Figures that did not agree, 0x0051 may yet reach: the read -- and
        // every one after it, judged in their order -- waits kLeadMs for that.
        // A value equal to them by then, of this epoch and not of another
        // maximum, agrees.
        const Point* against = base;
        if (keeping && v != Verdict::Agree && r.figure >= 0 && r.figure_max > 0) {
            if (r.read_at_ms + kLeadMs + kJudgeSlackMs > now_ms) break;
            for (const Point& p : points_) {
                if (p.at_ms <= to) continue;
                if (p.at_ms > r.read_at_ms + kLeadMs) break;
                if (p.epoch == epoch && p.value == static_cast<std::uint32_t>(r.figure) &&
                    (!p.max_known || p.max == static_cast<std::uint32_t>(r.figure_max))) {
                    v = Verdict::Agree;
                    against = &p;
                    ++counts_.caught_up;
                    break;
                }
            }
        }
        apply(v, r, *against, against->max_known ? percent(against->value, against->max) : -1.0f, false, now_ms);
    }
    pending_.erase(pending_.begin(), pending_.begin() + done);
}

int BarCheck::recent_disagreements() const {
    const std::size_t n = std::min(recent_n_, kRecent);
    int out = 0;
    for (std::size_t i = 0; i < n; ++i) out += recent_[(recent_n_ - 1 - i) % kRecent] ? 1 : 0;
    return out;
}

void BarCheck::apply(Verdict v, const Read& r, const Point& p, float packet_pct, bool over, std::uint64_t now) {
    const bool exact = r.figure >= 0 && r.figure_max > 0;
    switch (v) {
        case Verdict::Between:
            // Counts for nothing: neither a step towards trust nor away.
            ++counts_.between;
            return;
        case Verdict::Agree:
            ++counts_.agree;
            if (exact) ++counts_.exact;
            remember(count_values_, p.value, kValuesKept);
            counts_.values = count_values_.size();
            if (counts_.lowest < 0.0f || r.pct < counts_.lowest) counts_.lowest = r.pct;
            recent_[recent_n_++ % kRecent] = false;
            row_ = 0;
            episode_ = false;
            break;
        case Verdict::Disagree: {
            ++counts_.disagree;
            // Kept for the log with the state it was judged in; an over_max
            // has no read to keep.
            if (!over && r.pct >= 0.0f) {
                disagreements_.push_back(Disagreement{ now, state_, r, p });
                if (disagreements_.size() > kDisagreementsKept) disagreements_.erase(disagreements_.begin());
            }
            // One HUD moment against one packet value is one disagreement,
            // however many reads see it: reads come every 150 ms, and two of
            // them 163 ms apart, 11031 against one 12643, put MP off for the
            // session (2026-11-03 22:44:28.552 and .715). Each new packet
            // value starts a new one -- not each 0x0051: a frame that moved
            // only the other bar repeats this bar's value at a new time.
            const int pct = static_cast<int>(std::lround(r.pct));
            if (!over && episode_ && episode_value_ == p.value && episode_max_ == p.max &&
                episode_epoch_ == p.epoch && episode_figure_ == r.figure && episode_pct_ == pct) {
                ++counts_.same_episode;
                return;
            }
            episode_ = !over;
            episode_value_ = p.value;
            episode_max_ = p.max;
            episode_epoch_ = p.epoch;
            episode_figure_ = r.figure;
            episode_pct_ = pct;
            recent_[recent_n_++ % kRecent] = true;
            ++row_;
            break;
        }
    }

    switch (state_) {
        case State::Off:
            return;
        case State::Learning:
            if (v == Verdict::Agree) {
                ++learn_agree_;
                if (exact) ++learn_exact_;
                remember(learn_values_, p.value, kValuesKept);
                if (r.pct < kLowPct) low_seen_ = true;
                if (learn_agree_ >= needed_ && static_cast<int>(learn_values_.size()) >= kTrustValues &&
                    recent_disagreements() == 0 && (!hp_ || low_seen_))
                    move_to(State::Trusted, now, r, p, packet_pct, over, false);
            }
            return;
        case State::Trusted:
        case State::Doubt:
            if (v == Verdict::Disagree) {
                const bool row = row_ >= kDisagreeRow;
                if (row || recent_disagreements() >= kDisagreeRecent) {
                    // Distrusted: learnt again from the start, and for longer.
                    ++distrusts_;
                    move_to(distrusts_ >= kDistrustsOff ? State::Off : State::Learning, now, r, p, packet_pct, over,
                            row);
                } else if (state_ == State::Trusted) {
                    move_to(State::Doubt, now, r, p, packet_pct, over, false);
                } else {
                    // Still in doubt: the agreements in a row start again.
                    doubt_agree_ = 0;
                }
            } else if (state_ == State::Doubt && ++doubt_agree_ >= kDoubtClears) {
                move_to(State::Trusted, now, r, p, packet_pct, over, false);
            }
            return;
    }
}

void BarCheck::move_to(State to, std::uint64_t now, const Read& r, const Point& p, float packet_pct, bool over,
                       bool in_a_row) {
    Change c;
    c.from = state_;
    c.to = to;
    c.at_ms = now;
    c.read = r;
    c.packet = p;
    c.packet_pct = packet_pct;
    c.over_max = over;
    c.in_a_row = in_a_row;
    c.agreements = learn_agree_;
    c.values = static_cast<int>(learn_values_.size());
    c.exact = learn_exact_;
    c.distrusts = distrusts_;
    changes_.push_back(c);

    state_ = to;
    since_ms_ = now;
    doubt_agree_ = 0;
    if (to == State::Learning || to == State::Off) {
        learn_agree_ = 0;
        learn_exact_ = 0;
        learn_values_.clear();
        low_seen_ = false;
        needed_ = kRelearnAgreements;
    }
}

BarCheck::Progress BarCheck::progress() const {
    Progress out;
    out.state = state_;
    out.since_ms = since_ms_;
    out.agreements = learn_agree_;
    out.needed = needed_;
    out.values = static_cast<int>(learn_values_.size());
    out.low_seen = low_seen_;
    out.doubt_agreements = doubt_agree_;
    out.distrusts = distrusts_;
    return out;
}

std::vector<BarCheck::Change> BarCheck::take_changes() {
    std::vector<Change> out;
    out.swap(changes_);
    return out;
}

BarCheck::Counts BarCheck::take_counts() {
    const Counts out = counts_;
    counts_ = Counts{};
    count_values_.clear();
    return out;
}

std::vector<BarCheck::Disagreement> BarCheck::take_disagreements() {
    std::vector<Disagreement> out;
    out.swap(disagreements_);
    return out;
}

// ---- HandOver ----

void HandOver::start(float pct, std::uint64_t read_at_ms) {
    pct_ = pct;
    read_at_ = read_at_ms;
    live_ = pct >= 0.0f && read_at_ms != 0;
    ++started_;
}

float HandOver::give(std::uint64_t now_ms, std::uint64_t screen_at_ms, std::uint64_t stale_ms) {
    if (!live_) return -1.0f;
    // The screen has a newer read of its own to give: it decides from here.
    // That very read is no reason to stop -- the pace cut back to the run's
    // on the next tick takes it from the screen (paced_from_ms) until a newer
    // one lands. Or this one is too old to drink on: nobody's then.
    if (screen_at_ms > read_at_ || now_ms > read_at_ + stale_ms) {
        live_ = false;
        return -1.0f;
    }
    return pct_;
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
    if (!in.reading) return "the bars are not being read, so nothing checks the packets";
    if (!in.trusted) return "the check against the bars says no";
    return "";
}

bool vitals_usable(const VitalsUsable& in, std::uint64_t now_ms) { return *vitals_unusable_why(in, now_ms) == '\0'; }

bool vitals_rest_allowed(const VitalsRest& in, std::uint64_t now_ms) {
    if (!in.hp_usable || !in.mp_usable || in.usable_since_ms == 0) return false;
    if (now_ms < in.usable_since_ms || now_ms - in.usable_since_ms < kRestAfterUsableMs) return false;
    if (in.pet_key || in.critical || in.finding_bars || in.hp_zero_since_ms != 0) return false;
    if (in.death_seen_ms != 0 && (now_ms < in.death_seen_ms || now_ms - in.death_seen_ms < kRestAfterDeathMs))
        return false;
    return true;
}

int vitals_rest_period_ms(int run_ms, int idle_ms) { return std::max(run_ms, std::min(idle_ms, kRestMostMs)); }

bool map_load_holds(const MapLoad& in, std::uint64_t now_ms) {
    if (!in.on_window || in.entry_ms == 0 || in.last_server_ms == 0) return false;
    if (now_ms > in.last_server_ms && now_ms - in.last_server_ms > kLinkQuietMs) return false;
    if (now_ms > in.entry_ms && now_ms - in.entry_ms >= kLoadHoldMs) return false;
    return in.last_own_ms < in.entry_ms;
}

} // namespace gobot
