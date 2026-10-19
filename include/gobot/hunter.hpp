#pragma once

#include "gobot/execution_coordinator.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/mob_detector.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gobot {

struct HunterConfig {
    /// Whether START on the simple screen goes hunting -- that screen's
    /// fighting switch. The hunter itself never reads it: F11 and the Attack
    /// switch start it regardless.
    bool enabled = false;

    /// Your character's name, exactly as it appears under its feet.
    ///
    /// This is how the bot sees its own character. Everything else on screen
    /// is found by its health bar, and a plate with bars may be yours or
    /// another player's -- the bars say a player stands there, not which one
    /// -- so without this the bot knows where the monsters are but not where
    /// it is standing, and cannot walk to one.
    ///
    /// Left blank, there is no "you" at all: a plate with bars is never taken
    /// for you, and the screens refuse to start hunting without a name.
    std::string character_name;

    std::uint32_t attack_key = 'Z';
    std::uint32_t left_key = 0x25;      ///< VK_LEFT
    std::uint32_t right_key = 0x27;     ///< VK_RIGHT

    /// The game's jump key, for the hop towards a monster above; 0 is none,
    /// and the hunter never hops. Space when hunt.ini has no jump_key -- what
    /// every file written before the jump box said, and so what an existing
    /// setup keeps -- though Space is this game's attack (GoC's own default
    /// jump is the Up arrow). The shipped hunt.ini has none: a key that acts
    /// on the game is the player's to give.
    std::uint32_t jump_key = 0x20;      ///< VK_SPACE

    /// Whether "Thử" (JumpCheck) last saw the character jump on jump_key.
    /// What the checklist goes by; a new key has not been tried.
    bool jump_checked = false;
    void set_jump_key(std::uint32_t vk) {
        if (vk != jump_key) jump_checked = false;
        jump_key = vk;
    }

    /// Horizontal distance, in screen pixels, at which the attack is thrown.
    /// Wider than it sounds: most skills here sweep a good way in front.
    int reach = 150;

    /// How far above or below the target may be and still count as reachable.
    /// Past this the bot would attack through a floor and hit nothing. With
    /// a "Kiểu đánh" the band is up_reach and down_reach instead; this stays
    /// for a file read by an older GoBot, which knows only this.
    int height_tolerance = 60;

    /// "Kiểu đánh": the attack style whose numbers these are, by its id in
    /// gobot/attack_style.hpp -- a class and weapon, or one of GoC's four
    /// sets. Empty is "Như trước": the hunter as it always was.
    ///
    /// A style turns GoC's own fighting rules on (goc_rules): a target in
    /// reach is kept until it dies instead of being picked afresh every
    /// give_up_ms, one whose bar never moves is given up on (no_damage_ms),
    /// and the character turns only when it is not facing the target already.
    /// Opt-in, the player's rule: a setup that chose none fights as before.
    std::string style;
    bool goc_rules() const { return !style.empty(); }

    /// GoC's minRange: the closest a target should be. Inside it, with
    /// keep_distance on, the character steps back. 0 is none -- and 50 px
    /// for keep_distance, as GoC takes it. Never more than `reach`.
    int min_distance = 0;

    /// The band above and below the character's feet, in screen pixels.
    /// `up_reach` / `down_reach` are how far above or below a monster can be
    /// hit at all (GoC's highestHitY / lowestHitY); `up_no_jump` /
    /// `down_no_jump` how far without jumping (higherThreshold /
    /// lowerThreshold). -1 follows height_tolerance -- the no-jump pair half
    /// of it -- which is the hunter before styles, so a hunt.ini that sets
    /// only height_tolerance works as it always did. Read through up(),
    /// down(), up_nj() and down_nj().
    int up_reach = -1;
    int down_reach = -1;
    int up_no_jump = -1;
    int down_no_jump = -1;
    int up() const { return up_reach >= 0 ? up_reach : height_tolerance; }
    int down() const { return down_reach >= 0 ? down_reach : height_tolerance; }
    int up_nj() const { return up_no_jump >= 0 ? up_no_jump : height_tolerance / 2; }
    int down_nj() const { return down_no_jump >= 0 ? down_no_jump : height_tolerance / 2; }

