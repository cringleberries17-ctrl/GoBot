#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gobot {

using Bytes = std::vector<std::uint8_t>;

/// A single frame lifted out of the game's TCP stream.
struct GamePacket {
    std::uint16_t opcode = 0;
    Bytes         payload;          ///< frame body, header already stripped
    bool          from_server = true;
    std::uint64_t timestamp_ms = 0;
};

/// Which side of the TCP conversation a byte run came from.
enum class Direction { ToServer, ToClient };

constexpr const char* to_string(Direction d) {
    return d == Direction::ToServer ? "C->S" : "S->C";
}

/// Milliseconds since an arbitrary fixed origin (steady clock).
std::uint64_t now_ms();

} // namespace gobot
