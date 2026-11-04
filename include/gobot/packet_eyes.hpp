#pragma once

// "Vị trí từ gói tin": the hunter's eyes on the game's own packets, with the
// screen as the fallback.
//
// MobDetector reads you and the monsters off the screen by their name plates,
// and the plates flap: the character's own was lost and found 281 times in 7
// minutes of a real hunt. The game's frames say where you and every monster
// are (PacketWorld). PacketEyes hands the hunter one or the other, decided
// once at the start of each pass (Hunter::Eyes::frame): you and the monsters
// from one PacketWorld snapshot while the packets are healthy, and the
// detector's three functions, untouched, whenever they are not. Back to the
// screen at once; on to the packets only after kHealthyForMs of health. Each
// switch moves Hunter::Eyes::view, and the hunter lets go of what it held by
// where things stood.
//
// Beside it, the pure parts the GUI's capture for positions is watched and
// kept attached with: packet_health, which says whether the packets may steer
// and, when not, why; compare_views and summarize_views, which lay what the
// packets say over what the screen shows for the 10-second log line; and
// should_reattach, which says when the capture is to be started again.
//
// Nothing here has met the game's real bytes: every offset under it is the
// docs', tested on hand-built frames (packet_world.hpp). That one world pixel
// is one screen pixel is inferred from walking speeds only; packet_scale and
// packet_dy in hunt.ini change how the two are laid over each other without a
// new build, and the log line prints what was measured beside them.

#include "gobot/hunter.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/packet_world.hpp"
#include "gobot/tcp_reassembler.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace gobot {

/// A plate standing at `x`, `y`: its x() and y() are that point, as a plate
/// the detector makes is where its name's middle and its bar's top are.
/// What a packet's position is handed to the hunter as.
Nameplate packet_plate(int x, int y);

/// Why the positions come from where they come from: the packets, or the
/// screen and the first reason the packets are not used.
enum class PacketWhy : std::uint8_t {
    InUse,          ///< the packets steer
    Off,            ///< packet_positions=off: no capture for positions at all
    Watch,          ///< packet_positions=watch: watched and logged, the screen fights
    NoNpcap,        ///< Npcap is not installed, or its service is stopped
    NoCapture,      ///< no capture running: it would not start, or it was stopped by hand
    NoTraffic,      ///< nothing of the game's TCP for kTcpQuietMs: wrong adapter, or disconnected
    ListInUse,      ///< a monster list (Lọc quái) is in use, and packets carry no names
    NotMoved,       ///< no report of yours since the capture began: you have not moved or cast
    Foreign,        ///< another character's reports within kForeignMs: a second client here
    WrongSize,      ///< frames of a size never seen within kWrongSizeMs: a game update, maybe
    MapChanged,     ///< a map entry, and neither its table nor kSettleMs since
    NoMonsters,     ///< started mid-map: no monster frame yet, or not for kSettleMs
    Uncertain,      ///< you moved with no report kReportLateMs longer than one is followed, or with no facing
    Settling,       ///< healthy, but not yet for kHealthyForMs
};

/// The reason in the log's English.
const char* packet_why_name(PacketWhy why);

/// The simple screen's one line about it: "Vị trí: từ gói tin" while the
/// packets steer, "Vị trí: từ màn hình — <lý do>" otherwise.
std::string position_line_vi(PacketWhy why);

/// What the GUI's capture is doing, as far as positions go.
struct PacketLink {
    /// False when the last start of the capture for positions found Npcap
    /// missing or its service stopped, and no capture runs: one running,
    /// whoever started it, is Npcap working. Asked only at a start: looking
    /// costs a service query.
    bool          npcap = true;
    bool          capturing = false;    ///< a capture runs, whoever started it
    std::uint64_t last_tcp_ms = 0;      ///< PacketCapture::Stats::last_tcp_ms; 0 for none yet
};

/// Whether the packets may steer, and if not the first reason why not.
struct PacketHealth {
    bool      ok = false;
    PacketWhy why = PacketWhy::Off;

    /// How long each rule of packet_health looks back.
    static constexpr std::uint64_t kTcpQuietMs  = 3'000;     ///< game TCP heard within this
    static constexpr std::uint64_t kForeignMs   = 30'000;    ///< no other character's report within this
    static constexpr std::uint64_t kWrongSizeMs = 60'000;    ///< no frame of a strange size within this
    /// After a map entry with no table, or from the first monster frame of a
    /// capture begun mid-map: every slot has sent an event by then (the
    /// longest gap seen between one slot's events was 5.6 s).
    static constexpr std::uint64_t kSettleMs    = 6'000;
    /// How much longer than PacketWorld follows a move of yours with no
    /// report (View::own_follow_ms, kAirReckonMs) it may go before the packets
    /// are let go of: the report that ends a move comes a moment after the key
    /// is up, and the hunter asks the moment it is. A step back held 860 ms
    /// -- GoC's longest, and the turn's tap -- is past the 600 ms a walk the
    /// hunter does not hold is followed; a jump can be past kAirReckonMs; a
    /// held walk let go of near its 3 s.
    static constexpr std::uint64_t kReportLateMs = 300;
};