    /// The band set by hand -- the "Tầm đánh" window, the full screen's Up
    /// and Down: up and down as given, the no-jump band pinned where it is
    /// now, and height_tolerance the larger of the two for an older GoBot.
    void set_band(int up_px, int down_px);

    /// "Giữ khoảng cách": a target closer than min_distance (50 px with none
    /// set) is stepped back from before it is attacked, as GoC's backing_away
    /// does. It moves the character, so it ships off. Never while standing
    /// still, and never more than keep_distance_steps steps in a row from one
    /// target: GoBot cannot see where a platform ends, and GoC can.
    bool keep_distance = false;
    int  keep_distance_steps = 3;

    /// How far to the side, in pixels, a target may be before the character
    /// turns to it. GoC's "Hạn chế quay người", 12 in the player's config; the
    /// hunter's number before it was a setting.
    int facing_tolerance = 12;

    /// With a style: a target in reach whose health bar has not gone down in
    /// this much time spent attacking it is given up on -- something the
    /// attacks do not reach. Never one on "Đánh trước".
    std::uint32_t no_damage_ms = 15000;

    /// Don't chase forever. A target that stays out of reach this long is
    /// dropped and the next one picked, which is what stops the bot walking
    /// into a wall under a platform it cannot climb.
    ///
    /// Without a style this counts from when the target was picked, in reach
    /// or not, so every fight is picked afresh this often -- as it always has
    /// been. With one it counts only the time out of reach.
    std::uint32_t give_up_ms = 4000;

    /// Health, as a fraction of the plate's bar, below which a creature counts
    /// as finished: not worth picking, and worth abandoning if it is the
    /// current target.
    ///
    /// Target selection used to ignore the health bar entirely -- it picked by
    /// position alone and noticed a death only when the plate had moved more
    /// than 120 pixels from where it was. So the bot went on swinging at
    /// something whose bar was empty until the give-up timer fired seconds
    /// later, which is most of what "it attacks corpses" looks like from
    /// outside.
    ///
    /// Not zero. MobDetector's reading is approximate -- its own comment says
    /// the empty part of a bar is hard to bound -- so a creature at one or two
    /// percent reads as a sliver rather than as nothing, and a bar that is
    /// genuinely empty can measure slightly above zero.
    float dead_below = 0.05f;

    /// Prefer the most wounded reachable creature over the nearest one.
    ///
    /// Finishing something already hurt returns a kill sooner than starting on
    /// a fresh one, and it stops the bot spreading damage across a whole
    /// screen of monsters and killing none of them. Only ever among creatures
    /// it could already reach, so this never sends it chasing across the map.
    bool finish_wounded = true;

    /// How long a walk step presses the direction key.
    std::uint32_t step_ms = 180;

    /// Gap between attacks. The macro's own skill timers still run, so this is
    /// only the hunter's own swing.
    std::uint32_t attack_interval_ms = 700;

    /// Jump when the target is above and out of reach.
    bool jump_to_reach = true;

    /// Pick up what the monsters drop, with the game's pick-up key.
    ///
    /// Pressed a few times straight after each kill, and every
    /// `loot_interval_ms` while fighting. What a monster drops lands where it
    /// stood, which is in front of the character, so after a kill is when
    /// there is something to pick up. The commercial client's "Nhặt đồ"
    /// presses it continuously instead.
    ///
    /// A switch apart from the key, so switching it off does not forget which
    /// key it was.
    bool          loot = false;
    std::uint32_t loot_key = 0;
    std::uint32_t loot_interval_ms = 2000;
    bool loots() const { return loot && loot_key != 0; }

    /// Stand still: attack whatever comes within reach and never walk to
    /// anything. For a spot the monsters come to. The commercial client's
    /// "Chế độ lười".
    bool stay_put = false;

