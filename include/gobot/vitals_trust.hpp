#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gobot {

// ---- your HP and MP from the packets, checked against the bars ----
//
// The packets plan, item D. Wrapped 0x0051 and 0x0050 say your HP and MP, but
// their layout is only what the recordings of 2026-10-18 showed: nothing on
// today's server has proven it. So nothing goes by them until a gate has
// compared them, read by read, with what the bars on the screen showed at the
// same moment -- and the screen decides again the moment they disagree.
//
// Everything here is pure: given times on now_ms()'s clock, no thread, no
// window, no capture. GoBot's window (packets_vitals.cpp) feeds it and asks it.

/// One bar's gate: HP's or MP's packet value against the bar read off the
/// screen.
///
/// A read is judged kHalfWindowMs after it was taken (plus kJudgeSlackMs, as
/// frames reach GoBot in batches), and only when the packet value held still
/// from kHalfWindowMs before the read to kHalfWindowMs after it, and came in
/// the epoch the packets are in now (PacketSelf): a value that moved around
/// the read, or one from before a gap in the capture, says nothing either way.
///
/// With the figures the game prints on the bar read, they must be the packet
/// value and maximum exactly. Without them, by the bar's percentage: within
/// kAgreePoints agrees, more than kDisagreePoints apart disagrees, and in
/// between counts for nothing. HP above a maximum the stat sheet gave
/// (over_max) is a disagreement.
///
/// Trust is earned by the figures exactly, at the read, as ever; it is kept
/// allowing for the HUD running ahead of 0x0051 (2026-11-03). While Trusted or
/// in Doubt, figures within kFigureSlackHpPct / kFigureSlackMpPct of the
/// packet value are between -- a hit or a cast's cost the packets have not
/// sent yet -- and a read that does not agree waits kLeadMs for 0x0051 to
/// reach its figures exactly, which then agree (caught up). And reads of one
/// HUD moment against one packet value make one disagreement, however many
/// reads see it.
///
/// Learning -> Trusted after kTrustAgreements agreements over kTrustValues
/// values or more, none of the last kRecent judged reads a disagreement -- and
/// for HP, one agreement under kLowPct, so that a full bar alone never earns
/// it. Trusted -> Doubt at one disagreement; Doubt -> Trusted after
/// kDoubtClears agreements in a row. Two disagreements in a row, or three among
/// the last kRecent, distrust it: Learning again, kRelearnAgreements needed --
/// and after kDistrustsOff distrusts, Off for the session.
class BarCheck {
public:
    enum class State : std::uint8_t {
        Learning,   ///< not gone by: still comparing
        Trusted,    ///< gone by (if the rest of usable() holds)
        Doubt,      ///< one disagreement: not gone by until kDoubtClears agreements in a row
        Off,        ///< distrusted kDistrustsOff times: never again this session
    };

    static constexpr std::uint64_t kHalfWindowMs = 400;
    static constexpr std::uint64_t kJudgeSlackMs = 100;
    static constexpr float         kAgreePoints = 2.0f;
    static constexpr float         kDisagreePoints = 5.0f;
    static constexpr int           kTrustAgreements = 20;
    static constexpr int           kRelearnAgreements = 50;
    static constexpr int           kTrustValues = 3;
    static constexpr std::size_t   kRecent = 20;
    static constexpr int           kDoubtClears = 5;
    static constexpr int           kDisagreeRow = 2;     ///< in a row: distrusted
    static constexpr int           kDisagreeRecent = 3;  ///< among the last kRecent: distrusted
    static constexpr int           kDistrustsOff = 3;
    static constexpr float         kLowPct = 98.0f;
    static constexpr std::size_t   kPoints = 64;         ///< packet values kept
    static constexpr std::size_t   kPending = 32;        ///< screen reads waiting to be judged
    static constexpr std::size_t   kValuesKept = 64;     ///< distinct values remembered
    /// The HUD runs ahead of 0x0051: a hit or a cast's cost shows on the bar
    /// first (2026-11-03: 0x0051 reached the bars' figures 0.4-0.78 s after
    /// the read eight times; a cast's cost stayed between them 3 s and more
    /// three times). A read of a bar trusted or in doubt whose figures did not
    /// agree waits kLeadMs: a value equal to them that comes by then agrees.
    static constexpr std::uint64_t kLeadMs = 900;
    /// Figures this far from the packets' value, in points of the maximum, are
    /// between -- a hit or a cast in flight -- never a disagreement, while the
    /// bar is trusted or in doubt: HP a hit (277 and 307 of 10084: 2.7-3.0%),
    /// MP one or two casts (573-1612 of 15280: 3.75-10.55%).
    static constexpr float         kFigureSlackHpPct = 5.0f;
    static constexpr float         kFigureSlackMpPct = 12.0f;

