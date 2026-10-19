#pragma once

#include "gobot/common.hpp"
#include "gobot/framing.hpp"

#include <cstdint>
#include <map>
#include <optional>
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

/// Which block of the protocol config framed a recording.
enum class Spec {
    Server,     ///< [Framing]
    Client,     ///< [FramingClient]
    Udp,        ///< what UDP is framed with: [FramingUdp], or [Framing] without it
};

/// The blocks a recording can be framed with, as one config gives them.
struct Framings {
    FramingSpec                server;    ///< [Framing]
    std::optional<FramingSpec> client;    ///< [FramingClient], when the config has it
    std::optional<FramingSpec> udp;       ///< [FramingUdp], when the config has it

    /// What UDP is framed with, in both directions (load_udp_framing).
    const FramingSpec& for_udp() const { return udp ? *udp : server; }
};

/// The three blocks of a protocol config. `have_server` is false when it has
/// no [Framing], without which nothing is read.
Framings load_framings(const std::string& config, bool& have_server);

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

    /// The block that framed it. The half is still the one the name says: a
    /// client half framed with [Framing] is labelled client -> server.
    Spec        spec = Spec::Server;
    /// Both [Framing] and [FramingClient] were tried -- on a TCP .c2s file,
    /// or one whose name says neither -- and `spec` is the one that read
    /// more. What the other read:
    bool        measured = false;
    std::size_t other_frames = 0;
    std::size_t other_unread = 0;

    std::size_t size = 0;       ///< bytes in the file
    std::size_t consumed = 0;   ///< bytes the scan went through: its frames and resyncs
    std::size_t largest = 0;    ///< the longest frame read off the file, header included
    /// Where the bytes in no frame are, run by run: those slid past, then
    /// whatever lay beyond the end of the scan.
    std::vector<ByteRun> unread_runs;

    /// How many bytes no frame read off the file accounts for: the resyncs,
    /// and the rest of the file when the scan stopped short of its end. It
    /// stops there when it trusts a length that runs past the end, and a
    /// resync count never shows that (round 1b review, R1b-REV-1).
    std::size_t unread() const { return size - consumed + resyncs; }
};

/// What a reading says about the spec that made it.
enum class Fit {
    Described,      ///< frames, and at most 5% of the file in none of them
    Empty,          ///< the file holds nothing
    NothingRead,    ///< not one frame
    TooMuchUnread,  ///< more than 5% of the file in no frame
    Oversized,      ///< frames that average more than 512 bytes
};

/// Whether a reading describes its file, as --verify and the --from readers
/// judge it. The bytes in no frame count, resync bytes or not: a scan that
/// stops early reads little and resyncs nothing. And frames that average
/// more than 512 bytes are a length field that is not one, reading a few
/// huge frames that swallow the real ones (protocol-findings section 4).
Fit judge(const Reading& r);

/// The warning the --from readers print before what they show: judge()'s
/// verdict in words, and nothing when judge() calls the reading a description
/// of its file. No other rule adds one, so the readers and --verify always
/// agree about a reading. A resync count set against the frame count was such
/// a rule once, and warned on shop.c2s -- six 18-byte runs, 108 resync bytes
/// against 92 frames, 2.5% of the file -- while --verify called the same
/// reading a description (round 3 review, R3-REV2-1). Each line is indented,
/// the first starts "!!", and the text ends in a newline.
std::string reading_warning(const Reading& r);

/// The direction a recording's name implies, if its name implies one.
///
/// `--record` writes `<name>.s2c` and `<name>.c2s`, and `<name>.flowN.s2c` and
/// `<name>.flowN.c2s` per connection, so the name usually settles it. Worth
/// preferring over judging by the bytes: the two framings in the config BOTH
/// tile the client's half -- it is framed in the server's layout, and the
/// two find the same frames in it -- so its bytes cannot say which half it
/// is, and a client recording would quietly become a server one.
bool half_from_name(const std::string& path, Half& out);

/// Whether a recording holds UDP: `--record` writes the game's datagrams, one
/// after another, to `<name>.udp.s2c` and `<name>.udp.c2s`.
bool udp_from_name(const std::string& path);

/// A recording tiled from its first byte with the block that fits it, and
/// what came of it.
struct Tiling {
    Reading         reading;
    FrameScanResult scan;       ///< its frames, each wrapper's insides after it
    FramingSpec     spec;       ///< the spec of the block reading.spec names
};

/// `data`, the bytes of a recording called `name`, framed with:
///
///   - for a .udp.s2c or .udp.c2s file, what UDP is framed with
///     (Framings::for_udp), both directions alike;
///   - for a .s2c file, [Framing];
///   - for a .c2s file, whichever of [Framing] and [FramingClient] reads more
///     of it: more frames, or as many and fewer bytes in none -- [Framing] on
///     a tie, as its check word proves every header it reads;
///   - for a name that says neither, the same choice, and the half the block
///     kept stands for.
///
/// `force` names the block instead. The frames are labelled with the half
/// the name says, whichever block framed them.
///
/// Why a .c2s file is measured rather than given [FramingClient]: in the
/// P0-2 recordings (2026-10-18) the client's TCP half is in the server's
/// layout too. Every frame is an 0x0081, whose check word at offset 6 is what
/// [FramingClient] reads as an opcode, and whose 12 bytes past the declared
/// length are [FramingClient]'s "body + 4". So the two find the same frames,
/// until bytes that are not a frame: there [FramingClient], with no check
/// word, trusts a false length and swallows the rest of the file.
Tiling tile_named(const Bytes& data, const std::string& name, const Framings& framings,
                  std::optional<Spec> force = std::nullopt);

