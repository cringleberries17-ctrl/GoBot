#pragma once

#include "gobot/common.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace gobot {

/// How many frames of each kind the capture handed over, for the "frames:"
/// part of the Load line: which opcodes the game sends, and how often.
///
/// A frame's kind is its class and its opcode. The classes:
///
///   S  the server, straight off the TCP stream
///   W  the server, from inside an 0x0081 (any depth)
///   U  the server, over UDP
///   C  your client, over TCP: its "opcode" is the frame's check word, which
///      is the wrapper's class rather than what is inside (protocol findings
///      P15) -- the client's wrappers are ciphered and never unpack
///   D  your client, over UDP
///
/// The same opcode number can mean something else in another class (0x0014
/// inside 0x0081 is a character selected, plain a channel switch), so the
/// classes are never added together.
///
/// add() runs on the capture thread, the rest on GoBot's: one lock of its
/// own, held for a lookup a frame.
class FrameTally {
public:
    /// The frames: text never runs past this many bytes.
    static constexpr std::size_t kMaxText = 1536;
    /// How many of each class's most frequent kinds say whether what the game
    /// sends has changed (top_changed).
    static constexpr std::size_t kTopKinds = 3;

    /// The class letter of `p`: S, W, U, C or D.
    static char class_of(const GamePacket& p);

    /// Count every frame of a batch.
    void add(const std::vector<GamePacket>& all);

    /// Each class's `per_class` most frequent kinds since the last take,
    /// most first, then how many kinds and frames were left out, as
    /// "S 0038:1234 03C6:60 +2 kinds (7) | W 0042:2", held to kMaxText -- a
    /// text cut short ends " ..." -- and the counts start again. "" when
    /// nothing came.
    std::string take(std::size_t per_class = 12);

    /// Whether any class's kTopKinds most frequent kinds, as a set, are not
    /// those of the last take that had any frames: the game sending something
    /// else now -- a hunt begun, a map loaded. True before the first take
    /// once anything came; false with nothing counted.
    bool top_changed() const;

    /// The kinds seen for the first time this session since the last call,
    /// in the order they came, at most `most` of them named, as
    /// "S 0086, W 0050"; "" for none.
    std::string take_new_kinds(std::size_t most = 16);

    /// Frames counted since the last take.
    std::uint64_t counted() const;

private:
    /// The top kinds of each class, as a set per class, under mutex_.
    std::string top_key() const;

    mutable std::mutex                       mutex_;
    std::map<std::uint32_t, std::uint64_t>   counts_;      ///< (class << 16 | opcode) -> frames
    std::unordered_set<std::uint32_t>        seen_;        ///< every kind ever counted
    std::vector<std::uint32_t>               new_kinds_;   ///< seen first since the last take_new_kinds
    std::string                              top_at_take_;
    std::uint64_t                            counted_ = 0;
};

} // namespace gobot
