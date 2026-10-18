#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gobot {

using Bytes = std::vector<std::uint8_t>;

/// A single frame lifted out of the game's traffic.
struct GamePacket {
    std::uint16_t opcode = 0;
    Bytes         payload;          ///< frame body, header already stripped
    bool          from_server = true;
    std::uint64_t timestamp_ms = 0;

    /// How many 0x0081 wrappers this frame was found inside: 0 for a frame
    /// read straight off the connection, 1 for the frame inside one, 2 for a
    /// frame inside a wrapper that was itself inside one (see extract_frames).
    std::uint8_t  depth = 0;

    /// Read from a UDP datagram rather than the TCP stream.
    bool          udp = false;

    /// A frame of the only kind GoBot read before 2026-10-18: off the TCP
    /// stream and not from inside a wrapper. Whatever acts on the game takes
    /// only these until it is known what the others mean (plan item P0-3).
    /// The same opcode number can mean something else inside a wrapper --
    /// GoC's own log calls 0x0014 inside 0x0081 a character being selected,
    /// and outside it a channel switch -- so the kinds are not mixed when
    /// frames are counted or decoded by opcode.
    bool plain() const { return depth == 0 && !udp; }
};

/// Which side of the conversation a byte run came from.
enum class Direction { ToServer, ToClient };

constexpr const char* to_string(Direction d) {
    return d == Direction::ToServer ? "C->S" : "S->C";
}

/// Milliseconds since an arbitrary fixed origin (steady clock).
std::uint64_t now_ms();

/// Paths are UTF-8 everywhere in GoBot -- exe_relative() gives them so -- and
/// go to Windows through its wide calls. The narrow ones read the bytes in the
/// machine's code page instead, so a folder named with Vietnamese letters (the
/// Desktop of a Windows user whose name has them, say) came out as another
/// path, and nothing in it -- configs, pictures, the font -- was found.
std::wstring utf8_to_wide(const std::string& utf8);
std::string  wide_to_utf8(const std::wstring& wide);

/// The few file-system calls GoBot makes with those paths, wide underneath.
bool path_exists(const std::string& path);   ///< a file or a folder
bool is_folder(const std::string& path);
bool make_folder(const std::string& path);   ///< one level; true if it is there now
bool remove_file(const std::string& path);
/// Moves a file to `to`, in place of whatever file was there.
bool move_file(const std::string& from, const std::string& to);
/// The names (not paths) of the files in `folder` matching `pattern`, such as
/// "*.png" -- folders left out -- in UTF-8 and in no particular order.
std::vector<std::string> files_in(const std::string& folder, const std::string& pattern);
/// fopen, for a UTF-8 path.
std::FILE* open_file(const std::string& path, const char* mode);

/// The version this copy of GoBot was packaged as: the first line of
/// VERSION.txt beside the executable, which package.ps1 writes. Empty for a
/// build that was never packaged. Read once.
std::string package_version();
/// The second line of the same file, "commit <hash>", or empty.
std::string package_commit();
/// What the window title and the simple screen show: package_version(), or
/// the Vietnamese for "a build of your own" when there is none.
std::string version_shown();
/// The reading behind package_version(), for the tests: one line (0 or 1) of
/// a VERSION.txt's text. A BOM and either kind of line ending are allowed
/// for; only printable ASCII is kept, at most 40 characters of it.
std::string version_line(const std::string& text, int line);

} // namespace gobot
