#include "gobot/bot_brain.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace gobot {

const char* to_string(BotPhase p) {
    switch (p) {
        case BotPhase::Stopped:       return "Stopped";
        case BotPhase::Idle:          return "Idle";
        case BotPhase::Seeking:       return "Seeking";
        case BotPhase::Engaging:      return "Engaging";
        case BotPhase::Repositioning: return "Repositioning";
        case BotPhase::Recovering:    return "Recovering";
        case BotPhase::Patrolling:    return "Patrolling";
        case BotPhase::Suspended:     return "Suspended";
    }
    return "?";
}

BotBrain::BotBrain(const BotConfig& config, GameState& state, CommandExecutor& executor)
    : config_(config), cfg_(config), state_(state), executor_(executor) {
    skill_ready_ms_.assign(BotConfig::kSkillSlots, 0);
}

BotBrain::~BotBrain() { stop(); }

void BotBrain::update_config(const BotConfig& cfg) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = cfg;
}

void BotBrain::start() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (running_.load()) return;

    // A previous run may have stopped itself from inside tick() (death with
    // continue_auto_after_death off). Its thread object is still joinable, so
    // reap it before starting another.
    if (thread_.joinable()) thread_.join();

    {
        std::lock_guard<std::mutex> lock(origin_mutex_);
        if (!origin_set_) {
            origin_ = state_.self().pos;
            origin_set_ = true;
        }
    }

    running_.store(true);
    set_phase(BotPhase::Idle);
    thread_ = std::thread(&BotBrain::tick_loop, this);
    GB_INFO("BotBrain started");
}

void BotBrain::stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);

    running_.store(false);

    // Join on `joinable`, not on the old value of running_: tick() can clear
    // running_ on its own, and keying the join off that flag would leave a
    // live thread attached to thread_ — whose destructor then calls
    // std::terminate.
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            // Self-stop from inside tick(). The loop will exit on its own;
            // joining here would deadlock.
            thread_.detach();
        } else {
            thread_.join();
        }
    }

    set_phase(BotPhase::Stopped);
    executor_.set_auto_attack(false);
    executor_.enqueue(Command::stop_move());
    GB_INFO("BotBrain stopped");
}

void BotBrain::set_phase(BotPhase p) {
    const BotPhase old = phase_.exchange(p);
    if (old != p) GB_DEBUG("Phase %s -> %s", to_string(old), to_string(p));
}

void BotBrain::set_waypoints(const std::vector<Waypoint>& wps) {
    std::lock_guard<std::mutex> lock(waypoint_mutex_);
    waypoints_ = wps;
    waypoint_index_ = 0;
}

std::vector<Waypoint> BotBrain::waypoints() const {
    std::lock_guard<std::mutex> lock(waypoint_mutex_);
    return waypoints_;
}

void BotBrain::set_origin(const Vec2& origin) {
    std::lock_guard<std::mutex> lock(origin_mutex_);
    origin_ = origin;
    origin_set_ = true;
}

void BotBrain::reanchor_origin() {
    const Vec2 here = state_.self().pos;
    std::lock_guard<std::mutex> lock(origin_mutex_);
    origin_ = here;
    origin_set_ = true;
    GB_DEBUG("Origin re-anchored to (%.0f, %.0f)", here.x, here.y);
}

Vec2 BotBrain::origin() const {
    std::lock_guard<std::mutex> lock(origin_mutex_);
    return origin_;
}

BotBrain::Stats BotBrain::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

// ------------------------------------------------------------------- loop

void BotBrain::tick_loop() {
    while (running_.load()) {
        // cfg_ is refreshed by tick() and only ever read on this thread.
        const int interval = std::max(20, cfg_.decision_interval_ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        if (!running_.load()) break;

        tick();

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.ticks;
        }
    }
}

void BotBrain::tick() {
    // Refresh the per-tick snapshot once, here. Every decision below — and
    // every helper it calls — then reads one consistent config, instead of
    // racing update_config() partway through a tick.
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        cfg_ = config_;
    }

    const Self me = state_.self();

    // Death handling first: nothing else is meaningful while dead.
    if (me.dead) {
        if (!was_dead_) {
            was_dead_ = true;
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.deaths;
            GB_WARN("Character died");
        }
        set_phase(BotPhase::Recovering);
        executor_.set_auto_attack(false);
        executor_.enqueue(Command::stop_move());
        recover(me);
        return;
    }

    if (was_dead_) {
        was_dead_ = false;
        GB_INFO("Character back up");
        if (!cfg_.continue_auto_after_death) {
            GB_INFO("continue_auto_after_death is off -- stopping");
            running_.store(false);
            return;
        }
    }

    // Stale entities would otherwise have us attacking ghosts.
    state_.sweep_stale();

    const auto monsters = state_.monsters();
    const auto target   = select_target(me, monsters);

    if (!target) {
        target_id_.store(0);
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.ticks_idle;
        }
        idle(me);
        return;
    }

    if (target_id_.load() != target->id) {
        target_id_.store(target->id);
        last_target_switch_ms_ = now_ms();
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.targets_engaged;
    }

    if (in_attack_range(me, *target)) {
        engage(me, *target);
    } else {
        seek(me, *target);
    }
}