    /// A packet value, as it came.
    struct Point {
        std::uint64_t at_ms = 0;
        std::uint32_t value = 0;
        std::uint32_t max = 0;
        bool          max_known = false;   ///< from the stat sheet or the bars' figures, not the highest seen
        std::uint32_t epoch = 0;
    };

    /// A read of the bar off the screen: its percentage, and the figures
    /// printed on it when this very read made them out (-1 otherwise).
    struct Read {
        std::uint64_t read_at_ms = 0;
        float         pct = -1.0f;
        int           figure = -1;
        int           figure_max = -1;
    };

    enum class Verdict : std::uint8_t { Agree, Disagree, Between };

    /// A change of state, and what made it: the read and the packet value it
    /// was judged against (an over_max has no read: its pct is -1).
    struct Change {
        State         from = State::Learning;
        State         to = State::Learning;
        std::uint64_t at_ms = 0;
        Read          read;
        Point         packet;
        float         packet_pct = -1.0f;
        bool          over_max = false;    ///< made by HP above the stat sheet's maximum
        bool          in_a_row = false;    ///< distrusted by disagreements in a row, not by the recent count
        int           agreements = 0;      ///< for a trust: in the learning that earned it
        int           values = 0;
        int           exact = 0;
        int           distrusts = 0;
    };

    /// What was judged, since take_counts().
    struct Counts {
        std::uint64_t agree = 0;
        std::uint64_t disagree = 0;
        std::uint64_t between = 0;
        std::uint64_t exact = 0;          ///< agreements by the printed figures
        std::uint64_t over_max = 0;       ///< of the disagreements, HP above the stat sheet's maximum
        std::uint64_t moving = 0;        ///< not judged: the packet value moved around the read
        std::uint64_t stale = 0;          ///< not judged: the value from an older epoch
        std::uint64_t no_packet = 0;      ///< not judged: no packet value before the read
        std::uint64_t no_maximum = 0;     ///< not judged: no maximum to make a percentage of
        std::uint64_t no_figures = 0;     ///< not judged: figures_only, none made out, the bar long
        std::uint64_t empty = 0;          ///< not judged: figures_only, none made out, the bar empty: the HUD dimmed
        std::uint64_t caught_up = 0;      ///< of the agreements: figures 0x0051 reached within kLeadMs after the read
        std::uint64_t same_episode = 0;   ///< of the disagreements: the last one's HUD moment and packet value again
        std::uint64_t values = 0;         ///< distinct packet values among the agreements
        float         lowest = -1.0f;     ///< the lowest agreed percentage; -1 none
    };

    /// A read judged against a packet value it disagreed with, and the state it
    /// was judged in, for the log.
    struct Disagreement {
        std::uint64_t at_ms = 0;
        State         state = State::Learning;
        Read          read;
        Point         packet;
    };
    static constexpr std::size_t kDisagreementsKept = 8;

