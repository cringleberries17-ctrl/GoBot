#include "gobot/bag.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <future>
#include <map>
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

/// The dark panel of words in the bin's pictures, from their top-left. Every
/// dialog's frame and buttons are the same, so the words are what tells them
/// apart: matched here, the bin's question scores 0.24 at most against any
/// other dialog's words and 1.000 against itself on the live client.
constexpr ScreenRect kBinWords{ 20, 12, 158, 28 };

/// The bin's two dialogs, where the pair of buttons sits in each picture, and
/// its panel of words.
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
    ScreenRect  words;              ///< the panel of words, from the picture's top-left
};
const DialogPicture kBinDialogs[] = {
    { "grabbed/del_item_confirm_1", 101, 51, false, kBinWords },   // bag::kAskDialog
    { "grabbed/del_item_confirm_2", 100, 50, true, kBinWords },    // bag::kCountDialog
};

/// The question a fragment's right-click brings up, as the live client draws
/// it (bag::kRegisterDialog): the game's dialog frame a line taller than the
/// bin's question, its three lines of words in the panel's inside, 168x41 from
/// 17,11 of the picture, and its two buttons at 105,56, drawn as the bin's
/// question draws its own pair -- found there at 1.000 on the 96 frames of the
/// live client with the question and its buttons at rest or under the
/// pointer. On those its words, looked for there, score 1.000; on the 137
/// other frames with a dialog's two buttons, under 0.3; and in the other
/// questions' pictures (kOtherQuestions) 0.30 at most. Never one of the bin's
/// dialogs.
const DialogPicture kRegisterPicture = { bag::kRegisterDialog, 105, 56, false, ScreenRect{ 17, 11, 168, 41 } };

/// Every other question GoBot has a picture of, all in the game's one dialog
/// frame over [Xác Nhận][Hủy Bỏ]: the bin's two, the revive question -- "Bạn
/// có muốn sử dụng…", which a question about using an item would open with as
/// well -- and the drop and sell questions. The registration's picture is used
/// only when its words are found in none of them.
const char* const kOtherQuestions[] = { "grabbed/del_item_confirm_1", "grabbed/del_item_confirm_2",
                                        "grabbed/hoisinh_confirm", "grabbed/drop_confirm",
                                        "grabbed/sell_confirm" };

/// How long a right-click on a fragment is given to bring up its question,
/// and how long its question, once answered, to go.
constexpr int kUseAskMs = 1500;
constexpr int kUseCloseMs = 1800;
/// How long after a right-click that brought nothing up the use sweep waits,
/// and looks again, before it right-clicks anything else: a question that
/// comes up late, the game lagging, is then seen for the one it is, not taken
/// for the next piece's.
constexpr int kUseLateMs = 1500;

/// A close button, as the popup closer finds one: each drawing at 0.90, and
/// the bag's own within a few pixels of where the layout puts it.
constexpr float kCloseScore = 0.90f;
constexpr int kCloseSlack = 4;

/// Bright pixels a panel needs to be words at all. The pictures of the drop
/// question and of the notice have an empty panel -- ten or so -- and would
/// pass for any dialog.
constexpr int kWordsAtLeast = 100;
constexpr float kWordsScore = 0.90f;
/// How alike the registration question's words have to be for Xác Nhận:
/// stricter than the bin's pictures are held to. Its words are the same for
/// every piece and are all there is to tell it by, and the player's rule is
/// that only this question, with these words, is confirmed. On every frame of
/// the live client with the question up its words score 1.000, while the same
/// box with one letter taken out scores 0.988, its third line gone 0.945, and
/// a short word of its first line in other letters 0.947 -- all of which
/// kWordsScore would take for it.
constexpr float kRegisterWordsScore = 0.99f;

/// The monster book (bag::find_book): how alike its tab has to be, and its X.
/// The tab scores 1.00 on every saved frame with the book open and 0.58 at
/// most anywhere on the rest; the X 1.00 where the tab says, and 0.94 with
/// the pointer over it.
constexpr float kBookScore = 0.90f;
constexpr float kBookXScore = 0.85f;
/// How far from where it is expected the X may be found, and how far from
/// where it was a book looked at again may be.
constexpr int kBookSlack = 3;
/// How long a click on the book's X is given to close it, and how often it is
/// looked at meanwhile; and how many looks the X itself is looked for in
/// before the book counts as one that cannot be closed.
constexpr int kBookCloseMs = 1500;
constexpr int kBookLookMs = 150;
constexpr int kBookXLooks = 3;

/// How alike the strip has to be. Lower than a fixed button: whichever tab is
/// selected is drawn differently, and the picture was taken with the first.
constexpr float kStripScore = 0.62f;
/// How much darker than every other tab the chosen one is, at least, in mean
/// grey. Measured 16 to 23 darker on the live client and in its own picture.
constexpr int kChosenTabDarker = 10;
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

/// The bright pixels of a panel of words that the picture shows. A part made
/// see-through -- a fragment's name, which may differ from question to
/// question while the rest stays the same -- counts no more here than it does
/// in the match.
int bright_words(const Frame& words) {
    int bright = 0;
    for (std::size_t i = 0; i < words.pixels.size(); i += 4)
        if (words.pixels[i + 3] >= TemplateMatcher::kOpaque &&
            (words.pixels[i] + words.pixels[i + 1] + words.pixels[i + 2]) / 3 > 150)
            ++bright;
    return bright;
}

/// How alike the screen is to bin dialog `pic`'s panel of words, where that
/// panel would sit for a pair of buttons at `at`: the best within two pixels,
/// when `least` or better, else -1. -1 too for a picture that is not loaded
/// or has no words to tell anything apart by.
float words_at(const Frame& f, const TemplateStore& store, const ScreenRect& at,
               const DialogPicture& pic, float least) {
    const Frame* t = store.get(pic.name);
    if (t == nullptr) return -1.0f;
    const Frame words = crop(*t, pic.words);
    if (!words.valid()) return -1.0f;
    if (bright_words(words) < kWordsAtLeast) return -1.0f;     // no words: tells nothing apart

    // Right where they sit relative to the buttons, give or take two pixels,
    // and nowhere else.
    const ScreenRect place = clamp_to(f, ScreenRect{ at.x - pic.pair_dx + pic.words.x - 2,
                                                     at.y - pic.pair_dy + pic.words.y - 2,
                                                     pic.words.w + 4, pic.words.h + 4 });
    if (place.w < words.width || place.h < words.height) return -1.0f;
    const auto m = TemplateMatcher::find(f, place, words, least);
    return m.found ? m.score : -1.0f;
}

/// The oldest PNGs of one kind in `dir` -- a probe's frames, or the rest --
/// deleted until `keep` are left. Named by when they were taken, they sort
/// oldest first.
void prune_pngs(const std::string& dir, bool probes, std::size_t keep) {
    std::vector<std::string> old;
    for (const std::string& name : files_in(dir, "*.png")) {
        const bool probe = name.rfind("probe_", 0) == 0;
        if (probe == probes) old.push_back(name);
    }
    std::sort(old.begin(), old.end());
    for (std::size_t i = 0; i + keep < old.size(); ++i)
        remove_file(dir + "\\" + old[i]);
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
    close_dx        = ini.get_int("Bag", "close_dx", close_dx);
    close_dy        = ini.get_int("Bag", "close_dy", close_dy);
    count_w         = ini.get_int("Bag", "count_w", count_w);
    count_h         = ini.get_int("Bag", "count_h", count_h);
    edge            = ini.get_int("Bag", "edge", edge);
    badge_w         = ini.get_int("Bag", "badge_w", badge_w);
    badge_h         = ini.get_int("Bag", "badge_h", badge_h);
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

    // Which tab is chosen: the client draws it darker than the others, its
    // inside 92 to 99 of 255 where theirs are 115 to 123. Only when one stands
    // out from all the rest by that much; a strip half covered says nothing.
    {
        int mean[kBagTabs] = {};
        bool readable = true;
        for (int t = 0; t < kBagTabs && readable; ++t) {
            const ScreenRect inside{ v.strip.x + t * l.tab_w + 2, v.strip.y + 2, l.tab_w - 4, v.strip.h - 4 };
            if (inside.x < 0 || inside.y < 0 || inside.right() > f.width || inside.bottom() > f.height ||
                !inside.valid()) {
                readable = false;
                break;
            }
            long sum = 0;
            for (int y = inside.y; y < inside.bottom(); ++y)
                for (int x = inside.x; x < inside.right(); ++x) sum += lum(f, x, y);
            mean[t] = static_cast<int>(sum / (static_cast<long>(inside.w) * inside.h));
        }
        if (readable) {
            int darkest = 0;
            for (int t = 1; t < kBagTabs; ++t)
                if (mean[t] < mean[darkest]) darkest = t;
            bool stands_out = true;
            for (int t = 0; t < kBagTabs; ++t)
                if (t != darkest && mean[t] - mean[darkest] < kChosenTabDarker) stands_out = false;
            if (stands_out) v.selected_tab = darkest;
        }
    }

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
            const bool in_badge = x >= out.width - l.badge_w && y >= out.height - l.badge_h;
            out.pixels[(static_cast<std::size_t>(y) * out.width + x) * 4 + 3] =
                (at_edge || in_count || in_badge) ? 0 : 255;
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
    // The ring over the digit's cell: an unlit pixel next to a lit one, side
    // or corner.
    const int cw = std::max(w, kCellW);
    const auto lit_at = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < w && y < h && on[static_cast<std::size_t>(y) * w + x] != 0;
    };
    g.ring.assign(static_cast<std::size_t>(cw) * h, 0);
    g.ring_n = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < cw; ++x) {
            if (lit_at(x, y)) continue;
            bool next_to = false;
            for (int dy = -1; dy <= 1 && !next_to; ++dy)
                for (int dx = -1; dx <= 1 && !next_to; ++dx) next_to = lit_at(x + dx, y + dy);
            if (!next_to) continue;
            g.ring[static_cast<std::size_t>(y) * cw + x] = 1;
            ++g.ring_n;
        }
    loaded_ = true;
    for (const Glyph& each : digits_)
        if (each.w == 0) loaded_ = false;
}