// -------------------------------------------------------- target selection

bool BotBrain::in_working_area(const Vec2& p) const {
    // Guard against an unconfigured (all-zero) box, which would reject the
    // whole map and leave the bot permanently idle.
    if (cfg_.working_max_x <= cfg_.working_min_x) return true;
    if (cfg_.working_max_y <= cfg_.working_min_y) return true;

    return p.x >= cfg_.working_min_x && p.x <= cfg_.working_max_x &&
           p.y >= cfg_.working_min_y && p.y <= cfg_.working_max_y;
}

bool BotBrain::vertically_hittable(const Self& me, const Monster& m) const {
    // Side-scroller reach is asymmetric: you hit further below you than above.
    const float dy = m.pos.y - me.pos.y;      // +ve = monster is lower
    if (dy >= 0) return dy <= cfg_.lowest_hit_y + cfg_.lower_threshold;
    return -dy <= cfg_.highest_hit_y + cfg_.higher_threshold;
}

bool BotBrain::eligible(const Self& me, const Monster& m) const {
    if (m.dead) return false;
    if (m.hp == 0 && m.max_hp > 0) return false;
    if (cfg_.ignore_fly_monster && m.flying) return false;
    if (!in_working_area(m.pos)) return false;

    const float dist = distance(me.pos, m.pos);
    if (dist > cfg_.attack_range) return false;
    if (cfg_.min_range > 0 && dist < cfg_.min_range) return false;

    return true;
}

bool BotBrain::in_attack_range(const Self& me, const Monster& m) const {
    const float dx = std::abs(m.pos.x - me.pos.x);
    return dx <= cfg_.attack_range && vertically_hittable(me, m);
}

std::optional<Monster> BotBrain::select_target(const Self& me,
                                               const std::vector<Monster>& monsters) const {
    std::vector<const Monster*> candidates;
    candidates.reserve(monsters.size());
    for (const auto& m : monsters) {
        if (eligible(me, m)) candidates.push_back(&m);
    }
    if (candidates.empty()) return std::nullopt;

    // Sticky targeting: without it, two monsters at near-equal distance make
    // the bot alternate every tick and never land a hit on either.
    const std::int32_t current = target_id_.load();
    if (current != 0 && now_ms() - last_target_switch_ms_ < kTargetStickyMs) {
        for (const auto* m : candidates) {
            if (m->id == current) return *m;
        }
    }

    if (cfg_.priority_boss) {
        std::vector<const Monster*> bosses;
        for (const auto* m : candidates) {
            if (m->boss) bosses.push_back(m);
        }
        if (!bosses.empty()) candidates.swap(bosses);
    }

    auto by_distance = [&me](const Monster* a, const Monster* b) {
        return distance(me.pos, a->pos) < distance(me.pos, b->pos);
    };

    const Monster* best = nullptr;

    switch (cfg_.target_mode) {
        case TargetMode::LowestHp:
            best = *std::min_element(candidates.begin(), candidates.end(),
                [](const Monster* a, const Monster* b) { return a->hp_pct() < b->hp_pct(); });
            break;

        case TargetMode::HighestHp:
            best = *std::max_element(candidates.begin(), candidates.end(),
                [](const Monster* a, const Monster* b) { return a->hp_pct() < b->hp_pct(); });
            break;

        case TargetMode::Weakest:
            // Absolute HP, not percent: finishing a nearly-dead elite beats
            // starting on a full-health trash mob.
            best = *std::min_element(candidates.begin(), candidates.end(),
                [](const Monster* a, const Monster* b) {
                    const int ha = a->hp < 0 ? 999999 : a->hp;
                    const int hb = b->hp < 0 ? 999999 : b->hp;
                    return ha < hb;
                });
            break;

        case TargetMode::Nearest:
        default:
            best = *std::min_element(candidates.begin(), candidates.end(), by_distance);
            break;
    }

    return best ? std::optional<Monster>(*best) : std::nullopt;
}

// --------------------------------------------------------------- behaviours

