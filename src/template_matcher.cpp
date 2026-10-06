#include "gobot/template_matcher.hpp"
#include "gobot/logger.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

namespace gobot {
namespace {

inline int grey_of(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return (r * 77 + g * 151 + b * 28) >> 8;
}

/// How much the search is shrunk before the first pass.
///
/// Scaled to the template rather than fixed. The coarse pass costs the screen
/// area times the template area, both divided by the step to the fourth power,
/// so a step that suits a twenty-pixel icon leaves a four-hundred-pixel dialog
/// taking half a second. Shrinking a big template harder costs it nothing --
/// it has detail to spare -- while a small one cannot afford to lose any.
///
/// Chosen so the template keeps at least six blocks across its shorter side.
/// Below that there is not enough of it left to recognise: the blocks grow
/// wider than the features, the picture averages towards flat grey, and the
/// first pass rejects the right answer before anything can refine it.
///
/// A small picture therefore gets a step of one and is searched at full size.
/// That costs the screen area times the template area with nothing divided
/// away, which is a few hundred milliseconds across a whole screen -- but it
/// only happens for templates about a dozen pixels across, where there was
/// never anything to gain by shrinking, and where forcing a step of three
/// turned an eleven pixel icon into nine blocks and lost it.
///
/// Capped the other way so a large dialog does not cost half a second.
int coarse_step(int w, int h) {
    const int smaller = std::min(w, h);
    return std::clamp(smaller / 6, 1, 16);
}

/// Promising places from the coarse pass that get looked at properly.
constexpr std::size_t kRefine = 12;

/// How far either side the search looks at each level on the way down. Two is
/// enough: halving the step moves the answer by at most a cell or so.
constexpr int kDescendWindow = 2;

/// How far below the asked-for score a coarse hit may be and still be worth
/// refining. The coarse pass is blurry and scores lower than the truth, so
/// holding it to the final standard throws away the right answer.
constexpr double kCoarseSlack = 0.25;

} // namespace

TemplateMatcher::Reduced TemplateMatcher::reduce(const Frame& f, const ScreenRect& area,
                                                 int step) {
    Reduced out;
    if (!f.valid() || step <= 0) return out;

    const int x0 = std::max(0, area.x), y0 = std::max(0, area.y);
    const int x1 = std::min(f.width, area.right()), y1 = std::min(f.height, area.bottom());
    if (x1 <= x0 || y1 <= y0) return out;

    out.w = (x1 - x0) / step;
    out.h = (y1 - y0) / step;
    if (out.w <= 0 || out.h <= 0) return out;

    out.grey.resize(static_cast<std::size_t>(out.w) * out.h);
    out.solid.assign(static_cast<std::size_t>(out.w) * out.h, 1);

    for (int y = 0; y < out.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            // The mean of the block rather than one pixel from it, so that a
            // thin bright line does not vanish between samples.
            int sum = 0, n = 0, opaque = 0;
            for (int dy = 0; dy < step; ++dy) {
                for (int dx = 0; dx < step; ++dx) {
                    const int sx = x0 + x * step + dx, sy = y0 + y * step + dy;
                    if (sx >= x1 || sy >= y1) continue;
                    std::uint8_t r, g, b;
                    f.get(sx, sy, r, g, b);
                    const std::size_t i = (static_cast<std::size_t>(sy) * f.width + sx) * 4;
                    const bool solid = i + 3 >= f.pixels.size() ||
                                       f.pixels[i + 3] >= kOpaque;
                    if (solid) ++opaque;
                    sum += grey_of(r, g, b);
                    ++n;
                }
            }
            const std::size_t i = static_cast<std::size_t>(y) * out.w + x;
            out.grey[i] = n ? sum / n : 0;
            // A block counts as part of the picture when most of it does.
            out.solid[i] = (n && opaque * 2 >= n) ? 1 : 0;
            if (out.solid[i]) ++out.count;
        }
    }
    return out;
}

