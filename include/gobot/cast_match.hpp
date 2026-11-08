#pragma once

#include "gobot/common.hpp"
#include "gobot/self_frames.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gobot {

class PacketWorld;

// ---- your casts and your buffs, from the packets (the packets plan, F and H) ----
//
// Your client tells the server of every skill it casts, over UDP, and the
// server answers a buff with a time and a stat. All of it was found in the
// P0-2 recordings of 2026-10-18 (docs/protocol-findings.md §4e); of a cast,
// only its x, y, its size and how often it comes are proven on today's server.
// So nothing here acts. Each cast is paired with the macro's press before it,
// to learn which skill each key casts and how far away a named target was,
// and a buff's frames are set beside its key: for the log, until the owner's
// logs settle the layouts and the unit of a buff's time.
//
// The offsets count from the frame's first byte, header included, as the docs
// count them; a payload starts after the 8-byte header.

/// 0x0031 from your client: one skill cast.
struct CastFrame {
    std::uint32_t id = 0;           ///< your id, at 12
    std::uint16_t x = 0;            ///< where you cast it, whole world pixels, at 16
    std::uint16_t y = 0;            ///< at 18
    std::uint32_t skill = 0;        ///< the skill's id, at 20
    std::uint8_t  kind = 0;         ///< byte 24: 1 an attack, 2 a buff
    bool          named = false;    ///< byte 26 is 2: the cast names a target monster
    std::uint16_t slot = 0xFFFF;    ///< that monster's slot, u16 at 28; 0xFFFF when none is named
    std::uint32_t counter = 0;      ///< the client's action counter, u32 at 88
};

/// 0x0077 from the server: a skill and a value, after a buff. The value was
/// 72,000 for both buffs recorded; whether it is in seconds, milliseconds or
/// steps of 25 ms is not known.
struct BuffTimeFrame {
    std::uint32_t skill = 0;        ///< at 12
    std::uint32_t value = 0;        ///< at 16
};

/// 0x005A from the server: what a buff changed, and the value it left.
struct BuffStatFrame {
    std::uint32_t skill = 0;        ///< at 12
    std::int32_t  change = 0;       ///< at 20: -1002, then +1060, in the recording
    std::uint32_t value = 0;        ///< at 24: 2150, then 4212
};

/// 0x0031 from your client over UDP, straight off a datagram, 92 bytes. x, y,
/// the size and the count are PROVEN; the skill, the kind, the target and the
/// counter DOCUMENTED. Anything else gives nothing, and `fit`, when given,
/// says why.
std::optional<CastFrame> read_cast(const GamePacket& p, FrameFit* fit = nullptr);

/// 0x0077 from the server over TCP, plain, 24 bytes. DOCUMENTED.
std::optional<BuffTimeFrame> read_buff_time(const GamePacket& p, FrameFit* fit = nullptr);

/// 0x005A from the server over TCP, inside 0x0081, 32 bytes. DOCUMENTED.
std::optional<BuffStatFrame> read_buff_stat(const GamePacket& p, FrameFit* fit = nullptr);

/// 0x002E from your client over UDP, plain, 72 bytes: one followed each buff
/// cast in the recording. Nothing of it is read; it is only counted.
FrameFit buff_follow_fit(const GamePacket& p);

/// Your casts, each paired with the press of the macro's that made it.
///
/// The capture thread hands it every batch (on_packets); GoBot's thread pairs
/// the macro's presses with them once a second (pair) and takes the minute's
/// counts for the log (take_minute). One lock, every answer a copy.
///
/// A press is paired with the first cast not yet paired that came from
/// kEarlyMs before it to kPairMs after it: the press is stamped as its key
/// goes down (MacroEngine::set_on_press), and kEarlyMs is slack for the two
/// clocks' stamps. Only presses of keys that can cast are handed to it -- a
/// potion sends no cast, and would take the next skill's (GuiApp::more_tick).
/// A press is paired only kSettleMs after it, when its cast has surely come. A
/// key's skill is learned from its last kVotes pairs: kLearnPairs of them, and
/// at least kLearnPct% for one skill.
class CastMatch {
public:
    static constexpr std::uint64_t kPairMs = 700;
    static constexpr std::uint64_t kEarlyMs = 200;
    static constexpr std::uint64_t kSettleMs = 1000;
    static constexpr int           kLearnPairs = 5;
    static constexpr int           kLearnPct = 80;
    static constexpr std::size_t   kVotes = 20;     ///< a key's last pairs that vote for its skill
    static constexpr std::size_t   kCasts = 256;    ///< casts kept for pairing
    static constexpr std::size_t   kRecent = 8;     ///< a key's last pairs kept, for the buffs' times
    static constexpr std::size_t   kKeys = 32;      ///< keys followed; a press of any other is passed over
    static constexpr std::size_t   kSkills = 32;    ///< skills whose reach is kept

    /// A key the macro pressed, and when.
    struct Press {
        std::uint32_t vk = 0;
        std::uint64_t at_ms = 0;
    };

