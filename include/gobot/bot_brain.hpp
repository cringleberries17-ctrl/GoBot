#pragma once

#include "gobot/command_executor.hpp"
#include "gobot/config.hpp"
#include "gobot/game_state.hpp"
#include "gobot/pathfinder.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace gobot {

/// What the bot is currently trying to do. Surfaced for the UI and the log.
enum class BotPhase {
    Stopped,
    Idle,           ///< nothing in range
    Seeking,        ///< moving toward a target
    Engaging,       ///< in range, attacking
    Repositioning,  ///< backing away / correcting facing
    Recovering,     ///< dead, or returning to origin
    Patrolling,
    Suspended,      ///< window lost focus or a dialog is up
};

const char* to_string(BotPhase p);

/// A point on the patrol route.
struct Waypoint {
    Vec2          pos;
    std::uint32_t dwell_ms = 0;     ///< linger here before moving on
};

/// The decision layer.
///
/// Runs one tick loop: read GameState, pick a target, decide what to press,
/// push Commands. It never touches the keyboard directly — everything goes
/// through CommandExecutor, so the coordinator can arbitrate and a single
/// stop() reliably halts the character.
class BotBrain {
public:
    BotBrain(const BotConfig& config,
             GameState& state,
             CommandExecutor& executor);
    ~BotBrain();

    BotBrain(const BotBrain&) = delete;
    BotBrain& operator=(const BotBrain&) = delete;

    void start();
    void stop();
    bool running() const { return running_.load(); }

    void update_config(const BotConfig& cfg);
    void set_pathfinder(PathFinder* pf) { pathfinder_ = pf; }

    BotPhase phase() const { return phase_.load(); }
    std::int32_t current_target() const { return target_id_.load(); }

    void set_waypoints(const std::vector<Waypoint>& wps);
    std::vector<Waypoint> waypoints() const;

    /// Anchor for return_to_original_location. Defaults to wherever the
    /// character stood when start() was first called.
    void set_origin(const Vec2& origin);
    /// Re-anchor to the character's current position. Called when the user
    /// toggles the bot on, so "home" is where they switched it on, not
    /// wherever it happened to be the first time it ever ran.
    void reanchor_origin();
    Vec2 origin() const;

    struct Stats {
        std::uint64_t ticks = 0;
        std::uint64_t targets_engaged = 0;
        std::uint64_t kills_observed = 0;
        std::uint64_t deaths = 0;
        std::uint64_t ticks_idle = 0;
    };
    Stats stats() const;

private:
    void tick_loop();
    void tick();

    // --- target selection ---
    std::optional<Monster> select_target(const Self& me,
                                         const std::vector<Monster>& monsters) const;
    bool eligible(const Self& me, const Monster& m) const;
    bool in_attack_range(const Self& me, const Monster& m) const;
    bool in_working_area(const Vec2& p) const;
    bool vertically_hittable(const Self& me, const Monster& m) const;

    // --- behaviours ---
    void engage(const Self& me, const Monster& target);
    void seek(const Self& me, const Monster& target);
    void idle(const Self& me);
    void patrol(const Self& me);
    void recover(const Self& me);

    /// Pick the next skill honouring rotation mode, cooldowns, AoE box and the
    /// minimum-targets rule.
    std::optional<int> choose_skill(const Self& me,
                                    const Monster& target,
                                    const std::vector<Monster>& all) const;
    int  count_targets_in_aoe(const Self& me, const SkillSlot& s,
                              const std::vector<Monster>& all) const;
    bool face_toward(const Self& me, const Vec2& p);

    void set_phase(BotPhase p);

    BotConfig          config_;
    mutable std::mutex config_mutex_;

    /// Snapshot of config_ taken once per tick. Only the brain thread touches
    /// it, so the decision helpers can read it without locking — and, more
    /// importantly, every decision within one tick sees a consistent config
    /// rather than one that can change halfway through.
    BotConfig cfg_;

    GameState&       state_;
    CommandExecutor& executor_;
    PathFinder*      pathfinder_ = nullptr;

    /// Guards start/stop so concurrent callers can't both join the thread.
    std::mutex        lifecycle_mutex_;
    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<BotPhase>     phase_{BotPhase::Stopped};
    std::atomic<std::int32_t> target_id_{0};

    mutable std::mutex    waypoint_mutex_;
    std::vector<Waypoint> waypoints_;
    std::size_t           waypoint_index_ = 0;
    std::uint64_t         waypoint_arrived_ms_ = 0;
    int                   patrol_direction_ = 1;   ///< sweep mode

    mutable std::mutex origin_mutex_;
    Vec2               origin_;
    bool               origin_set_ = false;

    /// Rotation cursor for sequential_skill.
    mutable std::size_t next_skill_ = 0;
    mutable std::vector<std::uint64_t> skill_ready_ms_;

    std::uint64_t last_target_switch_ms_ = 0;
    bool          was_dead_ = false;

    mutable std::mutex stats_mutex_;
    Stats              stats_;

    /// Don't thrash between two equidistant monsters.
    static constexpr std::uint64_t kTargetStickyMs = 400;
};

} // namespace gobot
