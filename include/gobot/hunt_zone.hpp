#pragma once

// A hunting zone drawn on the "Bản đồ" tab (2026.11.04c): a free rectangle of
// one map, in the world pixels of its .map and the packets, and a "chỗ về" --
// the home, a point on a floor inside it that the bot walks back to when no
// monster is left. One per map stem, in configs\spots.ini, which GoBot alone
// writes and the package never ships.
//
// Pure, but for ZoneBook's file: the gestures that edit a zone on the canvas,
// snapping the home to a floor, the checks a zone is refused or warned by,
// and the queue its saves go through. The GUI thread edits; the map tab's
// worker reads and writes the file.
//
// The owner's three answers live here, each in one place, so another answer is
// a small change: when a zone takes over (zone_take, Q1), what "Đặt chỗ này"
// does on a map with a zone (kSpotHereMovesZone, Q2) and how large a zone may
// be (zone_max_size, Q3).
//
// And, for 2026.11.04d (the first live hunt with a zone, 2026-11-04 20:39: the
// character walked down t52_s2's sloping ground out of the box and never came
// back), the zone's floors as a walk goes over them: whether a walk stays in
// the box (walk_stays_in), the way back into it (return_point), whether the
// character is home by height as well as across (zone_at_home), and the
// warnings a box the bot cannot walk back from gets. The owner's answers of
// 04d that live here, (a) until answered: a walk never leaves the box, a fall
// onto a floor inside it aside (kZoneWalkStaysIn, kZoneFallInsideOk, 04d Q1);
// outside it with a way back, the walk back comes first (zone_walks_back_first,
// Q2); "Tạo vùng quanh nhân vật" keeps the home where the character stands
// (kZoneAroundHomeLowest, Q5), and the box's red is said in red
// (zone_red_is_bad).
//
// And, for 2026.11.05b, the way back may jump straight up as well
// (return_point's `up`, jump_plan.hpp): the owner's box on t52_s2 has its home
// on the spawn ledge, which the ground under it reaches by Ctrl + ↑ alone.

#include "gobot/jump_plan.hpp"
#include "gobot/map_book.hpp"
#include "gobot/map_sketch.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gobot {

/// One map's hunting zone. World pixels, y growing downward; the box's edges
/// are inside it.
struct HuntZone {
    int home_x = 0, home_y = 0;     ///< "chỗ về": on a floor (its top - 1) when the map's file is good
    int left = 0, top = 0, right = 0, bottom = 0;
    std::string saved;              ///< local time of its last save: "2026-11-04 21:36"

    int  width() const { return right - left; }
    int  height() const { return bottom - top; }
    bool contains(int x, int y) const { return x >= left && x <= right && y >= top && y <= bottom; }
};

/// The same box and home -- what was saved and when aside.
bool same_place(const HuntZone& a, const HuntZone& b);
/// For the log: "home 1821,359, box 1521..2121 x 200..520".
std::string zone_text(const HuntZone& z);
/// How far outside the box x, y is, the larger of across and up or down; 0
/// inside. What "within 150 px of it" is measured by.
int zone_outside_px(const HuntZone& z, int x, int y);

inline constexpr int kZoneMinW = 100;       ///< narrower is refused
inline constexpr int kZoneMinH = 40;
/// On the canvas, in screen pixels: a corner is gripped within kZoneGripPx of
/// both its edges, an edge within kZoneEdgePx of it, the home's pin within
/// kZonePinPx of its head, and the box moved by a strip kZoneStripPx tall
/// inside its top edge. Its inside is no grip: a drag there pans.
inline constexpr int kZoneGripPx = 8;
inline constexpr int kZoneEdgePx = 5;
inline constexpr int kZonePinPx = 10;
inline constexpr int kZoneStripPx = 14;
/// Snapping the home: a floor up to this above the point is the one it is on;
/// columns this far either way are looked in when its own has no floor.
inline constexpr int kSnapAbovePx = 8;
inline constexpr int kSnapSearchCols = 16;

/// The owner's Q2 (2026-11-04; (a) until answered): "Đặt chỗ này" on a map with
/// a saved zone moves the zone to where the character stands -- the same size,
/// saved, with Hoàn tác. False is (b): the spot for this run only, the zone left
/// as it is.
inline constexpr bool kSpotHereMovesZone = true;