    /// Where the current state stands.
    struct Progress {
        State         state = State::Learning;
        std::uint64_t since_ms = 0;       ///< when it came into this state; 0 at the start
        int           agreements = 0;     ///< Learning: so far in this learning
        int           needed = kTrustAgreements;
        int           values = 0;         ///< Learning: distinct values among them
        bool          low_seen = false;   ///< Learning, HP: one agreement under kLowPct
        int           doubt_agreements = 0;   ///< Doubt: in a row so far
        int           distrusts = 0;
    };

    /// `hp`: the HP bar, which also needs an agreement under kLowPct.
    explicit BarCheck(bool hp = true);

    /// A packet value that came at `at_ms`.
    void packet(const Point& p);
    /// The packets' maximum changed with no value coming with it: the values
    /// kept go with none from now on, and are judged by the figures' value
    /// alone -- never against a maximum the bar no longer has.
    void max_changed();
    /// A read of the bar, judged by settle() once it is due.
    void screen(const Read& r);
    /// HP above a maximum the stat sheet gave, seen at `now_ms`: a
    /// disagreement.
    void over_max(std::uint64_t now_ms);
    /// Judge every read due at `now_ms`, the packets being in `epoch` now.
    void settle(std::uint64_t now_ms, std::uint32_t epoch);

    /// With the font there, a read that lacks the figures judged by the bar's
    /// length only under kBarTrueBelowPct: the caption, printed right-aligned
    /// over the bar, paints out the end of its fill, which then measures where
    /// the caption begins (VitalsReader) -- a disagreement nobody made. A
    /// 10-character caption ("9415/10084") begins near 62% of the bar; an
    /// 11-character one ("12643/15280", a value of 10000 or more) at 56.7%
    /// (2026-11-03), so a fuller bar measures 56% -- judged by that under a cut
    /// of 60, it was 4 false disagreements against MP at 66-90%. Under 54 the
    /// fill measures true whatever the caption (VitalsReader::kCaptionShadowPct).
    ///
    /// And with the font there, a read that lacks the figures and measures
    /// empty -- under kEmptyBelowPct, the macro's own "down"
    /// (MacroEngine::kDownHpPct) -- is not judged at all. A bar the game draws
    /// at 0 prints its 0/max, which is read and judged exactly; an empty one
    /// with no figures is the HUD dimmed, as by the fade into a map load, which
    /// VitalsReader::measure takes for an empty trough, and it says nothing of
    /// HP.
    void set_figures_only(bool on) { figures_only_ = on; }
    static constexpr float kBarTrueBelowPct = 54.0f;
    static constexpr float kEmptyBelowPct = 0.5f;

    State state() const { return state_; }
    bool trusted() const { return state_ == State::Trusted; }
    Progress progress() const;
    /// The changes since the last take, oldest first.
    std::vector<Change> take_changes();
    /// The counts since the last take, and from 0 again.
    Counts take_counts();
    /// The disagreements judged since the last take, oldest first, at most kDisagreementsKept.
    std::vector<Disagreement> take_disagreements();

    /// The verdict on one read: the packet value `got` against `read` -- exactly by the
    /// figures when the read has them, otherwise by the percentage bands.
    /// `judged` false when there is nothing to judge by (no maximum).
    /// Figures `figure_slack_pct` points of their maximum or less from the
    /// value, either way, are between (kFigureSlackHpPct, kFigureSlackMpPct);
    /// another maximum disagrees whatever the slack.
    static Verdict judge(const Point& got, const Read& read, bool& judged, float figure_slack_pct = 0.0f);
    /// value / max as a percentage, held to 0..100; -1 with no maximum.
    static float percent(std::uint32_t value, std::uint32_t max);

    static const char* state_name(State s);

private:
    void apply(Verdict v, const Read& r, const Point& p, float packet_pct, bool over, std::uint64_t now);
    void move_to(State to, std::uint64_t now, const Read& r, const Point& p, float packet_pct, bool over,
                 bool in_a_row);
    int recent_disagreements() const;

