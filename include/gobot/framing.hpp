#pragma once

#include "gobot/common.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gobot {

/// How the game delimits messages inside its TCP stream -- and its UDP
/// datagrams, which use the same layout.
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
    ///
    /// Those twelve bytes are not a trailer, though: they are the end of
    /// another frame. See inner_offset.
    std::uint16_t trailer_opcode = 0;
    std::uint32_t trailer_bytes  = 0;

    bool has_trailer() const { return trailer_bytes != 0; }

    /// Where a whole frame of its own starts inside a trailer_opcode frame,
    /// counted from that frame's first byte. 0 = do not look inside.
    ///
    /// GoC's parser (GhostCore.dll, PacketCapture::ProcessPacketStream) reads
    /// an 0x0081 frame as its declared length + 12 bytes and takes byte 13 to
    /// the end as a complete game frame -- its own marker, opcode and length --
    /// which it checks and forwards like any other. So the frame inside runs
    /// from byte 13 to declared + 12, and the twelve bytes the length does not
    /// count are its last twelve. Nothing inside one had ever been decoded:
    /// the payload GoBot kept for 0x0081 starts five bytes before the inner
    /// frame and stops twelve bytes short of its end.
    ///
    /// Below header_size this could not be a frame of its own, and nothing is
    /// unwrapped. A spec built in code starts at 0 and reads exactly as it did
    /// before; the config readers default it to 13 when the key is missing,
    /// so a protocol.ini written before 2026-10-18 unwraps as well.
    std::uint32_t inner_offset = 0;

    bool unwraps() const { return has_trailer() && inner_offset >= header_size; }

    /// Unpack each wrapper, instead of taking GoC's frame at inner_offset.
    ///
    /// The P0-2 recordings (2026-10-18) showed what an 0x0081 holds: one
    /// frame, packed with liblzf. Its first word is that frame's length
    /// unpacked, its length field counts the packed bytes, bytes 8-11 mean
    /// nothing, and the packed stream runs from byte 12 to the end
    /// (protocol-findings 4e). The stream opens with nine literal bytes, the
    /// frame's header and its first body byte, which is why a whole header
    /// sits at byte 13; past it the body is packed. So the frame GoC takes
    /// from byte 13 has the right opcode and length, and no field after its
    /// first body byte.
    ///
    /// With this set, each wrapper unwraps() would open is unpacked instead
    /// (unpack_wrapper): its one frame comes out whole, at the next depth,
    /// and a wrapper that does not unpack is left shut and counted
    /// (UnwrapCounts::unpack_failed). GoC's 0x0020 rule does not apply: that
    /// first word is the unpacked length, and 0x0020 a frame of 32 bytes.
    ///
    /// No config key sets it: it is false unless code sets it. The tools
    /// that read recordings set it; the live capture does not yet (P0-3).
    bool unpack = false;

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

/// How deep wrappers inside wrappers are opened.
///
/// GoC opens an 0x0081 found inside an 0x0081 too, by calling its own parser
/// again, with no limit; each level costs only 13 bytes, so one large frame of
/// nonsense could nest a thousand deep. Frames down to this depth come out --
/// four, where anything real is expected at one or two -- and a wrapper at the
/// limit comes out shut, counted in UnwrapCounts::too_deep.
constexpr std::uint8_t kMaxWrapDepth = 4;

/// What looking inside wrappers found besides the frames themselves. Every
/// frame that could not or would not be taken out of one is counted here, and
/// so are two things worth knowing about the ones that were.
struct UnwrapCounts {
    std::size_t inner      = 0;   ///< frames taken out of wrappers, at any depth

    /// Wrappers left shut because their first two bytes read 0x0020 instead of
    /// the usual marker. GoC's rule: on meeting one, it stops reading the TCP
    /// segment it is in. GoBot reads a reassembled stream, not segments, and
    /// each header it accepts is proven by its check word, so it skips only
    /// what GoC would never have read in any case -- this wrapper's inside --
    /// and keeps framing after it. A length that was not to be trusted shows
    /// up there as resync bytes, which are counted as always. Inside another
    /// wrapper, where no check word is required, GoC's rule is kept to the
    /// letter: the rest of that wrapper's run of frames is not read either.
    std::size_t unopened   = 0;