/// The owner's Q1 (2026-11-04; (a) until answered): a saved zone takes over
/// from where the character stands only when it stands within this of the box
/// -- inside it, or this close -- or back this close to where the zone's spot
/// was left (a door there and back, a log in). True is (b): at once, wherever
/// the character is, and it walks home.
inline constexpr int  kZoneTakePx = 150;
inline constexpr bool kZoneTakeAnywhere = false;

/// Why a zone waiting at the hunter takes over, or that it waits (zone_take).
enum class ZoneTake : std::uint8_t {
    Wait,       ///< the character outside it and not back where it left: hunting where it stands
    Edited,     ///< an edit of the zone in use: wherever the character is
    Inside,
    Near,       ///< within kZoneTakePx of the box
    Back,       ///< back within kZoneTakePx of where the zone's spot was left
    Anywhere,   ///< kZoneTakeAnywhere
};
const char* zone_take_name(ZoneTake t);     ///< for the log: "inside", "near", ...
/// The Q1 rule, for a settled fix at x, y: `edit_in_use` -- the zone is the
/// one in use, edited; `back_where_left` -- a spot of this zone was set aside
/// and the character stands within kZoneTakePx of where it left it.
ZoneTake zone_take(const HuntZone& z, float x, float y, bool edit_in_use, bool back_where_left);

/// What a zone of one map may be: its size, and whether its floors are to be
/// gone by -- false with no page, or one doubtful (a map entry whose file
/// MapTrust found off, Mismatch or Distrusted): then the home is never moved
/// without a floor to check it against, and nothing is checked against them.
struct ZoneLimits {
    int  w_px = 10240;
    int  h_px = 10240;
    bool floors = true;
};
ZoneLimits zone_limits(const GameMapBook::Page* page, bool doubtful);

/// The owner's Q3 (2026-11-04; (a) until answered): a zone may be as large as
/// the map. (b) would be 4000 x 1000, the old ±2000 and ±500.
MapPoint zone_max_size(const ZoneLimits& l);

/// Why a zone is refused, in the card's Vietnamese; null when it is not:
/// smaller than kZoneMinW x kZoneMinH, outside the map or larger than
/// zone_max_size, its home outside it -- and, with floors, no floor in it, or
/// its home on none.
const char* zone_refusal(const HuntZone& z, const ZoneLimits& l, const GameMapBook::Page* page);
bool zone_valid(const HuntZone& z, const ZoneLimits& l, const GameMapBook::Page* page);

/// The home's floor: column round(wx), clamped to the map, and of its floors
/// that are not walls (attr 4) the first whose top is no more than
/// kSnapAbovePx above wy -- the floor at the point or just below it -- or else
/// the nearest above. With none in that column, the columns kSnapSearchCols
/// either way, nearest first. (x, top - 1): where the packets put feet on it.
/// With `within`, a place outside that box is passed over. None found: none.
std::optional<MapPoint> snap_home(const GameMapBook::Page& p, int wx, int wy, const HuntZone* within = nullptr);

/// The owner's Q5 of 04d ((a) until answered): "Tạo vùng quanh nhân vật" puts
/// the home where the character stands, and the card says in red when much of
/// the box cannot walk back to it (zone_red_is_bad). True is (b): the home on
/// the lowest floor of the box (zone_home_lowest).
inline constexpr bool kZoneAroundHomeLowest = false;

/// A new zone round `at`: the home there -- snapped to a floor when the
/// limits have floors, none when no floor is near -- and the box half_w and
/// half_h round it, cut to the map; with kZoneAroundHomeLowest the home then
/// on the box's lowest floor. Not checked: zone_refusal says.
std::optional<HuntZone> zone_around(const GameMapBook::Page* page, MapPoint at, int half_w, int half_h,
                                    const ZoneLimits& l);
/// The standing point of the box's lowest floor -- not a wall, its standing
/// point inside the box -- the one nearest `near_x` of the lowest: Q5's (b).
/// None for a box with no floor.
std::optional<MapPoint> zone_home_lowest(const GameMapBook::Page& p, const HuntZone& z, int near_x);
/// `from` moved to `at` ("Dời vùng tới ..."): its size, and its home's place in
/// it, kept; the home snapped as zone_around snaps it; the box slid back onto
/// the map where it would leave it. Not checked.
std::optional<HuntZone> zone_moved(const GameMapBook::Page* page, const HuntZone& from, MapPoint at,
                                   const ZoneLimits& l);
