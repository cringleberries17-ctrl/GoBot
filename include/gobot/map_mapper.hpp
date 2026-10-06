#pragma once

#include "gobot/pathfinder.hpp"
#include "gobot/screen_capture.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gobot {

/// Where on screen the minimap sits, and what the player marker looks like.
///
/// Measured from the top-right corner, because that is the edge the minimap is
/// anchored to — the game window resizes from the left and bottom, so a
/// top-left offset would drift.
struct MinimapSpec {
    int right_margin = 10;      ///< px from the window's right edge
    int top = 70;               ///< px from the top
    int width = 200;
    int height = 112;

    /// Marker colour, matched within `tolerance` per channel.
    ///
    /// Not hard-coded to a guess: the map art contains plenty of saturated
    /// yellow (mushrooms, flowers) that a fixed threshold picks up as a false
    /// marker. The GUI samples this from a pixel the user points at, which
    /// turns a guess into a measurement.
    std::uint8_t marker_r = 255;
    std::uint8_t marker_g = 216;
    std::uint8_t marker_b = 0;
    int          tolerance = 40;

    /// Reject blobs that are too big or too small to be the marker. The real
    /// one is a compact bar; scattered terrain pixels are not.
    int min_pixels = 3;
    int max_pixels = 400;
    int max_extent = 24;        ///< max bounding-box side, px
};

/// One detection from a frame.
struct MarkerHit {
    bool  found = false;
    float x = 0;                ///< minimap-relative, px
    float y = 0;
    int   pixels = 0;
    int   extent = 0;           ///< bounding-box longest side
};

/// A platform recovered from where the character actually walked.
struct TracedPlatform {
    int   id = 0;
    float x1 = 0, x2 = 0;       ///< minimap pixel coordinates
    float y = 0;
    int   samples = 0;          ///< how much evidence supports it
};

/// Map identity, derived from the minimap's title bar pixels.
///
/// The title is rendered text in a script we are not going to OCR reliably, so
/// instead we fingerprint the pixels. Two visits to the same map produce the
/// same hash; a different map produces a different one. That is all a cache
/// key needs to do.
struct MapIdentity {
    std::uint64_t hash = 0;
    std::string   label;        ///< user-supplied, for their own benefit
    bool valid() const { return hash != 0; }
};

/// Learns a map's walkable geometry by watching where the character goes.
///
/// Deliberately not a platform detector. The map art here is organic — tree
/// branches, roots, foliage — with no geometric signature separating a surface
/// you can stand on from one painted behind it, so any edge-based detector
/// invents platforms out of background and misses real ones.
///
/// Where the character has actually stood is ground truth instead, and needs no
/// interpretation. The trade is that the map is only known where you have been,
/// which is why it accumulates across sessions rather than being built in one
/// pass.
class MapMapper {
public:
    MapMapper();
    ~MapMapper();

    MapMapper(const MapMapper&) = delete;
    MapMapper& operator=(const MapMapper&) = delete;

    void set_target_window(void* hwnd);
    void set_minimap(const MinimapSpec& spec);
    MinimapSpec minimap() const;

    void start();
    void stop();
    bool running() const { return running_.load(); }

    /// Sample the colour at a point in the most recent frame, for calibration.
    bool sample_pixel(int x, int y, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) const;

    /// Most recent frame, for the GUI to display.
    bool latest_frame(Frame& out) const;

    MarkerHit   last_hit() const;
    MapIdentity identity() const;
    void        set_label(const std::string& label);

    /// Every recorded position, minimap-relative.
    std::vector<std::pair<float, float>> trace() const;
    std::size_t trace_size() const;
    void clear_trace();

    /// Platforms derived from the trace so far.
    std::vector<TracedPlatform> platforms() const;

    /// Convert the trace into a tile grid the existing PathFinder consumes.
    MapData build_map_data(int tile_size = 4) const;

    bool save(const std::string& path) const;
    bool load(const std::string& path);

    /// Default file name for the current map's identity.
    std::string suggested_filename() const;

    struct Stats {
        std::uint64_t frames = 0;
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t rejected_size = 0;   ///< blob found but wrong shape
        bool          capture_from_screen = false;
    };
    Stats stats() const;

private:
    void loop();
    MarkerHit find_marker(const Frame& f, const MinimapSpec& spec) const;
    MapIdentity fingerprint(const Frame& f, const MinimapSpec& spec) const;
    void rebuild_platforms();

    std::atomic<void*> hwnd_{nullptr};
    std::atomic<bool>  running_{false};
    std::thread        thread_;

    mutable std::mutex mutex_;
    MinimapSpec        spec_;
    Frame              frame_;
    MarkerHit          hit_;
    MapIdentity        identity_;

    /// Occupancy counts, keyed by quantised position. The modal y for each x
    /// column is the floor: walking dwells on it, a jump arc passes through
    /// any given point once.
    std::vector<std::pair<float, float>> trace_;
    std::vector<TracedPlatform>          platforms_;
    bool                                 platforms_dirty_ = false;

    /// Debounce for the map fingerprint. The hashed region is a live render,
    /// so its value differs on most frames; only one that holds still for
    /// several frames is a genuine map change.
    std::uint64_t pending_hash_ = 0;
    int           pending_count_ = 0;
    static constexpr int kIdentityStableFrames = 25;

    mutable std::mutex stats_mutex_;
    Stats              stats_;

    static constexpr std::size_t kMaxTrace = 200000;
};

} // namespace gobot
