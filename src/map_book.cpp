#include "gobot/map_book.hpp"
#include "gobot/logger.hpp"
#include "gobot/map_file.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace gobot {
namespace {

constexpr std::size_t kRecordBytes = 36;
constexpr std::size_t kAttrOffset = 2;      // within a record
constexpr std::size_t kHeightOffset = 3;
constexpr std::uint8_t kNoSurface = 0xFF;
/// A whole file read for a page is never larger than this: the game's
/// largest .map is 2.4 MB, its largest .prj under 1 MB.
constexpr long long kFileMax = 64LL * 1024 * 1024;

std::int32_t i32_at(const std::uint8_t* p) {
    std::int32_t v = 0;
    std::memcpy(&v, p, sizeof v);
    return v;
}

/// Where a record is in a planar .map: every layer-0 record, then every
/// layer-1 record, each block column by column.
std::size_t record_at(std::size_t w, std::size_t h, int layer, int cx, int cy) {
    return 8 + static_cast<std::size_t>(layer) * w * h * kRecordBytes +
           (static_cast<std::size_t>(cx) * h + static_cast<std::size_t>(cy)) * kRecordBytes;
}

char lower(std::uint8_t c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
}

/// `n` bytes at `p` are `text`, whatever the case.
bool same_nocase(const std::uint8_t* p, const char* text, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (lower(p[i]) != text[i]) return false;
    return true;
}

bool name_char(std::uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/// A stem is a file name GoBot makes or lists -- letters, digits, _ - and a
/// dot not first (the game ships a t58.s0.map) -- never a path.
bool plain_stem(const std::string& stem) {
    if (stem.empty() || stem.size() > 64 || stem[0] == '.' || stem.find("..") != std::string::npos) return false;
    return std::all_of(stem.begin(), stem.end(), [](char c) {
        return name_char(static_cast<std::uint8_t>(c)) || c == '.' || c == '-';
    });
}

bool read_file(const std::string& path, Bytes& out) {
    out.clear();
    std::FILE* f = open_file(path, "rb");
    if (f == nullptr) return false;
    bool ok = _fseeki64(f, 0, SEEK_END) == 0;
    const long long size = ok ? _ftelli64(f) : -1;
    ok = ok && size >= 0 && size <= kFileMax && _fseeki64(f, 0, SEEK_SET) == 0;
    if (ok) {
        out.resize(static_cast<std::size_t>(size));
        ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
    }
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
}

std::string gates_text(const std::vector<MapGate>& gates) {
    std::string s;
    for (std::size_t i = 0; i < gates.size() && i < 6; ++i) {
        if (i > 0) s += "; ";
        s += std::to_string(gates[i].x) + "," + std::to_string(gates[i].y);
    }
    if (gates.size() > 6) s += "; ...";
    return s;
}

} // namespace

// ------------------------------------------------------------------ floors

std::span<const MapFloor> MapFloors::at(int x) const {
    if (x < 0 || x >= w_px() || start.size() != static_cast<std::size_t>(w_px()) + 1) return {};
    const std::size_t from = start[static_cast<std::size_t>(x)];
    const std::size_t to = start[static_cast<std::size_t>(x) + 1];
    if (from > to || to > floors.size()) return {};
    return std::span<const MapFloor>(floors.data() + from, to - from);
}

std::optional<MapFloor> GameMapBook::Page::floor_near(int x, int y) const {
    for (const MapFloor& f : floors.at(x))
        if (y >= f.y - kOnFloorPx && y <= f.y + f.depth + kOnFloorPx) return f;
    return std::nullopt;
}

std::optional<MapFloor> GameMapBook::Page::floor_below(int x, int y, int gap) const {
    for (const MapFloor& f : floors.at(x))
        if (f.y > y + gap) return f;
    return std::nullopt;
}

MapTrail GameMapBook::Page::follow(int x, int y, int dir, int max_px) const {
    MapTrail t;
    const std::optional<MapFloor> start = floor_near(x, y);
    if (!start || start->wall()) return t;
    const int step = dir < 0 ? -1 : 1;
    const int w = floors.w_px();
    t.pts.push_back({ x, start->y });
    for (;;) {
        const MapPoint last = t.pts.back();
        if (std::abs(last.x - x) >= max_px) return t;
        // The next column's floor nearest the last top, within kFollowLinkPx;
        // with none, the columns after it, across a hole of kFollowHolePx.
        const MapFloor* next = nullptr;
        int at = last.x;
        for (int gap = 1; gap <= kFollowHolePx + 1 && next == nullptr; ++gap) {
            at = last.x + step * gap;
            if (at < 0 || at >= w) {
                t.stop = true;      // the map's edge: no walking on, nor off
                return t;
            }
            for (const MapFloor& f : floors.at(at)) {
                const int d = std::abs(f.y - last.y);
                if (d <= kFollowLinkPx && (next == nullptr || d < std::abs(next->y - last.y))) next = &f;
            }
        }
        if (next == nullptr) {
            t.ledge = true;
            return t;
        }
        if (next->wall()) {
            t.stop = true;
            return t;
        }
        t.pts.push_back({ at, next->y });
    }
}

std::optional<int> GameMapBook::Page::follow_to(int x, int y, int to_x) const {
    const MapTrail t = follow(x, y, to_x < x ? -1 : 1, std::abs(to_x - x));
    // The last point not past to_x: to_x's own, or the one before its hole.
    const MapPoint* got = nullptr;
    bool reached = false;
    for (const MapPoint& p : t.pts) {
        const bool past = to_x < x ? p.x < to_x : p.x > to_x;
        if (p.x == to_x || past) reached = true;
        if (!past) got = &p;
    }
    if (!reached || got == nullptr) return std::nullopt;
    return got->y;
}

std::string GameMapBook::Page::gap_text(int x, int y) const {
    char text[160];
    if (x < 0 || x >= floors.w_px()) {
        std::snprintf(text, sizeof text, "outside the map's %d px across", floors.w_px());
        return text;
    }
    const std::span<const MapFloor> column = floors.at(x);
    if (column.empty()) return "no floor in its column";
    if (const std::optional<MapFloor> on = floor_near(x, y)) {
        std::snprintf(text, sizeof text, "on floor %d (attr %u)", on->y, static_cast<unsigned>(on->attr));
        return text;
    }
    // Top first: the first floor lower than the point is the nearest below
    // it, the last one not lower the nearest above.
    const MapFloor* below = nullptr;
    const MapFloor* above = nullptr;
    for (const MapFloor& f : column) {
        if (f.y > y) {
            if (below == nullptr) below = &f;
        } else {
            above = &f;
        }
    }
    std::string s;
    if (below != nullptr) {
        std::snprintf(text, sizeof text, "%d px above floor %d (attr %u)", below->y - y, below->y,
                      static_cast<unsigned>(below->attr));
        s = text;
    } else {
        s = "no floor below it";
    }
    if (above != nullptr) {
        std::snprintf(text, sizeof text, ", %d px below floor %d (attr %u)", y - above->y, above->y,
                      static_cast<unsigned>(above->attr));
        s += text;
    } else {
        s += ", no floor above it";
    }
    return s;
}

std::optional<MapFloors> GameMapBook::parse_map(const Bytes& file) {
    if (file.size() < 8) return std::nullopt;
    const std::int32_t w = i32_at(file.data());
    const std::int32_t h = i32_at(file.data() + 4);
    if (w <= 0 || h <= 0 || w > kMaxCells || h > kMaxCells) return std::nullopt;
    const std::size_t uw = static_cast<std::size_t>(w);
    const std::size_t uh = static_cast<std::size_t>(h);
    // The size identity is what says the file is this format: every one of
    // the game's 781 maps is exactly this long.
    if (file.size() != 8 + uw * uh * 2 * kRecordBytes) return std::nullopt;

    MapFloors out;
    out.w_cells = w;
    out.h_cells = h;
    const int w_px = w * kCellPx;
    out.start.reserve(static_cast<std::size_t>(w_px) + 1);

    struct Row {
        int          y;
        std::uint8_t attr;
        std::uint8_t layer;
    };
    std::vector<Row> rows;
    rows.reserve(static_cast<std::size_t>(h) * 2);
    for (int x = 0; x < w_px; ++x) {
        out.start.push_back(static_cast<std::uint32_t>(out.floors.size()));
        const int cx = x / kCellPx;
        const std::size_t i = static_cast<std::size_t>(x % kCellPx);
        rows.clear();
        std::size_t first_layer = 0;
        for (int layer = 0; layer < 2; ++layer) {
            for (int cy = 0; cy < h; ++cy) {
                const std::uint8_t* rec = file.data() + record_at(uw, uh, layer, cx, cy);
                const std::uint8_t v = rec[kHeightOffset + i];
                if (v == kNoSurface) continue;
                rows.push_back({ cy * kCellPx + std::min<int>(v, kCellPx - 1), rec[kAttrOffset],
                                 static_cast<std::uint8_t>(layer) });
            }
            if (layer == 0) first_layer = rows.size();
        }
        // Each layer's rows came down the column in order; the two are merged
        // by y, layer 0's first where both have one.
        std::inplace_merge(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(first_layer), rows.end(),
                           [](const Row& a, const Row& b) { return a.y < b.y; });
        const std::size_t column = out.floors.size();
        for (const Row& r : rows) {
            if (out.floors.size() > column && r.y - out.floors.back().y <= kMergePx) {
                MapFloor& f = out.floors.back();
                f.depth = static_cast<std::uint8_t>(r.y - f.y);
                if (f.attr == 1 && r.attr != 1) f.attr = r.attr;
                continue;
            }
            MapFloor f;
            f.y = r.y;
            f.attr = r.attr;
            f.layer = r.layer;
            out.floors.push_back(f);
        }
    }
    out.start.push_back(static_cast<std::uint32_t>(out.floors.size()));
    return out;
}

// ------------------------------------------------------------------- gates

std::vector<MapGate> GameMapBook::parse_prj(const Bytes& file) {
    // The kinds first: [i32 id][u32 type] right before the sprite's path. The
    // four bytes before the id are a tag the kinds all have, but they are
    // not asked for: a gate missed is a drop near a gate, an odd one only a
    // drop refused.
    static const char kDir[] = "\\data\\obj\\object\\";
    static const char* const kGateSprites[] = { "warpzone", "ind_portal" };
    constexpr std::size_t kDirLen = sizeof kDir - 1;
    const std::size_t n = file.size();
    const std::uint8_t* b = file.data();

    std::set<std::int32_t> kinds;
    for (std::size_t i = 8; i + kDirLen <= n; ++i) {
        if (b[i] != '\\' || !same_nocase(b + i, kDir, kDirLen)) continue;
        std::size_t j = i + kDirLen;
        std::string name;
        while (j < n && name.size() < 64 && name_char(b[j])) name += lower(b[j++]);
        if (j + 5 > n || !same_nocase(b + j, ".spr", 4) || b[j + 4] != 0) continue;
        if (std::none_of(std::begin(kGateSprites), std::end(kGateSprites),
                         [&](const char* g) { return name == g; }))
            continue;
        const std::int32_t id = i32_at(b + i - 8);
        if (id > 0 && id < 100000000) kinds.insert(id);
    }

    // Then each placing of one: [i32 kind][u8][f32 1.0][i32 x][i32 y].
    std::vector<MapGate> gates;
    if (kinds.empty()) return gates;
    for (std::size_t k = 0; k + 17 <= n; ++k) {
        if (b[k + 5] != 0x00 || b[k + 6] != 0x00 || b[k + 7] != 0x80 || b[k + 8] != 0x3F) continue;
        const std::int32_t kind = i32_at(b + k);
        if (kinds.count(kind) == 0) continue;
        MapGate g;
        g.x = i32_at(b + k + 9);
        g.y = i32_at(b + k + 13);
        g.kind = kind;
        const bool twice = std::any_of(gates.begin(), gates.end(), [&](const MapGate& o) {
            return o.x == g.x && o.y == g.y && o.kind == g.kind;
        });
        if (!twice) gates.push_back(g);
    }
    return gates;
}

std::string GameMapBook::stem_of(std::uint16_t map, std::uint16_t area) {
    char stem[32];
    std::snprintf(stem, sizeof stem, "t%u_s%u", static_cast<unsigned>(map), static_cast<unsigned>(area));
    return stem;
}

std::string GameMapBook::default_data_dir() {
    // data\Map's own folder, found the one way MapFile finds it.
    const std::string map_dir = MapFile::default_map_dir();
    const std::size_t slash = map_dir.find_last_of("\\/");
    if (map_dir.empty() || slash == std::string::npos || slash == 0) return {};
    return map_dir.substr(0, slash);
}

// -------------------------------------------------------------------- book

GameMapBook::GameMapBook() = default;

GameMapBook::GameMapBook(std::string data_dir) : dir_(std::move(data_dir)), looked_(true) {}

std::string GameMapBook::data_dir() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (looked_) return dir_;
    }
    // The installer's record, asked with no lock held.
    std::string dir = default_data_dir();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!looked_) {
        looked_ = true;
        dir_ = std::move(dir);
        if (dir_.empty())
            GB_INFO("Map book: the game's data folder (data\\Map, data\\Project) was not found -- no floors or "
                    "gates for any map");
        else
            GB_INFO("Map book: the game's data folder is %s", dir_.c_str());
    }
    return dir_;
}

