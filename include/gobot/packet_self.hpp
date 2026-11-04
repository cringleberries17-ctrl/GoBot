#pragma once

#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/self_frames.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace gobot {

// ---- [Packets] in app.ini ----

/// Whether one use of the game's packets may act: auto lets its gate decide,
/// off keeps the screen deciding, as before. Either way the gate goes on
/// comparing and writing its counts to the log.
enum class PacketUse : std::uint8_t {
    Auto,
    Off,
};

/// "auto" or "off".
const char* packet_use_name(PacketUse use);

/// [Packets] in app.ini, read once as GoBot starts. A key that is missing, or
/// says anything but off, is auto.
struct PacketsSettings {
    PacketUse vitals = PacketUse::Auto;         ///< your HP and MP from the packets, once checked against the bars
    PacketUse bag = PacketUse::Auto;            ///< the bag's potion counts, for "Low" and the shop
    PacketUse detector_rest = PacketUse::Auto;  ///< the name-plate reader slower while the packets steer
};
PacketsSettings packets_settings(const IniFile& app_ini);
/// "vitals auto, bag auto, detector_rest off", for the log.
std::string describe_packets_settings(const PacketsSettings& s);

// ---- what the packets say of your own character ----

/// Your HP and MP, the bag and the link to the server, as the game's frames
/// give them (gobot/self_frames.hpp), with when each came and the epoch it
/// came in.
///
/// None of it is proven on today's server, so nothing here acts. It is kept
/// for the gates that compare it with the screen first (the packets plan,
/// items D and E), and written to the log.
///
/// The epoch says whether a value may still be the game's now. It rises at
/// every gap in what the capture saw -- frames dropped, a wrapper that would
/// not unpack, the server's stream resynced (mark_gap) -- at relink() and
/// reset(), and whenever the capture's start count changes, whoever started
/// it. A value from an older epoch is kept for the log, and goes by nothing
/// until a frame brings it again.
///
/// The capture thread feeds it (on_packets) while GoBot's reads it: one lock,
/// every answer a copy, and no string built or line logged under the lock.
class PacketSelf {
public:
    static constexpr std::size_t kHistory = 64;       ///< HP/MP points kept
    static constexpr std::size_t kPresses = 128;      ///< macro presses kept
    static constexpr std::size_t kBagFrames = 8;      ///< bag frames kept
    static constexpr std::size_t kTickTimes = 64;     ///< 0x03C6 times kept, for ticks a minute
    static constexpr int         kFirstFrames = 2;    ///< first frames of a kind written to the log
    /// A map entry (0x0086) with at least two of the wrapped 0x001E, 0x0042,
    /// 0x0051 and 0x0050 within this of it is a world-entry burst: the
    /// character came into the world, at a login or through a door.
    static constexpr std::uint64_t kBurstMs = 2000;
    static constexpr std::size_t kOpcodeCount = 10;   ///< opcodes()

    /// Where the maxima came from.
    enum class MaxFrom : std::uint8_t {
        None,        ///< none yet
        StatSheet,   ///< 0x0050
        Seen,        ///< the highest HP or MP a frame has had: maybe too low
        Screen,      ///< the bars' own figures (set_max_from_screen)
    };

    struct Vitals {
        std::uint32_t hp = 0;
        std::uint32_t mp = 0;
        std::uint32_t max_hp = 0;       ///< never below hp: max(max, hp)
        std::uint32_t max_mp = 0;
        std::uint64_t hp_ms = 0;        ///< when HP last came; 0 for never
        std::uint64_t mp_ms = 0;
        std::uint64_t max_ms = 0;
        std::uint32_t hp_epoch = 0;     ///< the epoch HP came in
        std::uint32_t mp_epoch = 0;
        MaxFrom       max_from = MaxFrom::None;
        /// HP above a maximum the stat sheet gave: the sheet is out of date,
        /// or the layout is not what it is taken for.
        std::uint64_t over_max = 0;
        /// 0x02F0's MP, for the log alone.
        std::uint32_t udp_mp = 0;
        std::uint64_t udp_mp_ms = 0;
        /// The seq of the newest HP/MP point (VitalsPoint::seq); 0 for none since
        /// the start or reset().
        std::uint64_t points = 0;
    };

    /// One HP/MP frame, as it came.
    struct VitalsPoint {
        std::uint64_t at_ms = 0;
        std::uint32_t hp = 0;
        std::uint32_t mp = 0;
        /// Its place among every HP/MP frame ever taken, from 1, never reset: two
        /// frames in one millisecond (one segment, one read of the capture) told apart.
        std::uint64_t seq = 0;
    };

    struct Bag {
        bool          known = false;    ///< a bag frame has come since the start or reset()
        BagFrame      slots{};
        std::uint64_t at_ms = 0;
        std::uint32_t epoch = 0;
        std::uint32_t gold = 0;         ///< 0x006B, for the log
        std::uint64_t gold_ms = 0;
    };

    /// One bag frame, as it came.
    struct BagSeen {
        std::uint64_t at_ms = 0;
        BagFrame      slots{};
    };

    /// A key the macro pressed, and when (set_on_press).
    struct Press {
        std::uint32_t vk = 0;
        std::uint64_t at_ms = 0;
    };

    /// The link to the server, as its frames show it.
    struct Link {
        std::uint64_t last_server_ms = 0;   ///< any frame from the server over TCP
        std::uint64_t ticks = 0;            ///< the server's 0x03C6, once a second
        std::uint64_t ticks_last_min = 0;   ///< of them, within 60 s of the moment asked at
        std::uint64_t last_tick_ms = 0;
        std::uint64_t entries = 0;          ///< map entries, 0x0086
        std::uint64_t last_entry_ms = 0;
        std::uint64_t bursts = 0;           ///< world-entry bursts (kBurstMs)
        std::uint64_t selected = 0;         ///< 0x0014 inside 0x0081: a character selected
        std::uint64_t channel = 0;          ///< 0x0014 plain: a channel switch
    };

