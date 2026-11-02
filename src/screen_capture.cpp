#include "gobot/screen_capture.hpp"
#include "gobot/load_meter.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

namespace gobot {

// Not in older SDK headers, and we want it: it makes PrintWindow capture
// composited/DirectX content rather than returning a blank bitmap.
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace {

/// Whether a bitmap is entirely one colour.
///
/// PrintWindow reports success for a Direct3D window and then paints only the
/// background brush, so the return value is not evidence that anything was
/// captured. A real game frame is never a single flat colour; a window that
/// declined to render is nothing but.
///
/// Sampled on a coarse grid rather than exhaustively: if any two pixels differ
/// the answer is already no, and the grid finds that in the first few probes.
bool looks_blank(const void* bits, int w, int h) {
    const auto* px = static_cast<const std::uint32_t*>(bits);
    const std::uint32_t first = px[0] & 0x00FFFFFFu;

    const int sx = std::max(1, w / 48);
    const int sy = std::max(1, h / 48);
    for (int y = 0; y < h; y += sy) {
        const std::uint32_t* row = px + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; x += sx)
            if ((row[x] & 0x00FFFFFFu) != first) return false;
    }
    return true;
}

/// The points drawn_where_seen has looked up on this thread, for the Load
/// line: up to 48x48 WindowFromPoint calls a grab while the game is covered.
thread_local std::uint64_t t_points_looked = 0;

/// Whether one window lies over the whole of a client area -- `origin`, `w` x
/// `h` on the screen, of the window whose root is `root` -- as WindowFromPoint
/// sees it: the window at the area's middle, when that is another window and
/// a plain rectangle to the mouse -- enabled, neither layered nor see-through
/// to it, no window region of its own -- and its rectangle holds the whole
/// area. Then no point of the area is this window's, as drawn_where_seen
/// would find point by point, up to 2,352 look-ups a grab while the game is
/// covered. Anything less plain says nothing, and the look-up goes ahead.
bool covered_whole(HWND root, POINT origin, int w, int h) {
    ++t_points_looked;
    const HWND at = WindowFromPoint(POINT{ origin.x + w / 2, origin.y + h / 2 });
    if (at == nullptr) return false;
    const HWND over = GetAncestor(at, GA_ROOT);
    if (over == nullptr || over == root || !IsWindowEnabled(over)) return false;
    if ((GetWindowLongW(over, GWL_EXSTYLE) & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) != 0) return false;
    RECT shape{};
    if (GetWindowRgnBox(over, &shape) != ERROR) return false;     // shaped: it may have holes
    RECT r{};
    if (!GetWindowRect(over, &r)) return false;
    return r.left <= origin.x && r.top <= origin.y && r.right >= origin.x + w && r.bottom >= origin.y + h;
}

/// Whether a copy of the screen over a window's client area shows the window
/// itself drawn: two sampled points of different colour, both where the
/// screen shows this window and not something over it. Only the points of a
/// new colour are looked up, and only on a grab whose PrintWindow came back
/// blank. `seen`, when asked: whether any sampled point showed the window.
bool drawn_where_seen(HWND hwnd, POINT origin, const void* bits, int w, int h, bool* seen = nullptr) {
    const HWND root = GetAncestor(hwnd, GA_ROOT);
    // Covered whole by one plain window: not seen anywhere, said with one
    // look-up rather than one a sampled point.
    if (covered_whole(root, origin, w, h)) return false;
    const auto* px = static_cast<const std::uint32_t*>(bits);
    const int sx = std::max(1, w / 48);
    const int sy = std::max(1, h / 48);
    bool have = false;
    std::uint32_t first = 0;
    for (int y = 0; y < h; y += sy) {
        const std::uint32_t* row = px + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; x += sx) {
            const std::uint32_t c = row[x] & 0x00FFFFFFu;
            if (have && c == first) continue;
            ++t_points_looked;
            const HWND at = WindowFromPoint(POINT{ origin.x + x, origin.y + y });
            if (at == nullptr || GetAncestor(at, GA_ROOT) != root) continue;   // covered, or off every screen
            if (seen != nullptr) *seen = true;
            if (have) return true;
            have = true;
            first = c;
        }
    }
    return false;   // one flat colour where it can be seen, or not seen at all
}

} // namespace

