#include "gobot/map_sketch.hpp"
#include "gobot/hunter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace gobot {
namespace {

/// Each run's box, from every column's point.
void box_runs(std::vector<MapRun>& runs) {
    for (MapRun& r : runs) {
        if (r.pts.empty()) continue;
        r.x0 = r.x1 = r.pts.front().x;
        r.y0 = r.y1 = r.pts.front().y;
        for (const MapPoint& p : r.pts) {
            r.x0 = std::min(r.x0, p.x);
            r.x1 = std::max(r.x1, p.x);
            r.y0 = std::min(r.y0, p.y);
            r.y1 = std::max(r.y1, p.y);
        }
    }
}

} // namespace

std::size_t MapSketch::points() const {
    std::size_t n = 0;
    for (const MapRun& r : runs) n += r.pts.size();
    return n;
}

std::vector<MapRun> link_floors(const MapFloors& f, int x0, int x1, int link_px,
                                const std::function<bool(int x, const MapFloor&)>& keep) {
    std::vector<MapRun> runs;
    x0 = std::max(0, x0);
    x1 = std::min(f.w_px() - 1, x1);
    if (x0 > x1) return runs;

    // The floors kept in the last column and this one, and the run each is in.
    std::vector<const MapFloor*> prev, cur;
    std::vector<std::size_t> prev_run, cur_run;
    struct Pair {
        int         d;
        std::size_t k;      ///< the last column's floor
        std::size_t j;      ///< this column's
    };
    std::vector<Pair> pairs;
    std::vector<char> used_k, used_j;
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    for (int x = x0; x <= x1; ++x) {
        cur.clear();
        for (const MapFloor& fl : f.at(x))
            if (!keep || keep(x, fl)) cur.push_back(&fl);
        cur_run.assign(cur.size(), kNone);
        // Every pair of the same attribute within link_px, nearest first --
        // then the last column's floor first, then this one's: the order the
        // sizes of the game's maps were measured in (floor_segments.py).
        pairs.clear();
        for (std::size_t j = 0; j < cur.size(); ++j)
            for (std::size_t k = 0; k < prev.size(); ++k) {
                const int d = std::abs(cur[j]->y - prev[k]->y);
                if (d <= link_px && cur[j]->attr == prev[k]->attr) pairs.push_back({ d, k, j });
            }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
            if (a.d != b.d) return a.d < b.d;
            if (a.k != b.k) return a.k < b.k;
            return a.j < b.j;
        });
        used_k.assign(prev.size(), 0);
        used_j.assign(cur.size(), 0);
        for (const Pair& p : pairs) {
            if (used_k[p.k] || used_j[p.j]) continue;
            used_k[p.k] = 1;
            used_j[p.j] = 1;
            cur_run[p.j] = prev_run[p.k];
            runs[cur_run[p.j]].pts.push_back({ x, cur[p.j]->y });
        }
        // One left unlinked begins a run of its own.
        for (std::size_t j = 0; j < cur.size(); ++j) {
            if (cur_run[j] != kNone) continue;
            MapRun r;
            r.attr = cur[j]->attr;
            r.layer = cur[j]->layer;
            r.pts.push_back({ x, cur[j]->y });
            cur_run[j] = runs.size();
            runs.push_back(std::move(r));
        }
        std::swap(prev, cur);
        std::swap(prev_run, cur_run);
    }
    box_runs(runs);
    return runs;
}

std::vector<MapPoint> simplify_run(const std::vector<MapPoint>& pts, double eps) {
    if (pts.size() < 3) return pts;
    std::vector<char> kept(pts.size(), 0);
    kept.front() = 1;
    kept.back() = 1;
    // A stack, not recursion: a run can be 8,960 columns long.
    std::vector<std::pair<std::size_t, std::size_t>> todo{ { 0, pts.size() - 1 } };
    while (!todo.empty()) {
        const auto [a, b] = todo.back();
        todo.pop_back();
        const double x1 = pts[a].x, y1 = pts[a].y, x2 = pts[b].x, y2 = pts[b].y;
        const double dx = x2 - x1, dy = y2 - y1;
        double n = std::sqrt(dx * dx + dy * dy);
        if (n == 0.0) n = 1.0;
        double best = -1.0;
        std::size_t at = 0;
        for (std::size_t i = a + 1; i < b; ++i) {
            const double d = std::abs(dy * pts[i].x - dx * pts[i].y + x2 * y1 - y2 * x1) / n;
            if (d > best) {
                best = d;
                at = i;
            }
        }
        if (best > eps) {
            kept[at] = 1;
            todo.push_back({ a, at });
            todo.push_back({ at, b });
        }
    }
    std::vector<MapPoint> out;
    for (std::size_t i = 0; i < pts.size(); ++i)
        if (kept[i]) out.push_back(pts[i]);
    return out;
}

