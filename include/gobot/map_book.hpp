#pragma once

#include "gobot/common.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace gobot {

/// A point in the game's world: pixels, y growing downward -- where the
/// packets put you and the monsters (PacketWorld), and what the game's own
/// map files are drawn in.
struct MapPoint {
    int x = 0;
    int y = 0;
};

/// One floor in one pixel column of a map, out of the game's own map file:
/// a surface a character stands on.
struct MapFloor {
    /// Its top, in world pixels. The packets put a character or a monster
    /// standing on it 0-1 px above: 49 of 49 live points of 2026-11-02/03.
    int          y = 0;
    /// How far below `y` the deepest row of the file taken as this floor is:
    /// 0 to kMergePx. A point is on the floor within kOnFloorPx of any row.
    std::uint8_t depth = 0;
    /// The file record's byte 2. 1 is a platform: the character jumps up
    /// through it and walks off its ends (live, 2026-11-03), and drops through
    /// it with Ctrl + ↓. 2 is solid ground -- on the player's maps only the
    /// very bottom, where Ctrl + ↓ plays the jump-down and nothing happens.
    /// Others are rare: GoC reads 4 as a wall, 5 and 6 as blocks. Rows taken
    /// as one floor keep the first of theirs other than 1: never a platform
    /// where one of them is not.
    std::uint8_t attr = 0;
    std::uint8_t layer = 0;     ///< which of the file's two blocks its top row is in: 0 or 1

    bool platform() const { return attr == 1; }
};

/// A gate -- a warp zone -- as the game's .prj file places it, or as GoBot
/// learned one this session (kind 0). ↓ on it takes the character to another
/// map, and so does Ctrl + ↓ (the player's hand test, 2026-11-04).
struct MapGate {
    int          x = 0;
    int          y = 0;
    std::int32_t kind = 0;      ///< the object's id: 3900000 피스존, 3900008 전투존, ...; 0 learned
};

/// A map's floors, column by column (GameMapBook::parse_map).
struct MapFloors {
    int w_cells = 0;
    int h_cells = 0;
    /// Column x's floors are floors[start[x] .. start[x + 1]), top first.
    std::vector<std::uint32_t> start;
    std::vector<MapFloor>      floors;

    int w_px() const { return w_cells * 32; }
    int h_px() const { return h_cells * 32; }
    /// Column x's floors, top first; none outside the map.
    std::span<const MapFloor> at(int x) const;
};

/// The game's own description of a map: its floors from data\Map\<stem>.map
/// and its gates from data\Project\<stem>.prj -- every floor and every gate,
/// with no walking the map to learn them (the drop plan's §6; data.md). What
/// a drop through a platform (Ctrl + ↓) is checked against: where the
/// character will land, whether it stands on the bottom ground, and how far
/// the nearest gate is.
///
/// The .map is read PLANAR: [i32 W][i32 H], then all W x H layer-0 records of
/// 36 bytes, cell by cell down each column, then all layer-1 records -- as
/// GoC reads it. In a record, byte 2 is the attribute and bytes 3-34 the
/// surface's row in each of the cell's 32 pixel columns, 0xFF for none.
/// MapFile reads each cell as 72 interleaved bytes, which puts not one live
/// position on a floor; it is left as it is, for the Map tab, the CLI and the
/// pathfinder.
///
/// The .prj lists its objects' kinds -- [i32 id][u32 type] and the sprite's
/// path -- and then places them, each [i32 kind][u8][f32 1.0][i32 x][i32 y]
/// after a name of 20 bytes. A kind whose sprite is \Data\OBJ\Object\
/// warpzone.spr (or ind_portal.spr) is a gate: on all 1,285 of the game's
/// .prj files that reads the same 1,700 gates as any looser reading.
///
/// A map is read on its first ask, on the asking thread -- the hunter's --
/// once, and kept, a map with no file too. No lock is held while a file is
/// read, so another thread asking is never held up by one. A file missing,
/// cut short or garbled is no data, never a crash, and no game installed is
/// no data for every map.
///
/// Also what GoBot learned this session, per map: gates of its own (a map
/// entry's spawn, either end of a drop that changed the map), the points
/// where Ctrl + ↓ did not drop, and the maps whose file a drop found wrong.
/// Safe from any thread. (The drop plan's "MapBook": that name is the
/// minimap's book of names typed by hand, minimap_tracker.hpp.)
class GameMapBook {
public:
    static constexpr int kCellPx = 32;
    /// A row of a column within this below the last floor's top is that floor.
    static constexpr int kMergePx = 2;
    /// A standing point is on a floor when one of the floor's rows is within
    /// this, up or down. Live: 0-1 px.
    static constexpr int kOnFloorPx = 3;
    /// A no-drop point covers this far either way (the drop plan's 11a).
    static constexpr int kNoDropXPx = 64;
    static constexpr int kNoDropYPx = 24;
    /// More cells either way than this is not a map (as MapFile).
    static constexpr int kMaxCells = 4096;
    /// Points remembered per map, each kind: the oldest go past this.
    static constexpr std::size_t kLearnedMax = 256;