namespace {
std::atomic<int> print_paused{ 0 };
}  // namespace

PrintWindowPause::PrintWindowPause() { ++print_paused; }
PrintWindowPause::~PrintWindowPause() { --print_paused; }
bool PrintWindowPause::active() { return print_paused.load() > 0; }

bool PrintWindowRule::printed(bool drew) {
    if (drew) {
        const bool back = !trying_;
        trying_ = true;
        misses_ = 0;
        return back;
    }
    // A probe that drew nothing waits its turn again.
    if (!trying_) countdown_ = kProbeEvery;
    return false;
}

bool PrintWindowRule::screen(bool drawn) {
    if (!trying_ || !drawn || ++misses_ < kMissesBeforeScreen) return false;
    trying_ = false;
    countdown_ = kProbeEvery;
    return true;
}

// ---- the process's verdict, window by window ----

PrintWindowVerdicts::Entry* PrintWindowVerdicts::find(const Window& w) {
    for (Entry& e : entries_)
        if (e.window == w) return &e;
    return nullptr;
}

const PrintWindowVerdicts::Entry* PrintWindowVerdicts::find(const Window& w) const {
    for (const Entry& e : entries_)
        if (e.window == w) return &e;
    return nullptr;
}

PrintWindowVerdicts::Entry& PrintWindowVerdicts::entry(const Window& w, std::uint64_t now) {
    if (Entry* e = find(w)) return *e;
    if (entries_.size() >= kMaxWindows) {
        // The one longest unasked: a window long closed, most likely.
        auto oldest = std::min_element(entries_.begin(), entries_.end(),
                                       [](const Entry& a, const Entry& b) { return a.used_ms < b.used_ms; });
        entries_.erase(oldest);
    }
    Entry e;
    e.window = w;
    e.used_ms = now;
    entries_.push_back(e);
    return entries_.back();
}

PrintWindowVerdicts::Turn PrintWindowVerdicts::turn(const Window& w, std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = entry(w, now);
    e.used_ms = now;
    Turn t;
    t.refused = e.refused;
    if (e.refused) {
        // A probe, at most once every kReprobeMs; none in light mode.
        if (light_.load() || now < e.probe_ms + kReprobeMs) return t;
        e.probe_ms = now;
        t.ask = true;
        return t;
    }
    // A window PrintWindow drew lately is not gated: its blank picture is the
    // game's own black frame (a loading screen), and the first grab after the
    // game draws again must ask, or the screen's copy -- the window covering
    // the game -- is read until the gate's time is up.
    const bool drew_lately = e.drew_ms != 0 && now < e.drew_ms + kDrewTrustMs;
    if (e.unclear && !drew_lately) {
        if (now < e.unclear_ms + kUnclearAskMs) return t;
        e.unclear_ms = now;
    }
    t.ask = true;
    return t;
}

bool PrintWindowVerdicts::drew(const Window& w, std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry* e = find(w);
    if (e == nullptr) return false;
    const bool was = e->refused;
    e->refused = false;
    e->unclear = false;
    e->drew_ms = now;
    return was;
}

void PrintWindowVerdicts::judged(const Window& w) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Entry* e = find(w)) e->unclear = false;
}

void PrintWindowVerdicts::unclear(const Window& w, std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = entry(w, now);
    if (e.refused) return;      // a probe that showed nothing: still refused
    e.unclear = true;
    e.unclear_ms = now;
}

bool PrintWindowVerdicts::refuse(const Window& w, std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = entry(w, now);
    if (e.refused) return false;
    e.refused = true;
    e.unclear = false;
    e.probe_ms = now;
    return true;
}

bool PrintWindowVerdicts::refused(const Window& w) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Entry* e = find(w);
    return e != nullptr && e->refused;
}

