#pragma once

#include "gobot/game_state.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/map_file.hpp"
#include "gobot/map_table.hpp"
#include "gobot/screen_capture.hpp"

#include <string>
#include <vector>

namespace gobot {

/// Works out which map the character is standing in, and loads its geometry.
///
/// Everything needed was already on the machine and none of it was joined up.
/// The game ships exact collision data for all 783 of its maps; it ships an
/// index saying which of them each named place is; and the bot can read the
/// name off the minimap's title bar. The commercial client did none of this --
/// it asked its vendor's server for map geometry, which is why it was useless
/// without a subscription.
///
/// Two things make the join less direct than it sounds, and both are handled
/// here. Names are not unique: a place called "ThanhÂmCốc" exists twice, as
/// "[1]" and "[2]", and the bracketed number is the only difference. And the
/// glyph set has no punctuation, so the brackets come back as nonsense and the
/// number with them.
///
/// The creature list settles it. The server broadcasts every creature's
/// position in map pixels -- the same units and the same origin the collision
/// file uses -- so the right map is the one those positions fit inside. Where
/// two candidates share a name, the one 6400 pixels wide is the one holding a
/// creature at x=6350, and the one 4800 wide is not.
class MapLocator {
public:
    /// Paths may be left empty to use the game's and the glyph set's own.
    bool init(const std::string& font_dir = {},
              const std::string& table_path = {},
              const std::string& map_dir = {});

    bool ready() const { return font_.loaded() && table_.loaded(); }

    /// Look at a frame and decide where we are.
    ///
    /// `world` is the creature list as last decoded from the wire; pass an
    /// empty one and identification still works wherever the name is unique,
    /// which is most places.
    ///
    /// Cheap to call every frame: the title bar is read only when the picture
    /// of it changes, and the geometry is loaded only when the answer does.
    bool identify(const Frame& f, const std::vector<Monster>& world);

    /// Where we are, or null before the first successful identification.
    const MapTable::Entry* here() const { return here_; }

    /// The collision data for that place.
    const MapFile& geometry() const { return geometry_; }
    bool has_geometry() const { return geometry_.loaded(); }

    /// What the title bar last said, and how sure the reader was.
    const std::string& last_read() const { return last_read_; }
    float last_confidence() const { return last_confidence_; }

    /// True on the frame the answer changed, so a caller can rebuild whatever
    /// it had hung off the old map.
    bool take_changed();

    /// Everything the name alone could have meant, for when it is ambiguous
    /// and there is no creature list to settle it.
    const std::vector<MapTable::Entry>& candidates() const { return candidates_; }

private:
    /// A name with the characters the glyph set cannot draw taken out, so that
    /// what the game wrote and what was read off the screen can be compared on
    /// the same terms.
    static std::string readable(const std::string& s);

    GlyphFont   font_;
    MapTable    table_;
    std::string map_dir_;

    std::vector<MapTable::Entry> candidates_;
    MapTable::Entry              chosen_;
    const MapTable::Entry*       here_ = nullptr;
    MapFile                      geometry_;

    std::string last_read_;
    float       last_confidence_ = 0.0f;
    bool        changed_ = false;
};

} // namespace gobot