    /// The monster lists -- "Lọc quái" on the simple screen -- as the names
    /// written on the plates.
    ///
    /// A monster on `skip_names` ("Không đánh") is never attacked. One on
    /// `first_names` ("Đánh trước") is gone for before any other on its level,
    /// and takes over from an unlisted one being fought once it comes within
    /// reach. With `first_only` nothing else is attacked at all, which is what
    /// GoC's "Loại mob" does. A name on both lists is skipped. Names are
    /// matched loosely, since they are read off the screen: see
    /// monster_name_matches. Both empty, and `first_only` off, is the hunter
    /// as it was before the lists.
    ///
    /// GoC decides all of this on its server, from what the packets say a
    /// monster is -- boss, flying, which kind. Here the name is all there is,
    /// so "skip the flying ones" is their names on the skip list and "boss
    /// first" is the boss on the other.
    std::vector<std::string> skip_names;
    std::vector<std::string> first_names;
    bool                     first_only = false;

    /// Only act while the game window has focus. Same reasoning as the macro:
    /// off, the keys go somewhere else.
    bool require_focus = true;

    /// Bring the game to the front when hunting is switched on.
    ///
    /// Clicking START HUNTING takes focus off the game, and the hunter refuses
    /// to send anything to an unfocused window — so without this it starts and
    /// reports "waiting for the game to have focus" forever. Only on the way
    /// on; it never grabs the foreground back while running.
    bool focus_on_start = true;

    std::uint32_t toggle_key = 122;     ///< F11

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

/// Picks a monster and goes for it.
///
/// Deliberately simple: this game is a side-scroller, so "targeting" is facing
/// the right way and being close enough. There is no target lock to acquire —
/// attacks hit whatever is in front of the character — so the whole job is
/// choosing which monster to walk towards and knowing when to stop walking and
/// start hitting.
class Hunter {
public:
    Hunter(InputManager& input, ExecutionCoordinator& coord, MobDetector& detector);
    ~Hunter();

    Hunter(const Hunter&) = delete;
    Hunter& operator=(const Hunter&) = delete;

    void set_config(const HunterConfig& c);
    HunterConfig config() const;

    void start();
    void stop();
    bool running() const { return running_.load(); }

    /// What the hunter sees with, presses keys with and tells the time by: the
    /// detector, the game's keyboard and the steady clock unless set. Set
    /// before start(), never while it runs -- which is how the tests put
    /// scripted plates in front of it and write down every key it sends.
    /// `frame` is the number of the picture the plates were read from, which
    /// grows with each new one: the detector's frame count unless set, and
    /// one a pass when the plates are scripted and it is not -- a script's
    /// plates are as they are the moment it is asked.
    struct Eyes {
        std::function<bool(Nameplate&, bool*)>  player;   ///< your character; `fresh` may be null
        std::function<std::vector<Nameplate>()> mobs;     ///< the monsters, nearest first
        std::function<std::uint64_t()>          frame;
    };
    struct Hands {
        std::function<bool()>                   focused;
        std::function<bool(std::uint32_t, int)> key;          ///< keycode, hold ms
        std::function<void(std::uint32_t)>      key_up;
        std::function<bool(std::uint32_t)>      held;         ///< whether a key is down now
        std::function<void()>                   release_all;
    };
    /// `sleep_ms` is how a pass waits a moment -- GoC's 50 ms after a step
    /// back or a turn: a real sleep unless set, and a test's own clock wound
    /// on when it is.
    struct Clock {
        std::function<std::uint64_t()>     now_ms;
        std::function<void(std::uint32_t)> sleep_ms;
    };
    void set_eyes(Eyes e)   { eyes_ = std::move(e); }
    void set_hands(Hands h) { hands_ = std::move(h); }
    void set_clock(Clock c) { clock_ = std::move(c); }