std::shared_ptr<const GameMapBook::Page> GameMapBook::read_page(const std::string& dir, const std::string& stem) const {
    Bytes bytes;
    const std::string map_path = dir + "\\Map\\" + stem + ".map";
    if (!read_file(map_path, bytes)) {
        GB_INFO("Map book: %s -- no map file %s: no floors or gates for it", stem.c_str(), map_path.c_str());
        return nullptr;
    }
    std::optional<MapFloors> floors = parse_map(bytes);
    if (!floors) {
        GB_WARN("Map book: %s -- %s is not a map file that reads (%zu bytes): no floors or gates for it",
                stem.c_str(), map_path.c_str(), bytes.size());
        return nullptr;
    }
    auto page = std::make_shared<Page>();
    page->stem = stem;
    page->floors = std::move(*floors);
    if (read_file(dir + "\\Project\\" + stem + ".prj", bytes)) {
        page->prj_read = true;
        page->gates = parse_prj(bytes);
    }
    std::string gates = "no .prj, so no gates";
    if (page->prj_read)
        gates = page->gates.empty() ? std::string("no gates in its .prj")
                                    : std::to_string(page->gates.size()) + " gate(s): " + gates_text(page->gates);
    GB_INFO("Map book: %s from the game's files -- floors %dx%d px, %s", stem.c_str(), page->floors.w_px(),
            page->floors.h_px(), gates.c_str());
    return page;
}

