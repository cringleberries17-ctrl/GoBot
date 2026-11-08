#include "gobot/hunt_zone.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/hunter.hpp"
#include "gobot/packet_world.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace gobot {
namespace {

const char* const kRefuseSmall = "Vùng phải rộng ít nhất 100 px và cao ít nhất 40 px.";
const char* const kRefuseOffMap = "Vùng phải nằm trong bản đồ.";
const char* const kRefuseLarge = "Vùng lớn quá cỡ cho phép.";
const char* const kRefuseHome = "Chỗ về phải nằm trên một bục, trong vùng.";
const char* const kRefuseNoFloor = "Vùng phải chứa ít nhất một bục để làm chỗ về.";

/// GoC reads attribute 4 as a wall: never stood on.
bool wall(const MapFloor& f) { return f.wall(); }

/// Where the packets put feet on a floor: its top less one.
int stand_y(const MapFloor& f) { return f.y - 1; }

/// v kept within lo..hi; `keep` when the two cross -- a zone that was not
/// valid to begin with is left as it was.
int bounded(int v, int lo, int hi, int keep) { return lo > hi ? keep : std::clamp(v, lo, hi); }

/// A floor of the box to stand on: not a wall, its standing point inside.
bool box_floor(const HuntZone& z, const MapFloor& f) {
    return !wall(f) && stand_y(f) >= z.top && stand_y(f) <= z.bottom;
}

bool box_has_floor(const GameMapBook::Page& p, const HuntZone& z) {
    const int x0 = std::max(0, z.left), x1 = std::min(p.floors.w_px() - 1, z.right);
    for (int x = x0; x <= x1; ++x)
        for (const MapFloor& f : p.floors_at(x))
            if (box_floor(z, f)) return true;
    return false;
}

/// The home's floor, not a wall; none when it stands on none.
std::optional<MapFloor> home_floor(const GameMapBook::Page& p, const HuntZone& z) {
    const std::optional<MapFloor> f = p.floor_near(z.home_x, z.home_y);
    if (!f || wall(*f)) return std::nullopt;
    return f;
}

/// The runs of the box's floors -- and of the home's own, wherever it stands
/// against the box's top or bottom -- as link_floors links them.
std::vector<MapRun> box_runs(const GameMapBook::Page& p, const HuntZone& z, const std::optional<MapFloor>& home) {
    return link_floors(p.floors, z.left, z.right, kSketchLinkPx, [&](int x, const MapFloor& f) {
        if (wall(f)) return false;
        return box_floor(z, f) || (home && x == z.home_x && f.y == home->y);
    });
}

const MapRun* run_with(const std::vector<MapRun>& runs, int x, int y) {
    for (const MapRun& r : runs)
        if (x >= r.x0 && x <= r.x1)
            for (const MapPoint& p : r.pts)
                if (p.x == x && p.y == y) return &r;
    return nullptr;
}

/// The pieces of `pts` (one run, a column a point) where `on` holds, each
/// simplified as the floors are, into `out`.
template <typename On>
void pieces(const std::vector<MapPoint>& pts, On on, std::vector<std::vector<MapPoint>>& out) {
    std::vector<MapPoint> piece;
    for (const MapPoint& p : pts) {
        if (on(p)) {
            piece.push_back(p);
            continue;
        }
        if (!piece.empty()) out.push_back(simplify_run(piece));
        piece.clear();
    }
    if (!piece.empty()) out.push_back(simplify_run(piece));
}

/// A run's line -- simplified -- passing through the box: the floor tops it
/// joins, a standing point 1 px above them inside.
bool run_in_box(const MapRun& r, const HuntZone& z) {
    if (r.x1 < z.left || r.x0 > z.right || r.y1 - 1 < z.top || r.y0 - 1 > z.bottom) return false;
    const auto inside_y = [&](double y) { return y - 1.0 >= z.top && y - 1.0 <= z.bottom; };
    if (r.pts.size() == 1) return z.contains(r.pts[0].x, r.pts[0].y - 1);
    for (std::size_t i = 1; i < r.pts.size(); ++i) {
        const MapPoint a = r.pts[i - 1], b = r.pts[i];
        const int xa = std::max(a.x, z.left), xb = std::min(b.x, z.right);
        if (xa > xb) continue;
        const double span = b.x == a.x ? 1.0 : static_cast<double>(b.x - a.x);
        const double ya = a.y + (b.y - a.y) * (xa - a.x) / span;
        const double yb = a.y + (b.y - a.y) * (xb - a.x) / span;
        // A straight piece: inside at an end, or crossing the box between.
        if (inside_y(ya) || inside_y(yb) || (std::min(ya, yb) - 1.0 < z.top && std::max(ya, yb) - 1.0 > z.bottom))
            return true;
    }
    return false;
}

/// A whole number and nothing else.
bool int_of(const std::string& s, int& out) {
    if (s.empty()) return false;
    const char* end = s.data() + s.size();
    const std::from_chars_result r = std::from_chars(s.data(), end, out);
    return r.ec == std::errc() && r.ptr == end;
}

const char* const kKeys[] = { "home_x", "home_y", "left", "top", "right", "bottom", "saved" };

bool known_key(const std::string& k) {
    return std::any_of(std::begin(kKeys), std::end(kKeys), [&](const char* n) { return k == n; });
}

/// A section of spots.ini as a zone: the six numbers there and whole, the box
/// at least kZoneMinW x kZoneMinH on no map larger than a .map can be, its
/// home inside.
bool read_zone(const IniFile& ini, const std::string& section, HuntZone& z) {
    int* const at[] = { &z.home_x, &z.home_y, &z.left, &z.top, &z.right, &z.bottom };
    for (std::size_t i = 0; i < 6; ++i)
        if (!int_of(ini.get(section, kKeys[i]), *at[i])) return false;
    z.saved = ini.get(section, "saved");
    ZoneLimits any;
    any.w_px = any.h_px = GameMapBook::kMaxCells * GameMapBook::kCellPx;
    any.floors = false;
    return zone_refusal(z, any, nullptr) == nullptr;
}

void write_zone(IniFile& ini, const std::string& section, const HuntZone& z) {
    ini.set_int(section, "home_x", z.home_x);
    ini.set_int(section, "home_y", z.home_y);
    ini.set_int(section, "left", z.left);
    ini.set_int(section, "top", z.top);
    ini.set_int(section, "right", z.right);
    ini.set_int(section, "bottom", z.bottom);
    ini.set(section, "saved", z.saved);
}

} // namespace

