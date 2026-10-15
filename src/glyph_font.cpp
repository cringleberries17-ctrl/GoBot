#include "gobot/glyph_font.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

namespace gobot {
namespace {

/// GDI+ started once and left running for the life of the process.
///
/// Only used to decode a few hundred tiny PNGs at startup; the alternative is
/// writing a PNG decoder, which is a lot of code to avoid one library that
/// ships with Windows.
struct GdiPlus {
    ULONG_PTR token = 0;
    bool ok = false;

    GdiPlus() {
        Gdiplus::GdiplusStartupInput input;
        ok = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
        if (!ok) GB_WARN("GDI+ would not start -- glyphs cannot be loaded");
    }
    ~GdiPlus() { if (ok) Gdiplus::GdiplusShutdown(token); }
};

GdiPlus& gdiplus() {
    static GdiPlus instance;
    return instance;
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

} // namespace


std::string GlyphFont::default_dir() {
    // Beside the executable first.
    //
    // Everything below this is a path on the machine this was built on, which
    // is no use to anybody it is sent to. Without a set of glyphs the figures
    // on the bars cannot be read, nor the chat, nor the list of players on the
    // map -- three features go quiet at once on a machine that has only the
    // game installed. Looking here first is what makes a folder you can copy
    // somewhere else work.
    const std::string beside = exe_relative("bitmap_font");
    if (is_folder(beside)) return beside;

    // Then where the commercial client keeps the sets it extracted from the
    // game. Read in place; nothing is copied out of there.
    //
    // It ships TWO, and the order here matters. data\images\bitmap_font holds
    // 183 glyphs; data\bitmap_font holds 179 and is missing F, J, W and
    // lowercase w -- precisely the Latin letters absent from Vietnamese
    // orthography, so whoever assembled it worked from Vietnamese text and
    // never noticed. Reading the short set turns "ExampleWin" into
    // "Exampley in", which is the misreading the loose name matching in
    // party_invite.cpp and player_list.cpp was built to absorb.
    //
    // Prefer the complete set. Keep the short one as a fallback: a machine
    // with only the older client installed still gets Vietnamese names right.
    const char* candidates[] = {
        "C:\\Program Files\\ZTeam\\GoC\\data\\images\\bitmap_font",
        "C:\\Program Files (x86)\\ZTeam\\GoC\\data\\images\\bitmap_font",
        "C:\\Program Files\\ZTeam\\GoC\\data\\bitmap_font",
        "C:\\Program Files (x86)\\ZTeam\\GoC\\data\\bitmap_font",
    };
    for (const char* c : candidates)
        if (is_folder(c)) return c;
    return {};
}

bool GlyphFont::load_dir(const std::wstring& dir) {
    WIN32_FIND_DATAW find{};
    HANDLE h = FindFirstFileW((dir + L"\\*.png").c_str(), &find);
    if (h == INVALID_HANDLE_VALUE) return false;

    int loaded = 0;
    do {
        if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        std::wstring name = find.cFileName;
        const std::size_t dot = name.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0) continue;
        name.resize(dot);                    // the file is named after the character

        Gdiplus::Bitmap bmp((dir + L"\\" + find.cFileName).c_str());
        if (bmp.GetLastStatus() != Gdiplus::Ok) continue;

        Glyph g;
        g.ch = to_utf8(name);
        g.w = static_cast<int>(bmp.GetWidth());
        g.h = static_cast<int>(bmp.GetHeight());
        if (g.w <= 0 || g.h <= 0 || g.w > 64 || g.h > 64) continue;

        g.ink.assign(static_cast<std::size_t>(g.w) * g.h, 0);
        for (int y = 0; y < g.h; ++y) {
            for (int x = 0; x < g.w; ++x) {
                Gdiplus::Color c;
                bmp.GetPixel(x, y, &c);
                // The glyph files themselves are white on black and fully
                // opaque, whatever colour the game later draws them in: the
                // drawn part is simply the bright part.
                if (c.GetA() >= 128 &&
                    level(c.GetR(), c.GetG(), c.GetB()) >= 128) {
                    g.ink[static_cast<std::size_t>(y) * g.w + x] = 1;
                    ++g.ink_count;
                }
            }
        }
        if (g.ink_count < 2) continue;       // a blank or a stray

        // The strokes, plus every pixel touching one. The rest of the box is
        // open space the game fills with whatever is behind the text, and
        // including it would measure the background rather than the letter.
        const int rw = g.w + 2, rh = g.h + 2;
        g.role.assign(static_cast<std::size_t>(rw) * rh, 0);
        for (int ry = 0; ry < rh; ++ry) {
            for (int rx = 0; rx < rw; ++rx) {
                const int gx = rx - 1, gy = ry - 1;
                const bool inside = gx >= 0 && gy >= 0 && gx < g.w && gy < g.h;
                if (inside && g.ink[static_cast<std::size_t>(gy) * g.w + gx]) {
                    g.role[static_cast<std::size_t>(ry) * rw + rx] = 1;
                    continue;
                }
                bool touches = false;
                for (int dy = -1; dy <= 1 && !touches; ++dy) {
                    for (int dx = -1; dx <= 1 && !touches; ++dx) {
                        const int nx = gx + dx, ny = gy + dy;
                        if (nx < 0 || ny < 0 || nx >= g.w || ny >= g.h) continue;
                        touches = g.ink[static_cast<std::size_t>(ny) * g.w + nx] != 0;
                    }
                }
                if (touches) g.role[static_cast<std::size_t>(ry) * rw + rx] = 2;
            }
        }
        for (std::uint8_t v : g.role) if (v) ++g.role_count;

        // Probes, spread evenly through the glyph's own ink so that they are
        // not all on the same stroke.
        {
            std::vector<std::pair<int, int>> ink_at;
            for (int gy = 0; gy < g.h; ++gy)
                for (int gx = 0; gx < g.w; ++gx)
                    if (g.ink[static_cast<std::size_t>(gy) * g.w + gx])
                        ink_at.emplace_back(gx, gy);

            const int want = std::min(4, static_cast<int>(ink_at.size()));
            for (int i = 0; i < want; ++i) {
                const auto& at = ink_at[(ink_at.size() - 1) * i / std::max(1, want - 1)];
                g.probe[i][0] = at.first;
                g.probe[i][1] = at.second;
            }
            g.probes = want;
        }

        tallest_ = std::max(tallest_, g.h);
        glyphs_.push_back(std::move(g));
        ++loaded;
    } while (FindNextFileW(h, &find));