/// This run's spot, taken where the character stood (Hunter::Spot), as a
/// zone -- "Lưu vùng này cho bản đồ", or a drag on its box: its box rounded and
/// cut to the map, its home the spot's x, y, snapped inside the box to a floor
/// when the limits have floors -- none when no floor is near -- and kept
/// inside it otherwise. Not checked.
std::optional<HuntZone> zone_from_spot(const GameMapBook::Page* page, float x, float y, float left, float top,
                                       float right, float bottom, const ZoneLimits& l);

/// What the mouse is over on the canvas.
enum class ZoneGrip : std::uint8_t { None, Home, Strip, L, R, T, B, TL, TR, BL, BR };

/// The box on the canvas, screen pixels: what is drawn and what is gripped.
struct ZoneScreen {
    double l = 0, t = 0, r = 0, b = 0;
    MapCamera::P home;              ///< the pin's point
    MapCamera::P head;              ///< its head, kZonePinPx above
};
ZoneScreen zone_screen(const HuntZone& z, const MapCamera& cam, MapCamera::P origin, MapCamera::P size);

/// The grip under `mouse`, in this order: the pin, a corner, an edge, the
/// strip; never the inside. Without `pin_and_strip` -- no floors to put the
/// home on -- edges and corners only.
ZoneGrip zone_grip(const HuntZone& z, const MapCamera& cam, MapCamera::P origin, MapCamera::P size,
                   MapCamera::P mouse, bool pin_and_strip = true);

/// A drag on a grip, from its press: the zone and the camera as they were
/// then. The camera stays frozen for the drag -- Follow does not move it, nor
/// the wheel -- so what is under the mouse is what was under it.
struct ZoneGesture {
    ZoneGrip     grip = ZoneGrip::None;
    HuntZone     from;
    MapCamera    cam;
    MapCamera::P origin, size, press;
};

/// The zone the drag makes with the mouse at `mouse`, through the gesture's own
/// camera alone: the pin moves the home within the box; the strip moves the
/// box and home together, kept on the map; an edge or a corner moves only its
/// own sides, never past the home, never under kZoneMinW x kZoneMinH, never off
/// the map nor over zone_max_size. Not snapped: that is for the release.
HuntZone gesture_draft(const ZoneGesture& g, MapCamera::P mouse, const ZoneLimits& l);

/// The home's own floor followed left and right inside the box by the rule
/// the floors are drawn by (link_floors): every column's point. None when the
/// home is on no floor.
std::vector<MapPoint> home_run(const GameMapBook::Page& p, const HuntZone& z);

/// Red floor columns -- more than one jump below the home -- past this share
/// of the box's are said in red (zone_red_is_bad): the owner's box of
/// 2026-11-04 20:39 on t52_s2 had 1068 of 1814, and the bot left it for good.
inline constexpr int kZoneRedBadPercent = 50;
/// A gate this near the home -- straight across -- is warned of (ZoneCheck::
/// home_by_gate): a map entry's spawn, where few monsters come and no drop
/// goes. Both homes of 2026-11-04 were 7 px from one. Never a refusal: a walk
/// back presses no ↓.
inline constexpr int kZoneHomeGatePx = 60;

/// What zone_check found: a refusal, or the warnings with the floor pieces
/// they are about (red, amber) for the canvas.
struct ZoneCheck {
    const char* refuse_vi = nullptr;
    bool below_unreachable = false;     ///< other floors more than kOneJumpPx below the home: not walked back from
    bool home_slope_low = false;        ///< the home's own floor runs down more than kOneJumpPx below it
    bool above_by_edge = false;         ///< other floors more than kSpotFloorPx above it
    /// The box's floors, a point a column -- the home's own at its column too
    /// -- and how many of them are red: below_unreachable and home_slope_low's.
    int  floor_cols = 0;
    int  red_cols = 0;
    bool red_bad = false;               ///< zone_red_is_bad: much of the box cannot walk back -- said in red
    int  home_gate_px = -1;             ///< the gate nearest the home, when within kZoneHomeGatePx; -1 none
    bool home_by_gate = false;          ///< ... one is: warned of, drops or not
    bool gate_near = false;             ///< drop_down: a no-drop mark or a gate in the box
    bool too_short_for_drops = false;   ///< drop_down: no drop between its floors fits the box's height
    int  shortest_drop_px = 0;          ///< ... the shortest drop from it, floor to floor: the box wants kDropFeetPx more
    std::vector<std::vector<MapPoint>> red, amber;
};
/// The rule red_bad goes by: more than kZoneRedBadPercent of the floor
/// columns red.
bool zone_red_is_bad(int red_cols, int floor_cols);
/// The zone against its map: zone_refusal, and -- with the limits' floors --
/// the warnings, at each edit's release, never a frame at a time. `no_drop`
/// is the map's no_drop_marks; `gates` the map's as the tab has them, the
/// page's and those learned (MapViewResult::gates) -- none: the page's.
ZoneCheck zone_check(const GameMapBook::Page* page, const HuntZone& z, const ZoneLimits& l,
                     std::span<const MapRun> no_drop, bool drop_down, std::span<const MapGate> gates = {});

