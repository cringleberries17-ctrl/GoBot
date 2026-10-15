#pragma once

#include "gobot/bar_spec.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/input_manager.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gobot {

/// What has to be true before a key fires.
enum class MacroTrigger {
    Always,     ///< a plain repeating timer
    HpBelow,    ///< only while HP is at or under the threshold
    MpBelow,
};

const char*  to_string(MacroTrigger t);
MacroTrigger macro_trigger_from_string(const std::string& s);

/// A key that fires on its own repeating timer.
///
/// This is what a game macro actually is most of the time: a skill on a 5
/// second cooldown, a buff on a 10 minute one, a potion every 30 seconds —
/// each running independently rather than as one rigid sequence. Modelling
/// them as separate timers means adding a skill cannot disturb the timing of
/// the others.
struct MacroKey {
    std::uint32_t keycode = 0;
    std::string   label;
    bool          enabled = false;

    std::uint32_t interval_ms = 1000;
    std::uint32_t hold_ms = 0;          ///< 0 = InputManager's own default
    std::uint32_t start_delay_ms = 0;   ///< stagger, so timers don't all align
    int           jitter_pct = 10;      ///< +/- this much on each interval

    /// Skip firing while another key is mid-press, rather than queueing up
    /// behind it. A macro that catches up on missed presses produces bursts,
    /// which is both useless in game and conspicuous.
    bool drop_if_busy = true;

    /// Gate the key on a vital.
    ///
    /// This is the difference between a potion key and a timer. "HP below 40"
    /// drinks when you are actually hurt; a 30-second timer drinks whether you
    /// need it or not, and empties your inventory doing it.
    ///
    /// `interval_ms` still applies as the minimum gap between firings, so it
    /// reads as the potion's cooldown rather than as a polling rate.
    MacroTrigger trigger = MacroTrigger::Always;
    int          threshold_pct = 50;

    bool conditional() const { return trigger != MacroTrigger::Always; }
};

/// What a key is for, worked out from how it is set up.
///
/// Keys carry no role of their own: a potion is a key with a trigger on it,
/// and a buff is a key on a long timer. The simple screen groups them by this,
/// so a player sees "HP potion, key 1" rather than a table of nine columns.
enum class MacroKeyKind {
    Skill,      ///< a short timer: fired over and over while fighting
    Buff,       ///< a long timer: cast again every few minutes
    HpPotion,   ///< drinks when HP drops
    MpPotion,   ///< drinks when MP drops
};

/// Timers at least this long are buffs, anything shorter a skill. A minute
/// sits well clear of both: skill cooldowns here run to seconds, buffs to
/// minutes.
constexpr std::uint32_t kBuffMinIntervalMs = 60'000;

MacroKeyKind macro_key_kind(const MacroKey& k);

/// A buff's interval as the simple screen takes it, in seconds: from the
/// shortest that is still a buff -- shorter, and the key would turn into a
/// skill and jump rows -- to two hours.
constexpr int kBuffMinSeconds = static_cast<int>(kBuffMinIntervalMs / 1000);
constexpr int kBuffMaxSeconds = 7200;
int clamp_buff_seconds(int seconds);

/// One entry in an ordered sequence.
struct MacroStep {
    std::uint32_t keycode = 0;          ///< 0 = pure delay
    std::uint32_t hold_ms = 0;
    std::uint32_t delay_after_ms = 100;
    std::string   label;
};

struct MacroConfig {
    static constexpr std::size_t kMaxKeys = 12;

    bool                  timers_enabled = true;
    std::vector<MacroKey> keys;

    bool                   sequence_enabled = false;
    bool                   sequence_loop = true;
    std::vector<MacroStep> sequence;

    /// Only send input while the target window has focus. Off means keys go
    /// to whatever is in front, which will eventually be something you did not
    /// intend.
    bool require_focus = true;

    /// Bring the game to the front when the macro is switched on.
    ///
    /// Without this, pressing START MACRO leaves the game behind the GoBot
    /// window and the macro sits there holding, which reads as "it does not
    /// work". Only on the way on: re-focusing whenever attention wanders would
    /// mean fighting the user for their own desktop.
    bool focus_on_start = true;

    /// Applies on top of each key's own jitter.
    int global_jitter_pct = 0;

    std::uint32_t toggle_key = 121;     ///< F10
    std::uint32_t panic_key = 123;      ///< F12 — stop and release everything

    /// Where HP and MP are drawn on screen. Kept with the macro rather than in
    /// a config of their own: they exist to make the conditional keys above
    /// work, and one Save should cover a key and its trigger together.
    bool    vitals_enabled = false;
    BarSpec hp_bar;
    BarSpec mp_bar;

    /// True when at least one key actually depends on a reading.
    bool needs_vitals() const;

    /// Put the bars where they usually are, as a starting point to nudge.
    void seed_vitals_defaults();