    FindClose(h);
    return loaded > 0;
}

bool GlyphFont::load(const std::string& dir) {
    glyphs_.clear();
    tallest_ = 0;
    cap_height_ = 0;
    if (dir.empty() || !gdiplus().ok) return false;

    const std::wstring base = to_wide(dir);
    bool any = false;
    any |= load_dir(base + L"\\upper");
    any |= load_dir(base + L"\\lower");
    any |= load_dir(base);               // a flat directory works too

    for (const Glyph& g : glyphs_)
        if (g.ch.size() == 1 && g.ch[0] >= 'A' && g.ch[0] <= 'Z')
            cap_height_ = std::max(cap_height_, g.h);

    // Widest first. A narrow glyph is often a piece of a wide one -- 'i' sits
    // inside 'n' -- and trying the wide one first stops a name being read as
    // a string of stems.
    std::sort(glyphs_.begin(), glyphs_.end(), [](const Glyph& a, const Glyph& b) {
        if (a.w != b.w) return a.w > b.w;
        return a.ink_count > b.ink_count;
    });

    GB_INFO("GlyphFont: %zu glyphs from %s", glyphs_.size(), dir.c_str());
    return any;
}

/// The glyphs spelling a string, with nulls for spaces.
///
/// Vietnamese runs to three bytes a letter, so a character boundary is the
/// first byte that is not a continuation.
bool GlyphFont::spell(const std::string& utf8, std::vector<const Glyph*>& out) const {
    out.clear();
    for (std::size_t i = 0; i < utf8.size();) {
        std::size_t j = i + 1;
        while (j < utf8.size() && (utf8[j] & 0xC0) == 0x80) ++j;
        const std::string ch = utf8.substr(i, j - i);
        i = j;

        if (ch == " ") { out.push_back(nullptr); continue; }
        const auto it = std::find_if(glyphs_.begin(), glyphs_.end(),
                                     [&](const Glyph& g) { return g.ch == ch; });
        if (it == glyphs_.end()) return false;
        out.push_back(&*it);
    }
    return !out.empty();
}

bool GlyphFont::render_line(const std::string& utf8, Frame& out) const {
    if (glyphs_.empty() || utf8.empty()) return false;

    std::vector<const Glyph*> run;
    if (!spell(utf8, run)) return false;

    const int pad = 3;
    const int space = 3;
    int width = pad * 2, tall = 0;
    for (const Glyph* g : run) {
        width += g ? g->w + 1 : space;
        if (g) tall = std::max(tall, g->h);
    }

    out.width  = width;
    out.height = tall + pad * 2;
    out.pixels.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
    for (std::size_t i = 3; i < out.pixels.size(); i += 4) out.pixels[i] = 255;

    const int baseline = pad + tall;          // the row every glyph stands on
    int x = pad;
    for (const Glyph* g : run) {
        if (!g) { x += space; continue; }
        for (int gy = 0; gy < g->h; ++gy) {
            for (int gx = 0; gx < g->w; ++gx) {
                if (!g->ink[static_cast<std::size_t>(gy) * g->w + gx]) continue;
                const int sy = baseline - g->h + gy;
                const std::size_t i =
                    (static_cast<std::size_t>(sy) * out.width + (x + gx)) * 4;
                out.pixels[i] = out.pixels[i + 1] = out.pixels[i + 2] = 255;
            }
        }
        x += g->w + 1;
    }
    return true;
}

GlyphFont::Patch GlyphFont::patch_of(const Frame& f, const ScreenRect& r) {
    Patch p;
    if (!f.valid() || !r.valid()) return p;

    p.x0 = std::max(0, r.x);
    p.y0 = std::max(0, r.y);
    p.w  = std::min(f.width, r.right()) - p.x0;
    p.h  = std::min(f.height, r.bottom()) - p.y0;
    if (p.w <= 0 || p.h <= 0) { p.w = p.h = 0; return p; }

    // Brightness once. Everything above this is a lookup rather than a pixel
    // decode, which matters: the matcher reads every pixel of a region a few
    // thousand times over.
    p.level.resize(static_cast<std::size_t>(p.w) * p.h);
    for (int y = 0; y < p.h; ++y) {
        for (int x = 0; x < p.w; ++x) {
            std::uint8_t pr, pg, pb;
            f.get(p.x0 + x, p.y0 + y, pr, pg, pb);
            p.level[static_cast<std::size_t>(y) * p.w + x] = level(pr, pg, pb);
        }
    }

    // And which of them could be part of a letter. Reading a region tries
    // every glyph at every position in it, so this one pass over the pixels
    // pays for itself many times over: a glyph that touches no stroke at all
    // is dismissed without arithmetic.
    //
    // Read from the frame rather than from the level map above, so that the
    // rows just outside the region still count: a stroke on the top row of a
    // name plate is outlined by pixels above the plate.
    p.stroke.assign(static_cast<std::size_t>(p.w) * p.h, 0);
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x)
            p.stroke[static_cast<std::size_t>(y) * p.w + x] =
                looks_like_stroke(f, p.x0 + x, p.y0 + y) ? 1 : 0;
    return p;
}

