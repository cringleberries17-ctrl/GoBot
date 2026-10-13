#include "gobot/bag.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <set>
#include <thread>

#include <windows.h>

namespace gobot {

namespace {

// The pictures this goes by, under the names the template store gives them.
//
// The strip as the Vietnamese client draws it. The commercial client's own
// picture of the window has page numbers where this has tab names, so it is
// no use for finding the strip, only for measuring the window -- which is
// where the layout's numbers came from.
const char* const kStrips[]    = { "grabbed/inventory", "inventory" };
const char* const kTrashCans[] = { "inven/0/trash_can", "inven/0/trash_can1",
                                   "inven/1/trash_can", "inven/1/trash_can1" };

/// [Xác Nhận][Hủy Bỏ], the pair under every question the game asks.
const char* const kConfirm = "grabbed/confirm_button";

/// A message with only Xác Nhận under it: what the client says when it
/// refuses something. Its picture has no words, so it is only ever reported,
/// never answered.
const char* const kNotice = "grabbed/dialog_ok";

/// The bin's two dialogs, and where the pair of buttons sits in each picture.
///
/// The question for one item, and the quantity for a stack -- which a single
/// item of a kind that stacks gets as well. The game fills the box in itself
/// with the whole stack: 1 for a single piece, 2 for a stack of two, seen
/// live. The question's picture draws its two buttons a pixel closer together
/// than the quantity's does, so the pair is looked for in both drawings; the
/// quantity's picture stops above its buttons, and where they sit was measured
/// on the live client, which matched that picture at 1.000.
struct DialogPicture {
    const char* name;
    int         pair_dx, pair_dy;   ///< the pair, from the picture's top-left
    bool        quantity;
};
const DialogPicture kBinDialogs[] = {
    { "grabbed/del_item_confirm_1", 101, 51, false },   // bag::kAskDialog
    { "grabbed/del_item_confirm_2", 100, 50, true },    // bag::kCountDialog
};

/// The dark panel of words in those pictures, from their top-left. Every
/// dialog's frame and buttons are the same, so the words are what tells them
/// apart: matched here, the bin's question scores 0.24 at most against any
/// other dialog's words and 1.000 against itself on the live client.
constexpr int kWordsX = 20, kWordsY = 12, kWordsW = 158, kWordsH = 28;
/// Bright pixels a panel needs to be words at all. The pictures of the drop
/// question and of the notice have an empty panel -- ten or so -- and would
/// pass for any dialog.
constexpr int kWordsAtLeast = 100;
constexpr float kWordsScore = 0.90f;

/// How alike the strip has to be. Lower than a fixed button: whichever tab is
/// selected is drawn differently, and the picture was taken with the first.
constexpr float kStripScore = 0.62f;
constexpr float kTrashScore = 0.80f;
constexpr float kButtonScore = 0.82f;

/// The quantity box, from the pair of buttons: left of them, a little inset.
constexpr int kBoxDx = -88, kBoxDy = 3, kBoxW = 82;

/// How long the quantity box is watched before its number is taken, and how
/// often it is looked at meanwhile: a whole blink of its caret, which is on
/// for about half a second and off for about as long, with some to spare.
constexpr int kBoxWatchMs = 1200;
constexpr int kBoxLookMs = 150;

/// How alike a slot has to stay to what it held before the drag for the item
/// to count as still there.
constexpr float kStillThere = 0.80f;

ScreenRect clamp_to(const Frame& f, ScreenRect r) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(f.width, r.x + r.w), y1 = std::min(f.height, r.y + r.h);
    return ScreenRect{ x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0) };
}

int lum(const Frame& f, int x, int y) {
    std::uint8_t r, g, b;
    f.get(x, y, r, g, b);
    return (r + g + b) / 3;
}

std::string stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
    return buf;
}

/// Part of a picture, as it is.
Frame crop(const Frame& f, const ScreenRect& r) {
    Frame out;
    if (!f.valid() || r.x < 0 || r.y < 0 || r.right() > f.width || r.bottom() > f.height ||
        !r.valid())
        return out;
    out.width = r.w;
    out.height = r.h;
    out.pixels.resize(static_cast<std::size_t>(r.w) * r.h * 4);
    for (int y = 0; y < r.h; ++y)
        std::copy_n(f.pixels.begin() + ((static_cast<std::size_t>(r.y + y) * f.width + r.x) * 4),
                    static_cast<std::size_t>(r.w) * 4,
                    out.pixels.begin() + static_cast<std::size_t>(y) * r.w * 4);
    return out;
}

/// Two found rectangles that are one thing, found twice.
bool same_place(const ScreenRect& a, const ScreenRect& b) {
    return std::abs(a.x - b.x) <= 3 && std::abs(a.y - b.y) <= 3;
}

} // namespace

// ----------------------------------------------------------------- layout

void BagLayout::load(const IniFile& ini) {
    tab_w           = ini.get_int("Bag", "tab_w", tab_w);
    cell            = ini.get_int("Bag", "cell", cell);
    pitch           = ini.get_int("Bag", "pitch", pitch);
    grid_dx         = ini.get_int("Bag", "grid_dx", grid_dx);
    grid_dy         = ini.get_int("Bag", "grid_dy", grid_dy);
    cols            = std::clamp(ini.get_int("Bag", "cols", cols), 1, 12);
    rows            = std::clamp(ini.get_int("Bag", "rows", rows), 1, 12);
    scroll_up_dx    = ini.get_int("Bag", "scroll_up_dx", scroll_up_dx);
    scroll_up_dy    = ini.get_int("Bag", "scroll_up_dy", scroll_up_dy);
    scroll_down_dx  = ini.get_int("Bag", "scroll_down_dx", scroll_down_dx);
    scroll_down_dy  = ini.get_int("Bag", "scroll_down_dy", scroll_down_dy);
    track_top_dy    = ini.get_int("Bag", "track_top_dy", track_top_dy);
    track_bottom_dy = ini.get_int("Bag", "track_bottom_dy", track_bottom_dy);
    bin_dx          = ini.get_int("Bag", "bin_dx", bin_dx);
    bin_dy          = ini.get_int("Bag", "bin_dy", bin_dy);
    bin_w           = ini.get_int("Bag", "bin_w", bin_w);
    bin_h           = ini.get_int("Bag", "bin_h", bin_h);
    count_w         = ini.get_int("Bag", "count_w", count_w);
    count_h         = ini.get_int("Bag", "count_h", count_h);
    edge            = ini.get_int("Bag", "edge", edge);
}

const char* bag_tab_name(int tab) {
    switch (tab) {
        case 0: return "Trang Bị 1";
        case 1: return "Trang Bị 2";
        case 2: return "Tiêu Dùng";
        case 3: return "Khác";
        case 4: return "Linh Thú";
        default: return "?";
    }
}

ScreenRect BagView::slot(const BagLayout& l, int col, int row) const {
    return ScreenRect{ strip.x + l.grid_dx + col * l.pitch,
                       strip.y + l.grid_dy + row * l.pitch, l.cell, l.cell };
}

std::vector<ScreenRect> BagView::slots(const BagLayout& l) const {
    const int rows = slot_rows < 0 ? l.rows : std::min(slot_rows, l.rows);
    std::vector<ScreenRect> out;
    out.reserve(static_cast<std::size_t>(l.cols * rows));
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < l.cols; ++c) out.push_back(slot(l, c, r));
    return out;
}

void BagView::tab_point(const BagLayout& l, int tab, int& x, int& y) const {
    x = strip.x + tab * l.tab_w + l.tab_w / 2;
    y = strip.y + strip.h / 2;
}

// ---------------------------------------------------------------- looking

