#pragma once

// "Tự tìm quái trong vùng" (2026.11.05b, HunterConfig::seek_in_zone): the
// hunter goes to the monsters anywhere inside the map tab's area that it can
// get to -- along its floor, off a floor's end onto one below inside the box,
// or by a jump up -- and fights each from a standing point on a floor inside
// the box. The owner, after the hunt of 2026-11-05 23:02 on 05: "đang chưa tự
// tìm quái ở trong ô đánh quái. Nên có thể tự tìm quái và di chuyển tới
// đánh". In 05 the hunter went for nothing it had not picked from its view,
// walked back to the home's floor point when nothing was left, walked toward
// a monster's x down t52_s2's slope into the box's bottom edge, and never
// climbed back to the ledge.
//
// Pure (the seek design's §3): the floors of the box the character gets to
// from where it stands, each column's cost (seek_map), and for one monster
// the cheapest standing point it is in reach from (seek_attack) -- the walk
// to it never leaving the box, as walk_stays_in judges a walk. The hunter's
// side, what it goes for and how, is hunter_seek.cpp.
//
// The owner's questions of the seek, (a) until answered, each in one place:
// what it does with nothing it can get to (kSeekIdleHome, Q1), the order it
// goes by (kSeekTiered, Q2), monsters just outside the box (kSeekOutsideHit,
// Q5) and another player's (kSeekOthersHurtMs, Q6). Q3 and Q4 are
// mob_place.hpp's; Q7 is HunterConfig::seek_in_zone, on by default.

#include "gobot/hunt_zone.hpp"
#include "gobot/jump_plan.hpp"
#include "gobot/map_book.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gobot {

/// A fall off a floor's end costs this much more than walking its width, a
/// jump up this much: a walk on the character's own floor first, cost for
/// cost.
inline constexpr int kSeekFallCostPx = 200;
inline constexpr int kSeekJumpCostPx = 600;
/// An attack point stands this far at least inside where its floor leaves
/// the box, or a walk off its end lands outside it: Hunter::kZoneWalkSlackPx,
/// the look-ahead a walk toward a target is judged by (walk_kept_in), so a
/// walk to an attack point is never refused.
inline constexpr int kSeekEdgeInsetPx = 48;
/// In reach of an attack point by this much more across, and this much more
/// up or down, than the hunter's own reach: where the character stops, and
/// where the packets put it, are a few pixels off.
inline constexpr int kSeekReachMarginPx = 24;
inline constexpr int kSeekBandMarginPx = 8;
/// The monster gone for is changed for another at most once in
/// kSeekCommitMs, and only for one kSeekSwitchPx cheaper to get to -- but at
/// once for one in reach, or in a better tier.
inline constexpr int           kSeekSwitchPx = 200;
inline constexpr std::uint64_t kSeekCommitMs = 2000;
/// Getting there: the cost to its attack point kSeekProgressPx less within
/// kSeekStuckMs of walking, or it is passed over -- Stuck.
inline constexpr int           kSeekProgressPx = 40;
inline constexpr std::uint64_t kSeekStuckMs = 4000;
/// At the attack point -- within kSeekArrivePx across -- with its plate not
/// in reach for kSeekArriveMs: not there -- NotFound, passed over.
inline constexpr int           kSeekArrivePx = 24;
inline constexpr std::uint64_t kSeekArriveMs = 1500;
/// The turn to a monster walks the character this far toward it at most
/// (Hunter::kTurnTapMs, about 21 px at a walk): an attack point has its
/// monster in reach from there too, with the margins -- on t52_s2's slope 21
/// px across is 10 px of height, more than kSeekBandMarginPx. A walk to an
/// attack point away from its monster is not stopped for it in reach short
/// of the point -- within kSeekAimPx of it, or past it.
inline constexpr int           kSeekTurnPx = 24;
inline constexpr int           kSeekAimPx = 4;
/// A monster passed over by the seek -- Stuck, NotFound -- is not gone for
/// again for this long.
inline constexpr std::uint64_t kSeekPassOverMs = 20000;
/// The owner's Q6 ((a) until answered): a monster another player hurt within
/// this is not gone for -- in reach it is fought as ever.
inline constexpr std::uint64_t kSeekOthersHurtMs = 10000;
/// The floors and the attack points worked out again at most this often,
/// and whenever a fall or a jump lands.
inline constexpr std::uint64_t kSeekPlanEveryMs = 250;
/// The log: every monster where it is placed, every kSeekSnapshotMs while
/// the seek is on, kSeekSnapshotMax of them named; "nothing to seek" again
/// every kSeekSaidMs while it lasts.
inline constexpr std::uint64_t kSeekSnapshotMs = 30000;
inline constexpr std::size_t   kSeekSnapshotMax = 12;
inline constexpr std::uint64_t kSeekSaidMs = 10000;
/// The floors worked out from one standing point, at most.
inline constexpr std::size_t   kSeekPiecesMax = 24;