bool GlyphFont::looks_like_stroke(const Frame& f, int x, int y) {
    std::uint8_t r, g, b;
    f.get(x, y, r, g, b);
    const int v = level(r, g, b);
    if (v < kMinInkLevel) return false;

    // Above or below, not both: the middle of a tall stem has neither, and a
    // stem is as much a part of a letter as the bar of a 'T'.
    f.get(x, y - 2, r, g, b);
    if (level(r, g, b) + kOutlineDrop < v) return true;
    f.get(x, y + 2, r, g, b);
    return level(r, g, b) + kOutlineDrop < v;
}

/// How well a glyph explains the pixels at one position, as a correlation
/// between the glyph's shape and what is on screen.
///
/// Not a comparison against a threshold: a threshold cannot be set once for
/// white name plates, a yellow map title and a half-transparent character
/// name, and setting it loosely turns every grey rock face into a page of
/// writing. Correlation asks a question with the same answer whatever colour
/// the text is and whatever it is drawn over -- does this shape, exactly, sit
/// here?
///
/// It has to be the shape and not merely the brightness. Asking only whether
/// the strokes are brighter than their surroundings lets a glyph one pixel
/// wide match any stroke at all, and a line of text comes back as a row of
/// l's: every narrow glyph fits inside every wide one, and being a perfect fit
/// for a fragment is not the same as being right. Correlation charges for the
/// bright pixels a glyph leaves unexplained, so covering the whole letter
/// beats covering a slice of it.
///
/// The one-pixel ring around the box is part of the template. The game
/// outlines its text in black, so the frame is where much of the evidence
/// lives -- and for a glyph whose box is solid ink it is the only evidence
/// there is.
GlyphFont::Fit GlyphFont::fit(const Glyph& g, const Patch& p, int px, int py) const {
    // Nearly every position a glyph is tried at is somewhere it is not. A
    // handful of lookups say so before any arithmetic happens: every probe
    // has to land on something bright, and at least one of them on something
    // that is actually shaped like a stroke rather than merely lit.
    bool any_stroke = false;
    for (int i = 0; i < g.probes; ++i) {
        const int sx = px + g.probe[i][0], sy = py + g.probe[i][1];
        if (sx < 0 || sy < 0 || sx >= p.w || sy >= p.h) continue;
        if (p.level[static_cast<std::size_t>(sy) * p.w + sx] < kProbeLevel) return Fit{};
        any_stroke = any_stroke || p.is_stroke(sx, sy);
    }
    if (!any_stroke) return Fit{};

    double sum_v = 0.0, sum_vv = 0.0, sum_tv = 0.0;
    int n = 0, n_on = 0, on = 0, off = 0;

    const int rw = g.w + 2;
    for (int ry = 0; ry < g.h + 2; ++ry) {
        const int sy = py - 1 + ry;
        if (sy < 0 || sy >= p.h) continue;
        const int* row = &p.level[static_cast<std::size_t>(sy) * p.w];
        const std::uint8_t* roles = &g.role[static_cast<std::size_t>(ry) * rw];

        for (int rx = 0; rx < rw; ++rx) {
            const int sx = px - 1 + rx;
            if (sx < 0 || sx >= p.w) continue;
            const std::uint8_t role = roles[rx];
            if (!role) continue;                    // open space: not evidence

            const double v = row[sx];
            sum_v  += v;
            sum_vv += v * v;
            sum_tv += role == 1 ? v : -v;
            ++n;
            if (role == 1) { ++n_on; on += row[sx]; } else { off += row[sx]; }
        }
    }
    if (n_on == 0 || n - n_on < 3) return Fit{};

    // Most of the template has to be on the patch at all. Without this a glyph
    // hanging off the edge is judged on the sliver that landed.
    if (n * 4 < g.role_count * 3) return Fit{};

    // Bright enough to be a stroke, and brighter than what hems it in.
    // Correlation alone is blind to contrast: a faint pattern in the right
    // shape scores as well as real writing.
    const int on_mean  = on / n_on;
    const int off_mean = off / (n - n_on);
    if (on_mean < kMinInkLevel) return Fit{};
    if (on_mean - off_mean < kMinSeparation) return Fit{};

    const double mean_v = sum_v / n;
    const double mean_t = static_cast<double>(2 * n_on - n) / n;
    const double var_t  = n - static_cast<double>(n) * mean_t * mean_t;
    const double var_v  = sum_vv - static_cast<double>(n) * mean_v * mean_v;
    if (var_t <= 0.0 || var_v <= 1.0) return Fit{};

    Fit out;
    const double cov = sum_tv - static_cast<double>(n) * mean_t * mean_v;
    out.correlation = cov / std::sqrt(var_t * var_v);
    if (out.correlation < kMinQuality) return Fit{};

    // Now count. The line between stroke and background is drawn halfway
    // between what this glyph claims each of them to be, so it moves with the
    // text's colour and opacity instead of being fixed once for writing that
    // is sometimes white, sometimes yellow and sometimes half see-through.
    const int mid = (on_mean + off_mean) / 2;
    for (int ry = 0; ry < g.h + 2; ++ry) {
        const int sy = py - 1 + ry;
        if (sy < 0 || sy >= p.h) continue;
        const int* row = &p.level[static_cast<std::size_t>(sy) * p.w];
        const std::uint8_t* roles = &g.role[static_cast<std::size_t>(ry) * rw];

        for (int rx = 0; rx < rw; ++rx) {
            const int sx = px - 1 + rx;
            if (sx < 0 || sx >= p.w) continue;
            const std::uint8_t role = roles[rx];
            if (!role) continue;

            const bool bright = row[sx] >= mid;
            if (role == 1) out.evidence += bright ? kStrokeFound : kStrokeInvented;
            else           out.evidence += bright ? kOutlineWrong : kOutlineRight;
        }
    }
    return out;
}

