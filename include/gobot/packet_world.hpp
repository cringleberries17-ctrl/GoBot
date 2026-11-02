#pragma once

#include "gobot/common.hpp"
#include "gobot/own_position.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace gobot {

/// You and every monster of the map, from the game's own frames, worked out
/// to any moment asked for.
///
/// The P0-2 recordings (2026-10-18; docs/protocol-findings.md §4e and §4f)
/// showed both in the stream, in world pixels with y growing downward:
///
///   - your client tells the server where your character is, over UDP,
///     whenever a walk starts, stops or turns, a jump leaves the ground or
///     lands, and a skill is cast (read_own_position); and the server puts
///     you at a spawn point at every map entry (read_map_entry);
///   - the server sends every monster of the map, near you or not: a table at
///     each map load (read_map_monsters), a spawn 2-4 s after each kill
///     (read_monster_spawn), and an event at each stop, walk and hit
///     (read_monster_state).
///
/// None of it comes on a clock. Standing still sends nothing, and a walking
/// monster nothing until it turns or stops. So the world keeps the last fix
/// of each and works out where it is at the moment asked for: a walk goes on
/// along its facing at its speed, a jump rises and holds at its top, and
/// whatever stands stays where it is.
///
/// The capture thread feeds it while anything else reads it: on_packets(),
/// reset(), view() and stats() each take the one lock. Nothing live feeds it
/// yet -- plan item P0-3 wires it in -- so GoBot behaves as it did. It is
/// tested on hand-built frames only: no recording of the game's real bytes
/// has been through it.
class PacketWorld {
public:
    // ---- the numbers the working-out rests on

    /// A speed in these frames is pixels per step of 25 ms: 40 steps a second.
    /// Your walking 9.0 is then 360 px/s and a jump's -20 is -800 px/s, GoC's
    /// own walking and jump speeds exactly; a monster's 2.6 is 104 px/s, where
    /// 102 was measured.
    static constexpr double kStepsPerSecond = 40.0;

    /// How long a walk of yours is followed with no report. Letting go of the
    /// key sends a stop -- 930 of the 933 walks of the hunt of 2026-10-27 had
    /// one within a second -- but a walk into a wall sends nothing; past this,
    /// your position is held where the walk had taken it and marked uncertain
    /// until the next report. Standing still is never uncertain: it sends
    /// nothing because nothing changes.
    ///
    /// Long enough for a walk the hunter holds from one look to the next
    /// (hold_walk), which sends nothing between its start and its stop: it is
    /// let go of and taken up again at Hunter::kWalkRenewMs, inside this. At
    /// 600 ms, as it was while the hunter stepped, every walk longer than that
    /// left the packets for the screen. Any other walk -- a step back, a turn,
    /// one of the player's own -- is followed kOwnStepReckonMs, as before: the
    /// caller of view() says which (Hunter::walk_held_until_ms).
    static constexpr std::uint64_t kOwnReckonMs = 3000;
    static constexpr std::uint64_t kOwnStepReckonMs = 600;

    /// The same off the ground, where the next report is the landing; and how
    /// long a jump is taken to last with none. A report of another kind later
    /// than this after the last 0x002A that had you in the air has you on the
    /// ground again: the landing was lost on the way, or refused. Otherwise one
    /// lost landing would keep you in the air until your next jump, every walk
    /// followed for this long and standing still uncertain.
    static constexpr std::uint64_t kAirReckonMs = 1500;

    /// How long a monster's walk lasts: about 200 steps, counted from the walk's
    /// start and not from its turns (a median of 5.1 s over 249 walks). A
    /// walker is followed this long and then held where it got to.
    static constexpr std::uint64_t kMobWalkMs = 5000;

    /// Gravity, in pixels per second per second: GoC's. A jump at -800 px/s
    /// tops out about 133 px up, a third of a second after it left the ground.
    static constexpr double kGravity = 2400.0;

    /// A walk start of yours counts as followed by a stop when one comes within
    /// this long (Stats::own_walks_stopped): as long as a walk is followed, so
    /// that a held walk let go of after a second or two is not counted as one
    /// whose stop was lost. It was 1 s while the hunter stepped.
    static constexpr std::uint64_t kStopWithinMs = kOwnReckonMs;

    /// x and y never go past this: the frames that carry them as whole pixels
    /// carry them as u16. A report of yours outside 0..kMaxCoord is refused.
    static constexpr float kMaxCoord = 65535.0f;

