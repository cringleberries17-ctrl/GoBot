#pragma once

// The "Bản đồ" tab's drawing of a map, worked out once per map on its worker
// (2026.11.04c): the floors of the game's .map as polylines, the floor pieces
// a drop is refused from for a gate, and the camera the canvas is seen
// through. Pure -- no ImGui, no file, no clock -- so all of it is tested.
//
// A .map has a floor per pixel column (GameMapBook::parse_map): 11,242 on the
// median of the game's 781 maps, 80,480 on the largest. Linked column to
// column into runs and simplified at 1 px, they are 34 runs and 319 points on
// the median map, 311 runs and 2,901 points at most -- a few thousand points a
// frame at worst.

#include "gobot/map_book.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace gobot {

/// One floor followed across the columns it runs through: world pixels, the
/// floors' tops, x growing by one a column before it is simplified. Its box
/// is that of every column's point, simplified or not.
struct MapRun {
    std::uint8_t          attr = 0;     ///< its first column's (MapFloor::attr): runs never mix two
    std::uint8_t          layer = 0;    ///< its first column's
    std::vector<MapPoint> pts;
    int                   x0 = 0, x1 = 0, y0 = 0, y1 = 0;
};

/// A map's floors as runs, for drawing: what build_sketch makes of a page.
struct MapSketch {
    std::string         stem;
    int                 w_px = 0;
    int                 h_px = 0;
    std::size_t         columns = 0;    ///< floor columns it was made of (MapFloors::floors)
    std::vector<MapRun> runs;

    std::size_t points() const;
};

/// A floor of column x links to one of column x - 1 of the same attribute
/// whose top is within this, up or down: nearest first, one to one. Slopes on
/// the game's maps rise at most 1 px a column; 2 to 8 give the same runs.
inline constexpr int kSketchLinkPx = 4;
/// Points dropped while the run stays within this of the line kept.
inline constexpr double kSketchEps = 1.0;

/// The runs of the floors of columns x0..x1 that `keep` lets through, linked
/// as build_sketch links them, every column's point kept: the home's own floor
/// and the warnings of a hunting zone are found by the same rule the floors
/// are drawn by (hunt_zone.hpp). Null `keep`: every floor.
std::vector<MapRun> link_floors(const MapFloors& f, int x0, int x1, int link_px = kSketchLinkPx,
                                const std::function<bool(int x, const MapFloor&)>& keep = {});

/// Ramer-Douglas-Peucker: the points of `pts` kept while every one dropped is
/// within `eps` of the line between the two kept around it. The ends always.
std::vector<MapPoint> simplify_run(const std::vector<MapPoint>& pts, double eps = kSketchEps);

/// Every floor of `f` linked into runs (link_floors) and each run simplified
/// (simplify_run): built once per map, on the map tab's worker.
MapSketch build_sketch(const std::string& stem, const MapFloors& f, int link_px = kSketchLinkPx,
                       double eps = kSketchEps);

/// The platform pieces a drop is refused from for a gate, as the drop rule
/// refuses them (Hunter::drop_rule, its rule 15): standing on a platform at
/// y0 = its top - 1, landing where Page::floor_below(x, y0, kDropFeetPx) has
/// it, with a gate of `gates` within drop_gate_px across whose y lies from
/// kDropGateSpanPx above y0 to kDropGateSpanPx below the landing. Only the
/// gate's part of the rule: one of its conditions, never the whole of it.
/// Linked into runs and simplified as the floors are. Nothing is dropped
/// through from the bottom ground, nor from a floor with nothing below it, and
/// none of those are marked.
std::vector<MapRun> no_drop_marks(const GameMapBook::Page& p, std::span<const MapGate> gates, int drop_gate_px);

/// The canvas's view of the world: world pixels, y growing downward as in
/// the packets and the .map, never packet_scale or packet_dy. `cx`, `cy` is
/// the world point at the canvas's middle; `zoom` screen pixels a world pixel.
struct MapCamera {
    struct P {
        double x = 0.0;
        double y = 0.0;
    };

    /// Zoomed out to this much of the fit at most, a margin round the map.
    static constexpr double kOutOfFit = 0.9;
    /// Zoomed in to this at least, and to the fit when that is larger.
    static constexpr double kMaxZoom = 2.0;
    /// "Theo nhân vật" follows at this, within the two.
    static constexpr double kFollowZoom = 0.5;
    /// The wheel and [−] [+]: this much a notch.
    static constexpr double kZoomStep = 1.2;

    double cx = 0.0;
    double cy = 0.0;
    double zoom = kFollowZoom;

    P to_screen(P w, P origin, P size) const;
    P to_world(P s, P origin, P size) const;

    /// The zoom a whole map w_px x h_px fits the canvas cw x ch at.
    static double fit_zoom(int w_px, int h_px, double cw, double ch);
    static double zmin_for(int w_px, int h_px, double cw, double ch) { return fit_zoom(w_px, h_px, cw, ch) * kOutOfFit; }
    static double zmax_for(double zmin);
    static double follow_zoom(double zmin, double zmax);

    /// Zoom by `factor` within zmin..zmax, the world point under screen point
    /// `s` staying where it is.
    void zoom_about(P s, double factor, P origin, P size, double zmin, double zmax);
    /// The middle kept on the map: within 0..w_px, 0..h_px -- never the map
    /// panned out of the canvas.
    void clamp_centre(int w_px, int h_px);
    /// The whole map in view, at the fit.
    void fit(int w_px, int h_px, P size);
};

} // namespace gobot