std::vector<GlyphFont::Placed> GlyphFont::candidates(const Frame& f, const ScreenRect& r,
                                                     std::size_t count) const {
    std::vector<Placed> out;
    const Patch p = patch_of(f, r);
    if (!p.valid()) return out;

    for (const Glyph& g : glyphs_) {
        if (g.w > p.w) continue;
        for (int py = 0; py + g.h <= p.h; ++py) {
            const Fit got = fit(g, p, 0, py);
            if (got.correlation < kMinQuality) continue;
            out.push_back(Placed{ ScreenRect{ p.x0, p.y0 + py, g.w, g.h }, g.ch,
                                  static_cast<float>(got.correlation),
                                  static_cast<float>(got.evidence) });
        }
    }
    // Ordered by what decides the read, not by what gates it.
    std::sort(out.begin(), out.end(), [](const Placed& a, const Placed& b) {
        return a.evidence > b.evidence;
    });
    if (out.size() > count) out.resize(count);
    return out;
}

void GlyphFont::runs(const Patch& p, const std::vector<const Glyph*>& want, std::size_t need,
                     const std::function<void(const Run&)>& each) const {
    const Glyph* first = nullptr;
    std::size_t first_at = 0, last_at = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (!want[i]) continue;
        if (!first) { first = want[i]; first_at = i; }
        last_at = i;
    }
    if (!first) return;

    Run run;
    for (int py = 0; py + first->h <= p.h; ++py) {
        for (int px = 0; px + first->w <= p.w; ++px) {
            // A higher bar than reading asks for. Reading has a region that is
            // known to hold writing and only has to say what it says; this is
            // looking at a whole screen, so a merely plausible first letter
            // starts a search in every bush.
            const Fit lead = fit(*first, p, px, py);
            if (lead.correlation < kMinLeadQuality) continue;

            // The first letter fits here. Walk the rest of the string from it,
            // left and right, on the baseline it implies.
            const int foot = py + first->h;
            run.left = px;
            run.right = px + first->w;
            run.top = py;
            run.foot = foot;
            run.hits = 1;
            run.score = lead.correlation;
            run.last_found = first_at == last_at;
            run.letters.clear();
            run.letters.push_back(Run::Letter{ first, px, py });

            const auto walk = [&](std::size_t from, int step, int start_edge) {
                int at = start_edge;
                int missed_in_a_row = 0;
                for (std::size_t k = from; k < want.size(); k = static_cast<std::size_t>(
                         static_cast<std::ptrdiff_t>(k) + step)) {
                    const Glyph* g = want[k];
                    if (!g) { at += step > 0 ? kSpaceGap : -kSpaceGap; continue; }

                    // Letters sit a pixel apart, but a plate drawn over
                    // scenery can shift one either way; try the neighbours.
                    //
                    // And a little below the foot, as reading does: the files
                    // are cropped to their own ink, so a "g", a "y" or a
                    // dot-below vowel ends lower than the letters beside it
                    // while sharing their baseline. Stood on the foot, such a
                    // letter was never found, and a name ending in one --
                    // "Long", "Huy" -- could never be found whole.
                    double got = 0.0;
                    int landed = at, got_x = at, got_y = 0;
                    for (int drop = 0; drop <= kMaxDescent; ++drop) {
                        const int gy = foot - g->h + drop;
                        if (gy < 0 || gy + g->h > p.h) continue;
                        for (int slack = 0; slack <= 2; ++slack) {
                            const int gx = step > 0 ? at + slack - 1
                                                    : at - g->w - slack + 1;
                            if (gx < 0 || gx + g->w > p.w) continue;
                            const Fit c = fit(*g, p, gx, gy);
                            if (c.correlation <= got) continue;
                            got = c.correlation;
                            got_x = gx;
                            got_y = gy;
                            landed = step > 0 ? gx + g->w : gx;
                        }
                    }
                    if (got < kMinQuality) {
                        // A letter or two can be eaten by a flower or a
                        // casting effect and the name is still there. A run of
                        // them is not a name with gaps in it, it is somewhere
                        // else that happens to start the same way -- which is
                        // how a character's name was once found in the quest
                        // log, six letters of it picked out of unrelated text.
                        if (++missed_in_a_row > 2) return;
                        at += step * (g->w + 1);
                        continue;
                    }
                    missed_in_a_row = 0;
                    run.score += got;
                    ++run.hits;
                    if (k == last_at) run.last_found = true;
                    run.letters.push_back(Run::Letter{ g, got_x, got_y });
                    at = landed + (step > 0 ? 1 : -1);
                    if (step > 0) run.right = std::max(run.right, landed);
                    else          run.left  = std::min(run.left, landed);
                }
            };
            walk(first_at + 1, +1, px + first->w + 1);
            if (first_at > 0) walk(first_at - 1, -1, px - 1);

            if (run.hits < need) continue;
            each(run);
        }
    }
}

