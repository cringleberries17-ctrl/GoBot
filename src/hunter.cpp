#include "gobot/hunter.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace gobot {

// ---------------------------------------------------------------- config

bool HunterConfig::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) {
        GB_WARN("HunterConfig: cannot read %s -- using defaults", path.c_str());
        return false;
    }
    enabled            = ini.get_bool("Hunt", "enabled", false);
    character_name     = ini.get("Hunt", "character_name", "");
    attack_key         = static_cast<std::uint32_t>(ini.get_int("Hunt", "attack_key", 'Z'));
    left_key           = static_cast<std::uint32_t>(ini.get_int("Hunt", "left_key", 0x25));
    right_key          = static_cast<std::uint32_t>(ini.get_int("Hunt", "right_key", 0x27));
    jump_key           = static_cast<std::uint32_t>(ini.get_int("Hunt", "jump_key", 0x20));
    reach              = ini.get_int("Hunt", "reach", 150);
    height_tolerance   = ini.get_int("Hunt", "height_tolerance", 60);
    give_up_ms         = static_cast<std::uint32_t>(ini.get_int("Hunt", "give_up_ms", 4000));
    dead_below         = ini.get_float("Hunt", "dead_below", 0.05f);
    finish_wounded     = ini.get_bool("Hunt", "finish_wounded", true);
    step_ms            = static_cast<std::uint32_t>(ini.get_int("Hunt", "step_ms", 180));
    attack_interval_ms = static_cast<std::uint32_t>(ini.get_int("Hunt", "attack_interval_ms", 700));
    jump_to_reach      = ini.get_bool("Hunt", "jump_to_reach", true);
    require_focus      = ini.get_bool("Hunt", "require_focus", true);
    focus_on_start     = ini.get_bool("Hunt", "focus_on_start", true);
    toggle_key         = static_cast<std::uint32_t>(ini.get_int("Hunt", "toggle_key", 122));
    return true;
}

bool HunterConfig::save(const std::string& path) const {
    IniFile ini;
    ini.load(path);        // preserve the rest of the file
    ini.set_bool("Hunt", "enabled", enabled);
    ini.set    ("Hunt", "character_name", character_name);
    ini.set_int ("Hunt", "attack_key", static_cast<int>(attack_key));
    ini.set_int ("Hunt", "left_key", static_cast<int>(left_key));
    ini.set_int ("Hunt", "right_key", static_cast<int>(right_key));
    ini.set_int ("Hunt", "jump_key", static_cast<int>(jump_key));
    ini.set_int ("Hunt", "reach", reach);
    ini.set_int ("Hunt", "height_tolerance", height_tolerance);
    ini.set_int ("Hunt", "give_up_ms", static_cast<int>(give_up_ms));
    ini.set_float("Hunt", "dead_below", dead_below);
    ini.set_bool("Hunt", "finish_wounded", finish_wounded);
    ini.set_int ("Hunt", "step_ms", static_cast<int>(step_ms));
    ini.set_int ("Hunt", "attack_interval_ms", static_cast<int>(attack_interval_ms));
    ini.set_bool("Hunt", "jump_to_reach", jump_to_reach);
    ini.set_bool("Hunt", "require_focus", require_focus);
    ini.set_bool("Hunt", "focus_on_start", focus_on_start);
    ini.set_int ("Hunt", "toggle_key", static_cast<int>(toggle_key));
    return ini.save(path, "GoBot hunting");
}

// ---------------------------------------------------------------- hunter

const char* Hunter::to_string(State s) {
    switch (s) {
        case State::NoPlayer:    return "cannot see your character";
        case State::NoTargets:   return "no monsters in view";
        case State::Approaching: return "walking to the target";
        case State::Attacking:   return "attacking";
        case State::Unfocused:   return "waiting for the game to have focus";
        case State::Idle:        break;
    }
    return "idle";
}

Hunter::Hunter(InputManager& input, ExecutionCoordinator& coord, MobDetector& detector)
    : input_(input), coord_(coord), detector_(detector) {}

Hunter::~Hunter() { stop(); }

void Hunter::set_config(const HunterConfig& c) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = c;
}

HunterConfig Hunter::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

Hunter::Status Hunter::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void Hunter::start() {
    if (running_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    have_target_ = false;
    thread_ = std::thread(&Hunter::loop, this);
    GB_INFO("=== HUNT ON ===");
}

void Hunter::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    release_movement();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = State::Idle;
        status_.has_target = false;
    }
    GB_INFO("=== HUNT OFF ===");
}

void Hunter::release_movement() {
    // release_all() rather than key_up(), which is gated on the game having
    // focus. The moment that matters is exactly the moment focus is lost with
    // a direction held: the game still has the key down, the character keeps
    // running, and a gated release does nothing about it. release_all() sends
    // the key-up regardless, and is a no-op when nothing is held.
    input_.release_all();
}