/// Whether the packets may steer at `now`, from `v` (PacketWorld::view at
/// `now`), the world's `s`, the capture's `link` and the hunter's `cfg`. All
/// of these must hold, and the first that does not is the reason:
///
///   - packet_positions is On;
///   - Npcap is there, a capture runs, and game TCP was heard within
///     kTcpQuietMs;
///   - no monster list is in use (the packets carry no names);
///   - a report of yours has come since the capture began;
///   - no other character's report within kForeignMs, and no frame of a size
///     never seen within kWrongSizeMs;
///   - the monsters are settled: the map's table since its entry, or
///     kSettleMs since the entry -- or, with no entry seen, since the first
///     monster frame;
///   - you are known, a move of yours has a facing to follow, and none has
///     gone unreported for kReportLateMs longer than it is followed.
///
/// Standing still is never a reason: it sends nothing because nothing moves.
PacketHealth packet_health(const PacketWorld::View& v, const PacketWorld::Stats& s, const PacketLink& link,
                           const HunterConfig& cfg, std::uint64_t now);

/// Where the packets put you in their world at `now`, for the hunting spot,
/// from the same `v`, `s` and `link` as packet_health -- by its capture and
/// "you" rules alone: whatever steers, and whether a list is in use or the
/// monsters have settled, where you are does not depend on it.
///
///   - NoCapture: no Npcap, no capture, or no game TCP within kTcpQuietMs;
///   - FirstReport: no report of yours since the capture began;
///   - Lost: another character's report within kForeignMs, one of your
///     reports' opcodes of a strange size within kWrongSizeMs (any other
///     opcode's says nothing of where you are), or you not known, moving
///     with no facing, or a move unreported kReportLateMs past the time it
///     is followed;
///   - Known otherwise.
///
/// x and y are worked out to `now`; rx and ry are the last report's own. The
/// map entries ever (View::map_epoch) and the last one's spawn (View::entry_x,
/// entry_y) come with it, through a capture started again too; and which way
/// you face, with when a 0x0027 last said so (Me::said_facing, Me::facing_ms).
WorldFix world_fix(const PacketWorld::View& v, const PacketWorld::Stats& s, const PacketLink& link,
                   std::uint64_t now);

// ---------------------------------------------------------------- the eyes

class PacketEyes {
public:
    /// The screen's three, as the hunter would have used them: MobDetector's
    /// player(), mobs() and frame count in the GUI.
    struct Screen {
        std::function<bool(Nameplate&, bool*)>  player;
        std::function<std::vector<Nameplate>()> mobs;
        std::function<std::uint64_t()>          frame;
    };
    /// The rest it goes by, each asked from the hunter's thread at the start
    /// of a pass: the capture, the clock the frames are stamped on (now_ms()
    /// unless set), the game's client size (the box monsters are offered
    /// from) and the hunter's config -- packet_positions, the lists,
    /// dead_below, packet_scale, packet_dy and the character's name.
    struct Inputs {
        std::function<PacketLink()>     link;
        std::function<std::uint64_t()>  now;
        std::function<bool(int&, int&)> view_size;
        std::function<HunterConfig()>   config;
        /// Until when a walk of yours is the hunter's, held from one look to
        /// the next (Hunter::walk_held_until_ms): followed kOwnReckonMs with
        /// no report until then, kOwnStepReckonMs after -- as before the held
        /// walk, whose start and stop are all the client says of it. Unset: no
        /// walk is ever the hunter's.
        std::function<std::uint64_t()>  walk_held_until;
        /// The game's count of your take-offs (GuiApp::packet_takeoffs),
        /// handed on as the hunter's Eyes::takeoffs for its drops. Unset:
        /// none.
        std::function<std::optional<std::uint64_t>()> takeoffs;
    };

    enum class Source : std::uint8_t { Screen, Packets };