    /// One cast, as it came.
    struct Cast {
        std::uint64_t at_ms = 0;
        CastFrame     frame;
        /// It named a target, and the packet world had that monster at the
        /// cast's own time: how far it was from where you cast, each way.
        bool          reach_known = false;
        int           dx = 0;
        int           dy = 0;
        std::uint32_t vk = 0;           ///< the key whose press it was paired with; 0 for none
        std::uint64_t press_ms = 0;     ///< that press's time
        bool          settled = false;  ///< every press that could take it has been paired
    };

    /// One press of a key and the cast paired with it.
    struct Paired {
        std::uint64_t press_ms = 0;
        std::uint64_t cast_ms = 0;
        std::uint32_t skill = 0;
    };

    /// One key the macro pressed.
    struct Key {
        std::uint32_t vk = 0;
        bool          buff = false;         ///< a buff key of the macro's, as its last press was paired
        std::uint64_t pressed = 0;          ///< this minute
        std::uint64_t cast = 0;             ///< this minute: of those, paired with a cast
        std::uint64_t pressed_all = 0;
        std::uint64_t cast_all = 0;
        std::uint32_t skill = 0;            ///< learned; 0 until then
        std::uint32_t top_skill = 0;        ///< the skill most of its last pairs cast
        int           top_votes = 0;        ///< how many of them
        int           pairs = 0;            ///< of its last kVotes pairs
        std::uint64_t attacks = 0;          ///< its casts by kind, all: 1
        std::uint64_t buffs = 0;            ///< 2
        std::uint64_t other_kinds = 0;      ///< anything else
        std::deque<std::uint32_t> votes;    ///< the skills of its last kVotes pairs
        std::vector<Paired> recent;         ///< its last kRecent pairs, oldest first
        /// A buff key's bursts judged this minute (Burst), and of them those
        /// a kind-2 cast came with: whichever press it was paired with.
        std::uint64_t bursts = 0;
        std::uint64_t bursts_seen = 0;

        /// A buff key whose casts are not all of kind 2, or a skill key with
        /// one that is.
        bool kind_mismatch() const { return buff ? attacks + other_kinds > 0 : buffs > 0; }
    };

    /// One burst of a buff key's presses -- the macro presses a buff several
    /// times, buff_press_gap_ms apart -- and whether a cast of yours of kind
    /// 2 came with it: from kEarlyMs before its first press to kPairMs after
    /// its last, whichever press the pairing gave it to (2026.11.04d: buff S
    /// "cast" at 20:39:13 on 2026-11-04 left no buff cast in the packets).
    /// Log only: the kind byte is DOCUMENTED, not PROVEN.
    struct Burst {
        std::uint32_t vk = 0;
        std::uint64_t first_ms = 0;
        std::uint64_t last_ms = 0;
        int           presses = 0;
        bool          seen = false;         ///< a kind-2 cast in its window
        std::uint64_t cast_ms = 0;          ///< the first such cast's time
        std::uint32_t skill = 0;            ///< ... and its skill
    };
    /// Presses of one buff key no further apart than this are one burst.
    static constexpr std::uint64_t kBurstGapMs = 2000;

    /// How far from you a skill's named targets were, the farthest seen.
    struct Reach {
        std::uint32_t skill = 0;
        std::uint64_t casts = 0;
        int           most_dx = 0;
        int           most_dy = 0;
    };

    /// One minute, for the log.
    struct Minute {
        std::uint64_t casts = 0;
        std::uint64_t attacks = 0;
        std::uint64_t buffs = 0;
        std::uint64_t other_kinds = 0;
        std::uint64_t named = 0;
        std::uint64_t named_unplaced = 0;   ///< named, the target not in the packet world at the time
        std::uint64_t unpaired = 0;         ///< casts no press of the macro's took
        std::uint64_t wrong_size = 0;       ///< 0x0031 from your client of another size
        std::vector<Key>   keys;            ///< every key ever pressed, in the order first pressed
        std::vector<Reach> reach;           ///< every skill a target was placed for, since the start
    };

    CastMatch() = default;
    CastMatch(const CastMatch&) = delete;
    CastMatch& operator=(const CastMatch&) = delete;

    /// One batch, on the capture thread, after `world` has had it: each cast
    /// kept, and a named target looked for where `world` has it at the cast's
    /// own time.
    void on_packets(const std::vector<GamePacket>& all, const PacketWorld& world);

    /// One cast at `at_ms`, its target's distance given when known.
    void add(std::uint64_t at_ms, const CastFrame& c, bool reach_known = false, int dx = 0, int dy = 0);

    /// The macro's presses from pair_from() on -- any others are passed over
    /// -- each paired with its cast once kSettleMs has gone by since it, at
    /// `now`. `buff_keys` are the macro's buff keys now. A cast no press can
    /// take any more is counted as unpaired.
    void pair(const std::vector<Press>& presses, const std::vector<std::uint32_t>& buff_keys, std::uint64_t now);

    /// The time the next pair() starts from: presses before it are paired.
    std::uint64_t pair_from() const;

    /// The buff keys' bursts judged since the last call, oldest first: each
    /// once no press of the pairing can join it any more -- kBurstGapMs gone
    /// by after its last -- by which time its casts have come.
    std::vector<Burst> take_bursts();