    /// Insides too short to hold a frame header, or holding a frame that runs
    /// past its end.
    std::size_t unreadable = 0;

    /// Wrappers at kMaxWrapDepth, not opened.
    std::size_t too_deep   = 0;

    /// Frames whose own length field does not match the room they fill. The
    /// frame is still taken as the whole room, as GoC takes it; this says
    /// how often the two readings would differ.
    std::size_t length_differs = 0;

    /// Of those, the frames whose own length claims MORE than the room holds.
    /// The others claim less, which is what a room holding a second frame
    /// after the first would look like. In the P0-2 recordings (2026-10-18)
    /// every inner frame that disagreed claimed more; none claimed less.
    std::size_t length_longer = 0;

    /// Frames whose header check word does not hold. GoC never checks it, so
    /// neither does unwrapping; the count says whether inner frames carry one.
    std::size_t checksum_bad = 0;

    /// Wrappers unpacked (FramingSpec::unpack), each into its one frame.
    /// Unpacking counts here, in unpack_failed, inner and too_deep, and
    /// leaves the counts above at 0: they belong to GoC's reading from
    /// inner_offset.
    std::size_t unpacked = 0;

    /// Wrappers that did not unpack (FramingSpec::unpack), left shut: nothing
    /// inside them is taken for a frame. Every wrapper the client sent over
    /// TCP in the P0-2 recordings does not: their content is ciphered.
    std::size_t unpack_failed = 0;

    UnwrapCounts& operator+=(const UnwrapCounts& o);
};

/// Plain liblzf decompression of the `n` bytes at `in` into `out`, which it
/// replaces. A control byte c below 32 is followed by c + 1 bytes to copy as
/// they are. Any other copies (c >> 5) + 2 bytes -- with the next byte added
/// to c >> 5 when that is 7 -- from ((c & 31) << 8) + the byte after it, + 1,
/// bytes back from the end of what is out so far, one byte at a time, so that
/// a copy may run on into the bytes it is writing. False, with `out` empty,
/// when the bytes are not a whole stream of that form, a copy reaches back
/// before the start, or the result would pass `limit` bytes.
bool lzf_unpack(const std::uint8_t* in, std::size_t n, std::size_t limit, Bytes& out);

/// The one frame inside an 0x0081 wrapper `w` of `span` bytes -- its declared
/// length plus the trailer, as framing reads one -- unpacked into `frame`; or
/// false, with `frame` empty, when that is not what `w` holds. Taken only as
/// protocol-findings 4e says: the bytes from 12 to the end are as many as the
/// wrapper's length field says, they unpack with none left over to exactly
/// as many bytes as its first word says, and those are a frame whose own
/// length field agrees, whose check word holds, and whose first word is
/// 0x0105 -- or which is a wrapper itself, whose first word is a length.
bool unpack_wrapper(const FramingSpec& spec, const std::uint8_t* w, std::size_t span,
                    Bytes& frame);

/// A run of bytes in a buffer: where it starts, and how long it is.
struct ByteRun {
    std::size_t offset = 0;
    std::size_t length = 0;
};

/// Outcome of pulling frames off the front of a stream buffer.
struct FrameScanResult {
    /// In stream order. When the spec unwraps, the frames found inside a
    /// wrapper come straight after it, marked with their depth, and the
    /// wrapper itself keeps exactly the place and payload it always had.
    std::vector<GamePacket> packets;
    std::size_t consumed = 0;        ///< bytes to erase from the front
    std::size_t resyncs  = 0;        ///< bytes discarded hunting for alignment
    bool        locked   = false;    ///< pass back as `locked` on the next call
    UnwrapCounts unwrap;             ///< see FramingSpec::inner_offset