int CountReader::scan(const Frame& f, const ScreenRect& area, int max_top, bool caret, bool cells,
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
    //
    // In `cells` the box is the digit's cell, and of it only the digit's
    // strokes and the ring round them count: what else is in the cell is
    // the icon under the count, which may be white. A 1 is 3 pixels of a
    // 5-pixel cell; over its own 3 columns alone it had room for two stray
    // white pixels, and the egg's rim put three there.
    const auto fit = [&](int d, int x, int y, double& agree, double& strokes, bool& hidden) {
        const Glyph& g = digits_[d];
        agree = strokes = 0.0;
        hidden = false;
        const int cw = cells ? std::max(g.w, kCellW) : g.w;
        if (x + cw > kW || y + g.h > kH || g.lit == 0) return;
        int seen = 0, seen_lit = 0, same = 0, hit = 0;
        for (int gy = 0; gy < g.h; ++gy)
            for (int gx = 0; gx < cw; ++gx) {
                const bool want = gx < g.w && g.on[static_cast<std::size_t>(gy) * g.w + gx] != 0;
                if (cells && !want && g.ring[static_cast<std::size_t>(gy) * cw + gx] == 0) continue;
                const std::uint8_t have = map[static_cast<std::size_t>(y + gy) * kW + x + gx];
                if (have == kUnknown) continue;
                ++seen;
                if (want) ++seen_lit;
                if (want == (have == kStroke)) ++same;
                if (want && have == kStroke) ++hit;
            }
        const int box = cells ? g.lit + g.ring_n : g.w * g.h;
        hidden = seen * 4 < box * 3 || seen_lit * 4 < g.lit * 3;
        if (seen == 0 || seen_lit == 0) return;
        agree = static_cast<double>(same) / seen;
        strokes = static_cast<double>(hit) / seen_lit;
    };
    // What a digit's box has to agree by. In cells a little less: the ring
    // is the outline's own pixels, where the client's soft edge now and then
    // lifts one into white -- the strokes are held to as much as ever.
    const double agree_least = cells ? 0.85 : 0.88;

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
                if (agree < agree_least || strokes < 0.88) continue;
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
                // The next cell: the client's digits are kCellW + 1 apart, a
                // 1 too, in a slot's corner.
                x += (cells ? std::max(digits_[found].w, kCellW) : digits_[found].w) + 1;
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
    const int n = scan(f, ScreenRect{ slot.x, slot.y, 28, 16 }, 6, false, true, white, stray);
    if (white == 0) return 1;                 // nothing printed: a stack of one
    if (n <= 0 || n > 9999) return kUnreadable;
    return n;
}

int CountReader::read_field(const Frame& f, const ScreenRect& area) const {
    if (!loaded_ || !f.valid()) return kUnreadable;
    int white = 0, stray = 0;
    const int n = scan(f, area, std::max(0, area.h - 8), true, false, white, stray);
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

/// An icon as another icon is compared with it: its own transparency, less
/// the corner the count is printed over. The icon sits about two pixels into
/// the slot, so the slot's count corner is this much of the icon.
Frame icon_drawing(const ItemCatalog::Entry& e) {
    Frame p = e.icon;
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x)
            if (x < 20 && y < 11) p.pixels[(static_cast<std::size_t>(y) * p.width + x) * 4 + 3] = 0;
    return p;
}

/// How much of an icon's bottom-right corner a badge in its slot can cover.
/// The client draws an icon in the middle of its slot, a pixel right of the
/// middle -- measured on every real frame, on icons 30 to 34 wide and 26 to
/// 34 tall -- so this much of any icon up to the slot's size holds all of
/// the slot's badge corner (BagLayout::badge_w) that lies over it: to the
/// pixel for an icon of 34 by 34, with room to spare for anything smaller.
constexpr int kIconBadge = 12;

/// An icon as a slot is matched against it: less the badge corner as well,
/// which a slot can have and an icon never does.
///
/// Not for deciding which drawings are one, neither icon against icon nor
/// icon against a picture cut from the bag: two drawings that differ mostly
/// in that corner are two things. Compared without it, the plain and the
/// decorated gourd potions -- 8820011 and 8820013, and their blue twins --
/// came out as one, at 0.85 icon against icon, and a picture of one took the
/// other's slots at 0.91. Decided with it, they are two, and the one not
/// listed is a rival in the bag that matches its own slots better than the
/// listed one does -- which is what leaves them alone, since with the corner
/// left out the listed one still scores over min_score there.
Frame icon_picture(const ItemCatalog::Entry& e) {
    Frame p = icon_drawing(e);
    for (int y = std::max(0, p.height - kIconBadge); y < p.height; ++y)
        for (int x = std::max(0, p.width - kIconBadge); x < p.width; ++x)
            p.pixels[(static_cast<std::size_t>(y) * p.width + x) * 4 + 3] = 0;
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

/// One of the client's fragment icons, the only catalog items that are
/// registered: GoC ships nine, spend/fragment/1103013 to 1103093.
bool is_fragment(const std::string& id) {
    return id.rfind("spend/fragment/", 0) == 0;
}

/// A picture cut from the bag as an icon is compared with it: masked like the
/// picture it is matched by, but with the badge corner kept, as icon_drawing
/// keeps it. See icon_picture for why.
Frame cut_drawing(const Frame& cut, const BagLayout& l) {
    BagLayout with_corner = l;
    with_corner.badge_w = 0;
    with_corner.badge_h = 0;
    return bag::masked(cut, with_corner);
}

void add_kin(TrashList::Item& item, const std::string& id) {
    if (std::find(item.kin.begin(), item.kin.end(), id) == item.kin.end()) item.kin.push_back(id);
}

} // namespace

std::string ItemCatalog::default_dir() {
    const std::string staged = TemplateStore::default_dir();
    if (!staged.empty() && path_exists(staged + "\\spend"))
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

        for (const std::string& file : files_in(dir, "*.png")) {
            Entry e;
            if (!load_png(dir + "\\" + file, e.icon) || !e.icon.valid()) continue;
            e.id = std::string(k.folder) + "/" + file.substr(0, file.size() - 4);
            e.tile = slot_tile(e.icon);
            group.entries.push_back(std::move(e));
        }
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
    // Icon against icon, so with the badge corner: see icon_picture.
    const Frame mine = icon_drawing(*me);
    for (const Group& g : groups)
        for (const Entry& e : g.entries)
            if (e.id != id && e.tile.valid() && one_drawing(mine, me->tile, icon_drawing(e), e.tile))
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
    // Not below 0.85: in grey, two different fragments score up to 0.73 on a
    // real slot and two different potions 0.77, and a hand-edited 0.70 would
    // let those through wherever the catalog has nothing better to say.
    min_score        = std::clamp(ini.get_float("Trash", "min_score", min_score), 0.85f, 0.99f);
    max_per_run      = std::clamp(ini.get_int("Trash", "max_per_run", max_per_run), 1, 500);
    max_uses_per_run = std::clamp(ini.get_int("Trash", "max_uses_per_run", max_uses_per_run), 1, 100);
    // Read as the file has it, or off: never left as an earlier reading had
    // it, which would keep it on after the line was taken out of the file.
    use_fragments    = ini.get_bool("Trash", "use_fragments", false);
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
    use_ids    = ids("use");
    layout.load(ini);
    return true;
}

bool CleanSettings::save_items(const std::string& path, const std::function<void()>& between) const {
    const auto join = [](const std::vector<std::string>& ids) {
        std::string out;
        for (const std::string& id : ids) {
            if (!out.empty()) out += ',';
            out += id;
        }
        return out;
    };
    // What use= holds on disk now, which a pass may be going by: every id
    // that leaves it stays on it until delete= is as it will be.
    std::vector<std::string> both = use_ids;
    CleanSettings on_disk;
    if (on_disk.load(path))
        for (const std::string& id : on_disk.use_ids)
            if (std::find(both.begin(), both.end(), id) == both.end()) both.push_back(id);
    const auto step = [&](bool written) {
        if (written && between) between();
        return written;
    };
    return step(IniFile::set_in_place(path, "Items", "use", join(both))) &&
           step(IniFile::set_in_place(path, "Items", "delete", join(delete_ids))) &&
           step(IniFile::set_in_place(path, "Items", "drop", join(drop_ids))) &&
           IniFile::set_in_place(path, "Items", "use", join(use_ids));
}

// ------------------------------------------------------------------ list

const char* TrashList::folder(Action a) {
    switch (a) {
        case Action::Drop: return "drop";
        case Action::Use:  return "use";
        case Action::Delete: break;
    }
    return "delete";
}

