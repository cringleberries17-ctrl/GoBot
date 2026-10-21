#pragma once

#include "gobot/bar_spec.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/input_manager.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
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

    /// A skill key that keeps its own timer when skills follow the fight
    /// (MacroConfig::skills_follow_fight): pressed every interval_ms as
    /// before, fight or no fight, MP or none -- for a skill that is really a
    /// buff on the character. "Luôn bấm theo giờ".
    bool always_timer = false;

    /// How far to the side the target may be for this skill to reach it, in
    /// pixels; 0 is any distance. GoC's skill_N_aoe_max_x. Only when skills
    /// follow the fight.
    int range_px = 0;
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

/// A skill's cooldown as the "Kỹ năng" window takes it: half a second to 59 s,
/// in tenths. A minute or more and the key would be a buff, and leave the
/// skills.
constexpr std::uint32_t kSkillMinMs = 500;
constexpr std::uint32_t kSkillMaxMs = 59'000;
std::uint32_t clamp_skill_ms(std::uint32_t ms);
/// A skill's range in pixels, 0 for any distance.
constexpr int kSkillMaxRangePx = 1000;

/// The keys of one kind, in the order of the keys. A potion row on the
/// simple screen shows the first of its kind as Lv1 and the second as Lv2,
/// as GoC has them; any after that only on the full screen.
std::vector<std::size_t> macro_keys_of_kind(const std::vector<MacroKey>& keys, MacroKeyKind kind);

/// A potion's threshold as the simple screen takes it, in percent.
constexpr int kPotionMinPct = 5;
constexpr int kPotionMaxPct = 95;
int clamp_potion_pct(int pct);

/// Of the potions of `kind` among `keys` that want to drink with their bar
/// read at `pct` -- on, with a key and a cooldown, `pct` at or under the
/// mark -- the one that may: the lowest mark, the first in the order of the
/// keys on a tie. GoC's tiers: only the more urgent one drinks. -1 for none,
/// and for no reading (`pct` under 0). The engine's rule, and what the full
/// screen shows as "firing".
int macro_urgent_potion(const std::vector<MacroKey>& keys, MacroKeyKind kind, float pct);

/// A potion key as the simple screen makes one: tier 1 or 2 of the HP or MP
/// row, on, drinking at most every 3 s. Lv1 drinks under 50% HP or 30% MP --
/// what the screen has always put back when a row had no key -- and Lv2,
/// the second potion, lower: 30% HP, 20% MP, labelled "HP potion 2" and "MP
/// potion 2". Anything but a potion kind gives a key with no keycode.
MacroKey macro_potion_key(MacroKeyKind kind, std::uint32_t keycode, int tier);

/// Take out the keys the simple screen has emptied: a skill or buff whose box
/// was cleared, and a potion row's Lv2 or later likewise. A row's Lv1 stays,
/// key or none -- the row is drawn from it. True if anything went.
bool drop_empty_keys(std::vector<MacroKey>& keys);

/// Which of `was` each of `now` is: the same keycode, trigger and kind, the
/// first of `was` not taken already -- so a key taken out of the middle of a
/// list moves nothing else's timer, nor its tally, onto the key that comes
/// up into its place. An index into `was` for each of `now`, -1 for a key
/// new to the list.
std::vector<int> macro_key_match(const std::vector<MacroKey>& was, const std::vector<MacroKey>& now);

/// One entry in an ordered sequence.
struct MacroStep {
    std::uint32_t keycode = 0;          ///< 0 = pure delay
    std::uint32_t hold_ms = 0;
    std::uint32_t delay_after_ms = 100;
    std::string   label;
};

struct MacroConfig {
    /// Room for the player's skills, buffs and both tiers of each potion: the
    /// player's GoC set came to eleven keys before the second tiers. A build
    /// from before 16 reads only key_0..key_11 of macro.ini, and drops the
    /// rest when it saves.
    static constexpr std::size_t kMaxKeys = 16;

