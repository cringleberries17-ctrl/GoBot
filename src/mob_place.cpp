#include "gobot/mob_place.hpp"
#include "gobot/hunter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace gobot {
namespace {

int round_px(double v) { return static_cast<int>(std::lround(v)); }

/// The top of the floor a placed point stands on, `above` the point's height
/// over it: its column's floor that many pixels under it, as along_floor kept
/// it; -1 none.
int top_under(const GameMapBook::Page& p, double x, double y) {
    const std::optional<MapFloor> on = p.floor_near(round_px(x), round_px(y));
    return on && !on->wall() ? on->y : -1;
}

} // namespace

std::optional<MapDot> along_floor(const GameMapBook::Page& p, double fx, double fy, double to_x, int max_px) {
    const int x0 = round_px(fx);
    const int y0 = round_px(fy);
    const std::optional<MapFloor> on = p.floor_near(x0, y0);
    if (!on || on->wall()) return std::nullopt;
    const double above = fy - on->y;
    const int tx = round_px(to_x);
    const int dir = tx < x0 ? -1 : 1;
    const MapTrail t = p.follow(x0, y0, dir, std::min(std::abs(tx - x0), max_px));
    if (t.pts.empty()) return std::nullopt;
    // Followed as far as asked, the last point is to_x's own or past it, over
    // a hole; else the floor ended, or the bound came, before it.
    const MapPoint& last = t.pts.back();
    const bool past = dir < 0 ? last.x < tx : last.x > tx;
    MapDot d;
    if (last.x == tx || past) {
        const MapPoint& at = past ? t.pts[t.pts.size() - 2] : last;     // the start is never past
        d.x = to_x;
        d.y = at.y + above;
        d.top = at.y;
        d.why = DotWhy::Followed;
    } else {
        d.x = last.x;
        d.y = last.y + above;
        d.top = last.y;
        d.why = DotWhy::Held;
    }
    return d;
}

MapDot mob_place(const GameMapBook::Page* page, const PacketWorld::Mob& m, std::uint64_t now, bool project) {
    MapDot d;
    d.x = m.x;
    d.y = m.y;
    if (m.dead || page == nullptr) return d;
    const bool walked = round_px(m.x) != round_px(m.frame_x);
    // A walk the world has run out: followed on from its frame, along its
    // floor, as far as its speed has carried it since -- the moment the world
    // holds it, the same x the world has, so the place never jumps.
    if (project && kPlaceProjectWalks && walked && !m.moving && m.facing != 0 && m.speed > 0.0f &&
        m.speed <= PacketWorld::kPlausibleWalkSpeed) {
        const std::uint64_t age = std::min<std::uint64_t>(now > m.event_ms ? now - m.event_ms : 0, kPlaceWalkOnMaxMs);
        const double px = static_cast<double>(m.speed) * PacketWorld::kStepsPerSecond * static_cast<double>(age) / 1000.0;
        const double to = m.frame_x + static_cast<double>(m.facing) * px;
        if (std::optional<MapDot> on = along_floor(*page, m.frame_x, m.frame_y, to, kPlaceProjectMaxPx)) {
            on->why = DotWhy::Projected;
            return *on;
        }
    }
    // A walk moved its x alone: its frame's floor, followed there.
    if (m.moving || walked) {
        if (const std::optional<MapDot> on = along_floor(*page, m.frame_x, m.frame_y, m.x)) return *on;
        return d;
    }
    // Standing where its frame put it: on a floor, or set down on its own.
    const int x = round_px(m.x);
    const int y = round_px(m.y);
    if (page->floor_near(x, y)) {
        d.top = top_under(*page, m.x, m.y);
        return d;
    }
    if (const std::optional<int> top = Hunter::monster_floor(*page, x, y)) {
        d.y = *top - kDotStandPx;
        d.top = *top;
        d.why = DotWhy::SetDown;
    }
    return d;
}

} // namespace gobot