std::size_t PrintWindowVerdicts::windows() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

PrintWindowVerdicts& print_window_verdicts() {
    static PrintWindowVerdicts verdicts;
    return verdicts;
}

void PrintWindowAsk::attach(const PrintWindowVerdicts::Window& w, std::uint64_t now) {
    // A capture that grabs another window now -- the auto login's, looking
    // at VTCZone and then the launcher -- counts that window's blank
    // pictures from nothing; the verdict on each is the process's anyway.
    if (!attached_ || w != window_) rule_ = PrintWindowRule{};
    window_ = w;
    now_ = now;
    attached_ = true;
}

bool PrintWindowAsk::ask() {
    if (!attached_) return rule_.ask();
    const PrintWindowVerdicts::Turn t = verdicts().turn(window_, now_);
    refused_ = t.refused;
    if (refused_) return t.ask;     // the verdict's probe, or nothing
    // Not refused, though this capture gave it up itself: another capture's
    // probe has seen it draw since. Tried again, from nothing.
    if (!rule_.trying()) rule_ = PrintWindowRule{};
    return t.ask && rule_.ask();
}

bool PrintWindowAsk::printed(bool drew) {
    if (!attached_) return rule_.printed(drew);
    rule_.printed(drew);
    // Back once the window had been refused, whoever refused it: said once.
    return drew && verdicts().drew(window_, now_);
}

bool PrintWindowAsk::screen(bool drawn) {
    if (!attached_) return rule_.screen(drawn);
    if (!drawn) {
        // Covered where it would be seen, or one flat colour: nothing said
        // about PrintWindow, and not asked again for a while.
        verdicts().unclear(window_, now_);
        return false;
    }
    verdicts().judged(window_);
    if (!rule_.screen(true)) return false;
    return verdicts().refuse(window_, now_);
}

bool PrintWindowAsk::trying() const {
    if (!attached_) return rule_.trying();
    return !refused_ && rule_.trying();
}

bool save_png(const Frame& f, const std::string& path) {
    if (!f.valid() || path.empty()) return false;

    // Started and stopped around the one call. Saving a frame is a debugging
    // act, not something a capture loop does, so there is nothing to amortise.
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) return false;

    bool ok = false;
    {
        // The frame is already the layout GDI+ wants: 32-bit BGRA, top-down,
        // tightly packed. Hand it the buffer rather than copying pixel by pixel.
        Gdiplus::Bitmap bmp(f.width, f.height, f.width * 4, PixelFormat32bppRGB,
                            const_cast<BYTE*>(f.pixels.data()));
        if (bmp.GetLastStatus() == Gdiplus::Ok) {
            UINT n = 0, bytes = 0;
            Gdiplus::GetImageEncodersSize(&n, &bytes);
            std::vector<std::uint8_t> buffer(bytes);
            auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
            if (bytes != 0 && Gdiplus::GetImageEncoders(n, bytes, codecs) == Gdiplus::Ok) {
                for (UINT i = 0; i < n && !ok; ++i) {
                    if (std::wcscmp(codecs[i].MimeType, L"image/png") != 0) continue;
                    const int wide = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1,
                                                         nullptr, 0);
                    std::wstring wpath(static_cast<std::size_t>(wide), L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wide);
                    ok = bmp.Save(wpath.c_str(), &codecs[i].Clsid, nullptr) == Gdiplus::Ok;
                }
            }
        }
    }

    Gdiplus::GdiplusShutdown(token);
    if (!ok) GB_WARN("could not write %s", path.c_str());
    return ok;
}

ScreenCapture::~ScreenCapture() { release(); }