    /// For tests, which drive the hunter by hand on a clock of their own:
    /// start as start() does but with no thread, and run one pass of the loop
    /// -- what the thread does every 60 ms -- at each step(). stop() ends it.
    void start_stepped();
    void step();

    /// Whether the hunter is fighting right now: on its last pass it was
    /// attacking a live target in reach, facing it, with the game in front,
    /// no hold on and no step being taken. What "skills follow the fight"
    /// waits for (MacroEngine::Fight). False at once when the target dies or
    /// goes, when it walks or steps back, when a hold begins, when the game
    /// loses focus and when hunting stops -- but a pass that cannot see the
    /// character keeps it for kEngagedGraceMs, as a name plate drops out for
    /// a frame or two all the time. Read from any thread.
    bool engaged() const;
    /// How far to the side the target was, in pixels, on that pass.
    int engaged_dx() const { return engaged_dx_.load(); }
    /// The attack key of the config in use, for any thread.
    std::uint32_t attack_key() const { return attack_key_.load(); }
    static constexpr std::uint64_t kEngagedGraceMs = 500;

    /// What it is doing, for the UI. Plain words -- this is the panel's
    /// explanation of why nothing is happening when nothing is happening.
    enum class State { Idle, NoPlayer, NoTargets, Approaching, Attacking, Unfocused,
                       Waiting };   ///< standing still, nothing within reach yet

    struct Status {
        State      state = State::Idle;
        Nameplate  target;
        bool       has_target = false;
        int        dx = 0, dy = 0;          ///< target relative to the player
        std::uint64_t attacks = 0;
        std::uint64_t steps = 0;
        /// Gave up: out of reach too long, or -- with a style -- attacked for
        /// no_damage_ms without its bar going down.
        std::uint64_t targets_dropped = 0;

        /// Steps back taken to keep the distance ("Giữ khoảng cách").
        std::uint64_t backsteps = 0;

        /// engaged(), and since when on the hunter's clock; and the last pass
        /// that counted as fighting.
        bool          engaged = false;
        std::uint64_t engaged_since_ms = 0;
        std::uint64_t last_engaged_ms = 0;

        /// Let go because the bar was empty. Worth counting separately from
        /// targets_dropped: one of these is a kill and the other is a failure
        /// to reach something, and a panel showing only the sum cannot tell
        /// you which the bot has been doing all afternoon.
        std::uint64_t targets_finished = 0;

        std::uint64_t loots = 0;              ///< pick-up key presses

        /// Monsters the "Không đánh" list kept it off: one for each seen with
        /// its name on the list that had not been seen there a moment before
        /// -- not one a frame, which would count the same monster seven times
        /// a second.
        std::uint64_t skipped_by_list = 0;

        /// Every monster in view was ruled out by the lists, so there is
        /// nothing to attack -- as against no monsters at all, which is
        /// NoTargets on its own. What the simple screen says it is doing.
        bool only_skipped = false;

        /// When "no monsters in view" began, on the hunter's clock; 0 once
        /// there is a target again.
        std::uint64_t no_target_since_ms = 0;
    };
    Status status() const;

    static const char* to_string(State s);

    /// Whether a monster `dy` from the character (screen pixels, y growing
    /// downward, so negative is above) stands on its level: the height half
    /// of in_reach, and what target choice calls "on the character's level".
    /// Up to up() above and down() below, which are height_tolerance both
    /// ways without a style.
    static bool level_with(const HunterConfig& cfg, int dy);

    /// Whether a monster `dx`, `dy` from the character is close enough to
    /// attack where it stands.
    static bool in_reach(const HunterConfig& cfg, int dx, int dy);

    /// The distance a target is kept at with keep_distance: min_distance, or
    /// GoC's 50 px when that is 0.
    static int keep_away_px(const HunterConfig& cfg);
    static constexpr int kDefaultKeepAwayPx = 50;

    /// How long a step back from a target `dx` to the side is held to get to
    /// `away` px from it: GoC's (away - |dx| + 20) px at 355 px a second,
    /// from 50 to 800 ms.
    static int backstep_ms(int away, int dx);

