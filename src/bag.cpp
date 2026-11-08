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
#include <mutex>
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

/// Whether `f` is black, or as good as, sampled on a coarse grid over its
/// middle: what the screen held of the game on the live client for a moment
/// now and then (2026-10-30) -- black but for the corners Windows rounds. A
/// screen copy carries whatever lies over the game, and that keeps to its
/// edges -- the taskbar, a notification, those corners -- so only the middle
/// half each way is asked, and a few points in a hundred are let off. No frame
/// of the game in the world comes near.
bool black(const Frame& f) {
    if (!f.valid()) return true;
    const int x0 = f.width / 4, x1 = f.width - f.width / 4;
    const int y0 = f.height / 4, y1 = f.height - f.height / 4;
    const int sx = std::max(1, (x1 - x0) / 48), sy = std::max(1, (y1 - y0) / 48);
    int points = 0, lit = 0;
    for (int y = y0; y < y1; y += sy)
        for (int x = x0; x < x1; x += sx) {
            std::uint8_t r = 0, g = 0, b = 0;
            f.get(x, y, r, g, b);
            ++points;
            if (r > 16 || g > 16 || b > 16) ++lit;
        }
    return lit * 100 <= points * 3;
}

/// A message with only Xác Nhận under it: what the client says when it
/// refuses something, and what it says when a charm has been used. Its
/// picture has no words. A message right after a right-click, with no question
/// asked, is never clicked; a charm's own message after its question's Xác
/// Nhận is closed with its own Xác Nhận (BagCleaner::close_notice), told by the
/// pair's label and this picture's frame round it (bag::lone_ok).
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
/// The band under the tabs' labels (2026.11.04d): the strip's bottom rows,
/// this many. The chosen tab is drawn dark there too -- 63 to 80 against 110
/// to 131 on every live picture -- and the game's red glow round a tab, which
/// lights its top rows and on 2026-11-04 left no tab darkest by
/// kChosenTabDarker over the whole inside, never reaches it.
constexpr int kTabBandRows = 4;
constexpr float kTrashScore = 0.80f;
constexpr float kButtonScore = 0.82f;

/// The quantity box, from the pair of buttons: left of them, a little inset.
constexpr int kBoxDx = -88, kBoxDy = 3, kBoxW = 82;

/// How long the quantity box is watched before its number is taken, and how
/// often it is looked at meanwhile: a whole blink of its caret, which is on
/// for about half a second and off for about as long, with some to spare --
/// or, sooner, three looks that agree over kBoxEnoughMs, longer than the
/// caret is ever on, so they span it off as well as on.
constexpr int kBoxWatchMs = 1200;
constexpr int kBoxLookMs = 100;
constexpr int kBoxEnoughMs = 750;
/// Two looks this far apart, at least, to take the item for gone after Xác
/// Nhận -- or a dialog for another's: a frame caught being drawn is not.
constexpr int kDoneApartMs = 150;

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

/// The largest of the three channels' differences between two colours.
int most_off(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t r2, std::uint8_t g2, std::uint8_t b2) {
    return std::max({ std::abs(static_cast<int>(r) - static_cast<int>(r2)),
                      std::abs(static_cast<int>(g) - static_cast<int>(g2)),
                      std::abs(static_cast<int>(b) - static_cast<int>(b2)) });
}

/// Pixel (x, y) of an icon with its own transparency: false where it is
/// see-through, or outside the icon.
bool icon_px(const Frame& icon, int x, int y, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
    if (x < 0 || y < 0 || x >= icon.width || y >= icon.height) return false;
    const std::size_t i = (static_cast<std::size_t>(y) * icon.width + x) * 4;
    if (icon.pixels[i + 3] < TemplateMatcher::kOpaque) return false;
    b = icon.pixels[i];
    g = icon.pixels[i + 1];
    r = icon.pixels[i + 2];
    return true;
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

/// What has been said once this GoBot start, by key: a charm run comes every
/// few seconds to an hour, and a slot it leaves alone for the same reason
/// each time is worth one line, not one a run.
std::mutex g_said_mutex;
std::set<std::string> g_said;

/// True the first time `key` is asked this GoBot start.
bool said_once(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_said_mutex);
    return g_said.insert(key).second;
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
    // inside 92 to 99 of 255 where theirs are 115 to 123 -- and the band under
    // its label far darker still. Only when one stands out from all the rest
    // by that much; a strip half covered says nothing. The band first: the
    // game's red glow round a tab lights the inside but never the band. The
    // inside where the band says nothing, and the two at odds say nothing.
    {
        const auto mean_of = [&f](const ScreenRect& r) {
            if (r.x < 0 || r.y < 0 || r.right() > f.width || r.bottom() > f.height || !r.valid()) return -1;
            long sum = 0;
            for (int y = r.y; y < r.bottom(); ++y)
                for (int x = r.x; x < r.right(); ++x) sum += lum(f, x, y);
            return static_cast<int>(sum / (static_cast<long>(r.w) * r.h));
        };
        const auto chosen = [](const int (&mean)[kBagTabs]) {
            int darkest = 0;
            for (int t = 0; t < kBagTabs; ++t) {
                if (mean[t] < 0) return -1;
                if (mean[t] < mean[darkest]) darkest = t;
            }
            for (int t = 0; t < kBagTabs; ++t)
                if (t != darkest && mean[t] - mean[darkest] < kChosenTabDarker) return -1;
            return darkest;
        };
        for (int t = 0; t < kBagTabs; ++t) {
            const int x = v.strip.x + t * l.tab_w + 2;
            v.tab_mean[t] = mean_of(ScreenRect{ x, v.strip.y + 2, l.tab_w - 4, v.strip.h - 4 });
            v.tab_band[t] = mean_of(ScreenRect{ x, v.strip.y + v.strip.h - kTabBandRows, l.tab_w - 4, kTabBandRows });
        }
        const int by_band = chosen(v.tab_band);
        const int by_inside = chosen(v.tab_mean);
        v.selected_tab = by_band < 0 ? by_inside : (by_inside < 0 || by_inside == by_band) ? by_band : -1;
    }

    // The bin is where the layout says. The can is only drawn in it while it
    // is empty, so looking for the can says whether it is -- looked for right
    // there and nowhere else.
    v.trash = ScreenRect{ v.strip.x + l.bin_dx, v.strip.y + l.bin_dy, l.bin_w, l.bin_h };
    v.bin_on_frame = v.trash.x >= 0 && v.trash.y >= 0 && v.trash.right() <= f.width && v.trash.bottom() <= f.height;
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

IconPlace place_icon(const Frame& f, const ScreenRect& slot, const Frame& icon, const BagLayout& l) {
    IconPlace best;
    if (!f.valid() || !icon.valid() || !slot.valid()) return best;
    // In colour, pixel for pixel: the bag draws a charm's icon exactly as the
    // catalog has it -- 0 off on every one of the eight, live -- so what is
    // measured is how far the slot is from that, not how alike in shape.
    const int mid_x = (slot.w - icon.width) / 2, mid_y = (slot.h - icon.height) / 2;
    for (int oy = mid_y - kBareSlack; oy <= mid_y + kBareSlack; ++oy)
        for (int ox = mid_x - kBareSlack; ox <= mid_x + kBareSlack; ++ox) {
            long total = 0;
            int n = 0;
            for (int y = l.edge; y < slot.h - l.edge; ++y)
                for (int x = l.edge; x < slot.w - l.edge; ++x) {
                    if (x < kCountCornerW && y < kCountCornerH) continue;                  // the count's
                    if (x >= slot.w - l.badge_w && y >= slot.h - l.badge_h) continue;     // a badge's
                    std::uint8_t ir, ig, ib;
                    if (!icon_px(icon, x - ox, y - oy, ir, ig, ib)) continue;
                    std::uint8_t r, g, b;
                    f.get(slot.x + x, slot.y + y, r, g, b);
                    total += most_off(r, g, b, ir, ig, ib);
                    ++n;
                }
            if (n < kBareFitLeast) continue;
            const float mean = static_cast<float>(total) / static_cast<float>(n);
            if (!best.found || mean < best.mean) {
                best.found = true;
                best.x = ox;
                best.y = oy;
                best.mean = mean;
            }
        }
    return best;
}

bool icon_fits(const Frame& f, const ScreenRect& slot, const Frame& icon, const BagLayout& l, float* fit) {
    const IconPlace p = place_icon(f, slot, icon, l);
    if (fit) *fit = p.mean;
    return p.found && p.mean <= kBareFitMean;
}

bool bare_corner(const Frame& f, const ScreenRect& slot, const Frame& icon, const BagLayout& l, int* unlike,
                 float* fit) {
    if (unlike) *unlike = -1;
    const IconPlace p = place_icon(f, slot, icon, l);
    if (fit) *fit = p.mean;
    if (!p.found || p.mean > kBareFitMean) return false;
    // Every pixel of the corner, the edge left out -- the hover and selection
    // highlights' -- as the icon has it there, or dark where it has none.
    // Strokes and outline alike: a count is no white needed to be one.
    int off = 0;
    const int w = std::min(kCountCornerW, slot.w - l.edge), h = std::min(kCountCornerH, slot.h - l.edge);
    for (int y = l.edge; y < h; ++y)
        for (int x = l.edge; x < w; ++x) {
            std::uint8_t r, g, b;
            f.get(slot.x + x, slot.y + y, r, g, b);
            std::uint8_t ir, ig, ib;
            if (icon_px(icon, x - p.x, y - p.y, ir, ig, ib)) {
                if (most_off(r, g, b, ir, ig, ib) > kBareOff) ++off;
            } else if ((r + g + b) / 3 > kBareGround) {
                ++off;
            }
        }
    if (unlike) *unlike = off;
    return off == 0;
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

int CountReader::read_charm(const Frame& f, const ScreenRect& slot, const Frame* icon, const BagLayout& l,
                            bool* bare) const {
    if (bare) *bare = false;
    if (!loaded_ || !f.valid()) return kUnreadable;
    // The corner read reads, read the same way. A number printed there is
    // from 2 up: a lone 1 is the icon's own white, read as a digit.
    int white = 0, stray = 0;
    const int n = scan(f, ScreenRect{ slot.x, slot.y, bag::kCountCornerW, bag::kCountCornerH }, 6, false, true,
                       white, stray);
    const bool number = white > 0 && n >= 2 && n <= 9999;
    if (icon != nullptr && icon->valid()) {
        if (number) return n;
        // No number: a stack of one only with the corner the icon's own,
        // nothing over it -- white there or not. A count not printed in white,
        // or not read, over 8871081's corner, which has no white, is no one.
        if (!bag::bare_corner(f, slot, *icon, l)) return kUnreadable;
        if (bare) *bare = true;
        return 1;
    }
    if (white == 0) return 1;                 // nothing printed: a stack of one
    return number ? n : kUnreadable;
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
    item.colour_of = e.id;
    item.action = a;
    item.min_score = min_score;
    item.picture = icon_picture(e);
    item.shown = e.tile.valid() ? e.tile : slot_tile(e.icon);
    // A charm's icon whole, listed or a rival: every colour's, for telling its
    // stack of one by (CountReader::read_charm), chosen or not.
    if (bag::use_kind_of(e.id) == UseKind::Charm) item.bare = e.icon;
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

/// How a charm on the Dùng list is told from its drawing in another colour,
/// each of which is another charm (TrashList::judge). Measured on the live
/// client's slot of 8871081 (Tiêu Dùng row 1, cell 2, on 2026-11-02 and
/// 11-03), against the catalog's five colours of that card: in grey all five
/// score 0.93 to 0.98 there, which tells nothing; in colour its own scores
/// 0.952 and 0.974, the best other -- blue, 8871021 -- 0.794 and 0.816, a
/// margin of 0.158 on both days, red 0.73 to 0.75 and green 0.52 to 0.54.
/// Its mean colour there is 164,78,192 against its icon's 166,77,196, 4 off
/// at most in a channel; the other colours' icons are 30 to 99 off in one
/// channel at least. Not held to more than 0.90: the right colour came to
/// 0.952 on one of the two days, and a charm of the player's own kind left
/// alone is a charm not used. A hue no icon has is what the mean is for.
///
/// The margin is over the colours whose mean is near as well: the catalog's
/// icons are nearer one another than that slot was to the other colours --
/// icon against icon the grey card 8871011 scores 0.911 on the green 41 and
/// 0.906 on the red 31, the grey diamond 51 0.920 on the pink 61 -- and each
/// of those is 39 to 97 off in its mean, which no slot of the right colour
/// has come near.
constexpr float kCharmColourLeast = 0.90f;
constexpr float kCharmColourMargin = 0.10f;
constexpr int kCharmMeanOff = 20;
/// A picture with fewer opaque pixels than this has too few to tell a colour by.
constexpr int kColourPixelsLeast = 100;

/// How alike the frame is in colour to `pic` placed at x, y: one correlation
/// over the picture's opaque pixels of red, green and blue as one list -- so a
/// purple card and a blue one, one drawing, come out far apart, where one
/// correlation each channel would take each channel's mean out and with it
/// the very hue told by -- and how far the frame's mean colour there is from
/// the picture's own, the largest of the three channels' differences. False
/// for a place off the frame, a picture with fewer than kColourPixelsLeast
/// opaque pixels, or either side flat.
bool colour_likeness(const Frame& f, int x, int y, const Frame& pic, float& likeness, int& mean_off) {
    if (!f.valid() || !pic.valid() || x < 0 || y < 0 || x + pic.width > f.width || y + pic.height > f.height)
        return false;
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    long long frame_sum[3] = {}, pic_sum[3] = {};
    int n = 0;
    for (int py = 0; py < pic.height; ++py)
        for (int px = 0; px < pic.width; ++px) {
            const std::size_t i = (static_cast<std::size_t>(py) * pic.width + px) * 4;
            if (pic.pixels[i + 3] < TemplateMatcher::kOpaque) continue;
            std::uint8_t r = 0, g = 0, b = 0;
            f.get(x + px, y + py, r, g, b);
            const int fc[3] = { r, g, b };
            const int pc[3] = { pic.pixels[i + 2], pic.pixels[i + 1], pic.pixels[i] };
            for (int c = 0; c < 3; ++c) {
                sa += fc[c];
                sb += pc[c];
                saa += static_cast<double>(fc[c]) * fc[c];
                sbb += static_cast<double>(pc[c]) * pc[c];
                sab += static_cast<double>(fc[c]) * pc[c];
                frame_sum[c] += fc[c];
                pic_sum[c] += pc[c];
            }
            ++n;
        }
    if (n < kColourPixelsLeast) return false;
    const double m = 3.0 * n;
    const double va = saa - sa * sa / m, vb = sbb - sb * sb / m;
    if (va <= m || vb <= m) return false;       // flat: nothing to compare
    likeness = static_cast<float>((sab - sa * sb / m) / std::sqrt(va * vb));
    mean_off = 0;
    for (int c = 0; c < 3; ++c)
        mean_off = std::max(mean_off, static_cast<int>(std::lround(
                                          std::abs(static_cast<double>(frame_sum[c] - pic_sum[c])) / n)));
    return true;
}

/// Which of a charm's colours -- their catalog icons in `pics` -- the frame
/// shows where the grey match put the drawing, `at`, give or take a pixel:
/// each icon at its best place there, anywhere it fits inside `at` -- a
/// picture cut from the bag is a whole slot, the icon a little narrower --
/// and a pixel beyond. Decided only when the best scores kCharmColourLeast or
/// more and its mean colour is within kCharmMeanOff of the frame's there, and
/// every other colour either scores kCharmColourMargin less or is more than
/// kCharmMeanOff off in its mean. `next` is the second best, for the log.
struct ColourCall {
    std::string best, next;
    float best_score = -1.0f, next_score = -1.0f;
    int mean_off = 255;
    bool decided = false;
};
ColourCall decide_colour(const Frame& f, const ScreenRect& at,
                         const std::vector<std::pair<std::string, const Frame*>>& pics) {
    struct Scored {
        std::string id;
        float score = -1.0f;
        int   off = 255;
    };
    std::vector<Scored> scored;
    for (const auto& [id, pic] : pics) {
        if (pic == nullptr || !pic->valid()) continue;
        Scored one;
        one.id = id;
        const int x1 = at.x + std::max(0, at.w - pic->width) + 1;
        const int y1 = at.y + std::max(0, at.h - pic->height) + 1;
        for (int y = at.y - 1; y <= y1; ++y)
            for (int x = at.x - 1; x <= x1; ++x) {
                float s = 0.0f;
                int o = 0;
                if (colour_likeness(f, x, y, *pic, s, o) && s > one.score) {
                    one.score = s;
                    one.off = o;
                }
            }
        if (one.score > -1.0f) scored.push_back(one);
    }
    ColourCall c;
    if (scored.empty()) return c;
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) { return a.score > b.score; });
    c.best = scored[0].id;
    c.best_score = scored[0].score;
    c.mean_off = scored[0].off;
    if (scored.size() > 1) {
        c.next = scored[1].id;
        c.next_score = scored[1].score;
    }
    c.decided = c.best_score >= kCharmColourLeast && c.mean_off <= kCharmMeanOff &&
                std::all_of(scored.begin() + 1, scored.end(), [&](const Scored& o) {
                    return c.best_score - o.score >= kCharmColourMargin || o.off > kCharmMeanOff;
                });
    return c;
}

/// Catalog icon `id` as a slot is matched against it (icon_picture): the
/// listed item's picture when the list has it, else the rival's. Null when
/// neither has it.
const Frame* icon_of(const TrashList& list, const std::string& id) {
    for (const TrashList::Item& it : list.items)
        if (it.id == id && it.picture.valid()) return &it.picture;
    for (const TrashList::Item& r : list.rivals)
        if (r.id == id && r.picture.valid()) return &r.picture;
    return nullptr;
}

/// Charm colour `id`'s catalog icon whole (TrashList::Item::bare): the listed
/// item's when the list has it, else the rival's. Null when neither has it --
/// a colour not told, or not the catalog's. Never a picture cut from the bag,
/// whose corner may have its count in it.
const Frame* bare_of(const TrashList& list, const std::string& id) {
    if (id.empty()) return nullptr;
    for (const TrashList::Item& it : list.items)
        if (it.id == id && it.bare.valid()) return &it.bare;
    for (const TrashList::Item& r : list.rivals)
        if (r.id == id && r.bare.valid()) return &r.bare;
    return nullptr;
}

/// The charm colours among `kin`, each with its icon, for decide_colour.
std::vector<std::pair<std::string, const Frame*>> charm_colours(const TrashList& list,
                                                                const std::vector<std::string>& kin) {
    std::vector<std::pair<std::string, const Frame*>> out;
    for (const std::string& id : kin)
        if (bag::use_kind_of(id) == UseKind::Charm)
            if (const Frame* p = icon_of(list, id)) out.emplace_back(id, p);
    return out;
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
    // The same for charms: off unless the file says so, every load.
    use_charms       = ini.get_bool("Trash", "use_charms", false);
    charm_every_minutes = std::clamp(ini.get_int("Trash", "charm_every_minutes", charm_every_minutes),
                                     kCharmEveryLeast, kCharmEveryMost);
    charm_wait_ms    = std::clamp(ini.get_int("Trash", "charm_wait_ms", charm_wait_ms), kCharmWaitLeast,
                                  kCharmWaitMost);
    charm_frame_score = std::clamp(ini.get_float("Trash", "charm_frame_score", charm_frame_score),
                                   kCharmFrameLeast, kCharmFrameMost);
    // The potion shop: off unless the file says so, every load.
    buy_potions      = ini.get_bool("Trash", "buy_potions", false);
    buy_every_minutes = std::clamp(ini.get_int("Trash", "buy_every_minutes", buy_every_minutes), kBuyEveryLeast,
                                   kBuyEveryMost);
    buy_hp_to        = std::clamp(ini.get_int("Trash", "buy_hp_to", buy_hp_to), 0, kBuySlotsMost);
    buy_hp_below     = std::clamp(ini.get_int("Trash", "buy_hp_below", buy_hp_below), 0, buy_hp_to);
    buy_mp_to        = std::clamp(ini.get_int("Trash", "buy_mp_to", buy_mp_to), 0, kBuySlotsMost);
    buy_mp_below     = std::clamp(ini.get_int("Trash", "buy_mp_below", buy_mp_below), 0, buy_mp_to);
    buy_opener       = ini.get("Trash", "buy_opener", buy_opener);
    if (buy_opener.empty()) buy_opener = "other/8950604";
    buy_frame_score  = std::clamp(ini.get_float("Trash", "buy_frame_score", buy_frame_score), kBuyFrameLeast,
                                  kBuyFrameMost);
    click_to_move    = ini.get_bool("Trash", "click_to_move", click_to_move);
    press_ms         = std::clamp(ini.get_int("Trash", "press_ms", press_ms), kPressLeastMs, kPressMostMs);
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

        // Quietly: the charm job builds the list every half minute, and the
        // log had a line for each folder each time (2026-11-03).
        TemplateStore store;
        store.load(sub, true);
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
        // Only fragments and charms are used from the catalog: a right-click
        // on anything else may use it, or equip it, there and then. Nor does a
        // drawing in another group come along with one -- it stays a rival,
        // and a slot that looks as much like it is left alone.
        const UseKind kind = bag::use_kind_of(id);
        if (a == Action::Use && kind == UseKind::None) continue;
        Item item = catalog_item(*e, a, min_score);
        item.kin = catalog.same_drawing(id);
        if (a == Action::Use)
            item.kin.erase(std::remove_if(item.kin.begin(), item.kin.end(),
                                          [kind](const std::string& k) { return bag::use_kind_of(k) != kind; }),
                           item.kin.end());
        item.kin.push_back(id);
        items.push_back(std::move(item));
    }
    // The same drawing in other colours goes too, each as an item of its own:
    // its own icon is what matches it best, and the screen can say which
    // chosen icon brought it. Held to that one's group, so a drawing that is
    // only like one of these, and not like what was chosen, is a rival still.
    // Not for a charm to use: each colour of a charm is another effect -- the
    // purple card 8871081 is "Kinh nghiệm tăng 10" -- and only the colours
    // chosen are used. The others are left off the list, among the rivals,
    // and a slot of one is told by its colour and left alone (judge).
    const std::size_t chosen = items.size();
    for (std::size_t i = first; i < chosen; ++i) {
        if (a == Action::Use && bag::use_kind_of(items[i].id) == UseKind::Charm) continue;
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

    // A charm cut from the bag for the Dùng list: which of those colours it
    // is, decided once, here -- by the same rule a slot's colour is told by
    // (judge), against the same icons, which cover only the icon's own
    // pixels. Never the cut against a slot in colour: a cut keeps the whole
    // slot, its dark ground too, which would bring two hues nearer than was
    // ever measured. Undecided, it stays empty, and every slot the cut takes
    // is a doubt (Doubt::Colour) -- never right-clicked.
    for (Item& it : items) {
        if (!it.id.empty() || it.action != Action::Use || !it.charm()) continue;
        it.colour_of.clear();
        const auto pics = charm_colours(*this, it.kin);
        if (pics.empty() || !it.shown.valid()) continue;
        const ColourCall c = decide_colour(it.shown, ScreenRect{ 0, 0, it.shown.width, it.shown.height }, pics);
        if (c.decided) it.colour_of = c.best;
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
    // Only a fragment's stack has a rest to delete after: a charm is used one
    // at a time, an hour apart, and what is left of its stack is for the
    // next hour.
    for (Item& it : items)
        if (it.action == Action::Use && it.fragment() && (on_delete(it.id) || on_delete(it.via)))
            it.then_delete = true;
    add_rivals(catalog);
    // A charm cut from the bag whose colour add_rivals could not tell: never
    // used, and said once a GoBot start -- the list is built again every
    // half minute.
    for (const Item& it : items) {
        if (!it.id.empty() || it.action != Action::Use || !it.charm() || !it.colour_of.empty()) continue;
        if (!said_once("cut colour|" + it.file)) continue;
        const ColourCall c = decide_colour(it.shown, ScreenRect{ 0, 0, it.shown.width, it.shown.height },
                                           charm_colours(*this, it.kin));
        GB_INFO("Trash: the charm cut from the bag %s: colour not sure (best %s %.3f, next %s %.3f, mean off %d) -- "
                "never right-clicked",
                it.clock_id().c_str(), c.best.empty() ? "none" : c.best.c_str(), static_cast<double>(c.best_score),
                c.next.empty() ? "none" : c.next.c_str(), static_cast<double>(c.next_score), c.mean_off);
    }
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
    // Each item's own match, kept for the colour of a charm below: a slot is
    // only ever another listed item's whose own drawing matched it.
    std::vector<TemplateMatcher::Match> grey(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& item = items[i];
        const Frame& p = item.picture;
        if (!p.valid() || area.w < p.width || area.h < p.height) continue;
        const float need = item.min_score > 0.0f ? item.min_score : min_score;
        const auto m = TemplateMatcher::find(f, area, p, need, TemplateMatcher::Tone::Grey);
        if (!m.found) continue;
        grey[i] = m;
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
    // colour is not a rival -- by the player's rule it is the same thing; a
    // charm's colour is told below.
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

    // A charm to use: its drawing is every colour's, and each colour is
    // another charm. Which one the slot holds is told by its colour, where
    // the drawing was found -- and a colour that is not sure leaves the slot
    // alone: a charm of the player's not used is a charm used later, one
    // used that was not chosen is gone. So too what won on the delete or drop
    // list drawn like a charm on the Dùng list, when that charm's own drawing
    // missed the slot: a slot of the colour ticked to use, or of one brought
    // along to the delete list by another colour, is not deleted for it. Its
    // kin may hold more than charms, so that is not asked of it.
    const bool use_drawing =
        won.action != Action::Use && std::any_of(items.begin(), items.end(), [&](const Item& u) {
            return u.action == Action::Use && u.charm() &&
                   std::any_of(u.kin.begin(), u.kin.end(), [&](const std::string& id) {
                       return bag::use_kind_of(id) == UseKind::Charm &&
                              std::find(won.kin.begin(), won.kin.end(), id) != won.kin.end();
                   });
        });
    if (won.action == Action::Use ? !won.charm() : !use_drawing) return v;
    const ColourCall c = decide_colour(f, grey[static_cast<std::size_t>(v.item)].at, charm_colours(*this, won.kin));
    v.colour_id = c.best;
    v.colour = c.best_score;
    v.colour_next_id = c.next;
    v.colour_next = c.next_score;
    v.colour_mean_off = c.best.empty() ? -1 : c.mean_off;
    const auto not_taken = [&](Verdict::Doubt why) {
        v.doubt = why;
        v.like = v.item;
        v.like_score = v.score;
        v.item = -1;
        v.score = 0.0f;
        v.passed_over = -1;
        v.passed_over_score = 0.0f;
        return v;
    };
    if (!c.decided) return not_taken(Verdict::Doubt::Colour);
    // That colour's on the Dùng list -- the one whose drawing won, or another
    // chosen as well -- whose own drawing matched the slot: a colour that won
    // on another item's drawing alone is not taken for it.
    int to = -1;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        if (!grey[i].found || it.action != Action::Use || it.colour_of != c.best || !it.charm()) continue;
        if (to < 0 || grey[i].score > grey[static_cast<std::size_t>(to)].score) to = static_cast<int>(i);
    }
    if (to >= 0) {
        v.item = to;
        v.score = grey[static_cast<std::size_t>(to)].score;
        return v;
    }
    // A picture cut from the bag whose own colour could not be told when the
    // list was built (add_rivals): which colours the player meant to keep is
    // not known, so nothing it wins is taken for anything else -- deleted
    // least of all.
    if (won.id.empty() && won.colour_of.empty()) return not_taken(Verdict::Doubt::Colour);
    // Else that very id on the delete or drop list, chosen there itself and
    // not brought along by another colour: Dùng the purple card and delete the
    // grey one, and a grey one is deleted. One that came along with a colour
    // chosen there (via), like anything not on the list, is left alone.
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        if (!grey[i].found || it.action == Action::Use || it.id != c.best || !it.via.empty()) continue;
        if (to < 0 || grey[i].score > grey[static_cast<std::size_t>(to)].score) to = static_cast<int>(i);
    }
    if (to >= 0) {
        v.item = to;
        v.score = grey[static_cast<std::size_t>(to)].score;
        v.passed_over = -1;
        v.passed_over_score = 0.0f;
        return v;
    }
    return not_taken(Verdict::Doubt::None);
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
        // A charm's icon chosen to delete or drop for itself: a slot of that
        // very colour is deleted, a charm on the Dùng list being told by its
        // colour (judge) -- it gives way to none.
        if (!it.id.empty() && it.via.empty() && bag::use_kind_of(it.id) == UseKind::Charm) continue;
        if ((!it.id.empty() && on_use(it.id)) || std::any_of(it.kin.begin(), it.kin.end(), on_use))
            out.push_back(i);
    }
    return out;
}