    /// Where the resync bytes were, run by run, in the order they came; their
    /// lengths add up to `resyncs`. Not what lies past `consumed`, which was
    /// never reached. For the tools that show what a spec could not read.
    std::vector<ByteRun> skipped;

    /// Frames read straight off the buffer, leaving out those found inside
    /// wrappers: the count every caller had before anything was unwrapped.
    std::size_t outer_frames() const { return packets.size() - unwrap.inner; }
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
///
/// When the spec unwraps (FramingSpec::inner_offset), each wrapper is followed
/// by what is inside it, read the way GoC reads it:
///
///   - a wrapper whose first two bytes read 0x0020 is left shut and counted;
///   - otherwise the inside is one frame filling the whole room, unless that
///     frame is a wrapper too: then the room is read as a run of frames, each
///     by its own length, and any wrapper among them is opened in turn, down
///     to kMaxWrapDepth.
///
/// The inner frames carry the wrapper's direction and timestamp, and their
/// payload starts after their own header_size bytes. Their header check word
/// is not required, since GoC requires none; whether it holds is counted.
///
/// When the spec also unpacks (FramingSpec::unpack), each wrapper is
/// unpacked instead: its one frame comes straight after it, whole, or
/// nothing does and the wrapper is counted as one that did not unpack.
FrameScanResult extract_frames(const FramingSpec& spec,
                               const std::uint8_t* buf,
                               std::size_t size,
                               Direction dir,
                               std::uint64_t timestamp_ms,
                               bool locked = true);

/// Frame one UDP datagram on its own.
///
/// GoC reads each datagram as a run of whole frames from its first byte, in
/// the same layout as the TCP stream, and so does this: the frames come out
/// as extract_frames would give them, marked `udp`, wrappers opened the same
/// way. Nothing carries over to the next datagram. Bytes no frame accounts
/// for -- a frame cut short at the end, or a datagram that is not frames at
/// all -- are counted in `resyncs` and placed in `skipped`, and `consumed` is
/// always the whole size.
///
/// GoC also throws away opcodes 0x002E and 0x0031 when they come over UDP.
/// This keeps them: what they are is for a recording to show.
FrameScanResult extract_datagram(const FramingSpec& spec,
                                 const std::uint8_t* data,
                                 std::size_t size,
                                 Direction dir,
                                 std::uint64_t timestamp_ms);

/// Read a framing block -- [Framing], or [FramingClient] for what the client
/// sends -- out of a protocol config. `found` says whether the section is
/// there. A key that is missing takes the default, and inner_frame_offset
/// defaults to 13 (see FramingSpec::inner_offset) so that a config written
/// before it existed unwraps 0x0081 too.
///
/// One reader for gobot_sniff, gobot_cli and the tests, so that all of them
/// frame a stream the same way.
FramingSpec load_framing(const std::string& path, bool& found,
                         const char* section = "Framing");

/// How the game's UDP datagrams are framed, both directions: by a
/// [FramingUdp] block when the config has one -- a whole block, with the keys
/// of [Framing] -- and otherwise by `server`, the [Framing] block the caller
/// read. `found` says whether there was a [FramingUdp].
///
/// Never by [FramingClient], not even what the client sends. In the P0-2
/// recordings (2026-10-18) the client's datagrams carry the server's header
/// -- opcode at 2, the whole frame's length at 4, a check word at 6 that
/// holds on every frame -- and tile 100% that way, against 2 to 14% under
/// [FramingClient]; the server's datagrams the same. So a config written
/// before the block existed frames UDP right without it.
FramingSpec load_udp_framing(const std::string& path, const FramingSpec& server, bool& found);

/// Where a frame was found, for tools that list frames: "" for a frame off
/// the TCP stream, otherwise e.g. "inside 0x0081", "inside 0x0081 x2", "UDP",
/// or "UDP, inside 0x0081".
std::string where_found(const GamePacket& p);

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