std::shared_ptr<const GameMapBook::Page> GameMapBook::get(const std::string& stem) {
    if (!plain_stem(stem)) return nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pages_.find(stem);
        if (it != pages_.end()) return it->second;
    }
    const std::string dir = data_dir();
    std::shared_ptr<const Page> page = dir.empty() ? nullptr : read_page(dir, stem);
    std::lock_guard<std::mutex> lock(mutex_);
    // Another thread may have read it meanwhile: the first one kept stays.
    return pages_.emplace(stem, std::move(page)).first->second;
}

std::vector<std::string> GameMapBook::stems() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stems_) return *stems_;
    }
    const std::string dir = data_dir();
    std::vector<std::string> list;
    if (!dir.empty()) {
        bool complete = true;
        for (const std::string& name : files_in(dir + "\\Map", "*.map", &complete)) {
            if (name.size() <= 4) continue;
            const std::string ext = name.substr(name.size() - 4);
            if (!same_nocase(reinterpret_cast<const std::uint8_t*>(ext.data()), ".map", 4)) continue;
            const std::string stem = name.substr(0, name.size() - 4);
            if (plain_stem(stem)) list.push_back(stem);
        }
        // A listing Windows cut short is neither kept nor used: one file fits
        // among some, and not among all, and naming goes by all of them. Asked
        // again next time.
        if (!complete) {
            GB_DEBUG("Map book: the list of %s\\Map stopped short (%zu files) -- asked again later", dir.c_str(),
                     list.size());
            return {};
        }
        std::sort(list.begin(), list.end());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stems_) stems_ = std::move(list);
    return *stems_;
}

std::size_t GameMapBook::map_files() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stems_) return stems_->size();
    }
    return stems().size();
}