/// The owner's Q2 ((a) until answered): monsters a walk on the character's
/// own floor gets to first, then those below (falls), then those a jump up
/// gets to -- cost for cost within each. False is (b): cost alone.
inline constexpr bool kSeekTiered = true;
/// The owner's Q1 ((a) until answered): nothing in the box to go to, the
/// character stands where it is, inside the box; outside it, it walks back
/// (the owner's Q2 of 04d). True is (b): back to the home, as 05.
inline constexpr bool kSeekIdleHome = false;
/// The owner's Q5 ((a) until answered): a monster outside the box is fought
/// only when it is in reach already, never gone to. True is (b): gone to,
/// from a standing point inside the box.
inline constexpr bool kSeekOutsideHit = false;

/// One monster of the whole map as the seek has it (PacketEyes::world): where
/// it is placed on its floor (mob_place, projected), which it is, and what
/// the plates say of it.
struct SeekMob {
    std::int32_t  slot = -1;
    std::uint32_t gen = 0;
    MapPoint      at;                   ///< its placed standing point, world pixels
    int           floor_y = -1;         ///< the floor it is placed on: its top there; -1 none
    std::uint8_t  why = 0;              ///< DotWhy, as mob_place placed it
    std::uint64_t frame_age_ms = 0;     ///< since its last frame
    float         hp = 1.0f;            ///< as make_plates lays it on the bar
    bool          hurt_by_other = false;///< another player hurt it within kSeekOthersHurtMs (your id proven)
};

/// What the planner goes by, in world pixels.
struct SeekRules {
    int reach = 0;                      ///< HunterConfig::reach x packet_scale
    int up = 0, down = 0;               ///< up(), down() x packet_scale
    int dy_px = 0;                      ///< packet_dy x packet_scale: on a monster's height against yours, as the plates
    int reach_margin = kSeekReachMarginPx;
    int band_margin = kSeekBandMarginPx;
    int turn_px = kSeekTurnPx;          ///< in reach from this far toward the monster too; 0 not judged
    int falls_max = kZoneReturnFallsMax;
    int jumps_max = kZoneReturnJumpsMax;
    /// The jumps up the floors are worked out with (jump_legs_from) -- none,
    /// no jump at all -- and whether they may be taken: not, a monster only
    /// a jump gets to is NeedsJumpOff, its plan said for the log alone.
    const JumpRules* jumps = nullptr;
    bool             jumps_go = true;
};

/// One leg of a way to an attack point.
struct SeekLeg {
    enum class Kind : std::uint8_t { Walk, Fall, Jump };
    Kind     kind = Kind::Walk;
    MapPoint from;                      ///< a standing point: where it begins
    MapPoint to;                        ///< ... and where it ends
    /// Where to walk to now: the attack point (Walk), kZoneWalkOffPastPx past
    /// the floor's end (Fall), the take-off (Jump).
    int      walk_x = 0;
    std::optional<JumpPlan> jump;
};
/// "W", "F", "J".
const char* seek_leg_letter(SeekLeg::Kind k);

/// One floor of the box got to, by one way: its columns walked to without
/// leaving the box (walk_stays_in's rule from where it is entered), those an
/// attack point may stand on, and the way there.
struct SeekPiece {
    int floor_l = 0, floor_r = 0;       ///< its first and last columns
    int use_l = -1, use_r = -1;         ///< its first and last usable columns; -1 none
    std::vector<MapPoint> pts;          ///< each column's floor top, x order (a hole of 1-2 px has none)
    /// Parallel to pts: an attack point may stand there -- inside the box,
    /// and a walk kSeekEdgeInsetPx on either way stays in it (walk_stays_in).
    std::vector<char> usable;
    int entry_x = 0;                    ///< where it is entered: the stand, a landing
    int entry_cost = 0;
    int falls = 0, jumps = 0;
    std::vector<SeekLeg> legs;          ///< from where the character stands, first first; none for its own floor

