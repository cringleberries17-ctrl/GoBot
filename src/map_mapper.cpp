#include "gobot/map_mapper.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>
#include <thread>

namespace gobot {
namespace {

bool close_enough(std::uint8_t a, std::uint8_t b, int tol) {
    return std::abs(static_cast<int>(a) - static_cast<int>(b)) <= tol;
}

} // namespace

MapMapper::MapMapper() = default;

MapMapper::~MapMapper() { stop(); }

void MapMapper::set_target_window(void* hwnd) { hwnd_.store(hwnd); }

void MapMapper::set_minimap(const MinimapSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    spec_ = spec;
}

MinimapSpec MapMapper::minimap() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return spec_;
}

void MapMapper::start() {
    if (running_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&MapMapper::loop, this);
    GB_INFO("Map mapper started");
}

void MapMapper::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    GB_INFO("Map mapper stopped");
}

// ------------------------------------------------------------------ capture

void MapMapper::loop() {
    ScreenCapture capture;
    Frame frame;

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!running_.load()) break;

        void* hwnd = hwnd_.load();
        if (!hwnd) continue;

        if (!capture.grab(hwnd, frame)) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.misses;
            continue;
        }

        MinimapSpec spec;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            spec = spec_;
        }

        const MarkerHit hit = find_marker(frame, spec);
        const MapIdentity id = fingerprint(frame, spec);

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.frames;
            if (hit.found) ++stats_.hits;
            else if (hit.pixels > 0) ++stats_.rejected_size;
            stats_.capture_from_screen = capture.from_screen();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        frame_ = frame;
        hit_ = hit;

        // A changed fingerprint means a different map; the trace belongs to
        // the old one and must not bleed into the new.
        //
        // Debounced, because the region being hashed is a live render: other
        // players walk through it, effects animate, and the hash therefore
        // differs on almost every frame. Acting on the first difference threw
        // the trace away ten times a second and the mapper could never
        // accumulate anything. Only a value that holds still is a real map
        // change.
        if (id.valid()) {
            if (id.hash == pending_hash_) {
                ++pending_count_;
            } else {
                pending_hash_ = id.hash;
                pending_count_ = 1;
            }

            if (pending_count_ >= kIdentityStableFrames && id.hash != identity_.hash) {
                if (identity_.valid()) {
                    GB_INFO("Map changed (%016llx -> %016llx); clearing trace",
                            static_cast<unsigned long long>(identity_.hash),
                            static_cast<unsigned long long>(id.hash));
                    trace_.clear();
                    platforms_.clear();
                    platforms_dirty_ = true;
                }
                identity_.hash = id.hash;
            }
        }

        if (!hit.found) continue;

        // Record only genuine movement. Standing still would otherwise pile
        // thousands of samples onto one spot and dominate the histogram.
        if (!trace_.empty()) {
            const auto& last = trace_.back();
            if (std::abs(last.first - hit.x) < 0.5f && std::abs(last.second - hit.y) < 0.5f)
                continue;
        }

        if (trace_.size() < kMaxTrace) {
            trace_.emplace_back(hit.x, hit.y);
            platforms_dirty_ = true;
        }
    }
}

// ------------------------------------------------------------------ marker

MarkerHit MapMapper::find_marker(const Frame& f, const MinimapSpec& spec) const {
    MarkerHit hit;
    if (!f.valid()) return hit;

    const int x0 = f.width - spec.right_margin - spec.width;
    const int y0 = spec.top;
    if (x0 < 0 || y0 < 0) return hit;

    long long sum_x = 0, sum_y = 0;
    int count = 0;
    int min_x = INT32_MAX, max_x = -1, min_y = INT32_MAX, max_y = -1;

    for (int y = y0; y < std::min(y0 + spec.height, f.height); ++y) {
        for (int x = x0; x < std::min(x0 + spec.width, f.width); ++x) {
            std::uint8_t r, g, b;
            f.get(x, y, r, g, b);
            if (!close_enough(r, spec.marker_r, spec.tolerance) ||
                !close_enough(g, spec.marker_g, spec.tolerance) ||
                !close_enough(b, spec.marker_b, spec.tolerance))
                continue;

            sum_x += x; sum_y += y; ++count;
            min_x = std::min(min_x, x); max_x = std::max(max_x, x);
            min_y = std::min(min_y, y); max_y = std::max(max_y, y);
        }
    }

    hit.pixels = count;
    if (count == 0) return hit;

    hit.extent = std::max(max_x - min_x, max_y - min_y) + 1;

    // Shape and size are what separate the marker from terrain art of a
    // similar colour. Scattered mushrooms span the whole minimap; the marker
    // is a small compact blob.
    if (count < spec.min_pixels || count > spec.max_pixels || hit.extent > spec.max_extent)
        return hit;    // found pixels, rejected as the wrong shape

    hit.found = true;
    hit.x = static_cast<float>(static_cast<double>(sum_x) / count - x0);
    hit.y = static_cast<float>(static_cast<double>(sum_y) / count - y0);
    return hit;
}