int GameMapBook::misses(const std::string& stem, std::span<const MapPoint> points) {
    if (!plain_stem(stem)) return -1;
    std::shared_ptr<const Page> page;
    {
        // A page not kept -- its read failed, or the file is not a map -- is
        // asked of the file itself: which of the two it was decides.
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pages_.find(stem);
        if (it != pages_.end() && it->second) page = it->second;
    }
    if (page) {
        int off = 0;
        for (const MapPoint& p : points)
            if (!page->on_floor(p)) ++off;
        return off;
    }

    const std::string dir = data_dir();
    if (dir.empty()) return -1;
    std::FILE* f = open_file(dir + "\\Map\\" + stem + ".map", "rb");
    if (f == nullptr) return -2;
    // The size first, then the head: a seek, a tell or a read that fails on
    // a file whose size and head say a map is a read that failed (-2), not a
    // file that is no map (-1).
    bool io = _fseeki64(f, 0, SEEK_END) == 0;
    const long long size = io ? _ftelli64(f) : -1;
    io = io && size >= 0;
    std::uint8_t head[8]{};
    bool ok = io && size >= static_cast<long long>(sizeof head);
    if (ok) {
        io = _fseeki64(f, 0, SEEK_SET) == 0 && std::fread(head, 1, sizeof head, f) == sizeof head;
        ok = io;
    }
    const std::int32_t w = ok ? i32_at(head) : 0;
    const std::int32_t h = ok ? i32_at(head + 4) : 0;
    ok = ok && w > 0 && h > 0 && w <= kMaxCells && h <= kMaxCells &&
         size == 8 + static_cast<long long>(w) * h * 2 * static_cast<long long>(kRecordBytes);
    int off = 0;
    std::vector<std::uint8_t> run;
    for (std::size_t k = 0; ok && k < points.size(); ++k) {
        const MapPoint p = points[k];
        bool on = false;
        // A row within kOnFloorPx of y lies in these cells of column x.
        const int lo = p.y - kOnFloorPx;
        const int hi = p.y + kOnFloorPx;
        if (p.x >= 0 && p.x < w * kCellPx && hi >= 0 && lo < h * kCellPx) {
            const int cx = p.x / kCellPx;
            const std::size_t i = static_cast<std::size_t>(p.x % kCellPx);
            const int cy0 = std::max(0, lo) / kCellPx;
            const int cy1 = std::min(h * kCellPx - 1, hi) / kCellPx;
            run.resize(static_cast<std::size_t>(cy1 - cy0 + 1) * kRecordBytes);
            for (int layer = 0; ok && !on && layer < 2; ++layer) {
                const std::size_t at =
                    record_at(static_cast<std::size_t>(w), static_cast<std::size_t>(h), layer, cx, cy0);
                ok = _fseeki64(f, static_cast<long long>(at), SEEK_SET) == 0 &&
                     std::fread(run.data(), 1, run.size(), f) == run.size();
                io = ok;
                for (int cy = cy0; ok && !on && cy <= cy1; ++cy) {
                    const std::size_t rec = static_cast<std::size_t>(cy - cy0) * kRecordBytes;
                    const std::uint8_t v = run[rec + kHeightOffset + i];
                    if (v == kNoSurface) continue;
                    const int row = cy * kCellPx + std::min<int>(v, kCellPx - 1);
                    on = std::abs(row - p.y) <= kOnFloorPx;
                }
            }
        }
        if (!on) ++off;
    }
    std::fclose(f);
    return !io ? -2 : ok ? off : -1;
}

std::vector<MapGate> GameMapBook::gates(const std::string& stem) {
    std::vector<MapGate> out;
    if (const std::shared_ptr<const Page> page = get(stem)) out = page->gates;
    for (const MapGate& g : learned_gates(stem)) out.push_back(g);
    return out;
}

void GameMapBook::learn_gate(const std::string& stem, MapPoint p) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MapPoint>& v = learned_[stem].gates;
    if (std::any_of(v.begin(), v.end(), [&](const MapPoint& q) { return q.x == p.x && q.y == p.y; })) return;
    if (v.size() >= kLearnedMax) v.erase(v.begin());
    v.push_back(p);
}

std::vector<MapGate> GameMapBook::learned_gates(const std::string& stem) const {
    std::vector<MapGate> out;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = learned_.find(stem);
    if (it == learned_.end()) return out;
    for (const MapPoint& p : it->second.gates) {
        MapGate g;
        g.x = p.x;
        g.y = p.y;
        out.push_back(g);
    }
    return out;
}

void GameMapBook::learn_no_drop(const std::string& stem, MapPoint p) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MapPoint>& v = learned_[stem].no_drop;
    if (v.size() >= kLearnedMax) v.erase(v.begin());
    v.push_back(p);
}

bool GameMapBook::no_drop_near(const std::string& stem, MapPoint p) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = learned_.find(stem);
    if (it == learned_.end()) return false;
    return std::any_of(it->second.no_drop.begin(), it->second.no_drop.end(), [&](const MapPoint& q) {
        return std::abs(q.x - p.x) <= kNoDropXPx && std::abs(q.y - p.y) <= kNoDropYPx;
    });
}

bool GameMapBook::unlearn_no_drop(const std::string& stem, MapPoint p) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = learned_.find(stem);
    if (it == learned_.end()) return false;
    std::vector<MapPoint>& v = it->second.no_drop;
    const auto at = std::find_if(v.begin(), v.end(), [&](const MapPoint& q) { return q.x == p.x && q.y == p.y; });
    if (at == v.end()) return false;
    v.erase(at);
    return true;
}

void GameMapBook::distrust(const std::string& stem) {
    std::lock_guard<std::mutex> lock(mutex_);
    distrusted_.insert(stem);
}

bool GameMapBook::distrusted(const std::string& stem) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return distrusted_.count(stem) != 0;
}

void GameMapBook::learn_no_jump(const std::string& stem, const NoJump& n) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NoJump>& v = learned_[stem].no_jump;
    if (v.size() >= kLearnedMax) v.erase(v.begin());
    v.push_back(n);
}

std::vector<GameMapBook::NoJump> GameMapBook::no_jumps(const std::string& stem) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = learned_.find(stem);
    return it == learned_.end() ? std::vector<NoJump>{} : it->second.no_jump;
}

bool GameMapBook::unlearn_no_jump(const std::string& stem, MapPoint take) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = learned_.find(stem);
    if (it == learned_.end()) return false;
    std::vector<NoJump>& v = it->second.no_jump;
    const auto at = std::find_if(v.begin(), v.end(), [&](const NoJump& n) {
        return n.take.x == take.x && n.take.y == take.y;
    });
    if (at == v.end()) return false;
    v.erase(at);
    return true;
}

void GameMapBook::learn_solid(const std::string& stem, MapPoint on) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MapPoint>& v = learned_[stem].solid;
    if (std::any_of(v.begin(), v.end(), [&](const MapPoint& q) { return q.x == on.x && q.y == on.y; })) return;
    if (v.size() >= kLearnedMax) v.erase(v.begin());
    v.push_back(on);
}

std::vector<MapPoint> GameMapBook::solid_floors(const std::string& stem) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = learned_.find(stem);
    return it == learned_.end() ? std::vector<MapPoint>{} : it->second.solid;
}

bool GameMapBook::unlearn_solid(const std::string& stem, MapPoint on) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = learned_.find(stem);
    if (it == learned_.end()) return false;
    std::vector<MapPoint>& v = it->second.solid;
    const auto at = std::find_if(v.begin(), v.end(), [&](const MapPoint& q) { return q.x == on.x && q.y == on.y; });
    if (at == v.end()) return false;
    v.erase(at);
    return true;
}

