#pragma once

#include "gobot/mob_detector.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace gobot {

/// The monster lists on Đánh quái: "Không đánh", the monsters never to attack,
/// and "Đánh trước", the ones to go for first. GoC decides the same things on
/// its server from what the packets say a monster is ("Loại mob", "Ưu tiên
/// đánh Boss", "Bỏ qua quái bay"); GoBot has only the name on the plate, read
/// off the screen.

/// How many names each list keeps.
constexpr std::size_t kMaxMonsterNames = 20;

/// Whether a name read off a plate is the one on a list.
///
/// Loosely, because the name is read off the screen. Both sides are folded to
/// bare lowercase letters and digits -- accents, đ and spaces gone, so "Nga
/// Quỷ" typed on the list is the "NgaQuy" the reader makes of a plate it has
/// no ỷ for -- and then they are the same, or at least 80% of the letters
/// line up (PartyChat::alike): a listed name of five letters or more may have
/// one in five wrong or missing. A listed name of four letters or fewer has
/// to match exactly, so a short name cannot take in a longer one that merely
/// starts the same way: "HoLy" would otherwise take in every "HoLyX", which
/// is four letters in five. An empty name, on either side, matches nothing.
bool monster_name_matches(const std::string& read, const std::string& listed);

/// Whether `read` matches any name on `list`.
bool monster_on_list(const std::vector<std::string>& list, const std::string& read);

/// A list as hunt.ini keeps it: names separated by commas, semicolons or line
/// breaks, each trimmed and kept in UTF-8 exactly as typed, the first
/// kMaxMonsterNames of them.
std::vector<std::string> split_monster_names(const std::string& text);
std::string              join_monster_names(const std::vector<std::string>& names);

/// The monster names seen lately, for "Quái đang thấy": what the player picks
/// the lists from.
///
/// A name is read off a plate several times a second, and not always the
/// same way -- a letter lost to the grass, an accent the glyph set has not
/// got -- so the reads are grouped the way the lists match them, and each
/// group is shown once, spelt the way it was read most often.
class MonsterSightings {
public:
    /// How far back a sighting counts, and how many names are listed.
    static constexpr std::uint64_t kWindowMs = 10'000;
    static constexpr std::size_t   kMaxShown = 12;

    /// Bounds on what is kept, so a screen full of monsters for an afternoon
    /// cannot grow it for ever: seven frames a second of a dozen plates is
    /// under a thousand reads in the window, and a reader that misspells a
    /// name a different way every time makes a new spelling each time.
    static constexpr std::size_t kMaxSpellings = 64;
    static constexpr std::size_t kMaxReadsEach = 2000;

    /// One frame's plates. Monsters with a name read count; players, and
    /// plates whose name could not be read, do not.
    void saw(const std::vector<Nameplate>& plates, std::uint64_t now_ms);
    /// One name read at `now_ms`. An empty one is not counted.
    void saw(const std::string& name, std::uint64_t now_ms);

    /// The names seen in the last kWindowMs, the most often seen first, up to
    /// kMaxShown of them.
    std::vector<std::string> names(std::uint64_t now_ms) const;

    /// How many spellings are kept now: never more than kMaxSpellings.
    std::size_t spellings() const { return spellings_.size(); }

    void clear() { spellings_.clear(); }

private:
    /// One way a name was read, and when: kept per spelling rather than per
    /// read, so that grouping them -- which compares names letter by letter
    /// -- costs a few dozen comparisons a frame, not one for every read.
    struct Spelling {
        std::string               name;       ///< as read
        std::string               key;        ///< folded, as the lists compare it
        std::uint64_t             first = 0;  ///< the order it was first read in
        std::deque<std::uint64_t> at;         ///< when it was read, oldest first
    };
    std::vector<Spelling> spellings_;
    std::uint64_t         order_ = 0;
};

} // namespace gobot
