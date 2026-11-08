#pragma once

// "Thử" beside the jump key: press it once and see whether the character
// jumped -- and, without pressing anything, what else that key is already
// used for.
//
// The jump key is the one key of the hunter's that is a guess: GoC's built-in
// default is the Up arrow, every hunt.ini before the jump box said Space, and
// Space is this game's attack. So the player sets it and tries it: look at the
// character's name plate, press the key once, watch the plate rise and fall.
// The check is a class of its own, on seams, so that it is tested offline on
// scripted plates and never reaches for the real keyboard in a test; the GUI
// and gobot_cli --jumptest run the same one.
//
// And "Thử ↑", "Thử ↑↑", "Thử Ctrl + ↑" (2026.11.05b, run_kind): one jump of
// each kind the hunter's jumps up use, judged by your client's own take-offs
// by kind -- a jump, a second one in the air, a high jump -- with the flight's
// time and the gravity it shows, measured. Never under a floor the map shows
// above: that jump may land on it.

#include "gobot/execution_coordinator.hpp"
#include "gobot/hotkeys.hpp"
#include "gobot/jump_plan.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/packet_world.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gobot {

class InputManager;

class JumpCheck {
public:
    /// The character's plate: MobDetector::player, which says whether it was
    /// seen just now or only remembered. And, when set, your client's own
    /// take-offs as the packets count them (the 0x002A it sends as the
    /// character leaves the ground, jumps again in the air or jumps high): a
    /// running total, or nothing when the packets cannot tell -- no capture on
    /// the game, your id not known. Asked the moment before the press and
    /// after the watch; while it can tell, its rise decides, and the plate is
    /// only watched beside it (GuiApp::packet_takeoffs).
    ///
    /// For the kinds (run_kind): your take-offs by kind, nothing when the
    /// packets cannot tell (GuiApp::packet_air_counts); your own reports since
    /// a moment, oldest first (GuiApp::packet_own_since); and how far above
    /// your feet the nearest floor the map shows is, within kJumpUpCeilingPx
    /// (floor_above) -- nothing for none, or for no map known.
    struct Eyes {
        std::function<bool(Nameplate&, bool*)>        player;
        std::function<std::optional<std::uint64_t>()> takeoffs;
        std::function<std::optional<PacketWorld::AirCounts>()>           air;
        std::function<std::vector<PacketWorld::OwnTrace>(std::uint64_t)> own_since;
        std::function<std::optional<int>()>                              floor_above;
    };
    /// How one jump of a kind went out (Hands::jump).
    struct Press {
        bool          went = false;     ///< its ↑ went down
        /// All of it: ↑↑'s second ↑ as well, Ctrl + ↑'s ↑ held its time
        /// inside Ctrl.
        bool          whole = false;
        std::uint64_t down_ms = 0;      ///< its first ↑'s down, on the frames' clock (now_ms())
        std::uint32_t held_ms = 0;      ///< the longest an ↑ of it was held
    };
    /// `focus` brings the game to the front and says whether it is;
    /// `focused` only says whether it is, the moment before the press --
    /// `focus` is asked again when it is not set; `key` presses one key for
    /// `hold` ms and says whether it went out. `jump` makes one jump of a
    /// kind, asking `go_on` through it: ↑ InputManager::jump_once, ↑↑
    /// jump_twice at kJumpUpSecondAtMs, Ctrl + ↑ high_jump -- always ↑.
    struct Hands {
        std::function<bool()>                   focus;
        std::function<bool()>                   focused;
        std::function<bool(std::uint32_t, int)> key;
        std::function<Press(JumpKind, const std::function<bool()>&)> jump;
    };
    /// The steady clock and a real sleep, unless set.
    struct Clock {
        std::function<std::uint64_t()>     now_ms;
        std::function<void(std::uint32_t)> sleep_ms;
    };

    explicit JumpCheck(ExecutionCoordinator& coord) : coord_(coord) {}

    void set_eyes(Eyes e)   { eyes_ = std::move(e); }
    void set_hands(Hands h) { hands_ = std::move(h); }
    void set_clock(Clock c) { clock_ = std::move(c); }