double TemplateMatcher::score_at(const Reduced& hay, const Reduced& needle,
                                 int px, int py) {
    // Zero-mean normalised correlation over the template's solid pixels only.
    // Subtracting the means is what makes this survive the same dialog drawn
    // over a brighter background; normalising is what makes the score
    // comparable between templates of different sizes and contrasts.
    double sum_a = 0.0, sum_b = 0.0;
    int n = 0;
    for (int y = 0; y < needle.h; ++y) {
        const int hy = py + y;
        if (hy < 0 || hy >= hay.h) return -1.0;
        for (int x = 0; x < needle.w; ++x) {
            const std::size_t ni = static_cast<std::size_t>(y) * needle.w + x;
            if (!needle.solid[ni]) continue;
            const int hx = px + x;
            if (hx < 0 || hx >= hay.w) return -1.0;
            sum_a += hay.grey[static_cast<std::size_t>(hy) * hay.w + hx];
            sum_b += needle.grey[ni];
            ++n;
        }
    }
    if (n < 8) return -1.0;

    const double mean_a = sum_a / n, mean_b = sum_b / n;
    double cov = 0.0, var_a = 0.0, var_b = 0.0;
    for (int y = 0; y < needle.h; ++y) {
        for (int x = 0; x < needle.w; ++x) {
            const std::size_t ni = static_cast<std::size_t>(y) * needle.w + x;
            if (!needle.solid[ni]) continue;
            const double a = hay.grey[static_cast<std::size_t>(py + y) * hay.w + (px + x)] - mean_a;
            const double b = needle.grey[ni] - mean_b;
            cov += a * b;
            var_a += a * a;
            var_b += b * b;
        }
    }
    // A flat patch correlates with everything and with nothing; say nothing.
    if (var_a < 1e-6 || var_b < 1e-6) return -1.0;
    const double s = cov / std::sqrt(var_a * var_b);
    return std::isfinite(s) ? s : -1.0;
}

// The hand-rolled matcher. Built only when OpenCV is not: see
// template_matcher_cv.cpp, which provides the same two functions on top of
// cv::matchTemplate. Kept rather than deleted so the project still builds and
// its tests still pass on a machine that cannot fetch OpenCV.
#ifndef GOBOT_HAVE_OPENCV

