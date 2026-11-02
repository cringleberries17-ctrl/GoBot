#include "gobot/command_executor.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace gobot {

// -------------------------------------------------------------- Command ctors

const char* to_string(CommandType t) {
    switch (t) {
        case CommandType::None:          return "None";
        case CommandType::MoveLeft:      return "MoveLeft";
        case CommandType::MoveRight:     return "MoveRight";
        case CommandType::StopMove:      return "StopMove";
        case CommandType::Jump:          return "Jump";
        case CommandType::DoubleJump:    return "DoubleJump";
        case CommandType::JumpDown:      return "JumpDown";
        case CommandType::FaceLeft:      return "FaceLeft";
        case CommandType::FaceRight:     return "FaceRight";
        case CommandType::UseSkill:      return "UseSkill";
        case CommandType::NormalAttack:  return "NormalAttack";
        case CommandType::StartAttack:   return "StartAttack";
        case CommandType::StopAttack:    return "StopAttack";
        case CommandType::UseBuff:       return "UseBuff";
        case CommandType::UseItem:       return "UseItem";
        case CommandType::Loot:          return "Loot";
        case CommandType::TargetMonster: return "TargetMonster";
        case CommandType::PathfindTo:    return "PathfindTo";
        case CommandType::StopPathfind:  return "StopPathfind";
        case CommandType::Chat:          return "Chat";
        case CommandType::KeyDown:       return "KeyDown";
        case CommandType::KeyUp:         return "KeyUp";
        case CommandType::KeyPress:      return "KeyPress";
        case CommandType::Wait:          return "Wait";
    }
    return "?";
}

namespace {
Command make(CommandType t, Priority p) {
    Command c;
    c.type = t;
    c.priority = p;
    c.issued_ms = now_ms();
    return c;
}
} // namespace

Command Command::move_left(Priority p)  { auto c = make(CommandType::MoveLeft, p);  c.ttl_ms = 500; return c; }
Command Command::move_right(Priority p) { auto c = make(CommandType::MoveRight, p); c.ttl_ms = 500; return c; }
Command Command::stop_move(Priority p)  { return make(CommandType::StopMove, p); }
Command Command::jump(Priority p)       { auto c = make(CommandType::Jump, p); c.ttl_ms = 600; return c; }

Command Command::skill(std::uint32_t vk, int slot, Priority p) {
    auto c = make(CommandType::UseSkill, p);
    c.keycode = vk;
    c.slot = slot;
    c.ttl_ms = 400;      // a stale cast is a wasted global cooldown
    return c;
}

Command Command::buff(std::uint32_t vk, int slot) {
    auto c = make(CommandType::UseBuff, Priority::Buff);
    c.keycode = vk;
    c.slot = slot;
    return c;
}

Command Command::heal(std::uint32_t vk) {
    auto c = make(CommandType::UseItem, Priority::Heal);
    c.keycode = vk;
    c.ttl_ms = 1000;
    return c;
}

Command Command::face(bool left) {
    return make(left ? CommandType::FaceLeft : CommandType::FaceRight, Priority::Movement);
}

Command Command::pathfind(Vec2 pos) {
    auto c = make(CommandType::PathfindTo, Priority::Movement);
    c.position = pos;
    return c;
}

Command Command::wait(std::uint32_t ms) {
    auto c = make(CommandType::Wait, Priority::Idle);
    c.duration_ms = ms;
    return c;
}

Command Command::chat(const std::string& text) {
    auto c = make(CommandType::Chat, Priority::Idle);
    c.text = text;
    return c;
}

// ----------------------------------------------------------- CommandExecutor

CommandExecutor::CommandExecutor(const BotConfig& config,
                                 GameState& state,
                                 InputManager& input,
                                 ExecutionCoordinator& coord)
    : config_(config), state_(state), input_(input), coord_(coord) {
    skill_last_ms_.assign(BotConfig::kSkillSlots, 0);
}

CommandExecutor::~CommandExecutor() { stop(); }

void CommandExecutor::update_config(const BotConfig& cfg) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = cfg;
    input_.set_delay_range(cfg.command_delay_min, cfg.command_delay_max);
    input_.set_left_key(cfg.key_left);
    input_.set_right_key(cfg.key_right);
    input_.set_up_key(cfg.key_up);
    input_.set_down_key(cfg.key_down);
    input_.set_jump_key(cfg.key_jump);
    input_.set_attack_key(cfg.key_normal_attack);
    input_.set_loot_key(cfg.key_loot);
}

