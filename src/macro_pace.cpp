// The cast-paced rotation (MacroConfig::cast_paced, 2026.11.08): the skill
// keys on their timers pressed one at a time, in the order of the keys, each
// once the game has cast the one before -- your own 0x0031, handed in by the
// capture thread (note_own_cast) -- and that cast's lock is over. See
// gobot/macro.hpp.
//
// Since 2026.11.16b it runs while the hunter runs too, the skills following
// the fight or not, and on by default (the owner's "Ra chiêu theo vòng cả khi
// đánh quái"): a hold of the hunter's or of the fight's keeps the rotation at
// its key, a cast of someone else's -- the hunter's swing -- gives the press
// waiting back instead of answering it, and the hunter leaves its attack key
// to the rotation when that key is one of the rotation's (swing_yields).
//
// 2026.11.17, after the owner's "skill ra không đồng đều" while hunting: a
// key's cooldown is learned only from a refusal that can be one (pace_skip)
// and never past the shortest gap its skill was cast again after; a cast of
// a key's skill by hand stamps the key (pace_stamp); a skill another key is
// known to cast is never learned or counted against this one (pace_ours);
// the game taking no key while the packets come is probed a key a second,
// not handed to the timers; the character down holds every skill; a buff's
// cast seen ends its other presses (pace_buff_cast).
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

/// Whether `k` is a key of the rotation: a skill on its timer. A key on ↓,
/// which never goes out (InputManager::never_pressed), is none of it; nor,
/// while the skills follow the fight (`follow`), one on always_timer -- it
/// keeps its own timer, fight or none, as a buff does (2026.11.16b).
bool in_rotation(const MacroKey& k, bool follow) {
    return !k.conditional() && macro_timer_on(k) && macro_key_kind(k) == MacroKeyKind::Skill &&
           !InputManager::never_pressed(k.keycode) && !(follow && k.always_timer);
}

/// The keys of the rotation, by name, in order: "Z X C V".
std::string rotation_text(const MacroConfig& cfg, bool follow) {
    std::string out;
    for (const MacroKey& k : cfg.keys)
        if (in_rotation(k, follow)) out += (out.empty() ? "" : " ") + InputManager::key_name(k.keycode);
    return out.empty() ? std::string("no skill on") : out;
}

const char* mode_text(Mode m, bool hunting) {
    // Paced beside the hunter (2026.11.16b), said so.
    if (m == Mode::Paced && hunting) return "paced with the hunter";
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
        case Turn::Waiting:       return "waiting";
    }
    return "?";
}