// ------------------------------------------- walking in the zone (2026.11.04d)

/// GoC's walking speed: PacketWorld's 9 px a step, 40 steps a second.
inline constexpr double kZoneWalkPxPerS = 360.0;
/// How far ahead a walk's own floor is looked along before it goes on
/// (walk_stays_in): a pass, the key on its way to the game and the report on
/// its way back, at a walk. 48 px is 135 ms, short of that.
inline constexpr int kZoneWalkAheadPx = 96;
/// A walk off a floor's end is worked out at these shares of a walk's speed
/// across -- the arrow let go of at once, up to held all the way down -- and
/// is judged by where each of them lands.
inline constexpr double kZoneWalkOffShares[] = { 0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0 };
/// Walk-offs on a way back, at most (return_point); and jumps straight up,
/// when they may go (2026.11.05b): a floor between for a ledge higher than
/// one jump, never three.
inline constexpr int kZoneReturnFallsMax = 3;
inline constexpr int kZoneReturnJumpsMax = 2;
/// A way back that first walks off the floor it starts on is walked toward
/// this far past that floor's end (ZoneReturn::walk_x).
inline constexpr int kZoneWalkOffPastPx = 16;

/// The owner's Q1 of 04d ((a) until answered): with a zone in use, no walk
/// takes the character out of its box -- down a slope, or off a floor's end --
/// but a fall that lands on a floor inside the box is one it may take
/// (kZoneFallInsideOk). False for either is (b): walks as before, or never off
/// an end at all.
inline constexpr bool kZoneWalkStaysIn = true;
inline constexpr bool kZoneFallInsideOk = true;

/// Where a walk off a floor's end lands: the feet leave `end` -- the floor's
/// last point, its top -- going `dir` at `speed_px_s` across, fall under
/// PacketWorld::kGravity, and come down on the first floor in their way that
/// is not a wall, column by column; at speed 0, straight down the column past
/// the end. Its standing point (top - 1); none when nothing of the map is
/// under them.
std::optional<MapPoint> walk_off_landing(const GameMapBook::Page& p, MapPoint end, int dir, double speed_px_s);

/// What a walk's look along its own floor found (walk_stays_in).
struct ZoneWalk {
    enum class Why : std::uint8_t {
        Inside,     ///< every standing point ahead in the box, and any walk-off's landing
        Closer,     ///< outside the box, and nothing ahead further outside than now: a way in
        Leaves,     ///< a standing point ahead further outside than now (`at`): down a slope, past an edge
        WalkOff,    ///< the floor ends ahead (`at`, its last standing point), and a walk off it lands further outside -- `land` -- or on nothing
        Unjudged,   ///< `from` stands on no floor of the page, or kZoneWalkStaysIn is off: let be
    };
    bool                    ok = true;
    Why                     why = Why::Unjudged;
    MapPoint                at;
    std::optional<MapPoint> land;
    int                     out_px = 0;     ///< how far outside the box `at` (Leaves) or `land` (WalkOff) is
};
const char* zone_walk_name(ZoneWalk::Why w);     ///< for the log: "leaves the area along its floor", ...
/// Q1's rule for a walk from `from` -- a standing point -- `dir` (below 0 left,
/// else right): its own floor followed ahead_px (Page::follow, holes and
/// all), each standing point on it, and where a walk off its end would land
/// at each of kZoneWalkOffShares. Refused when one of them is further outside
/// the box (zone_outside_px) than `from` is: inside, every one in the box --
/// a fall onto a floor in it allowed (kZoneFallInsideOk); outside, a step that
/// comes no further out is always allowed, so the way in is never barred.
/// From 987,683 on t52_s2's ground, the owner's box ending at 694: right, the
/// ground leaves it at x 1035 -- refused; left, it climbs inside -- allowed.
ZoneWalk walk_stays_in(const GameMapBook::Page& p, const HuntZone& z, MapPoint from, int dir,
                       int ahead_px = kZoneWalkAheadPx);