    /// The cost to column x: the entry's, and the walk along the floor.
    int cost_at(int x) const { return entry_cost + (x > entry_x ? x - entry_x : entry_x - x); }
};

/// The box's floors from one standing point, cheapest first.
struct SeekMap {
    MapPoint from;
    bool     jumps = false;             ///< worked out with jumps up
    std::vector<SeekPiece> pieces;
};

/// How a monster is got to: a walk on the character's own floor, falls
/// only, or a jump up on the way.
enum class SeekTier : std::uint8_t { OwnFloor, Below, Above };
/// Why a monster is not gone for.
enum class SeekNo : std::uint8_t {
    None,
    Outside,        ///< its point outside the box (kSeekOutsideHit)
    NoFloor,        ///< on no floor of the map: a phantom, a knock in mid-air
    NoAttackPoint,  ///< no standing point the character gets to has it in reach
    NeedsJumpOff,   ///< only a jump up gets there, and jumps up are off
    Passed,         ///< passed over a while (the hunter's: Stuck, NotFound, a give-up)
    Others,         ///< another player hurt it within kSeekOthersHurtMs
    /// Only a jump up gets there, and its jumps up are used up: the hunter's
    /// Hunter::kJumpUpsPerTarget presses, or kJumpUpSettlesPerTarget settles,
    /// for it (2026.11.05b).
    JumpsUsed,
};
const char* seek_no_name(SeekNo w);         ///< for the log: "no place inside the area to hit it from", ...
const char* seek_tier_name(SeekTier t);     ///< for the log: "your floor", "below", "above"

/// A monster's attack point and the way there.
struct SeekPlan {
    MapPoint attack;                    ///< the standing point it is hit from
    SeekTier tier = SeekTier::OwnFloor;
    int      cost = 0;
    int      piece = -1;                ///< SeekMap::pieces' index
    std::vector<SeekLeg> legs;          ///< the falls and jumps before the last walk
    SeekLeg  first;                     ///< what to do now: legs' first, or the walk to `attack`
};

/// The box's floors the character gets to from `from` -- a standing point
/// (its floor's top - 1) -- cheapest first (a Dijkstra over floors): its own
/// floor followed both ways, cut where a walk would come further out of the
/// box than `from` is (walk_stays_in); off its ends where every
/// kZoneWalkOffShares lands in the box (zone_falls, falls_max at most); and,
/// with rules.jumps, the jumps up jump_legs_from finds from a usable column
/// (jumps_max at most). The same floor may be had by two ways, each kept
/// that is cheaper somewhere along it. Usable columns: inside the box, and
/// walk_stays_in with kSeekEdgeInsetPx either way.
/// On t52_s2's owner box from 880,634: one floor, usable 313..986; with jumps,
/// the ledge by Ctrl + ↑ at about 802 and the ramp off the ledge's right end.
SeekMap seek_map(const GameMapBook::Page& p, const HuntZone& z, MapPoint from, const SeekRules& rules);

/// The cheapest standing point of `m` from which `mob` is in reach -- |dx|
/// within reach - reach_margin, its height against it (with dy_px) within
/// up - band_margin above and down - band_margin below -- and from the column
/// of its floor turn_px toward the monster as well (the turn's walk; a floor
/// ending first is no attack point), ranked by tier, then cost
/// (kSeekTiered). None, `why` the reason: Outside, NoFloor,
/// Others, NoAttackPoint; NeedsJumpOff when only a way with a jump up has
/// one and rules.jumps_go is false -- that plan in `would`, for the log.
std::optional<SeekPlan> seek_attack(const SeekMap& m, const HuntZone& z, const SeekMob& mob, const SeekRules& rules,
                                    SeekNo* why = nullptr, std::optional<SeekPlan>* would = nullptr);

} // namespace gobot