    bool                  timers_enabled = true;
    std::vector<MacroKey> keys;

    bool                   sequence_enabled = false;
    bool                   sequence_loop = true;
    std::vector<MacroStep> sequence;

    /// Skills follow the fight ("Chỉ khi đang đánh quái"): with the hunter
    /// running, the skill keys -- all but those on always_timer -- and the
    /// sequence's steps go only while it is attacking a target in reach and
    /// facing it (Hunter::engaged), never with MP under skill_mp_floor_pct,
    /// never with the character down, and each only within its range_px of
    /// the target. Off, or with no hunter running, every key keeps its timer
    /// as it always has. Ships off: it changes how an existing setup fights.
    bool skills_follow_fight = false;

    /// GoC's "Dùng skill tuần tự". On: every skill whose cooldown is over
    /// goes, in the order of the keys, skill_gap_ms after the one before.
    /// Off: only the first of them, one chance every 300 ms.
    bool sequential_skills = true;

    /// Under this much MP, read just now, no skill goes when skills follow
    /// the fight: the hunter's own swing carries on, which costs none. GoC
    /// fixes it at 10. 0 is never; at most kSkillMaxMpFloor.
    int skill_mp_floor_pct = 10;
    static constexpr int kSkillMaxMpFloor = 50;

    /// The gap after one skill's key comes up before the next goes down, in
    /// a burst. GoC's is 50 ms; kept in the file so a live test can widen it
    /// should the game drop presses that close.
    std::uint32_t skill_gap_ms = 50;
    static constexpr std::uint32_t kSkillGapMinMs = 30;
    static constexpr std::uint32_t kSkillGapMaxMs = 1000;

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
    ///
    /// Keys added, taken out or changed while it runs keep their times by
    /// which key they are (macro_key_match), not by where they stand: a
    /// skill taken out from in front of a potion otherwise left the potion
    /// with that skill's due time, and it did not drink for minutes with HP
    /// low. A potion new to the list is due at once, any other new key at its
    /// start delay; the presses each key has made go with it.
    ///
    /// So too for a pass of the timers already under way -- the GUI calls
    /// this on its own thread, whenever a key is pressed on the timers' --
    /// which finds each key it is working on by which key it is, not by where
    /// it stood when the pass began: see key_ids_.
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
    /// A reading older than this is treated as absent: pushed this long ago,
    /// or -- by whoever pushes it, with VitalsReader::if_fresh -- read off
    /// the bars this long ago.
    static constexpr std::uint64_t kVitalsStaleMs = 1500;

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
    /// interval later. Skills are not: a missed skill is just missed, and the
    /// next one comes soon anyway. Potions have rules of their own, below.
    static constexpr std::uint32_t kBuffRetryMs = 2000;
    /// Buffs never go out closer together than this -- at a start, after a
    /// re-cast or tried again: the game drops a cast that comes right on top
    /// of another.
    static constexpr std::uint64_t kBuffGapMs = 1200;

