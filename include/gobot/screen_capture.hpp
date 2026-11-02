#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace gobot {

struct GrabOwner;

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

/// One verdict per window on PrintWindow, for the whole process.
///
/// Every capture used to judge PrintWindow for itself: the vitals reader's,
/// the detector's, and every short-lived one a job or a look makes -- the
/// shop's, the bag's, the keeper's, "Thử xem", the login tab's every few
/// seconds -- asked it on its first three grabs and more of a window that had
/// refused it all session, and once in 60 grabs ever after. And while the
/// game was covered by another window, or black on a loading screen, there
/// was nothing to judge it by: every grab of every capture asked it, each
/// with up to 120 ms of copies after, at 6 to 22 grabs a second. Here, window
/// by window:
///
/// - Refused: some capture's rule gave PrintWindow up on it (refuse). Every
///   capture of it, new ones too, copies the screen, and it is asked again --
///   a probe -- once every kReprobeMs for the whole process; never in light
///   mode ("Nhẹ máy"), where the verdict this session reached stands. Light
///   mode refuses nothing by itself: a window this session has not seen
///   refuse is tried as ever. A probe that draws has it tried again
///   everywhere (drew).
/// - Not refused, and the last ask found nothing to judge PrintWindow by -- a
///   blank picture, and the window covered where it would be seen, or one
///   flat colour (unclear): asked once every kUnclearAskMs for the whole
///   process until an ask settles it; otherwise on every grab, as before.
///   Not so a window PrintWindow has drawn within the last kDrewTrustMs: a
///   blank picture of that is the game's own black frame -- a loading screen
///   after a map change -- and every grab asks, as before, so that the first
///   grab after the game draws again has its picture. Gated, it would read
///   the window covering the game for up to 3 s after every loading screen:
///   HP unread, the potions held, the hunter looking at GoBot's own window.
///
/// A window is its handle and the process that owns it: a handle Windows
/// gives again to another program's window is another window. Pure -- the
/// time is the caller's, so that the tests give their own -- and
/// thread-safe.
class PrintWindowVerdicts {
public:
    static constexpr std::uint64_t kReprobeMs = 300'000;
    static constexpr std::uint64_t kUnclearAskMs = 3000;
    /// PrintWindow drew the window this recently: a blank picture of it is
    /// not held to kUnclearAskMs. Longer than any loading screen; a window
    /// that drew once and has been blank and covered since is gated again
    /// after it, which is the costly case the gate is for.
    static constexpr std::uint64_t kDrewTrustMs = 30'000;
    /// Windows remembered; past this the one longest unasked is forgotten.
    static constexpr std::size_t kMaxWindows = 64;

    struct Window {
        const void*   handle = nullptr;
        std::uint32_t process = 0;
        bool operator==(const Window& o) const { return handle == o.handle && process == o.process; }
        bool operator!=(const Window& o) const { return !(*this == o); }
    };

    /// One grab's turn: whether it asks PrintWindow, and whether the window
    /// stands refused.
    struct Turn {
        bool ask = false;
        bool refused = false;
    };
    /// A grab of `w` at `now`. An ask allowed by the time -- a probe of a
    /// window refused, or a window with nothing to judge by asked again -- is
    /// this grab's: the next capture to come waits its full time.
    Turn turn(const Window& w, std::uint64_t now);

    /// PrintWindow drew `w` at `now`: it is tried again everywhere, and a
    /// blank picture of it for kDrewTrustMs after is asked on every grab.
    /// True when it had been refused.
    bool drew(const Window& w, std::uint64_t now);
    /// A blank picture of `w`, with the screen showing it drawn: counted
    /// against PrintWindow, so the next grab asks again at once.
    void judged(const Window& w);
    /// A blank picture of `w` with nothing to judge it by: the window covered
    /// where it would be seen, or one flat colour.
    void unclear(const Window& w, std::uint64_t now);
    /// A capture gave PrintWindow up on `w`. True when it had not been
    /// refused already: said once for the process.
    bool refuse(const Window& w, std::uint64_t now);

