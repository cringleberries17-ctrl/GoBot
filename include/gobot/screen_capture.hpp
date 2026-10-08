#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gobot {

/// A captured frame: 32-bit BGRA, top-down, tightly packed.
struct Frame {
    int                       width = 0;
    int                       height = 0;
    std::vector<std::uint8_t> pixels;      ///< width*height*4, BGRA

    bool valid() const {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<std::size_t>(width) * height * 4;
    }

    /// Pixel at (x, y). Out-of-range reads return black rather than faulting:
    /// callers scan regions whose bounds come from configuration, and a stale
    /// region after a window resize should not crash the capture thread.
    void get(int x, int y, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) const {
        if (x < 0 || y < 0 || x >= width || y >= height) { r = g = b = 0; return; }
        const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
        b = pixels[i]; g = pixels[i + 1]; r = pixels[i + 2];
    }
};

/// Write a frame out as a PNG.
///
/// Worth having for its own sake: everything in this project reasons about
/// pixels it was handed, and when a reader disagrees with what is plainly on
/// screen the fastest way to settle it is to look at the frame the reader was
/// actually given, not the one you assume it got.
bool save_png(const Frame& f, const std::string& path);

/// Read a PNG -- a screenshot, say -- into a frame. Defined beside the
/// template loader, which reads them the same way.
bool load_png(const std::string& path, Frame& out);

/// Grabs the client area of a window.
///
/// Uses PrintWindow with PW_RENDERFULLCONTENT, which asks the window to render
/// itself into our bitmap rather than copying whatever is on screen at those
/// coordinates. That keeps working when the game is behind another window —
/// which it routinely is, since the mapper's own UI sits on top of it.
///
/// Falls back to a screen BitBlt for windows that refuse PrintWindow; those
/// frames are only valid while the window is unobscured, which `from_screen`
/// reports so callers can say so rather than silently mapping the wrong thing.
///
/// "Refuse" includes returning success having painted nothing but the
/// background brush, which is what a Direct3D window typically does — so the
/// result is checked, not trusted.
class ScreenCapture {
public:
    /// Capture `hwnd`'s client area into `out`. False if the window is gone,
    /// minimised, or zero-sized.
    bool grab(void* hwnd, Frame& out);

    /// True when the last grab came from the screen rather than from the
    /// window itself, and so may contain whatever was covering it.
    bool from_screen() const { return from_screen_; }

    /// Frames captured and how many fell back.
    std::uint64_t grabs() const { return grabs_; }
    std::uint64_t fallbacks() const { return fallbacks_; }

    ~ScreenCapture();

private:
    void release();

    // Reused across grabs: creating a DC and DIB per frame is the single
    // largest cost in a capture loop.
    void*         dc_ = nullptr;          ///< HDC (memory)
    void*         bitmap_ = nullptr;      ///< HBITMAP
    void*         bits_ = nullptr;        ///< raw DIB pixels, owned by the bitmap
    int           cached_w_ = 0;
    int           cached_h_ = 0;

    /// Whether PrintWindow is worth trying for this window. Cleared the first
    /// time it comes back blank, and retried every so often in case the window
    /// was merely still loading.
    bool          try_print_window_ = true;
    int           print_probe_countdown_ = 0;

    bool          from_screen_ = false;
    std::uint64_t grabs_ = 0;
    std::uint64_t fallbacks_ = 0;
};

} // namespace gobot