    /// Health this long before the packets take over; the screen takes over
    /// again at the first pass they are not healthy.
    static constexpr std::uint64_t kHealthyForMs = 2'000;
    /// A killed monster stays this long at HP 0, so the hunter sees its
    /// target finished -- counted, and picked up after -- and then goes.
    static constexpr std::uint64_t kDeadShownMs = 1'500;
    /// Monsters are offered within this much of the game's client width and
    /// height of you, either way: about what the screen shows, as the player
    /// chose -- the packets name every monster of the map, up to 4,400 px
    /// away, and the hunter's "nearest with health anywhere" would otherwise
    /// walk the whole map.
    static constexpr double kBoxFraction = 0.6;
    /// The hand-over back to the screen. While the packets steer, the
    /// detector may look only every DetectorPace::kRestMs, and its last
    /// picture at a switch back can be that old. So when no new picture has
    /// come for kScreenFreshMs -- twice the detector's own wait between looks
    /// -- the switch keeps the packets' last plates, view and number for the
    /// hunter until the detector has counted a picture taken after it (two
    /// past its count at the switch: the look under way then may have been
    /// grabbed before), and for kHandOverMs at most. source() and why() say
    /// the screen from the switch on, so GoBot's window wakes the detector at
    /// once (GuiApp::detector_pace_tick). With pictures at the detector's own
    /// pace the switch is at once, as it always was. While it rests -- its
    /// last two pictures more than kScreenFreshMs apart -- a picture is used
    /// at once only up to MobDetector::kLookGapMs old, never older than one at
    /// its own pace would be.
    static constexpr std::uint64_t kHandOverMs = 300;
    static constexpr std::uint64_t kScreenFreshMs = 2 * static_cast<std::uint64_t>(MobDetector::kLookGapMs);

    PacketEyes(const PacketWorld& world, Screen screen, Inputs inputs);

    PacketEyes(const PacketEyes&) = delete;
    PacketEyes& operator=(const PacketEyes&) = delete;

    /// For Hunter::set_eyes: frame, player, mobs, view and where, bound to
    /// this. It must outlive the hunter it is given to.
    Hunter::Eyes eyes();

    /// What the hunter's eyes call, from the hunter's thread.
    ///
    /// frame(), asked first in every pass, takes one PacketWorld view, judges
    /// it (packet_health), switches the source as the rules say and, with the
    /// packets as the source, makes the pass's plates from that one view:
    /// you, named after the character; the living monsters inside the box,
    /// nearest first (MobDetector::nearest_first), their HP laid between just
    /// above dead_below and full -- a living monster is never finished, and
    /// every blow still shows on its bar -- and a killed one at 0 for
    /// kDeadShownMs. Its number is then the world's updates. With the
    /// screen as the source, the screen's three are passed through untouched.
    std::uint64_t          frame();
    bool                   player(Nameplate& out, bool* fresh);
    std::vector<Nameplate> mobs();
    std::uint32_t          view() const { return view_.load(); }

    /// Where the packets put you (world_fix), as the last frame() worked it
    /// out from its view -- the packets steering or only watched. False, and
    /// `out` NoCapture, with packet_positions off or before the first pass.
    /// From any thread.
    bool where(WorldFix& out);

    /// The monsters standing where their own frame put them, from the same
    /// view: alive, not walking, and no speed of a walk or a blow on them --
    /// a stop's place, never one worked out. In world pixels, with the
    /// frame's time: standing points for the map the hunter's drops go by
    /// (MapTrust). Every one of the map's, not only those in the box. None
    /// with packet_positions off. From any thread.
    std::vector<MapTrust::Mob> stands();
    static constexpr std::size_t kStandsMax = 64;

    /// Back to the screen, as hunting starts: the packets earn their
    /// kHealthyForMs afresh. Called before the hunter starts.
    void restart();

    /// For the screens, from any thread.
    Source    source() const { return source_.load(); }
    PacketWhy why() const { return why_.load(); }

    /// Time between passes, by what it was spent on: the packets the source,
    /// the screen the source, and the packets healthy -- in Watch too, where
    /// they never steer, so a watching hunt says how often they could have.
    /// A gap of more than a second between passes (hunting stopped) counts a
    /// second.
    struct Stats {
        std::uint64_t packets_ms = 0;
        std::uint64_t screen_ms = 0;
        std::uint64_t healthy_ms = 0;
        std::uint64_t switches = 0;
        /// Switches back to the screen that kept the packets' plates a
        /// moment (kHandOverMs): how many, how many of them ran out the
        /// time with no newer picture, and the time spent in them all.
        std::uint64_t handovers = 0;
        std::uint64_t handovers_capped = 0;
        std::uint64_t handover_ms = 0;
    };
    Stats stats() const;

private:
    std::uint64_t clock() const;
    /// The pass's plates from `v`, under the lock.
    void make_plates(const PacketWorld::View& v, const HunterConfig& cfg, std::uint64_t now);