std::string MacroEngine::pace_status_vi(const PaceStatus& s) {
    char buf[200];
    switch (s.mode) {
        case Mode::Paced:
            std::snprintf(buf, sizeof buf,
                          s.hunting ? "Đang theo vòng cùng Tự đánh quái — khóa chiêu ước %u ms, %llu vòng."
                                    : "Đang theo vòng — khóa chiêu ước %u ms, %llu vòng.",
                          s.lock_ms, static_cast<unsigned long long>(s.rounds));
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
    // Off with the switch on: only the key sequence keeps it so since
    // 2026.11.16b -- or the moment before the first pass.
    return s.others ? "Chuỗi phím (Sequence) đang bật, nên không theo vòng: mỗi phím bấm theo giờ như thường."
                    : "Chưa theo vòng: mỗi phím bấm theo giờ riêng.";
}

void MacroEngine::pace_publish() {
    std::lock_guard<std::mutex> lock(pace_mutex_);
    pace_status_.mode = pace_mode_;
    pace_status_.since_ms = pace_mode_ms_;
    pace_status_.lock_ms = pace_lock_ms_;
    pace_status_.rounds = pace_rounds_;
    pace_status_.last_cast_ms = pace_last_cast_ms_;
    pace_status_.others = pace_others_;
    pace_status_.hunting = pace_hunting_;
    pace_status_.probing = pace_probe_;
    pace_status_.floors.clear();
    for (const PaceKey& k : pace_keys_)
        pace_status_.floors.push_back(PaceStatus::Floor{ k.keycode, k.floor_ms, k.min_recast_ms, k.known_skill });
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
    // rest keep the skill learned of them. Their cooldowns, last casts and
    // shortest re-casts are learned afresh (2026.11.17): a floor kept over a
    // stop -- C's 33 s from 23:51 to 00:12 on 2026-11-17, across three
    // restarts -- and a last cast minutes old, from which the first refusal
    // after the start was measured, as a cooldown of that long. A real
    // cooldown is learned again within a minute.
    std::erase_if(pace_keys_, [this](const PaceKey& k) {
        return std::find(key_ids_.begin(), key_ids_.end(), k.id) == key_ids_.end();
    });
    for (PaceKey& k : pace_keys_) {
        k.last_cast_ms = 0;
        k.floor_ms = 0;
        k.floor_uses = 0;
        k.check_after = kPaceFloorCheckTurns;
        k.min_recast_ms = 0;
        k.waited = false;
        k.candidate_ms = 0;
        k.candidate_at = 0;
        k.seen_ms = 0;
        k.seen_at = 0;
        k.skipped = 0;
        k.skipped_learned = 0;
        k.minute_pressed = 0;
        k.minute_cast = 0;
        k.mismatch = 0;
    }
    pace_press_ = PacePress{};
    pace_mode_ = Mode::Off;
    pace_mode_ms_ = now;
    pace_cursor_ = 0;
    pace_cursor_id_ = 0;
    pace_clean_ = 0;
    pace_drops_ = 0;
    pace_others_ = false;
    pace_hunting_ = false;
    pace_swing_ms_ = 0;
    pace_attack_vk_ = 0;
    pace_given_back_ = 0;
    pace_given_back_id_ = 0;
    pace_held_look_ms_ = 0;
    swing_yield_vk_.store(0);
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
    pace_probe_ = false;
    pace_probe_ms_ = 0;
    pace_probe_from_ = 0;
    pace_free_ms_ = now;
    pace_was_held_ = false;
    pace_burst_vk_ = 0;
    pace_burst_ms_ = 0;
    skill_down_since_ = 0;
    skills_down_now_ = false;
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
    // Free again after the timers or off: a refusal in the next moments
    // teaches no cooldown (2026.11.17). A probe ends with any change.
    if (mode != pace_mode_) pace_free_ms_ = now;
    if (pace_probe_) {
        pace_minute_.probe_ms += now > pace_probe_from_ ? now - pace_probe_from_ : 0;
        pace_probe_ = false;
    }
    pace_mode_ = mode;
    pace_mode_ms_ = now;
    pace_press_ = PacePress{};
    pace_unanswered_ms_ = 0;
    pace_unanswered_n_ = 0;
    pace_given_back_ = 0;
    // Not pacing: the hunter swings its own key again.
    if (mode != Mode::Paced) swing_yield_vk_.store(0);
    pace_publish();
}

bool MacroEngine::pace_pass(const MacroConfig& cfg, const Fight& fight, bool sequence, std::uint64_t now) {
    const bool hunting = fight.mode != Fight::Mode::NoHunter;
    const bool follow = following(cfg, fight);
    // The hunter's swing as this pass has it: the casts taken below are told
    // from it (pace_ours).
    pace_swing_ms_ = fight.swing_ms;
    pace_attack_vk_ = fight.attack_key;
    // On, but off for the sequence pressing keys too: said under the tick,
    // the mode Off all along or not. And whether the hunter runs beside it.
    const bool off_sequence = cfg.cast_paced && sequence;
    if (off_sequence != pace_others_ || hunting != pace_hunting_) {
        if (hunting != pace_hunting_ && pace_mode_ != Mode::Off)
            GB_INFO("Paced: %s", hunting ? "the hunter runs -- the rotation goes on beside it, under its holds"
                                         : "the hunter stopped -- the rotation goes on alone");
        pace_others_ = off_sequence;
        pace_hunting_ = hunting;
        pace_publish();
    }
    if (!cfg.cast_paced || sequence) {
        swing_yield_vk_.store(0);
        if (pace_mode_ != Mode::Off) {
            pace_set_mode(Mode::Off, now);
            GB_INFO("Paced: off -- %s",
                    !cfg.cast_paced ? "switched off; every skill on its own timer again"
                                    : "the key sequence presses keys too, and its casts cannot be told from the "
                                      "rotation's; every skill on its own timer");
        }
        // Your casts meanwhile: the lock's start, should it come on again.
        pace_take_casts(now);
        return false;
    }
    if (pace_mode_ == Mode::Off) {
        // On. Your casts have been seen -- the packets come -- and the round
        // starts at once; none yet, the timers until the first.
        const std::string order = rotation_text(cfg, follow);
        const char* const with = hunting ? ", with the hunter" : "";
        if (casts_noted_.load(std::memory_order_acquire) > 0) {
            pace_set_mode(Mode::Paced, now);
            if (hunting)
                GB_INFO("Paced: on, with the hunter -- the skills one after another in the order of the keys (%s), "
                        "each once the game has cast the one before, under the hunter's holds; lock %u ms",
                        order.c_str(), pace_lock_ms_);
            else
                GB_INFO("Paced: on -- the skills one after another in the order of the keys (%s), each once the game "
                        "has cast the one before; lock %u ms",
                        order.c_str(), pace_lock_ms_);
        } else {
            pace_set_mode(Mode::Fallback, now);
            GB_INFO("Paced: on%s, but no cast of yours in the packets yet -- the skills (%s) on their own timers "
                    "until the first",
                    with, order.c_str());
        }
    }
    pace_take_casts(now);
    if (pace_mode_ != Mode::Paced) swing_yield_vk_.store(0);
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
    // A buff's burst under way, its cast this: the rest of its presses are
    // not needed (2026.11.17).
    pace_buff_cast(at_ms, skill);
    // The game takes keys again: the round as before, from a moment on.
    if (pace_probe_) {
        const std::uint64_t was = pace_probe_from_;
        pace_minute_.probe_ms += now > was ? now - was : 0;
        pace_probe_ = false;
        pace_free_ms_ = now;
        GB_INFO("Paced: your cast came after %s of the game taking no key -- the skills one after another again",
                pace_seconds(now > was ? now - was : 0).c_str());
    }

    if (pace_mode_ == Mode::Fallback) {
        // Its key's cooldown started in the game, whoever pressed it.
        pace_stamp(at_ms, skill, 0);
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
        // Its key's cooldown started in the game all the same (2026.11.17):
        // a cast of Z's skill by hand, 3 s after Z's last of the rotation's,
        // made Z's next press refused, and that refusal was measured from
        // the rotation's own cast 24 s before -- learned as Z's cooldown.
        pace_stamp(at_ms, skill, 0);
        // One from before the key went down, come only now, whose lock the key
        // went down in (2026.11.16b: the hunter's swing a moment ahead of the
        // key, a buff's cast late): the key's press was dropped in it. Given
        // back, and pressed once that lock is over -- re-pressed inside it, it
        // was dropped again and the key skipped, a cooldown learned it has not.
        if (pace_press_.active && at_ms + pace_lock_ms_ > pace_press_.down_ms) pace_give_back();
        pace_publish();
        return;
    }

    PaceKey& pk = pace_key(pace_press_.id);
    // Not the key's own (2026.11.16b): a skill it is not known to cast, or,
    // its skill not known yet, one that came with the hunter's swing. The
    // character is in that cast's lock now, and the key's press was dropped in
    // it or is still to come: given back -- no try, nothing learned of the key
    // -- and pressed again once that lock is over. Taken for the key's, the
    // swing's cast passed the key's turn with the key never cast.
    bool clean = true;
    if (!pace_ours(pk, skill, at_ms, clean)) {
        ++pace_minute_.other;
        pace_stamp(at_ms, skill, 0);
        // Another skill again and again with no swing near -- and none that
        // another key is known to cast (pace_ours: never clean then) -- the
        // player put another skill on the key. Learned afresh.
        if (clean && pk.known_skill != 0 && ++pk.mismatch >= kPaceSkillUnlearn) {
            GB_INFO("Paced: key %s cast skill %u %d times in a row, not the %u learned for it -- learned afresh",
                    InputManager::key_name(pk.keycode).c_str(), skill, pk.mismatch, pk.known_skill);
            pk.known_skill = 0;
            pk.run = 0;
            pk.mismatch = 0;
        }
        pace_give_back();
        pace_publish();
        return;
    }
    pk.mismatch = 0;
    // One with no swing near teaches the key's skill: kPaceSkillLearn in a
    // row of one skill, and that skill is the key's.
    if (clean) {
        if (skill == pk.run_skill) {
            ++pk.run;
        } else {
            pk.run_skill = skill;
            pk.run = 1;
        }
        if (pk.run >= kPaceSkillLearn) pk.known_skill = pk.run_skill;
    }
    ++pk.minute_cast;
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
    // with a floor raises it -- by half and a lock at most since 2026.11.17,
    // never past its cap (pace_floor_cap): one refusal took Z's floor from
    // 2.9 s to 23.8 s at once on 2026-11-17, and X's and D's to 70 s.
    for (PaceKey& o : pace_keys_) {
        if (o.id != pk.id && o.candidate_at != 0 && at_ms <= o.candidate_at + kPaceFloorTakenMs) {
            if (o.floor_ms == 0 && (o.seen_ms == 0 || at_ms > o.seen_at + kPaceSeenMs)) {
                o.seen_ms = o.candidate_ms;
                o.seen_at = at_ms;
            } else {
                const std::uint64_t step = o.floor_ms * 3 / 2 + pace_lock_ms_;
                std::uint64_t want = o.floor_ms == 0 ? std::min(o.seen_ms, o.candidate_ms)
                                                     : std::max(o.floor_ms, std::min(o.candidate_ms, step));
                const std::uint64_t cap = pace_floor_cap(o);
                if (want > cap) {
                    want = cap;
                    ++pace_minute_.capped;
                }
                // The checks start afresh only for a floor that moved: one
                // that stood keeps its doubled gap (the 2026.11.17 review).
                if (want != o.floor_ms) {
                    o.floor_uses = 0;
                    o.check_after = kPaceFloorCheckTurns;
                }
                o.floor_ms = want;
                o.seen_ms = 0;
            }
        }
        o.candidate_ms = 0;
        o.candidate_at = 0;
    }
    // Cast sooner than its floor said it could be: the floor was wrong ...
    // or than its sighting said: no cooldown that long. A real one is never
    // cast sooner than the wait it was skipped after. And its shortest
    // re-cast (pace_recast). A cast taken for the key's is of its known skill
    // (pace_ours), or the one it is learning.
    pace_recast(pk, at_ms);
    // Any other key known to cast the same skill: its cooldown began too.
    pace_stamp(at_ms, skill, pk.id);
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
    // The next key's turn: its own presses given back count afresh.
    pace_given_back_ = 0;
    pace_given_back_id_ = 0;
    if (pace_cursor_ >= pace_press_.n) {
        pace_cursor_ = 0;
        pace_round_end();
    }
}

bool MacroEngine::pace_ours(const PaceKey& pk, std::uint32_t skill, std::uint64_t at_ms, bool& clean) const {
    // The hunter's swing went down near this press -- from a confirm's wait
    // before the key went down to the cast -- and the cast may be the swing's.
    // The key the swing presses is cast whoever pressed it: its own.
    const bool swing_key = pace_attack_vk_ != 0 && pk.keycode == pace_attack_vk_;
    const bool swung = pace_swing_ms_ != 0 && pace_swing_ms_ + kPaceConfirmMs >= pace_press_.down_ms &&
                       pace_swing_ms_ <= at_ms + kPaceEarlyMs;
    // A skill another key of the rotation's is known to cast is that key's,
    // never this one's to learn nor to count against it (2026.11.17): X,
    // its skill not known for the moment, waited while Z's 51205 came by
    // hand with no swing near, three times -- and was "learned afresh".
    const bool anothers = pk.known_skill != skill && pace_known_elsewhere(skill, pk.id);
    clean = (!swung || swing_key) && !anothers;
    if (pk.known_skill != 0) return skill == pk.known_skill;
    return clean;
}

bool MacroEngine::pace_known_elsewhere(std::uint32_t skill, std::uint64_t except_id) const {
    if (skill == 0) return false;
    for (const PaceKey& k : pace_keys_)
        if (k.id != except_id && k.known_skill == skill) return true;
    return false;
}

void MacroEngine::pace_stamp(std::uint64_t at_ms, std::uint32_t skill, std::uint64_t except_id) {
    if (skill == 0) return;
    for (PaceKey& k : pace_keys_)
        if (k.id != except_id && k.known_skill == skill) pace_recast(k, at_ms);
}

void MacroEngine::pace_recast(PaceKey& k, std::uint64_t at_ms) {
    if (k.last_cast_ms != 0 && at_ms > k.last_cast_ms) {
        const std::uint64_t gap = at_ms - k.last_cast_ms;
        if (k.min_recast_ms == 0 || gap < k.min_recast_ms) k.min_recast_ms = gap;
        // Cast sooner than its floor said it could be: the floor was wrong.
        if (k.floor_ms != 0 && gap < k.floor_ms) {
            k.floor_ms = 0;
            k.floor_uses = 0;
            k.check_after = kPaceFloorCheckTurns;
        }
        // ... or than its sighting said: no cooldown that long.
        if (k.seen_ms != 0 && gap < k.seen_ms) k.seen_ms = 0;
    }
    // A floor no longer than its shortest re-cast: the game cast it again
    // after that long.
    if (k.floor_ms != 0 && k.min_recast_ms != 0 && k.floor_ms > k.min_recast_ms) k.floor_ms = k.min_recast_ms;
    if (at_ms > k.last_cast_ms) k.last_cast_ms = at_ms;
    // A refusal before this cast was measured from an older one.
    k.candidate_ms = 0;
    k.candidate_at = 0;
}

std::uint64_t MacroEngine::pace_floor_cap(const PaceKey& k) const {
    return k.min_recast_ms != 0 ? std::min(kPaceFloorMaxMs, k.min_recast_ms) : kPaceFloorMaxMs;
}

void MacroEngine::pace_buff_cast(std::uint64_t at_ms, std::uint32_t skill) {
    if (pace_burst_vk_ == 0 || at_ms + kPaceEarlyMs < pace_burst_ms_) return;
    // A skill of the rotation's keys, or one with the hunter's swing near:
    // not the buff's. Its cast still to come, or lost: the presses go on.
    if (pace_known_elsewhere(skill, 0)) return;
    if (pace_swing_ms_ != 0 && pace_swing_ms_ + kPaceConfirmMs >= pace_burst_ms_ && pace_swing_ms_ <= at_ms + kPaceEarlyMs)
        return;
    // The buff cast: the burst's other presses were only to make sure of it
    // (buff_presses), and each held the rotation 200 ms more -- the owner's
    // four presses for one cast, 2026-11-17. Pressed only while it is not.
    std::uint64_t spared = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = buff_repeats_.begin(); it != buff_repeats_.end();) {
            if (it->keycode != pace_burst_vk_) {
                ++it;
                continue;
            }
            spared += static_cast<std::uint64_t>(it->left);
            it = buff_repeats_.erase(it);
        }
    }
    pace_minute_.buff_spared += spared;
    pace_burst_vk_ = 0;
    pace_burst_ms_ = 0;
}