bool BotBrain::face_toward(const Self& me, const Vec2& p) {
    if (!cfg_.face_target) return true;

    const float dx = p.x - me.pos.x;
    if (std::abs(dx) <= static_cast<float>(cfg_.facing_tolerance)) return true;

    const bool want_left = dx < 0;
    const bool facing_left = me.facing == Facing::Left;
    if (want_left == facing_left) return true;

    executor_.enqueue(Command::face(want_left));
    return false;
}

int BotBrain::count_targets_in_aoe(const Self& me, const SkillSlot& s,
                                   const std::vector<Monster>& all) const {
    int n = 0;
    for (const auto& m : all) {
        if (m.dead) continue;
        if (cfg_.ignore_fly_monster && m.flying) continue;

        // AoE box is relative to the player and mirrors with facing.
        const float rel_x = (me.facing == Facing::Right) ? (m.pos.x - me.pos.x)
                                                         : (me.pos.x - m.pos.x);
        const float rel_y = m.pos.y - me.pos.y;

        if (rel_x < s.aoe_min_x || rel_x > s.aoe_max_x) continue;
        if (s.aoe_max_y > s.aoe_min_y && (rel_y < s.aoe_min_y || rel_y > s.aoe_max_y)) continue;
        ++n;
    }
    return n;
}

std::optional<int> BotBrain::choose_skill(const Self& me,
                                          const Monster& target,
                                          const std::vector<Monster>& all) const {
    const std::uint64_t now = now_ms();

    auto usable = [&](std::size_t i) -> bool {
        const SkillSlot& s = cfg_.skills[i];
        if (!s.enabled || s.keycode == 0) return false;
        if (now < skill_ready_ms_[i]) return false;
        if (s.targets > 1 && count_targets_in_aoe(me, s, all) < s.targets) return false;

        // Single-target skills still respect their own reach.
        const float rel_x = std::abs(target.pos.x - me.pos.x);
        if (s.aoe_max_x > 0 && rel_x > s.aoe_max_x) return false;
        return true;
    };

    if (cfg_.sequential_skill) {
        // Rotate through the enabled slots so the whole kit gets used rather
        // than the first ready skill every time.
        for (std::size_t n = 0; n < BotConfig::kSkillSlots; ++n) {
            const std::size_t i = (next_skill_ + n) % BotConfig::kSkillSlots;
            if (!usable(i)) continue;
            next_skill_ = (i + 1) % BotConfig::kSkillSlots;
            skill_ready_ms_[i] = now + cfg_.skills[i].cooldown_ms;
            return static_cast<int>(i);
        }
        return std::nullopt;
    }

    // Priority mode: lowest slot index wins.
    for (std::size_t i = 0; i < BotConfig::kSkillSlots; ++i) {
        if (!usable(i)) continue;
        skill_ready_ms_[i] = now + cfg_.skills[i].cooldown_ms;
        return static_cast<int>(i);
    }
    return std::nullopt;
}

void BotBrain::engage(const Self& me, const Monster& target) {
    set_phase(BotPhase::Engaging);
    executor_.stop_pathfinding();

    // Stop closing the moment we're in range; drifting past the target is the
    // most common way a melee bot ends up swinging at nothing.
    executor_.enqueue(Command::stop_move());

    if (!face_toward(me, target.pos)) return;      // turn first, hit next tick

    if (cfg_.backing_away && cfg_.min_range > 0) {
        const float dx = std::abs(target.pos.x - me.pos.x);
        if (dx < cfg_.min_range) {
            set_phase(BotPhase::Repositioning);
            executor_.enqueue(target.pos.x > me.pos.x ? Command::move_left()
                                                      : Command::move_right());
            return;
        }
    }

    if (!cfg_.auto_attack) return;
    if (me.jumping && !cfg_.allow_attack_while_jumping) return;

    const auto monsters = state_.monsters();
    if (const auto slot = choose_skill(me, target, monsters)) {
        executor_.enqueue(Command::skill(cfg_.skills[*slot].keycode, *slot));
    } else {
        executor_.enqueue([] {
            Command c;
            c.type = CommandType::NormalAttack;
            c.priority = Priority::Combat;
            c.issued_ms = now_ms();
            c.ttl_ms = 300;
            return c;
        }());
    }
}