    const PacketWorld& world_;
    Screen             screen_;
    Inputs             inputs_;

    std::mutex             mutex_;              ///< what the hunter's thread works with
    bool                   healthy_ = false;    ///< steering would be healthy since healthy_since_ms_
    std::uint64_t          healthy_since_ms_ = 0;
    bool                   was_healthy_ = false;   ///< at the last pass, for healthy_ms
    std::uint64_t          last_pass_ms_ = 0;
    bool                   passed_ = false;     ///< a pass has run since the start or restart()
    int                    box_w_ = 1024;       ///< the client size last known
    int                    box_h_ = 768;
    Nameplate              me_;
    bool                   me_fresh_ = false;
    std::vector<Nameplate> mobs_;
    WorldFix               fix_;                ///< the last frame()'s: where()
    bool                   fix_on_ = false;     ///< it was worked out: the packets watched
    std::vector<MapTrust::Mob> stands_;         ///< the last frame()'s: stands()
    /// The screen's pictures as the passes see them: the detector's count at
    /// the last pass, and when it was last seen to move on.
    std::uint64_t          pictures_ = 0;
    std::uint64_t          picture_new_ms_ = 0;
    /// The time between the last two times it moved on: over kScreenFreshMs,
    /// the detector is resting.
    std::uint64_t          picture_gap_ms_ = 0;
    /// A hand-over under way (handing_over_): when it began, the detector's
    /// count then, and the number the packets' last pass gave, served through
    /// it with that pass's plates.
    std::uint64_t          handover_from_ms_ = 0;
    std::uint64_t          handover_pictures_ = 0;
    std::uint64_t          packet_number_ = 0;

    std::atomic<Source>        source_{Source::Screen};
    std::atomic<PacketWhy>     why_{PacketWhy::Off};
    std::atomic<std::uint32_t> view_{0};
    std::atomic<std::uint64_t> packets_ms_{0};
    std::atomic<std::uint64_t> screen_ms_{0};
    std::atomic<std::uint64_t> healthy_ms_{0};
    std::atomic<std::uint64_t> switches_{0};
    std::atomic<bool>          handing_over_{false};
    std::atomic<std::uint64_t> handovers_{0};
    std::atomic<std::uint64_t> handovers_capped_{0};
    std::atomic<std::uint64_t> handover_ms_{0};
};

// ------------------------------------------------------ the detector's pace

/// What the name-plate reader's pace hangs on, as GoBot's window sees it at
/// one tick (GuiApp::detector_pace_tick): the hunt, where the hunter's eyes
/// are, and everything else that reads the detector's plates or the frames
/// they come with.
struct DetectorPace {
    bool               hunting = false;
    PacketEyes::Source source = PacketEyes::Source::Screen;  ///< PacketEyes::source()
    PacketWhy          why = PacketWhy::Off;                 ///< PacketEyes::why()
    bool               jump_test = false;       ///< a jump key's "Thử" watching your plate
    bool               monster_look = false;    ///< "Quái đang thấy" listing what is in view
    bool               camera_panels = false;   ///< the Radar or the movement panels open
    bool               walking = false;         ///< a navigator walk, which follows the camera
    bool               rest_off = false;        ///< app.ini [Packets] detector_rest=off

    /// The wait between looks while the packets steer, and otherwise: the
    /// detector's own, as it always was.
    static constexpr int kRestMs = 600;
    static constexpr int kRunMs = MobDetector::kLookGapMs;
};

/// Whether the detector may rest -- look every DetectorPace::kRestMs rather
/// than kRunMs: hunting, with the packets steering and healthy (the eyes'
/// source the packets, and why() InUse), no "Thử", no "Quái đang thấy", no
/// Radar or movement panel, no walk, and detector_rest not off. `why`, when
/// given, gets the first of those that does not hold, in the log's English,
/// or "" when it may rest.
bool detector_rest(const DetectorPace& p, const char** why = nullptr);

// ------------------------------------------------------- watching the two

/// What the packets say laid over what the screen shows, at one moment.
struct ViewComparison {
    /// You stood settled -- known, standing, on the ground, not uncertain,
    /// and no report for kSettledMs -- so the two could be laid over each
    /// other. Nothing below is filled in otherwise.
    bool  valid = false;
    float offset_x = 0.0f;      ///< your world position less your plate's
    float offset_y = 0.0f;

