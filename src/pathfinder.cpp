#include "gobot/pathfinder.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <utility>

namespace gobot {

bool PathFinder::init(const MapData& map) {
    ready_ = false;
    platforms_.clear();
    edges_.clear();
    by_row_.clear();

    if (!map.valid()) {
        GB_WARN("PathFinder::init given an invalid map (%dx%d, %zu attrs)",
                map.width, map.height, map.attrs.size());
        return false;
    }

    map_ = map;
    build_platforms();
    build_edges();

    ready_ = !platforms_.empty();
    GB_INFO("PathFinder: %zu platforms, map %s (%dx%d)",
            platforms_.size(), map_.name.c_str(), map_.width, map_.height);
    return ready_;
}

bool PathFinder::standable(int tx, int ty) const {
    const std::uint8_t a = map_.at(tx, ty);
    if (a & Tile_Blocked) return false;
    if (!(a & (Tile_Ground | Tile_Platform))) return false;

    // Needs headroom: a surface with a wall directly above is not standable.
    const std::uint8_t above = map_.at(tx, ty - 1);
    if (above & Tile_Blocked) return false;

    return true;
}

void PathFinder::build_platforms() {
    // Scan each row left to right, grouping consecutive standable tiles into
    // one platform. A side-scroller character moves along surfaces, so a
    // contiguous run is the natural unit of navigation.
    for (int ty = 0; ty < map_.height; ++ty) {
        int run_start = -1;

        for (int tx = 0; tx <= map_.width; ++tx) {
            const bool ok = (tx < map_.width) && standable(tx, ty);

            if (ok && run_start < 0) {
                run_start = tx;
            } else if (!ok && run_start >= 0) {
                Platform p;
                p.id           = static_cast<int>(platforms_.size());
                p.tile_y       = ty;
                p.tile_x_start = run_start;
                p.tile_x_end   = tx - 1;
                p.world_y      = static_cast<float>(ty * map_.tile_h);

                by_row_[ty].push_back(p.id);
                platforms_.push_back(p);
                run_start = -1;
            }
        }
    }
    edges_.resize(platforms_.size());
}

void PathFinder::build_edges() {
    const int max_fall = map_.height;    // falling is bounded only by the map

    for (std::size_t i = 0; i < platforms_.size(); ++i) {
        const Platform& from = platforms_[i];

        for (std::size_t j = 0; j < platforms_.size(); ++j) {
            if (i == j) continue;
            const Platform& to = platforms_[j];

            // Horizontal gap between the two runs (0 if they overlap in x).
            int gap = 0;
            if (to.tile_x_start > from.tile_x_end)      gap = to.tile_x_start - from.tile_x_end;
            else if (to.tile_x_end < from.tile_x_start) gap = from.tile_x_start - to.tile_x_end;

            const int dy = to.tile_y - from.tile_y;      // +ve = target is lower

            // Where on `from` we'd stand to attempt the transition.
            const int exit_tx = std::clamp((to.tile_x_start + to.tile_x_end) / 2,
                                           from.tile_x_start, from.tile_x_end);
            const Vec2 exit = map_.to_world(exit_tx, from.tile_y);

            Edge e;
            e.to = static_cast<int>(j);
            e.exit_point = exit;

            if (dy == 0 && gap <= 1) {
                // Effectively the same surface with a one-tile notch.
                e.kind = MoveKind::Walk;
                e.cost = static_cast<float>(gap + 1);
            } else if (dy == 0 && gap <= jump_range_) {
                // Two ledges at the same height with a gap between them. This
                // is the single most common layout in a side-scroller, and
                // omitting it leaves whole sections of a map unreachable.
                e.kind = MoveKind::Jump;
                e.cost = static_cast<float>(gap) + 4.0f;
            } else if (dy > 0 && gap <= jump_range_ && dy <= max_fall) {
                // Dropping down is cheap and always available.
                e.kind = (gap == 0 && (map_.at(exit_tx, from.tile_y) & Tile_Platform))
                             ? MoveKind::DropThrough
                             : MoveKind::Fall;
                e.cost = static_cast<float>(gap) + static_cast<float>(dy) * 0.5f + 2.0f;
            } else if (dy < 0) {
                const int rise = -dy;
                const int reach = double_jump_ ? jump_height_ * 2 : jump_height_;
                if (rise > reach || gap > jump_range_) continue;

                e.kind = (rise > jump_height_) ? MoveKind::DoubleJump : MoveKind::Jump;
                // Jumping is riskier than walking or falling: a missed jump
                // costs a retry, so weight it up.
                e.cost = static_cast<float>(gap) + static_cast<float>(rise) * 1.5f + 4.0f;
                if (e.kind == MoveKind::DoubleJump) e.cost += 3.0f;
            } else {
                continue;
            }

            edges_[i].push_back(e);
        }
    }

    std::size_t edge_count = 0;
    for (const auto& v : edges_) edge_count += v.size();
    GB_DEBUG("PathFinder: %zu edges", edge_count);
}

std::optional<int> PathFinder::platform_at(const Vec2& pos) const {
    if (!ready_) return std::nullopt;

    const int tx = map_.to_tile_x(pos.x);
    const int ty = map_.to_tile_y(pos.y);

    // The character's feet sit on the platform, so check the tile it stands on
    // and the one just below.
    for (int probe : { ty, ty + 1 }) {
        auto row = by_row_.find(probe);
        if (row == by_row_.end()) continue;
        for (int id : row->second) {
            if (platforms_[static_cast<std::size_t>(id)].contains_x(tx)) return id;
        }
    }
    return std::nullopt;
}

std::optional<int> PathFinder::closest_platform(const Vec2& pos) const {
    if (!ready_ || platforms_.empty()) return std::nullopt;

    int   best = -1;
    float best_d = std::numeric_limits<float>::max();

    for (const auto& p : platforms_) {
        const int tx = std::clamp(map_.to_tile_x(pos.x), p.tile_x_start, p.tile_x_end);
        const Vec2 surface = map_.to_world(tx, p.tile_y);
        const float d = distance(pos, surface);
        if (d < best_d) {
            best_d = d;
            best = p.id;
        }
    }
    return best < 0 ? std::nullopt : std::optional<int>(best);
}

const Platform* PathFinder::platform(int id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= platforms_.size()) return nullptr;
    return &platforms_[static_cast<std::size_t>(id)];
}