bool TrashList::load(const std::string& dir, const BagLayout& l) {
    items.clear();
    rivals.clear();
    // Use first, for the tab to show them so. Which of them takes a slot is
    // not the order's doing: judge gives a slot any item on the Dùng list
    // matches to that item, so a picture put in two folders by hand -- or
    // one cut for the delete list and a fragment chosen later to use -- is
    // registered rather than deleted, which cannot be had back the other way.
    for (const Action a : { Action::Use, Action::Delete, Action::Drop }) {
        const std::string sub = dir + "\\" + folder(a);
        if (!path_exists(sub)) continue;

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
            item.drawing = cut_drawing(item.shown, l);
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
    make_folder(dir);
    make_folder(sub);

    // Named by when, which is unique enough for something a person clicks.
    std::string path;
    for (int n = 0; n < 100; ++n) {
        path = sub + "\\item_" + stamp() + "_" + std::to_string(n) + ".png";
        if (!path_exists(path)) break;
    }
    if (!save_png(cut, path)) return {};

    Item item;
    item.file = path;
    item.action = a;
    item.shown = cut;
    item.picture = bag::masked(cut, l);
    item.drawing = cut_drawing(cut, l);
    items.push_back(std::move(item));
    return path;
}

void TrashList::add_picture(const Frame& cut, Action a, const BagLayout& l) {
    Item item;
    item.action = a;
    item.shown = cut;
    item.picture = bag::masked(cut, l);
    item.drawing = cut_drawing(cut, l);
    items.push_back(std::move(item));
}

void TrashList::add_catalog(const ItemCatalog& catalog, const std::vector<std::string>& ids,
                            Action a, float min_score) {
    const std::size_t first = items.size();
    for (const std::string& id : ids) {
        const ItemCatalog::Entry* e = catalog.find(id);
        if (e == nullptr || listed(items, id)) continue;
        // Only fragments are registered from the catalog: a right-click on
        // anything else may use it, or equip it, there and then. Nor does a
        // drawing in another group come along with one -- it stays a rival,
        // and a slot that looks as much like it is left alone.
        if (a == Action::Use && !is_fragment(id)) continue;
        Item item = catalog_item(*e, a, min_score);
        item.kin = catalog.same_drawing(id);
        if (a == Action::Use)
            item.kin.erase(std::remove_if(item.kin.begin(), item.kin.end(),
                                          [](const std::string& k) { return !is_fragment(k); }),
                           item.kin.end());
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
    // Learned afresh: what an earlier call learned would be taken below for
    // what the comparison found, and its colours added on top of it.
    for (Item& it : items)
        if (it.id.empty()) it.kin.clear();
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
            //
            // Decided the way same_drawing decides it for two icons, with the
            // badge corner compared on both sides: see icon_picture. Left out,
            // the plain and the decorated gourds came out as one drawing, and
            // a picture of either took the other's slots.
            const Frame icon = icon_drawing(e);
            const Frame wide = slot_tile(e.icon, 40);
            for (Item& it : items) {
                if (!it.id.empty() || !it.shown.valid() || !it.drawing.valid()) continue;
                if (one_drawing(icon, wide, it.drawing, it.shown)) add_kin(it, e.id);
            }
        }

    // And each of those icons' drawing in its other colours, as choosing it
    // from the catalog would bring. A badge over the picture's corner can hold
    // the picture under kSameDrawing of some of them -- a buff with a P on it,
    // of the same buff in another colour -- and as rivals those would tie with
    // it on every slot holding it.
    std::map<std::string, std::vector<std::string>> colours;
    for (Item& it : items) {
        if (!it.id.empty()) continue;
        const std::vector<std::string> found = it.kin;
        for (const std::string& id : found) {
            auto c = colours.find(id);
            if (c == colours.end()) c = colours.emplace(id, catalog.same_drawing(id)).first;
            for (const std::string& other : c->second) add_kin(it, other);
        }
    }
}

void TrashList::build(const std::string& dir, const CleanSettings& s, const ItemCatalog& catalog) {
    load(dir, s.layout);
    // The Dùng list first, so an id on it and on the delete list as well is
    // one item, used and then deleted, rather than a deletion that comes
    // first and leaves nothing to register.
    add_catalog(catalog, s.use_ids, Action::Use, s.catalog_score);
    add_catalog(catalog, s.delete_ids, Action::Delete, s.catalog_score);
    add_catalog(catalog, s.drop_ids, Action::Drop, s.catalog_score);
    const auto on_delete = [&](const std::string& id) {
        return !id.empty() && std::find(s.delete_ids.begin(), s.delete_ids.end(), id) != s.delete_ids.end();
    };
    for (Item& it : items)
        if (it.action == Action::Use && (on_delete(it.id) || on_delete(it.via))) it.then_delete = true;
    add_rivals(catalog);
}

bool TrashList::remove(std::size_t index) {
    if (index >= items.size()) return false;
    if (!items[index].file.empty()) remove_file(items[index].file);
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

TrashList::Verdict TrashList::judge(const Frame& f, const ScreenRect& slot,
                                    float min_score) const {
    Verdict v;
    const ScreenRect area = clamp_to(f, ScreenRect{ slot.x - 3, slot.y - 3,
                                                    slot.w + 6, slot.h + 6 });
    // The drawing alone, in grey, without the count's corner or a badge's.
    // Measured on every real frame, the right icon scores 0.947 or more on
    // its item in any colour the bag draws it -- gold, orange, the red of
    // GoC's own pictures -- and 0.99 on a buff with a badge over it, the best
    // other drawing 0.77 at most, and the enlarged copy a slot shows while
    // its item waits in the bin 0.77 at most.
    int use = -1;                   // the best of the items on the Dùng list that match
    float use_score = 0.0f;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& item = items[i];
        const Frame& p = item.picture;
        if (!p.valid() || area.w < p.width || area.h < p.height) continue;
        const float need = item.min_score > 0.0f ? item.min_score : min_score;
        const auto m = TemplateMatcher::find(f, area, p, need, TemplateMatcher::Tone::Grey);
        if (!m.found) continue;
        if (m.score > v.score) {
            v.item = static_cast<int>(i);
            v.score = m.score;
        }
        if (item.action == Action::Use && m.score > use_score) {
            use = static_cast<int>(i);
            use_score = m.score;
        }
    }
    if (v.item < 0) return v;

    // Registered rather than deleted: a slot an item on the Dùng list matches
    // is that item's, whatever on the delete or drop list matches it better.
    // A picture cut from the bag scores 1.00 on its own slot, and a catalog
    // icon 0.947 or more on the same drawing -- so a fragment chosen to use
    // from the catalog, with an older cut of it on the delete list, would
    // otherwise be deleted whole on every pass and never registered. What is
    // left of it after the registration goes only when the Dùng item says so
    // (then_delete). Any doubt below leaves the slot alone, never falls back
    // to the deletion.
    if (use >= 0 && use != v.item) {
        v.passed_over = v.item;
        v.passed_over_score = v.score;
        v.item = use;
        v.score = use_score;
    }

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

std::vector<std::size_t> TrashList::yielding_to_use() const {
    // What the Dùng list stands for in catalog icons: each item's own id, and
    // the icons it is drawn like -- a picture cut from the bag has only those.
    std::vector<std::string> use;
    for (const Item& it : items) {
        if (it.action != Action::Use) continue;
        if (!it.id.empty()) use.push_back(it.id);
        use.insert(use.end(), it.kin.begin(), it.kin.end());
    }
    const auto on_use = [&](const std::string& id) { return std::find(use.begin(), use.end(), id) != use.end(); };
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        if (it.action == Action::Use) continue;
        if ((!it.id.empty() && on_use(it.id)) || std::any_of(it.kin.begin(), it.kin.end(), on_use))
            out.push_back(i);
    }
    return out;
}

bool TrashList::Item::fragment() const {
    // A catalog icon is one by its id. A picture cut from the bag is one by
    // what add_rivals found it drawn like: fragment icons, and nothing else --
    // a picture drawn like no icon at all, or like a potion as well, is not.
    if (!id.empty() && !is_fragment(id)) return false;
    if (kin.empty()) return false;
    return std::all_of(kin.begin(), kin.end(), [](const std::string& k) { return is_fragment(k); });
}

namespace bag {

bool fragment_cut(const Frame& cut, const BagLayout& l, const ItemCatalog& catalog) {
    // The very list a pass would build with this picture on the Dùng list,
    // and the very test the pass puts it to.
    TrashList one;
    one.add_picture(cut, TrashList::Action::Use, l);
    one.add_rivals(catalog);
    return !one.items.empty() && one.items.front().fragment();
}

} // namespace bag

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

    const bool can_register = store.get(kRegisterPicture.name) != nullptr;
    for (const ScreenRect& at : found) {
        Dialog d;
        d.pair = at;
        for (int k = 0; k < static_cast<int>(std::size(kBinDialogs)); ++k) {
            const DialogPicture& pic = kBinDialogs[k];
            const float words = words_at(f, store, at, pic, kWordsScore);
            if (words >= kWordsScore && words > d.words) {
                d.kind = k;
                d.words = words;
                d.quantity = pic.quantity;
            }
        }
        if (d.quantity)
            d.box = ScreenRect{ at.x + kBoxDx, at.y + kBoxDy, kBoxW, std::max(4, at.h - 6) };
        // The registration question, by its words, at kRegisterWordsScore --
        // the best place within two pixels looked for at that score, so that
        // a nearer miss found first is not taken for the answer -- and never
        // a dialog the bin's pictures took for one of the bin's: a dialog is
        // one thing. How near words that fall short came, from kWordsScore,
        // is kept for the log and the tests.
        if (can_register && !d.ours()) {
            const float words = words_at(f, store, at, kRegisterPicture, kRegisterWordsScore);
            d.registers = words >= kRegisterWordsScore;
            d.register_words =
                d.registers ? words : std::max(0.0f, words_at(f, store, at, kRegisterPicture, kWordsScore));
        }
        out.push_back(d);
    }
    return out;
}

std::vector<float> words_scores(const Frame& f, const TemplateStore& store, const Dialog& d) {
    std::vector<float> out;
    for (const DialogPicture& pic : kBinDialogs) out.push_back(words_at(f, store, d.pair, pic, -1.0f));
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
    const Frame* pair = store.get(kConfirm);
    if (pair == nullptr) return false;
    if (a == TrashList::Action::Drop) return false;      // see the header
    for (const DialogPicture& pic : kBinDialogs)
        if (store.get(pic.name) == nullptr) return false;
    if (a != TrashList::Action::Use) return true;

    // The registration question: a picture with words to tell it by, and its
    // two buttons where its own layout has them -- or its words would be
    // looked for in the wrong place, and never match.
    const Frame* ask = store.get(kRegisterPicture.name);
    if (ask == nullptr) return false;
    const Frame words = crop(*ask, kRegisterPicture.words);
    if (!words.valid()) return false;
    if (bright_words(words) < kWordsAtLeast) {
        GB_INFO("Trash: %s is not used: too few words are left in it to tell a question by", kRegisterPicture.name);
        return false;
    }
    const ScreenRect buttons = clamp_to(*ask, ScreenRect{ kRegisterPicture.pair_dx - 2, kRegisterPicture.pair_dy - 2,
                                                          pair->width + 4, pair->height + 4 });
    if (buttons.w < pair->width || buttons.h < pair->height) return false;
    // In either drawing of the two buttons find_dialogs knows them by: the
    // store's, and the bin's question's own, a pixel closer together.
    bool placed = TemplateMatcher::find(*ask, buttons, *pair, kButtonScore).found;
    if (!placed) {
        const Frame* bin_ask = store.get(kBinDialogs[kAskDialog].name);
        if (bin_ask == nullptr) return false;
        const Frame other = crop(*bin_ask, ScreenRect{ kBinDialogs[kAskDialog].pair_dx, kBinDialogs[kAskDialog].pair_dy,
                                                       pair->width, pair->height });
        placed = other.valid() && TemplateMatcher::find(*ask, buttons, other, kButtonScore).found;
    }
    if (!placed) {
        GB_INFO("Trash: %s is not used: its two buttons are not where a question has them", kRegisterPicture.name);
        return false;
    }

    // And words that no other question has. Cut down to what stays the same
    // from piece to piece, the name see-through, what is left could be no more
    // than the "Bạn có muốn" every question opens with -- and then the revive
    // question, or the bin's, would be confirmed as a registration. Searched
    // for anywhere in each picture, not only where its words sit.
    for (const char* name : kOtherQuestions) {
        const Frame* other = store.get(name);
        if (other == nullptr || other->width < words.width || other->height < words.height) continue;
        if (TemplateMatcher::find(*other, ScreenRect{ 0, 0, other->width, other->height }, words, kWordsScore)
                .found) {
            GB_INFO("Trash: %s is not used: its words are found in %s as well", kRegisterPicture.name, name);
            return false;
        }
    }
    return true;
}

bool notice_up(const Frame& f, const TemplateStore& store) {
    // The wordless picture of a one-button message matches the bin's own
    // question at 0.84, so only with no dialog of two buttons up.
    const Frame* notice = store.get(kNotice);
    if (notice == nullptr || !f.valid() || !find_dialogs(f, store).empty()) return false;
    return TemplateMatcher::find(f, ScreenRect{ 0, 0, f.width, f.height }, *notice, 0.75f).found;
}

bool can_see_windows(const TemplateStore& store) {
    for (const char* name : kCloseButtons)
        if (store.get(name) == nullptr) return false;
    return true;
}

bool other_window_up(const Frame& f, const TemplateStore& store, const BagView& v, const BagLayout& l,
                     ScreenRect* at, const ScreenRect* also_own) {
    if (!can_see_windows(store) || !f.valid()) return true;
    // Each drawing on its own thread: matched through their see-through
    // corners, these are the dearest searches of a look, and the game is held
    // while they run.
    const ScreenRect all{ 0, 0, f.width, f.height };
    std::vector<std::future<std::vector<TemplateMatcher::Match>>> looks;
    for (const char* name : kCloseButtons) {
        const Frame* x = store.get(name);
        looks.push_back(std::async(std::launch::async, [&f, all, x] {
            return f.width >= x->width && f.height >= x->height ? TemplateMatcher::find_all(f, all, *x, kCloseScore, 16)
                                                                : std::vector<TemplateMatcher::Match>{};
        }));
    }
    // The bag's own X, where it always is: any other is a window's that a
    // right-click might hand the item to -- but for `also_own`, which the
    // caller knows for one that is not.
    bool other = false;
    for (auto& look : looks)
        for (const TemplateMatcher::Match& m : look.get()) {
            const bool own = (v.found && std::abs(m.at.x - (v.strip.x + l.close_dx)) <= kCloseSlack &&
                              std::abs(m.at.y - (v.strip.y + l.close_dy)) <= kCloseSlack) ||
                             (also_own != nullptr && std::abs(m.at.x - also_own->x) <= kCloseSlack &&
                              std::abs(m.at.y - also_own->y) <= kCloseSlack);
            if (own || other) continue;
            other = true;
            if (at) *at = m.at;
        }
    return other;
}

