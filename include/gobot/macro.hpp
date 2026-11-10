#pragma once

#include "gobot/bar_spec.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/input_manager.hpp"

#include <array>
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

class IniFile;

/// What has to be true before a key fires.
enum class MacroTrigger {
    Always,     ///< a plain repeating timer
    HpBelow,    ///< only while HP is at or under the threshold
    MpBelow,
    PetHpBelow, ///< only while the pet's HP is at or under the threshold: "pet_hp_below"
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
    /// This key's own hold, in ms: at most MacroConfig::kHoldMostMs -- no key
    /// of the game is held down, every one is a press. 0, as almost every key
    /// has, is the config's key_hold_min_ms..key_hold_max_ms.
    std::uint32_t hold_ms = 0;
    std::uint32_t start_delay_ms = 0;   ///< stagger, so timers don't all align
    int           jitter_pct = 10;      ///< +/- this much on each interval

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

    /// A buff, whatever its interval: "Tự buff" on the simple screen, "a
    /// buff" on the full one. Cast on its own clock, fight or no fight, and
    /// again after a death, a revive or logging back in -- every interval_ms,
    /// 0 included, which is as often as a buff may go (macro_timer_interval_ms).
    /// Not set, as in a file written before it, a key on a timer of
    /// kBuffMinIntervalMs or more is a buff and a shorter one a skill.
    bool buff = false;
};

/// What a key is for, worked out from how it is set up.
///
/// A potion is a key with a trigger on it, and a buff a key marked as one
/// (MacroKey::buff) or, unmarked, on a long timer. The simple screen groups
/// them by this, so a player sees "HP potion, key 1" rather than a table of
/// nine columns.
enum class MacroKeyKind {
    Skill,      ///< a short timer: fired over and over while fighting
    Buff,       ///< on its own clock, fight or no fight, and again after a death
    HpPotion,   ///< drinks when HP drops
    MpPotion,   ///< drinks when MP drops
    PetPotion,  ///< heals the pet when the pet's HP drops
};

/// A key on a timer that is not marked as a buff is one at this interval or
/// longer, and a skill under it. A minute sits well clear of both: skill
/// cooldowns here run to seconds, buffs to minutes -- but for the buffs a
/// player marks, which may be anything (MacroKey::buff).
constexpr std::uint32_t kBuffMinIntervalMs = 60'000;

MacroKeyKind macro_key_kind(const MacroKey& k);

/// A buff's interval as the simple screen takes it, in seconds: 0 to two
/// hours. Its keys are marked as buffs, so a short one stays in its row; at
/// 0 it goes as often as a buff may (kBuffRepeatMinMs).
constexpr int kBuffMinSeconds = 0;
constexpr int kBuffMaxSeconds = 7200;
int clamp_buff_seconds(int seconds);

/// No buff is cast again sooner than this after its last cast, whatever its
/// interval: one at 0 s goes every 5 s. GoC's rule -- a buff is queued once
/// its cooldown is over and 5 s have passed since it last was -- which the
/// player chose for a buff at 0 (2026-10-27).
constexpr std::uint32_t kBuffRepeatMinMs = 5'000;

/// How long after its press a key on a timer is due again: its interval,
/// and for a buff never under kBuffRepeatMinMs.
std::uint32_t macro_timer_interval_ms(const MacroKey& k);

/// Whether a key on a timer (no trigger) is pressed at all: on, with a key,
/// and an interval above 0 -- or a buff, which at 0 goes as often as it may.
bool macro_timer_on(const MacroKey& k);

/// Whether the timers press `k` as a buff: on (macro_timer_on) and a buff by
/// its kind. A buff row switched off is none, whatever its interval: its key
/// may be a skill's now (2026-11-08: two buff rows left off on A and S, which
/// the owner had made attack skills, had A and S logged as buffs all run).
bool macro_buff_on(const MacroKey& k);

/// The keycodes of the buff rows the timers press (macro_buff_on), in the
/// order of the keys: what each press of theirs should cast is a buff. For the
/// log of your casts (CastMatch); nothing presses by it.
std::vector<std::uint32_t> macro_buff_keycodes(const std::vector<MacroKey>& keys);

/// The keycodes on a buff row the timers press (macro_buff_on) and on a skill
/// row they press (macro_timer_on, kind Skill) at once, each once, in the
/// order of the keys. Both clocks press such a key, and its presses are logged
/// as a buff's (2026-11-08 22:13-22:15: A and S both buffs and attack skills,
/// the buffs' four presses each in among the skills). Warned of, never
/// refused: the window says so (macro_clash_note_vi), and the log once a run
/// (macro_clash_line) -- only while "Repeating keys" (timers_enabled) is on,
/// as with it off no row is pressed at all.
std::vector<std::uint32_t> macro_buff_skill_clashes(const std::vector<MacroKey>& keys);
/// The window's words for it, in Vietnamese; "" with no such key.
std::string macro_clash_note_vi(const std::vector<MacroKey>& keys);
/// The log's, at MACRO ON; "" with no such key.
std::string macro_clash_line(const std::vector<MacroKey>& keys);

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

