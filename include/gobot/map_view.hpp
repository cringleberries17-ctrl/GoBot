#pragma once

// Which map the "Bản đồ" tab shows, and the job its worker runs (2026.11.04c).
//
// The hunter's MapTrust runs only with drop_down on, so the tab keeps its own
// on its worker: a map entry (0x0086) seen since the capture began names the
// map; with none, the standing points do (MapTrust's naming), the file trusted
// or with no gate. A zone is never applied on another map's pixels: an
// identity carries the map entries it was had at (WorldFix::epoch) and the
// log-in count it was had under (trust_gen), and is gone by only while both
// are still the live ones (usable_identity).
//
// The job reads the files -- a map's page, its sketch, tb_Map's names -- and
// is all here, pure but for GameMapBook's reads, so it is tested on made-up
// map files: the GUI thread never reads a file for the tab.

#include "gobot/hunt_zone.hpp"
#include "gobot/hunter.hpp"
#include "gobot/jump_plan.hpp"
#include "gobot/map_book.hpp"
#include "gobot/map_sketch.hpp"
#include "gobot/map_table.hpp"
#include "gobot/mob_place.hpp"
#include "gobot/packet_eyes.hpp"
#include "gobot/packet_world.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace gobot {

/// Which map the character is on, as the tab knows it. (Not the minimap's
/// MapIdentity of map_mapper.hpp, a fingerprint of its title.)
struct KnownMap {
    enum class By : std::uint8_t {
        None,
        Entry,      ///< a map entry since the capture began: its numbers' file
        Named,      ///< no entry: MapTrust named it from standing points, trusted or with no gate
    };
    By            by = By::None;
    std::string   stem;             ///< lowercase "t52_s2"; "" with None
    std::uint32_t epoch = 0;        ///< WorldFix::epoch it was had at
    std::uint64_t trust_gen = 0;    ///< the GUI's log-in count it was had under
    /// An entry's map whose file MapTrust found off (Mismatch, Distrusted):
    /// the map is known, its floors are not gone by.
    bool          file_doubtful = false;
    MapTrust::Report report;        ///< the tab's MapTrust, for the status line
};

/// The rules of the plan's §3.3: Entry when the fix has a map entry; else
/// Named when MapTrust named the map and it is Trusted or has NoGates -- never
/// a named file that is doubtful; else None. None while the capture says
/// nothing (NoCapture): a door may be gone through unseen.
KnownMap map_identity(const WorldFix& f, const MapTrust::Report& r, std::uint64_t trust_gen);

/// `id` while it is still to be gone by; None when it was had under another
/// log in (trust_gen), is an entry of the epoch the log in left (lose_epoch,
/// while lose_epoch_on), or another map entry has come since (live_epoch, the
/// GUI's own fix's).
KnownMap usable_identity(const KnownMap& id, std::uint64_t trust_gen_now, bool lose_epoch_on,
                         std::uint32_t lose_epoch, std::uint32_t live_epoch);

/// A log in this long after the keeper's crossing began is that crossing's.
inline constexpr std::uint64_t kCrossKeepMs = 5 * 60 * 1000;
/// The map entry a log in leaves behind: the one the character was in as the
/// keeper's crossing began -- entries counted during the crossing are the new
/// game's -- when one began within kCrossKeepMs (cross_ms not 0); otherwise
/// the place's now.
std::uint32_t lose_epoch_for(std::uint64_t now, std::uint64_t cross_ms, std::uint32_t cross_epoch,
                             std::uint32_t place_epoch);

