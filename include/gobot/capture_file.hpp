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
    std::size_t frames = 0;
    std::size_t resyncs = 0;
    std::string error;          ///< set when ok is false
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

/// Pull every payload of one opcode out of a recording.
///
/// `client` may be implausible when the config has no [FramingClient]; the
/// server spec is then the only candidate.
Reading extract(const std::string& path,
                const FramingSpec& server,
                const FramingSpec& client,
                std::uint16_t opcode,
                std::vector<Bytes>& out);

} // namespace capture_file
} // namespace gobot
