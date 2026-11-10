// The cast-paced rotation (MacroConfig::cast_paced, 2026.11.08): the skill
// keys on their timers pressed one at a time, in the order of the keys, each
// once the game has cast the one before -- your own 0x0031, handed in by the
// capture thread (note_own_cast) -- and that cast's lock is over. See
// gobot/macro.hpp.
//
// Everything here but note_own_cast and pace_status runs on the timer pass's
// thread, or a test's step(): its state is that pass's alone, and arm()'s at a
// start. note_own_cast is the capture thread's and touches the ring's atomics
// only; the status is under pace_mutex_, which is never held while another
// lock is taken.

#include "gobot/macro.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <string>
#include <vector>

namespace gobot {
namespace {

using Mode = MacroEngine::PaceStatus::Mode;
using Turn = MacroEngine::PaceStatus::Turn;

/// A key's last kPaceSuspectTurns turns, a bit each, the newest lowest.
constexpr std::uint8_t kHistoryMask = static_cast<std::uint8_t>((1u << MacroEngine::kPaceSuspectTurns) - 1u);

/// The last kPaceDropWindow first presses at the guess, a bit each, the newest
/// lowest: 1 for one dropped whose re-press was cast.
constexpr unsigned kDropsMask = (1u << MacroEngine::kPaceDropWindow) - 1u;

std::uint8_t push_turn(std::uint8_t history, bool bad) {
    return static_cast<std::uint8_t>(((static_cast<unsigned>(history) << 1) | (bad ? 1u : 0u)) & kHistoryMask);
}

/// `ms` in seconds with one decimal: "2.0 s".
std::string pace_seconds(std::uint64_t ms) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f s", static_cast<double>(ms) / 1000.0);
    return buf;
}

/// The keys of the rotation, by name, in order: "Z X C V". A key on ↓, which
/// never goes out (InputManager::never_pressed), is none of it.
std::string rotation_text(const MacroConfig& cfg) {
    std::string out;
    for (const MacroKey& k : cfg.keys)
        if (!k.conditional() && macro_timer_on(k) && macro_key_kind(k) == MacroKeyKind::Skill &&
            !InputManager::never_pressed(k.keycode))
            out += (out.empty() ? "" : " ") + InputManager::key_name(k.keycode);
    return out.empty() ? std::string("no skill on") : out;
}

const char* mode_text(Mode m) {
    switch (m) {
        case Mode::Paced:    return "paced";
        case Mode::Fallback: return "on the timers";
        case Mode::Off:      break;
    }
    return "off";
}

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

} // namespace

// ------------------------------------------------------------ your casts

void MacroEngine::note_own_cast(std::uint32_t skill, std::uint64_t at_ms) {
    // The capture thread's alone, one writer: the count is its own to move.
    // The slot is marked as being written -- its version odd -- before any of
    // it changes, and the count moved only once it is whole, so the timer
    // pass never takes half of one, nor one written over meanwhile.
    const std::uint64_t n = casts_noted_.load(std::memory_order_relaxed);
    CastSlot& s = cast_ring_[n % kCastRing];
    const std::uint64_t v = s.version.load(std::memory_order_relaxed);
    s.version.store(v + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s.n.store(n, std::memory_order_relaxed);
    s.at_ms.store(at_ms, std::memory_order_relaxed);
    s.skill.store(skill, std::memory_order_relaxed);
    s.version.store(v + 2, std::memory_order_release);
    casts_noted_.store(n + 1, std::memory_order_release);
}

void MacroEngine::pace_take_casts(std::uint64_t now) {
    const std::uint64_t noted = casts_noted_.load(std::memory_order_acquire);
    std::uint64_t from = casts_read_;
    // More than the ring holds since the last look -- never, at a cast in
    // half a second and a look every 10 ms: the oldest are gone.
    if (noted - from > kCastRing) from = noted - kCastRing;
    for (std::uint64_t n = from; n < noted; ++n) {
        const CastSlot& s = cast_ring_[n % kCastRing];
        const std::uint64_t v = s.version.load(std::memory_order_acquire);
        if (v & 1u) continue;               // being written with a newer one
        const std::uint64_t which = s.n.load(std::memory_order_relaxed);
        const std::uint64_t at = s.at_ms.load(std::memory_order_relaxed);
        const std::uint32_t skill = s.skill.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s.version.load(std::memory_order_relaxed) != v || which != n) continue;
        pace_cast(at, skill, now);
    }
    casts_read_ = noted;
}

