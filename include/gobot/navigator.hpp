#pragma once

#include "gobot/camera_tracker.hpp"
#include "gobot/execution_coordinator.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/mob_detector.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gobot {

/// A place in a level, worth coming back to.
struct SavedPlace {
    std::string   name;
    double        x = 0.0;
    double        y = 0.0;
    std::uint64_t map = 0;      ///< which map, by its minimap fingerprint
};

/// Saved places, keyed by map.
///
/// Positions are in the camera tracker's level coordinates, which have their
/// origin wherever tracking happened to start — so a saved position only means
/// anything once that origin is pinned to something in the level. The left-hand
/// edge of the map does that: walk into it and the camera stops while you keep
/// going, which is a place the game itself defines. Until then, waypoints are
/// good for the session and no further, and the UI says so rather than offering
/// positions that will be a hundred pixels out tomorrow.
class PlaceBook {
public:
    bool load(const std::string& path);
    bool save(const std::string& path) const;

    std::vector<SavedPlace> for_map(std::uint64_t map) const;
    void add(const SavedPlace& w);                       ///< replaces one of the same name
    void remove(std::uint64_t map, const std::string& name);
    void clear_map(std::uint64_t map);
    std::size_t size() const;

private:
    mutable std::mutex    mutex_;
    std::vector<SavedPlace> points_;
};

struct NavigatorConfig {
    std::uint32_t left_key = 0x25;
    std::uint32_t right_key = 0x27;
    std::uint32_t jump_key = 0x20;

    /// Close enough to have arrived, in level pixels.
    int arrive_tolerance = 40;

    /// One walking step.
    std::uint32_t step_ms = 180;

    /// Give up on a leg that is taking this long — a wall, a gap, a ledge.
    std::uint32_t leg_timeout_ms = 20000;

    /// Pause at each end of a patrol, so the bot does not look like it is
    /// bouncing off invisible walls.
    std::uint32_t dwell_ms = 800;

    bool require_focus = true;

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

/// Walks the character to places.
///
/// Deliberately thin: it steers along one axis, because that is what a
/// side-scroller gives you. Vertical movement is a jump when the target is
/// above, and otherwise the floor decides. It does not know about walls, so
/// every leg has a timeout and gives up rather than pressing into scenery
/// forever.
class Navigator {
public:
    Navigator(InputManager& input, ExecutionCoordinator& coord,
              MobDetector& detector, CameraTracker& camera);
    ~Navigator();

    Navigator(const Navigator&) = delete;
    Navigator& operator=(const Navigator&) = delete;

    void set_config(const NavigatorConfig& c);
    NavigatorConfig config() const;

    /// Walk to one place and stop.
    void go_to(const SavedPlace& w);

    /// Walk back and forth along a list of places, forever.
    void patrol(const std::vector<SavedPlace>& route);

    void stop();
    bool running() const { return running_.load(); }

    enum class State { Idle, NoPosition, Walking, Arrived, GaveUp, Unfocused };

    struct Status {
        State       state = State::Idle;
        std::string target;
        double      target_x = 0.0;
        double      distance = 0.0;
        std::size_t leg = 0;
        std::size_t legs = 0;
        std::uint64_t arrivals = 0;
        std::uint64_t give_ups = 0;
    };
    Status status() const;

    static const char* to_string(State s);

private:
    void loop();
    void release_movement();

    InputManager&         input_;
    ExecutionCoordinator& coord_;
    MobDetector&          detector_;
    CameraTracker&        camera_;

    std::atomic<bool> running_{false};
    std::thread       thread_;

    mutable std::mutex    mutex_;
    NavigatorConfig       config_;
    Status                status_;
    std::vector<SavedPlace> route_;
    bool                  looping_ = false;
};

/// Pins the coordinate origin to the left-hand edge of the map.
///
/// Walking into the edge is the one thing in a side-scroller that happens at a
/// place the level itself defines: the camera stops dead while the character
/// keeps moving across the screen. Catching that gives every later position a
/// fixed reference, which is the difference between a SavedPlace that survives
/// until tomorrow and one that is only good until the next restart.
class EdgeAnchor {
public:
    /// Feed the camera's movement and the character's position on screen.
    /// `walking` says whether something is actually pressing a direction —
    /// a stationary character also fails to scroll the camera.
    void update(int camera_dx, int player_screen_x, bool walking);

    void reset();

    bool   anchored() const;
    double offset() const;      ///< add to a camera position to make it absolute

private:
    mutable std::mutex mutex_;
    bool   anchored_ = false;
    double offset_ = 0.0;
    int    last_player_x_ = -1;
    int    stuck_frames_ = 0;

    /// Frames of "the character is moving but the view is not" before calling
    /// it an edge. Long enough not to fire on a single dropped match.
    static constexpr int kStuckFrames = 4;
};

} // namespace gobot