    bool                         hp_;
    bool                         figures_only_ = false;
    State                        state_ = State::Learning;
    std::uint64_t                since_ms_ = 0;
    std::vector<Point>           points_;      ///< oldest first, at most kPoints
    std::vector<Read>            pending_;     ///< oldest first, at most kPending
    std::array<bool, kRecent>    recent_{};    ///< true: a disagreement; the last kRecent judged
    std::size_t                  recent_n_ = 0;
    int                          row_ = 0;     ///< disagreements in a row
    int                          learn_agree_ = 0;
    int                          learn_exact_ = 0;
    int                          needed_ = kTrustAgreements;
    bool                         low_seen_ = false;
    std::vector<std::uint32_t>   learn_values_;
    int                          doubt_agree_ = 0;
    int                          distrusts_ = 0;
    /// The last disagreement's HUD moment and packet value: the packet value
    /// (with its maximum and epoch), the read's figure and rounded percent.
    bool                         episode_ = false;
    std::uint32_t                episode_value_ = 0;
    std::uint32_t                episode_max_ = 0;
    std::uint32_t                episode_epoch_ = 0;
    int                          episode_figure_ = -1;
    int                          episode_pct_ = -1;
    Counts                       counts_;
    std::vector<std::uint32_t>   count_values_;
    std::vector<Change>          changes_;
    std::vector<Disagreement>    disagreements_;   ///< oldest first, at most kDisagreementsKept
};

/// The read a bar's gate disagreed with, handed to the macro as the packets
/// stop being gone by: the screen's own answer may be no reading for a
/// moment -- a read taken at the resting pace, before the pace was cut back to
/// the run's, is not handed on (VitalsReader::paced_from_ms) -- and a potion
/// due on that read is due now, not one read later.
///
/// Given while it is within `stale_ms` of when it was read and the screen has
/// no newer read of its own to give (`screen_at_ms`, 0 for none); then never
/// again: one hand-over per start().
class HandOver {
public:
    void start(float pct, std::uint64_t read_at_ms);
    void end() { live_ = false; }
    bool live() const { return live_; }
    /// What to give the macro at `now_ms`: the read, or -1.
    float give(std::uint64_t now_ms, std::uint64_t screen_at_ms, std::uint64_t stale_ms);
    std::uint64_t started() const { return started_; }

private:
    float         pct_ = -1.0f;
    std::uint64_t read_at_ = 0;
    bool          live_ = false;
    std::uint64_t started_ = 0;
};

/// The reader's periods over the last kSpanMs, for how long HP may go unread
/// before the character counts as out of the world: one unreadable read at a
/// second's pace is a second and more without HP, which is no reason to hold
/// the macro. Kept wide for kSpanMs after the pace is cut, as the reads before
/// the cut were that far apart.
class PeriodWindow {
public:
    static constexpr std::uint64_t kSpanMs = 3000;
    static constexpr std::size_t   kKept = 16;
    /// The period in use from `now_ms` on.
    void note(std::uint64_t now_ms, int period_ms);
    /// The longest period in use at any time from kSpanMs before `now_ms`
    /// to it; 0 before any note.
    int longest(std::uint64_t now_ms) const;

private:
    struct Span {
        std::uint64_t from_ms = 0;
        int           period_ms = 0;
    };
    std::array<Span, kKept> spans_{};     ///< oldest first
    std::size_t             n_ = 0;
};

/// How long HP may go unread at the reader's last read before the character
/// is out of the world: `floor_ms` (MacroEngine::kOutOfWorldAfterMs), or two
/// reads and their work at the longest period in use, whichever is longer --
/// 1000 at 150 ms, 2250 at a second.
std::uint64_t world_window_ms(int longest_period_ms, std::uint64_t floor_ms);
constexpr std::uint64_t kWorldWindowWorkMs = 250;

