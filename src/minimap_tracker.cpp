#include "gobot/minimap_tracker.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cstdio>

namespace gobot {
namespace {

inline int luma(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return (r * 77 + g * 151 + b * 28) >> 8;
}

void copy_region(const Frame& f, const ScreenRect& r, Frame& out) {
    out.width = r.w;
    out.height = r.h;
    out.pixels.assign(static_cast<std::size_t>(r.w) * r.h * 4, 0);
    for (int y = 0; y < r.h; ++y) {
        for (int x = 0; x < r.w; ++x) {
            std::uint8_t cr, cg, cb;
            f.get(r.x + x, r.y + y, cr, cg, cb);
            const std::size_t i = (static_cast<std::size_t>(y) * r.w + x) * 4;
            out.pixels[i] = cb;
            out.pixels[i + 1] = cg;
            out.pixels[i + 2] = cr;
            out.pixels[i + 3] = 255;
        }
    }
}

std::string to_hex(std::uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::uint64_t from_hex(const std::string& s) {
    return std::strtoull(s.c_str(), nullptr, 16);
}

} // namespace

// -------------------------------------------------------------- tracker

MinimapTracker::Layout MinimapTracker::default_layout(int client_w, int client_h) {
    Layout l;
    if (client_w <= 0 || client_h <= 0) return l;

    // Measured on a 1024x768 client: the picture runs from 6px inside the
    // right edge, 204 wide and 112 tall, starting 26 rows down, with the name
    // bar in the 18 rows above it.
    const int right_margin = 6, w = 204, h = 112, top = 26;
    l.body = ScreenRect{ client_w - right_margin - w, top, w, h };

    // The title is the writing, not the whole bar. The bar's own top and
    // bottom edges are bright lines running its full width, and a reader
    // handed one will attach it to the nearest letter -- an 'Â' with that
    // lower edge beneath it makes a perfectly good 'Ậ'. Keeping the edges out
    // is the difference between reading the map's name and nearly reading it.
    l.title = ScreenRect{ client_w - right_margin - w + 2, top - 16, w - 4, 13 };
    return l;
}

MinimapTracker::Spot MinimapTracker::find_marker(const Frame& f, const ScreenRect& body) {
    Spot out;
    if (!f.valid() || !body.valid()) return out;

    const int x0 = std::max(0, body.x), y0 = std::max(0, body.y);
    const int x1 = std::min(f.width, body.right()), y1 = std::min(f.height, body.bottom());

    // The marker's own pixels, averaged. It is a small solid block a few
    // pixels across, so the mean of its pixels is its middle -- and taking the
    // mean rather than the first hit keeps the answer steady as the block's
    // edges blend differently against whatever map art is behind them.
    double sx = 0.0, sy = 0.0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            std::uint8_t r, g, b;
            f.get(x, y, r, g, b);
            if (!is_marker(r, g, b)) continue;
            sx += x;
            sy += y;
            ++out.pixels;
        }
    }
    if (out.pixels < kMinMarkerPixels) return out;

    out.x = static_cast<float>(sx / out.pixels - body.x);
    out.y = static_cast<float>(sy / out.pixels - body.y);
    out.valid = true;
    return out;
}

void MinimapTracker::set_layout(const Layout& l) {
    std::lock_guard<std::mutex> lock(mutex_);
    layout_ = l;
    fingerprint_ = pending_ = 0;
    pending_count_ = 0;
}

MinimapTracker::Layout MinimapTracker::layout() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return layout_;
}

void MinimapTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    fingerprint_ = pending_ = 0;
    pending_count_ = 0;
    changed_ = false;
}

std::uint64_t MinimapTracker::fingerprint() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fingerprint_;
}

int MinimapTracker::stable_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_count_;
}

bool MinimapTracker::take_changed() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool c = changed_;
    changed_ = false;
    return c;
}

bool MinimapTracker::body_image(Frame& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!body_.valid()) return false;
    out = body_;
    return true;
}

bool MinimapTracker::title_image(Frame& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!title_.valid()) return false;
    out = title_;
    return true;
}

int MinimapTracker::distance(std::uint64_t a, std::uint64_t b) {
    std::uint64_t x = a ^ b;
    int n = 0;
    while (x) { x &= x - 1; ++n; }
    return n;
}

