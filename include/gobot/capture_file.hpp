#pragma once

#include "gobot/common.hpp"
#include "gobot/framing.hpp"

#include <string>
#include <vector>

namespace gobot {

/// Reading back a stream written by `gobot_sniff --record`.
///
/// Analysis that can only be done live is analysis that can only be done once:
/// the thing you wanted to measure has already happened by the time you know
/// what to measure. A recording can be re-read after a hypothesis changes,
/// which is the difference between measuring and remembering.
namespace capture_file {

/// Which half of the conversation a recording holds.
enum class Half { Server, Client };

/// How the direction was decided, which matters more than it looks.
enum class HalfSource {
    Name,       ///< the file was called .s2c or .c2s, which settles it
    Scan,       ///< neither, so both specs were tried
    Forced,     ///< the caller said
};

struct Reading {
    bool        ok = false;
    Half        half = Half::Server;
    HalfSource  decided_by = HalfSource::Scan;
    std::size_t frames = 0;     ///< read straight off the stream, wrappers' insides apart
    std::size_t resyncs = 0;
    std::string error;          ///< set when ok is false

    /// Frames taken out of 0x0081 wrappers, any opcode.
    std::size_t inner = 0;
    /// Frames of the opcode asked for that were the other kind -- inside a
    /// wrapper when plain ones were asked for, or the reverse -- and so were
    /// left out. Worth saying when nothing of the kind asked for was found.
    std::size_t other_kind = 0;
    UnwrapCounts unwrap;
};

/// The direction a recording's name implies, if its name implies one.
///
/// `--record` writes `<name>.s2c` and `<name>.c2s`, and `<name>.flow0.s2c` for
/// a single connection, so the name usually settles it. Worth preferring over
/// trying both specs and judging: this game's two framings BOTH tile a short
/// client capture with zero resyncs, because the scanner stops at the first
/// header it cannot use rather than running to the end of the file. A tie is
/// then decided by whichever was tried first, and a client recording quietly
/// becomes a server one.
bool half_from_name(const std::string& path, Half& out);

/// Whether a recording holds UDP: `--record` writes the game's datagrams, one
/// after another, to `<name>.udp.s2c` and `<name>.udp.c2s`.
bool udp_from_name(const std::string& path);

/// Every frame a recording holds, appended to `out` in the order they come,
/// those found inside 0x0081 straight after their wrapper -- read as
/// `extract` reads it, its half told by its name or by trying both specs.
///
/// From a recording of UDP (udp_from_name) every frame is marked `udp`. Its
/// datagrams were written one after another and are read as one run of
/// frames, which is what datagrams that hold whole frames read as.
Reading read_all(const std::string& path,
                 const FramingSpec& server,
                 const FramingSpec& client,
                 std::vector<GamePacket>& out);

/// Pull every payload of one opcode out of a recording.
///
/// `client` may be implausible when the config has no [FramingClient]; the
/// server spec is then the only candidate.
///
/// Frames read straight off the stream, by default, which is all this ever
/// gave; with `inner`, only those found inside 0x0081 wrappers instead. The
/// two are never mixed: one opcode number need not mean the same thing in
/// both places.
Reading extract(const std::string& path,
                const FramingSpec& server,
                const FramingSpec& client,
                std::uint16_t opcode,
                std::vector<Bytes>& out,
                bool inner = false);

/// A whole recording tiled with one spec from its first byte, as the server's
/// half unless `dir` says otherwise: what `gobot_sniff --verify` reports, and
/// what the tests check a real capture against, so that both read it the
/// same way. The direction only labels the frames; it frames nothing.
FrameScanResult tile(const Bytes& recording, const FramingSpec& spec,
                     Direction dir = Direction::ToClient);

} // namespace capture_file
} // namespace gobot