void Hunter::loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        if (!running_.load()) break;

        HunterConfig cfg;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cfg = config_;
        }

        const auto set_state = [this](State s) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.state = s;
        };

        if (cfg.require_focus && !input_.window_focused()) {
            release_movement();
            set_state(State::Unfocused);
            continue;
        }

        Nameplate me;
        if (!detector_.player(me)) {
            release_movement();
            set_state(State::NoPlayer);
            have_target_ = false;
            continue;
        }

        const std::vector<Nameplate> mobs = detector_.mobs();
        if (mobs.empty()) {
            release_movement();
            set_state(State::NoTargets);
            have_target_ = false;
            continue;
        }

        const std::uint64_t now = now_ms();

        // Stay on the current target while it is still there. Re-picking the
        // nearest every frame makes the bot oscillate between two monsters a
        // similar distance away and never reach either.
        const Nameplate* target = nullptr;
        if (have_target_) {
            int best = 0;
            for (const Nameplate& n : mobs) {
                const int d = std::abs(n.x() - target_x_) + std::abs(n.y() - target_y_);
                if (!target || d < best) { target = &n; best = d; }
            }
            // Too far from where it was: it died and this is a different one.
            if (best > 120) target = nullptr;

            // Its bar is empty. Let go now rather than going on swinging at it
            // until the give-up timer fires several seconds later.
            if (target && target->hp <= cfg.dead_below) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.targets_finished;
                target = nullptr;
            }
            if (target && now - target_since_ms_ > cfg.give_up_ms) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.targets_dropped;
                target = nullptr;
            }
        }
        if (!target) {
            // Prefer something it can actually reach.
            //
            // The list is sorted nearest-first, but nearest includes things
            // standing on a ledge overhead. Chasing one of those means walking
            // back and forth underneath it until the give-up timer fires, over
            // and over, which is what the bot spent its first live run doing:
            // seventy-nine steps and one swing.
            Nameplate me_now;
            if (detector_.player(me_now)) {
                const Nameplate* wounded = nullptr;
                for (const Nameplate& n : mobs) {
                    if (std::abs(n.y() - me_now.y()) > cfg.height_tolerance) continue;
                    if (n.hp <= cfg.dead_below) continue;    // already finished
                    if (!target) target = &n;                // nearest reachable
                    if (cfg.finish_wounded &&
                        (!wounded || n.hp < wounded->hp)) wounded = &n;
                }
                // Finish something already hurt instead of starting afresh.
                // Only among those it could reach anyway, so this never sends
                // it chasing across the screen.
                if (wounded && wounded->hp < 1.0f) target = wounded;
            }

            // Nothing level, or no idea where we are: the nearest that still
            // has health, and let the timer decide.
            if (!target) {
                for (const Nameplate& n : mobs) {
                    if (n.hp > cfg.dead_below) { target = &n; break; }
                }
            }

            // Everything in view reads as finished. Standing still beats
            // swinging at a corpse, which holds the hunter in place while
            // something living walks past.
            if (!target) {
                release_movement();
                set_state(State::NoTargets);
                have_target_ = false;
                continue;
            }
            target_since_ms_ = now;
        }

        target_x_ = target->x();
        target_y_ = target->y();
        have_target_ = true;

        const int dx = target->x() - me.x();
        const int dy = target->y() - me.y();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.target = *target;
            status_.has_target = true;
            status_.dx = dx;
            status_.dy = dy;
        }

        const bool level = std::abs(dy) <= cfg.height_tolerance;
        const bool close = std::abs(dx) <= cfg.reach;

        if (close && level) {
            release_movement();

            // Face it. A tap turns the character without moving it far, and a
            // skill thrown the wrong way hits nothing at all.
            const std::uint32_t face = dx < 0 ? cfg.left_key : cfg.right_key;
            if (std::abs(dx) > 12) {
                if (auto token = coord_.acquire("hunt-face", Priority::Combat))
                    input_.key_press(face, 30);
            }

            if (now - last_attack_ms_ >= cfg.attack_interval_ms) {
                if (auto token = coord_.acquire("hunt-attack", Priority::Combat)) {
                    input_.key_press(cfg.attack_key, 60);
                    last_attack_ms_ = now;
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++status_.attacks;
                }
            }
            set_state(State::Attacking);
            continue;
        }

        // Walk towards it.
        set_state(State::Approaching);
        if (auto token = coord_.acquire("hunt-move", Priority::Movement)) {
            const std::uint32_t go = dx < 0 ? cfg.left_key : cfg.right_key;
            const std::uint32_t other = dx < 0 ? cfg.right_key : cfg.left_key;
            input_.key_up(other);
            input_.key_press(go, static_cast<int>(cfg.step_ms));

            // Up and out of reach: a hop is the cheapest thing that might help,
            // and the give-up timer catches the case where it does not.
            if (cfg.jump_to_reach && dy < -cfg.height_tolerance / 2)
                input_.key_press(cfg.jump_key, 50);

            std::lock_guard<std::mutex> lock(mutex_);
            ++status_.steps;
        }
    }

    release_movement();
}

} // namespace gobot