namespace bag {

BagView find(const Frame& f, const TemplateStore& store, const BagLayout& l) {
    BagView v;
    if (!f.valid()) return v;

    const ScreenRect all{ 0, 0, f.width, f.height };
    for (const char* name : kStrips) {
        const Frame* t = store.get(name);
        if (t == nullptr) continue;
        const auto m = TemplateMatcher::find(f, all, *t, kStripScore);
        if (m.found && m.score > v.score) {
            v.found = true;
            v.score = m.score;
            v.strip = m.at;
        }
        if (v.found) break;           // the two names are one picture
    }
    if (!v.found) return v;

    // The bin is where the layout says. The can is only drawn in it while it
    // is empty, so looking for the can says whether it is -- looked for right
    // there and nowhere else.
    v.trash = ScreenRect{ v.strip.x + l.bin_dx, v.strip.y + l.bin_dy, l.bin_w, l.bin_h };
    const ScreenRect around = clamp_to(f, ScreenRect{ v.trash.x - 4, v.trash.y - 4,
                                                      v.trash.w + 8, v.trash.h + 8 });
    for (const char* name : kTrashCans) {
        const Frame* t = store.get(name);
        if (t == nullptr || around.w < t->width || around.h < t->height) continue;
        const auto m = TemplateMatcher::find(f, around, *t, kTrashScore);
        if (m.found) {
            v.bin_empty = true;
            v.trash = m.at;
            break;
        }
    }

    // Which rows on show are slots. Each slot has a frame, and the frame
    // between two slots is one flat colour down its length; the row with the
    // button for buying more room is one panel across the whole width, and a
    // tooltip over a row breaks its frames up too. Rows after the first that
    // is not slots are left out with it.
    int rows = 0;
    for (int r = 0; r < l.rows; ++r) {
        int framed = 0;
        for (int c = 0; c + 1 < l.cols; ++c) {
            const ScreenRect s = v.slot(l, c, r);
            const int x = s.x + l.cell + 1;      // the right side of this slot's frame
            int lo = 255, hi = 0, n = 0;
            for (int y = s.y + 6; y < s.y + l.cell - 6; ++y) {
                const int p = lum(f, x, y);
                lo = std::min(lo, p);
                hi = std::max(hi, p);
                ++n;
            }
            if (n > 0 && hi - lo <= 8) ++framed;
        }
        if (l.cols > 1 && framed * 4 < (l.cols - 1) * 3) break;
        ++rows;
    }
    v.slot_rows = rows;

    // The scroll bar's thumb: the white run down the track. Everything else
    // in the track is its dark middle or its two lighter ends; anything that
    // is neither is something drawn over it, and then it says nothing.
    const int tx = v.strip.x + l.scroll_up_dx;
    const int top = v.strip.y + l.track_top_dy, bottom = v.strip.y + l.track_bottom_dy;
    int run_top = -1, run_len = 0, cur_top = -1, cur_len = 0;
    bool clear = top >= 0 && bottom < f.height && tx >= 0 && tx < f.width && bottom > top;
    for (int y = top; clear && y <= bottom; ++y) {
        const int p = lum(f, tx, y);
        if (p >= 250) {
            if (cur_len == 0) cur_top = y;
            if (++cur_len > run_len) {
                run_len = cur_len;
                run_top = cur_top;
            }
        } else {
            cur_len = 0;
            if (p < 40 || (p > 110 && p < 240)) clear = false;
        }
    }
    const int span = bottom - top + 1;
    if (clear && run_len >= 6) {
        if (run_len >= span - 1) {
            v.total_rows = l.rows;
            v.first_row = 0;
        } else {
            v.total_rows = std::max(l.rows, static_cast<int>(std::lround(
                               static_cast<double>(l.rows) * span / run_len)));
            const int travel = span - run_len;
            v.first_row = v.total_rows > l.rows
                ? static_cast<int>(std::lround(static_cast<double>(run_top - top) *
                                               (v.total_rows - l.rows) / travel))
                : 0;
            v.first_row = std::clamp(v.first_row, 0, v.total_rows - l.rows);
        }
    }
    return v;
}

Frame cut(const Frame& f, const ScreenRect& r) {
    Frame out;
    if (!f.valid() || !r.valid()) return out;
    out.width = r.w;
    out.height = r.h;
    out.pixels.assign(static_cast<std::size_t>(r.w) * r.h * 4, 0);
    for (int y = 0; y < r.h; ++y) {
        for (int x = 0; x < r.w; ++x) {
            std::uint8_t cr, cg, cb;
            f.get(r.x + x, r.y + y, cr, cg, cb);
            const std::size_t i = (static_cast<std::size_t>(y) * r.w + x) * 4;
            out.pixels[i] = cb;
            out.pixels[i + 1] = cg;
            out.pixels[i + 2] = cr;
            out.pixels[i + 3] = 255;
        }
    }
    return out;
}

Frame masked(const Frame& picture, const BagLayout& l) {
    Frame out = picture;
    if (!out.valid()) return out;
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const bool at_edge = x < l.edge || y < l.edge ||
                                 x >= out.width - l.edge || y >= out.height - l.edge;
            const bool in_count = x < l.count_w && y < l.count_h;
            out.pixels[(static_cast<std::size_t>(y) * out.width + x) * 4 + 3] =
                (at_edge || in_count) ? 0 : 255;
        }
    }
    return out;
}

bool is_empty(const Frame& f, const ScreenRect& r, const BagLayout& l) {
    // The brightest few pixels decide it, not the average: an item drawn small
    // in the middle of a dark slot averages out dark too.
    int counts[256] = {};
    int n = 0;
    for (int y = r.y + l.edge + 1; y < r.y + r.h - l.edge - 1; ++y)
        for (int x = r.x + l.edge + 1; x < r.x + r.w - l.edge - 1; ++x) {
            ++counts[lum(f, x, y)];
            ++n;
        }
    if (n == 0) return true;
    int seen = 0;
    for (int v = 255; v >= 0; --v) {
        seen += counts[v];
        if (seen * 50 >= n) return v < 70;      // the 98th percentile
    }
    return true;
}

std::uint64_t grid_hash(const Frame& f, const BagView& v, const BagLayout& l) {
    std::uint64_t h = 1469598103934665603ull;
    for (const ScreenRect& s : v.slots(l)) {
        for (int y = s.y + 3; y < s.y + s.h - 3; y += 3)
            for (int x = s.x + 3; x < s.x + s.w - 3; x += 3) {
                h ^= static_cast<std::uint64_t>(lum(f, x, y) / 8);
                h *= 1099511628211ull;
            }
    }
    return h;
}

bool same_page(const Frame& a, const BagView& va, const Frame& b, const BagView& vb,
               const BagLayout& l) {
    const auto sa = va.slots(l), sb = vb.slots(l);
    if (sa.size() != sb.size() || sa.empty()) return false;
    for (std::size_t i = 0; i < sa.size(); ++i) {
        // The middle of each slot, coarsely: enough to tell items apart, too
        // coarse to notice a glint or the highlight round the slot under the
        // pointer. Every slot has to agree: one ▼ click moves two or three
        // rows, and a page with two items on it differs from the empty page
        // below it in two slots only.
        long da = 0, db = 0;
        int n = 0;
        for (int y = 8; y < l.cell - 8; y += 2)
            for (int x = 8; x < l.cell - 8; x += 2) {
                da += lum(a, sa[i].x + x, sa[i].y + y);
                db += lum(b, sb[i].x + x, sb[i].y + y);
                ++n;
            }
        if (n == 0 || std::abs(da - db) / n > 6) return false;
    }
    return true;
}

float alike(const Frame& a, const Frame& b, const BagLayout& l) {
    if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height) return -1.0f;
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    int n = 0;
    for (int y = l.edge; y < a.height - l.edge; ++y)
        for (int x = l.edge; x < a.width - l.edge; ++x) {
            if (x < l.count_w && y < l.count_h) continue;    // the count changes
            const double pa = lum(a, x, y), pb = lum(b, x, y);
            sa += pa; sb += pb; saa += pa * pa; sbb += pb * pb; sab += pa * pb;
            ++n;
        }
    if (n < 24) return -1.0f;
    const double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
    if (va < n * 4.0 || vb < n * 4.0) return -1.0f;       // flat: nothing to compare
    return static_cast<float>((sab - sa * sb / n) / std::sqrt(va * vb));
}

} // namespace bag

// ---------------------------------------------------------------- counts

bool CountReader::load(const std::string& font_dir) {
    loaded_ = false;
    int got = 0;
    for (int d = 0; d < 10; ++d) {
        Frame f;
        if (!load_png(font_dir + "\\lower\\" + std::to_string(d) + ".png", f) || !f.valid())
            continue;
        std::vector<std::uint8_t> on(static_cast<std::size_t>(f.width) * f.height, 0);
        for (int y = 0; y < f.height; ++y)
            for (int x = 0; x < f.width; ++x)
                on[static_cast<std::size_t>(y) * f.width + x] = lum(f, x, y) > 128 ? 1 : 0;
        set_digit(d, f.width, f.height, on);
        ++got;
    }
    loaded_ = got == 10;
    return loaded_;
}

void CountReader::set_digit(int d, int w, int h, const std::vector<std::uint8_t>& on) {
    if (d < 0 || d > 9 || w <= 0 || h <= 0 ||
        on.size() != static_cast<std::size_t>(w) * h) return;
    Glyph& g = digits_[d];
    g.w = w;
    g.h = h;
    g.on = on;
    g.lit = 0;
    for (const auto v : on) g.lit += v ? 1 : 0;
    loaded_ = true;
    for (const Glyph& each : digits_)
        if (each.w == 0) loaded_ = false;
}