    /// Potions, as GoC drinks them (go_client's heal check):
    ///
    /// - Of the potion keys of one kind whose reading is at or under their
    ///   threshold, only the one with the lowest threshold may drink -- the
    ///   first in key order on a tie. The others wait, even while that one is
    ///   on its cooldown: counted as held_tier.
    /// - The potion gates are looked at on every pass of the timers -- a
    ///   reading, a cooldown over or a hold ended is acted on at once, before
    ///   anything else of the pass -- except for kGatePollMs after a potion of
    ///   that kind went out: GoC's "every HP update, at most once in 100 ms
    ///   after a press".
    /// - The HP potion goes first, then the MP potion no sooner than
    ///   kPotionGapMs after the HP key came up, and both before any other key
    ///   due in the same pass.
    /// - While a potion is due and has not gone out, no other key of the
    ///   macro is started -- for at most kPotionFirstMs of time in which it
    ///   could have gone, so a potion that cannot be pressed does not starve
    ///   the rest. A hold, the game not in front or the character out of the
    ///   world refuse every key alike, and do not count. The sequence, on a
    ///   thread of its own, waits too: from BẮT ĐẦU until the potions have
    ///   first been looked at, from the moment one is found due until the HP
    ///   potion and then the MP one have gone, and in the moment between a
    ///   reading and the pass that acts on it.
    /// - A potion press that does not go out never starts its cooldown. It is
    ///   tried again kPotionRetryMs later when another job held the keys, and
    ///   the moment a hold, the game being behind another window or the
    ///   character being out of the world is over. Its cooldown -- interval_ms,
    ///   jittered -- counts from the press that went out, and is that of the
    ///   key: every potion on the same keycode, Lv1 and Lv2 on one key among
    ///   them, starts its own with it.
    /// - A potion due when the macro starts drinks on the first look: a
    ///   potion's start delay is not waited out.
    ///
    /// The cooldown is GoBot's own. GoC has none and drinks every 100 ms while
    /// low, reading HP from the game itself; GoBot reads a bar off the screen
    /// every 150 ms, and a potion shows on it a moment after it is drunk, so
    /// drinking that often would waste potions.
    static constexpr std::uint32_t kPotionRetryMs = 30;
    static constexpr std::uint32_t kPotionGapMs = 20;
    static constexpr std::uint64_t kPotionFirstMs = 300;

    /// What the hunter is doing, for skills that follow the fight
    /// (MacroConfig::skills_follow_fight). Asked on the macro's own threads,
    /// so whatever answers reads atomics only.
    struct Fight {
        enum class Mode {
            NoHunter,   ///< not hunting: every key on its timer, as before
            Idle,       ///< hunting, and not attacking anything this moment
            Fighting,   ///< attacking a target in reach, and facing it
        };
        Mode          mode = Mode::NoHunter;
        int           dx = 0;             ///< how far to the side the target is, while Fighting
        std::uint32_t attack_key = 0;     ///< the hunter's own swing: not the skills' to press
    };
    /// Before start(), never while it runs. Unset is no hunter.
    void set_fight(std::function<Fight()> f) { fight_ = std::move(f); }

    /// With sequential_skills off: a skill at most this often, GoC's attack
    /// thread's beat.
    static constexpr std::uint32_t kSkillChanceMs = 300;
    /// A skill whose press did not go out is tried again this soon, still
    /// due: its cooldown starts only from a press that went out.
    static constexpr std::uint32_t kSkillRetryMs = 30;

    /// The time as the engine reads it, in milliseconds: the steady clock
    /// unless set. `sleep_ms` is how it waits a few milliseconds inside a
    /// pass -- the gap between an HP and an MP potion -- and a real sleep
    /// unless set: a test on a clock of its own winds that clock on instead.
    struct Clock {
        std::function<std::uint64_t()>      now_ms;
        std::function<void(std::uint32_t)>  sleep_ms;
    };
    /// What a press is sent with: whether the game is in front, and the key
    /// itself. The game's keyboard unless set. `held` says whether a key is
    /// down now, and `release_all` lets go of every key whatever has focus:
    /// for a press whose key went down and could not come up again, the game
    /// having lost focus during it. With `key` set and `held` not, no press
    /// is taken for one of those.
    struct Hands {
        std::function<bool()>                    focused;
        std::function<bool(std::uint32_t, int)>  key;    ///< keycode, hold ms (-1 = default)
        std::function<bool(std::uint32_t)>       held;
        std::function<void()>                    release_all;
    };
    /// Both before start(), never while it runs.
    void set_clock(Clock c) { clock_ = std::move(c); }
    void set_hands(Hands h) { hands_ = std::move(h); }

