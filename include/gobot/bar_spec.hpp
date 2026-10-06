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

} // namespace gobot