MapIdentity MapMapper::fingerprint(const Frame& f, const MinimapSpec& spec) const {
    MapIdentity id;
    if (!f.valid()) return id;

    // The title bar sits directly above the minimap body. Hashing its pixels
    // identifies the map without reading the text, which is in a script we
    // would not OCR reliably anyway.
    const int x0 = f.width - spec.right_margin - spec.width;
    const int y1 = spec.top;
    const int y0 = std::max(0, y1 - 22);

    std::uint64_t h = 1469598103934665603ull;
    int sampled = 0;

    for (int y = y0; y < y1; y += 2) {
        for (int x = x0; x < std::min(x0 + spec.width, f.width); x += 2) {
            std::uint8_t r, g, b;
            f.get(x, y, r, g, b);
            // Quantise: anti-aliasing makes exact pixel values vary slightly
            // between frames, and the hash has to be stable across them.
            const std::uint8_t q = static_cast<std::uint8_t>(((r >> 5) << 5) |
                                                             ((g >> 5) << 2) |
                                                              (b >> 6));
            h ^= q;
            h *= 1099511628211ull;
            ++sampled;
        }
    }
    if (sampled < 16) return id;

    id.hash = h;
    return id;
}

// ------------------------------------------------------------------ queries

bool MapMapper::sample_pixel(int x, int y, std::uint8_t& r, std::uint8_t& g,
                             std::uint8_t& b) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!frame_.valid()) return false;
    if (x < 0 || y < 0 || x >= frame_.width || y >= frame_.height) return false;
    frame_.get(x, y, r, g, b);
    return true;
}

bool MapMapper::latest_frame(Frame& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!frame_.valid()) return false;
    out = frame_;
    return true;
}

MarkerHit MapMapper::last_hit() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hit_;
}

MapIdentity MapMapper::identity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return identity_;
}

void MapMapper::set_label(const std::string& label) {
    std::lock_guard<std::mutex> lock(mutex_);
    identity_.label = label;
}

std::vector<std::pair<float, float>> MapMapper::trace() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return trace_;
}

std::size_t MapMapper::trace_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return trace_.size();
}

void MapMapper::clear_trace() {
    std::lock_guard<std::mutex> lock(mutex_);
    trace_.clear();
    platforms_.clear();
    platforms_dirty_ = false;
}

MapMapper::Stats MapMapper::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

// --------------------------------------------------------------- platforms

void MapMapper::rebuild_platforms() {
    // Caller holds the lock.
    platforms_.clear();
    platforms_dirty_ = false;
    if (trace_.size() < 20) return;

    // Occupancy histogram over a 1px grid.
    //
    // For each x column the most-occupied y is the floor. Walking dwells on a
    // surface and so revisits the same cells repeatedly; a jump arc passes
    // through any given cell roughly once. That difference is what separates
    // ground from air without needing to know when the character was airborne.
    std::map<int, std::map<int, int>> column;
    for (const auto& [x, y] : trace_)
        ++column[static_cast<int>(std::lround(x))][static_cast<int>(std::lround(y))];

    struct Floor { int x; int y; int weight; };
    std::vector<Floor> floors;

    for (const auto& [x, ys] : column) {
        int best_y = 0, best_n = 0, total = 0;
        for (const auto& [y, n] : ys) {
            total += n;
            if (n > best_n) { best_n = n; best_y = y; }
        }
        // Require the mode to actually dominate. A column crossed only in
        // mid-air has its samples spread thin, and admitting it would draw a
        // platform through empty space.
        if (best_n >= 3 && best_n * 3 >= total)
            floors.push_back({ x, best_y, best_n });
    }

    std::sort(floors.begin(), floors.end(),
              [](const Floor& a, const Floor& b) { return a.x < b.x; });

    // Join adjacent columns of similar height into runs.
    constexpr int kMaxGap = 3;      ///< px of x allowed between columns
    constexpr int kMaxStep = 2;     ///< px of y allowed between neighbours

    for (std::size_t i = 0; i < floors.size();) {
        std::size_t j = i;
        long long sum_y = floors[i].y;
        int weight = floors[i].weight;

        while (j + 1 < floors.size() &&
               floors[j + 1].x - floors[j].x <= kMaxGap &&
               std::abs(floors[j + 1].y - floors[j].y) <= kMaxStep) {
            ++j;
            sum_y += floors[j].y;
            weight += floors[j].weight;
        }

        const int span = floors[j].x - floors[i].x;
        if (span >= 2) {            // a single column is noise, not a platform
            TracedPlatform p;
            p.id = static_cast<int>(platforms_.size());
            p.x1 = static_cast<float>(floors[i].x);
            p.x2 = static_cast<float>(floors[j].x);
            p.y = static_cast<float>(static_cast<double>(sum_y) / (j - i + 1));
            p.samples = weight;
            platforms_.push_back(p);
        }
        i = j + 1;
    }
}