    /// The fastest anything is taken to move, in pixels per step: you walk at 9
    /// and jump at -20 to -36, and a monster walks at 2.6. Only a speed that is
    /// followed is held to it. A report of yours, a monster's walk, or a table
    /// with a living monster, whose speed is past it or below 0 is refused, and
    /// so is a jump that rises faster. No frame is lost over a speed that
    /// nothing follows: a landing's or a fall's vertical speed, or the 7 or 18
    /// a blow throws a monster at, which a harder blow may pass.
    static constexpr float kMaxSpeed = 50.0f;

    /// How many opcodes it reads: see opcodes().
    static constexpr std::size_t kOpcodeCount = 9;

    /// A 0x0038 of a state read_monster_state does not know moves its monster
    /// to the x and y it carries -- standing, its HP as it was -- only when
    /// that is within this of where the monster was worked out to be, both
    /// ways (read_monster_spot). No recording had such a state, so whether
    /// those bytes hold its place there is not known: the hunt of 2026-10-27
    /// had 3,399 of them against 5,804 of the three known, and refused, they
    /// left a monster where its last stop or walk had it.
    static constexpr float kUnknownPlacePx = 200.0f;

    /// The frames of an unknown state written to the log, for working out what
    /// the state is: this many of each.
    static constexpr int kUnknownLogged = 2;

    // ---- what it knows

    /// You, as worked out for one moment.
    struct Me {
        bool          known = false;    ///< a report or a map entry has come since the start or reset()
        std::uint32_t id = 0;           ///< your character's id, as your last report gave it
        float         x = 0.0f;         ///< world pixels
        float         y = 0.0f;         ///< world pixels, growing downward
        int           facing = 0;       ///< +1 toward larger x, -1 toward smaller x, 0 not known
        /// Which way the last 0x0027 said you face (+1, -1; 0 while none has
        /// since the start, a reset or a map entry), and when it came. Only a
        /// 0x0027's: its facing byte is the one checked against the game, and
        /// this is what the hunter turns the character by (WorldFix::facing),
        /// while `facing`, which a 0x0029 or a 0x0028 sets too, is only what a
        /// walk is followed by. A cast or a jump's 0x002A says nothing of it,
        /// and leaves both as they were.
        int           said_facing = 0;
        std::uint64_t facing_ms = 0;
        float         speed = 0.0f;     ///< pixels per step, from the last report that gives one
        bool          moving = false;   ///< going sideways, on the ground or in the air: speed above 0
        /// Off the ground, by the state of the last 0x002A. A report of another
        /// kind more than kAirReckonMs after it puts you back on the ground,
        /// its landing taken to have been lost on the way.
        bool          airborne = false;
        /// Moving, but for longer than it is followed with no report
        /// (View::own_follow_ms, kAirReckonMs), or without a facing to follow;
        /// or in the air with no landing for longer than kAirReckonMs. x and y
        /// are then where the working-out stopped, not where you are.
        bool          uncertain = false;
        std::uint64_t report_ms = 0;    ///< when the last report or map entry came
        /// Where that report or map entry put you, as it came: x and y worked
        /// out from it. A walk into a wall sends nothing, and x runs on while
        /// these stay.
        float         report_x = 0.0f;
        float         report_y = 0.0f;
        std::optional<OwnReport> kind;  ///< what the last fix was: none for a map entry
    };

    /// One slot's monster, as worked out for one moment.
    struct Mob {
        std::uint32_t slot = 0;         ///< its place on the map: what its frames name it by
        std::uint32_t type_id = 0;      ///< from the table or a spawn; 0 until one names it
        std::uint8_t  level = 0;        ///< likely the level, from the same; 0 until then
        float         x = 0.0f;         ///< world pixels, the space of your own
        float         y = 0.0f;         ///< as its last frame said: 60-69 px out while a walker crosses a ledge
        int           facing = 0;       ///< +1 toward larger x, -1 toward smaller x, 0 if neither
        float         speed = 0.0f;     ///< pixels per step, as its last frame said: 7 or 18 after a blow
        bool          moving = false;   ///< walking now: a walk under way that has not run its kMobWalkMs
        std::uint32_t hp = 0;           ///< 0 once dead
        std::uint32_t max_hp = 0;
        /// max_hp came from the table or a spawn. Otherwise it is the most HP
        /// the slot has been seen with, which is too low for a monster first
        /// seen hurt.
        bool          max_hp_known = false;
        /// Killed, or at 0 HP or below in the map's table, and its slot not yet
        /// given a new monster.
        bool          dead = false;
        std::uint64_t dead_ms = 0;      ///< when it was killed, or the table came that had it dead
        std::uint64_t event_ms = 0;     ///< when its last frame came
    };