void MacroEngine::pace_give_back() {
    ++pace_minute_.given_back;
    if (pace_given_back_id_ != pace_press_.id) {
        pace_given_back_id_ = pace_press_.id;
        pace_given_back_ = 0;
    }
    if (++pace_given_back_ > kPaceGiveBackMost) {
        // Given back as often as a turn may be: its turn over, with no cast
        // and nothing learned of it -- a swing on a beat of its own, or a
        // skill the key casts that is not the one learned, holds the round up
        // no longer.
        pace_step(pace_press_.keycode, Turn::NoCast);
        pace_advance();
    } else {
        pace_cursor_ = pace_press_.pos;
        pace_cursor_id_ = pace_press_.id;
    }
    pace_press_ = PacePress{};
}

void MacroEngine::pace_skip() {
    PaceKey& pk = pace_key(pace_press_.id);
    ++pk.skipped;
    const bool probe = pace_press_.no_learn == kProbing;
    // A probe's press is no turn of the key's (2026.11.17): the game took no
    // key at all then, and nothing is learned of this one.
    if (!probe) {
        pk.history = push_turn(pk.history, true);
        // Its next re-press waits out the longest lock, until a first press
        // of it is cast: the cast before it may lock longer than the guess.
        pk.late = true;
    }
    // A check under its floor not cast: the floor stands, and the next check
    // waits twice as many turns (2026.11.17).
    if (pace_press_.check && !probe && pk.floor_ms != 0)
        pk.check_after = std::min(kPaceFloorUses, pk.check_after * 2);
    // Its cooldown is longer than it had waited at its last key-down: taken
    // as its least once another key casts -- the game casting all along.
    // Only from a skip that can be the cooldown's (2026.11.17): not a probe's,
    // nor one in the moments after a hold, a fallback or the start, or after
    // its turn waited; not one whose turn another's cast took first; and not
    // one longer since its last cast than its skill was ever cast again
    // after -- the game refused it for something else then: a stun, the
    // landing, a pile of monsters.
    const std::uint64_t last = pace_press_.check ? pace_press_.down_ms : pace_press_.again_ms;
    if (probe) {
        ++pace_minute_.skip_probe;
    } else if (pace_press_.no_learn != kLearn) {
        ++pace_minute_.skip_after_hold;
    } else if (pace_given_back_id_ == pace_press_.id && pace_given_back_ > 0) {
        ++pace_minute_.skip_beside;
    } else if (pace_press_.check && pk.floor_ms != 0) {
        // A check under its floor refused (the 2026.11.17 review): it came
        // sooner than the floor, so it can never raise it -- and taught as a
        // candidate, the next cast's raise reset the check gap doubled just
        // above to kPaceFloorCheckTurns, a key with a long cooldown checked
        // every 3 turns all along. Nothing to teach.
    } else if (pk.last_cast_ms != 0 && last > pk.last_cast_ms) {
        if (last - pk.last_cast_ms >= pace_floor_cap(pk)) {
            ++pace_minute_.skip_long;
        } else {
            ++pace_minute_.skip_taught;
            pk.candidate_ms = last - pk.last_cast_ms;
            pk.candidate_at = last;
        }
    }
    pace_step(pace_press_.keycode, Turn::NoCast);
    pace_advance();
    pace_press_ = PacePress{};
    pace_publish();
}