/// Every frame a recording holds, appended to `out` in the order they come,
/// those found inside 0x0081 straight after their wrapper -- framed as
/// tile_named frames it.
///
/// From a recording of UDP (udp_from_name) every frame is marked `udp`. Its
/// datagrams were written one after another and are read as one run of
/// frames, which is what datagrams that hold whole frames read as.
Reading read_all(const std::string& path, const Framings& framings,
                 std::vector<GamePacket>& out, std::optional<Spec> force = std::nullopt);

/// The same, given [Framing] and [FramingClient] as two specs: `client` is
/// taken for no [FramingClient] at all when it is not plausible, and UDP is
/// framed with `server`.
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
/// half unless `dir` says otherwise: what the tests check a real capture
/// against, so that they read it as the tools do. The direction only labels
/// the frames; it frames nothing.
FrameScanResult tile(const Bytes& recording, const FramingSpec& spec,
                     Direction dir = Direction::ToClient);

// ------------------------------------------------ one opcode's frames as numbers

/// A number in a frame body, little-endian, as the game's fields are.
enum class Number { U8, I8, U16, I16, U32, I32, F32 };

/// "u8", "i8", "u16", "i16", "u32", "i32" or "f32", in any case; false for
/// anything else.
bool number_from_name(const std::string& name, Number& out);
const char* number_name(Number n);
std::size_t number_size(Number n);

/// The number at `offset` of `body`, or false when the body is too short.
/// `raw` gets its bytes as an integer, which tells two values apart exactly:
/// a float's NaN equals nothing, itself included.
bool read_number(const Bytes& body, std::size_t offset, Number n, double& value,
                 std::uint32_t* raw = nullptr);

/// A number as the tools print it: an integer as itself; a float with two
/// decimals from 0.01 up to ten million, and 0 as 0.00, otherwise in
/// exponent form.
std::string format_number(double value, Number n);

/// An offset or a count as --at and --step take one: decimal digits, read as
/// decimal even after a leading zero ("012" is twelve, "08" eight), or hex
/// digits after 0x or 0X. False for anything else -- a sign, a space, no
/// digits, a number too big for `out` -- rather than a number misread. Read
/// the way C reads a literal, "012" was ten and "08" was refused (round 3
/// review, R3-REV2-5).
bool parse_count(const std::string& text, std::size_t& out);

/// What the number at one offset does over a run of frames, in their order.
struct Column {
    std::size_t offset = 0;
    std::size_t frames = 0;     ///< bodies long enough to hold it
    std::size_t distinct = 0;
    std::size_t changes = 0;    ///< values that differ from the one before
    std::size_t rises = 0;      ///< of those, larger than the one before
    std::size_t falls = 0;      ///< and smaller
    double first = 0, last = 0;
    double min = 0, max = 0;    ///< a float's NaN left out

    bool constant() const { return changes == 0; }
};

/// The number at `offset`, read as `n`, over the bodies long enough to hold
/// it, in their order.
Column column(const std::vector<Bytes>& bodies, Number n, std::size_t offset);

/// The columns from `start`, every `step` bytes (0: the number's own size),
/// as far as every one of `bodies` reaches.
std::vector<Column> columns(const std::vector<Bytes>& bodies, Number n,
                            std::size_t start, std::size_t step);

// -------------------------------------------- writing a recording, per connection

/// Each connection's two halves kept apart, for `--record` to write as
/// `<name>.flowN.s2c` and `<name>.flowN.c2s`.
///
/// `<name>.s2c` and `<name>.c2s` hold every connection's bytes as they came,
/// interleaved where their segments fell, which tiles under no framing. The
/// client's half was only ever written that way: in the P0-2 recordings it
/// carries 18-byte runs that are not frames of the game's stream, and the one
/// .c2s file cannot say which connection they came on.
class FlowHalves {
public:
    /// Bytes one connection carried one way. `name` says which connection it
    /// is, and the first one given is kept.
    void add(std::uint64_t flow, const std::string& name, bool to_client,
             const std::uint8_t* data, std::size_t size);

    struct File {
        std::string path;
        std::string connection;     ///< the name add() was given
        std::size_t bytes = 0;
        bool        to_client = true;
        bool        written = false;
    };

    /// Every half that carried anything, as `<base>.flowN.s2c` or
    /// `<base>.flowN.c2s`: one N for both halves of a connection. The
    /// connections that carried anything to the client are numbered first,
    /// in the order they always were, so `<base>.flow0.s2c` is the file it
    /// always was; then those that carried only the client's half. Written
    /// when `write` is set; `written` says whether that worked.
    std::vector<File> files(const std::string& base, bool write) const;

    std::size_t connections() const { return flows_.size(); }

private:
    struct Halves {
        std::string name;
        Bytes s2c, c2s;
    };
    std::map<std::uint64_t, Halves> flows_;
};

} // namespace capture_file
} // namespace gobot
