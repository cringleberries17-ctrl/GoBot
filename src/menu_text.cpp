#include "gobot/menu_text.hpp"

#include "gobot/common.hpp"
#include "gobot/logger.hpp"
#include "gobot/template_matcher.hpp"

#include <algorithm>
#include <cstdio>
#include <mutex>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace gobot {

namespace {

std::uint16_t be16(const std::vector<std::uint8_t>& b, std::size_t at) {
    return at + 2 <= b.size() ? static_cast<std::uint16_t>((b[at] << 8) | b[at + 1]) : 0;
}
std::uint32_t be32(const std::vector<std::uint8_t>& b, std::size_t at) {
    return at + 4 <= b.size() ? (static_cast<std::uint32_t>(be16(b, at)) << 16) | be16(b, at + 2) : 0;
}

/// GoC's WOD.ttf, where its pictures are: <GoC>\data\font\WOD.ttf.
std::string goc_font_path() {
    const std::string images = TemplateStore::client_images();
    if (images.empty()) return {};
    std::string dir = images;
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    const std::size_t slash = dir.find_last_of("\\/");
    if (slash == std::string::npos) return {};
    const std::string path = dir.substr(0, slash) + "\\font\\WOD.ttf";
    return path_exists(path) ? path : std::string{};
}

int grey_of(const Frame& f, int x, int y) {
    std::uint8_t r, g, b;
    f.get(x, y, r, g, b);
    return (r * 299 + g * 587 + b * 114) / 1000;
}

/// `area` of `f`, each pixel white where `keep` says, black elsewhere; opaque.
template <typename Keep>
Frame cut_to_ink(const Frame& f, const ScreenRect& area, Keep keep) {
    Frame out;
    out.width = area.w;
    out.height = area.h;
    out.pixels.assign(static_cast<std::size_t>(area.w) * area.h * 4, 0);
    for (int y = 0; y < area.h; ++y)
        for (int x = 0; x < area.w; ++x) {
            const std::uint8_t v = keep(grey_of(f, area.x + x, area.y + y)) ? 255 : 0;
            std::uint8_t* p = &out.pixels[(static_cast<std::size_t>(y) * area.w + x) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    return out;
}

} // namespace

std::string MenuText::ttf_family(const std::string& path) {
    std::FILE* fp = open_file(path, "rb");
    if (!fp) return {};
    std::vector<std::uint8_t> b;
    std::uint8_t buf[65536];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0 && b.size() < (8u << 20)) b.insert(b.end(), buf, buf + n);
    std::fclose(fp);
    const std::uint16_t tables = be16(b, 4);
    std::size_t name_at = 0;
    for (std::uint16_t i = 0; i < tables; ++i) {
        const std::size_t rec = 12 + static_cast<std::size_t>(i) * 16;
        if (rec + 16 > b.size()) break;
        if (b[rec] == 'n' && b[rec + 1] == 'a' && b[rec + 2] == 'm' && b[rec + 3] == 'e') name_at = be32(b, rec + 8);
    }
    if (name_at == 0 || name_at + 6 > b.size()) return {};
    const std::uint16_t count = be16(b, name_at + 2);
    const std::size_t strings = name_at + be16(b, name_at + 4);
    std::string fallback;
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t r = name_at + 6 + static_cast<std::size_t>(i) * 12;
        if (r + 12 > b.size()) break;
        const std::uint16_t platform = be16(b, r), id = be16(b, r + 6), len = be16(b, r + 8), off = be16(b, r + 10);
        if (id != 1) continue;
        const std::size_t at = strings + off;
        if (at + len > b.size()) continue;
        if (platform == 3 || platform == 0) {
            std::wstring w;
            for (std::size_t k = 0; k + 1 < len; k += 2) w.push_back(static_cast<wchar_t>((b[at + k] << 8) | b[at + k + 1]));
            if (!w.empty()) return wide_to_utf8(w);
        } else if (platform == 1 && fallback.empty()) {
            fallback.assign(reinterpret_cast<const char*>(&b[at]), len);
        }
    }
    return fallback;
}