int CountReader::scan(const Frame& f, const ScreenRect& area, int max_top, bool caret,
                      int& white, int& stray) const {
    white = stray = 0;
    const int kW = area.w, kH = area.h;
    if (!f.valid() || kW <= 0 || kH <= 0) return -1;

    // The digits are white and the item under them is not: bright and grey
    // picks out the one from the other, even over a white potion, because
    // the digits carry a dark outline that keeps them apart.
    //
    // A quantity box's caret is yellow -- 200 to 255 red and green, 55 blue
    // at most, brighter where a stroke is under it -- so it is never taken
    // for a 1. While it is on it covers the last column of a digit five
    // pixels wide, and counted as ground it cost a 2 three of its nineteen
    // strokes, so that the game's own count in the box did not read. In a box
    // its pixels are unknown instead: left out of the comparison either way.
    // Not in a slot's corner, where yellow is the item's own.
    enum : std::uint8_t { kGround = 0, kStroke = 1, kUnknown = 2 };
    std::vector<std::uint8_t> map(static_cast<std::size_t>(kW) * kH, kGround);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            std::uint8_t r, g, b;
            f.get(area.x + x, area.y + y, r, g, b);
            const int lo = std::min({ r, g, b }), hi = std::max({ r, g, b });
            std::uint8_t& m = map[static_cast<std::size_t>(y) * kW + x];
            if ((r + g + b) / 3 > 175 && hi - lo < 40) {
                m = kStroke;
                ++white;
            } else if (caret && r > 150 && g > 150 && b < 100) {
                m = kUnknown;
            }
        }
    if (!loaded_ || white == 0) return -1;

    // How well digit `d` sits at (x, y): the share of its box that agrees,
    // and the share of its strokes that are there, both of what can be seen.
    // A digit with a quarter of its box or of its strokes out of sight is
    // `hidden`, and is not read there: that is a guess, not a reading. The
    // caret over a 2 hides a fifth of its box and three of its nineteen
    // strokes. How well a hidden digit fits what can be seen is still
    // measured, for telling whether another digit read there is sure.
    const auto fit = [&](int d, int x, int y, double& agree, double& strokes, bool& hidden) {
        const Glyph& g = digits_[d];
        agree = strokes = 0.0;
        hidden = false;
        if (x + g.w > kW || y + g.h > kH || g.lit == 0) return;
        int seen = 0, seen_lit = 0, same = 0, hit = 0;
        for (int gy = 0; gy < g.h; ++gy)
            for (int gx = 0; gx < g.w; ++gx) {
                const bool want = g.on[static_cast<std::size_t>(gy) * g.w + gx] != 0;
                const std::uint8_t have = map[static_cast<std::size_t>(y + gy) * kW + x + gx];
                if (have == kUnknown) continue;
                ++seen;
                if (want) ++seen_lit;
                if (want == (have == kStroke)) ++same;
                if (want && have == kStroke) ++hit;
            }
        hidden = seen * 4 < g.w * g.h * 3 || seen_lit * 4 < g.lit * 3;
        if (seen == 0 || seen_lit == 0) return;
        agree = static_cast<double>(same) / seen;
        strokes = static_cast<double>(hit) / seen_lit;
    };

    // Left to right along each starting row; the row that reads the most
    // digits wins. Where each digit was read is kept, to tell afterwards
    // whether the digits read are all the white there is.
    //
    // A digit read where a hidden one fits what can be seen as well is no
    // reading: with the caret over its first column a 6 looks like a 3, so
    // what reads as 3 there may be a 6. Only ever so where something is out
    // of sight, which a slot's corner never has.
    struct Line {
        std::string      text;
        std::vector<int> at;            ///< where each digit's box starts
        int              y = 0;
        bool             unsure = false;
    };
    Line best;
    for (int y = 0; y + 8 <= kH && y <= max_top; ++y) {
        Line line;
        line.y = y;
        int x = 0, misses = 0;
        while (x < kW - 2 && line.text.size() < 6) {
            int found = -1;
            double found_agree = 0.0;
            bool hidden_fits = false;
            for (int d = 0; d < 10; ++d) {
                double agree = 0.0, strokes = 0.0;
                bool hidden = false;
                fit(d, x, y, agree, strokes, hidden);
                if (agree < 0.88 || strokes < 0.88) continue;
                if (hidden) {
                    hidden_fits = true;
                } else if (agree > found_agree) {
                    found = d;
                    found_agree = agree;
                }
            }
            if (found >= 0) {
                line.text += static_cast<char>('0' + found);
                line.at.push_back(x);
                line.unsure = line.unsure || hidden_fits;
                x += digits_[found].w + 1;
                misses = 0;
            } else {
                if (!line.text.empty() && ++misses > 2) break;
                ++x;
            }
        }
        if (line.text.size() > best.text.size()) best = std::move(line);
    }

    // White outside every digit read: a digit that did not read, or
    // something that is no digit at all.
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            if (map[static_cast<std::size_t>(y) * kW + x] != kStroke) continue;
            bool inside = false;
            for (std::size_t i = 0; i < best.text.size() && !inside; ++i) {
                const Glyph& g = digits_[best.text[i] - '0'];
                inside = x >= best.at[i] && x < best.at[i] + g.w && y >= best.y && y < best.y + g.h;
            }
            if (!inside) ++stray;
        }

    // A count never starts with a nought, and none is that long.
    if (best.unsure || best.text.empty() || best.text.size() > 5 ||
        (best.text.size() > 1 && best.text[0] == '0'))
        return -1;
    return std::atoi(best.text.c_str());
}

int CountReader::read(const Frame& f, const ScreenRect& slot) const {
    if (!loaded_ || !f.valid()) return kUnreadable;
    // White the digits leave over says nothing in a slot's corner: the item
    // under the count can be white itself.
    int white = 0, stray = 0;
    const int n = scan(f, ScreenRect{ slot.x, slot.y, 28, 16 }, 6, false, white, stray);
    if (white == 0) return 1;                 // nothing printed: a stack of one
    if (n <= 0 || n > 9999) return kUnreadable;
    return n;
}

int CountReader::read_field(const Frame& f, const ScreenRect& area) const {
    if (!loaded_ || !f.valid()) return kUnreadable;
    int white = 0, stray = 0;
    const int n = scan(f, area, std::max(0, area.h - 8), true, white, stray);
    if (white == 0) return 0;                 // the caret alone is an empty box
    // Nothing in a box is white but its number, so white the digits read
    // leave over is a digit that did not read -- most of it under the caret
    // -- and the digits before it are not the number in the box: "12" with
    // the 2 under the caret is not 1.
    if (n < 0 || stray > 0) return kUnreadable;
    return n;
}

// --------------------------------------------------------------- catalog

namespace {

/// An icon drawn into the middle of an empty slot `size` pixels square, the
/// way the bag shows it. The bag's slots are 34; a wider one leaves room to
/// search a picture cut from the bag over it.
Frame slot_tile(const Frame& icon, int size = 34) {
    Frame t;
    t.width = size;
    t.height = size;
    t.pixels.assign(static_cast<std::size_t>(size) * size * 4, 255);
    for (std::size_t i = 0; i < t.pixels.size(); i += 4)
        t.pixels[i] = t.pixels[i + 1] = t.pixels[i + 2] = 16;
    const int ox = (size - icon.width) / 2, oy = (size - icon.height) / 2;
    for (int y = 0; y < icon.height; ++y)
        for (int x = 0; x < icon.width; ++x) {
            const std::size_t s = (static_cast<std::size_t>(y) * icon.width + x) * 4;
            if (icon.pixels[s + 3] < 128) continue;
            const int tx = ox + x, ty = oy + y;
            if (tx < 0 || ty < 0 || tx >= size || ty >= size) continue;
            const std::size_t d = (static_cast<std::size_t>(ty) * size + tx) * 4;
            t.pixels[d] = icon.pixels[s];
            t.pixels[d + 1] = icon.pixels[s + 1];
            t.pixels[d + 2] = icon.pixels[s + 2];
        }
    return t;
}

/// An icon as it is matched: its own transparency, less the corner the count
/// is printed over. The icon sits about two pixels into the slot, so the
/// slot's count corner is this much of the icon.
Frame icon_picture(const ItemCatalog::Entry& e) {
    Frame p = e.icon;
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x)
            if (x < 20 && y < 11) p.pixels[(static_cast<std::size_t>(y) * p.width + x) * 4 + 3] = 0;
    return p;
}

/// A catalog icon as a list item.
TrashList::Item catalog_item(const ItemCatalog::Entry& e, TrashList::Action a, float min_score) {
    TrashList::Item item;
    item.id = e.id;
    item.action = a;
    item.min_score = min_score;
    item.picture = icon_picture(e);
    item.shown = e.tile.valid() ? e.tile : slot_tile(e.icon);
    return item;
}

/// How alike the drawing `tpl` is to the best place for it in `in`, colour
/// left out: the two correlated in grey, over `tpl`'s opaque pixels. -1 when
/// it does not fit or nothing is alike at all.
float drawn_like(const Frame& in, const Frame& tpl) {
    if (!in.valid() || !tpl.valid() || in.width < tpl.width || in.height < tpl.height)
        return -1.0f;
    const auto m = TemplateMatcher::find(in, ScreenRect{ 0, 0, in.width, in.height }, tpl, -1.0f,
                                         TemplateMatcher::Tone::Grey);
    return m.found ? m.score : -1.0f;
}

/// Two pictures are one drawing when each is found in the other: one way
/// only is a drawing that holds the other -- a bag with a potion beside it
/// holds the same bag alone -- and those are two items.
bool one_drawing(const Frame& a, const Frame& a_in, const Frame& b, const Frame& b_in) {
    return drawn_like(b_in, a) >= TrashList::kSameDrawing &&
           drawn_like(a_in, b) >= TrashList::kSameDrawing;
}