bool same_place(const HuntZone& a, const HuntZone& b) {
    return a.home_x == b.home_x && a.home_y == b.home_y && a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
}

std::string zone_text(const HuntZone& z) {
    char text[128];
    std::snprintf(text, sizeof text, "home %d,%d, box %d..%d x %d..%d", z.home_x, z.home_y, z.left, z.right, z.top,
                  z.bottom);
    return text;
}

int zone_outside_px(const HuntZone& z, int x, int y) {
    const int dx = x < z.left ? z.left - x : x > z.right ? x - z.right : 0;
    const int dy = y < z.top ? z.top - y : y > z.bottom ? y - z.bottom : 0;
    return std::max(dx, dy);
}

const char* zone_take_name(ZoneTake t) {
    switch (t) {
        case ZoneTake::Wait:     return "waits";
        case ZoneTake::Edited:   return "edited";
        case ZoneTake::Inside:   return "inside";
        case ZoneTake::Near:     return "near";
        case ZoneTake::Back:     return "back where it left";
        case ZoneTake::Anywhere: return "wherever the character stands";
    }
    return "?";
}

ZoneTake zone_take(const HuntZone& z, float x, float y, bool edit_in_use, bool back_where_left) {
    if (edit_in_use) return ZoneTake::Edited;
    const int xi = static_cast<int>(std::lround(x));
    const int yi = static_cast<int>(std::lround(y));
    if (z.contains(xi, yi)) return ZoneTake::Inside;
    if (zone_outside_px(z, xi, yi) <= kZoneTakePx) return ZoneTake::Near;
    if (back_where_left) return ZoneTake::Back;
    return kZoneTakeAnywhere ? ZoneTake::Anywhere : ZoneTake::Wait;
}

ZoneLimits zone_limits(const GameMapBook::Page* page, bool doubtful) {
    ZoneLimits l;
    if (page == nullptr || page->floors.w_px() <= 0) {
        l.floors = false;
        return l;
    }
    l.w_px = page->floors.w_px();
    l.h_px = page->floors.h_px();
    l.floors = !doubtful;
    return l;
}

MapPoint zone_max_size(const ZoneLimits& l) {
    // Q3 (a): the whole map. For (b): { std::min(4000, l.w_px), std::min(1000, l.h_px) }.
    return { l.w_px, l.h_px };
}

const char* zone_refusal(const HuntZone& z, const ZoneLimits& l, const GameMapBook::Page* page) {
    if (z.width() < kZoneMinW || z.height() < kZoneMinH) return kRefuseSmall;
    if (z.left < 0 || z.top < 0 || z.right > l.w_px - 1 || z.bottom > l.h_px - 1) return kRefuseOffMap;
    const MapPoint most = zone_max_size(l);
    if (z.width() > most.x || z.height() > most.y) return kRefuseLarge;
    if (!z.contains(z.home_x, z.home_y)) return kRefuseHome;
    if (l.floors && page != nullptr) {
        if (!box_has_floor(*page, z)) return kRefuseNoFloor;
        if (!home_floor(*page, z)) return kRefuseHome;
    }
    return nullptr;
}

bool zone_valid(const HuntZone& z, const ZoneLimits& l, const GameMapBook::Page* page) {
    return zone_refusal(z, l, page) == nullptr;
}

