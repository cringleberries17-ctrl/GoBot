#pragma once

#include "gobot/execution_coordinator.hpp"
#include "gobot/hunt_seek.hpp"
#include "gobot/hunt_zone.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/map_book.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/packet_world.hpp"

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

    /// "Nhảy lên bục trên" (2026.11.05b): jump straight up -- ↑, ↑↑ or Ctrl
    /// + ↑, chosen by the game's own map file with a margin (jump_plan.hpp)
    /// -- onto the floor of a monster above, and onto the floors of a way
    /// back to the home. Only with the map tab's area in use on a map
    /// MapTrust trusts, packet_positions=on, stay_put off and the jump key ↑;
    /// the take-off, the landing and the monster inside the area. On by
    /// default -- the owner's answer 5 of 2026-11-05, "Bật sẵn" -- and a
    /// hunt.ini without it reads true. Off, or off for the run, nothing new
    /// is pressed: the log says where a jump would have gone. Independent of
    /// jump_to_reach, which is the old hop's alone.
    bool jump_up = true;

    /// "Tự tìm quái trong vùng" (2026.11.05b): with the map tab's area in
    /// use, the packets steering and stay_put off, go to any monster inside
    /// its box the character gets to -- in view or not -- along its floor,
    /// off a floor's end onto one below inside the box, or by a jump up when
    /// jump_up may -- and fight it from a standing point on a floor inside the
    /// box (hunt_seek.hpp); with nothing it gets to, stand where it is, inside
    /// the box. Never out of the box. On by default -- the owner's Q7 (a), and
    /// a hunt.ini without it reads true. Off: the hunter of 2026.11.05 and its
    /// jumps up, exactly. This run's spot, no spot, the screen's plates, Watch
    /// and the lists are as they were either way.
    bool seek_in_zone = true;

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
    /// switched off, the spot is dropped: nothing of it is saved. A map with
    /// a hunting area drawn on the map tab has that area instead, by the
    /// rule of Hunter::set_zone; spot_half_w and spot_half_h are then the
    /// size of a new one. That area, with its map's file gone by (2026.11.04d,
    /// the owner's answers (a) until answered): no walk toward a monster
    /// leaves its box (Q1); outside it with a way back the map shows, the
    /// walk back -- along the floor, never a hop -- comes before any fight,
    /// in reach or not (Q2); and home is by height as well as across.
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
        /// With drop_down on (packets On, stay_put off) -- or the map tab's
        /// area in use with them, for the jumps up (jump_watch) -- also asked
        /// once at the start of each pass -- a second time with the spot on --
        /// and for kPressWatchMs after a drop's or a jump's key whatever the
        /// setup;
        /// face_from_packets may then ask again in the in-reach branch, as it
        /// does with the spot off. With drop_down off, nothing new is asked.
        /// Unset, or false, there is no capture to say: NoCapture, and the
        /// hunter's own idea of the facing stands.
        std::function<bool(WorldFix&)>          where;
        /// The game's own count of your take-offs (GuiApp::packet_takeoffs):
        /// asked as a drop's key goes out and while it falls, for whether
        /// the character left the ground. Unset, or none: not known.
        std::function<std::optional<std::uint64_t>()> takeoffs;
        /// The monsters where their own frame put them, in the packets' world
        /// pixels (PacketEyes::stands): standing points for the map drops go
        /// by once two frames of one monster agree (MapTrust). Asked once a
        /// pass with drop_down on. Unset: none.
        std::function<std::vector<MapTrust::Mob>()> stands;
        /// For the jumps up (2026.11.05b): your take-offs and landings by
        /// kind (GuiApp::packet_air_counts) -- nothing when the packets
        /// cannot tell -- and your own reports since a moment on the frames'
        /// clock, oldest first (GuiApp::packet_own_since): what a flight is
        /// judged by. Asked as a jump's key goes out and while it is in the
        /// air. Either unset: no jump up is ever pressed (NotWired).
        std::function<std::optional<PacketWorld::AirCounts>()>           air;
        std::function<std::vector<PacketWorld::OwnTrace>(std::uint64_t)> own_since;
        /// For "Tự tìm quái trong vùng" (2026.11.05b): every living monster of
        /// the map, placed on its floor (PacketEyes::world) -- no box round
        /// you. Asked once a pass, after `mobs`, with seek_in_zone on and the
        /// map tab's area in use. True only while the packets steer: false, or
        /// unset, and the seek is not on -- the hunter as before it.
        std::function<bool(std::vector<SeekMob>&)> world;
    };
    /// How one jump up went out (Hands::jump).
    struct JumpKeys {
        bool          went = false;     ///< its first ↑ went down
        /// All of it: ↑↑'s second ↑ as well, Ctrl + ↑'s ↑ held its time
        /// inside Ctrl. ↑↑ without its second is judged as a plain ↑.
        bool          whole = false;
        std::uint64_t down_ms = 0;      ///< the first ↑'s down, on the frames' clock
        std::uint64_t last_ms = 0;      ///< the last ↑'s down: ↑↑'s second, else down_ms
        std::uint32_t held_ms = 0;      ///< the longest an ↑ of it was held
        /// Ctrl + ↑: Ctrl alone before ↑, and from ↑'s up to Ctrl's.
        std::uint32_t lead_ms = 0;
        std::uint32_t tail_ms = 0;
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
        /// Whether what stays owed is a high jump's ↑ (InputManager::up_owed):
        /// the words a hunt stopped on it goes by (2026.11.05b).
        std::function<bool()>                   up_owed;
        /// One jump up of a kind, always ↑, with what is asked through it:
        /// ↑ InputManager::jump_once, ↑↑ jump_twice at kJumpUpSecondAtMs,
        /// Ctrl + ↑ high_jump. Never InputManager::jump() nor double_jump().
        std::function<JumpKeys(JumpKind, const std::function<bool()>&)> jump;
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
    /// Told of each press of the attack key the hunter itself makes, with its
    /// key-down's time on the hunter's clock -- for the log's pairing of your
    /// casts with the keys that made them (CastMatch, 2026.11.04d), which had
    /// the macro's presses only. On the hunter's thread: whatever it is
    /// answers at once and presses nothing. Set before start(); unset, nobody
    /// is told.
    void set_on_attack(std::function<void(std::uint32_t vk, std::uint64_t at_ms)> f) { on_attack_ = std::move(f); }

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
    /// The monster's floor (monster_floor): the floor under where it last
    /// stood, else the first of its column no more than kDropFloorMatchPx
    /// above its point and no more than this below it. Its frames put it off
    /// its floor -- a contact frame anywhere near it, a knock, a hop -- and a
    /// walk moves its x alone: three of the four monsters of 2026-11-04 20:40
    /// were 61-78 px over theirs.
    static constexpr int           kDropMonsterFloorPx = 100;
    /// The owner's Q6 of 04d ((a) until answered): a target below a drop
    /// could go to (drops_may_go) no more than this across, the walk is let
    /// go of above it, and the drop judged from where the character stands
    /// then -- settled, kDropMarginPressPx either side -- not along a walk
    /// that never stops there. 0 is (b): walked on, as before.
    static constexpr int           kDropStopAbovePx = 24;
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

    // ---- the jump up onto a floor above (HunterConfig::jump_up): the jump-up
    // plan's §6, §7 and §11; the shared numbers -- kDropGapMs, kPressWatchMs,
    // kDropAfterEntryMs, kDropSettleMs, kDropStayMs, kDropOwedPasses,
    // kDropSameFloorPx -- are the drop's above

    /// Within this across of the take-off the walk to it is let go of and the
    /// stop waited for (kDropSettleMs); stopped off a column that passes, it
    /// is tapped toward it this many times at most, each clamp(|dx| x 1000 /
    /// 360, 60, 200) ms -- then not again for kJumpUpSettleBackoffMs. The
    /// coordinator's token, refused, is waited for kJumpUpTokenWaitMs.
    static constexpr int           kJumpUpLeadPx = 24;
    static constexpr int           kJumpUpFineTapsMax = 2;
    static constexpr std::uint64_t kJumpUpSettleBackoffMs = 2000;
    static constexpr std::uint64_t kJumpUpTokenWaitMs = 1000;
    /// No take-off this long after the last key: the key did nothing. Still
    /// in the air this long after the first: what came of it is not known.
    /// ↑↑'s second jump within this of the first take-off.
    static constexpr std::uint64_t kJumpUpJudgeMs = 900;
    static constexpr std::uint64_t kJumpUpAirMaxMs = 2200;
    static constexpr std::uint64_t kJumpUpSecondSeenMs = 700;
    /// Landed within this of the take-off's height on its floor: fell back.
    /// On the planned floor within this of its top there: worked.
    static constexpr int           kJumpUpLandPx = 24;
    static constexpr int           kJumpUpFloorMatchPx = 12;
    /// No more than this many presses in a minute, nor for one monster, nor
    /// settles let go of for one monster without a press.
    static constexpr int           kJumpUpsPerMinute = 4;
    static constexpr int           kJumpUpsPerTarget = 2;
    static constexpr int           kJumpUpSettlesPerTarget = 2;
    /// After a jump toward a target that worked, no drop for this long
    /// (DropWhy::AfterJump): it would take the character back down.
    static constexpr std::uint64_t kJumpUpStayMs = 8000;
    /// Jumps up off until the next start after this many presses in a row
    /// that did not get there, or this many in a run.
    static constexpr int           kJumpUpFailsInRow = 3;
    static constexpr int           kJumpUpFailsPerRun = 5;
    /// A key whose kind never came this many times -- Ctrl + ↑ with no high
    /// jump, ↑↑ with no second -- is off for the run; a floor fallen back
    /// from this many times is solid from below this session; a jump's ↑
    /// held longer than this latches HeldLong (InputManager::kJumpHeldLongMs).
    static constexpr int           kJumpUpKindMisses = 2;
    static constexpr int           kJumpUpSolidAfter = 2;
    static constexpr std::uint32_t kJumpUpHeldMaxMs = 250;
    /// g measured from a flight outside these, this many times in a run:
    /// the jump is not what the model says -- Physics. Logged, never fed back.
    static constexpr double        kJumpUpGravityLo = 2100.0;
    static constexpr double        kJumpUpGravityHi = 2800.0;
    static constexpr int           kJumpUpGravityOut = 2;
    /// A fall back shorter than this share of the key's shortest full arc
    /// (2v / g at the highest gravity) was bumped from below; longer, it came
    /// up short.
    static constexpr double        kJumpUpBumpShare = 0.8;
    /// "would jump" and the refusals said at INFO at most this often; your
    /// own 0x002A and 0x0031 traced at DEBUG this long after a jump's key.
    static constexpr std::uint64_t kJumpUpSayMs = 30000;
    static constexpr std::uint64_t kJumpUpTraceMs = 3000;
    /// A monster reachable only by a jump is picked only when nothing on the
    /// level, below or on the character's own floor is (choose_target).
    static constexpr bool          kJumpUpTargetsLast = true;

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
    ///
    /// The area is the box left..right x top..bottom, its edges inside it:
    /// x +- half_w and y +- half_h for a spot taken where the character
    /// stood (set_half); the map tab's own for one from its hunting area
    /// (set_zone, `from_map`, of the map `stem`), whose x, y is its home --
    /// "chỗ về" -- and half_w, half_h half its sizes, for any old reader.
    /// `map_aside`: set aside by zone_aside -- neither a door back nor a
    /// return after a log in brings it back, only a set_zone.
    struct Spot {
        bool          set = false;
        float         x = 0.0f, y = 0.0f;
        int           half_w = 0, half_h = 0;
        float         left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
        std::uint32_t epoch = 0;
        bool          suspended = false;
        bool          login = false;
        float         left_x = 0.0f, left_y = 0.0f;
        bool          map_known = false;
        std::uint16_t map = 0, area = 0;
        bool          away_known = false;
        std::uint16_t away_map = 0, away_area = 0;
        bool          from_map = false;
        std::string   stem;
        bool          map_aside = false;

        /// Whether wx, wy is in the area, or no more than `slack` past its
        /// edge.
        bool inside(double wx, double wy, double slack = 0.0) const {
            return wx >= left - slack && wx <= right + slack && wy >= top - slack && wy <= bottom + slack;
        }
        /// The spot at at_x, at_y, its area hw either side and hh above and
        /// below.
        void set_half(float at_x, float at_y, int hw, int hh) {
            x = at_x;
            y = at_y;
            half_w = hw;
            half_h = hh;
            left = at_x - static_cast<float>(hw);
            right = at_x + static_cast<float>(hw);
            top = at_y - static_cast<float>(hh);
            bottom = at_y + static_cast<float>(hh);
        }
    };

    /// A hunting area drawn on the map tab (configs\spots.ini, HuntZone),
    /// sent for one map entry: its map's file, lowercase ("t52_s2"), the map
    /// entries the fix had counted as it was sent (WorldFix::epoch), its home
    /// and its box, edges inside -- world pixels, y growing downward.
    struct Zone {
        std::string   stem;
        std::uint32_t epoch = 0;
        float         home_x = 0.0f, home_y = 0.0f;
        float         left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
    };
    /// Why a hunting area sent (set_zone) is not in use yet.
    enum class ZoneWait : std::uint8_t {
        None,           ///< none waiting
        Ahead,          ///< sent for a map entry the fix has not come to, or no capture
        NotSettled,     ///< the character not standing on the ground
        Outside,        ///< the character outside it and not back where it left: hunting where it stands
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
        /// The map tab's area, its map's file gone by (2026.11.04d): away from
        /// the home with no way back into the box the map shows -- walking,
        /// falling off ends, the home no more than one jump above
        /// (return_point): fighting where it stands. What TooHigh is for this
        /// run's spot.
        NoReturn,
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
        AfterJump,          ///< a jump up toward a target worked under kJumpUpStayMs ago (2026.11.05b)
    };
    /// In the log's English.
    static const char* drop_why_name(DropWhy w);
    /// Whether the next look from the same standing point refuses again: a
    /// gate, the floors under and below, the area, no drop here before, the
    /// monster's drops used up -- not the clock, a hold, a walk or the packets.
    static bool drop_lasting(DropWhy w);

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
    /// MapChanged untick drop_down and save it, with JumpMapChanged jump_up
    /// (a jump up's key changed the map, 2026.11.05b), with BothMapChanged
    /// both (a drop and a jump up both under watch as it changed). KeysStuck:
    /// a drop's ↓ or Ctrl up left owed; JumpKeysStuck: a high jump's ↑ or
    /// Ctrl. Read from any thread.
    enum class DropStop : std::uint8_t { None, MapChanged, KeysStuck, JumpMapChanged, BothMapChanged, JumpKeysStuck };
    DropStop drop_stop() const { return drop_stop_.load(); }

    // ---- the jump up (HunterConfig::jump_up, 2026.11.05b)

    /// A jump up under way: walking to its take-off, settled to press, or in
    /// the air from its key.
    enum class JumpUpPhase : std::uint8_t { Idle, Approach, Settle, Air };
    /// What it is for: onto the floor of the target above, a leg of the way
    /// back to the home (return_point), or a leg toward a monster sought in
    /// the area ("Tự tìm quái trong vùng") -- the way back's rules, with the
    /// per-monster caps and AfterDrop.
    enum class JumpUpFor : std::uint8_t { Target, Home, Seek };
    /// What came of a press (the jump-up plan's §7).
    enum class JumpUpVerdict : std::uint8_t {
        None,
        Worked,     ///< landed on the planned floor
        FellBack,   ///< came down on the take-off's floor, where it stood
        KindMiss,   ///< Ctrl + ↑ with no high jump, ↑↑ with no second one
        Lower,      ///< on another floor below the planned one, in the area
        Higher,     ///< on a floor above the planned one: Physics
        Outside,    ///< landed outside the area: Physics
        OffMap,     ///< on no floor of the map's file: distrusted, OffMap
        Nothing,    ///< no take-off within kJumpUpJudgeMs of the last key
        Unknown,    ///< the packets lost, a hold, the game behind, or still in the air at kJumpUpAirMaxMs
        Gate,       ///< the map changed within kPressWatchMs of the key
    };
    static const char* jump_verdict_name(JumpUpVerdict v);
    /// Why jumps up are off until the next start.
    enum class JumpUpOff : std::uint8_t {
        None,
        Fails,          ///< kJumpUpFailsInRow in a row, or kJumpUpFailsPerRun, did not get there
        Physics,        ///< landed higher than planned, or outside the area, or g twice out of range
        OffMap,         ///< landed on no floor of the file
        MapChanged,     ///< a press changed the map: the hunt stops (drop_stop)
        HeldLong,       ///< an ↑ held over kJumpUpHeldMaxMs
        KeysStuck,      ///< a key-up stayed owed: the hunt stops (drop_stop)
    };
    static const char* jump_off_name(JumpUpOff o);
    /// Why a jump up does not go (jump_up_rule): the first of its rules that
    /// does not hold, in their order. None: it goes.
    enum class JumpUpWhy : std::uint8_t {
        None,
        Off,                ///< jump_up off
        NoPackets,          ///< packet_positions not on
        StayPut,            ///< Đứng yên một chỗ
        NoArea,             ///< no area of the map tab's in use, with its map's file
        Latched,            ///< off until the next start (JumpUpOff), or every key it needs off for the run
        KeysOwed,           ///< a chord's key-up has not gone out
        JumpKey,            ///< the jump key is not ↑
        KeyClash,           ///< a walk or attack key is Ctrl: no Ctrl + ↑, and only it would do
        NotWired,           ///< no map book, veto or air counts: the command line's hunter
        NotPacketTarget,    ///< the target is a screen plate
        LevelMonster,       ///< one to fight on the character's own level
        NotAbove,           ///< not more than up() above
        OutsideArea,        ///< the monster outside the area
        OnOwnFloor,         ///< on the character's own floor: walked to
        NotKnown,           ///< the packets do not know where the character is
        AfterEntry,         ///< a map entry under kDropAfterEntryMs ago
        NotSettled,         ///< not standing, with the stop of its walk reported
        InAir,              ///< a hop, a drop or a jump still in the air
        TooSoon,            ///< the last press -- a drop's or a jump's -- under kDropGapMs ago
        AfterDrop,          ///< a drop that worked under kDropStayMs ago (not on the way back)
        StepJudge,          ///< a step back not judged yet
        Walking,            ///< a direction key still down
        MinuteCap,          ///< kJumpUpsPerMinute in the last minute
        TargetCap,          ///< kJumpUpsPerTarget presses, or kJumpUpSettlesPerTarget settles, for this monster
        Held,               ///< a hold
        Unfocused,          ///< the game not in front
        Loot,               ///< a kill's pick-ups still owed
        Veto,               ///< GoBot says no (set_drop_veto)
        MapUnknown,         ///< the area's map not trusted (MapTrust)
        OffFloor,           ///< where it stands is on no floor of that file
        Plan,               ///< no jump the planner keeps (JumpUpCall::no)
        OffColumn,          ///< at the press, where it stopped does not pass
    };
    static const char* jump_why_name(JumpUpWhy w);

    // ---- "Tự tìm quái trong vùng" (HunterConfig::seek_in_zone, 2026.11.05b)

    /// Why nothing is gone for, with the seek on: the first of these.
    enum class SeekNone : std::uint8_t {
        None,
        Empty,      ///< no living monster inside the area
        AboveOff,   ///< only some a jump up gets to, and jump-ups are off
        NoWay,      ///< none the character gets to from where it stands
        Passed,     ///< only some passed over a while, or another player's
    };
    static const char* seek_none_name(SeekNone w);

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
        /// in (Spot::login), `spot_map_aside` the map tab (zone_aside,
        /// Spot::map_aside) -- and why it does not steer the hunter now --
        /// SpotWhy::None when it does, or with none switched on.
        bool    spot_set = false;
        float   spot_x = 0.0f, spot_y = 0.0f;
        int     spot_half_w = 0, spot_half_h = 0;
        bool    spot_suspended = false;
        bool    spot_login = false;
        bool    spot_map_aside = false;
        SpotWhy spot_why = SpotWhy::None;
        /// Its area (Spot::left ...), whether it is the map tab's hunting
        /// area of the map `spot_stem` -- spot_x, spot_y its home -- and the
        /// map entries counted as it was set or came back (Spot::epoch).
        float         spot_left = 0.0f, spot_top = 0.0f, spot_right = 0.0f, spot_bottom = 0.0f;
        bool          spot_from_map = false;
        std::string   spot_stem;
        std::uint32_t spot_epoch = 0;
        /// A hunting area from the map tab sent and not in use yet: why, of
        /// which map, and with Outside how far outside it the character
        /// stands (zone_outside_px). None with none waiting.
        ZoneWait      zone_wait = ZoneWait::None;
        std::string   zone_wait_stem;
        int           zone_wait_px = 0;
        /// From the character to the spot, in world pixels, y growing
        /// downward, as of the last look that knew both -- the spot set and
        /// not suspended, the character Known; 0 otherwise.
        float   spot_dx = 0.0f, spot_dy = 0.0f;
        /// How far outside the spot's area the character stands, at every
        /// pass it is in use (zone_outside_px): 0 inside it, -1 not known --
        /// no spot in use, or the packets not placing the character.
        int     spot_outside_px = -1;
        /// At the home, at the last pass the spot was in use: the map tab's
        /// area with its map's file, across and on its floor (zone_at_home);
        /// otherwise within spot_tolerance across, as go_home has it.
        bool    spot_home = false;
        /// The map tab's area with its map's file (2026.11.04d): the way back
        /// into its box the map shows from where the character stands
        /// (return_point) -- whether there is one and where it ends -- and
        /// whether, outside the box with it, the walk back comes before any
        /// fight (the owner's Q2, zone_walks_back_first).
        bool    spot_way = false;
        int     spot_way_x = 0, spot_way_y = 0;
        bool    spot_back_first = false;
        /// ... worked out with jumps up able to go (2026.11.05b): walking,
        /// falling or jumping up; the jumps up on it; and, when it starts
        /// with one (ZoneReturn::jump), that one's key and take-off x -- what
        /// the card says the way back is.
        bool     spot_way_by_jumps = false;
        int      spot_way_jumps = 0;
        bool     spot_way_jump_first = false;
        JumpKind spot_way_jump_kind = JumpKind::Up;
        int      spot_way_jump_x = 0;
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

        /// "Nhảy lên bục trên" (HunterConfig::jump_up, 2026.11.05b): what it
        /// is at -- walking to a take-off, settled to press, in the air -- for
        /// what, with which key from which column onto which floor; the
        /// presses this run and what came of them, and the jumps it would
        /// have made with the tick off or off for the run.
        JumpUpPhase   jump_phase = JumpUpPhase::Idle;
        JumpUpFor     jump_for = JumpUpFor::Target;
        JumpKind      jump_kind = JumpKind::Up;
        int           jump_x = 0;
        int           jump_land_top = 0;
        /// ... its take-off's standing point's height and the landing's
        /// column: the map tab's arrow (JumpPlan::take, land).
        int           jump_feet_y = 0;
        int           jump_land_x = 0;
        std::uint64_t jumps = 0;
        std::uint64_t jumps_worked = 0;
        std::uint64_t jumps_fell_back = 0;
        std::uint64_t jumps_would = 0;
        /// The jump that would have gone last, with the tick off or jumps off
        /// for the run -- toward a monster, on the way back, or to one sought
        /// -- and when it was last worked out: the map tab draws it while
        /// fresh. 0: none this run.
        JumpPlan      jump_would_plan = {};
        std::uint64_t jump_would_ms = 0;
        /// Why the last jump up was not taken, with its number -- the floor's
        /// rise, a key -- the planner's reason with Plan, and when; what for,
        /// and the monster's name ("" on the way back).
        JumpUpWhy     jump_wait = JumpUpWhy::None;
        int           jump_wait_px = 0;
        JumpNo        jump_wait_no = JumpNo::None;
        std::uint64_t jump_wait_ms = 0;
        JumpUpFor     jump_wait_for = JumpUpFor::Target;
        std::string   jump_wait_name;
        /// Off until the next start, and why; the keys off for the run (bit
        /// 1 << JumpKind, kJumpUpKindMisses); a press that changed the map.
        JumpUpOff     jump_off = JumpUpOff::None;
        std::uint8_t  jump_kinds_off = 0;
        bool          jump_map_changed = false;

        /// "Tự tìm quái trong vùng" (HunterConfig::seek_in_zone, 2026.11.05b):
        /// whether it was on at the last pass; the monster gone for -- its slot
        /// and generation, where it is placed, in view or not, projected or
        /// not, its attack point, how it is got to and the leg under way
        /// ('W' a walk, 'F' a fall off an end, 'J' a jump up) and where that
        /// goes; or nothing gone for, and why. The box's living monsters, of
        /// them those above with no way up (jump-ups off), and those passed
        /// over; the slots the tab may grey -- no way to them, or passed over.
        /// No State of its own: Approaching while it goes, NoTargets with none.
        bool          seek_active = false;
        bool          seek_on = false;
        int           seek_slot = -1;
        std::uint32_t seek_gen = 0;
        int           seek_mob_x = 0, seek_mob_y = 0;
        bool          seek_in_view = false;
        bool          seek_projected = false;
        int           seek_at_x = 0, seek_at_y = 0;
        SeekTier      seek_tier = SeekTier::OwnFloor;
        char          seek_leg = 'W';
        int           seek_leg_x = 0;
        bool          seek_none = false;
        SeekNone      seek_none_why = SeekNone::None;
        int           seek_in_area = 0;
        int           seek_above_no_way = 0;
        int           seek_passed = 0;
        std::vector<std::uint32_t> seek_grey_slots;
        /// ... where the character stood as the way was worked out (the
        /// card's "bên trái 445 px"), and a jump the way starts with: its key
        /// and the floor it lands on. Each of the slots the tab may grey, by
        /// slot and generation, with why -- and, passed over, for how long
        /// more: the tab's hover.
        int           seek_you_x = 0, seek_you_y = 0;
        bool          seek_planned = false;     ///< seek_at_x, seek_at_y and the leg are a plan's
        JumpKind      seek_jump_kind = JumpKind::Up;
        int           seek_jump_land_top = 0;
        struct SeekGrey {
            std::uint32_t slot = 0;
            std::uint32_t gen = 0;
            SeekNo        why = SeekNo::None;
            std::uint64_t left_ms = 0;
        };
        std::vector<SeekGrey> seek_grey;
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

    /// A hunting area from the map tab for one map entry, in place of the
    /// spot taken where the character stands. Kept pending until it applies
    /// -- at that entry, the character standing settled inside it or near
    /// it, or back near where a spot of it was left; an edit of the one in
    /// use at once, wherever the character is (zone_take, the owner's Q1) --
    /// dropped once the fix's map entries are past its epoch, and replaced
    /// by a newer one. Taken, a map entry suspends it as any spot. Asks made
    /// meanwhile are done at the next pass in the order made. One of no map
    /// (zone_stem), or whose home is outside its box, is refused. From any
    /// thread.
    void set_zone(const Zone& z);
    /// That map's area deleted on the tab: a spot from it -- and one pending
    /// -- is dropped, and the spot taken again where the character stands.
    /// Another map's is left alone. From any thread.
    void clear_zone(const std::string& stem);
    /// The tab no longer knows the character is on that map: a spot from its
    /// area is set aside, as at a map entry -- and one pending dropped --
    /// until a set_zone brings it back; no door back nor return after a log
    /// in does. Another map's is left alone. From any thread.
    void zone_aside(const std::string& stem);

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
    /// A walk toward a target with the map tab's area in use is looked along
    /// its floor (walk_stays_in) as far as it goes before it stops for the
    /// target -- to reach across, or under it -- and this further, a look's
    /// walk and the report's way back; kZoneWalkAheadPx at most.
    static constexpr int kZoneWalkSlackPx = 48;
    /// A walk refused at the area's edge is said at INFO at most this often,
    /// and counted in the minute line every time.
    static constexpr std::uint64_t kWalkKeptSayMs = 5000;

    /// The owner's Q3 of 04d ((a) until answered): with no hop to follow
    /// (hops_to_targets false), choose_target's last resort passes over a
    /// monster more than up() above -- but one on the character's own floor,
    /// up its slope, which a walk gets to. It was walked under until
    /// give_up_ms, down t52_s2's slope out of the box (2026-11-04 20:43). False
    /// is (b): any height, as before.
    static constexpr bool kSkipAboveUnreached = true;
    /// Whether a hop toward a monster above can follow a pick at all: the
    /// rule hop() goes by -- jump_to_reach on, a jump key that goes out, and
    /// the spot off -- but for the moment after a drop.
    static bool hops_to_targets(const HunterConfig& cfg);
    /// A monster's floor at column x, its point at y (world pixels): the
    /// first floor of the column, not a wall, whose top is no more than
    /// kDropFloorMatchPx above y and no more than kDropMonsterFloorPx below
    /// it -- its top; none with no such floor.
    static std::optional<int> monster_floor(const GameMapBook::Page& p, int x, int y);

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
        /// With the seek on (2026.11.05b): not within reach, and no standing
        /// point inside the area the character gets to has it in reach --
        /// never walked toward (mark_no_way).
        NoWay,
    };
    /// Not to be attacked.
    static bool refused(Listed l) {
        return l == Listed::Skip || l == Listed::NearSkip || l == Listed::NotFirst ||
               l == Listed::GaveUp || l == Listed::OutOfArea || l == Listed::NoWay;
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
    ///
    /// `own_floor`, parallel to `mobs`: which of them the map has on the
    /// character's own floor (1) or not (0) -- given only when no hop can
    /// follow a pick (hops_to_targets) and a map file is gone by: the map
    /// tab's area's, or with no spot the one drops trust, never for this
    /// run's spot, which goes on as before. With it, the last resort passes
    /// over one more than up() above that is not (kSkipAboveUnreached, the
    /// owner's Q3 of 04d). Null: any height, as ever. 2 (2026.11.05b): a jump
    /// up gets onto its floor (jumps_may_go) -- picked last of all
    /// (kJumpUpTargetsLast), once nothing else is.
    static int choose_target(const HunterConfig& cfg, const std::vector<Nameplate>& mobs,
                             const Nameplate* me, const std::vector<Listed>& listed,
                             bool* wounded_picked = nullptr, bool below_first = false,
                             const std::vector<char>* own_floor = nullptr);

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
        std::uint64_t last_jump_worked_ms = 0;  ///< a jump up toward a target that worked; 0 none (AfterJump)
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
        /// Where the target last stood, by its own frames (two of them at one
        /// height, MapTrust's rule): its floor is the target's floor
        /// (monster_floor). None: by its point alone.
        bool          target_stood = false;
        MapPoint      target_stand;
    };
    /// What drop_rule made of them.
    struct DropCall {
        DropWhy why = DropWhy::None;
        int     px = 0;             ///< with the reason: a gate's distance, how far below or across, ...
        bool    placed = false;     ///< x0 .. target_floor below were worked out: a map file was gone by
        int     x0 = 0, y0 = 0;     ///< where the character was judged to stand, world pixels
        int     stand_y = -1;       ///< the floor under it, -1 not known
        int     land_y = 0;         ///< the floor the file says the fall lands on
        bool    direct = false;     ///< the target's own floor, not one between
        int     gate_px = -1;       ///< the nearest gate across in the fall's band; -1 none
        int     tx = 0, ty = 0;     ///< the target's point, world pixels
        int     target_floor = -1;  ///< its floor's top at tx (monster_floor); -1 not known
        bool ok() const { return why == DropWhy::None; }
    };
    /// The drop plan's §3.2, in its order, with the player's answers of
    /// 2026-11-04: the first rule that does not hold. Pure: facts in, a
    /// reason out. `f.press` checks the items a press needs over those a
    /// settle begins with -- the character settled, its stop reported, no
    /// direction key down, standing on a floor of the file, the landing
    /// read at kDropMarginPressPx either side -- and kDropPressSlackPx more
    /// across. Where it stands is the last report's own at the press; as a
    /// settle begins, the fix worked out to now, at the height of the floor
    /// the last report stood on, followed there (a walk's height goes stale
    /// on a slope). Slopes as the game's maps have them (2026.11.04d): the
    /// landing's floor and the character's own followed column by column to
    /// either margin, and the landing the target's floor when that floor,
    /// followed to the target, is the target's (monster_floor) -- else past
    /// it when lower than it by more than kDropFloorMatchPx.
    static DropCall drop_rule(const HunterConfig& cfg, const DropFacts& f);

    /// A monster's floor's top at its column tx, its point ty: the floor it
    /// last stood on (`stood`, at `stand`), followed there -- a walk moves a
    /// monster's x alone, so its point may be as far off that floor as the
    /// slope it walked, and kDropMonsterFloorPx more; further, it has gone
    /// elsewhere since -- else monster_floor at its point. What a drop and a
    /// jump up to it land on.
    static std::optional<int> target_floor(const GameMapBook::Page& p, bool stood, MapPoint stand, int tx, int ty);

    /// Everything a jump up is judged by at one look (jump_up_rule), laid out
    /// so a test can set any one of them.
    struct JumpUpFacts {
        JumpUpFor     purpose = JumpUpFor::Target;
        bool          press = false;            ///< at the press -- its items too -- not as it is walked to
        std::uint64_t now = 0;                  ///< the hunter's clock
        /// The map tab's area in use with its map's file (spot_mode Use and a
        /// page), and MapTrust trusting that same map.
        bool          area = false;
        bool          trusted = false;
        bool          latched = false;          ///< jumps up off until the next start
        std::uint8_t  kinds_off = 0;            ///< keys off for the run: bit 1 << JumpKind
        bool          owed = false;             ///< a chord's key-up not out
        bool          wired = false;            ///< a map book, a veto, air counts and your reports
        /// The monster: Target -- the target above, from the packets (slot 0
        /// or more); Seek -- the one sought. Its point in the packets'
        /// world, and where it last stood by its own frames (its floor).
        bool          packet_target = false;
        int           slot = -1;
        std::uint32_t gen = 0;
        int           dy = 0;                   ///< Target: as the plates have it, screen pixels
        float         target_wx = 0.0f;
        float         target_wy = 0.0f;
        bool          target_stood = false;
        MapPoint      target_stand;
        bool          level_monster = false;    ///< one to fight on the level (level_monster)
        WorldFix      fix;                      ///< where the packets put the character
        std::uint64_t entry_ms = 0;
        std::uint64_t settle_from_ms = 0;
        bool          walk_held = false;
        bool          in_air = false;
        std::uint64_t last_press_ms = 0;        ///< a drop's or a jump's; 0 for none this run
        std::uint64_t last_drop_worked_ms = 0;
        bool          step_judge = false;
        bool          left_held = false;
        bool          right_held = false;
        int           presses_minute = 0;
        int           presses_target = 0;
        int           settles_target = 0;
        bool          held = false;
        bool          focused = true;
        bool          loot_owed = false;
        const char*   veto = nullptr;
        /// The area's file and box; where the character stands on it, worked
        /// out along its floor -- at the press, the last report's own point.
        std::shared_ptr<const GameMapBook::Page> page;
        HuntZone      box;
        std::optional<MapPoint> stand;
        /// What the planner goes by -- the keys' bits are set by the rule --
        /// and the jump to take: Home and Seek, their way's leg; Target, the
        /// one under way (kept while its run still holds it).
        JumpRules     rules;
        std::optional<JumpPlan> plan;
        float         scale = 1.0f;             ///< packet_scale: reach in world pixels
    };
    /// What jump_up_rule made of them.
    struct JumpUpCall {
        JumpUpWhy why = JumpUpWhy::None;
        JumpNo    no = JumpNo::None;            ///< with Plan and OffColumn: the planner's reason
        int       px = 0;                       ///< with the reason: the floor's rise, a key, how far off
        std::optional<JumpPlan> plan;           ///< the jump, once planned
        JumpColumn column;                      ///< at the press: where it stopped, judged
        MapPoint  target;                       ///< Target: the monster's floor's standing point at its column
        bool      placed = false;               ///< `target` was worked out on the file
        bool ok() const { return why == JumpUpWhy::None; }
    };
    /// The jump-up plan's §6 table, in its order: the first rule that does
    /// not hold. Pure: facts in, a reason out -- the planner run with Target
    /// (plan_jump_to), the way's leg taken as given with Home and Seek, and at
    /// the press the column the character stopped on judged afresh: it must
    /// pass the planner, its ±kJumpUpMarginPx samples too, and land on the
    /// planned floor. Ctrl + ↑ only with no walk or attack key on Ctrl.
    static JumpUpCall jump_up_rule(const HunterConfig& cfg, const JumpUpFacts& f);

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
    /// `Floor`: a walk back the map shows (return_point) -- along the floor
    /// the character stands on, or off its end to fall into the box -- renewed
    /// as Spot's, and never a hop: a hop off the floor leaves the way.
    enum class WalkFor : std::uint8_t { Target, Spot, Floor };

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
    /// toward the spot either. On a walk back the map shows (Floor), never;
    /// with jump_up on and jump_area, never: jumps there are planned.
    void hop(const HunterConfig& cfg, int dy, std::uint64_t now, WalkFor what);
    /// Whether the last jump the hunter pressed may still have the character
    /// in the air at `now`: kJumpAirMs from it -- or its last drop, until a
    /// landing is reported or kDropFallMaxMs from ↓ going down -- or its last
    /// jump up (jump_in_air).
    bool in_air(std::uint64_t now) const {
        return (last_hop_ms_ != 0 && now - last_hop_ms_ < kJumpAirMs) || drop_in_air(now) || jump_in_air(now);
    }
    bool drop_in_air(std::uint64_t now) const {
        return drop_t_down_ != 0 && !drop_landed_ && (now < drop_t_down_ || now - drop_t_down_ < kDropFallMaxMs);
    }
    /// A jump up judged in the air, or its key down under kJumpUpAirMaxMs ago
    /// with no landing judged since.
    bool jump_in_air(std::uint64_t now) const {
        return jump_.phase == JumpUpPhase::Air ||
               (jump_t_key_ != 0 && !jump_landed_ && (now < jump_t_key_ || now - jump_t_key_ < kJumpUpAirMaxMs));
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
    /// Within track_spot, after the map entries and the log in: the map
    /// tab's asks of this pass -- a delete, an aside, a new area -- and the
    /// area pending, taken into `spot` when it applies.
    void track_zone(Spot& spot, std::optional<Zone> asked, const std::string& clear, const std::string& aside);
    /// Those of `mobs` outside the spot's area and out of reach of `me`,
    /// marked OutOfArea in `listed` -- the current target, at `target_at`,
    /// only kSpotKeepPx past the edge. In reach too while the walk back
    /// comes first (ZoneHere::back_first, the owner's Q2 of 04d).
    void mark_out_of_area(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                          int target_at, std::vector<Listed>& listed) const;
    /// With nothing to fight: the walk back to the spot, if it is to be had
    /// -- or kept up, or let go of there. True while walking back; false, and
    /// the caller lets go of the keys as before. The map tab's area with its
    /// map's file: go_home_zone.
    bool go_home(const HunterConfig& cfg, std::uint64_t now);
    /// Something to fight this pass: the idle time before a walk back counts
    /// from here, and a walk back under way is over.
    void spot_busy(std::uint64_t now);
    /// The walk back under way is over: arrived, given up, or overtaken.
    void end_return();
    /// The spot's numbers into status_.
    void publish_spot();

    // ---- the map tab's area against its map's file (2026.11.04d)

    /// The file of a spot's map, to go by: the map tab's area's, not
    /// suspended, with a map book -- null for this run's spot, and for a map
    /// this session found wrong: a drop's landing (GameMapBook::distrusted),
    /// or two standing points of yours off its floors (the hunter's MapTrust,
    /// Mismatch). Read on its first ask, on the hunter's thread.
    std::shared_ptr<const GameMapBook::Page> zone_page(const Spot& s) const;
    /// Where the character stands on `p` by `f`: its last report's point, on
    /// the floor it stood on -- with `worked_out`, that floor followed to the
    /// x worked out since, a walk's height going stale on a slope. None for a
    /// fix not Known or in the air, a report on no floor, or a walk past its
    /// floor's end.
    static std::optional<MapPoint> stand_on(const GameMapBook::Page& p, const WorldFix& f, bool worked_out);
    /// What the map tab's area in use makes of where the character stands,
    /// worked out at every pass by track_spot (track_zone_way): its file and
    /// box, where the character stands on it, how far outside the box, the
    /// way back (return_point) and whether that comes first (the owner's Q2).
    struct ZoneHere {
        std::shared_ptr<const GameMapBook::Page> page;  ///< null: none -- the hunter as before 04d
        HuntZone                box;
        std::optional<MapPoint> stand;                  ///< worked out along its floor
        std::optional<MapPoint> report;                 ///< at its last report
        int                     outside_px = 0;
        bool                    home = false;           ///< zone_at_home, worked out
        std::optional<ZoneReturn> way;
        bool                    back_first = false;     ///< zone_walks_back_first, no walk back given up on
        bool                    judged = false;         ///< a way back asked of the map from a standing point: this pass, or carried
    };
    /// Within track_spot, the spot in use: zone_ for this pass; a way back
    /// asked of the map only when where the character stands has changed.
    void track_zone_way(const HunterConfig& cfg, const Spot& spot);
    /// Before a walk toward `target` with the map tab's area in use: the walk
    /// looked along its floor (walk_stays_in, the owner's Q1 of 04d). Refused
    /// -- out of the box down a slope, or off an end to land outside it --
    /// the keys are let go of, the target passed over for twice give_up_ms
    /// and its fight ended, said at INFO: true, and the pass walks no further.
    /// `goal_px`, 0 or more: the walk goes that far across and stops -- the
    /// seek's walk to an attack point (2026.11.05b) -- in place of where this
    /// target comes in reach.
    bool walk_kept_in(const HunterConfig& cfg, const Nameplate& target, int dx, int dy, std::uint64_t now,
                      int goal_px = -1);
    /// go_home with the map tab's area and its map's file: home by height as
    /// well as across (zone_at_home); otherwise the way back the map shows --
    /// walked to along the floor, or off its end into the box, never a hop
    /// (WalkFor::Floor) -- and stood at once there; with none, NoReturn in
    /// place of TooHigh, and the character fights where it stands. A way back
    /// as before (ZoneReturn::as_before) hops as every walk back did.
    bool go_home_zone(const HunterConfig& cfg, const Spot& spot, std::uint64_t now);
    /// For choose_target's `own_floor`: which of `mobs` more than up() above
    /// `me` stand on the character's own floor -- followed from `stand` to
    /// each one's column, the floor there theirs (monster_floor). `f` lays
    /// the plates over the world.
    std::vector<char> own_floor_marks(const HunterConfig& cfg, const std::vector<Nameplate>& mobs,
                                      const Nameplate& me, const GameMapBook::Page& p, MapPoint stand,
                                      const WorldFix& f) const;

    // ---- the drop (HunterConfig::drop_down)

    /// drop_down on, the packets steering, not standing still: what asks
    /// anything new of the eyes.
    static bool drop_wanted(const HunterConfig& cfg);
    /// A drop could go now: drop_wanted, not off for the run, the map
    /// trusted, the spot in use if on -- what choose_target's below_first
    /// and the stop above a target below (Q6) go by.
    bool drops_may_go(const HunterConfig& cfg) const;
    /// The owner's Q6 (kDropStopAbovePx): `target`, from the packets, below
    /// past max(down(), drop_min_px) and no more than kDropStopAbovePx
    /// across, with a drop able to go -- the walk to it let go of above it.
    bool drop_stop_above(const HunterConfig& cfg, const Nameplate& target, int dx, int dy) const;
    /// The monsters' frames of this pass (Eyes::stands), for where each last
    /// stood: two of a monster's frames kMobStandMs apart at one height,
    /// within kMobSameYPx, as MapTrust judges one standing.
    void note_stands(const std::vector<MapTrust::Mob>& mobs);
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
    /// A press watched for a map change (press_, press_other_).
    struct PressRecord;
    /// A map change within kPressWatchMs of the press `r`, whatever was
    /// judged before: undone, both ends learned as gates, drops off and the
    /// hunter halted for GoBot's window to stop the hunt -- drop_watch stores
    /// the stop, once for both presses when two were under watch.
    void drop_gate(PressRecord& r, std::uint64_t now);
    /// Drops off until the next start.
    void latch_drops(DropOff why);
    /// Why the target `dy` below is not dropped to: into status_, counted for
    /// the minute line, and said in the log at each change of reason -- at
    /// INFO once per target for what the player can do something about, DEBUG
    /// otherwise.
    void drop_refused(const HunterConfig& cfg, const DropCall& call, const Nameplate& target, int dy,
                      std::uint64_t now);
    /// The presses of the last minute, and for the monster `slot`, `gen`; its
    /// settles let go of without a press.
    int drop_presses_minute(std::uint64_t now);
    int drop_presses_for(int slot, std::uint32_t gen) const;
    int drop_settles_for(int slot, std::uint32_t gen) const;
    /// The minute line's part for drops: "" with no press this minute.
    std::string drop_minute_text() const;

    // ---- the jump up (HunterConfig::jump_up, 2026.11.05b): hunter_jump.cpp

    /// The packets steering, not standing still, the map tab's area in use
    /// with its map's file: the jump planner runs -- for real, or for the
    /// log alone (the "would jump" line) -- and the map is watched
    /// (drop_watch). jump_up on or off.
    bool jump_watch(const HunterConfig& cfg) const;
    /// For the area's file `page`: the packets steering, not standing still,
    /// the spot in use, MapTrust trusting that same map, the jump key ↑ and
    /// the hunter wired -- a jump up would go but for the tick and the latch.
    bool jump_ready(const HunterConfig& cfg, const GameMapBook::Page* page) const;
    /// jump_ready for the area in use this pass. With jump_up on, hop() never
    /// presses here -- off for the run too: the jumps are planned.
    bool jump_area(const HunterConfig& cfg) const;
    /// jump_area, jump_up ticked and not off for the run: the planner's ways
    /// and marks have jumps in them, and keys go.
    bool jumps_may_go(const HunterConfig& cfg) const;
    /// What the planner goes by this pass, on the area's file: the keys (none
    /// off for the run, no Ctrl + ↑ with a walk or attack key on Ctrl), the
    /// gates (kJumpUpGateBand only), the reach, and what this session learned
    /// of that map -- no-jump points and floors solid from below
    /// (GameMapBook). Its gates point into jump_gates_: one pass's.
    JumpRules jump_rules(const HunterConfig& cfg, const std::shared_ptr<const GameMapBook::Page>& page);
    /// Which of `mobs` more than up() above, not on the character's own
    /// floor (`marks` 0), a jump gets onto: the nearest one, marked 2 when
    /// jumps may go -- the rest left as they are, choose_target taking the
    /// first anyway; with the tick off or off for the run, said in the log
    /// ("would jump") and left 0.
    void jump_marks(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                    const std::vector<Listed>& listed, const GameMapBook::Page& p, MapPoint stand,
                    const WorldFix& f, std::vector<char>& marks);
    /// The facts for jump_up_rule at this look: for `purpose`, the monster
    /// `slot`, `gen` at world point `target_w` (none for Home), `dy` the
    /// plates' height (Target), and `plan` the leg or the jump under way.
    JumpUpFacts jump_facts(const HunterConfig& cfg, JumpUpFor purpose, int slot, std::uint32_t gen,
                           std::optional<MapPoint> target_w, int dy, bool level, const std::optional<JumpPlan>& plan,
                           std::uint64_t now, bool press);
    /// In the walk branch, after drop_toward: a target above, not on the
    /// character's own floor, with jump_up on and the area in use -- walked
    /// to its take-off along the floor, settled, and the key pressed; never
    /// walked under blindly. Held still in the air. True while it has the
    /// pass: the pass walks no further.
    bool jump_toward(const HunterConfig& cfg, const Nameplate& target, const Nameplate& me,
                     const std::vector<Nameplate>& mobs, const std::vector<Listed>& listed, int dx, int dy,
                     std::uint64_t now);
    /// At a jump's take-off -- within kJumpUpLeadPx of it, or settling there
    /// -- for `purpose`: the walk let go of and the stop waited for, the
    /// column judged, fine taps, the key. The way back's and the seek's legs
    /// come here with their plan; a target's from jump_toward. `slot`,
    /// `gen` the monster (-1 for none), `name` for the log. True while it
    /// has the pass; false once it gave the jump up for now (refused at the
    /// press, a back-off), the call said in the log and the status.
    bool jump_at(const HunterConfig& cfg, JumpUpFor purpose, const JumpPlan& plan, int slot, std::uint32_t gen,
                 const std::string& name, std::uint64_t now);
    /// Target and Seek: the take-off walked to along the character's own
    /// floor (WalkFor::Floor, looked along as walk_kept_in looks), then
    /// jump_at there. For Seek the caller sets jump_.target_w first. True
    /// while it has the pass; false when the walk would leave the area.
    bool jump_go(const HunterConfig& cfg, JumpUpFor purpose, const JumpPlan& plan, int slot, std::uint32_t gen,
                 const std::string& name, std::uint64_t now);
    /// The key, the column passed: false when it was not pressed after all.
    bool press_jump(const HunterConfig& cfg, const JumpUpCall& call, const std::string& name, std::uint64_t now);
    /// With no Hands::jump: the input's own -- ↑ jump_once, ↑↑ jump_twice at
    /// kJumpUpSecondAtMs, Ctrl + ↑ high_jump -- always ↑ (0x26).
    JumpKeys send_jump(JumpKind kind, const std::function<bool()>& go_on);
    /// Once a pass, after drop_follow: a flight judged by your own reports and
    /// the counts, a settle or a walk to a take-off not renewed at the last
    /// pass let go of, and the flight's reports traced at DEBUG. No key.
    void jump_follow(const HunterConfig& cfg, std::uint64_t now);
    /// A settle over without a press: as end_settle for the drop -- the
    /// give-up clock given back, counted against kJumpUpSettlesPerTarget;
    /// with `backoff`, none begun again for kJumpUpSettleBackoffMs.
    void jump_settle_end(const char* why, bool backoff, std::uint64_t now);
    /// A hold, the game behind, a view change, a log in: a settle let go of,
    /// a flight Unknown. The press record and the in-air guard stay.
    void jump_hold(const char* why);
    /// The flight judged `v`, landed at `lx`, `ly`: what it learns, counts and
    /// latches, and what the target or the way back makes of it.
    void end_flight(const HunterConfig& cfg, JumpUpVerdict v, std::uint64_t now, int lx, int ly, const char* why,
                    const std::string& more);
    /// A map change within kPressWatchMs of the jump's key `r` (drop_gate's
    /// for a jump): its learning and counts undone, the take-off and the
    /// entry learned as gates, jumps off and the hunter halted -- the stop
    /// JumpMapChanged, stored by drop_watch.
    void jump_gate(PressRecord& r, std::uint64_t now);
    void latch_jumps(JumpUpOff why);
    /// Why a jump up for `purpose` is not taken: into status_, counted for the
    /// minute line, and said at INFO once a monster or way for what the owner
    /// can act on -- DEBUG otherwise.
    void jump_refused(const HunterConfig& cfg, const JumpUpCall& call, JumpUpFor purpose, int slot,
                      std::uint32_t gen, const std::string& name, std::uint64_t now);
    /// The tick off or off for the run: where a jump would have gone, said
    /// once a monster or way, and at most every kJumpUpSayMs; counted.
    void jump_would(const JumpPlan& plan, JumpUpFor purpose, int slot, std::uint32_t gen, const std::string& name,
                    std::uint64_t now);
    int jump_presses_minute(std::uint64_t now);
    int jump_presses_for(int slot, std::uint32_t gen) const;
    int jump_settles_for(int slot, std::uint32_t gen) const;
    /// Into status_: the phase, the counts, the latch.
    void publish_jump();
    /// The minute line's part for the jumps up: "" with nothing this minute.
    std::string jump_minute_text() const;

    // ---- "Tự tìm quái trong vùng" (HunterConfig::seek_in_zone, 2026.11.05b):
    // hunter_seek.cpp

    /// One monster of the whole map as this pass's planning has it: where it
    /// is placed, and its attack point -- or why none, and the plan a jump up
    /// would have with jump-ups off.
    struct SeekLook {
        SeekMob                 mob;
        SeekNo                  why = SeekNo::None;
        std::optional<SeekPlan> plan;
        std::optional<SeekPlan> would;
    };
    /// What the seek takes for this pass: a plate (an index into the pass's
    /// monsters), or a monster out of view to go to (`goal`), or neither.
    struct SeekChoice {
        int  index = -1;
        bool goal = false;
    };
    /// With no plate to fight: going to one out of view (or held in a
    /// flight), standing with nothing to go to inside the box, or not the
    /// seek's -- outside the box, the walk back as before.
    enum class SeekIdle : std::uint8_t { NotMine, Going, Standing };

    /// The setup the seek runs in, but for the packets steering this pass:
    /// seek_in_zone, the map tab's area in use with its file, the character
    /// known, packet_positions on, stay_put off.
    bool seek_wanted(const HunterConfig& cfg) const;
    /// Once a pass, after the monsters are seen: the whole map (Eyes::world),
    /// whether the seek is on (seek_on_) -- not while the walk back comes
    /// first -- the floors and the attack points worked out again when due
    /// (kSeekPlanEveryMs, a landing, the box or what was learned), the
    /// monsters' snapshot every kSeekSnapshotMs, the status.
    void seek_look(const HunterConfig& cfg, std::uint64_t now);
    /// The rules the planner goes by, in world pixels.
    SeekRules seek_rules(const HunterConfig& cfg) const;
    /// The look at monster `slot`, `gen`; null when the world list has none.
    const SeekLook* seek_look_of(int slot, std::uint32_t gen) const;
    /// With the seek on, after mark_out_of_area: those of `mobs` not within
    /// reach with no attack point marked NoWay -- a monster below a drop could
    /// go to left to the drop -- and those the seek passed over, GaveUp; NoWay
    /// said once a monster at INFO.
    void mark_no_way(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                     std::vector<Listed>& listed, std::uint64_t now);
    /// The seek's pick (the design's §4): the target `kept` (an index, -1
    /// none) in reach; else one in reach now; else, by tier and cost, the
    /// box's monsters the character gets to -- in view or not -- with
    /// choose_target's drop target below between your floor and the falls;
    /// the one gone for kept unless another is in a better tier, or
    /// kSeekSwitchPx cheaper with kSeekCommitMs since the last switch.
    SeekChoice seek_choose(const HunterConfig& cfg, const std::vector<Nameplate>& mobs, const Nameplate& me,
                           const std::vector<Listed>& listed, std::uint64_t now, int kept, bool* wounded);
    /// The walk branch's and the no-plate pass's: to the attack point of the
    /// monster gone for -- `target` its plate when in view -- by the first
    /// leg of its way: a walk along the floor (WalkFor::Floor, never a hop),
    /// a walk off an end, or a jump up (JumpUpFor::Seek, jump_go); held still
    /// in a flight. Stuck, not found there, or a lasting refusal: passed over
    /// (seek_skip) -- a hold or the game behind stood through, not counted.
    /// False when it has no way of its own: walked to as before.
    bool seek_go(const HunterConfig& cfg, const Nameplate* target, std::uint64_t now);
    /// With no plate to fight (`c` the pick): to the one out of view, or
    /// standing inside the box with nothing -- said every kSeekSaidMs -- or,
    /// outside it, not the seek's.
    SeekIdle seek_idle(const HunterConfig& cfg, const SeekChoice& c, std::uint64_t now);
    /// The give-up clock of `target`, not in reach, held while the seek gets
    /// nearer its attack point: no kSeekStuckMs of walking without
    /// kSeekProgressPx.
    bool seek_holds_clock(const Nameplate& target, std::uint64_t now) const;
    /// `target`, the monster gone for, in reach at the edge of its band on a
    /// walk away from it to its attack point, short of the walk's aim: not
    /// fought yet -- the turn to it would walk the character back out of the
    /// band. The in-reach branch waits until the point is got to -- and from
    /// there on fights it, the flag spent.
    bool seek_walks_away(const Nameplate& target);
    /// Monster `slot`, `gen` passed over by the seek until `until_ms`; and
    /// whether one is.
    void seek_skip(int slot, std::uint32_t gen, std::uint64_t until_ms, SeekNo why);
    bool seek_skipped(int slot, std::uint32_t gen, std::uint64_t now) const;
    /// The monster gone for let go of -- `why` for the log, null for none.
    void seek_let_go(const char* why);
    /// Once a pass, first: the time since the last pass to what that pass
    /// was spent on, for the minute line.
    void seek_time(std::uint64_t now);
    /// Into status_.
    void seek_publish();
    /// The minute line's part for the seek: "" with the seek never on this
    /// minute; and the flush of a minute cut short at stop().
    std::string seek_minute_text() const;
    void seek_minute_reset();

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
    /// The map tab's area against its map's file, this pass (track_zone_way);
    /// the way back last asked of the map, from where and for which box; the
    /// goal a walk back's give-up clock measures toward, and whether "no way
    /// back" was said.
    ZoneHere                  zone_;
    bool                      way_asked_ = false;
    MapPoint                  way_from_;
    HuntZone                  way_box_;
    std::optional<ZoneReturn> way_;
    bool                      return_goal_on_ = false;
    MapPoint                  return_goal_;
    bool                      said_no_way_ = false;
    /// Walks toward a target refused at the area's edge this minute, and
    /// when the last was said at INFO (kWalkKeptSayMs).
    int                       minute_walks_kept_ = 0;
    std::uint64_t             walk_kept_said_ms_ = 0;
    /// The map tab's hunting areas: what the other threads ask, under mutex_
    /// -- a stem, the delete or the aside of that map's -- and the hunter
    /// thread's own: the area waiting to apply, why, and whether its wait
    /// was logged.
    std::optional<Zone> zone_asked_;
    std::string         zone_clear_asked_;
    std::string         zone_aside_asked_;
    std::optional<Zone> zone_pending_;
    ZoneWait            zone_wait_ = ZoneWait::None;
    int                 zone_wait_px_ = 0;
    bool                said_zone_wait_ = false;

    // ---- the drop (HunterConfig::drop_down): the hunter's own thread's

    /// The game's map files and GoBot's own say against a drop: both set, or
    /// no drop (set_map_book, set_drop_veto). Which map the character is on,
    /// and whether its file is gone by.
    GameMapBook*                 book_ = nullptr;
    std::function<const char*()> drop_veto_;
    std::function<void(std::uint32_t, std::uint64_t)> on_attack_;     ///< set_on_attack
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
        /// The file the landing was read from: a landing judged on the floor
        /// it said, followed to where the character came down -- on a slope
        /// a few pixels across is that many lower or higher.
        std::shared_ptr<const GameMapBook::Page> page;
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
        /// A jump up's key, not a drop's (2026.11.05b): drop_gate is
        /// jump_gate's then. Its verdict, and what that learned: a no-jump
        /// point at the take-off, a floor solid from below (its point).
        bool          jump = false;
        JumpKind      kind = JumpKind::Up;
        JumpUpVerdict jverdict = JumpUpVerdict::None;
        bool          no_jump = false;
        bool          solid = false;
        MapPoint      solid_at;
    };
    PressRecord press_;
    /// The other kind's press, still watched when press_ opened: a drop's
    /// under a jump's, or a jump's under a drop's (kDropGapMs lets one follow
    /// the other). A map change within its kPressWatchMs may as well be its;
    /// a press of its own kind closes it (2026.11.05b).
    PressRecord press_other_;
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
    /// Where each monster last stood, by its own frames (note_stands): its
    /// slot and generation, the height its frames have put it at since
    /// `y_ms`, its last frame, and the point it last stood at.
    struct MobStand {
        std::uint32_t slot = 0;
        std::uint32_t gen = 0;
        int           y = 0;
        std::uint64_t y_ms = 0;
        std::uint64_t last_ms = 0;
        bool          stood = false;
        MapPoint      at;
    };
    std::vector<MobStand> mob_stands_;
    /// The minute line's for the drop's judging (drop_minute_text): looks at
    /// a target below that asked drop_rule, settles begun, and the refusals
    /// acted on for each reason (drop_refused) -- not a settle's waits.
    static constexpr std::size_t kDropWhys = static_cast<std::size_t>(DropWhy::AfterJump) + 1;
    int           minute_drop_looks_ = 0;
    int           minute_drop_settles_ = 0;
    int           minute_drop_why_[kDropWhys] = {};
    /// The minute line's: presses, and what came of them.
    int           minute_drops_ = 0;
    int           minute_drop_worked_ = 0;
    int           minute_drop_nothing_ = 0;
    int           minute_drop_jump_ = 0;
    int           minute_drop_rose_ = 0;
    int           minute_drop_off_map_ = 0;
    int           minute_drop_gate_ = 0;
    int           minute_drop_unknown_ = 0;

    // ---- the jump up (HunterConfig::jump_up, 2026.11.05b): the hunter's own
    // thread's

    /// A jump up under way: what for, the monster and the jump; walking to
    /// its take-off or settling there -- since when, whether a walk was held
    /// as it began, the pass that last renewed it, the token refused since,
    /// the fine taps made and the give-up clocks as it found them -- or in
    /// the air: the keys' times, the counts before them, where it stood and
    /// on which file.
    struct JumpUpState {
        JumpUpPhase   phase = JumpUpPhase::Idle;
        JumpUpFor     purpose = JumpUpFor::Target;
        int           slot = -1;
        std::uint32_t gen = 0;
        std::string   name;
        /// Target and Seek: the monster in the packets' world, the plates'
        /// height of it (Target), and whether one stands on the level.
        MapPoint      target_w;
        int           dy = 0;
        bool          level = false;
        JumpPlan      plan;
        std::uint64_t settle_from_ms = 0;
        std::uint64_t since0 = 0, reach0 = 0;
        bool          walk_held = false;
        std::uint64_t renewed_pass = 0;
        std::uint64_t token_from_ms = 0;
        int           taps = 0;
        std::uint64_t key_ms = 0;           ///< the first ↑ down
        std::uint64_t last_key_ms = 0;      ///< the last ↑ down
        bool          whole = false;        ///< JumpKeys::whole
        bool          air_known = false;
        PacketWorld::AirCounts air0;
        bool          took_off = false;
        int           x0 = 0, y0 = 0;       ///< the standing point it pressed from
        std::string   stem;
        std::shared_ptr<const GameMapBook::Page> page;
        HuntZone      box;
    };
    JumpUpState jump_;
    /// When the last jump's first ↑ went down, and whether its landing was
    /// judged since: jump_in_air. When the last toward a target worked.
    std::uint64_t jump_t_key_ = 0;
    bool          jump_landed_ = false;
    std::uint64_t last_jump_worked_ms_ = 0;
    std::uint64_t jump_backoff_ms_ = 0;     ///< no settle begun before this
    int           jump_fails_row_ = 0;
    int           jump_fails_run_ = 0;
    JumpUpOff     jump_latch_ = JumpUpOff::None;
    std::uint8_t  jump_kinds_off_ = 0;
    int           jump_kind_misses_[kJumpKinds] = {};
    int           jump_gravity_out_ = 0;
    /// The presses of the last minute, and those and the settles for each
    /// monster this run (Target, Seek).
    std::vector<std::uint64_t> jump_presses_;
    std::vector<TargetPresses> jump_targets_;
    /// Falls back onto each landing floor this session, by its two ends:
    /// kJumpUpSolidAfter makes it solid from below.
    struct FellOnto {
        std::string stem;
        MapPoint    l_end, r_end;
        int         n = 0;
    };
    std::vector<FellOnto> jump_fell_;
    /// Moves at every point or floor learned, or unlearned: the way back is
    /// asked of the map again (track_zone_way), as when the stand moves.
    std::uint32_t jump_epoch_ = 0;
    /// Whether the way last asked had jumps in it, and at which epoch.
    bool          way_jumps_ = false;
    std::uint32_t way_epoch_ = 0;
    /// The way back with jumps worked out for the log alone, the tick off --
    /// from where, at which epoch -- and whether a passing reason holds the
    /// walk back at its take-off this pass: its give-up clock stands.
    std::optional<ZoneReturn> jump_dry_way_;
    MapPoint      jump_dry_from_;
    bool          jump_dry_asked_ = false;
    bool          jump_wait_home_ = false;
    /// The gates the planner's rules point at (JumpRules::gates).
    std::vector<MapGate> jump_gates_;
    /// jump_marks' floors a jump lands on from where the character stands
    /// (jump_legs_from): worked out once a standing point, box, epoch and
    /// set of keys.
    std::vector<JumpPlan> jump_legs_;
    bool          jump_legs_known_ = false;
    MapPoint      jump_legs_from_;
    HuntZone      jump_legs_box_;
    std::uint32_t jump_legs_epoch_ = 0;
    std::uint8_t  jump_legs_kinds_ = 0;
    /// Said: the last refusal per monster or way and those at INFO; the last
    /// "would jump" per monster or way, and when any was.
    int           jump_said_slot_ = -2;
    std::uint32_t jump_said_gen_ = 0;
    JumpUpWhy     jump_said_why_ = JumpUpWhy::None;
    std::uint64_t jump_said_info_ = 0;
    int           jump_would_slot_ = -2;
    std::uint32_t jump_would_gen_ = 0;
    int           jump_would_kind_ = -1, jump_would_top_ = 0;
    std::uint64_t jump_would_ms_ = 0;
    /// Your own 0x002A and 0x0031 traced at DEBUG until this, from this report on.
    std::uint64_t jump_trace_until_ = 0;
    std::uint64_t jump_traced_ms_ = 0;
    /// The minute line's (jump_minute_text): looks at a target above or a
    /// way's leg that asked the rule, the would-jumps, settles begun, the
    /// presses by key and what came of them, g measured, and the refusals
    /// acted on for each reason.
    static constexpr std::size_t kJumpUpWhys = static_cast<std::size_t>(JumpUpWhy::OffColumn) + 1;
    int           minute_jump_looks_ = 0;
    int           minute_jump_would_ = 0;
    int           minute_jump_settles_ = 0;
    int           minute_jumps_[kJumpKinds] = {};
    int           minute_jump_worked_ = 0;
    int           minute_jump_fell_ = 0;
    int           minute_jump_kind_miss_ = 0;
    int           minute_jump_nothing_ = 0;
    int           minute_jump_lower_ = 0;
    int           minute_jump_higher_ = 0;
    int           minute_jump_unknown_ = 0;
    int           minute_jump_gate_ = 0;
    double        minute_jump_g_sum_ = 0.0;
    int           minute_jump_g_n_ = 0;
    int           minute_jump_why_[kJumpUpWhys] = {};

    // ---- "Tự tìm quái trong vùng" (HunterConfig::seek_in_zone, 2026.11.05b):
    // the hunter's own thread's

    /// This pass: the setup wanted, the whole map had (Eyes::world), and the
    /// seek on.
    bool          seek_wanted_ = false;
    bool          seek_world_on_ = false;
    bool          seek_on_ = false;
    std::vector<SeekMob> seek_world_;
    /// The floors worked out last (seek_map): from where, in which box, at
    /// which of the session's learnings, with which keys, and when; and the
    /// attack points from them (seek_looks_), when.
    SeekMap       seek_map_;
    bool          seek_map_known_ = false;
    MapPoint      seek_map_from_;
    HuntZone      seek_map_box_;
    const GameMapBook::Page* seek_map_page_ = nullptr;
    std::uint32_t seek_map_epoch_ = 0;
    std::uint8_t  seek_map_kinds_ = 0;
    bool          seek_map_jumps_ = false;
    bool          seek_map_go_ = false;
    std::uint64_t seek_planned_ms_ = 0;
    std::vector<SeekLook> seek_looks_;
    std::uint64_t seek_looked_ms_ = 0;
    /// The monster gone for: which, its plan, since when and the last switch;
    /// picked as the drop's target below (walked to as before); its progress
    /// -- the cost at the last kSeekProgressPx, the walking time since, the
    /// pass that counted -- when it was got to (its attack point), the way it
    /// last walked, and whether its plate was in view at the last pass.
    struct SeekState {
        bool          on = false;
        int           slot = -1;
        std::uint32_t gen = 0;
        SeekPlan      plan;
        bool          planned = false;
        bool          by_drop = false;
        std::uint64_t since_ms = 0;
        std::uint64_t switch_ms = 0;
        int           mark_cost = 0;
        std::uint64_t quiet_ms = 0;
        std::uint64_t quiet_pass = 0;
        std::uint64_t quiet_seen_ms = 0;
        std::uint64_t arrived_ms = 0;
        bool          reached = false;          ///< in reach after a walk to it: counted once
        /// The walk goes away from the monster to its attack point (aim_x):
        /// not stopped for the monster in reach until there
        /// (seek_walks_away).
        bool          away = false;
        int           aim_x = 0;
        int           dir = 0;
        bool          in_view = false;
        MapPoint      mob;
        std::uint8_t  mob_why = 0;
        std::uint64_t mob_age_ms = 0;
    };
    SeekState seek_;
    /// Monsters the seek passes over a while, by slot and generation.
    struct SeekSkip {
        int           slot = -1;
        std::uint32_t gen = 0;
        std::uint64_t until_ms = 0;
        SeekNo        why = SeekNo::Passed;
    };
    std::vector<SeekSkip> seek_skip_;
    /// Monsters below whose drop was refused, with the character right above
    /// them, for a reason the next look gives again (drop_lasting): not the
    /// drop's target until until_ms -- their own way instead, a walk off an
    /// end. What drop_toward refused at this pass: DropWhy::None when it went,
    /// waited or did not judge.
    std::vector<SeekSkip> seek_no_drop_;
    DropWhy       drop_why_pass_ = DropWhy::None;
    /// Said: the last goal said at INFO; the NoWay monsters said at INFO
    /// (slot, gen); nothing to seek since when, and last said; the last
    /// snapshot.
    std::uint64_t seek_said_goal_ms_ = 0;
    std::vector<std::pair<int, std::uint32_t>> seek_said_no_way_;
    bool          seek_none_ = false;
    SeekNone      seek_none_why_ = SeekNone::None;
    std::uint64_t seek_none_since_ms_ = 0;
    std::uint64_t seek_none_said_ms_ = 0;
    std::uint64_t seek_snapshot_ms_ = 0;
    /// The minute line's: goals by tier, got to, switched, not found, stuck,
    /// NoWay marks; the time with nothing to seek; and the time budget --
    /// each pass's time, up to 250 ms, to what it was spent on.
    enum class PassKind : std::uint8_t {
        Attacking, Approaching, Seeking, Nothing, WalkingBack, Waiting, Held, Behind, NoPlayer, Screen
    };
    static constexpr std::size_t kPassKinds = static_cast<std::size_t>(PassKind::Screen) + 1;
    int           minute_seek_goals_[3] = {};
    int           minute_seek_reached_ = 0;
    int           minute_seek_switched_ = 0;
    int           minute_seek_not_found_ = 0;
    int           minute_seek_stuck_ = 0;
    int           minute_seek_no_way_ = 0;
    int           minute_seek_passes_ = 0;
    std::uint64_t minute_seek_none_ms_ = 0;
    std::uint64_t minute_time_ms_[kPassKinds] = {};
    std::uint64_t pass_at_ms_ = 0;
};

} // namespace gobot