    enum class Outcome {
        Jumped,         ///< the plate rose kJumpedPx or more; or, by the packets, a take-off went out
        NoJump,         ///< it stayed within kStillPx over kStillReads fresh reads; or, by the packets, none went out
        Unsure,         ///< something in between, or too few reads to say
        NoPlate,        ///< pressed, but the character was not seen to check by
        Held,           ///< a hold is on: nothing pressed
        Busy,           ///< another job kept the keys: nothing pressed
        NotInFront,     ///< the game was not in front: nothing pressed
        Stopped,        ///< told to stop before the press -- F12, GoBot closing: nothing pressed
        NoKey,          ///< no jump key: nothing pressed
        NeverPressed,   ///< the key is ↓, which GoBot never presses: nothing pressed
        /// The game was in front the moment before the press, and the press
        /// did not come back as gone out: the game lost focus during it. The
        /// key may have reached the game; nothing was checked.
        Unchecked,
    };
    struct Result {
        Outcome     outcome = Outcome::NoKey;
        bool        pressed = false;   ///< the one press went out, or may have (Unchecked)
        int         rose_px = 0;       ///< the highest the plate went above where it stood
        int         reads = 0;         ///< fresh reads of the plate after the press
        std::string hold;              ///< what the hold is, for Held
        /// Jumped or NoJump said by the packets' take-offs (Eyes::takeoffs),
        /// and how many went out after the press; the plate's figures above
        /// are then only what it did beside them.
        bool        by_packets = false;
        int         takeoffs = 0;
    };

    /// One check: the plate read where it stands, the game in front, the
    /// coordinator's token, `jump_key` pressed exactly once for kHoldMs, and
    /// the plate watched for kWatchMs. When the packets can tell the moment
    /// before the press and again after the watch, their take-offs decide: one
    /// or more is Jumped, none NoJump, whatever the plate did -- on a map where
    /// the camera follows a jump the plate hardly moves. Otherwise the plate
    /// decides, as it always has. Nothing at all is pressed while a hold is
    /// on. `stop`, when set before the press, means no press at all; after it,
    /// the looking is cut short and what has been seen is the result -- the
    /// packets then decide only for a take-off already counted, and the plate
    /// otherwise.
    Result run(std::uint32_t jump_key, const std::atomic<bool>* stop = nullptr);

    /// What the result says to the player, in Vietnamese. `key` is the key's
    /// name as the key box shows it.
    static std::string said_vi(const Result& r, const std::string& key);
    /// The same, in the log's English.
    static const char* outcome_name(Outcome o);

    static constexpr int           kBaselineReads = 3;     ///< fresh reads before the press
    static constexpr std::uint32_t kBaselineWaitMs = 3000; ///< for them, at most
    static constexpr int           kHoldMs = 80;
    static constexpr std::uint32_t kWatchMs = 1500;
    /// Looks are this far apart: about the detector's own rate, so each is
    /// mostly a new frame.
    static constexpr std::uint32_t kLookEveryMs = 150;
    static constexpr int           kJumpedPx = 25;
    static constexpr int           kStillPx = 10;
    static constexpr int           kStillReads = 4;

    // ---- "Thử ↑", "Thử ↑↑", "Thử Ctrl + ↑" (2026.11.05b) ----

    enum class KindOutcome {
        /// The kind's take-off counted: ↑ a jump going up (state 1, not a
        /// walk-off), ↑↑ that and a second jump (state 6), Ctrl + ↑ a high
        /// jump (state 2).
        Jumped,
        KindMissed,     ///< a take-off, but not the kind's: ↑↑ with no second, Ctrl + ↑ a plain jump
        NoJump,         ///< no take-off counted after the press
        Cut,            ///< pressed in part: ↑↑'s second ↑ not sent, Ctrl + ↑'s ↑ up too soon
        Unsure,         ///< the watch cut short with nothing counted, or the packets lost meanwhile
        NotSent,        ///< no ↑ went down at the press: the game left the front, or a stop or a hold came
        FloorAbove,     ///< the map shows a floor within kJumpUpCeilingPx above: nothing pressed
        NoPackets,      ///< the packets cannot tell the kinds apart: nothing pressed
        Held,           ///< a hold is on: nothing pressed
        Busy,           ///< another job kept the keys: nothing pressed
        NotInFront,     ///< the game was not in front: nothing pressed
        Stopped,        ///< told to stop before the press: nothing pressed
    };
    struct KindResult {
        JumpKind      kind = JumpKind::Up;
        KindOutcome   outcome = KindOutcome::NoPackets;
        bool          pressed = false;      ///< an ↑ of it went down
        std::uint32_t held_ms = 0;          ///< the longest an ↑ was held
        bool          held_long = false;    ///< past InputManager::kJumpHeldLongMs
        std::string   hold;                 ///< what the hold is, for Held
        int           floor_above_px = 0;   ///< FloorAbove: how far above the feet
        /// The packets' counts from the press to the end of the watch: the
        /// jumps going up (walk-offs left out), the walk-offs, the second
        /// jumps, the high jumps and the landings.
        int           jumps = 0;
        int           walkoffs = 0;
        int           second = 0;
        int           high = 0;
        int           landed = 0;
        /// From your own reports since the press (flight_of), -1 for none
        /// seen: the first take-off after the key, and from it to the
        /// landing -- a 0x002A landed, or failing one the first report of a
        /// walk, never a cast's.
        int           takeoff_ms = -1;
        int           flight_ms = -1;
        int           rose_px = 0;          ///< how far above the take-off it landed: 0 on flat ground
        /// g≈, px/s², from the last push up to the landing (gravity_from):
        /// what the hunter's jumps are modelled by, kJumpUpGravity.
        std::optional<double> gravity;
    };