std::optional<MapPoint> snap_home(const GameMapBook::Page& p, int wx, int wy, const HuntZone* within) {
    const int w = p.floors.w_px();
    if (w <= 0) return std::nullopt;
    const int x0 = std::clamp(wx, 0, w - 1);
    // One column: the first floor no more than kSnapAbovePx above the point,
    // top first, or else the lowest above it -- the nearest.
    const auto in_column = [&](int x) -> std::optional<MapPoint> {
        if (x < 0 || x >= w) return std::nullopt;
        std::optional<MapPoint> above;
        for (const MapFloor& f : p.floors_at(x)) {
            if (wall(f)) continue;
            const MapPoint at{ x, stand_y(f) };
            if (within != nullptr && !within->contains(at.x, at.y)) continue;
            if (f.y >= wy - kSnapAbovePx) return at;
            above = at;
        }
        return above;
    };
    if (const std::optional<MapPoint> here = in_column(x0)) return here;
    for (int d = 1; d <= kSnapSearchCols; ++d) {
        if (const std::optional<MapPoint> left = in_column(x0 - d)) return left;
        if (const std::optional<MapPoint> right = in_column(x0 + d)) return right;
    }
    return std::nullopt;
}

std::optional<HuntZone> zone_around(const GameMapBook::Page* page, MapPoint at, int half_w, int half_h,
                                    const ZoneLimits& l) {
    MapPoint home = at;
    if (l.floors && page != nullptr) {
        const std::optional<MapPoint> snapped = snap_home(*page, at.x, at.y);
        if (!snapped) return std::nullopt;
        home = *snapped;
    }
    const MapPoint most = zone_max_size(l);
    half_w = std::min(half_w, most.x / 2);
    half_h = std::min(half_h, most.y / 2);
    HuntZone z;
    z.home_x = home.x;
    z.home_y = home.y;
    z.left = std::max(0, home.x - half_w);
    z.right = std::min(l.w_px - 1, home.x + half_w);
    z.top = std::max(0, home.y - half_h);
    z.bottom = std::min(l.h_px - 1, home.y + half_h);
    // 04d Q5 (b): the home on the box's lowest floor -- everything above it
    // comes down to it. (a), as it is: where the character stands.
    if constexpr (kZoneAroundHomeLowest) {
        if (l.floors && page != nullptr) {
            if (const std::optional<MapPoint> low = zone_home_lowest(*page, z, home.x)) {
                z.home_x = low->x;
                z.home_y = low->y;
            }
        }
    }
    return z;
}

std::optional<MapPoint> zone_home_lowest(const GameMapBook::Page& p, const HuntZone& z, int near_x) {
    std::optional<MapPoint> best;
    const int x0 = std::max(0, z.left), x1 = std::min(p.floors.w_px() - 1, z.right);
    for (int x = x0; x <= x1; ++x) {
        for (const MapFloor& f : p.floors_at(x)) {
            if (!box_floor(z, f)) continue;
            const MapPoint at{ x, stand_y(f) };
            if (!best || at.y > best->y ||
                (at.y == best->y && std::abs(at.x - near_x) < std::abs(best->x - near_x)))
                best = at;
        }
    }
    return best;
}

std::optional<HuntZone> zone_moved(const GameMapBook::Page* page, const HuntZone& from, MapPoint at,
                                   const ZoneLimits& l) {
    MapPoint home = at;
    if (l.floors && page != nullptr) {
        const std::optional<MapPoint> snapped = snap_home(*page, at.x, at.y);
        if (!snapped) return std::nullopt;
        home = *snapped;
    }
    HuntZone z = from;
    z.home_x = home.x;
    z.home_y = home.y;
    z.left = home.x - (from.home_x - from.left);
    z.right = home.x + (from.right - from.home_x);
    z.top = home.y - (from.home_y - from.top);
    z.bottom = home.y + (from.bottom - from.home_y);
    // Slid back onto the map, the home where it is; cut only when the box is
    // larger than the map.
    const auto slide = [](int& lo, int& hi, int last) {
        if (lo < 0) {
            hi -= lo;
            lo = 0;
        }
        if (hi > last) {
            lo = std::max(0, lo - (hi - last));
            hi = last;
        }
    };
    slide(z.left, z.right, l.w_px - 1);
    slide(z.top, z.bottom, l.h_px - 1);
    return z;
}

std::optional<HuntZone> zone_from_spot(const GameMapBook::Page* page, float x, float y, float left, float top,
                                       float right, float bottom, const ZoneLimits& l) {
    const auto px = [](float v) { return static_cast<int>(std::lround(v)); };
    HuntZone z;
    z.left = std::max(0, px(left));
    z.top = std::max(0, px(top));
    z.right = std::min(l.w_px - 1, px(right));
    z.bottom = std::min(l.h_px - 1, px(bottom));
    if (z.left > z.right || z.top > z.bottom) return std::nullopt;
    z.home_x = std::clamp(px(x), z.left, z.right);
    z.home_y = std::clamp(px(y), z.top, z.bottom);
    if (l.floors && page != nullptr) {
        const std::optional<MapPoint> snapped = snap_home(*page, z.home_x, z.home_y, &z);
        if (!snapped) return std::nullopt;
        z.home_x = snapped->x;
        z.home_y = snapped->y;
    }
    return z;
}

ZoneScreen zone_screen(const HuntZone& z, const MapCamera& cam, MapCamera::P origin, MapCamera::P size) {
    ZoneScreen s;
    const MapCamera::P tl = cam.to_screen({ static_cast<double>(z.left), static_cast<double>(z.top) }, origin, size);
    const MapCamera::P br =
        cam.to_screen({ static_cast<double>(z.right), static_cast<double>(z.bottom) }, origin, size);
    s.l = tl.x;
    s.t = tl.y;
    s.r = br.x;
    s.b = br.y;
    s.home = cam.to_screen({ static_cast<double>(z.home_x), static_cast<double>(z.home_y) }, origin, size);
    s.head = { s.home.x, s.home.y - kZonePinPx };
    return s;
}