// ------------------------------------------------------------------- trust

const char* MapTrust::why_text(Why w) {
    switch (w) {
        case Why::Trusted:    return "trusted";
        case Why::NoBook:     return "no map book";
        case Why::NoCapture:  return "the packets cannot say where you are";
        case Why::NoFiles:    return "the game's map files were not found";
        case Why::NoFile:     return "no map file for this map";
        case Why::NoGates:    return "its .prj lists no gate";
        case Why::Distrusted: return "a drop found its file wrong this session";
        case Why::Mismatch:   return "two standing points of yours off its file's floors";
        case Why::Confirming: return "map entry not by a gate: standing points being checked";
        case Why::Naming:     return "map entry not seen: naming it from standing points";
        case Why::Ambiguous:  return "map entry not seen: more than one map file fits";
    }
    return "?";
}

MapTrust::MapTrust(GameMapBook* book, const char* who) : book_(book), who_(who ? who : "Hunter") {}

void MapTrust::set_book(GameMapBook* book) {
    book_ = book;
    reset();
    epoch_set_ = false;
}

void MapTrust::reset() {
    stem_.clear();
    page_.reset();
    trusted_ = false;
    how_ = How::None;
    entry_learned_ = false;
    entry_spent_ = false;
    mismatch_ = false;
    miss_ = {};
    miss_gap_.clear();
    confirm_.clear();
    said_no_gate_ = false;
    said_not_by_gate_ = false;
    own_off_ = false;
    own_off_at_ = {};
    own_off_ms_ = 0;
    own_off_gap_.clear();
    mob_off_ = 0;
    mob_off_new_ = 0;
    mob_off_new_standing_ = 0;
    mob_off_last_.clear();
    clear_naming();
    // own_seen_ms_ and mob_seen_ stay: a report is never taken twice, and a
    // monster's generation tells another in its slot. So do the times things
    // were last said: never more often than they say.
}

void MapTrust::clear_naming() {
    name_pts_.clear();
    cands_.clear();
    cursor_ = 0;
    next_best_.clear();
    next_misses_ = 0;
    fit_ = 0;
    said_ambiguous_ = false;
}

void MapTrust::lose(const char* why) {
    if (trusted_)
        GB_INFO("%s: the game's map file %s is no longer gone by -- %s", who_.c_str(), stem_.c_str(),
                why ? why : "let go of");
    const bool mismatch = mismatch_;
    const MapPoint miss = miss_;
    std::string miss_gap = miss_gap_;
    std::string stem = stem_;
    reset();
    // Still this entry's stay: a file found wrong stays so, and its spawn by
    // a gate is not had again -- only standing points, or a new entry.
    if (entry_mode_) {
        stem_ = std::move(stem);
        mismatch_ = mismatch;
        miss_ = miss;
        miss_gap_ = std::move(miss_gap);
        entry_spent_ = true;
    }
}

std::vector<MapTrust::Stand> MapTrust::standing_points(const Look& look) {
    std::vector<Stand> out;
    if (look.settled && look.report_ms != 0 && look.report_ms != own_seen_ms_) {
        own_seen_ms_ = look.report_ms;
        // A map entry's spawn is where the character came in, perhaps in the
        // air above its floor (t1_s4, 2026-11-03: 20 px): never a standing point.
        const bool spawn = std::abs(look.rx - look.entry_x) <= kEntrySpawnPx &&
                           std::abs(look.ry - look.entry_y) <= kEntrySpawnPx;
        if (!spawn) {
            Stand s;
            s.at = { look.rx, look.ry };
            s.own = true;
            s.standing = true;
            s.at_ms = look.report_ms;
            out.push_back(s);
        }
    }
    // A monster's frame, each once: it stands once a later frame of the same
    // monster puts it at the same height kMobStandMs on. A spawn is 9-36 px
    // above its floor, a frame in the air anywhere: neither alone is a point.
    if (mob_seen_.size() > 1024) mob_seen_.clear();
    for (const Mob& m : look.mobs) {
        auto [it, fresh] = mob_seen_.try_emplace(m.slot);
        MobSeen& seen = it->second;
        if (!fresh && seen.gen == m.gen && seen.last_ms == m.at_ms) continue;
        Stand s;
        s.at = m.at;
        s.slot = m.slot;
        s.at_ms = m.at_ms;
        if (fresh || seen.gen != m.gen) {
            // A slot first seen, or another monster in it: its first frame.
            seen = MobSeen{ m.gen, m.at_ms, m.at.y, m.at_ms };
        } else {
            seen.last_ms = m.at_ms;
            if (std::abs(m.at.y - seen.y) > kMobSameYPx) {
                seen.y = m.at.y;
                seen.y_ms = m.at_ms;
            } else {
                s.standing = m.at_ms >= seen.y_ms + kMobStandMs;
            }
        }
        out.push_back(s);
    }
    return out;
}

void MapTrust::note_mobs(const std::vector<Stand>& points, const GameMapBook::Page& page, const std::string& stem,
                         std::uint64_t now) {
    for (const Stand& s : points) {
        if (s.own || page.on_floor(s.at)) continue;
        ++mob_off_;
        ++mob_off_new_;
        if (s.standing) ++mob_off_new_standing_;
        char text[256];
        std::snprintf(text, sizeof text, "slot %u at %d,%d, %s", static_cast<unsigned>(s.slot), s.at.x, s.at.y,
                      page.gap_text(s.at.x, s.at.y).c_str());
        mob_off_last_ = text;
    }
    if (mob_off_new_ == 0 || (mob_off_said_ && now < mob_off_said_ms_ + kMobOffSayMs)) return;
    GB_INFO("%s: %d monster frame(s) off %s's floors ignored (%d of them standing) -- the last, %s; a monster's "
            "point never lets a map file go",
            who_.c_str(), mob_off_new_, stem.c_str(), mob_off_new_standing_, mob_off_last_.c_str());
    mob_off_said_ = true;
    mob_off_said_ms_ = now;
    mob_off_new_ = 0;
    mob_off_new_standing_ = 0;
}