bool GlyphFont::find_text(const Frame& f, const ScreenRect& area,
                          const std::string& utf8, ScreenRect& where,
                          float* confidence) const {
    if (confidence) *confidence = 0.0f;
    if (glyphs_.empty() || utf8.empty()) return false;

    std::vector<const Glyph*> want;
    if (!spell(utf8, want)) return false;

    // How many letters have to be found, counting from the whole string.
    const std::size_t letters = static_cast<std::size_t>(
        std::count_if(want.begin(), want.end(), [](const Glyph* g) { return g != nullptr; }));
    if (letters == 0) return false;
    const std::size_t need = std::max<std::size_t>(
        2, static_cast<std::size_t>(letters * kMinFoundFraction));

    const Patch p = patch_of(f, area);
    if (!p.valid()) return false;

    double best_score = 0.0;
    bool found = false;
    runs(p, want, need, [&](const Run& r) {
        if (r.score <= best_score) return;
        best_score = r.score;
        found = true;
        where = ScreenRect{ p.x0 + r.left, p.y0 + r.top, r.right - r.left, r.foot - r.top };
        if (confidence)
            *confidence = static_cast<float>(r.score / static_cast<double>(letters));
    });
    return found;
}

bool GlyphFont::alone(const Frame& f, const Patch& p, const Run& r, int line_h) const {
    // The colour the name is written in, from its own strokes. Measured, not
    // assumed: names are white, yellow or green, and drawn half-transparent
    // over whatever is behind them.
    long sum_r = 0, sum_g = 0, sum_b = 0;
    int inked = 0;
    for (const Run::Letter& l : r.letters)
        for (int gy = 0; gy < l.g->h; ++gy)
            for (int gx = 0; gx < l.g->w; ++gx) {
                if (!l.g->ink[static_cast<std::size_t>(gy) * l.g->w + gx]) continue;
                std::uint8_t cr, cg, cb;
                f.get(p.x0 + l.x + gx, p.y0 + l.y + gy, cr, cg, cb);
                if (level(cr, cg, cb) < kMinInkLevel) continue;
                sum_r += cr; sum_g += cg; sum_b += cb;
                ++inked;
            }
    if (inked == 0) return false;      // no stroke of its own to compare with
    const int ink_r = static_cast<int>(sum_r / inked);
    const int ink_g = static_cast<int>(sum_g / inked);
    const int ink_b = static_cast<int>(sum_b / inked);

    // The line: from a space's width before the first letter to a space's
    // width after the last, and from the baseline as high as letters reach.
    // A letter closer than a space is part of the same word -- "Tuan" before
    // "Anh", or the "2" after "KhiCon" -- and one further off is another word.
    //
    // And the marks Vietnamese puts over and under a letter, so that "Ánh"
    // is not taken for "Anh": in each letter's own columns, as far above it
    // as an accent reaches and as far below the line as a dot. Not right
    // across the line, and not over a letter that carries its own accent:
    // higher up is whatever the game draws above a name.
    constexpr int kMarkAbove = 3;      // an accent's two rows, and the gap under it
    constexpr int kMarkBelow = 2;      // a dot's row, and the gap over it
    const int foot = p.y0 + r.foot;
    const int band_top = foot - line_h;
    const int x0 = std::max(0, p.x0 + r.left - kSpaceGap);
    const int x1 = std::min(f.width, p.x0 + r.right + kSpaceGap);
    const int y0 = std::max(0, band_top - kMarkAbove);
    const int y1 = std::min(f.height, foot + kMarkBelow);
    if (x1 <= x0 || y1 <= y0 || foot <= y0) return false;
    const int bw = x1 - x0, bh = y1 - y0;

    std::vector<std::uint8_t> look(static_cast<std::size_t>(bw) * bh, 0);
    const auto mark = [&](int from_x, int to_x, int from_y, int to_y) {
        for (int y = std::max(y0, from_y); y < std::min(y1, to_y); ++y)
            for (int x = std::max(x0, from_x); x < std::min(x1, to_x); ++x)
                look[static_cast<std::size_t>(y - y0) * bw + (x - x0)] = 1;
    };
    mark(x0, x1, band_top, foot);
    for (const Run::Letter& l : r.letters) {
        const int top = p.y0 + l.y;
        const int from = l.g->h <= cap_height_ ? top - kMarkAbove : top;
        mark(p.x0 + l.x, p.x0 + l.x + l.g->w, from, y1);
    }

    // What the letters found account for: their strokes, and a pixel round
    // each in case one sits a pixel off.
    std::vector<std::uint8_t> own(static_cast<std::size_t>(bw) * bh, 0);
    for (const Run::Letter& l : r.letters)
        for (int gy = 0; gy < l.g->h; ++gy)
            for (int gx = 0; gx < l.g->w; ++gx) {
                if (!l.g->ink[static_cast<std::size_t>(gy) * l.g->w + gx]) continue;
                const int fx = p.x0 + l.x + gx, fy = p.y0 + l.y + gy;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int bx = fx + dx - x0, by = fy + dy - y0;
                        if (bx >= 0 && by >= 0 && bx < bw && by < bh)
                            own[static_cast<std::size_t>(by) * bw + bx] = 1;
                    }
            }

    // Anything else written in the name's colour there makes it part of some
    // other writing: more letters of a longer name, another letter where one
    // of this name's was not found, or a mark this name does not have. A glow
    // or a pet over a letter is another colour, or no stroke at all, and does
    // not count.
    int stray = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const std::size_t at = static_cast<std::size_t>(y - y0) * bw + (x - x0);
            if (!look[at] || own[at]) continue;
            if (!looks_like_stroke(f, x, y)) continue;
            std::uint8_t cr, cg, cb;
            f.get(x, y, cr, cg, cb);
            if (std::abs(cr - ink_r) > kSameInk || std::abs(cg - ink_g) > kSameInk ||
                std::abs(cb - ink_b) > kSameInk)
                continue;
            if (++stray >= kStrayInk) return false;
        }
    return true;
}