/// A potion key as the simple screen makes one: tier 1 or 2 of the HP, MP or
/// pet row, on, drinking at most every 3 s. Lv1 drinks under 50% HP or 30% MP
/// -- what the screen has always put back when a row had no key -- or 50% of
/// the pet's HP, and Lv2, the second potion, lower: 30% HP, 20% MP, 30% of the
/// pet's, labelled "HP potion 2", "MP potion 2" and "Pet potion 2". Anything
/// but a potion kind gives a key with no keycode.
MacroKey macro_potion_key(MacroKeyKind kind, std::uint32_t keycode, int tier);

/// Take out the keys the simple screen has emptied: a skill or buff whose box
/// was cleared, and a potion row's Lv2 or later likewise. The HP and MP rows'
/// Lv1 stays, key or none -- the row is drawn from it. The pet's row keeps no
/// key without one, Lv1 or not: it is drawn without one until it is given one.
/// True if anything went.
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
    std::uint32_t hold_ms = 0;          ///< as MacroKey::hold_ms
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

    /// GoC's "Dùng skill tuần tự". Only while the skills follow the fight --
    /// skills_follow_fight with the hunter running (run_skills); the macro
    /// alone keeps every key on its own timer, and this does nothing there
    /// (2026-11-08: ticked, with no hunter, all run). On: every skill whose
    /// cooldown is over goes, in the order of the keys, skill_gap_ms after
    /// the one before. Off: only the first of them, one chance every 300 ms.
    bool sequential_skills = true;

    /// The skills on their timers paced by your own casts in the packets
    /// (2026.11.08, W1): one round in the order of the keys, each skill once,
    /// the next pressed only once the game has cast the one before -- its
    /// 0x0031 seen -- and its lock is over; a key's interval_ms is then only
    /// the least time from its last cast. Off, and in a file without it: every
    /// key on its own timer, as before. Only with the skills on their timers
    /// (not following the fight) and nothing else pressing keys -- no hunter
    /// running, no key sequence; potions go first as ever, and a buff due
    /// while a skill's cast is waited for, or inside its lock, goes once that
    /// lock is over. See MacroEngine::note_own_cast and run_paced.
    bool cast_paced = false;

    /// Under this much MP, read just now, no skill goes when skills follow
    /// the fight: the hunter's own swing carries on, which costs none. GoC
    /// fixes it at 10. 0 is never; at most kSkillMaxMpFloor.
    int skill_mp_floor_pct = 10;
    static constexpr int kSkillMaxMpFloor = 50;

    /// The gap after one skill's key comes up before the next skill's goes
    /// down: in a burst of the skills that follow the fight, and between skills
    /// on their own timers that come due together. GoC's is 50 ms. The simple
    /// screen's "Tốc độ bấm kỹ năng" sets it: kSkillGapSteps; the file takes
    /// anything from kSkillGapMinMs to kSkillGapMaxMs.
    std::uint32_t skill_gap_ms = 50;
    static constexpr std::uint32_t kSkillGapMinMs = 30;
    static constexpr std::uint32_t kSkillGapMaxMs = 1000;
    static constexpr std::uint32_t kSkillGapSteps[] = { 50, 60, 80, 100, 150 };

    /// How long a key of the macro is held down: a random time between these,
    /// for a key with no hold of its own. 0 for both: the input manager's own
    /// 25-60 ms, which is what a config built in code gets. A game that reads
    /// its keyboard once a frame misses a press shorter than a frame now and
    /// then -- the key goes "down and up" and nothing happens -- so a file
    /// that does not say gets kDefaultHoldMinMs to kDefaultHoldMaxMs.
    /// kHoldMostMs is also the most a key's or a step's own hold_ms is taken
    /// at, from the file and on the full screen: a key's hold keeps the timer
    /// thread, and a potion due waits it out.
    std::uint32_t key_hold_min_ms = 0;
    std::uint32_t key_hold_max_ms = 0;
    static constexpr std::uint32_t kHoldLeastMs = 20;
    static constexpr std::uint32_t kHoldMostMs = 500;
    static constexpr std::uint32_t kDefaultHoldMinMs = 70;
    static constexpr std::uint32_t kDefaultHoldMaxMs = 110;

    /// A potion whose bar has not risen this long after its press, while it
    /// still wants to drink, is pressed again at once -- not after the rest of
    /// its cooldown: the game may have ignored the press. Once per press. 0:
    /// never, the plain cooldown, which is what a config built in code gets; a
    /// file that does not say gets kDefaultRecheckMs. A potion shows on the
    /// bar a moment after it is drunk (about half a second on the live game),
    /// so the least is kRecheckLeastMs.
    std::uint32_t potion_recheck_ms = 0;
    static constexpr std::uint32_t kRecheckLeastMs = 500;
    static constexpr std::uint32_t kRecheckMostMs = 3000;
    static constexpr std::uint32_t kDefaultRecheckMs = 1000;
    /// How much the bar has to have risen, in percent of the bar, to count.
    static constexpr float kRecheckRisePct = 2.0f;

    /// A potion whose bar is under half of its mark -- the character in
    /// trouble: skills that drain MP, or blows that take HP, faster than one
    /// potion in interval_ms makes up -- may drink again this long after its
    /// last press, instead of after the interval. Between the two marks it
    /// is still the interval. 0, which is what a config built in code gets:
    /// never, the plain cooldown; a file that does not say gets
    /// kDefaultUrgentMs. A press the game refuses for its own cooldown costs
    /// no potion, as GoC, which presses every 100 ms while low, shows.
    std::uint32_t potion_urgent_ms = 0;
    static constexpr std::uint32_t kUrgentLeastMs = 500;
    static constexpr std::uint32_t kUrgentMostMs = 5000;
    static constexpr std::uint32_t kDefaultUrgentMs = 1000;

    /// A buff is cast by pressing its key this many times, buff_press_gap_ms
    /// apart. GoC presses each buff 4 times, 200 ms apart -- its docs say it
    /// counts the cast only once the server confirms it, but its client writes
    /// that confirmation where nothing reads it (2026-11-08) -- since a single
    /// press is lost by the game now and then, and a lost buff is not tried
    /// again for a whole interval.
    /// 1, which is what a config built in code gets, is the single press; a
    /// file that does not say gets kDefaultBuffPresses.
    std::uint32_t buff_presses = 1;
    std::uint32_t buff_press_gap_ms = 200;
    static constexpr std::uint32_t kBuffPressesMost = 6;
    static constexpr std::uint32_t kBuffPressGapLeastMs = 100;
    static constexpr std::uint32_t kBuffPressGapMostMs = 500;
    static constexpr std::uint32_t kDefaultBuffPresses = 4;
    static constexpr std::uint32_t kDefaultBuffPressGapMs = 200;

    /// Only send input while the target window has focus. Off means keys go
    /// to whatever is in front, which will eventually be something you did not
    /// intend.
    bool require_focus = true;

    /// Bring the game to the front when the macro is switched on.
    ///
    /// Without this, pressing START MACRO leaves the game behind the GoBot
    /// window and the macro sits there holding, which reads as "it does not
    /// work". Only on the way on. Keeping it in front while running is a
    /// separate switch, refocus in [Simple] in app.ini
    /// (GuiApp::keep_game_in_front, FrontKeeper), on by default.
    bool focus_on_start = true;

    /// Applies on top of each key's own jitter.
    int global_jitter_pct = 0;

    std::uint32_t toggle_key = 121;     ///< F10
    std::uint32_t panic_key = 123;      ///< F12 — stop and release everything

    /// Where HP and MP are drawn on screen. Kept with the macro rather than in
    /// a config of their own: they exist to make the conditional keys above
    /// work, and one Save should cover a key and its trigger together. The
    /// pet's bar is not kept: the reader finds it on the HUD by itself.
    bool    vitals_enabled = false;
    BarSpec hp_bar;
    BarSpec mp_bar;

    /// True when at least one key actually depends on a reading.
    bool needs_vitals() const;

    /// Put the bars where they usually are, as a starting point to nudge.
    void seed_vitals_defaults();

    bool load(const std::string& path);
    bool save(const std::string& path) const;
    /// What save() writes, put into `ini`: two configs compared without a
    /// file (settings_changes).
    void store(IniFile& ini) const;
    void apply_defaults();
};

