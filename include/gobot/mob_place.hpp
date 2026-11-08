#pragma once

// Where a monster of the packets is placed on the game's own floors: one rule
// for the map tab's dots (2026.11.05), the hunter's plates and the monsters
// "Tự tìm quái trong vùng" goes to (2026.11.05b). Moved here from map_view,
// whose mob_dot forwards to it, so the tab and the hunter cannot drift apart.
//
// Between its frames the packets give a walking monster's x alone: the world
// follows it along its facing and keeps the height its last frame said
// (follow_mob), so on t52_s2's ramps its dot floated 10-170 px over the
// floor's line or sank under it, and jumped back at its next frame -- the
// owner's "lệch chỗ" of 2026-11-05. Placed here on its floor instead, from
// the game's own map file.
//
// And a walk the world has run out (PacketWorld::kMobWalkMs from its start,
// held where it got to) is not a monster standing: the game sends nothing
// more of a walk until it turns or stops, and in the hunt of 2026-11-05 23:03
// the ramp's monsters walked 1,400-1,900 px into the owner's box while the
// world held them at x 2594 for 20 s. For the seek they are followed on
// (Projected): along their frame's floor, in its facing, at its speed, held
// at the floor's end, for kPlaceWalkOnMaxMs since the frame at most.
//
// The seek's owner answers that live here, (a) until answered: a walk run out
// is followed on (kPlaceProjectWalks, Q4), and the hunter's plates stand
// where the monsters are placed when the map tab's file is gone by
// (kPlatesOnFloors, Q3) -- attack points are planned on floors, and a plate
// at a walk's stale height would read out of reach on arrival. The map tab's
// own dots keep 2026.11.05's rule: mob_dot places, never projects.

#include "gobot/map_book.hpp"
#include "gobot/packet_world.hpp"

#include <cstdint>
#include <optional>

namespace gobot {

/// Why a monster is placed where it is (mob_place, mob_dot, you_dot).
enum class DotWhy : std::uint8_t {
    AsIs,       ///< where the packets put it: on its floor already, no map file, or no floor to go by
    Followed,   ///< a walk: on the floor its frame stood on, followed to its x
    Held,       ///< ... that floor ends before its x: at its end, where the game turns a monster round
    SetDown,    ///< standing on no floor -- a spawn point, a knock: on its floor (Hunter::monster_floor)
    Hidden,     ///< dead: not drawn (kMapDeadShownMs)
    Projected,  ///< a walk run out, followed on along its floor (kPlaceProjectWalks): where it likely is
};

/// Where one monster is placed, in world pixels.
struct MapDot {
    double x = 0.0;
    double y = 0.0;
    DotWhy why = DotWhy::AsIs;
    /// The floor it is placed on: its top at x; -1 when none is known.
    int    top = -1;

    bool shown() const { return why != DotWhy::Hidden; }
};

/// A walk is followed along its floor this many columns from its frame at
/// most, and held where that got to: a monster walks 520 px at most (2.6 px a
/// step for 5 s), a blow's 7 sent as a walk 1,400 px. 64 dots ten times a
/// second stay cheap whatever a frame says.
inline constexpr int kDotFollowMaxPx = 2048;
/// A dot set down on its floor stands this far above the floor's top, as a
/// stop there puts a monster (0-3 px; most often 1).
inline constexpr int kDotStandPx = 1;

/// The owner's Q4 of the seek ((a) until answered): a walk run out is
/// followed on along its frame's floor. False is (b): held where the world
/// holds it, as 2026.11.05 has it.
inline constexpr bool kPlaceProjectWalks = true;
/// ... for this long since its frame at most, and held at the point that
/// gets to: 2,600 px at a monster's 2.6 px a step.
inline constexpr std::uint64_t kPlaceWalkOnMaxMs = 25000;
/// ... along this many columns at most: kPlaceWalkOnMaxMs at the fastest a
/// monster is taken to walk (PacketWorld::kPlausibleWalkSpeed).
inline constexpr int kPlaceProjectMaxPx = 4000;
/// The owner's Q3 of the seek ((a) until answered): the hunter's plates of
/// the packets' monsters stand where mob_place puts them while a map file is
/// gone by (PacketEyes::Inputs::floors). False is (b): the packets' own x
/// and y, as 2026.11.05 has them.
inline constexpr bool kPlatesOnFloors = true;

/// The floor a point at fx, fy stands on, followed to column to_x
/// (GameMapBook::Page::follow, at most max_px columns): at to_x -- or at the
/// column before its hole, as follow_to has it -- Followed; where the floor
/// ends, a wall or the map's edge comes first, or max_px, Held. The point's
/// height over the floor's top is kept. None when it stands on no floor, or
/// on a wall.
std::optional<MapDot> along_floor(const GameMapBook::Page& p, double fx, double fy, double to_x,
                                  int max_px = kDotFollowMaxPx);

/// Where monster `m` is placed at `now` on the map `page` -- null when no
/// map file is gone by: where the packets put it.
///   - Dead: AsIs, where the blow found it.
///   - A walk run out (`project`, kPlaceProjectWalks): not walking now, its x
///     moved from its frame's, facing one way at a monster's speed (above 0,
///     up to PacketWorld::kPlausibleWalkSpeed -- never a blow's): its
///     frame's floor followed from frame_x as far as its speed carries it in
///     the time since the frame, kPlaceWalkOnMaxMs at most -- Projected, at
///     that column or held at the floor's end; AsIs as below when its frame
///     stood on no floor.
///   - A walk, under way or run out -- its x moved from its frame's
///     (frame_x): the floor its frame stood on, followed column by column
///     across holes of 1-2 px (GameMapBook::Page::follow), to its x, at the
///     frame's height over that floor; Held where the floor ends first -- or
///     a wall, the map's edge, or kDotFollowMaxPx; AsIs when its frame stood
///     on no floor.
///   - Standing on no floor: SetDown on the floor the hunter takes for it --
///     the first at or below it within Hunter::kDropMonsterFloorPx
///     (Hunter::monster_floor) -- kDotStandPx above its top; AsIs with none.
MapDot mob_place(const GameMapBook::Page* page, const PacketWorld::Mob& m, std::uint64_t now, bool project);

} // namespace gobot