    /// One map, out of the game's files.
    struct Page {
        std::string          stem;
        MapFloors            floors;
        bool                 prj_read = false;  ///< its .prj was there and read
        std::vector<MapGate> gates;             ///< the .prj's gates, in its order: none without one

        std::span<const MapFloor> floors_at(int x) const { return floors.at(x); }
        /// The floor a point at x, y stands on -- one with a row within
        /// kOnFloorPx of y -- the top one if two are; none if it is on none.
        std::optional<MapFloor> floor_near(int x, int y) const;
        bool on_floor(MapPoint p) const { return floor_near(p.x, p.y).has_value(); }
        /// The first floor whose top is more than `gap` below y at x: where a
        /// fall straight down from there lands. None for nothing below.
        std::optional<MapFloor> floor_below(int x, int y, int gap) const;
    };

    /// A .map file's bytes, read planar into floors. None unless it is
    /// exactly 8 + W x H x 72 bytes with W and H 1 to kMaxCells.
    static std::optional<MapFloors> parse_map(const Bytes& file);
    /// The gates a .prj file's bytes place. None for none, or for a file cut
    /// before them.
    static std::vector<MapGate> parse_prj(const Bytes& file);
    /// "t52_s2" for map 52, area 2: the files' stem for a map entry's numbers.
    static std::string stem_of(std::uint16_t map, std::uint16_t area);
    /// The game's data folder -- the one data\Map is in -- found as
    /// MapFile::default_map_dir finds data\Map: the installer's record, then
    /// the usual place. Empty when the game is not there.
    static std::string default_data_dir();

    /// The game's own data folder, looked up at the first need.
    GameMapBook();
    /// Exactly `data_dir` (the folder Map and Project are in), never looked
    /// up: empty for none. What the tests use.
    explicit GameMapBook(std::string data_dir);
    GameMapBook(const GameMapBook&) = delete;
    GameMapBook& operator=(const GameMapBook&) = delete;

    /// The data folder in use; empty for none.
    std::string data_dir();

    /// The map `stem` names, read on the first ask and kept: null when its
    /// .map is missing or does not read. A missing .prj leaves it no gates.
    std::shared_ptr<const Page> get(const std::string& stem);

    /// Every .map in data\Map, by stem, sorted: listed once. Only names
    /// ending ".map" -- not the game's t53_s4.map_, which Windows' "*.map"
    /// also finds. A listing Windows cut short is none, and not kept.
    std::vector<std::string> stems();
    std::size_t map_files();

    /// For naming a map from standing points: how many of `points` lie on
    /// no floor of `stem`'s .map (kOnFloorPx), read straight from the file --
    /// a few bytes a point, never the whole of it -- or from its page when
    /// it was read. -1 when the file is not a map (its size and head say so)
    /// or the stem is no file name; -2 when it could not be opened, or a read
    /// of it failed: whether it fits is not known.
    int misses(const std::string& stem, std::span<const MapPoint> points);

