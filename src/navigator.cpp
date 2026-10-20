#include "gobot/navigator.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace gobot {
namespace {

std::string hex64(std::uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

} // namespace

// ------------------------------------------------------------- waypoints

std::vector<SavedPlace> PlaceBook::for_map(std::uint64_t map) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SavedPlace> out;
    for (const SavedPlace& w : points_)
        if (w.map == map) out.push_back(w);
    return out;
}

void PlaceBook::add(const SavedPlace& w) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (SavedPlace& p : points_) {
        if (p.map == w.map && p.name == w.name) { p = w; return; }
    }
    points_.push_back(w);
}

void PlaceBook::remove(std::uint64_t map, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    points_.erase(std::remove_if(points_.begin(), points_.end(),
                                 [&](const SavedPlace& p) {
                                     return p.map == map && p.name == name;
                                 }),
                  points_.end());
}

void PlaceBook::clear_map(std::uint64_t map) {
    std::lock_guard<std::mutex> lock(mutex_);
    points_.erase(std::remove_if(points_.begin(), points_.end(),
                                 [&](const SavedPlace& p) { return p.map == map; }),
                  points_.end());
}

std::size_t PlaceBook::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return points_.size();
}

bool PlaceBook::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    points_.clear();
    for (int i = 0; i < 4096; ++i) {
        char sec[32];
        std::snprintf(sec, sizeof(sec), "Point.%d", i);
        if (!ini.has_section(sec)) continue;
        SavedPlace w;
        w.name = ini.get(sec, "name", "");
        w.map  = std::strtoull(ini.get(sec, "map", "0").c_str(), nullptr, 16);
        w.x    = ini.get_float(sec, "x", 0.0f);
        w.y    = ini.get_float(sec, "y", 0.0f);
        if (!w.name.empty() && w.map != 0) points_.push_back(std::move(w));
    }
    GB_INFO("PlaceBook: %zu point(s) from %s", points_.size(), path.c_str());
    return true;
}

bool PlaceBook::save(const std::string& path) const {
    IniFile ini;
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < points_.size(); ++i) {
        char sec[32];
        std::snprintf(sec, sizeof(sec), "Point.%zu", i);
        ini.set      (sec, "name", points_[i].name);
        ini.set      (sec, "map", hex64(points_[i].map));
        ini.set_float(sec, "x", static_cast<float>(points_[i].x));
        ini.set_float(sec, "y", static_cast<float>(points_[i].y));
    }
    return ini.save(path, "GoBot saved positions, by map");
}

// ------------------------------------------------------------ edge anchor

void EdgeAnchor::update(int camera_dx, int player_screen_x, bool walking) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (anchored_) return;

    if (!walking || last_player_x_ < 0) {
        last_player_x_ = player_screen_x;
        stuck_frames_ = 0;
        return;
    }

    const int moved = player_screen_x - last_player_x_;
    last_player_x_ = player_screen_x;

    // The character crossing the screen while the view stays put is the map
    // running out. Moving left is the left-hand edge, which is the one with a
    // known coordinate.
    if (camera_dx == 0 && moved < -2) {
        if (++stuck_frames_ >= kStuckFrames) {
            anchored_ = true;
            // The camera is as far left as it goes, so this camera position is
            // the origin. Whatever it currently reads is the offset to remove.
            offset_ = 0.0;
            GB_INFO("Position origin pinned to the map's left edge");
        }
    } else {
        stuck_frames_ = 0;
    }
}

void EdgeAnchor::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    anchored_ = false;
    offset_ = 0.0;
    last_player_x_ = -1;
    stuck_frames_ = 0;
}

bool EdgeAnchor::anchored() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return anchored_;
}

double EdgeAnchor::offset() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return offset_;
}

// ------------------------------------------------------------- navigator

bool NavigatorConfig::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) return false;
    left_key         = static_cast<std::uint32_t>(ini.get_int("Move", "left_key", 0x25));
    right_key        = static_cast<std::uint32_t>(ini.get_int("Move", "right_key", 0x27));
    // Up, the game's jump, when the file has no key: as the shipped file says.
    jump_key         = static_cast<std::uint32_t>(ini.get_int("Move", "jump_key", 0x26));
    arrive_tolerance = ini.get_int("Move", "arrive_tolerance", 40);
    step_ms          = static_cast<std::uint32_t>(ini.get_int("Move", "step_ms", 180));
    leg_timeout_ms   = static_cast<std::uint32_t>(ini.get_int("Move", "leg_timeout_ms", 20000));
    dwell_ms         = static_cast<std::uint32_t>(ini.get_int("Move", "dwell_ms", 800));
    require_focus    = ini.get_bool("Move", "require_focus", true);
    return true;
}

bool NavigatorConfig::save(const std::string& path) const {
    IniFile ini;
    ini.load(path);
    ini.set_int ("Move", "left_key", static_cast<int>(left_key));
    ini.set_int ("Move", "right_key", static_cast<int>(right_key));
    ini.set_int ("Move", "jump_key", static_cast<int>(jump_key));
    ini.set_int ("Move", "arrive_tolerance", arrive_tolerance);
    ini.set_int ("Move", "step_ms", static_cast<int>(step_ms));
    ini.set_int ("Move", "leg_timeout_ms", static_cast<int>(leg_timeout_ms));
    ini.set_int ("Move", "dwell_ms", static_cast<int>(dwell_ms));
    ini.set_bool("Move", "require_focus", require_focus);
    return ini.save(path, "GoBot movement");
}