void MinimapTracker::capture_images(const Frame& f) {
    copy_region(f, layout_.body, body_);
    if (layout_.title.valid()) copy_region(f, layout_.title, title_);

    // Block averages, which is what makes this survive the weather: a handful
    // of animated specks barely move the mean of a 23x14 block.
    grey_.assign(static_cast<std::size_t>(kHashW) * kHashH, 0);
    const ScreenRect& b = layout_.body;
    for (int cy = 0; cy < kHashH; ++cy) {
        for (int cx = 0; cx < kHashW; ++cx) {
            const int x0 = b.x + b.w * cx / kHashW;
            const int x1 = b.x + b.w * (cx + 1) / kHashW;
            const int y0 = b.y + b.h * cy / kHashH;
            const int y1 = b.y + b.h * (cy + 1) / kHashH;
            long long sum = 0;
            int n = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    std::uint8_t r, g, bl;
                    f.get(x, y, r, g, bl);
                    sum += luma(r, g, bl);
                    ++n;
                }
            }
            grey_[static_cast<std::size_t>(cy) * kHashW + cx] =
                static_cast<std::uint8_t>(n ? sum / n : 0);
        }
    }
}

std::uint64_t MinimapTracker::hash_body() const {
    // A difference hash: each bit says whether one block is brighter than the
    // one to its right. Differences survive what absolute levels do not --
    // the panel dims and brightens with the time of day in the level.
    std::uint64_t h = 0;
    int bit = 0;
    for (int y = 0; y < kHashH; ++y) {
        for (int x = 0; x + 1 < kHashW; ++x, ++bit) {
            const std::size_t i = static_cast<std::size_t>(y) * kHashW + x;
            if (grey_[i] > grey_[i + 1]) h |= (1ull << bit);
        }
    }
    return h;
}

void MinimapTracker::update(const Frame& f) {
    if (!f.valid()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!layout_.valid()) layout_ = default_layout(f.width, f.height);
    if (layout_.body.right() > f.width || layout_.body.bottom() > f.height) return;

    capture_images(f);
    const std::uint64_t h = hash_body();

    if (distance(h, pending_) <= kSameMap) {
        ++pending_count_;
    } else {
        pending_ = h;
        pending_count_ = 1;
    }

    if (pending_count_ >= kStableFrames && distance(h, fingerprint_) > kSameMap) {
        fingerprint_ = h;
        changed_ = true;
        GB_INFO("Map changed -- minimap fingerprint %s", to_hex(h).c_str());
    }
}

// -------------------------------------------------------------- map book

const MapBook::Entry* MapBook::find(std::uint64_t fp) const {
    const Entry* best = nullptr;
    int best_d = MinimapTracker::kSameMap + 1;
    for (const Entry& e : entries_) {
        const int d = MinimapTracker::distance(e.fingerprint, fp);
        if (d < best_d) { best_d = d; best = &e; }
    }
    return best;
}

MapBook::Entry* MapBook::find_mut(std::uint64_t fp) {
    Entry* best = nullptr;
    int best_d = MinimapTracker::kSameMap + 1;
    for (Entry& e : entries_) {
        const int d = MinimapTracker::distance(e.fingerprint, fp);
        if (d < best_d) { best_d = d; best = &e; }
    }
    return best;
}

std::string MapBook::name_for(std::uint64_t fp) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Entry* e = find(fp);
    return e ? e->name : std::string{};
}

void MapBook::set_name(std::uint64_t fp, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Entry* e = find_mut(fp)) {
        e->name = name;
        // Re-anchor on the fingerprint we actually saw: the stored one may be
        // a few bits away and drifting toward the live picture keeps the match
        // tight as the level's weather cycles.
        e->fingerprint = fp;
        return;
    }
    entries_.push_back(Entry{ fp, name, {} });
}

std::string MapBook::map_file_for(std::uint64_t fp) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Entry* e = find(fp);
    return e ? e->map_file : std::string{};
}

void MapBook::set_map_file(std::uint64_t fp, const std::string& stem) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Entry* e = find_mut(fp)) { e->map_file = stem; return; }
    entries_.push_back(Entry{ fp, {}, stem });
}

std::vector<MapBook::Entry> MapBook::entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

std::size_t MapBook::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

bool MapBook::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    for (int i = 0; i < 4096; ++i) {
        char sec[32];
        std::snprintf(sec, sizeof(sec), "Map.%d", i);
        if (!ini.has_section(sec)) continue;
        Entry e;
        e.fingerprint = from_hex(ini.get(sec, "fingerprint", "0"));
        e.name        = ini.get(sec, "name", "");
        e.map_file    = ini.get(sec, "map_file", "");
        if (e.fingerprint != 0) entries_.push_back(std::move(e));
    }
    GB_INFO("MapBook: %zu map(s) from %s", entries_.size(), path.c_str());
    return true;
}

bool MapBook::save(const std::string& path) const {
    IniFile ini;
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        char sec[32];
        std::snprintf(sec, sizeof(sec), "Map.%zu", i);
        ini.set(sec, "fingerprint", to_hex(entries_[i].fingerprint));
        ini.set(sec, "name", entries_[i].name);
        ini.set(sec, "map_file", entries_[i].map_file);
    }
    return ini.save(path, "GoBot map names, keyed by what the minimap looks like");
}

} // namespace gobot