/// What the job keeps from one run to the next: the worker's alone.
struct MapViewState {
    MapTrust      view_trust{ nullptr, "Map view" };
    GameMapBook*  book = nullptr;           ///< the book view_trust was given
    std::uint64_t lost_gen = 0;             ///< the last trust_gen lose() was called for
    bool          ran = false;
    std::uint64_t last_resets = 0;          ///< PacketWorld::Stats::resets at the last run
    std::uint64_t last_job_ms = 0;
    /// The map read last, and its sketch: built once per map.
    std::string                              map_stem;
    std::shared_ptr<const GameMapBook::Page> page;
    std::shared_ptr<const MapSketch>         sketch;
    std::size_t                              sketches = 0;      ///< built so far
    std::shared_ptr<const std::vector<MapRun>> no_drop;
    std::string                              no_drop_stem;
    std::size_t                              no_drop_gates = 0;
    int                                      no_drop_px = -1;
    std::set<std::string>                    said;              ///< stems whose read was logged
    bool                                     said_no_data = false;
    /// Where tb_Map.tbl is: asked once, on the worker. Unset: no names.
    std::function<std::string()> table_path;
    bool                         table_tried = false;
    MapTable                     table;
    std::string                  name_stem;
    std::string                  name;
};

/// One run's result, for the GUI to copy under its lock.
struct MapViewResult {
    KnownMap                                   id;
    WorldFix                                   fix;     ///< the job's own
    std::shared_ptr<const GameMapBook::Page>   page;    ///< the identity's map's file; null without
    std::shared_ptr<const MapSketch>           sketch;
    std::shared_ptr<const std::vector<MapRun>> no_drop; ///< no_drop_marks at no_drop_px
    int                                        no_drop_px = 0;
    std::vector<MapGate>                       gates;   ///< the page's, then those learned (kind 0)
    std::size_t                                learned_gates = 0;
    /// What jumps up learned of the map this session (2026.11.05b): the
    /// points a key did not get up from (GameMapBook::no_jumps), the floors
    /// solid from below (solid_floors), and those floors as runs to draw
    /// (solid_marks).
    std::vector<GameMapBook::NoJump>           no_jumps;
    std::vector<MapPoint>                      solid;
    std::vector<MapRun>                        solid_runs;
    std::string                                map_name;    ///< tb_Map's; "" for none
    bool                                       data_dir_ok = true;
    bool                                       capture_gap = false;  ///< this run named afresh for a gap
};

struct MapViewJob {
    /// More than this between runs, or the capture started again: the tab's
    /// MapTrust lets go, as at a capture lost -- the map may have changed.
    static constexpr std::uint64_t kGapMs = 2000;
    /// Real time spent reading files to name a map, a run.
    static constexpr std::uint32_t kWorkMs = 20;

    /// One run, on the map tab's worker:
    ///   1. lose() when trust_gen moved on since the last;
    ///   2. a look with no capture first, after a capture reset (Stats::resets)
    ///      or more than kGapMs since the last run;
    ///   3. the fix (world_fix) and the look -- standing monsters by
    ///      packet_stands -- to the tab's MapTrust;
    ///   4. the identity (map_identity);
    ///   5. the map's page and sketch, read and built when the stem changes;
    ///   6. its gates, the page's and those learned -- and what jumps up
    ///      learned of it, the floors solid from below as runs (solid_marks);
    ///   7. the no-drop marks, built again when the stem, the gates' count or
    ///      drop_gate_px changes;
    ///   8. its name from tb_Map.
    static MapViewResult run(const PacketWorld::View& view, const PacketWorld::Stats& stats, const PacketLink& link,
                             std::uint64_t now, std::uint64_t trust_gen, int drop_gate_px, GameMapBook& book,
                             MapViewState& ws);
};

// ------------------------------------------------------- the tab's rules
//
// What the GUI thread decides about the tab at each frame or pass, kept here
// so that it is tested: what the status line and the canvas say, whether a
// zone may be edited, whether a saved zone goes to the hunter, and what the
// card's line under the zone says. The words are the GUI's (simple_map.cpp).