std::vector<GlyphFont::Spot> GlyphFont::find_whole(const Frame& f, const ScreenRect& area,
                                                   const std::string& utf8) const {
    std::vector<Spot> out;
    if (glyphs_.empty() || utf8.empty()) return out;

    std::vector<const Glyph*> want;
    if (!spell(utf8, want)) return out;

    // As many letters as find_text asks for: a letter may be eaten. Which
    // letters, and what stands where one is missing, is alone()'s question.
    const std::size_t letters = static_cast<std::size_t>(
        std::count_if(want.begin(), want.end(), [](const Glyph* g) { return g != nullptr; }));
    if (letters == 0) return out;
    const std::size_t need = std::max<std::size_t>(
        2, static_cast<std::size_t>(letters * kMinFoundFraction));

    // As high as the name's own letters reach, and at least as high as a
    // capital: a name of small letters must still see the stem of a capital
    // written right before it.
    int line_h = cap_height_;
    for (const Glyph* g : want)
        if (g) line_h = std::max(line_h, g->h);

    const Patch p = patch_of(f, area);
    if (!p.valid()) return out;

    runs(p, want, need, [&](const Run& r) {
        // The last letter found, as the first always is. A name a letter
        // short at its end is a shorter name -- "KhiCon" when "KhiCon2" is
        // sought -- and nothing on the line beyond it can say otherwise.
        if (!r.last_found) return;
        if (!alone(f, p, r, line_h)) return;

        Spot s;
        s.where = ScreenRect{ p.x0 + r.left, p.y0 + r.top, r.right - r.left, r.foot - r.top };
        s.sure = static_cast<float>(r.score / static_cast<double>(letters));

        // The same writing is met again from the starting points beside it.
        // Keep its best read, once.
        bool beaten = false;
        for (const Spot& o : out)
            if (o.where.overlaps(s.where) && o.sure >= s.sure) { beaten = true; break; }
        if (beaten) return;
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&](const Spot& o) { return o.where.overlaps(s.where); }),
                  out.end());
        out.push_back(s);
    });
    return out;
}