ZoneGrip zone_grip(const HuntZone& z, const MapCamera& cam, MapCamera::P origin, MapCamera::P size,
                   MapCamera::P mouse, bool pin_and_strip) {
    const ZoneScreen s = zone_screen(z, cam, origin, size);
    const double mx = mouse.x, my = mouse.y;
    if (pin_and_strip && std::hypot(mx - s.head.x, my - s.head.y) <= kZonePinPx) return ZoneGrip::Home;
    const auto within = [](double a, double b, int px) { return std::abs(a - b) <= px; };
    const bool cl = within(mx, s.l, kZoneGripPx), cr = within(mx, s.r, kZoneGripPx);
    const bool ct = within(my, s.t, kZoneGripPx), cb = within(my, s.b, kZoneGripPx);
    if (ct && cl) return ZoneGrip::TL;
    if (ct && cr) return ZoneGrip::TR;
    if (cb && cl) return ZoneGrip::BL;
    if (cb && cr) return ZoneGrip::BR;
    const bool along_x = mx >= s.l - kZoneEdgePx && mx <= s.r + kZoneEdgePx;
    const bool along_y = my >= s.t - kZoneEdgePx && my <= s.b + kZoneEdgePx;
    if (along_y && within(mx, s.l, kZoneEdgePx)) return ZoneGrip::L;
    if (along_y && within(mx, s.r, kZoneEdgePx)) return ZoneGrip::R;
    if (along_x && within(my, s.t, kZoneEdgePx)) return ZoneGrip::T;
    if (along_x && within(my, s.b, kZoneEdgePx)) return ZoneGrip::B;
    if (pin_and_strip && mx >= s.l && mx <= s.r && my >= s.t && my <= s.t + kZoneStripPx) return ZoneGrip::Strip;
    return ZoneGrip::None;
}

HuntZone gesture_draft(const ZoneGesture& g, MapCamera::P mouse, const ZoneLimits& l) {
    // World pixels from the press, through the camera as it was then.
    const MapCamera::P a = g.cam.to_world(g.press, g.origin, g.size);
    const MapCamera::P b = g.cam.to_world(mouse, g.origin, g.size);
    const int dx = static_cast<int>(std::lround(b.x - a.x));
    const int dy = static_cast<int>(std::lround(b.y - a.y));
    const HuntZone& f = g.from;
    HuntZone z = f;
    const int last_x = l.w_px - 1, last_y = l.h_px - 1;
    const MapPoint most = zone_max_size(l);
    switch (g.grip) {
        case ZoneGrip::None:
            break;
        case ZoneGrip::Home:
            z.home_x = bounded(f.home_x + dx, f.left, f.right, f.home_x);
            z.home_y = bounded(f.home_y + dy, f.top, f.bottom, f.home_y);
            break;
        case ZoneGrip::Strip: {
            const int sx = bounded(dx, -f.left, last_x - f.right, 0);
            const int sy = bounded(dy, -f.top, last_y - f.bottom, 0);
            z.left += sx;
            z.right += sx;
            z.home_x += sx;
            z.top += sy;
            z.bottom += sy;
            z.home_y += sy;
            break;
        }
        default: {
            const bool l_edge = g.grip == ZoneGrip::L || g.grip == ZoneGrip::TL || g.grip == ZoneGrip::BL;
            const bool r_edge = g.grip == ZoneGrip::R || g.grip == ZoneGrip::TR || g.grip == ZoneGrip::BR;
            const bool t_edge = g.grip == ZoneGrip::T || g.grip == ZoneGrip::TL || g.grip == ZoneGrip::TR;
            const bool b_edge = g.grip == ZoneGrip::B || g.grip == ZoneGrip::BL || g.grip == ZoneGrip::BR;
            // Each side within the map, the size limits against the side
            // across from it, and never past the home.
            if (l_edge)
                z.left = bounded(f.left + dx, std::max(0, f.right - most.x),
                                 std::min(f.right - kZoneMinW, f.home_x), f.left);
            if (r_edge)
                z.right = bounded(f.right + dx, std::max(f.left + kZoneMinW, f.home_x),
                                  std::min(last_x, f.left + most.x), f.right);
            if (t_edge)
                z.top = bounded(f.top + dy, std::max(0, f.bottom - most.y),
                                std::min(f.bottom - kZoneMinH, f.home_y), f.top);
            if (b_edge)
                z.bottom = bounded(f.bottom + dy, std::max(f.top + kZoneMinH, f.home_y),
                                   std::min(last_y, f.top + most.y), f.bottom);
            break;
        }
    }
    return z;
}

std::vector<MapPoint> home_run(const GameMapBook::Page& p, const HuntZone& z) {
    const std::optional<MapFloor> home = home_floor(p, z);
    if (!home) return {};
    const std::vector<MapRun> runs = box_runs(p, z, home);
    const MapRun* own = run_with(runs, z.home_x, home->y);
    return own ? own->pts : std::vector<MapPoint>{};
}