/// What the tab says of the map, and draws (the plan's §1.3): the first of
/// these that holds.
enum class MapShows : std::uint8_t {
    PositionsOff,       ///< packet_positions off: nothing to place anything by
    NoCapture,          ///< no Npcap, no capture, or no game traffic
    Changing,           ///< a map entry the job has not looked at yet: positions only, no edits
    Naming,             ///< not known: MapTrust naming, or confirming an entry's file
    Ambiguous,          ///< not known: another file fits as well
    NamedDoubtful,      ///< not known: the file named was found off, and is not used
    NothingYet,         ///< not known, nothing under way
    NoGame,             ///< no game data folder: positions only
    NoFile,             ///< known, with no .map for it
    EntryDoubtful,      ///< known by an entry, its file found off: drawn faded
    NoGates,            ///< known, its .prj with no gate
    ByEntry,            ///< known by an entry since the capture began
    ByName,             ///< known from standing points
};
struct MapShowsIn {
    bool            positions_off = false;
    WorldFix::State state = WorldFix::State::NoCapture;     ///< the GUI's own fix's
    std::uint32_t   live_epoch = 0;                         ///< ... and its map entries
    bool            ran = false;            ///< a job has run since the tab began
    KnownMap        id;                     ///< the last job's
    KnownMap        usable;                 ///< usable_identity of it
    bool            page = false;           ///< the job had the map's file
    bool            data_dir_ok = true;
};
MapShows map_shows(const MapShowsIn& in);
/// Whether the map's floors are drawn: known, with its file.
bool map_drawn(MapShows s);

/// Whether a zone may be edited on the tab now (the plan's §4.4).
enum class MapEdit : std::uint8_t {
    Ok,
    BookBad,            ///< spots.ini is there and does not read: never written over
    BookLoading,        ///< spots.ini not read yet: an edit now would be lost
    Changing,           ///< a map entry the job has not looked at yet
    MapUnknown,         ///< no usable identity; or the positions off, or no capture: the map last known may be gone by
    NoGame,             ///< no game data folder: no map file to set one on
};
MapEdit map_edit(bool book_loaded, bool book_bad, const KnownMap& usable, MapShows shows);

/// Whether a zone may be put where the character stands (`at`, its settled
/// report): on a map named from standing points only as zone_send would send
/// it -- none of yours off its floors, and `at` on them.
bool zone_here_ok(const KnownMap& usable, const GameMapBook::Page* page, MapPoint at);
/// Whether this run's spot -- where the character stood -- may be kept as the
/// map's zone: on a map named from standing points, none of yours off its
/// floors.
bool spot_keep_ok(const KnownMap& usable);

/// Whether the saved zone `z` of the map `usable` names is sent to the hunter
/// (the plan's §4.8), but for the hunt and the ticks: it fits the map's file
/// -- with a page not doubtful, zone_valid against it; otherwise its size and
/// the map's -- and, for a map named from standing points, nothing of yours is
/// off its floors and the last settled report of this map entry (`settled`,
/// at `settled_epoch`) is on them.
enum class ZoneSend : std::uint8_t {
    Send,
    NoMap,              ///< no usable identity
    DoesNotFit,         ///< not on the map's floors, or off the map
    NotSure,            ///< named: no settled report of this entry on its floors yet, or one of yours off them
};
ZoneSend zone_send(const KnownMap& usable, const HuntZone& z, const GameMapBook::Page* page, bool have_settled,
                   std::uint32_t settled_epoch, MapPoint settled);

/// The card's line about the zone (the plan's §4.9): the first that holds.
enum class ZoneCard : std::uint8_t {
    HuntOff,            ///< Tự đánh quái not ticked
    BookBad,
    BookLoading,
    MapUnknown,         ///< hunting with the spot, zones saved, the map not known: the spot where it stands
    NoMap,              ///< otherwise with the map not known
    Empty,              ///< no zone for this map and no spot
    SessionSpot,        ///< no zone for this map: this run's spot
    SpotOff,            ///< a zone, "Chỗ đánh quái" not ticked
    NotHunting,         ///< a zone, ticked, no hunt
    InUse,              ///< the hunter's spot is this zone
    /// ... the character outside its box (Status::spot_outside_px) and the
    /// hunter giving no reason of its own (SpotWhy::None): never the green
    /// "in use" -- on 2026-11-04 it stood 522 px off under it (2026.11.04d).
    OutsideArea,
    Suspended,          ///< ... set aside: a door, a log in, the map let go of
    Outside,            ///< sent, waiting: the character outside it
    Waiting,            ///< sent, waiting for the character to stand settled; or held back (NotSure)
    DoesNotFit,         ///< not sent: off the map's floors
    Other,              ///< the hunter's own line (simple_spot_state): sent and not taken yet, or another map's spot
};
struct ZoneCardIn {
    bool        hunt_enabled = false;
    bool        spot = false;           ///< "Chỗ đánh quái" ticked
    bool        hunting = false;
    bool        book_loaded = false;
    bool        book_bad = false;
    bool        book_any = false;       ///< a zone saved for any map
    std::string stem;                   ///< the map the tab knows the character is on: "" none
    bool        zone = false;           ///< a zone saved for it
    ZoneSend    send = ZoneSend::NoMap; ///< ... and whether it goes to the hunter
};
ZoneCard zone_card(const ZoneCardIn& in, const Hunter::Status& hs);

