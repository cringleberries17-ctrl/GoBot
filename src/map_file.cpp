#include "gobot/map_file.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <windows.h>

namespace gobot {
namespace {

constexpr std::size_t kCellBytes = 72;
constexpr std::size_t kRecordBytes = 36;
constexpr std::size_t kHeightOffset = 3;     // within a record
constexpr std::size_t kHeightCount = 32;

std::string stem_of(const std::string& path) {
    auto slash = path.find_last_of("\\/");
    std::string f = (slash == std::string::npos) ? path : path.substr(slash + 1);
    auto dot = f.find_last_of('.');
    return (dot == std::string::npos) ? f : f.substr(0, dot);
}

} // namespace

bool MapFile::load(const std::string& path) {
    loaded_ = false;
    surfaces_built_ = false;
    for (auto& s : surfaces_) s.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        GB_WARN("MapFile: cannot open %s", path.c_str());
        return false;
    }

    std::int32_t w = 0, h = 0;
    in.read(reinterpret_cast<char*>(&w), 4);
    in.read(reinterpret_cast<char*>(&h), 4);
    if (!in || w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        GB_WARN("MapFile: %s has implausible dimensions %dx%d", path.c_str(), w, h);
        return false;
    }

    const std::size_t expected = static_cast<std::size_t>(w) * h * kCellBytes;
    cells_.resize(expected);
    in.read(reinterpret_cast<char*>(cells_.data()), static_cast<std::streamsize>(expected));

    // The size identity is what confirms the format rather than merely
    // assuming it: every one of the game's 781 maps satisfies it exactly.
    if (static_cast<std::size_t>(in.gcount()) != expected) {
        GB_WARN("MapFile: %s is %zu bytes short of %d x %d x 72",
                path.c_str(), expected - static_cast<std::size_t>(in.gcount()), w, h);
        cells_.clear();
        return false;
    }

    width_ = w;
    height_ = h;
    path_ = path;
    name_ = stem_of(path);
    loaded_ = true;

    GB_INFO("MapFile: %s  %d x %d cells (%d x %d px)",
            name_.c_str(), width_, height_, width_px(), height_px());
    return true;
}

const std::uint8_t* MapFile::cell(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return nullptr;
    // Column-major: walking the file visits a whole column before moving right.
    const std::size_t index = static_cast<std::size_t>(x) * height_ + y;
    return cells_.data() + index * kCellBytes;
}

void MapFile::build_surfaces() const {
    if (surfaces_built_) return;
    surfaces_built_ = true;
    for (auto& s : surfaces_) s.clear();

    for (int x = 0; x < width_; ++x) {
        for (int y = 0; y < height_; ++y) {
            const std::uint8_t* c = cell(x, y);
            if (!c) continue;

            for (int layer = 0; layer < kLayers; ++layer) {
                const std::uint8_t* rec = c + layer * kRecordBytes;
                for (std::size_t i = 0; i < kHeightCount; ++i) {
                    const std::uint8_t v = rec[kHeightOffset + i];
                    if (v == kNoSurface) continue;
                    // The byte is an offset inside this cell's 32 px, so the
                    // world position is the cell origin plus that offset.
                    const int px = x * kCellPixels + static_cast<int>(i);
                    const int py = y * kCellPixels + std::min<int>(v, kCellPixels - 1);
                    surfaces_[layer].emplace_back(px, py);
                }
            }
        }
    }
}

int MapFile::surface_at(int layer, int px, int py_cell) const {
    if (!loaded_ || layer < 0 || layer >= kLayers) return -1;
    const int cx = px / kCellPixels;
    const int i  = px % kCellPixels;
    const std::uint8_t* c = cell(cx, py_cell);
    if (!c) return -1;
    const std::uint8_t v = c[layer * kRecordBytes + kHeightOffset + i];
    if (v == kNoSurface) return -1;
    return py_cell * kCellPixels + std::min<int>(v, kCellPixels - 1);
}

const std::vector<std::pair<int, int>>& MapFile::surfaces(int layer) const {
    build_surfaces();
    static const std::vector<std::pair<int, int>> empty;
    if (layer < 0 || layer >= kLayers) return empty;
    return surfaces_[layer];
}

std::size_t MapFile::surface_count() const {
    build_surfaces();
    std::size_t n = 0;
    for (const auto& s : surfaces_) n += s.size();
    return n;
}

MapData MapFile::to_map_data(int tile_size, int layer_mask) const {
    MapData map;
    if (!loaded_) return map;
    if (tile_size < 1) tile_size = 1;

    build_surfaces();

    map.name   = name_;
    map.id     = 0;
    map.tile_w = tile_size;
    map.tile_h = tile_size;
    map.width  = std::max(1, (width_px()  + tile_size - 1) / tile_size);
    map.height = std::max(1, (height_px() + tile_size - 1) / tile_size);
    map.attrs.assign(static_cast<std::size_t>(map.width) * map.height, Tile_Empty);

    for (int layer = 0; layer < kLayers; ++layer) {
        if (!(layer_mask & (1 << layer))) continue;
        for (const auto& [px, py] : surfaces_[layer]) {
            const int tx = px / tile_size;
            const int ty = py / tile_size;
            if (tx < 0 || ty < 0 || tx >= map.width || ty >= map.height) continue;
            map.attrs[static_cast<std::size_t>(ty) * map.width + tx] = Tile_Ground;
        }
    }

    GB_INFO("MapFile: %s -> %d x %d grid at %d px/tile",
            name_.c_str(), map.width, map.height, tile_size);
    return map;
}

// ------------------------------------------------------------------ lookup

std::string MapFile::find(const std::string& dir, const std::string& stem) {
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::path(dir) / (stem + ".map");
    if (std::filesystem::exists(p, ec)) return p.string();
    return {};
}

std::vector<std::string> MapFile::list(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        const auto p = e.path();
        if (p.extension() != ".map") continue;
        out.push_back(p.stem().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string MapFile::default_map_dir() {
    // The installer records the game's location; reading it beats asking the
    // user to find a path they have no reason to know.
    static const char* kKeys[] = {
        "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Ghost Online VTC",
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Ghost Online VTC",
    };

    for (const char* key : kKeys) {
        HKEY h{};
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;

        char buf[MAX_PATH]{};
        DWORD size = sizeof(buf), type = 0;
        // DisplayIcon points at Game.exe, whose folder is the install root.
        LONG r = RegQueryValueExA(h, "DisplayIcon", nullptr, &type,
                                  reinterpret_cast<LPBYTE>(buf), &size);
        RegCloseKey(h);
        if (r != ERROR_SUCCESS || type != REG_SZ) continue;

        std::string s(buf);
        const auto slash = s.find_last_of("\\/");
        if (slash == std::string::npos) continue;
        s = s.substr(0, slash) + "\\data\\Map";

        std::error_code ec;
        if (std::filesystem::is_directory(s, ec)) return s;
    }

    // Fall back to where this build of the game is normally installed.
    const char* kGuesses[] = {
        "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\data\\Map",
    };
    for (const char* g : kGuesses) {
        std::error_code ec;
        if (std::filesystem::is_directory(g, ec)) return g;
    }
    return {};
}

} // namespace gobot