bool listed(const std::vector<TrashList::Item>& items, const std::string& id) {
    for (const TrashList::Item& it : items)
        if (it.id == id) return true;
    return false;
}

} // namespace

std::string ItemCatalog::default_dir() {
    const std::string staged = TemplateStore::default_dir();
    if (!staged.empty() &&
        GetFileAttributesA((staged + "\\spend").c_str()) != INVALID_FILE_ATTRIBUTES)
        return staged;
    return TemplateStore::client_images();
}

bool ItemCatalog::load(const std::string& images_dir) {
    groups.clear();
    if (images_dir.empty()) return false;
    const struct { const char* folder; const char* name; } kGroups[] = {
        { "spend/fragment", "Mảnh ghép" },
        { "spend/craft",    "Nguyên liệu" },
        { "spend/hp",       "Hồi máu" },
        { "spend/mana",     "Hồi mana" },
        { "spend/buff",     "Bùa" },
        { "seal",           "Ấn" },
        { "other",          "Khác" },
    };
    for (const auto& k : kGroups) {
        Group group;
        group.folder = k.folder;
        group.name = k.name;
        std::string dir = images_dir + "\\" + k.folder;
        std::replace(dir.begin(), dir.end(), '/', '\\');

        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA((dir + "\\*.png").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            Entry e;
            std::string file = fd.cFileName;
            if (!load_png(dir + "\\" + file, e.icon) || !e.icon.valid()) continue;
            e.id = std::string(k.folder) + "/" + file.substr(0, file.size() - 4);
            e.tile = slot_tile(e.icon);
            group.entries.push_back(std::move(e));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        std::sort(group.entries.begin(), group.entries.end(),
                  [](const Entry& a, const Entry& b) { return a.id < b.id; });
        if (!group.entries.empty()) groups.push_back(std::move(group));
    }
    return !groups.empty();
}

const ItemCatalog::Entry* ItemCatalog::find(const std::string& id) const {
    for (const Group& g : groups)
        for (const Entry& e : g.entries)
            if (e.id == id) return &e;
    return nullptr;
}

std::size_t ItemCatalog::size() const {
    std::size_t n = 0;
    for (const Group& g : groups) n += g.entries.size();
    return n;
}

std::vector<std::string> ItemCatalog::same_drawing(const std::string& id) const {
    std::vector<std::string> out;
    const Entry* me = find(id);
    if (me == nullptr || !me->tile.valid()) return out;
    const Frame mine = icon_picture(*me);
    for (const Group& g : groups)
        for (const Entry& e : g.entries)
            if (e.id != id && e.tile.valid() && one_drawing(mine, me->tile, icon_picture(e), e.tile))
                out.push_back(e.id);
    return out;
}

// --------------------------------------------------------------- settings

bool CleanSettings::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path) || !ini.has_section("Trash")) return false;
    enabled          = ini.get_bool("Trash", "enabled", enabled);
    interval_minutes = std::clamp(ini.get_int("Trash", "interval_minutes", interval_minutes), 1, 720);
    bag_key          = static_cast<std::uint32_t>(ini.get_int("Trash", "bag_key", static_cast<int>(bag_key)));
    const char* const kTabKeys[kBagTabs] = { "tab_equip1", "tab_equip2", "tab_use",
                                             "tab_other", "tab_pet" };
    for (int i = 0; i < kBagTabs; ++i) tabs[i] = ini.get_bool("Trash", kTabKeys[i], tabs[i]);
    // Not below 0.85: in grey, two different fragments score up to 0.74 on a
    // real slot and two different potions 0.81, and a hand-edited 0.70 would
    // let those through wherever the catalog has nothing better to say.
    min_score        = std::clamp(ini.get_float("Trash", "min_score", min_score), 0.85f, 0.99f);
    max_per_run      = std::clamp(ini.get_int("Trash", "max_per_run", max_per_run), 1, 500);
    click_to_move    = ini.get_bool("Trash", "click_to_move", click_to_move);
    catalog_score    = std::clamp(ini.get_float("Trash", "catalog_score", catalog_score), 0.85f, 0.99f);

    const auto ids = [&](const char* key) {
        std::vector<std::string> out;
        std::string all = ini.get("Items", key, ""), one;
        all += ',';
        for (const char c : all) {
            if (c == ',' || c == ' ') {
                if (!one.empty()) out.push_back(one);
                one.clear();
            } else {
                one += c;
            }
        }
        return out;
    };
    delete_ids = ids("delete");
    drop_ids   = ids("drop");
    layout.load(ini);
    return true;
}

// ------------------------------------------------------------------ list

const char* TrashList::folder(Action a) {
    return a == Action::Drop ? "drop" : "delete";
}

bool TrashList::load(const std::string& dir, const BagLayout& l) {
    items.clear();
    rivals.clear();
    for (const Action a : { Action::Delete, Action::Drop }) {
        const std::string sub = dir + "\\" + folder(a);
        if (GetFileAttributesA(sub.c_str()) == INVALID_FILE_ATTRIBUTES) continue;

        TemplateStore store;
        store.load(sub);
        for (const std::string& name : store.names()) {
            const Frame* picture = store.get(name);
            if (picture == nullptr || !picture->valid()) continue;
            Item item;
            item.file = sub + "\\" + name + ".png";
            item.action = a;
            item.shown = *picture;
            for (std::size_t i = 3; i < item.shown.pixels.size(); i += 4)
                item.shown.pixels[i] = 255;
            item.picture = bag::masked(item.shown, l);
            items.push_back(std::move(item));
        }
    }
    // A folder that does not exist yet is an empty list, not a failure.
    return true;
}

