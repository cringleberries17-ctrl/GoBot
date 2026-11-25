#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gobot {

// ---- your HP and MP from the packets ----
//
// The packets plan, item D. Wrapped 0x0051 and 0x0050 say your HP and MP, and
// 0x0051's MP is kept up by 0x02F0 in a fight (PacketSelf). Until 2026.11.16b
// nothing went by them before a gate had compared them, read by read, with the
// bars on the screen. On 2026-11-17 that gate kept MP on the screen all night:
// with the skills cast one after another MP moved too often for a read to be
// judged, and the EXP bar's glowing tip over MP's figures read 3% where the
// packets said 92-100% -- the MP potion went 60 times a minute. The owner's
// call that day: HP and MP go by the packets whenever they have them, with no
// gate (vitals_usable); the bars decide only when they do not -- no capture, a
// gap in it, nothing since a start or a map load, the server quiet. Should the
// server ever send HP or MP otherwise, nothing here acts on it: the minute's
// line sets the bars' figures beside the packets (BarCompare), for the log.
//
// Everything here is pure: given times on now_ms()'s clock, no thread, no
// window, no capture. GoBot's window (packets_vitals.cpp) feeds it and asks it.

/// value / max as a percentage, held to 0..100; -1 with no maximum.
float vitals_percent(std::uint32_t value, std::uint32_t max);

/// One bar's figures off the screen beside its packet value, for the log
/// alone (2026.11.17): nothing goes by what it finds.
///
/// A read is compared kLeadMs after it was taken (plus kJudgeSlackMs, as
/// frames reach GoBot in batches), against every packet value of the current
/// epoch from the one in force kBeforeMs before the read to the last within
/// kLeadMs after it: the HUD runs ahead of 0x0051 by a hit or a cast's cost,
/// which 0x0051 reached 0.4-0.78 s after the read (2026-11-03). Same: the
/// figures are one of those values exactly, and the maximum too when the
/// packets know theirs. Short: one of them with digits lost -- the reader's
/// miss (figure_lost_a_digit, figure_lost_leading_digits), not the packets
/// wrong. Other: neither. Only figures the read made out itself are
/// compared; a read with no value of this epoch around it is not.
class BarCompare {
public:
    static constexpr std::uint64_t kBeforeMs = 400;
    static constexpr std::uint64_t kLeadMs = 900;
    static constexpr std::uint64_t kJudgeSlackMs = 100;
    static constexpr std::size_t   kPoints = 64;      ///< packet values kept
    static constexpr std::size_t   kPending = 32;     ///< reads waiting to be compared
    static constexpr std::size_t   kOddKept = 8;      ///< reads not Same kept for the log

    /// A packet value, as it came.
    struct Point {
        std::uint64_t at_ms = 0;
        std::uint32_t value = 0;
        std::uint32_t max = 0;
        bool          max_known = false;   ///< from the stat sheet or the bars' figures, not the highest seen
        std::uint32_t epoch = 0;
    };

    /// The figures a read of the bar made out itself.
    struct Read {
        std::uint64_t read_at_ms = 0;
        int           figure = -1;
        int           figure_max = -1;
    };

    enum class Verdict : std::uint8_t { Same, Short, Other };

    /// A read that was not Same, and the packet value it came nearest.
    struct Odd {
        Verdict verdict = Verdict::Other;
        Read    read;
        Point   packet;
    };

    /// What was compared, since take_counts().
    struct Counts {
        std::uint64_t same = 0;
        std::uint64_t short_digits = 0;
        std::uint64_t other = 0;
        std::uint64_t no_packet = 0;    ///< not compared: no value of this epoch around the read
    };

    /// A packet value that came at `p.at_ms`.
    void packet(const Point& p);
    /// The packets' maximum changed with no value coming with it: the values
    /// kept go with none from now on, and are compared by the figure alone.
    void max_changed();
    /// A read's figures; one without them is not kept.
    void screen(const Read& r);
    /// Compare every read due at `now_ms`, the packets being in `epoch` now.
    void settle(std::uint64_t now_ms, std::uint32_t epoch);