void CommandExecutor::start() {
    if (running_.exchange(true)) return;

    {
        BotConfig cfg;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            cfg = config_;
        }
        update_config(cfg);
    }

    exec_thread_ = std::thread(&CommandExecutor::execution_loop, this);
    path_thread_ = std::thread(&CommandExecutor::pathfinding_loop, this);

    GB_INFO("CommandExecutor started");
}

void CommandExecutor::stop() {
    // A stopped executor is a disarmed one: started again, it presses nothing
    // until the bot is.
    armed_.store(false);
    if (!running_.exchange(false)) return;

    queue_cv_.notify_all();

    if (exec_thread_.joinable()) exec_thread_.join();
    if (path_thread_.joinable()) path_thread_.join();

    clear_queue();

    // Leaving a direction key held would have the character run into a wall
    // forever after the bot has "stopped".
    input_.release_all();

    GB_INFO("CommandExecutor stopped");
}

void CommandExecutor::arm() {
    std::lock_guard<std::mutex> gate(press_mutex_);
    // Nothing queued before the bot was started is carried out once it is: a
    // PathfindTo or a face command has no TTL to age it out.
    clear_queue();
    armed_.store(true);
    GB_DEBUG("CommandExecutor armed");
}

void CommandExecutor::halt() {
    // Said before the gate is waited for as well as under it: the gate does
    // not serve its waiters in order, and the queue's or the route's thread,
    // getting it first, then finds the bot stopped and presses nothing.
    armed_.store(false);
    std::lock_guard<std::mutex> gate(press_mutex_);
    armed_.store(false);
    clear_queue();
    stop_pathfinding();
    // stop_pathfinding's stop_move lets go of the two direction keys only.
    // This lets go of every key held here, whatever window is in front.
    input_.release_all();
    GB_DEBUG("CommandExecutor halted");
}

void CommandExecutor::enqueue(const Command& cmd) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        // Movement is a level, not an event: a newer direction supersedes any
        // queued one, so collapse rather than accumulate.
        if (cmd.type == CommandType::MoveLeft || cmd.type == CommandType::MoveRight ||
            cmd.type == CommandType::StopMove) {
            queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const Command& c) {
                return c.type == CommandType::MoveLeft ||
                       c.type == CommandType::MoveRight ||
                       c.type == CommandType::StopMove;
            }), queue_.end());
        }
        queue_.push_back(cmd);
    }
    queue_cv_.notify_one();
}

void CommandExecutor::enqueue(const std::vector<Command>& cmds) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.insert(queue_.end(), cmds.begin(), cmds.end());
    }
    queue_cv_.notify_all();
}

void CommandExecutor::drop(CommandType type) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                [type](const Command& c) { return c.type == type; }),
                 queue_.end());
}

void CommandExecutor::clear_queue() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.clear();
}

std::size_t CommandExecutor::queue_size() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return queue_.size();
}

CommandExecutor::Stats CommandExecutor::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void CommandExecutor::report(const std::string& msg) {
    if (on_status_) on_status_(msg);
}

bool CommandExecutor::held_by_pause() {
    // Asked twice where keys are pressed: before the coordinator's token is
    // asked for, so as not to take it from anyone for nothing, and once it is
    // held. The second is what counts. A stop pauses this before it ends the
    // trash pass's hold on the coordinator, so whatever gets the token once
    // that hold has ended sees the pause here, however the two raced.
    if (!paused_.load()) return false;
    std::lock_guard<std::mutex> lock(stats_mutex_);
    ++stats_.held_paused;
    return true;
}

// ------------------------------------------------------------- main dispatch

void CommandExecutor::execution_loop() {
    while (running_.load()) {
        Command cmd;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(50),
                               [this] { return !queue_.empty() || !running_.load(); });
            if (!running_.load()) break;
            if (queue_.empty()) continue;

            // Highest priority first; ties break by insertion order.
            auto best = std::max_element(queue_.begin(), queue_.end(),
                [](const Command& a, const Command& b) { return a.priority < b.priority; });

            cmd = *best;
            queue_.erase(best);
        }

        if (cmd.expired(now_ms())) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.dropped_expired;
            continue;
        }

        if (!dispatch(cmd)) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.dropped_blocked;
        }
    }
}