bool MapTrust::own_lets_go(const Stand& s, const GameMapBook::Page& page, const std::string& stem,
                           std::uint64_t now, std::string& why) {
    if (page.on_floor(s.at)) {
        if (own_off_)
            GB_DEBUG("%s: you on %s's floors again at %d,%d -- the point off them at %d,%d let pass", who_.c_str(),
                     stem.c_str(), s.at.x, s.at.y, own_off_at_.x, own_off_at_.y);
        own_off_ = false;
        return false;
    }
    const std::string gap = page.gap_text(s.at.x, s.at.y);
    if (own_off_ && s.at_ms >= own_off_ms_ + kOwnOffApartMs) {
        char text[512];
        std::snprintf(text, sizeof text,
                      "%s; before it, %d,%d %.1f s earlier: %s; none of yours on its floors between", gap.c_str(),
                      own_off_at_.x, own_off_at_.y, static_cast<double>(s.at_ms - own_off_ms_) / 1000.0,
                      own_off_gap_.c_str());
        why = text;
        return true;
    }
    if (own_off_) return false;     // within kOwnOffApartMs of the first: the first is kept
    own_off_ = true;
    own_off_at_ = s.at;
    own_off_ms_ = s.at_ms;
    own_off_gap_ = gap;
    if (!own_off_said_ || now >= own_off_said_ms_ + kOwnOffSayMs) {
        own_off_said_ = true;
        own_off_said_ms_ = now;
        GB_INFO("%s: you stand at %d,%d, on no floor of %s's file -- %s; one point is let pass: only a second of "
                "yours off its floors, %llu ms or more later with none on them between, lets the file go",
                who_.c_str(), s.at.x, s.at.y, stem.c_str(), gap.c_str(),
                static_cast<unsigned long long>(kOwnOffApartMs));
    } else {
        GB_DEBUG("%s: you stand at %d,%d, on no floor of %s's file -- %s; let pass", who_.c_str(), s.at.x, s.at.y,
                 stem.c_str(), gap.c_str());
    }
    return false;
}

void MapTrust::update(const Look& look, std::uint32_t work_ms) {
    if (book_ == nullptr) {
        why_ = Why::NoBook;
        return;
    }
    if (!look.capture) {
        if (trusted_)
            GB_INFO("%s: the game's map file %s is no longer gone by -- the packets lost you (the capture stopped "
                    "or started again)",
                    who_.c_str(), stem_.c_str());
        if (epoch_set_ && entry_mode_) {
            lost_set_ = true;
            lost_epoch_ = epoch_;
            // A file found wrong stays so for the rest of this entry's stay,
            // as at lose(): the capture back, it is not trusted again.
            lost_stem_ = stem_;
            lost_mismatch_ = mismatch_;
            lost_miss_ = miss_;
            lost_miss_gap_ = miss_gap_;
        }
        reset();
        epoch_set_ = false;
        why_ = Why::NoCapture;
        return;
    }
    // A map entry always wins: one since the last look starts everything
    // afresh, and so does one first seen with the map named before it.
    if (!epoch_set_ || look.epoch != epoch_ || look.map_known != entry_mode_) {
        if (trusted_ && epoch_set_)
            GB_INFO("%s: the game's map file %s is no longer gone by -- %s", who_.c_str(), stem_.c_str(),
                    look.epoch != epoch_ ? "a map entry" : "the map's entry seen");
        reset();
        epoch_set_ = true;
        epoch_ = look.epoch;
        entry_mode_ = look.map_known;
        // Back from a capture lost during the same entry's stay: its spawn
        // by a gate is not had again, as at lose().
        if (lost_set_ && lost_epoch_ != look.epoch) lost_set_ = false;
        entry_spent_ = look.map_known && lost_set_;
    }
    const std::vector<Stand> points = standing_points(look);
    if (look.map_known)
        by_entry(look, points);
    else
        by_name(look, points, work_ms);
}