    /// What the monster lists make of a plate.
    enum class Listed {
        Free,       ///< nothing: the lists have no say over it
        First,      ///< its name is on "Đánh trước"
        Skip,       ///< its name is on "Không đánh"
        NearSkip,   ///< nameless, right where one on "Không đánh" was a moment ago
        NotFirst,   ///< not on "Đánh trước", with only those to be attacked
        GaveUp,     ///< given up on a moment ago for taking no damage (a style's rule)
    };
    /// Not to be attacked.
    static bool refused(Listed l) {
        return l == Listed::Skip || l == Listed::NearSkip || l == Listed::NotFirst ||
               l == Listed::GaveUp;
    }

    /// Which of `mobs` (nearest first) to go for, with no target yet: the
    /// nearest on the character's level, or the most wounded there with
    /// finish_wounded -- of those on "Đánh trước" when any are there; with
    /// none on the level, the nearest with health. Never one the lists
    /// refuse, nor one already finished. `me` null: where the character
    /// stands is not known, and only the last rule applies. `listed` is
    /// parallel to `mobs`. An index into `mobs`, or -1.
    static int choose_target(const HunterConfig& cfg, const std::vector<Nameplate>& mobs,
                             const Nameplate* me, const std::vector<Listed>& listed);

    /// Standing still: of those within reach of `me` now, the nearest along
    /// the ground -- the nearest on "Đánh trước" if any is. -1 for none.
    static int choose_in_reach(const HunterConfig& cfg, const std::vector<Nameplate>& mobs,
                               const Nameplate& me, const std::vector<Listed>& listed);

private:
    void loop();
    /// One pass of the loop: the thread's every 60 ms, and step()'s.
    void pass();
    /// What a start clears: the target, and what the lists remembered.
    void fresh_run();
    /// Every key let go, whatever thread pressed it: for the game losing
    /// focus, and for stopping.
    void release_movement();
    /// The direction keys let go of, each only if it is down: what the hunter
    /// itself may have left held. Not release_all(), which would cut short a
    /// key another thread is in the middle of pressing -- a potion among them.
    void let_go_of_moves(const HunterConfig& cfg);

    /// One press of the pick-up key, if the input is free. True if it went.
    bool press_loot(const HunterConfig& cfg);

    /// Through the seams, or the real thing when one is not set.
    bool                   see_player(Nameplate& out, bool* fresh = nullptr) const;
    std::vector<Nameplate> see_mobs() const;
    bool                   focused() const;
    bool                   press(std::uint32_t vk, int hold_ms);
    void                   let_go(std::uint32_t vk);
    bool                   is_down(std::uint32_t vk) const;
    std::uint64_t          clock_now() const;

    /// Wait `ms` inside a pass: through the clock's seam, or a real sleep.
    void pause(std::uint32_t ms);

    /// What the lists make of each of `mobs` this pass, after bringing the
    /// places skipped monsters were seen up to date.
    std::vector<Listed> judge(const HunterConfig& cfg, const std::vector<Nameplate>& mobs,
                              std::uint64_t now);
    /// Said once per name per run: which monster the skip list kept it off.
    void say_skipped(const std::string& name);

    /// Those of `mobs` where a target was given up on for taking no damage a
    /// moment ago, marked GaveUp in `listed` -- each spot followed to the
    /// nearest plate as it moves, and forgotten after no_damage_ms.
    void mark_gave_up(const std::vector<Nameplate>& mobs, std::uint64_t now,
                      std::vector<Listed>& listed);

    /// engaged(): on, with the target `dx` to the side, at `now`; and off.
    void engage(int dx, std::uint64_t now);
    void disengage();

