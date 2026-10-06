#pragma once

#include <string>
#include <vector>

namespace gobot {

/// The game's own list of places: which collision file each named map is.
///
/// The bot already has the geometry -- the game ships 783 `.map` files holding
/// exact surface data, which is what the commercial client's server was
/// parsing and charging for. What it did not have was a way to tell which of
/// the 783 it is standing in.
///
/// The game answers that itself, in `zVietnam\Chart\tb_Map.tbl`: a row per
/// place giving its theme, its stage, and the name shown on the minimap. The
/// collision files are named after exactly those two numbers -- theme 14,
/// stage 0 is `t14_s0.map` -- so reading the name off the title bar and
/// looking it up here names the file.
///
/// The table is lightly obfuscated: every byte is exclusive-ORed with 0x11,
/// and each string is stored backwards. Neither is encryption and neither was
/// meant to stop anyone; it keeps the file from being readable by accident.
class MapTable {
public:
    struct Entry {
        int         theme = 0;
        int         stage = 0;
        std::string name;       ///< UTF-8, as the game writes it

        /// The collision file's stem, e.g. "t14_s0".
        std::string stem() const;
    };

    bool load(const std::string& path);
    bool loaded() const { return !entries_.empty(); }

    const std::vector<Entry>& entries() const { return entries_; }

    /// The place with exactly this name, or null.
    const Entry* find(const std::string& name) const;

    /// The place whose name best fits text read off the screen.
    ///
    /// Read text is not clean: the title bar scrolls, so the name arrives
    /// repeated and clipped, the glyph set has no punctuation so brackets come
    /// back as nonsense, and the odd letter is lost to whatever is behind it.
    /// What survives is a long, distinctive run of the name itself, so the
    /// match is made on the longest place name that appears inside the read --
    /// longest because "ThanhÂmCốc" and "ThanhÂmQuan" share a prefix, and the
    /// shorter would otherwise win on a reading that contains both.
    const Entry* best_match(const std::string& read) const;

    /// The places whose names most nearly fit, longest shared opening first.
    /// For saying why a reading did not match anything.
    std::vector<Entry> closest(const std::string& read, std::size_t count) const;

    /// Where the game keeps the table, if it can be found from the registry.
    static std::string default_path();

    /// Every byte of the file is exclusive-ORed with this.
    static constexpr unsigned char kMask = 0x11;

private:
    std::vector<Entry> entries_;
};

} // namespace gobot
