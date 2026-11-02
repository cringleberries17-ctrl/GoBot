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
///
/// When PrintWindow is worth asking, grab by grab: given up once it has come
/// back blank kMissesBeforeScreen times in a row while the screen showed the
/// window itself drawn -- not at a single blank, nor while the window is one
/// flat colour where it can be seen, as the game's black loading screen is --
/// and asked again every kProbeEvery grabs after that. Given up at the first
/// blank, it was given up for 600 grabs at a map change on 2026-10-26, and
/// everything read meanwhile came off the screen with whatever lay over the
/// game: 29 frames that were not the HUD in 76 s of hunting. On its own so
/// that the rule can be tried without a window that refuses to draw.
class PrintWindowRule {
public:
    static constexpr int kMissesBeforeScreen = 3;
    static constexpr int kProbeEvery = 60;

    /// Whether to ask PrintWindow this grab.
    bool ask() { return trying_ || --countdown_ <= 0; }
    /// What it gave when asked: a picture, or nothing. True when it is back
    /// after being given up.
    bool printed(bool drew);
    /// After a picture of nothing: whether the screen showed the window itself
    /// drawn. True when PrintWindow is given up now.
    bool screen(bool drawn);
    bool trying() const { return trying_; }

private:
    bool trying_ = true;
    int  misses_ = 0;
    int  countdown_ = 0;
};

/// While one is alive, no ScreenCapture in the process asks PrintWindow --
/// the vitals reader's, the mob detector's and every other's read the screen
/// instead: for the shop's purchase, which has the game in front throughout.
/// See ScreenCapture::set_screen_only for why.
class PrintWindowPause {
public:
    PrintWindowPause();
    ~PrintWindowPause();
    PrintWindowPause(const PrintWindowPause&) = delete;
    PrintWindowPause& operator=(const PrintWindowPause&) = delete;
    static bool active();
};

class ScreenCapture {
public:
    /// Capture `hwnd`'s client area into `out`. False if the window is gone,
    /// minimised, or zero-sized.
    bool grab(void* hwnd, Frame& out);

    /// True when the last grab came from the screen rather than from the
    /// window itself, and so may contain whatever was covering it.
    bool from_screen() const { return from_screen_; }

    /// Never ask PrintWindow, only copy the screen: for a run that has the
    /// game in front and its clicks checked against what is on top anyway. On
    /// the live client (2026-10-30/31) PrintWindow was being tried grab after
    /// grab just as the shop's quantity box came up, and the digits typed then
    /// never reached the box -- whether it was the cause is not known.
    void set_screen_only(bool on) { screen_only_ = on; }

    /// Frames captured and how many fell back.
    std::uint64_t grabs() const { return grabs_; }
    std::uint64_t fallbacks() const { return fallbacks_; }

    ScreenCapture() = default;
    ~ScreenCapture();

    /// Not copyable: it owns a memory DC and a DIB, and two copies would
    /// delete them twice. Nothing moves one either -- every capture lives
    /// where it is made -- so no move is declared, and none is generated.
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

private:
    void release();

    /// After PrintWindow drew nothing, the screen copied again this many times,
    /// this far apart, while it shows the window one flat colour: see grab.
    static constexpr int kRecopies = 3;
    static constexpr int kRecopyGapMs = 40;

    // Reused across grabs: creating a DC and DIB per frame is the single
    // largest cost in a capture loop.
    void*         dc_ = nullptr;          ///< HDC (memory)
    void*         bitmap_ = nullptr;      ///< HBITMAP
    void*         old_bitmap_ = nullptr;  ///< HGDIOBJ the memory DC came with: put back before the DIB is deleted
    void*         bits_ = nullptr;        ///< raw DIB pixels, owned by the bitmap
    int           cached_w_ = 0;
    int           cached_h_ = 0;

    /// Whether PrintWindow is worth trying for this window: see PrintWindowRule.
    PrintWindowRule print_rule_;

    bool          from_screen_ = false;
    bool          screen_only_ = false;
    std::uint64_t grabs_ = 0;
    std::uint64_t fallbacks_ = 0;
};

} // namespace gobot