    /// A plate and the standing packet monster under it.
    struct Pair {
        std::uint32_t slot = 0;
        int           screen_dx = 0;    ///< the plate, from your plate
        int           screen_dy = 0;
        float         world_dx = 0.0f;  ///< its monster, from you, in world pixels
        float         world_dy = 0.0f;
        float residual_x() const { return world_dx - static_cast<float>(screen_dx); }
        float residual_y() const { return world_dy - static_cast<float>(screen_dy); }
    };
    std::vector<Pair> pairs;

    /// The medians of the pairs' residuals, world less screen: what is left
    /// once your own two positions are laid over each other. -15 in y is a
    /// monster level with you that the screen reads 15 px below you, which
    /// packet_dy 15 makes the packets read the same. 0 with no pair.
    float median_x = 0.0f;
    float median_y = 0.0f;
    /// World dx per screen dx, the least-squares slope over the pairs, once
    /// kScalePairs of them span kScaleSpanPx of the screen.
    std::optional<double> scale;

    int screen_only = 0;    ///< plates with no living packet monster near where they put one
    int packets_only = 0;   ///< standing packet monsters inside `seen` with no plate

    /// How long you stand with no report before the two are compared.
    static constexpr std::uint64_t kSettledMs = 300;
    /// How far from where a plate puts it a packet monster may be and still
    /// be that plate's, in world pixels.
    static constexpr float kPairPx = 80.0f;
    /// What a scale is measured over: this many pairs, this far apart.
    static constexpr int kScalePairs = 3;
    static constexpr int kScaleSpanPx = 300;
};

/// Lay `v` (PacketWorld::view at `now`) over the screen: `me`, your plate as
/// seen in the frame `plates` come from, and `plates`, the monsters' (players
/// left out). The offset is your world position less your plate's; each plate
/// is paired, one to one and nearest first, with the standing packet monster
/// within kPairPx of where the offset puts it. Walking monsters are not
/// paired -- their y is a guess between events -- nor counted as packets
/// only; and only while you stand settled is anything compared. `seen` is
/// the part of the screen plates are looked for in (the detector's area):
/// a standing monster the offset puts inside it with no plate is counted as
/// one the screen missed.
ViewComparison compare_views(const PacketWorld::View& v, std::uint64_t now, const Nameplate& me,
                             const std::vector<Nameplate>& plates, const ScreenRect& seen);

/// The comparisons of a few seconds, pooled: what the log line says.
struct ViewSummary {
    int looks = 0;          ///< comparisons handed in
    int compared = 0;       ///< of them, valid
    int pairs = 0;          ///< over them all
    /// Over every pair. median_y and its spread (the 90th centile less the
    /// 10th) say whether your y and a monster's are one point of the body:
    /// a steady offset is what packet_dy is for.
    std::optional<float> median_x;
    std::optional<float> median_y;
    float spread_y = 0.0f;
    std::optional<double> scale;    ///< as compare_views measures one, over every pair
    int screen_only = 0;
    int packets_only = 0;
};
ViewSummary summarize_views(const std::vector<ViewComparison>& looks);

// ---------------------------------------------------- keeping it attached

/// Where the game's traffic was found: its process, the far end of each of its
/// connections, and its own UDP ports -- what the capture for positions is
/// built from (build_filter_for).
struct GameLink {
    std::uint32_t              pid = 0;
    std::vector<Endpoint>      servers;
    std::vector<std::uint16_t> udp_ports;

    /// At most one start of the capture for positions in this long, for a
    /// new UDP port or the game's TCP gone quiet.
    static constexpr std::uint64_t kReattachEveryMs = 10'000;
    /// For a new game process or a new TCP connection -- a login, a
    /// reconnect, a door to another server -- this soon: the map entry that
    /// follows comes within a second or two, and missed, the map is not
    /// known until the next door.
    static constexpr std::uint64_t kReattachNewMs = 1'000;
    /// Game TCP silent this long on the capture for positions: start it again.
    static constexpr std::uint64_t kReattachSilentMs = 5'000;
};

/// Whether to start the capture for positions again: the game's process has
/// changed (it was started again), it has a connection or a UDP port the
/// capture was not built with (a reconnect, a socket opened later), or its
/// TCP has been silent for kReattachSilentMs on it -- a new process or
/// connection not within kReattachNewMs of the last start, a new UDP port or
/// the silence not within kReattachEveryMs, and never with no game or no
/// connection to capture. `why`, when given, gets the reason, in the log's
/// English.
bool should_reattach(const GameLink& before, const GameLink& now, std::uint64_t tcp_silent_ms,
                     std::uint64_t since_try_ms, const char** why = nullptr);

} // namespace gobot