// ------------------------------------------------------------ the status

MacroEngine::PaceStatus MacroEngine::pace_status() const {
    std::lock_guard<std::mutex> lock(pace_mutex_);
    return pace_status_;
}

const char* MacroEngine::pace_turn_name(PaceStatus::Turn turn) {
    switch (turn) {
        case Turn::Cast:          return "cast";
        case Turn::CastOnRepress: return "cast on the re-press";
        case Turn::NoCast:        return "no cast";
        case Turn::Cooling:       return "cooling";
        case Turn::NotDue:        return "not due";
    }
    return "?";
}

std::string MacroEngine::pace_status_vi(const PaceStatus& s) {
    char buf[200];
    switch (s.mode) {
        case Mode::Paced:
            std::snprintf(buf, sizeof buf, "Đang theo vòng — khóa chiêu ước %u ms, %llu vòng.", s.lock_ms,
                          static_cast<unsigned long long>(s.rounds));
            return buf;
        case Mode::Fallback:
            if (s.last_cast_ms == 0) return "Tạm theo giờ: chưa thấy gói tin ra chiêu nào của bạn.";
            std::snprintf(buf, sizeof buf,
                          "Tạm theo giờ vì không thấy gói tin ra chiêu — khóa chiêu ước %u ms; thấy lại thì theo "
                          "vòng tiếp.",
                          s.lock_ms);
            return buf;
        case Mode::Off:
            break;
    }
    return s.others ? "Tự đánh quái hoặc chuỗi phím cũng đang bấm phím, nên không theo vòng: mỗi phím bấm theo giờ "
                      "như thường."
                    : "Kỹ năng đang theo trận đánh (Tự đánh quái), nên không theo vòng.";
}

void MacroEngine::pace_publish() {
    std::lock_guard<std::mutex> lock(pace_mutex_);
    pace_status_.mode = pace_mode_;
    pace_status_.since_ms = pace_mode_ms_;
    pace_status_.lock_ms = pace_lock_ms_;
    pace_status_.rounds = pace_rounds_;
    pace_status_.last_cast_ms = pace_last_cast_ms_;
    pace_status_.others = pace_others_;
}

void MacroEngine::pace_step(std::uint32_t keycode, PaceStatus::Turn turn) {
    // A round is at most every key once, but for a list changed during it.
    if (pace_round_.size() < 2 * MacroConfig::kMaxKeys) pace_round_.push_back(PaceStatus::Step{ keycode, turn });
}

void MacroEngine::pace_round_end() {
    ++pace_rounds_;
    ++pace_minute_.rounds;
    {
        std::lock_guard<std::mutex> lock(pace_mutex_);
        pace_status_.last_round = pace_round_;
        pace_status_.rounds = pace_rounds_;
    }
    pace_round_.clear();
}

// ------------------------------------------------------------ the modes

void MacroEngine::pace_arm(std::uint64_t now) {
    // Under mutex_, from arm(). A key taken out of the list is let go of; the
    // rest keep what was learned of them, the game's.
    std::erase_if(pace_keys_, [this](const PaceKey& k) {
        return std::find(key_ids_.begin(), key_ids_.end(), k.id) == key_ids_.end();
    });
    for (PaceKey& k : pace_keys_) {
        k.candidate_ms = 0;
        k.candidate_at = 0;
        k.seen_ms = 0;
        k.seen_at = 0;
        k.skipped = 0;
        k.skipped_learned = 0;
    }
    pace_press_ = PacePress{};
    pace_mode_ = Mode::Off;
    pace_mode_ms_ = now;
    pace_cursor_ = 0;
    pace_cursor_id_ = 0;
    pace_clean_ = 0;
    pace_drops_ = 0;
    pace_others_ = false;
    pace_prev_dropped_ = false;
    pace_prev_id_ = 0;
    pace_unanswered_ms_ = 0;
    pace_unanswered_n_ = 0;
    pace_retry_ms_ = 0;
    pace_buff_ms_ = 0;
    pace_rounds_ = 0;
    pace_round_.clear();
    pace_minute_ = PaceMinute{};
    pace_log_ms_ = 0;
    pace_fallback_from_ = now;
    // The casts that came while the macro was off: off, they are the lock's
    // start only.
    pace_take_casts(now);
    pace_publish();
    std::lock_guard<std::mutex> lock(pace_mutex_);
    pace_status_.last_round.clear();
}

