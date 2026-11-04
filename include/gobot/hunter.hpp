#pragma once

#include "gobot/execution_coordinator.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/map_book.hpp"
#include "gobot/mob_detector.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gobot {

/// Where the hunter's positions may come from: "Vị trí từ gói tin".
///
/// Like HunterConfig::enabled, the hunter itself never reads it. The GUI
/// starts a capture of the game's own traffic by it whenever hunting starts,
/// and PacketEyes (gobot/packet_eyes.hpp) decides by it, pass by pass, whether
/// you and the monsters come from the game's frames or from the screen.
enum class PacketPositions : std::uint8_t {
    Off,    ///< no capture for positions: the screen, as always
    Watch,  ///< capture, and log what the packets say beside the screen; the screen fights
    On,     ///< the packets steer while they are healthy, the screen whenever they are not
};

/// "off", "watch" or "on", as hunt.ini writes it.
const char* packet_positions_name(PacketPositions p);
/// hunt.ini's word back, whatever its case and spaces. Anything else --
/// a word no GoBot wrote, or none -- is Watch: capture and log only.
PacketPositions packet_positions_from(const std::string& word);

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
    /// sets. Empty is "Như trước": the hunter as it always was, but for the
    /// turn to a target.
    ///
    /// A style turns GoC's own fighting rules on (goc_rules): a target in
    /// reach is kept until it dies instead of being picked afresh every
    /// give_up_ms, and one whose bar never moves is given up on
    /// (no_damage_ms). Opt-in, the player's rule: a setup that chose none
    /// fights as before. Turning is no longer among them: every setup, a style
    /// or none, turns to the target only when it is not facing it already
    /// (Hunter::kTurnTapMs). The player asked for that -- a tap on every pass,
    /// as "Như trước" turned, walked a ranged character in on its monsters a
    /// few pixels at a time while it shot.
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
    /// still, and never more than keep_distance_steps steps one way until the
    /// character has walked back the other way -- whichever monsters they were
    /// from: GoBot cannot see where a platform ends, and GoC can.
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
    /// into a wall under a platform it cannot climb. One not within reach at
    /// any look in all that time -- a hold, or the game out of focus, not
    /// counted -- is passed over for twice as long, "Đánh trước" or not:
    /// picked straight back, it held the bot under its ledge for good.
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

    /// Walk to a target with the direction key held down from one look to the
    /// next, as a player walks, and let go of it when the walk is over: the
    /// target in reach, or the other way, or nothing to go for. Off, each look
    /// presses the key for step_ms and lets go, as GoBot always walked -- and
    /// the character stops between steps, four times a second: 933 walks in
    /// the 4.5 minutes of the hunt of 2026-10-27, all but 3 stopped within a
    /// second of starting.
    bool hold_walk = true;

    /// How long a walk step presses the direction key, with hold_walk off.
    std::uint32_t step_ms = 180;

    /// Gap between attacks. The macro's own skill timers still run, so this is
    /// only the hunter's own swing.
    std::uint32_t attack_interval_ms = 700;

    /// Jump when the target is above and out of reach.
    bool jump_to_reach = true;

    /// "Nhảy xuống bậc dưới": drop through the platform underfoot with Ctrl
    /// + ↓ (InputManager::drop_ctrl_down, the player's key for it) to the
    /// target on a floor below -- more than max(down(), drop_min_px) below,
    /// no more than drop_side_px to the side, and nothing to fight on the
    /// character's own level. Only with the packets steering
    /// (packet_positions=on), never with stay_put, and with the hunting spot
    /// on only where the character stands, lands and the target all lie
    /// inside its area. Only on a map whose file GoBot trusts (MapTrust), and
    /// only where that file says the fall lands on the target's floor or one
    /// between, never past it, never through the bottom ground and never
    /// with a gate within drop_gate_px across of where the character stands,
    /// falls or lands: on a gate Ctrl + ↓ changes the map (the player's hand
    /// test, 2026-11-04). A map change after a press stops the hunt and
    /// unticks this (GuiApp). It moves the character, so it ships off.
    bool drop_down = false;
    int  drop_min_px = 100;     ///< 60..470
    int  drop_side_px = 120;    ///< 20..300
    int  drop_gate_px = 150;    ///< 40..400

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
    /// "Chế độ lười". It turns to a target only when not facing it already,
    /// as every setup does (Hunter::kTurnTapMs).
    bool stay_put = false;

    /// "Chỗ đánh quái": hunt round one spot and walk back to it when there is
    /// nothing to fight. The spot is where the character stands, settled on
    /// the ground, at the first look the packets know it (WorldFix) -- in
    /// their world pixels, which do not move with the view. Only monsters
    /// inside the area of spot_half_w either side of it and spot_half_h above
    /// and below are walked to; one within reach is fought wherever it
    /// stands, since the area bounds walking, not hitting. No hop toward a
    /// monster while it is on: a hop can leave the character on the platform
    /// above, off its spot with no way down (↓ is never pressed). With no
    /// position to go by -- no capture, the packets lost you, a map entry or
    /// a log in since (Hunter::suspend_spot) -- the character stands, as with
    /// stay_put. It moves the character, so it ships off. One spot a run,
    /// taken afresh at each start and when switched on again mid-run --
    /// switched off, the spot is dropped: nothing of it is saved yet.
    bool          spot = false;
    int           spot_half_w = 300;            ///< 50..2000
    int           spot_half_h = 100;            ///< 20..500
    /// Further from the spot than this along the ground, with nothing to
    /// fight for spot_return_delay_ms, the character walks back: GoC's
    /// return_tolerance_x. At half_w or more it walks back only once pushed
    /// out of the area.
    int           spot_tolerance = 48;          ///< 10..1000
    std::uint32_t spot_return_delay_ms = 1500;  ///< 0..60000
    /// A walk back that the game's own reports put no Hunter::kSpotProgressPx
    /// nearer the spot in this long -- a walk into a wall sends none, and the
    /// packets work it out as going on -- is let go of and tried again
    /// spot_retry_ms later; after Hunter::kSpotMaxTries in a row, not again
    /// until the spot is set again or the character is back at it. Not under
    /// 3 s: a walk on a clear floor sends no report between its start and
    /// the stop of its renewal (Hunter::kWalkRenewMs, 2.5 s), and would be
    /// given up on before that came.
    std::uint32_t spot_give_up_ms = 4000;       ///< 3000..60000
    std::uint32_t spot_retry_ms = 30000;        ///< 1000..600000

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
    /// on. Keeping it in front while running is a separate switch, refocus in
    /// [Simple] in app.ini (GuiApp::keep_game_in_front, FrontKeeper), on by
    /// default.
    bool focus_on_start = true;

    /// "Vị trí từ gói tin": see PacketPositions. Watch when hunt.ini has none,
    /// as every file before it: the capture runs all session and only writes
    /// what the packets say beside what the screen shows; the screen still
    /// fights. Ticked on the simple screen it is On.
    PacketPositions packet_positions = PacketPositions::Watch;

    /// How the packets' world pixels lie over the screen's, for when a live
    /// hunt shows they do not line up one to one -- nothing has measured that
    /// on the game yet. `packet_scale` is world pixels per screen pixel, both
    /// ways: 1, as the walking speeds imply (0.25 to 4). `packet_dy` is added
    /// to every packet monster's height against you, in screen pixels: 0 takes
    /// your y and a monster's for the same point of the body, where on screen
    /// a monster level with you reads about 15 px below you (-200 to 200).
    /// The hunter's reach and band were set in screen pixels, so these make
    /// the packets read as the screen does. gobot_gui.log's "Positions" line,
    /// every 10 s of hunting, prints what it measured beside both.
    float packet_scale = 1.0f;
    int   packet_dy = 0;

    std::uint32_t toggle_key = 122;     ///< F11

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

