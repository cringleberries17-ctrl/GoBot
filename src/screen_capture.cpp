#include "gobot/screen_capture.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
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

} // namespace

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
    if (bitmap_) { DeleteObject(static_cast<HBITMAP>(bitmap_)); bitmap_ = nullptr; }
    if (dc_)     { DeleteDC(static_cast<HDC>(dc_)); dc_ = nullptr; }
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

        SelectObject(mem, bmp);
        dc_ = mem;
        bitmap_ = bmp;
        bits_ = bits;
        cached_w_ = w;
        cached_h_ = h;
    }

    HDC mem = static_cast<HDC>(dc_);

    // Ask the window to draw itself. Works when it is covered, which matters
    // because our own window is usually sitting on top of it.
    from_screen_ = false;
    BOOL ok = FALSE;

    if (try_print_window_ || --print_probe_countdown_ <= 0) {
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
        ok = PrintWindow(hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
        if (ok && looks_blank(bits_, w, h)) ok = FALSE;

        if (!ok && try_print_window_) {
            // Direct3D windows routinely do this. Say so once rather than
            // leaving a silent per-frame fallback for someone to discover.
            GB_INFO("PrintWindow returns a blank frame for this window -- "
                    "reading pixels off the screen instead; keep the game "
                    "unobscured and fully on screen");
        }
        try_print_window_ = (ok != FALSE);

        // Re-probe occasionally: a window that renders nothing while loading
        // may well cooperate once it is up.
        if (!ok) print_probe_countdown_ = 600;
    }

    if (!ok) {
        // Fall back to lifting pixels off the screen. Only correct while the
        // window is actually visible; reported via from_screen().
        POINT origin{ 0, 0 };
        ClientToScreen(hwnd, &origin);
        HDC screen = GetDC(nullptr);
        if (!screen) return false;
        ok = BitBlt(mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY);
        ReleaseDC(nullptr, screen);
        from_screen_ = true;
        ++fallbacks_;
    }
    if (!ok) return false;

    out.width = w;
    out.height = h;
    out.pixels.resize(static_cast<std::size_t>(w) * h * 4);
    std::memcpy(out.pixels.data(), bits_, out.pixels.size());

    ++grabs_;
    return true;
}

} // namespace gobot