// ------------------------------------------------------------ the presses

bool MacroEngine::shut_out(Refused why, std::uint32_t vk) const {
    return why == Refused::Gated && coord_.pass_keys_open() && !coord_.lets_through(vk);
}

void MacroEngine::pace_refused(Refused why, std::uint64_t now) {
    // One a bag job's key window shut for a click of its own goes the moment
    // it opens again, as on the timers (2026.11.10); and one that met another
    // key going -- the hunter's arrows and swing take the keys all the time
    // (2026.11.16b) -- a moment later, not a whole look on: each such wait was
    // 100 ms of no skill.
    pace_retry_ms_ = now + (why == Refused::Gated || why == Refused::Busy ? kSkillRetryMs : kGatePollMs);
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
    // For the minute's line, each key's presses beside its casts.
    if (i < pass_ids_.size()) ++pace_key(pass_ids_[i]).minute_pressed;
}

void MacroEngine::run_paced(const MacroConfig& cfg, bool follow, std::uint64_t now) {
    if (pace_mode_ != Mode::Paced) {
        swing_yield_vk_.store(0);
        return;
    }

    // The rotation: the skill keys on, in the order of the keys, each by
    // which key it is -- the keys may change between passes. ↓ never goes out
    // (InputManager::never_pressed): no turn of its own; nor, following the
    // fight, a key on its own timer.
    struct Place {
        std::size_t   i;
        std::uint64_t id;
    };
    std::vector<Place> rot;
    for (std::size_t i = 0; i < cfg.keys.size(); ++i)
        if (in_rotation(cfg.keys[i], follow)) rot.push_back(Place{ i, i < pass_ids_.size() ? pass_ids_[i] : 0 });
    // Each one's own, made before any is held: none moves after this.
    for (const Place& p : rot) pace_key(p.id).keycode = cfg.keys[p.i].keycode;
    const std::size_t n = rot.size();
    const auto place_of = [&rot, n](std::uint64_t id) {
        for (std::size_t at = 0; at < n; ++at)
            if (rot[at].id == id) return at;
        return n;
    };

    // The hunter and the fight as they are now (2026.11.16b), not as at the
    // pass's start: the keys pressed since held the thread. Held -- walking
    // back, a moment of danger of the hunter's, and following the fight no
    // fight, MP under the floor or the character down -- the rotation waits
    // at its key, and nothing of it goes out; the timers held the same skills
    // the same way.
    const Fight fight = fight_now();
    const bool returning = fight.mode == Fight::Mode::Returning;
    const bool hunter_hold = fight.mode == Fight::Mode::Held;
    const SkillGate gate = follow ? skill_gate(cfg.skill_mp_floor_pct, fight) : SkillGate::Pass;
    // The character down, following the fight or not (2026.11.17): no skill
    // at a death panel. Not following, nothing held it before, and the
    // rotation's unanswered presses fell back to the timers -- 220 keys a
    // minute at a dead character on 2026-11-17.
    const bool dead = skills_down_now_;
    const bool held = returning || hunter_hold || gate != SkillGate::Pass || dead;
    // Free again after a hold: a refusal in the next moments -- landing,
    // turning, still in a stun -- teaches no cooldown (2026.11.17).
    if (held) {
        pace_was_held_ = true;
    } else if (pace_was_held_) {
        pace_was_held_ = false;
        pace_free_ms_ = now;
    }

    // Whether key `k` waits for the fight this moment: its target further
    // than its range_px, or the dash's key with its landing past an edge --
    // and, following the fight, the hunter's attack key with the target
    // further than the hunter lets its own swing of it go (Fight::attack_far,
    // F3). Before 2026.11.16b only the hunter pressed that key then, within
    // that reach; the rotation pressing it in its turn at a target 300 px
    // off, where its skill reaches some 76, would waste the cast and a whole
    // lock. Not on "Luôn bấm theo giờ": the timers pressed it at any distance.
    const auto waits = [&](const MacroKey& k) {
        if (skill_waits(k, fight, true) != SkillWait::None) return true;
        return follow && fight.mode == Fight::Mode::Fighting && fight.attack_far && fight.attack_key != 0 &&
               k.keycode == fight.attack_key;
    };

    // How the key at `at` would take its turn now: passed over -- its
    // interval not over since its last cast, inside the cooldown learned for
    // it, waiting for the fight -- or pressed (Turn::Cast).
    const auto turn_of = [&](std::size_t at) {
        const MacroKey& k = cfg.keys[rot[at].i];
        const PaceKey& pk = pace_key(rot[at].id);
        if (pk.last_cast_ms != 0 && now < pk.last_cast_ms + macro_timer_interval_ms(k)) return Turn::NotDue;
        if (pk.floor_ms != 0 && pk.last_cast_ms != 0 && now < pk.last_cast_ms + pk.floor_ms &&
            pk.floor_uses < pk.check_after)
            return Turn::Cooling;
        // The target further than the key reaches, or the dash's key with its
        // landing past an edge (2026.11.16b): its turn passes, as the timers
        // and run_skills pass it over -- waited for in place, the one key
        // would hold every other up for as long as the target stays far.
        if (waits(k)) return Turn::Waiting;
        return Turn::Cast;
    };

    // The hunter's swing left to the rotation (swing_yields) while it presses
    // freely: its attack key a key of the rotation's -- pressed on top every
    // attack_interval_ms, the one skill cast all the time (2026-11-15) -- or
    // a key of the rotation's ready to go or waiting for its cast, whose slot
    // the swing would take. With every key on its interval or cooldown, the
    // swing fills the gap; held, mostly the hunter's own again -- it costs
    // no MP (below).
    // The attack key out of its own range_px is no turn of the rotation's
    // either: the hunter's swing, at its own reach, as before.
    // Kept through the holds after which the hunter swings at once and
    // during which it has no swing to make: a turn not yet confirmed (not
    // facing, no swing), nothing to fight, and following the fight no fight.
    // The hunter ends those in a pass of its own -- the turn confirmed, a
    // target engaged -- and swings in that same pass, before this pass has
    // seen the change: cleared under them, that first swing of every fight
    // and after every turn went out, and took the rotation's slot -- the
    // attack key cast some 5-15 times a minute on top of its turns. Cleared
    // under the rest: walking back, MP under the floor, the character down,
    // a drop, a jump's flight (the hunter swings in it), a seek leg, outside
    // the area -- the swing the hunter's own there, as ever.
    const bool yield_kept = !returning && !dead && (hunter_hold ? fight.hold == Fight::Hold::Turning ||
                                                             fight.hold == Fight::Hold::NoTarget
                                                       : gate == SkillGate::Pass || gate == SkillGate::NotFighting);
    std::uint32_t yield = 0;
    if (yield_kept && fight.attack_key != 0) {
        bool rotation_s = pace_press_.active;
        for (std::size_t at = 0; at < n && !rotation_s; ++at) {
            const Turn turn = turn_of(at);
            rotation_s = turn == Turn::Cast ||
                         (cfg.keys[rot[at].i].keycode == fight.attack_key && turn != Turn::Waiting);
        }
        if (rotation_s) yield = fight.attack_key;
    }
    swing_yield_vk_.store(yield);
    // Held, no key goes out, and none goes out unanswered.
    if (held) {
        pace_unanswered_ms_ = 0;
        pace_unanswered_n_ = 0;
    }

    // Keys going out and no cast of yours for kPaceStaleMs: none is coming --
    // out of the world, in a town, the capture stopped. The timers, until
    // the next. On your own frames, never the capture's gaps.
    if (pace_unanswered_ms_ != 0 && now >= pace_unanswered_ms_ + kPaceStaleMs) {
        const std::uint64_t quiet = now - pace_unanswered_ms_;
        const std::uint64_t went = pace_unanswered_n_;
        // The packets still coming (2026.11.17): the capture is fine and the
        // game is taking no key -- the character down with HP the packets
        // have not caught up with, a stun, a pile of monsters (the death of
        // 00:07-00:09 on 2026-11-17: the timers 25.8 and 29.0 s of those
        // minutes, 220-230 keys a minute). The rotation stays and probes, a
        // key a second, until your
        // next cast; the packets gone quiet too, the timers as before.
        const std::uint64_t heard = packets_ms_.load(std::memory_order_relaxed);
        if (heard != 0 && heard + kPaceStaleMs >= now) {
            pace_unanswered_ms_ = 0;
            pace_unanswered_n_ = 0;
            // The press still waiting went into it: it teaches nothing either.
            if (pace_press_.active) pace_press_.no_learn = kProbing;
            if (!pace_probe_) {
                pace_probe_ = true;
                pace_probe_from_ = now;
                GB_INFO("Paced: no cast of yours for %s while %llu key(s) went out, the packets coming -- the game "
                        "takes no key: one key a second until your next cast",
                        pace_seconds(quiet).c_str(), static_cast<unsigned long long>(went));
                pace_publish();
            }
            return;
        }
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
        } else if (held || waits(cfg.keys[rot[at].i])) {
            // A hold since it went out, or its key now waiting on its own:
            // its cast may still come, in its window; none, the press is
            // given back -- no try, nothing learned of the key -- and the key
            // goes again from its place once free. A re-press under a hold
            // would only be dropped, and a skip teach a cooldown it has not.
            const std::uint64_t last = pace_press_.again_ms != 0 ? pace_press_.again_ms : pace_press_.down_ms;
            if (now < last + kPaceConfirmMs) return;
            ++pace_minute_.given_back;
            pace_cursor_ = at;
            pace_cursor_id_ = rot[at].id;
            pace_press_ = PacePress{};
            pace_publish();
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
            } else if (why == Refused::Failed || shut_out(why, k.keycode)) {
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
    if (held) {
        // Counted as the timers count a skill they hold, a look every
        // kGatePollMs -- the rotation's next key, one.
        if (n != 0 && now >= pace_held_look_ms_) {
            pace_held_look_ms_ = now + kGatePollMs;
            ++pace_minute_.held;
            if (hunter_hold) {
                count_hunter_hold(fight.hold, 1);
            } else if (returning) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.held_returning;
            } else {
                count_held(dead ? SkillGate::Dead : gate, 1);
            }
            if (dead) ++pace_minute_.dead;
        }
        return;
    }
    if (n == 0) return;
    // Probing (the game taking no key while the packets come): a key a
    // kPaceProbeMs at most.
    if (pace_probe_ && now < pace_probe_ms_ + kPaceProbeMs) return;

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
        const Turn turn = turn_of(at);
        if (turn != Turn::Cast) {
            passed.push_back(Passed{ at, turn });
            continue;
        }
        found = at;
        break;
    }
    // The keys waiting for the fight, counted as the timers count them: each
    // once as its turn passes -- or, none due, a look every kGatePollMs.
    const auto count_waits = [&] {
        std::uint64_t range = 0, dash = 0;
        for (const Passed& p : passed)
            if (p.turn == Turn::Waiting)
                ++(skill_waits(cfg.keys[rot[p.at].i], fight, true) == SkillWait::Dash ? dash : range);
        if (range == 0 && dash == 0) return false;
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.held_range += range;
        stats_.held_dash += dash;
        return true;
    };
    // None due: nothing pressed, and the round goes on from where it was.
    if (found == n) {
        if (now >= pace_held_look_ms_ && count_waits()) pace_held_look_ms_ = now + kGatePollMs;
        return;
    }
    count_waits();
    for (std::size_t p = 0; p < passed.size(); ++p) {
        if (p == wrap_at) pace_round_end();
        PaceKey& pk = pace_key(rot[passed[p].at].id);
        if (passed[p].turn == Turn::Waiting) pk.waited = true;
        if (passed[p].turn == Turn::Cooling) {
            ++pk.floor_uses;
            ++pk.skipped;
            ++pk.skipped_learned;
            pk.history = push_turn(pk.history, true);
        }
        pace_step(pk.keycode, passed[p].turn);
    }
    if (wrap_at == passed.size()) pace_round_end();
    // Another key's turn than the one given back: its own count afresh.
    if (rot[found].id != pace_given_back_id_) {
        pace_given_back_ = 0;
        pace_given_back_id_ = 0;
    }
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
        if (why != Refused::Failed && !shut_out(why, k.keycode)) {
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
    p.at_lock = at_lock && !pace_probe_;
    // A probe's press goes once, as a check does: no re-press.
    p.check = check || pace_probe_;
    // What its skip may teach (2026.11.17): nothing while probing, nor in
    // the moments after a hold, a fallback or the start, nor after its turn
    // passed waiting.
    p.no_learn = pace_probe_                                       ? kProbing
                 : pk.waited || down < pace_free_ms_ + kPaceSettleMs ? kAfterHold
                                                                   : kLearn;
    pk.waited = false;
    if (pace_probe_) pace_probe_ms_ = down;
    pace_press_ = p;
}