bool CommandExecutor::execute_now(const Command& cmd) { return dispatch(cmd); }

bool CommandExecutor::dispatch(const Command& cmd) {
    // Not armed, the bot is not running: refused before the token, so nobody's
    // keys are taken for nothing, and not counted as held by a pause.
    if (!armed_.load()) return false;
    // Before the token is asked for: paused, nothing of this is to happen.
    if (held_by_pause()) return false;

    auto token = coord_.acquire("executor", cmd.priority);
    if (!token) return false;
    // And again once it is held: see held_by_pause.
    if (held_by_pause()) return false;

    if (!input_.window_focused()) {
        // Not an error: the user alt-tabbed. The brain keeps thinking, we
        // just don't type into whatever they switched to.
        return false;
    }

    // See press_mutex_. Halted while the token was asked for: refused.
    std::unique_lock<std::mutex> gate(press_mutex_);
    if (!armed_.load()) return false;

    bool ok = false;

    switch (cmd.type) {
        case CommandType::MoveLeft:     ok = input_.move_left();  break;
        case CommandType::MoveRight:    ok = input_.move_right(); break;
        case CommandType::StopMove:     ok = input_.stop_move();  break;
        case CommandType::Jump:         ok = input_.jump();       break;
        case CommandType::DoubleJump:   ok = input_.double_jump();break;
        case CommandType::JumpDown:     ok = input_.jump_down();  break;
        case CommandType::FaceLeft:     ok = input_.face_left();  break;
        case CommandType::FaceRight:    ok = input_.face_right(); break;
        case CommandType::NormalAttack: ok = input_.normal_attack(); break;
        case CommandType::Loot:         ok = input_.loot();       break;

        case CommandType::UseSkill: {
            // Cooldown is the brain's business, not ours. Gating it here too
            // meant two clocks enforcing the same rule: the brain marks a
            // skill spent when it decides, we marked it when we fired, and the
            // dispatch delay between them pushed our window later every cycle
            // until the effective cooldown was double what was configured.
            if (cmd.slot >= 0 && static_cast<std::size_t>(cmd.slot) < skill_last_ms_.size())
                skill_last_ms_[static_cast<std::size_t>(cmd.slot)] = now_ms();

            ok = input_.use_skill(cmd.keycode);
            if (ok) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.skills_cast;
            }
            break;
        }

        case CommandType::UseBuff:
            ok = input_.use_skill(cmd.keycode);
            if (ok) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.buffs_cast;
            }
            break;

        case CommandType::UseItem:
            ok = input_.key_press(cmd.keycode);
            if (ok) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.heals;
            }
            break;

        case CommandType::KeyDown:  ok = input_.key_down(cmd.keycode); break;
        case CommandType::KeyUp:    ok = input_.key_up(cmd.keycode);   break;
        case CommandType::KeyPress: ok = input_.key_press(cmd.keycode, cmd.duration_ms ? static_cast<int>(cmd.duration_ms) : -1); break;

        // Nothing: the bot's on and off are arm() and halt().
        case CommandType::StartAttack:
        case CommandType::StopAttack:  ok = true; break;

        case CommandType::PathfindTo:   set_path_target(cmd.position); ok = true; break;
        case CommandType::StopPathfind: stop_pathfinding();            ok = true; break;

        case CommandType::Chat: ok = input_.send_chat(cmd.text); break;

        case CommandType::Wait:
            // Presses nothing, so the gate is let go: a halt is not held up
            // for as long as this waits.
            gate.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(cmd.duration_ms));
            ok = true;
            break;

        case CommandType::TargetMonster:
            state_.set_selected_monster(cmd.target_id);
            ok = true;
            break;

        case CommandType::None:
            ok = true;
            break;
    }

    // The press is done: a halt may go ahead while this pauses.
    if (gate.owns_lock()) gate.unlock();

    if (ok) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.executed;
    }

    // The keys are let go of for the gap: the macro's potions and buffs -- the
    // packet bot's own -- press at the same priority as these commands, and
    // with the token held through it too, a queue fed every 80 ms never left
    // them a moment.
    { ExecutionCoordinator::Token done = std::move(token); }

    // Inter-action spacing. Uniform-rate input is the most obvious tell a bot
    // produces, so every action is followed by a randomised gap.
    std::this_thread::sleep_for(std::chrono::milliseconds(input_.random_delay()));
    return ok;
}

