#pragma once

#include "gobot/execution_coordinator.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/mob_detector.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace gobot {

struct HunterConfig {
    /// Whether START on the simple screen goes hunting -- that screen's
    /// fighting switch. The hunter itself never reads it: F11 and the Attack
    /// switch start it regardless.
    bool enabled = false;

    /// Your character's name, exactly as it appears under its feet.
    ///
    /// This is how the bot sees its own character. Everything else on screen
    /// is found by its health bar, and the local character has none -- its
    /// health is in the HUD -- so without this the bot knows where the
    /// monsters are but not where it is standing, and cannot walk to one.
    ///
    /// Leave it blank for the old behaviour: the character is found only when
    /// it happens to be wearing a health bar with a mana bar under it.
    std::string character_name;

    std::uint32_t attack_key = 'Z';
    std::uint32_t left_key = 0x25;      ///< VK_LEFT
    std::uint32_t right_key = 0x27;     ///< VK_RIGHT
    std::uint32_t jump_key = 0x20;      ///< VK_SPACE

    /// Horizontal distance, in screen pixels, at which the attack is thrown.
    /// Wider than it sounds: most skills here sweep a good way in front.
    int reach = 150;

    /// How far above or below the target may be and still count as reachable.
    /// Past this the bot would attack through a floor and hit nothing.
    int height_tolerance = 60;

    /// Don't chase forever. A target that stays out of reach this long is
    /// dropped and the next one picked, which is what stops the bot walking
    /// into a wall under a platform it cannot climb.
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

    /// What it is doing, for the UI. Plain words -- this is the panel's
    /// explanation of why nothing is happening when nothing is happening.
    enum class State { Idle, NoPlayer, NoTargets, Approaching, Attacking, Unfocused };

    struct Status {
        State      state = State::Idle;
        Nameplate  target;
        bool       has_target = false;
        int        dx = 0, dy = 0;          ///< target relative to the player
        std::uint64_t attacks = 0;
        std::uint64_t steps = 0;
        std::uint64_t targets_dropped = 0;    ///< gave up: out of reach too long

        /// Let go because the bar was empty. Worth counting separately from
        /// targets_dropped: one of these is a kill and the other is a failure
        /// to reach something, and a panel showing only the sum cannot tell
        /// you which the bot has been doing all afternoon.
        std::uint64_t targets_finished = 0;
    };
    Status status() const;

    static const char* to_string(State s);

private:
    void loop();
    void release_movement();

    InputManager&         input_;
    ExecutionCoordinator& coord_;
    MobDetector&          detector_;

    std::atomic<bool> running_{false};
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
};

} // namespace gobot