std::vector<TemplateMatcher::Match> TemplateMatcher::find_all(
        const Frame& frame, const ScreenRect& area, const Frame& tpl,
        float min_score, std::size_t limit, Tone) {
    // The tone is ignored here: this matcher reduces everything to grey, which
    // is exactly the limitation that moved matching to OpenCV.
    std::vector<Match> out;
    if (!frame.valid() || !tpl.valid()) return out;

    const ScreenRect look = area.valid() ? area
                                         : ScreenRect{ 0, 0, frame.width, frame.height };
    if (tpl.width > look.w || tpl.height > look.h) return out;

    const ScreenRect whole_tpl{ 0, 0, tpl.width, tpl.height };

    // Coarse first.
    const int step = coarse_step(tpl.width, tpl.height);
    const Reduced hay_c = reduce(frame, look, step);
    const Reduced ndl_c = reduce(tpl, whole_tpl, step);
    if (hay_c.w <= 0 || ndl_c.w <= 0 || ndl_c.count < 8) return out;

    struct Hit { double score; int x, y; };
    std::vector<Hit> coarse;
    for (int y = 0; y + ndl_c.h <= hay_c.h; ++y) {
        for (int x = 0; x + ndl_c.w <= hay_c.w; ++x) {
            const double s = score_at(hay_c, ndl_c, x, y);
            if (s < min_score - kCoarseSlack) continue;
            coarse.push_back(Hit{ s, x, y });
        }
    }
    if (coarse.empty()) return out;
    std::sort(coarse.begin(), coarse.end(),
              [](const Hit& a, const Hit& b) { return a.score > b.score; });

    // Then down to full size in halving steps, rather than in one jump.
    //
    // Going straight from the coarse answer to full resolution means searching
    // the whole blurred block it stands for -- a window of plus or minus the
    // step, at full size. That is fine for an icon and ruinous for a dialog:
    // a template four hundred pixels across costs over a billion comparisons
    // per candidate, and a single look-up takes twenty seconds.
    //
    // Halving instead keeps the window at a couple of pixels the whole way
    // down, so each level costs about the same and the total is a small
    // multiple of one full-size comparison.
    std::vector<Reduced> hay_levels, ndl_levels;
    std::vector<int>     steps;
    for (int s = step; s >= 1; s /= 2) {
        hay_levels.push_back(reduce(frame, look, s));
        ndl_levels.push_back(reduce(tpl, whole_tpl, s));
        steps.push_back(s);
        if (s == 1) break;
    }
    if (hay_levels.empty() || hay_levels.back().w <= 0) return out;

    const std::size_t tries = std::min(coarse.size(), kRefine + limit);
    for (std::size_t i = 0; i < tries; ++i) {
        int bx = coarse[i].x, by = coarse[i].y;
        double best = coarse[i].score;

        for (std::size_t level = 1; level < hay_levels.size(); ++level) {
            const Reduced& hay = hay_levels[level];
            const Reduced& ndl = ndl_levels[level];
            if (hay.w <= 0 || ndl.w <= 0) break;

            // Carry the answer down by the ratio the step actually changed
            // by, not by two.
            //
            // The levels are made by halving, but integer halving does not
            // always halve: a step of three becomes one, a ratio of three.
            // Assuming two there lands the search a third of the way to where
            // the answer is, misses it, and reports the picture absent -- for
            // templates of one size and not another, which is a maddening
            // thing to be told. The first version of this failed exactly so,
            // and only for templates whose shorter side was around eighteen
            // pixels.
            const double ratio = static_cast<double>(steps[level - 1]) / steps[level];
            bx = static_cast<int>(std::lround(bx * ratio));
            by = static_cast<int>(std::lround(by * ratio));
            best = -1.0;
            int fx = bx, fy = by;
            for (int dy = -kDescendWindow; dy <= kDescendWindow; ++dy) {
                for (int dx = -kDescendWindow; dx <= kDescendWindow; ++dx) {
                    const int px = bx + dx, py = by + dy;
                    if (px < 0 || py < 0) continue;
                    if (px + ndl.w > hay.w || py + ndl.h > hay.h) continue;
                    const double s = score_at(hay, ndl, px, py);
                    if (s > best) { best = s; fx = px; fy = py; }
                }
            }
            bx = fx;
            by = fy;
        }
        if (best < min_score) continue;

        const int best_x = bx, best_y = by;

        Match m;
        m.found = true;
        m.score = static_cast<float>(best);
        m.at = ScreenRect{ look.x + best_x, look.y + best_y, tpl.width, tpl.height };

        // Overlapping hits are the same thing found twice.
        bool duplicate = false;
        for (const Match& had : out) {
            if (!had.at.overlaps(m.at)) continue;
            duplicate = true;
            break;
        }
        if (!duplicate) out.push_back(m);
        if (out.size() >= limit) break;
    }

    std::sort(out.begin(), out.end(),
              [](const Match& a, const Match& b) { return a.score > b.score; });
    return out;
}

TemplateMatcher::Match TemplateMatcher::find(const Frame& frame, const ScreenRect& area,
                                             const Frame& tpl, float min_score,
                                             Tone tone) {
    const auto all = find_all(frame, area, tpl, min_score, 1, tone);
    return all.empty() ? Match{} : all.front();
}

#endif // !GOBOT_HAVE_OPENCV

// ------------------------------------------------------------------- store