    /// For tests, which drive the timers by hand on a clock of their own:
    /// set the timers up as start() does, with no threads, and run one pass
    /// of them at the clock's time with each step() -- and the sequence's
    /// turn too, whenever it is due by that clock: the very turn its thread
    /// takes (sequence_turn), so a test of the sequence counts on the test's
    /// time, not on how much of the real CPU its thread got. stop() ends it.
    void start_stepped();
    void step();
    /// The two halves of a step(), for a test that stands in for the two
    /// threads itself, each where its thread would be: one pass of the
    /// timers at the clock's time -- what the timer thread does each time it
    /// wakes -- and the sequence's turn when it is due by the clock, the next
    /// one counted from the end of this one, as its thread sleeps after it.
    /// A test that runs one of them from inside the other's press or pause
    /// -- the clock's sleep_ms, the hands' key -- has the two meet as the
    /// threads do: a press while the other holds the keys is refused.
    void step_timers();
    void step_sequence();

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

        /// A potion whose condition held, and whose cooldown was over, kept
        /// from drinking because another of its kind sits lower and is low
        /// enough too: only the more urgent tier drinks.
        std::uint64_t held_tier = 0;

        /// Presses held because the character was not in the world.
        std::uint64_t held_suspended = 0;

        /// Skills that follow the fight, held back while ready: no fight, MP
        /// under the floor, the character down, or the target further than
        /// the skill reaches. One for each ready key at each look, a look
        /// every 100 ms -- the sequence's steps at each of theirs, 50 ms.
        std::uint64_t held_not_fighting = 0;
        std::uint64_t held_low_mp = 0;
        std::uint64_t held_dead = 0;
        std::uint64_t held_range = 0;

        /// Steps of the sequence that went out: key_presses has only the keys.
        std::uint64_t sequence_presses = 0;

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

    // ---- the black box ----
    //
    // On 2026-10-20 the character died with the HP potion on, the slot's count
    // standing still for the 36 s before while the MP potion's fell: nothing in
    // the log said what HP read, nor why the HP potion did not go. So the
    // engine keeps the last kBlackBoxKeepMs of both, on its own clock: each
    // vitals reading pushed, and each potion key's decision on each look at
    // it -- recorded when the reason changes, and at every press.

    /// What a look at a potion key decided.
    enum class PotionWhy {
        Pressed,        ///< the press went out
        AboveMark,      ///< the reading is above the key's mark: nothing to drink
        NoReading,      ///< no reading of its bar: none pushed, or older than 1.5 s
        Cooling,        ///< at or under the mark, its interval (with jitter) not over since its last press
        OtherTier,      ///< another key of its kind sits lower and is under its mark too: that one drinks
        NotInFront,     ///< the game is not the window in front (require_focus)
        NotInWorld,     ///< the macro is suspended: the character is not in the world
        KeysHeld,       ///< something of GoBot's holds the game -- a chat line, a revive: which, with it
        KeysBusy,       ///< another job had the keys that moment (tried again 30 ms on)
        KeyFailed,      ///< the key did not go out
        BehindHp,       ///< an MP potion waiting while the HP potion waits for the keys
    };
    /// "pressed", "above the mark", "no reading", "cooling", "other tier",
    /// "game not in front", "not in the world", "keys held", "keys busy",
    /// "key failed", "behind the HP potion".
    static const char* potion_why_name(PotionWhy why);