MapSketch build_sketch(const std::string& stem, const MapFloors& f, int link_px, double eps) {
    MapSketch s;
    s.stem = stem;
    s.w_px = f.w_px();
    s.h_px = f.h_px();
    s.columns = f.floors.size();
    s.runs = link_floors(f, 0, f.w_px() - 1, link_px);
    for (MapRun& r : s.runs) r.pts = simplify_run(r.pts, eps);
    return s;
}

std::vector<MapRun> no_drop_marks(const GameMapBook::Page& p, std::span<const MapGate> gates, int drop_gate_px) {
    const MapFloors& fl = p.floors;
    const int w = fl.w_px();
    if (gates.empty() || w <= 0 || fl.floors.empty()) return {};
    // Which of the map's floors are marked, by their place in fl.floors.
    std::vector<char> marked(fl.floors.size(), 0);
    int lo = w, hi = -1;
    for (const MapGate& g : gates) {
        const int from = std::max(0, g.x - drop_gate_px);
        const int to = std::min(w - 1, g.x + drop_gate_px);
        for (int x = from; x <= to; ++x) {
            for (const MapFloor& f : fl.at(x)) {
                // As drop_rule judges a press there: the floor stood on, a
                // platform; where the fall lands; and the gate's band.
                const int y0 = f.y - 1;
                const std::optional<MapFloor> under = p.floor_near(x, y0);
                if (!under || !under->platform()) continue;
                const std::optional<MapFloor> land = p.floor_below(x, y0, Hunter::kDropFeetPx);
                if (!land) continue;
                if (g.y < y0 - Hunter::kDropGateSpanPx || g.y > land->y + Hunter::kDropGateSpanPx) continue;
                marked[static_cast<std::size_t>(&f - fl.floors.data())] = 1;
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
        }
    }
    if (hi < lo) return {};
    std::vector<MapRun> runs = link_floors(fl, lo, hi, kSketchLinkPx, [&](int, const MapFloor& f) {
        return marked[static_cast<std::size_t>(&f - fl.floors.data())] != 0;
    });
    for (MapRun& r : runs) r.pts = simplify_run(r.pts, kSketchEps);
    return runs;
}

// ------------------------------------------------------------------ camera

MapCamera::P MapCamera::to_screen(P w, P origin, P size) const {
    return { origin.x + size.x / 2.0 + (w.x - cx) * zoom, origin.y + size.y / 2.0 + (w.y - cy) * zoom };
}

MapCamera::P MapCamera::to_world(P s, P origin, P size) const {
    const double z = zoom > 0.0 ? zoom : kFollowZoom;
    return { cx + (s.x - origin.x - size.x / 2.0) / z, cy + (s.y - origin.y - size.y / 2.0) / z };
}

double MapCamera::fit_zoom(int w_px, int h_px, double cw, double ch) {
    if (w_px <= 0 || h_px <= 0 || cw <= 0.0 || ch <= 0.0) return kFollowZoom;
    return std::min(cw / w_px, ch / h_px);
}

double MapCamera::zmax_for(double zmin) { return std::max(kMaxZoom, zmin); }

double MapCamera::follow_zoom(double zmin, double zmax) { return std::clamp(kFollowZoom, zmin, std::max(zmin, zmax)); }

void MapCamera::zoom_about(P s, double factor, P origin, P size, double zmin, double zmax) {
    const P w = to_world(s, origin, size);
    zoom = std::clamp(zoom * factor, zmin, std::max(zmin, zmax));
    cx = w.x - (s.x - origin.x - size.x / 2.0) / zoom;
    cy = w.y - (s.y - origin.y - size.y / 2.0) / zoom;
}

void MapCamera::clamp_centre(int w_px, int h_px) {
    cx = std::clamp(cx, 0.0, static_cast<double>(std::max(0, w_px)));
    cy = std::clamp(cy, 0.0, static_cast<double>(std::max(0, h_px)));
}

void MapCamera::fit(int w_px, int h_px, P size) {
    zoom = fit_zoom(w_px, h_px, size.x, size.y);
    cx = w_px / 2.0;
    cy = h_px / 2.0;
}

} // namespace gobot
