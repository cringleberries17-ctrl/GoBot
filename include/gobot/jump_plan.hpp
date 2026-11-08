#pragma once

// Jumping straight up onto a floor above (2026.11.05b, "Nhảy lên bục trên",
// HunterConfig::jump_up): which key reaches which floor, by the game's own
// map file and a model of the jump with a margin either way, and where along
// the character's floor to take off from -- the jump-up plan's §3 and §5.
// Pure: no key, no clock, nothing learned kept here; what the hunter learned
// comes in through JumpRules::refused.
//
// The model, each number in one place. A jump goes straight up -- no arrow
// held, no steering in the air -- at the game's own 0x002A speeds: ↑ -20 a
// step, the second ↑ of ↑↑ setting it to -20 again (state 6), Ctrl + ↑ -36
// (state 2), 40 steps a second. Gravity is not known to better than a
// bracket: kJumpUpGravity, GoC's 2400 between the walk-off's 1.4 a step and
// 10 % more. ↑↑'s second key goes kJumpUpSecondAtMs after the first, late or
// early by the press's jitter and the game's lag. So each key's peak is a
// range (jump_peak), and a floor is landed on for sure only kJumpUpClearPx
// under the lowest peak, with none between that and kJumpUpClearPx over the
// highest. The owner's "Theo GoC" of 2026-11-05 -- ↑ ~133, ↑↑ ~280, Ctrl + ↑
// ~432 px -- are nominal peaks: within these ranges, or above them, and so
// only upper bounds.
//
// The owner's answers of 2026-11-05 that live here: the arc may pass above
// the box's top, the take-off, the landing and the monster inside it (Q3);
// Ctrl + ↑ on a gate does not change the map, so no gate band is kept
// (kJumpUpGateBand, Q2) -- the hunter's 10 s map-change watch after any jump
// press stays as a net. And the judges': no floor outside the box within
// kJumpUpCeilingPx above the take-off, whatever the model says -- the owner's
// "higher than 600 px needs Ctrl + ↑" would be a jump this model does not
// know, and it still lands inside.

#include "gobot/map_book.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace gobot {

struct HuntZone;

/// The three keys, cheapest first: ↑ (state 1), ↑↑ (state 1, then 6), Ctrl + ↑
/// (state 2). Never a sideways one.
enum class JumpKind : std::uint8_t { Up, UpUp, CtrlUp };
inline constexpr int kJumpKinds = 3;
/// For the log: "Up", "Up,Up", "Ctrl+Up".
const char* jump_kind_name(JumpKind k);
/// For the card and the map tab: "↑", "↑↑", "Ctrl+↑".
const char* jump_kind_key(JumpKind k);