    /// The black box, for the log right after "ALERT death": the last
    /// kBlackBoxMs up to now on the engine's clock, in at most kBlackBoxLines
    /// lines of at most kBlackBoxLineChars characters, times in seconds before
    /// now. The first line is HP and MP as read -- every change of the last
    /// 5 s, and before that the lowest HP of each 2 s; then a line for each
    /// potion key, HP's first, of its decisions -- each run of one reason with
    /// how long it lasted, every press marked; and a last line of the presses
    /// of each. A key by its name and mark only, never a label the player
    /// typed. Empty when nothing was recorded. From any thread.
    std::vector<std::string> black_box_lines() const;
    static constexpr std::uint64_t kBlackBoxMs = 30'000;
    /// Kept for longer than it says: a death is alerted up to 3 s after HP
    /// read 0.
    static constexpr std::uint64_t kBlackBoxKeepMs = 45'000;
    /// At most this many records are kept, vitals and decisions together.
    static constexpr std::size_t kBlackBoxMax = 600;
    static constexpr std::size_t kBlackBoxLines = 8;
    static constexpr std::size_t kBlackBoxLineChars = 600;
    /// Records in the black box now, for tests.
    std::size_t black_box_size() const;

private:
    void timer_loop();
    /// One pass over the timed keys at `now`: the timer thread's, and step()'s.
    void run_timers(std::uint64_t now);
    /// The potions' part of a pass, first: see kPotionRetryMs.
    void run_potions(const MacroConfig& cfg, std::uint64_t now);
    /// Whether a potion is due and has not gone out, within kPotionFirstMs of
    /// falling due: nothing else of the macro starts meanwhile.
    bool potion_first(std::uint64_t now) const;
    /// Whether a potion is due now -- the one of its kind that may drink, by
    /// the reading and its cooldown -- whether or not the timer pass has
    /// looked yet. The sequence's thread asks before each step: a reading
    /// comes in at any moment, the pass that acts on it up to 10 ms later,
    /// and a step pressed in between held the keys while the potion waited.
    bool potion_due(std::uint64_t now) const;
    /// How long a step waits on potion_due for the timer pass to take the
    /// potion up, at most: the pass comes every 10 ms, a key it is pressing
    /// holds it up to 60, and it looks at a kind only kGatePollMs after a
    /// drink of it.
    static constexpr std::uint64_t kPotionNoticeMs = 100;
    /// The potions looked at, and pressed if due; then potion_first. Before
    /// every other key of a pass: a reading may come in while a key is down.
    bool potions_first(const MacroConfig& cfg, std::uint64_t now);

    /// What the hunter is doing, from set_fight's source; no hunter without one.
    Fight fight_now() const;
    /// Whether the skills follow the fight now: switched on, and hunting.
    static bool following(const MacroConfig& cfg, const Fight& fight);
    /// The skill keys that follow the fight: their part of a pass, after
    /// the potions and the keys on their own timers -- which hold the thread
    /// while they are pressed, so the fight is asked about afresh here.
    void run_skills(const MacroConfig& cfg, std::uint64_t now);
    /// Why a skill that follows the fight -- or a step of the sequence -- is
    /// not to go just now: the gates they share.
    enum class SkillGate { Pass, NotFighting, LowMp, Dead };
    SkillGate skill_gate(int mp_floor_pct, const Fight& fight) const;
    void count_held(SkillGate g, std::uint64_t n);
    /// Once a minute while anything was pressed or held: one line of what the
    /// skills did. From the keys as they are now, which the tallies follow.
    void log_skills(std::uint64_t now);
    /// Once a minute while a potion key was pressed, or held for anything but
    /// its reading being above the mark: one line of what each potion key did
    /// -- presses, and the seconds spent in each reason, which add up to the
    /// minute. "Potions: HP key 1 (under 60%): 5 pressed; 48.3 s above the
    /// mark, 11.7 s cooling | MP key 2 (under 58%): ...".
    void log_potions(std::uint64_t now);
    /// Every key's first due time, from `now`.
    void arm(std::uint64_t now);
    /// recast_buffs, now, the character being in the world.
    bool apply_recast(const std::string& why);
    /// The sequence's thread: one turn after another, each followed by the
    /// wait it asks for, slept in slices so that a stop is quick.
    void sequence_loop_fn();
    /// One turn of the sequence at `now`: the step whose turn it is pressed,
    /// or held where it is -- a potion due first, a skill that follows the
    /// fight with no fight -- and how long until the next turn, in ms: the
    /// step's own pause after it once it has gone, the wait before the next
    /// look while it is held. The thread sleeps that; step() waits that much
    /// of the test's clock.
    std::uint32_t sequence_turn(std::uint64_t now);