void MacroEngine::pace_set_mode(PaceStatus::Mode mode, std::uint64_t now) {
    // The time on the timers, for the minute's line.
    if (pace_mode_ == Mode::Fallback && mode != Mode::Fallback)
        pace_minute_.fallback_ms += now > pace_fallback_from_ ? now - pace_fallback_from_ : 0;
    if (mode == Mode::Fallback && pace_mode_ != Mode::Fallback) pace_fallback_from_ = now;
    // A round broken off is no round.
    if (mode != pace_mode_) pace_round_.clear();
    pace_mode_ = mode;
    pace_mode_ms_ = now;
    pace_press_ = PacePress{};
    pace_unanswered_ms_ = 0;
    pace_unanswered_n_ = 0;
    pace_publish();
}

bool MacroEngine::pace_pass(const MacroConfig& cfg, bool follow, bool others, std::uint64_t now) {
    // On, but off for the hunter or the sequence pressing keys too: said under
    // the tick, the mode Off all along or not.
    const bool off_others = cfg.cast_paced && !follow && others;
    if (off_others != pace_others_) {
        pace_others_ = off_others;
        pace_publish();
    }
    if (!cfg.cast_paced || follow || others) {
        if (pace_mode_ != Mode::Off) {
            pace_set_mode(Mode::Off, now);
            GB_INFO("Paced: off -- %s",
                    !cfg.cast_paced ? "switched off; every skill on its own timer again"
                    : follow        ? "the skills follow the fight now, and go with it"
                                    : "the hunter or the key sequence presses keys too, and their casts cannot be "
                                      "told from the rotation's; every skill on its own timer");
        }
        // Your casts meanwhile: the lock's start, should it come on again.
        pace_take_casts(now);
        return false;
    }
    if (pace_mode_ == Mode::Off) {
        // On. Your casts have been seen -- the packets come -- and the round
        // starts at once; none yet, the timers until the first.
        const std::string order = rotation_text(cfg);
        if (casts_noted_.load(std::memory_order_acquire) > 0) {
            pace_set_mode(Mode::Paced, now);
            GB_INFO("Paced: on -- the skills one after another in the order of the keys (%s), each once the game "
                    "has cast the one before; lock %u ms",
                    order.c_str(), pace_lock_ms_);
        } else {
            pace_set_mode(Mode::Fallback, now);
            GB_INFO("Paced: on, but no cast of yours in the packets yet -- the skills (%s) on their own timers "
                    "until the first",
                    order.c_str());
        }
    }
    pace_take_casts(now);
    return pace_mode_ == Mode::Paced;
}

// ------------------------------------------------------------ a cast

MacroEngine::PaceKey& MacroEngine::pace_key(std::uint64_t id) {
    for (PaceKey& k : pace_keys_)
        if (k.id == id) return k;
    PaceKey k;
    k.id = id;
    pace_keys_.push_back(k);
    return pace_keys_.back();
}

