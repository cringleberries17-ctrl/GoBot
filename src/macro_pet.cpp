// The pet pressed over and over (MacroConfig::pet_every_ms, 2026.11.10): the
// owner's "Bơm máu pet chậm" -- GoBot's own 3-s wait between two of the pet's
// foods kept the pet under its mark half the time on 2026-11-10, where GoC
// presses it every 200 ms. Here: what came of each press (PetHealWatch, the
// minute's "Pet heals" line, the plateau's picture asked for), and the turn a
// skill gets after the pet's own press (pet_yields). See gobot/macro.hpp.
//
// All of it but take_pet_plateau runs on the timer pass's thread, or a test's
// step(): its state is that pass's alone, and arm()'s at a start.

#include "gobot/macro.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace gobot {
namespace {

/// A percent as the line says it: rounded.
int round_pct(float pct) { return static_cast<int>(std::lround(pct)); }

/// The middle one of `v`, the higher of the two middle ones for an even count.
template <typename T>
T median_of(std::vector<T> v) {
    if (v.empty()) return T{};
    const auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2);
    std::nth_element(v.begin(), mid, v.end());
    return *mid;
}

} // namespace

// ------------------------------------------------------------- the watch

void PetHealWatch::reset() {
    open_.clear();
    closed_.clear();
    last_ = -1.0f;
    rise_at_ = rise_down_ = rose_down_ = 0;
    minute_ms_ = 0;
    keycode_ = 0;
    pressed_ = judged_ = rose_ = unread_ = moving_ = 0;
    rises_.clear();
    delays_.clear();
    gap_ms_ = 0;
    runs_.clear();
    in_run_ = false;
}

void PetHealWatch::pressed(std::uint32_t keycode, std::uint64_t down_ms, float before) {
    Press p;
    p.o.keycode = keycode;
    p.o.down_ms = down_ms;
    p.o.before = before;
    open_.push_back(p);
    keycode_ = keycode;
    ++pressed_;
}

void PetHealWatch::reading(std::uint64_t at_ms, float pct) {
    if (pct < 0.0f) {
        // No rise is made out across a gap in the readings.
        last_ = -1.0f;
    } else {
        if (last_ >= 0.0f && pct >= last_ + MacroConfig::kRecheckRisePct) {
            const float jump = pct - last_;
            if (rise_at_ != 0 && at_ms <= rise_at_ + kRiseMergeMs) {
                // The rest of the heal just seen: to its press, if it had one.
                for (Press& p : open_)
                    if (rise_down_ != 0 && p.o.down_ms == rise_down_ && p.o.rose) {
                        p.o.rise += jump;
                        break;
                    }
            } else {
                // A heal of its own: the earliest press still open that the
                // game could have answered by now. None -- something else
                // healed the pet -- is no press's.
                Press* to = nullptr;
                for (Press& p : open_)
                    if (!p.o.rose && at_ms >= p.o.down_ms + kRiseLeastMs) {
                        to = &p;
                        break;
                    }
                rise_at_ = std::max<std::uint64_t>(at_ms, 1);
                rise_down_ = 0;
                if (to) {
                    to->o.rose = true;
                    to->o.rise = jump;
                    to->o.delay_ms = at_ms - to->o.down_ms;
                    rise_down_ = to->o.down_ms;
                }
            }
        }
        last_ = pct;
        // Where the bar went in each press's window.
        for (Press& p : open_) {
            if (at_ms < p.o.down_ms || at_ms > p.o.down_ms + kJudgeMs) continue;
            if (!p.o.read) {
                p.low = p.high = pct;
                p.o.read = true;
            } else {
                p.low = std::min(p.low, pct);
                p.high = std::max(p.high, pct);
            }
        }
    }
    // The presses kJudgeMs old are judged; they went out in order.
    while (!open_.empty() && at_ms >= open_.front().o.down_ms + kJudgeMs) {
        close(open_.front());
        open_.pop_front();
    }
}