    /// Its gates: the .prj's, then those learned this session (kind 0).
    std::vector<MapGate> gates(const std::string& stem);
    void learn_gate(const std::string& stem, MapPoint p);
    std::vector<MapGate> learned_gates(const std::string& stem) const;

    /// A point where Ctrl + ↓ did not drop -- nothing happened, a jump in
    /// place, or a rise -- and whether one is within kNoDropXPx, kNoDropYPx.
    void learn_no_drop(const std::string& stem, MapPoint p);
    bool no_drop_near(const std::string& stem, MapPoint p) const;
    /// Takes back one learned at exactly `p` -- a verdict a map change later
    /// overrode. Whether there was one.
    bool unlearn_no_drop(const std::string& stem, MapPoint p);

    /// A drop on it landed where its file did not say: its file is not gone
    /// by again this session (MapTrust).
    void distrust(const std::string& stem);
    bool distrusted(const std::string& stem) const;

private:
    struct Learned {
        std::vector<MapPoint> gates;
        std::vector<MapPoint> no_drop;
    };

    std::shared_ptr<const Page> read_page(const std::string& dir, const std::string& stem) const;

    mutable std::mutex                                  mutex_;
    std::string                                         dir_;
    bool                                                looked_ = false;  ///< dir_ is final
    std::map<std::string, std::shared_ptr<const Page>>  pages_;           ///< null: tried, no data
    std::optional<std::vector<std::string>>             stems_;
    std::map<std::string, Learned>                      learned_;
    std::set<std::string>                               distrusted_;
};

/// Which map the character is on, and whether the game's file of it is to be
/// gone by -- per map and per map entry (WorldFix::epoch): the drop plan's
/// §6.3, and the player's answer 9 (2026-11-04) for GoBot opened mid-map
/// ("map 0/0"). The ONE way the map is seeded:
///
///   - A map entry (0x0086) seen since the capture began names it, and always
///     wins. Its file is trusted (a) at once when the entry's spawn is within
///     kGateMatchXPx, kGateMatchYPx of a gate its .prj lists -- every live
///     entry was -- or (b) after kTrustPoints standing points on its floors,
///     each kTrustSpreadXPx or kTrustSpreadYPx from the others.
///   - With no entry seen, the standing points name it: kNamePoints at least,
///     each kNameApartXPx or kNameApartYPx from the others, EVERY one on a
///     floor of the one .map file of all the game's -- and every other file
///     missing kNameMarginMisses of them at least. The first 12 points apart
///     in the log of 2026-11-03 22:36 lie on t52_s1 alone of 781 files, the
///     next best missing 6 of them.
///
/// Either way its .prj must list a gate, and a map a drop found wrong this
/// session (GameMapBook::distrusted) is never trusted again.
///
/// A standing point is yours when settled (WorldFix::settled), once per
/// report -- never at the entry's spawn, where you may still be in the air --
/// or a monster's, where its own stop frame put it, once per frame. Every one
/// seen is checked against the file in use: one off its floors (kOnFloorPx)
/// and an entry's file is not gone by again until the next entry -- the
/// capture lost and back meanwhile, or lose(), changes nothing of that; a
/// named map is let go of, and naming starts again from that point on.
///
/// Everything is let go of when the capture stops or starts again (the map
/// may have changed unseen), at each map entry, and at lose() -- a re-login,
/// Tìm lại, the plates' view changing -- after which an entry's file is
/// trusted again only by (b), or a new entry.
///
/// Naming reads every .map, a few bytes a point, a slice at each update()
/// (kWorkMs of the real clock) once kNamePoints are in -- then only the files
/// that still fit, for later points; all of them again at most every
/// kNameRescanMs. A file that could not be opened or read is tried again at
/// each update, and nothing is named until it reads: left out, a copy of the
/// map beside it would be the one that fits. One thread: the hunter's.
class MapTrust {
public:
    static constexpr int kGateMatchXPx = 40;
    static constexpr int kGateMatchYPx = 30;
    static constexpr int kTrustPoints = 3;
    static constexpr int kTrustSpreadXPx = 64;
    static constexpr int kTrustSpreadYPx = 24;
    static constexpr int kNamePoints = 12;
    /// The next best file must miss this many of the points at least: half of
    /// kNamePoints. Live: 6 of 12 on t52_s1, 23 of 32 on t52_s2.
    static constexpr int kNameMarginMisses = 6;
    static constexpr int kNameApartXPx = 8;
    static constexpr int kNameApartYPx = 4;
    /// Points kept toward a name: past this with none named, naming starts again.
    static constexpr std::size_t kNameMaxPoints = 256;
    /// Every .map read again at most this often (the hunter's clock): after
    /// points no file fits, a named map let go of, or kNameMaxPoints.
    static constexpr std::uint64_t kNameRescanMs = 30000;
    /// Your point this close to the entry's spawn, both ways, is not one.
    static constexpr int kEntrySpawnPx = 2;
    /// Real time spent reading files at one update(): one file at least.
    static constexpr std::uint32_t kWorkMs = 8;