bool zone_red_is_bad(int red_cols, int floor_cols) {
    return floor_cols > 0 && red_cols * 100 > floor_cols * kZoneRedBadPercent;
}

ZoneCheck zone_check(const GameMapBook::Page* page, const HuntZone& z, const ZoneLimits& l,
                     std::span<const MapRun> no_drop, bool drop_down, std::span<const MapGate> gates) {
    ZoneCheck c;
    c.refuse_vi = zone_refusal(z, l, page);
    if (c.refuse_vi != nullptr || page == nullptr || !l.floors) return c;

    // The floors against the home, run by run, by the rule they are drawn by:
    // the home's own run is where it walks; any other is a floor of its own.
    const std::optional<MapFloor> home = home_floor(*page, z);
    const std::vector<MapRun> runs = box_runs(*page, z, home);
    const MapRun* own = home ? run_with(runs, z.home_x, home->y) : nullptr;
    for (const MapRun& r : runs) {
        const bool mine = &r == own;
        // More than one jump below the home: go_home calls that TooHigh.
        const auto low = [&](const MapPoint& p) { return p.y - 1 - z.home_y > Hunter::kOneJumpPx; };
        // Above it, past the level a walk back keeps to: only off an edge.
        const auto high = [&](const MapPoint& p) { return !mine && z.home_y - (p.y - 1) > Hunter::kSpotFloorPx; };
        for (const MapPoint& p : r.pts) {
            ++c.floor_cols;
            if (low(p)) {
                ++c.red_cols;
                (mine ? c.home_slope_low : c.below_unreachable) = true;
            }
            if (high(p)) c.above_by_edge = true;
        }
        pieces(r.pts, low, c.red);
        if (!mine) pieces(r.pts, high, c.amber);
    }
    // Much of it red is said in red, at every edit: the owner's box of
    // 2026-11-04 20:39 -- 1068 of 1814 -- was said in amber, and hunted.
    c.red_bad = zone_red_is_bad(c.red_cols, c.floor_cols);

    // The home by a gate, drops or not: a map entry's spawn, as both homes of
    // 2026-11-04 were. Warned of, never refused.
    const std::span<const MapGate> doors = gates.empty() ? std::span<const MapGate>(page->gates) : gates;
    for (const MapGate& g : doors) {
        const int d = static_cast<int>(std::lround(std::hypot(g.x - z.home_x, g.y - z.home_y)));
        if (d <= kZoneHomeGatePx && (c.home_gate_px < 0 || d < c.home_gate_px)) c.home_gate_px = d;
    }
    c.home_by_gate = c.home_gate_px >= 0;

    if (!drop_down) return c;
    // A gate's no-drop piece, or a gate, inside the box.
    c.gate_near = std::any_of(no_drop.begin(), no_drop.end(), [&](const MapRun& r) { return run_in_box(r, z); }) ||
                  std::any_of(page->gates.begin(), page->gates.end(),
                              [&](const MapGate& g) { return z.contains(g.x, g.y); });
    // The shortest drop from a platform standing in the box: the box must be
    // that tall, and a little, to hold where it stands and where it lands.
    int shortest = INT_MAX;
    const int x0 = std::max(0, z.left), x1 = std::min(page->floors.w_px() - 1, z.right);
    for (int x = x0; x <= x1; ++x) {
        for (const MapFloor& f : page->floors_at(x)) {
            if (!f.platform() || !box_floor(z, f)) continue;
            const std::optional<MapFloor> land = page->floor_below(x, stand_y(f), Hunter::kDropFeetPx);
            if (!land || land->y - stand_y(f) < Hunter::kDropMinFallPx) continue;
            shortest = std::min(shortest, land->y - f.y);
        }
    }
    if (shortest != INT_MAX) {
        c.shortest_drop_px = shortest;
        c.too_short_for_drops = z.height() < shortest + Hunter::kDropFeetPx;
    }
    return c;
}

// ------------------------------------------------------- walking in the zone

std::optional<MapPoint> walk_off_landing(const GameMapBook::Page& p, MapPoint end, int dir, double speed_px_s) {
    const int step = dir < 0 ? -1 : 1;
    const int w = p.floors.w_px(), h = p.floors.h_px();
    const int feet = end.y - 1;
    if (speed_px_s <= 0.0) {
        // Straight down the column past the end: the first floor below the feet.
        const int x = end.x + step;
        if (x < 0 || x >= w) return std::nullopt;
        for (const MapFloor& f : p.floors_at(x))
            if (!wall(f) && stand_y(f) > feet) return MapPoint{ x, stand_y(f) };
        return std::nullopt;
    }
    // A column further each 1/speed s, the feet kGravity's way lower: down on
    // the first floor whose standing line they cross -- one rising toward
    // them a link's worth between two columns too. At the map's side, on
    // straight down.
    double before = feet;
    int x = end.x;
    for (int k = 1;; ++k) {
        if (end.x + step * k >= 0 && end.x + step * k < w) x = end.x + step * k;
        const double t = k / speed_px_s;
        const double y = feet + 0.5 * PacketWorld::kGravity * t * t;
        for (const MapFloor& f : p.floors_at(x)) {
            const int s = stand_y(f);
            if (!wall(f) && s > before - GameMapBook::kFollowLinkPx && s <= y) return MapPoint{ x, s };
        }
        if (y > h) return std::nullopt;
        before = y;
    }
}