void PetHealWatch::close(Press& p) {
    Outcome& o = p.o;
    o.is_static = !o.rose && o.read && o.before >= 0.0f && p.high - o.before <= kStaticPct &&
                  o.before - p.low <= kStaticPct;
    closed_.push_back(o);
    ++judged_;
    if (o.rose) {
        ++rose_;
        rises_.push_back(o.rise);
        delays_.push_back(o.delay_ms);
        if (rose_down_ != 0 && o.down_ms > rose_down_) {
            const std::uint64_t gap = o.down_ms - rose_down_;
            gap_ms_ = gap_ms_ == 0 ? gap : std::min(gap_ms_, gap);
        }
        rose_down_ = o.down_ms;
        in_run_ = false;
    } else if (!o.read || o.before < 0.0f) {
        // Nothing to judge it by; a run of still presses goes on past it.
        ++unread_;
    } else if (o.is_static) {
        if (!in_run_ || runs_.empty()) runs_.push_back(Run{ round_pct(o.before), 0 });
        in_run_ = true;
        ++runs_.back().n;
    } else {
        ++moving_;
        in_run_ = false;
    }
}

std::vector<PetHealWatch::Outcome> PetHealWatch::take_closed() {
    std::vector<Outcome> out;
    out.swap(closed_);
    return out;
}

std::string PetHealWatch::minute_line(std::uint64_t now) {
    if (minute_ms_ == 0) {
        minute_ms_ = std::max<std::uint64_t>(now, 1);
        return {};
    }
    if (now < minute_ms_ + 60'000) return {};
    minute_ms_ = now;
    std::string line;
    if (pressed_ > 0 || judged_ > 0) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "Pet heals: key %s, %llu pressed; of %llu judged, %llu rose",
                      InputManager::key_name(keycode_).c_str(), static_cast<unsigned long long>(pressed_),
                      static_cast<unsigned long long>(judged_), static_cast<unsigned long long>(rose_));
        line = buf;
        if (rose_ > 0) {
            std::snprintf(buf, sizeof buf, " (median +%d%% after %llu ms", round_pct(median_of(rises_)),
                          static_cast<unsigned long long>(median_of(delays_)));
            line += buf;
            if (gap_ms_ != 0) {
                std::snprintf(buf, sizeof buf, "; two that rose %.1f s apart at the closest",
                              static_cast<double>(gap_ms_) / 1000.0);
                line += buf;
            }
            line += ")";
        }
        std::uint64_t still = 0;
        for (const Run& r : runs_) still += r.n;
        if (still > 0) {
            std::snprintf(buf, sizeof buf, ", %llu static -- the bar still -- in %zu run%s (",
                          static_cast<unsigned long long>(still), runs_.size(), runs_.size() == 1 ? "" : "s");
            line += buf;
            for (std::size_t i = 0; i < runs_.size() && i < kRunsSaid; ++i) {
                std::snprintf(buf, sizeof buf, "%s%zu at %d%%", i ? ", " : "", runs_[i].n, runs_[i].pct);
                line += buf;
            }
            if (runs_.size() > kRunsSaid) line += ", and " + std::to_string(runs_.size() - kRunsSaid) + " more";
            line += ")";
        }
        if (moving_ > 0)
            line += ", " + std::to_string(moving_) + " no rise with the bar moving";
        if (unread_ > 0) line += ", " + std::to_string(unread_) + " with no reading";
    }
    // The next minute from nothing: a run going on is counted afresh in it.
    pressed_ = judged_ = rose_ = unread_ = moving_ = 0;
    rises_.clear();
    delays_.clear();
    gap_ms_ = 0;
    runs_.clear();
    in_run_ = false;
    return line;
}

// ------------------------------------------------------------ the engine