    bool        refused(const Window& w) const;
    std::size_t windows() const;

    /// Light mode: a window refused is never asked again this session.
    void set_light(bool on) { light_.store(on); }
    bool light() const { return light_.load(); }

private:
    struct Entry {
        Window        window;
        bool          refused = false;
        bool          unclear = false;
        std::uint64_t probe_ms = 0;     ///< refused: when it was last asked, or refused
        std::uint64_t unclear_ms = 0;   ///< unclear: when it was last asked
        std::uint64_t drew_ms = 0;      ///< when PrintWindow last drew it; 0 never
        std::uint64_t used_ms = 0;      ///< the last grab's turn
    };
    /// Under mutex_: the entry for `w`, or null.
    Entry*       find(const Window& w);
    const Entry* find(const Window& w) const;
    /// Under mutex_: found, or made -- in place of the one longest unasked
    /// when kMaxWindows are kept.
    Entry&       entry(const Window& w, std::uint64_t now);

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::atomic<bool>  light_{ false };
};

/// The process's own, which every ScreenCapture asks. GoBot's window tells it
/// whether light mode is on.
PrintWindowVerdicts& print_window_verdicts();

/// What ScreenCapture::grab asks, grab by grab: whether to ask PrintWindow,
/// and what came of it. Its own PrintWindowRule counts the window's blank
/// pictures -- three running, with the screen showing it drawn, give it up --
/// inside the process-wide verdict on the window (PrintWindowVerdicts), which
/// decides first: a window refused is asked only by the verdict's probe, and
/// one with nothing to judge it by no more often than the verdict allows.
/// Before any window is attached it is the rule alone.
class PrintWindowAsk {
public:
    /// The window this grab is of, and the time: before ask(). Another
    /// window than the last starts this capture's own count afresh.
    void attach(const PrintWindowVerdicts::Window& w, std::uint64_t now);
    /// Whether to ask PrintWindow this grab.
    bool ask();
    /// What it gave when asked: a picture, or nothing. True when it is back
    /// after being refused.
    bool printed(bool drew);
    /// After a picture of nothing: whether the screen showed the window
    /// itself drawn. True when PrintWindow is given up on the window now --
    /// for the whole process, and said once.
    bool screen(bool drawn);
    /// Still being tried for this window: not refused, and not given up.
    bool trying() const;
    /// The verdicts asked: the process's unless set, as tests do.
    void set_verdicts(PrintWindowVerdicts* v) { verdicts_ = v; }

private:
    PrintWindowVerdicts& verdicts() { return verdicts_ != nullptr ? *verdicts_ : print_window_verdicts(); }

    PrintWindowRule             rule_;
    PrintWindowVerdicts*        verdicts_ = nullptr;
    PrintWindowVerdicts::Window window_;
    bool                        attached_ = false;
    bool                        refused_ = false;   ///< as this grab's turn found the window
    std::uint64_t               now_ = 0;
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

    /// Whose frames these are, for the Load line's grabs owner by owner
    /// (gobot/load_meter.hpp): "vitals", "detector", "shop" ... "other" until
    /// set. A literal, or a string that outlives the capture.
    void set_label(const char* label) {
        label_ = label;
        owner_ = nullptr;
    }

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

    /// Whether PrintWindow is worth trying for this window: this capture's
    /// PrintWindowRule, inside the process's verdict on the window
    /// (PrintWindowAsk).
    PrintWindowAsk print_rule_;

    bool          from_screen_ = false;
    bool          screen_only_ = false;
    std::uint64_t grabs_ = 0;
    std::uint64_t fallbacks_ = 0;

    const char*   label_ = "other";
    GrabOwner*    owner_ = nullptr;       ///< label_'s counts, looked up at the first grab
};

} // namespace gobot