const char* zone_walk_name(ZoneWalk::Why w) {
    switch (w) {
        case ZoneWalk::Why::Inside:   return "stays in the area";
        case ZoneWalk::Why::Closer:   return "comes no further out of the area";
        case ZoneWalk::Why::Leaves:   return "leaves the area along its floor";
        case ZoneWalk::Why::WalkOff:  return "walks off its floor's end, out of the area";
        case ZoneWalk::Why::Unjudged: return "not judged";
    }
    return "?";
}

ZoneWalk walk_stays_in(const GameMapBook::Page& p, const HuntZone& z, MapPoint from, int dir, int ahead_px) {
    ZoneWalk out;
    if constexpr (!kZoneWalkStaysIn) return out;
    const MapTrail t = p.follow(from.x, from.y, dir, ahead_px);
    if (t.pts.empty()) return out;
    // How far outside the character is now, by its floor's standing line: a
    // fix a pixel or two off it would let a step out pass as no further.
    const int now = zone_outside_px(z, t.pts.front().x, t.pts.front().y - 1);
    for (const MapPoint& q : t.pts) {
        const int o = zone_outside_px(z, q.x, q.y - 1);
        if (o <= now) continue;
        out.ok = false;
        out.why = ZoneWalk::Why::Leaves;
        out.at = { q.x, q.y - 1 };
        out.out_px = o;
        return out;
    }
    if (t.ledge) {
        // Off the end ahead: wherever the fall lands, at a walk's speed or
        // with the arrow let go of, it lands no further out -- in the box,
        // from inside it (Q1).
        const MapPoint end = t.pts.back();
        for (const double share : kZoneWalkOffShares) {
            const std::optional<MapPoint> land = walk_off_landing(p, end, dir, share * kZoneWalkPxPerS);
            const int o = land ? zone_outside_px(z, land->x, land->y) : INT_MAX;
            if (kZoneFallInsideOk && o <= now) continue;
            out.ok = false;
            out.why = ZoneWalk::Why::WalkOff;
            out.at = { end.x, end.y - 1 };
            out.land = land;
            out.out_px = land ? o : 0;
            return out;
        }
    }
    out.why = now == 0 ? ZoneWalk::Why::Inside : ZoneWalk::Why::Closer;
    return out;
}

std::vector<ZoneLegs::Fall> zone_falls(const GameMapBook::Page& p, const HuntZone& z, const MapTrail& left,
                                       const MapTrail& right) {
    std::vector<ZoneLegs::Fall> out;
    // Off each end: on to the floor the fall lands on, when it lands in the
    // box at every kZoneWalkOffShares; from where a walk's speed puts it.
    for (const MapTrail* t : { &left, &right }) {
        if (!t->ledge) continue;
        const int dir = t == &left ? -1 : 1;
        const MapPoint end = t->pts.back();
        std::optional<MapPoint> land;
        bool in_box = true;
        for (const double share : kZoneWalkOffShares) {
            land = walk_off_landing(p, end, dir, share * kZoneWalkPxPerS);
            in_box = in_box && land && z.contains(land->x, land->y);
        }
        if (!in_box) continue;
        out.push_back(ZoneLegs::Fall{ end, dir, *land });
    }
    return out;
}

ZoneLegs zone_legs_from(const GameMapBook::Page& p, const HuntZone& z, MapPoint at, const JumpRules* up) {
    ZoneLegs l;
    l.left = p.follow(at.x, at.y, -1);
    l.right = p.follow(at.x, at.y, 1);
    if (l.left.pts.empty() || l.right.pts.empty()) return l;
    l.falls = zone_falls(p, z, l.left, l.right);
    if (up != nullptr) l.jumps = jump_legs_from(p, z, at, *up);
    return l;
}