/// Take-off speeds, px/s up: ↑ and the second ↑ of ↑↑ (0x002A -20 x 40), and
/// Ctrl + ↑ (-36 x 40).
inline constexpr double kJumpUpV = 800.0;
inline constexpr double kJumpUpVHigh = 1440.0;
/// Gravity, px/s², lowest to highest: the walk-off's 1.4 a step, GoC's 2400,
/// and 10 % over it.
inline constexpr double kJumpUpGravity[] = { 2240.0, 2400.0, 2640.0 };
/// ↑↑: the second key goes this long after the first key went down (what
/// InputManager::jump_twice is given), and is modelled at its push from this
/// much before it to this much after it: 0.28 to 0.45 s after the take-off.
/// Should a live ↑↑ show no state 6, this alone moves (to about 430) and the
/// model's window with it.
inline constexpr int kJumpUpSecondAtMs = 340;
inline constexpr int kJumpUpSecondEarlyMs = 60;
inline constexpr int kJumpUpSecondLateMs = 110;
/// A floor is landed on for sure this far under the lowest peak, and no
/// floor may lie between that and this far over the highest: the feet clear
/// it, or never reach it.
inline constexpr int kJumpUpClearPx = 12;
/// A floor that is not a platform (attr 1) this far over the highest peak, or
/// nearer, stops a jump: the head in it. Unknown attributes count as such.
inline constexpr int kJumpUpHeadPx = 64;
/// A floor no more than this above the feet is the take-off's own, or below.
inline constexpr int kJumpUpFeetPx = 3;
/// A take-off is judged at its column and this far either way, every
/// kJumpUpMarginStepPx: where the character stops is that far off at worst.
inline constexpr int kJumpUpMarginPx = 24;
inline constexpr int kJumpUpMarginStepPx = 8;
/// A run of take-off columns is used when this wide at least, and walked to
/// this far inside its ends at least.
inline constexpr int kJumpUpRunMinPx = 64;
inline constexpr int kJumpUpRunInsetPx = 32;
/// No floor outside the box this far above the feet or nearer, at any column
/// judged: the 650 px ceiling guard.
inline constexpr int kJumpUpCeilingPx = 650;
/// A take-off's cost: kJumpUpKindCostPx a key dearer than ↑, the walk to it,
/// and kJumpUpHeadroomWeight a pixel of headroom -- the lowest peak over the
/// rise -- short of kJumpUpHeadroomWantPx.
inline constexpr int kJumpUpHeadroomWantPx = 64;
inline constexpr int kJumpUpHeadroomWeight = 4;
inline constexpr int kJumpUpKindCostPx = 300;
/// Take-offs are looked for this far along the character's floor, either way.
inline constexpr int kJumpUpSearchPx = 600;
/// The owner's Q2 of 2026-11-05: Ctrl + ↑ standing on a gate does not change
/// the map -- no band round a gate. True brings back the judges' band: Ctrl +
/// ↑ refused within JumpRules::gate_px across and Hunter::kDropGateSpanPx up
/// or down of a gate, as Ctrl + ↓ is. ↑ and ↑↑ are never banded.
inline constexpr bool kJumpUpGateBand = false;

/// A key's peak above the feet, px: lowest (the highest gravity, ↑↑'s second
/// push at the worst end of its window) to highest -- whole pixels, the
/// lowest rounded down and the highest up.
struct JumpPeak {
    int lo = 0;
    int hi = 0;
    /// The highest floor top, above the take-off floor's, landed on for sure.
    int usable() const { return lo - kJumpUpClearPx; }
    /// The next floor above must be higher than this.
    int beyond() const { return hi + kJumpUpClearPx; }
};
/// ↑ 121..143, ↑↑ 213..286, Ctrl + ↑ 392..463: usable to 109, 201 and 380.
JumpPeak jump_peak(JumpKind k);

/// Why a jump is refused, the first that holds. A take-off column's
/// (judge_column) up to Solid; a monster's or a way's (plan_jump_to) the rest.
enum class JumpNo : std::uint8_t {
    None,
    OffFloor,       ///< the take-off stands on no floor of the page, or on a wall
    Blocked,        ///< a floor not a platform (attr != 1) in the way up
    Short,          ///< no floor reached for sure: too high, or none
    Ambiguous,      ///< a floor top in the key's uncertain band: lands or not
    Ceiling,        ///< a floor outside the box within kJumpUpCeilingPx above
    Edge,           ///< the landing's floor ends, or changes, within the margin
    Outside,        ///< the take-off, the landing or the monster outside the box
    Weaker,         ///< lost Ctrl or lost second ↑: the plain ↑ lands outside, or is blocked
    Learned,        ///< a jump of this key, or a weaker one, failed here this session
    Solid,          ///< the landing's floor turned out solid from below this session
    Gate,           ///< Ctrl + ↑ by a gate (kJumpUpGateBand only)
    Narrow,         ///< take-off columns that pass, but no run kJumpUpRunMinPx wide
    Far,            ///< lands on the monster's floor, but out of reach and the walk to it leaves the box
    NotTargetFloor, ///< lands, but on another floor than the monster's
};
/// For the log: "falls short", "a floor outside the area above", ...
const char* jump_no_name(JumpNo w);