    bool load(const std::string& path);
    bool save(const std::string& path) const;
    void apply_defaults();
};

/// Runs the macro.
///
/// Timers and the sequence run on separate threads and both go through
/// ExecutionCoordinator, so two keys are never pressed at the same instant.
class MacroEngine {
public:
    MacroEngine(InputManager& input, ExecutionCoordinator& coord);
    ~MacroEngine();

    MacroEngine(const MacroEngine&) = delete;
    MacroEngine& operator=(const MacroEngine&) = delete;

    /// Takes effect at once. A timed key whose interval is made shorter while
    /// the macro runs -- a buff's seconds box turned down from 300 to 60 --
    /// comes due at its last press plus the new interval, rather than when
    /// the old one runs out: a box changed during a run otherwise waited up
    /// to the whole old interval first.
    void set_config(const MacroConfig& cfg);
    MacroConfig config() const;

    void start();
    void stop();
    bool running() const { return running_.load(); }

    /// Feed in the current vitals as percentages; -1 for a bar that cannot be
    /// read. Push these regularly — a reading older than a second or so is
    /// treated as missing, because a potion rule acting on stale HP is worse
    /// than one that holds off.
    ///
    /// HP read at nothing (under kDownHpPct) is also the character down --
    /// dead, at the death panel -- which tells a re-cast asked for afterwards
    /// from one more ask about the last return: see recast_buffs.
    void set_vitals(float hp_pct, float mp_pct);
    static constexpr float kDownHpPct = 0.5f;

    /// What the engine is currently acting on: the last pushed values, or -1
    /// each once they have gone stale.
    void current_vitals(float& hp_pct, float& mp_pct) const;

    /// Hold every key without stopping, and say why.
    ///
    /// For when the character is not in the world: disconnected, at the
    /// channel list, on a loading screen, dead at a revive prompt. The timers
    /// keep running and the macro stays "on", but nothing is pressed — which
    /// is the difference between coming back to a bot that paused for an hour
    /// and coming back to one that spent the hour hammering potion keys at a
    /// disconnect dialog.
    ///
    /// Suspending rather than stopping, because stopping is a thing the user
    /// did and this is not: the macro should pick up again by itself when the
    /// character does.
    ///
    /// A suspension of kRecastAfterSuspendMs or more -- a death, a disconnect,
    /// a re-login, not a map change -- re-casts every buff when it ends: see
    /// recast_buffs.
    void set_suspended(bool suspended, const std::string& why = {});
    bool suspended() const { return suspended_.load(); }
    std::string suspended_reason() const;

    /// Cast every buff again, soon, one after another: each enabled Buff key
    /// comes due kRecastFirstMs from now and the rest kRecastGapMs apart, in
    /// the order of the keys, give or take a tenth of the gap -- and goes on
    /// on its own interval from that press.
    ///
    /// For after a death, a revive or a re-login, which take every buff off
    /// the character while their timers run on: a 1600-s buff was otherwise
    /// gone for up to 27 minutes. GoC resets every buff's last cast at those
    /// moments ("Buff state reset: forcing all buffs to cast immediately").
    ///
    /// One return is asked about several times -- the revive, HP coming back
    /// from 0, the HUD coming back, all within a second or two -- and is
    /// re-cast once: a call within kRecastMergeMs of the last re-cast, with
    /// the character not seen down since (HP read at 0, or out of the world),
    /// does nothing. Anything else is a return of its own and re-casts: a
    /// second death soon after a revive, or a death soon after BẮT ĐẦU -- a
    /// start is not a re-cast. Asked for while suspended, it is done the
    /// moment the suspension ends: nothing can be cast out of the world, and
    /// the asks made meanwhile are one. Nothing while the macro is not
    /// running. True when it re-armed anything, or will; `why` goes into the
    /// log.
    bool recast_buffs(const std::string& why);

    static constexpr std::uint64_t kRecastFirstMs = 1000;
    static constexpr std::uint64_t kRecastGapMs = 1500;
    static constexpr std::uint64_t kRecastMergeMs = 5000;
    static constexpr std::uint64_t kRecastAfterSuspendMs = 10000;
    /// A buff whose press did not go out -- held off, the game not in front,
    /// another job holding the keys -- is tried again this soon, not a whole
    /// interval later. Skills and potions are not: a missed skill is just
    /// missed, and the next one comes soon anyway.
    static constexpr std::uint32_t kBuffRetryMs = 2000;
    /// Buffs never go out closer together than this -- at a start, after a
    /// re-cast or tried again: the game drops a cast that comes right on top
    /// of another.
    static constexpr std::uint64_t kBuffGapMs = 1200;