    Counts take_counts();
    /// The reads not Same since the last take, oldest first, at most kOddKept.
    std::vector<Odd> take_odd();

    /// One packet value against one read's figures.
    static Verdict compare(const Point& p, const Read& r);

private:
    std::vector<Point> points_;    ///< oldest first, at most kPoints
    std::vector<Read>  pending_;   ///< oldest first, at most kPending
    Counts             counts_;
    std::vector<Odd>   odd_;
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

/// Whether a bar's packet value goes now -- the packets have your HP or MP:
/// every one of these, at the moment of asking, never kept from an earlier
/// ask. Nothing about the bars on the screen: the packets are gone by
/// whatever the bars read (2026.11.17).
struct VitalsUsable {
    bool          use_on = false;          ///< [Packets] vitals is not off
    bool          capturing = false;       ///< a capture runs
    /// That capture is the one started for the positions, on the game
    /// window's own process: another client's traffic -- a capture started
    /// for anything else lets it in, and so does the window followed being
    /// switched to another client -- is never your HP.
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
    /// An enabled pet potion key, the macro running: the pet's HP is only on
    /// the screen, so the rest is kRestPetMs, not refused (2026.11.16).
    bool          pet_key = false;
    bool          critical = false;        ///< a critical hold
    bool          finding_bars = false;    ///< "Tự tìm" or FIND THE BARS
    std::uint64_t hp_zero_since_ms = 0;    ///< HP read at 0 on the screen since; 0 not
    std::uint64_t death_seen_ms = 0;       ///< the death panel last seen; 0 not since the revive
};
constexpr std::uint64_t kRestAfterUsableMs = 10'000;
constexpr std::uint64_t kRestAfterDeathMs = 60'000;
constexpr int           kRestMostMs = 1000;
/// The rest with a pet potion key: the pet's HP read twice a second, well
/// inside the macro's 1.5 s before a read is too old to drink on
/// (MacroEngine::kVitalsStaleMs) -- 6 reads a second down to 2 on a slow PC.
constexpr int           kRestPetMs = 500;
bool vitals_rest_allowed(const VitalsRest& in, std::uint64_t now_ms);
/// The resting pace: never quicker than the run's, never slower than
/// kRestMostMs -- kRestPetMs with a pet potion key -- or vitals_idle_ms.
int vitals_rest_period_ms(int run_ms, int idle_ms, bool pet_key = false);

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
/// caption (packets_vitals.cpp); nothing goes by it.
bool figure_lost_leading_one(int figure, std::uint32_t packet);

/// Whether a bar's figure is `packet` with one of its digits lost, any one
/// (2026.11.13): 1552 or 1152 for 11552, 1859 for 18590. The reader passes
/// over a digit it took for a lookalike ('l' for '1', 'O' or 'D' for '0'),
/// and where that leaves no hole on the pitch -- the first digit, the last
/// -- the figure comes out short by that digit. BarCompare counts such a
/// figure, or such a maximum, as Short: a misread of the bar, not the
/// packets wrong. A figure under 0, or a packet value under 100, is not.
bool figure_lost_a_digit(int figure, std::uint32_t packet);

/// Whether a bar's figure is `packet` with one or more of its first digits
/// lost (2026.11.16): packet % 10^k, k from 1 up while 10^k is no more than
/// the value -- 462 for 16462, 59 for 9859, 7616 for 17616. A user's MP
/// caption read so, its first digits too far from the rest for a hole on the
/// pitch to show. BarCompare counts such a figure, or such a maximum, as
/// Short, as it does figure_lost_a_digit's. A figure of 0 or under, or a
/// packet value under 100, is not: 0 is HP at 0.
bool figure_lost_leading_digits(int figure, std::uint32_t packet);

} // namespace gobot