void MacroEngine::pace_cast(std::uint64_t at_ms, std::uint32_t skill, std::uint64_t now) {
    // Whoever's key it was, the character is in its lock from here.
    if (at_ms > pace_last_cast_ms_) pace_last_cast_ms_ = at_ms;
    if (pace_mode_ == Mode::Off) return;
    // Your casts come: keys going out with none are counted afresh.
    pace_unanswered_ms_ = 0;
    pace_unanswered_n_ = 0;
    // A buff's own: the next skill goes its lock after it.
    if (pace_buff_ms_ != 0 && at_ms + kPaceEarlyMs >= pace_buff_ms_) pace_buff_ms_ = 0;

    if (pace_mode_ == Mode::Fallback) {
        const std::uint64_t was = pace_mode_ms_;
        pace_set_mode(Mode::Paced, now);
        GB_INFO("Paced: your cast came after %s on the timers -- the skills one after another again",
                pace_seconds(now > was ? now - was : 0).c_str());
        return;
    }

    // None waited for, or one from before the key went down: someone else's
    // key -- a buff's, the hunter's, a hand's -- or one come too late.
    if (!pace_press_.active || at_ms + kPaceEarlyMs < pace_press_.down_ms) {
        ++pace_minute_.other;
        pace_publish();
        return;
    }

    PaceKey& pk = pace_key(pace_press_.id);
    // Stamped after the re-press went down: the first was dropped. Two
    // presses at least 160 ms apart, and a cast 27-87 ms after its key: no
    // cast can be taken for the other's.
    const bool again =
        !pace_press_.check && pace_press_.again_ms != 0 && at_ms + kPaceEarlyMs >= pace_press_.again_ms;
    if (again) {
        ++pace_minute_.on_repress;
        pace_clean_ = 0;
        pace_drops_ = static_cast<std::uint16_t>(((static_cast<unsigned>(pace_drops_) << 1) | 1u) & kDropsMask);
        // The lock was longer than guessed -- once this is not the only drop in
        // the last kPaceDropWindow (one alone is a stun, a hitch: not the
        // lock), and unless this key dropped a press or was skipped in its
        // last turns: its cooldown just over, the turn after its own, more
        // likely. Two keys in a row is the lock.
        const bool suspect = (pk.history & kHistoryMask) != 0 && !(pace_prev_dropped_ && pace_prev_id_ != pk.id);
        const bool repeated = std::popcount(pace_drops_) >= kPaceDropsToRaise;
        if (!suspect && repeated) pace_lock_ms_ = std::min(kPaceLockMaxMs, pace_lock_ms_ + kPaceLockUpMs);
    } else if (pace_press_.at_lock) {
        pace_drops_ = static_cast<std::uint16_t>((static_cast<unsigned>(pace_drops_) << 1) & kDropsMask);
        // Enough first presses cast at the guess, no re-press cast between --
        // a skip or a late one neither counts nor breaks it: try a little
        // sooner.
        if (++pace_clean_ >= kPaceLockDownAfter) {
            pace_lock_ms_ = std::max(kPaceLockMinMs, pace_lock_ms_ - kPaceLockDownMs);
            pace_clean_ = 0;
        }
    }
    pk.history = push_turn(pk.history, again);
    // Cast on its first press: the lock before it was as guessed.
    if (!again) pk.late = false;
    pace_prev_dropped_ = again;
    pace_prev_id_ = pk.id;

    // The game was casting: the keys skipped just before were on cooldowns of
    // their own, at least as long as they had waited -- taken on a key's
    // second such skip within kPaceSeenMs: one stop of the game's (a stun, a
    // hitch, the capture late) skips every key in it, cooldown or none. A key
    // with a floor raises it at once.
    for (PaceKey& o : pace_keys_) {
        if (o.id != pk.id && o.candidate_at != 0 && at_ms <= o.candidate_at + kPaceFloorTakenMs) {
            if (o.floor_ms == 0 && (o.seen_ms == 0 || at_ms > o.seen_at + kPaceSeenMs)) {
                o.seen_ms = o.candidate_ms;
                o.seen_at = at_ms;
            } else {
                o.floor_ms = std::max(o.floor_ms, o.floor_ms == 0 ? std::min(o.seen_ms, o.candidate_ms) : o.candidate_ms);
                o.floor_uses = 0;
                o.seen_ms = 0;
            }
        }
        o.candidate_ms = 0;
        o.candidate_at = 0;
    }
    // Cast sooner than its floor said it could be: the floor was wrong.
    if (pk.floor_ms != 0 && pk.last_cast_ms != 0 && at_ms < pk.last_cast_ms + pk.floor_ms) {
        pk.floor_ms = 0;
        pk.floor_uses = 0;
    }
    // ... or than its sighting said: no cooldown that long. A real one is
    // never cast sooner than the wait it was skipped after.
    if (pk.seen_ms != 0 && pk.last_cast_ms != 0 && at_ms < pk.last_cast_ms + pk.seen_ms) pk.seen_ms = 0;
    pk.last_cast_ms = at_ms;
    pk.skill = skill;
    ++pace_minute_.confirmed;
    pace_step(pace_press_.keycode, again ? Turn::CastOnRepress : Turn::Cast);
    pace_advance();
    pace_press_ = PacePress{};
    pace_publish();
}

void MacroEngine::pace_advance() {
    pace_cursor_ = pace_press_.pos + 1;
    pace_cursor_id_ = 0;
    if (pace_cursor_ >= pace_press_.n) {
        pace_cursor_ = 0;
        pace_round_end();
    }
}