std::vector<TracedPlatform> MapMapper::platforms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (platforms_dirty_) const_cast<MapMapper*>(this)->rebuild_platforms();
    return platforms_;
}

MapData MapMapper::build_map_data(int tile_size) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (platforms_dirty_) const_cast<MapMapper*>(this)->rebuild_platforms();

    MapData map;
    if (platforms_.empty()) return map;

    MinimapSpec spec = spec_;
    if (tile_size < 1) tile_size = 1;

    map.name   = identity_.label.empty() ? "traced" : identity_.label;
    map.id     = static_cast<std::int32_t>(identity_.hash & 0x7FFFFFFF);
    map.tile_w = tile_size;
    map.tile_h = tile_size;
    map.width  = std::max(1, (spec.width  + tile_size - 1) / tile_size);
    map.height = std::max(1, (spec.height + tile_size - 1) / tile_size);
    map.attrs.assign(static_cast<std::size_t>(map.width) * map.height, Tile_Empty);

    for (const auto& p : platforms_) {
        const int ty = std::clamp(static_cast<int>(std::lround(p.y)) / tile_size, 0, map.height - 1);
        const int tx1 = std::clamp(static_cast<int>(std::lround(p.x1)) / tile_size, 0, map.width - 1);
        const int tx2 = std::clamp(static_cast<int>(std::lround(p.x2)) / tile_size, 0, map.width - 1);
        for (int tx = tx1; tx <= tx2; ++tx)
            map.attrs[static_cast<std::size_t>(ty) * map.width + tx] = Tile_Ground;
    }
    return map;
}

// ------------------------------------------------------------------- files

std::string MapMapper::suggested_filename() const {
    std::lock_guard<std::mutex> lock(mutex_);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "map_%016llx.ini",
                  static_cast<unsigned long long>(identity_.hash));
    return buf;
}

bool MapMapper::save(const std::string& path) const {
    std::vector<TracedPlatform> plats = platforms();
    MapIdentity id = identity();
    MinimapSpec spec = minimap();

    IniFile ini;
    char hash[32];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(id.hash));
    ini.set("Map", "hash", hash);
    ini.set("Map", "label", id.label);
    ini.set_int("Map", "minimap_width", spec.width);
    ini.set_int("Map", "minimap_height", spec.height);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ini.set_int("Map", "trace_samples", static_cast<int>(trace_.size()));
    }

    ini.set_int("Platforms", "count", static_cast<int>(plats.size()));
    for (std::size_t i = 0; i < plats.size(); ++i) {
        const std::string p = "p" + std::to_string(i) + "_";
        ini.set_float("Platforms", p + "x1", plats[i].x1);
        ini.set_float("Platforms", p + "x2", plats[i].x2);
        ini.set_float("Platforms", p + "y",  plats[i].y);
        ini.set_int  ("Platforms", p + "samples", plats[i].samples);
    }

    if (!ini.save(path, "GoBot traced map -- learned by walking, not by looking"))
        return false;

    GB_INFO("Saved %zu platforms to %s", plats.size(), path.c_str());
    return true;
}

bool MapMapper::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) return false;

    std::vector<TracedPlatform> plats;
    const int n = ini.get_int("Platforms", "count", 0);
    for (int i = 0; i < n; ++i) {
        const std::string p = "p" + std::to_string(i) + "_";
        TracedPlatform tp;
        tp.id      = i;
        tp.x1      = ini.get_float("Platforms", p + "x1", 0);
        tp.x2      = ini.get_float("Platforms", p + "x2", 0);
        tp.y       = ini.get_float("Platforms", p + "y", 0);
        tp.samples = ini.get_int  ("Platforms", p + "samples", 0);
        plats.push_back(tp);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    platforms_ = std::move(plats);
    platforms_dirty_ = false;
    identity_.label = ini.get("Map", "label", "");
    const std::string h = ini.get("Map", "hash", "");
    if (!h.empty()) identity_.hash = std::strtoull(h.c_str(), nullptr, 16);

    GB_INFO("Loaded %zu platforms from %s", platforms_.size(), path.c_str());
    return true;
}

} // namespace gobot
