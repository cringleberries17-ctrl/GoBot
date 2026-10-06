#pragma once

#include "gobot/pathfinder.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace gobot {

/// A GhostOnline `.map` file: the game's own collision data.
///
/// The game ships these in data\Map — 781 of them, one per map section. They
/// hold exact surface geometry, which is why the original ZTeam client asked
/// its server for map data rather than trying to recognise platforms on
/// screen: the server had parsed these same files. Reading them directly needs
/// no screen capture, no packet sniffing, and no walking the map to learn it.
///
/// Layout, established by inspection:
///
///     [int32 width_cells][int32 height_cells]
///     cells stored COLUMN-MAJOR: index = x * height + y
///     cell = 72 bytes = two 36-byte records (two collision layers)
///       record = [u8 tile_id][u8][u8][32 x u8 surface height][u8 attr]
///
/// The 32 height bytes are one per pixel-column across the cell's 32 px width;
/// 0xff means no surface in that column. So a cell describes the terrain
/// profile through it at full horizontal resolution, which is what lets the
/// game have smooth sloped ground rather than blocky steps.
class MapFile {
public:
    static constexpr int kCellPixels = 32;
    static constexpr int kLayers = 2;
    static constexpr std::uint8_t kNoSurface = 0xFF;

    /// Parse a .map file. False if it is missing or not this format.
    bool load(const std::string& path);
    bool loaded() const { return loaded_; }

    int width_cells() const { return width_; }
    int height_cells() const { return height_; }
    int width_px() const { return width_ * kCellPixels; }
    int height_px() const { return height_ * kCellPixels; }

    const std::string& path() const { return path_; }
    const std::string& name() const { return name_; }

    /// Surface height at a pixel column, for one layer, or -1 if none.
    /// Returns the absolute pixel y, not the within-cell offset.
    int surface_at(int layer, int px, int py_cell) const;

    /// Every surface pixel: (x, y) in world pixels, per layer.
    const std::vector<std::pair<int, int>>& surfaces(int layer) const;

    /// Total surface pixels across both layers — a quick sanity figure.
    std::size_t surface_count() const;

    /// Rasterise into the tile grid PathFinder consumes.
    ///
    /// `tile_size` trades resolution against graph size: 8 keeps slopes
    /// recognisable while giving PathFinder a grid it can walk quickly.
    MapData to_map_data(int tile_size = 8, int layer_mask = 0x3) const;

    /// Find a map by its file stem (e.g. "t14_s0") under `dir`.
    static std::string find(const std::string& dir, const std::string& stem);

    /// List the map stems available in `dir`.
    static std::vector<std::string> list(const std::string& dir);

    /// Where the game keeps them, if it can be found from the registry.
    static std::string default_map_dir();

private:
    bool        loaded_ = false;
    int         width_ = 0;
    int         height_ = 0;
    std::string path_;
    std::string name_;

    /// Raw cells, column-major as stored: cells_[x * height_ + y].
    std::vector<std::uint8_t> cells_;

    mutable std::vector<std::pair<int, int>> surfaces_[kLayers];
    mutable bool surfaces_built_ = false;

    void build_surfaces() const;
    const std::uint8_t* cell(int x, int y) const;
};

} // namespace gobot