void MapTrust::by_entry(const Look& look, const std::vector<Stand>& points) {
    const std::string stem = GameMapBook::stem_of(look.map, look.area);
    if (stem_ != stem) {
        const bool spent = entry_spent_;
        reset();
        entry_spent_ = spent;
        stem_ = stem;
        if (lost_set_ && lost_epoch_ == epoch_ && lost_mismatch_ && lost_stem_ == stem) {
            mismatch_ = true;
            miss_ = lost_miss_;
            miss_gap_ = lost_miss_gap_;
        }
    }
    if (!entry_learned_) {
        // Where an entry put the character is a gate's end, a door of this map.
        entry_learned_ = true;
        book_->learn_gate(stem, { look.entry_x, look.entry_y });
    }
    if (!page_) page_ = book_->get(stem);
    if (!page_) {
        trusted_ = false;
        why_ = Why::NoFile;
        return;
    }
    // The monsters' frames off its floors: counted and said, never more.
    note_mobs(points, *page_, stem, look.now_ms);
    if (mismatch_) {
        why_ = Why::Mismatch;
        return;
    }
    // Yours: a second off its floors, kOwnOffApartMs after the first with
    // none on them between, and it is not gone by until the next entry.
    for (const Stand& s : points) {
        if (!s.own) continue;
        std::string why;
        if (!own_lets_go(s, *page_, stem, look.now_ms, why)) continue;
        mismatch_ = true;
        miss_ = s.at;
        miss_gap_ = page_->gap_text(s.at.x, s.at.y);
        own_off_ = false;
        trusted_ = false;
        how_ = How::None;
        GB_WARN("%s: the game's map file %s does not match where you stand (%d,%d) -- no drops on it until the "
                "next map entry (%s)",
                who_.c_str(), stem.c_str(), s.at.x, s.at.y, why.c_str());
        why_ = Why::Mismatch;
        return;
    }
    if (book_->distrusted(stem)) {
        trusted_ = false;
        how_ = How::None;
        why_ = Why::Distrusted;
        return;
    }
    if (page_->gates.empty()) {
        if (!said_no_gate_) {
            said_no_gate_ = true;
            GB_INFO("%s: %s -- its .prj %s, so its file is not gone by", who_.c_str(), stem.c_str(),
                    page_->prj_read ? "lists no gate" : "was not found");
        }
        why_ = Why::NoGates;
        return;
    }
    if (trusted_) {
        why_ = Why::Trusted;
        return;
    }
    // (a) The spawn by a gate the file lists.
    if (!entry_spent_) {
        int best = -1;
        for (const MapGate& g : page_->gates) {
            const int dx = std::abs(g.x - look.entry_x);
            const int dy = std::abs(g.y - look.entry_y);
            if (dx <= kGateMatchXPx && dy <= kGateMatchYPx && (best < 0 || dx + dy < best)) best = dx + dy;
        }
        if (best >= 0) {
            trusted_ = true;
            how_ = How::Entry;
            why_ = Why::Trusted;
            GB_INFO("%s: %s from the game's files: %zu gate(s) (%s), floors %dx%d -- trusted: map entry %d,%d is "
                    "by a gate",
                    who_.c_str(), stem.c_str(), page_->gates.size(), gates_text(page_->gates).c_str(),
                    page_->floors.w_px(), page_->floors.h_px(), look.entry_x, look.entry_y);
            return;
        }
        if (!said_not_by_gate_) {
            said_not_by_gate_ = true;
            GB_INFO("%s: %s -- map entry %d,%d is by none of its %zu gate(s) (%s): its file is gone by after %d "
                    "standing points on its floors",
                    who_.c_str(), stem.c_str(), look.entry_x, look.entry_y, page_->gates.size(),
                    gates_text(page_->gates).c_str(), kTrustPoints);
        }
    }
    // (b) Standing points on its floors, spread out: yours, and the monsters'
    // that stand -- a monster's frame alone is none. A monster that STANDS off
    // them is a miss, as it is to a name: the points so far count no more. Only
    // here, before trust: trust had is never touched by a monster. Not while a
    // point of yours off them is let pass: one of yours on them first.
    for (const Stand& s : points) {
        if (s.own || !s.standing || page_->on_floor(s.at)) continue;
        if (!confirm_.empty() && (!confirm_cut_said_ || look.now_ms >= confirm_cut_said_ms_ + kMobOffSayMs)) {
            confirm_cut_said_ = true;
            confirm_cut_said_ms_ = look.now_ms;
            GB_INFO("%s: %s -- a monster stands off its floors (slot %u at %d,%d, %s): the %zu standing "
                    "point(s) on them so far count no more",
                    who_.c_str(), stem.c_str(), static_cast<unsigned>(s.slot), s.at.x, s.at.y,
                    page_->gap_text(s.at.x, s.at.y).c_str(), confirm_.size());
        }
        confirm_.clear();
    }
    for (const Stand& s : points) {
        if (!s.standing || !page_->on_floor(s.at)) continue;
        const bool apart = std::all_of(confirm_.begin(), confirm_.end(), [&](const MapPoint& q) {
            return std::abs(q.x - s.at.x) >= kTrustSpreadXPx || std::abs(q.y - s.at.y) >= kTrustSpreadYPx;
        });
        if (apart) confirm_.push_back(s.at);
    }
    if (static_cast<int>(confirm_.size()) >= kTrustPoints && !own_off_) {
        trusted_ = true;
        how_ = How::Points;
        why_ = Why::Trusted;
        GB_INFO("%s: %s trusted by %d standing points on its floors (its gates: %s)", who_.c_str(), stem.c_str(),
                kTrustPoints, gates_text(page_->gates).c_str());
        return;
    }
    why_ = Why::Confirming;
}

void MapTrust::by_name(const Look& look, const std::vector<Stand>& points, std::uint32_t work_ms) {
    // Named: a monster's frame off its floors is only counted and said; two
    // of yours off them, kOwnOffApartMs apart with none on them between, let
    // it go -- and naming starts again from those two.
    if (!stem_.empty() && page_) {
        note_mobs(points, *page_, stem_, look.now_ms);
        for (const Stand& s : points) {
            if (!s.own) continue;
            const MapPoint first = own_off_at_;
            std::string why;
            if (!own_lets_go(s, *page_, stem_, look.now_ms, why)) continue;
            GB_WARN("%s: %s, named from standing points, does not match where you stand (%d,%d) -- no longer "
                    "taken for the map; naming it again (%s)",
                    who_.c_str(), stem_.c_str(), s.at.x, s.at.y, why.c_str());
            stem_.clear();
            page_.reset();
            trusted_ = false;
            how_ = How::None;
            own_off_ = false;
            restart_naming(nullptr);
            add_name_point(first);  // the second comes with this update's points below
            break;
        }
        if (!stem_.empty()) {
            judge_named();
            return;
        }
    }
    if (book_->map_files() == 0) {
        why_ = Why::NoFiles;
        return;
    }
    // Yours, every settled report; a monster's only where it stands.
    for (const Stand& s : points)
        if (s.standing) add_name_point(s.at);
    if (name_pts_.size() > kNameMaxPoints) restart_naming("no file was told from the rest by that many points");
    why_ = Why::Naming;
    if (static_cast<int>(name_pts_.size()) < kNamePoints) return;
    if (cands_.empty()) {
        // A full read of every file, at most every kNameRescanMs.
        if (scanned_ && look.now_ms - scan_ms_ < kNameRescanMs) return;
        scanned_ = true;
        scan_ms_ = look.now_ms;
        for (std::string& s : book_->stems()) cands_.push_back({ std::move(s) });
        cursor_ = 0;
    }
    scan(work_ms);
    decide();
}

void MapTrust::add_name_point(MapPoint p) {
    const bool apart = std::all_of(name_pts_.begin(), name_pts_.end(), [&](const MapPoint& q) {
        return std::abs(q.x - p.x) >= kNameApartXPx || std::abs(q.y - p.y) >= kNameApartYPx;
    });
    if (apart) name_pts_.push_back(p);
}