std::vector<std::string> MenuText::faces() {
    static std::once_flag once;
    static std::vector<std::string> list;
    std::call_once(once, [] {
        const std::string wod = goc_font_path();
        if (!wod.empty()) {
            const int added = AddFontResourceExW(utf8_to_wide(wod).c_str(), FR_PRIVATE, nullptr);
            const std::string family = ttf_family(wod);
            if (added > 0 && !family.empty()) list.push_back(family);
            GB_INFO("Menu text: GoC's %s -- %s", wod.c_str(),
                    added > 0 && !family.empty() ? ("family '" + family + "'").c_str() : "could not be used");
        } else {
            GB_INFO("Menu text: GoC's WOD.ttf not found -- Arial and Tahoma only");
        }
        list.push_back("Arial");
        list.push_back("Tahoma");
    });
    return list;
}

Frame MenuText::render(const std::string& utf8, const std::string& face, int px, bool bold) {
    Frame out;
    const std::wstring text = utf8_to_wide(utf8);
    if (text.empty() || px <= 0) return out;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return out;
    HFONT font = CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                             utf8_to_wide(face).c_str());
    HGDIOBJ old_font = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    const int w = sz.cx + 8, h = sz.cy + 8;
    if (sz.cx <= 0 || sz.cy <= 0 || w > 4000 || h > 400) {
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteDC(dc);
        return out;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteDC(dc);
        return out;
    }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    SetBkMode(dc, OPAQUE);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));
    RECT all{ 0, 0, w, h };
    HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    FillRect(dc, &all, black);
    TextOutW(dc, 4, 4, text.c_str(), static_cast<int>(text.size()));
    GdiFlush();
    const std::uint8_t* src = static_cast<const std::uint8_t*>(bits);
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (src[(static_cast<std::size_t>(y) * w + x) * 4 + 1] >= 128) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
    if (x1 >= 0) {
        x0 = std::max(0, x0 - 1);
        y0 = std::max(0, y0 - 1);
        x1 = std::min(w - 1, x1 + 1);
        y1 = std::min(h - 1, y1 + 1);
        out.width = x1 - x0 + 1;
        out.height = y1 - y0 + 1;
        out.pixels.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
        for (int y = 0; y < out.height; ++y)
            for (int x = 0; x < out.width; ++x) {
                const std::uint8_t v = src[(static_cast<std::size_t>(y + y0) * w + (x + x0)) * 4 + 1] >= 128 ? 255 : 0;
                std::uint8_t* p = &out.pixels[(static_cast<std::size_t>(y) * out.width + x) * 4];
                p[0] = p[1] = p[2] = v;
                p[3] = 255;
            }
    }
    SelectObject(dc, old_bmp);
    SelectObject(dc, old_font);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(dc);
    return out;
}

MenuText::Hit MenuText::find(const Frame& f, const ScreenRect& want, const std::string& utf8, float min_score) {
    Hit best;
    if (!f.valid()) return best;
    ScreenRect area = want;
    area.x = std::max(0, area.x);
    area.y = std::max(0, area.y);
    area.w = std::min(f.width - area.x, area.w);
    area.h = std::min(f.height - area.y, area.h);
    if (!area.valid()) return best;
    const Frame bright = cut_to_ink(f, area, [](int g) { return g > kCut; });
    const Frame dark = cut_to_ink(f, area, [](int g) { return g < 255 - kCut; });
    const ScreenRect whole{ 0, 0, area.w, area.h };
    static const int kSizes[] = { 11, 12, 13, 14 };
    for (const std::string& face : faces()) {
        for (int px : kSizes) {
            for (int b = 0; b < 2; ++b) {
                const Frame tpl = render(utf8, face, px, b != 0);
                if (!tpl.valid() || tpl.width >= area.w || tpl.height >= area.h) continue;
                for (int d = 0; d < 2; ++d) {
                    const TemplateMatcher::Match m = TemplateMatcher::find(d ? dark : bright, whole, tpl, 0.30f);
                    if (m.score <= best.score) continue;
                    best.score = m.score;
                    best.at = ScreenRect{ area.x + m.at.x, area.y + m.at.y, m.at.w, m.at.h };
                    best.px = px;
                    best.bold = b != 0;
                    best.dark = d != 0;
                    best.face = face;
                }
            }
        }
    }
    best.found = best.score >= min_score && best.at.valid();
    return best;
}

} // namespace gobot