/// One floor's legs from a standing point on it: what the way back
/// (return_point) and the seek (hunt_seek.hpp's seek_map) both go on from a
/// floor they have reached (2026.11.05b, one rule for both).
struct ZoneLegs {
    MapTrail left, right;               ///< the floor followed each way to its ends (Page::follow)
    /// A walk off an end, its fall landing in the box at every
    /// kZoneWalkOffShares: the end's floor top, which way, and where a walk's
    /// full speed lands it -- a standing point.
    struct Fall {
        MapPoint end;
        int      dir = 0;
        MapPoint land;
    };
    std::vector<Fall>     falls;
    std::vector<JumpPlan> jumps;        ///< with jump rules: jump_legs_from's
};
/// The falls off the ledge ends of `left` and `right` -- a floor followed from
/// one point each way -- each landing in the box at every kZoneWalkOffShares:
/// left's first.
std::vector<ZoneLegs::Fall> zone_falls(const GameMapBook::Page& p, const HuntZone& z, const MapTrail& left,
                                       const MapTrail& right);
/// The legs from `at`: its floor followed both ways, zone_falls off its ends,
/// and -- with `up` -- the jumps up jump_legs_from finds by those rules.
ZoneLegs zone_legs_from(const GameMapBook::Page& p, const HuntZone& z, MapPoint at, const JumpRules* up = nullptr);

/// The way back into a zone from where the character stands (return_point).
struct ZoneReturn {
    MapPoint at;                ///< where it ends: a standing point (top - 1) in the box -- the home when the way gets there
    int      floor_y = 0;       ///< that floor's top
    bool     home = false;      ///< `at` is the home
    int      falls = 0;         ///< walk-offs on the way, each landing in the box
    int      jumps = 0;         ///< jumps straight up on the way, each landing in the box
    /// Where to walk to now: at.x, when walking along the floor the
    /// character stands on gets there; else kZoneWalkOffPastPx past that
    /// floor's end the way goes off it, or the take-off of the jump it starts
    /// with (`jump`) -- after the landing, the way is asked for again.
    int      walk_x = 0;
    /// The jump the way starts with, when it does: walked to (walk_x), let
    /// go of, and pressed there.
    std::optional<JumpPlan> jump;
    /// Nothing in the box is reached by walking and falling, and the home is
    /// no more than Hunter::kOneJumpPx above: walked toward as every walk back
    /// was before 04d, hops and all. Never so on a way the map shows: those
    /// hop never -- a hop off the floor walked along leaves the way. Never so
    /// when jumps may go: those are planned, or there is no way.
    bool     as_before = false;

    /// Walking alone gets there: never a hop, never a jump.
    bool own_floor() const { return falls == 0 && jumps == 0 && !as_before; }
};
/// The way back into the box from `from`: the floor it stands on followed both
/// ways to its ends (Page::follow), and off them -- each fall landing on a
/// floor inside the box, at every kZoneWalkOffShares -- on to the floors they
/// land on, kZoneReturnFallsMax at most; never up but by a slope -- or, with
/// `up`, by the jumps jump_legs_from finds by those rules, kZoneReturnJumpsMax
/// at most. Of the standing points in the box so reached: the home, by the
/// fewest jumps, then falls; else, by the fewest jumps, then falls, the one
/// nearest the home across, then up or down. With none, the home as_before
/// when no more than one jump above -- without `up` only; else none -- no way
/// back, and the character fights where it stands (Q2). Without `up`, exactly
/// the way of 04d.
/// From 1402,802 on t52_s2 -- the owner's box 265..1399 x 174..694, home
/// 880,274 on the spawn ledge -- the ground it stands on, at x 880: floor 635,
/// standing at 880,634, left of it; with jumps, the home itself: left along the
/// ground to x 810 or so, and Ctrl + ↑.
std::optional<ZoneReturn> return_point(const GameMapBook::Page& p, const HuntZone& z, MapPoint from,
                                       const JumpRules* up = nullptr);