void BotBrain::seek(const Self& me, const Monster& target) {
    if (cfg_.lazy_mode) {
        // Lazy mode waits for monsters to come to us — useful on spawn camps
        // and much less conspicuous than sprinting across the map.
        set_phase(BotPhase::Idle);
        executor_.enqueue(Command::stop_move());
        return;
    }

    set_phase(BotPhase::Seeking);

    const float dx = target.pos.x - me.pos.x;
    const float dy = target.pos.y - me.pos.y;

    // A target well above us needs pathing, not walking into the platform edge.
    if (cfg_.enable_tile_pathfinding && pathfinder_ && pathfinder_->ready() &&
        std::abs(dy) > cfg_.highest_hit_y) {
        executor_.enqueue(Command::pathfind(target.pos));
        return;
    }

    face_toward(me, target.pos);
    executor_.enqueue(dx > 0 ? Command::move_right() : Command::move_left());

    // Small upward hop for a ledge just above us.
    if (dy < -static_cast<float>(cfg_.facing_tolerance) && std::abs(dy) < cfg_.highest_hit_y) {
        executor_.enqueue(cfg_.double_jump ? [] {
            Command c;
            c.type = CommandType::DoubleJump;
            c.priority = Priority::Movement;
            c.issued_ms = now_ms();
            c.ttl_ms = 600;
            return c;
        }() : Command::jump());
    }
}

void BotBrain::idle(const Self& me) {
    executor_.enqueue(Command::stop_move());

    std::size_t wp_count;
    {
        std::lock_guard<std::mutex> lock(waypoint_mutex_);
        wp_count = waypoints_.size();
    }

    if (cfg_.patrol_use_waypoints && wp_count > 0) {
        patrol(me);
        return;
    }

    if (cfg_.return_to_original_location) {
        const Vec2 home = origin();
        if (std::abs(me.pos.x - home.x) > cfg_.return_tolerance_x ||
            std::abs(me.pos.y - home.y) > cfg_.return_tolerance_y) {
            set_phase(BotPhase::Recovering);
            if (cfg_.enable_tile_pathfinding && pathfinder_ && pathfinder_->ready()) {
                executor_.enqueue(Command::pathfind(home));
            } else {
                executor_.enqueue(home.x > me.pos.x ? Command::move_right() : Command::move_left());
            }
            return;
        }
    }

    set_phase(BotPhase::Idle);
}

void BotBrain::patrol(const Self& me) {
    set_phase(BotPhase::Patrolling);

    std::lock_guard<std::mutex> lock(waypoint_mutex_);
    if (waypoints_.empty()) return;
    if (waypoint_index_ >= waypoints_.size()) waypoint_index_ = 0;

    const Waypoint& wp = waypoints_[waypoint_index_];

    const bool arrived = std::abs(me.pos.x - wp.pos.x) <= cfg_.return_tolerance_x &&
                         std::abs(me.pos.y - wp.pos.y) <= cfg_.return_tolerance_y;

    if (!arrived) {
        waypoint_arrived_ms_ = 0;
        if (cfg_.enable_tile_pathfinding && pathfinder_ && pathfinder_->ready()) {
            executor_.enqueue(Command::pathfind(wp.pos));
        } else {
            executor_.enqueue(wp.pos.x > me.pos.x ? Command::move_right() : Command::move_left());
        }
        return;
    }

    executor_.enqueue(Command::stop_move());

    if (waypoint_arrived_ms_ == 0) waypoint_arrived_ms_ = now_ms();
    if (now_ms() - waypoint_arrived_ms_ < wp.dwell_ms) return;

    waypoint_arrived_ms_ = 0;

    if (cfg_.patrol_sweep_mode) {
        // Ping-pong along the route instead of teleporting back to the start.
        if (patrol_direction_ > 0 && waypoint_index_ + 1 >= waypoints_.size()) {
            patrol_direction_ = -1;
        } else if (patrol_direction_ < 0 && waypoint_index_ == 0) {
            patrol_direction_ = 1;
        }
        waypoint_index_ = static_cast<std::size_t>(
            static_cast<std::ptrdiff_t>(waypoint_index_) + patrol_direction_);
    } else {
        ++waypoint_index_;
        if (waypoint_index_ >= waypoints_.size()) {
            if (!cfg_.patrol_loop_cycle) {
                waypoint_index_ = waypoints_.size() - 1;
                return;
            }
            waypoint_index_ = 0;
        }
    }
}

void BotBrain::recover(const Self& me) {
    (void)me;   // position is irrelevant until there is a revive flow to drive
    if (!cfg_.player_auto_revive) return;

    // Revive flows are dialog-driven and vary by client build. Without a
    // mapped dialog packet there is nothing reliable to press, so this stays
    // a hook rather than a guess that mashes keys on a death screen.
    static std::uint64_t last_attempt = 0;
    if (now_ms() - last_attempt < 5000) return;
    last_attempt = now_ms();

    GB_INFO("Auto-revive is enabled but no revive interaction is mapped; "
            "add a self_revive opcode to protocol.ini to drive it.");
}

} // namespace gobot