std::optional<float> PathFinder::height_at(const Vec2& pos) const {
    if (!ready_) return std::nullopt;

    const int tx = map_.to_tile_x(pos.x);
    const int ty = map_.to_tile_y(pos.y);

    for (int y = ty; y < map_.height; ++y) {
        if (standable(tx, y)) return static_cast<float>(y * map_.tile_h);
    }
    return std::nullopt;
}

bool PathFinder::same_platform(const Vec2& a, const Vec2& b) const {
    const auto pa = platform_at(a);
    const auto pb = platform_at(b);
    return pa && pb && *pa == *pb;
}

float PathFinder::heuristic(int from, int to) const {
    const Platform& a = platforms_[static_cast<std::size_t>(from)];
    const Platform& b = platforms_[static_cast<std::size_t>(to)];

    const float ax = static_cast<float>(a.tile_x_start + a.tile_x_end) * 0.5f;
    const float bx = static_cast<float>(b.tile_x_start + b.tile_x_end) * 0.5f;

    // Manhattan on the tile grid, with vertical weighted like a jump. Stays
    // admissible because no edge is cheaper than its own geometry.
    return std::abs(ax - bx) + std::abs(static_cast<float>(a.tile_y - b.tile_y));
}

std::vector<PathStep> PathFinder::find_path(const Vec2& from, const Vec2& to) const {
    std::vector<PathStep> out;
    if (!ready_) return out;

    const auto start_opt = platform_at(from) ? platform_at(from) : closest_platform(from);
    const auto goal_opt  = platform_at(to)   ? platform_at(to)   : closest_platform(to);
    if (!start_opt || !goal_opt) return out;

    const int start = *start_opt;
    const int goal  = *goal_opt;

    if (start == goal) {
        // Same surface: walk straight there, no search needed.
        out.push_back({ MoveKind::Walk, to, goal });
        return out;
    }

    const std::size_t n = platforms_.size();
    std::vector<float> g_score(n, std::numeric_limits<float>::max());
    std::vector<int>   came_from(n, -1);
    std::vector<MoveKind> came_kind(n, MoveKind::Walk);
    std::vector<Vec2>  came_exit(n);
    std::vector<bool>  closed(n, false);

    using Node = std::pair<float, int>;      // f_score, platform
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;

    g_score[static_cast<std::size_t>(start)] = 0.0f;
    open.emplace(heuristic(start, goal), start);

    while (!open.empty()) {
        const int current = open.top().second;
        open.pop();

        if (current == goal) break;
        if (closed[static_cast<std::size_t>(current)]) continue;
        closed[static_cast<std::size_t>(current)] = true;

        for (const Edge& e : edges_[static_cast<std::size_t>(current)]) {
            if (closed[static_cast<std::size_t>(e.to)]) continue;

            const float tentative = g_score[static_cast<std::size_t>(current)] + e.cost;
            if (tentative >= g_score[static_cast<std::size_t>(e.to)]) continue;

            g_score[static_cast<std::size_t>(e.to)]   = tentative;
            came_from[static_cast<std::size_t>(e.to)] = current;
            came_kind[static_cast<std::size_t>(e.to)] = e.kind;
            came_exit[static_cast<std::size_t>(e.to)] = e.exit_point;

            open.emplace(tentative + heuristic(e.to, goal), e.to);
        }
    }

    if (came_from[static_cast<std::size_t>(goal)] < 0 && start != goal) {
        GB_DEBUG("PathFinder: no route from platform %d to %d", start, goal);
        return out;
    }

    // Walk the parent chain back and reverse it.
    std::vector<PathStep> reversed;
    for (int cur = goal; cur != start && cur >= 0; cur = came_from[static_cast<std::size_t>(cur)]) {
        PathStep step;
        step.kind        = came_kind[static_cast<std::size_t>(cur)];
        step.platform_id = cur;
        step.target      = came_exit[static_cast<std::size_t>(cur)];
        reversed.push_back(step);

        if (came_from[static_cast<std::size_t>(cur)] < 0) break;
    }

    out.assign(reversed.rbegin(), reversed.rend());

    // Final approach to the requested point, not just its platform.
    if (!out.empty()) out.push_back({ MoveKind::Walk, to, goal });

    return out;
}

} // namespace gobot