bool can_see_book(const TemplateStore& store) {
    return store.get(kBookTitle) != nullptr && store.get(kBookClose) != nullptr;
}

Book find_book(const Frame& f, const TemplateStore& store, const ScreenRect* window) {
    Book b;
    const Frame* title = store.get(kBookTitle);
    const Frame* x = store.get(kBookClose);
    if (title == nullptr || x == nullptr || !f.valid()) return b;
    // The tab anywhere, or where a book with `window` has it.
    const ScreenRect look =
        window == nullptr
            ? ScreenRect{ 0, 0, f.width, f.height }
            : clamp_to(f, ScreenRect{ window->x - kBookWindow.x - kBookSlack, window->y - kBookWindow.y - kBookSlack,
                                      title->width + 2 * kBookSlack, title->height + 2 * kBookSlack });
    if (look.w < title->width || look.h < title->height) return b;
    const auto t = TemplateMatcher::find(f, look, *title, kBookScore);
    if (!t.found) return b;
    b.found = true;
    b.score = t.score;
    b.title = t.at;
    b.window = ScreenRect{ t.at.x + kBookWindow.x, t.at.y + kBookWindow.y, kBookWindow.w, kBookWindow.h };
    // Its X where the tab says, and nowhere else: every close button of the
    // client is one drawing, so an X is the book's only by where it is.
    const ScreenRect around = clamp_to(f, ScreenRect{ t.at.x + kBookXDx - kBookSlack, t.at.y + kBookXDy - kBookSlack,
                                                      x->width + 2 * kBookSlack, x->height + 2 * kBookSlack });
    if (around.w >= x->width && around.h >= x->height) {
        const auto m = TemplateMatcher::find(f, around, *x, kBookXScore);
        if (m.found) {
            b.has_x = true;
            b.x_score = m.score;
            b.x = m.at;
        }
    }
    return b;
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
    make_folder(debug_dir_);

    // A few dozen at most: the newest are the ones that matter. A probe's
    // frames are a record of their own, kept apart from these: see probe.
    prune_pngs(debug_dir_, false, 29);

    static int n = 0;
    const std::string path = debug_dir_ + "\\" + stamp() + "_" + std::to_string(n++ % 100) +
                             "_" + what + ".png";
    if (save_png(f, path)) GB_INFO("Trash: kept the screen as %s", path.c_str());
}

void BagCleaner::say(const std::string& line) {
    GB_INFO("Trash: %s", line.c_str());
    if (report_) report_(line);
}

std::chrono::steady_clock::time_point BagCleaner::now() const {
    return clock_.now ? clock_.now() : std::chrono::steady_clock::now();
}

