#pragma once

#include "gobot/game_state.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gobot {

/// Per-tile walkability attributes, as sniffed from the map data packet.
enum TileAttr : std::uint8_t {
    Tile_Empty    = 0,
    Tile_Ground   = 1 << 0,   ///< solid, stand on top
    Tile_Platform = 1 << 1,   ///< one-way: land from above, drop through
    Tile_Blocked  = 1 << 2,   ///< wall
    Tile_Ladder   = 1 << 3,
    Tile_Water    = 1 << 4,
    Tile_Warp     = 1 << 5,
};

/// Tile grid for one map.
struct MapData {
    std::string             name;
    std::int32_t            id = 0;
    int                     width = 0;      ///< tiles
    int                     height = 0;
    int                     tile_w = 32;    ///< game units per tile
    int                     tile_h = 32;
    std::vector<std::uint8_t> attrs;        ///< width*height, row-major

    bool valid() const { return width > 0 && height > 0 && attrs.size() == static_cast<std::size_t>(width) * height; }

    std::uint8_t at(int tx, int ty) const {
        if (tx < 0 || ty < 0 || tx >= width || ty >= height) return Tile_Blocked;
        return attrs[static_cast<std::size_t>(ty) * width + tx];
    }

    int  to_tile_x(float x) const { return static_cast<int>(x) / tile_w; }
    int  to_tile_y(float y) const { return static_cast<int>(y) / tile_h; }
    Vec2 to_world(int tx, int ty) const {
        return { static_cast<float>(tx * tile_w + tile_w / 2),
                 static_cast<float>(ty * tile_h + tile_h / 2) };
    }
};

/// A horizontal run of standable tiles — the node type of the platform graph.
struct Platform {
    int   id = 0;
    int   tile_y = 0;
    int   tile_x_start = 0;
    int   tile_x_end = 0;       ///< inclusive
    float world_y = 0;

    bool contains_x(int tx) const { return tx >= tile_x_start && tx <= tile_x_end; }
};

/// How to get from one platform to the next.
enum class MoveKind { Walk, Jump, DoubleJump, Fall, DropThrough, Ladder };

struct PathStep {
    MoveKind kind = MoveKind::Walk;
    Vec2     target;             ///< world position to reach
    int      platform_id = -1;
};

/// A* over a platform-adjacency graph.
///
/// Tile-level A* is the wrong model for a side-scroller: the character can't
/// occupy arbitrary tiles, it stands on platform surfaces and transitions
/// between them by walking off, jumping, or dropping through. So the graph
/// nodes are platforms and the edges are the transitions the movement model
/// actually supports — which also makes the path directly executable as a
/// short list of key presses.
class PathFinder {
public:
    /// Build the platform graph. Call once per map.
    bool init(const MapData& map);
    bool ready() const { return ready_; }

    void set_double_jump(bool enabled) { double_jump_ = enabled; }
    /// Max horizontal tiles crossable in one jump.
    void set_jump_range(int tiles) { jump_range_ = tiles; }
    /// Max upward tiles reachable by a jump.
    void set_jump_height(int tiles) { jump_height_ = tiles; }

    std::optional<int> platform_at(const Vec2& pos) const;
    std::optional<int> closest_platform(const Vec2& pos) const;
    const Platform*    platform(int id) const;
    std::size_t        platform_count() const { return platforms_.size(); }

    /// Ground height under `pos`, or nullopt over a pit.
    std::optional<float> height_at(const Vec2& pos) const;

    /// Path from `from` to `to`. Empty when unreachable or already there.
    std::vector<PathStep> find_path(const Vec2& from, const Vec2& to) const;

    /// Straight-line walk on the same platform — the common case, skips A*.
    bool same_platform(const Vec2& a, const Vec2& b) const;

    const MapData& map() const { return map_; }

private:
    struct Edge {
        int      to = 0;
        MoveKind kind = MoveKind::Walk;
        float    cost = 1.0f;
        Vec2     exit_point;    ///< where on the source platform to trigger it
    };

    void build_platforms();
    void build_edges();
    bool standable(int tx, int ty) const;
    float heuristic(int from_platform, int to_platform) const;

    MapData                          map_;
    std::vector<Platform>            platforms_;
    std::vector<std::vector<Edge>>   edges_;
    /// tile_y -> platform ids on that row, for fast lookup
    std::unordered_map<int, std::vector<int>> by_row_;

    bool ready_ = false;
    bool double_jump_ = false;
    int  jump_range_ = 4;
    int  jump_height_ = 3;
};

} // namespace gobot