std::optional<ZoneReturn> return_point(const GameMapBook::Page& p, const HuntZone& z, MapPoint from,
                                       const JumpRules* up) {
    const std::optional<MapFloor> home = home_floor(p, z);
    // A floor reached: where on it, after how many falls and jumps and how
    // much walking, and -- when the first leg goes off the floor it starts on
    // -- where that walk is aimed, and the jump it is when it is one.
    struct Leg {
        MapPoint at;
        int      falls = 0;
        int      jumps = 0;
        int      walked = 0;
        int      first_x = 0;
        std::optional<JumpPlan> first_jump;
    };
    // Best first: the home; the fewest jumps, then falls; the nearest the home
    // across, then up or down; the least walking.
    using Rank = std::array<int, 6>;
    std::optional<ZoneReturn> best;
    Rank best_rank{};
    std::vector<Leg> todo{ Leg{ from, 0, 0, 0, 0, std::nullopt } };
    std::vector<Leg> jumped;                            // the legs a jump more on: the next round's
    std::vector<std::pair<MapPoint, MapPoint>> seen;    // each floor once, by its two ends
    // A round per number of jumps -- one, without `up` -- so a floor is had by
    // the fewest jumps first, and by the fewest falls in its round.
    const int rounds = up != nullptr ? kZoneReturnJumpsMax + 1 : 1;
    for (int round = 0; round < rounds && !todo.empty(); ++round) {
        // The home had with fewer jumps is never bettered.
        if (best && best->home) break;
        for (std::size_t i = 0; i < todo.size(); ++i) {
            const Leg leg = todo[i];
            const MapTrail left = p.follow(leg.at.x, leg.at.y, -1);
            const MapTrail right = p.follow(leg.at.x, leg.at.y, 1);
            if (left.pts.empty() || right.pts.empty()) continue;
            const MapPoint l_end = left.pts.back(), r_end = right.pts.back();
            if (std::any_of(seen.begin(), seen.end(), [&](const std::pair<MapPoint, MapPoint>& s) {
                    return s.first.x == l_end.x && s.first.y == l_end.y && s.second.x == r_end.x &&
                           s.second.y == r_end.y;
                }))
                continue;
            seen.emplace_back(l_end, r_end);
            const bool first = leg.falls == 0 && leg.jumps == 0;
            for (const MapTrail* t : { &left, &right }) {
                for (const MapPoint& q : t->pts) {
                    const MapPoint s{ q.x, q.y - 1 };
                    if (!z.contains(s.x, s.y)) continue;
                    const bool at_home = home && q.x == z.home_x && q.y == home->y;
                    const int walked = leg.walked + std::abs(q.x - leg.at.x);
                    const Rank rank{ at_home ? 0 : 1, leg.jumps, leg.falls, std::abs(s.x - z.home_x),
                                     std::abs(s.y - z.home_y), walked };
                    if (best && !(rank < best_rank)) continue;
                    best_rank = rank;
                    ZoneReturn r;
                    r.at = at_home ? MapPoint{ z.home_x, z.home_y } : s;
                    r.floor_y = q.y;
                    r.home = at_home;
                    r.falls = leg.falls;
                    r.jumps = leg.jumps;
                    r.walk_x = first ? r.at.x : leg.first_x;
                    r.jump = leg.first_jump;
                    best = r;
                }
            }
            // Off each end: on to the floor the fall lands on (zone_falls, the
            // seek's rule too).
            if (leg.falls < kZoneReturnFallsMax) {
                for (const ZoneLegs::Fall& fall : zone_falls(p, z, left, right)) {
                    Leg next = leg;
                    next.at = fall.land;
                    next.falls = leg.falls + 1;
                    next.walked = leg.walked + std::abs(fall.end.x - leg.at.x) + std::abs(fall.land.x - fall.end.x);
                    if (first) next.first_x = fall.end.x + fall.dir * kZoneWalkOffPastPx;
                    todo.push_back(next);
                }
            }
            // Straight up off it, onto the floors a jump lands on in the box --
            // the next round's, unless the home is had already.
            if (up != nullptr && leg.jumps < kZoneReturnJumpsMax && !(best && best->home)) {
                for (const JumpPlan& j : jump_legs_from(p, z, leg.at, *up)) {
                    Leg next = leg;
                    next.at = j.land();
                    next.jumps = leg.jumps + 1;
                    next.walked = leg.walked + j.walk_px;
                    if (first) {
                        next.first_x = j.x;
                        next.first_jump = j;
                    }
                    jumped.push_back(next);
                }
            }
        }
        todo = std::move(jumped);
        jumped.clear();
    }
    if (best) return best;
    // With jumps: no blind hop -- the way is planned, or there is none.
    if (up != nullptr) return std::nullopt;
    // Nothing in the box by walking and falling: the home as every walk back
    // went before -- toward it, hops and all, unless jump_up is ticked where
    // jumps up may be planned (Hunter::hop never presses there) -- when it is
    // no more than one jump above; else no way back.
    if (from.y - z.home_y > Hunter::kOneJumpPx) return std::nullopt;
    ZoneReturn r;
    r.at = { z.home_x, z.home_y };
    r.floor_y = home ? home->y : z.home_y + 1;
    r.home = true;
    r.walk_x = z.home_x;
    r.as_before = true;
    return r;
}

bool zone_walks_back_first(int outside_px, const std::optional<ZoneReturn>& way) {
    // Q2 (a): only with a way back the map shows. (b) would be: never --
    // `return false;` -- monsters in reach fought wherever they stand.
    return outside_px > kZoneOutsideBackPx && way && !way->as_before;
}

bool zone_stands_at(const GameMapBook::Page* p, MapPoint goal, MapPoint at, int tol_px) {
    if (std::abs(at.x - goal.x) > tol_px) return false;
    if (p != nullptr) {
        const std::optional<MapFloor> mine = p->floor_near(at.x, at.y);
        const std::optional<MapFloor> its = p->floor_near(goal.x, goal.y);
        if (mine && !wall(*mine) && its) {
            const std::optional<int> top = p->follow_to(at.x, at.y, goal.x);
            return top && std::abs(*top - its->y) <= GameMapBook::kFollowLinkPx;
        }
    }
    return std::abs(at.y - goal.y) <= Hunter::kSpotFloorPx;
}