void ScreenCapture::release() {
    HDC dc = static_cast<HDC>(dc_);
    // The DC's own bitmap back in first, so the DIB is in no DC when it is
    // deleted; and the DC deleted before the DIB, so it is in none even
    // should that have failed.
    if (dc && old_bitmap_) SelectObject(dc, static_cast<HGDIOBJ>(old_bitmap_));
    if (dc) DeleteDC(dc);
    if (bitmap_ && !DeleteObject(static_cast<HGDIOBJ>(bitmap_))) {
        // Said once: short-lived captures are made every few seconds.
        static std::atomic<bool> said{ false };
        if (!said.exchange(true))
            GB_WARN("ScreenCapture: a frame bitmap could not be freed -- GDI handles are being lost");
    }
    dc_ = nullptr;
    bitmap_ = nullptr;
    old_bitmap_ = nullptr;
    bits_ = nullptr;
    cached_w_ = cached_h_ = 0;
}

bool ScreenCapture::grab(void* hwnd_raw, Frame& out) {
    HWND hwnd = static_cast<HWND>(hwnd_raw);
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return false;

    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;

    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    if (w != cached_w_ || h != cached_h_ || !dc_) {
        release();

        HDC screen = GetDC(nullptr);
        if (!screen) return false;

        HDC mem = CreateCompatibleDC(screen);
        if (!mem) { ReleaseDC(nullptr, screen); return false; }

        BITMAPINFO bi{};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;         // negative: top-down rows
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen);

        if (!bmp || !bits) {
            if (bmp) DeleteObject(bmp);
            DeleteDC(mem);
            return false;
        }

        // Keep the bitmap the DC came with, to put back before the DIB is
        // deleted. One still selected into a DC cannot be deleted at all --
        // DeleteObject refuses it and DeleteDC does not free it -- so every
        // capture torn down or resized used to leave its DIB behind, 3.7 MB
        // and a GDI handle at 1280x720, until the 10,000-handle quota ran out
        // and every grab failed.
        HGDIOBJ old = SelectObject(mem, bmp);
        if (!old) {
            DeleteObject(bmp);
            DeleteDC(mem);
            return false;
        }
        dc_ = mem;
        bitmap_ = bmp;
        old_bitmap_ = old;
        bits_ = bits;
        cached_w_ = w;
        cached_h_ = h;
    }

    HDC mem = static_cast<HDC>(dc_);

    // What this grab costs, counted under the capture's label for the Load
    // line (gobot/load_meter.hpp): PrintWindow and BitBlt timed, the copies
    // made again and the points looked up, and the frame handed out.
    if (owner_ == nullptr) owner_ = &grab_owners().owner(label_);
    GrabOwner& counted = *owner_;

    // Ask the window to draw itself. Works when it is covered, which matters
    // because our own window is usually sitting on top of it.
    from_screen_ = false;
    BOOL ok = FALSE;
    bool printed = false;     // PrintWindow was asked this grab, and drew nothing

    // Which window this is, for the process's verdict on it, which the rule
    // goes by first: one that refused PrintWindow is not asked by every
    // capture of it, nor one with nothing to judge it by on every grab
    // (PrintWindowVerdicts).
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        print_rule_.attach(PrintWindowVerdicts::Window{ hwnd, static_cast<std::uint32_t>(pid) }, GetTickCount64());
    }

    if (!screen_only_ && !PrintWindowPause::active() && print_rule_.ask()) {
        // Clear first. PrintWindow can return success having painted nothing
        // at all, in which case the bitmap would still hold the previous
        // frame and the blank check below would be fooled by it.
        std::memset(bits_, 0, static_cast<std::size_t>(w) * h * 4);

        // PW_CLIENTONLY matters as much as PW_RENDERFULLCONTENT. Without it
        // PrintWindow draws the window frame as well, putting the caption bar
        // and border inside the bitmap and pushing the game's own pixels down
        // and right by however thick they are -- while the screen fallback
        // below starts at the client origin and does not. Every saved
        // rectangle would then mean two different things depending on which
        // path the frame happened to come from.
        const std::uint64_t print_from = load_clock_us();
        ok = PrintWindow(hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
        counted.print_window.add_since(print_from);
        if (ok && looks_blank(bits_, w, h)) ok = FALSE;
        printed = !ok;

        if (print_rule_.printed(ok != FALSE))
            GB_INFO("PrintWindow draws this window again (seen by the %s capture) -- every capture reads "
                    "it from its own picture, whatever lies over it",
                    label_);
    }

    if (!ok) {
        // Fall back to lifting pixels off the screen. Only correct while the
        // window is actually visible; reported via from_screen().
        POINT origin{ 0, 0 };
        const auto copy = [&]() -> BOOL {
            // Asked again on every copy: a copy made after a pause may find the
            // window gone or minimised meanwhile.
            if (!IsWindow(hwnd) || IsIconic(hwnd)) return FALSE;
            origin = POINT{ 0, 0 };
            if (!ClientToScreen(hwnd, &origin)) return FALSE;
            HDC screen = GetDC(nullptr);
            if (!screen) return FALSE;
            // Black, rather than whatever the last frame left, wherever the copy
            // does not reach: the part of a window dragged off the screen. An old
            // picture of a page there would read as that page still being up.
            // Only then: a window wholly on the screen is copied whole.
            const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            if (origin.x < vx || origin.y < vy ||
                origin.x + w > vx + GetSystemMetrics(SM_CXVIRTUALSCREEN) ||
                origin.y + h > vy + GetSystemMetrics(SM_CYVIRTUALSCREEN))
                std::memset(bits_, 0, static_cast<std::size_t>(w) * h * 4);
            const std::uint64_t copy_from = load_clock_us();
            const BOOL copied = BitBlt(mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY);
            counted.bitblt.add_since(copy_from);
            ReleaseDC(nullptr, screen);
            return copied;
        };
        ok = copy();
        from_screen_ = true;
        ++fallbacks_;

        // Right after PrintWindow was asked, the screen can show the game black
        // until it draws its next frame: on the live client (2026-10-30) a copy
        // black but for the corners Windows rounds came once in every
        // kProbeEvery grabs, and for seconds on end while PrintWindow was being
        // tried -- every grab asked it, every copy after it was black, and so it
        // was never seen to be drawn and never given up. Copied again a moment
        // later, a few times, while the window is one flat colour where it can
        // be seen; never on a grab that did not ask PrintWindow, nor with the
        // window seen nowhere -- covered whole, nothing to wait for.
        const std::uint64_t points_before = t_points_looked;
        bool seen = false;
        bool drawn = printed && ok && drawn_where_seen(hwnd, origin, bits_, w, h, &seen);
        for (int again = 0; printed && ok && !drawn && seen && again < kRecopies; ++again) {
            Sleep(kRecopyGapMs);
            counted.recopies.fetch_add(1, std::memory_order_relaxed);
            ok = copy();
            drawn = ok && drawn_where_seen(hwnd, origin, bits_, w, h);
        }
        counted.points.fetch_add(t_points_looked - points_before, std::memory_order_relaxed);

        // A blank picture is PrintWindow's fault only when the screen shows the
        // window itself drawn: Direct3D windows can do this. One that is as
        // blank where it can be seen -- the game's black loading screen, with a
        // toast or the taskbar over a corner of it -- says nothing about
        // PrintWindow. See PrintWindowRule.
        if (printed && ok && print_rule_.trying() && print_rule_.screen(drawn)) {
            // Direct3D windows routinely do this. Say so once rather than
            // leaving a silent per-frame fallback for someone to discover:
            // once for the whole process, which goes by it from now on.
            GB_INFO("PrintWindow returns a blank frame for this window (seen by the %s capture) -- every "
                    "capture reads pixels off the screen instead; keep the game unobscured and fully on "
                    "screen. %s",
                    label_,
                    print_window_verdicts().light()
                        ? "Light mode: it is not asked again this session."
                        : "It is asked again once every 5 minutes.");
        }
    }
    if (!ok) return false;

    out.width = w;
    out.height = h;
    out.pixels.resize(static_cast<std::size_t>(w) * h * 4);
    std::memcpy(out.pixels.data(), bits_, out.pixels.size());

    ++grabs_;
    counted.grabs.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace gobot