void MapTrust::restart_naming(const char* why) {
    if (why != nullptr) {
        ++restarts_;
        if (restarts_ == 1)
            GB_INFO("%s: map not named from %zu standing points -- %s; naming starts again", who_.c_str(),
                    name_pts_.size(), why);
        else
            GB_DEBUG("%s: map not named from %zu standing points -- %s; naming starts again (%d times)",
                     who_.c_str(), name_pts_.size(), why, restarts_);
    }
    clear_naming();
}

void MapTrust::scan(std::uint32_t work_ms) {
    if (cands_.empty()) return;
    const std::size_t n = name_pts_.size();
    const std::uint64_t t0 = now_ms();
    for (std::size_t visited = 0; visited < cands_.size(); ++visited) {
        Cand& c = cands_[cursor_];
        cursor_ = (cursor_ + 1) % cands_.size();
        if (c.out || c.checked >= n) continue;
        const std::span<const MapPoint> fresh(name_pts_.data() + c.checked, n - c.checked);
        const int m = book_->misses(c.stem, fresh);
        if (m == -2) {
            // Not opened, or a read failed: whether it fits is not known, so
            // it stays to be read -- every point of it at the next update --
            // and nothing is named meanwhile (decide). Left out, a copy of the
            // map beside it would be the one file that fits.
            if (!c.io_fail)
                GB_INFO("%s: map naming -- %s.map could not be read; nothing is named until it is", who_.c_str(),
                        c.stem.c_str());
            c.io_fail = true;
            if (now_ms() - t0 >= work_ms) break;
            continue;
        }
        c.io_fail = false;
        if (m < 0) {
            c.out = true;
            c.unread = true;
        } else {
            c.misses += m;
            if (c.misses >= kNameMarginMisses) c.out = true;
        }
        c.checked = n;
        if (now_ms() - t0 >= work_ms) break;
    }
}

void MapTrust::decide() {
    const std::size_t n = name_pts_.size();
    std::size_t pending = 0, fit = 0, live = 0;
    const Cand* only = nullptr;
    const Cand* next = nullptr;
    for (const Cand& c : cands_) {
        if (!c.out && c.checked < n) ++pending;
        if (!c.out) ++live;
        if (!c.out && c.misses == 0) {
            ++fit;
            only = &c;
        }
    }
    if (pending > 0) return;
    for (const Cand& c : cands_)
        if (&c != only && !c.unread && c.checked > 0 && (next == nullptr || c.misses < next->misses)) next = &c;
    fit_ = static_cast<int>(fit);
    next_best_ = next ? next->stem : std::string();
    next_misses_ = next ? next->misses : 0;
    if (fit == 0) {
        restart_naming("no map file has them all on its floors");
        return;
    }
    if (fit == 1 && live == 1) {
        stem_ = only->stem;
        page_ = book_->get(stem_);
        if (!page_) {
            stem_.clear();
            restart_naming("the one file they fit does not read whole");
            return;
        }
        how_ = How::Named;
        own_off_ = false;
        mob_off_ = 0;
        mob_off_new_ = 0;
        mob_off_new_standing_ = 0;
        mob_off_last_.clear();
        GB_INFO("%s: map named from %zu standing points: %s -- the only one of %zu map files they all lie on; the "
                "next best, %s, misses %d of them",
                who_.c_str(), n, stem_.c_str(), cands_.size(), next_best_.empty() ? "none" : next_best_.c_str(),
                next_misses_);
        cands_.clear();
        cursor_ = 0;
        judge_named();
        return;
    }
    why_ = Why::Ambiguous;
    if (!said_ambiguous_) {
        said_ambiguous_ = true;
        std::string fits;
        int listed = 0;
        for (const Cand& c : cands_) {
            if (c.out || c.misses != 0) continue;
            if (++listed > 3) {
                fits += ", ...";
                break;
            }
            fits += (listed > 1 ? ", " : "") + c.stem;
        }
        GB_INFO("%s: %zu standing points all lie on %zu map file(s) (%s), and %s misses only %d of them -- not "
                "named until one file alone fits",
                who_.c_str(), n, fit, fits.c_str(), next_best_.empty() ? "none" : next_best_.c_str(), next_misses_);
    }
}

void MapTrust::judge_named() {
    trusted_ = false;
    if (book_->distrusted(stem_)) {
        why_ = Why::Distrusted;
        return;
    }
    if (page_->gates.empty()) {
        if (!said_no_gate_) {
            said_no_gate_ = true;
            GB_INFO("%s: %s -- its .prj %s, so its file is not gone by", who_.c_str(), stem_.c_str(),
                    page_->prj_read ? "lists no gate" : "was not found");
        }
        why_ = Why::NoGates;
        return;
    }
    trusted_ = true;
    why_ = Why::Trusted;
}

MapTrust::Report MapTrust::report() const {
    Report r;
    r.why = why_;
    r.how = how();
    r.stem = stem_;
    r.epoch = epoch_;
    r.miss = miss_;
    r.miss_gap = miss_gap_;
    r.own_off = own_off_;
    r.mob_off = mob_off_;
    // how() hides a name while it is not trusted: a map with no gate (NoGates)
    // is named all the same, and what it is called is still known.
    r.named = !entry_mode_ && how_ == How::Named && !stem_.empty();
    r.fit = fit_;
    r.next_best = next_best_;
    r.next_misses = next_misses_;
    if (why_ == Why::Confirming) {
        // Points past the needed (one of yours off the floors let pass holds
        // trust back meanwhile) show as the needed: never 4/3.
        r.points = std::min(static_cast<int>(confirm_.size()), kTrustPoints);
        r.needed = kTrustPoints;
    } else if (why_ == Why::Naming || why_ == Why::Ambiguous) {
        r.points = static_cast<int>(name_pts_.size());
        r.needed = kNamePoints;
        r.files = cands_.size();
        for (const Cand& c : cands_)
            if (c.out || c.checked >= name_pts_.size()) ++r.read;
    }
    return r;
}

} // namespace gobot