bool MacroEngine::skill_ready(const MacroConfig& cfg, std::uint64_t now) const {
    // Paced, a skill waits for the game's cast and its lock, not for the keys.
    if (pace_mode_ == PaceStatus::Mode::Paced) return false;
    // Walking back to the hunting area (2026.11.10): no skill goes, and the
    // pet waits on none.
    const Fight fight = fight_now();
    if (fight.mode == Fight::Mode::Returning) return false;
    const bool follow = following(cfg, fight);
    // run_skills' keys: its look due, a fight, the gate open.
    const bool fight_open =
        follow && now >= skill_look_ms_ && skill_gate(cfg.skill_mp_floor_pct, fight) == SkillGate::Pass;
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < cfg.keys.size(); ++i) {
        const MacroKey& k = cfg.keys[i];
        if (k.conditional() || !macro_timer_on(k) || macro_key_kind(k) != MacroKeyKind::Skill) continue;
        // Nor one a hold refuses -- a bag job's key window lets only some keys
        // through, and only between its own clicks (2026.11.10).
        if (coord_.critical_active() && !coord_.lets_through(k.keycode)) continue;
        if (!follow || k.always_timer) {
            // The timers' own: skill_gap_ms after the last one came up.
            if (now < skill_free_ms_) continue;
        } else {
            if (!fight_open) continue;
            if (fight.attack_key != 0 && k.keycode == fight.attack_key) continue;
            if (k.range_px > 0 && fight.dx > k.range_px) continue;
        }
        const int s = slot(i);
        if (s >= 0 && static_cast<std::size_t>(s) < due_ms_.size() && now >= due_ms_[static_cast<std::size_t>(s)])
            return true;
    }
    return false;
}

bool MacroEngine::pet_yields(const MacroConfig& cfg, std::uint64_t now) {
    if (!line_pet_last_.load() || !skill_ready(cfg, now)) {
        pet_yield_since_ = 0;
        return false;
    }
    if (pet_yield_since_ == 0) pet_yield_since_ = std::max<std::uint64_t>(now, 1);
    // That skill not gone after all -- refused, held a moment longer -- the
    // pet does not wait on it for ever.
    return now < pet_yield_since_ + kPetYieldMostMs;
}

void MacroEngine::pet_outcomes(const MacroConfig& cfg, std::uint64_t now) {
    float hp, mp, pet;
    current_vitals(hp, mp, pet);
    pet_heals_.reading(now, pet);
    for (const PetHealWatch::Outcome& o : pet_heals_.take_closed()) {
        if (o.rose || !o.read || !o.is_static) {
            if (o.read) pet_static_said_ = false;
            continue;
        }
        // A press that did nothing, the bar still. At 70-75% -- where most of
        // them were on 2026-11-10 -- the first of a run has its slot's picture
        // kept, for whether the game refuses the food there or the bar is
        // misread.
        const int pct = static_cast<int>(std::lround(o.before));
        if (!pet_plateau_seen_ && pct >= kPlateauLowPct && pct <= kPlateauHighPct) {
            pet_plateau_seen_ = true;
            pet_plateau_pct_.store(pct);
            pet_plateau_.store(true);
        }
        // Pressed more often than the potion check comes, no press of the
        // pet's is checked (run_potions): the first of each still run is
        // said instead, and the minute's line counts them.
        std::uint32_t every = 0;
        for (const MacroKey& k : cfg.keys)
            if (k.keycode == o.keycode && macro_key_kind(k) == MacroKeyKind::PetPotion) {
                every = macro_potion_cooldown_ms(cfg, k);
                break;
            }
        if (every == 0 || every >= cfg.potion_recheck_ms || pet_static_said_) continue;
        pet_static_said_ = true;
        GB_INFO("Potion check: Pet key %s pressed at %d%%, the bar within %.0f%% of it for %llu ms -- no rise; pressed "
                "on every %u ms all the same (more often than the %u-ms check), the minute's \"Pet heals\" line counts "
                "the run",
                InputManager::key_name(o.keycode).c_str(), pct, static_cast<double>(PetHealWatch::kStaticPct),
                static_cast<unsigned long long>(PetHealWatch::kJudgeMs), every, cfg.potion_recheck_ms);
    }
    const std::string line = pet_heals_.minute_line(now);
    if (!line.empty()) GB_INFO("%s", line.c_str());
}

bool MacroEngine::take_pet_plateau(int& pct) {
    if (!pet_plateau_.exchange(false)) return false;
    pct = pet_plateau_pct_.load();
    return true;
}

} // namespace gobot