    /// The fight with the current target, for the log. `slack`: how far a
    /// step back may have shifted the view since the last pass.
    void track_fight(const HunterConfig& cfg, const Nameplate& target, std::uint64_t now,
                     int slack);
    /// The fight is over -- a kill, a give-up, a list, another target -- and
    /// said in the log with the distances it was fought at.
    void end_fight(const char* why);
    /// A look with no target to be had -- no monster in view, or none that
    /// may be attacked: the fight goes on, unless its monster has not been
    /// seen for kPlayerMemoryMs. A plate drops out for a look or two all the
    /// time, and each such look ended the fight, splitting its line in the
    /// log in two.
    void fight_unseen(std::uint64_t now, const char* why);
    /// An attack went out with the target `dx` to the side, `away` being
    /// keep_away_px.
    void note_attack(int dx, int away);
    /// With a style: whether `target` has been attacked for no_damage_ms
    /// without its bar going down.
    bool no_damage(const HunterConfig& cfg, const Nameplate& target, std::uint64_t now);
    /// The steps back follow `target` as the fight does, by where it is; one
    /// somewhere else, or after kPlayerMemoryMs unseen, starts them afresh.
    void follow_back_steps(const Nameplate& target, std::uint64_t now, int slack);
    /// The no-damage watch follows `target` the same way.
    void follow_watch(const Nameplate& target, std::uint64_t now, int slack);
    /// What the last step back from the target ran into, by what has moved
    /// on screen since: "a wall" when nothing has, "a ledge" when the height
    /// has changed more than a step can. Null when neither.
    const char* step_went_wrong(const Nameplate& me, const Nameplate& target) const;
    /// The number of the picture the plates come from: see Eyes::frame.
    std::uint64_t picture() const;
    /// Once a minute while hunting, one line of how the fights went.
    void minute_log(const HunterConfig& cfg, std::uint64_t now);

    InputManager&         input_;
    ExecutionCoordinator& coord_;
    MobDetector&          detector_;

    Eyes  eyes_;
    Hands hands_;
    Clock clock_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stepped_{false};      ///< started by start_stepped: no thread
    std::thread       thread_;

    mutable std::mutex mutex_;
    HunterConfig       config_;
    Status             status_;

    /// Which plate we are chasing, remembered between frames by position so a
    /// jitter in the detector does not make the bot change its mind every
    /// tick and stand still vibrating.
    int           target_x_ = 0, target_y_ = 0;
    bool          have_target_ = false;
    std::uint64_t target_since_ms_ = 0;
    std::uint64_t last_attack_ms_ = 0;

    /// Whether the target is on "Đánh trước" -- nothing takes over from one
    /// that is -- and when one last took over.
    bool          target_first_ = false;
    std::uint64_t last_switch_ms_ = 0;
    bool          switched_ = false;

    /// Where monsters on "Không đánh" were last seen, kept a moment after
    /// their name was last read: a name drops out for a frame or two all the
    /// time, and a nameless plate right where a skipped monster stood is that
    /// monster.
    struct SkipSpot {
        int           x = 0, y = 0;
        std::uint64_t until_ms = 0;
    };
    std::vector<SkipSpot>    skip_spots_;
    std::vector<std::string> said_skipped_;

    int           loot_left_ = 0;       ///< presses still owed to the last kill
    std::uint64_t last_loot_ms_ = 0;

    // ---- read by other threads ----
    std::atomic<bool>          engaged_{false};
    std::atomic<int>           engaged_dx_{0};
    std::atomic<std::uint32_t> attack_key_{'Z'};

    // ---- the hunter's own thread's, or step()'s ----
    std::uint64_t last_engaged_ms_ = 0;

    /// Which way the character faces, as far as the hunter knows: set by its
    /// own steps and turns, unknown at a start, with the game out of focus or
    /// the character out of sight, and once a hold is over -- whatever ran in
    /// it may have turned the character. With a style or keep_distance the
    /// character turns only when this says it is not facing the target.
    enum class Facing { Unknown, Left, Right };
    Facing facing_ = Facing::Unknown;
    bool   held_before_ = false;        ///< a hold was on at the last pass
    /// The last pass stepped back, so this one faces the target and attacks.
    bool   just_stepped_ = false;
    /// A step back shifts the view, and the target's plate with it, by as
    /// much as the step: how much, and until when that is allowed for when
    /// the target is followed from one look to the next.
    int           step_slack_px_ = 0;
    std::uint64_t step_slack_until_ms_ = 0;