void MacroEngine::pace_skip() {
    PaceKey& pk = pace_key(pace_press_.id);
    ++pk.skipped;
    pk.history = push_turn(pk.history, true);
    // Its next re-press waits out the longest lock, until a first press of it
    // is cast: the cast before it may lock longer than the guess.
    pk.late = true;
    // Its cooldown is longer than it had waited at its last key-down: taken
    // as its least once another key casts -- the game casting all along.
    const std::uint64_t last = pace_press_.check ? pace_press_.down_ms : pace_press_.again_ms;
    if (pk.last_cast_ms != 0 && last > pk.last_cast_ms) {
        pk.candidate_ms = last - pk.last_cast_ms;
        pk.candidate_at = last;
    }
    pace_step(pace_press_.keycode, Turn::NoCast);
    pace_advance();
    pace_press_ = PacePress{};
    pace_publish();
}

// ------------------------------------------------------------ the presses

void MacroEngine::pace_refused(Refused why, std::uint64_t now) {
    pace_retry_ms_ = now + kGatePollMs;
    // No key out is no key going out unanswered.
    pace_unanswered_ms_ = 0;
    pace_unanswered_n_ = 0;
    // For the Skills line, as on the timers: GoBot's own window clicked.
    if (why == Refused::Unfocused) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.skills_unfocused;
    }
}

void MacroEngine::pace_went(const MacroConfig& cfg, std::size_t i, std::uint64_t at) {
    const MacroKey& k = cfg.keys[i];
    skill_free_ms_ = clock_now() + cfg.skill_gap_ms;
    if (pace_unanswered_ms_ == 0) pace_unanswered_ms_ = at;
    ++pace_unanswered_n_;
    // Its timer armed from this press, as the timers arm it: fallen back,
    // they go on from here. Worked out before the lock: jittered() takes it.
    const std::uint32_t next = jittered(macro_timer_interval_ms(k), k.jitter_pct);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int s = slot(i);
        if (s >= 0 && static_cast<std::size_t>(s) < due_ms_.size()) due_ms_[static_cast<std::size_t>(s)] = at + next;
        if (s >= 0 && static_cast<std::size_t>(s) < pressed_ms_.size()) pressed_ms_[static_cast<std::size_t>(s)] = at;
    }
    count_press(i);
}