bool zone_at_home(const GameMapBook::Page* p, const HuntZone& z, MapPoint at, int tol_px) {
    return zone_stands_at(p, { z.home_x, z.home_y }, at, tol_px);
}

// -------------------------------------------------------------- the file

std::string zone_stem(const std::string& stem) {
    std::string s;
    if (stem.size() < 4 || stem.size() > 24) return s;
    for (const char c : stem) s += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    // t<digits>_s<digits>
    std::size_t i = 0;
    const auto digits = [&] {
        const std::size_t from = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        return i > from;
    };
    const bool ok = s[i++] == 't' && digits() && i < s.size() && s[i++] == '_' && i < s.size() && s[i++] == 's' &&
                    digits() && i == s.size();
    return ok ? s : std::string();
}

const char* ZoneBook::header() {
    return "GoBot: hunting areas drawn on the Bản đồ tab, one per map. The section is\n"
           "the game's map file name (data\\Map\\t52_s2.map). World pixels of the game's\n"
           "packets and map files, y growing downward. Written by GoBot; sections and\n"
           "keys it does not know are kept.";
}

ZoneBook::Load ZoneBook::load(const std::string& path) {
    zones_.clear();
    skipped_ = 0;
    if (!path_exists(path)) return Load::Missing;
    IniFile ini;
    if (is_folder(path) || !ini.load(path)) return Load::Bad;
    // Sections come sorted, a name in capitals before the same in lowercase:
    // the lowercase one is the one kept when both read.
    for (const std::string& section : ini.sections()) {
        const std::string stem = zone_stem(section);
        if (stem.empty()) continue;
        HuntZone z;
        if (!read_zone(ini, section, z)) {
            ++skipped_;
            continue;
        }
        if (zones_.count(stem) != 0 && section != stem) continue;
        zones_[stem] = z;
        revs_[stem] = ++rev_;
    }
    return Load::Ok;
}

std::optional<HuntZone> ZoneBook::get(const std::string& stem) const {
    const auto it = zones_.find(zone_stem(stem));
    if (it == zones_.end()) return std::nullopt;
    return it->second;
}

bool ZoneBook::put(const std::string& stem, const HuntZone& z) {
    const std::string s = zone_stem(stem);
    if (s.empty()) return false;
    zones_[s] = z;
    revs_[s] = ++rev_;
    return true;
}

void ZoneBook::erase(const std::string& stem) {
    const std::string s = zone_stem(stem);
    if (s.empty()) return;
    zones_.erase(s);
    revs_[s] = ++rev_;
}

std::vector<std::string> ZoneBook::stems() const {
    std::vector<std::string> out;
    for (const auto& [s, z] : zones_) out.push_back(s);
    return out;
}

std::uint64_t ZoneBook::revision(const std::string& stem) const {
    const auto it = revs_.find(zone_stem(stem));
    return it == revs_.end() ? 0 : it->second;
}

bool ZoneBook::save_many(const std::string& path, const std::map<std::string, std::optional<HuntZone>>& batch) {
    for (const auto& [stem, v] : batch)
        if (stem.empty() || zone_stem(stem) != stem) return false;
    // The file as it is now, hand edits and all: one there that does not read
    // is never written over.
    IniFile in;
    if (path_exists(path) && (is_folder(path) || !in.load(path))) return false;

    IniFile out;
    // The keys GoBot does not know, of each section being written -- in
    // whatever case its name was.
    std::map<std::string, std::map<std::string, std::string>> extra;
    for (const std::string& section : in.sections()) {
        const std::string stem = zone_stem(section);
        if (!stem.empty() && batch.count(stem) != 0) {
            for (const std::string& k : in.keys(section))
                if (!known_key(k)) extra[stem][k] = in.get(section, k);
            continue;
        }
        for (const std::string& k : in.keys(section)) out.set(section, k, in.get(section, k));
    }
    for (const auto& [stem, v] : batch) {
        if (!v) continue;
        for (const auto& [k, value] : extra[stem]) out.set(stem, k, value);
        write_zone(out, stem, *v);
    }

    const std::string fresh = path + ".new";
    bool ok = out.save(fresh, header());
    if (ok) {
        // Read back before it takes the file's place: a write cut short is
        // never moved over the one that was whole.
        IniFile back;
        ok = back.load(fresh) && back.sections().size() == out.sections().size();
        for (const auto& [stem, v] : batch) {
            if (!ok) break;
            HuntZone z;
            ok = v ? read_zone(back, stem, z) && same_place(z, *v) : !back.has_section(stem);
        }
    }
    if (ok) ok = move_file(fresh, path);
    if (!ok) remove_file(fresh);
    return ok;
}

void ZoneSaveQueue::commit(const std::string& stem, const std::optional<HuntZone>& v) { owed_[stem] = v; }

std::optional<ZoneSaveQueue::Batch> ZoneSaveQueue::take() {
    if (out_ || owed_.empty()) return std::nullopt;
    Batch b;
    b.swap(owed_);
    out_ = true;
    return b;
}

void ZoneSaveQueue::done(const Batch& batch, bool ok) {
    out_ = false;
    if (ok) return;
    // Owed again -- but never over a newer value given since.
    for (const auto& [stem, v] : batch) owed_.try_emplace(stem, v);
}

} // namespace gobot