/// What changed from `was` to `now` -- configs as their files hold them
/// (MacroConfig::store, HunterConfig::store) -- on one line for the log:
/// "Keys.key_9_keycode 72 (H) -> 70 (F); Hunt.enabled true -> false", a key's
/// code with its name. The first `most` changes, then how many more; "" when
/// nothing changed.
std::string settings_changes(const IniFile& was, const IniFile& now, std::size_t most = 12);

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
    ///
    /// `pet_pct` is the pet's HP likewise (VitalsReader::pet_percent), -1
    /// when there is no reading of it -- as from a caller that reads none:
    /// the pet's potions then hold, as HP's do with no HP read.
    void set_vitals(float hp_pct, float mp_pct, float pet_pct = -1.0f);
    static constexpr float kDownHpPct = 0.5f;
    /// A reading older than this is treated as absent: pushed this long ago,
    /// or -- by whoever pushes it, with VitalsReader::if_fresh -- read off
    /// the bars this long ago.
    static constexpr std::uint64_t kVitalsStaleMs = 1500;
    /// HP unread this long is the character out of the world, and the macro
    /// is held (VitalsReader::in_world). One unreadable frame is not: they
    /// come every few seconds off the screen, and every other read as a
    /// character dies -- 8 holds and let-gos in 3 s on 2026-10-26, each one
    /// a skill lost. About six reads.
    static constexpr std::uint64_t kOutOfWorldAfterMs = 1000;
    static_assert(kOutOfWorldAfterMs <= kVitalsStaleMs,
                  "never in the world on a reading the macro would not act on");

    /// What the engine is currently acting on: the last pushed values, or -1
    /// each once they have gone stale.
    void current_vitals(float& hp_pct, float& mp_pct) const;
    void current_vitals(float& hp_pct, float& mp_pct, float& pet_pct) const;

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
    ///   kPotionGapMs after the HP key came up, then the pet's no sooner than
    ///   kPotionGapMs after the later of those two -- never while the HP or
    ///   the MP potion waits for the keys: the character's own come first --
    ///   and all of them before any other key due in the same pass.
    /// - While a potion is due and has not gone out, no other key of the
    ///   macro is started -- for at most kPotionFirstMs of time in which it
    ///   could have gone, so a potion that cannot be pressed does not starve
    ///   the rest. A hold, the game not in front or the character out of the
    ///   world refuse every key alike, and do not count. The sequence, on a
    ///   thread of its own, waits too: from BẮT ĐẦU until the potions have
    ///   first been looked at, from the moment one is found due until the HP
    ///   potion, then the MP one, then the pet's have gone, and in the moment
    ///   between a reading and the pass that acts on it. And at BẮT ĐẦU, every
    ///   key waits for HP, MP and the pet's HP read afresh -- each that had a
    ///   reading before -- kStartReadMs at most (start_read_until_).
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
    /// Between the keys of two potions, from one coming up to the next going
    /// down: a game that reads its keyboard once a frame gets them in two
    /// frames. (20 ms before: the second was lost now and then.)
    static constexpr std::uint32_t kPotionGapMs = 60;
    static constexpr std::uint64_t kPotionFirstMs = 300;
    /// How long a start waits for HP and MP read afresh (start_read_until_):
    /// the read the start woke, whose grab can take 60 ms and more in a
    /// VMware guest, and the GUI's next frame to hand it over.
    static constexpr std::uint64_t kStartReadMs = 500;

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

    /// Told of every key the macro pressed that went out -- the key, and the
    /// time on the engine's clock it went down -- from the macro's own
    /// threads, once the press is over: what the game's packets say came of
    /// it, which can come while the key is still held, is weighed
    /// against it (a potion's bag frame, a skill's cast). Never told of a
    /// press refused (held, out of the world, the game not in front, another
    /// key going). Whatever it is answers at once and presses nothing. Before
    /// start(), never while it runs; unset is nobody told.
    void set_on_press(std::function<void(std::uint32_t vk, std::uint64_t at_ms)> f) { on_press_ = std::move(f); }

    /// With sequential_skills off, and the skills following the fight: a
    /// skill at most this often -- the beat GoC's docs give its attack thread,
    /// which its client never starts (2026-11-08).
    static constexpr std::uint32_t kSkillChanceMs = 300;
    /// A skill whose press did not go out is tried again this soon, still
    /// due: its cooldown starts only from a press that went out.
    static constexpr std::uint32_t kSkillRetryMs = 30;

    // ---- the cast-paced rotation (MacroConfig::cast_paced, 2026.11.08) ----
    //
    // On 2026-11-08 the timers sent a skill key down about every 147 ms, some
    // 390 a minute, against some 77 casts: the game drops a key that goes down
    // while the character is still in the last cast's lock, so whichever key
    // came first after it was cast, and the order was a lottery. Paced, a
    // skill goes only once the game has cast the one before -- your own 0x0031
    // in the packets -- and that cast's lock is over: one round in the order
    // of the keys, each skill once, a key's interval_ms only the least time
    // from its last cast. One press waits for its cast at a time: no cast, the
    // same key once more; none again, it is skipped this round -- a cooldown,
    // learned so that it is passed over without a press next time. With no
    // cast of yours for kPaceStaleMs while keys go out, the timers take over
    // until the next one. Only with the skills on their timers and the macro's
    // timers pressing alone -- no hunter, no key sequence: their casts cannot
    // be told from the rotation's. The potions first as ever; a buff due while
    // a skill's cast is waited for, or inside its lock, goes once that lock is
    // over, and the next skill waits for the buff's cast.

    /// One cast of yours: your client's own 0x0031 over UDP under your id --
    /// its skill, and when it came: GamePacket::timestamp_ms, on now_ms()'s
    /// clock as clock_now() is (a test's own, on the test's). From the capture
    /// thread: atomics only, no lock of the engine's -- a ring of kCastRing,
    /// each slot a sequence lock. The timer pass reads it; nothing else of the
    /// engine acts on it, and with cast_paced off nothing at all.
    void note_own_cast(std::uint32_t skill, std::uint64_t at_ms);
    static constexpr std::size_t kCastRing = 16;

    /// The lock: from your cast's stamp to the first key-down the game takes
    /// again. Guessed at first, and never outside the least and the most.
    /// Learned: kPaceLockUpMs more when a first press was dropped and its
    /// re-press cast, once kPaceDropsToRaise of the last kPaceDropWindow first
    /// presses at the guess were (one alone -- a stun, a frame hitch -- is not
    /// the lock), and unless the key dropped one or was skipped in its last
    /// kPaceSuspectTurns turns, its cooldown more likely than the lock, and
    /// the key before it cast on its first; kPaceLockDownMs less after
    /// kPaceLockDownAfter first presses cast, each pressed within
    /// kPaceAtLockMs of the guess -- counted afresh at arm(), after each step
    /// down and after a re-press that was cast; a skip, a first press cast
    /// late and a spell on the timers neither count nor reset it.
    static constexpr std::uint32_t kPaceLockStartMs = 700;
    static constexpr std::uint32_t kPaceLockMinMs = 550;
    static constexpr std::uint32_t kPaceLockMaxMs = 900;
    static constexpr std::uint32_t kPaceLockUpMs = 20;
    static constexpr std::uint32_t kPaceLockDownMs = 5;
    static constexpr int           kPaceLockDownAfter = 20;
    static constexpr std::uint32_t kPaceAtLockMs = 30;
    static constexpr int           kPaceSuspectTurns = 3;
    static constexpr int           kPaceDropWindow = 10;
    static constexpr int           kPaceDropsToRaise = 2;
    /// A press's cast is one stamped no sooner than kPaceEarlyMs before its
    /// key-down, waited for kPaceConfirmMs: its latency, 27-87 ms, and the
    /// capture's batch, up to 70 more. None by kPaceRepressMs after the key
    /// went down -- and skill_gap_ms after it came up -- the same key once
    /// more: a cast sooner than that is mostly on its way still.
    static constexpr std::uint64_t kPaceEarlyMs = 5;
    static constexpr std::uint64_t kPaceConfirmMs = 220;
    static constexpr std::uint64_t kPaceRepressMs = 160;
    /// No cast of yours this long from the first key that went out after the
    /// last one: the timers, until the next cast.
    static constexpr std::uint64_t kPaceStaleMs = 2000;
    /// A key skipped -- no cast from its two presses -- has a cooldown longer
    /// than the time since its last cast: taken, as the least of it, once
    /// another key casts within kPaceFloorTakenMs (the game was casting, so it
    /// was the key's own) -- on the key's second such skip within kPaceSeenMs:
    /// one stop of the game's (a stun, a hitch, the capture late) skips every
    /// key in it, cooldown or none. A key with a floor raises it at once.
    /// Passed over while under it, kPaceFloorUses times; then pressed once at
    /// its turn, a check: a cast under it drops it.
    static constexpr std::uint64_t kPaceFloorTakenMs = 1500;
    static constexpr int           kPaceFloorUses = 20;
    static constexpr std::uint64_t kPaceSeenMs = 60'000;

    /// What the rotation is doing, for the GUI: a copy, from any thread.
    struct PaceStatus {
        enum class Mode {
            Off,        ///< cast_paced off, the skills follow the fight, or the hunter or the sequence presses keys too: every key as before
            Paced,      ///< the rotation, on your casts
            Fallback,   ///< no cast of yours yet, or none for kPaceStaleMs while keys went out: the timers
        };
        /// How a key's turn in a round went.
        enum class Turn {
            Cast,           ///< its press was cast
            CastOnRepress,  ///< its first press was dropped and the second cast
            NoCast,         ///< no cast from its presses: skipped this round
            Cooling,        ///< passed over without a press: inside the cooldown learned for it
            NotDue,         ///< passed over without a press: its interval_ms not over since its last cast
        };
        struct Step {
            std::uint32_t keycode = 0;
            Turn          turn = Turn::Cast;
        };
        Mode              mode = Mode::Off;
        std::uint64_t     since_ms = 0;         ///< when it came to this mode, on clock_now()
        std::uint32_t     lock_ms = kPaceLockStartMs;   ///< the lock as now learned
        std::uint64_t     rounds = 0;           ///< whole rounds since the macro started
        std::uint64_t     last_cast_ms = 0;     ///< your last cast, on clock_now(); 0 for none
        std::vector<Step> last_round;           ///< the last whole round, in order
        bool              others = false;       ///< Off because the hunter or the sequence presses keys too
    };
    PaceStatus pace_status() const;
    /// "cast", "cast on the re-press", "no cast", "cooling", "not due".
    static const char* pace_turn_name(PaceStatus::Turn turn);
    /// The status in a few words for GoBot's window, in Vietnamese: "Đang
    /// theo vòng -- khóa chiêu ước 700 ms, 12 vòng.", "Tạm theo giờ vì không
    /// thấy gói tin ra chiêu ...". Off: what an Off with the switch on means,
    /// the skills following the fight, or the hunter or the sequence pressing
    /// keys too (PaceStatus::others).
    static std::string pace_status_vi(const PaceStatus& s);

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
    /// having lost focus during it, and as the macro is held for the
    /// character out of the world (set_suspended). With `key` set and `held`
    /// not, no press is taken for one of those.
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
        /// The pet's potions held for no reading of the pet's HP, apart from
        /// HP's and MP's: no pet out is no bar that needs setting up.
        std::uint64_t held_no_pet_reading = 0;

        /// A potion whose condition held, and whose cooldown was over, kept
        /// from drinking because another of its kind sits lower and is low
        /// enough too: only the more urgent tier drinks.
        std::uint64_t held_tier = 0;

        /// Presses held because the character was not in the world.
        std::uint64_t held_suspended = 0;

        /// Potions pressed again early because the bar had not risen
        /// (MacroConfig::potion_recheck_ms), and the checks made.
        std::uint64_t potion_repressed = 0;
        std::uint64_t potion_checks = 0;

        /// Skills that follow the fight, held back while ready: no fight, MP
        /// under the floor, the character down, or the target further than
        /// the skill reaches. One for each ready key at each look, a look
        /// every 100 ms -- the sequence's steps at each of theirs, 50 ms.
        std::uint64_t held_not_fighting = 0;
        std::uint64_t held_low_mp = 0;
        std::uint64_t held_dead = 0;
        std::uint64_t held_range = 0;

        /// Skill keys refused because the game was not in front
        /// (require_focus), on their timers or following the fight: of
        /// skipped_unfocused, which counts every key's, for the Skills line.
        std::uint64_t skills_unfocused = 0;

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
        BehindHp,       ///< an MP or pet potion waiting while the HP potion waits for the keys
        BehindMp,       ///< a pet potion waiting while the MP potion waits for the keys
    };
    /// "pressed", "above the mark", "no reading", "cooling", "other tier",
    /// "game not in front", "not in the world", "keys held", "keys busy",
    /// "key failed", "behind the HP potion", "behind the MP potion".
    static const char* potion_why_name(PotionWhy why);

    /// The black box, for the log right after "ALERT death": the last
    /// kBlackBoxMs up to now on the engine's clock, in at most kBlackBoxLines
    /// lines of at most kBlackBoxLineChars characters, times in seconds before
    /// now. The first line is HP and MP as read -- and the pet's HP, "HP/MP/
    /// pet", once a reading of it or a decision of a pet potion is in the box
    /// -- every change of the last 5 s, and before that the lowest HP of each
    /// 2 s; then a line for each potion key, HP's first, then MP's, then the
    /// pet's, of its decisions -- each run of one reason with how long it
    /// lasted, every press marked; and a last line of the presses of each. A
    /// key by its name and mark only, never a label the player typed. Empty
    /// when nothing was recorded. From any thread.
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

    /// The cast-paced rotation's part of a pass, before the keys on their
    /// timers: on or off by the config and the fight -- one line each time it
    /// changes -- and your casts since the last pass taken in. `others`: the
    /// hunter or the key sequence presses keys too, whose casts cannot be told
    /// from the rotation's -- off. Whether the skills are the rotation's this
    /// pass (run_paced), not the timers'.
    bool pace_pass(const MacroConfig& cfg, bool follow, bool others, std::uint64_t now);
    /// The casts note_own_cast has put in the ring since the last look, oldest
    /// first, each to pace_cast.
    void pace_take_casts(std::uint64_t now);
    /// One cast of yours at `at_ms`: the lock's start; the press waiting for
    /// it confirmed, or the timers given back to the rotation.
    void pace_cast(std::uint64_t at_ms, std::uint32_t skill, std::uint64_t now);
    /// The skills paced: the press waiting for its cast seen to -- pressed
    /// again, or skipped -- then, the lock over, the next key of the round.
    /// After the potions and the keys on their timers.
    void run_paced(const MacroConfig& cfg, std::uint64_t now);
    /// A press of the rotation's that went out, of the pass's `i`-th key,
    /// its key down at `at`: the gap after it, its timer armed as the timers
    /// would have, its tally.
    void pace_went(const MacroConfig& cfg, std::size_t i, std::uint64_t at);
    /// The press waiting for its cast gone with no cast from it: the key
    /// skipped this round, its cooldown's least learned once another casts.
    void pace_skip();
    /// The cursor past the press's key; a round ended there.
    void pace_advance();
    /// A key's turn in the round under way, and the round's end.
    void pace_step(std::uint32_t keycode, PaceStatus::Turn turn);
    void pace_round_end();
    /// Into `mode` at `now`, the status to match.
    void pace_set_mode(PaceStatus::Mode mode, std::uint64_t now);
    void pace_publish();
    /// Once a minute while cast_paced is on: "Paced: ...".
    void log_paced(std::uint64_t now);
    /// A run's start: the rotation from its first key, off until its first
    /// pass, the casts that came meanwhile taken as the lock's start only.
    /// What was learned -- the lock, each key's last cast and cooldown -- is
    /// the game's, and stays.
    void pace_arm(std::uint64_t now);
    /// Once a minute while a potion key was pressed, or held for anything but
    /// its reading being above the mark: one line of what each potion key did
    /// -- presses, and the seconds spent in each reason, which add up to the
    /// minute. "Potions: HP key 1 (under 60%): 5 pressed; 48.3 s above the
    /// mark, 11.7 s cooling | MP key 2 (under 58%): ... | Pet key 4 (under
    /// 50%): ...".
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
    /// A press of the cast-paced rotation's refused -- held, busy, the game
    /// not in front, out of the world: no try; looked at again kGatePollMs on.
    void pace_refused(Refused why, std::uint64_t now);

    /// One kind of potion's part of the timer pass, HP's, MP's or the pet's:
    /// when it is next looked at -- kGatePollMs after a press, kPotionRetryMs
    /// after another job kept the keys -- why its last try did not go out,
    /// and whether one of its keys is due and has not gone out.
    struct PotionKind {
        std::uint64_t look_ms = 0;
        Refused       refused = Refused::None;
        bool          waiting = false;
        /// The last press that went out, to see whether the bar rose
        /// (MacroConfig::potion_recheck_ms): when, the reading it was
        /// pressed at, the key, and whether it is itself the early press
        /// that a check asked for -- which is not checked again.
        std::uint64_t check_at = 0;
        float         check_before = 0.0f;
        std::uint32_t check_keycode = 0;
        bool          check_is_repress = false;
        bool          repress_next = false;   ///< the next press of this kind is the early one
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
    std::atomic<float>         vit_pet_{-1.0f};
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

    /// The presses of a buff's cast still to come after the first
    /// (MacroConfig::buff_presses): the key, its hold, how many are left, when
    /// the next is due and how far apart, and when they are given up on.
    /// Guarded by mutex_.
    struct BuffRepeat {
        std::uint32_t keycode = 0;
        std::uint32_t hold_ms = 0;
        std::uint32_t left = 0;
        std::uint32_t gap_ms = 0;
        std::uint64_t at = 0;
        std::uint64_t until = 0;
    };
    std::vector<BuffRepeat> buff_repeats_;
    /// The buff key whose refusal the log last said, so that a buff refused
    /// every 2 s for minutes is said once. 0 for none; the timer pass's.
    std::uint32_t buff_refused_said_ = 0;
    /// One due repeat of a buff's cast, if any is: at most one a pass.
    void run_buff_repeats(std::uint64_t now);
    /// Whether the potion key `k`, in place `s` of the due times, may drink at
    /// `now` with its bar at `v`: its cooldown is over, or the bar is under
    /// half its mark and urgent_ms (MacroConfig::potion_urgent_ms) have passed
    /// since its last press. With mutex_ held.
    bool potion_cooled(const MacroKey& k, int s, float v, std::uint64_t now, std::uint32_t urgent_ms) const;
    /// Buffs go out no closer than this: kBuffGapMs, or the whole of a cast
    /// and a little over, if that is longer.
    static std::uint64_t buff_spacing(const MacroConfig& cfg);

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

    /// The kinds of potion, in the order a pass looks at them: HP's, MP's,
    /// the pet's. potion_kind_ is in that order too.
    static constexpr MacroKeyKind kPotionKinds[3] = { MacroKeyKind::HpPotion, MacroKeyKind::MpPotion,
                                                      MacroKeyKind::PetPotion };
    /// HP's, MP's and the pet's potions; the timer pass's alone.
    PotionKind        potion_kind_[3];
    /// When the held counters of the potions next count: once a
    /// kGatePollMs, as when the gates were looked at only that often -- what
    /// the counters have always counted. The timer pass's alone.
    std::uint64_t     potion_count_ms_ = 0;
    /// When an HP potion's key last came up, for the gap before an MP one --
    /// and with mp_up_ms_, an MP one's, before a pet one; the timer pass's
    /// alone. 0: none yet.
    std::uint64_t     hp_up_ms_ = 0;
    std::uint64_t     mp_up_ms_ = 0;
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
    /// Until when the start waits for HP, MP and the pet's HP read afresh:
    /// each that had a reading pushed before the start, which arm() drops
    /// (start_unread_). The first potion, skill, buff or step waits for the
    /// read the start woke, kStartReadMs at most (potion_first); 0 once all
    /// are read. Never moved on: a potion that goes meanwhile ends no wait,
    /// and holds the rest no later than kStartReadMs from the start either.
    /// Written by arm() and the timer pass, read by the sequence too.
    std::atomic<std::uint64_t> start_read_until_{0};
    /// HP's, MP's and the pet's: had a reading before the start and has had
    /// none since. arm()'s, then the timer pass's alone.
    bool              start_unread_[3] = { false, false, false };

    std::function<Fight()> fight_;
    std::function<void(std::uint32_t, std::uint64_t)> on_press_;   ///< set_on_press
    /// When the skills that follow the fight are next looked at -- the gap
    /// after the last one's key came up, a chance's beat, a retry; the timer
    /// pass's alone.
    std::uint64_t     skill_look_ms_ = 0;
    /// Before this no skill on its own timer goes down: skill_gap_ms after the
    /// last such skill's key came up.
    std::uint64_t     skill_free_ms_ = 0;
    /// The minute's line: when it was last written, and the counts then.
    std::uint64_t     skill_log_ms_ = 0;
    std::uint64_t     skill_log_pressed_ = 0;
    Stats             skill_log_held_;

    // ---- the cast-paced rotation: the timer pass's alone (arm()'s at a
    // start), but for the ring, which note_own_cast writes from the capture
    // thread, and the status, under pace_mutex_ ----

    /// One slot of the ring: `version` odd while note_own_cast writes it;
    /// `n` is which cast it holds, counted from 0.
    struct CastSlot {
        std::atomic<std::uint64_t> version{ 0 };
        std::atomic<std::uint64_t> n{ 0 };
        std::atomic<std::uint64_t> at_ms{ 0 };
        std::atomic<std::uint32_t> skill{ 0 };
    };
    std::array<CastSlot, kCastRing> cast_ring_{};
    std::atomic<std::uint64_t>      casts_noted_{ 0 };   ///< casts ever written to the ring
    std::uint64_t                   casts_read_ = 0;     ///< of them, those the timer pass has taken

    /// A key of the rotation's, found by which key it is (key_ids_).
    struct PaceKey {
        std::uint64_t id = 0;
        std::uint32_t keycode = 0;          ///< as it was last looked at, for the log
        std::uint64_t last_cast_ms = 0;     ///< its last cast confirmed; 0 for none
        std::uint32_t skill = 0;            ///< ... and that cast's skill
        std::uint64_t floor_ms = 0;         ///< its cooldown is longer than this, as learned; 0 none
        int           floor_uses = 0;       ///< turns passed over by it since it was learned or checked
        std::uint64_t candidate_ms = 0;     ///< a floor from its last skip, until another key casts ...
        std::uint64_t candidate_at = 0;     ///< ... from that skip's last key-down
        std::uint64_t seen_ms = 0;          ///< a floor seen once and not yet taken; a second within kPaceSeenMs takes it; 0 none
        std::uint64_t seen_at = 0;          ///< ... when it was seen
        std::uint8_t  history = 0;          ///< its last kPaceSuspectTurns turns: 1 for a first press dropped or a skip
        std::uint64_t skipped = 0;          ///< this minute: turns with no cast, learned or pressed
        std::uint64_t skipped_learned = 0;  ///< ... of them, passed over without a press
        bool          late = false;         ///< skipped on a turn, and no first press cast since: its re-press waits out the longest lock
    };
    std::vector<PaceKey> pace_keys_;
    PaceKey& pace_key(std::uint64_t id);
    /// The press waiting for its cast: which key, where in the rotation of
    /// `n` keys, its key-down and key-up, the re-press's key-down (0 before
    /// it), whether it went at the lock's guessed end, and whether it is a
    /// floor's check, pressed once only.
    struct PacePress {
        bool          active = false;
        std::uint64_t id = 0;
        std::uint32_t keycode = 0;
        std::size_t   pos = 0;
        std::size_t   n = 0;
        std::uint64_t down_ms = 0;
        std::uint64_t up_ms = 0;
        std::uint64_t again_ms = 0;
        bool          at_lock = false;
        bool          check = false;
    };
    PacePress        pace_press_;
    PaceStatus::Mode pace_mode_ = PaceStatus::Mode::Off;
    std::uint64_t    pace_mode_ms_ = 0;         ///< since when in it
    std::size_t      pace_cursor_ = 0;          ///< the place in the rotation looked at next
    std::uint64_t    pace_cursor_id_ = 0;       ///< the key there, so a list changed keeps its place; 0 unknown
    std::uint32_t    pace_lock_ms_ = kPaceLockStartMs;
    int              pace_clean_ = 0;           ///< first presses cast at the lock's guessed end since arm(), the last step down or the last re-press cast; skips and late ones leave it
    std::uint16_t    pace_drops_ = 0;           ///< the last kPaceDropWindow first presses at the guess: 1 dropped, cast on its re-press
    bool             pace_others_ = false;      ///< cast_paced on, but the hunter or the sequence presses keys too: off
    bool             pace_prev_dropped_ = false;    ///< the last key confirmed dropped its first press
    std::uint64_t    pace_prev_id_ = 0;         ///< ... which key it was
    std::uint64_t    pace_last_cast_ms_ = 0;    ///< your last cast, any; the lock counts from it
    std::uint64_t    pace_unanswered_ms_ = 0;   ///< the first key-down since your last cast; 0 none
    std::uint64_t    pace_unanswered_n_ = 0;    ///< presses out since then
    std::uint64_t    pace_retry_ms_ = 0;        ///< after a refusal, nothing is pressed before this
    std::uint64_t    pace_buff_ms_ = 0;         ///< a buff's press, whose cast is waited for; 0 none
    std::uint64_t    pace_rounds_ = 0;          ///< whole rounds since the start
    std::vector<PaceStatus::Step> pace_round_;  ///< the turns of the round under way
    /// The minute's counts, for log_paced.
    struct PaceMinute {
        std::uint64_t rounds = 0;
        std::uint64_t confirmed = 0;
        std::uint64_t on_repress = 0;       ///< of them, cast on the re-press
        std::uint64_t repressed = 0;
        std::uint64_t other = 0;            ///< casts of yours no press of the rotation's was waiting for
        std::uint64_t fallback_ms = 0;
    };
    PaceMinute    pace_minute_;
    std::uint64_t pace_log_ms_ = 0;
    std::uint64_t pace_fallback_from_ = 0;      ///< the fallback time counted up to here
    mutable std::mutex pace_mutex_;
    PaceStatus         pace_status_;            ///< under pace_mutex_

    mutable std::mutex stats_mutex_;
    Stats              stats_;

    // ---- the black box: under box_mutex_, which is taken with no other lock
    // of the engine's held, and holds none of them ----
    struct BoxVitals {
        std::uint64_t t = 0;
        int           hp = -1, mp = -1;     ///< rounded percent; -1 no reading
        int           pet = -1;             ///< the pet's HP, likewise
    };
    struct BoxDecision {
        std::uint64_t t = 0;
        std::uint64_t key = 0;              ///< which key (key_ids_)
        std::uint32_t keycode = 0;
        int           mark = 0;             ///< its threshold, percent
        MacroKeyKind  kind = MacroKeyKind::HpPotion;    ///< an HP, MP or pet potion
        PotionWhy     why = PotionWhy::AboveMark;
        std::string   text;                 ///< for KeysHeld: what holds the game
    };
    /// How many reasons there are, for a tally of each.
    static constexpr int kPotionWhys = static_cast<int>(PotionWhy::BehindMp) + 1;
    /// A potion key's minute, for log_potions: its presses, and how long it
    /// spent in each reason, from its decisions as they change.
    struct PotionMinute {
        std::uint32_t keycode = 0;
        int           mark = 0;
        MacroKeyKind  kind = MacroKeyKind::HpPotion;
        std::uint64_t presses = 0;
        std::uint64_t ms[kPotionWhys] = {};
        PotionWhy     why = PotionWhy::AboveMark;     ///< the reason now, since `since`
        std::string   text;
        std::string   held_by;          ///< what last held the keys, for KeysHeld
        std::uint64_t since = 0;
        bool          active = false;   ///< pressed, or held for anything but above the mark, this minute
    };
    /// Records a vitals reading pushed at `t`, when it says something new.
    void box_vitals(std::uint64_t t, float hp_pct, float mp_pct, float pet_pct);
    /// Records a potion key's decision at `t`: when its reason or its text
    /// changed since its last, and every press. `kind` is the key's.
    void box_decide(std::uint64_t t, std::uint64_t key, const MacroKey& k, MacroKeyKind kind, PotionWhy why,
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
