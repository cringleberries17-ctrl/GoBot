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

#include "gobot/execution_coordinator.hpp"
#include "gobot/hotkeys.hpp"
#include "gobot/mob_detector.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gobot {

class JumpCheck {
public:
    /// The character's plate: MobDetector::player, which says whether it was
    /// seen just now or only remembered.
    struct Eyes {
        std::function<bool(Nameplate&, bool*)> player;
    };
    /// `focus` brings the game to the front and says whether it is;
    /// `focused` only says whether it is, the moment before the press --
    /// `focus` is asked again when it is not set; `key` presses one key for
    /// `hold` ms and says whether it went out.
    struct Hands {
        std::function<bool()>                   focus;
        std::function<bool()>                   focused;
        std::function<bool(std::uint32_t, int)> key;
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
        Jumped,         ///< the plate rose kJumpedPx or more
        NoJump,         ///< it stayed within kStillPx over kStillReads fresh reads
        Unsure,         ///< something in between, or too few reads to say
        NoPlate,        ///< pressed, but the character was not seen to check by
        Held,           ///< a hold is on: nothing pressed
        Busy,           ///< another job kept the keys: nothing pressed
        NotInFront,     ///< the game was not in front: nothing pressed
        Stopped,        ///< told to stop before the press -- F12, GoBot closing: nothing pressed
        NoKey,          ///< no jump key: nothing pressed
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
    };

    /// One check: the plate read where it stands, the game in front, the
    /// coordinator's token, `jump_key` pressed exactly once for kHoldMs, and
    /// the plate watched for kWatchMs. Nothing at all is pressed while a hold
    /// is on. `stop`, when set before the press, means no press at all; after
    /// it, the looking is cut short and what has been seen is the result.
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

    // ---- the key, and what else it is ----

    enum class Advice {
        Clean,          ///< nothing else on it
        NotSet,         ///< 0: no hop at all
        Conflict,       ///< the attack key, the loot key, an arrow, a macro key...
        SpaceIsAttack,  ///< Space, which the game attacks with, while the attack key is another
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