// ------------------------------------------------- where a dot is drawn
//
// Between its frames the packets give a walking monster's x alone: the world
// follows it along its facing and keeps the height its last frame said
// (follow_mob), so on t52_s2's ramps its dot floated 10-170 px over the
// floor's line or sank under it, and jumped back at its next frame -- the
// owner's "lệch chỗ" of 2026-11-05. Your own dot did the same between your
// reports. The tab draws the dots on their floors instead, from the game's
// own map file. The grey the tab gives a dot the hunter will not go to keeps
// the world's own heights (the owner's Q2 (a) of 2026.11.05). The placing
// itself -- DotWhy, MapDot, along_floor, kDotFollowMaxPx, kDotStandPx -- is
// mob_place.hpp's since 2026.11.05b, shared with the hunter's plates and the
// seek; mob_dot forwards to it and never projects a walk.

/// The owner's Q1 of 2026.11.05 ((a) until answered): how long a dead monster
/// stays on the tab after its kill. 0 hides it at once, as GoC's list has only
/// the monsters with HP above 0; (b) is PacketEyes::kDeadShownMs, the 1.5 s it
/// stayed faded where the killing blow found it until 04d, while the game
/// shows the body thrown back. The hunter's own kDeadShownMs is not this.
inline constexpr std::uint64_t kMapDeadShownMs = 0;

/// Where the tab draws monster `m` at `now` on the map `page` -- null when no
/// map is drawn, or its file is found off (dot_floors): where the packets put
/// it. mob_place's rule, with no walk projected (2026.11.05's own):
///   - Dead: Hidden from kMapDeadShownMs after its kill; until then where the
///     blow found it.
///   - A walk, under way or run out -- its x moved from its frame's (frame_x):
///     the floor its frame stood on, followed column by column across holes
///     of 1-2 px (GameMapBook::Page::follow), to its x, at the frame's height
///     over that floor; Held where the floor ends first -- or a wall, the
///     map's edge, or kDotFollowMaxPx; AsIs when its frame stood on no floor.
///   - Standing on no floor: SetDown on the floor the hunter takes for it --
///     the first at or below it within Hunter::kDropMonsterFloorPx
///     (Hunter::monster_floor) -- kDotStandPx above its top; AsIs with none.
MapDot mob_dot(const GameMapBook::Page* page, const PacketWorld::Mob& m, std::uint64_t now);

/// Where the tab draws you, at `x`, `y` -- the fix's own, or where you were
/// last known while Lost: walking on the ground with the fix Known, your floor
/// from your last report (WorldFix::rx, ry) followed to x as a monster's walk
/// is, Held where it ends first; in the air, lost, standing still, or with no
/// page, at x, y.
MapDot you_dot(const GameMapBook::Page* page, const WorldFix& fix, double x, double y);

/// Whether the tab draws the monsters -- their dots, the ring on the one
/// being fought, their hover, the grid's box round them: not while nothing
/// places anything (PositionsOff, NoCapture). The world keeps its last
/// monsters until a capture starts again, a walker going on for 5 s, so until
/// 04d they stayed drawn under "Chưa có gói tin của game", the grid's camera
/// fitted to them.
bool map_shows_mobs(MapShows s);