namespace {

struct GdiPlusOnce {
    ULONG_PTR token = 0;
    bool ok = false;
    GdiPlusOnce() {
        Gdiplus::GdiplusStartupInput in;
        ok = Gdiplus::GdiplusStartup(&token, &in, nullptr) == Gdiplus::Ok;
    }
    ~GdiPlusOnce() { if (ok) Gdiplus::GdiplusShutdown(token); }
};

GdiPlusOnce& gdiplus() {
    static GdiPlusOnce instance;
    return instance;
}

bool read_png(const std::wstring& path, Frame& out) {
    if (!gdiplus().ok) return false;
    Gdiplus::Bitmap bmp(path.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;

    out.width = static_cast<int>(bmp.GetWidth());
    out.height = static_cast<int>(bmp.GetHeight());
    if (out.width <= 0 || out.height <= 0 || out.width > 4096 || out.height > 4096)
        return false;

    out.pixels.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
    Gdiplus::BitmapData data{};
    Gdiplus::Rect rect(0, 0, out.width, out.height);
    if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data)
            != Gdiplus::Ok)
        return false;

    const auto* src = static_cast<const std::uint8_t*>(data.Scan0);
    for (int y = 0; y < out.height; ++y) {
        std::memcpy(out.pixels.data() + static_cast<std::size_t>(y) * out.width * 4,
                    src + static_cast<std::size_t>(y) * data.Stride,
                    static_cast<std::size_t>(out.width) * 4);
    }
    bmp.UnlockBits(&data);
    return true;
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

void load_into(const std::wstring& dir, const std::string& prefix,
               std::unordered_map<std::string, Frame>& into) {
    WIN32_FIND_DATAW find{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        const std::wstring name = find.cFileName;
        if (name == L"." || name == L"..") continue;

        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            load_into(dir + L"\\" + name, prefix + to_utf8(name) + "/", into);
            continue;
        }
        const std::size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos) continue;
        if (_wcsicmp(name.c_str() + dot, L".png") != 0) continue;

        Frame f;
        if (!read_png(dir + L"\\" + name, f)) continue;
        into[prefix + to_utf8(name.substr(0, dot))] = std::move(f);
    } while (FindNextFileW(h, &find));

    FindClose(h);
}

} // namespace

namespace {

/// Decode a PNG that is already in memory.
bool read_png_blob(const std::uint8_t* data, std::size_t size, Frame& out) {
    if (!gdiplus().ok || !data || size < 32) return false;

    IStream* stream = nullptr;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) return false;
    if (void* p = GlobalLock(mem)) {
        std::memcpy(p, data, size);
        GlobalUnlock(mem);
    } else {
        GlobalFree(mem);
        return false;
    }
    if (CreateStreamOnHGlobal(mem, TRUE, &stream) != S_OK) {
        GlobalFree(mem);
        return false;
    }

    bool ok = false;
    {
        Gdiplus::Bitmap bmp(stream);
        if (bmp.GetLastStatus() == Gdiplus::Ok) {
            out.width = static_cast<int>(bmp.GetWidth());
            out.height = static_cast<int>(bmp.GetHeight());
            if (out.width > 0 && out.height > 0 &&
                out.width <= 4096 && out.height <= 4096) {
                out.pixels.assign(
                    static_cast<std::size_t>(out.width) * out.height * 4, 0);
                Gdiplus::BitmapData bits{};
                Gdiplus::Rect rect(0, 0, out.width, out.height);
                if (bmp.LockBits(&rect, Gdiplus::ImageLockModeRead,
                                 PixelFormat32bppARGB, &bits) == Gdiplus::Ok) {
                    for (int y = 0; y < out.height; ++y)
                        std::memcpy(out.pixels.data() +
                                        static_cast<std::size_t>(y) * out.width * 4,
                                    static_cast<const std::uint8_t*>(bits.Scan0) +
                                        static_cast<std::size_t>(y) * bits.Stride,
                                    static_cast<std::size_t>(out.width) * 4);
                    bmp.UnlockBits(&bits);
                    ok = true;
                }
            }
        }
    }
    stream->Release();
    return ok;
}

