#pragma once

#include "gobot/command.hpp"
#include "gobot/config.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/game_state.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/pathfinder.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gobot {

/// Turns Commands into key presses, on its own threads.
///
/// Four threads rather than one loop, because these run on genuinely different
/// clocks: the command queue drains as fast as input allows, healing must
/// react within a tick of an HP packet, buffs fire on multi-minute timers, and
/// pathfinding is a slow feedback loop against position updates. Folding them
/// into one loop makes the slowest one set the latency for all.
class CommandExecutor {
public:
    CommandExecutor(const BotConfig& config,
                    GameState& state,
                    InputManager& input,
                    ExecutionCoordinator& coord);
    ~CommandExecutor();

    CommandExecutor(const CommandExecutor&) = delete;
    CommandExecutor& operator=(const CommandExecutor&) = delete;

    void start();
    void stop();
    bool running() const { return running_.load(); }

    /// Config is copied on start; call to pick up edits while running.
    void update_config(const BotConfig& cfg);

    void set_pathfinder(PathFinder* pf) { pathfinder_ = pf; }

    // ---- queue ----
    void enqueue(const Command& cmd);
    void enqueue(const std::vector<Command>& cmds);
    /// Drop queued commands of this type — used when a target dies and its
    /// approach commands are now pointless.
    void drop(CommandType type);
    void clear_queue();
    std::size_t queue_size() const;

    /// Run a command inline on the caller's thread, bypassing the queue.
    bool execute_now(const Command& cmd);

    // ---- combat toggle ----
    void set_auto_attack(bool on) { auto_attack_.store(on); }
    bool auto_attack() const { return auto_attack_.load(); }

    // ---- pathing ----
    void set_path_target(const Vec2& target);
    void stop_pathfinding();
    bool pathfinding() const { return pathfinding_.load(); }
    std::vector<PathStep> current_path() const;

    // ---- stats ----
    struct Stats {
        std::uint64_t executed = 0;
        std::uint64_t dropped_expired = 0;
        std::uint64_t dropped_blocked = 0;
        std::uint64_t skills_cast = 0;
        std::uint64_t buffs_cast = 0;
        std::uint64_t heals = 0;
    };
    Stats stats() const;

    using StateCallback = std::function<void(const std::string&)>;
    void set_status_callback(StateCallback cb) { on_status_ = std::move(cb); }

private:
    void execution_loop();
    void heal_loop();
    void buff_loop();
    void pathfinding_loop();

    bool dispatch(const Command& cmd);
    bool step_path();
    void report(const std::string& msg);

    BotConfig            config_;
    mutable std::mutex   config_mutex_;

    GameState&            state_;
    InputManager&         input_;
    ExecutionCoordinator& coord_;
    PathFinder*           pathfinder_ = nullptr;

    mutable std::mutex      queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Command>     queue_;

    std::thread exec_thread_, heal_thread_, buff_thread_, path_thread_;

    std::atomic<bool> running_{false};
    std::atomic<bool> auto_attack_{false};
    std::atomic<bool> pathfinding_{false};

    mutable std::mutex    path_mutex_;
    std::vector<PathStep> path_;
    std::size_t           path_index_ = 0;
    Vec2                  path_target_;

    /// Last-fired timestamps, indexed by slot, for cooldown gating.
    std::vector<std::uint64_t> skill_last_ms_;
    std::vector<std::uint64_t> buff_last_ms_;
    std::uint64_t              heal_last_ms_ = 0;

    /// Position + timestamp used to notice we've stopped making progress.
    Vec2          stuck_ref_;
    std::uint64_t stuck_since_ms_ = 0;
    static constexpr float         kStuckDistance = 8.0f;
    static constexpr std::uint64_t kStuckTimeoutMs = 3000;

    mutable std::mutex stats_mutex_;
    Stats              stats_;

    StateCallback on_status_;
};

} // namespace gobot
