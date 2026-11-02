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
/// Two threads rather than one loop, because these run on genuinely different
/// clocks: the command queue drains as fast as input allows, and pathfinding
/// is a slow feedback loop against position updates. Folding them into one
/// loop makes the slowest one set the latency for all.
///
/// No potions and no buffs here: they are the macro's (MacroEngine), fed the
/// packets' HP and MP while the bot runs (packet_vitals). A heal and a buff
/// thread here, reading bot_default.ini, drank beside the macro's potions for
/// the same HP drop.
///
/// The threads run from launch but press nothing until arm(), the bot's
/// start. halt() is its stop: synchronous, and the only way back to pressing
/// nothing (see armed_ and press_mutex_).
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

    /// The bot was started; BotBrain::start calls this. From here the threads
    /// may press keys: the queue and the route.
    ///
    /// Until then they press nothing and ask nobody for the keys. They run
    /// from launch, and with nothing to hold them back they pressed before
    /// START, after STOP, F12 or the disconnect watchdog, and in gobot_cli
    /// --status. Whatever is still queued is dropped.
    void arm();
    /// The bot was stopped; BotBrain::stop calls this. Synchronous: on return
    /// the queue is empty, the route is dropped, every held key is up, and
    /// nothing in here presses a key again until arm().
    ///
    /// A press already under way on one of the threads is waited for: under
    /// half a second for a double jump, the longest, or a KeyPress command's
    /// duration_ms. A pause after an action, or a Wait, is never waited for.
    /// Nothing is asked of the coordinator, because letting go must work
    /// whoever holds the keys. Not to be called alone while the brain runs,
    /// which would go on queueing into an executor that refuses it all: call
    /// BotBrain::stop().
    void halt();
    bool armed() const { return armed_.load(); }

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

    /// Run a command inline on the caller's thread, bypassing the queue. Like
    /// a queued one, it presses nothing unless the bot is started (arm()):
    /// false, and not counted, otherwise.
    bool execute_now(const Command& cmd);

    /// Press no keys at all until this is set off again. The threads keep
    /// running: a queued command is dropped, and a route waits.
    ///
    /// For a box in the game that would take keys as typing -- the trash
    /// bin's quantity dialog, which a stopped trash pass can leave open with
    /// nothing left to watch it. The coordinator's critical mode cannot cover
    /// that: it was the pass's hold, and it ends with the pass. These threads
    /// run from launch and press whenever the bot is started (arm() to
    /// halt()), which it may well be then.
    void set_paused(bool on) { paused_.store(on); }
    bool paused() const { return paused_.load(); }

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
        /// Commands that were due while paused.
        std::uint64_t held_paused = 0;
    };
    Stats stats() const;

    using StateCallback = std::function<void(const std::string&)>;
    void set_status_callback(StateCallback cb) { on_status_ = std::move(cb); }

private:
    void execution_loop();
    void pathfinding_loop();

    bool dispatch(const Command& cmd);
    bool step_path();
    void report(const std::string& msg);
    /// Paused, and so counted as held. Asked before the token is asked for,
    /// and again once it is held, which is the answer that counts.
    bool held_by_pause();

    BotConfig            config_;
    mutable std::mutex   config_mutex_;

    GameState&            state_;
    InputManager&         input_;
    ExecutionCoordinator& coord_;
    PathFinder*           pathfinder_ = nullptr;

    mutable std::mutex      queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Command>     queue_;

    std::thread exec_thread_, path_thread_;

    std::atomic<bool> running_{false};
    /// The bot is started: see arm() and halt(). Read without a lock to skip
    /// early, and again under press_mutex_ just before a key goes down, which
    /// is the answer that counts.
    std::atomic<bool> armed_{false};
    std::atomic<bool> pathfinding_{false};
    std::atomic<bool> paused_{false};

    /// Held across "armed? then press" on every thread here, and by halt()
    /// across "disarm, then let go". Checking the flag alone left a gap: a
    /// thread past its check when a stop came would press its key after the
    /// stop had let go of everything, holding a direction key after BOT
    /// DISABLED. Never held across a sleep that presses nothing, while asking
    /// the coordinator, or while calling out (report, on_status_).
    ///
    /// Taken before queue_mutex_, path_mutex_, stats_mutex_ and whatever
    /// InputManager and GameState take inside, never the other way round. So
    /// clear_queue() and stop_pathfinding() must not take it: halt() calls
    /// them holding it.
    std::mutex press_mutex_;

    mutable std::mutex    path_mutex_;
    std::vector<PathStep> path_;
    std::size_t           path_index_ = 0;
    Vec2                  path_target_;

    /// Last-fired timestamps, indexed by slot, for cooldown gating.
    std::vector<std::uint64_t> skill_last_ms_;

    /// Position + timestamp used to notice we've stopped making progress.
    /// Guarded by path_mutex_: set_path_target restarts them on the
    /// executor's thread, and step_path moves them on, on the path thread.
    Vec2          stuck_ref_;
    std::uint64_t stuck_since_ms_ = 0;
    static constexpr float         kStuckDistance = 8.0f;
    static constexpr std::uint64_t kStuckTimeoutMs = 3000;

    mutable std::mutex stats_mutex_;
    Stats              stats_;

    StateCallback on_status_;
};

} // namespace gobot