    /// One jump of `kind`, always ↑, and the packets watched kKindWatchMs or
    /// until it lands. Nothing pressed while a hold is on; nor when the map
    /// shows a floor within kJumpUpCeilingPx above (Eyes::floor_above) -- no
    /// gate is asked of: Ctrl + ↑ on a gate does not change the map (the
    /// owner, 2026-11-05) -- nor when the packets cannot tell the kinds apart
    /// (Eyes::air). The token is taken only from nobody, as the drop takes
    /// it. `stop` as run()'s: before the press nothing goes; after it, what
    /// was counted is the result.
    KindResult run_kind(JumpKind kind, const std::atomic<bool>* stop = nullptr);
    /// One jump of `kind` through `input`'s own presses, as Hands::jump has
    /// it: ↑ InputManager::jump_once, ↑↑ jump_twice at kJumpUpSecondAtMs,
    /// Ctrl + ↑ high_jump -- always ↑ (0x26), never ↓, never the drop's chord;
    /// the hunter's own (Hunter::send_jump) the same way. What GoBot's "Thử
    /// ↑", "Thử ↑↑" and "Thử Ctrl + ↑" press.
    static Press press_with(InputManager& input, JumpKind kind, const std::function<bool()>& go_on);
    static std::string kind_said_vi(const KindResult& r);
    static const char* kind_outcome_name(KindOutcome o);
    /// A kind's watch: the longest flight -- Ctrl + ↑ falling back, ↑↑ --
    /// with its press, and more.
    static constexpr std::uint32_t kKindWatchMs = 2500;

    /// What your own reports since a key say of its jump: the first take-off
    /// -- a 0x002A in the air going up -- the last push up before the landing
    /// (↑↑'s second), and the landing: the first 0x002A landed after the
    /// take-off or, failing one, the first report of a walk (0x0027, 0x0028,
    /// 0x0029) -- never a cast, whose y holds at the top.
    struct Flight {
        bool                    took_off = false;
        PacketWorld::OwnTrace   off;
        PacketWorld::OwnTrace   push;       ///< the last push up: `off` itself for one push
        bool                    landed = false;
        PacketWorld::OwnTrace   land;
    };
    static Flight flight_of(const std::vector<PacketWorld::OwnTrace>& reports);

    // ---- the key, and what else it is ----

    enum class Advice {
        Clean,          ///< nothing else on it
        NotSet,         ///< 0: no hop at all
        Conflict,       ///< the attack key, the loot key, an arrow, a macro key...
        SpaceIsAttack,  ///< Space, which the game attacks with, while the attack key is another
        DownChangesMap, ///< ↓, which on a gate changes the map: never pressed (InputManager::never_pressed)
    };
    struct KeyAdvice {
        Advice      kind = Advice::Clean;
        std::string what_vi;    ///< for Conflict: what it is already, "phím kỹ năng"
    };
    /// What there is to say about `jump` as the jump key, with `attack_key`
    /// the hunter's attack key and `others` every other key GoBot presses or
    /// watches (GuiApp::game_keys_in_use, without the jump key itself).
    static KeyAdvice advice(std::uint32_t jump, std::uint32_t attack_key,
                            const std::vector<NamedKey>& others);

    /// The walker's jump key, [Move] jump_key in hunt.ini: what it presses to
    /// get up to a saved place above it ("Go", "Patrol these"). No box sets
    /// it, so it is only said: a warning in Vietnamese when it is the attack
    /// key -- `attack_key`, the hunter's, or Space, the game's own attack --
    /// which pressed to jump attacks instead; empty when it is not. hunt.ini
    /// ships it as Up, the game's jump; a player's own file is never changed.
    static std::string walk_key_warning_vi(std::uint32_t walk_jump, std::uint32_t attack_key);

private:
    std::uint64_t clock_now() const;
    void          pause(std::uint32_t ms) const;
    /// The plate's y when seen just now.
    bool          look(int& y) const;

    ExecutionCoordinator& coord_;
    Eyes  eyes_;
    Hands hands_;
    Clock clock_;
};

} // namespace gobot