    struct Stats {
        /// One opcode's frames: read, refused for their size, or for their
        /// side (side, transport or depth).
        struct Frames {
            std::uint16_t opcode = 0;
            std::uint64_t used = 0;
            std::uint64_t wrong_size = 0;
            std::uint64_t wrong_side = 0;
        };
        std::array<Frames, kOpcodeCount> frames{};    ///< in opcodes()' order
        Frames of(std::uint16_t opcode) const;        ///< all 0 for one not listed

        std::uint64_t gaps = 0;             ///< mark_gap()
        std::uint64_t last_gap_ms = 0;
        const char*   last_gap_why = "";    ///< a string literal, as mark_gap was given it
        std::uint64_t relinks = 0;
        std::uint64_t resets = 0;
        std::uint64_t starts_seen = 0;      ///< capture start counts that raised the epoch
    };

    /// How a kind's first frames go to the log.
    enum class FirstFrame : std::uint8_t {
        Never,     ///< not at all: it may hold a name, or is not of interest
        Hex,       ///< its payload's bytes
        Decoded,   ///< its fields read, never its bytes
    };

    PacketSelf();
    PacketSelf(const PacketSelf&) = delete;
    PacketSelf& operator=(const PacketSelf&) = delete;

    /// One batch, from a capture whose start count is `start_count`
    /// (PacketCapture::start_count): a count other than the last raises the
    /// epoch before anything of the batch is kept. The first kFirstFrames
    /// frames of each kind first_frame_kind() names are written to the log,
    /// once per game process.
    void on_packets(const std::vector<GamePacket>& all, std::uint64_t start_count);

    /// A key the macro pressed at `at_ms` (MacroEngine::set_on_press), from
    /// the macro's threads.
    void note_press(std::uint32_t vk, std::uint64_t at_ms);

    /// Something of the stream was missed at `now` -- `why` a string literal
    /// -- and nothing kept may be the game's now: the epoch rises.
    void mark_gap(std::uint64_t now, const char* why);

    /// The capture started again on the same game process with no new
    /// connection: what is kept stays for the log, and the epoch rises.
    void relink();

    /// A new game process or connection: the values, the bag and the first
    /// frames said are forgotten, and the epoch rises. The counts and the
    /// macro's presses are kept.
    void reset();

    /// The bars' own figures, for a maximum no stat sheet has given: taken
    /// only while none has.
    void set_max_from_screen(std::uint32_t max_hp, std::uint32_t max_mp, std::uint64_t at_ms);

    std::uint32_t epoch() const;
    /// The capture start count of the last batch; 0 before any.
    std::uint64_t start_count() const;
    Vitals vitals() const;
    /// The last kHistory HP/MP frames, oldest first.
    std::vector<VitalsPoint> history() const;
    Bag bag() const;
    /// The last kBagFrames bag frames, oldest first.
    std::vector<BagSeen> bag_frames() const;
    /// The macro's presses at or after `from_ms`, oldest first.
    std::vector<Press> presses_since(std::uint64_t from_ms) const;
    /// With ticks_last_min counted back from `now`.
    Link link(std::uint64_t now) const;
    Stats stats() const;

    /// Every opcode any part of the packets plan reads, for Apply's opcode
    /// filter: 0x0051, 0x0050, 0x02F0, 0x0068, 0x006B, 0x0077, 0x005A,
    /// 0x002E, 0x0014 and 0x03C6. PacketWorld::opcodes() has the rest.
    static const std::vector<std::uint16_t>& opcodes();

    /// How the first frames of `opcode` go to the log. Never for 0x0014,
    /// 0x001E and 0x0065, which carry names, nor for any kind not listed.
    static FirstFrame first_frame_kind(std::uint16_t opcode);

private:
    struct First {
        GamePacket packet;
    };

    void take(const GamePacket& p, std::vector<First>& firsts);     ///< under mutex_
    void count(std::uint16_t opcode, FrameFit fit);                 ///< under mutex_
    void burst_kind(std::size_t k, std::uint64_t t);                ///< under mutex_
    void check_burst();                                             ///< under mutex_

    mutable std::mutex                          mutex_;
    std::uint32_t                               epoch_ = 1;
    std::uint64_t                               start_count_ = 0;
    Vitals                                      vitals_;
    std::array<VitalsPoint, kHistory>           history_{};
    std::size_t                                 history_n_ = 0;     ///< points ever pushed
    std::uint64_t                               points_taken_ = 0;  ///< HP/MP points ever taken; reset() keeps it
    Bag                                         bag_;
    std::array<BagSeen, kBagFrames>             bag_seen_{};
    std::size_t                                 bag_seen_n_ = 0;
    std::array<Press, kPresses>                 presses_{};
    std::size_t                                 presses_n_ = 0;
    Link                                        link_;
    std::array<std::uint64_t, kTickTimes>       tick_times_{};
    std::size_t                                 tick_n_ = 0;
    /// When each of wrapped 0x001E, 0x0042, 0x0051 and 0x0050 last came,
    /// and whether the last map entry has been counted as a burst.
    std::array<std::uint64_t, 4>                burst_ms_{};
    bool                                        burst_counted_ = false;
    Stats                                       stats_;
    /// How many first frames of each opcode have gone to the log, by opcode.
    std::vector<std::pair<std::uint16_t, int>>  firsts_said_;
};

} // namespace gobot
