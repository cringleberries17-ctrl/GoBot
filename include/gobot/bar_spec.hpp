#pragma once

#include <cstdint>

namespace gobot {

/// A bar on screen whose fill represents a value.
///
/// Lives in its own header because both the vitals reader and the macro
/// configuration need it, and the macro has no business pulling in a capture
/// thread just to remember where the health bar is.
struct BarSpec {
    int x = 0, y = 0;           ///< top-left, in the window's client area
    int width = 0, height = 0;

    /// Colour of the filled portion, matched within `tolerance` per channel.
    std::uint8_t r = 200, g = 40, b = 40;
    int tolerance = 60;

    bool valid() const { return width > 2 && height > 0; }
    int  right() const { return x + width; }
    int  bottom() const { return y + height; }
};

/// The game's own fill colours, across the middle rows of its bars at every
/// HUD size seen: HP's red and MP's blue, matched within kFillTolerance --
/// the colours a bar starts from, and goes back to when it holds one that
/// is no fill colour at all (VitalsReader::reads_as_fill).
struct FillColour {
    std::uint8_t r, g, b;
};
inline constexpr FillColour kHpFill{ 239, 0, 0 };
inline constexpr FillColour kMpFill{ 0, 49, 206 };
inline constexpr int        kFillTolerance = 80;

} // namespace gobot