// -------------------------------------------------------------- pathfinding

void CommandExecutor::set_path_target(const Vec2& target) {
    if (!pathfinder_ || !pathfinder_->ready()) {
        GB_DEBUG("Pathfind requested but no map data available");
        return;
    }

    const Self me = state_.self();
    auto path = pathfinder_->find_path(me.pos, target);

    std::lock_guard<std::mutex> lock(path_mutex_);
    path_ = std::move(path);
    path_index_ = 0;
    path_target_ = target;
    stuck_ref_ = me.pos;
    stuck_since_ms_ = now_ms();
    pathfinding_.store(!path_.empty());

    if (path_.empty())
        GB_DEBUG("No path to (%.0f, %.0f)", target.x, target.y);
}

// Never takes press_mutex_: halt() calls this holding it.
void CommandExecutor::stop_pathfinding() {
    pathfinding_.store(false);
    {
        std::lock_guard<std::mutex> lock(path_mutex_);
        path_.clear();
        path_index_ = 0;
    }
    input_.stop_move();
}

std::vector<PathStep> CommandExecutor::current_path() const {
    std::lock_guard<std::mutex> lock(path_mutex_);
    return path_;
}

void CommandExecutor::pathfinding_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        if (!pathfinding_.load() || paused_.load()) continue;
        if (!step_path()) stop_pathfinding();
    }
}

bool CommandExecutor::step_path() {
    // The bot was stopped, and its route went with it: false has
    // pathfinding_loop drop it.
    if (!armed_.load()) return false;

    const Self me = state_.self();

    PathStep step;
    bool stuck = false;
    {
        std::lock_guard<std::mutex> lock(path_mutex_);
        if (path_index_ >= path_.size()) return false;    // arrived
        step = path_[path_index_];

        // Progress check: if we haven't moved meaningfully in a while, the
        // route is blocked by geometry the platform graph doesn't model. Give
        // up rather than grind against a wall. Under the lock, because
        // set_path_target restarts this clock from the executor's thread.
        if (distance(me.pos, stuck_ref_) > kStuckDistance) {
            stuck_ref_ = me.pos;
            stuck_since_ms_ = now_ms();
        } else if (now_ms() - stuck_since_ms_ > kStuckTimeoutMs) {
            stuck = true;
        }
    }
    if (stuck) {
        GB_WARN("Pathfinding stuck at (%.0f, %.0f); abandoning route", me.pos.x, me.pos.y);
        report("path stuck");
        return false;
    }

    const float dx = step.target.x - me.pos.x;
    const float dy = step.target.y - me.pos.y;

    constexpr float kArriveX = 20.0f;
    constexpr float kArriveY = 40.0f;

    if (std::abs(dx) <= kArriveX && std::abs(dy) <= kArriveY) {
        std::lock_guard<std::mutex> lock(path_mutex_);
        ++path_index_;
        if (path_index_ >= path_.size()) {
            input_.stop_move();
            return false;
        }
        return true;
    }

    auto token = coord_.acquire("pathfind", Priority::Movement);
    if (!token) return true;      // combat has the keyboard; retry next tick
    // Paused while the token was asked for: the path waits, as it does in
    // pathfinding_loop.
    if (held_by_pause()) return true;

    if (!input_.window_focused()) return true;

    // See press_mutex_. Halted while the token was asked for: the route is
    // dropped (pathfinding_loop does that on false) and nothing is pressed.
    // Declared after the token, so it is let go of first.
    std::lock_guard<std::mutex> gate(press_mutex_);
    if (!armed_.load()) return false;

    switch (step.kind) {
        case MoveKind::Jump:
            if (std::abs(dx) > kArriveX) (dx > 0 ? input_.move_right() : input_.move_left());
            input_.jump();
            break;

        case MoveKind::DoubleJump:
            if (std::abs(dx) > kArriveX) (dx > 0 ? input_.move_right() : input_.move_left());
            input_.double_jump();
            break;

        case MoveKind::DropThrough:
            input_.stop_move();
            input_.jump_down();
            break;

        case MoveKind::Fall:
        case MoveKind::Walk:
        case MoveKind::Ladder:
        default:
            if (dx > kArriveX)       input_.move_right();
            else if (dx < -kArriveX) input_.move_left();
            else                     input_.stop_move();
            break;
    }
    return true;
}

} // namespace gobot