UseKind TrashList::Item::use_kind() const {
    // A catalog icon is one by its id. A picture cut from the bag is one by
    // what add_rivals found it drawn like: icons of one kind, and nothing else
    // -- a picture drawn like no icon at all, or like a potion as well, or
    // like a fragment and a charm both, is none.
    if (!id.empty() && bag::use_kind_of(id) == UseKind::None) return UseKind::None;
    if (kin.empty()) return UseKind::None;
    const UseKind first = bag::use_kind_of(kin.front());
    if (first == UseKind::None) return UseKind::None;
    for (const std::string& k : kin)
        if (bag::use_kind_of(k) != first) return UseKind::None;
    return first;
}

bool TrashList::Item::fragment() const { return use_kind() == UseKind::Fragment; }
bool TrashList::Item::charm() const { return use_kind() == UseKind::Charm; }

namespace bag {

UseKind use_kind_of(const std::string& id) {
    if (id.rfind("spend/fragment/", 0) == 0) return UseKind::Fragment;
    if (id.rfind("spend/buff/", 0) == 0) return UseKind::Charm;
    return UseKind::None;
}

bool fragment_cut(const Frame& cut, const BagLayout& l, const ItemCatalog& catalog) {
    // The very list a pass would build with this picture on the Dùng list,
    // and the very test the pass puts it to.
    TrashList one;
    one.add_picture(cut, TrashList::Action::Use, l);
    one.add_rivals(catalog);
    return !one.items.empty() && one.items.front().fragment();
}

bool charm_cut(const Frame& cut, const BagLayout& l, const ItemCatalog& catalog) {
    TrashList one;
    one.add_picture(cut, TrashList::Action::Use, l);
    one.add_rivals(catalog);
    return !one.items.empty() && one.items.front().charm();
}

} // namespace bag

// ------------------------------------------------------------------ charm clock

std::string CharmClock::default_path() {
    // Outside the program folder, as the webhook and the lines Tự chat said are
    // kept: a new version is unzipped into a new folder, and the charm used an
    // hour ago is still working when it starts.
    static const std::string path = [] {
        wchar_t env[MAX_PATH * 2] = {};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", env, static_cast<DWORD>(std::size(env)));
        if (n > 0 && n < std::size(env)) return wide_to_utf8(std::wstring(env) + L"\\GoBot\\charm_clock.txt");
        return exe_relative("charm_clock.txt");
    }();
    return path;
}

bool CharmClock::load() {
    // What is known stays known: the file read again adds to it, a later time
    // of either wins, and one that cannot be read takes nothing away. A write
    // that failed, or was refused for a moment, must not make the charm just
    // used due again.
    std::FILE* f = open_file(path_, "rb");
    if (f == nullptr) return false;
    std::string text;
    char buf[1024];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    // One id and its time a line, "id=seconds": the id may hold any character
    // but a line break, a picture's path among them, so split at the last '='.
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const std::size_t eq = line.rfind('=');
        if (line.empty() || eq == std::string::npos || eq == 0) continue;
        const std::string seconds = line.substr(eq + 1);
        if (seconds.empty() || seconds.find_first_not_of("0123456789") != std::string::npos ||
            seconds.size() > 15)
            continue;
        std::int64_t& known = next_[line.substr(0, eq)];
        known = std::max(known, static_cast<std::int64_t>(std::stoll(seconds)));
    }
    return true;
}

bool CharmClock::due(const std::string& id, std::int64_t now_s) const {
    const auto it = next_.find(id);
    if (it == next_.end() || now_s >= it->second) return true;
    // A time further ahead than any charm lasts is not one of ours: a clock
    // that was set forward when it was written, or a file edited by hand.
    return it->second - now_s > static_cast<std::int64_t>(CleanSettings::kCharmEveryMost) * 60 + 3600;
}

std::int64_t CharmClock::next(const std::string& id) const {
    const auto it = next_.find(id);
    return it == next_.end() ? 0 : it->second;
}

bool CharmClock::set_next(const std::string& id, std::int64_t at_s) {
    if (id.empty() || id.find('\n') != std::string::npos) return false;
    next_[id] = at_s;
    return save();
}

bool CharmClock::save() {
    // What the file holds now first: another GoBot -- or another thread -- may
    // have written since this one read it, and a time it set is not lost to a
    // write of ours.
    CharmClock on_disk(path_);
    if (on_disk.load())
        for (const auto& each : on_disk.next_) {
            const auto mine = next_.find(each.first);
            if (mine == next_.end()) next_[each.first] = each.second;
        }
    const std::size_t slash = path_.find_last_of("\\/");
    if (slash != std::string::npos) make_folder(path_.substr(0, slash));
    std::string text;
    for (const auto& each : next_) text += each.first + "=" + std::to_string(each.second) + "\n";
    // Written beside it and moved over it, so that a reader never finds half.
    const std::string fresh = path_ + ".new";
    std::FILE* f = open_file(fresh, "wb");
    if (f == nullptr) return false;
    bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
        remove_file(fresh);
        return false;
    }
    if (!MoveFileExW(utf8_to_wide(fresh).c_str(), utf8_to_wide(path_).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        remove_file(fresh);
        return false;
    }
    return true;
}