void MacroEngine::run_paced(const MacroConfig& cfg, std::uint64_t now) {
    if (pace_mode_ != Mode::Paced) return;

    // The rotation: the skill keys on, in the order of the keys, each by
    // which key it is -- the keys may change between passes.
    struct Place {
        std::size_t   i;
        std::uint64_t id;
    };
    std::vector<Place> rot;
    for (std::size_t i = 0; i < cfg.keys.size(); ++i) {
        const MacroKey& k = cfg.keys[i];
        if (k.conditional() || !macro_timer_on(k) || macro_key_kind(k) != MacroKeyKind::Skill) continue;
        // ↓ never goes out (InputManager::never_pressed): no turn of its own.
        if (InputManager::never_pressed(k.keycode)) continue;
        rot.push_back(Place{ i, i < pass_ids_.size() ? pass_ids_[i] : 0 });
    }
    // Each one's own, made before any is held: none moves after this.
    for (const Place& p : rot) pace_key(p.id).keycode = cfg.keys[p.i].keycode;
    const std::size_t n = rot.size();
    const auto place_of = [&rot, n](std::uint64_t id) {
        for (std::size_t at = 0; at < n; ++at)
            if (rot[at].id == id) return at;
        return n;
    };

    // Keys going out and no cast of yours for kPaceStaleMs: none is coming --
    // out of the world, in a town, the capture stopped. The timers, until
    // the next. On your own frames, never the capture's gaps.
    if (pace_unanswered_ms_ != 0 && now >= pace_unanswered_ms_ + kPaceStaleMs) {
        const std::uint64_t quiet = now - pace_unanswered_ms_;
        const std::uint64_t went = pace_unanswered_n_;
        pace_set_mode(Mode::Fallback, now);
        GB_INFO("Paced: no cast of yours for %s while %llu key(s) went out -- the skills on their own timers "
                "until your next cast",
                pace_seconds(quiet).c_str(), static_cast<unsigned long long>(went));
        return;
    }

    // The press waiting for its cast: pressed once more, or given up on.
    if (pace_press_.active) {
        const std::size_t at = place_of(pace_press_.id);
        if (at == n) {
            pace_press_ = PacePress{};      // its key taken out meanwhile
        } else if (pace_press_.check || pace_press_.again_ms != 0) {
            const std::uint64_t last = pace_press_.check ? pace_press_.down_ms : pace_press_.again_ms;
            if (now < last + kPaceConfirmMs) return;
            // No cast from either: skipped this round -- and the next key at
            // once, the lock long over.
            pace_press_.pos = at;
            pace_press_.n = n;
            pace_skip();
        } else {
            // None yet: the same key again, once the gap after its key-up is
            // over and a cast would have shown.
            std::uint64_t again =
                std::max<std::uint64_t>(pace_press_.up_ms + cfg.skill_gap_ms, pace_press_.down_ms + kPaceRepressMs);
            // Skipped on an earlier turn: the cast before it may lock longer
            // than the guess, so the re-press waits out the longest lock.
            if (pace_key(pace_press_.id).late)
                again = std::max<std::uint64_t>(again, pace_last_cast_ms_ + kPaceLockMaxMs + kPaceRepressMs);
            if (now < again || now < pace_retry_ms_) return;
            if (potions_first(cfg, clock_now())) return;
            const MacroKey& k = cfg.keys[rot[at].i];
            const std::uint64_t down = clock_now();
            Refused why = Refused::None;
            if (press(k.keycode, k.hold_ms, "macro-paced", &why)) {
                pace_went(cfg, rot[at].i, down);
                pace_press_.again_ms = down;
                pace_press_.pos = at;
                pace_press_.n = n;
                ++pace_minute_.repressed;
            } else if (why == Refused::Failed) {
                // The re-press never went out (Windows not taking it): the
                // first press's own wait ends the turn, a skip -- never a
                // re-press tried for good, holding every other key up.
                pace_press_.check = true;
            } else {
                pace_refused(why, now);
            }
            return;
        }
    }
    if (n == 0) return;

    // The next key: once your last cast's lock is over, the gap after the
    // last key's key-up, a refusal's wait, and a buff's cast and the rest of
    // its presses.
    if (now < pace_last_cast_ms_ + pace_lock_ms_ || clock_now() < skill_free_ms_ || now < pace_retry_ms_) return;
    if (pace_buff_ms_ != 0) {
        if (now < pace_buff_ms_ + kPaceConfirmMs) return;
        pace_buff_ms_ = 0;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!buff_repeats_.empty()) return;
    }
    // A potion fallen due meanwhile goes first; the round waits where it is.
    if (potions_first(cfg, clock_now())) return;

    // From the cursor, the first key whose interval is over since its last
    // cast and that is not inside the cooldown learned for it. Those passed
    // over have had their turn this round.
    std::size_t cursor = pace_cursor_id_ != 0 ? place_of(pace_cursor_id_) : n;
    if (cursor == n) cursor = pace_cursor_ < n ? pace_cursor_ : 0;
    struct Passed {
        std::size_t at;
        Turn        turn;
    };
    std::vector<Passed> passed;
    std::size_t wrap_at = kNone;            // where in `passed` the round ended
    std::size_t found = n;
    for (std::size_t step = 0; step < n; ++step) {
        std::size_t at = cursor + step;
        if (at >= n) {
            at -= n;
            if (wrap_at == kNone) wrap_at = passed.size();
        }
        const MacroKey& k = cfg.keys[rot[at].i];
        const PaceKey& pk = pace_key(rot[at].id);
        if (pk.last_cast_ms != 0 && now < pk.last_cast_ms + macro_timer_interval_ms(k)) {
            passed.push_back(Passed{ at, Turn::NotDue });
            continue;
        }
        if (pk.floor_ms != 0 && pk.last_cast_ms != 0 && now < pk.last_cast_ms + pk.floor_ms &&
            pk.floor_uses < kPaceFloorUses) {
            passed.push_back(Passed{ at, Turn::Cooling });
            continue;
        }
        found = at;
        break;
    }
    // None due: nothing pressed, and the round goes on from where it was.
    if (found == n) return;
    for (std::size_t p = 0; p < passed.size(); ++p) {
        if (p == wrap_at) pace_round_end();
        PaceKey& pk = pace_key(rot[passed[p].at].id);
        if (passed[p].turn == Turn::Cooling) {
            ++pk.floor_uses;
            ++pk.skipped;
            ++pk.skipped_learned;
            pk.history = push_turn(pk.history, true);
        }
        pace_step(pk.keycode, passed[p].turn);
    }
    if (wrap_at == passed.size()) pace_round_end();
    pace_cursor_ = found;
    pace_cursor_id_ = rot[found].id;

    PaceKey& pk = pace_key(rot[found].id);
    const MacroKey& k = cfg.keys[rot[found].i];
    // Still under its floor, passed over kPaceFloorUses times: pressed once,
    // a check of it.
    const bool check = pk.floor_ms != 0 && pk.last_cast_ms != 0 && now < pk.last_cast_ms + pk.floor_ms;
    const std::uint64_t down = clock_now();
    const bool at_lock = down <= pace_last_cast_ms_ + pace_lock_ms_ + kPaceAtLockMs;
    Refused why = Refused::None;
    if (!press(k.keycode, k.hold_ms, "macro-paced", &why)) {
        if (why != Refused::Failed) {
            pace_refused(why, now);
            return;
        }
        // The key never goes out -- Windows not taking it: its turn passes with
        // no cast, and the round goes on to the next key. Waited for in place,
        // it would hold every other key up for good. Nothing went out, so
        // nothing is learned of it, and the stale window is left as it is: the
        // timers still take over when the keys that do go out are not answered.
        // kGatePollMs on, so a key failing every time is no 10-ms spin.
        pace_retry_ms_ = now + kGatePollMs;
        pace_press_ = PacePress{};
        pace_press_.id = rot[found].id;
        pace_press_.keycode = k.keycode;
        pace_press_.pos = found;
        pace_press_.n = n;
        pace_step(k.keycode, Turn::NoCast);
        pace_advance();
        pace_press_ = PacePress{};
        pace_publish();
        return;
    }
    const std::uint64_t up = clock_now();
    if (check) pk.floor_uses = 0;
    pace_went(cfg, rot[found].i, down);
    PacePress p;
    p.active = true;
    p.id = rot[found].id;
    p.keycode = k.keycode;
    p.pos = found;
    p.n = n;
    p.down_ms = down;
    p.up_ms = up;
    p.at_lock = at_lock;
    p.check = check;
    pace_press_ = p;
}