    /// The skill `vk` is learned to cast; 0 for none.
    std::uint32_t learned(std::uint32_t vk) const;

    std::vector<Key>  keys() const;
    std::vector<Cast> casts() const;

    /// The minute's counts, and a new minute: the keys' and the casts' counts
    /// of this minute start again from 0; what was learned stays.
    Minute take_minute();

private:
    Key* key_of(std::uint32_t vk);                  ///< under mutex_; nullptr past kKeys
    static void vote(Key& k, std::uint32_t skill);
    /// Under mutex_: `b` judged by the casts kept, counted to its key and
    /// set among those take_bursts gives.
    void judge(Burst b);

    mutable std::mutex mutex_;
    std::deque<Cast>   casts_;
    std::vector<Key>   keys_;
    std::vector<Reach> reach_;
    Minute             minute_;                     ///< its counts only
    std::uint64_t      pair_from_ = 0;
    std::vector<Burst> open_bursts_;                ///< one a buff key at most, not judged yet
    std::vector<Burst> judged_;                     ///< judged, for take_bursts
};

/// The frames that follow your buffs: 0x0077 and 0x005A from the server, each
/// naming a skill, and your client's 0x002E. The capture thread hands it every
/// batch; GoBot's thread takes the minute's for the log. One lock.
class BuffWatch {
public:
    /// A 0x002E this long after a buff cast of yours follows it.
    static constexpr std::uint64_t kFollowMs = 2000;
    static constexpr std::size_t   kSkills = 32;     ///< skills followed; frames naming any other are counted

    /// What the frames said of one skill.
    struct Skill {
        std::uint32_t skill = 0;
        std::uint64_t times = 0;        ///< 0x0077 naming it this minute
        std::uint32_t time_value = 0;   ///< the last one's value
        std::uint64_t time_ms = 0;      ///< and when it came; 0 for never
        std::uint64_t stats = 0;        ///< 0x005A naming it this minute
        std::int32_t  change = 0;       ///< the last one's change
        std::uint32_t value = 0;        ///< and the value it left
        std::uint64_t stat_ms = 0;      ///< when it came; 0 for never
    };

    struct Minute {
        std::vector<Skill> skills;          ///< every skill named since the start, in the order first named
        std::uint64_t buff_casts = 0;       ///< your casts of kind 2 this minute
        std::uint64_t follows = 0;          ///< 0x002E this minute
        std::uint64_t follows_after = 0;    ///< of them, within kFollowMs of a buff cast
        std::uint64_t other_skills = 0;     ///< frames naming a skill past kSkills
        std::uint64_t wrong_size = 0;       ///< 0x0077, 0x005A or 0x002E of another size
    };

    BuffWatch() = default;
    BuffWatch(const BuffWatch&) = delete;
    BuffWatch& operator=(const BuffWatch&) = delete;

    void on_packets(const std::vector<GamePacket>& all);

    /// The minute's counts, and a new minute; the skills' last values stay.
    Minute take_minute();

private:
    Skill* skill_of(std::uint32_t skill);           ///< under mutex_; nullptr past kSkills

    mutable std::mutex mutex_;
    Minute             minute_;
    std::uint64_t      last_buff_cast_ms_ = 0;
};

/// How long after a key's press a buff's 0x0077 may come and be that press's.
constexpr std::uint64_t kBuffAfterMs = 10'000;

/// One buff, as the minute's frames tell of it: its key, when one has been
/// learned for its skill.
struct BuffSeen {
    std::uint32_t vk = 0;               ///< the key learned for it; 0 for none
    std::uint32_t skill = 0;
    bool          timed = false;        ///< a 0x0077 named it this minute
    std::uint32_t time_value = 0;
    /// How long after the key's last press before it the 0x0077 came, when a
    /// press of the key was paired with a cast of the skill within
    /// kBuffAfterMs before it; -1 otherwise.
    std::int64_t  after_press_ms = -1;
    bool          stat = false;         ///< a 0x005A named it this minute
    std::int32_t  change = 0;
    std::uint32_t value = 0;
};

/// Each skill a 0x0077 or 0x005A named this minute, beside the key learned to
/// cast it (a buff key first), in the order the skills were first named.
std::vector<BuffSeen> pair_buffs(const std::vector<CastMatch::Key>& keys, const BuffWatch::Minute& m);

/// "Casts (packets): ..." for one minute; "" when nothing was cast or pressed
/// in it.
std::string casts_line(const CastMatch::Minute& m);

/// "Buff (packets): key S -- ..." for one burst judged: the kind-2 cast seen
/// and how long after the first press, or none in its window -- maybe not
/// cast at all, which nothing acts on.
std::string burst_line(const CastMatch::Burst& b);

/// "Buffs (packets): ..." for one minute, with each buff key's interval in the
/// macro (key, seconds); "" when nothing of a buff came in it.
std::string buffs_line(const std::vector<BuffSeen>& seen, const BuffWatch::Minute& m,
                       const std::vector<std::pair<std::uint32_t, std::uint32_t>>& intervals_s);

} // namespace gobot