    /// Everything, worked out for one moment.
    struct View {
        Me               me;
        std::vector<Mob> mobs;                  ///< in slot order, the dead ones too
        std::uint16_t    map = 0;               ///< from the last map entry: (map, area) is one place
        std::uint16_t    area = 0;
        std::uint64_t    map_since_ms = 0;      ///< when it came; 0 if none has since the start or reset()
        bool             table_seen = false;    ///< the map's table has come since then
        std::uint64_t    first_monster_ms = 0;  ///< the first monster frame since then; 0 for none
        /// How many batches held a frame the world used: a number that moves
        /// when what it knows does, for the hunter's picture number. It never
        /// goes back, not even at reset(), so a number taken before one is
        /// never taken for a newer one.
        std::uint64_t    updates = 0;
        /// Map entries ever used: a number that moves at each one, and that
        /// reset() -- a capture started again -- does not take back. Whether
        /// the character has been through a door since a moment ago, when the
        /// map itself is not known again until the next entry.
        std::uint32_t    map_epoch = 0;
        /// Where the last of those put you, its spawn point: kept through
        /// reset() as map_epoch is, so an entry first looked at after a
        /// capture started again is still measured from where it put you. 0
        /// before any.
        float            entry_x = 0.0f;
        float            entry_y = 0.0f;
        /// How long a walk of yours on the ground was followed with no report
        /// in this view: kOwnReckonMs for one the hunter holds, otherwise
        /// kOwnStepReckonMs (view()'s own_follow_ms).
        std::uint64_t    own_follow_ms = kOwnReckonMs;

        /// The monster in `slot`, or none.
        const Mob* mob(std::uint32_t slot) const;
    };

    /// What it was fed since the start or reset(), for the log.
    struct Stats {
        /// One opcode's frames, of those that came from the side it is read
        /// from: your client over UDP, or the server.
        struct Frames {
            std::uint16_t opcode = 0;
            std::uint64_t used = 0;         ///< read and applied
            std::uint64_t wrong_size = 0;   ///< refused: not the size every one of the recordings had
            std::uint64_t refused = 0;      ///< refused: the right size, but unreadable or out of range
        };
        std::array<Frames, kOpcodeCount> frames{};  ///< in the order opcodes() lists them
        Frames of(std::uint16_t opcode) const;      ///< all 0 for an opcode it does not read

        /// Your reports used, by kind: the used count of the kind's opcode.
        std::uint64_t own(OwnReport kind) const;
        std::uint64_t foreign = 0;          ///< reports of your client's kinds under another id: passed over

        std::uint64_t map_entries = 0;
        std::uint64_t tables = 0;
        std::uint64_t spawns = 0;
        std::uint64_t stops = 0;
        std::uint64_t walks = 0;            ///< walk events, turns included
        std::uint64_t hits = 0;
        std::uint64_t kills = 0;            ///< hits that left a living monster at 0 HP or below
        std::uint64_t unknown_states = 0;   ///< 0x0038 of a state read_monster_state does not know
        /// Of those, how many put their monster where they said: near enough
        /// to where it was worked out to be (kUnknownPlacePx). The rest are
        /// refused.
        std::uint64_t unknown_placed = 0;
        /// Which states those were, and how many of each: the first
        /// kUnknownKinds states met, in the order they were -- with, of those
        /// for a monster known and alive, how far on average the x, y they
        /// carry was from where it was worked out to be (the larger of the
        /// two), and how many were near enough to place it. Small and steady,
        /// those bytes are its place; large, they are something else.
        struct Kind {
            std::uint8_t  state = 0;
            std::uint64_t count = 0;
            std::uint64_t judged = 0;
            double        off_px = 0.0;       ///< summed over `judged`
            std::uint64_t placed = 0;
        };
        static constexpr std::size_t kUnknownKinds = 6;
        std::array<Kind, kUnknownKinds> unknown_kinds{};

        /// Your walks started from standing (0x0027), and of those how many a
        /// report that has you standing followed within kStopWithinMs, and how
        /// many none did: whether letting go of a walk key always says so.
        std::uint64_t own_walks = 0;
        std::uint64_t own_walks_stopped = 0;
        std::uint64_t own_walks_unstopped = 0;

        /// Your 0x002A used, by state: 1 off the ground, 6 a second jump, 2 a
        /// high jump, 0 landed, and any other; and your 0x0031 casts used
        /// while in the air. Whether a jump attack or a double jump went out
        /// as it was meant to, for the log.
        std::uint64_t air_jumps = 0;
        std::uint64_t air_second = 0;
        std::uint64_t air_high = 0;
        std::uint64_t air_landed = 0;
        std::uint64_t air_other = 0;
        std::uint64_t casts_in_air = 0;