/// A length written seven bits per byte, high bit meaning "another follows".
/// How .NET writes string lengths.
bool read_7bit(const std::vector<std::uint8_t>& b, std::size_t& at, std::uint32_t& out) {
    out = 0;
    for (int shift = 0; shift <= 28; shift += 7) {
        if (at >= b.size()) return false;
        const std::uint8_t byte = b[at++];
        out |= static_cast<std::uint32_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) return true;
    }
    return false;
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& b, std::size_t at) {
    if (at + 4 > b.size()) return 0;
    return static_cast<std::uint32_t>(b[at]) |
           (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

/// One picture inside a .NET assembly, with the name it was given.
struct NamedBlob {
    std::string name;
    std::size_t at = 0;
    std::size_t size = 0;
};

/// Read the resource directory out of a .NET assembly.
///
/// The pictures a WPF application ships are not loose in the file: they sit in
/// one `.g.resources` stream that carries a name for each. Scanning the file
/// for PNG signatures finds the bytes but throws the names away, and the names
/// are the entire point -- `images/mining/npc_menu.png` and
/// `images/hpmp_indicator2.png` say what each picture is for, where `t137` and
/// `t208` say nothing and have to be identified by eye, one at a time, against
/// a running game.
///
/// The format is fixed and documented by its own reader:
///
///     magic 0xBEEFCACE, header version, header size, then that many bytes of
///     type names to skip; format version; how many resources; how many types,
///     then each type's name; pad to an eight byte boundary; a hash per
///     resource; a name position per resource; where the data starts; then the
///     names, each a length in bytes followed by UTF-16 and the offset of its
///     data; and finally the data, each a type code followed by a length and
///     the bytes.
std::vector<NamedBlob> dotnet_resources(const std::vector<std::uint8_t>& blob) {
    std::vector<NamedBlob> out;

    for (std::size_t magic = 0; magic + 32 < blob.size(); ++magic) {
        if (blob[magic] != 0xCE || blob[magic + 1] != 0xCA ||
            blob[magic + 2] != 0xEF || blob[magic + 3] != 0xBE) continue;

        std::size_t p = magic + 8;
        const std::uint32_t class_info = read_u32(blob, p);
        p += 4 + class_info;
        if (p + 12 > blob.size()) continue;

        const std::uint32_t version = read_u32(blob, p); p += 4;
        const std::uint32_t count   = read_u32(blob, p); p += 4;
        const std::uint32_t types   = read_u32(blob, p); p += 4;
        if (version != 2 || count == 0 || count > 100000) continue;

        bool sane = true;
        for (std::uint32_t t = 0; t < types && sane; ++t) {
            std::uint32_t len = 0;
            if (!read_7bit(blob, p, len) || p + len > blob.size()) sane = false;
            else p += len;
        }
        if (!sane) continue;

        while ((p - magic) % 8 != 0) ++p;
        p += static_cast<std::size_t>(count) * 4;          // one hash each
        const std::size_t name_positions = p;
        p += static_cast<std::size_t>(count) * 4;
        if (p + 4 > blob.size()) continue;

        const std::size_t data_section = magic + read_u32(blob, p);
        p += 4;
        const std::size_t name_section = p;

        for (std::uint32_t i = 0; i < count; ++i) {
            std::size_t q = name_section + read_u32(blob, name_positions + i * 4);
            std::uint32_t bytes = 0;
            if (!read_7bit(blob, q, bytes) || q + bytes + 4 > blob.size()) break;

            // UTF-16, and these names are plain ASCII paths. Anything else is
            // not a picture path and is skipped below anyway.
            std::string name;
            name.reserve(bytes / 2);
            for (std::uint32_t k = 0; k + 1 < bytes; k += 2)
                name.push_back(static_cast<char>(blob[q + k]));
            q += bytes;

            std::size_t data = data_section + read_u32(blob, q);
            std::uint32_t type_code = 0;
            if (!read_7bit(blob, data, type_code)) continue;
            if (data + 4 > blob.size()) continue;

            const std::uint32_t size = read_u32(blob, data);
            data += 4;
            if (size < 32 || data + size > blob.size()) continue;

            out.push_back(NamedBlob{ name, data, size });
        }
        if (!out.empty()) break;
    }
    return out;
}

/// Walk a PNG's chunks to find where it ends. Needed because the pictures sit
/// end to end inside a larger file with nothing marking their extent.
std::size_t png_length(const std::uint8_t* p, std::size_t avail) {
    if (avail < 16) return 0;
    std::size_t at = 8;                       // past the signature
    while (at + 12 <= avail) {
        const std::uint32_t len = (static_cast<std::uint32_t>(p[at]) << 24) |
                                  (static_cast<std::uint32_t>(p[at + 1]) << 16) |
                                  (static_cast<std::uint32_t>(p[at + 2]) << 8) |
                                   static_cast<std::uint32_t>(p[at + 3]);
        if (len > avail) return 0;
        const bool end = std::memcmp(p + at + 4, "IEND", 4) == 0;
        at += 12 + len;
        if (at > avail) return 0;
        if (end) return at;
    }
    return 0;
}

} // namespace

bool TemplateStore::load_from_assembly(const std::string& dll_path,
                                       const std::string& name_map_ini) {
    by_name_.clear();
    if (dll_path.empty()) return false;

    std::FILE* f = std::fopen(dll_path.c_str(), "rb");
    if (!f) {
        GB_WARN("TemplateStore: cannot open %s", dll_path.c_str());
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) { std::fclose(f); return false; }

    std::vector<std::uint8_t> blob(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(blob.data(), 1, blob.size(), f);
    std::fclose(f);
    if (got != blob.size()) return false;

    static const std::uint8_t kPng[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    int ordinal = 0;

    // The names first, if the assembly keeps a resource directory -- which a
    // WPF application always does.
    for (const NamedBlob& entry : dotnet_resources(blob)) {
        if (entry.size < 8) continue;
        if (std::memcmp(blob.data() + entry.at, kPng, 8) != 0) continue;

        Frame picture;
        if (!read_png_blob(blob.data() + entry.at, entry.size, picture)) continue;

        // Shortened to what the name actually says. "images/" is on every one
        // of them and says nothing, and the extension is not part of the name
        // anywhere else in this project.
        std::string name = entry.name;
        if (name.rfind("images/", 0) == 0) name.erase(0, 7);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0)
            name.erase(name.size() - 4);
        if (name.empty()) continue;

        by_name_[name] = std::move(picture);
        ++ordinal;
    }

    // Failing that, take the bytes and number them. Worse in every way -- a
    // picture called t137 has to be identified by eye against a running game --
    // but it beats having nothing when an assembly is laid out differently.
    if (ordinal == 0) {
        for (std::size_t i = 0; i + 8 < blob.size();) {
            if (std::memcmp(blob.data() + i, kPng, 8) != 0) { ++i; continue; }

            const std::size_t len = png_length(blob.data() + i, blob.size() - i);
            if (len == 0) { ++i; continue; }

            Frame picture;
            if (read_png_blob(blob.data() + i, len, picture)) {
                char name[16];
                std::snprintf(name, sizeof name, "t%03d", ordinal);
                by_name_[name] = std::move(picture);
                ++ordinal;
            }
            i += len;
        }
        GB_WARN("TemplateStore: %s has no resource directory, so its %d pictures "
                "are numbered rather than named", dll_path.c_str(), ordinal);
    }

    const int named = apply_names(name_map_ini);
    GB_INFO("TemplateStore: %d pictures from %s, %d short names applied",
            ordinal, dll_path.c_str(), named);
    return ordinal > 0;
}

int TemplateStore::apply_names(const std::string& name_map_ini) {
    // Short names for pictures that already have long ones, and names at all
    // for the set that arrives with none. Either way the mapping is this
    // project's own work and lives in this project's own config, so that the
    // features below read as what they mean -- "close_button" rather than
    // "inven/0/btn_CloseBox.spr/0" or "t016".
    if (name_map_ini.empty()) return 0;

    IniFile ini;
    if (!ini.load(name_map_ini)) return 0;

    int named = 0;
    for (const std::string& key : ini.keys("Templates")) {
        const std::string which = ini.get("Templates", key, "");
        const Frame* src = get(which);
        if (!src) continue;         // a name for a source we did not load from
        by_name_[key] = *src;
        ++named;
    }
    return named;
}

std::string TemplateStore::client_images() {
    // No executable-relative case here on purpose: a package carries that set
    // as its own `templates\`, which `load_best` finds before it ever asks
    // this. This is only for a machine that has the commercial client.
    const char* candidates[] = {
        "C:\\Program Files\\ZTeam\\GoC\\data\\images",
        "C:\\Program Files (x86)\\ZTeam\\GoC\\data\\images",
    };
    for (const char* c : candidates) {
        const DWORD attr = GetFileAttributesA(c);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            return c;
    }
    return {};
}

bool TemplateStore::load_best(const std::string& name_map_ini) {
    source_.clear();

    // Staged beside the executable, if the build put them there.
    const std::string staged = default_dir();
    if (GetFileAttributesA(staged.c_str()) != INVALID_FILE_ATTRIBUTES && load(staged)) {
        source_ = staged;
        apply_names(name_map_ini);
        return true;
    }

    // The commercial client's extracted set: named, and both skins.
    const std::string images = client_images();
    if (!images.empty() && load(images)) {
        source_ = images;
        apply_names(name_map_ini);
        return true;
    }

    // The helper's embedded set, unnamed and single-skinned.
    const std::string dll = helper_assembly();
    if (!dll.empty() && load_from_assembly(dll, name_map_ini)) {
        source_ = dll;
        return true;
    }

    GB_WARN("TemplateStore: no UI pictures anywhere; features that recognise "
            "dialogs will stay off");
    return false;
}

std::string TemplateStore::helper_assembly() {
    // Beside the executable first. This is the one file out of that whole
    // installation that this project reads -- the pictures are resources
    // inside it -- so a package carries the file rather than the install.
    const std::string beside = exe_relative("TungHaVGOBot.dll");
    if (GetFileAttributesA(beside.c_str()) != INVALID_FILE_ATTRIBUTES)
        return beside;

    const char* candidates[] = {
        "C:\\Program Files\\TH_VGO_Bot_Helper\\TungHaVGOBot.dll",
        "C:\\Program Files (x86)\\TH_VGO_Bot_Helper\\TungHaVGOBot.dll",
    };
    for (const char* c : candidates)
        if (GetFileAttributesA(c) != INVALID_FILE_ATTRIBUTES) return c;
    return {};
}

bool TemplateStore::load(const std::string& dir) {
    by_name_.clear();
    if (dir.empty()) return false;
    load_into(to_wide(dir), {}, by_name_);
    GB_INFO("TemplateStore: %zu templates from %s", by_name_.size(), dir.c_str());
    return !by_name_.empty();
}

bool TemplateMatcher::uses_opencv() {
#ifdef GOBOT_HAVE_OPENCV
    return true;
#else
    return false;
#endif
}

#ifndef GOBOT_HAVE_OPENCV
std::string TemplateMatcher::backend() {
    return "this project's own matcher (OpenCV was not built in)";
}
#endif

void TemplateStore::put(const std::string& name, const Frame& picture) {
    if (name.empty() || !picture.valid()) return;
    by_name_[name] = picture;
}

bool TemplateStore::has(const std::string& name) const {
    return by_name_.find(name) != by_name_.end();
}

const Frame* TemplateStore::get(const std::string& name) const {
    const auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : &it->second;
}

std::vector<std::string> TemplateStore::names() const {
    std::vector<std::string> out;
    out.reserve(by_name_.size());
    for (const auto& [name, _] : by_name_) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

TemplateMatcher::Match TemplateStore::find(const std::string& name, const Frame& frame,
                                           const ScreenRect& area,
                                           float min_score) const {
    const Frame* tpl = get(name);
    if (!tpl) {
        GB_WARN("TemplateStore: nothing called '%s' is loaded", name.c_str());
        return {};
    }
    return TemplateMatcher::find(frame, area, *tpl, min_score);
}

std::string TemplateStore::default_dir() {
    return exe_relative("templates");
}

} // namespace gobot