const char* Navigator::to_string(State s) {
    switch (s) {
        case State::NoPosition: return "cannot tell where you are";
        case State::Walking:    return "walking";
        case State::Arrived:    return "arrived";
        case State::GaveUp:     return "could not get there";
        case State::Unfocused:  return "waiting for the game to have focus";
        case State::Idle:       break;
    }
    return "idle";
}

Navigator::Navigator(InputManager& input, ExecutionCoordinator& coord,
                     MobDetector& detector, CameraTracker& camera)
    : input_(input), coord_(coord), detector_(detector), camera_(camera) {}

Navigator::~Navigator() { stop(); }

void Navigator::set_config(const NavigatorConfig& c) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = c;
}

NavigatorConfig Navigator::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

Navigator::Status Navigator::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void Navigator::go_to(const SavedPlace& w) {
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        route_ = { w };
        looping_ = false;
        status_ = Status{};
        status_.legs = 1;
    }
    running_.store(true);
    thread_ = std::thread(&Navigator::loop, this);
    GB_INFO("Walking to '%s'", w.name.c_str());
}

void Navigator::patrol(const std::vector<SavedPlace>& route) {
    stop();
    if (route.size() < 2) {
        GB_WARN("A patrol needs at least two places");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        route_ = route;
        looping_ = true;
        status_ = Status{};
        status_.legs = route.size();
    }
    running_.store(true);
    thread_ = std::thread(&Navigator::loop, this);
    GB_INFO("Patrolling %zu places", route.size());
}

void Navigator::stop() {
    if (running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
    } else if (thread_.joinable()) {
        thread_.join();
    }
    release_movement();
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state != State::Arrived && status_.state != State::GaveUp)
        status_.state = State::Idle;
}

void Navigator::release_movement() {
    // Unconditionally, like the hunter: key_up is gated on the game having
    // focus, and the moment that matters is losing focus with a direction held.
    input_.release_all();
}

void Navigator::loop() {
    std::size_t leg = 0;
    int direction = 1;
    std::uint64_t leg_started = now_ms();

    const auto set_state = [this](State s) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = s;
    };

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        if (!running_.load()) break;

        NavigatorConfig cfg;
        std::vector<SavedPlace> route;
        bool looping = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cfg = config_;
            route = route_;
            looping = looping_;
        }
        if (route.empty()) break;
        if (leg >= route.size()) leg = route.size() - 1;

        if (cfg.require_focus && !input_.window_focused()) {
            release_movement();
            set_state(State::Unfocused);
            continue;
        }

        // Where we are. The character's place on screen plus where the view is
        // looking: neither alone is a position in the level.
        Nameplate me;
        const CameraTracker::Fix fx = camera_.fix();
        if (!detector_.player(me) || !fx.tracking) {
            release_movement();
            set_state(State::NoPosition);
            continue;
        }

        double wx = 0.0, wy = 0.0;
        camera_.to_world(me.x(), me.y(), wx, wy);

        const SavedPlace& target = route[leg];
        const double dx = target.x - wx;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            status_.target = target.name;
            status_.target_x = target.x;
            status_.distance = dx;
            status_.leg = leg;
        }

        if (std::abs(dx) <= cfg.arrive_tolerance) {
            release_movement();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.arrivals;
            }
            set_state(State::Arrived);

            if (!looping) break;

            // Turn round at the ends, so a two-point route is a patrol rather
            // than a one-way trip.
            if (leg == route.size() - 1) direction = -1;
            else if (leg == 0)           direction = 1;
            leg = static_cast<std::size_t>(static_cast<int>(leg) + direction);
            leg_started = now_ms();

            for (std::uint32_t slept = 0; slept < cfg.dwell_ms && running_.load(); slept += 50)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (now_ms() - leg_started > cfg.leg_timeout_ms) {
            release_movement();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++status_.give_ups;
            }
            set_state(State::GaveUp);
            GB_WARN("Could not reach '%s' -- %.0f px short", target.name.c_str(), dx);
            if (!looping) break;
            if (leg == route.size() - 1) direction = -1;
            else if (leg == 0)           direction = 1;
            leg = static_cast<std::size_t>(static_cast<int>(leg) + direction);
            leg_started = now_ms();
            continue;
        }

        set_state(State::Walking);
        if (auto token = coord_.acquire("navigate", Priority::Movement)) {
            const std::uint32_t go = dx < 0 ? cfg.left_key : cfg.right_key;
            const std::uint32_t other = dx < 0 ? cfg.right_key : cfg.left_key;
            input_.key_up(other);
            input_.key_press(go, static_cast<int>(cfg.step_ms));

            // Target well above us: hop. The floor decides whether that helps,
            // and the leg timeout decides when it has not.
            if (target.y - wy < -60.0) input_.key_press(cfg.jump_key, 50);
        }
    }

    release_movement();
}

} // namespace gobot