void BagCleaner::sleep(int ms) {
    if (ms <= 0) return;
    if (clock_.sleep) clock_.sleep(ms);
    else std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool BagCleaner::wait_ms(int ms) {
    for (int slept = 0; slept < ms; slept += 30) {
        if (cancelled_) return false;
        sleep(std::min(30, ms - slept));
    }
    return !cancelled_;
}

void BagCleaner::pause_ms(int ms) {
    sleep(ms);
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

bool BagCleaner::right_click(int x, int y) {
    // Pretend hands with no right click of their own are a test that never
    // meant one: nothing reaches the game.
    if (hands_.click || hands_.drag || hands_.key) return hands_.right_click ? hands_.right_click(x, y) : false;
    return hands_.right_click ? hands_.right_click(x, y) : input_.click_at(x, y, true);
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
    late_ask_ = false;

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
    const bool can_use = bag::can_check(store_, TrashList::Action::Use);
    bool any_use = false;
    for (const TrashList::Item& it : trash_->items) any_use = any_use || it.action == TrashList::Action::Use;
    std::set<long long> seen;        // slots dealt with this pass: tab, row of the tab, column
    // List items registered this pass: no other slot of theirs is right-clicked
    // again this pass. Not what deleting goes by: a fragment's drawing is one
    // piece of every monster's nine, and only `registered` says which stack
    // was registered.
    std::set<int> used;
    std::set<int> no_use;            // right-clicked to no end: not tried again this pass
    // The slots registered from that still hold some of their stack, by tab,
    // row of the tab and column: the only places what is left of an item to
    // be used and then deleted is deleted from.
    Registrations registered;
    // Whether those places still hold: false once a deletion's slot is not
    // seen empty afterwards, as it would not be in a bag that closes up its
    // gaps -- and in such a bag a place says nothing about what is there.
    bool by_place = true;
    // Slot `index` of the page `was` shows empty on a fresh look, on that same
    // page by the scroll bar.
    const auto slot_left_empty = [&](const BagView& was, int index) {
        Frame g;
        BagView w;
        if (!grab_view(g, w) || !was.scroll_known() || !w.scroll_known() || w.first_row != was.first_row)
            return false;
        const auto now = w.slots(l);
        return static_cast<std::size_t>(index) < now.size() &&
               bag::is_empty(g, now[static_cast<std::size_t>(index)], l);
    };
    int done = 0, uses = 0;
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

        // On Tiêu Dùng, the items on the Dùng list are registered before
        // anything there is deleted, as GoC orders its pass: an item to be
        // used and then deleted is registered first. Not on a look-only pass,
        // not without the picture to tell the registration question by, and
        // not unless [Trash] use_fragments says so: the picture is shipped,
        // and its being there is no say-so.
        if (tab == kUseTab && any_use && can_use && settings_.use_fragments && !dry_run_) {
            use_sweep(tab, v, res, stop, used, no_use, uses, registered);
            if (stop || cancelled_) break;
            scroll_to_top(v);
        }
        // What is left of a registered stack still to be deleted on this tab.
        const auto rests_waiting = [&] {
            if (tab != kUseTab || !by_place) return false;
            for (const auto& each : registered)
                if (each.second.left > 0 && trash_->items[static_cast<std::size_t>(each.second.item)].then_delete)
                    return true;
            return false;
        };

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
                // An item on the Dùng list is not deleted -- but for what is
                // left of a stack this pass registered, of an item to be
                // deleted once used: from that very slot, found by where it is
                // -- only where the scroll bar says which row this is -- and
                // only while it shows just what it did right after, the same
                // drawing and the same count. Every other slot drawn the same
                // waits for a registration of its own: the drawing says only
                // which of a monster's nine pieces it is.
                const auto reg = item.action == TrashList::Action::Use && v.scroll_known()
                                     ? registered.find(key)
                                     : registered.end();
                const bool from_here = reg != registered.end();     // registered from this slot
                bool rest = false;
                std::string rest_kept;          // why what is left there stays
                if (from_here && item.then_delete && reg->second.left > 0) {
                    const int holds = counts_ ? counts_->read(f, slots[s]) : CountReader::kUnreadable;
                    if (!by_place)
                        rest_kept = "túi đồ có thể đã tự xếp lại sau một lần xoá, nên không còn chắc đây là ô "
                                    "vừa đăng ký";
                    else if (bag::alike(bag::cut(f, slots[s]), reg->second.look, l) < kStillThere)
                        rest_kept = "ô không còn giống lúc vừa đăng ký";
                    else if (holds != reg->second.left)
                        rest_kept = "số lượng không còn là " + std::to_string(reg->second.left) +
                                    " như lúc vừa đăng ký";
                    else
                        rest = true;
                }
                const bool to_use = item.action == TrashList::Action::Use && !rest;
                if (!to_use)         ++res.matched;
                else if (!from_here) ++res.use_matched;
                char percent[24];
                std::snprintf(percent, sizeof percent, "%.0f%%", static_cast<double>(k.score) * 100.0);
                // Which listed item, when it is one that came along with a
                // chosen icon for being drawn the same.
                const std::string score = " (giống " + std::string(percent) +
                                          (item.via.empty() ? std::string()
                                                            : ", " + item_name(item)) + ")";
                if (to_use) {
                    if (from_here) {
                        // Registered from here this pass. What is left stays,
                        // and when it was to go, it is said why not.
                        if (!rest_kept.empty())
                            say(std::string("không xoá phần còn lại ở ") + where + score + ": " + rest_kept);
                        continue;
                    }
                    // What the use sweep did not register: said, never
                    // right-clicked here.
                    const char* why_not = !item.fragment()
                                              ? "hình này không giống mảnh ghép nào, mà chuột phải vào món "
                                                "khác là dùng luôn món đó"
                                        : !settings_.use_fragments
                                              ? "Dùng đang tắt (use_fragments=false trong configs\\trash.ini)"
                                        : !can_use ? "GoBot chưa có hình hộp thoại đăng ký mảnh ghép của "
                                                     "game để kiểm tra"
                                        : tab != kUseTab ? "chỉ dùng đồ ở ngăn Tiêu Dùng"
                                                         : nullptr;
                    // An item on the delete or drop list that matched the
                    // slot better and gave way to this one (judge): said, so
                    // that the player sees why a picture of theirs on that
                    // list leaves it be.
                    std::string over;
                    if (k.passed_over >= 0) {
                        const TrashList::Item& o = trash_->items[static_cast<std::size_t>(k.passed_over)];
                        char by[24];
                        std::snprintf(by, sizeof by, "%.0f%%", static_cast<double>(k.passed_over_score) * 100.0);
                        over = "; cũng giống " + item_name(o) + " trong danh sách " +
                               (o.action == TrashList::Action::Drop ? "vứt" : "xoá") + " (" + by +
                               "), nhưng danh sách Dùng được ưu tiên";
                    }
                    if (dry_run_)
                        say(std::string("thấy đồ sẽ dùng ở ") + where + score +
                            (item.then_delete ? ", rồi xoá phần còn lại của ô đó" : "") +
                            (why_not ? std::string(", nhưng sẽ không dùng: ") + why_not : std::string()) + over);
                    else if (why_not)
                        say(std::string("thấy đồ sẽ dùng ở ") + where + score + ", nhưng sẽ không dùng: " +
                            why_not + over);
                    else if (item.then_delete)
                        say(std::string("không xoá món ở ") + where + score +
                            (v.scroll_known() ? ": chưa đăng ký từ ô này"
                                              : ": không đọc được thanh cuộn, nên không biết đây có phải ô vừa "
                                                "đăng ký không") +
                            "; chỉ xoá phần còn lại của đúng ô vừa đăng ký" + over);
                    continue;
                }
                if (dry_run_) {
                    if (item.action == TrashList::Action::Drop)
                        say(std::string("thấy đồ vứt đi ở ") + where + score +
                            (can_drop ? "" : ", nhưng sẽ không vứt: GoBot chưa có hình hộp "
                                             "thoại vứt đồ của game để kiểm tra"));
                    else
                        say(std::string("thấy đồ bỏ đi ở ") + where + score);
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
                const bool gone = trash_one(f, v, static_cast<int>(s), count);
                if (gone) {
                    ++done;
                    ++res.deleted;
                    if (rest) {
                        say("đã xoá phần còn lại (" + std::to_string(count) + ") của món vừa dùng ở " + where +
                            score);
                        registered.erase(reg);
                    } else {
                        say(std::string("đã xoá món ở ") + where + score);
                    }
                } else {
                    ++res.failed;
                    say(std::string("không bỏ được món ở ") + where + score);
                    if (res.failed >= 3) {
                        res.trouble = "bỏ đồ không được 3 lần, dừng lại";
                        stop = true;
                    }
                }
                // What is left of a registered stack is found again by where
                // it is. A bag that closed up the gap a deletion left would
                // have moved it, and something else drawn the same could stand
                // there now: once a slot a deletion was tried on is not seen
                // empty, nothing more is deleted by place this pass. Whether
                // the deletion read as done or not: in such a bag one that
                // went reads as failed when the next stack drawn the same
                // moves into its slot -- "still there".
                if (rests_waiting() && !slot_left_empty(v, static_cast<int>(s))) {
                    by_place = false;
                    say(gone ? "ô vừa xoá không trống ra (túi đồ có thể đã tự xếp lại), nên không xoá phần còn lại "
                               "của món đã dùng nữa"
                             : "ô vừa thử xoá không trống ra (món đồ còn đó, hoặc túi đồ có thể đã tự xếp lại), nên "
                               "không xoá phần còn lại của món đã dùng nữa");
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
    // A right-click of this pass that brought nothing up while it was watched
    // may have brought its question up since -- the delete sweep answers none
    // but the bin's. A dialog up now is taken for it: GoBot's own question
    // left open, which holds everything off as an item left in the bin does,
    // until a look shows it gone (UF-R7).
    if (!res.stuck && late_ask_) {
        Frame g;
        if (grab_ && grab_(g) && g.valid() && !bag::find_dialogs(g, store_).empty()) {
            res.stuck = true;
            keep(g, "use_left_open");
            if (res.trouble.empty())
                res.trouble = "một hộp thoại hiện ra muộn sau khi bấm chuột phải vào món đồ; bấm Hủy Bỏ trong game";
        }
    }
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
    const auto start = now();
    for (;;) {
        if (!wait_ms(kBoxLookMs)) return CountReader::kUnreadable;     // stopped
        const bool long_enough = now() - start >= std::chrono::milliseconds(kBoxWatchMs);
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

// --------------------------------------------------------------------- use

BagCleaner::UseResult BagCleaner::use_one(const Frame& f, const BagView& v, int index, int count) {
    const BagLayout& l = settings_.layout;
    UseResult out;
    const auto slots = v.slots(l);
    if (index < 0 || index >= static_cast<int>(slots.size())) {
        out.why = "không có ô đó";
        return out;
    }
    const ScreenRect s = slots[static_cast<std::size_t>(index)];
    const Frame before = bag::cut(f, s);
    if (!mouse_ok(s.cx(), s.cy())) {
        out.why = "không bấm được vào ô đồ";
        return out;
    }
    // The pass stops, and whatever may still be up keeps everything held off
    // (item_out_) until a look shows nothing waiting.
    const auto lost = [&](const std::string& why, const Frame& shot, const char* what) {
        keep(shot, what);
        out.outcome = UseOutcome::Lost;
        out.why = why;
        return out;
    };
    // Hủy Bỏ on every dialog with two buttons up -- for a stop, and for
    // anything that is not the registration question as GoBot asked it.
    // Hủy Bỏ never destroys anything.
    const auto refuse = [&](const std::string& why, const Frame& shot, const char* what) {
        keep(shot, what);
        if (!cancel_all(5)) return lost("hộp thoại không đóng lại khi bấm Hủy Bỏ", shot, "use_stuck");
        item_out_ = false;
        out.outcome = UseOutcome::Refused;
        out.why = why;
        return out;
    };
    // A slot that did not come out the way a registration leaves it, or the
    // way a right-click that did nothing leaves it: the frame kept, and the
    // pass stops on it -- nothing more right-clicked, nothing of it deleted.
    const auto doubt = [&](UseOutcome how, const std::string& why, const Frame& shot, const char* what) {
        keep(shot, what);
        out.outcome = how;
        out.why = why;
        return out;
    };

    // 1. One right-click on the middle of the slot. From here until a look
    //    shows no dialog up, what it brought up may be waiting.
    item_out_ = true;
    if (!right_click(s.cx(), s.cy())) {
        item_out_ = false;
        out.why = "không điều khiển được chuột trong cửa sổ game";
        return out;
    }

    // 2. What it brings up, within a second and a half -- watched to the end
    //    even after a stop, so that a question coming up late is not left
    //    open. A dialog fading in can fail its words for a frame; one that is
    //    still not the registration question a frame later is not, and the
    //    same for a message with only Xác Nhận.
    bag::Dialog d;
    bool asked = false;
    int others = 0, notices = 0;
    Frame g;
    for (int waited = 0; waited < kUseAskMs && !asked; waited += 150) {
        pause_ms(150);
        Frame h;
        if (!grab_ || !grab_(h) || !h.valid()) continue;
        g = std::move(h);
        const auto dialogs = bag::find_dialogs(g, store_);
        if (dialogs.size() == 1 && dialogs.front().registers) {
            d = dialogs.front();
            asked = true;
            break;
        }
        if (!dialogs.empty()) {
            // The bin's question, the revive question's words, a shop's,
            // anything else -- or the registration question with something
            // else up beside it: never confirmed.
            if (++others >= 2 || cancelled_) {
                bool bins = false;
                for (const bag::Dialog& each : dialogs) bins = bins || each.ours();
                return refuse(bins ? "hiện ra hộp thoại của thùng rác chứ không phải hộp thoại đăng ký"
                                   : "hộp thoại hiện ra không phải hộp thoại đăng ký mảnh ghép",
                              g, "use_wrong_dialog");
            }
            continue;
        }
        if (bag::notice_up(g, store_) && ++notices >= 2) {
            // Left for the player to read: nothing is pressed on it.
            keep(g, "use_notice");
            item_out_ = false;
            out.outcome = UseOutcome::Notice;
            out.why = "sau khi bấm chuột phải game hiện một thông báo; để nguyên cho người chơi đọc";
            return out;
        }
    }

    if (!asked) {
        // Nothing came up: what the right-click did, if anything, is in the
        // slot. As it was, it cannot be used, and the pass goes on. Gone or
        // less, something took it without asking -- a shop or a storage
        // window open beside the bag would -- and anything that cannot be
        // told may be the same: either way nothing more is right-clicked.
        // Its question may come up later still, the game lagging: the pass
        // keeps that in mind to its end.
        late_ask_ = true;
        Frame h;
        BagView w;
        if (!grab_view(h, w)) return lost("mất dấu túi đồ", h.valid() ? h : g, "use_lost");
        if (!bag::find_dialogs(h, store_).empty()) return refuse("một hộp thoại hiện ra muộn", h, "use_late_dialog");
        item_out_ = false;
        Frame look, shot = h;
        std::string why;
        switch (slot_now(v, index, before, count, look, shot, why)) {
            case SlotNow::Same:
                out.outcome = UseOutcome::Nothing;
                out.why = "bấm chuột phải mà không thấy gì";
                return out;
            case SlotNow::Asking:
                // A question come up later still: answered as one that comes
                // up a moment after the wait is.
                return refuse("một hộp thoại hiện ra muộn", shot, "use_late_dialog");
            case SlotNow::Gone:
            case SlotNow::Less:
                return doubt(UseOutcome::UsedUnasked,
                             "đã dùng (không hỏi): bấm chuột phải thì không có câu hỏi nào mà món đồ đã bớt đi -- "
                             "có thể đã dùng, cũng có thể một cửa hàng hay kho đồ đang mở đã lấy nó -- nên không "
                             "dùng thêm món nào",
                             shot, "use_unasked");
            case SlotNow::Unknown:
                break;
        }
        return doubt(UseOutcome::Unsure,
                     "bấm chuột phải thì không có câu hỏi nào, và không xem lại được ô đồ (" + why +
                         "), nên không dùng thêm món nào",
                     shot, "use_unsure");
    }

    // 3. The registration question. Looked at once more on a fresh frame,
    //    the moment before Xác Nhận: still there, still its words, and
    //    nothing else up -- or Hủy Bỏ.
    if (cancelled_) return refuse("dừng giữa chừng", g, "use_stopped");
    Frame k;
    if (!grab_ || !grab_(k) || !k.valid()) return refuse("không chụp được màn hình game", g, "use_blind");
    const auto again = bag::find_dialogs(k, store_);
    if (again.empty()) {
        item_out_ = false;
        out.outcome = UseOutcome::Refused;
        out.why = "hộp thoại đăng ký đóng lại trước khi bấm";
        return out;
    }
    if (again.size() != 1 || !again.front().registers || !same_place(again.front().pair, d.pair))
        return refuse("lúc sắp bấm Xác Nhận thì trên màn hình không chỉ có hộp thoại đăng ký", k, "use_changed");
    // Nor any window but the bag and the book the right-click opened with
    // its question: a window that came up since the look before the
    // right-click -- a shop, a trade -- may have been what asked.
    {
        const bag::Book book = bag::find_book(k, store_);
        const BagView kv = bag::find(k, store_, l);
        ScreenRect other;
        if (bag::other_window_up(k, store_, kv, l, &other, book.has_x ? &book.x : nullptr)) {
            GB_INFO("Trash: a close button that is neither the bag's nor the book's at %d,%d -- Hủy Bỏ", other.x,
                    other.y);
            return refuse("lúc sắp bấm Xác Nhận thì có một cửa sổ khác đang mở, nên không bấm", k, "use_other_window");
        }
    }
    if (!mouse_ok(d.confirm_x(), d.y())) return refuse("không bấm được Xác Nhận", k, "use_blocked");
    click(d.confirm_x(), d.y());

    // 4. The question gone from where it was.
    bool closed = false;
    Frame after = k;
    for (int waited = 0; waited < kUseCloseMs && !closed; waited += 150) {
        pause_ms(150);
        Frame h;
        if (!grab_ || !grab_(h) || !h.valid()) continue;
        after = std::move(h);
        bool there = false;
        for (const bag::Dialog& each : bag::find_dialogs(after, store_))
            there = there || (each.registers && same_place(each.pair, d.pair));
        closed = !there;
    }
    if (!closed) return lost("hộp thoại đăng ký không đóng lại sau khi bấm Xác Nhận", after, "use_stuck");
    pause_ms(300);
    Frame h;
    BagView w;
    const bool bag_seen = grab_view(h, w);
    // Anything with two buttons now is no question GoBot asked, whatever it
    // follows from: Hủy Bỏ. And with its question confirmed and no telling
    // whether the registration took, the pass stops on it -- as it does when
    // the same comes up a moment later, on a look back at the slot.
    if (h.valid() && !bag::find_dialogs(h, store_).empty()) {
        if (!cancel_all(5)) return lost("hộp thoại không đóng lại khi bấm Hủy Bỏ", h, "use_stuck");
        item_out_ = false;
        return doubt(UseOutcome::Unsure,
                     "sau khi bấm Xác Nhận game hỏi thêm một câu khác, nên bấm Hủy Bỏ; không biết đã đăng ký "
                     "hay chưa",
                     h, "use_next_dialog");
    }
    if (!bag_seen)
        return lost("sau khi đăng ký, túi đồ bị che (có thể Yêu Quái Bách Khoa đã mở ra): đóng nó trong game",
                    h.valid() ? h : after, "use_covered");
    item_out_ = false;
    if (bag::notice_up(h, store_)) {
        keep(h, "use_notice_after");
        out.outcome = UseOutcome::Notice;
        out.why = "sau khi bấm Xác Nhận game hiện một thông báo; để nguyên cho người chơi đọc";
        return out;
    }

    // 5. Only gone, or one less in the stack, counts: in that very slot, on
    //    the page it was on, the same on two looks. Anything else the
    //    question may or may not have done, and nothing is built on it -- not
    //    counted as used, nothing of it deleted, and the pass stops. Even the
    //    stack as it was: a bag that closed up the gap a single piece left
    //    would show the next piece drawn the same there, just so.
    Frame look, shot = h;
    std::string why;
    switch (slot_now(v, index, before, count, look, shot, why)) {
        case SlotNow::Gone:
            out.outcome = UseOutcome::Used;
            return out;
        case SlotNow::Less:
            out.outcome = UseOutcome::Used;
            out.left = count - 1;
            out.after = std::move(look);
            return out;
        case SlotNow::Same:
            return doubt(UseOutcome::NotTaken, "đã bấm Xác Nhận mà số lượng không giảm", shot, "use_not_taken");
        case SlotNow::Asking:
            // Asked something more, a moment on: no question GoBot asked, so
            // Hủy Bỏ, as on the look before -- and still no telling whether
            // the registration took.
            if (!cancel_all(5)) return lost("hộp thoại không đóng lại khi bấm Hủy Bỏ", shot, "use_stuck");
            item_out_ = false;
            return doubt(UseOutcome::Unsure,
                         "sau khi bấm Xác Nhận game hỏi thêm một câu khác, nên bấm Hủy Bỏ; không biết đã đăng ký "
                         "hay chưa",
                         shot, "use_unsure");
        case SlotNow::Unknown:
            break;
    }
    return doubt(UseOutcome::Unsure,
                 "đã bấm Xác Nhận mà không thấy ô đồ trống ra hay bớt đúng một (" + why +
                     "), nên không biết đã đăng ký hay chưa",
                 shot, "use_unsure");
}

BagCleaner::SlotNow BagCleaner::slot_now(const BagView& v, int index, const Frame& before, int count,
                                         Frame& look, Frame& shot, std::string& why) {
    const BagLayout& l = settings_.layout;
    SlotNow first = SlotNow::Unknown;
    for (int n = 0; n < 2; ++n) {
        if (n > 0) pause_ms(150);
        Frame g;
        BagView w;
        const bool found = grab_view(g, w);
        if (g.valid()) shot = g;
        if (!found) {
            why = "mất dấu túi đồ";
            return SlotNow::Unknown;
        }
        // Something up now that the look before did not show: it waits on an
        // answer, and everything stays held off until a look shows it gone.
        if (!bag::find_dialogs(g, store_).empty()) {
            item_out_ = true;
            why = "có một hộp thoại hiện ra";
            return SlotNow::Asking;
        }
        if (!v.scroll_known() || !w.scroll_known()) {
            why = "không đọc được thanh cuộn của túi đồ, nên không biết có còn đúng trang không";
            return SlotNow::Unknown;
        }
        if (w.first_row != v.first_row) {
            why = "trang túi đồ đã đổi";
            return SlotNow::Unknown;
        }
        const auto slots = w.slots(l);
        if (static_cast<std::size_t>(index) >= slots.size()) {
            why = "ô đồ bị che";
            return SlotNow::Unknown;
        }
        const ScreenRect s = slots[static_cast<std::size_t>(index)];
        look = bag::cut(g, s);
        SlotNow is = SlotNow::Unknown;
        if (bag::is_empty(g, s, l)) {
            is = SlotNow::Gone;
        } else if (bag::alike(look, before, l) < kStillThere) {
            why = "ô đồ giờ có hình khác";
            return SlotNow::Unknown;
        } else {
            const int left = counts_ ? counts_->read(g, s) : CountReader::kUnreadable;
            if (left == count) {
                is = SlotNow::Same;
            } else if (count >= 2 && left == count - 1) {
                is = SlotNow::Less;
            } else {
                why = left == CountReader::kUnreadable
                          ? std::string("không đọc lại được số lượng trên ô đồ")
                          : "số lượng trên ô đồ từ " + std::to_string(count) + " thành " + std::to_string(left);
                return SlotNow::Unknown;
            }
        }
        if (n > 0 && is != first) {
            why = "hai lần xem lại ô đồ không giống nhau";
            return SlotNow::Unknown;
        }
        first = is;
    }
    return first;
}

bool BagCleaner::shut_book(std::string& why, Frame& shot) {
    // The book and its X on a fresh look, a few looks at most: a frame caught
    // while the game redraws can lose the X for one look.
    bag::Book book;
    for (int look = 0; look < kBookXLooks; ++look) {
        if (look > 0) pause_ms(kBookLookMs);
        Frame f;
        if (!grab_ || !grab_(f) || !f.valid()) continue;
        shot = f;
        book = bag::find_book(f, store_);
        if (!book.found) return true;           // not open: nothing to close
        // A click goes to whatever is up first: never with a dialog up, nor
        // a message with only Xác Nhận, which the player is to read.
        if (!bag::find_dialogs(f, store_).empty() || bag::notice_up(f, store_)) {
            why = "Yêu Quái Bách Khoa đang mở cùng một hộp thoại, nên không bấm nút X của nó";
            return false;
        }
        if (book.has_x) break;
    }
    if (!shot.valid()) {
        why = "không chụp được màn hình game";
        return false;
    }
    if (!book.found) return true;
    if (!book.has_x) {
        why = "thấy Yêu Quái Bách Khoa đang mở mà không thấy nút X của nó, nên không đóng được";
        return false;
    }
    const int x = book.x.cx(), y = book.x.cy();
    if (!mouse_ok(x, y)) {
        why = "không bấm được nút X của Yêu Quái Bách Khoa";
        return false;
    }
    say("đóng Yêu Quái Bách Khoa bằng nút X của nó");
    click(x, y);
    // Gone, where it was: its tab no longer there.
    for (int waited = 0; waited < kBookCloseMs; waited += kBookLookMs) {
        pause_ms(kBookLookMs);
        Frame g;
        if (!grab_ || !grab_(g) || !g.valid()) continue;
        shot = g;
        if (!bag::find_book(g, store_, &book.window).found) return true;
    }
    why = "Yêu Quái Bách Khoa không đóng lại khi bấm nút X của nó";
    return false;
}

void BagCleaner::use_sweep(int tab, BagView& v, Result& res, bool& stop, std::set<int>& used,
                           std::set<int>& no_use, int& uses, Registrations& registered) {
    const BagLayout& l = settings_.layout;
    // Whether a right-click registered anything is read off the slot's count,
    // so without the game's digits nothing is right-clicked at all.
    if (counts_ == nullptr || !counts_->loaded()) {
        say("không có bộ chữ số của game để đọc số lượng trên ô đồ, nên không dùng món nào");
        return;
    }
    // Nor with no way to tell another of the game's windows open beside the
    // bag, which a right-click might hand the item to.
    if (!bag::can_see_windows(store_)) {
        say("không có hình nút đóng cửa sổ của game để biết có cửa sổ khác đang mở cạnh túi đồ không, nên không "
            "dùng món nào");
        return;
    }
    // Nor without the book's pictures: every right-click opens Yêu Quái Bách
    // Khoa, and it has to be closed again, and seen closed, before the next.
    if (!bag::can_see_book(store_)) {
        say("không có hình Yêu Quái Bách Khoa để đóng nó lại sau mỗi lần bấm chuột phải, nên không dùng món nào");
        return;
    }
    // A dialog up after one of this sweep's own right-clicks, which use_one
    // had stopped watching for: most likely that right-click's question, come
    // late. Never confirmed -- whose it is cannot be told by now -- but
    // answered Hủy Bỏ, which loses nothing, and everything held off while it
    // will not close: a question of GoBot's own left open counts as an item
    // left out (UF-R7). The pass stops either way.
    bool clicked = false;
    const auto answer_late = [&](const Frame& g) {
        keep(g, "use_late_dialog");
        item_out_ = true;
        if (cancel_all(5)) {
            item_out_ = false;
            res.trouble = "một hộp thoại hiện ra muộn sau khi bấm chuột phải vào món đồ; đã bấm Hủy Bỏ, nên không "
                          "dùng thêm món nào";
        } else {
            res.trouble = "một hộp thoại hiện ra muộn sau khi bấm chuột phải vào món đồ, và bấm Hủy Bỏ không đóng "
                          "được: đóng nó trong game";
        }
        stop = true;
    };
    std::set<long long> seen;
    int scrolls = 0;
    bool hid_pointer = false;
    for (int round = 0; round < 200 && !stop && !cancelled_; ++round) {
        Frame f;
        if (!grab_view(f, v)) {
            res.trouble = "mất dấu túi đồ";
            stop = true;
            return;
        }
        // Nothing is right-clicked over anything else: not with a dialog up
        // or the bin holding something, nor a message with only Xác Nhận.
        if (!clean(f, v)) {
            if (clicked && !bag::find_dialogs(f, store_).empty()) {
                answer_late(f);
                return;
            }
            res.trouble = "thùng rác đang có đồ chờ xoá hoặc có hộp thoại đang mở; xong việc đó trong game "
                          "rồi thử lại";
            stop = true;
            return;
        }
        if (bag::notice_up(f, store_)) {
            say("đang có một thông báo trong game, nên không dùng món nào; đóng nó rồi thử lại");
            return;
        }
        // And only on Tiêu Dùng, by the strip: on another tab a right-click
        // equips what it lands on.
        if (v.selected_tab != tab) {
            say(std::string("không chắc túi đồ đang ở ngăn ") + bag_tab_name(tab) + ", nên không dùng món nào");
            return;
        }
        if (v.slot_rows < l.rows && !v.at_bottom(l) && !hid_pointer) {
            hid_pointer = true;
            if (!hands_.click) input_.move_pointer(v.strip.x + l.scroll_up_dx + 30, v.strip.y + 100);
            if (!wait_ms(300)) return;
            continue;
        }
        // Whether a right-click registered anything is read off the slot on
        // this same page, by the scroll bar. With the bar not to be read --
        // something over it -- that could not be read either.
        if (!v.scroll_known()) {
            say("không đọc được thanh cuộn của túi đồ, nên không dùng món nào");
            return;
        }

        bool acted = false;
        const auto slots = v.slots(l);
        for (std::size_t s = 0; s < slots.size() && !stop && !cancelled_; ++s) {
            const int r = static_cast<int>(s) / l.cols, c = static_cast<int>(s) % l.cols;
            const int row = v.first_row + r;
            const long long key = static_cast<long long>(row) * 100 + c;
            if (seen.count(key) || bag::is_empty(f, slots[s], l)) continue;
            const TrashList::Verdict k = trash_->judge(f, slots[s], settings_.min_score);
            if (k.item < 0) continue;       // and a doubt is said by the delete sweep
            const TrashList::Item& item = trash_->items[static_cast<std::size_t>(k.item)];
            // A fragment only, and one slot of a drawing a pass: the others
            // drawn the same are most likely other monsters' pieces, each
            // with its own question, and wait for the next pass. What is
            // passed over here the delete sweep says.
            if (item.action != TrashList::Action::Use || !item.fragment() || used.count(k.item) ||
                no_use.count(k.item))
                continue;
            seen.insert(key);

            char where[96];
            std::snprintf(where, sizeof where, "%s, hàng %d ô %d", bag_tab_name(tab),
                          (v.scroll_known() ? v.first_row : 0) + r + 1, c + 1);
            char percent[24];
            std::snprintf(percent, sizeof percent, "%.0f%%", static_cast<double>(k.score) * 100.0);
            const std::string score = " (giống " + std::string(percent) +
                                      (item.via.empty() ? std::string() : ", " + item_name(item)) + ")";
            if (uses >= settings_.max_uses_per_run) {
                say("đã dùng " + std::to_string(uses) + " món, đủ cho một lần");
                return;
            }
            // Whether it was registered is read off this count afterwards: a
            // count that does not read now could not tell then either.
            const int count = counts_->read(f, slots[s]);
            if (count == CountReader::kUnreadable || count <= 0) {
                say(std::string("không dùng món ở ") + where + score +
                    ": không đọc được số lượng trên ô đồ, nên sau khi đăng ký không xem lại được");
                continue;
            }
            // Nor with another of the game's windows open beside the bag --
            // Kho Cash, a shop, a storage or trade window -- which may take
            // the item without a question: any close button but the bag's own
            // says one is. Looked for on this same frame, the moment before.
            // The book among them: one this sweep opened it has shut already,
            // so one open now is the player's, or one a pass that stopped
            // left, and it is not closed for them.
            if (bag::find_book(f, store_).found) {
                say("Yêu Quái Bách Khoa đang mở, nên không dùng món nào; đóng nó rồi thử lại");
                keep(f, "use_book_up");
                return;
            }
            ScreenRect other;
            if (bag::other_window_up(f, store_, v, l, &other)) {
                say("đang có cửa sổ khác mở cạnh túi đồ (kho, cửa hàng…), nên không dùng món nào; đóng nó rồi "
                    "thử lại");
                GB_INFO("Trash: a close button that is not the bag's own at %d,%d -- nothing right-clicked",
                        other.x, other.y);
                keep(f, "use_other_window");
                return;
            }

            clicked = true;
            const UseResult u = use_one(f, v, static_cast<int>(s), count);
            acted = true;
            hid_pointer = false;
            switch (u.outcome) {
                case UseOutcome::Used: {
                    ++uses;
                    ++res.used;
                    used.insert(k.item);
                    // What is left in the slot is kept by where it is: the
                    // tab, the row of the tab and the column -- the scroll bar
                    // was read before and after, or this was no Used.
                    std::string rest;
                    if (u.left > 0 && v.scroll_known()) {
                        const long long place = static_cast<long long>(tab) * 100000000LL + key;
                        registered[place] = Registered{ k.item, u.left, u.after };
                        if (item.then_delete)
                            rest = "; còn " + std::to_string(u.left) + ", sẽ xoá phần còn lại của ô này";
                    }
                    say(std::string("đã dùng món ở ") + where + score + rest);
                    break;
                }
                case UseOutcome::NotTaken:
                    no_use.insert(k.item);
                    ++res.failed;
                    say(std::string("dừng ở món ") + where + score + ": " + u.why);
                    res.trouble = u.why;
                    stop = true;
                    break;
                case UseOutcome::Unsure:
                case UseOutcome::UsedUnasked:
                    no_use.insert(k.item);
                    ++res.use_unsure;
                    say(std::string("dừng ở món ") + where + score + ": " + u.why);
                    res.trouble = u.why;
                    stop = true;
                    break;
                case UseOutcome::Nothing: {
                    no_use.insert(k.item);
                    say(std::string("không dùng được món ở ") + where + score + ": " + u.why);
                    // Its question may come up yet, late: nothing more is
                    // right-clicked until a look a moment on shows none, so
                    // that it is never answered as the next piece's -- the
                    // next round's look sees to one that comes after this.
                    pause_ms(kUseLateMs);
                    Frame g;
                    if (grab_ && grab_(g) && g.valid() && !bag::find_dialogs(g, store_).empty()) answer_late(g);
                    break;
                }
                case UseOutcome::Refused:
                    no_use.insert(k.item);
                    ++res.failed;
                    say(std::string("không dùng món ở ") + where + score + ": " + u.why);
                    if (res.failed >= 3) {
                        res.trouble = "dùng đồ không được 3 lần, dừng lại";
                        stop = true;
                    }
                    break;
                case UseOutcome::Untouched:
                    say(std::string("không dùng món ở ") + where + score + ": " + u.why);
                    res.trouble = u.why;
                    stop = true;
                    break;
                case UseOutcome::Notice:
                case UseOutcome::Lost:
                    say(std::string("dừng ở món ") + where + score + ": " + u.why);
                    res.trouble = u.why;
                    stop = true;
                    break;
            }
            // The book the right-click opened stays open whatever the answer
            // was, and while it is, nothing else is right-clicked: shut by its
            // X, and seen shut, before the next slot -- after a registration,
            // a Hủy Bỏ, or a right-click that brought nothing up. The pass
            // stops when it will not shut. A pass that stops anyway leaves it
            // as it is, as it leaves everything else after a surprise; and a
            // stop presses nothing more.
            const bool goes_on = u.outcome == UseOutcome::Used || u.outcome == UseOutcome::Refused ||
                                 u.outcome == UseOutcome::Nothing;
            if (goes_on && !stop && !cancelled_) {
                std::string why;
                Frame shot;
                if (!shut_book(why, shot)) {
                    keep(shot, "use_book_open");
                    say("dừng sau món ở " + std::string(where) + ": " + why);
                    res.trouble = why;
                    stop = true;
                }
            }
            break;
        }
        if (acted || stop) continue;

        // Nothing more on this page: the next rows, as the delete sweep goes.
        if (v.at_bottom(l) || ++scrolls > 30) return;
        const int dx = v.strip.x + l.scroll_down_dx, dy = v.strip.y + l.scroll_down_dy;
        if (!mouse_ok(dx, dy)) {
            res.trouble = "không bấm được vào túi đồ: cửa sổ game bị che hoặc không ở trên cùng";
            stop = true;
            return;
        }
        click(dx, dy);
        hid_pointer = false;
        if (!wait_ms(250)) return;
        Frame g;
        BagView w;
        if (!grab_view(g, w)) continue;
        const bool moved = v.scroll_known() && w.scroll_known() ? w.first_row > v.first_row
                                                                : !bag::same_page(f, v, g, w, l);
        if (!moved) return;
    }
}

// ------------------------------------------------------------------ probe

namespace {

/// How long a probe watches a dialog that has come up -- from the click on
/// the bin, or from the first look that showed it when it came up as the item
/// landed -- and how often it looks at most. The bin's dialog was seen live
/// to be up and settled within half a second of the click.
constexpr int kProbeWatchMs = 1500;
constexpr int kProbeLookMs = 40;

/// How long the item may take to show in the bin after the drag, as a pass
/// gives it; and how long it then has to stay there with nothing up before
/// the bin is clicked, so that a question coming up a moment after the item
/// lands is put down to the drop rather than to the click.
constexpr int kProbeLandMs = 1200;
constexpr int kProbeSettleMs = 300;

/// Frames one probe keeps at most, and frames of earlier probes kept at most:
/// the last two or three probes'.
constexpr int kProbeFramesMax = 100;
constexpr std::size_t kProbeFramesKept = 60;

} // namespace

bool BagCleaner::cancel_all(int tries) {
    // Hủy Bỏ, the right half of the pair, and nothing else: the item goes
    // back, and anybody else's dialog is said no to, which loses nothing.
    bring_forward();
    for (int attempt = 0; attempt < tries; ++attempt) {
        Frame f;
        if (!grab_ || !grab_(f) || !f.valid()) {
            pause_ms(150);
            continue;
        }
        const auto dialogs = bag::find_dialogs(f, store_);
        if (dialogs.empty()) return true;
        bag::Dialog d = dialogs.front();
        for (const bag::Dialog& each : dialogs)
            if (each.ours()) { d = each; break; }
        if (!mouse_ok(d.cancel_x(), d.y(), false)) {
            keep(f, "cancel_blocked");
            return false;
        }
        say(std::string("bấm Hủy Bỏ trên hộp thoại ") + bag::dialog_name(d.kind));
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
    }
    Frame f;
    return grab_ && grab_(f) && f.valid() && bag::find_dialogs(f, store_).empty();
}

BagCleaner::Probe BagCleaner::probe(int tab, int row, int col) {
    // Nothing in a probe presses Xác Nhận: there is no confirm_x() here, nor
    // in cancel_all and back_out, which give the item back with Hủy Bỏ. Nor
    // does it press any key once the item has moved: the bag's key, the only
    // one, comes first, and only on a look with no dialog up.
    Probe res;
    const BagLayout& l = settings_.layout;
    const auto give_up = [&](const std::string& why) {
        res.trouble = why;
        say(why);
        return res;
    };
    if (tab < 0 || tab >= kBagTabs || row < 0 || row >= l.rows || col < 0 || col >= l.cols)
        return give_up("không có ô đó trong túi đồ");
    if (store_.get(kConfirm) == nullptr)
        return give_up("thiếu hình nút Xác Nhận, nên không nhận ra được hộp thoại nào");
    if (cancelled_) return give_up("dừng trước khi bắt đầu");
    item_out_ = false;
    bring_forward();

    // Nothing is touched but on a clean screen: the bag found, the can in the
    // bin, and no dialog with the two buttons anywhere.
    const auto unclean = [&](const Frame& f, const BagView& v) {
        if (!f.valid()) return std::string("không chụp được màn hình game");
        if (!bag::find_dialogs(f, store_).empty())
            return std::string("có một hộp thoại đang mở; đóng nó trong game rồi thử lại");
        if (!v.found) return std::string("không thấy túi đồ, hoặc túi đồ bị che");
        return std::string("thùng rác đang có đồ chờ xoá; bấm vào thùng rác trong game, chọn Hủy "
                           "Bỏ, rồi thử lại");
    };
    Frame f;
    BagView v;
    if (!grab_view(f, v)) {
        // Shut, it seems. Its key goes to a dialog when one is up -- into a
        // quantity box, say -- so not then.
        if (!f.valid() || !bag::find_dialogs(f, store_).empty()) return give_up(unclean(f, v));
        press(settings_.bag_key);
        for (int i = 0; i < 12 && !v.found; ++i) {
            if (!wait_ms(150)) return give_up("dừng giữa chừng");
            grab_view(f, v);
        }
        if (!v.found)
            return give_up("không mở được túi đồ (phím " + InputManager::key_name(settings_.bag_key) +
                           "), hoặc túi đồ bị che");
    }
    if (!clean(f, v)) return give_up(unclean(f, v));

    int tx = 0, ty = 0;
    v.tab_point(l, tab, tx, ty);
    if (!mouse_ok(tx, ty))
        return give_up("không bấm được vào túi đồ: cửa sổ game bị che hoặc không ở trên cùng");
    click(tx, ty);
    if (!wait_ms(350)) return give_up("dừng giữa chừng");
    scroll_to_top(v);
    if (cancelled_) return give_up("dừng giữa chừng");
    if (!grab_view(f, v) || !clean(f, v)) return give_up(unclean(f, v));
    if (v.scroll_known() && !v.at_top()) return give_up("không cuộn được túi đồ lên đầu");
    if (!v.scroll_known())
        say("không đọc được thanh cuộn, nên hàng được đếm từ hàng đang hiện trên cùng");
    if (row >= (v.slot_rows < 0 ? l.rows : v.slot_rows))
        return give_up("hàng " + std::to_string(row + 1) +
                       " không hiện ra: bị che, hoặc ngăn này không có hàng đó");

    const ScreenRect s = v.slot(l, col, row);
    char where[96];
    std::snprintf(where, sizeof where, "%s, hàng %d ô %d", bag_tab_name(tab), row + 1, col + 1);
    if (bag::is_empty(f, s, l)) return give_up(std::string("ô ở ") + where + " đang trống");
    const Frame before = bag::cut(f, s);
    const int count = counts_ ? counts_->read(f, s) : CountReader::kUnreadable;
    say(std::string("thử với món ở ") + where +
        (count > 0 ? ", số lượng " + std::to_string(count) : std::string(", không đọc được số lượng")));

    // From the drag on, the item is given back before this returns, whatever
    // happens: Hủy Bỏ on whatever dialog is up; with none up, the bin clicked
    // and its dialog answered Hủy Bỏ, the way a pass gives an item back; and
    // Hủy Bỏ on whatever that click brings up that a pass would leave alone.
    const auto give_back = [&] {
        for (int round = 0; round < 2; ++round) {
            cancel_all(5);
            Frame g;
            BagView w;
            if (!grab_view(g, w) || clean(g, w)) return;
            back_out();
        }
        cancel_all(5);
    };
    // Back: no dialog, the can in the bin, and the slot holding what it held
    // -- the same page, as nothing has scrolled it since.
    const auto finish = [&] {
        pause_ms(300);
        Frame g;
        BagView w;
        res.back = grab_view(g, w) && clean(g, w) && !bag::is_empty(g, w.slot(l, col, row), l) &&
                   bag::alike(bag::cut(g, w.slot(l, col, row)), before, l) >= kStillThere;
        if (res.back) {
            item_out_ = false;
            say("món đồ đã về lại ô cũ, thùng rác trống");
        } else {
            if (res.trouble.empty()) res.trouble = "món đồ chưa về lại ô cũ, hoặc thùng rác chưa trống";
            say("món đồ chưa về lại ô cũ, hoặc thùng rác chưa trống: bấm Hủy Bỏ trong game trước "
                "khi gõ phím nào");
            keep(g, "probe_not_back");
        }
        return res;
    };

    if (!mouse_ok(s.cx(), s.cy()) || !mouse_ok(v.trash.cx(), v.trash.cy()))
        return give_up("không bấm được vào túi đồ: cửa sổ game bị che hoặc không ở trên cùng");
    if (cancelled_) return give_up("dừng giữa chừng");

    // Every frame from the drag on, kept and read -- not only those after the
    // click on the bin. A question that comes up as the item lands is the
    // very answer a probe is for, and not a sign that the item stayed out.
    const std::string name = "probe_" + stamp();
    if (!debug_dir_.empty()) {
        make_folder(debug_dir_);
        prune_pngs(debug_dir_, true, kProbeFramesKept);
    }
    // What the looks find, over all of them: the bin's own dialog with the
    // best words, or how near any other came to the bin's.
    bool seen = false;
    int kind = -1;
    float ours = 0.0f, nearest = -1.0f;
    // One look: kept as probe_<time>_NN.png and said, every dialog with the
    // two buttons on it weighed. How many of those it has, or -1 when there
    // was no frame to look at.
    const auto look_at = [&](Frame& k, long long ms, const char* since) {
        if (res.frames >= kProbeFramesMax || !grab_ || !grab_(k) || !k.valid()) return -1;
        char nn[8];
        std::snprintf(nn, sizeof nn, "%02d", res.frames++);
        std::string line =
            std::string("khung ") + nn + " (+" + std::to_string(ms) + " ms " + since + ")";
        if (!debug_dir_.empty() && save_png(k, debug_dir_ + "\\" + name + "_" + nn + ".png"))
            line += ", lưu " + name + "_" + nn + ".png";
        const auto dialogs = bag::find_dialogs(k, store_);
        if (dialogs.empty()) {
            // A refusal has only Xác Nhận under it, and is never pressed:
            // said, for whoever reads the frames.
            const Frame* notice = store_.get(kNotice);
            const bool told = notice != nullptr &&
                              TemplateMatcher::find(k, ScreenRect{ 0, 0, k.width, k.height },
                                                    *notice, 0.75f).found;
            line += told ? ": không có hộp thoại hai nút, mà có một thông báo chỉ có nút Xác "
                           "Nhận (để nguyên)"
                         : ": không có hộp thoại nào";
        }
        for (const bag::Dialog& d : dialogs) {
            seen = true;
            const std::vector<float> words = bag::words_scores(k, store_, d);
            // Below 0.30 the matcher does not say how far below.
            const auto score = [](float s) {
                char buf[16];
                std::snprintf(buf, sizeof buf, "%.3f", static_cast<double>(s));
                return s < 0.0f ? std::string("dưới 0.30") : std::string(buf);
            };
            char at[48];
            std::snprintf(at, sizeof at, ": hộp thoại ở %d,%d là ", d.pair.x, d.pair.y);
            line += at + std::string(bag::dialog_name(d.kind)) +
                    "; chữ giống câu hỏi bỏ vào thùng rác " + score(words[bag::kAskDialog]) +
                    ", câu hỏi số lượng " + score(words[bag::kCountDialog]) + "; " +
                    (d.quantity ? "có ô số lượng" : "không có ô số lượng");
            if (d.quantity && counts_ && counts_->loaded())
                line += ", ô ghi " + std::to_string(counts_->read_field(k, d.box));
            if (d.ours() && d.words > ours) {
                ours = d.words;
                kind = d.kind;
            }
            for (const float each : words) nearest = std::max(nearest, each);
        }
        say(line);
        return static_cast<int>(dialogs.size());
    };
    const auto ms_since = [this](std::chrono::steady_clock::time_point t) {
        return static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now() - t).count());
    };
    // Not faster than this; saving a frame makes it slower anyway.
    const auto pace = [&](std::chrono::steady_clock::time_point look) {
        const long long spent = ms_since(look);
        if (spent < kProbeLookMs) sleep(static_cast<int>(kProbeLookMs - spent));
    };
    const auto what_came_up = [&] {
        res.dialog = kind == bag::kCountDialog ? "quantity" : kind == bag::kAskDialog ? "ask" : "foreign";
        res.words = kind >= 0 ? ours : std::max(0.0f, nearest);
    };

    // 1. Onto the bin, which takes it and waits -- or asks there and then.
    item_out_ = true;
    res.moved = true;
    const bool moved = settings_.click_to_move
                           ? click(s.cx(), s.cy()) && wait_ms(220) && click(v.trash.cx(), v.trash.cy())
                           : drag(s.cx(), s.cy(), v.trash.cx(), v.trash.cy());
    const auto dropped = now();
    Frame g;
    BagView w;
    long long asked = -1;       // a dialog first seen, this long after the drop
    long long settled = -1;     // the bin showing the item with nothing up since
    bool waiting = false;
    while (moved && !cancelled_ && res.frames < kProbeFramesMax) {
        const auto look = now();
        const long long ms = ms_since(dropped);
        // A dialog watched for its while, or the item not shown in the bin in
        // time; and whatever happens, not longer than both together.
        if (asked >= 0 ? ms > asked + kProbeWatchMs : settled < 0 && ms > kProbeLandMs) break;
        if (ms > kProbeLandMs + kProbeWatchMs) break;
        const int up = look_at(g, ms, "sau khi thả vào thùng rác");
        if (up > 0 && asked < 0) {
            asked = ms;
            say("hộp thoại hiện ra ngay khi món đồ vào thùng rác, trước khi bấm vào thùng rác: xem "
                "tiếp một lúc, và không bấm vào thùng rác");
        } else if (up == 0 && asked < 0) {
            w = bag::find(g, store_, l);
            if (!w.found || w.bin_empty) {
                settled = -1;
            } else if (settled < 0) {
                settled = ms;
            } else if (ms - settled >= kProbeSettleMs) {
                waiting = true;
                break;
            }
        }
        pace(look);
    }
    if (asked >= 0) {
        // What the bin asks, and when: as the item landed. The bin is left
        // alone -- what it asks is up already -- and the answer is Hủy Bỏ.
        res.at_drop = true;
        what_came_up();
        give_back();
        return finish();
    }
    if (!waiting || cancelled_ || !mouse_ok(w.trash.cx(), w.trash.cy())) {
        res.trouble = cancelled_     ? "dừng giữa chừng"
                    : !moved         ? "không điều khiển được chuột trong cửa sổ game"
                    : settled >= 0 && !waiting
                        ? "món đồ vào thùng rác, nhưng sau đó không chụp được màn hình game"
                    : !waiting       ? "món đồ không vào thùng rác"
                                     : "không bấm được vào thùng rác";
        say(res.trouble);
        give_back();
        return finish();
    }

    // 2. The bin clicked, which is what asks; then every frame for about a
    //    second and a half, kept and read, with no click and no key in
    //    between, whatever is up.
    click(w.trash.cx(), w.trash.cy());
    const auto clicked = now();
    while (!cancelled_ && res.frames < kProbeFramesMax) {
        const auto look = now();
        const long long ms = ms_since(clicked);
        if (ms > kProbeWatchMs) break;
        Frame k;
        look_at(k, ms, "sau khi bấm vào thùng rác");
        pace(look);
    }
    if (seen) what_came_up();
    else      say("bấm vào thùng rác mà không thấy hộp thoại nào");

    // 3. Hủy Bỏ on whatever came up, the bin's or not -- or, with nothing up,
    //    the item given back the way a pass gives one back.
    give_back();
    return finish();
}

} // namespace gobot