// ------------------------------------------------------------ the log

void MacroEngine::log_paced(std::uint64_t now) {
    if (pace_log_ms_ == 0) {
        pace_log_ms_ = std::max<std::uint64_t>(now, 1);
        if (pace_mode_ == Mode::Fallback) pace_fallback_from_ = now;
        return;
    }
    if (now < pace_log_ms_ + 60'000) return;
    pace_log_ms_ = now;
    if (pace_mode_ == Mode::Fallback) {
        pace_minute_.fallback_ms += now > pace_fallback_from_ ? now - pace_fallback_from_ : 0;
        pace_fallback_from_ = now;
    }
    const PaceMinute m = pace_minute_;
    pace_minute_ = PaceMinute{};

    // Each key skipped -- no cast from its presses, or passed over inside its
    // cooldown as learned -- and the cooldown's least as learned.
    std::string skipped;
    for (PaceKey& k : pace_keys_) {
        if (k.skipped == 0) continue;
        std::string part = InputManager::key_name(k.keycode) + " " + std::to_string(k.skipped);
        if (k.skipped_learned != 0 || k.floor_ms != 0) {
            part += " (" + std::to_string(k.skipped_learned) + " learned";
            if (k.floor_ms != 0) part += ", floor " + pace_seconds(k.floor_ms);
            part += ")";
        }
        skipped += (skipped.empty() ? "" : ", ") + part;
        k.skipped = 0;
        k.skipped_learned = 0;
    }
    if (pace_mode_ == Mode::Off && m.confirmed == 0 && m.repressed == 0 && m.other == 0 && m.fallback_ms == 0 &&
        skipped.empty())
        return;
    GB_INFO("Paced: %llu rounds; %llu casts confirmed (%llu on a re-press); %llu re-presses; skipped (cooldown): "
            "%s; fallback %s; lock_est %u ms; other casts %llu; now %s",
            static_cast<unsigned long long>(m.rounds), static_cast<unsigned long long>(m.confirmed),
            static_cast<unsigned long long>(m.on_repress), static_cast<unsigned long long>(m.repressed),
            skipped.empty() ? "none" : skipped.c_str(), pace_seconds(m.fallback_ms).c_str(), pace_lock_ms_,
            static_cast<unsigned long long>(m.other), mode_text(pace_mode_));
}

} // namespace gobot