    /// Why a press did not go out.
    enum class Refused { None, Suspended, Unfocused, Busy, Held, Failed };
    bool press(std::uint32_t keycode, std::uint32_t hold_ms, const char* owner,
               Refused* why = nullptr);
    /// A refusal that every key of the macro meets alike: a hold, the game
    /// not in front, the character out of the world.
    static bool refuses_all(Refused why) {
        return why == Refused::Held || why == Refused::Unfocused || why == Refused::Suspended;
    }
    /// Whether that is still so, asked without trying a press.
    bool still_refused(const MacroConfig& cfg, Refused why) const;

    /// One kind of potion's part of the timer pass, HP's or MP's: when it is
    /// next looked at -- kGatePollMs after a press, kPotionRetryMs after
    /// another job kept the keys -- why its last try did not go out, and
    /// whether one of its keys is due and has not gone out.
    struct PotionKind {
        std::uint64_t look_ms = 0;
        Refused       refused = Refused::None;
        bool          waiting = false;
    };
    std::uint32_t jittered(std::uint32_t base, int pct) const;
    std::uint64_t clock_now() const;
    /// Wait `ms` inside a pass: through the clock's seam, or a real sleep.
    void pause_ms(std::uint32_t ms);

    enum class Gate { Pass, Blocked, NoReading };

    /// Test a key's trigger against a reading of its bar, `pct` (-1: none).
    /// One reading for every key of a kind, so the tier chosen and the keys
    /// counted as held are judged on the same one.
    static Gate check_gate(const MacroKey& k, float pct);

    /// How long after a potion of one kind went out the next of that kind is
    /// looked for: GoC's "at most once in 100 ms after a press". Otherwise
    /// the gates are looked at on every pass: a potion key's due time is its
    /// cooldown, not its reaction time. Also the beat at which the held
    /// counters count, and a skill held by a gate is looked at again.
    static constexpr std::uint32_t kGatePollMs = 100;

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
    /// The sequence's turns: its thread's alone, or step()'s when stepped.
    /// When step() next takes a turn, on clock_now(); since when a potion
    /// found due has been waited for, by the step whose turn it is (0: none);
    /// and whether the round now under way was counted when the last step
    /// came round to the first, which may wait a while before it goes.
    std::uint64_t     seq_next_ms_ = 0;
    std::uint64_t     seq_noticed_ = 0;
    bool              seq_wrapped_ = false;
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

    /// Which key each of config_.keys is: an id that stays with the key when
    /// keys are added, taken out or moved (macro_key_match), and a new one for
    /// a key new to the list. Parallel to config_.keys, guarded by mutex_.
    ///
    /// A pass of the timers works from its own copy of the keys, taken as it
    /// begins, and the keys can change under it: set_config runs on the GUI's
    /// thread, and the timer thread holds no lock while a key is pressed. So
    /// every due time, press time and tally the pass reads or writes is found
    /// by this id (slot), never by where the key stood in its copy. By place,
    /// a skill taken out while another was being pressed had the pass read
    /// the HP potion's due time as a buff's -- the buff went out again at
    /// once -- and write the buff's 300-s interval onto the potion, which then
    /// did not drink for five minutes with HP low.
    std::vector<std::uint64_t> key_ids_;
    std::uint64_t              next_key_id_ = 1;
    /// The ids of the keys of the pass under way, in the order of its copy;
    /// the timer pass's alone.
    std::vector<std::uint64_t> pass_ids_;
    /// Where the pass's `i`-th key stands among config_.keys now -- and so in
    /// due_ms_ and pressed_ms_ -- or -1 once it has been taken out. Under
    /// mutex_.
    int slot(std::size_t i) const;
    /// One more press, in the tallies, of the pass's `i`-th key, wherever it
    /// stands now; none for a key taken out meanwhile.
    void count_press(std::size_t i);

