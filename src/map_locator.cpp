#include "gobot/map_locator.hpp"
#include "gobot/logger.hpp"
#include "gobot/minimap_tracker.hpp"

#include <algorithm>

namespace gobot {
namespace {

/// How far outside a map's bounds a creature may be reported and still count
/// as inside it. Creatures stand on the floor and the floor is the edge, so a
/// little slack avoids rejecting the right map over a rounding.
constexpr float kBoundsSlack = 64.0f;

} // namespace

std::string MapLocator::readable(const std::string& s) {
    // Everything the glyph set can draw is a letter or a digit; the brackets
    // and parentheses the game puts round a map's number are not in it. Both
    // sides of the comparison lose them, so "ThanhÂmCốc[1]" and a reading of
    // "ThanhÂmCốcld l" are compared as "ThanhÂmCốc" and "ThanhÂmCốcldl" --
    // which still differ, but no longer differ over punctuation.
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c >= 0x80) { out.push_back(static_cast<char>(c)); continue; }  // UTF-8 tail
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            out.push_back(static_cast<char>(c));
    }
    return out;
}

bool MapLocator::init(const std::string& font_dir, const std::string& table_path,
                      const std::string& map_dir) {
    const std::string fd = font_dir.empty() ? GlyphFont::default_dir() : font_dir;
    if (fd.empty() || !font_.load(fd)) {
        GB_WARN("MapLocator: no glyph set -- the map's name cannot be read");
        return false;
    }

    const std::string tp = table_path.empty() ? MapTable::default_path() : table_path;
    if (tp.empty() || !table_.load(tp)) {
        GB_WARN("MapLocator: no map table -- a name cannot be turned into a file");
        return false;
    }

    map_dir_ = map_dir.empty() ? MapFile::default_map_dir() : map_dir;
    if (map_dir_.empty())
        GB_WARN("MapLocator: no map directory -- names will resolve but geometry "
                "will not load");
    return true;
}

bool MapLocator::take_changed() {
    const bool was = changed_;
    changed_ = false;
    return was;
}

bool MapLocator::identify(const Frame& f, const std::vector<Monster>& world) {
    if (!ready() || !f.valid()) return false;

    const auto layout = MinimapTracker::default_layout(f.width, f.height);
    if (!layout.title.valid()) return false;

    const std::string read = font_.read_line(f, layout.title);
    last_confidence_ = font_.last_confidence();
    if (read.empty()) return here_ != nullptr;

    // Re-deciding only when the reading changes keeps this to one glyph match
    // a frame instead of a table scan and a file load.
    const bool fresh_reading = (read != last_read_);
    last_read_ = read;
    if (!fresh_reading && here_) return true;

    // Every place whose name the reading could be hiding, compared with the
    // punctuation neither side can agree on removed.
    const std::string want = readable(read);
    candidates_.clear();
    std::size_t longest = 0;
    for (const MapTable::Entry& e : table_.entries()) {
        const std::string name = readable(e.name);
        if (name.size() < 6) continue;              // too short to be distinctive
        if (want.find(name) == std::string::npos) continue;
        if (name.size() > longest) { longest = name.size(); candidates_.clear(); }
        if (name.size() == longest) candidates_.push_back(e);
    }

    // Nothing matched whole. Fall back to the longest name the reading opens
    // with, which is what survives when the number on the end is unreadable.
    if (candidates_.empty()) {
        longest = 0;
        for (const MapTable::Entry& e : table_.entries()) {
            const std::string name = readable(e.name);
            if (name.size() < 6) continue;
            std::size_t shared = 0;
            for (std::size_t start = 0; start + 6 <= want.size(); ++start) {
                std::size_t n = 0;
                while (n < name.size() && start + n < want.size() &&
                       want[start + n] == name[n]) ++n;
                shared = std::max(shared, n);
            }
            if (shared < 6 || shared < name.size() * 3 / 4) continue;
            if (shared > longest) { longest = shared; candidates_.clear(); }
            if (shared == longest) candidates_.push_back(e);
        }
    }
    if (candidates_.empty()) return here_ != nullptr;

    // One name, several places. The creature list decides: the right map is
    // the one every creature the server has told us about fits inside.
    const MapTable::Entry* picked = &candidates_.front();
    if (candidates_.size() > 1 && !world.empty()) {
        float max_x = 0.0f, max_y = 0.0f;
        for (const Monster& m : world) {
            max_x = std::max(max_x, m.pos.x);
            max_y = std::max(max_y, m.pos.y);
        }

        const MapTable::Entry* fits = nullptr;
        int smallest = 0;
        for (const MapTable::Entry& e : candidates_) {
            const std::string path = map_dir_.empty() ? std::string{}
                                                      : MapFile::find(map_dir_, e.stem());
            MapFile probe;
            if (path.empty() || !probe.load(path)) continue;
            if (max_x > probe.width_px() + kBoundsSlack) continue;
            if (max_y > probe.height_px() + kBoundsSlack) continue;
            // The smallest map that still holds every creature. A larger one
            // holds them too, so "fits" alone would always pick the biggest.
            const int area = probe.width_px() * probe.height_px();
            if (fits && area >= smallest) continue;
            fits = &e;
            smallest = area;
        }
        if (fits) picked = fits;
        else GB_WARN("MapLocator: \"%s\" is ambiguous and no candidate holds every "
                     "creature; guessing %s", read.c_str(), picked->stem().c_str());
    }

    if (here_ && picked->theme == chosen_.theme && picked->stage == chosen_.stage)
        return true;

    chosen_ = *picked;
    here_ = &chosen_;
    changed_ = true;

    geometry_ = MapFile{};
    const std::string path = map_dir_.empty() ? std::string{}
                                              : MapFile::find(map_dir_, chosen_.stem());
    if (path.empty() || !geometry_.load(path)) {
        GB_WARN("MapLocator: in \"%s\" (%s) but its collision file is missing",
                chosen_.name.c_str(), chosen_.stem().c_str());
    } else {
        GB_INFO("MapLocator: \"%s\" -> %s, %dx%d px, %zu surface pixels",
                chosen_.name.c_str(), chosen_.stem().c_str(),
                geometry_.width_px(), geometry_.height_px(),
                geometry_.surface_count());
    }
    return true;
}

} // namespace gobot