std::int64_t CharmClock::next_after(How how, int every_minutes, std::int64_t now_s) {
    const int minutes = how == How::NotUsed ? kRetryMinutes : std::max(1, every_minutes);
    return now_s + static_cast<std::int64_t>(minutes) * 60;
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
    // Nor a black frame, which shows nothing either way.
    if (!f.valid() || black(f)) return false;
    // Any dialog at all: one of the bin's whose words did not match is still
    // the bin's, and holds the item.
    if (!find_dialogs(f, store).empty()) return false;
    const BagView v = find(f, store, l);
    // A bin out of the frame holds nothing anybody can be typing into: no
    // dialog is up, and GoBot can put nothing there.
    return !v.found || v.bin_empty || !v.bin_on_frame;
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

namespace {

/// Where the two buttons are in picture `pic`, from its top-left, by either
/// drawing find_dialogs knows them by.
bool pair_in(const Frame& pic, const TemplateStore& store, int& dx, int& dy) {
    const Frame* pair = store.get(kConfirm);
    if (pair == nullptr || !pic.valid()) return false;
    std::vector<Frame> drawings{ *pair };
    if (const Frame* ask = store.get(kBinDialogs[bag::kAskDialog].name)) {
        Frame other = crop(*ask, ScreenRect{ kBinDialogs[bag::kAskDialog].pair_dx,
                                             kBinDialogs[bag::kAskDialog].pair_dy, pair->width, pair->height });
        if (other.valid()) drawings.push_back(std::move(other));
    }
    const ScreenRect all{ 0, 0, pic.width, pic.height };
    for (const Frame& p : drawings) {
        const auto m = TemplateMatcher::find(pic, all, p, kButtonScore);
        if (m.found) {
            dx = m.at.x;
            dy = m.at.y;
            return true;
        }
    }
    return false;
}

/// The words "Xác Nhận" on the left button of the pair's picture (kConfirm),
/// x 4..37 and y 4..16 from its top-left: what a message's lone button is told
/// by. The button is 4 left of them and 4 above, and 42 wide, as each half of
/// the pair is.
constexpr ScreenRect kOkLabel{ 4, 4, 34, 13 };
constexpr int kOkButtonW = 42;
/// How alike the label, the frame right of its button and the button with the
/// frame's corner have to be (bag::lone_ok). On the live message (2026-11-02
/// and 11-03) each scores 1.000; the label about 0.38 on the question's Hủy Bỏ,
/// the frame under 0.1 right of the question's Xác Nhận, and the corner about
/// 0.86 on its Hủy Bỏ and 0.73 on its Xác Nhận.
constexpr float kOkLabelScore = 0.95f;
constexpr float kLoneEdgeScore = 0.90f;
constexpr float kLoneCornerScore = 0.95f;
/// Where a charm's message has its button, from the pair of the question that
/// brought it: where that question's Hủy Bỏ was -- 557,397 on the live client
/// for a pair at 514,397. Its label may be three pixels either way of that, and
/// from 20 above to 40 below: a box taller for a longer text keeps its button
/// at its bottom right.
constexpr int kMessageDx = 43;
constexpr int kMessageSlack = 3, kMessageAbove = 20, kMessageBelow = 40;
/// The question's box from its pair, measured on the live client: 410-614 x
/// 341-429 for a pair at 514,397.
constexpr ScreenRect kQuestionBox{ -104, -56, 204, 88 };

/// The pieces a lone Xác Nhận is told by, cut from pictures GoBot ships: the
/// label from kConfirm; and from kNotice -- where that label is found in it, at
/// 149,57 -- the frame right of the button and under it from 4 above the button
/// (`edge`, 12x28), and the button with the frame's corner round it (`corner`,
/// 58x28). Each with where it sits from the label's top-left.
struct LonePieces {
    Frame label, edge, corner;
    int   edge_dx = 0, edge_dy = 0, corner_dx = 0, corner_dy = 0;
};

bool lone_pieces(const TemplateStore& store, LonePieces& out) {
    const Frame* pair = store.get(kConfirm);
    const Frame* notice = store.get(kNotice);
    if (pair == nullptr || notice == nullptr) return false;
    out.label = crop(*pair, kOkLabel);
    if (!out.label.valid() || notice->width < out.label.width || notice->height < out.label.height) return false;
    const auto m = TemplateMatcher::find(*notice, ScreenRect{ 0, 0, notice->width, notice->height }, out.label,
                                         kOkLabelScore);
    if (!m.found) return false;
    const int bx = m.at.x - kOkLabel.x, by = m.at.y - kOkLabel.y;
    const ScreenRect edge{ bx + kOkButtonW, by - 4, notice->width - (bx + kOkButtonW), notice->height - (by - 4) };
    const ScreenRect corner{ bx - 4, by - 4, notice->width - (bx - 4), notice->height - (by - 4) };
    out.edge = crop(*notice, edge);
    out.corner = crop(*notice, corner);
    // A frame worth the name on either side: a few pixels at least.
    if (!out.edge.valid() || !out.corner.valid() || out.edge.width < 4 || out.edge.height < 8) return false;
    out.edge_dx = edge.x - m.at.x;
    out.edge_dy = edge.y - m.at.y;
    out.corner_dx = corner.x - m.at.x;
    out.corner_dy = corner.y - m.at.y;
    return true;
}

/// How alike `piece` is to the frame at x, y, give or take `slack` pixels: the
/// best there, or -1 off the frame.
float piece_at(const Frame& f, const Frame& piece, int x, int y, int slack) {
    const ScreenRect area = clamp_to(f, ScreenRect{ x - slack, y - slack, piece.width + 2 * slack,
                                                    piece.height + 2 * slack });
    if (area.w < piece.width || area.h < piece.height) return -1.0f;
    const auto m = TemplateMatcher::find(f, area, piece, -1.0f);
    return m.found ? m.score : -1.0f;
}

/// The frame of a message right of the button whose label is at `label`, as a
/// lone Xác Nhận has it: what is still there with its button lit, and gone
/// with the message.
bool message_edge_at(const Frame& f, const TemplateStore& store, const ScreenRect& label) {
    LonePieces p;
    if (!f.valid() || !lone_pieces(store, p)) return false;
    return piece_at(f, p.edge, label.x + p.edge_dx, label.y + p.edge_dy, 1) >= kLoneEdgeScore;
}

/// How much of the registration question's box has to be there, under its
/// words, for another question to be taken for a box of the same kind. Rows
/// from a little above its buttons to its bottom edge, the whole width: the
/// buttons, the margins either side of them and the frame's lower edge, none
/// of it words. A pointer over a button lowers it a little. The score it has
/// to reach is the settings' (charm_frame_score).
constexpr int kFrameAbove = 6;

/// The questions other_question_up tells a charm's from, by their pictures.
constexpr const char* kOtherQuestions[] = { "grabbed/hoisinh_confirm", "grabbed/drop_confirm",
                                            "grabbed/sell_confirm" };

/// How alike the words over `d`'s buttons are to question picture `name`'s,
/// where that picture puts them: -1 for a picture that is not there, has no
/// buttons where one has them, or has no words to tell by -- the drop
/// question's -- and else at least kWordsScore or -1.
float other_words(const Frame& f, const TemplateStore& store, const Dialog& d, const char* name) {
    const Frame* pic = store.get(name);
    if (pic == nullptr) return -1.0f;
    int dx = 0, dy = 0;
    if (!pair_in(*pic, store, dx, dy) || dy < 20 || pic->width < 40) return -1.0f;
    // The panel of words above the buttons, inset as the registration's is:
    // what a picture of a question tells it from the others by. A picture with
    // no words in it, the drop question's, tells nothing.
    const DialogPicture dp = { name, dx, dy, false, ScreenRect{ 17, 11, pic->width - 34, dy - 15 } };
    return words_at(f, store, d.pair, dp, kWordsScore);
}

} // namespace

bool other_question_up(const Frame& f, const TemplateStore& store, const Dialog& d) {
    for (const char* name : kOtherQuestions)
        if (other_words(f, store, d, name) >= kWordsScore) return true;
    return false;
}

bool charm_question(const Frame& f, const TemplateStore& store, const Dialog& d, float frame_score) {
    // The bin's questions and the fragment registration's are other
    // questions, and are told by their words.
    if (d.ours() || d.registers) return false;
    // The game's dialog as the registration question is drawn: its box under
    // the words, where it sits for these buttons.
    const Frame* ask = store.get(kRegisterPicture.name);
    if (ask == nullptr || !f.valid()) return false;
    const int top = kRegisterPicture.pair_dy - kFrameAbove;
    const Frame strip = crop(*ask, ScreenRect{ 0, top, ask->width, ask->height - top });
    if (!strip.valid()) return false;
    const ScreenRect place = clamp_to(f, ScreenRect{ d.pair.x - kRegisterPicture.pair_dx - 2,
                                                     d.pair.y - kFrameAbove - 2, strip.width + 4,
                                                     strip.height + 4 });
    if (place.w < strip.width || place.h < strip.height) return false;
    if (!TemplateMatcher::find(f, place, strip, frame_score).found) return false;
    // And none of the other questions it has a picture of.
    return !other_question_up(f, store, d);
}

bool can_check_charm(const TemplateStore& store) {
    if (store.get(kConfirm) == nullptr) return false;
    for (const DialogPicture& pic : kBinDialogs)
        if (store.get(pic.name) == nullptr) return false;
    // The registration question's box is what a charm's is compared with.
    if (store.get(kRegisterPicture.name) == nullptr) return false;
    // And every other question it has to tell a charm's from: with one of them
    // missing, that question could be taken for a charm's.
    for (const char* name : kOtherQuestions)
        if (store.get(name) == nullptr) return false;
    // And the message a charm's use brings, to close it with its own Xác Nhận:
    // the pair's label found in its picture, with the frame round it.
    LonePieces lone;
    if (!lone_pieces(store, lone)) return false;
    // And the close buttons, to know of another window open beside the bag.
    return can_see_windows(store);
}

// ---------------------------------------------------------------- the shop

namespace {

/// How alike the shop's pictures have to be: its first row and its buttons,
/// each looked for where the other says it is, and its full-bag message.
constexpr float kShopScore = 0.85f;
constexpr int kShopSlack = 8;
/// A dialog whose words are this near the sell question's are the sell
/// question's: the buy question has a word of its own in the same sentence,
/// and the box and frame are the same, so only near-identical words are the
/// sell question's. Every other question is told at kWordsScore.
constexpr float kSellWordsScore = 0.97f;

} // namespace

Shop find_shop(const Frame& f, const TemplateStore& store) {
    Shop s;
    const Frame* row = store.get(kShopRow);
    const Frame* buttons = store.get(kShopButtons);
    if (!f.valid() || row == nullptr || buttons == nullptr) return s;
    const auto r = TemplateMatcher::find(f, ScreenRect{ 0, 0, f.width, f.height }, *row, kShopScore);
    if (!r.found) return s;
    // The buttons in the row's column, below it and no lower than under the
    // list's first row: another shop's, or a window of buttons like them, is
    // not this one. The row is not always the list's first -- the live shop has
    // shown two other items above it, the buttons 257 under it, not 299.
    const ScreenRect around = clamp_to(f, ScreenRect{ r.at.x + kShopButtonsDx - kShopSlack,
                                                      r.at.y + row->height,
                                                      buttons->width + 2 * kShopSlack,
                                                      kShopButtonsDy - row->height + buttons->height + kShopSlack });
    if (around.w < buttons->width || around.h < buttons->height) return s;
    const auto b = TemplateMatcher::find(f, around, *buttons, kShopScore);
    if (!b.found) return s;
    s.found = true;
    s.row = r.at;
    s.buttons = b.at;
    s.row_score = r.score;
    s.buttons_score = b.score;
    return s;
}

bool shop_full_up(const Frame& f, const TemplateStore& store, ScreenRect* at) {
    const Frame* full = store.get(kShopFull);
    if (!f.valid() || full == nullptr) return false;
    const auto m = TemplateMatcher::find(f, ScreenRect{ 0, 0, f.width, f.height }, *full, kShopScore);
    if (!m.found) return false;
    // Its words as well: the picture is nearly all frame, and another message
    // of one line -- not enough money, a limit -- passes for it as a whole.
    const ScreenRect in_picture{ 20, 22, 158, 16 };
    const Frame words = crop(*full, in_picture);
    if (!words.valid() || bright_words(words) < kWordsAtLeast) return false;
    const ScreenRect place = clamp_to(f, ScreenRect{ m.at.x + in_picture.x - 2, m.at.y + in_picture.y - 2,
                                                     words.width + 4, words.height + 4 });
    if (place.w < words.width || place.h < words.height) return false;
    if (!TemplateMatcher::find(f, place, words, kWordsScore).found) return false;
    if (at != nullptr) *at = m.at;
    return true;
}

bool shop_row_up(const Frame& f, const TemplateStore& store) {
    const Frame* row = store.get(kShopRow);
    return f.valid() && row != nullptr &&
           TemplateMatcher::find(f, ScreenRect{ 0, 0, f.width, f.height }, *row, kShopScore).found;
}

bool shop_left_up(const Frame& f, const TemplateStore& store) {
    if (!f.valid() || black(f)) return true;
    if (find_shop(f, store).found || shop_row_up(f, store) || shop_full_up(f, store)) return true;
    const ScreenRect all{ 0, 0, f.width, f.height };
    // Its buttons alone: an item's description can hide its row.
    if (const Frame* b = store.get(kShopButtons))
        if (TemplateMatcher::find(f, all, *b, kShopScore).found) return true;
    // A question find_dialogs misses with one of its buttons drawn pressed or
    // lit: either button alone, anywhere.
    if (const Frame* pair = store.get(kConfirm)) {
        const int half = pair->width / 2 - 1;
        const Frame halves[] = { crop(*pair, ScreenRect{ 0, 0, half, pair->height }),
                                 crop(*pair, ScreenRect{ pair->width - half, 0, half, pair->height }) };
        for (const Frame& h : halves)
            if (h.valid() && TemplateMatcher::find(f, all, h, kButtonScore).found) return true;
    }
    return false;
}

bool shop_question(const Frame& f, const TemplateStore& store, const Dialog& d, const CountReader& counts,
                   float frame_score, bool check_box) {
    // The bin's questions and the registration's are other questions, told by
    // their words.
    if (d.ours() || d.registers || !counts.loaded()) return false;
    const Frame* sell = store.get(kSellQuestion);
    const Frame* pair = store.get(kConfirm);
    if (sell == nullptr || pair == nullptr || !f.valid()) return false;
    int dx = 0, dy = 0;
    if (!pair_in(*sell, store, dx, dy)) return false;
    // The game's own box as the sell question draws it: from a little above
    // its buttons to its bottom edge, the whole width -- with its box see-
    // through, whose digits are what the question asks about and differ.
    const int top = dy - kFrameAbove;
    Frame strip = crop(*sell, ScreenRect{ 0, top, sell->width, sell->height - top });
    if (!strip.valid()) return false;
    const ScreenRect box_in_picture{ dx + kBoxDx, dy + kBoxDy - top, kBoxW, std::max(4, pair->height - 6) };
    for (int y = std::max(0, box_in_picture.y); y < std::min(strip.height, box_in_picture.bottom()); ++y)
        for (int x = std::max(0, box_in_picture.x); x < std::min(strip.width, box_in_picture.right()); ++x)
            strip.pixels[(static_cast<std::size_t>(y) * strip.width + x) * 4 + 3] = 0;
    const ScreenRect place = clamp_to(f, ScreenRect{ d.pair.x - dx - 2, d.pair.y - kFrameAbove - 2,
                                                     strip.width + 4, strip.height + 4 });
    if (place.w < strip.width || place.h < strip.height) return false;
    if (!TemplateMatcher::find(f, place, strip, frame_score).found) return false;
    // None of the other questions GoBot has a picture of -- the revive and the
    // drop questions -- and not the sell question itself, its words near
    // identical; the buy question's own differ in a word.
    for (const char* name : kOtherQuestions) {
        const float words = other_words(f, store, d, name);
        const bool sell_words = std::string(name) == kSellQuestion;
        if (words >= (sell_words ? kSellWordsScore : kWordsScore)) return false;
    }
    // A box with a number in it, or nothing, that reads -- at the first look;
    // looked at again with the number typed (check_box false), the box's own
    // digits can sit under the caret, and the frame and the words are what
    // say it is still the same question.
    if (!check_box) return true;
    const ScreenRect box{ d.pair.x + kBoxDx, d.pair.y + kBoxDy, kBoxW, std::max(4, d.pair.h - 6) };
    return counts.read_field(f, box) != CountReader::kUnreadable;
}

bool can_check_shop(const TemplateStore& store) {
    if (store.get(kConfirm) == nullptr) return false;
    for (const DialogPicture& pic : kBinDialogs)
        if (store.get(pic.name) == nullptr) return false;
    for (const char* name : kOtherQuestions)
        if (store.get(name) == nullptr) return false;
    for (const char* name : { kShopRow, kShopButtons, kShopFull, kSellQuestion })
        if (store.get(name) == nullptr) return false;
    int dx = 0, dy = 0;
    if (!pair_in(*store.get(kSellQuestion), store, dx, dy)) return false;
    return can_see_windows(store);
}

bool notice_up(const Frame& f, const TemplateStore& store) {
    // The wordless picture of a one-button message matches the bin's own
    // question at 0.84, so only with no dialog of two buttons up.
    const Frame* notice = store.get(kNotice);
    if (notice == nullptr || !f.valid() || !find_dialogs(f, store).empty()) return false;
    return TemplateMatcher::find(f, ScreenRect{ 0, 0, f.width, f.height }, *notice, 0.75f).found;
}

bool lone_ok(const Frame& f, const TemplateStore& store, const ScreenRect& band, ScreenRect* at, LoneOk* how) {
    LonePieces p;
    if (!f.valid() || !lone_pieces(store, p)) return false;
    const ScreenRect area = clamp_to(f, band);
    if (area.w < p.label.width || area.h < p.label.height) return false;
    // The label, best first -- a band the size of the frame may hold more than
    // one -- each with the frame where a lone button's would be round it. A
    // question's Hủy Bỏ has another label, and its Xác Nhận has Hủy Bỏ to its
    // right, not the box's frame; a lit button fails its label and corner.
    const auto labels = TemplateMatcher::find_all(f, area, p.label, how != nullptr ? -1.0f : kOkLabelScore, 4);
    bool found = false;
    for (std::size_t i = 0; i < labels.size() && !found; ++i) {
        const TemplateMatcher::Match& m = labels[i];
        LoneOk seen;
        seen.label = m.at;
        seen.label_score = m.score;
        seen.edge_score = piece_at(f, p.edge, m.at.x + p.edge_dx, m.at.y + p.edge_dy, 1);
        seen.corner_score = piece_at(f, p.corner, m.at.x + p.corner_dx, m.at.y + p.corner_dy, 1);
        if (how != nullptr && i == 0) *how = seen;
        if (m.score < kOkLabelScore || seen.edge_score < kLoneEdgeScore || seen.corner_score < kLoneCornerScore)
            continue;
        found = true;
        if (how != nullptr) *how = seen;
        if (at != nullptr) *at = m.at;
    }
    // And no dialog of two buttons anywhere: asked last, the dearest look.
    return found && find_dialogs(f, store).empty();
}

ScreenRect message_band(const ScreenRect& pair) {
    const int x = pair.x + kMessageDx + kOkLabel.x, y = pair.y + kOkLabel.y;
    return ScreenRect{ x - kMessageSlack, y - kMessageAbove, kOkLabel.w + 2 * kMessageSlack,
                       kOkLabel.h + kMessageAbove + kMessageBelow };
}

ScreenRect message_box(const ScreenRect& pair) {
    return ScreenRect{ pair.x + kQuestionBox.x, pair.y + kQuestionBox.y, kQuestionBox.w,
                       kQuestionBox.h + kMessageBelow };
}

bool message_up(const Frame& f, const TemplateStore& store) {
    if (!f.valid()) return false;
    return notice_up(f, store) || lone_ok(f, store, ScreenRect{ 0, 0, f.width, f.height });
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
    : input_(input), store_(store), raw_grab_(std::move(grab)) {
    if (raw_grab_) grab_ = [this](Frame& f) { return see(f); };
}

bool BagCleaner::see(Frame& f) {
    if (!raw_grab_(f) || !f.valid()) {
        f = Frame{};
        return false;
    }
    // A black stretch is waited out once, a stop's looks included -- Hủy Bỏ
    // and the shop's ESC after a stop are what most need a look, and they
    // bring the game in front, which is what blacks it out. A stretch that
    // outlasted the wait is not waited for again: the looks after it give up
    // at the first black frame, until one is not.
    for (int looks = 1; black(f); ++looks) {
        if (blacked_out_ || looks >= kBlackLooks) {
            if (!blacked_out_ && !black_said_) {
                black_said_ = true;
                keep(f, "black_screen");
                say("màn hình game đen suốt " + std::to_string(kBlackLooks * kBlackGapMs / 1000) +
                    " giây, nên không nhìn được gì trên đó");
            }
            blacked_out_ = true;
            f = Frame{};
            return false;
        }
        sleep(kBlackGapMs);
        if (!raw_grab_(f) || !f.valid()) {
            f = Frame{};
            return false;
        }
    }
    blacked_out_ = false;
    last_w_ = f.width;
    last_h_ = f.height;
    return true;
}

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

bool BagCleaner::click(int x, int y, bool need_front) {
    return hands_.click ? hands_.click(x, y) : click_game(x, y, false, need_front);
}

bool BagCleaner::drag(int fx, int fy, int tx, int ty) {
    if (hands_.drag) return hands_.drag(fx, fy, tx, ty);
    // As click_game: only onto the game, asked again as the button goes down,
    // the press carrying its point; and a stop before then moves nothing.
    InputManager::GameClick why = InputManager::GameClick::Failed;
    if (input_.drag_on_game(fx, fy, tx, ty, [this] { return cancelled_.load(); }, &why)) return true;
    say_not_made(why, fx, fy);
    return false;
}

bool BagCleaner::press(std::uint32_t vk) {
    return hands_.key ? hands_.key(vk) : input_.key_press(vk);
}

bool BagCleaner::press_scan(std::uint32_t vk) {
    if (hands_.scan_key) return hands_.scan_key(vk);
    if (hands_.key) return hands_.key(vk);
    return input_.type_key(vk, false, 45, InputManager::KeyInject::Scancode);
}

bool BagCleaner::right_click(int x, int y) {
    // Pretend hands with no right click of their own are a test that never
    // meant one: nothing reaches the game.
    if (hands_.click || hands_.drag || hands_.key) return hands_.right_click ? hands_.right_click(x, y) : false;
    return hands_.right_click ? hands_.right_click(x, y) : click_game(x, y, true, true);
}

bool BagCleaner::double_click(int x, int y) {
    if (hands_.double_click) return hands_.double_click(x, y);
    if (hands_.click) return hands_.click(x, y) && hands_.click(x, y);
    // Pretend hands with no click of their own never meant one: nothing
    // reaches the game.
    if (hands_.drag || hands_.key || hands_.right_click) return false;
    return click_game(x, y, false, true, 2);
}

bool BagCleaner::click_game(int x, int y, bool right_button, bool need_front, int presses) {
    // Only with the game on top at the point -- and in front, but for giving
    // an item back -- asked again as the button goes down: the frames come
    // from PrintWindow, which shows the game even when it is covered. A stop
    // meanwhile holds up any click but one that gives an item back.
    InputManager::GameClick why = InputManager::GameClick::Failed;
    const bool clicked =
        need_front ? input_.click_on_game(x, y, right_button, [this] { return cancelled_.load(); }, &why, true, presses)
                   : input_.click_on_game(x, y, right_button, {}, &why, false, presses);
    if (clicked) return true;
    say_not_made(why, x, y);
    return false;
}

void BagCleaner::say_not_made(InputManager::GameClick why, int x, int y) {
    switch (why) {
        case InputManager::GameClick::NotInFront:
            say("cửa sổ game không ở trên cùng, nên không dùng chuột");
            break;
        case InputManager::GameClick::Covered:
            say("có cửa sổ khác che game ở chỗ cần bấm, nên không dùng chuột");
            break;
        case InputManager::GameClick::NoWindow:     // nothing aimed at: nothing to say, as mouse_ok
        case InputManager::GameClick::Stopped:      // said where the stop is seen
            break;
        default:
            GB_INFO("Trash: no click at %d,%d -- %s", x, y, InputManager::game_click_name(why));
            break;
    }
}

bool BagCleaner::grab_view(Frame& f, BagView& v) {
    if (!grab_ || !grab_(f) || !f.valid()) return false;
    v = bag::find(f, store_, settings_.layout);
    if (v.found) last_strip_ = v.strip;
    return v.found;
}

void BagCleaner::park_pointer(bool title_first, const ScreenRect* avoid) {
    // Pretend hands move a pretend pointer, when they have one, in a game the
    // size of the last look; the real one is never moved by them.
    const bool pretend = hands_.click || hands_.double_click || hands_.drag || hands_.key;
    if (pretend && !hands_.move) return;
    if (!last_strip_.valid()) return;
    const BagLayout& l = settings_.layout;
    int w = 0, h = 0;
    if (pretend) {
        w = last_w_;
        h = last_h_;
    } else if (!input_.window_size(w, h)) {
        return;
    }
    if (w <= 0 || h <= 0) return;
    // A box whose buttons the pointer would light, and a margin round it.
    ScreenRect off{};
    if (avoid != nullptr && avoid->valid())
        off = ScreenRect{ avoid->x - 24, avoid->y - 24, avoid->w + 48, avoid->h + 48 };
    // A shop open beside the bag, from its buttons: not rested on either.
    ScreenRect shop{};
    if (shop_buttons_.valid())
        shop = ScreenRect{ shop_buttons_.x + bag::kShopWindowFromButtons.x - 6,
                           shop_buttons_.y + bag::kShopWindowFromButtons.y - 6, bag::kShopWindowFromButtons.w + 16,
                           bag::kShopWindowFromButtons.h + 20 };
    const ScreenRect& s = last_strip_;
    const POINT title{ s.x + 40, s.y + l.close_dy };
    const POINT right{ s.x + l.scroll_up_dx + 30, s.y + 100 };
    const POINT left{ s.x - 30, s.y + 100 };
    const POINT order[] = { title_first ? title : right, title_first ? right : title, left };
    for (const POINT& p : order)
        if (p.x >= 2 && p.y >= 2 && p.x < w - 2 && p.y < h - 2 && !shop.contains(p.x, p.y) &&
            !off.contains(p.x, p.y)) {
            if (pretend) hands_.move(p.x, p.y);
            else input_.move_pointer(p.x, p.y);
            return;
        }
}

bool BagCleaner::clean(const Frame& f, const BagView& v) {
    // A bin out of the frame is taken for empty: nothing can be seen or put
    // there, and with no dialog up nothing in it takes a key. A pass, which
    // would put items there, stops on it before this is asked (run).
    return v.found && (v.bin_empty || !v.bin_on_frame) && bag::find_dialogs(f, store_).empty();
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
    Frame f;
    BagView now;
    if (!grab_view(f, now)) return;
    // A tab of ten rows takes more than a few clicks, and the game does not
    // answer every one: see scroll_arrow.
    for (int i = 0; i < 20 && !cancelled_ && !now.at_top(); ++i)
        if (!scroll_arrow(false, now)) break;
    v = now;
}

bool BagCleaner::scroll_arrow(bool down, BagView& v) {
    const BagLayout& l = settings_.layout;
    Frame f0;
    BagView before;
    if (!look_page(f0, before)) return false;
    // The game takes a click on an arrow now and not then -- a second click on
    // the very same point soon after the first is taken for a double-click,
    // and the page takes its time to move -- so a click that moved nothing is
    // tried again a pixel aside and waited for longer, a few times, before the
    // page is called the last.
    static const int kWaits[] = { 250, 400, 600, 800 };
    for (int attempt = 0; attempt < 4 && !cancelled_; ++attempt) {
        const int x = before.strip.x + (down ? l.scroll_down_dx : l.scroll_up_dx) + (attempt % 2);
        const int y = before.strip.y + (down ? l.scroll_down_dy : l.scroll_up_dy);
        if (!mouse_ok(x, y)) return false;
        click(x, y);
        if (!wait_ms(kWaits[attempt])) return false;
        Frame g;
        BagView after;
        if (!look_page(g, after)) return false;
        const bool moved = before.scroll_known() && after.scroll_known()
                               ? (down ? after.first_row > before.first_row : after.first_row < before.first_row)
                               : !bag::same_page(f0, before, g, after, l);
        if (moved) {
            v = after;
            return true;
        }
    }
    return false;
}

BagCleaner::Result BagCleaner::run() {
    Result res = run_pass();
    // A charm run's own message not seen closed -- left by a stop, the mouse
    // not to be had, or come too late to watch for -- is the next run's to
    // close (set_own_notice); one two clicks did not close is the player's.
    if (mode_ == Mode::CharmsOnly && last_asked_.valid() && !last_closed_ && !last_gave_up_) {
        res.notice_left = true;
        res.notice_place = last_asked_;
        res.notice_carried = last_carried_;
    }
    return res;
}

BagCleaner::Result BagCleaner::run_pass() {
    // Its clicks and drags at the pace the settings ask for, on this thread only.
    const InputManager::Pace pace(settings_.press_ms);
    Result res;
    const BagLayout& l = settings_.layout;

    if (trash_ == nullptr || trash_->items.empty()) {
        res.trouble = mode_ == Mode::ShopBuy ? "thiếu danh sách món để đếm bình" : "danh sách đồ bỏ đi đang trống";
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
    // A charm run's own message from the last run is closed first, and the
    // bag key is not pressed under anyone else's.
    if (mode_ == Mode::CharmsOnly && !charms_start(f, v, res)) {
        res.stuck = item_out_;      // a dialog that came up as it closed, not answered
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
            keep(f, "bag_not_found");
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
        // Nor without a look: the bag left open does no harm.
        if (!grab_ || !grab_(g) || !g.valid()) return;
        if (!bag::find_dialogs(g, store_).empty() || question_still(g)) {
            say("để túi đồ mở, vì vẫn còn một hộp thoại đang mở");
            return;
        }
        // Nor, in a charm run, over a message of the game's: the key may go to
        // it, and the player has it to read or to close.
        if (mode_ == Mode::CharmsOnly && bag::message_up(g, store_)) {
            say("để túi đồ mở, vì vẫn còn một thông báo đang mở");
            return;
        }
        press(settings_.bag_key);
    };

    // A pass puts items in the bin: one with its bin out of the game's window
    // -- the bag dragged low, easy in a small window -- cannot, and says why.
    if (mode_ == Mode::Pass && !v.bin_on_frame) {
        keep(f, "bin_off_frame");
        res.trouble = "thùng rác của túi đồ nằm ngoài cửa sổ game; kéo túi đồ lên giữa màn hình game rồi thử lại";
        close_bag();
        return res;
    }

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

    // The potion shop: Tiêu Dùng counted, Khác for the Người Giữ Sân item,
    // the shop, and the bag shut again afterwards if it was opened here --
    // when it is still there: an ESC that closed the shop may have closed it.
    if (mode_ == Mode::ShopBuy) {
        bool stop = false;
        // A trial buys nothing, and is allowed with the switch off.
        if (!settings_.buy_potions && !dry_run_) {
            res.trouble = "Tự mua bình đang tắt (buy_potions=false trong configs\\trash.ini)";
        } else {
            shop_buy(v, res, stop);
        }
        res.stuck = item_out_;
        // The screen at the end: a quantity box, the shop or its message still
        // up -- a Hủy Bỏ that did not land, a shop that would not shut -- is
        // what the keys let go would be typed into: the hold stays, and the
        // player is told to close it.
        // No look at all is no better: what is up is not known.
        Frame g;
        BagView w;
        const bool seen = grab_ && grab_(g) && g.valid();
        if (!seen) {
            res.stuck = true;
            res.trouble += std::string(res.trouble.empty() ? "" : "; ") +
                           "không nhìn được màn hình game để biết cửa hàng đã đóng chưa: xem lại trong game";
        } else if (!dialogs_here(g, store_).empty() || question_still(g) ||
                   (shop_q_.valid() && !seen_at(shop_q_)) || shop_now(g).found || bag::shop_row_up(g, store_) ||
                   bag::shop_full_up(g, store_)) {
            res.stuck = true;
            keep(g, "shop_left_open");
            res.trouble += std::string(res.trouble.empty() ? "" : "; ") +
                           "hộp thoại hoặc cửa hàng vẫn đang mở: đóng nó trong game";
        }
        // Nor the bag shut over something left up: the key would go into it.
        if (seen && !res.stuck && grab_view(g, w)) close_bag();
        return res;
    }

    // Charms only: Tiêu Dùng and nothing else -- no other tab, no use sweep of
    // fragments, nothing deleted -- quick, and the bag shut again afterwards.
    if (mode_ == Mode::CharmsOnly) {
        bool stop = false;
        if (!settings_.use_charms) {
            res.trouble = "Dùng bùa đang tắt (use_charms=false trong configs\\trash.ini)";
        } else if (!bag::can_check_charm(store_)) {
            res.trouble = "GoBot chưa có hình của game để dùng bùa (nút Xác Nhận, hộp thoại thùng rác, hộp thoại "
                          "đăng ký mảnh ghép hay nút đóng cửa sổ)";
        } else if (dry_run_) {
            say("thử tìm: không bấm chuột phải vào bùa nào");
        } else {
            // Tiêu Dùng clicked when the strip does not show it chosen, and
            // looked at in short steps until it does, not waited for as long
            // as a pass waits: select_tab, as the shop's count has it.
            if (select_tab(kUseTab, v, res, "charm_tab") && !cancelled_) {
                scroll_to_top(v);
                charm_sweep(kUseTab, v, res, stop);
            }
            // The message of the last Xác Nhận, when it has not been seen
            // closed: watched for and closed, however the sweep ended -- but
            // by a stop, which ends everything.
            finish_charms(res);
        }
        // Nothing was put in the bin, so a stop has nothing to give back: a
        // question left open is answered Hủy Bỏ where it came up (refuse).
        res.stuck = item_out_;
        close_bag();
        return res;
    }

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
    bool colour_said = false;        // a charm of a colour not sure, said once a pass
    for (int tab = 0; tab < kBagTabs && !stop && !cancelled_; ++tab) {
        if (!settings_.tabs[tab]) continue;

        // The game on top where the tab is, whether or not it needs a click:
        // a pass that cannot click does not start judging. Then the tab
        // clicked and seen chosen, looked at every 80 ms rather than waited a
        // fixed 350 for and never checked.
        int tx = 0, ty = 0;
        v.tab_point(l, tab, tx, ty);
        if (!mouse_ok(tx, ty)) {
            res.trouble = "không bấm được vào túi đồ: cửa sổ game bị che hoặc không ở trên cùng";
            break;
        }
        if (!select_tab(tab, v, res, "pass_tab")) {
            if (cancelled_) break;
            // A strip whose chosen tab cannot be read at all -- no tab drawn
            // darker, as a client of another look may draw it -- is gone on
            // with once clicked, as the pass always did: it judges items by
            // their pictures, whichever tab. One seen showing another tab
            // stops it.
            if (v.found && v.selected_tab < 0) {
                res.trouble.clear();
                res.tab_unsure = false;
            } else {
                stop = true;
                break;
            }
        }
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
                park_pointer();
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
                    // A charm drawn like one on the Dùng list, of a colour
                    // not sure: no doubt between two drawings, and a pass
                    // never right-clicks a charm anyway. Said once a pass.
                    if (k.doubt == TrashList::Verdict::Doubt::Colour) {
                        seen.insert(key);
                        if (!colour_said) {
                            colour_said = true;
                            char colour[24];
                            std::snprintf(colour, sizeof colour, "%.2f", static_cast<double>(k.colour));
                            say(std::string("bùa ở ") + where + " cùng hình với loại đã chọn nhưng không chắc màu (" +
                                colour + "), để nguyên");
                        }
                        continue;
                    }
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
                else if (!from_here && !item.charm()) ++res.use_matched;     // a charm has a run of its own
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
                    const char* why_not = item.charm()
                                              ? "bùa được dùng riêng, mỗi giờ một cái, không dùng trong lúc dọn"
                                        : !item.fragment()
                                              ? "hình này không giống mảnh ghép hay bùa nào, mà chuột phải vào "
                                                "món khác là dùng luôn món đó"
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

                // Not with HP fallen under the potion's mark meanwhile, nor the
                // hold taken over: the potions wait on this pass. Asked between
                // items, with nothing in the bin and no dialog up.
                if (guard_ && !guard_()) {
                    res.trouble = "máu xuống thấp hoặc bot không còn giữ phím, nên dừng dọn rác giữa chừng";
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
                } else if (!cancelled_) {
                    // A stop is not a failure: nothing was tried that did not work.
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
            // is at the bottom or scrolling changes nothing -- a click the game
            // did not take tried again a pixel aside, the pointer moved off the
            // bar before it is read (scroll_arrow), so no row is left behind.
            if (v.at_bottom(l) || ++scrolls > 30) break;
            const int dx = v.strip.x + l.scroll_down_dx, dy = v.strip.y + l.scroll_down_dy;
            if (!mouse_ok(dx, dy)) {
                res.trouble = "không bấm được vào túi đồ: cửa sổ game bị che hoặc không ở trên cùng";
                stop = true;
                break;
            }
            hid_pointer = false;
            if (!scroll_arrow(true, v)) break;
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
            click(d.cancel_x(), d.y(), false);
            park_pointer();
            for (int i = 0; i < 12; ++i) {
                pause_ms(150);
                Frame g;
                if (!grab_ || !grab_(g) || !g.valid()) continue;
                bool still = false;
                for (const bag::Dialog& each : bag::find_dialogs(g, store_))
                    still = still || same_place(each.pair, d.pair);
                // Gone only where the game itself is on the screen there.
                if (!still && seen_at(d.pair)) break;
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
            click(v.trash.cx(), v.trash.cy(), false);
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

bag::Dialog BagCleaner::wait_for_dialog(int ms, bool& foreign, Frame* last,
                                        const std::function<bool(const Frame&)>& done) {
    foreign = false;
    int others = 0;
    bool gone_seen = false;
    std::chrono::steady_clock::time_point gone_at{};
    // One frame for every look, grabbed into again rather than 3 MB made anew
    // every 150 ms. The newest goes to `last` by a swap, which hands its old
    // buffer back for the next grab.
    Frame f;
    for (int waited = 0; waited < ms; waited += 150) {
        if (!wait_ms(150)) break;
        if (!grab_ || !grab_(f) || !f.valid()) continue;
        const auto dialogs = bag::find_dialogs(f, store_);
        for (const bag::Dialog& d : dialogs)
            if (d.ours()) return d;
        // A dialog fading in can fail the words for a frame; one that is
        // still not the bin's a frame later is not.
        if (!dialogs.empty()) ++others;
        // Nothing up and the item seen gone, twice, kDoneApartMs apart: the
        // bin asks again at once when it does at all, and while it has a
        // question to ask the slot still shows the item's enlarged copy.
        if (done && dialogs.empty() && done(f)) {
            if (!gone_seen) {
                gone_seen = true;
                gone_at = now();
            } else if (now() - gone_at >= std::chrono::milliseconds(kDoneApartMs)) {
                answered_early_ = true;
                if (last) std::swap(*last, f);
                break;
            }
        } else {
            gone_seen = false;
        }
        if (last) std::swap(*last, f);
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
                if (reads >= 3 && now() - start >= std::chrono::milliseconds(kBoxEnoughMs)) return value;
            }
        }
        if (long_enough) break;
    }
    return reads >= 2 ? value : CountReader::kUnreadable;
}

int BagCleaner::answer(const bag::Dialog& first, int count, const std::function<bool(const Frame&)>& done) {
    answered_early_ = false;
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
        // Off the button at once: lit with the pointer on it, the pair is not
        // the pair find_dialogs knows, and once the dialog is gone the pointer
        // would rest on the bag's grid, drawing an item's description there.
        park_pointer();

        // Gone from where it was -- or replaced there by the bin's other
        // dialog, which is the next question rather than this one stuck. Gone
        // only where the game itself is on the screen there: a copy of the
        // screen shows whatever lies over it.
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
            closed = !still && seen_at(d.pair);
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
        // other way round. Only its own dialog is answered. Over at once once
        // the item is seen gone (done).
        bool foreign = false;
        const bag::Dialog next = wait_for_dialog(900, foreign, nullptr, done);
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
    if (settings_.click_to_move) {
        // Once the first click has picked the item up, the second always
        // follows, as a giving-back click: a stop between the two left the
        // item on the pointer.
        moved = click(s.cx(), s.cy());
        if (moved) {
            // The pause between the two clicks at the settings' pace, never under 150.
            pause_ms(std::max(150, 220 * settings_.press_ms / CleanSettings::kPressDefaultMs));
            moved = click(v.trash.cx(), v.trash.cy(), false);
        }
    } else {
        moved = drag(s.cx(), s.cy(), v.trash.cx(), v.trash.cy());
    }
    if (!moved) {
        say(cancelled_ ? "dừng giữa chừng" : "không điều khiển được chuột trong cửa sổ game");
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
    //    the item goes back to the bag. The item seen gone -- the can back,
    //    the same page, its slot empty -- ends the wait for another question.
    const auto gone_from = [&](const Frame& g2) {
        const BagView w2 = bag::find(g2, store_, l);
        if (!w2.found || !w2.bin_empty) return false;
        if (v.scroll_known() && (!w2.scroll_known() || w2.first_row != v.first_row)) return false;
        const auto now2 = w2.slots(l);
        return static_cast<std::size_t>(index) < now2.size() &&
               bag::is_empty(g2, now2[static_cast<std::size_t>(index)], l);
    };
    if (answer(d, count, gone_from) != kDone) {
        back_out();
        return false;
    }

    // 4. Only gone counts: the can back in the bin, none of the bin's dialogs
    //    up, and the slot no longer holding what it held -- on a fresh look,
    //    at once when the answer already saw it gone.
    if (!answered_early_) wait_ms(300);
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
        out.why = cancelled_ ? "dừng giữa chừng" : "không điều khiển được chuột trong cửa sổ game";
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
    if (!click(d.confirm_x(), d.y())) {
        // Not pressed after all -- stopped, or the game no longer in front or
        // on top there as the button was to go down: Hủy Bỏ, as for any other
        // doubt at this step, rather than leave the question open.
        if (cancelled_) return refuse("dừng giữa chừng", k, "use_stopped");
        return refuse("không bấm được Xác Nhận", k, "use_blocked");
    }

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
                                         Frame& look, Frame& shot, std::string& why, int gap_ms,
                                         const Frame* charm) {
    const BagLayout& l = settings_.layout;
    SlotNow first = SlotNow::Unknown;
    for (int n = 0; n < 2; ++n) {
        if (n > 0) pause_ms(gap_ms);
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
            // A charm's read as the sweep read it: one only while the slot is
            // still that colour's icon with nothing over its corner -- a bag
            // that closed up its gaps would have moved another single in, and
            // that reads as no count at all.
            const int left = !counts_ ? CountReader::kUnreadable
                             : charm  ? counts_->read_charm(g, s, charm, l)
                                      : counts_->read(g, s);
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
    if (!click(x, y)) {
        why = cancelled_ ? "dừng giữa chừng" : "không bấm được nút X của Yêu Quái Bách Khoa";
        return false;
    }
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
            park_pointer();
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

// ------------------------------------------------------------------ charms

namespace {

/// A right-click on a charm is looked at this often until its question is up
/// (or the stack has gone down), against the fragment's 150 ms: a charm has
/// no monster book to wait for, and the game shows its question at once.
constexpr int kCharmPollMs = 60;
/// How long a question that has been answered is given to go, how long after
/// it has the slot is first looked at again, and the gap between that look and
/// the one that has to agree with it.
constexpr int kCharmCloseMs = 1500;
constexpr int kCharmSettleMs = 120;
constexpr int kCharmGapMs = 80;
/// After a right-click that brought nothing and changed nothing, before the
/// screen is looked at once more for a question come late.
constexpr int kCharmLateMs = 400;

/// After Xác Nhận, with the stack still as it was: the further looks at the
/// slot, each this long after the one before, before it is called unchanged.
constexpr int kCharmLagLooks = 4;
constexpr int kCharmLagMs = 250;

/// The slots of Tiêu Dùng the packets' bag lists (0x0068, gobot/self_frames.hpp):
/// twelve rows of six.
constexpr int kPacketSlots = 72;

/// After Xác Nhận on a charm's question, once the question is gone: how long
/// the game's message is watched for -- it came about 0.36 s after the click on
/// the live client -- and how much longer once it is first seen, for the look
/// that has to agree.
constexpr int kCharmNoticeMs = 1000;
constexpr int kCharmNoticeMoreMs = 300;
/// How long a click on the message's Xác Nhận is given to close it, and how
/// long after it a second click may come: one sooner on the same point would
/// be a double-click to the game.
constexpr int kNoticeCloseMs = 1500;
constexpr int kNoticeRetryMs = 300;
/// How long after its Xác Nhận a message where that question puts one is taken
/// for GoBot's own, come late: closed at the next round, before the next
/// right-click, or at the end of the run.
constexpr int kOwnNoticeMs = 3000;

/// Milliseconds from `from` to `to`.
int ms_between(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
}

} // namespace

void BagCleaner::forget_said_once() {
    std::lock_guard<std::mutex> lock(g_said_mutex);
    g_said.clear();
}

void BagCleaner::set_deadline_ms(int ms) {
    deadline_set_ = ms > 0;
    if (deadline_set_) deadline_ = now() + std::chrono::milliseconds(ms);
}

bool BagCleaner::charm_must_stop(Result& res, bool& stop) {
    if (guard_ && !guard_()) {
        res.trouble = std::string("bot không còn giữ phím (có việc khác đã lấy lại, hoặc máu xuống thấp), nên dừng ") + (mode_ == Mode::ShopBuy ? "mua bình" : "dùng bùa");
        stop = true;
        return true;
    }
    if (deadline_set_ && now() >= deadline_) {
        res.trouble = mode_ == Mode::ShopBuy ? "mua bình lâu quá thời gian cho phép, nên dừng" : "dùng bùa lâu quá thời gian cho phép, nên dừng";
        stop = true;
        return true;
    }
    return false;
}

void BagCleaner::set_own_notice(const ScreenRect& pair) {
    last_asked_ = pair;
    last_asked_at_ = now();
    last_carried_ = pair.valid();
    last_closed_ = false;
    last_gave_up_ = false;
}

int BagCleaner::charm_deadline_ms(int due_kinds) {
    return std::clamp(12'000 + 3'000 * std::max(0, due_kinds), 20'000, 40'000);
}

bool BagCleaner::charm_due_any() const {
    if (trash_ == nullptr) return false;
    for (const TrashList::Item& it : trash_->items)
        if (it.action == TrashList::Action::Use && it.charm() && (!charm_due_ || charm_due_(it.clock_id())))
            return true;
    return false;
}

bool BagCleaner::own_message(const Frame& f, ScreenRect* at) {
    if (!last_asked_.valid() || last_closed_ || last_gave_up_) return false;
    // Come within a few seconds of the Xác Nhận it follows, or handed over
    // from the last run: a message there later than that is anyone's.
    if (!last_carried_ && ms_between(last_asked_at_, now()) >= kOwnNoticeMs) return false;
    return bag::lone_ok(f, store_, bag::message_band(last_asked_), at);
}

BagCleaner::NoticeClose BagCleaner::close_notice(const ScreenRect& band, const ScreenRect& box, Frame& shot,
                                                 int& clicks) {
    clicks = 0;
    const auto stopped = [this] { return cancelled_ || (guard_ && !guard_()); };
    // What a look shows of the message. Lingers: its frame is still right of
    // where the last click went, its label not to be told -- lit by the
    // pointer, or hidden -- so it is neither gone nor to be clicked.
    enum class Seen { Blind, Dialog, Up, Lingers, Gone };
    ScreenRect clicked{};
    const auto look = [&](ScreenRect& at) {
        Frame g;
        if (!grab_ || !grab_(g) || !g.valid()) return Seen::Blind;
        shot = g;
        if (!bag::find_dialogs(g, store_).empty()) return Seen::Dialog;
        if (bag::lone_ok(g, store_, band, &at)) return Seen::Up;
        if (clicked.valid() && bag::message_edge_at(g, store_, clicked)) return Seen::Lingers;
        return Seen::Gone;
    };
    // Gone on two looks kDoneApartMs apart, within `ms`: Closed; a dialog:
    // Dialog; else NotClosed -- Stopped as soon as the guard says no.
    const auto watch_gone = [&](int ms) {
        bool once = false;
        std::chrono::steady_clock::time_point since{};
        for (int waited = 0; waited < ms; waited += kCharmPollMs) {
            if (stopped()) return NoticeClose::Stopped;
            pause_ms(kCharmPollMs);
            ScreenRect at;
            const Seen s = look(at);
            if (s == Seen::Dialog) return NoticeClose::Dialog;
            if (s == Seen::Blind) continue;
            if (s != Seen::Gone) {
                once = false;
            } else if (!once) {
                once = true;
                since = now();
            } else if (ms_between(since, now()) >= kDoneApartMs) {
                return NoticeClose::Closed;
            }
        }
        return NoticeClose::NotClosed;
    };

    // 1. A fresh look: the message there to be clicked, or gone already --
    //    on a second look a moment later as well.
    ScreenRect at;
    Seen s = look(at);
    if (s == Seen::Dialog) return NoticeClose::Dialog;
    if (s != Seen::Up) {
        pause_ms(kDoneApartMs);
        s = look(at);
        if (s == Seen::Dialog) return NoticeClose::Dialog;
        if (s == Seen::Blind) return NoticeClose::NotClicked;
        if (s != Seen::Up) return NoticeClose::Closed;
    }
    std::chrono::steady_clock::time_point clicked_at{};
    for (int attempt = 0; attempt < 2; ++attempt) {
        // 2. Its Xác Nhận, in the middle of its label -- a pixel aside the
        //    second time -- and the pointer moved off the box at once.
        if (stopped()) return NoticeClose::Stopped;
        const int x = at.x + at.w / 2 + attempt, y = at.y + at.h / 2;
        if (!mouse_ok(x, y)) return NoticeClose::NotClicked;
        if (!click(x, y)) return cancelled_ ? NoticeClose::Stopped : NoticeClose::NotClicked;
        ++clicks;
        clicked = at;
        clicked_at = now();
        park_pointer(true, &box);
        // 3. Seen gone.
        const NoticeClose c = watch_gone(kNoticeCloseMs);
        if (c != NoticeClose::NotClosed || attempt == 1) return c;
        // 4. Still up: once more, no sooner than kNoticeRetryMs after the
        //    click, on a fresh look -- the pointer moved off, its label is not
        //    lit -- and only with its label there to be clicked.
        const int since = ms_between(clicked_at, now());
        if (since < kNoticeRetryMs) pause_ms(kNoticeRetryMs - since);
        if (stopped()) return NoticeClose::Stopped;
        s = look(at);
        if (s == Seen::Dialog) return NoticeClose::Dialog;
        if (s == Seen::Gone) {
            pause_ms(kDoneApartMs);
            ScreenRect again;
            s = look(again);
            if (s == Seen::Dialog) return NoticeClose::Dialog;
            if (s == Seen::Gone) return NoticeClose::Closed;
        }
        if (s != Seen::Up) break;
    }
    // No key is ever pressed for it.
    return NoticeClose::NotClosed;
}

void BagCleaner::notice_trouble(NoticeClose how, const Frame& shot, Result& res) {
    std::string why;
    switch (how) {
        case NoticeClose::Closed:
            return;
        case NoticeClose::NotClosed:
            // The player's from now on: never clicked again, nor handed over.
            last_gave_up_ = true;
            keep(shot, "charm_notice_stuck");
            why = "thông báo của game không đóng lại khi bấm Xác Nhận: đóng nó trong game";
            break;
        case NoticeClose::Dialog:
            keep(shot, "charm_notice_dialog");
            item_out_ = true;
            if (cancel_all(5)) {
                item_out_ = false;
                why = "lúc đóng thông báo của game thì có một hộp thoại khác hiện ra; đã bấm Hủy Bỏ";
            } else {
                why = "lúc đóng thông báo của game thì có một hộp thoại khác hiện ra, và bấm Hủy Bỏ không đóng "
                      "được: đóng nó trong game";
            }
            break;
        case NoticeClose::NotClicked:
            why = "không bấm được Xác Nhận trên thông báo của game";
            break;
        case NoticeClose::Stopped:
            if (guard_ && !guard_()) {
                bool stop = false;
                charm_must_stop(res, stop);
            }
            return;
    }
    if (res.trouble.empty()) res.trouble = why;
    say(why);
}

int BagCleaner::close_own(const Frame& f, Result& res, const char* when) {
    if (!own_message(f)) {
        // One handed over from the last run that is not up: gone.
        if (last_carried_ && last_asked_.valid() && !last_gave_up_) last_closed_ = true;
        return 0;
    }
    const int ago = ms_between(last_asked_at_, now());
    Frame shot;
    int clicks = 0;
    const NoticeClose how = close_notice(bag::message_band(last_asked_), bag::message_box(last_asked_), shot, clicks);
    if (how == NoticeClose::Closed) {
        last_closed_ = true;
        if (clicks > 0) ++res.charm_notices;
        if (last_carried_)
            GB_INFO("Trash: charm message from the use at %d,%d (last run), closed %s -- %d click(s)", last_asked_.x,
                    last_asked_.y, when, clicks);
        else
            GB_INFO("Trash: charm message from the use at %d,%d (%d ms ago), closed %s -- %d click(s)", last_asked_.x,
                    last_asked_.y, ago, when, clicks);
        return 1;
    }
    GB_INFO("Trash: charm message from the use at %d,%d %s: not closed (%d click(s))", last_asked_.x, last_asked_.y,
            when, clicks);
    notice_trouble(how, shot, res);
    if (res.trouble.empty()) res.trouble = "dừng giữa chừng";
    return -1;
}

void BagCleaner::finish_charms(Result& res) {
    if (!last_asked_.valid() || last_closed_ || last_gave_up_) return;
    const auto stopped = [this] { return cancelled_ || (guard_ && !guard_()); };
    if (stopped()) return;
    const ScreenRect band = bag::message_band(last_asked_);
    // Watched for until the time a message is given after its Xác Nhận -- the
    // most of which has mostly passed by now.
    while (!last_carried_ && ms_between(last_asked_at_, now()) < kCharmNoticeMs + kCharmNoticeMoreMs) {
        if (stopped()) return;
        pause_ms(kCharmPollMs);
        Frame g;
        if (grab_ && grab_(g) && g.valid() && bag::lone_ok(g, store_, band)) break;
    }
    if (stopped()) return;
    Frame shot;
    int clicks = 0;
    const NoticeClose how = close_notice(band, bag::message_box(last_asked_), shot, clicks);
    if (how == NoticeClose::Closed) {
        last_closed_ = true;
        if (clicks > 0) {
            ++res.charm_notices;
            GB_INFO("Trash: charm message from the use at %d,%d (%d ms ago), closed at the end of the run -- %d "
                    "click(s)",
                    last_asked_.x, last_asked_.y, ms_between(last_asked_at_, now()), clicks);
        }
        return;
    }
    GB_INFO("Trash: charm message from the use at %d,%d at the end of the run: not closed (%d click(s))",
            last_asked_.x, last_asked_.y, clicks);
    notice_trouble(how, shot, res);
}

bool BagCleaner::charms_start(Frame& f, BagView& v, Result& res) {
    // GoBot's own, handed over from the last run, closed -- not on a look-only
    // run, which presses nothing.
    if (!dry_run_) {
        const int own = close_own(f, res, v.found ? "at the start" : "before the bag key");
        if (own < 0) return false;
        if (own > 0) {
            grab_view(f, v);
            if (!f.valid()) {
                res.trouble = "không chụp được màn hình game";
                return false;
            }
        }
    }
    // The bag open: gone through, to use what is due or to say what waits --
    // but not for a message handed over from the last run with nothing due,
    // whose run is over once that message is closed: neither the bag's tab
    // nor its scroll bar is touched for it.
    if (v.found) return charm_due_any() || !last_carried_;
    // Anyone else's: left, and the bag key not pressed under it -- with the
    // bag open, the sweep's first round says the same, as it always has.
    if (bag::message_up(f, store_)) {
        say("đang có một thông báo trong game, nên không dùng bùa nào; đóng nó rồi thử lại");
        return false;
    }
    // A run for a message to close, and nothing due: the bag left shut.
    return charm_due_any();
}

BagCleaner::UseResult BagCleaner::charm_one(const Frame& f, const BagView& v, int index, int count,
                                            const Frame& icon) {
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
    const auto lost = [&](const std::string& why, const Frame& shot, const char* what) {
        keep(shot, what);
        out.outcome = UseOutcome::Lost;
        out.why = why;
        return out;
    };
    // Hủy Bỏ on every dialog with two buttons up -- for a stop, and for
    // anything that is not the question of a charm as GoBot takes it for one.
    const auto refuse = [&](const std::string& why, const Frame& shot, const char* what) {
        keep(shot, what);
        item_out_ = true;
        if (!cancel_all(5)) return lost("hộp thoại không đóng lại khi bấm Hủy Bỏ", shot, "charm_stuck");
        item_out_ = false;
        out.outcome = UseOutcome::Refused;
        out.why = why;
        return out;
    };
    const auto doubt = [&](UseOutcome how, const std::string& why, const Frame& shot, const char* what) {
        keep(shot, what);
        out.outcome = how;
        out.why = why;
        return out;
    };

    // 1. One right-click on the middle of the slot, and the pointer moved off
    //    the middle of the game's window, where its question comes up: off the
    //    slot, whose description would lie over the grid, and off the buttons
    //    of what comes, which it would light.
    item_out_ = true;
    if (!right_click(s.cx(), s.cy())) {
        item_out_ = false;
        out.why = cancelled_ ? "dừng giữa chừng" : "không điều khiển được chuột trong cửa sổ game";
        return out;
    }
    {
        const ScreenRect centre{ f.width / 2 - 110, f.height / 2 - 60, 220, 120 };
        park_pointer(true, &centre);
    }

    // 2. What it brings up, in short steps: the question of a charm, as the
    //    game's own dialog drawn like the fragment's and none of the other
    //    questions GoBot knows (bag::charm_question). A dialog fading in can
    //    fail its box for a frame; one that is still not a charm's a frame
    //    later is not. Watched to the end even after a stop, so that a
    //    question coming up late is not left open. A message with only Xác
    //    Nhận instead -- no question asked -- is never clicked: it is watched
    //    on to the end, for a dialog after it, and the stack says the rest.
    bag::Dialog d;
    bool asked = false, noticed = false;
    int others = 0, notices = 0;
    Frame g;
    // Said of a dialog after such a message: answered Hủy Bỏ, and not used --
    // no Xác Nhận was pressed.
    const auto after_notice = [&](UseResult r) {
        if (r.outcome == UseOutcome::Refused) {
            r.outcome = UseOutcome::Notice;
            r.why = "sau khi bấm chuột phải game hiện một thông báo rồi một hộp thoại; đã bấm Hủy Bỏ, không dùng bùa";
        }
        return r;
    };
    for (int waited = 0; waited < settings_.charm_wait_ms && !asked; waited += kCharmPollMs) {
        pause_ms(kCharmPollMs);
        Frame h;
        if (!grab_ || !grab_(h) || !h.valid()) continue;
        g = std::move(h);
        const auto dialogs = bag::find_dialogs(g, store_);
        if (!noticed && dialogs.size() == 1 &&
            bag::charm_question(g, store_, dialogs.front(), settings_.charm_frame_score)) {
            d = dialogs.front();
            asked = true;
            break;
        }
        if (!dialogs.empty()) {
            if (++others >= 2 || cancelled_) {
                if (noticed) return after_notice(refuse("", g, "charm_notice_dialog"));
                bool bins = false;
                for (const bag::Dialog& each : dialogs) bins = bins || each.ours();
                return refuse(bins ? "hiện ra hộp thoại của thùng rác chứ không phải câu hỏi dùng bùa"
                                   : "hộp thoại hiện ra không phải câu hỏi dùng bùa (không giống hộp thoại của "
                                     "game, hoặc là một câu hỏi GoBot đã biết là câu khác)",
                              g, "charm_wrong_dialog");
            }
            continue;
        }
        if (!noticed && bag::notice_up(g, store_) && ++notices >= 2) {
            noticed = true;
            keep(g, "charm_notice");
        }
    }

    if (!asked) {
        // No question: what the right-click did, if anything, is in the slot.
        // As it was, nothing came of it. Gone or less with no question asked
        // is no use GoBot can vouch for -- a shop or a storage window open
        // beside the bag would take the item just so -- and nothing more is
        // right-clicked after it.
        late_ask_ = true;
        Frame h;
        BagView w;
        if (!grab_view(h, w)) return lost("mất dấu túi đồ", h.valid() ? h : g, "charm_lost");
        if (!bag::find_dialogs(h, store_).empty()) {
            if (noticed) return after_notice(refuse("", h, "charm_notice_dialog"));
            return refuse("một hộp thoại hiện ra muộn", h, "charm_late_dialog");
        }
        item_out_ = false;
        Frame look, shot = h;
        std::string why;
        switch (slot_now(v, index, before, count, look, shot, why, kCharmGapMs, &icon)) {
            case SlotNow::Same:
                if (noticed) {
                    out.outcome = UseOutcome::Notice;
                    out.why = "sau khi bấm chuột phải game hiện một thông báo; để nguyên cho người chơi đọc";
                    return out;
                }
                out.outcome = UseOutcome::Nothing;
                out.why = "bấm chuột phải mà không thấy câu hỏi nào, và số lượng không đổi";
                return out;
            case SlotNow::Asking:
                if (noticed) return after_notice(refuse("", shot, "charm_notice_dialog"));
                return refuse("một hộp thoại hiện ra muộn", shot, "charm_late_dialog");
            case SlotNow::Gone:
            case SlotNow::Less:
                return doubt(UseOutcome::UsedUnasked,
                             "bấm chuột phải thì không có câu hỏi nào mà bùa đã bớt đi -- có thể đã dùng, cũng có "
                             "thể một cửa hàng hay kho đồ đang mở đã lấy nó -- nên không dùng thêm bùa nào",
                             shot, "charm_unasked");
            case SlotNow::Unknown:
                break;
        }
        return doubt(UseOutcome::Unsure,
                     "bấm chuột phải thì không có câu hỏi nào, và không xem lại được ô đồ (" + why +
                         "), nên không dùng thêm bùa nào",
                     shot, "charm_unsure");
    }

    // 3. The question, on a fresh look the moment before Xác Nhận: still
    //    there, still a charm's, nothing else up, and no window of the
    //    game's but the bag -- or Hủy Bỏ.
    if (cancelled_) return refuse("dừng giữa chừng", g, "charm_stopped");
    Frame k;
    if (!grab_ || !grab_(k) || !k.valid()) return refuse("không chụp được màn hình game", g, "charm_blind");
    const auto again = bag::find_dialogs(k, store_);
    if (again.empty()) {
        item_out_ = false;
        out.outcome = UseOutcome::Refused;
        out.why = "câu hỏi dùng bùa đóng lại trước khi bấm";
        return out;
    }
    if (again.size() != 1 || !bag::charm_question(k, store_, again.front(), settings_.charm_frame_score) ||
        !same_place(again.front().pair, d.pair))
        return refuse("lúc sắp bấm Xác Nhận thì trên màn hình không chỉ có câu hỏi dùng bùa", k, "charm_changed");
    {
        const BagView kv = bag::find(k, store_, l);
        ScreenRect other;
        if (bag::other_window_up(k, store_, kv, l, &other)) {
            GB_INFO("Trash: a close button that is not the bag's own at %d,%d -- Hủy Bỏ", other.x, other.y);
            return refuse("lúc sắp bấm Xác Nhận thì có một cửa sổ khác đang mở, nên không bấm", k,
                          "charm_other_window");
        }
    }
    // That look was slow (the windows are searched for in the whole frame):
    // the question asked once more, with nothing in between but the click.
    if (cancelled_) return refuse("dừng giữa chừng", k, "charm_stopped");
    {
        Frame last;
        if (!grab_ || !grab_(last) || !last.valid()) return refuse("không chụp được màn hình game", k, "charm_blind");
        const auto now_up = bag::find_dialogs(last, store_);
        if (now_up.size() != 1 || !bag::charm_question(last, store_, now_up.front(), settings_.charm_frame_score) ||
            !same_place(now_up.front().pair, d.pair))
            return refuse("lúc sắp bấm Xác Nhận thì trên màn hình không chỉ có câu hỏi dùng bùa", last,
                          "charm_changed");
        k = std::move(last);
    }
    if (!mouse_ok(d.confirm_x(), d.y())) return refuse("không bấm được Xác Nhận", k, "charm_blocked");
    if (!click(d.confirm_x(), d.y())) {
        if (cancelled_) return refuse("dừng giữa chừng", k, "charm_stopped");
        return refuse("không bấm được Xác Nhận", k, "charm_blocked");
    }
    // From here on the charm is at least perhaps used. The pointer off the
    // box, so that nothing of it, or of the message after it, is lit.
    out.asked = d.pair;
    out.asked_at = now();
    const ScreenRect band = bag::message_band(d.pair);
    const ScreenRect box = bag::message_box(d.pair);
    park_pointer(true, &box);
    // What the game asked, kept the first time in a GoBot start: a picture of
    // the question of a charm, to tell it apart more closely from if need be.
    if (said_once("kept|charm_question")) keep(k, "charm_question");
    // The guard and a stop, asked on every look of every wait from here: with
    // either, nothing more is waited for or clicked.
    const auto stopping = [this] { return cancelled_ || (guard_ && !guard_()); };

    // 4. The question gone from where it was.
    bool closed = false;
    Frame after = k;
    // Watched for even after a stop, as the question was: whether it closed
    // is what says if anything is left in the way of the macro's keys. Then
    // the stop waits for nothing more.
    for (int waited = 0; waited < kCharmCloseMs && !closed; waited += kCharmPollMs) {
        if (!out.stopped && stopping()) out.stopped = true;
        pause_ms(kCharmPollMs);
        Frame h;
        if (!grab_ || !grab_(h) || !h.valid()) continue;
        after = std::move(h);
        bool there = false;
        for (const bag::Dialog& each : bag::find_dialogs(after, store_))
            there = there || same_place(each.pair, d.pair);
        closed = !there;
    }
    if (!closed)
        return lost(out.stopped ? "dừng giữa chừng, câu hỏi dùng bùa vẫn đang mở sau khi bấm Xác Nhận"
                                : "câu hỏi dùng bùa không đóng lại sau khi bấm Xác Nhận",
                    after, "charm_stuck");
    // Nothing of the player's waits in a dialog any more: the question is gone.
    if (bag::find_dialogs(after, store_).empty()) item_out_ = false;
    // Hủy Bỏ on a dialog that comes up after Xác Nhận: no telling then whether
    // the charm was used. Held to be in the way of the macro's keys until Hủy
    // Bỏ is seen to close it, whatever an earlier look found.
    const auto next_dialog = [&](const Frame& shot) {
        item_out_ = true;
        if (!cancel_all(5)) return lost("hộp thoại không đóng lại khi bấm Hủy Bỏ", shot, "charm_stuck");
        item_out_ = false;
        return doubt(UseOutcome::Unsure,
                     "sau khi bấm Xác Nhận game hỏi thêm một câu khác, nên bấm Hủy Bỏ; không biết bùa đã dùng "
                     "hay chưa",
                     shot, "charm_next_dialog");
    };

    // 4b. The game's message that the charm worked: a lone Xác Nhận where the
    //     question's Hủy Bỏ was (bag::lone_ok), on two looks at least
    //     kDoneApartMs apart in one place -- watched for up to kCharmNoticeMs,
    //     and a little longer once it is first seen.
    bool told = false;
    Frame told_frame;
    {
        bool seen = false;
        ScreenRect seen_at;
        std::chrono::steady_clock::time_point seen_since{};
        int limit = kCharmNoticeMs;
        const auto from = now();
        while (ms_between(from, now()) < limit) {
            if (stopping()) {
                out.stopped = true;
                break;
            }
            pause_ms(kCharmPollMs);
            Frame h;
            if (!grab_ || !grab_(h) || !h.valid()) continue;
            if (!bag::find_dialogs(h, store_).empty()) return next_dialog(h);
            item_out_ = false;
            ScreenRect at;
            if (!bag::lone_ok(h, store_, band, &at)) {
                seen = false;
                continue;
            }
            if (!seen || !same_place(at, seen_at)) {
                seen = true;
                seen_at = at;
                seen_since = now();
                if (out.notice_ms < 0) out.notice_ms = ms_between(out.asked_at, seen_since);
                limit = std::max(limit, std::min(kCharmNoticeMs + kCharmNoticeMoreMs,
                                                 ms_between(from, now()) + kDoneApartMs + 2 * kCharmPollMs));
                continue;
            }
            if (ms_between(seen_since, now()) >= kDoneApartMs) {
                told = true;
                told_frame = std::move(h);
                break;
            }
        }
    }

    // 4c. Closed with its own Xác Nhận -- the only Xác Nhận clicked after a
    //     charm's right-click but the question's -- before the stack is looked
    //     at: the box may lie over the bag.
    if (told && !out.stopped) {
        // The first of a GoBot start kept, before it is closed: another kind's
        // message, perhaps taller, is what the next fix would go by.
        if (said_once("kept|charm_notice_ok")) keep(told_frame, "charm_notice_ok");
        const auto click_from = now();
        Frame shot;
        int clicks = 0;
        const NoticeClose how = close_notice(band, box, shot, clicks);
        out.notice_clicks = clicks;
        const int took = ms_between(click_from, now());
        switch (how) {
            case NoticeClose::Closed:
                out.notice_closed = true;
                GB_INFO("Trash: charm message after Xác Nhận at %d,%d, %d ms after the click; closed in %d ms (%d "
                        "click(s))",
                        d.pair.x, d.pair.y, out.notice_ms, took, clicks);
                break;
            case NoticeClose::Dialog:
                GB_INFO("Trash: charm message after Xác Nhận at %d,%d: a dialog came up while it was closed",
                        d.pair.x, d.pair.y);
                return next_dialog(shot);
            case NoticeClose::NotClosed:
                keep(shot, "charm_notice_stuck");
                out.notice_left = true;
                out.notice_stuck = true;
                break;
            case NoticeClose::NotClicked:
                out.notice_left = true;
                break;
            case NoticeClose::Stopped:
                out.notice_left = true;
                out.stopped = true;
                break;
        }
        if (!out.notice_closed)
            GB_INFO("Trash: charm message after Xác Nhận at %d,%d, %d ms after the click; left open (%s, %d click(s))",
                    d.pair.x, d.pair.y, out.notice_ms,
                    how == NoticeClose::NotClosed    ? "would not close"
                    : how == NoticeClose::NotClicked ? "not clicked"
                                                     : "stopped",
                    clicks);
    } else if (told) {
        out.notice_left = true;
    }

    if (!out.stopped) pause_ms(kCharmSettleMs);
    Frame h;
    BagView w;
    const bool bag_seen = grab_view(h, w);
    if (h.valid() && !bag::find_dialogs(h, store_).empty()) return next_dialog(h);
    if (!bag_seen)
        return lost("sau khi dùng bùa, túi đồ bị che (có thể một cửa sổ khác đã mở ra): đóng nó trong game",
                    h.valid() ? h : after, "charm_covered");
    item_out_ = false;
    if (!told && !out.stopped && bag::message_up(h, store_) && !bag::lone_ok(h, store_, band)) {
        // A message, but not where the question puts the charm's own: not
        // GoBot's to close. Xác Nhận is pressed, so this is a charm perhaps
        // used, not one that was not.
        return doubt(UseOutcome::Unsure,
                     "sau khi bấm Xác Nhận game hiện một thông báo (để nguyên cho người chơi đọc); không biết bùa "
                     "đã dùng hay chưa",
                     h, "charm_notice_after");
    }

    // 5. Only gone, or one less in the stack, in that very slot, the same on
    //    two looks, counts as used. A stack that has not changed is watched a
    //    little longer first: the game may update it after the server's
    //    answer, and a charm used and not seen to be is a charm used twice --
    //    but not after a stop, which waits for nothing.
    Frame look, shot = h;
    std::string why;
    SlotNow state = slot_now(v, index, before, count, look, shot, why, kCharmGapMs, &icon);
    for (int i = 0; i < kCharmLagLooks && state == SlotNow::Same && !cancelled_ && !out.stopped; ++i) {
        pause_ms(kCharmLagMs);
        state = slot_now(v, index, before, count, look, shot, why, kCharmGapMs, &icon);
    }
    // A message closed, and the stack not one less: whatever the game said
    // there is kept, for it was perhaps not that the charm worked.
    const auto other_notice = [&] {
        if (told) keep(told_frame, "charm_notice_other");
    };
    const std::string closed_said = out.notice_closed ? " (đã đóng thông báo của game, đã lưu ảnh)" : "";
    switch (state) {
        case SlotNow::Gone:
            out.outcome = UseOutcome::Used;
            return out;
        case SlotNow::Less:
            out.outcome = UseOutcome::Used;
            out.left = count - 1;
            out.after = std::move(look);
            return out;
        case SlotNow::Same:
            other_notice();
            return doubt(UseOutcome::NotTaken, "đã bấm Xác Nhận mà số lượng bùa không giảm" + closed_said, shot,
                         "charm_not_taken");
        case SlotNow::Asking:
            return next_dialog(shot);
        case SlotNow::Unknown:
            break;
    }
    other_notice();
    return doubt(UseOutcome::Unsure,
                 "đã bấm Xác Nhận mà không thấy ô đồ trống ra hay bớt đúng một (" + why +
                     "), nên không biết bùa đã dùng hay chưa" + closed_said,
                 shot, "charm_unsure");
}

void BagCleaner::charm_sweep(int tab, BagView& v, Result& res, bool& stop) {
    const BagLayout& l = settings_.layout;
    if (counts_ == nullptr || !counts_->loaded()) {
        say("không có bộ chữ số của game để đọc số lượng trên ô đồ, nên không dùng bùa nào");
        return;
    }
    if (!bag::can_see_windows(store_)) {
        say("không có hình nút đóng cửa sổ của game để biết có cửa sổ khác đang mở cạnh túi đồ không, nên không "
            "dùng bùa nào");
        return;
    }
    bool clicked = false;
    const auto answer_late = [&](const Frame& g) {
        keep(g, "charm_late_dialog");
        item_out_ = true;
        if (cancel_all(5)) {
            item_out_ = false;
            res.trouble = "một hộp thoại hiện ra muộn sau khi bấm chuột phải vào bùa; đã bấm Hủy Bỏ, nên không "
                          "dùng thêm bùa nào";
        } else {
            res.trouble = "một hộp thoại hiện ra muộn sau khi bấm chuột phải vào bùa, và bấm Hủy Bỏ không đóng "
                          "được: đóng nó trong game";
        }
        stop = true;
    };
    // A charm cut from the bag whose colour could not be told when the list
    // was built (TrashList::add_rivals): never used, and said once a GoBot
    // start, with what to choose instead.
    for (const TrashList::Item& it : trash_->items)
        if (it.id.empty() && it.action == TrashList::Action::Use && it.charm() && it.colour_of.empty() &&
            said_once("cut colour said|" + it.clock_id()))
            say("bùa cắt từ túi " + it.clock_id() + ": không chắc màu, không dùng; chọn bùa trong danh sách Bùa thay "
                "cho hình cắt");
    std::set<long long> seen;
    // By kind: each clock id and each colour once a run -- a picture cut from
    // the bag and the catalog's icon of its colour are one charm.
    std::set<std::string> tried;
    // And so from run to run: a kind is due only when every charm on the
    // Dùng list of its colour is. A cut and the catalog's icon of its colour
    // each have a clock, and the one not found in a run that used the other
    // is looked for again in ten minutes -- its colour then used early. This
    // can only leave a charm unclicked, never click one.
    const auto due_by_colour = [&](const TrashList::Item& it) {
        if (!charm_due_) return true;
        if (!charm_due_(it.clock_id())) return false;
        if (it.colour_of.empty()) return true;
        for (const TrashList::Item& other : trash_->items)
            if (other.action == TrashList::Action::Use && other.charm() && other.colour_of == it.colour_of &&
                !charm_due_(other.clock_id()))
                return false;
        return true;
    };
    // The kinds due as the run begins, by clock id with their colour: once
    // each has been tried the run is done, and the pages below are not looked
    // at. None due -- a run to say what waits -- goes through the bag as ever.
    std::vector<std::pair<std::string, std::string>> due;
    for (const TrashList::Item& it : trash_->items) {
        if (it.action != TrashList::Action::Use || !it.charm()) continue;
        const std::string cid = it.clock_id();
        if (!due_by_colour(it)) continue;
        if (std::none_of(due.begin(), due.end(), [&](const auto& d) { return d.first == cid; }))
            due.emplace_back(cid, it.colour_of);
    }
    res.charms_due = static_cast<int>(due.size());
    const auto all_tried = [&] {
        if (due.empty()) return false;
        for (const auto& d : due)
            if (!tried.count(d.first) && (d.second.empty() || !tried.count(d.second))) return false;
        return true;
    };
    // A run for a message handed over from the last, with nothing due: that
    // message closed, and nothing else.
    const bool closing_only = due.empty() && last_carried_ && last_asked_.valid();
    // A slot drawn like a charm on the Dùng list and left alone for its
    // colour (TrashList::judge): counted, and logged once a run; a colour not
    // sure is said as well, once a GoBot start for that slot and kind.
    const auto left_for_colour = [&](const TrashList::Verdict& k, int row, int col, long long key) {
        const bool unsure = k.doubt == TrashList::Verdict::Doubt::Colour;
        const bool other = k.doubt == TrashList::Verdict::Doubt::None && !k.colour_id.empty();
        if ((!unsure && !other) || k.like < 0) return;
        const TrashList::Item& like = trash_->items[static_cast<std::size_t>(k.like)];
        if (like.action != TrashList::Action::Use || !like.charm()) return;
        seen.insert(key);
        const std::string drawn = like.clock_id();
        if (other) {
            ++res.charms_other_colour;
            GB_INFO("Trash: charm slot row %d cell %d left alone: drawn like %s, coloured %s (%.3f, next %.3f): not "
                    "ticked",
                    row + 1, col + 1, drawn.c_str(), k.colour_id.c_str(), static_cast<double>(k.colour),
                    static_cast<double>(k.colour_next));
            return;
        }
        ++res.charms_colour_doubt;
        GB_INFO("Trash: charm slot row %d cell %d left alone: drawn like %s, colour not sure: best %s %.3f, next %s "
                "%.3f, mean off %d",
                row + 1, col + 1, drawn.c_str(), k.colour_id.empty() ? "none" : k.colour_id.c_str(),
                static_cast<double>(k.colour), k.colour_next_id.empty() ? "none" : k.colour_next_id.c_str(),
                static_cast<double>(k.colour_next), k.colour_mean_off);
        if (!said_once("colour|" + std::to_string(row) + "|" + std::to_string(col) + "|" + drawn)) return;
        char where[96];
        std::snprintf(where, sizeof where, "%s, hàng %d ô %d", bag_tab_name(tab), row + 1, col + 1);
        char colour[24];
        std::snprintf(colour, sizeof colour, "%.2f", static_cast<double>(k.colour));
        say(std::string("bùa ở ") + where + " cùng hình với loại đã chọn nhưng màu không giống (" + colour +
            "), không dùng");
    };
    int uses = 0, scrolls = 0;
    bool hid_pointer = false;
    // Looks at the strip that read no tab chosen and were looked at again --
    // kTabRelooks a run at most -- those since the last that read Tiêu Dùng,
    // and the first of those, for the log.
    int unread = 0, streak = 0;
    BagView first_unread;
    for (int round = 0; round < 200 && !stop && !cancelled_; ++round) {
        if (charm_must_stop(res, stop)) return;
        Frame f;
        if (!grab_view(f, v)) {
            res.trouble = "mất dấu túi đồ";
            stop = true;
            return;
        }
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
        // A message of the game's: GoBot's own -- the last Xác Nhận's, come
        // late, or handed over from the last run -- closed, and the round
        // begun again on a fresh look; anyone else's left, and nothing pressed.
        {
            const int own = close_own(f, res, "at the next round");
            if (own < 0) {
                stop = true;
                return;
            }
            if (own > 0) continue;
        }
        if (bag::message_up(f, store_)) {
            say("đang có một thông báo trong game, nên không dùng bùa nào; đóng nó rồi thử lại");
            return;
        }
        if (closing_only) return;
        // Only on Tiêu Dùng, by the strip: on another tab a right-click equips.
        // A look that reads no tab chosen at all -- something drawn over the
        // strip for a moment -- is looked at again, the pointer moved off, a
        // few times a run; another tab read for sure stops it at once. Either
        // way the run stops saying so, not as a bag with no charm in it.
        if (v.selected_tab != tab) {
            if (v.found && v.selected_tab < 0 && unread < kTabRelooks) {
                if (streak++ == 0) first_unread = v;
                ++unread;
                park_pointer();
                if (!wait_ms(kTabRelookMs)) return;
                continue;
            }
            tab_unsure(f, v, tab, "charm_sweep");
            res.tab_unsure = true;
            say(std::string("không chắc túi đồ đang ở ngăn ") + bag_tab_name(tab) + ", nên không dùng bùa nào");
            return;
        }
        if (streak > 0) {
            tab_read_late(first_unread, tab, streak, "charm_sweep");
            streak = 0;
        }
        if (v.slot_rows < l.rows && !v.at_bottom(l) && !hid_pointer) {
            hid_pointer = true;
            park_pointer();
            if (!wait_ms(300)) return;
            continue;
        }
        if (!v.scroll_known()) {
            say("không đọc được thanh cuộn của túi đồ, nên không dùng bùa nào");
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
            if (k.item < 0) {
                left_for_colour(k, row, c, key);
                continue;
            }
            const TrashList::Item& item = trash_->items[static_cast<std::size_t>(k.item)];
            if (item.action != TrashList::Action::Use || !item.charm()) continue;
            seen.insert(key);
            ++res.charms_seen;

            char where[96];
            std::snprintf(where, sizeof where, "%s, hàng %d ô %d", bag_tab_name(tab), row + 1, c + 1);
            const std::string cid = item.clock_id();
            if (!due_by_colour(item)) {
                ++res.charms_waiting;
                say(std::string("bùa ở ") + where + " chưa tới giờ dùng lại");
                continue;
            }
            if (tried.count(cid) || (!item.colour_of.empty() && tried.count(item.colour_of))) continue;
            if (uses >= kMaxCharms) {
                say("đã dùng " + std::to_string(uses) + " bùa, đủ cho một lần");
                return;
            }
            // The count, read by the catalog icon of the slot's colour -- the
            // colour judge told, its own id for a catalog charm: a stack of
            // one has no number printed, and is told by its corner being that
            // icon's with nothing over it. A picture cut from the bag is never
            // gone by: its corner may have its count in it.
            const Frame* icon = bare_of(*trash_, item.colour_of);
            bool bare = false;
            const int count = counts_->read_charm(f, slots[s], icon, l, &bare);
            if (count == CountReader::kUnreadable || count <= 0) {
                ++res.charms_unread;
                char detail[64];
                if (icon == nullptr) {
                    std::snprintf(detail, sizeof detail, "no icon of its colour");
                } else {
                    int unlike = -1;
                    float fit = 0.0f;
                    bag::bare_corner(f, slots[s], *icon, l, &unlike, &fit);
                    if (unlike < 0)
                        std::snprintf(detail, sizeof detail, "icon does not fit (fit %.1f)", static_cast<double>(fit));
                    else
                        std::snprintf(detail, sizeof detail, "fit %.1f, %d pixel(s) unlike it",
                                      static_cast<double>(fit), unlike);
                }
                GB_INFO("Trash: charm %s (%s) at %s row %d cell %d: no count read, and the corner is not its bare "
                        "icon (%s) -- not right-clicked",
                        cid.c_str(), item.colour_of.c_str(), bag_tab_name(tab), row + 1, c + 1, detail);
                say(std::string("không dùng bùa ở ") + where +
                    ": không đọc được số lượng trên ô đồ, nên sau khi dùng không xem lại được");
                continue;
            }
            // The packets' bag, read only: a slot it says holds something
            // else -- with the count the screen reads there -- is not
            // right-clicked; nor a stack of one, as the screen reads it, that
            // it says holds another count, or nothing. It never decides that
            // one is. Its slot j is Tiêu Dùng's row * 6 + cell from the top:
            // on the live client (2026-11-03) slots 0 to 11 held the screen's
            // counts cell for cell, and slot 1 was 8871081, 63 and then 62 --
            // and at 19:54 slot 11 8871071 x1, a stack of one with no number.
            // Asked only where the scroll bar says which row this is.
            std::string packets = "not asked";
            if (charm_packets_ && v.scroll_known() && l.cols == 6) {
                const int j = row * l.cols + c;
                if (j >= 0 && j < kPacketSlots) {
                    std::string said;
                    const PacketSays says = charm_packets_(item.colour_of, j, count, said);
                    const bool one_differs = count == 1 && says == PacketSays::CountDiffers;
                    if (says == PacketSays::Other || one_differs) {
                        ++res.charms_vetoed;
                        GB_INFO("Trash: charm %s (%s) at %s row %d cell %d left alone, count %d%s -- packets: slot %d "
                                "holds %s: not right-clicked",
                                cid.c_str(), item.colour_of.c_str(), bag_tab_name(tab), row + 1, c + 1, count,
                                bare ? " (nothing printed over its icon)" : "", j, said.c_str());
                        if (said_once("packets|" + std::to_string(j) + "|" + item.colour_of + "|" + said)) {
                            if (one_differs)
                                say(std::string("không dùng bùa ở ") + where + ": theo gói tin ô này " +
                                    (said == "empty" ? std::string("trống") : "có " + said) +
                                    ", mà màn hình thấy 1 cái");
                            else
                                say(std::string("không dùng bùa ở ") + where + ": theo gói tin ô này là món khác (" +
                                    said + "), không phải loại đã chọn");
                        }
                        continue;
                    }
                    // Another count than one printed: most likely not the slot
                    // the screen shows -- a page scrolled, a bag sorted -- and
                    // it says nothing either way.
                    packets = says == PacketSays::Agrees ? "slot " + std::to_string(j) + " holds " + said + ", agrees"
                              : says == PacketSays::CountDiffers
                                  ? "slot " + std::to_string(j) + " holds " + said + ", the screen reads " +
                                        std::to_string(count) + ": not the same slot, not compared"
                                  : said;
                }
            }
            ScreenRect other;
            if (bag::other_window_up(f, store_, v, l, &other)) {
                say("đang có cửa sổ khác mở cạnh túi đồ (kho, cửa hàng…), nên không dùng bùa nào; đóng nó rồi "
                    "thử lại");
                GB_INFO("Trash: a close button that is not the bag's own at %d,%d -- no charm right-clicked",
                        other.x, other.y);
                keep(f, "charm_other_window");
                return;
            }
            // Asked again the moment before the click: the hold may have gone
            // while this looked.
            if (charm_must_stop(res, stop)) return;
            // And a message of the game's, on a fresh look: one may have come
            // while the windows were looked for -- the last charm's, come late.
            // GoBot's own is closed and the round begun again, nothing claimed
            // or right-clicked; anyone else's stops the run.
            {
                Frame g;
                if (!grab_ || !grab_(g) || !g.valid()) {
                    res.trouble = "không chụp được màn hình game";
                    stop = true;
                    return;
                }
                const int own = close_own(g, res, "before the next right-click");
                if (own < 0) {
                    stop = true;
                    return;
                }
                if (own > 0) {
                    seen.erase(key);
                    --res.charms_seen;
                    acted = true;
                    break;
                }
                if (bag::message_up(g, store_)) {
                    say("đang có một thông báo trong game, nên không dùng bùa nào; đóng nó rồi thử lại");
                    return;
                }
            }

            GB_INFO("Trash: charm %s (%s) at %s row %d cell %d: drawing %.3f, colour %.3f, mean off %d (next %s %.3f), "
                    "count %d%s -- packets: %s",
                    cid.c_str(), item.colour_of.c_str(), bag_tab_name(tab), row + 1, c + 1,
                    static_cast<double>(k.score), static_cast<double>(k.colour), k.colour_mean_off,
                    k.colour_next_id.empty() ? "none" : k.colour_next_id.c_str(), static_cast<double>(k.colour_next),
                    count, bare ? " (nothing printed over its icon)" : "", packets.c_str());
            clicked = true;
            tried.insert(cid);
            if (!item.colour_of.empty()) tried.insert(item.colour_of);
            // Written down as used before anything that cannot be taken back.
            if (charm_claim_) charm_claim_(cid);
            // Looked at again after the use the way its count was read here.
            const Frame none;
            const UseResult u = charm_one(f, v, static_cast<int>(s), count, icon != nullptr ? *icon : none);
            acted = true;
            hid_pointer = false;
            // The last Xác Nhận pressed on a charm question, for a message of
            // its that comes late; none, when this one pressed none.
            last_asked_ = u.asked;
            last_asked_at_ = u.asked_at;
            last_carried_ = false;
            last_closed_ = u.notice_closed;
            last_gave_up_ = u.notice_stuck;
            if (u.notice_closed && u.notice_clicks > 0) ++res.charm_notices;
            const auto note = [&](CharmClock::How how, const std::string& why) {
                res.charm_tries.push_back(Result::CharmTry{ cid, how, why });
            };
            // The game's message after it, as it was left.
            const std::string notice_said =
                u.notice_closed ? " (đã đóng thông báo của game)"
                : u.notice_stuck
                    ? "; thông báo của game vẫn còn mở, không đóng lại khi bấm Xác Nhận: đóng nó trong game"
                : u.notice_left ? "; thông báo của game vẫn còn mở"
                                : "";
            switch (u.outcome) {
                case UseOutcome::Used:
                    ++uses;
                    ++res.charms_used;
                    note(CharmClock::How::Used, "");
                    say(std::string("đã dùng bùa ở ") + where + (u.left > 0 ? "; còn " + std::to_string(u.left) : "") +
                        notice_said);
                    // Used, with its message left up: nothing more is
                    // right-clicked under it.
                    if (u.notice_left) {
                        res.trouble = "đã dùng bùa ở " + std::string(where) + notice_said;
                        stop = true;
                    }
                    break;
                case UseOutcome::NotTaken:
                    // Xác Nhận was pressed: the stack not going down in the
                    // time watched is no proof the charm was not taken.
                    note(CharmClock::How::MaybeUsed, u.why);
                    ++res.failed;
                    say(std::string("dừng ở bùa ") + where + ": " + u.why);
                    res.trouble = u.why;
                    stop = true;
                    break;
                case UseOutcome::Unsure:
                case UseOutcome::UsedUnasked:
                    // Right-clicked, and no telling whether it took: counted as
                    // used for the clock, for a charm used twice is spent early.
                    note(CharmClock::How::MaybeUsed, u.why);
                    ++res.use_unsure;
                    say(std::string("dừng ở bùa ") + where + ": " + u.why);
                    res.trouble = u.why;
                    stop = true;
                    break;
                case UseOutcome::Nothing: {
                    note(CharmClock::How::NotUsed, u.why);
                    say(std::string("không dùng được bùa ở ") + where + ": " + u.why);
                    pause_ms(kCharmLateMs);
                    Frame g;
                    if (grab_ && grab_(g) && g.valid() && !bag::find_dialogs(g, store_).empty()) answer_late(g);
                    break;
                }
                case UseOutcome::Refused:
                    note(CharmClock::How::NotUsed, u.why);
                    ++res.failed;
                    say(std::string("không dùng bùa ở ") + where + ": " + u.why);
                    if (res.failed >= 3) {
                        res.trouble = "dùng bùa không được 3 lần, dừng lại";
                        stop = true;
                    }
                    break;
                case UseOutcome::Untouched:
                    tried.erase(cid);
                    tried.erase(item.colour_of);
                    say(std::string("không dùng bùa ở ") + where + ": " + u.why);
                    res.trouble = u.why;
                    stop = true;
                    break;
                case UseOutcome::Notice:
                    note(CharmClock::How::NotUsed, u.why);
                    say(std::string("dừng ở bùa ") + where + ": " + u.why);
                    res.trouble = u.why;
                    stop = true;
                    break;
                case UseOutcome::Lost:
                    note(CharmClock::How::MaybeUsed, u.why);
                    say(std::string("dừng ở bùa ") + where + ": " + u.why);
                    res.trouble = u.why;
                    stop = true;
                    break;
            }
            // The guard said no, or a stop came, while it waited: nothing more.
            if (u.stopped && !stop) {
                if (!charm_must_stop(res, stop)) stop = true;
            }
            // Every kind due tried: done, the pages below not looked at.
            if (!stop && all_tried()) return;
            break;
        }
        if (acted || stop) continue;

        // Nothing more on this page: the next rows.
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
        click(d.cancel_x(), d.y(), false);
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
    // As in trash_one: once the item is picked up, the second click follows.
    const auto click_move = [&] {
        if (!click(s.cx(), s.cy())) return false;
        pause_ms(220);
        return click(v.trash.cx(), v.trash.cy(), false);
    };
    const bool moved = settings_.click_to_move ? click_move()
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

// ------------------------------------------------------------------- shop

namespace {

/// A look every this often while something is waited for, and how long each
/// wait lasts: the shop after the right-click, the quantity question after Mua,
/// the question gone after Xác Nhận, the shop gone after ESC.
constexpr int kShopLookMs = 150;
constexpr int kShopOpenMs = 3500;
constexpr int kShopAskMs = 3500;
constexpr int kShopGoneMs = 2500;
constexpr int kShopSettleMs = 250;
/// How the quantity box is read: a look every kShopBoxLookMs until
/// kShopBoxAgree looks in a row read the same number, for kShopBoxWatchMs at
/// most. On the live client the box shows no caret, and a number read the same
/// three times running was always what it held; a key landing late shows as a
/// change, and is waited out. The gap after each key typed into it is the
/// settings' press_ms.
constexpr int kShopBoxWatchMs = 1200;
constexpr int kShopBoxLookMs = 100;
constexpr int kShopBoxAgree = 3;
constexpr int kShopFixTries = 3;
/// How alike the shop's buttons have to be, looked for alone where they were.
constexpr float kShopButtonsScore = 0.85f;

}  // namespace

std::vector<bag::Dialog> BagCleaner::dialogs_here(const Frame& f, const TemplateStore& store) const {
    // The shop's own Mua | Thoát are two buttons side by side, and a pair of
    // buttons is what find_dialogs looks for: not a dialog, and Hủy Bỏ is never
    // pressed on them -- the right one is Thoát.
    std::vector<bag::Dialog> out;
    for (const bag::Dialog& d : bag::find_dialogs(f, store)) {
        const ScreenRect& s = shop_buttons_;
        const bool over = s.valid() && d.pair.x < s.right() + 4 && s.x < d.pair.right() + 4 &&
                          d.pair.y < s.bottom() + 4 && s.y < d.pair.bottom() + 4;
        if (!over) out.push_back(d);
    }
    return out;
}

bool BagCleaner::question_still(const Frame& f) const {
    const Frame* pair = store_.get(kConfirm);
    if (!shop_q_.valid() || pair == nullptr || !f.valid()) return false;
    // Either button alone, as either drawing of the pair has it, where the
    // question had its pair: one is drawn pressed after keys the box did not
    // take, or lit with the pointer over it after a click, never both at once.
    const int half_w = pair->width / 2 - 1;
    std::vector<std::pair<Frame, int>> halves;      // a button, and how far into the pair it is
    std::vector<Frame> drawings{ *pair };
    if (const Frame* ask = store_.get(kBinDialogs[bag::kAskDialog].name))
        drawings.push_back(crop(*ask, ScreenRect{ kBinDialogs[bag::kAskDialog].pair_dx,
                                                  kBinDialogs[bag::kAskDialog].pair_dy, pair->width, pair->height }));
    for (const Frame& d : drawings) {
        if (!d.valid()) continue;
        halves.emplace_back(crop(d, ScreenRect{ 0, 0, half_w, d.height }), 0);
        halves.emplace_back(crop(d, ScreenRect{ d.width - half_w, 0, half_w, d.height }), d.width - half_w);
    }
    for (const auto& [half, dx] : halves) {
        if (!half.valid()) continue;
        const ScreenRect around =
            clamp_to(f, ScreenRect{ shop_q_.x + dx - 3, shop_q_.y - 3, half.width + 6, half.height + 6 });
        if (around.w < half.width || around.h < half.height) continue;
        if (TemplateMatcher::find(f, around, half, kButtonScore).found) return true;
    }
    return false;
}

bool BagCleaner::shop_cancel(int tries) {
    // Hủy Bỏ on a dialog that is not the shop's own buttons, as cancel_all --
    // the shop's question first, by where it is, which find_dialogs may not see
    // with its Hủy Bỏ drawn pressed.
    bring_forward();
    for (int attempt = 0; attempt < tries; ++attempt) {
        Frame f;
        if (!grab_ || !grab_(f) || !f.valid()) {
            pause_ms(150);
            continue;
        }
        const auto dialogs = dialogs_here(f, store_);
        bag::Dialog d;
        if (shop_q_.valid() && !question_still(f)) {
            // Taken for gone only where the game itself is on the screen, and
            // on a second look: a screen copy holds whatever lies over it.
            if (!seen_at(shop_q_)) return false;
            Frame again;
            pause_ms(150);
            if (grab_ && grab_(again) && again.valid() && !question_still(again)) shop_q_ = ScreenRect{};
            else continue;
        }
        if (shop_q_.valid()) {
            d.pair = shop_q_;
        } else {
            if (dialogs.empty()) return true;
            d = dialogs.front();
        }
        if (!mouse_ok(d.cancel_x(), d.y(), false)) {
            keep(f, "cancel_blocked");
            return false;
        }
        say("bấm Hủy Bỏ trên hộp thoại đang mở");
        click(d.cancel_x(), d.y(), false);
        for (int i = 0; i < 12; ++i) {
            pause_ms(150);
            Frame g;
            if (!grab_ || !grab_(g) || !g.valid()) continue;
            bool still = same_place(d.pair, shop_q_) && question_still(g);
            for (const bag::Dialog& each : dialogs_here(g, store_)) still = still || same_place(each.pair, d.pair);
            if (!still) break;
        }
    }
    Frame f;
    if (!grab_ || !grab_(f) || !f.valid() || question_still(f)) return false;
    if (shop_q_.valid() && !seen_at(shop_q_)) return false;
    shop_q_ = ScreenRect{};
    return dialogs_here(f, store_).empty();
}

bool BagCleaner::seen_at(const ScreenRect& r) {
    if (skip_focus_check_) return true;
    // The game itself on top at the middle and two corners of `r`: what a copy
    // of the screen shows there is the game's.
    HWND game = static_cast<HWND>(input_.target_window());
    if (game == nullptr) return false;
    for (const POINT at : { POINT{ r.cx(), r.cy() }, POINT{ r.x + 2, r.y + 2 }, POINT{ r.right() - 2, r.bottom() - 2 } }) {
        POINT p = at;
        if (!ClientToScreen(game, &p)) return false;
        HWND under = WindowFromPoint(p);
        if (under == nullptr || (under != game && GetAncestor(under, GA_ROOT) != game)) return false;
    }
    return true;
}

bool BagCleaner::look_page(Frame& f, BagView& v) {
    // The page on show with its scroll bar read: the pointer resting over the
    // bar's track or its arrows -- where the last click left it -- spoils the
    // thumb, and then which row is on top is not known. Moved off the bag and
    // looked at again, a few times.
    // A frame without the bag in it is looked at again a few times first (a
    // black one already was: see see()).
    for (int i = 0; i < 4 && !grab_view(f, v); ++i) {
        if (cancelled_ || !wait_ms(200)) return false;
        if (i == 3) return false;
    }
    for (int i = 0; i < 4 && !v.scroll_known() && !cancelled_; ++i) {
        park_pointer();
        if (!wait_ms(150)) break;
        if (!grab_view(f, v)) return false;
    }
    return true;
}

bool BagCleaner::select_tab(int tab, BagView& v, Result& res, const char* where) {
    const BagLayout& l = settings_.layout;
    if (v.selected_tab == tab) return true;
    Frame f;
    // Clicked, the pointer moved off the strip -- left on the tab it clicked,
    // the game lights it, and a tab lit is no tab read -- and looked at in
    // short steps until the tab is the chosen one. A strip that then reads no
    // tab chosen at all is looked at again a few times, a moment apart; one
    // that reads another tab chosen, for sure, is clicked once more, a pixel
    // aside -- the game may have let the first click go -- and no more.
    for (int clicks = 0; clicks < 2 && v.selected_tab != tab; ++clicks) {
        if (clicks > 0 && (!v.found || v.selected_tab < 0)) break;
        int tx = 0, ty = 0;
        v.tab_point(l, tab, tx, ty);
        tx += clicks;
        if (!mouse_ok(tx, ty)) {
            res.trouble = "không bấm được vào túi đồ: cửa sổ game bị che hoặc không ở trên cùng";
            return false;
        }
        click(tx, ty);
        park_pointer();
        for (int i = 0; i < 10 && v.selected_tab != tab; ++i) {
            if (!wait_ms(80)) return false;
            grab_view(f, v);
        }
        const BagView first = v;
        int more = 0;
        for (; more < kTabRelooks && v.found && v.selected_tab < 0; ++more) {
            if (!wait_ms(kTabRelookMs)) return false;
            grab_view(f, v);
        }
        if (more > 0 && v.selected_tab == tab) tab_read_late(first, tab, more, where);
    }
    if (v.selected_tab != tab) {
        if (v.found) {
            tab_unsure(f, v, tab, where);
            res.tab_unsure = true;
        }
        res.trouble = std::string("không chuyển được sang ngăn ") + bag_tab_name(tab) + " của túi đồ";
        return false;
    }
    return true;
}

void BagCleaner::tab_unsure(const Frame& f, const BagView& v, int tab, const char* where) {
    GB_INFO("Trash: tab not read as %d (%s) at %s -- read %d, strip at %d,%d (%.2f), tab means %d %d %d %d %d, "
            "under the labels %d %d %d %d %d",
            tab, bag_tab_name(tab), where, v.selected_tab, v.strip.x, v.strip.y, static_cast<double>(v.score),
            v.tab_mean[0], v.tab_mean[1], v.tab_mean[2], v.tab_mean[3], v.tab_mean[4], v.tab_band[0], v.tab_band[1],
            v.tab_band[2], v.tab_band[3], v.tab_band[4]);
    if (said_once(std::string("tab_unsure|") + where)) keep(f, (std::string("tab_unsure_") + where).c_str());
}

void BagCleaner::tab_read_late(const BagView& first, int tab, int looks, const char* where) {
    GB_INFO("Trash: tab %d (%s) read at %s after %d more looks -- the first read %d, strip at %d,%d (%.2f), tab "
            "means %d %d %d %d %d",
            tab, bag_tab_name(tab), where, looks, first.selected_tab, first.strip.x, first.strip.y,
            static_cast<double>(first.score), first.tab_mean[0], first.tab_mean[1], first.tab_mean[2],
            first.tab_mean[3], first.tab_mean[4]);
}

bool BagCleaner::shop_count(int tab, BagView& v, Result& res, bool& stop, int& hp, int& mp) {
    const BagLayout& l = settings_.layout;
    hp = mp = 0;
    if (!select_tab(tab, v, res, "shop_tab")) {
        stop = true;
        return false;
    }
    scroll_to_top(v);
    {
        // From the top, or the rows above the page on show are never counted --
        // and the potions in them bought again.
        Frame f0;
        BagView t0;
        if (!look_page(f0, t0) || !t0.at_top()) {
            keep(f0, t0.found && !t0.scroll_known() ? "shop_scroll_unknown" : "shop_not_at_top");
            if (t0.found && !t0.scroll_known()) {
                res.trouble = "không đọc được thanh cuộn của túi đồ, nên không đếm được số ô (ảnh giữ ở trash_debug)";
                stop = true;
                return false;
            }
            res.trouble = "không cuộn lên đầu ngăn " + std::string(bag_tab_name(tab)) + " được, nên không đếm được số ô";
            stop = true;
            return false;
        }
    }
    std::set<long long> seen;
    int scrolls = 0, doubtful = 0;
    // Looks that read no tab chosen and were looked at again -- kTabRelooks a
    // count at most -- those since the last that read the tab, and the first
    // of those, for the log.
    int unread = 0, streak = 0;
    BagView first_unread;
    bool complete = false;
    for (int round = 0; round < 80 && !cancelled_; ++round) {
        if (charm_must_stop(res, stop)) return false;
        Frame f;
        if (!look_page(f, v)) {
            keep(f, "shop_bag_lost");
            res.trouble = "mất dấu túi đồ";
            stop = true;
            return false;
        }
        if (!clean(f, v)) {
            keep(f, "shop_not_clean");
            res.trouble = "thùng rác đang có đồ chờ xoá hoặc có hộp thoại đang mở; xong việc đó trong game rồi "
                          "thử lại";
            stop = true;
            return false;
        }
        // Counted only on a look that reads the tab: on another, Tiêu Dùng's
        // potions would be counted none, and bought. A look that reads no tab
        // chosen at all -- something drawn over the strip for a moment -- is
        // looked at again, the pointer moved off, and the round done again from
        // there: nothing has scrolled, and no slot is counted twice. Another
        // tab read for sure stops it at once.
        if (v.selected_tab != tab) {
            if (v.found && v.selected_tab < 0 && unread < kTabRelooks) {
                if (streak++ == 0) first_unread = v;
                ++unread;
                park_pointer();
                if (!wait_ms(kTabRelookMs)) break;
                continue;
            }
            tab_unsure(f, v, tab, "shop_count");
            res.tab_unsure = true;
            res.trouble = std::string("không chắc túi đồ đang ở ngăn ") + bag_tab_name(tab);
            stop = true;
            return false;
        }
        if (streak > 0) {
            tab_read_late(first_unread, tab, streak, "shop_count");
            streak = 0;
        }
        if (!v.scroll_known()) {
            keep(f, "shop_scroll_unknown");
            res.trouble = "không đọc được thanh cuộn của túi đồ, nên không đếm được số ô";
            stop = true;
            return false;
        }
        const auto slots = v.slots(l);
        for (std::size_t s = 0; s < slots.size(); ++s) {
            const int r = static_cast<int>(s) / l.cols, c = static_cast<int>(s) % l.cols;
            const long long key = static_cast<long long>(v.first_row + r) * 100 + c;
            if (seen.count(key) || bag::is_empty(f, slots[s], l)) continue;
            seen.insert(key);
            const TrashList::Verdict k = trash_->judge(f, slots[s], settings_.catalog_score);
            if (k.item < 0) {
                if (k.doubt == TrashList::Verdict::Doubt::Rival && k.like >= 0) ++doubtful;
                continue;
            }
            const TrashList::Item& it = trash_->items[static_cast<std::size_t>(k.item)];
            const std::string& id = it.via.empty() ? it.id : it.via;
            if (id == CleanSettings::kBuyHpId) ++hp;
            else if (id == CleanSettings::kBuyMpId) ++mp;
        }
        if (v.at_bottom(l)) {
            complete = true;
            break;
        }
        if (++scrolls > 30) break;
        if (!scroll_arrow(true, v)) break;
    }
    if (!complete) {
        Frame lost;
        BagView seen_now;
        grab_view(lost, seen_now);
        keep(lost, "shop_count_incomplete");
        if (res.trouble.empty()) res.trouble = "không đếm hết được các ô của ngăn Tiêu Dùng (thanh cuộn không chạy xuống hết trang)";
        stop = true;
        return false;
    }
    say("ngăn Tiêu Dùng có " + std::to_string(hp) + " ô Thịt bò khô (HP) và " + std::to_string(mp) +
        " ô Rượu lá tre (MP)" +
        (doubtful > 0 ? "; " + std::to_string(doubtful) + " ô giống mà không chắc nên không tính" : ""));
    return true;
}

bool BagCleaner::shop_open(BagView& v, Result& res, Frame& shot, bag::Shop& shop, bool& clicked) {
    const BagLayout& l = settings_.layout;
    clicked = false;
    {
        // The shop up already -- opened late after the last run gave up on it,
        // or by hand -- with nothing over it: taken as it is, as GoC does.
        Frame f0;
        if (grab_ && grab_(f0) && f0.valid()) {
            const bag::Shop open = bag::find_shop(f0, store_);
            if (open.found) {
                shop_buttons_ = open.buttons;       // its own Mua | Thoát are no dialog
                if (dialogs_here(f0, store_).empty() && !bag::notice_up(f0, store_) && !question_still(f0)) {
                    shop = open;
                    shot = f0;
                    shop_row_dy_ = open.buttons.y - open.row.y;
                    say("cửa hàng đã mở sẵn, mua luôn");
                    return true;
                }
                shop_buttons_ = ScreenRect{};
            }
        }
    }
    if (!select_tab(kOtherTab, v, res, "shop_other_tab")) return false;
    scroll_to_top(v);

    // The Người Giữ Sân item: the one slot of Khác the settings' icon is
    // drawn like, confidently -- no unlisted look-alike -- on the first page
    // it is on. Anything else on Khác is never right-clicked: a right-click
    // there uses what it lands on.
    // The item by the settings' catalog icon, which the live client's bag
    // draws it as at 1.000 (2026-10-31): no picture of the player's is needed.
    const auto is_opener = [&](const TrashList::Item& it) {
        return (it.via.empty() ? it.id : it.via) == settings_.buy_opener;
    };
    int index = -1;
    int first_row = -1;
    int doubtful = 0, seen_items = 0, pages = 0;
    float found_score = 0.0f;
    Frame last_page;
    for (int round = 0, scrolls = 0; round < 40 && index < 0 && !cancelled_; ++round) {
        Frame f;
        if (!look_page(f, v)) {
            keep(f, "shop_bag_lost");
            res.trouble = "mất dấu túi đồ";
            return false;
        }
        if (v.selected_tab != kOtherTab) {
            res.trouble = "không chắc túi đồ đang ở ngăn Khác";
            return false;
        }
        ++pages;
        last_page = f;
        const auto slots = v.slots(l);
        for (std::size_t s = 0; s < slots.size() && index < 0; ++s) {
            if (bag::is_empty(f, slots[s], l)) continue;
            ++seen_items;
            const TrashList::Verdict k = trash_->judge(f, slots[s], settings_.catalog_score);
            if (k.item < 0) {
                if (k.doubt == TrashList::Verdict::Doubt::Rival && k.like >= 0) {
                    ++doubtful;
                    if (doubtful == 1) {
                        keep(f, "shop_doubt");
                        say("một ô của ngăn Khác giống " + (trash_->items[static_cast<std::size_t>(k.like)].id.empty() ? std::string("hình đã chỉ") : trash_->items[static_cast<std::size_t>(k.like)].id) + " (" + std::to_string(static_cast<int>(k.like_score * 100)) + "%) mà " + k.rival + " cũng giống như vậy (" + std::to_string(static_cast<int>(k.rival_score * 100)) + "%), nên không chắc");
                    }
                }
                continue;
            }
            const TrashList::Item& it = trash_->items[static_cast<std::size_t>(k.item)];
            if (!is_opener(it)) continue;
            index = static_cast<int>(s);
            first_row = v.first_row;
            found_score = k.score;
        }
        if (index >= 0) break;
        if (!v.scroll_known() || v.at_bottom(l) || ++scrolls > 10) break;
        if (!scroll_arrow(true, v)) break;
    }
    if (index < 0) {
        keep(last_page, "shop_no_opener");
        res.trouble = doubtful > 0
            ? "không chắc món nào của ngăn Khác là Người Giữ Sân (giống mà lẫn với món khác)"
            : "không thấy món Người Giữ Sân trong ngăn Khác (" + std::to_string(seen_items) + " món trên " +
                  std::to_string(pages) + " trang, không món nào giống " + settings_.buy_opener +
                  "). Món Người Giữ Sân cần nằm trong ngăn Khác của túi đồ";
        return false;
    }
    {
        char line[160];
        std::snprintf(line, sizeof line, "thấy món Người Giữ Sân ở ngăn Khác, hàng %d ô %d (độ giống %.2f)",
                      first_row + index / l.cols + 1, index % l.cols + 1, found_score);
        say(line);
    }

    // Looked at once more, the moment before the click: the same page, the same
    // slot, nothing else of the game's open beside the bag, nothing up.
    Frame f;
    if (!look_page(f, v) || v.selected_tab != kOtherTab || !v.scroll_known() || v.first_row != first_row) {
        res.trouble = "túi đồ đã đổi trang trước khi bấm chuột phải vào món Người Giữ Sân";
        return false;
    }
    const auto slots = v.slots(l);
    if (static_cast<std::size_t>(index) >= slots.size()) return false;
    const TrashList::Verdict now = trash_->judge(f, slots[static_cast<std::size_t>(index)], settings_.catalog_score);
    if (now.item < 0 || !is_opener(trash_->items[static_cast<std::size_t>(now.item)])) {
        res.trouble = "ô của món Người Giữ Sân không còn như vừa thấy";
        return false;
    }
    if (!dialogs_here(f, store_).empty() || bag::notice_up(f, store_) || bag::find_shop(f, store_).found) {
        res.trouble = "đang có hộp thoại hoặc cửa hàng mở sẵn; đóng nó trong game rồi thử lại";
        return false;
    }
    ScreenRect other;
    if (bag::other_window_up(f, store_, v, l, &other)) {
        res.trouble = "đang có cửa sổ khác mở cạnh túi đồ (kho, cửa hàng…); đóng nó rồi thử lại";
        keep(f, "shop_other_window");
        return false;
    }
    bool stop = false;
    if (charm_must_stop(res, stop)) return false;
    const ScreenRect s = slots[static_cast<std::size_t>(index)];
    if (!mouse_ok(s.cx(), s.cy())) {
        res.trouble = "không bấm được vào túi đồ: cửa sổ game bị che hoặc không ở trên cùng";
        return false;
    }
    say("bấm chuột phải vào món Người Giữ Sân");
    if (!right_click(s.cx(), s.cy())) {
        res.trouble = cancelled_ ? "dừng giữa chừng" : "không bấm chuột phải được vào túi đồ";
        return false;
    }
    clicked = true;         // from here on a shop may come, seen or not
    // Then off the item, a move and no click, onto the bag's title bar: with
    // the pointer resting on it the game draws the item's description, and on
    // the live client (2026-10-31) that lay over the shop's Mua | Thoát, so
    // the shop was not seen to open.
    park_pointer(true);

    // The shop, seen: its first row and its buttons where the row says. Any
    // dialog or message that comes up instead is not answered but for Hủy Bỏ.
    Frame g;
    bool reparked = false;
    for (int waited = 0; waited < kShopOpenMs; waited += kShopLookMs) {
        if (!wait_ms(kShopLookMs)) return false;
        if (!grab_ || !grab_(g) || !g.valid()) continue;
        shop = bag::find_shop(g, store_);
        // Its row seen without its buttons: something -- the description of
        // whatever the pointer rests on -- lies over them. Moved off once more.
        if (!shop.found && !reparked && bag::shop_row_up(g, store_)) {
            reparked = true;
            park_pointer(true);
        }
        if (shop.found) {
            shot = g;
            shop_buttons_ = shop.buttons;
            shop_row_dy_ = shop.buttons.y - shop.row.y;
            char line[160];
            std::snprintf(line, sizeof line, "cửa hàng đã mở (hàng đầu ở %d,%d, nút Mua ở %d,%d)", shop.row.x,
                          shop.row.y, shop.buttons.x, shop.buttons.y);
            say(line);
            return true;
        }
        if (!dialogs_here(g, store_).empty()) {
            keep(g, "shop_wrong_dialog");
            shop_cancel(5);
            res.trouble = "bấm chuột phải vào món Người Giữ Sân mà hiện ra một hộp thoại, không phải cửa hàng";
            return false;
        }
    }
    keep(g, "shop_not_seen");
    res.trouble = "đã bấm chuột phải vào món Người Giữ Sân mà không thấy cửa hàng hiện ra (hàng đầu có Rượu lá "
                  "tre và Thịt bò khô cùng nút Mua | Thoát)";
    return false;
}

int BagCleaner::shop_box(const ScreenRect& pair, Frame& last, int min_ms) {
    // A number that changes while it is watched -- a key landing late -- is
    // watched once more before it is called mixed.
    const int first = shop_box_once(pair, last, min_ms);
    return first == kBoxMixed ? shop_box_once(pair, last, min_ms) : first;
}

int BagCleaner::shop_box_once(const ScreenRect& pair, Frame& last, int min_ms) {
    int agreed = CountReader::kUnreadable, same = 0;
    bool mixed = false;
    for (int waited = 0; waited <= kShopBoxWatchMs; waited += kShopBoxLookMs) {
        Frame f;
        if (!grab_ || !grab_(f) || !f.valid()) {
            if (!wait_ms(kShopBoxLookMs)) break;
            continue;
        }
        last = f;
        // Where it was, by its pair, or by its Xác Nhận alone when it is the
        // shop's question -- see question_still.
        bool there = same_place(pair, shop_q_) && question_still(f);
        for (const bag::Dialog& d : dialogs_here(f, store_)) there = there || same_place(d.pair, pair);
        if (!there) return kBoxGone;
        const ScreenRect box{ pair.x + kBoxDx, pair.y + kBoxDy, kBoxW, std::max(4, pair.h - 6) };
        const int n = counts_->read_field(f, box);
        if (n != CountReader::kUnreadable) {
            if (same > 0 && n == agreed) {
                ++same;
            } else {
                if (same > 0) mixed = true;
                agreed = n;
                same = 1;
            }
            if (same >= kShopBoxAgree && waited >= min_ms) return agreed;
        }
        if (!wait_ms(kShopBoxLookMs)) break;
    }
    if (mixed) return kBoxMixed;
    return CountReader::kUnreadable;
}

bag::Shop BagCleaner::shop_now(const Frame& f) const {
    bag::Shop s = bag::find_shop(f, store_);
    if (s.found || !shop_buttons_.valid() || !f.valid()) return s;
    // Its first row hidden -- by the quantity box, say -- its buttons alone,
    // where they were: a shop that cannot be seen is still a shop open.
    const Frame* b = store_.get(bag::kShopButtons);
    if (b == nullptr) return s;
    const ScreenRect around = clamp_to(f, ScreenRect{ shop_buttons_.x - 6, shop_buttons_.y - 6, b->width + 12,
                                                      b->height + 12 });
    if (around.w < b->width || around.h < b->height) return s;
    const auto m = TemplateMatcher::find(f, around, *b, kShopButtonsScore);
    if (!m.found) return s;
    s.found = true;
    s.buttons = m.at;
    s.buttons_score = m.score;
    s.row = ScreenRect{ m.at.x - bag::kShopButtonsDx, m.at.y - shop_row_dy_, 294, 21 };
    return s;
}

int BagCleaner::shop_purchase(bool hp, Result& res) {
    const std::string name = hp ? "Thịt bò khô" : "Rượu lá tre";
    // A fresh look at the shop for every purchase, and nothing clicked from an
    // older one: where its row is now.
    bag::Shop shop;
    {
        Frame f0;
        if (grab_ && grab_(f0)) shop = shop_now(f0);
        if (!shop.found) {
            keep(f0, "shop_gone");
            res.trouble = "không thấy cửa hàng trên màn hình trước khi chọn " + name;
            return kShopStopped;
        }
        // Nothing up over it: a question left from the last potion would take
        // the double-click, and the keys after it.
        if (!dialogs_here(f0, store_).empty() || question_still(f0)) {
            keep(f0, "shop_dialog_up");
            res.trouble = "đang có một hộp thoại mở trước khi chọn " + name + "; đóng nó trong game";
            return kShopStopped;
        }
        shop_buttons_ = shop.buttons;
        if (shop.row_score > 0.0f) shop_row_dy_ = shop.buttons.y - shop.row.y;
    }
    // The potion's cell double-clicked, as the player does: that brings up the
    // quantity box with the keys going into it. Mua is never clicked -- the
    // box it brings up took no key on the live client (2026-10-31), its Hủy Bỏ
    // drawn pressed instead.
    const int cx = hp ? shop.hp_x() : shop.mp_x(), cy = shop.item_y();
    if (!mouse_ok(cx, cy)) {
        res.trouble = "không bấm được vào cửa hàng: cửa sổ game bị che hoặc không ở trên cùng";
        return kShopStopped;
    }
    say("bấm đúp vào " + name);
    if (!double_click(cx, cy)) {
        res.trouble = cancelled_ ? "dừng giữa chừng" : "không bấm đúp được vào " + name;
        return kShopStopped;
    }
    // Then off the list, a move and no click, onto the bare panel left of Mua:
    // a pointer resting on an item has the game draw its description beside
    // it, over where the quantity box comes up.
    if (!hands_.click && !hands_.double_click) input_.move_pointer(shop.buttons.x - 30, shop.button_y());

    // The quantity question: the one dialog, taken for a shop's by its box and
    // words; or the message that the bag is full. Any other dialog is answered
    // Hủy Bỏ, and the run stops.
    bag::Dialog q;
    bool asked = false;
    int others = 0;
    Frame g;
    for (int waited = 0; waited < kShopAskMs && !asked; waited += kShopLookMs) {
        if (!wait_ms(kShopLookMs)) return kShopStopped;
        Frame h;
        if (!grab_ || !grab_(h) || !h.valid()) continue;
        g = std::move(h);
        ScreenRect at;
        if (bag::shop_full_up(g, store_, &at)) {
            keep(g, "shop_full");
            say("game báo túi đồ đã đầy");
            if (mouse_ok(at.x + bag::kShopFullOkX, at.y + bag::kShopFullOkY)) click(at.x + bag::kShopFullOkX, at.y + bag::kShopFullOkY);
            return kShopFullBag;
        }
        const auto dialogs = dialogs_here(g, store_);
        if (dialogs.size() == 1 && bag::shop_question(g, store_, dialogs.front(), *counts_, settings_.buy_frame_score)) {
            q = dialogs.front();
            asked = true;
        } else if (!dialogs.empty() && ++others >= 2) {
            keep(g, "shop_wrong_dialog");
            shop_cancel(5);
            res.trouble = "hiện ra một hộp thoại không giống hộp nhập số lượng của cửa hàng, nên bấm Hủy Bỏ";
            return kShopStopped;
        }
    }
    if (!asked) {
        keep(g, "shop_no_question");
        res.trouble = "bấm đúp vào " + name + " mà không thấy hộp nhập số lượng";
        return kShopStopped;
    }
    shop_q_ = q.pair;
    if (dry_run_) keep(g, "shop_question");     // a trial's record; a purchase keeps pictures only of trouble

    // What the box holds, as the game filled it in: nothing, or a 1 (as the
    // sell box's) -- anything else is not guessed at.
    Frame last;
    const int have = shop_box(q.pair, last);
    if (have != 0 && have != 1) {
        keep(last, "shop_box");
        shop_cancel(5);
        res.trouble = have == kBoxGone       ? "hộp nhập số lượng biến mất"
                      : have == kBoxMixed    ? "số trong hộp nhập số lượng đổi liên tục, nên không nhập"
                      : have == CountReader::kUnreadable ? "không đọc được số game để sẵn trong hộp nhập số lượng"
                                                         : "game để sẵn số " + std::to_string(have) +
                                                               " trong hộp nhập số lượng, không phải 0 hay 1";
        return kShopStopped;
    }
    say("hộp nhập số lượng đang chứa " + std::to_string(have));
    if (dry_run_) {
        if (!shop_cancel(5)) {
            keep(last, "shop_cancel_failed");
            res.trouble = "thử mua: đã bấm Hủy Bỏ mà không thấy hộp nhập số lượng đóng lại; đóng nó trong game";
            return kShopStopped;
        }
        say("thử mua: dừng ở hộp nhập số lượng, đã bấm Hủy Bỏ, không gõ gì");
        return kShopProbed;
    }

    // 100, typed digit by digit, the dialog seen in its place before every key,
    // and the box read back through a blink of its caret: 100, or Hủy Bỏ.
    const std::string target = std::to_string(CleanSettings::kBuyQuantity);
    // The one dialog, where it was, on a fresh look: before every key. Seen
    // by find_dialogs, it is still the shop's question by its box and frame;
    // not seen -- its Hủy Bỏ drawn pressed -- its Xác Nhận is where it was, and
    // no other dialog is up.
    const auto box_up = [&] {
        Frame k;
        if (!grab_ || !grab_(k) || !k.valid()) return false;
        const auto up = dialogs_here(k, store_);
        if (up.empty()) return question_still(k);
        return up.size() == 1 && same_place(up.front().pair, q.pair) &&
               bag::shop_question(k, store_, up.front(), *counts_, settings_.buy_frame_score, false);
    };
    // The box clicked into, at its right end where the caret goes after what is
    // in it: the second way of typing. On the live client (2026-10-31) digits
    // typed at the box Mua brought up never reached it, its Hủy Bỏ drawn
    // pressed instead, clicked into or not.
    const ScreenRect box{ q.pair.x + kBoxDx, q.pair.y + kBoxDy, kBoxW, std::max(4, q.pair.h - 6) };
    const auto focus_box = [&] {
        const int bx = box.right() - 6, by = box.cy();
        if (!box_up() || !mouse_ok(bx, by)) return false;
        if (!click(bx, by) || !wait_ms(kShopSettleMs)) return false;
        return box_up();
    };
    // Each key with its key code, as the chat types; once that has put nothing
    // in the box, clicked into and as its scan code alone, as GoC types.
    bool scan = shop_scan_;
    const auto type_in = [&](const std::string& digits) {
        // Straight in, as the player types after the double-click; clicked
        // into first only on the second way of typing.
        if (scan && !focus_box()) return false;
        for (const char c : digits) {
            if (!box_up()) return false;
            const auto vk = static_cast<std::uint32_t>(c);     // a digit's key code is its character
            if (!(scan ? press_scan(vk) : press(vk))) return false;
            if (!wait_ms(settings_.press_ms)) return false;
        }
        return true;
    };
    std::string typed = have == 1 ? "00" : "100";
    int now_has = have;
    bool keys_refused = false;
    for (int fix = 0; fix <= kShopFixTries; ++fix) {
        if (!typed.empty() && !type_in(typed)) {
            shop_cancel(5);
            res.trouble = cancelled_ ? "dừng giữa chừng" : "hộp nhập số lượng không còn như lúc đầu khi gõ số";
            return kShopStopped;
        }
        // After digits typed on what was read -- a top-up, an erase -- the box
        // is watched as long as it used to be: keys sent on a stale picture
        // could still be landing.
        now_has = shop_box(q.pair, last, fix > 0 ? kShopBoxWatchMs - 2 * kShopBoxLookMs : 0);
        if (now_has == CleanSettings::kBuyQuantity) break;
        if (now_has < 0 || fix == kShopFixTries) break;
        if (now_has == have && !typed.empty()) {
            keep(last, "shop_keys_not_taken");
            // Neither way reached the box: no more keys -- where they go
            // instead is not known, and the game's potion keys are digits.
            if (scan) {
                keys_refused = true;
                break;
            }
            say("phím số không vào ô số lượng (ô vẫn là " + std::to_string(have) +
                "); bấm lại vào ô và gõ lại bằng mã quét phím");
            scan = true;
            continue;
        }
        // Short of 100 and a start of it: the rest of its digits. Otherwise
        // erased with Backspace and typed again.
        const std::string shown = std::to_string(now_has);
        if (now_has > 0 && target.compare(0, shown.size(), shown) == 0 && shown.size() < target.size()) {
            typed = target.substr(shown.size());
        } else {
            bool erased = true;
            for (std::size_t i = 0; erased && i < std::max<std::size_t>(shown.size(), 1) + 1; ++i)
                erased = box_up() && (scan ? press_scan(VK_BACK) : press(VK_BACK));
            if (!erased || !wait_ms(settings_.press_ms)) {
                shop_cancel(5);
                res.trouble = cancelled_ ? "dừng giữa chừng" : "hộp nhập số lượng không còn như lúc đầu khi xoá số";
                return kShopStopped;
            }
            typed = target;
        }
    }
    if (now_has != CleanSettings::kBuyQuantity) {
        keep(last, "shop_box");
        shop_cancel(5);
        res.trouble = keys_refused
            ? "game không nhận phím số vào ô số lượng (cả mã phím lẫn mã quét phím, ô vẫn là " +
                  std::to_string(have) + "), nên bấm Hủy Bỏ"
            : "số trong hộp nhập số lượng không thành " + target + " (đọc được " +
                  (now_has >= 0 ? std::to_string(now_has) : std::string("không rõ")) + "), nên bấm Hủy Bỏ";
        return kShopStopped;
    }
    say("đã nhập " + target + ", hộp đọc được " + target);
    shop_scan_ = scan;      // the next purchase of the run types the way this one did

    // Xác Nhận on a fresh look: the one dialog, where it was.
    Frame k;
    if (!grab_ || !grab_(k) || !k.valid()) {
        shop_cancel(5);
        res.trouble = "không chụp được màn hình game để bấm Xác Nhận";
        return kShopStopped;
    }
    const auto still = dialogs_here(k, store_);
    const bool seen_whole = still.size() == 1 && same_place(still.front().pair, q.pair) &&
                            bag::shop_question(k, store_, still.front(), *counts_, settings_.buy_frame_score, false);
    if (!seen_whole && !(still.empty() && question_still(k))) {
        keep(k, "shop_changed");
        shop_cancel(5);
        res.trouble = "lúc sắp bấm Xác Nhận thì trên màn hình không chỉ có hộp nhập số lượng";
        return kShopStopped;
    }
    if (!mouse_ok(q.confirm_x(), q.y(), true) || !click(q.confirm_x(), q.y())) {
        shop_cancel(5);
        res.trouble = cancelled_ ? "dừng giữa chừng" : "không bấm được Xác Nhận";
        return kShopStopped;
    }
    // The question gone, and what came after: the bag-full message, or nothing.
    for (int waited = 0; waited < kShopGoneMs; waited += kShopLookMs) {
        if (!wait_ms(kShopLookMs)) return kShopStopped;
        Frame h;
        if (!grab_ || !grab_(h) || !h.valid()) continue;
        ScreenRect at;
        if (bag::shop_full_up(h, store_, &at)) {
            keep(h, "shop_full");
            say("game báo túi đồ đã đầy");
            if (mouse_ok(at.x + bag::kShopFullOkX, at.y + bag::kShopFullOkY)) click(at.x + bag::kShopFullOkX, at.y + bag::kShopFullOkY);
            return kShopFullBag;
        }
        bool there = question_still(h) || !seen_at(q.pair);
        for (const bag::Dialog& d : dialogs_here(h, store_)) there = there || same_place(d.pair, q.pair);
        if (!there) {
            shop_q_ = ScreenRect{};
            if (bag::notice_up(h, store_)) {
                keep(h, "shop_notice");
                res.trouble = "sau khi bấm Xác Nhận game hiện một thông báo lạ (để nguyên cho người chơi đọc)";
                return kShopStopped;
            }
            say("đã mua " + target + " " + name);
            return kShopBought;
        }
    }
    keep(k, "shop_not_closed");
    shop_cancel(5);
    res.trouble = "hộp nhập số lượng không đóng lại sau khi bấm Xác Nhận";
    return kShopStopped;
}

bool BagCleaner::shop_close(const bag::Shop& shop, Result& res) {
    // ESC once -- the game closes the window in front -- and looked at; the
    // shop's own Thoát when it is still there. ESC is never pressed twice: with
    // nothing left to close it brings up the game's own menu.
    bring_forward();
    // The bag before the ESC: the bag clicked last is the window in front, and
    // the ESC may shut it and leave the shop -- seen, that is its one ESC
    // done, and Thoát comes at once rather than after the whole wait.
    Frame f0;
    const bool bag_before = grab_ && grab_(f0) && f0.valid() && bag::find(f0, store_, settings_.layout).found;
    const auto gone = [&](Frame* last, bool after_esc) {
        for (int waited = 0; waited < kShopGoneMs; waited += kShopLookMs) {
            pause_ms(kShopLookMs);
            Frame f;
            if (!grab_ || !grab_(f) || !f.valid()) continue;
            if (last != nullptr) *last = f;
            if (!shop_now(f).found) return true;
            if (after_esc && bag_before && !bag::find(f, store_, settings_.layout).found) return false;
        }
        return false;
    };
    say("bấm ESC để đóng cửa hàng");
    press(VK_ESCAPE);
    Frame last;
    if (gone(&last, true)) return true;
    // Still open: its Thoát, where a look after the ESC shows the shop -- and
    // only there. No look since the ESC is no shop seen: nothing is clicked
    // where it was before, which the ESC may have closed.
    (void)shop;
    const bag::Shop now = shop_now(last);
    // Added to what stopped the run, never in its place -- and once.
    const auto trouble = [&](const std::string& why) {
        if (res.trouble.find(why) == std::string::npos) res.trouble += (res.trouble.empty() ? "" : "; ") + why;
    };
    if (!now.found) {
        trouble("không nhìn được màn hình game sau khi bấm ESC để đóng cửa hàng; xem lại trong game");
        return false;
    }
    if (mouse_ok(now.thoat_x(), now.button_y(), false)) {
        say("ESC không đóng được cửa hàng, bấm Thoát");
        click(now.thoat_x(), now.button_y(), false);
        if (gone(&last, false)) return true;
    }
    keep(last, "shop_not_closed");
    trouble("không đóng được cửa hàng (ESC rồi nút Thoát); đóng nó trong game");
    return false;
}

void BagCleaner::shop_buy(BagView& v, Result& res, bool& stop) {
    if (counts_ == nullptr || !counts_->loaded()) {
        res.trouble = "không có bộ chữ số của game để đọc số trong hộp nhập số lượng";
        return;
    }
    if (!bag::can_check_shop(store_)) {
        res.trouble = "GoBot chưa có hình của game để mua bình (cửa hàng Người Giữ Sân, hộp bán, hộp thoại thùng "
                      "rác hay nút đóng cửa sổ)";
        return;
    }
    if (trash_ == nullptr || trash_->items.empty()) {
        res.trouble = "thiếu danh sách món để đếm bình";
        return;
    }
    shop_buttons_ = ScreenRect{};
    shop_q_ = ScreenRect{};
    shop_scan_ = false;
    int prev_hp = -1, prev_mp = -1;
    int bought_hp = 0, bought_mp = 0;        // what the round before bought, of each
    bool want_hp = false, want_mp = false;   // under their thresholds when first counted
    // The shop is kept open from one round to the next -- the bag beside it is
    // counted all the same, as it was on the live client -- and shut once, at
    // the end, whatever the end is: nothing left open with the keys let go.
    // Looked for as the shop, or as its buttons alone where they were, when
    // something hides its row.
    bool shop_up = false;
    // Right-clicked and the shop never seen: it may still come -- on the live
    // client it came after the wait for it was over (2026-10-31) -- and is
    // waited for once more, a stop included, and shut too.
    bool late = false;
    const auto close_shop = [&] {
        if (late) {
            for (int waited = 0; waited < kShopOpenMs; waited += kShopLookMs) {
                Frame g;
                if (grab_ && grab_(g) && g.valid() && shop_now(g).found) break;
                pause_ms(kShopLookMs);
            }
        }
        bool closed = false;
        for (int look = 0; look < 2 && !closed; ++look) {
            Frame g;
            // No look is not a shop seen shut.
            if (!grab_ || !grab_(g) || !g.valid()) continue;
            const bag::Shop open = shop_now(g);
            if (!open.found) {
                // Its row with its buttons hidden -- an item's description over
                // them -- is still up: the pointer moved off, and looked again.
                if (bag::shop_row_up(g, store_)) {
                    park_pointer(true);
                    pause_ms(250);
                    continue;
                }
                closed = true;
                break;
            }
            // A late shop with something over it gets no ESC: run()'s end
            // check keeps the hold for the player.
            if (late && (!dialogs_here(g, store_).empty() || question_still(g))) break;
            closed = shop_close(open, res);
        }
        shop_buttons_ = ScreenRect{};
        shop_up = !closed;
        late = false;
        return closed;
    };
    // The bag counted beside an open shop only where the shop is clear of what
    // the count clicks -- the tabs, the slots, the scroll arrows: a shop dragged
    // over the bag is shut first, and opened again when it is wanted. Its
    // window from its buttons, as the client's sprite has them (197,391 into a
    // window about 330x420), with room to spare.
    const auto clear_for_count = [&](BagView& bv) {
        Frame g;
        if (!grab_ || !grab_(g) || !g.valid()) return false;
        const bag::Shop s = shop_now(g);
        if (!s.found) return true;
        const BagLayout& l = settings_.layout;
        const ScreenRect win{ s.buttons.x + bag::kShopWindowFromButtons.x - 6,
                              s.buttons.y + bag::kShopWindowFromButtons.y - 6, bag::kShopWindowFromButtons.w + 16,
                              bag::kShopWindowFromButtons.h + 20 };
        // The tabs, the slots, the scroll arrows and the bin with the margin
        // its can is looked for in: what the count clicks and reads.
        const ScreenRect work{ bv.strip.x - 4, bv.strip.y - 4, std::max(l.scroll_up_dx, l.bin_dx + l.bin_w) + 24,
                               std::max(l.scroll_down_dy, l.bin_dy + l.bin_h) + 12 };
        const bool over = win.x < work.right() && work.x < win.right() && win.y < work.bottom() && work.y < win.bottom();
        if (!over) return true;
        if (!close_shop()) return false;
        Frame f;
        return grab_view(f, bv);
    };
    const auto rounds = [&] {
        for (int round = 0; round < kShopRounds && !stop && !cancelled_; ++round) {
            if (charm_must_stop(res, stop)) return;
            // The bag, found again: a closed shop may have taken it with it. Its
            // key only on a look with nothing up -- with a dialog up it would go
            // into its box.
            Frame f;
            bool seen = grab_view(f, v);
            if (!seen && shop_up && f.valid() && dialogs_here(f, store_).empty() && !question_still(f) &&
                shop_now(f).found) {
                // The shop kept open lying over the bag's strip -- likely in a
                // small game window: shut, nothing else being up, and looked
                // at again; the next purchase opens it again.
                if (!close_shop()) {
                    if (res.trouble.empty()) res.trouble = "cửa hàng đang che túi đồ và không đóng được";
                    stop = true;
                    return;
                }
                seen = grab_view(f, v);
            }
            if (!seen) {
                if (!f.valid() || !dialogs_here(f, store_).empty() || shop_now(f).found) {
                    res.trouble = "không thấy túi đồ, mà đang có hộp thoại hoặc cửa hàng mở; đóng nó trong game";
                    stop = true;
                    return;
                }
                press(settings_.bag_key);
                for (int i = 0; i < 12 && !v.found; ++i) {
                    if (!wait_ms(150)) return;
                    grab_view(f, v);
                }
                if (!v.found) {
                    res.trouble = "không mở lại được túi đồ (phím " + InputManager::key_name(settings_.bag_key) + ")";
                    stop = true;
                    return;
                }
            }
            int hp = 0, mp = 0;
            if (!clear_for_count(v)) {
                if (res.trouble.empty()) res.trouble = "cửa hàng đang che túi đồ và không đóng được, nên không đếm được bình";
                stop = true;
                return;
            }
            if (!shop_count(kUseTab, v, res, stop, hp, mp)) return;
            res.shop_hp_after = hp;
            res.shop_mp_after = mp;
            if (round == 0) {
                res.shop_hp_before = hp;
                res.shop_mp_before = mp;
                want_hp = settings_.buy_hp_below > 0 && hp < settings_.buy_hp_below;
                want_mp = settings_.buy_mp_below > 0 && mp < settings_.buy_mp_below;
            } else {
                // What the last round bought has to show as slots, potion by
                // potion: a purchase that adds no slot is a stack that took it, a
                // cap the count does not know, or the wrong item bought -- and
                // that potion is not bought again, nor on the next look.
                const bool no_hp = bought_hp > 0 && hp <= prev_hp;
                const bool no_mp = bought_mp > 0 && mp <= prev_mp;
                if (no_hp || no_mp) {
                    res.shop_no_gain = true;
                    res.trouble = std::string("đã mua ") + (no_hp && no_mp ? "cả Thịt bò khô lẫn Rượu lá tre"
                                                            : no_hp       ? "Thịt bò khô"
                                                                          : "Rượu lá tre") +
                                  " mà số ô không tăng (chồng cũ nhận thêm, hoặc GoBot chọn nhầm món), nên không mua "
                                  "nữa";
                    return;
                }
            }
            prev_hp = hp;
            prev_mp = mp;
            int need_hp = want_hp ? settings_.buy_hp_to - hp : 0;
            int need_mp = want_mp ? settings_.buy_mp_to - mp : 0;
            if (need_hp <= 0 && need_mp <= 0 && !dry_run_) {
                say(round == 0 ? "đủ bình, không cần mua" : "đã mua đủ bình");
                return;
            }
            if (dry_run_) {
                say("thử mua: cần mua " + std::to_string(std::max(0, need_hp)) + " ô Thịt bò khô, " +
                    std::to_string(std::max(0, need_mp)) + " ô Rượu lá tre; thử cả hai món một lần");
                need_hp = need_mp = 1;
            } else if (round == 0) {
                // The first round is a try: one of each, then the slots are counted
                // and what grew is seen to be the potion that was asked for, before
                // the rest is bought.
                need_hp = std::min(need_hp, 1);
                need_mp = std::min(need_mp, 1);
            }
            need_hp = std::clamp(need_hp, 0, std::max(0, kShopMaxBuys - res.shop_bought));
            need_mp = std::clamp(need_mp, 0, std::max(0, kShopMaxBuys - res.shop_bought - need_hp));
            if (need_hp + need_mp <= 0) {
                res.trouble = "đã mua đủ số lần tối đa của một lượt (" + std::to_string(kShopMaxBuys) + ")";
                return;
            }

            Frame shot;
            bag::Shop shop;
            bool kept = false;
            if (shop_up) {
                Frame g;
                if (grab_ && grab_(g) && g.valid()) shop = shop_now(g);
                kept = shop.found;
            }
            bool clicked = false;
            if (!kept && !shop_open(v, res, shot, shop, clicked)) {
                if (clicked) shop_up = late = true;
                stop = true;
                return;
            }
            shop_up = true;
            late = false;
            res.shop_opened = true;
            bool leave = false;
            bought_hp = bought_mp = 0;
            for (int kind = 0; kind < 2 && !stop && !leave; ++kind) {
                const bool hp_kind = kind == 0;
                const int n = hp_kind ? need_hp : need_mp;
                for (int i = 0; i < n && !stop && !leave; ++i) {
                    if (charm_must_stop(res, stop)) break;
                    const int r = shop_purchase(hp_kind, res);
                    if (r == kShopBought) {
                        ++res.shop_bought;
                        ++(hp_kind ? bought_hp : bought_mp);
                    } else if (r == kShopFullBag) {
                        res.shop_full = true;
                        leave = true;
                    } else if (r == kShopProbed) {
                        break;                  // one of each is all a trial asks for
                    } else {
                        stop = true;
                    }
                }
            }
            if (dry_run_) leave = true;
            if (stop || cancelled_) return;
            // The purchases a moment to reach the bag before it is counted again.
            if (!wait_ms(kShopSettleMs)) return;
            if (leave) {
                // The count once more, for the message -- the bag in sight, nothing
                // pressed but its tab.
                if (!dry_run_ && res.shop_bought > 0 && grab_view(f, v) && clear_for_count(v)) {
                    int h2 = 0, m2 = 0;
                    bool again_stop = false;
                    Result quiet;
                    if (shop_count(kUseTab, v, quiet, again_stop, h2, m2)) {
                        res.shop_hp_after = h2;
                        res.shop_mp_after = m2;
                    }
                }
                return;
            }
        }
        // The rounds are used up: counted once more, to say where it stands.
        Frame f;
        if (!cancelled_ && grab_view(f, v) && clear_for_count(v)) {
            int hp = 0, mp = 0;
            bool again_stop = false;
            Result quiet;
            if (shop_count(kUseTab, v, quiet, again_stop, hp, mp)) {
                res.shop_hp_after = hp;
                res.shop_mp_after = mp;
            }
        }
    };
    rounds();
    if (shop_up && !close_shop()) stop = true;
}
} // namespace gobot
