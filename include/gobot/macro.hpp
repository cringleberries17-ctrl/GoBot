#pragma once

#include "gobot/bar_spec.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/input_manager.hpp"

#include <atomic>
#include <cstdint>
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

    void set_config(const MacroConfig& cfg);
    MacroConfig config() const;

    void start();
    void stop();
    bool running() const { return running_.load(); }

    /// Feed in the current vitals as percentages; -1 for a bar that cannot be
    /// read. Push these regularly — a reading older than a second or so is
    /// treated as missing, because a potion rule acting on stale HP is worse
    /// than one that holds off.
    void set_vitals(float hp_pct, float mp_pct);

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
    void set_suspended(bool suspended, const std::string& why = {});
    bool suspended() const { return suspended_.load(); }
    std::string suspended_reason() const;

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
    void sequence_loop_fn();
    bool press(std::uint32_t keycode, std::uint32_t hold_ms, const char* owner);
    std::uint32_t jittered(std::uint32_t base, int pct) const;

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

    /// Absolute time each key is next due. Parallel to config_.keys.
    std::vector<std::uint64_t> due_ms_;

    mutable std::mutex stats_mutex_;
    Stats              stats_;
};

} // namespace gobot