/// What a jump is judged by beyond the map and the box.
struct JumpRules {
    /// The keys that may go: bit 1 << JumpKind. Ctrl + ↑'s is off when a
    /// walk or attack key is Ctrl.
    std::uint8_t kinds = 7;
    /// The gates (the page's and those learned), and the band round them --
    /// only with kJumpUpGateBand; 0 is none.
    std::span<const MapGate> gates;
    int gate_px = 0;
    /// The ceiling guard; 0 turns it off -- only a test does.
    int ceiling_px = kJumpUpCeilingPx;
    /// plan_jump_to: a monster this near across where the jump lands is
    /// fought from there, its floor walked or not.
    int reach_px = 0;
    /// JumpPlan::score's weight of the headroom short: a test pins the
    /// scoring with 0.
    int headroom_weight = kJumpUpHeadroomWeight;
    /// What the hunter learned: a take-off point where this key, or a weaker
    /// one, did not get up (Learned), a landing floor solid from below
    /// (Solid). None to let it go.
    std::function<JumpNo(MapPoint take, int land_top, JumpKind kind)> refused;

    bool allows(JumpKind k) const { return ((kinds >> static_cast<int>(k)) & 1u) != 0; }
};

/// One take-off column judged for one key.
struct JumpColumn {
    JumpNo why = JumpNo::None;
    int    take_top = -1;       ///< the take-off floor's top; -1 off every floor
    /// The landing floor: the column it was found in -- the take-off's, or
    /// the one before a hole of it there (Page::follow_to's) -- and its top
    /// there; -1 none.
    int    land_x = -1;
    int    land_top = -1;
    int    rise = 0;            ///< take_top - land_top
    int    headroom = 0;        ///< the lowest peak over the rise
    /// The floor that refused it (Blocked, Ceiling, Ambiguous, Weaker): its
    /// column and top; -1 none.
    int    bad_x = -1;
    int    bad_top = -1;

    bool ok() const { return why == JumpNo::None; }
};
/// The column rule (the plan's §3), for a take-off at `take` -- a standing
/// point; its floor's top is what rises are measured from:
///   1. it stands on a floor that is not a wall, inside the box -- or, coming
///      back into it, no more than `take_out_px` outside (zone_outside_px:
///      walk_stays_in's Closer);
///   2. at every column kJumpUpMarginPx either way, every kJumpUpMarginStepPx:
///      no floor not a platform up to the highest peak + kJumpUpHeadPx; no
///      floor outside the box up to rules.ceiling_px; no floor top in the
///      uncertain band (JumpPeak::usable .. beyond); a floor reached for sure
///      -- the highest no higher than usable() -- or a hole of it no wider
///      than kFollowHolePx; the neighbours' floors linked as Page::follow
///      links them;
///   3. the landing's standing point inside the box;
///   4. for ↑↑ and Ctrl + ↑, the plain ↑ a lost key leaves lands back or in
///      the box, unblocked;
///   5. not refused: a gate (Ctrl + ↑ with a band), then rules.refused.
JumpColumn judge_column(const GameMapBook::Page& p, const HuntZone& z, MapPoint take, JumpKind kind,
                        const JumpRules& rules, int take_out_px = 0);

/// A jump to take: the key, where to take off (x, feet_y) and onto what.
struct JumpPlan {
    JumpKind kind = JumpKind::Up;
    int      x = 0;             ///< the take-off column
    int      feet_y = 0;        ///< its standing point: the take-off floor's top - 1
    int      land_x = 0;        ///< the landing floor's column: x, or the one before a hole of it at x
    int      land_top = 0;      ///< its top there: stands at land_top - 1
    int      rise = 0;
    int      headroom = 0;
    int      run_l = 0;         ///< the run of take-off columns it is in
    int      run_r = 0;
    int      walk_px = 0;       ///< across, from where the character stands
    int      score = 0;         ///< kind x kJumpUpKindCostPx + walk_px + the headroom short, weighted

    MapPoint take() const { return { x, feet_y }; }
    /// Where it stands after: a standing point on the landing floor.
    MapPoint land() const { return { land_x, land_top - 1 }; }
};