    /// HP's and MP's potions; the timer pass's alone.
    PotionKind        potion_kind_[2];
    /// When the held counters of the potions next count: once a
    /// kGatePollMs, as when the gates were looked at only that often -- what
    /// the counters have always counted. The timer pass's alone.
    std::uint64_t     potion_count_ms_ = 0;
    /// When an HP potion's key last came up, for the gap before an MP one;
    /// the timer pass's alone. 0: none yet.
    std::uint64_t     hp_up_ms_ = 0;
    /// Since when a potion has been due without going out, on clock_now() --
    /// begun again while every key is refused alike; 0 when none is. Set as
    /// one is found due, before it is pressed, and cleared once none waits:
    /// the MP potion waits kPotionGapMs after the HP one, unpressed, and the
    /// sequence pressed a step in that gap. Written by the timer pass, read
    /// by the sequence too.
    std::atomic<std::uint64_t> potion_waiting_since_{0};
    /// potion_waiting_since_ was set at the start, before the potions were
    /// first looked at -- a potion under its mark at BẮT ĐẦU is due at once,
    /// and the sequence's first step went before it. The first look sets the
    /// time afresh, or clears it. The timer pass's, and arm()'s.
    bool              potion_since_provisional_ = false;

    std::function<Fight()> fight_;
    /// When the skills that follow the fight are next looked at -- the gap
    /// after the last one's key came up, a chance's beat, a retry; the timer
    /// pass's alone.
    std::uint64_t     skill_look_ms_ = 0;
    /// The minute's line: when it was last written, and the counts then.
    std::uint64_t     skill_log_ms_ = 0;
    std::uint64_t     skill_log_pressed_ = 0;
    Stats             skill_log_held_;

    mutable std::mutex stats_mutex_;
    Stats              stats_;

    // ---- the black box: under box_mutex_, which is taken with no other lock
    // of the engine's held, and holds none of them ----
    struct BoxVitals {
        std::uint64_t t = 0;
        int           hp = -1, mp = -1;     ///< rounded percent; -1 no reading
    };
    struct BoxDecision {
        std::uint64_t t = 0;
        std::uint64_t key = 0;              ///< which key (key_ids_)
        std::uint32_t keycode = 0;
        int           mark = 0;             ///< its threshold, percent
        bool          hp = true;            ///< an HP potion, or an MP one
        PotionWhy     why = PotionWhy::AboveMark;
        std::string   text;                 ///< for KeysHeld: what holds the game
    };
    /// A potion key's minute, for log_potions: its presses, and how long it
    /// spent in each reason, from its decisions as they change.
    struct PotionMinute {
        std::uint32_t keycode = 0;
        int           mark = 0;
        bool          hp = true;
        std::uint64_t presses = 0;
        std::uint64_t ms[static_cast<int>(PotionWhy::BehindHp) + 1] = {};
        PotionWhy     why = PotionWhy::AboveMark;     ///< the reason now, since `since`
        std::string   text;
        std::string   held_by;          ///< what last held the keys, for KeysHeld
        std::uint64_t since = 0;
        bool          active = false;   ///< pressed, or held for anything but above the mark, this minute
    };
    /// Records a vitals reading pushed at `t`, when it says something new.
    void box_vitals(std::uint64_t t, float hp_pct, float mp_pct);
    /// Records a potion key's decision at `t`: when its reason or its text
    /// changed since its last, and every press.
    void box_decide(std::uint64_t t, std::uint64_t key, const MacroKey& k, bool hp, PotionWhy why,
                    const std::string& text = {});
    void box_trim(std::uint64_t now);   ///< box_mutex_ held
    mutable std::mutex       box_mutex_;
    std::deque<BoxVitals>    box_vitals_;
    std::deque<BoxDecision>  box_decisions_;
    BoxVitals                box_last_vitals_;
    bool                     box_have_vitals_ = false;
    /// Each key's last decision recorded, and its minute so far.
    std::map<std::uint64_t, std::pair<PotionWhy, std::string>> box_last_;
    std::map<std::uint64_t, PotionMinute> potion_minute_;
    std::uint64_t            potion_log_ms_ = 0;
};

} // namespace gobot
