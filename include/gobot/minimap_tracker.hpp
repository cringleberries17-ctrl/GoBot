#pragma once

#include "gobot/mob_detector.hpp"
#include "gobot/screen_capture.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace gobot {

/// Which map you are standing in, read off the game's own minimap.
///
/// The minimap panel is a scaled rendering of the whole level, and no two
/// levels look alike, so a hash of those pixels names the map. That is a much
/// better handle than the title bar beside it, which would mean recognising
/// Vietnamese text in a bitmap font.
///
/// Matched by distance rather than equality. The picture is not quite still —
/// weather and water animate in it, about two percent of the pixels from one
/// second to the next — so an exact hash never repeats. A difference hash
/// compared with a tolerance does.
///
/// What this cannot give you is where you are standing. The minimap draws no
/// marker for the character and none for anything else; it is scenery. Finding
/// the main view inside it by image matching was tried and does not work — the
/// correlation is flat across every scale and offset, which is what no signal
/// looks like.
class MinimapTracker {
public:
    struct Layout {
        ScreenRect body;        ///< the map picture
        ScreenRect title;       ///< the name bar above it
        bool valid() const { return body.valid(); }
    };

    /// Where the panel sits in a client of this size. Measured on 1024x768;
    /// it is anchored to the top-right corner, so it is placed by its distance
    /// from the right edge rather than by an absolute x.
    static Layout default_layout(int client_w, int client_h);

    void   set_layout(const Layout& l);
    Layout layout() const;

    /// Feed one captured frame.
    void update(const Frame& f);
    void reset();

    /// Identity of the map currently drawn; 0 before the first frame settles.
    std::uint64_t fingerprint() const;

    /// How many frames the current picture has held steady. The panel churns
    /// while a map loads, and acting on those intermediate states would name
    /// half a dozen maps on every warp.
    int stable_frames() const;

    /// Set by whoever is watching, so the UI can say "map changed" once rather
    /// than every frame.
    bool take_changed();

    /// The minimap picture and the name bar above it, for drawing in our own
    /// window. Showing the pixels is how the map's name gets in front of the
    /// user without reading the text.
    bool body_image(Frame& out) const;
    bool title_image(Frame& out) const;

    /// Where the character's own marker sits on the minimap.
    ///
    /// Found reliably -- the marker is a flat saturated yellow the map art
    /// never uses, and of the twenty-odd thousand pixels in the panel the only
    /// ones wearing it are its own.
    ///
    /// It does NOT tell you where you are, which is worth saying plainly
    /// because it looks as though it should. The panel is not a map of the
    /// level with a dot on it; it is a window onto the level that scrolls to
    /// keep the character in the middle. The marker therefore reads the same
    /// couple of numbers wherever the character goes, and it is the picture
    /// behind it that moves. Measured across a walk, it did not shift by a
    /// tenth of a pixel.
    ///
    /// Still worth having: it says where the middle of the minimap's view is
    /// in screen pixels, which is what the panel's own scroll has to be
    /// measured against, and it is the cheapest proof that the panel is
    /// showing a live map at all rather than a loading screen.
    struct Spot {
        bool  valid = false;
        float x = 0.0f, y = 0.0f;   ///< in minimap pixels, from the body's corner
        int   pixels = 0;           ///< how much of the marker was found
    };
    static Spot find_marker(const Frame& f, const ScreenRect& body);

    /// The marker's own colour. A flat, fully saturated yellow that the map
    /// art never uses: of the twenty-odd thousand pixels in the panel, the
    /// only ones wearing it are the marker's.
    static bool is_marker(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        return r >= 245 && g >= 210 && g <= 250 && b <= 30;
    }

    /// Fewer than this and it is a stray pixel of artwork, not the marker.
    static constexpr int kMinMarkerPixels = 6;

    /// Difference between two fingerprints, in bits. Under `kSameMap` is the
    /// same place.
    static int distance(std::uint64_t a, std::uint64_t b);
    static constexpr int kSameMap = 10;

private:
    void capture_images(const Frame& f);
    std::uint64_t hash_body() const;

    mutable std::mutex mutex_;
    Layout             layout_;

    std::vector<std::uint8_t> grey_;        ///< body, 9x8 block averages
    std::uint64_t             fingerprint_ = 0;
    std::uint64_t             pending_ = 0;
    int                       pending_count_ = 0;
    bool                      changed_ = false;

    Frame body_;
    Frame title_;

    static constexpr int kStableFrames = 5;
    static constexpr int kHashW = 9;        ///< 8 comparisons per row
    static constexpr int kHashH = 8;
};

/// Names the user has given to maps, kept against their fingerprints.
///
/// The game knows the name and draws it; GoBot cannot read it without OCR of a
/// bitmap font in a language with a lot of diacritics. Letting the user type it
/// once against the picture is a better trade than getting it wrong
/// automatically.
class MapBook {
public:
    bool load(const std::string& path);
    bool save(const std::string& path) const;

    /// Name for a fingerprint, or empty when this map has not been named.
    std::string name_for(std::uint64_t fp) const;
    void        set_name(std::uint64_t fp, const std::string& name);

    /// The `.map` file the user linked to this map, if any.
    std::string map_file_for(std::uint64_t fp) const;
    void        set_map_file(std::uint64_t fp, const std::string& stem);

    struct Entry {
        std::uint64_t fingerprint = 0;
        std::string   name;
        std::string   map_file;
    };
    std::vector<Entry> entries() const;
    std::size_t        size() const;

private:
    /// Nearest entry within the tolerance, or end().
    const Entry* find(std::uint64_t fp) const;
    Entry*       find_mut(std::uint64_t fp);

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

} // namespace gobot