std::string TrashList::add(const std::string& dir, const Frame& cut, Action a,
                           const BagLayout& l) {
    if (!cut.valid()) return {};
    const std::string sub = dir + "\\" + folder(a);
    CreateDirectoryA(dir.c_str(), nullptr);
    CreateDirectoryA(sub.c_str(), nullptr);

    // Named by when, which is unique enough for something a person clicks.
    std::string path;
    for (int n = 0; n < 100; ++n) {
        path = sub + "\\item_" + stamp() + "_" + std::to_string(n) + ".png";
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    if (!save_png(cut, path)) return {};

    Item item;
    item.file = path;
    item.action = a;
    item.shown = cut;
    item.picture = bag::masked(cut, l);
    items.push_back(std::move(item));
    return path;
}

void TrashList::add_picture(const Frame& cut, Action a, const BagLayout& l) {
    Item item;
    item.action = a;
    item.shown = cut;
    item.picture = bag::masked(cut, l);
    items.push_back(std::move(item));
}

void TrashList::add_catalog(const ItemCatalog& catalog, const std::vector<std::string>& ids,
                            Action a, float min_score) {
    const std::size_t first = items.size();
    for (const std::string& id : ids) {
        const ItemCatalog::Entry* e = catalog.find(id);
        if (e == nullptr || listed(items, id)) continue;
        Item item = catalog_item(*e, a, min_score);
        item.kin = catalog.same_drawing(id);
        item.kin.push_back(id);
        items.push_back(std::move(item));
    }
    // The same drawing in other colours goes too, each as an item of its own:
    // its own icon is what matches it best, and the screen can say which
    // chosen icon brought it. Held to that one's group, so a drawing that is
    // only like one of these, and not like what was chosen, is a rival still.
    const std::size_t chosen = items.size();
    for (std::size_t i = first; i < chosen; ++i) {
        const std::vector<std::string> group = items[i].kin;
        const std::string via = items[i].id;
        for (const std::string& id : group) {
            const ItemCatalog::Entry* e = catalog.find(id);
            if (e == nullptr || listed(items, id)) continue;
            Item item = catalog_item(*e, a, min_score);
            item.kin = group;
            item.via = via;
            items.push_back(std::move(item));
        }
    }
}

void TrashList::add_rivals(const ItemCatalog& catalog) {
    rivals.clear();
    for (const ItemCatalog::Group& g : catalog.groups)
        for (const ItemCatalog::Entry& e : g.entries) {
            if (!listed(items, e.id)) rivals.push_back(catalog_item(e, Action::Delete, 0.0f));

            // A picture cut from the bag may be of this very item, or of its
            // drawing in another colour -- a red piece, where the icon is
            // gold. Then the icon is the same thing as what was chosen rather
            // than something to tell it from: as a rival it would tie on
            // every slot holding it, and nothing would be thrown away.
            // The picture is a whole slot, so the icon is drawn on a wider
            // one for the picture to be searched over.
            const Frame icon = icon_picture(e);
            const Frame wide = slot_tile(e.icon, 40);
            for (Item& it : items) {
                if (!it.id.empty() || !it.shown.valid()) continue;
                if (one_drawing(icon, wide, it.picture, it.shown) &&
                    std::find(it.kin.begin(), it.kin.end(), e.id) == it.kin.end())
                    it.kin.push_back(e.id);
            }
        }
}

void TrashList::build(const std::string& dir, const CleanSettings& s, const ItemCatalog& catalog) {
    load(dir, s.layout);
    add_catalog(catalog, s.delete_ids, Action::Delete, s.catalog_score);
    add_catalog(catalog, s.drop_ids, Action::Drop, s.catalog_score);
    add_rivals(catalog);
}

bool TrashList::remove(std::size_t index) {
    if (index >= items.size()) return false;
    if (!items[index].file.empty()) DeleteFileA(items[index].file.c_str());
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

TrashList::Verdict TrashList::judge(const Frame& f, const ScreenRect& slot,
                                    float min_score) const {
    Verdict v;
    const ScreenRect area = clamp_to(f, ScreenRect{ slot.x - 3, slot.y - 3,
                                                    slot.w + 6, slot.h + 6 });
    // The drawing alone, in grey. Measured on every real frame, the right
    // icon scores 0.952 or more on its item in any colour the bag draws it --
    // gold, orange, the red of GoC's own pictures -- the best other drawing
    // 0.81 at most, and the enlarged copy a slot shows while its item waits
    // in the bin 0.79 at most.
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& item = items[i];
        const Frame& p = item.picture;
        if (!p.valid() || area.w < p.width || area.h < p.height) continue;
        const float need = item.min_score > 0.0f ? item.min_score : min_score;
        const auto m = TemplateMatcher::find(f, area, p, need, TemplateMatcher::Tone::Grey);
        if (m.found && m.score > v.score) {
            v.item = static_cast<int>(i);
            v.score = m.score;
        }
    }
    if (v.item < 0) return v;

    // Something not on the list, drawn differently, that looks as much like
    // the slot is as good an answer, and the slot is left alone: a bag with a
    // potion beside it is not the same bag alone. The same drawing in another
    // colour is not a rival -- by the player's rule it is the same thing.
    const Item& won = items[static_cast<std::size_t>(v.item)];
    for (const Item& rival : rivals) {
        if (std::find(won.kin.begin(), won.kin.end(), rival.id) != won.kin.end()) continue;
        const Frame& p = rival.picture;
        if (!p.valid() || area.w < p.width || area.h < p.height) continue;
        const auto m = TemplateMatcher::find(f, area, p, v.score - kRivalMargin,
                                             TemplateMatcher::Tone::Grey);
        if (!m.found) continue;
        v.doubt = Verdict::Doubt::Rival;
        v.like = v.item;
        v.like_score = v.score;
        v.rival = rival.id;
        v.rival_score = m.score;
        v.item = -1;
        v.score = 0.0f;
        return v;
    }
    return v;
}

int TrashList::match(const Frame& f, const ScreenRect& slot, float min_score,
                     float* score) const {
    const Verdict v = judge(f, slot, min_score);
    if (score) *score = v.item >= 0 ? v.score : v.like_score;
    return v.item;
}

// --------------------------------------------------------------- dialogs

namespace bag {

const char* dialog_name(int kind) {
    switch (kind) {
        case kAskDialog:   return "hỏi có bỏ vào thùng rác không";
        case kCountDialog: return "hỏi số lượng cần xoá";
        default:           return "không phải của thùng rác";
    }
}

std::vector<Dialog> find_dialogs(const Frame& f, const TemplateStore& store) {
    std::vector<Dialog> out;
    const Frame* pair = store.get(kConfirm);
    if (pair == nullptr || !f.valid()) return out;

    // Both drawings of the two buttons: the store's, and the question's own,
    // a pixel closer together.
    std::vector<Frame> pairs{ *pair };
    if (const Frame* ask = store.get(kBinDialogs[kAskDialog].name)) {
        Frame other = crop(*ask, ScreenRect{ kBinDialogs[kAskDialog].pair_dx,
                                             kBinDialogs[kAskDialog].pair_dy, pair->width,
                                             pair->height });
        if (other.valid()) pairs.push_back(std::move(other));
    }
    const ScreenRect all{ 0, 0, f.width, f.height };
    std::vector<ScreenRect> found;
    for (const Frame& p : pairs)
        for (const auto& m : TemplateMatcher::find_all(f, all, p, kButtonScore, 8)) {
            bool again = false;
            for (const ScreenRect& r : found) again = again || same_place(r, m.at);
            if (!again) found.push_back(m.at);
        }

    for (const ScreenRect& at : found) {
        Dialog d;
        d.pair = at;
        for (int k = 0; k < static_cast<int>(std::size(kBinDialogs)); ++k) {
            const DialogPicture& pic = kBinDialogs[k];
            const Frame* t = store.get(pic.name);
            if (t == nullptr) continue;
            const Frame words = crop(*t, ScreenRect{ kWordsX, kWordsY, kWordsW, kWordsH });
            if (!words.valid()) continue;
            int bright = 0;
            for (std::size_t i = 0; i < words.pixels.size(); i += 4)
                if ((words.pixels[i] + words.pixels[i + 1] + words.pixels[i + 2]) / 3 > 150) ++bright;
            if (bright < kWordsAtLeast) continue;     // no words: tells nothing apart

            // Right where they sit relative to the buttons, give or take two
            // pixels, and nowhere else.
            const ScreenRect place = clamp_to(f, ScreenRect{ at.x - pic.pair_dx + kWordsX - 2,
                                                             at.y - pic.pair_dy + kWordsY - 2,
                                                             kWordsW + 4, kWordsH + 4 });
            if (place.w < words.width || place.h < words.height) continue;
            const auto m = TemplateMatcher::find(f, place, words, kWordsScore);
            if (m.found && m.score > d.words) {
                d.kind = k;
                d.words = m.score;
                d.quantity = pic.quantity;
            }
        }
        if (d.quantity)
            d.box = ScreenRect{ at.x + kBoxDx, at.y + kBoxDy, kBoxW, std::max(4, at.h - 6) };
        out.push_back(d);
    }
    return out;
}

bool bin_asking(const Frame& f, const TemplateStore& store) {
    for (const Dialog& d : find_dialogs(f, store))
        if (d.ours()) return true;
    return false;
}

bool nothing_held(const Frame& f, const TemplateStore& store, const BagLayout& l) {
    if (!f.valid()) return false;
    // Any dialog at all: one of the bin's whose words did not match is still
    // the bin's, and holds the item.
    if (!find_dialogs(f, store).empty()) return false;
    const BagView v = find(f, store, l);
    return !v.found || v.bin_empty;
}

bool can_check(const TemplateStore& store, TrashList::Action a) {
    if (store.get(kConfirm) == nullptr) return false;
    if (a == TrashList::Action::Drop) return false;      // see the header
    for (const DialogPicture& pic : kBinDialogs)
        if (store.get(pic.name) == nullptr) return false;
    return true;
}

} // namespace bag

// ---------------------------------------------------------------- cleaner

namespace {

/// What the list calls an item, for the screen.
std::string item_name(const TrashList::Item& item) {
    if (item.id.empty()) return "hình đã chụp";
    if (!item.via.empty()) return item.id + " (cùng hình với " + item.via + ")";
    return item.id;
}

} // namespace

BagCleaner::BagCleaner(InputManager& input, const TemplateStore& store, Grab grab)
    : input_(input), store_(store), grab_(std::move(grab)) {}

void BagCleaner::keep(const Frame& f, const char* what) {
    if (debug_dir_.empty() || !f.valid()) return;
    CreateDirectoryA(debug_dir_.c_str(), nullptr);

    // A few dozen at most: the newest are the ones that matter.
    std::vector<std::string> old;
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((debug_dir_ + "\\*.png").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { old.push_back(fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(old.begin(), old.end());
    for (std::size_t i = 0; old.size() >= 30 && i + 29 < old.size(); ++i)
        DeleteFileA((debug_dir_ + "\\" + old[i]).c_str());

    static int n = 0;
    const std::string path = debug_dir_ + "\\" + stamp() + "_" + std::to_string(n++ % 100) +
                             "_" + what + ".png";
    if (save_png(f, path)) GB_INFO("Trash: kept the screen as %s", path.c_str());
}

void BagCleaner::say(const std::string& line) {
    GB_INFO("Trash: %s", line.c_str());
    if (report_) report_(line);
}

bool BagCleaner::wait_ms(int ms) {
    for (int slept = 0; slept < ms; slept += 30) {
        if (cancelled_) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(30, ms - slept)));
    }
    return !cancelled_;
}

void BagCleaner::pause_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool BagCleaner::click(int x, int y) {
    return hands_.click ? hands_.click(x, y) : input_.click_at(x, y);
}

bool BagCleaner::drag(int fx, int fy, int tx, int ty) {
    return hands_.drag ? hands_.drag(fx, fy, tx, ty) : input_.drag(fx, fy, tx, ty);
}

bool BagCleaner::press(std::uint32_t vk) {
    return hands_.key ? hands_.key(vk) : input_.key_press(vk);
}

bool BagCleaner::grab_view(Frame& f, BagView& v) {
    if (!grab_ || !grab_(f) || !f.valid()) return false;
    v = bag::find(f, store_, settings_.layout);
    return v.found;
}

bool BagCleaner::clean(const Frame& f, const BagView& v) {
    return v.found && v.bin_empty && bag::find_dialogs(f, store_).empty();
}

bool BagCleaner::mouse_ok(int x, int y, bool need_front) {
    if (skip_focus_check_) return true;
    // A click aimed at the game lands on whatever is on top at that point,
    // and the frames come from PrintWindow, which shows the game even when it
    // is covered. A pass that went on with GoBot over the bag would drag
    // items about in GoBot's own window.
    if (need_front && !input_.window_focused()) {
        say("cửa sổ game không ở trên cùng, nên không dùng chuột");
        return false;
    }
    HWND game = static_cast<HWND>(input_.target_window());
    POINT p{ x, y };
    if (game == nullptr || !ClientToScreen(game, &p)) return false;
    HWND under = WindowFromPoint(p);
    if (under == nullptr || (under != game && GetAncestor(under, GA_ROOT) != game)) {
        say("có cửa sổ khác che game ở chỗ cần bấm, nên không dùng chuột");
        return false;
    }
    return true;
}

void BagCleaner::bring_forward() {
    if (skip_focus_check_ || input_.window_focused()) return;
    input_.focus_window();
    pause_ms(150);
}

void BagCleaner::scroll_to_top(BagView& v) {
    const BagLayout& l = settings_.layout;
    Frame f;
    BagView now;
    if (!grab_view(f, now)) return;
    for (int i = 0; i < 15 && !cancelled_ && !now.at_top(); ++i) {
        const int x = now.strip.x + l.scroll_up_dx, y = now.strip.y + l.scroll_up_dy;
        if (!mouse_ok(x, y)) break;
        click(x, y);
        Frame g;
        BagView after;
        if (!wait_ms(200) || !grab_view(g, after)) break;
        const bool moved = now.scroll_known() && after.scroll_known()
                               ? after.first_row != now.first_row
                               : !bag::same_page(f, now, g, after, l);
        f = std::move(g);
        now = after;
        if (!moved) break;
    }
    v = now;
}

BagCleaner::Result BagCleaner::run() {
    Result res;
    const BagLayout& l = settings_.layout;

    if (trash_ == nullptr || trash_->items.empty()) {
        res.trouble = "danh sách đồ bỏ đi đang trống";
        return res;
    }
    if (store_.get(kConfirm) == nullptr) {
        res.trouble = "thiếu hình nút Xác Nhận, nên không nhận ra được hộp thoại nào";
        return res;
    }
    if (cancelled_) return res;          // stopped before it began
    item_out_ = false;

    Frame f;
    BagView v;
    bool opened_here = false;
    if (!grab_view(f, v) && f.valid() && !bag::find_dialogs(f, store_).empty()) {
        // A dialog up with the bag out of sight: the key that opens the bag
        // would go to the dialog -- into a quantity box, say. The bin's own is
        // said no to first; anything else is left alone.
        bool ours = false;
        for (const bag::Dialog& d : bag::find_dialogs(f, store_)) ours = ours || d.ours();
        if (dry_run_ || !ours) {
            res.trouble = "có một hộp thoại đang mở; đóng nó trong game rồi thử lại";
            return res;
        }
        back_out();                  // ends on the bag not being found, even when it worked
        if (!grab_ || !grab_(f) || !f.valid() || !bag::find_dialogs(f, store_).empty()) {
            res.trouble = stuck_.empty()
                ? std::string("hộp thoại của thùng rác đang mở, và không trả lại được")
                : stuck_;
            res.stuck = item_out_ || !f.valid() || bag::bin_asking(f, store_);
            return res;
        }
        item_out_ = false;
        grab_view(f, v);
    }
    if (!f.valid()) {
        // Not even a look at the screen: no key is pressed blind.
        res.trouble = "không chụp được màn hình game";
        return res;
    }
    if (!v.found) {
        press(settings_.bag_key);
        opened_here = true;
        for (int i = 0; i < 12 && !v.found; ++i) {
            if (!wait_ms(150)) return res;
            grab_view(f, v);
        }
        if (!v.found) {
            res.trouble = "không mở được túi đồ (phím " + InputManager::key_name(settings_.bag_key) +
                          "), hoặc túi đồ bị che";
            return res;
        }
    }
    // Closed again only over a clean screen: with a dialog up, the key would
    // go to the dialog -- into a quantity box, say -- instead of the bag.
    const auto close_bag = [&] {
        if (!opened_here) return;
        Frame g;
        if (grab_ && grab_(g) && g.valid() && !bag::find_dialogs(g, store_).empty()) {
            say("để túi đồ mở, vì vẫn còn một hộp thoại đang mở");
            return;
        }
        press(settings_.bag_key);
    };

    // Left over from before -- a pass that stopped half way, or a hand: an
    // item waiting in the bin, or the bin's dialog still open holding one,
    // goes back first. Otherwise the next item dropped on the bin is refused,
    // and a quantity box left open takes the macro's potion keys as numbers.
    if (!clean(f, v)) {
        if (dry_run_) {
            // Looking only touches nothing, not even Hủy Bỏ: it says what is
            // in the way instead.
            bool ours = false;
            const auto dialogs = bag::find_dialogs(f, store_);
            for (const bag::Dialog& d : dialogs) ours = ours || d.ours();
            res.trouble = ours ? "hộp thoại của thùng rác đang mở; bấm Hủy Bỏ trong game "
                                 "rồi thử lại"
                        : !dialogs.empty() ? "có một hộp thoại khác đang mở; đóng nó trong "
                                             "game rồi thử lại"
                                           : "thùng rác đang có đồ chờ xoá; bấm vào thùng rác "
                                             "trong game, chọn Hủy Bỏ, rồi thử lại";
            close_bag();
            return res;
        }
        if (!back_out()) {
            res.trouble = stuck_.empty() ? std::string("thùng rác đang có đồ chờ xoá hoặc có "
                                                       "hộp thoại đang mở, và không trả lại được")
                                         : stuck_;
            res.stuck = item_out_;
            close_bag();
            return res;
        }
    }
    res.ran = true;

    const bool can_drop = bag::can_check(store_, TrashList::Action::Drop);
    std::set<long long> seen;        // slots dealt with this pass: tab, row of the tab, column
    int done = 0;
    bool stop = false;
    for (int tab = 0; tab < kBagTabs && !stop && !cancelled_; ++tab) {
        if (!settings_.tabs[tab]) continue;

        int tx = 0, ty = 0;
        v.tab_point(l, tab, tx, ty);
        if (!mouse_ok(tx, ty)) {
            res.trouble = "không bấm được vào túi đồ: cửa sổ game bị che hoặc không ở trên cùng";
            break;
        }
        click(tx, ty);
        if (!wait_ms(350)) break;
        scroll_to_top(v);

        int scrolls = 0;
        bool hid_pointer = false;
        for (int round = 0; round < 200 && !stop && !cancelled_; ++round) {
            // A fresh look every time: after an item is thrown away, or fails
            // to be, the screen is not what it was.
            if (!grab_view(f, v)) {
                res.trouble = "mất dấu túi đồ";
                stop = true;
                break;
            }
            // Never a slot judged while the bin holds something or a dialog
            // is up: the slot an item was dragged from shows an enlarged copy
            // of it until it is gone or given back, and that copy is no item
            // to drag. Looking only says what is in the way and stops.
            if (!clean(f, v)) {
                if (dry_run_) {
                    res.trouble = "thùng rác đang có đồ chờ xoá hoặc có hộp thoại đang mở; "
                                  "xong việc đó trong game rồi thử lại";
                    stop = true;
                    break;
                }
                if (!back_out()) {
                    res.trouble = stuck_.empty()
                        ? std::string("thùng rác đang có đồ chờ xoá hoặc có hộp thoại đang mở, "
                                      "và không trả lại được")
                        : stuck_;
                    stop = true;
                    break;
                }
                continue;
            }
            // A tooltip over the grid hides rows; the pointer is moved off it
            // once rather than those rows skipped.
            if (v.slot_rows < l.rows && !v.at_bottom(l) && !hid_pointer) {
                hid_pointer = true;
                if (!hands_.click)
                    input_.move_pointer(v.strip.x + l.scroll_up_dx + 30, v.strip.y + 100);
                if (!wait_ms(300)) break;
                continue;
            }

            bool acted = false;
            const auto slots = v.slots(l);
            for (std::size_t s = 0; s < slots.size() && !stop && !cancelled_; ++s) {
                const int r = static_cast<int>(s) / l.cols, c = static_cast<int>(s) % l.cols;
                // Rows of the tab, when the scroll bar says where the page is,
                // so a row on show twice -- one click scrolls two rows or three
                // of the four -- is seen once and reported where it is.
                const int row = v.scroll_known() ? v.first_row + r : 1000 * (scrolls + 1) + r;
                const long long key = static_cast<long long>(tab) * 100000000LL +
                                      static_cast<long long>(row) * 100 + c;
                if (seen.count(key) || bag::is_empty(f, slots[s], l)) continue;

                const TrashList::Verdict k = trash_->judge(f, slots[s], settings_.min_score);
                char where[96];
                std::snprintf(where, sizeof where, "%s, hàng %d ô %d", bag_tab_name(tab),
                              (v.scroll_known() ? v.first_row : 0) + r + 1, c + 1);
                if (k.item < 0) {
                    if (k.doubt == TrashList::Verdict::Doubt::None || k.like < 0) continue;
                    seen.insert(key);
                    ++res.doubtful;
                    const TrashList::Item& like = trash_->items[static_cast<std::size_t>(k.like)];
                    char scores[64];
                    std::snprintf(scores, sizeof scores, " (%.0f%% và %.0f%%)",
                                  static_cast<double>(k.like_score) * 100.0,
                                  static_cast<double>(k.rival_score) * 100.0);
                    say(std::string("thấy món ở ") + where + " giống " + item_name(like) +
                        " nhưng cũng giống " + k.rival + scores +
                        ", là món khác hình không có trong danh sách, nên không bỏ");
                    continue;
                }

                seen.insert(key);
                const TrashList::Item& item = trash_->items[static_cast<std::size_t>(k.item)];
                ++res.matched;
                char percent[24];
                std::snprintf(percent, sizeof percent, "%.0f%%", static_cast<double>(k.score) * 100.0);
                // Which listed item, when it is one that came along with a
                // chosen icon for being drawn the same.
                const std::string score = " (giống " + std::string(percent) +
                                          (item.via.empty() ? std::string()
                                                            : ", " + item_name(item)) + ")";
                if (dry_run_) {
                    if (item.action == TrashList::Action::Delete)
                        say(std::string("thấy đồ bỏ đi ở ") + where + score);
                    else
                        say(std::string("thấy đồ vứt đi ở ") + where + score +
                            (can_drop ? "" : ", nhưng sẽ không vứt: GoBot chưa có hình hộp "
                                             "thoại vứt đồ của game để kiểm tra"));
                    continue;
                }
                if (item.action == TrashList::Action::Drop && !can_drop) {
                    say(std::string("không vứt món ở ") + where + score +
                        ": GoBot chưa có hình hộp thoại vứt đồ của game để kiểm tra");
                    continue;
                }
                if (done >= settings_.max_per_run) {
                    say("đã bỏ " + std::to_string(done) + " món, đủ cho một lần");
                    stop = true;
                    break;
                }

                const int count = counts_ ? counts_->read(f, slots[s]) : CountReader::kUnreadable;
                if (trash_one(f, v, static_cast<int>(s), count)) {
                    ++done;
                    ++res.deleted;
                    say(std::string("đã xoá món ở ") + where + score);
                } else {
                    ++res.failed;
                    say(std::string("không bỏ được món ở ") + where + score);
                    if (res.failed >= 3) {
                        res.trouble = "bỏ đồ không được 3 lần, dừng lại";
                        stop = true;
                    }
                }
                acted = true;
                hid_pointer = false;        // it has been somewhere else since
                break;
            }
            if (acted || stop) continue;

            // Nothing more on this page: the next rows, until the scroll bar
            // is at the bottom or scrolling changes nothing.
            if (v.at_bottom(l) || ++scrolls > 30) break;
            const int dx = v.strip.x + l.scroll_down_dx, dy = v.strip.y + l.scroll_down_dy;
            if (!mouse_ok(dx, dy)) {
                res.trouble = "không bấm được vào túi đồ: cửa sổ game bị che hoặc không ở trên cùng";
                stop = true;
                break;
            }
            click(dx, dy);
            hid_pointer = false;
            if (!wait_ms(250)) break;
            Frame g;
            BagView w;
            if (!grab_view(g, w)) continue;          // the next round says so
            const bool moved = v.scroll_known() && w.scroll_known()
                                   ? w.first_row > v.first_row
                                   : !bag::same_page(f, v, g, w, l);
            if (!moved) break;
        }
    }

    // A stop that came in the middle of an item has given it back already;
    // this is for anything else it left behind. When even this cannot click
    // -- the game covered where the button is -- whoever ran the pass is told,
    // so the macro is kept off a box that would take its keys as numbers.
    if (cancelled_ && !dry_run_ && !back_out() && res.trouble.empty())
        res.trouble = stuck_.empty() ? std::string("dừng giữa chừng, và không trả lại được món "
                                                   "đồ trong thùng rác")
                                     : stuck_;
    res.stuck = item_out_;
    close_bag();
    return res;
}

bool BagCleaner::back_out() {
    stuck_.clear();
    // The game to the front first. A stop is mostly pressed in GoBot's own
    // window -- unticking "Tự dọn rác", DỪNG, closing it -- or in a console,
    // and the answer has to reach the game all the same. From here on a click
    // only needs the game on top where it lands.
    bring_forward();
    for (int attempt = 0; attempt < 5; ++attempt) {
        Frame f;
        if (!grab_ || !grab_(f) || !f.valid()) {
            stuck_ = "không chụp được màn hình game";
            return false;
        }
        // The dialogs first, whether or not the bag is to be seen: the bin's
        // quantity dialog sits over the bag and is answered without it.
        const BagView v = bag::find(f, store_, settings_.layout);
        const auto dialogs = bag::find_dialogs(f, store_);
        bool ours = false;
        bag::Dialog d;
        for (const bag::Dialog& each : dialogs)
            if (each.ours()) { d = each; ours = true; break; }

        if (ours) {
            // Hủy Bỏ, the right half of the pair: the item goes back.
            if (!mouse_ok(d.cancel_x(), d.y(), false)) {
                stuck_ = "hộp thoại của thùng rác đang mở mà không bấm được Hủy Bỏ";
                item_out_ = true;
                say(stuck_);
                keep(f, "cancel_blocked");
                return false;
            }
            say(std::string("bấm Hủy Bỏ trên hộp thoại ") + bag::dialog_name(d.kind) +
                ", để món đồ về lại túi");
            click(d.cancel_x(), d.y());
            for (int i = 0; i < 12; ++i) {
                pause_ms(150);
                Frame g;
                if (!grab_ || !grab_(g) || !g.valid()) continue;
                bool still = false;
                for (const bag::Dialog& each : bag::find_dialogs(g, store_))
                    still = still || same_place(each.pair, d.pair);
                if (!still) break;
            }
            continue;
        }
        if (!dialogs.empty()) {
            // Not the bin's: the revive question, an invitation, a shop -- or
            // the bin's question in words that did not match, which is why an
            // item sent towards the bin this pass stays counted as out.
            // Not answered, whatever it is.
            stuck_ = "có một hộp thoại khác đang mở, nên không bấm gì";
            say(stuck_);
            keep(f, "other_dialog");
            return false;
        }
        if (!v.found) {
            stuck_ = "mất dấu túi đồ";
            return false;
        }
        if (!v.bin_empty) {
            // Something waits in the bin: clicking it brings up its dialog,
            // and Hủy Bỏ there is how it goes back.
            if (!mouse_ok(v.trash.cx(), v.trash.cy(), false)) {
                stuck_ = "thùng rác đang có đồ chờ xoá mà không bấm được vào thùng rác";
                item_out_ = true;
                say(stuck_);
                return false;
            }
            say("bấm vào thùng rác, để lấy lại món đồ đang chờ xoá");
            click(v.trash.cx(), v.trash.cy());
            bool up = false;
            for (int i = 0; i < 12 && !up; ++i) {
                pause_ms(150);
                Frame g;
                if (!grab_ || !grab_(g) || !g.valid()) continue;
                up = !bag::find_dialogs(g, store_).empty();
            }
            if (!up) {
                stuck_ = "thùng rác đang có đồ chờ xoá, và bấm vào thì không thấy hộp thoại";
                item_out_ = true;
                say(stuck_);
                keep(f, "bin_stuck");
                return false;
            }
            continue;
        }
        item_out_ = false;
        return true;
    }
    Frame f;
    BagView v;
    const bool ok = grab_view(f, v) && clean(f, v);
    if (ok) {
        item_out_ = false;
    } else {
        stuck_ = "thùng rác vẫn chưa trả lại món đồ";
        item_out_ = true;
        say(stuck_);
        keep(f, "bin_stuck");
    }
    return ok;
}

bag::Dialog BagCleaner::wait_for_dialog(int ms, bool& foreign, Frame* last) {
    foreign = false;
    int others = 0;
    for (int waited = 0; waited < ms; waited += 150) {
        if (!wait_ms(150)) break;
        Frame f;
        if (!grab_ || !grab_(f) || !f.valid()) continue;
        const auto dialogs = bag::find_dialogs(f, store_);
        for (const bag::Dialog& d : dialogs)
            if (d.ours()) return d;
        // A dialog fading in can fail the words for a frame; one that is
        // still not the bin's a frame later is not.
        if (!dialogs.empty()) ++others;
        if (last) *last = std::move(f);
    }
    foreign = others >= 2;
    return bag::Dialog{};
}

int BagCleaner::box_holds(const bag::Dialog& d, Frame& last) {
    // Every look until the caret has been on and off: one that reads a
    // number has to agree with every other that does, and a look that reads
    // nothing -- the dialog caught being drawn, say -- counts neither way.
    int value = CountReader::kUnreadable, reads = 0;
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (!wait_ms(kBoxLookMs)) return CountReader::kUnreadable;     // stopped
        const bool long_enough = std::chrono::steady_clock::now() - start >=
                                 std::chrono::milliseconds(kBoxWatchMs);
        Frame f;
        if (grab_ && grab_(f) && f.valid()) {
            bool there = false;
            for (const bag::Dialog& each : bag::find_dialogs(f, store_))
                there = there || (each.ours() && each.quantity && same_place(each.pair, d.pair));
            if (!there) {
                last = std::move(f);
                return kBoxGone;
            }
            const int n = counts_ ? counts_->read_field(f, d.box) : CountReader::kUnreadable;
            last = std::move(f);
            if (n != CountReader::kUnreadable) {
                if (reads > 0 && n != value) return kBoxMixed;
                value = n;
                ++reads;
            }
        }
        if (long_enough) break;
    }
    return reads >= 2 ? value : CountReader::kUnreadable;
}

int BagCleaner::answer(const bag::Dialog& first, int count) {
    bag::Dialog d = first;
    for (int asked = 0; asked < 2; ++asked) {
        if (d.quantity) {
            // Only the exact count in the stack is ever confirmed, so a count
            // that does not read is a stop, not a guess.
            if (counts_ == nullptr || !counts_->loaded() || count == CountReader::kUnreadable ||
                count <= 0) {
                say("không đọc được số lượng trên ô đồ, nên bỏ qua món này");
                Frame f;
                if (grab_ && grab_(f)) keep(f, "no_count");
                return kFailed;
            }
            // Nothing is ever typed into the box, nor is it clicked. The game
            // fills it in with the whole stack of whatever is in the bin, and
            // Xác Nhận is pressed only when that number is the count read off
            // the slot. A box that holds anything else through a whole blink
            // -- another number, nothing, a number that does not read or does
            // not hold still -- means the count was misread or the bin holds
            // something other than the item judged, since the bag can change
            // under a pass; and a deletion cannot be undone. So the item goes
            // back instead: trash_one answers Hủy Bỏ.
            Frame f;
            const int in_box = box_holds(d, f);
            if (cancelled_) return kFailed;
            if (in_box != count) {
                const std::string slot = std::to_string(count) + " như trên ô đồ";
                say(in_box == kBoxGone
                        ? std::string("hộp thoại hỏi số lượng không còn ở chỗ cũ, nên không bấm "
                                      "Xác Nhận")
                    : in_box == kBoxMixed
                        ? std::string("ô số lượng lúc đọc ra số này lúc ra số khác, nên không bấm "
                                      "Xác Nhận")
                    : in_box == CountReader::kUnreadable
                        ? std::string("không đọc được số trong ô số lượng, nên không bấm Xác Nhận")
                    : in_box == 0
                        ? "ô số lượng trống chứ không ghi " + slot + ", nên không bấm Xác Nhận"
                        : "ô số lượng ghi " + std::to_string(in_box) + " chứ không phải " + slot +
                              ", nên không bấm Xác Nhận");
                keep(f, "box");
                return kFailed;
            }
        }

        // Xác Nhận, the left half of the pair -- unless a stop came in the
        // meantime, which gives the item back instead of deleting it.
        if (cancelled_ || !mouse_ok(d.confirm_x(), d.y())) return kFailed;
        click(d.confirm_x(), d.y());

        // Gone from where it was -- or replaced there by the bin's other
        // dialog, which is the next question rather than this one stuck.
        bool closed = false;
        bag::Dialog then;
        Frame f;
        for (int i = 0; i < 12 && !closed; ++i) {
            if (!wait_ms(150)) return kFailed;
            if (!grab_ || !grab_(f) || !f.valid()) continue;
            bool still = false;
            for (const bag::Dialog& each : bag::find_dialogs(f, store_)) {
                if (!same_place(each.pair, d.pair)) continue;
                if (each.ours() && each.kind != d.kind) then = each;
                else still = true;
            }
            closed = !still;
        }
        if (!closed) {
            say("hộp thoại không đóng lại");
            keep(f, "stuck");
            return kFailed;
        }
        if (then.ours()) {
            d = then;
            continue;
        }

        // A refusal is a message with only Xác Nhận under it. Its picture has
        // no words, so it is not answered: what it says is the player's to
        // read. Only taken for one when no dialog with both buttons is up --
        // the wordless picture matches the bin's own question at 0.84, and
        // that question is the next step, not a refusal.
        if (const Frame* notice = store_.get(kNotice)) {
            if (bag::find_dialogs(f, store_).empty() &&
                TemplateMatcher::find(f, ScreenRect{ 0, 0, f.width, f.height }, *notice, 0.75f)
                    .found) {
                say("game không nhận và hiện một thông báo; để nguyên cho người chơi đọc");
                keep(f, "refused");
                return kFailed;
            }
        }

        // The bin may ask again -- the question after the quantity, or the
        // other way round. Only its own dialog is answered.
        bool foreign = false;
        const bag::Dialog next = wait_for_dialog(900, foreign);
        if (foreign) {
            say("sau khi bấm Xác Nhận game hiện một hộp thoại khác, nên không bấm");
            return kFailed;
        }
        if (!next.ours()) return kDone;
        d = next;
    }
    say("thùng rác hỏi quá nhiều lần, nên không bấm nữa");
    return kFailed;
}

bool BagCleaner::trash_one(const Frame& f, const BagView& v, int index, int count) {
    const BagLayout& l = settings_.layout;
    const auto slots = v.slots(l);
    if (index < 0 || index >= static_cast<int>(slots.size())) return false;
    const ScreenRect s = slots[static_cast<std::size_t>(index)];
    const Frame before = bag::cut(f, s);

    // 1. Onto the bin. It takes the item and waits, showing it where the can
    //    was, and leaves an enlarged copy in the slot.
    if (!mouse_ok(s.cx(), s.cy()) || !mouse_ok(v.trash.cx(), v.trash.cy())) return false;
    item_out_ = true;                // until it is seen gone, or given back
    bool moved = false;
    if (settings_.click_to_move)
        moved = click(s.cx(), s.cy()) && wait_ms(220) &&
                click(v.trash.cx(), v.trash.cy());
    else
        moved = drag(s.cx(), s.cy(), v.trash.cx(), v.trash.cy());
    if (!moved) {
        say("không điều khiển được chuột trong cửa sổ game");
        back_out();
        return false;
    }

    Frame g;
    BagView w;
    bool waiting = false;
    for (int i = 0; i < 8 && !waiting; ++i) {
        if (!wait_ms(150)) {
            back_out();
            return false;
        }
        waiting = grab_view(g, w) && !w.bin_empty && bag::find_dialogs(g, store_).empty();
    }
    if (!waiting) {
        say("món đồ không vào thùng rác");
        keep(g, "not_in_bin");
        back_out();
        return false;
    }

    // 2. A click on the bin, which is what asks.
    if (!mouse_ok(w.trash.cx(), w.trash.cy())) {
        back_out();
        return false;
    }
    click(w.trash.cx(), w.trash.cy());
    bool foreign = false;
    Frame shot;
    const bag::Dialog d = wait_for_dialog(2100, foreign, &shot);
    if (!d.ours()) {
        say(foreign ? "hộp thoại hiện ra không phải của thùng rác, nên không bấm"
                    : "bấm vào thùng rác mà không thấy hộp thoại xác nhận");
        keep(shot, foreign ? "wrong_dialog" : "no_dialog");
        back_out();
        return false;
    }

    // 3. The answer. Whatever goes wrong, the bin's dialog is said no to and
    //    the item goes back to the bag.
    if (answer(d, count) != kDone) {
        back_out();
        return false;
    }

    // 4. Only gone counts: the can back in the bin, none of the bin's dialogs
    //    up, and the slot no longer holding what it held.
    wait_ms(300);
    if (!grab_view(g, w)) return false;
    bool asking = false;
    for (const bag::Dialog& each : bag::find_dialogs(g, store_)) asking = asking || each.ours();
    if (!w.bin_empty || asking) {
        say("thùng rác vẫn còn món đồ");
        keep(g, "still_in_bin");
        back_out();
        return false;
    }
    // Nothing of it in the bin or in a dialog any more: gone, or back in the
    // bag, which is all the rest of this tells apart.
    if (bag::find_dialogs(g, store_).empty()) item_out_ = false;
    if (v.scroll_known() && w.scroll_known() && w.first_row != v.first_row) {
        say("trang túi đồ đã đổi, nên không biết món đồ còn hay không");
        return false;
    }
    const auto now = w.slots(l);
    if (static_cast<std::size_t>(index) >= now.size()) return false;
    const ScreenRect same = now[static_cast<std::size_t>(index)];
    if (!bag::is_empty(g, same, l) && bag::alike(bag::cut(g, same), before, l) >= kStillThere) {
        say("món đồ vẫn còn trong túi");
        keep(g, "still_there");
        return false;
    }
    return true;
}

} // namespace gobot