    enum class How : std::uint8_t {
        None,
        Entry,      ///< a map entry, its spawn by a listed gate
        Points,     ///< a map entry, and kTrustPoints standing points on the file's floors
        Named,      ///< no entry seen: named from standing points
    };
    enum class Why : std::uint8_t {
        Trusted,        ///< page() is the map's file, to be gone by
        NoBook,         ///< no GameMapBook: nothing is trusted
        NoCapture,      ///< the packets cannot say where you are
        NoFiles,        ///< none of the game's map files were found
        NoFile,         ///< no .map for the entry's numbers, or one that does not read
        NoGates,        ///< its .prj lists no gate, or is missing
        Distrusted,     ///< a drop on it landed where its file did not say, this session
        Mismatch,       ///< a standing point off its floors, during this entry's stay
        Confirming,     ///< entry seen, its spawn by no gate: `points` of kTrustPoints so far
        Naming,         ///< no entry seen: `points` of kNamePoints so far, or the files being read
        Ambiguous,      ///< no entry seen: another file fits too many of the points yet
    };
    static const char* why_text(Why w);

    /// A monster standing where its own stop frame put it, alive, not moving:
    /// never a place worked out.
    struct Mob {
        MapPoint      at;
        std::uint32_t slot = 0;
        std::uint64_t at_ms = 0;        ///< when that frame came: one point per frame
    };
    /// What one pass of the hunter sees, out of its WorldFix (rounded) and
    /// the packets' standing monsters.
    struct Look {
        std::uint64_t now_ms = 0;       ///< the hunter's clock
        /// The packets can say where you are: false for WorldFix NoCapture and
        /// FirstReport -- a capture stopped or started again.
        bool          capture = false;
        bool          settled = false;  ///< WorldFix::settled()
        int           rx = 0;           ///< WorldFix::rx, ry: where your last report put you
        int           ry = 0;
        std::uint64_t report_ms = 0;    ///< WorldFix::report_ms
        bool          map_known = false;
        std::uint16_t map = 0;
        std::uint16_t area = 0;
        std::uint32_t epoch = 0;
        int           entry_x = 0;      ///< WorldFix::entry_x, entry_y
        int           entry_y = 0;
        std::vector<Mob> mobs;
    };
    /// How it stands, for the log and the hint.
    struct Report {
        Why           why = Why::NoBook;
        How           how = How::None;
        std::string   stem;             ///< the map in use, or the entry's being confirmed; "" none
        std::uint32_t epoch = 0;
        int           points = 0;       ///< standing points taken toward trust or a name
        int           needed = 0;       ///< kTrustPoints or kNamePoints; 0 when none are wanted
        std::size_t   read = 0;         ///< naming: files read for all the points in
        std::size_t   files = 0;        ///< naming: of how many
        std::string   next_best;        ///< naming: the file that fits next best, ...
        int           next_misses = 0;  ///< ... and how many of the points it misses
        int           fit = 0;          ///< naming: how many files fit every point
        MapPoint      miss;             ///< Mismatch: the standing point off the floors
    };