std::string GlyphFont::read_line(const Frame& f, const ScreenRect& r) const {
    last_confidence_ = 0.0f;
    last_placed_.clear();
    if (glyphs_.empty() || !f.valid() || !r.valid()) return {};

    const Patch p = patch_of(f, r);
    if (!p.valid()) return {};
    const int w = p.w, h = p.h, x0 = p.x0, y0 = p.y0;

    // Where the line stands, measured rather than searched for.
    //
    // Letters on one line share a baseline, so the row most of them end on is
    // the baseline -- no matching required, just the lowest lit pixel in each
    // column and a tally. Pinning the search to it is what keeps a stray glyph
    // from floating at whatever height happens to fit, and finding it this way
    // rather than by trying every height is most of what makes reading a name
    // fast enough to do while the game is running.
    //
    // Rows lit nearly all the way across are left out of the tally. Writing is
    // sparse; a panel border, a health bar or a stretch of sky is not, and any
    // of them would otherwise outvote the text sitting above it.
    std::vector<int> feet(static_cast<std::size_t>(h) + 2, 0);
    {
        std::vector<std::uint8_t> row_is_text(static_cast<std::size_t>(h), 1);
        for (int y = 0; y < h; ++y) {
            int lit = 0;
            for (int x = 0; x < w; ++x) if (p.is_stroke(x, y)) ++lit;
            row_is_text[static_cast<std::size_t>(y)] = (lit * 10 <= w * 8) ? 1 : 0;
        }
        for (int x = 0; x < w; ++x) {
            for (int y = h - 1; y >= 0; --y) {
                if (!row_is_text[static_cast<std::size_t>(y)]) continue;
                if (!p.is_stroke(x, y)) continue;
                ++feet[static_cast<std::size_t>(y) + 1];
                break;
            }
        }
    }

    // The three most popular, because the most popular can be a row of
    // descenders or the underside of a line of accents.
    std::vector<int> bases;
    for (int round = 0; round < 3; ++round) {
        int pick = -1, most = 1;
        for (int i = 0; i < static_cast<int>(feet.size()); ++i)
            if (feet[i] > most) { most = feet[i]; pick = i; }
        if (pick < 0) break;
        bases.push_back(pick);
        feet[static_cast<std::size_t>(pick)] = 0;
    }
    if (bases.empty()) return {};

    // Second pass, pinned to that baseline -- and solved as one problem rather
    // than one letter at a time.
    //
    // Reading greedily from the left goes wrong at the first mediocre match:
    // it is taken, the scan jumps to the far side of it, and every letter
    // after that is a pixel out of step. The damage is not local, which is why
    // a line can come back with its last word right and its first unreadable.
    // Choosing the set of glyphs that together explain the most of the line
    // has no such failure -- a better match one column over simply wins, and
    // nothing earlier can commit the reader to missing it.
    struct Step {
        const Glyph* g = nullptr;
        int    py = 0;
        double q = 0.0;
        int    next = 0;
    };
    std::vector<double> best(static_cast<std::size_t>(w) + 1, 0.0);
    std::vector<Step>   step(static_cast<std::size_t>(w) + 1);
    std::vector<Step>   chosen;
    double chosen_total = 0.0;

    for (int base : bases) {
        std::fill(best.begin(), best.end(), 0.0);

        for (int x = w - 1; x >= 0; --x) {
            best[x] = best[x + 1];
            step[x] = Step{ nullptr, 0, 0.0, x + 1 };

            for (const Glyph& g : glyphs_) {
                if (x + g.w > w) continue;
                const int lo = std::max(0, base - g.h);
                const int hi = std::min(h - g.h, base - g.h + kMaxDescent);
                for (int py = lo; py <= hi; ++py) {
                    const Fit got = fit(g, p, x, py);
                    if (got.evidence <= 0.0) continue;

                    const double total = got.evidence + best[x + g.w];
                    if (total > best[x])
                        best[x] = total,
                        step[x] = Step{ &g, py, got.correlation, x + g.w };
                }
            }
        }
        if (best[0] <= chosen_total) continue;
        chosen_total = best[0];
        chosen = step;
    }
    if (chosen.empty()) return {};
    step = std::move(chosen);

    std::string out;
    double q_total = 0.0;
    int matched = 0, covered = 0, unread = 0, last_end = -1;

    for (int x = 0; x < w;) {
        const Step& s = step[x];
        if (!s.g) { ++x; continue; }

        if (matched > 0) {
            const int gap = x - last_end;
            // One blank column is ordinary letter spacing and a wide one is a
            // word break; anything between is a stretch of the line that no
            // glyph accounted for, and that is what should count against the
            // answer.
            if (gap >= kSpaceGap) out += ' ';
            else                  unread += std::max(0, gap - 1);
        }

        out += s.g->ch;
        last_placed_.push_back(Placed{
            ScreenRect{ x0 + x, y0 + s.py, s.g->w, s.g->h },
            s.g->ch, static_cast<float>(s.q), 0.0f });
        q_total += s.q;
        ++matched;
        covered += s.g->w;
        last_end = s.next;
        x = s.next;
    }
    if (matched == 0) return {};

    // Two things make a read trustworthy: each glyph fitted well, and together
    // they account for the line rather than picking a few letters out of noise.
    const double fit = q_total / matched;
    const double coverage = static_cast<double>(covered) / (covered + unread);

    last_confidence_ = static_cast<float>(std::max(0.0, fit) * coverage);
    return out;
}

} // namespace gobot