std::uint64_t MacroEngine::fallback_spacing_ms(const MacroConfig& cfg, bool follow) const {
    // Only once your casts have been seen this run: with none ever -- no
    // capture, no Npcap -- the timers are the player's, as always.
    if (pace_mode_ != Mode::Fallback || casts_noted_.load(std::memory_order_acquire) == 0) return 0;
    std::uint64_t n = 0;
    for (const MacroKey& k : cfg.keys) n += in_rotation(k, follow) ? 1 : 0;
    return n * pace_lock_ms_;
}

bool MacroEngine::skills_down(std::uint64_t now) {
    float hp = -1.0f, mp = -1.0f;
    current_vitals(hp, mp);
    if (hp < 0.0f || hp >= kDownHpPct) {
        skill_down_since_ = 0;
        return false;
    }
    // A moment's 0 is not a death: a reading can be wrong for a frame.
    if (skill_down_since_ == 0) skill_down_since_ = std::max<std::uint64_t>(now, 1);
    return now >= skill_down_since_ + kSkillsDownMs;
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
    if (pace_probe_) {
        pace_minute_.probe_ms += now > pace_probe_from_ ? now - pace_probe_from_ : 0;
        pace_probe_from_ = now;
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
            // 2026.11.17: what bounds it -- the shortest gap its skill was
            // cast again after, anyone's.
            if (k.min_recast_ms != 0) part += ", re-cast " + pace_seconds(k.min_recast_ms);
            part += ")";
        }
        skipped += (skipped.empty() ? "" : ", ") + part;
        k.skipped = 0;
        k.skipped_learned = 0;
    }
    // Each key's presses that went out beside its casts confirmed, in the
    // order the rotation first met them (2026.11.16b): how evenly the keys
    // came, read off one line -- "Z 9/9, X 9/8".
    std::string keys;
    for (PaceKey& k : pace_keys_) {
        if (k.minute_pressed == 0 && k.minute_cast == 0) continue;
        keys += (keys.empty() ? "" : ", ") + InputManager::key_name(k.keycode) + " " +
                std::to_string(k.minute_pressed) + "/" + std::to_string(k.minute_cast);
        k.minute_pressed = 0;
        k.minute_cast = 0;
    }
    if (pace_mode_ == Mode::Off && m.confirmed == 0 && m.repressed == 0 && m.other == 0 && m.fallback_ms == 0 &&
        skipped.empty() && keys.empty() && m.given_back == 0 && m.held == 0)
        return;
    // 2026.11.17's after 2026.11.16b's, so a line reads as before up to them:
    // each skip, whether it taught a cooldown and why not, floors cut to
    // their cap, the time probing and held for the character down, the buff
    // presses its cast seen spared.
    char learn[320];
    std::snprintf(learn, sizeof learn,
                  "; skips taught %llu, not taught: %llu after a hold or wait, %llu beside another's cast, %llu "
                  "longer than its re-cast, %llu probing; floors capped %llu; probing %s; held dead %.1f s; buff "
                  "presses spared %llu",
                  static_cast<unsigned long long>(m.skip_taught), static_cast<unsigned long long>(m.skip_after_hold),
                  static_cast<unsigned long long>(m.skip_beside), static_cast<unsigned long long>(m.skip_long),
                  static_cast<unsigned long long>(m.skip_probe), static_cast<unsigned long long>(m.capped),
                  pace_seconds(m.probe_ms).c_str(), static_cast<double>(m.dead * kGatePollMs) / 1000.0,
                  static_cast<unsigned long long>(m.buff_spared));
    // 2026.11.16b's after the mode, so a line reads as before up to it.
    GB_INFO("Paced: %llu rounds; %llu casts confirmed (%llu on a re-press); %llu re-presses; skipped (cooldown): "
            "%s; fallback %s; lock_est %u ms; other casts %llu; now %s"
            "; given back %llu; held %.1f s; per key pressed/cast: %s%s",
            static_cast<unsigned long long>(m.rounds), static_cast<unsigned long long>(m.confirmed),
            static_cast<unsigned long long>(m.on_repress), static_cast<unsigned long long>(m.repressed),
            skipped.empty() ? "none" : skipped.c_str(), pace_seconds(m.fallback_ms).c_str(), pace_lock_ms_,
            static_cast<unsigned long long>(m.other), mode_text(pace_mode_, pace_hunting_),
            static_cast<unsigned long long>(m.given_back), static_cast<double>(m.held * kGatePollMs) / 1000.0,
            keys.empty() ? "none" : keys.c_str(), learn);
}

} // namespace gobot