    /// The time as the engine reads it, in milliseconds: the steady clock
    /// unless set.
    struct Clock {
        std::function<std::uint64_t()> now_ms;
    };
    /// What a press is sent with: whether the game is in front, and the key
    /// itself. The game's keyboard unless set.
    struct Hands {
        std::function<bool()>                    focused;
        std::function<bool(std::uint32_t, int)>  key;    ///< keycode, hold ms (-1 = default)
    };
    /// Both before start(), never while it runs.
    void set_clock(Clock c) { clock_ = std::move(c); }
    void set_hands(Hands h) { hands_ = std::move(h); }

    /// For tests, which drive the timers by hand on a clock of their own:
    /// set the timers up as start() does, with no threads, and run one pass
    /// of them at the clock's time with each step(). stop() ends it.
    void start_stepped();
    void step();

    struct Stats {
        std::uint64_t presses = 0;
        std::uint64_t skipped_unfocused = 0;
        std::uint64_t skipped_busy = 0;
        std::uint64_t sequence_loops = 0;

        /// Conditional keys that came due but held: the condition was false,
        /// or there was no reading to test it against. Separated because they
        /// mean opposite things — the first is the macro working, the second
        /// is a bar that needs setting up.
        std::uint64_t held_condition = 0;
        std::uint64_t held_no_reading = 0;

        /// Presses held because the character was not in the world.
        std::uint64_t held_suspended = 0;

        /// Presses per timed key, in the order of the config's keys. What a
        /// panel needs to say "3 potions" rather than "412 presses", a figure
        /// that counts every skill along with them. Sequence steps are not
        /// counted here: they are not keys of the config.
        std::vector<std::uint64_t> key_presses;
    };
    Stats stats() const;
    void  reset_stats();

    /// Milliseconds until each timed key next fires; 0 when not running.
    std::vector<std::uint64_t> next_due_ms() const;

    /// Index of the sequence step about to run.
    std::size_t sequence_position() const { return seq_pos_.load(); }

private:
    void timer_loop();
    /// One pass over the timed keys at `now`: the timer thread's, and step()'s.
    void run_timers(std::uint64_t now);
    /// Every key's first due time, from `now`.
    void arm(std::uint64_t now);
    /// recast_buffs, now, the character being in the world.
    bool apply_recast(const std::string& why);
    void sequence_loop_fn();
    bool press(std::uint32_t keycode, std::uint32_t hold_ms, const char* owner);
    std::uint32_t jittered(std::uint32_t base, int pct) const;
    std::uint64_t clock_now() const;

    enum class Gate { Pass, Blocked, NoReading };

    /// Test a key's trigger. Takes no locks, so the caller can count the
    /// result without nesting the config and stats mutexes.
    Gate check_gate(const MacroKey& k) const;

    /// How long a conditional key waits before testing its trigger again.
    /// The key's own interval is its cooldown, not its reaction time.
    static constexpr std::uint32_t kGatePollMs = 120;

    /// A reading older than this is treated as absent.
    static constexpr std::uint64_t kVitalsStaleMs = 1500;

    InputManager&         input_;
    ExecutionCoordinator& coord_;

    std::atomic<float>         vit_hp_{-1.0f};
    std::atomic<float>         vit_mp_{-1.0f};
    std::atomic<std::uint64_t> vit_at_ms_{0};

    mutable std::mutex mutex_;
    MacroConfig        config_;

    std::thread       timer_thread_;
    std::thread       seq_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> suspended_{false};
    std::string       suspended_why_;
    std::atomic<std::size_t> seq_pos_{0};

    Clock             clock_;
    Hands             hands_;
    std::atomic<bool> stepped_{false};      ///< started by start_stepped: no threads
    /// When the current suspension began, on clock_now(); guarded by mutex_.
    std::uint64_t     suspended_at_ms_ = 0;
    /// When the buffs were last re-cast; guarded by mutex_. Valid once
    /// `recast_once_` is set -- which a start clears.
    std::uint64_t     recast_ms_ = 0;
    bool              recast_once_ = false;
    /// When the character was last seen down: HP read at 0, or going out of
    /// the world or coming back into it. On clock_now().
    std::atomic<std::uint64_t> down_ms_{0};
    /// A re-cast asked for while suspended, and what for; guarded by mutex_.
    bool              recast_pending_ = false;
    std::string       recast_why_;
    /// When a buff last went out, for kBuffGapMs; guarded by mutex_.
    std::uint64_t     last_buff_ms_ = 0;
    bool              buff_pressed_ = false;

    /// Absolute time each key is next due. Parallel to config_.keys.
    std::vector<std::uint64_t> due_ms_;
    /// When each key's press last went out, or 0; parallel to config_.keys,
    /// guarded by mutex_. For an interval shortened during a run.
    std::vector<std::uint64_t> pressed_ms_;

    mutable std::mutex stats_mutex_;
    Stats              stats_;
};

} // namespace gobot