/// The owner's Q2 of 04d ((a) until answered): further outside the box than
/// this, with a way back the map shows (not as_before), the character walks
/// back before it fights again -- monsters in reach outside the box let be.
/// With none, it fights where it stands. An edge a slope crosses would flip
/// it in and out at less: Hunter::kSpotKeepPx. The owner's answer of
/// 2026-11-10 ("Ngưng đánh, đi về trước"): once it holds, the walk back
/// first is latched until the character is inside the box -- past this edge
/// too -- and nothing is fought meanwhile, monsters in the box and in reach
/// among them (Hunter::track_back_walk); on 2026-11-10 a monster in the box
/// within the 450 px reach had every walk back cut short, and they started
/// only at the floor's two ends.
inline constexpr int kZoneOutsideBackPx = 60;
bool zone_walks_back_first(int outside_px, const std::optional<ZoneReturn>& way);

/// Whether a character standing at `at` is at `goal` -- the home, a way
/// back's point: within tol_px across, and on goal's own floor -- the floor
/// `at` stands on, followed to goal.x, has goal's floor's top there. Off every
/// floor (a fix in the air), or with no page, within Hunter::kSpotFloorPx up
/// or down instead. Never by x alone: 882,616 on t52_s2's ground is not at a
/// home at 880,274, 342 px above it.
bool zone_stands_at(const GameMapBook::Page* p, MapPoint goal, MapPoint at, int tol_px);
bool zone_at_home(const GameMapBook::Page* p, const HuntZone& z, MapPoint at, int tol_px);

// -------------------------------------------------------------- the file

/// A stem as spots.ini keeps it: lowercase, "t<digits>_s<digits>". Empty for
/// anything else -- never a section that is no map's.
std::string zone_stem(const std::string& stem);

/// configs\spots.ini: a section per map stem (lowercase), its seven keys home_x,
/// home_y, left, top, right, bottom and saved. Sections and keys it does not
/// know are kept on disk; a section that does not read as a zone is left out
/// here and kept there. Found whatever the case of its name.
class ZoneBook {
public:
    enum class Load : std::uint8_t {
        Ok,
        Missing,    ///< no file: an empty book
        Bad,        ///< a file -- or a folder -- that does not read: never written over
    };
    Load load(const std::string& path);

    std::optional<HuntZone> get(const std::string& stem) const;
    /// False, and nothing changed, for no stem (zone_stem).
    bool put(const std::string& stem, const HuntZone& z);
    void erase(const std::string& stem);
    std::vector<std::string> stems() const;
    /// Moves at each put or erase of that stem: what a zone sent to the
    /// hunter was, so an edit is sent again.
    std::uint64_t revision(const std::string& stem) const;
    std::size_t   size() const { return zones_.size(); }
    /// Sections the last load left out for not reading as a zone.
    std::size_t   skipped() const { return skipped_; }

    /// The file's first lines.
    static const char* header();
    /// The zones of `batch` written into the file at `path` -- a value put,
    /// none erased -- everything else in it kept: the file read afresh, every
    /// other section and key copied, a section whose name is one of them in
    /// another case merged into it, its keys GoBot does not know kept. Written
    /// to path.new, read back, and moved over the file. False, with nothing
    /// written, when the file is there and does not read, or a stem is none.
    static bool save_many(const std::string& path, const std::map<std::string, std::optional<HuntZone>>& batch);

private:
    std::map<std::string, HuntZone>      zones_;
    std::map<std::string, std::uint64_t> revs_;
    std::uint64_t                        rev_ = 0;
    std::size_t                          skipped_ = 0;
};

/// The saves owed to spots.ini: the latest value per stem, one batch out at a
/// time. A batch that failed is owed again, but for a stem given a newer
/// value meanwhile. The GUI thread's alone.
class ZoneSaveQueue {
public:
    using Batch = std::map<std::string, std::optional<HuntZone>>;

    void commit(const std::string& stem, const std::optional<HuntZone>& v);
    /// What is owed, now out -- none while a batch is out, or nothing is owed.
    std::optional<Batch> take();
    void done(const Batch& batch, bool ok);

    bool owed() const { return !owed_.empty(); }        ///< waiting to be taken
    bool out() const { return out_; }                   ///< a batch taken, not done

private:
    Batch owed_;
    bool  out_ = false;
};

} // namespace gobot