/// Where the game's packets put your character in their world, for the
/// hunting spot (HunterConfig::spot): world pixels, y growing downward, the
/// same whatever the view shows -- and which way it faces, for the turn to a
/// target (Hunter::kFacingTrustMs). PacketEyes works one out at every pass
/// from the same PacketWorld view the pass's plates come from, whether the
/// packets steer or are only watched.
struct WorldFix {
    enum class State : std::uint8_t {
        NoCapture,      ///< no Npcap, no capture running, or no game TCP for 3 s: nothing to go by
        FirstReport,    ///< traffic, but no report of yours since the capture began
        Lost,           ///< another character's reports a moment ago, or a move of yours unreported too long
        Known,          ///< where you are
    };
    State state = State::NoCapture;
    float x = 0.0f, y = 0.0f;       ///< worked out to now: a walk goes on between its reports
    /// Where the last report of yours -- or the last map entry -- put you,
    /// not worked out, and when on the frames' clock. A walk into a wall
    /// sends nothing: x runs on, and this stays.
    float         rx = 0.0f, ry = 0.0f;
    std::uint64_t report_ms = 0;
    /// Which way your last 0x0027 had you facing: +1 toward larger x -- to
    /// the right -- -1 to the left, 0 not known; and when that report came, on
    /// the frames' clock (PacketWorld::Me::said_facing, facing_ms). Only a
    /// 0x0027's, the one report whose facing was checked against the game: a
    /// 0x0029, a 0x0028, a cast or a jump moves neither.
    int           facing = 0;
    std::uint64_t facing_ms = 0;
    bool airborne = false;          ///< off the ground, by your last 0x002A
    bool moving = false;            ///< going sideways
    bool uncertain = false;         ///< followed past the time a move is followed with no report
    bool          map_known = false;    ///< a map entry since the capture began: map and area are it
    std::uint16_t map = 0, area = 0;
    /// Map entries ever seen, a capture started again or not: a number that
    /// moves at each one (PacketWorld::View::map_epoch).
    std::uint32_t epoch = 0;
    float entry_x = 0.0f, entry_y = 0.0f;   ///< the last map entry's spawn point
    /// Standing on the ground, with nothing worked out: where the spot is set.
    bool settled() const { return state == State::Known && !airborne && !moving && !uncertain; }
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
        /// Which frame of reference the plates are in -- the screen's, or the
        /// world's the game's packets give (PacketEyes): a number that moves
        /// when that changes. Asked once a pass, after `frame`. Unset, it
        /// never changes. On a change the hunter lets go of what it held by
        /// where things stood (forget_positions), and of nothing else.
        std::function<std::uint32_t()>          view;
        /// Where the packets put the character in their world (WorldFix):
        /// asked once a pass, after `view`, with HunterConfig::spot on. Off,
        /// it is asked only at a pass with a target in reach, for which way
        /// the character faces (WorldFix::facing), and then only with the
        /// packets steering (packet_positions=on) -- never twice a pass.
        /// With drop_down on (packets On, stay_put off), also asked once at
        /// the start of each pass -- a second time with the spot on -- and
        /// for kPressWatchMs after a drop's key whatever the setup;
        /// face_from_packets may then ask again in the in-reach branch, as it
        /// does with the spot off. With drop_down off, nothing new is asked.
        /// Unset, or false, there is no capture to say: NoCapture, and the
        /// hunter's own idea of the facing stands.
        std::function<bool(WorldFix&)>          where;
        /// The game's own count of your take-offs (GuiApp::packet_takeoffs):
        /// asked as a drop's key goes out and while it falls, for whether
        /// the character left the ground. Unset, or none: not known.
        std::function<std::optional<std::uint64_t>()> takeoffs;
        /// The monsters standing where their own frame put them, in the
        /// packets' world pixels (PacketEyes::stands): standing points for
        /// the map drops go by (MapTrust). Asked once a pass with drop_down
        /// on. Unset: none.
        std::function<std::vector<MapTrust::Mob>()> stands;
    };
    struct Hands {
        std::function<bool()>                   focused;
        std::function<bool(std::uint32_t, int)> key;          ///< keycode, hold ms
        std::function<bool(std::uint32_t)>      key_down;     ///< down, and left down: a held walk
        std::function<void(std::uint32_t)>      key_up;
        std::function<bool(std::uint32_t)>      held;         ///< whether a key is down now
        std::function<void()>                   release_all;
        /// Ctrl + ↓ (InputManager::drop_ctrl_down), with what is asked before
        /// each step of it; and the drop's key-ups still owed sent again
        /// (InputManager::settle_owed): true when nothing is owed.
        std::function<InputManager::DropPress(const std::function<bool()>&)> drop;
        std::function<bool()>                   settle_owed;
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

    /// The game's own map files, for drops (HunterConfig::drop_down): where
    /// a fall lands, the bottom ground and the gates, and what this session
    /// learned. Set before start(), never while it runs. Null -- as the
    /// command line's hunter has it -- and nothing is ever dropped to.
    void set_map_book(GameMapBook* book);
    /// What GoBot outside the hunter says against a drop at this moment --
    /// HP low or not known, the macro held, a walk, the keeper crossing, one
    /// of GoBot's own keys on Ctrl or ↓ -- in the log's English, or null for
    /// nothing. Asked on the hunter's thread, so it reads atomics only. Set
    /// before start(); unset, nothing is ever dropped to.
    void set_drop_veto(std::function<const char*()> veto);

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

    /// A walk held this long (HunterConfig::hold_walk) is let go of for
    /// kWalkRenewGapMs and taken up again: the game reports a walk of yours
    /// only as it starts and stops, and the packets follow one for
    /// PacketWorld::kOwnReckonMs with no report, so a stop and a start come
    /// before that runs out. On the screen it is a moment's pause.
    static constexpr std::uint64_t kWalkRenewMs = 2500;
    static constexpr std::uint32_t kWalkRenewGapMs = 40;
    /// A held walk the target comes no kWalkProgressPx nearer in for
    /// kWalkStuckMs is renewed at once: its start may never have reached the
    /// packets, which then have you standing while you walk.
    static constexpr std::uint64_t kWalkStuckMs = 400;
    static constexpr int           kWalkProgressPx = 40;
    /// How long after a walk is let go of it still counts as held for
    /// walk_held_until_ms(): its stop comes a moment after the key is up.
    static constexpr std::uint64_t kWalkStopGraceMs = 1000;

    /// Until when the packets should follow a walk of yours as one the hunter
    /// holds: UINT64_MAX while it holds one, kWalkStopGraceMs past the moment
    /// it let go of the last, 0 before any -- on the hunter's clock. A walk it
    /// does not hold -- a step back, a turn, one of the player's own -- is
    /// followed only as long as every walk was before the held walk
    /// (PacketWorld::kOwnStepReckonMs). Read from any thread.
    std::uint64_t walk_held_until_ms() const { return walk_until_ms_.load(); }
    /// How long a jump keeps the character in the air, with some to spare:
    /// GoC's jump, -800 px/s under 2400 px/s², is up and down in 667 ms. A hop
    /// toward a target above (jump_to_reach) comes no sooner after the last
    /// one, walking held or stepping: pressed again in the air, the jump key
    /// is a second jump -- a double jump, in a game that has one. It came
    /// every 300 ms with the walk held, and every look stepping.
    static constexpr std::uint64_t kJumpAirMs = 817;
    /// A walk held this long one way, from one look to the next, wins back
    /// the ground the steps back the other way gave up -- about 100 px at a
    /// walk -- and they are counted afresh (keep_distance_steps).
    static constexpr std::uint64_t kRegainWalkMs = 300;
    /// How long the tap that turns the character to a target in reach holds
    /// the arrow down -- the turn's tap alone: not a step back, a hop, a walk
    /// or an attack. The game reads its keyboard once a frame, and a press
    /// shorter than a frame is missed now and then (MacroConfig's
    /// key_hold_min_ms says the same). The 30 ms it was got by while the tap
    /// went out at every pass, a missed one made up for at the next; now the
    /// character is turned only when the hunter does not take it to be facing
    /// the target already, and a turn missed is not tried again at the next
    /// pass -- only once the packets show it missed (kFacingMissedMs). The
    /// attack's own press is as long. It walks the character about 21 px,
    /// but once a turn, so they do not add up; and a step back
    /// (keep_distance), which a turn always follows, is held as much longer
    /// than backstep_ms, so that turn walks it no nearer than the step meant.
    static constexpr int kTurnTapMs = 60;
    /// A report of yours that says which way you face (WorldFix::facing) is
    /// taken over the hunter's own idea of it at once when it came more than
    /// this long after the last key of the hunter's own that turned the
    /// character -- a turn's tap, a walk, a step back: it shows the character
    /// as that key left it, or as something since turned it. One stamped
    /// sooner may have been sent before the key reached the game; it is
    /// taken at once only when it says the way the key went, which is so
    /// whichever it was -- after a hold, say, with the key's own report the
    /// last. Both on the steady clock, now_ms(), in the GUI: the frames'
    /// stamps and the hunter's own.
    static constexpr std::uint64_t kFacingTrustMs = 150;
    /// A key that turns the character -- a walk starting, stopping or turning
    /// -- has the game send a report of it (0x0027), within a frame or two
    /// and the capture's moment. So when, this long after the hunter's own
    /// last turning key, the last report still says the other way and came
    /// no more than kFacingTrustMs after the key, the game never took the
    /// key -- a press it read no frame of, under a load, or in an animation
    /// -- and the report is taken: the hunter turns the character again. A
    /// report the capture lost costs a tap more, and the wait is long enough
    /// for a game at a few frames a second.
    static constexpr std::uint64_t kFacingMissedMs = 500;
    /// How far to the side the target was, in pixels, on that pass.
    int engaged_dx() const { return engaged_dx_.load(); }
    /// The attack key of the config in use, for any thread.
    std::uint32_t attack_key() const { return attack_key_.load(); }
    static constexpr std::uint64_t kEngagedGraceMs = 500;

    // ---- the drop through a platform (HunterConfig::drop_down): the plan's 11a

    /// Settled to press within drop_side_px across of the target, and pressed
    /// within this much more.
    static constexpr int           kDropPressSlackPx = 20;
    /// A settle waits this long for the report that the character stands;
    /// one given up on, or refused at the press, is not begun again for
    /// kDropSettleBackoffMs. The coordinator's token, refused, is waited for
    /// kDropTokenWaitMs.
    static constexpr std::uint64_t kDropSettleMs = 600;
    static constexpr std::uint64_t kDropSettleBackoffMs = 2000;
    static constexpr std::uint64_t kDropTokenWaitMs = 1000;
    /// No take-off and no landing lower this long after ↓ went down: Ctrl + ↓
    /// did nothing there. Still in the air kDropFallMaxMs after it -- a fall
    /// of 475 px takes about 0.63 s -- and what came of it is not known. Until
    /// then the drop counts as in the air (in_air), unless a landing came.
    static constexpr std::uint64_t kDropJudgeMs = 1000;
    static constexpr std::uint64_t kDropFallMaxMs = 1800;
    /// Landed this much lower than it stood -- or higher, a rise.
    static constexpr int           kDropLandPx = 24;
    /// The landing floor against the target's height, and where it landed
    /// against where the map file said: live, 0-5 px.
    static constexpr int           kDropFloorMatchPx = 12;
    /// The floor landed on is the first more than kDropFeetPx below where the
    /// character stands, at least kDropMinFallPx below it -- a floor between
    /// is a real one -- and the same floor, kDropSameFloorPx up or down, this
    /// far either side: at the settle and at the press.
    static constexpr int           kDropFeetPx = 8;
    static constexpr int           kDropMinFallPx = 60;
    static constexpr int           kDropSameFloorPx = 6;
    static constexpr int           kDropMarginSettlePx = 24;
    static constexpr int           kDropMarginPressPx = 8;
    /// A gate counts for drop_gate_px from this far above where the character
    /// stands to this far below where it lands: the standing point, the fall
    /// and the landing in one test.
    static constexpr int           kDropGateSpanPx = 60;
    /// No press sooner than kDropGapMs after the last; no more than
    /// kDropsPerMinute in a minute, nor kDropsPerTarget for one monster --
    /// nor a settle begun for one after kDropSettlesPerTarget let go of
    /// without a press, each a walk held off and let go of again.
    static constexpr std::uint64_t kDropGapMs = 1500;
    static constexpr int           kDropsPerMinute = 4;
    static constexpr int           kDropsPerTarget = 2;
    static constexpr int           kDropSettlesPerTarget = 2;
    /// After a drop that worked, no hop toward a target for this long.
    static constexpr std::uint64_t kDropStayMs = 8000;
    /// No drop this soon after a map entry first seen.
    static constexpr std::uint64_t kDropAfterEntryMs = 5000;
    /// Drops off until the next start after this many presses in a row that
    /// did nothing or jumped in place, or this many in a run.
    static constexpr int           kDropFailsInRow = 3;
    static constexpr int           kDropFailsPerRun = 5;
    /// ↓ held longer than this -- the hold is 110 ms at most -- and drops are
    /// off until the next start.
    static constexpr std::uint32_t kDropHeldMaxMs = 180;
    /// A map change this long after a press is the press's.
    static constexpr std::uint64_t kPressWatchMs = 10000;
    /// Passes in a row a drop's key-up stays owed before the hunter halts.
    static constexpr int           kDropOwedPasses = 3;

    /// What it is doing, for the UI. Plain words -- this is the panel's
    /// explanation of why nothing is happening when nothing is happening.
    enum class State { Idle, NoPlayer, NoTargets, Approaching, Attacking, Unfocused,
                       Waiting,     ///< standing still, nothing within reach yet
                       Returning }; ///< walking back to the hunting spot, nothing to fight

    /// The hunting spot (HunterConfig::spot), in the packets' world pixels:
    /// set where the character stood, the area round it, and the map entries
    /// seen when it was (WorldFix::epoch). Suspended by a map entry or a log
    /// in since, with where the character was last known before it. `login`:
    /// a log in suspended it (suspend_spot) with no map entry before -- back
    /// in use once the character stands within kSpotBackPx of where it was or
    /// of the spot, as well as at a map entry there; a map entry anywhere else
    /// since makes it one set aside by that entry. By a map entry, only at a
    /// map entry back where it left from. Never on a place the packets know
    /// is another: the spot's own, when they knew it as it was set or came
    /// back (`map`), and the one the entry that set it aside led to (`away`).
    struct Spot {
        bool          set = false;
        float         x = 0.0f, y = 0.0f;
        int           half_w = 0, half_h = 0;
        std::uint32_t epoch = 0;
        bool          suspended = false;
        bool          login = false;
        float         left_x = 0.0f, left_y = 0.0f;
        bool          map_known = false;
        std::uint16_t map = 0, area = 0;
        bool          away_known = false;
        std::uint16_t away_map = 0, away_area = 0;
    };

    /// Why the hunting spot does not steer the hunter just now, for the UI.
    enum class SpotWhy : std::uint8_t {
        None,           ///< in use, or none switched on
        WaitingFirst,   ///< no report of yours since the capture began: hunting as without one until one
        Settling,       ///< none yet, and the character not standing on the ground to take it: standing
        NoFix,          ///< no capture to say where the character is: standing
        Paused,         ///< the packets have lost the character: standing
        Suspended,      ///< a map entry or a log in since it was set: standing
        TooHigh,        ///< more than one jump above: not walked back to
        Retrying,       ///< a walk back got nowhere: tried again after spot_retry_ms
        GaveUp,         ///< kSpotMaxTries walks back in a row got nowhere: no more
    };

    /// Why the target below is not dropped to (drop_rule): the first of its
    /// rules that does not hold, in their order. None: it goes.
    enum class DropWhy : std::uint8_t {
        None,
        Off,                ///< drop_down off
        NoPackets,          ///< packet_positions not on
        StayPut,            ///< Đứng yên một chỗ
        Spot,               ///< the hunting spot on, but not in use: the character stands
        Latched,            ///< off until the next start (DropOff)
        KeysOwed,           ///< a drop's key-up has not gone out
        KeyClash,           ///< the jump, left, right or attack key is Ctrl
        NotWired,           ///< no map book or no veto: the command line's hunter
        NotPacketTarget,    ///< the target is not the packets' (a screen plate)
        LevelMonster,       ///< one to fight on the character's own level
        NotBelow,           ///< not more than max(down(), drop_min_px) below
        TooFarSide,         ///< more than drop_side_px across
        NotKnown,           ///< the packets do not know where the character is, or are unsure
        AfterEntry,         ///< a map entry under kDropAfterEntryMs ago
        NotSettled,         ///< not standing, with the stop of its walk reported
        InAir,              ///< a hop, or a drop, still in the air
        TooSoon,            ///< the last press under kDropGapMs ago
        StepJudge,          ///< a step back not judged yet
        Walking,            ///< a direction key still down
        MinuteCap,          ///< kDropsPerMinute presses in the last minute
        TargetCap,          ///< kDropsPerTarget presses, or kDropSettlesPerTarget settles, for this monster
        Held,               ///< a hold
        Unfocused,          ///< the game not in front
        Loot,               ///< a kill's pick-ups still owed
        Veto,               ///< GoBot says no (set_drop_veto)
        MapUnknown,         ///< no map file trusted for where the character is (MapTrust)
        OffFloor,           ///< where it stands is on no floor of that file
        Bottom,             ///< standing on solid ground: nothing to drop through
        NoLanding,          ///< no floor below, or not the same one either side
        ShortFall,          ///< the floor below is under kDropMinFallPx down
        PastMonster,        ///< the fall lands below the target's floor
        OutsideArea,        ///< with the spot: where it stands, lands or the target outside the area
        Gate,               ///< a gate within drop_gate_px
        NoDropHere,         ///< Ctrl + ↓ did not drop here before (GameMapBook::no_drop_near)
    };
    /// In the log's English.
    static const char* drop_why_name(DropWhy w);

    /// Why drops are off until the next start.
    enum class DropOff : std::uint8_t {
        None,
        Fails,          ///< kDropFailsInRow presses in a row, or kDropFailsPerRun, did not drop
        Rose,           ///< a press had the character rise: Ctrl + ↓ acts as a jump there
        OffMap,         ///< a fall landed where the map file did not say
        MapChanged,     ///< a press changed the map: the hunt stops (drop_stop)
        HeldLong,       ///< ↓ was held over kDropHeldMaxMs
        KeysStuck,      ///< a drop's key-up stayed owed: the hunt stops (drop_stop)
    };
    static const char* drop_off_name(DropOff o);

    /// What came of a press.
    enum class DropVerdict : std::uint8_t {
        None,
        Worked,         ///< landed on the floor the map file said
        Nothing,        ///< no take-off and no landing lower in kDropJudgeMs
        JumpInPlace,    ///< took off and came down where it stood
        Rose,           ///< came down higher
        OffMap,         ///< came down lower, not where the file said
        Gate,           ///< the map changed within kPressWatchMs
        Unknown,        ///< cut short, a hold, the game behind, the view or the packets lost
    };

    /// What GoBot's window is to do about the drop: stop hunting -- and with
    /// MapChanged untick drop_down and save it. Read from any thread.
    enum class DropStop : std::uint8_t { None, MapChanged, KeysStuck };
    DropStop drop_stop() const { return drop_stop_.load(); }

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
        /// Of targets_dropped, those in reach at some look of the last
        /// give_up_ms, and so picked straight back, never passed over: without
        /// a style, every fight at every give_up_ms, as it always was -- all
        /// 165 "given up" of the farm of 2026-11-03; with one, a target whose
        /// clock ran out through a hold or the game behind. targets_dropped
        /// less these is what was given up on for staying out of reach, or
        /// for taking no damage.
        std::uint64_t targets_repicked = 0;

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

        /// With NoTargets: a monster in view is being passed over a while --
        /// given up on a moment ago for staying out of reach, or for taking
        /// no damage -- as against none in view at all.
        bool passed_over = false;

        /// When "no monsters in view" began, on the hunter's clock; 0 once
        /// there is a target again.
        std::uint64_t no_target_since_ms = 0;

        /// The hunting spot (HunterConfig::spot): whether one is set, where,
        /// whether a map entry or a log in suspended it -- `spot_login` a log
        /// in (Spot::login) -- and why it does not steer the hunter now --
        /// SpotWhy::None when it does, or with none switched on.
        bool    spot_set = false;
        float   spot_x = 0.0f, spot_y = 0.0f;
        int     spot_half_w = 0, spot_half_h = 0;
        bool    spot_suspended = false;
        bool    spot_login = false;
        SpotWhy spot_why = SpotWhy::None;
        /// From the character to the spot, in world pixels, y growing
        /// downward, as of the last look that knew both -- the spot set and
        /// not suspended, the character Known; 0 otherwise.
        float   spot_dx = 0.0f, spot_dy = 0.0f;
        /// With NoTargets: the only monsters in view are outside the spot's
        /// area, and so not walked to.
        bool    outside_area = false;
        /// Walks back to the spot begun, and of them given up on (getting
        /// nowhere in spot_give_up_ms), since this run's start.
        std::uint64_t returns = 0;
        std::uint64_t return_give_ups = 0;

        /// "Nhảy xuống bậc dưới" (HunterConfig::drop_down): settling to press
        /// Ctrl + ↓, or falling from it; the presses this run, and of them
        /// those that landed where the map file said.
        bool          dropping = false;
        std::uint64_t drops = 0;
        std::uint64_t drops_worked = 0;
        /// Why the last target below was not dropped to, with the number that
        /// goes with it -- a gate's distance across, how far below -- and
        /// when, on the hunter's clock: 0 for none this run.
        DropWhy       drop_wait = DropWhy::None;
        int           drop_wait_px = 0;
        std::uint64_t drop_wait_ms = 0;
        /// Drops off until the next start, and why; with MapChanged or
        /// KeysStuck the hunt is to stop (drop_stop).
        DropOff       drop_off = DropOff::None;
        bool          drop_map_changed = false;
        bool          drop_keys_stuck = false;
        /// The map drops go by: its file, how it came to be trusted, or why
        /// none is (MapTrust::report). With drop_down off, as it was.
        MapTrust::Report drop_map;
    };
    Status status() const;

    /// The hunting spot as it is now. Read from any thread.
    Spot spot() const;
    /// "Đặt chỗ này": the spot is taken again where the character stands, at
    /// the next look that has it settled on the ground -- suspended or not,
    /// given up on or not. From any thread.
    void retake_spot();
    /// The character has been logged back in -- by the keeper, or in a game
    /// opened again and found by another window (GuiApp::retarget): where it
    /// now stands may be anywhere, so the spot stands aside (suspended, as a
    /// log in) until the character stands, settled, within kSpotBackPx of
    /// where it was last known -- or a map entry puts it there. One a map
    /// entry has suspended already is marked as a log in too, and keeps
    /// where the character left from. With no spot set, nothing. From any
    /// thread.
    void suspend_spot();

    /// The hunting spot's numbers, in world pixels.
    ///
    /// The current target is let go of as outside the area only this far
    /// past its edge: an edge a monster stands on would flip it in and out.
    static constexpr int kSpotKeepPx = 60;
    /// A spot higher above the character than this is out of one jump's
    /// reach (a jump tops out about 133 px up): not walked back to.
    static constexpr int kOneJumpPx = 120;
    /// Higher than this, the walk back hops on the way; level within it.
    static constexpr int kSpotFloorPx = 31;
    /// A walk back is getting somewhere while the reports put the character
    /// this much nearer the spot within spot_give_up_ms.
    static constexpr int kSpotProgressPx = 20;
    /// Walks back given up on in a row before no more are tried.
    static constexpr int kSpotMaxTries = 3;
    /// A map entry whose spawn is this close to where the character left the
    /// spot's map is a way back to it: a door there and back. After a log in
    /// (Spot::login), so is the character standing this close to where it
    /// was.
    static constexpr int kSpotBackPx = 150;
    /// Room left before the area's edge that a step back needs: a step is
    /// held 50 ms at the least, about 18 px, and the turn back to the target
    /// after it walks about 21 px of that back in (kTurnTapMs). With room for
    /// less, a step to the edge and the turn after it left room for another,
    /// and the character stepped out and turned in again until
    /// keep_distance_steps ran out.
    static constexpr int kSpotStepRoomPx = 39;

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
    /// from 50 to 800 ms. The step is held kTurnTapMs longer than this, for
    /// the turn back to the target that follows it.
    static int backstep_ms(int away, int dx);

    /// What the monster lists make of a plate.
    enum class Listed {
        Free,       ///< nothing: the lists have no say over it
        First,      ///< its name is on "Đánh trước"
        Skip,       ///< its name is on "Không đánh"
        NearSkip,   ///< nameless, right where one on "Không đánh" was a moment ago
        NotFirst,   ///< not on "Đánh trước", with only those to be attacked
        GaveUp,     ///< given up on a moment ago: no damage (a style's rule), or out of reach
        OutOfArea,  ///< outside the hunting spot's area, and not within reach
    };
    /// Not to be attacked.
    static bool refused(Listed l) {
        return l == Listed::Skip || l == Listed::NearSkip || l == Listed::NotFirst ||
               l == Listed::GaveUp || l == Listed::OutOfArea;
    }

    /// Which of `mobs` (nearest first) to go for, with no target yet: the
    /// nearest on the character's level, or the most wounded within reach
    /// with finish_wounded -- of those on "Đánh trước" when any are there; with
    /// none on the level, the nearest with health. Never one the lists
    /// refuse, nor one already finished. `me` null: where the character
    /// stands is not known, and only the last rule applies. `listed` is
    /// parallel to `mobs`. An index into `mobs`, or -1.
    ///
    /// A plate from the packets (Nameplate::slot 0 or more) is taken for one
    /// to finish only once your id has been proven the hitter's
    /// (Nameplate::actor_checked), and not while another player is hitting
    /// it: its bar falls under anyone's blows. `wounded_picked`, when given,
    /// is set when the most wounded was taken over the nearest.
    ///
    /// `below_first`: a drop could go now (drop_down on, not off for the run,
    /// the map trusted, the spot in use if on) -- with none on the level, the
    /// nearest from the packets more than max(down(), drop_min_px) below goes
    /// before any other, one above nearer by the plates' measure among them
    /// (the player's answer 1 of 2026-11-04).
    static int choose_target(const HunterConfig& cfg, const std::vector<Nameplate>& mobs,
                             const Nameplate* me, const std::vector<Listed>& listed,
                             bool* wounded_picked = nullptr, bool below_first = false);

    /// Standing still: of those within reach of `me` now, the nearest along
    /// the ground -- the nearest on "Đánh trước" if any is. -1 for none.
    static int choose_in_reach(const HunterConfig& cfg, const std::vector<Nameplate>& mobs,
                               const Nameplate& me, const std::vector<Listed>& listed);

    /// Where the nearest of `mobs` (nearest first) that may be attacked --
    /// living, and refused by nothing -- stands against `me`'s level: +1
    /// below it (dy over down()), -1 above it (-dy over up()), 0 on it or
    /// with none at all. Counted at every fresh pick for the minute line.
    static int nearest_off_level(const HunterConfig& cfg, const std::vector<Nameplate>& mobs,
                                 const Nameplate& me, const std::vector<Listed>& listed);

    /// Whether a living monster stands below `me`'s level (dy over down())
    /// within reach across (|dx| up to reach): one a drop could go to. One
    /// given up on a moment ago counts; not one the lists rule out, nor one
    /// outside the hunting spot's area.
    static bool below_in_reach(const HunterConfig& cfg, const std::vector<Nameplate>& mobs,
                               const Nameplate& me, const std::vector<Listed>& listed);

    /// Whether one of `mobs` stands on `me`'s level to be fought instead of
    /// dropping to a target below: from the packets (slot 0 or more),
    /// living, refused by nothing.
    static bool level_monster(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                              const std::vector<Listed>& listed);

    /// Everything a drop to the target below is judged by at one look
    /// (drop_rule), laid out so a test can set any one of them.
    struct DropFacts {
        bool          press = false;            ///< at the press -- its items too -- not as a settle begins
        std::uint64_t now = 0;                  ///< the hunter's clock
        bool          spot_use = false;         ///< the hunting spot in use (not standing for want of a fix)
        Spot          area;                     ///< and where it is
        bool          latched = false;          ///< drops off until the next start
        bool          owed = false;             ///< a drop's key-up not out
        bool          wired = false;            ///< a map book and a veto
        bool          packet_target = false;    ///< the target is the packets' own (slot 0 or more)
        int           dx = 0, dy = 0;           ///< the target from the character, as the plates have it
        float         target_wx = 0.0f;         ///< the target in the packets' world
        float         target_wy = 0.0f;
        bool          level_monster = false;    ///< one to fight on the level (level_monster)
        WorldFix      fix;                      ///< where the packets put the character
        std::uint64_t entry_ms = 0;             ///< when the hunter first saw this map entry
        std::uint64_t settle_from_ms = 0;       ///< when the settle began
        bool          walk_held = false;        ///< a walk was held as it began: its stop must be reported
        bool          in_air = false;           ///< a hop's or a drop's
        std::uint64_t last_press_ms = 0;        ///< 0 for none this run
        bool          step_judge = false;       ///< a step back not judged yet
        bool          left_held = false;
        bool          right_held = false;
        int           presses_minute = 0;       ///< in the last minute
        int           presses_target = 0;       ///< for this monster
        int           settles_target = 0;       ///< for this monster, let go of without a press
        bool          held = false;             ///< a hold
        bool          focused = true;
        bool          loot_owed = false;
        const char*   veto = nullptr;
        /// The trusted map's file -- null: none trusted -- with its gates and
        /// those learned, and whether a no-drop point is near where it stands.
        std::shared_ptr<const GameMapBook::Page> page;
        std::vector<MapGate> gates;
        bool          no_drop_here = false;
    };
    /// What drop_rule made of them.
    struct DropCall {
        DropWhy why = DropWhy::None;
        int     px = 0;             ///< with the reason: a gate's distance, how far below or across, ...
        int     x0 = 0, y0 = 0;     ///< where the character was judged to stand, world pixels
        int     stand_y = -1;       ///< the floor under it, -1 not known
        int     land_y = 0;         ///< the floor the file says the fall lands on
        bool    direct = false;     ///< the target's own floor, not one between
        int     gate_px = -1;       ///< the nearest gate across in the fall's band; -1 none
        bool ok() const { return why == DropWhy::None; }
    };
    /// The drop plan's §3.2, in its order, with the player's answers of
    /// 2026-11-04: the first rule that does not hold. Pure: facts in, a
    /// reason out. `f.press` checks the items a press needs over those a
    /// settle begins with -- the character settled, its stop reported, no
    /// direction key down, standing on a floor of the file, the landing
    /// read at kDropMarginPressPx either side -- and kDropPressSlackPx more
    /// across. Where it stands is the fix worked out to now as a settle
    /// begins, and the last report's own at the press.
    static DropCall drop_rule(const HunterConfig& cfg, const DropFacts& f);

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
    /// itself may have left held, a held walk among them. Not release_all(),
    /// which would cut short a key another thread is in the middle of
    /// pressing -- a potion among them.
    void let_go_of_moves(const HunterConfig& cfg);

    /// What a walk goes to: a monster, or the hunting spot. A walk to the
    /// spot hops by the spot's height, never toward a monster with the spot
    /// on, and is renewed every kWalkRenewMs as one the packets steer is --
    /// the reports of its stop and start are what its give-up goes by.
    enum class WalkFor : std::uint8_t { Target, Spot };

    /// One look's walk toward a target `dx`, `dy` away: the key held from the
    /// last look kept down, or pressed down -- or, with hold_walk off, a step
    /// of step_ms -- and a hop when the target is above.
    void walk(const HunterConfig& cfg, int dx, int dy, std::uint64_t now, WalkFor what = WalkFor::Target);
    /// This look's walk, for the steps back (Retreat): `side` 0 to the left
    /// and 1 to the right, `moved` whether the character is walking that way
    /// now. Held kRegainWalkMs over looks in a row, it counts the steps back
    /// the other way afresh.
    void note_walk(int side, bool moved, std::uint64_t now);
    /// A hop toward a target `dy` away, with jump_to_reach on and a jump key
    /// set, if it is above and out of reach of the ground and the last hop is
    /// not still in the air (in_air). Toward the spot the same, by its height:
    /// if it is more than kSpotFloorPx above -- with jump_to_reach off, no hop
    /// toward the spot either.
    void hop(const HunterConfig& cfg, int dy, std::uint64_t now, WalkFor what);
    /// Whether the last jump the hunter pressed may still have the character
    /// in the air at `now`: kJumpAirMs from it -- or its last drop, until a
    /// landing is reported or kDropFallMaxMs from ↓ going down.
    bool in_air(std::uint64_t now) const {
        return (last_hop_ms_ != 0 && now - last_hop_ms_ < kJumpAirMs) || drop_in_air(now);
    }
    bool drop_in_air(std::uint64_t now) const {
        return drop_t_down_ != 0 && !drop_landed_ && (now < drop_t_down_ || now - drop_t_down_ < kDropFallMaxMs);
    }

    /// One press of the pick-up key, if the input is free. True if it went.
    bool press_loot(const HunterConfig& cfg);

    /// Through the seams, or the real thing when one is not set.
    bool                   see_player(Nameplate& out, bool* fresh = nullptr) const;
    std::vector<Nameplate> see_mobs() const;
    bool                   focused() const;
    bool                   press(std::uint32_t vk, int hold_ms);
    bool                   hold_down(std::uint32_t vk);
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

    /// Those of `mobs` where a target was given up on a moment ago, marked
    /// GaveUp in `listed` -- each spot followed to the nearest plate as it
    /// moves, and forgotten after no_damage_ms for taking no damage, twice
    /// give_up_ms for staying out of reach. One on "Đánh trước" only for the
    /// second: the player asked for it by name, and only one it cannot get
    /// to is let go of.
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
    /// `n` is the target from here on: where it stands, and -- a plate from
    /// the packets -- its slot and generation, which the next look finds it
    /// by (find_target).
    void aim_at(const Nameplate& n);
    /// Where among `mobs` the target is now, -1 when it is not there: by its
    /// slot and generation when it has a slot and the plates carry slots --
    /// within kSlotSamePx of where it was, `slack` more -- and otherwise the
    /// plate nearest where it was, within kSameTargetPx and `slack`, as it
    /// always was. `respawned`: its slot was there with another generation.
    /// `nearest`: the plate nearest where it was, whatever the rule.
    int find_target(const std::vector<Nameplate>& mobs, int slack, bool* respawned = nullptr,
                    int* nearest = nullptr) const;
    /// A look with no target to be had -- no monster in view, or none that
    /// may be attacked: the fight goes on, unless its monster has not been
    /// seen for kPlayerMemoryMs. A plate drops out for a look or two all the
    /// time, and each such look ended the fight, splitting its line in the
    /// log in two.
    void fight_unseen(std::uint64_t now, const char* why);
    /// An attack went out with the target `dx` to the side.
    void note_attack(int dx);
    /// With a style: whether `target` has been attacked for no_damage_ms
    /// without its bar going down -- or, on a plate from the packets, without
    /// a blow of yours on it (Nameplate::hurt_ms) either.
    bool no_damage(const HunterConfig& cfg, const Nameplate& target, std::uint64_t now);
    /// The step back still to be judged follows `target` as the fight does,
    /// by where it is; one somewhere else, or after kPlayerMemoryMs unseen,
    /// and it is not judged. The count of steps (Retreat) is not this.
    void follow_back_steps(const Nameplate& target, std::uint64_t now, int slack);
    /// The no-damage watch follows `target` the same way.
    void follow_watch(const Nameplate& target, std::uint64_t now, int slack);
    /// What the last step back from the target ran into, by what has moved
    /// on screen since: "a wall" when nothing has, "a ledge" when the height
    /// has changed more than a step can. Null when neither.
    const char* step_went_wrong(const Nameplate& me, const Nameplate& target) const;
    /// The number of the picture the plates come from: see Eyes::frame.
    std::uint64_t picture() const;
    /// The plates are in another frame of reference than at the last pass
    /// (Eyes::view): the packets have taken over from the screen, or the
    /// screen from them. Everything held by where things stood means nothing
    /// in it and is let go of -- the target and whether it is on "Đánh
    /// trước", the spots skipped or given up on, the fight's log, the damage
    /// watch, the steps back and their slack. Not as a kill: no loot is owed
    /// and nothing is counted finished, or every switch would read as one.
    /// Which way the character faces, the loot still owed, engaged() and
    /// every count stay.
    void forget_positions();
    /// The minute line's counts for monsters off the character's level: a
    /// fresh pick taken up (nearest_off_level), and a pass with nothing to
    /// attack in reach (below_in_reach), timed pass to pass as walk_run_ms_.
    void note_pick(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                   const std::vector<Listed>& listed);
    void note_wait(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                   const std::vector<Listed>& listed, std::uint64_t now);
    /// Once a minute while hunting, one line of how the fights went.
    void minute_log(const HunterConfig& cfg, std::uint64_t now);
    /// Before the turn to a target in reach, with the packets steering
    /// (packet_positions=on): which way they have the character facing
    /// (WorldFix::facing), taken over facing_ when a report said so more than
    /// kFacingTrustMs after the hunter's own last key that set it
    /// (facing_key_ms_), when it says the way that key went
    /// (facing_key_way_), or, saying the other way, once kFacingMissedMs show
    /// the game never took the key. The fix this pass's track_spot took with
    /// the spot on; with it off, Eyes::where asked here. With none to be had,
    /// the packets not knowing the character, or watched only, facing_ stays
    /// as it is.
    void face_from_packets(const HunterConfig& cfg);

    // ---- the hunting spot (HunterConfig::spot)

    /// What the spot makes of a pass.
    enum class SpotMode : std::uint8_t {
        Off,    ///< none switched on: the hunter as it was
        Free,   ///< no report of yours yet: hunting as without a spot, the first walk's report places you
        Stand,  ///< nothing to go by: standing, as stay_put stands
        Use,    ///< the area bounds the walking, and the character walks back to the spot
    };
    /// Once a pass with the spot on, before anything is looked at: where the
    /// packets put the character (fix_), the map entries and log ins since
    /// the last pass, the spot taken when it is to be, and this pass's mode.
    void track_spot(const HunterConfig& cfg);
    /// Those of `mobs` outside the spot's area and out of reach of `me`,
    /// marked OutOfArea in `listed` -- the current target, at `target_at`,
    /// only kSpotKeepPx past the edge.
    void mark_out_of_area(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                          int target_at, std::vector<Listed>& listed) const;
    /// With nothing to fight: the walk back to the spot, if it is to be had
    /// -- or kept up, or let go of there. True while walking back; false, and
    /// the caller lets go of the keys as before.
    bool go_home(const HunterConfig& cfg, std::uint64_t now);
    /// Something to fight this pass: the idle time before a walk back counts
    /// from here, and a walk back under way is over.
    void spot_busy(std::uint64_t now);
    /// The walk back under way is over: arrived, given up, or overtaken.
    void end_return();
    /// The spot's numbers into status_.
    void publish_spot();

    // ---- the drop (HunterConfig::drop_down)

    /// drop_down on, the packets steering, not standing still: what asks
    /// anything new of the eyes.
    static bool drop_wanted(const HunterConfig& cfg);
    /// Once a pass, before a hold or the focus is looked at, with drops
    /// wanted or a press of the last kPressWatchMs: the keys a drop still
    /// owes sent again, where the packets put the character (drop_fix_), the
    /// map entry's age, a map change since a press (Gate), and the map drops
    /// go by (trust_).
    void drop_watch(const HunterConfig& cfg, std::uint64_t now);
    /// Once a pass, after minute_log: a fall judged, and a settle that
    /// drop_toward did not renew at the last pass let go of. No key.
    void drop_follow(const HunterConfig& cfg, std::uint64_t now);
    /// In the walk branch, before walk(): a target below, dropped to when
    /// drop_rule says -- settled first, then Ctrl + ↓. True while it settles,
    /// presses or falls: the pass walks no further.
    bool drop_toward(const HunterConfig& cfg, const Nameplate& target, const Nameplate& me,
                     const std::vector<Nameplate>& mobs, const std::vector<Listed>& listed, int dx, int dy,
                     std::uint64_t now);
    /// The facts for drop_rule, at this look.
    DropFacts drop_facts(const HunterConfig& cfg, const Nameplate& target, const Nameplate& me,
                         const std::vector<Nameplate>& mobs, const std::vector<Listed>& listed, int dx, int dy,
                         std::uint64_t now, bool press);
    /// The key, the settle over: false when it was given up on and the pass
    /// may walk.
    bool press_drop(const HunterConfig& cfg, const DropCall& call, const Nameplate& target, int dx, int dy,
                    std::uint64_t now);
    /// A settle over without a press; with `backoff`, none again for
    /// kDropSettleBackoffMs. Counted against kDropSettlesPerTarget, and the
    /// give-up clock the settle held still given back what it had, with the
    /// settle's time not counted.
    void end_settle(const char* why, bool backoff, std::uint64_t now);
    /// A hold, the game behind, a view change, a log in: a settle is let go
    /// of, a fall is Unknown. The press record and the in-air guard stay.
    void drop_hold(const char* why);
    /// The fall judged `v` -- landed at `rx`, `ry` -- with what it learns and
    /// counts.
    void end_fall(const HunterConfig& cfg, DropVerdict v, std::uint64_t now, int rx, int ry, const char* why);
    /// A map change within kPressWatchMs of a press, whatever was judged
    /// before: undone, both ends learned as gates, drops off and the hunter
    /// halted for GoBot's window to stop the hunt.
    void drop_gate(std::uint64_t now);
    /// Drops off until the next start.
    void latch_drops(DropOff why);
    /// Why the target `dy` below is not dropped to: into status_, and said in
    /// the log at each change of reason -- at INFO once per target for what
    /// the player can do something about, DEBUG otherwise.
    void drop_refused(const HunterConfig& cfg, const DropCall& call, const Nameplate& target, int dy,
                      std::uint64_t now);
    /// The presses of the last minute, and for the monster `slot`, `gen`; its
    /// settles let go of without a press.
    int drop_presses_minute(std::uint64_t now);
    int drop_presses_for(int slot, std::uint32_t gen) const;
    int drop_settles_for(int slot, std::uint32_t gen) const;
    /// The minute line's part for drops: "" with no press this minute.
    std::string drop_minute_text() const;

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
    /// tick and stand still vibrating. A plate from the packets is remembered
    /// by its slot and generation as well, -1 and 0 for the screen's: two
    /// monsters crossing are not swapped, and a respawn where the target died
    /// is not taken for it (aim_at, find_target).
    int           target_x_ = 0, target_y_ = 0;
    int           target_slot_ = -1;
    std::uint32_t target_gen_ = 0;
    bool          have_target_ = false;
    std::uint64_t target_since_ms_ = 0;
    /// The last look the target was within reach, or its pick, or a look the
    /// hunter could not walk (a hold, the game out of focus): given up on with
    /// this more than give_up_ms ago, it was out of reach the whole time.
    std::uint64_t reach_ms_ = 0;
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
    std::uint64_t loot_since_ms_ = 0;   ///< since when they are stood for (hold_walk); 0 when not

    // ---- read by other threads ----
    std::atomic<bool>          engaged_{false};
    std::atomic<int>           engaged_dx_{0};
    std::atomic<std::uint32_t> attack_key_{'Z'};

    // ---- the hunter's own thread's, or step()'s ----
    std::uint64_t last_engaged_ms_ = 0;

    /// Which way the character faces, as far as the hunter knows: set by its
    /// own steps and turns, unknown at a start, with the game out of focus or
    /// the character out of sight, and once a hold is over -- whatever ran in
    /// it may have turned the character. The character turns only when this
    /// says it is not facing the target, whatever the setup. Where the
    /// packets steer and know better, they put it right (face_from_packets):
    /// a turn the hunter's key missed, or one something else made -- and
    /// fill it in where it is not known.
    enum class Facing { Unknown, Left, Right };
    Facing facing_ = Facing::Unknown;
    /// When the last key of the hunter's own that set facing_ went out -- a
    /// turn's tap, a walk put down or stepped, a step back -- on the hunter's
    /// clock, 0 for none this run; and which way it went, Unknown for none.
    /// What a report of yours is weighed against (kFacingTrustMs,
    /// kFacingMissedMs).
    std::uint64_t facing_key_ms_ = 0;
    Facing        facing_key_way_ = Facing::Unknown;
    bool   held_before_ = false;        ///< a hold was on at the last pass
    /// The direction key held down for a walk (hold_walk), 0 for none, and
    /// since when -- or since its last renewal; how far off the target was at
    /// the last mark of progress, and when (kWalkStuckMs); and the last hop.
    /// Set through set_walk, which keeps walk_until_ms_ with it.
    std::uint32_t walk_vk_ = 0;
    std::uint64_t walk_since_ms_ = 0;
    int           walk_adx_ = 0;
    std::uint64_t walk_progress_ms_ = 0;
    std::uint64_t last_hop_ms_ = 0;
    std::atomic<std::uint64_t> walk_until_ms_{0};
    void set_walk(std::uint32_t vk);
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
        int              slot = -1;             ///< and which it was, from the packets (aim_at)
        std::uint32_t    gen = 0;
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
        int           slot = -1;            ///< and which it was, from the packets
        std::uint32_t gen = 0;
        std::uint64_t seen_ms = 0;
        /// The last blow of yours on it the packets had said (Nameplate::hurt_ms).
        std::uint64_t hurt_seen_ms = 0;
    };
    DamageWatch watch_;

    /// The last step back, still to be judged on the first picture taken
    /// wholly after it -- no step goes before -- with where things stood
    /// before it and which way it went (Retreat's index). Judged by the
    /// monster it was taken from, which is followed from look to look as the
    /// target is, by where it stands; another one, or this one unseen for
    /// MobDetector::kPlayerMemoryMs, and the step goes unjudged.
    struct BackSteps {
        int           x = 0, y = 0;         ///< where the monster was last seen
        int           slot = -1;            ///< and which it was, from the packets
        std::uint32_t gen = 0;
        std::uint64_t seen_ms = 0;
        bool          judge = false;
        std::uint64_t judge_from = 0;
        int           way = 0;
        int           from_me_x = 0, from_me_y = 0, from_x = 0, from_dy = 0;
    };
    BackSteps back_;
    /// The steps back taken one way since the character last walked the
    /// other way for kRegainWalkMs, from whichever monsters -- [0] to the
    /// left, [1] to the right: how many, and whether one met a wall or went
    /// down a ledge, after which none goes that way until such a walk. The
    /// bound is all that stands between the character and an edge GoBot
    /// cannot see. It was the steps in a row from one monster, counted afresh
    /// at every attack from far enough and every new monster: a monster that
    /// walked back in after each attack, or one after another from the same
    /// side, took the character back across its platform three steps at a
    /// time. Not a matter of where things stand on the screen, so a change of
    /// frame of reference (forget_positions) leaves it; a start clears it.
    struct Retreat {
        int  in_row = 0;
        bool blocked = false;
    };
    Retreat retreat_[2];
    /// The walk under way, for Retreat: which way (0 left, 1 right), how long
    /// it has been held over looks in a row, the pass it was last held on and
    /// when. walk_pass_ 0 is none.
    int           walk_side_ = 0;
    std::uint64_t walk_run_ms_ = 0;
    std::uint64_t walk_pass_ = 0;
    std::uint64_t walk_at_ms_ = 0;
    /// Passes run, for the picture's number with scripted plates.
    std::uint64_t passes_ = 0;
    /// Eyes::view as the last pass saw it.
    std::uint32_t view_ = 0;

    /// Where targets were given up on: for taking no damage, or, `beyond`,
    /// for staying out of reach.
    struct GaveUpSpot {
        int           x = 0, y = 0;
        std::uint64_t until_ms = 0;
        bool          beyond = false;
    };
    std::vector<GaveUpSpot> gave_up_;

    /// What the once-a-minute line says.
    std::uint64_t minute_from_ms_ = 0;
    int           minute_fights_ = 0;
    int           minute_far_ = 0;          ///< of them, fought from keep_away or further
    int           minute_backsteps_ = 0;
    std::uint64_t minute_dropped_ = 0;      ///< targets_dropped when the minute began
    int           minute_returns_ = 0;      ///< walks back to the spot begun
    int           minute_return_give_ups_ = 0;
    int           minute_turns_ = 0;        ///< turn taps sent (kTurnTapMs)
    /// A facing_ of Left or Right the packets put the other way
    /// (face_from_packets) -- not one filled in where it was not known; and
    /// of those, the ones for a key the game never took (kFacingMissedMs).
    int           minute_packet_facings_ = 0;
    int           minute_missed_keys_ = 0;
    /// The most wounded taken over the nearest (choose_target); and, with the
    /// packets' plates, the target kept by its slot, the looks at which the
    /// plate nearest where it was was another monster (a swap refused), and
    /// those at which its slot held a new generation (a respawn, not it).
    int           minute_wounded_ = 0;
    int           minute_slot_kept_ = 0;
    int           minute_swaps_refused_ = 0;
    int           minute_new_gen_ = 0;
    std::uint64_t minute_repicked_ = 0;     ///< targets_repicked when the minute began
    /// Monsters off the character's level, for the drop-down to come (the
    /// farm of 2026-11-03 could not tell): the fresh picks taken up, and of
    /// them those whose nearest monster stood below the level band or above
    /// it (nearest_off_level); and the time spent with nothing in reach while
    /// one stood below within reach across (below_in_reach), the pass it was
    /// last seen at and when.
    int           minute_picks_ = 0;
    int           minute_nearest_below_ = 0;
    int           minute_nearest_above_ = 0;
    std::uint64_t minute_below_wait_ms_ = 0;
    std::uint64_t below_wait_pass_ = 0;
    std::uint64_t below_wait_at_ms_ = 0;

    /// The hunting spot, and what the other threads ask of it: under mutex_.
    Spot spot_;
    bool retake_asked_ = false;
    bool suspend_asked_ = false;
    /// This pass's fix (Eyes::where) and what the spot makes of the pass.
    WorldFix fix_;
    SpotMode spot_mode_ = SpotMode::Off;
    SpotWhy  spot_why_ = SpotWhy::None;
    /// The map entries the last pass had seen (WorldFix::epoch), once one
    /// has; and where the character was last known (a Known fix): where it
    /// left the spot's map from at an entry.
    bool          epoch_seen_on_ = false;
    std::uint32_t epoch_seen_ = 0;
    float         known_x_ = 0.0f, known_y_ = 0.0f;
    /// A spot a log in suspended (Spot::login): whether a look from after the
    /// log in has come since -- one at which the packets did not know the
    /// character, as a capture started again on the new game begins, a map
    /// entry, or a report of yours newer than the last before the log in (the
    /// same game found again, its capture going on). Only such a look puts
    /// the spot back in use: a fix still from the old game is not the
    /// character come back.
    bool          login_gap_ = false;
    std::uint64_t login_report_ms_ = 0;     ///< the fix's report_ms as the log in set the spot aside
    /// The last look with something to fight: the walk back waits
    /// spot_return_delay_ms from it.
    std::uint64_t spot_busy_ms_ = 0;
    /// A walk back under way -- its give-up clock running, whether this
    /// pass walks or not -- and whether this pass and the last walked back.
    /// Its give-up: how far off the spot the reports put the character at the
    /// last kSpotProgressPx of progress, the walking time since, and the pass
    /// that counted last; the give-ups in a row, and when the next try may be.
    bool          return_on_ = false;
    bool          returning_ = false;
    bool          returned_last_ = false;
    float         return_mark_px_ = 0.0f;
    std::uint64_t return_quiet_ms_ = 0;
    std::uint64_t return_pass_ = 0;
    std::uint64_t return_seen_ms_ = 0;
    int           return_strikes_ = 0;
    std::uint64_t return_retry_ms_ = 0;
    bool          said_too_high_ = false;

    // ---- the drop (HunterConfig::drop_down): the hunter's own thread's

    /// The game's map files and GoBot's own say against a drop: both set, or
    /// no drop (set_map_book, set_drop_veto). Which map the character is on,
    /// and whether its file is gone by.
    GameMapBook*                 book_ = nullptr;
    std::function<const char*()> drop_veto_;
    MapTrust                     trust_;
    /// This pass's fix for the drop -- the spot's stays fix_ -- and whether
    /// the eyes gave one; the map entry it is in, and since when on the
    /// hunter's clock: the first look of it, at a start too.
    WorldFix      drop_fix_;
    bool          drop_fix_on_ = false;
    bool          drop_entry_on_ = false;
    std::uint32_t drop_entry_epoch_ = 0;
    std::uint64_t drop_entry_ms_ = 0;

    /// A drop under way: settling to press -- since when, whether a walk was
    /// held as it began, the pass that last renewed it, since when the token
    /// was refused, and the give-up clock as it found it -- or falling: when
    /// ↓ went down, the take-offs then, where it stood, where the file said it
    /// lands, and the target.
    enum class DropPhase : std::uint8_t { Idle, Settle, Fall };
    struct DropState {
        DropPhase     phase = DropPhase::Idle;
        std::uint64_t settle_from_ms = 0;
        std::uint64_t since0 = 0, reach0 = 0;   ///< target_since_ms_, reach_ms_ as the settle began
        bool          walk_held = false;
        std::uint64_t renewed_pass = 0;
        std::uint64_t token_from_ms = 0;
        int           slot = -1;
        std::uint32_t gen = 0;
        std::uint64_t down_ms = 0;
        bool          takeoffs_known = false;
        std::uint64_t takeoffs0 = 0;
        bool          took_off = false;
        int           x0 = 0, y0 = 0, land_y = 0;
        std::string   stem;
    };
    DropState drop_;
    /// A press, watched for kPressWatchMs for a map change whatever was
    /// judged of it: kept apart from drop_ -- forget_positions, a start and
    /// every verdict leave it -- with what its verdict learned and counted,
    /// for a map change to undo.
    struct PressRecord {
        bool          open = false;
        std::uint64_t down_ms = 0;
        std::uint32_t epoch0 = 0;
        int           x0 = 0, y0 = 0;
        std::string   stem;
        DropVerdict   verdict = DropVerdict::None;
        bool          no_drop = false;
        bool          failed = false;
        std::uint64_t minute_from = 0;      ///< the minute its verdict was counted in
    };
    PressRecord press_;
    /// The presses of the last minute, and those for each monster this run --
    /// with its settles let go of without a press.
    struct TargetPresses {
        int           slot = -1;
        std::uint32_t gen = 0;
        int           n = 0;
        int           settles = 0;
    };
    std::vector<std::uint64_t> drop_presses_;
    std::vector<TargetPresses> drop_targets_;
    std::uint64_t last_press_ms_ = 0;
    /// When the last ↓ went down, on the hunter's clock, and whether a
    /// landing since was reported: in_air.
    std::uint64_t drop_t_down_ = 0;
    bool          drop_landed_ = false;
    std::uint64_t last_drop_worked_ms_ = 0;
    std::uint64_t settle_backoff_ms_ = 0;   ///< no settle begun before this
    int           drop_fails_row_ = 0;
    int           drop_fails_run_ = 0;
    DropOff       drop_latch_ = DropOff::None;
    /// Halted for GoBot's window to stop the hunt: a map change, or a key-up
    /// still owed (drop_stop_). Every pass lets go of the moves and returns.
    bool          drop_halt_ = false;
    int           owed_passes_ = 0;
    /// A log in since the last pass (suspend_spot): under mutex_.
    bool          drop_suspend_asked_ = false;
    std::atomic<DropStop> drop_stop_{DropStop::None};
    /// The refusal said for the target: the last reason, and those said at
    /// INFO (a bit for each DropWhy).
    int           drop_said_slot_ = -2;
    std::uint32_t drop_said_gen_ = 0;
    DropWhy       drop_said_why_ = DropWhy::None;
    std::uint64_t drop_said_info_ = 0;
    /// The minute line's: presses, and what came of them.
    int           minute_drops_ = 0;
    int           minute_drop_worked_ = 0;
    int           minute_drop_nothing_ = 0;
    int           minute_drop_jump_ = 0;
    int           minute_drop_rose_ = 0;
    int           minute_drop_off_map_ = 0;
    int           minute_drop_gate_ = 0;
    int           minute_drop_unknown_ = 0;
};

} // namespace gobot