    /// The fight with the current target, for the log. Begun when one is
    /// picked somewhere the last one was not, and ended when it is let go of
    /// for good -- a kill, a give-up, the lists, another target -- or once
    /// its monster has gone unseen for MobDetector::kPlayerMemoryMs: a plate
    /// missing a look or two does not end it (fight_unseen). A bar misread
    /// as empty once does: most of the time it is a kill.
    struct FightLog {
        bool             on = false;
        int              x = 0, y = 0;          ///< where the target was last seen
        std::uint64_t    seen_ms = 0;
        int              keep_away = 0;         ///< keep_away_px when it began
        int              attacks = 0;
        std::vector<int> attack_dx;             ///< |dx| at each attack, the first 64
        int              backsteps = 0;
    };
    FightLog fight_;

    /// The no-damage watch on the monster being fought (with a style): the
    /// bar it has to fall from, the attacking time since it last did, the
    /// pass that counted last, and the attacks made on it. Kept apart from
    /// the fight's log, as the steps back are: it lived in the log, which a
    /// plate missing for one look ended, and with plates dropping out every
    /// few seconds as they do, a monster the attacks could not hurt held the
    /// bot for good. Follows the monster by where it stands, as the steps
    /// back do; another one, this one unseen for MobDetector::kPlayerMemoryMs,
    /// or a target let go of on purpose -- given up on, ruled out by the
    /// lists, taken over by one on "Đánh trước" -- starts it afresh. A bar
    /// read empty once does not: a kill is seen as damage first anyway, so
    /// the next monster to stand there inherits only a moment of quiet.
    struct DamageWatch {
        float         ref_hp = -1.0f;
        std::uint64_t quiet_ms = 0;
        std::uint64_t quiet_seen_ms = 0;
        int           attacks = 0;
        int           x = 0, y = 0;         ///< where the monster was last seen
        std::uint64_t seen_ms = 0;
    };
    DamageWatch watch_;

    /// The steps back from one monster: how many in a row, and whether one
    /// met a wall or went down a ledge. Kept apart from the fight's log,
    /// which a bar misread as empty ends -- and once a plate missing for one
    /// look ended too: the count started again with it, and the character
    /// walked back without end. The bound is all that stands between the
    /// character and an edge GoBot cannot see. The monster is followed from
    /// look to look as the target is, by where it stands; another one, or
    /// this one unseen for MobDetector::kPlayerMemoryMs, starts the count
    /// again.
    struct BackSteps {
        int           in_row = 0;           ///< without an attack from far enough since
        bool          no_more = false;      ///< a wall or a ledge met: none again from this one
        int           x = 0, y = 0;         ///< where the monster was last seen
        std::uint64_t seen_ms = 0;
        /// The last step, still to be judged on the first picture taken
        /// wholly after it -- no step goes before -- and where things stood
        /// before it.
        bool          judge = false;
        std::uint64_t judge_from = 0;
        int           from_me_x = 0, from_me_y = 0, from_x = 0, from_dy = 0;
    };
    BackSteps back_;
    /// Passes run, for the picture's number with scripted plates.
    std::uint64_t passes_ = 0;

    /// Where targets were given up on for taking no damage.
    struct GaveUpSpot {
        int           x = 0, y = 0;
        std::uint64_t until_ms = 0;
    };
    std::vector<GaveUpSpot> gave_up_;

    /// What the once-a-minute line says.
    std::uint64_t minute_from_ms_ = 0;
    int           minute_fights_ = 0;
    int           minute_far_ = 0;          ///< of them, fought from keep_away or further
    int           minute_backsteps_ = 0;
    std::uint64_t minute_dropped_ = 0;      ///< targets_dropped when the minute began
};

} // namespace gobot