    explicit MapTrust(GameMapBook* book = nullptr);
    void set_book(GameMapBook* book);

    /// One pass. Reads files on this thread: a map's at its first ask, and
    /// up to `work_ms` of naming.
    void update(const Look& look, std::uint32_t work_ms = kWorkMs);
    /// Trust is let go of: a re-login, Tìm lại, the plates' view changing. An
    /// entry's file is trusted again only by standing points or a new entry,
    /// and one found wrong stays so; a named map is named afresh.
    void lose(const char* why);

    bool trusted() const { return trusted_; }
    How how() const { return trusted_ ? how_ : How::None; }
    /// The map in use: its file while trusted; null otherwise.
    std::shared_ptr<const GameMapBook::Page> page() const { return trusted_ ? page_ : nullptr; }
    /// The entry's stem, or the one named: "" for none.
    const std::string& stem() const { return stem_; }
    Report report() const;

private:
    struct Stand {
        MapPoint at;
        bool     own = false;
    };
    struct Cand {
        std::string stem;
        std::size_t checked = 0;        ///< points read for so far
        int         misses = 0;
        bool        out = false;        ///< kNameMarginMisses missed, or the file is no map
        bool        unread = false;     ///< the file is no map
        bool        io_fail = false;    ///< its last read failed: still to be read, nothing named meanwhile
    };

    void reset();
    void clear_naming();
    std::vector<Stand> standing_points(const Look& look);
    void by_entry(const Look& look, const std::vector<Stand>& points);
    void by_name(const Look& look, const std::vector<Stand>& points, std::uint32_t work_ms);
    void add_name_point(MapPoint p);
    void scan(std::uint32_t work_ms);
    void decide();
    void judge_named();
    void restart_naming(const char* why);

    GameMapBook*                         book_ = nullptr;
    bool                                 epoch_set_ = false;
    std::uint32_t                        epoch_ = 0;
    bool                                 entry_mode_ = false;   ///< the map from an entry, not named
    std::string                          stem_;
    std::shared_ptr<const GameMapBook::Page> page_;
    bool                                 trusted_ = false;
    How                                  how_ = How::None;
    Why                                  why_ = Why::NoBook;
    // an entry's map
    bool                                 entry_learned_ = false;
    bool                                 entry_spent_ = false;  ///< (a) not to be had again: lose()
    bool                                 lost_set_ = false;     ///< the capture was lost during ...
    std::uint32_t                        lost_epoch_ = 0;       ///< ... this entry's stay
    std::string                          lost_stem_;            ///< ... on this map's file, ...
    bool                                 lost_mismatch_ = false; ///< ... found wrong then, ...
    MapPoint                             lost_miss_;            ///< ... by this point
    bool                                 mismatch_ = false;
    MapPoint                             miss_;
    std::vector<MapPoint>                confirm_;
    bool                                 said_no_gate_ = false;
    bool                                 said_not_by_gate_ = false;
    // naming
    std::vector<MapPoint>                name_pts_;
    std::vector<Cand>                    cands_;
    std::size_t                          cursor_ = 0;
    std::uint64_t                        scan_ms_ = 0;          ///< when the last full read began
    bool                                 scanned_ = false;      ///< one has, since GoBot started
    std::string                          next_best_;
    int                                  next_misses_ = 0;
    int                                  fit_ = 0;
    int                                  restarts_ = 0;
    bool                                 said_ambiguous_ = false;
    // each report or frame once
    std::uint64_t                        own_seen_ms_ = 0;
    std::map<std::uint32_t, std::uint64_t> mob_seen_;
};

} // namespace gobot