        /// The last map entry: the place, where it put you, and when. 0 for
        /// none since the start or reset().
        std::uint16_t entry_map = 0;
        std::uint16_t entry_area = 0;
        float         entry_x = 0.0f;
        float         entry_y = 0.0f;
        std::uint64_t entry_ms = 0;

        std::uint64_t last_own_ms = 0;          ///< when a report of yours was last used
        std::uint64_t last_monster_ms = 0;      ///< when a monster frame was last used
        std::uint64_t last_foreign_ms = 0;      ///< when one under another id last came
        std::uint64_t last_wrong_size_ms = 0;   ///< when a frame of the wrong size last came
        /// When one of your reports' opcodes last came of the wrong size: a
        /// game update that moved a report of yours, whose walks then go
        /// unread -- where you are is not to be gone by (world_fix).
        std::uint64_t last_own_wrong_size_ms = 0;
    };

    PacketWorld();
    PacketWorld(const PacketWorld&) = delete;
    PacketWorld& operator=(const PacketWorld&) = delete;

    /// One batch of frames, in the order they came, each at its own
    /// timestamp_ms. A frame of another opcode, or from the other side, is
    /// passed over; one of a size the recordings never had is refused and
    /// counted, as a game update that moved its fields would make it.
    void on_packets(const std::vector<GamePacket>& batch);

    /// Forget everything -- you, the monsters, the map and the counts -- as at
    /// a new capture. View::updates and View::map_epoch go on from where they
    /// were, and View::entry_x, entry_y stay with the last entry.
    void reset();

    /// You and every monster, worked out to `now_ms` on the clock the frames
    /// are stamped with -- a walk of yours on the ground followed for
    /// `own_follow_ms` with no report: kOwnReckonMs for a walk the hunter
    /// holds, kOwnStepReckonMs for any other.
    View view(std::uint64_t now_ms, std::uint64_t own_follow_ms = kOwnReckonMs) const;

    Stats stats() const;

    /// The opcodes it reads, for a capture's opcode filter: your reports
    /// 0x0027, 0x0028, 0x0029, 0x002A and 0x0031; the monsters' 0x0038, 0x003F
    /// and 0x0042; and the map entry, 0x0086.
    static const std::vector<std::uint16_t>& opcodes();

private:
    /// What became of one frame of an opcode it reads, from its side and of
    /// its size.
    enum class Taken : std::uint8_t {
        Used,       ///< applied
        Refused,    ///< unreadable, or values that cannot be right
        Foreign,    ///< a report under another id than yours
    };

    Taken take_own(const GamePacket& p);
    Taken take_entry(const GamePacket& p);
    Taken take_table(const GamePacket& p);
    Taken take_spawn(const GamePacket& p);
    Taken take_state(const GamePacket& p);
    /// A 0x0038 read_monster_state did not take: counted, and its monster
    /// placed when its state is not one of the three known (kUnknownPlacePx).
    Taken take_other_state(const GamePacket& p);

    /// A monster frame was used at `t`.
    void heard_monster(std::uint64_t t);
    /// A walk start of yours with no stop by `t` is one no stop followed.
    void time_walk(std::uint64_t t);
    /// reset(), under the lock already taken.
    void forget();

    /// Your last fix, as it came.
    struct Fix {
        Me            me;                  ///< x, y, speed and facing as the fix left them
        double        rise = 0.0;          ///< vertical speed at it, px/s, below 0 upward; 0 where y holds
        std::uint64_t air_from_ms = 0;     ///< when the last 0x002A that has you in the air came
        bool          id_locked = false;   ///< reports under another id than me.id are not yours
    };

    /// One slot, as its last frame left it.
    struct Slot {
        Mob           at;                 ///< x, y, speed and facing as that frame left them
        std::uint64_t walk_from_ms = 0;   ///< when its walk began: a turn does not move it
    };

    mutable std::mutex            mutex_;
    Fix                           fix_;
    std::map<std::uint32_t, Slot> slots_;
    std::uint16_t                 map_ = 0;
    std::uint16_t                 area_ = 0;
    std::uint64_t                 map_since_ms_ = 0;
    bool                          table_seen_ = false;
    std::uint64_t                 first_monster_ms_ = 0;
    std::uint64_t                 updates_ = 0;
    std::uint32_t                 map_epoch_ = 0;         ///< View::map_epoch: never cleared
    float                         entry_x_ = 0.0f;        ///< View::entry_x, entry_y: never cleared
    float                         entry_y_ = 0.0f;
    bool                          walk_timed_ = false;    ///< a walk start of yours waits for its stop
    std::uint64_t                 walk_from_ms_ = 0;
    Stats                         stats_;
    /// How many frames of each unknown state have gone to the log.
    std::array<std::uint8_t, 256> unknown_logged_{};
};

} // namespace gobot
