#pragma once

#include "gobot/common.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gobot {

/// How the game delimits messages inside its TCP stream.
///
/// GhostOnline's exact framing is not published, and the reference client did
/// not carry it either — it received the opcode allowlist from a remote server
/// at login. So framing is data, not code: `gobot_sniff --detect` scores
/// candidate layouts against a real capture and prints the winner as an INI
/// block you paste into configs/protocol.ini.
///
/// Layout assumed:
///
///     [ ...header... ][ ......... body ......... ]
///        ^len   ^op
///     |<-- header_size -->|
///
/// `length_field` holds either the whole frame size (length_includes_header)
/// or just the body size. `length_adjust` absorbs the off-by-N conventions
/// that show up in practice (e.g. length counted from after the length field).
struct FramingSpec {
    std::uint32_t header_size = 4;

    std::uint32_t length_offset = 0;
    std::uint32_t length_size   = 2;      ///< 1, 2 or 4
    bool          length_le     = true;
    bool          length_includes_header = true;
    std::int32_t  length_adjust = 0;

    std::uint32_t opcode_offset = 2;
    std::uint32_t opcode_size   = 2;      ///< 1 or 2
    bool          opcode_le     = true;

    std::uint32_t min_frame = 4;
    std::uint32_t max_frame = 65535;

    /// Offset of a header check word equal to the sum of the other header
    /// u16s, or kNoChecksum.
    ///
    /// Where a protocol provides one this is worth more than every heuristic
    /// combined. Length alone cannot tell a real frame header from two bytes
    /// that happen to read as a plausible size, so a scanner that has lost
    /// alignment will latch onto a bogus large length and swallow hundreds of
    /// real frames whole. A checksum makes each candidate boundary verifiable
    /// on its own: wrong guesses survive with probability 1/65536 instead of
    /// routinely.
    static constexpr std::uint32_t kNoChecksum = 0xFFFFFFFFu;
    std::uint32_t checksum_offset = kNoChecksum;

    bool has_checksum() const { return checksum_offset != kNoChecksum; }

    /// Does the header at `p` satisfy the checksum? True when none is
    /// configured, so callers need not special-case it.
    bool checksum_ok(const std::uint8_t* p, std::size_t available) const;

    /// Total frame size implied by a raw length field value.
    std::uint32_t total_size(std::uint64_t raw_length) const {
        std::int64_t total = static_cast<std::int64_t>(raw_length) + length_adjust;
        if (!length_includes_header) total += header_size;
        return total < 0 ? 0u : static_cast<std::uint32_t>(total);
    }

    /// Bytes that one opcode carries *past* its declared length.
    ///
    /// On this game's server stream, 0x0081 is followed by twelve bytes the
    /// length field does not count. Without this the scanner finishes the
    /// frame, meets twelve bytes of apparent garbage and resyncs -- and a
    /// resync does not cost one frame but a position: it slides a byte at a
    /// time until something parses again, swallowing whole real frames on the
    /// way. One 0x0081 every few hundred frames was costing most of the
    /// stream, and the loss looked like a wrong FramingSpec rather than a
    /// right one with an exception in it.
    ///
    /// Measured rather than guessed. Across four captures a trailer of 12
    /// takes the resync count to exactly zero at 100% coverage, while 11 and
    /// 13 leave hundreds and thousands of resyncs respectively.
    std::uint16_t trailer_opcode = 0;
    std::uint32_t trailer_bytes  = 0;

    bool has_trailer() const { return trailer_bytes != 0; }

    /// Total frame size, given the opcode that header carries.
    std::uint32_t total_size_for(std::uint64_t raw_length, std::uint64_t opcode) const {
        const std::uint32_t t = total_size(raw_length);
        if (trailer_bytes != 0 && opcode == trailer_opcode) return t + trailer_bytes;
        return t;
    }

    bool plausible() const;
    std::string describe() const;

    /// Serialise as a `[Framing]` INI block.
    std::string to_ini() const;
};

/// Outcome of pulling frames off the front of a stream buffer.
struct FrameScanResult {
    std::vector<GamePacket> packets;
    std::size_t consumed = 0;        ///< bytes to erase from the front
    std::size_t resyncs  = 0;        ///< bytes discarded hunting for alignment
    bool        locked   = false;    ///< pass back as `locked` on the next call
};

/// Extract as many whole frames as `buf` currently holds.
///
/// A frame whose declared size is outside [min_frame, max_frame] means we lost
/// alignment; the scanner then slides forward a byte at a time until a
/// plausible header appears, counting the skipped bytes in `resyncs`. A high
/// resync count is the signal that the FramingSpec is wrong (or the stream is
/// encrypted).
///
/// `locked` says whether the caller has previously confirmed frame alignment
/// on this stream, and it changes what an over-long frame means:
///
///   locked   — the frame is real and simply hasn't fully arrived. Wait.
///   unlocked — the bytes merely parse as a length. Slide past them.
///
/// Getting this backwards is costly in both directions: waiting while
/// unlocked stalls forever on a length that was never real, and sliding while
/// locked shreds a large packet that was only half-delivered. Start a stream
/// with `locked = false` — attaching to a game that is already connected drops
/// you mid-frame — and thread `FrameScanResult::locked` back in from then on.
FrameScanResult extract_frames(const FramingSpec& spec,
                               const std::uint8_t* buf,
                               std::size_t size,
                               Direction dir,
                               std::uint64_t timestamp_ms,
                               bool locked = true);

/// How well a candidate spec explains a captured byte run.
struct FramingScore {
    FramingSpec spec;
    std::size_t frames_parsed   = 0;
    std::size_t bytes_explained = 0;
    std::size_t distinct_opcodes = 0;
    double      coverage = 0.0;     ///< bytes_explained / total bytes
    double      score    = 0.0;     ///< composite ranking value

    bool operator<(const FramingScore& o) const { return score > o.score; }  // descending
};

/// A header check word discovered in a stream.
struct ChecksumLayout {
    std::uint32_t header_size = 0;
    std::uint32_t offset = 0;       ///< offset of the check word
    std::size_t   hits = 0;         ///< positions where the identity held
    double        expected = 0.0;   ///< hits expected by chance
};

/// Look for a header check word without assuming any framing.
///
/// Deliberately independent of tiling, because the two are otherwise circular:
/// a checksum makes frame boundaries verifiable and so keeps a scanner locked,
/// but a scanner that has lost lock produces garbage headers in which no
/// checksum can be found. Searching the raw bytes breaks the loop.
///
/// The test is a straight signal-to-noise comparison. At every offset, check
/// whether one u16 of a hypothetical header equals the sum of the others. Over
/// random data that holds once per 65536 positions; over a stream whose frames
/// really carry such a word it holds hundreds of times. The gap is large
/// enough that no tuning is involved.
std::vector<ChecksumLayout> detect_checksum(const std::uint8_t* sample, std::size_t size);

/// Brute-force the framing of a captured stream.
///
/// Tries every (length_offset, length_size, endianness, includes_header,
/// adjust, opcode_offset) combination within sane bounds and ranks them by how
/// cleanly they tile the sample with no resyncs. Feed it one direction of one
/// connection — mixing directions or connections produces garbage.
///
/// Returns candidates best-first. An empty result means nothing tiled, which
/// usually means the payload is encrypted or compressed.
std::vector<FramingScore> detect_framing(const std::uint8_t* sample,
                                         std::size_t size,
                                         std::size_t max_results = 10);

} // namespace gobot