/// A jump onto the floor of a monster above: `stand`, where the character
/// stands; `target`, the monster's floor's standing point at its column (its
/// top - 1, as the hunter's target_floor puts it). The character's own floor
/// followed kJumpUpSearchPx either way (Page::follow, holes and all) -- and,
/// for the runs' ends, kJumpUpRunInsetPx more -- no further than a walk stays
/// in the box (walk_stays_in); each column judged for each key rules allow;
/// the runs of those that land on the monster's floor -- followed to its
/// column, within Hunter::kDropSameFloorPx -- and from which it is in reach
/// across, or its floor in the box all the way to it; take-offs
/// kJumpUpRunInsetPx inside a run kJumpUpRunMinPx wide; the cheapest by
/// JumpPlan::score. None: `why_not` why -- the first of Outside (the monster
/// outside the box), Short (its floor above every usable rise) and the most
/// nearly kept of the columns' -- and `why_px` its floor's rise above the
/// character's at the column nearest it. Never more than one jump.
std::optional<JumpPlan> plan_jump_to(const GameMapBook::Page& p, const HuntZone& z, MapPoint stand, MapPoint target,
                                     const JumpRules& rules, JumpNo* why_not = nullptr, int* why_px = nullptr);

/// The jumps from where the character stands -- its floor looked along as for
/// plan_jump_to, coming back into the box by walk_stays_in's Closer -- onto
/// any floor in the box: the cheapest per landing floor (known by its two
/// ends, Page::follow), cheapest first. A way back's legs (return_point).
std::vector<JumpPlan> jump_legs_from(const GameMapBook::Page& p, const HuntZone& z, MapPoint stand,
                                     const JumpRules& rules);

/// A run of take-off columns for one key over every floor in the box: what
/// the tests check against the prototypes, and what the map tab draws.
struct JumpRun {
    JumpKind kind = JumpKind::Up;
    int      x0 = 0, x1 = 0;            ///< its first and last passing column
    int      take_y0 = 0, take_y1 = 0;  ///< the take-off floor's top at each
    int      land_y0 = 0, land_y1 = 0;  ///< the landing floor's top at each
    int      head_lo = 0, head_hi = 0;  ///< its least and most headroom

    int  width() const { return x1 - x0 + 1; }
    bool usable() const { return width() >= kJumpUpRunMinPx; }
    int  take_l() const { return x0 + kJumpUpRunInsetPx; }  ///< the take-offs walked to: take_l .. take_r
    int  take_r() const { return x1 - kJumpUpRunInsetPx; }
};
/// Every floor in the box (not a wall, its standing point inside), every
/// column, each key rules allow, judged; the passing points grouped as the
/// prototype groups them: a column at most kFollowHolePx + 1 on, the take-off
/// and landing tops each within kFollowLinkPx a column. Narrow ones too:
/// JumpRun::usable says. By key, then x.
std::vector<JumpRun> jump_runs(const GameMapBook::Page& p, const HuntZone& z, const JumpRules& rules);

/// Gravity from a flight: up from y0 at vy0 px/s (below 0 is up), down at y1
/// t_s seconds later -- 2((y1 - y0) - vy0 t) / t². None for no time.
std::optional<double> gravity_from(double y0, double vy0, double y1, double t_s);

/// The nearest floor of the page above a standing point -- any floor, a wall
/// or a platform, at its column and kJumpUpMarginPx either way -- more than
/// kJumpUpFeetPx over the character's own floor there (followed, slopes and
/// holes as a walk goes; its top at the stand's column past its ends) and
/// over the feet, and no more than `within_px` over the feet: how far, px.
/// None for none, or the stand on no floor. What "Thử" of a jump key's kind
/// goes by (JumpCheck::run_kind): a test jump straight up under a floor may
/// land on it.
std::optional<int> floor_above(const GameMapBook::Page& p, MapPoint stand, int within_px = kJumpUpCeilingPx);

} // namespace gobot