/// Every monster's dot in `v`, in v.mobs' order (mob_dot): none at all while
/// the tab does not show them (map_shows_mobs).
std::vector<MapDot> mob_dots(const GameMapBook::Page* page, const PacketWorld::View& v, MapShows shows,
                             std::uint64_t now);

/// The floors the dots are drawn on (mob_dots, you_dot): `page`, but none when
/// the entry's file is found off (EntryDoubtful), because its floors are not
/// gone by (KnownMap::file_doubtful), as zone_send and Hunter::zone_page have
/// it. Its floors are still drawn, faded.
const GameMapBook::Page* dot_floors(const GameMapBook::Page* page, MapShows shows);

/// Whether the tab draws living monster `m` grey -- "bot không đi tới" --
/// while the hunter goes by an area (`area_on`): outside the box it publishes
/// (Status::spot_left..spot_bottom) and out of reach of you at you_x, you_y
/// (none with `you` false), as the hunter has the plate -- packet_scale both
/// ways, packet_dy on the height; or in reach too while it walks back first
/// (Status::spot_back_first), when Hunter::mark_out_of_area lets what is in
/// reach outside the box be. The hunter's current target (`target`) only once
/// Hunter::kSpotKeepPx past the edge, as Hunter::mark_out_of_area lets it go.
/// Judged where the hunter's plates put it, the world's own m.x and m.y --
/// never its dot, whose height the hunter does not go by (the owner's Q2 (a)):
/// a grey of the tab's own heights would grey what the hunter fights, and
/// colour red what it lets be.
bool mob_grey(const HunterConfig& cfg, const Hunter::Status& hs, bool area_on, bool you, double you_x,
              double you_y, const PacketWorld::Mob& m, bool target);

// ------------------------------------- the jumps up and the seek (2026.11.05b)
//
// What the tab draws of "Nhảy lên bục trên" and "Tự tìm quái trong vùng", from
// the hunter's status and what the jumps learned (MapViewResult): the jump's
// arrow, the seek's marks, the floors found solid from below. The dots
// themselves keep 2026.11.05's rules (mob_dot, mob_grey).

/// The jump up the tab draws as an arrow, from its take-off to its landing,
/// labelled with its key: the one under way -- walked to, settled, in the air
/// -- or, while kJumpArrowFreshMs fresh, the last that would have gone with the
/// tick off or jumps off for the run (Status::jump_would_plan), `would`.
struct JumpArrow {
    JumpKind kind = JumpKind::Up;
    MapPoint take;          ///< standing point it presses from
    MapPoint land;          ///< standing point it lands on
    bool     would = false;
};
inline constexpr std::uint64_t kJumpArrowFreshMs = 2000;
std::optional<JumpArrow> jump_arrow(const Hunter::Status& hs, std::uint64_t now);

/// The seek's mark of living monster `m` while the seek is on
/// (Status::seek_grey, by slot and generation): no way to it from where the
/// character stands, or passed over a while -- the tab greys it and says why.
/// Null for none, and never for the one gone for.
const Hunter::Status::SeekGrey* seek_grey_of(const Hunter::Status& hs, const PacketWorld::Mob& m);

/// The floors `on` names -- a standing point on each -- followed both ways to
/// their ends (Page::follow), as runs to draw: the floors jumps up found solid
/// from below this session. None for a point on no floor.
std::vector<MapRun> solid_marks(const GameMapBook::Page& p, std::span<const MapPoint> on);

/// The owner's Q3 of 2026.11.05 ((a) until answered): how many frames a
/// second GoBot's window is drawn at while the tab is shown, behind the game
/// and in front with nobody touching it -- 5 until 04d, a walker's dot
/// stepping some 21 px of the world a frame and the view 69 while you walk.
/// With "Nhẹ máy", kMapTabLightFps, as before.
inline constexpr int kMapTabFps = 10;
inline constexpr int kMapTabLightFps = 2;

} // namespace gobot