/// Whether a bar's packet value may be gone by now: every one of these, at
/// the moment of asking -- never kept from an earlier ask.
struct VitalsUsable {
    bool          use_on = false;          ///< [Packets] vitals is not off
    bool          trusted = false;         ///< its BarCheck says Trusted
    bool          reading = false;         ///< the bars are being read, so the check goes on
    bool          capturing = false;       ///< a capture runs
    /// That capture is the one started for the positions, on the game
    /// window's own process: another client's traffic -- a capture of the
    /// Protocol tab's or Autopilot's lets it in, and so does the window
    /// followed being switched to another client -- is never your HP.
    bool          on_window = false;
    std::uint64_t self_starts = 0;         ///< PacketSelf::start_count: the capture its last batch came from
    std::uint64_t capture_starts = 0;      ///< PacketCapture::start_count
    std::uint32_t value_epoch = 0;         ///< the epoch the value came in
    std::uint32_t epoch = 0;               ///< PacketSelf's epoch now
    std::uint64_t value_ms = 0;            ///< when the value came; 0 never
    std::uint64_t last_server_ms = 0;      ///< the server's last frame over TCP; 0 none
    bool          max_known = false;       ///< from the stat sheet or the bars' figures
};
constexpr std::uint64_t kLinkQuietMs = 5000;
bool vitals_usable(const VitalsUsable& in, std::uint64_t now_ms);
/// Why not, for the log; "" when it is.
const char* vitals_unusable_why(const VitalsUsable& in, std::uint64_t now_ms);

/// Whether the HP/MP reader may rest -- read the bars at
/// vitals_rest_period_ms rather than the run's pace while a job runs.
struct VitalsRest {
    bool          hp_usable = false;
    bool          mp_usable = false;
    std::uint64_t usable_since_ms = 0;     ///< both usable without a break since; 0 not now
    bool          pet_key = false;         ///< an enabled pet potion key, the macro running: its HP is the screen's
    bool          critical = false;        ///< a critical hold
    bool          finding_bars = false;    ///< "Tự tìm" or FIND THE BARS
    std::uint64_t hp_zero_since_ms = 0;    ///< HP read at 0 on the screen since; 0 not
    std::uint64_t death_seen_ms = 0;       ///< the death panel last seen; 0 not since the revive
};
constexpr std::uint64_t kRestAfterUsableMs = 10'000;
constexpr std::uint64_t kRestAfterDeathMs = 60'000;
constexpr int           kRestMostMs = 1000;
bool vitals_rest_allowed(const VitalsRest& in, std::uint64_t now_ms);
/// The resting pace: never quicker than the run's, never slower than
/// kRestMostMs or vitals_idle_ms.
int vitals_rest_period_ms(int run_ms, int idle_ms);

/// Whether a map is loading by the packets: the server's map entry (0x0086,
/// proven) under kLoadHoldMs ago with no report of yours since, on a capture
/// of the game window's own process that the server has spoken on within
/// kLinkQuietMs. It only ever adds a hold to the screen's in-world verdict.
struct MapLoad {
    bool          on_window = false;       ///< a capture runs on the game window's process
    std::uint64_t last_server_ms = 0;
    std::uint64_t entry_ms = 0;            ///< the last map entry; 0 none
    std::uint64_t last_own_ms = 0;         ///< the last report of yours; 0 none
};
constexpr std::uint64_t kLoadHoldMs = 3000;
bool map_load_holds(const MapLoad& in, std::uint64_t now_ms);

/// Whether a bar's figure is `packet` with its leading 1 lost: the packet
/// value less 10^k, 10^k its leading digit's place and that digit a 1 -- 4798
/// for 14798, as the MP caption read all of 2026-11-08's run. A figure under
/// 0, or a packet value under 100, is not. Only for keeping a picture of the
/// caption (packets_vitals.cpp); no check goes by it.
bool figure_lost_leading_one(int figure, std::uint32_t packet);

} // namespace gobot
