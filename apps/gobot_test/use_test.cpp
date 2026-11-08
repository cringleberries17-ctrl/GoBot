// gobot_test -- "Dùng" in Dọn rác: registering fragments in Yêu Quái Bách Khoa
// with the trash pass, before anything on Tiêu Dùng is deleted.
//
// On a pretend game: the bag on Tiêu Dùng with items in it, the bin, and what a
// right-click on a slot brings up -- the registration question, the live
// client's own picture of it (grabbed/fragment_register, cut on 2026-10-19),
// with the monster book it opens with it, drawn from the book's two pictures;
// or the bin's question, the revive question, the same box with other words, a
// message with only Xác Nhận, or nothing. The book stays open after the
// question is answered, as on the live client, until its X is clicked. Every
// click, right-click, drag and key goes to the pretend game and is counted;
// nothing reaches the desktop, and the time is the pretend game's own.
//
// On cuts of the live client's frames (testdata\round7 beside this test: the
// question box, the book's tab and X, the ESC menu, the bin's own dialog --
// small parts, never a whole frame, none with a name in it), each put back
// where it was on a frame of the client's size: what the pass goes by, read
// off what the live client drew.
//
// And on real frames of the player's bag, for which slots are fragments: the
// only things the pass right-clicks, and the only cuts the tab offers "Dùng"
// for. A fragment's icon says only which of a monster's nine pieces it is --
// the game's own tooltip on a live frame names the monster -- so two slots
// drawn alike are taken here for what they mostly are, two monsters' pieces.

#include "gobot/bag.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

std::string temp_path(const char* name) {
    char dir[MAX_PATH]{};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return name;
    return std::string(dir) + "gobot_test_" + std::to_string(GetCurrentProcessId()) + "\\" + name;
}

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_all(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

/// The build tree of the checkout these tests were built from, where the live
/// tests saved their rounds (live\shots, frozen\..., handoff\...). Never
/// committed, and never written by GoBot as it plays.
#ifdef GOBOT_SOURCE_DIR
const char* const kLiveBuild = GOBOT_SOURCE_DIR "/build/";
#else
const char* const kLiveBuild = "";
#endif

void put_px(Frame& f, int x, int y, int r, int g, int b) {
    if (x < 0 || y < 0 || x >= f.width || y >= f.height) return;
    const std::size_t i = (static_cast<std::size_t>(y) * f.width + x) * 4;
    f.pixels[i] = static_cast<std::uint8_t>(b);
    f.pixels[i + 1] = static_cast<std::uint8_t>(g);
    f.pixels[i + 2] = static_cast<std::uint8_t>(r);
    f.pixels[i + 3] = 255;
}

void fill(Frame& f, const ScreenRect& r, int red, int green, int blue) {
    for (int y = r.y; y < r.bottom(); ++y)
        for (int x = r.x; x < r.right(); ++x) put_px(f, x, y, red, green, blue);
}

void blit(Frame& f, const Frame& t, int at_x, int at_y) {
    for (int y = 0; y < t.height; ++y)
        for (int x = 0; x < t.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * t.width + x) * 4;
            if (t.pixels[i + 3] < TemplateMatcher::kOpaque) continue;
            put_px(f, at_x + x, at_y + y, t.pixels[i + 2], t.pixels[i + 1], t.pixels[i]);
        }
}

Frame scene(int w, int h, int grey) {
    Frame f;
    f.width = w;
    f.height = h;
    f.pixels.assign(static_cast<std::size_t>(w) * h * 4, 255);
    for (std::size_t i = 0; i < f.pixels.size(); i += 4)
        f.pixels[i] = f.pixels[i + 1] = f.pixels[i + 2] = static_cast<std::uint8_t>(grey);
    return f;
}

/// Something colourful in a slot, inside its edge, nothing white where its
/// count goes; one drawing for each seed.
void paint_item(Frame& f, const ScreenRect& s, int seed) {
    for (int y = 4; y < s.h - 4; ++y)
        for (int x = 4; x < s.w - 4; ++x) {
            int r = 128 + static_cast<int>(100 * std::sin(x * 0.5 + seed));
            int g = 128 + static_cast<int>(100 * std::cos(y * 0.4 + seed * 2));
            int b = 40 + (x * y * (seed + 3)) % 200;
            if (x < 28 && y < 16 && (r + g + b) / 3 > 150) r = 200, g = 120, b = 60;
            put_px(f, s.x + x, s.y + y, r, g, b);
        }
}

/// How far the client prints a digit after the one before: its digits are 5
/// pixels wide and 6 apart, a 1 included.
constexpr int kDigitStep = 6;

/// A number in the client's own digits, white with a dark outline.
void print_number(Frame& f, int x, int y, const std::string& text, const std::string& font_dir) {
    for (const char c : text) {
        Frame g;
        load_png(font_dir + "\\lower\\" + std::string(1, c) + ".png", g);
        for (int pass = 0; pass < 2; ++pass)
            for (int gy = 0; gy < g.height; ++gy)
                for (int gx = 0; gx < g.width; ++gx) {
                    std::uint8_t r, gg, b;
                    g.get(gx, gy, r, gg, b);
                    if ((r + gg + b) / 3 <= 128) continue;
                    if (pass == 0) {
                        for (int oy = -1; oy <= 1; ++oy)
                            for (int ox = -1; ox <= 1; ++ox) put_px(f, x + gx + ox, y + gy + oy, 10, 10, 10);
                    } else {
                        put_px(f, x + gx, y + gy, 250, 250, 250);
                    }
                }
        x += kDigitStep;
    }
}

/// The panel of words in the bin's pictures, and in the registration
/// question's -- the live box a line taller -- as bag.cpp measures them.
const ScreenRect kBinPanel{ 20, 12, 158, 28 };
const ScreenRect kRegisterPanel{ 17, 11, 168, 41 };

/// Words of a question, as white strokes on the panel's dark ground: a
/// different pattern for each `seed`, so that one question's words are not
/// another's.
void write_words(Frame& pic, int seed, const ScreenRect& panel = kRegisterPanel) {
    fill(pic, panel, 33, 35, 41);
    for (int i = 0; i < 26; ++i) {
        const int x = panel.x + 3 + i * 6 + (i * seed) % 3;
        const int y = panel.y + 3 + ((i * (seed + 5)) % 2) * 12;
        const int h = 5 + (i * seed + 3) % 4;
        for (int dy = 0; dy < h; ++dy) put_px(pic, x, y + dy, 245, 245, 245);
        for (int dx = 0; dx < 1 + (i + seed) % 3; ++dx) put_px(pic, x + dx, y, 245, 245, 245);
    }
}

/// Where a question that names the piece would have the name: the left of
/// its second line, in the picture's own pixels.
const ScreenRect kNameArea{ 20, 25, 84, 15 };

/// The registration question as the game may well draw it, naming the piece:
/// the same words around a name that differs from piece to piece.
Frame named_question(const Frame& base, int name) {
    Frame q = base;
    write_words(q, 1);
    fill(q, kNameArea, 33, 35, 41);
    for (int i = 0; i < 13; ++i) {
        const int x = kNameArea.x + 2 + i * 6 + (i * name) % 4;
        const int y = kNameArea.y + 2 + (i * name) % 3;
        const int h = 4 + (i * (name + 2)) % 6;
        for (int dy = 0; dy < h; ++dy) put_px(q, x, y + dy, 245, 245, 245);
        for (int dx = 0; dx < 1 + (i * name + 1) % 4; ++dx) put_px(q, x + dx, y + h - 1, 245, 245, 245);
    }
    return q;
}

/// A picture with part of it see-through, the way the name would be cut out
/// of the registration question's picture.
Frame see_through(Frame pic, const ScreenRect& r) {
    for (int y = r.y; y < r.bottom() && y < pic.height; ++y)
        for (int x = r.x; x < r.right() && x < pic.width; ++x)
            pic.pixels[(static_cast<std::size_t>(y) * pic.width + x) * 4 + 3] = 0;
    return pic;
}

/// The pretend game's clock: on only by the cleaner's own waits and by each
/// look at the game, a frame's worth.
struct Clock {
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    BagCleaner::Clock clock() {
        BagCleaner::Clock c;
        c.now = [this] { return t; };
        c.sleep = [this](int ms) { t += std::chrono::milliseconds(ms); };
        return c;
    }
};

struct Kit {
    TemplateStore bare;         ///< the pictures a pass needs, without the registration question's
    TemplateStore store;        ///< the same with it: the live client's, as it ships
    Frame question, other;      ///< the registration question, and the same box with other words
    Frame close_x;              ///< the client's close button, as the bag and Kho Cash wear it
    Frame book_tab, book_x;     ///< the monster book's first tab and its X, as they ship
    CountReader counts;
    std::string font_dir;
    bool ok = false;
    std::string why;
    bool shop_ok = false;       ///< the shop's pictures are installed too
    Frame buy_question;         ///< the sell question's frame with other words
};

Kit& kit() {
    static Kit k;
    static const bool loaded = [] {
        const std::string ini = exe_relative("configs/templates.ini");
        for (const char* name : { "grabbed/inventory", "inven/0/trash_can", "grabbed/confirm_button",
                                  "grabbed/del_item_confirm_1", "grabbed/del_item_confirm_2", "grabbed/dialog_ok",
                                  "grabbed/hoisinh_confirm", bag::kCloseButtons[0], bag::kCloseButtons[1],
                                  bag::kCloseButtons[2], bag::kBookTitle, bag::kBookClose }) {
            Frame f;
            if (!TemplateStore::load_one(name, ini, f)) {
                k.why = name;
                return true;
            }
            k.bare.put(name, f);
            k.store.put(name, f);
        }
        k.close_x = *k.store.get(bag::kCloseButtons[0]);
        k.book_tab = *k.store.get(bag::kBookTitle);
        k.book_x = *k.store.get(bag::kBookClose);
        k.font_dir = GlyphFont::default_dir();
        if (k.font_dir.empty() || !k.counts.load(k.font_dir)) {
            k.why = "the game's digits";
            return true;
        }
        // The registration question as the live client draws it, and the
        // same box with other words in its panel: a question GoBot has no
        // picture of.
        if (!TemplateStore::load_one(bag::kRegisterDialog, ini, k.question)) {
            k.why = bag::kRegisterDialog;
            return true;
        }
        k.other = k.question;
        write_words(k.other, 4);
        k.store.put(bag::kRegisterDialog, k.question);
        // The drop and sell questions the charm run tells a charm's from, in the
        // store alone: a pass has no use for them.
        for (const char* name : { "grabbed/drop_confirm", "grabbed/sell_confirm" }) {
            Frame f;
            if (!TemplateStore::load_one(name, ini, f)) {
                k.why = name;
                return true;
            }
            k.store.put(name, f);
        }
        k.ok = true;
        // The potion shop's pictures, in the store alone, and the buy question
        // drawn as the sell question is -- its box, its frame -- with words of
        // its own: the picture the shop's question is taken to be. Without them
        // only the shop's tests are skipped.
        k.shop_ok = true;
        for (const char* name : { bag::kShopRow, bag::kShopButtons, bag::kShopFull }) {
            Frame f;
            if (!TemplateStore::load_one(name, ini, f)) {
                k.shop_ok = false;
                continue;
            }
            k.store.put(name, f);
        }
        if (k.shop_ok) {
            k.buy_question = *k.store.get(bag::kSellQuestion);
            write_words(k.buy_question, 7, ScreenRect{ 17, 11, k.buy_question.width - 34, 53 - 15 });
        }
        return true;
    }();
    (void)loaded;
    return k;
}

bool skip_without_kit() {
    if (kit().ok) return false;
    skip("the dialog pictures or the digits are not installed here: %s\n", kit().why.c_str());
    return true;
}

/// The bag's strip as the live client draws it with Tiêu Dùng chosen: that
/// tab darker than the others, Trang Bị 1 no longer.
Frame strip_on(int tab) {
    const Frame& base = *kit().store.get("grabbed/inventory");
    Frame s = base;
    const BagLayout l;
    const auto px = [&](Frame& f, int x, int y) { return &f.pixels[(static_cast<std::size_t>(y) * f.width + x) * 4]; };
    for (int y = 0; y < s.height; ++y)
        for (int x = 0; x < l.tab_w; ++x) {
            // Trang Bị 1 drawn as an unchosen tab: Trang Bị 2's look over it.
            const std::uint8_t* from = &base.pixels[(static_cast<std::size_t>(y) * base.width + x + l.tab_w) * 4];
            std::copy_n(from, 4, px(s, x, y));
        }
    if (tab > 0)
        for (int y = 0; y < s.height; ++y)
            for (int x = tab * l.tab_w; x < (tab + 1) * l.tab_w && x < s.width; ++x) {
                std::uint8_t* p = px(s, x, y);
                for (int c = 0; c < 3; ++c) p[c] = static_cast<std::uint8_t>(p[c] * 3 / 4);
            }
    return s;
}

/// A pretend game: the bag on Tiêu Dùng, items in its slots, the bin, and what
/// a right-click brings up.
struct Pretend {
    /// Charm: the question of a charm, as the game may draw it -- its own box,
    /// the registration question's, with words of its own.
    enum class Reply { Register, BinWords, Revive, Other, Notice, Nothing, Consumed, Charm };
    struct Item {
        int slot = 0;
        int seed = 1;
        int count = 1;
        Frame ask;              ///< its own registration question, naming it; else the kit's
        bool smudged = false;   ///< something white over its count that is no number
        /// The client's own icon for it, a slot's size, drawn in place of the
        /// made-up drawing of `seed`: a catalog icon as the bag shows it.
        Frame icon;
        int  tab = kUseTab;     ///< which tab of the bag it lies in, with tabs_work
        bool opener = false;    ///< the Người Giữ Sân item: a right-click opens the shop
        /// The live client's own pictures of its slot by the count in it, a
        /// slot's size, count and all: drawn as they are, in place of `icon`
        /// and a number printed over it, while it holds that count.
        std::vector<std::pair<int, Frame>> drawn;
        bool gone() const { return count <= 0; }
    };

    BagLayout l;
    Frame base;                 ///< the bag with empty slots and the can
    BagView v;
    std::vector<Item> items;
    Reply reply = Reply::Register;
    bool registers = true;      ///< Xác Nhận on the question takes the piece
    bool sticky = false;        ///< the question will not close for Hủy Bỏ
    bool thumb = true;          ///< the scroll bar's thumb is to be seen
    /// The bag closes up the gap an item leaves, everything after it moving
    /// back one slot -- which the live client is not known to do, and which a
    /// pass must not be fooled by if it does.
    bool compacts = false;
    /// A right-click that brings up a question opens the monster book with
    /// it, as on the live client, and the book stays open, whatever the
    /// answer, until a click on its X -- one that does nothing with
    /// `book_sticks`.
    bool book_opens = true;
    bool book_sticks = false;
    Clock time;

    // The game's message after a charm's question is confirmed (test_charm_notice),
    // as on the live client: grabbed/dialog_ok, its Xác Nhận label where the
    // question's Hủy Bỏ label was, `notice_late_ms` of the pretend game's time
    // after the click -- the first one `notice_late_first_ms` when that is set
    // -- and `notice_drop` taller for a longer text, its button as much lower.
    // A click on its button closes it, but with `notice_sticks`; with
    // `notice_takes` the stack goes down only as it shows, not at the click.
    bool notice_after = false;
    bool notice_sticks = false;
    bool notice_takes = false;
    int  notice_drop = 0;
    int  notice_late_ms = 0;
    int  notice_late_first_ms = -1;
    bool msg_on = false, msg_due = false;
    std::chrono::steady_clock::time_point msg_at{};
    ScreenRect msg_pair;                ///< the pair of the question it follows
    int  msg_about = -1;
    int  messages = 0, messages_closed = 0;
    int  notice_ok_clicks = 0, notice_other_clicks = 0;
    std::vector<std::pair<int, int>> notice_click_at;
    std::vector<std::chrono::steady_clock::time_point> notice_click_t;
    std::vector<int> moves_at_notice_click;     ///< `moves` at each click on its button
    /// What reaches the game while a message is up: keys, right-clicks.
    int  keys_with_message = 0, right_clicks_with_message = 0;
    /// Where the label of grabbed/dialog_ok is in that picture.
    static ScreenRect notice_label() {
        static const ScreenRect at = [] {
            const Frame& pair = *kit().store.get("grabbed/confirm_button");
            const Frame& ok = *kit().store.get("grabbed/dialog_ok");
            Frame label;
            label.width = 34;
            label.height = 13;
            for (int y = 0; y < 13; ++y)
                for (int x = 0; x < 34; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y + 4) * pair.width + x + 4) * 4;
                    label.pixels.insert(label.pixels.end(), pair.pixels.begin() + static_cast<std::ptrdiff_t>(i),
                                        pair.pixels.begin() + static_cast<std::ptrdiff_t>(i) + 4);
                }
            const auto m = TemplateMatcher::find(ok, ScreenRect{ 0, 0, ok.width, ok.height }, label, 0.9f);
            return m.found ? m.at : ScreenRect{ 149, 57, 34, 13 };
        }();
        return at;
    }
    /// The message's label on the screen: 46 right of the question's pair, as
    /// the live client's at 561,401 is of a pair find_dialogs puts at 515,397.
    ScreenRect msg_label() const {
        return ScreenRect{ msg_pair.x + 46, msg_pair.y + 4 + notice_drop, 34, 13 };
    }
    ScreenRect msg_button() const {
        const ScreenRect label = msg_label();
        return ScreenRect{ label.x - 4, label.y - 4, 42, 21 };
    }
    ScreenRect msg_rect() const {
        const Frame& ok = *kit().store.get("grabbed/dialog_ok");
        const ScreenRect label = msg_label();
        const ScreenRect in = notice_label();
        return ScreenRect{ label.x - in.x, label.y - in.y - notice_drop, ok.width, ok.height + notice_drop };
    }
    /// grabbed/dialog_ok `drop` rows taller: a row of its words panel drawn
    /// again, the button and the frame under it moved down.
    static Frame taller(const Frame& pic, int drop) {
        if (drop <= 0) return pic;
        constexpr int kSplit = 30;
        Frame out;
        out.width = pic.width;
        out.height = pic.height + drop;
        out.pixels.resize(static_cast<std::size_t>(out.width) * out.height * 4);
        for (int y = 0; y < out.height; ++y) {
            const int from = y < kSplit ? y : y < kSplit + drop ? kSplit : y - drop;
            std::copy_n(pic.pixels.begin() + static_cast<std::ptrdiff_t>(from) * pic.width * 4, pic.width * 4,
                        out.pixels.begin() + static_cast<std::ptrdiff_t>(y) * out.width * 4);
        }
        return out;
    }

    /// The pointer: where the last click, right-click or Hands::move left it.
    /// With `lit`, a button under it is drawn lit, as the game draws one --
    /// brighter, its letters dimmed -- which the pair and the label do not
    /// match. `no_move`: hands with no Hands::move.
    int  px = -1, py = -1;
    bool lit = false;
    bool no_move = false;
    int  moves = 0;
    static void light(Frame& f, const ScreenRect& r) {
        for (int y = r.y; y < r.bottom(); ++y)
            for (int x = r.x; x < r.right(); ++x) {
                std::uint8_t red = 0, green = 0, blue = 0;
                f.get(x, y, red, green, blue);
                const int grey = (red + green + blue) / 3;
                const int d = grey < 140 ? 60 : -80;
                put_px(f, x, y, std::clamp(red + d, 0, 255), std::clamp(green + d, 0, 255),
                       std::clamp(blue + d, 0, 255));
            }
    }

    /// The bag shut, with `bag_keyed`: nothing of it drawn until a key that is
    /// not ESC -- the bag key -- opens it; the next shuts it again.
    bool bag_shut = false, bag_keyed = false;
    /// A tab of `total_rows` rows (0: no more than show), `first_row` at the
    /// top: the thumb drawn for it, the slots of the rows on show drawn and
    /// clicked by their place on the page, and a click on the scroll bar's
    /// arrows moving it a row.
    int  total_rows = 0, first_row = 0, scroll_clicks = 0;

    // The potion shop (test_shop): tabs that can be clicked, the Người Giữ Sân
    // item on Khác, the shop's window, and a quantity question that takes
    // typed digits, with the box starting at 1 as the sell box does.
    bool tabs_work = false;
    int  cur_tab = kUseTab;
    /// Clicks on a tab, with tabs_work; the next `tab_clicks_lost` of them do
    /// nothing, as a click the game let go -- and with `tabs_stuck` none does.
    int  tab_clicks = 0, tab_clicks_lost = 0;
    bool tabs_stuck = false;
    /// The strip drawn with no tab chosen -- what a look reads as -1, as the
    /// live client's did on 2026-11-03 -- on the looks `unread` says, by their
    /// number from 1; and, with `hover_unreads`, while the pointer rests on the
    /// strip, as the game lights a control under it. `unread_seen` counts the
    /// looks so drawn.
    std::function<bool(int)> unread;
    bool hover_unreads = false;
    int  unread_seen = 0;
    bool opener_opens = true;       ///< a right-click on the item opens the shop
    /// The shop opens only this many looks after the right-click (0: at once),
    /// as on the live client once, after GoBot had stopped waiting for it.
    int  open_after_looks = 0;
    int  shop_due = -1;
    bool shop_on = false;
    int  shop_sel = -1;             ///< 0 Thịt bò khô, 1 Rượu lá tre, -1 none chosen
    bool qty_up = false;
    bool qty_is_sell = false;       ///< the question is the sell question, not the buy question
    bool full_bag = false;          ///< Mua brings the message that the bag is full instead
    bool full_up = false;
    bool shop_merges = false;       ///< a purchase fills a stack that is there, never a new slot
    bool swap_columns = false;      ///< the cell clicked for HP sells MP, and the other way
    bool esc_closes_shop = true;
    bool keys_dead = false;         ///< digits typed go nowhere
    /// The box takes digits only once clicked into, as the live client's did
    /// not before (2026-10-31): a digit at it before that draws its Hủy Bỏ
    /// pressed instead, and with `click_dead` a click into it does nothing.
    bool box_needs_click = false, click_dead = false;
    bool box_focused = false, cancel_pressed = false;
    bool scan_only = false;         ///< digits are taken only as scan codes alone
    bool qty_sticky = false;        ///< Hủy Bỏ on the quantity box does not close it
    /// A double-click on a potion's cell brings the quantity question up as Mua
    /// does, its box taking keys -- unless `double_unfocused` -- where Mua's
    /// does not (with box_needs_click), as on the live client.
    bool double_unfocused = false;
    int  double_clicks = 0, mua_clicks = 0;
    void open_question(bool focused) {
        if (full_bag) {
            full_up = true;
            return;
        }
        qty_up = true;
        typed = "1";
        box_focused = focused;
        cancel_pressed = false;
        shown = qty_is_sell ? *kit().store.get(bag::kSellQuestion) : kit().buy_question;
        shown_pair = question_pair(shown);
    }
    /// The potion's cell at (x, y): 0 Thịt bò khô, 1 Rượu lá tre, -1 none.
    int cell_at(int x, int y) const {
        const ScreenRect hp{ kSX + bag::kShopHpX - 14, kSY + row_drop + bag::kShopItemY - 8, 28, 16 };
        const ScreenRect mp{ kSX + bag::kShopMpX - 14, kSY + row_drop + bag::kShopItemY - 8, 28, 16 };
        return hp.contains(x, y) ? 0 : mp.contains(x, y) ? 1 : -1;
    }
    int  box_clicks = 0, scan_keys = 0;
    int  key_drops = 0;             ///< every this-many digit keys is lost (0: none)
    std::string typed = "1";
    std::vector<int> buys;          ///< what each confirmed purchase was (0 HP, 1 MP)
    Frame shop_icon_hp, shop_icon_mp;   ///< the potions' icons, for slots a purchase fills
    int  digit_keys = 0, backspaces = 0, esc_presses = 0, thoat_clicks = 0, opener_clicks = 0;
    int kSX = 700, kSY = 20;        ///< where the shop's row is: moved over the bag by a test
    /// How far down the list the potions' row is: 42 a row, the buttons where
    /// they are -- the live shop has had two other items above the potions.
    int row_drop = 0;
    static constexpr int kFX = 420, kFY = 40;       ///< where the full-bag message is
    ScreenRect shop_buttons_rect() const {
        const Frame& b = *kit().store.get(bag::kShopButtons);
        return ScreenRect{ kSX + bag::kShopButtonsDx, kSY + bag::kShopButtonsDy, b.width, b.height };
    }
    /// The two buttons of the buy question, where it is drawn.
    ScreenRect question_pair(const Frame& pic) const {
        Frame probe = scene(1024, 768, 60);
        blit(probe, pic, kQX, kQY);
        const auto found = bag::find_dialogs(probe, kit().store);
        return found.empty() ? ScreenRect{} : found.front().pair;
    }
    /// A click while the shop is up: true when the shop took it.
    bool shop_click(int x, int y) {
        if (full_up) {
            const ScreenRect ok{ kFX + bag::kShopFullOkX - 20, kFY + bag::kShopFullOkY - 8, 40, 16 };
            if (ok.contains(x, y)) {
                full_up = false;
                ++notice_clicks;
            }
            return true;
        }
        if (qty_up) {
            const ScreenRect p = shown_pair;
            const ScreenRect box{ p.x - 88, p.y + 3, 82, 15 };
            if (box.contains(x, y)) {
                ++box_clicks;
                if (!click_dead) {
                    box_focused = true;
                    cancel_pressed = false;
                }
                return true;
            }
            if (!p.contains(x, y)) return true;
            if (x < p.x + p.w / 2) {
                if (typed == "100" && !qty_is_sell) {
                    buys.push_back(shop_sel);
                    const int kind = swap_columns ? 1 - shop_sel : shop_sel;
                    if (!shop_merges) {
                        for (int slot = 0; slot < 24; ++slot) {
                            bool used = false;
                            for (const Item& it : items) used = used || (!it.gone() && it.tab == kUseTab && it.slot == slot);
                            if (used) continue;
                            Item bought{ slot, 3, 100 };
                            bought.icon = kind == 0 ? shop_icon_hp : shop_icon_mp;
                            items.push_back(bought);
                            break;
                        }
                    }
                } else {
                    ++wrong_confirms;
                }
            } else {
                ++cancels;
                if (qty_sticky) return true;
            }
            qty_up = false;
            return true;
        }
        const ScreenRect buttons = shop_buttons_rect();
        if (buttons.contains(x, y)) {
            if (x >= buttons.x + buttons.w / 2) {
                ++thoat_clicks;
                shop_on = false;
                shop_sel = -1;
            } else {
                ++mua_clicks;
                if (shop_sel >= 0) open_question(!box_needs_click);
            }
            return true;
        }
        const int cell = cell_at(x, y);
        if (cell >= 0) shop_sel = cell;
        return true;
    }

    // What is up.
    enum class Up { None, Question, Notice, InBin, BinAsking } up = Up::None;
    int about = -1;             ///< the item the question or the bin is about
    Frame shown;                ///< the question's picture
    ScreenRect shown_pair;      ///< its two buttons, where they are on the screen
    bool book = false;          ///< the monster book is open
    bool book_without_x = false;    ///< drawn without its X, as with the mouse held down on it
    /// Whether the book was open at each right-click, in order.
    std::vector<bool> book_at_right_click;

    static constexpr int kQX = 600, kQY = 120;      ///< where questions come up
    int notice_x = kQX, notice_y = kQY;             ///< where a message with Up::Notice is
    static constexpr int kBX = 600, kBY = 330;      ///< where the bin's quantity dialog comes up
    /// Where the book's tab is drawn: clear of the bag, its X and the
    /// questions, its X where the live client has it from the tab.
    static constexpr int kTabX = 40, kTabY = 10;
    static ScreenRect book_x_rect() {
        const Frame& x = kit().book_x;
        return ScreenRect{ kTabX + bag::kBookXDx, kTabY + bag::kBookXDy, x.width, x.height };
    }

    // What was done to it.
    int right_clicks = 0, confirms = 0, cancels = 0, wrong_confirms = 0, notice_clicks = 0;
    int drags = 0, bin_confirms = 0, keys = 0, slot_clicks = 0, clicks = 0;
    int book_clicks = 0, book_closes = 0;
    std::vector<std::pair<int, int>> right_at;
    std::string events;         ///< R: a right-click, U: a registration, D: a drag, X: a deletion
    std::string seq;            ///< the same, and B where the book was closed

    ScreenRect slot_rect(int s) const { return v.slots(l)[static_cast<std::size_t>(s)]; }
    /// A question shown where questions come up, and where its two buttons
    /// are: wherever the picture has them -- the revive question's frame is
    /// not the bin's.
    void show(const Frame& pic, Up as) {
        shown = pic;
        up = as;
        shown_pair = ScreenRect{};
        if (as != Up::Question) return;
        Frame probe = scene(1024, 768, 60);
        blit(probe, pic, kQX, kQY);
        const auto found = bag::find_dialogs(probe, kit().store);
        if (!found.empty()) shown_pair = found.front().pair;
    }
    ScreenRect bin_pair() const { return ScreenRect{ kBX + 100, kBY + 50, 85, 21 }; }
    ScreenRect box_rect() const {
        const ScreenRect p = bin_pair();
        return ScreenRect{ p.x - 88, p.y + 3, 82, 15 };
    }
    /// Where item `it` is drawn, when its row is on show.
    bool on_page(const Item& it, ScreenRect& r) const {
        const int s = it.slot - first_row * l.cols;
        if (bag_shut || s < 0 || s >= l.rows * l.cols) return false;
        r = slot_rect(s);
        return true;
    }
    Item* at(int x, int y) {
        for (Item& it : items) {
            ScreenRect r;
            if (!it.gone() && (!tabs_work || it.tab == cur_tab) && on_page(it, r) && r.contains(x, y)) return &it;
        }
        return nullptr;
    }
    /// Item `i` has just gone from its slot: in a bag that closes up its
    /// gaps, everything after it moves back one.
    void left(std::size_t i) {
        if (!compacts || !items[i].gone()) return;
        for (std::size_t j = 0; j < items.size(); ++j)
            if (j != i && !items[j].gone() && items[j].slot > items[i].slot) --items[j].slot;
    }

    Frame render() const {
        const Kit& k = kit();
        Frame f = bag_shut ? scene(base.width, base.height, 60) : base;
        // The scroll bar's thumb the whole length of its track: a tab of four
        // rows at most, at its top -- what the live client shows for one.
        const int span = l.track_bottom_dy - l.track_top_dy + 1;
        if (!bag_shut && total_rows > l.rows) {
            // Shorter, for a longer tab, and as far down its dark track as the
            // rows above the view.
            const int run = static_cast<int>(std::lround(static_cast<double>(span) * l.rows / total_rows));
            const int top = static_cast<int>(
                std::lround(static_cast<double>(span - run) * first_row / (total_rows - l.rows)));
            fill(f, ScreenRect{ v.strip.x + l.scroll_up_dx, v.strip.y + l.track_top_dy, 1, span }, 70, 70, 70);
            fill(f, ScreenRect{ v.strip.x + l.scroll_up_dx, v.strip.y + l.track_top_dy + top, 1, run }, 255, 255, 255);
        } else if (!bag_shut && thumb) {
            fill(f, ScreenRect{ v.strip.x + l.scroll_up_dx, v.strip.y + l.track_top_dy, 1, span }, 255, 255, 255);
        }
        if (tabs_work) blit(f, strip_on(cur_tab), 300, 300);
        for (const Item& it : items) {
            ScreenRect s;
            if (it.gone() || (tabs_work && it.tab != cur_tab) || !on_page(it, s)) continue;
            const auto live = std::find_if(it.drawn.begin(), it.drawn.end(),
                                           [&it](const std::pair<int, Frame>& d) { return d.first == it.count; });
            if (live != it.drawn.end()) {
                blit(f, live->second, s.x, s.y);
            } else {
                if (it.icon.valid())
                    blit(f, it.icon, s.x, s.y);
                else
                    paint_item(f, s, it.seed);
                if (it.count >= 2) print_number(f, s.x + 3, s.y + 3, std::to_string(it.count), k.font_dir);
            }
            if (it.smudged) fill(f, ScreenRect{ s.x + 4, s.y + 4, 7, 6 }, 250, 250, 250);
        }
        if (up == Up::InBin || up == Up::BinAsking) paint_item(f, ScreenRect{ v.trash.x - 4, v.trash.y - 2, 24, 26 }, 6);
        if (book) {
            blit(f, k.book_tab, kTabX, kTabY);
            const ScreenRect x = book_x_rect();
            if (!book_without_x) blit(f, k.book_x, x.x, x.y);
        }
        if (shop_on) {
            blit(f, *k.store.get(bag::kShopRow), kSX, kSY + row_drop);
            const ScreenRect b = shop_buttons_rect();
            blit(f, *k.store.get(bag::kShopButtons), b.x, b.y);
        }
        if (full_up) blit(f, *k.store.get(bag::kShopFull), kFX, kFY);
        if (qty_up) {
            blit(f, shown, kQX, kQY);
            const ScreenRect p = shown_pair;
            const ScreenRect box{ p.x - 88, p.y + 3, 82, 15 };
            for (int yy = box.y; yy < box.bottom(); ++yy)
                for (int xx = box.x; xx < box.right(); ++xx)
                    put_px(f, xx, yy, 65 + (yy - box.y), 65 + (yy - box.y), 70 + (yy - box.y));
            print_number(f, box.x + 7, box.y + 6, typed, k.font_dir);
            // Hủy Bỏ drawn pressed: darker, as the game's own pressed button.
            if (cancel_pressed)
                for (int yy = p.y + 1; yy < p.bottom() - 1; ++yy)
                    for (int xx = p.x + p.w / 2 + 2; xx < p.right() - 1; ++xx) {
                        std::uint8_t r = 0, gg = 0, b = 0;
                        f.get(xx, yy, r, gg, b);
                        put_px(f, xx, yy, r * 2 / 5, gg * 2 / 5, b * 2 / 5);
                    }
        }
        // The message after a charm's question, under any dialog that comes
        // after it; its button lit with the pointer over it.
        if (msg_on) {
            const ScreenRect m = msg_rect();
            blit(f, taller(*k.store.get("grabbed/dialog_ok"), notice_drop), m.x, m.y);
            if (lit && msg_button().contains(px, py)) light(f, msg_button());
        }
        if (up == Up::Question) blit(f, shown, kQX, kQY);
        if (up == Up::Notice) blit(f, shown, notice_x, notice_y);
        if (lit && up == Up::Question && shown_pair.contains(px, py)) {
            const ScreenRect p = shown_pair;
            light(f, px < p.x + p.w / 2 ? ScreenRect{ p.x, p.y, p.w / 2, p.h }
                                        : ScreenRect{ p.x + p.w / 2, p.y, p.w - p.w / 2, p.h });
        }
        if (up == Up::BinAsking) {
            blit(f, *k.store.get("grabbed/del_item_confirm_2"), kBX, kBY);
            fill(f, ScreenRect{ kBX, kBY + 49, 196, 30 }, 216, 216, 216);
            const ScreenRect box = box_rect();
            for (int yy = box.y; yy < box.bottom(); ++yy)
                for (int xx = box.x; xx < box.right(); ++xx)
                    put_px(f, xx, yy, 65 + (yy - box.y), 65 + (yy - box.y), 70 + (yy - box.y));
            const Item& it = items[static_cast<std::size_t>(about)];
            print_number(f, box.x + 7, box.y + 6, std::to_string(it.count), k.font_dir);
            blit(f, *k.store.get("grabbed/confirm_button"), bin_pair().x, bin_pair().y);
        }
        return f;
    }

    /// Every this-many looks comes back black but for the corners Windows
    /// rounds, as the live client's did now and then (0: none); and from look
    /// `black_from` on every one does (-1: never).
    int black_every = 0;
    int black_from = -1;
    int looks = 0, black_looks = 0;
    Frame look() {
        time.t += std::chrono::milliseconds(40);
        ++looks;
        if (shop_due >= 0 && looks >= shop_due) {
            shop_on = true;
            shop_due = -1;
        }
        if (msg_due && time.t >= msg_at) {
            msg_due = false;
            msg_on = true;
            ++messages;
            if (notice_takes && msg_about >= 0) {
                --items[static_cast<std::size_t>(msg_about)].count;
                left(static_cast<std::size_t>(msg_about));
            }
        }
        if ((black_every > 0 && looks % black_every == 0) || (black_from >= 0 && looks >= black_from)) {
            ++black_looks;
            Frame f = scene(base.width, base.height, 0);
            for (int y = base.height - 7; y < base.height; ++y) {
                put_px(f, 0, y, 33, 32, 30);
                put_px(f, base.width - 1, y, 33, 32, 30);
            }
            return f;
        }
        Frame f = render();
        if (!bag_shut && ((unread && unread(looks)) || (hover_unreads && v.strip.contains(px, py)))) {
            blit(f, strip_on(0), v.strip.x, v.strip.y);
            ++unread_seen;
        }
        return f;
    }

    BagCleaner::Hands hands() {
        BagCleaner::Hands h;
        h.right_click = [this](int x, int y) {
            ++right_clicks;
            if (msg_on) ++right_clicks_with_message;
            px = x;
            py = y;
            right_at.emplace_back(x, y);
            book_at_right_click.push_back(book);
            events += 'R';
            seq += 'R';
            Item* it = at(x, y);
            if (it != nullptr && it->opener) {
                ++opener_clicks;
                if (opener_opens && !shop_on) {
                    if (open_after_looks > 0) shop_due = looks + open_after_looks;
                    else shop_on = true;
                }
                return true;
            }
            if (it == nullptr || up != Up::None) return true;
            const Kit& k = kit();
            about = static_cast<int>(it - items.data());
            switch (reply) {
                case Reply::Register: show(it->ask.valid() ? it->ask : k.question, Up::Question); break;
                case Reply::BinWords: show(*k.store.get("grabbed/del_item_confirm_1"), Up::Question); break;
                case Reply::Revive:   show(*k.store.get("grabbed/hoisinh_confirm"), Up::Question); break;
                case Reply::Other:    show(k.other, Up::Question); break;
                case Reply::Charm:    show(k.other, Up::Question); break;
                case Reply::Notice:   show(*k.store.get("grabbed/dialog_ok"), Up::Notice); break;
                case Reply::Nothing:  break;
                case Reply::Consumed:
                    --it->count;
                    left(static_cast<std::size_t>(about));
                    break;
            }
            if (up == Up::Question && book_opens) book = true;
            return true;
        };
        h.click = [this](int x, int y) {
            ++clicks;
            px = x;
            py = y;
            // The scroll bar's arrows, on a tab longer than the view.
            if (total_rows > l.rows && !bag_shut) {
                const int ax = v.strip.x + l.scroll_down_dx;
                if (std::abs(x - ax) <= 3 && std::abs(y - (v.strip.y + l.scroll_down_dy)) <= 3) {
                    ++scroll_clicks;
                    first_row = std::min(first_row + 1, total_rows - l.rows);
                    return true;
                }
                if (std::abs(x - (v.strip.x + l.scroll_up_dx)) <= 3 && std::abs(y - (v.strip.y + l.scroll_up_dy)) <= 3) {
                    ++scroll_clicks;
                    first_row = std::max(first_row - 1, 0);
                    return true;
                }
            }
            // The message after a charm's question: its button closes it, but
            // with `notice_sticks`; anywhere else on it is counted. A click on
            // a question's buttons over it is the question's.
            if (msg_on && msg_rect().contains(x, y) && !(up == Up::Question && shown_pair.contains(x, y))) {
                if (msg_button().contains(x, y)) {
                    ++notice_ok_clicks;
                    notice_click_at.emplace_back(x, y);
                    notice_click_t.push_back(time.t);
                    moves_at_notice_click.push_back(moves);
                    if (!notice_sticks) {
                        msg_on = false;
                        ++messages_closed;
                        seq += 'N';
                    }
                } else {
                    ++notice_other_clicks;
                }
                return true;
            }
            if (tabs_work) {
                for (int t = 0; t < kBagTabs; ++t) {
                    int tx = 0, ty = 0;
                    v.tab_point(l, t, tx, ty);
                    if (std::abs(x - tx) <= l.tab_w / 2 && std::abs(y - ty) <= 8) {
                        ++tab_clicks;
                        if (tabs_stuck) return true;
                        if (tab_clicks_lost > 0) {
                            --tab_clicks_lost;
                            return true;
                        }
                        cur_tab = t;
                        return true;
                    }
                }
            }
            if (shop_on && shop_click(x, y)) return true;
            if (at(x, y) != nullptr) ++slot_clicks;
            // The book's X, with nothing in front of it: the book closes.
            if (book && !book_without_x && up == Up::None && book_x_rect().contains(x, y)) {
                ++book_clicks;
                if (!book_sticks) {
                    book = false;
                    ++book_closes;
                    seq += 'B';
                }
                return true;
            }
            if (up == Up::Notice) {
                const ScreenRect n{ notice_x, notice_y, shown.width, shown.height };
                if (n.contains(x, y)) ++notice_clicks;
                return true;
            }
            if (up == Up::Question) {
                const ScreenRect p = shown_pair;
                if (!p.contains(x, y)) return true;
                if (x < p.x + p.w / 2) {
                    if (reply == Reply::Register || reply == Reply::Charm) {
                        ++confirms;
                        if (registers) {
                            // With `notice_takes`, the stack goes down as the
                            // message shows instead.
                            if (!(reply == Reply::Charm && notice_after && notice_takes)) {
                                --items[static_cast<std::size_t>(about)].count;
                                left(static_cast<std::size_t>(about));
                            }
                            events += reply == Reply::Charm ? 'C' : 'U';
                            seq += reply == Reply::Charm ? 'C' : 'U';
                        }
                        // The game's message that the charm worked, a while on.
                        if (reply == Reply::Charm && notice_after) {
                            const int late = messages == 0 && notice_late_first_ms >= 0 ? notice_late_first_ms
                                                                                          : notice_late_ms;
                            msg_due = true;
                            msg_at = time.t + std::chrono::milliseconds(late);
                            msg_pair = p;
                            msg_about = about;
                        }
                    } else {
                        ++wrong_confirms;
                    }
                    up = Up::None;
                } else {
                    ++cancels;
                    if (!sticky) up = Up::None;
                }
                return true;
            }
            const ScreenRect near_bin{ v.trash.x - 4, v.trash.y - 4, v.trash.w + 8, v.trash.h + 8 };
            if (up == Up::InBin && near_bin.contains(x, y)) {
                up = Up::BinAsking;
                return true;
            }
            if (up == Up::BinAsking) {
                const ScreenRect p = bin_pair();
                if (!p.contains(x, y)) return true;
                if (x < p.x + p.w / 2) {
                    ++bin_confirms;
                    items[static_cast<std::size_t>(about)].count = 0;
                    events += 'X';
                    seq += 'X';
                    left(static_cast<std::size_t>(about));
                }
                up = Up::None;
            }
            return true;
        };
        h.drag = [this](int fx, int fy, int tx, int ty) {
            ++drags;
            events += 'D';
            seq += 'D';
            const ScreenRect near_bin{ v.trash.x - 4, v.trash.y - 4, v.trash.w + 8, v.trash.h + 8 };
            Item* it = at(fx, fy);
            if (up == Up::None && it != nullptr && near_bin.contains(tx, ty)) {
                about = static_cast<int>(it - items.data());
                up = Up::InBin;
            }
            return true;
        };
        h.double_click = [this, one = h.click](int x, int y) {
            ++double_clicks;
            const int cell = shop_on && !qty_up && !full_up ? cell_at(x, y) : -1;
            if (cell < 0) return one(x, y) && one(x, y);
            shop_sel = cell;
            open_question(!double_unfocused);
            return true;
        };
        h.scan_key = [this](std::uint32_t vk) {
            ++scan_keys;
            return key_in(vk, true);
        };
        h.key = [this](std::uint32_t vk) { return key_in(vk, false); };
        if (!no_move)
            h.move = [this](int x, int y) {
                ++moves;
                px = x;
                py = y;
                return true;
            };
        return h;
    }
    /// A key into the pretend game, as its key code or as its scan code alone.
    bool key_in(std::uint32_t vk, bool as_scan) {
        ++keys;
        if (msg_on) ++keys_with_message;
        if (bag_keyed && !qty_up && vk != VK_ESCAPE) {
            bag_shut = !bag_shut;
            return true;
        }
        if (qty_up) {
            if (vk >= '0' && vk <= '9') {
                ++digit_keys;
                if (box_needs_click && !box_focused) {
                    cancel_pressed = true;
                } else if (!keys_dead && !(scan_only && !as_scan) &&
                           !(key_drops > 0 && digit_keys % key_drops == 0) && typed.size() < 6) {
                    typed += static_cast<char>(vk);
                }
            } else if (vk == VK_BACK) {
                ++backspaces;
                if (!typed.empty()) typed.pop_back();
            }
            return true;
        }
        if (vk == VK_ESCAPE) {
            ++esc_presses;
            if (shop_on && esc_closes_shop) {
                shop_on = false;
                shop_sel = -1;
            }
        }
        return true;
    }
};

/// The pretend bag on 1024x768, the strip showing `tab` chosen, and its own
/// close button where the live client draws it.
bool make_bag(Pretend& g, int tab = kUseTab) {
    const Kit& k = kit();
    g.base = scene(1024, 768, 60);
    fill(g.base, ScreenRect{ 290, 300, 272, 220 }, 20, 22, 30);
    blit(g.base, strip_on(tab), 300, 300);
    blit(g.base, *k.store.get("inven/0/trash_can"), 300 + g.l.bin_dx, 300 + g.l.bin_dy);
    blit(g.base, k.close_x, 300 + g.l.close_dx, 300 + g.l.close_dy);
    g.v = bag::find(g.base, k.store, g.l);
    return g.v.found;
}

/// A pass on the pretend game: Tiêu Dùng only, `list` the list.
struct Pass {
    Pretend game;
    TrashList list;
    CleanSettings settings;
    std::vector<std::string> said;
    BagCleaner::Result result;
    std::string debug_dir;
    std::function<void(BagCleaner&)> during;    ///< called on every look, with the cleaner
    const CountReader* counts = nullptr;        ///< the game's digits unless set
    BagCleaner::Mode mode = BagCleaner::Mode::Pass;
    std::function<bool(const std::string&)> due;    ///< which charms are due; every one if unset
    std::function<bool()> guard;                    ///< the hold still on; unset: not asked
    std::function<void(const std::string&)> claim;  ///< called with a charm's id before it is right-clicked
    int deadline_ms = 0;
    ScreenRect own_notice;                          ///< handed to set_own_notice when set

    Pass() {
        settings.tabs[0] = settings.tabs[1] = settings.tabs[3] = settings.tabs[4] = false;
        settings.tabs[kUseTab] = true;
        // What these tests try is the pass with registering switched on;
        // off, as it ships, is tried on its own (test_switch).
        settings.use_fragments = true;
        // The charms' own switch, off as it ships, is tried on its own
        // (test_charm_switch); a pass leaves charms alone either way.
        settings.use_charms = true;
    }
    /// A charm in slot `slot` with `count` in its stack, on the Dùng list: a
    /// picture drawn like a charm icon, named for the clock by `file`.
    ///
    /// Its colour as building the list tells it (TrashList::add_rivals): the
    /// pretend catalog's icon of a charm id of its own -- one for each seed,
    /// unless `kin` names one -- is its own drawing, so judge tells its slot's
    /// colour as the client's own icons would be told.
    void charm(int slot, int seed, int count, const std::string& file = "", std::vector<std::string> kin = {}) {
        if (kin.empty()) kin = { "spend/buff/pretend_" + std::to_string(seed) };
        const std::string colour = kin.front();
        item(slot, seed, count, TrashList::Action::Use, false, true, std::move(kin));
        TrashList::Item& it = list.items.back();
        it.file = file.empty() ? "charm_" + std::to_string(slot) : file;
        it.colour_of = colour;
        const bool known = std::any_of(list.rivals.begin(), list.rivals.end(),
                                       [&](const TrashList::Item& r) { return r.id == colour; });
        if (!known) {
            TrashList::Item icon;
            icon.id = colour;
            icon.colour_of = colour;
            icon.picture = it.picture;
            icon.shown = it.shown;
            list.rivals.push_back(icon);
        }
    }
    /// The packets' bag on a charm's slot, as GoBot's window answers it
    /// (BagCleaner::set_charm_packets); unset: not asked.
    std::function<BagCleaner::PacketSays(const std::string&, int, int, std::string&)> packets;
    /// An item in slot `slot` with `count` in its stack, on the list for
    /// `action`. Its picture is taken to be drawn like a fragment icon, as
    /// add_rivals finds a fragment cut from the bag to be, unless `kin` says
    /// what else it is drawn like.
    void item(int slot, int seed, int count, TrashList::Action action, bool then_delete = false, bool listed = true,
              std::vector<std::string> kin = { "spend/fragment/1103063" }) {
        game.items.push_back(Pretend::Item{ slot, seed, count });
        if (!listed) return;
        Frame f = game.render();
        list.add_picture(bag::cut(f, game.slot_rect(slot)), action, game.l);
        list.items.back().then_delete = then_delete;
        list.items.back().kin = std::move(kin);
    }
    BagCleaner::Result run(bool look_only = false, const TemplateStore* store = nullptr) {
        InputManager no_window;
        BagCleaner* me = nullptr;
        BagCleaner cleaner(no_window, store ? *store : kit().store, [&](Frame& out) {
            if (during && me) during(*me);
            out = game.look();
            return true;
        });
        me = &cleaner;
        cleaner.set_settings(settings);
        cleaner.set_trash(&list);
        cleaner.set_counts(counts ? counts : &kit().counts);
        cleaner.set_dry_run(look_only);
        cleaner.set_hands(game.hands());
        cleaner.set_clock(game.time.clock());
        cleaner.set_mode(mode);
        if (due) cleaner.set_charm_due(due);
        if (guard) cleaner.set_guard(guard);
        if (claim) cleaner.set_charm_claim(claim);
        if (packets) cleaner.set_charm_packets(packets);
        if (deadline_ms > 0) cleaner.set_deadline_ms(deadline_ms);
        if (own_notice.valid()) cleaner.set_own_notice(own_notice);
        if (!debug_dir.empty()) cleaner.set_debug_dir(debug_dir);
        cleaner.on_report([this](const std::string& line) { said.push_back(line); });
        result = cleaner.run();
        return result;
    }
    bool says(const std::string& what) const {
        return std::any_of(said.begin(), said.end(), [&](const std::string& l) { return l.find(what) != std::string::npos; });
    }
    std::string last() const { return said.empty() ? std::string("(nothing said)") : said.back(); }
};

void test_pictures() {
    section("Dùng: the registration question, told by its words");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    check(!bag::can_check(k.bare, TrashList::Action::Use) && bag::can_check(k.store, TrashList::Action::Use) &&
              bag::can_check(k.bare, TrashList::Action::Delete),
          "without its picture nothing is registered; with it, it can be told from the rest");
    // A picture with no words, or its buttons elsewhere, is no use.
    {
        TemplateStore s = k.bare;
        Frame blank = k.question;
        fill(blank, kRegisterPanel, 33, 35, 41);
        s.put(bag::kRegisterDialog, blank);
        check(!bag::can_check(s, TrashList::Action::Use), "a picture with nothing in its panel of words: not used");
        Frame moved = k.question;
        fill(moved, ScreenRect{ 100, 52, 100, 30 }, 216, 216, 216);
        s.put(bag::kRegisterDialog, moved);
        check(!bag::can_check(s, TrashList::Action::Use), "nor one whose two buttons are not where a question has them");
        // The bin's own question in the registration's place: its buttons
        // are not where the registration question's are, so its words
        // would be looked for in the wrong place.
        s.put(bag::kRegisterDialog, *k.bare.get("grabbed/del_item_confirm_1"));
        check(!bag::can_check(s, TrashList::Action::Use), "nor a picture laid out as the bin's question is");
    }
    Frame f = scene(1024, 768, 60);
    blit(f, k.question, 100, 100);
    blit(f, *k.store.get("grabbed/del_item_confirm_1"), 100, 300);
    blit(f, k.other, 500, 100);
    blit(f, *k.store.get("grabbed/hoisinh_confirm"), 500, 300);
    const auto dialogs = bag::find_dialogs(f, k.store);
    int registers = 0, bins = 0;
    for (const bag::Dialog& d : dialogs) {
        registers += d.registers ? 1 : 0;
        bins += d.ours() ? 1 : 0;
        check(!(d.registers && d.ours()), "never both the bin's and the registration's");
    }
    check(dialogs.size() == 4 && registers == 1 && bins == 1,
          "four questions up: one the registration's, one the bin's, the revive question and another not -- " +
              std::to_string(dialogs.size()) + " found, " + std::to_string(registers) + " registration");
    const auto plain = bag::find_dialogs(f, k.bare);
    check(std::none_of(plain.begin(), plain.end(), [](const bag::Dialog& d) { return d.registers; }),
          "and without the picture none is taken for the registration's");
    check(bag::notice_up(scene(1024, 768, 60), k.store) == false, "no message up on an empty screen");
    Frame n = scene(1024, 768, 60);
    blit(n, *k.store.get("grabbed/dialog_ok"), 400, 300);
    check(bag::notice_up(n, k.store), "a message with only Xác Nhận is seen");

    // Which tab the strip shows chosen, which nothing is right-clicked without:
    // the live client's own strip, on Tiêu Dùng in every saved round.
    {
        Frame picture = scene(1024, 768, 60);
        blit(picture, *k.store.get("grabbed/inventory"), 300, 300);
        const BagView v = bag::find(picture, k.store, BagLayout{});
        check(v.found && v.selected_tab == 0, "the client's picture of the strip: Trang Bị 1 chosen -- " +
                                                  std::to_string(v.selected_tab));
        Frame live;
        if (load_png(std::string(kLiveBuild) + "live\\shots\\r3_B_000.png", live)) {
            const BagView w = bag::find(live, k.store, BagLayout{});
            check(w.found && w.selected_tab == kUseTab,
                  "live r3_B_000, the bag on Tiêu Dùng: Tiêu Dùng chosen -- " + std::to_string(w.selected_tab));
        } else {
            skip("no live\\shots\\r3_B_000.png here\n");
        }
    }
}

void test_registers() {
    section("Dùng: a listed fragment, registered with one right-click and its Xác Nhận (UF-A1)");
    if (skip_without_kit()) return;
    Pass p;
    if (!make_bag(p.game)) {
        check(false, "the pretend bag is found");
        return;
    }
    check(p.game.v.selected_tab == kUseTab, "the pretend strip shows Tiêu Dùng chosen");
    p.item(3, 2, 5, TrashList::Action::Use);
    const ScreenRect s = p.game.slot_rect(3);
    const auto r = p.run();
    check(r.ran && r.used == 1 && r.failed == 0 && r.trouble.empty() && p.game.items[0].count == 4,
          "used 1, and the stack of five is four -- " + p.last());
    check(p.game.right_clicks == 1 && p.game.right_at[0].first == s.cx() && p.game.right_at[0].second == s.cy(),
          "exactly one right-click, at the middle of the slot");
    check(p.game.confirms == 1 && p.game.cancels == 0 && p.game.wrong_confirms == 0,
          "exactly one Xác Nhận, on the registration question");
    check(p.game.keys == 0 && p.game.slot_clicks == 0 && p.game.drags == 0,
          "no key, no left click on the slot, no drag");
    check(p.says("đã dùng món ở Tiêu Dùng"), "and it says so: " + p.last());
    check(!r.stuck, "and nothing left held");

    // A stack of one: the slot goes empty, which counts as well.
    Pass one;
    make_bag(one.game);
    one.item(0, 5, 1, TrashList::Action::Use);
    const auto r1 = one.run();
    check(r1.used == 1 && one.game.items[0].gone() && one.game.right_clicks == 1, "a single piece: used, its slot empty");

    // Two slots of one drawing. The drawing says only which of a monster's
    // nine pieces it is, so these are most likely two monsters' pieces, each
    // with a question of its own: one is registered this pass, and the other
    // is left as it is for the next.
    Pass two;
    make_bag(two.game);
    two.item(0, 2, 5, TrashList::Action::Use);
    two.item(4, 2, 3, TrashList::Action::Use, false, false);
    const auto r2 = two.run();
    check(r2.used == 1 && two.game.right_clicks == 1 && two.game.items[1].count == 3,
          "two slots of one drawing: one right-click, used once, the other slot left as it is");
}

void test_refuses() {
    section("Dùng: anything but the registration question gets Hủy Bỏ (UF-A2)");
    if (skip_without_kit()) return;
    const struct {
        Pretend::Reply reply;
        const char* what;
    } cases[] = {
        { Pretend::Reply::BinWords, "the bin's question" },
        { Pretend::Reply::Revive, "the revive question" },
        { Pretend::Reply::Other, "a question with other words" },
    };
    for (const auto& c : cases) {
        Pass p;
        make_bag(p.game);
        p.game.reply = c.reply;
        p.item(2, 3, 4, TrashList::Action::Use);
        const auto r = p.run();
        check(p.game.cancels == 1 && p.game.confirms == 0 && p.game.wrong_confirms == 0 && r.used == 0 &&
                  p.game.items[0].count == 4 && p.game.up == Pretend::Up::None && p.game.right_clicks == 1,
              std::string(c.what) + ": Hủy Bỏ once, nothing confirmed, the item as it was -- " + p.last());
        check(r.failed == 1 && !r.stuck, std::string("  counted as failed, nothing held"));
    }
    {
        // A question that will not close for Hủy Bỏ: the pass stops, and
        // everything stays held off.
        Pass p;
        make_bag(p.game);
        p.game.reply = Pretend::Reply::Other;
        p.game.sticky = true;
        p.item(2, 3, 4, TrashList::Action::Use);
        const auto r = p.run();
        check(p.game.confirms == 0 && p.game.wrong_confirms == 0 && r.used == 0 && r.stuck && !r.trouble.empty(),
              "a question Hủy Bỏ will not close: never confirmed, the pass stops, held -- " + r.trouble);
    }
    {
        // Xác Nhận, and the stack as it was: not registered -- and a surprise,
        // so the pass stops there, the item to delete beside it left too.
        Pass p;
        make_bag(p.game);
        p.game.registers = false;
        p.item(2, 3, 4, TrashList::Action::Use);
        p.item(3, 7, 2, TrashList::Action::Delete);
        const auto r = p.run();
        check(p.game.confirms == 1 && r.used == 0 && r.failed == 1 && p.game.right_clicks == 1 &&
                  p.says("số lượng không giảm") && !r.trouble.empty() && p.game.drags == 0 &&
                  p.game.items[1].count == 2,
              "confirmed and the stack not one less: not counted as used, not tried again, the pass stopped -- " +
                  r.trouble);
    }
}

void test_nothing() {
    section("Dùng: no question at all (UF-A3)");
    if (skip_without_kit()) return;
    {
        Pass p;
        make_bag(p.game);
        p.game.reply = Pretend::Reply::Nothing;
        p.item(1, 4, 6, TrashList::Action::Use);
        p.item(5, 4, 2, TrashList::Action::Use, false, false);
        const auto r = p.run();
        check(r.used == 0 && p.game.confirms == 0 && p.game.cancels == 0 && p.game.items[0].count == 6 &&
                  p.says("không dùng được"),
              "nothing came up, the stack as it was: nothing pressed, 'không dùng được' -- " + p.last());
        check(p.game.right_clicks == 1, "and the same item not right-clicked again, in its other slot either");
    }
    {
        // Nothing came up and the stack went down anyway: taken without a
        // question -- by a shop or a storage window open beside the bag, as
        // like as not. No registration seen, so not counted as one, and the
        // pass stops before anything else is right-clicked or deleted.
        Pass p;
        make_bag(p.game);
        p.game.reply = Pretend::Reply::Consumed;
        p.item(1, 4, 6, TrashList::Action::Use, true);
        p.item(5, 4, 2, TrashList::Action::Use, true, false);
        p.item(3, 7, 2, TrashList::Action::Delete);
        const auto r = p.run();
        check(r.used == 0 && r.use_unsure == 1 && p.game.items[0].count == 5 && p.game.right_clicks == 1 &&
                  p.says("đã dùng (không hỏi)") && !r.trouble.empty() && p.game.drags == 0 &&
                  p.game.items[1].count == 2 && p.game.items[2].count == 2,
              "nothing came up and the stack went down: not counted as used, said, nothing more right-clicked or "
              "deleted -- " + p.last());
        // A single piece that went: the same.
        Pass q;
        make_bag(q.game);
        q.game.reply = Pretend::Reply::Consumed;
        q.item(1, 4, 1, TrashList::Action::Use, true);
        q.item(5, 4, 2, TrashList::Action::Use, true, false);
        const auto rq = q.run();
        check(rq.used == 0 && rq.use_unsure == 1 && q.game.items[0].gone() && q.game.drags == 0 &&
                  q.game.items[1].count == 2,
              "nothing came up and a single piece went: not used, the other slot drawn alike not deleted");
    }
}

void test_notice() {
    section("Dùng: a message with only Xác Nhận stops the pass (UF-A4)");
    if (skip_without_kit()) return;
    Pass p;
    make_bag(p.game);
    p.game.reply = Pretend::Reply::Notice;
    p.debug_dir = temp_path("use_debug");
    p.item(2, 3, 4, TrashList::Action::Use);
    p.item(4, 7, 3, TrashList::Action::Delete);
    const auto r = p.run();
    check(p.game.notice_clicks == 0 && p.game.confirms == 0 && p.game.cancels == 0 && r.used == 0,
          "no button pressed on it");
    check(!r.trouble.empty() && p.game.drags == 0 && p.game.items[1].count == 3,
          "the pass stops there, the item to delete left too -- " + r.trouble);
    check(!files_in(p.debug_dir, "*use_notice*.png").empty(), "and the frame is kept");
    // Already up before anything: nothing right-clicked.
    Pass q;
    make_bag(q.game);
    q.game.show(*kit().store.get("grabbed/dialog_ok"), Pretend::Up::Notice);
    q.item(2, 3, 4, TrashList::Action::Use);
    q.run();
    check(q.game.right_clicks == 0 && q.game.notice_clicks == 0, "a message up already: nothing right-clicked, "
                                                                 "nothing pressed on it -- " + q.last());
}

void test_order() {
    section("Dùng: on Tiêu Dùng, every registration before the first deletion (UF-A5)");
    if (skip_without_kit()) return;
    Pass p;
    make_bag(p.game);
    p.item(0, 2, 5, TrashList::Action::Use);                // registered, the rest kept
    p.item(1, 7, 3, TrashList::Action::Delete);             // deleted
    p.item(2, 9, 4, TrashList::Action::Use, true);          // registered, then the rest deleted
    const auto r = p.run();
    const std::string& e = p.game.events;
    const auto first_drag = e.find('D');
    const auto last_right = e.rfind('R');
    check(first_drag != std::string::npos && last_right != std::string::npos && last_right < first_drag,
          "every right-click before the first drag to the bin -- " + e);
    check(r.used == 2 && r.deleted == 2 && p.game.items[0].count == 4 && p.game.items[1].gone() &&
              p.game.items[2].gone(),
          "the plain one used and kept, the one to delete deleted, the one to use and delete used and then "
          "deleted -- used " + std::to_string(r.used) + ", deleted " + std::to_string(r.deleted));
    // One to use and then delete that cannot be used is not deleted either:
    // deleting it first would lose the piece for good.
    Pass q;
    make_bag(q.game);
    q.game.reply = Pretend::Reply::Nothing;
    q.item(2, 9, 4, TrashList::Action::Use, true);
    const auto rq = q.run();
    check(rq.used == 0 && rq.deleted == 0 && q.game.drags == 0 && q.game.items[0].count == 4,
          "to use and then delete, but not usable: left as it is");
}

/// The picture of slot `s` of `f` drawn a part `t` of the way toward another
/// drawing: less like the slot than a picture cut from it, which scores 1.00.
/// A quarter of the way scores 0.96 on the pretend bag, as a catalog icon
/// scores 0.947 or more against the bag's own drawing of it.
Frame drawn_toward(const Frame& f, const ScreenRect& s, const BagLayout& l, float t) {
    Frame other = f;
    paint_item(other, s, 4);
    Frame pic = bag::masked(bag::cut(f, s), l);
    const Frame away = bag::masked(bag::cut(other, s), l);
    for (std::size_t i = 0; i + 3 < pic.pixels.size() && i + 3 < away.pixels.size(); i += 4) {
        if (pic.pixels[i + 3] < TemplateMatcher::kOpaque) continue;
        for (std::size_t c = 0; c < 3; ++c) {
            const int from = pic.pixels[i + c], to = away.pixels[i + c];
            pic.pixels[i + c] = static_cast<std::uint8_t>(from + t * static_cast<float>(to - from));
        }
    }
    return pic;
}

std::string fixed3(float v) {
    char s[16];
    std::snprintf(s, sizeof s, "%.3f", static_cast<double>(v));
    return s;
}

void test_use_over_delete() {
    section("Dùng: a piece on the Dùng list is registered, never deleted whole, though an older Xoá picture of it "
            "matches better (RF-1)");
    if (skip_without_kit()) return;
    // The list in TrashList::build's order: the pictures cut from the bag
    // first -- an older one of slot `slot`, put on the delete list -- then
    // the Dùng choice made later, less like the slot than the cut, as a
    // catalog icon is.
    const auto older_cut_then_use = [](Pass& p, int slot, bool then_delete) {
        const Frame f = p.game.render();
        const ScreenRect s = p.game.slot_rect(slot);
        p.list.add_picture(bag::cut(f, s), TrashList::Action::Delete, p.game.l);
        p.list.items.back().kin = { "spend/fragment/1103063" };
        p.list.add_picture(bag::cut(f, s), TrashList::Action::Use, p.game.l);
        TrashList::Item& use = p.list.items.back();
        use.then_delete = then_delete;
        use.kin = { "spend/fragment/1103063" };
        use.picture = drawn_toward(f, s, p.game.l, 0.25f);
    };
    for (const bool then_delete : { false, true }) {
        // Beside it another drawing, on the delete list alone: still deleted.
        Pass p;
        make_bag(p.game);
        p.game.items.push_back(Pretend::Item{ 2, 9, 3 });
        older_cut_then_use(p, 2, then_delete);
        p.item(5, 7, 2, TrashList::Action::Delete);
        const TrashList::Verdict k = p.list.judge(p.game.render(), p.game.slot_rect(2), p.settings.min_score);
        check(k.item == 1 && k.passed_over == 0 && k.passed_over_score > k.score,
              "the slot judged the Dùng item (" + fixed3(k.score) + "), not the Xoá picture that matches it better (" +
                  fixed3(k.passed_over_score) + ") -- item " + std::to_string(k.item));
        const auto r = p.run();
        if (then_delete)
            check(p.game.events == "RUDXDX" && r.used == 1 && r.deleted == 2 && p.game.items[0].gone() &&
                      p.game.items[1].gone() && p.says("đã xoá phần còn lại (2)"),
                  "'Dùng, rồi bỏ phần còn lại': registered first, then the rest of that stack deleted, and the other "
                  "drawing -- " + p.game.events + " -- " + p.last());
        else
            check(p.game.events == "RUDX" && r.used == 1 && r.deleted == 1 && p.game.items[0].count == 2 &&
                      p.game.items[1].gone(),
                  "'Dùng': registered, the rest of the stack kept, only the other drawing deleted -- " +
                      p.game.events + " -- " + p.last());
    }
    {
        // As P2-11 ships, without the registration question's picture: not
        // right-clicked -- and not deleted either. The older Xoá picture of it
        // gives way all the same, and the pass says so.
        Pass p;
        make_bag(p.game);
        p.game.items.push_back(Pretend::Item{ 2, 9, 3 });
        older_cut_then_use(p, 2, false);
        const auto r = p.run(false, &kit().bare);
        check(p.game.right_clicks == 0 && p.game.drags == 0 && r.deleted == 0 && p.game.items[0].count == 3 &&
                  p.says("nhưng sẽ không dùng: GoBot chưa có hình hộp thoại") &&
                  p.says("cũng giống hình đã chụp trong danh sách xoá") && p.says("danh sách Dùng được ưu tiên"),
              "no picture of the question: neither right-clicked nor deleted, and why -- " + p.last());
    }
    {
        // Thử tìm: said as one to use, and the Xoá picture said to give way.
        Pass p;
        make_bag(p.game);
        p.game.items.push_back(Pretend::Item{ 2, 9, 3 });
        older_cut_then_use(p, 2, true);
        const auto r = p.run(true);
        check(r.use_matched == 1 && r.matched == 0 && p.game.right_clicks == 0 && p.game.drags == 0 &&
                  p.says("thấy đồ sẽ dùng ở Tiêu Dùng") && p.says("danh sách Dùng được ưu tiên"),
              "Thử tìm: 'sẽ dùng', and the Xoá picture said to give way -- " + p.last());
    }
    {
        // Something not on the list and drawn otherwise, within kRivalMargin
        // of the Dùng item though not of the cut: the Dùng item is in doubt,
        // and the slot is left alone -- the Xoá picture is not fallen back on.
        Pass p;
        make_bag(p.game);
        p.game.items.push_back(Pretend::Item{ 2, 9, 3 });
        older_cut_then_use(p, 2, false);
        const Frame f = p.game.render();
        TrashList look_alike;
        look_alike.add_picture(bag::cut(f, p.game.slot_rect(2)), TrashList::Action::Delete, p.game.l);
        TrashList::Item rival = look_alike.items.back();
        rival.id = "spend/hp/8820131";
        rival.picture = drawn_toward(f, p.game.slot_rect(2), p.game.l, 0.32f);
        p.list.rivals.push_back(rival);
        const TrashList::Verdict k = p.list.judge(f, p.game.slot_rect(2), p.settings.min_score);
        const auto r = p.run();
        check(k.item < 0 && k.like == 1 && k.doubt == TrashList::Verdict::Doubt::Rival &&
                  k.rival_score < 1.0f - TrashList::kRivalMargin && p.game.right_clicks == 0 &&
                  p.game.drags == 0 && r.deleted == 0 && p.game.items[0].count == 3,
              "a look-alike as near the Dùng item (" + fixed3(k.like_score) + " and " + fixed3(k.rival_score) +
                  "), though not as near the Xoá picture: left alone, never deleted for that -- " + p.last());
    }

    // The client's own icon, as the tab puts it on the Dùng list from the
    // catalog, and the list read back by TrashList::build from a folder that
    // holds the older Xoá cut of the very slot.
    ItemCatalog catalog;
    const std::string id = "spend/fragment/1103063";
    if (!catalog.load(ItemCatalog::default_dir()) || catalog.find(id) == nullptr) {
        skip("the item catalog is not installed here\n");
        return;
    }
    for (const bool then_delete : { false, true }) {
        Pass p;
        make_bag(p.game);
        p.game.items.push_back(Pretend::Item{ 2, 9, 3 });
        p.game.items.back().icon = catalog.find(id)->tile;
        const std::string dir = temp_path(then_delete ? "rf1_trash_rest" : "rf1_trash_use");
        TrashList cutter;
        const std::string file = cutter.add(dir, bag::cut(p.game.render(), p.game.slot_rect(2)),
                                            TrashList::Action::Delete, p.game.l);
        CleanSettings chosen;
        chosen.use_ids = { id };
        if (then_delete) chosen.delete_ids = { id };
        p.list.build(dir, chosen, catalog);
        const bool cut_first = !file.empty() && !p.list.items.empty() && p.list.items.front().file == file &&
                               p.list.items.front().action == TrashList::Action::Delete;
        const TrashList::Verdict k = p.list.judge(p.game.render(), p.game.slot_rect(2), p.settings.min_score);
        const TrashList::Item* won = k.item >= 0 ? &p.list.items[static_cast<std::size_t>(k.item)] : nullptr;
        check(cut_first && won != nullptr && won->action == TrashList::Action::Use && won->fragment() &&
                  won->then_delete == then_delete && k.passed_over == 0,
              std::string(then_delete ? "'Dùng, rồi bỏ phần còn lại'" : "'Dùng'") +
                  " from the catalog, the older Xoá cut first on the list: the slot judged " +
                  (won ? won->id : std::string("nothing")) + " (" + fixed3(k.score) + "), not the cut (" +
                  fixed3(k.passed_over_score) + ")");
        const auto yielding = p.list.yielding_to_use();
        check(yielding.size() == 1 && yielding[0] == 0, "  and the tab is told the Xoá cut gives way");
        const auto r = p.run();
        check(p.game.events == (then_delete ? "RUDX" : "RU") && r.used == 1 &&
                  (then_delete ? p.game.items[0].gone() : p.game.items[0].count == 2),
              std::string("  right-clicked and registered") + (then_delete ? ", then the rest deleted" : ", kept") +
                  ", never dragged to the bin first -- " + p.game.events + " -- " + p.last());
        DeleteFileA(file.c_str());
        RemoveDirectoryA((dir + "\\delete").c_str());
        RemoveDirectoryA(dir.c_str());
    }
    // A Xoá cut of another drawing does not give way to it.
    {
        TrashList list;
        CleanSettings chosen;
        chosen.use_ids = { id };
        Pretend g;
        make_bag(g);
        g.items.push_back(Pretend::Item{ 2, 7, 3 });
        list.add_picture(bag::cut(g.render(), g.slot_rect(2)), TrashList::Action::Delete, g.l);
        list.add_catalog(catalog, chosen.use_ids, TrashList::Action::Use, chosen.catalog_score);
        list.add_rivals(catalog);
        check(list.yielding_to_use().empty(), "a Xoá cut of another drawing is not said to give way");
    }
}

void test_look_only() {
    section("Dùng: Thử tìm says what it would use, and right-clicks nothing (UF-A6, UF-A7)");
    if (skip_without_kit()) return;
    {
        Pass p;
        make_bag(p.game);
        p.item(1, 4, 6, TrashList::Action::Use);
        p.item(3, 2, 2, TrashList::Action::Use, true);
        const auto r = p.run(true);
        check(r.use_matched == 2 && p.game.right_clicks == 0 && p.game.drags == 0 && p.game.confirms == 0 &&
                  p.game.cancels == 0 && p.game.slot_clicks == 0 && p.says("thấy đồ sẽ dùng ở Tiêu Dùng") &&
                  p.says("rồi xoá phần còn lại"),
              "look only: 'thấy đồ sẽ dùng' twice, no right-click, no drag, no click on a slot or a dialog -- " +
                  p.last());
    }
    {
        // Without the registration question's picture: said, never right-clicked.
        Pass p;
        make_bag(p.game);
        p.item(1, 4, 6, TrashList::Action::Use);
        const auto r = p.run(false, &kit().bare);
        check(r.used == 0 && p.game.right_clicks == 0 &&
                  p.says("nhưng sẽ không dùng: GoBot chưa có hình hộp thoại đăng ký mảnh ghép của game để kiểm tra"),
              "no picture of the question: 'sẽ không dùng', no right-click -- " + p.last());
    }
    {
        Pass p;
        make_bag(p.game);
        p.settings.max_uses_per_run = 2;
        p.item(0, 2, 5, TrashList::Action::Use);
        p.item(1, 4, 5, TrashList::Action::Use);
        p.item(2, 9, 5, TrashList::Action::Use);
        const auto r = p.run();
        check(r.used == 2 && p.game.right_clicks == 2 && p.says("đủ cho một lần"),
              "max_uses_per_run=2 and three to use: two used, the third left -- " + p.last());
    }
}

void test_safety() {
    section("Dùng: never off Tiêu Dùng, and a stop says no to the question");
    if (skip_without_kit()) return;
    {
        // The strip shows another tab chosen: a right-click there equips.
        Pass p;
        make_bag(p.game, 0);
        p.item(1, 4, 6, TrashList::Action::Use);
        p.run();
        check(p.game.right_clicks == 0 && (p.says("không chắc túi đồ đang ở ngăn Tiêu Dùng") ||
                                           p.result.trouble.find("không chuyển được sang ngăn Tiêu Dùng") !=
                                               std::string::npos),
              "the strip not showing Tiêu Dùng chosen: no right-click -- " + p.last() + " / " + p.result.trouble);
    }
    {
        // The bag dragged low in a small game window, its bin out of the frame:
        // not taken for holding anything, and a pass says why it cannot go.
        const Kit& k = kit();
        Frame low = scene(1024, 480, 60);
        fill(low, ScreenRect{ 290, 300, 272, 180 }, 20, 22, 30);
        blit(low, strip_on(kUseTab), 300, 300);
        const BagLayout l;
        const BagView v = bag::find(low, k.store, l);
        check(v.found && !v.bin_on_frame && bag::nothing_held(low, k.store, l),
              "a bin out of the frame: the bag found, the bin not on it, nothing taken for held");
        Pass p;
        make_bag(p.game);
        p.item(0, 3, 1, TrashList::Action::Delete);
        InputManager no_window;
        BagCleaner c(no_window, k.store, [&](Frame& out) {
            out = low;
            return true;
        });
        c.set_settings(p.settings);
        c.set_trash(&p.list);
        c.set_counts(&k.counts);
        c.set_skip_focus_check(true);
        const auto r = c.run();
        check(r.trouble.find("thùng rác của túi đồ nằm ngoài") != std::string::npos && !r.stuck && r.deleted == 0,
              "and a pass with its bin out of the window stops, saying so, holding nothing -- " + r.trouble);
    }
    {
        // HP falls under the potion's mark during a pass (the guard): it stops
        // between items, nothing dragged.
        Pass p;
        make_bag(p.game);
        p.item(0, 3, 1, TrashList::Action::Delete);
        p.item(1, 5, 1, TrashList::Action::Delete);
        p.mode = BagCleaner::Mode::Pass;
        p.guard = [] { return false; };
        const auto r = p.run();
        check(r.deleted == 0 && p.game.drags == 0 && r.trouble.find("máu xuống thấp") != std::string::npos,
              "HP under the mark during a pass: stopped between items, nothing dragged -- " + r.trouble);
    }
    {
        // A stop just after the right-click: the question that comes up is
        // answered Hủy Bỏ, never Xác Nhận, and the pass ends.
        Pass p;
        make_bag(p.game);
        p.item(1, 4, 6, TrashList::Action::Use);
        p.during = [&p](BagCleaner& c) {
            if (p.game.right_clicks == 1) c.cancel();
        };
        const auto r = p.run();
        check(p.game.confirms == 0 && p.game.cancels == 1 && p.game.up == Pretend::Up::None && r.used == 0 &&
                  p.game.items[0].count == 6,
              "stopped after the right-click: Hủy Bỏ on the question, nothing registered -- " + p.last());
    }
    {
        // Use is on Tiêu Dùng only: the same item judged on another tab is
        // said, not right-clicked.
        Pass p;
        make_bag(p.game, 3);
        p.settings.tabs[kUseTab] = false;
        p.settings.tabs[3] = true;
        p.item(1, 4, 6, TrashList::Action::Use);
        p.run();
        check(p.game.right_clicks == 0 && p.says("chỉ dùng đồ ở ngăn Tiêu Dùng"),
              "on Khác: 'chỉ dùng đồ ở ngăn Tiêu Dùng', no right-click -- " + p.last());
    }
}

void test_rest_of_one_slot() {
    section("Dùng, rồi bỏ phần còn lại: only the very stack registered loses its rest (R2B-1)");
    if (skip_without_kit()) return;
    {
        // Two slots of one drawing -- piece 6 of two monsters, say -- and the
        // player chose to use it and delete the rest. Slot 1 is registered
        // and what is left of it goes; slot 5, never registered, stays.
        Pass p;
        make_bag(p.game);
        p.item(0, 2, 4, TrashList::Action::Use, true);
        p.item(4, 2, 3, TrashList::Action::Use, true, false);
        const auto r = p.run();
        check(p.game.events == "RUDX" && r.used == 1 && r.deleted == 1 && p.game.items[0].gone() &&
                  p.game.items[1].count == 3,
              "one registration, the rest of that very stack deleted, the other slot drawn the same left -- " +
                  p.game.events);
        check(p.says("đã xoá phần còn lại (3)") && p.says("chỉ xoá phần còn lại của đúng ô vừa đăng ký"),
              "and it says what went, and why the other slot stayed -- " + p.last());
    }
    {
        // A single piece registered: nothing is left of it, and nothing else
        // drawn the same is deleted in its place.
        Pass p;
        make_bag(p.game);
        p.item(0, 2, 1, TrashList::Action::Use, true);
        p.item(4, 2, 3, TrashList::Action::Use, true, false);
        const auto r = p.run();
        check(p.game.events == "RU" && r.used == 1 && r.deleted == 0 && p.game.items[1].count == 3,
              "a single piece registered: nothing deleted, the other slot drawn the same left -- " + p.game.events);
    }
    {
        // The stack registered from holds one more by the time the delete
        // sweep comes to it -- a piece picked up meanwhile: not the stack the
        // registration left, so not deleted.
        Pass p;
        make_bag(p.game);
        p.item(0, 2, 4, TrashList::Action::Use, true);
        bool picked_up = false;
        p.during = [&](BagCleaner&) {
            if (picked_up || !p.says("đã dùng món ở")) return;
            picked_up = true;
            ++p.game.items[0].count;
        };
        const auto r = p.run();
        check(p.game.events == "RU" && r.used == 1 && r.deleted == 0 && p.game.items[0].count == 4 &&
                  p.says("số lượng không còn là 3"),
              "the stack no longer what the registration left: not deleted -- " + p.last());
    }
    {
        // The scroll bar not to be read when the delete sweep comes: which row
        // is which cannot be told, so nothing is deleted by place.
        Pass p;
        make_bag(p.game);
        p.item(0, 2, 4, TrashList::Action::Use, true);
        p.during = [&p](BagCleaner&) {
            if (p.says("đã dùng món ở")) p.game.thumb = false;
        };
        const auto r = p.run();
        check(p.game.events == "RU" && r.used == 1 && r.deleted == 0 && p.game.items[0].count == 3 &&
                  p.says("không đọc được thanh cuộn"),
              "the scroll bar unread in the delete sweep: the rest is not deleted -- " + p.last());
    }
}

void test_gaps_closed() {
    section("Dùng: a bag that closes up its gaps never gets the wrong slot deleted");
    if (skip_without_kit()) return;
    {
        // Registered from slot 2, three left. Then a plain deletion empties
        // slot 1 and everything after it moves back one: the rest is in slot
        // 2's old neighbour's place, and slot 2 holds another monster's piece
        // drawn the same, its stack the same. Neither is deleted: the slot
        // the deletion emptied was not seen empty.
        Pass p;
        make_bag(p.game);
        p.game.compacts = true;
        p.item(0, 7, 1, TrashList::Action::Delete);
        p.item(1, 2, 4, TrashList::Action::Use, true);
        p.item(2, 2, 3, TrashList::Action::Use, true, false);
        const auto r = p.run();
        check(p.game.items[0].gone() && p.game.items[1].count == 3 && p.game.items[2].count == 3 &&
                  r.used == 1 && r.deleted == 1,
              "the piece that moved into the registered slot is not deleted, nor the rest that moved away -- " +
                  p.game.events);
        check(p.says("túi đồ có thể đã tự xếp lại"), "and it says why -- " + p.last());
    }
    {
        // A single piece registered, and the next piece drawn the same moves
        // into its slot: just what the stack as it was would look like. Not
        // taken for a registration; the pass stops with nothing deleted.
        Pass p;
        make_bag(p.game);
        p.game.compacts = true;
        p.item(0, 2, 1, TrashList::Action::Use, true);
        p.item(1, 2, 1, TrashList::Action::Use, true, false);
        p.item(3, 7, 2, TrashList::Action::Delete);
        const auto r = p.run();
        check(p.game.events == "RU" && r.used == 0 && !r.trouble.empty() && p.game.drags == 0 &&
                  p.game.items[1].count == 1,
              "the next piece in the registered one's place: not counted, the pass stopped, nothing deleted -- " +
                  r.trouble);
    }
    {
        // Two stacks of the item to delete, then a piece registered from
        // slot 3 with one left, then another monster's piece drawn the same,
        // one in it too, never right-clicked. The first stack goes, the
        // second moves into its slot, and the deletion reads as failed --
        // "still there". Then the rest of the registered piece is in slot 2
        // and the other monster's piece in slot 3, its place: neither is
        // deleted, as after a deletion that reads as done (I-F3).
        Pass p;
        make_bag(p.game);
        p.game.compacts = true;
        p.item(0, 7, 1, TrashList::Action::Delete);
        p.item(1, 7, 1, TrashList::Action::Delete, false, false);
        p.item(2, 2, 2, TrashList::Action::Use, true);
        p.item(3, 2, 1, TrashList::Action::Use, true, false);
        const auto r = p.run();
        check(r.used == 1 && p.game.right_clicks == 1 && p.game.items[2].count == 1 && !p.game.items[3].gone() &&
                  p.game.items[3].count == 1 && p.says("túi đồ có thể đã tự xếp lại"),
              "a deletion that reads as failed in such a bag: the piece never right-clicked is not deleted, nor "
              "the rest that moved -- " + p.game.events + " -- " + p.last());
    }
}

void test_unproven() {
    section("Dùng: a registration counts only when the slot is seen to come out of it (R2B-2)");
    if (skip_without_kit()) return;
    {
        // Nothing registered, and the count misread after Xác Nhận as one
        // less -- the very number a registration leaves -- on every other
        // look. Two looks have to agree: not taken for one, nothing deleted.
        Pass p;
        make_bag(p.game);
        p.game.registers = false;
        p.item(2, 9, 5, TrashList::Action::Use, true);
        int looks = 0;
        p.during = [&](BagCleaner&) {
            if (p.game.confirms == 0) return;
            p.game.items[0].count = ++looks % 2 == 1 ? 4 : 5;
        };
        const auto r = p.run();
        check(r.used == 0 && r.use_unsure == 1 && p.game.drags == 0 && p.game.bin_confirms == 0 &&
                  !r.trouble.empty() && p.says("hai lần xem lại ô đồ không giống nhau"),
              "a count misread once as one less: not a registration, the pass stopped, nothing deleted -- " +
                  r.trouble);
    }
    {
        // Another drawing in the slot after Xác Nhận.
        Pass p;
        make_bag(p.game);
        p.item(2, 9, 5, TrashList::Action::Use, true);
        p.item(4, 9, 5, TrashList::Action::Use, true, false);
        p.during = [&](BagCleaner&) {
            if (p.game.confirms > 0) p.game.items[0].seed = 4;
        };
        const auto r = p.run();
        check(r.used == 0 && r.use_unsure == 1 && p.game.drags == 0 && p.game.items[1].count == 5 &&
                  p.says("hình khác"),
              "the slot drawn otherwise after Xác Nhận: not a registration, nothing deleted -- " + r.trouble);
    }
    {
        // Another question comes up while the slot is being looked at again
        // -- the third look after Xác Nhận: the question gone on the first,
        // nothing up on the second. Nobody here asked it: Hủy Bỏ, as on any
        // look before, and with no telling whether the piece was registered,
        // the pass stops and nothing is deleted.
        Pass p;
        make_bag(p.game);
        p.item(2, 9, 5, TrashList::Action::Use, true);
        int looks = 0;
        p.during = [&](BagCleaner&) {
            if (p.game.confirms > 0 && ++looks == 3) p.game.show(kit().other, Pretend::Up::Question);
        };
        const auto r = p.run();
        check(r.used == 0 && r.use_unsure == 1 && p.game.cancels == 1 && p.game.up == Pretend::Up::None &&
                  !r.stuck && p.game.drags == 0 && p.says("hỏi thêm một câu khác"),
              "a question up while the slot is looked at again: Hủy Bỏ, not a registration, nothing deleted -- " +
                  r.trouble);
    }
    {
        // The same question already up on the very first look after the
        // registration question closed -- the second look after Xác Nhận. The
        // piece was registered, as the game took it, and no look says so: the
        // pass stops there as well, with nothing more right-clicked or
        // deleted -- not counted as failed and gone on from (I-F1).
        Pass p;
        make_bag(p.game);
        p.item(0, 2, 3, TrashList::Action::Use);
        p.item(1, 4, 3, TrashList::Action::Use);
        p.item(2, 7, 2, TrashList::Action::Delete);
        int looks = 0;
        p.during = [&](BagCleaner&) {
            if (p.game.confirms == 1 && ++looks == 2) p.game.show(kit().other, Pretend::Up::Question);
        };
        const auto r = p.run();
        check(r.used == 0 && r.use_unsure == 1 && r.failed == 0 && p.game.right_clicks == 1 &&
                  p.game.cancels == 1 && p.game.up == Pretend::Up::None && !r.stuck && p.game.drags == 0 &&
                  p.game.items[1].count == 3 && p.game.items[2].count == 2 && p.says("không biết đã đăng ký"),
              "a question up on the first look after the registration question closed: Hủy Bỏ, counted unsure, "
              "the pass stopped, nothing more right-clicked or deleted -- " + p.game.events + " -- " + r.trouble);
    }
    {
        // The scroll bar not to be read after Xác Nhận: the page cannot be
        // told, and with it which slot is which.
        Pass p;
        make_bag(p.game);
        p.item(2, 9, 5, TrashList::Action::Use, true);
        p.during = [&](BagCleaner&) {
            if (p.game.confirms > 0) p.game.thumb = false;
        };
        const auto r = p.run();
        check(r.used == 0 && r.use_unsure == 1 && p.game.drags == 0 && p.says("thanh cuộn"),
              "the page not to be told after Xác Nhận: not a registration, nothing deleted -- " + r.trouble);
        // And with the scroll bar not to be read before, nothing is
        // right-clicked: what came of it could not be told.
        Pass q;
        make_bag(q.game);
        q.game.thumb = false;
        q.item(2, 9, 5, TrashList::Action::Use, true);
        const auto rq = q.run();
        check(q.game.right_clicks == 0 && rq.used == 0 && q.game.drags == 0 &&
                  q.says("không đọc được thanh cuộn của túi đồ, nên không dùng món nào"),
              "the scroll bar not to be read beforehand: never right-clicked -- " + q.last());
    }
    {
        // A count that does not read before the right-click: what a
        // registration did could not be read off it afterwards, so it is
        // never right-clicked.
        Pass p;
        make_bag(p.game);
        p.item(2, 9, 5, TrashList::Action::Use, true);
        p.game.items[0].smudged = true;
        const auto r = p.run();
        check(p.game.right_clicks == 0 && r.used == 0 && p.game.drags == 0 &&
                  p.says("không đọc được số lượng trên ô đồ"),
              "a count that does not read: never right-clicked -- " + p.last());
    }
    {
        // Without the game's digits nothing is right-clicked at all.
        CountReader none;
        Pass p;
        make_bag(p.game);
        p.counts = &none;
        p.item(2, 9, 5, TrashList::Action::Use);
        p.run();
        check(p.game.right_clicks == 0 && p.says("không có bộ chữ số"),
              "no digits: no right-click -- " + p.last());
    }
}

void test_late_question() {
    section("Dùng: a question that comes up only after the right-click was given up on (I-F2, UF-R7)");
    if (skip_without_kit()) return;
    // Piece A's right-click brings nothing up while use_one watches -- the
    // game lagging -- and its question comes up later: on the first look
    // after use_one gave up, on the second, or only once the delete sweep has
    // begun. Piece B, another drawing, would be right-clicked next, and an
    // item to delete is beside them.
    const struct {
        int at;                 ///< the look after "không thấy gì" it comes up on
        const char* what;
    } cases[] = {
        { 1, "on the first look after the right-click was given up on" },
        { 2, "on the second, where the next piece would have been right-clicked" },
    };
    for (const auto& c : cases) {
        Pass p;
        make_bag(p.game);
        p.game.reply = Pretend::Reply::Nothing;
        p.item(1, 4, 3, TrashList::Action::Use);
        p.item(3, 6, 3, TrashList::Action::Use);
        p.item(5, 7, 2, TrashList::Action::Delete);
        int looks = 0;
        p.during = [&](BagCleaner&) {
            if (!p.says("bấm chuột phải mà không thấy gì") || ++looks != c.at) return;
            p.game.about = 0;
            p.game.show(kit().question, Pretend::Up::Question);
        };
        const auto r = p.run();
        check(p.game.cancels == 1 && p.game.confirms == 0 && p.game.right_clicks == 1 &&
                  p.game.up == Pretend::Up::None && p.game.items[0].count == 3 && p.game.items[1].count == 3 &&
                  p.game.drags == 0 && !r.stuck && r.trouble.find("hiện ra muộn") != std::string::npos,
              std::string("A's question up ") + c.what + ": Hủy Bỏ once, never taken for B's, nothing more "
              "right-clicked or deleted, the pass stopped -- " + p.game.events + " -- " + r.trouble);
    }
    {
        // Up only once the delete sweep has begun, which answers none but the
        // bin's dialogs: left as it is, nothing pressed on it -- and the pass
        // ends with everything held off until a look shows it gone.
        Pass p;
        make_bag(p.game);
        p.game.reply = Pretend::Reply::Nothing;
        p.item(1, 4, 3, TrashList::Action::Use);
        p.item(5, 7, 2, TrashList::Action::Delete);
        int looks = 0;
        p.during = [&](BagCleaner&) {
            if (!p.says("bấm chuột phải mà không thấy gì") || ++looks != 4) return;
            p.game.about = 0;
            p.game.show(kit().question, Pretend::Up::Question);
        };
        const auto r = p.run();
        check(p.game.cancels == 0 && p.game.confirms == 0 && p.game.up == Pretend::Up::Question &&
                  p.game.drags == 0 && p.game.items[0].count == 3 && r.stuck && r.keeps_hold(false),
              "A's question up once the delete sweep has begun: nothing pressed on it, nothing deleted, and "
              "everything held off -- " + p.game.events + " -- " + r.trouble);
    }
    {
        // Nothing comes up at all: as before, the pass goes on to the next
        // piece and to the delete sweep, and nothing is held.
        Pass p;
        make_bag(p.game);
        p.game.reply = Pretend::Reply::Nothing;
        p.item(1, 4, 3, TrashList::Action::Use);
        p.item(5, 7, 2, TrashList::Action::Delete);
        const auto r = p.run();
        check(p.game.cancels == 0 && p.game.right_clicks == 1 && p.game.items[1].gone() && !r.stuck &&
                  r.trouble.empty(),
              "and nothing coming up at all: the pass goes on, the item to delete deleted, nothing held -- " +
                  p.game.events);
    }
}

void test_other_window() {
    section("Dùng: nothing right-clicked with another window open beside the bag (S-RS2-1)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    {
        // Kho Cash, say, open beside the bag: its close button beside it.
        // Nothing is right-clicked; the delete sweep goes on as before.
        Pass p;
        make_bag(p.game);
        blit(p.game.base, k.close_x, 760, 330);
        p.item(1, 4, 6, TrashList::Action::Use);
        p.item(3, 7, 2, TrashList::Action::Delete);
        const auto r = p.run();
        check(p.game.right_clicks == 0 && r.used == 0 && p.game.items[0].count == 6 && p.game.items[1].gone() &&
                  p.says("đang có cửa sổ khác mở cạnh túi đồ"),
              "another window's close button beside the bag: no right-click, the delete sweep as before -- " +
                  p.last());
    }
    {
        // The bag's own close button alone, where it always is: a pass as
        // every other here, the use sweep and all.
        Pass p;
        make_bag(p.game);
        p.item(1, 4, 6, TrashList::Action::Use);
        const auto r = p.run();
        check(p.game.right_clicks == 1 && r.used == 1, "the bag's own close button alone: right-clicked as usual");
    }
    {
        // Without the close button's pictures nothing can be ruled out, and
        // nothing is right-clicked.
        TemplateStore s;
        for (const std::string& name : k.store.names()) {
            bool close = false;
            for (const char* x : bag::kCloseButtons) close = close || name == x;
            if (!close) s.put(name, *k.store.get(name));
        }
        Pass p;
        make_bag(p.game);
        p.item(1, 4, 6, TrashList::Action::Use);
        const auto r = p.run(false, &s);
        check(p.game.right_clicks == 0 && r.used == 0 && p.says("không có hình nút đóng cửa sổ"),
              "no pictures of the close button: no right-click -- " + p.last());
    }
    // On the live client: Kho Cash open beside the bag on Tiêu Dùng, where
    // every other check of a use sweep passes, and the bag alone.
    const struct {
        const char* file;       ///< under kLiveBuild
        bool other;
        const char* what;
    } frames[] = {
        { "live\\shots\\r3_D_015.png", true, "live r3_D_015, Kho Cash open beside Tiêu Dùng" },
        { "live\\shots\\r3_B_000.png", false, "live r3_B_000, the bag alone" },
        { "live\\shots\\r1_A_014.png", false, "live r1_A_014, the bag alone" },
    };
    for (const auto& fr : frames) {
        Frame live;
        if (!load_png(std::string(kLiveBuild) + fr.file, live)) {
            skip("no %s here\n", fr.file);
            continue;
        }
        const BagLayout l;
        const BagView v = bag::find(live, k.store, l);
        ScreenRect at;
        const bool other = v.found && bag::other_window_up(live, k.store, v, l, &at);
        check(v.found && other == fr.other,
              std::string(fr.what) + ": " + (other ? "another window's close button at " + std::to_string(at.x) +
                                                         "," + std::to_string(at.y)
                                                   : std::string("only the bag's own close button")));
    }
}

void test_named_questions() {
    section("Dùng: a question that names the piece, and the name see-through in its picture (R2B-6)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    // The live question names no piece; one that did -- as a later client
    // might -- is drawn in its box, a name in its words.
    const Frame& frame = k.question;
    const Frame a = named_question(frame, 1), b = named_question(frame, 2);
    {
        // The picture cut from piece A's question, name and all: only A's
        // question is taken for the registration's, and B's gets Hủy Bỏ.
        TemplateStore s = k.bare;
        s.put(bag::kRegisterDialog, a);
        check(bag::can_check(s, TrashList::Action::Use), "a picture of one piece's question, name and all, is used");
        Pass p;
        make_bag(p.game);
        p.item(0, 2, 2, TrashList::Action::Use);
        p.item(1, 4, 3, TrashList::Action::Use);
        p.game.items[0].ask = a;
        p.game.items[1].ask = b;
        const auto r = p.run(false, &s);
        check(r.used == 1 && p.game.items[0].count == 1 && p.game.items[1].count == 3 && p.game.cancels == 1 &&
                  p.game.confirms == 1 && r.failed == 1,
              "with it, piece A registered and piece B's question answered Hủy Bỏ -- " + p.last());
    }
    {
        // The same picture with the name see-through: both pieces' questions
        // are the registration's, and both are registered.
        TemplateStore s = k.bare;
        s.put(bag::kRegisterDialog, see_through(a, kNameArea));
        check(bag::can_check(s, TrashList::Action::Use), "the name see-through: the picture is used");
        Pass p;
        make_bag(p.game);
        p.item(0, 2, 2, TrashList::Action::Use);
        p.item(1, 4, 3, TrashList::Action::Use);
        p.game.items[0].ask = a;
        p.game.items[1].ask = b;
        const auto r = p.run(false, &s);
        check(r.used == 2 && p.game.items[0].count == 1 && p.game.items[1].count == 2 && p.game.cancels == 0,
              "with it, both pieces registered -- " + p.last());
        // And still nothing else is.
        const struct {
            Pretend::Reply reply;
            const char* what;
        } others[] = { { Pretend::Reply::BinWords, "the bin's question" },
                       { Pretend::Reply::Revive, "the revive question" },
                       { Pretend::Reply::Other, "a question with other words" } };
        for (const auto& o : others) {
            Pass q;
            make_bag(q.game);
            q.game.reply = o.reply;
            q.item(2, 3, 4, TrashList::Action::Use);
            q.run(false, &s);
            check(q.game.cancels == 1 && q.game.confirms == 0 && q.game.wrong_confirms == 0 &&
                      q.game.items[0].count == 4,
                  std::string("  ") + o.what + ": still Hủy Bỏ");
        }
    }
    {
        // Too little left to tell a question by: not used.
        TemplateStore s = k.bare;
        s.put(bag::kRegisterDialog, see_through(a, ScreenRect{ 17, 11, 162, 41 }));
        check(!bag::can_check(s, TrashList::Action::Use), "nearly all the words see-through: not used");
    }
    {
        // Words that another question has: the registration's box with the
        // revive question's own pixels where its words are looked for, and
        // the same with the bin's -- what a cut down to "Bạn có muốn" and
        // little else could match. Neither is used.
        for (const char* name : { "grabbed/hoisinh_confirm", "grabbed/del_item_confirm_1" }) {
            TemplateStore s = k.bare;
            const Frame& other = *k.store.get(name);
            Frame mixed = k.question;
            const ScreenRect& p = kRegisterPanel;
            for (int y = p.y; y < p.bottom() && y < other.height; ++y)
                for (int x = p.x; x < p.right() && x < other.width; ++x)
                    for (int c = 0; c < 4; ++c)
                        mixed.pixels[(static_cast<std::size_t>(y) * mixed.width + x) * 4 + c] =
                            other.pixels[(static_cast<std::size_t>(y) * other.width + x) * 4 + c];
            s.put(bag::kRegisterDialog, mixed);
            check(!bag::can_check(s, TrashList::Action::Use), std::string("words ") + name + " has: not used");
        }
    }
}

void test_fragments_only() {
    section("Dùng: fragments only -- nothing else on Tiêu Dùng is right-clicked (R2B-5)");
    if (skip_without_kit()) return;
    {
        // A picture on the Dùng list drawn like a potion, or like nothing the
        // catalog has: said, never right-clicked -- a right-click drinks it,
        // opens it or reads it.
        Pass p;
        make_bag(p.game);
        p.item(1, 4, 6, TrashList::Action::Use, false, true, { "spend/hp/8820131" });
        p.item(3, 9, 2, TrashList::Action::Use, false, true, {});
        const auto r = p.run();
        check(p.game.right_clicks == 0 && r.used == 0 && p.says("không giống mảnh ghép hay bùa nào"),
              "not drawn like a fragment: never right-clicked -- " + p.last());
        Pass q;
        make_bag(q.game);
        q.item(1, 4, 6, TrashList::Action::Use, false, true, { "spend/fragment/1103013", "spend/hp/8820131" });
        q.run(true);
        check(q.game.right_clicks == 0 && q.says("nhưng sẽ không dùng: hình này không giống mảnh ghép hay bùa nào"),
              "drawn like a fragment and a potion both: Thử tìm says it will not be used -- " + q.last());
    }

    // The player's own Tiêu Dùng, on real frames: which slots a cut picture of
    // would be a fragment -- what the bag picture offers "Dùng" for.
    ItemCatalog catalog;
    if (!catalog.load(ItemCatalog::default_dir()) || catalog.find("spend/fragment/1103063") == nullptr) {
        skip("the item catalog is not installed here\n");
        return;
    }
    const struct {
        const char* file;               ///< under kLiveBuild
        std::vector<int> fragments;     ///< slots holding a fragment
        std::vector<int> others;        ///< potions, scrolls, boxes, books, bowls
        const char* what;
    } frames[] = {
        { "live\\shots\\r1_A_014.png", { 6, 7, 10 },
          { 0, 1, 2, 3, 4, 5, 8, 9, 11 }, "live r1_A_014: three fragments, a box, a bundle, potions" },
        { "frozen\\bin\\Release\\trash_debug\\20261009_003955_1_no_dialog.png",
          { 11, 13, 14 }, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 15, 16, 17, 18, 19, 20, 21, 22, 23 },
          "a saved pass's frame: fragments among scrolls, chests, books, bowls and potions" },
    };
    for (const auto& fr : frames) {
        Frame live;
        if (!load_png(std::string(kLiveBuild) + fr.file, live)) {
            skip("no %s here\n", fr.file);
            continue;
        }
        const BagLayout l;
        const BagView v = bag::find(live, kit().store, l);
        if (!v.found || v.selected_tab != kUseTab) {
            check(false, std::string(fr.what) + ": the bag on Tiêu Dùng is found");
            continue;
        }
        const auto slots = v.slots(l);
        std::string wrong;
        for (const int s : fr.fragments)
            if (static_cast<std::size_t>(s) >= slots.size() || !bag::fragment_cut(bag::cut(live, slots[s]), l, catalog))
                wrong += " " + std::to_string(s) + " (a fragment, not taken for one)";
        for (const int s : fr.others)
            if (static_cast<std::size_t>(s) < slots.size() && bag::fragment_cut(bag::cut(live, slots[s]), l, catalog))
                wrong += " " + std::to_string(s) + " (taken for a fragment)";
        check(wrong.empty(), std::string(fr.what) + ": every fragment taken for one, nothing else" +
                                 (wrong.empty() ? std::string() : " -- slots" + wrong));
    }
}

void test_save_order() {
    section("Dùng: the tab's lists written so that no read in between deletes a piece unregistered (R2B-4)");
    const std::string path = temp_path("gobot_use_order.ini");
    const std::string X = "spend/fragment/1103063", M = "spend/mana/8810101";
    const struct {
        std::vector<std::string> use_before, delete_before, use_after, delete_after;
        const char* what;
    } cases[] = {
        { {}, { M }, { X }, { M, X }, "nothing to 'use, then delete the rest'" },
        { { X }, { M, X }, {}, { M }, "'use, then delete the rest' back to nothing" },
        { { X }, { M, X }, {}, { M, X }, "'use, then delete the rest' to delete only" },
        { {}, { M, X }, { X }, { M }, "delete only to use only" },
        { { X }, { M }, {}, { M, X }, "use only to delete only" },
    };
    for (const auto& c : cases) {
        CleanSettings before;
        before.use_ids = c.use_before;
        before.delete_ids = c.delete_before;
        write_all(path, "[Trash]\nenabled=false\n");
        before.save_items(path);
        CleanSettings after = before;
        after.use_ids = c.use_after;
        after.delete_ids = c.delete_after;
        // Every read between two writes: an id that was to be used before or
        // will be after is never on the delete list alone.
        std::string bad;
        int reads = 0;
        const bool saved = after.save_items(path, [&] {
            CleanSettings seen;
            if (!seen.load(path)) {
                bad += " (unreadable)";
                return;
            }
            ++reads;
            const auto on = [](const std::vector<std::string>& ids, const std::string& id) {
                return std::find(ids.begin(), ids.end(), id) != ids.end();
            };
            for (const auto* ids : { &c.use_before, &c.use_after })
                for (const std::string& id : *ids)
                    if (on(seen.delete_ids, id) && !on(seen.use_ids, id)) bad += " " + id + " on delete= alone";
        });
        CleanSettings last;
        const bool ends_right = last.load(path) && last.use_ids == c.use_after && last.delete_ids == c.delete_after;
        check(saved && reads == 3 && bad.empty() && ends_right,
              std::string(c.what) + ": every read in between safe, and the file ends as chosen" + bad);
    }
    DeleteFileA(path.c_str());
}

void test_settings() {
    section("Dùng: [Items] use= and [Trash] max_uses_per_run in trash.ini (UF-A8)");
    const std::string path = temp_path("gobot_use_trash.ini");
    write_all(path, "[Trash]\nenabled=false\nmax_uses_per_run=7\n[Items]\ndelete=spend/mana/8810101\n"
                    "use=spend/fragment/1103013, spend/fragment/1103023\n");
    CleanSettings s;
    check(s.load(path) && s.use_ids.size() == 2 && s.use_ids[1] == "spend/fragment/1103023" &&
              s.max_uses_per_run == 7 && s.delete_ids.size() == 1,
          "the ids to use and the bound read back");
    check(IniFile::set_in_place(path, "Items", "use", "spend/fragment/1103033") && s.load(path) &&
              s.use_ids.size() == 1 && s.use_ids[0] == "spend/fragment/1103033",
          "and written back in place, as the tab writes them");
    write_all(path, "[Trash]\nmax_uses_per_run=900\n");
    CleanSettings big;
    check(big.load(path) && big.max_uses_per_run == 100 && big.use_ids.empty(), "a bound past 100 is 100");
    DeleteFileA(path.c_str());
#ifdef GOBOT_SOURCE_DIR
    CleanSettings shipped;
    const std::string text = read_all(std::string(GOBOT_SOURCE_DIR) + "/configs/trash.ini");
    check(shipped.load(std::string(GOBOT_SOURCE_DIR) + "/configs/trash.ini") && shipped.use_ids.empty() &&
              shipped.max_uses_per_run == 20 && text.find("\nuse=") == std::string::npos,
          "the shipped trash.ini uses nothing, at most 20 a pass");
    check(!shipped.use_fragments && text.find("\nuse_fragments=false\n") != std::string::npos,
          "and ships registering off: use_fragments=false");
#endif

    // The list: an id on both lists is one item, used and then deleted; a
    // non-fragment on the Dùng list is never one to use.
    ItemCatalog catalog;
    if (!catalog.load(ItemCatalog::default_dir()) || catalog.find("spend/fragment/1103013") == nullptr ||
        catalog.find("spend/hp/8820131") == nullptr) {
        skip("the item catalog is not installed here\n");
        return;
    }
    CleanSettings both;
    both.use_ids = { "spend/fragment/1103013", "spend/hp/8820131" };
    both.delete_ids = { "spend/fragment/1103013" };
    TrashList list;
    list.build(temp_path("no_trash_pictures"), both, catalog);
    int uses = 0, deletes_of_it = 0;
    bool then_delete = false, hp_used = false;
    for (const TrashList::Item& it : list.items) {
        if (it.id == "spend/fragment/1103013") {
            if (it.action == TrashList::Action::Use) {
                ++uses;
                then_delete = it.then_delete;
            } else {
                ++deletes_of_it;
            }
        }
        if (it.id == "spend/hp/8820131" && it.action == TrashList::Action::Use) hp_used = true;
        if (it.action == TrashList::Action::Use && it.id.rfind("spend/fragment/", 0) != 0 && !it.id.empty())
            hp_used = true;
    }
    check(uses == 1 && deletes_of_it == 0 && then_delete,
          "a fragment chosen to use and to delete: one Use item, deleted after it is used");
    check(!hp_used, "a potion on the Dùng list is not one to use, nor anything drawn like a fragment that is not one");
}

// ---------------------------------------------------- round 7: the live question

/// Where the cuts of the live frames are: beside this test, else the copy
/// handed over with them. Empty when neither is here.
std::string live_cuts_dir() {
    for (const std::string& d : { exe_relative("testdata\\round7"),
                                  std::string(kLiveBuild) + "handoff\\round7\\coder\\testdata" })
        if (!files_in(d, "*.png").empty()) return d;
    return {};
}

/// The cut `what` of a live frame put back where it was, on `canvas`: its
/// place is in its file name, <what>_<x>_<y>.png. False when it is not there.
bool put_back(Frame& canvas, const std::string& dir, const std::string& what, ScreenRect* where = nullptr) {
    for (const std::string& name : files_in(dir, (what + "_*.png").c_str())) {
        int x = 0, y = 0;
        // The two numbers after what_, before .png.
        if (std::sscanf(name.c_str() + what.size() + 1, "%d_%d.png", &x, &y) != 2) continue;
        Frame cut;
        if (!load_png(dir + "\\" + name, cut) || !cut.valid()) return false;
        blit(canvas, cut, x, y);
        if (where) *where = ScreenRect{ x, y, cut.width, cut.height };
        return true;
    }
    return false;
}

void test_live_question() {
    section("Dùng: the live client's registration question and monster book, on cuts of its frames");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    check(k.question.width == 204 && k.question.height == 86 && bag::can_check(k.store, TrashList::Action::Use),
          "the shipped picture, 204x86, tells the registration question from the bin's, the revive question, "
          "dropping and selling");
    const std::string dir = live_cuts_dir();
    if (dir.empty()) {
        skip("no cuts of the live frames here (testdata\\round7)\n");
        return;
    }
    // Each cut back where the live client drew it, on a frame of its size.
    const struct {
        const char* what;
        std::size_t dialogs;
        bool        registers;
        const char* said;
    } cases[] = {
        { "question_rest", 1, true, "the question, its buttons at rest (fragq_A_085)" },
        { "question_hover", 1, true, "the question, the pointer on Hủy Bỏ (fragq_A_030)" },
        { "question_book", 1, true, "the question in the second recording (book_A_144)" },
        { "question_pressed", 0, false, "the question, the mouse button down on Hủy Bỏ: no pair, no dialog "
                                        "(fragq_A_108)" },
        { "bin_question", 1, false, "the bin's own quantity dialog (r1_B_022): the bin's, never the registration's" },
        { "esc_menu", 0, false, "the ESC menu (book_A_104): no dialog" },
    };
    for (const auto& c : cases) {
        Frame f = scene(1280, 720, 40);
        if (!put_back(f, dir, c.what)) {
            skip("no %s cut here\n", c.what);
            continue;
        }
        const auto dialogs = bag::find_dialogs(f, k.store);
        std::size_t registers = 0;
        float words = 0.0f;
        for (const bag::Dialog& d : dialogs) {
            registers += d.registers ? 1 : 0;
            words = std::max(words, d.register_words);
        }
        check(dialogs.size() == c.dialogs && (registers == 1) == c.registers,
              std::string(c.said) + ": " + std::to_string(dialogs.size()) + " dialog(s), " +
                  std::to_string(registers) + " the registration's, its words " + fixed3(words));
        if (c.registers) {
            const bag::Dialog& d = dialogs.front();
            check(!d.ours() && d.pair.x == 643 && d.pair.y == 373 && words >= 0.99f,
                  std::string("  its pair at 643,373 and its words at 0.99 or more -- pair at ") +
                      std::to_string(d.pair.x) + "," + std::to_string(d.pair.y));
            // Without the picture, the same dialog is nobody's.
            const auto bare = bag::find_dialogs(f, k.bare);
            check(bare.size() == 1 && !bare.front().registers && !bare.front().ours(),
                  "  and without the picture it is no question GoBot answers");
        }
        if (std::string(c.what) == "bin_question")
            check(dialogs.size() == 1 && dialogs.front().ours() && dialogs.front().kind == bag::kCountDialog,
                  "  taken for the bin's quantity dialog, by its words");
    }

    // The book: its tab and X back where the live client drew them.
    {
        Frame f = scene(1280, 720, 40);
        const bool drawn = put_back(f, dir, "book_tab") && put_back(f, dir, "book_x_rest");
        if (!drawn) {
            skip("no cuts of the book here\n");
            return;
        }
        const bag::Book b = bag::find_book(f, k.store);
        check(b.found && b.score >= 0.99f && b.title.x == 306 && b.title.y == 156 && b.has_x && b.x.x == 807 &&
                  b.x.y == 160 && b.x_score >= 0.99f && b.window.x == 303 && b.window.y == 153,
              "the book's tab at 306,156 (" + fixed3(b.score) + ") and its X at 807,160 (" + fixed3(b.x_score) +
                  "), the window from 303,153 -- found at " + std::to_string(b.title.x) + "," +
                  std::to_string(b.title.y) + ", X at " + std::to_string(b.x.x) + "," + std::to_string(b.x.y));
        // Looked for again where it was, and not elsewhere.
        const ScreenRect elsewhere{ 100, 100, b.window.w, b.window.h };
        check(bag::find_book(f, k.store, &b.window).found && !bag::find_book(f, k.store, &elsewhere).found,
              "looked for again round its own window: found; round another place: not");
        // The X is the book's own: no other window.
        check(!bag::other_window_up(f, k.store, BagView{}, BagLayout{}, nullptr, &b.x) &&
                  bag::other_window_up(f, k.store, BagView{}, BagLayout{}),
              "its X is no other window's when it is the book's own, and is one when nothing says so");
    }
    for (const char* x : { "book_x_hover", "book_x_pressed" }) {
        Frame f = scene(1280, 720, 40);
        if (!put_back(f, dir, "book_tab") || !put_back(f, dir, x)) continue;
        const bag::Book b = bag::find_book(f, k.store);
        const bool hover = std::string(x) == "book_x_hover";
        check(b.found && b.has_x == hover && (!hover || (b.x.x == 807 && b.x.y == 160)),
              std::string(hover ? "the pointer over its X (book_A_090): the X still found, "
                                : "the mouse button down on its X (book_A_154): the book, and no X to click, ") +
                  fixed3(b.x_score));
    }
    {
        // The question up over the book, as a right-click brings them: the
        // registration question, and no window but the book's own.
        Frame f = scene(1280, 720, 40);
        put_back(f, dir, "book_tab");
        put_back(f, dir, "book_x_rest");
        put_back(f, dir, "question_rest");
        const auto dialogs = bag::find_dialogs(f, k.store);
        const bag::Book b = bag::find_book(f, k.store);
        check(dialogs.size() == 1 && dialogs.front().registers && b.found && b.has_x &&
                  !bag::other_window_up(f, k.store, BagView{}, BagLayout{}, nullptr, &b.x),
              "the question over the book: the registration question, and the book's X the only close button");
        // The ESC menu beside them: another window.
        put_back(f, dir, "esc_menu");
        ScreenRect at;
        const bool other = bag::other_window_up(f, k.store, BagView{}, BagLayout{}, &at, &b.x);
        check(other, "and with the ESC menu up as well, another window: its X at " + std::to_string(at.x) + "," +
                         std::to_string(at.y));
    }
    {
        Frame f = scene(1280, 720, 40);
        put_back(f, dir, "esc_menu");
        check(!bag::find_book(f, k.store).found, "the ESC menu is not taken for the book");
    }
}

void test_switch() {
    section("Dùng: its own switch, [Trash] use_fragments, off as shipped -- the picture alone is no switch");
    if (skip_without_kit()) return;
    {
        CleanSettings fresh;
        check(!fresh.use_fragments, "off unless a file says otherwise");
    }
    {
        // The picture there, the Dùng list filled, the switch off: nothing
        // right-clicked, and the deletion beside it as ever.
        Pass p;
        make_bag(p.game);
        p.settings.use_fragments = false;
        p.item(0, 2, 5, TrashList::Action::Use);
        p.item(3, 7, 2, TrashList::Action::Delete);
        const auto r = p.run();
        check(bag::can_check(kit().store, TrashList::Action::Use) && p.game.right_clicks == 0 &&
                  p.game.confirms == 0 && r.used == 0 && p.game.items[0].count == 5 && p.game.items[1].gone() &&
                  !p.game.book && p.says("nhưng sẽ không dùng: Dùng đang tắt"),
              "the picture there and the switch off: no right-click, the piece as it was, the item to delete "
              "deleted -- " + p.last());
        Pass q;
        make_bag(q.game);
        q.settings.use_fragments = false;
        q.item(0, 2, 5, TrashList::Action::Use);
        const auto rq = q.run(true);
        check(rq.use_matched == 1 && q.game.right_clicks == 0 && q.says("Dùng đang tắt"),
              "Thử tìm says it would use it, and that the switch is off -- " + q.last());
    }
    {
        // Read from the file: off without the line, on with it, and off again
        // once the line is gone -- never kept from an earlier reading.
        const std::string path = temp_path("gobot_use_switch.ini");
        CleanSettings s;
        write_all(path, "[Trash]\nenabled=false\n[Items]\nuse=spend/fragment/1103013\n");
        const bool without = s.load(path) && !s.use_fragments;
        write_all(path, "[Trash]\nuse_fragments=true\n");
        const bool on = s.load(path) && s.use_fragments;
        write_all(path, "[Trash]\nenabled=true\n");
        const bool off_again = s.load(path) && !s.use_fragments;
        check(without && on && off_again,
              "trash.ini without use_fragments: off; use_fragments=true: on; the line taken out again: off");
        DeleteFileA(path.c_str());
    }
}

void test_whole_sequence() {
    section("Dùng: right-click, the question, Xác Nhận, one less, the book shut, and only then the next slot");
    if (skip_without_kit()) return;
    {
        Pass p;
        make_bag(p.game);
        p.item(0, 2, 5, TrashList::Action::Use);        // a stack of five
        p.item(3, 4, 1, TrashList::Action::Use);        // a single piece, another drawing
        p.item(5, 7, 2, TrashList::Action::Delete);     // something to delete after
        const auto r = p.run();
        check(r.used == 2 && r.deleted == 1 && p.game.items[0].count == 4 && p.game.items[1].gone() &&
                  p.game.items[2].gone() && r.trouble.empty() && !r.stuck,
              "both registered -- five to four, the single piece gone -- and the item to delete deleted -- " +
                  p.last());
        check(p.game.seq == "RUBRUBDX",
              "in order: right-click, registered, book shut; right-click, registered, book shut; then the "
              "deletion -- " + p.game.seq);
        check(p.game.confirms == 2 && p.game.cancels == 0 && p.game.book_clicks == 2 && p.game.book_closes == 2 &&
                  !p.game.book && p.game.book_at_right_click == std::vector<bool>{ false, false },
              "two Xác Nhận, two clicks on the book's X, and no right-click with the book open");
        check(p.says("đóng Yêu Quái Bách Khoa bằng nút X của nó"), "and it says it shut the book");
    }
    {
        // Each question answered Hủy Bỏ -- words GoBot has no picture of: the
        // book shut after each, as after a registration, and nothing taken.
        Pass p;
        make_bag(p.game);
        p.game.reply = Pretend::Reply::Other;
        p.item(0, 2, 5, TrashList::Action::Use);
        p.item(3, 4, 2, TrashList::Action::Use);
        const auto r = p.run();
        check(p.game.seq == "RBRB" && p.game.cancels == 2 && p.game.confirms == 0 && p.game.wrong_confirms == 0 &&
                  r.used == 0 && r.failed == 2 && p.game.items[0].count == 5 && p.game.items[1].count == 2 &&
                  !p.game.book,
              "a question that does not match: Hủy Bỏ, the book shut, the next piece; nothing registered -- " +
                  p.game.seq);
    }
}

void test_book_will_not_close() {
    section("Dùng: a book that will not close stops the pass, nothing more right-clicked or deleted");
    if (skip_without_kit()) return;
    for (const bool no_x : { false, true }) {
        Pass p;
        make_bag(p.game);
        p.debug_dir = temp_path(no_x ? "use_book_no_x" : "use_book_sticks");
        if (no_x) p.game.book_without_x = true;
        else      p.game.book_sticks = true;
        p.item(0, 2, 5, TrashList::Action::Use);
        p.item(3, 4, 2, TrashList::Action::Use);
        p.item(5, 7, 2, TrashList::Action::Delete);
        const auto r = p.run();
        const std::string want = no_x ? "không thấy nút X" : "không đóng lại khi bấm nút X";
        check(r.used == 1 && p.game.items[0].count == 4 && p.game.right_clicks == 1 && p.game.items[1].count == 2 &&
                  p.game.drags == 0 && p.game.items[2].count == 2 && p.game.book &&
                  r.trouble.find(want) != std::string::npos &&
                  p.game.book_clicks == (no_x ? 0 : 1) && !files_in(p.debug_dir, "*use_book_open*.png").empty(),
              std::string(no_x ? "the book's X not to be seen" : "a click on the book's X that does nothing") +
                  ": the piece registered stays registered, the pass stops, the frame kept, nothing more "
                  "right-clicked or deleted -- " + r.trouble);
    }
    {
        // A stop just after the registration: the book is left as it is.
        Pass p;
        make_bag(p.game);
        p.item(0, 2, 5, TrashList::Action::Use);
        p.during = [&p](BagCleaner& c) {
            if (p.game.confirms == 1) c.cancel();
        };
        p.run();
        check(p.game.book_clicks == 0 && p.game.right_clicks == 1,
              "a stop after Xác Nhận: nothing more pressed, the book's X neither");
    }
}

void test_book_or_window_up() {
    section("Dùng: the book open beforehand, or a window come up with the question");
    if (skip_without_kit()) return;
    {
        // The book open before the sweep -- the player's, or left by a pass
        // that stopped: nothing right-clicked, and it is not closed for them.
        Pass p;
        make_bag(p.game);
        p.game.book = true;
        p.item(0, 2, 5, TrashList::Action::Use);
        p.item(3, 7, 2, TrashList::Action::Delete);
        const auto r = p.run();
        check(p.game.right_clicks == 0 && p.game.book_clicks == 0 && p.game.book && p.game.items[0].count == 5 &&
                  p.game.items[1].gone() && r.used == 0 && p.says("Yêu Quái Bách Khoa đang mở"),
              "the book already open: no right-click, the book left open, the deletion as ever -- " + p.last());
    }
    {
        // A window of the game comes up with the question -- its close
        // button beside the bag: never Xác Nhận, Hủy Bỏ.
        Pass p;
        make_bag(p.game);
        p.item(0, 2, 5, TrashList::Action::Use);
        bool drawn = false;
        p.during = [&](BagCleaner&) {
            if (drawn || p.game.right_clicks != 1) return;
            drawn = true;
            blit(p.game.base, kit().close_x, 760, 330);
        };
        const auto r = p.run();
        check(p.game.confirms == 0 && p.game.cancels == 1 && r.used == 0 && p.game.items[0].count == 5 &&
                  p.says("có một cửa sổ khác đang mở"),
              "a window come up with the question: Hủy Bỏ, nothing registered -- " + p.last());
    }
}

// ------------------------------------- round 7 review: the live question edited

/// The live question's first and third lines of words, in the shipped
/// picture's own pixels: "Bạn có muốn đăng ký mảnh quái vật vào" and "ng?",
/// clear of the panel's corner ornaments.
const ScreenRect kLineOne{ 30, 12, 140, 13 };
const ScreenRect kLineThree{ 40, 42, 120, 10 };

/// The commonest colour of `r` of `pic`, as red, green, blue: a panel's
/// ground.
void ground_of(const Frame& pic, const ScreenRect& r, int& red, int& green, int& blue) {
    std::vector<std::pair<std::uint32_t, int>> seen;
    for (int y = r.y; y < r.bottom(); ++y)
        for (int x = r.x; x < r.right(); ++x) {
            const std::uint8_t* p = &pic.pixels[(static_cast<std::size_t>(y) * pic.width + x) * 4];
            const std::uint32_t c = (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | p[0];
            auto it = std::find_if(seen.begin(), seen.end(), [c](const auto& s) { return s.first == c; });
            if (it == seen.end()) seen.emplace_back(c, 1);
            else ++it->second;
        }
    const auto most = std::max_element(seen.begin(), seen.end(),
                                       [](const auto& a, const auto& b) { return a.second < b.second; });
    const std::uint32_t c = most == seen.end() ? 0 : most->first;
    red = static_cast<int>((c >> 16) & 0xFF);
    green = static_cast<int>((c >> 8) & 0xFF);
    blue = static_cast<int>(c & 0xFF);
}

/// Bright pixels -- the words' strokes -- in `r` of `pic`.
int bright_in(const Frame& pic, const ScreenRect& r) {
    int n = 0;
    for (int y = r.y; y < r.bottom(); ++y)
        for (int x = r.x; x < r.right(); ++x) {
            const std::uint8_t* p = &pic.pixels[(static_cast<std::size_t>(y) * pic.width + x) * 4];
            n += (p[0] + p[1] + p[2]) / 3 > 150 ? 1 : 0;
        }
    return n;
}

/// `r` of `from` copied, as it is, to `tx`, `ty` of `to`.
void copy_block(const Frame& from, const ScreenRect& r, Frame& to, int tx, int ty) {
    for (int y = 0; y < r.h; ++y)
        std::copy_n(&from.pixels[(static_cast<std::size_t>(r.y + y) * from.width + r.x) * 4],
                    static_cast<std::size_t>(r.w) * 4,
                    &to.pixels[(static_cast<std::size_t>(ty + y) * to.width + tx) * 4]);
}

/// The live question with its words changed in place, its frame and buttons
/// as they were: what another question in the same box would look like -- the
/// one for a piece already registered, or another book's -- each of which the
/// round 7 review found taken for the registration question at 0.90.
std::vector<std::pair<std::string, Frame>> edited_questions(const Frame& q) {
    std::vector<std::pair<std::string, Frame>> out;
    int r = 0, g = 0, b = 0;
    ground_of(q, kRegisterPanel, r, g, b);
    {
        Frame e = q;
        fill(e, kLineThree, r, g, b);
        out.emplace_back("its third line, \"ng?\", gone", e);
    }
    {
        // The eight columns of its first line with the most strokes in them,
        // blanked: a letter, or a little more.
        int best_x = kLineOne.x, best = -1;
        for (int x = kLineOne.x; x + 8 <= kLineOne.right(); ++x) {
            const int n = bright_in(q, ScreenRect{ x, kLineOne.y, 8, kLineOne.h });
            if (n > best) {
                best = n;
                best_x = x;
            }
        }
        Frame e = q;
        fill(e, ScreenRect{ best_x, kLineOne.y, 8, kLineOne.h }, r, g, b);
        out.emplace_back("a letter of its first line taken out (" + std::to_string(best) + " strokes' pixels)", e);
    }
    {
        // 24 columns of its first line over another 24 of it: a short word in
        // other letters of the same font.
        Frame e = q;
        copy_block(q, ScreenRect{ 45, kLineOne.y, 24, kLineOne.h }, e, 120, kLineOne.y);
        out.emplace_back("a short word of its first line in other letters", e);
    }
    {
        // Two pieces of its first line changing places: the same letters in
        // another order.
        Frame e = q;
        copy_block(q, ScreenRect{ 40, kLineOne.y, 16, kLineOne.h }, e, 120, kLineOne.y);
        copy_block(q, ScreenRect{ 120, kLineOne.y, 16, kLineOne.h }, e, 40, kLineOne.y);
        out.emplace_back("two pieces of its first line swapped", e);
    }
    return out;
}

void test_edited_question() {
    section("Dùng: the live question with its words changed in place gets Hủy Bỏ -- only this question, with these "
            "words (R7-2)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    {
        Frame f = scene(1024, 768, 60);
        blit(f, k.question, 100, 100);
        const auto ds = bag::find_dialogs(f, k.store);
        check(ds.size() == 1 && ds.front().registers && ds.front().register_words >= 0.99f,
              "the question as the live client drew it: the registration question, its words " +
                  fixed3(ds.empty() ? -1.0f : ds.front().register_words));
    }
    // 0.90, what the bin's pictures are taken at, and the registration's was
    // before the review.
    int would_have = 0;
    for (const auto& [what, pic] : edited_questions(k.question)) {
        Frame f = scene(1024, 768, 60);
        blit(f, pic, 100, 100);
        const auto ds = bag::find_dialogs(f, k.store);
        const bool one = ds.size() == 1;
        const float words = one ? ds.front().register_words : -1.0f;
        would_have += one && words >= 0.90f ? 1 : 0;
        check(one && !ds.front().registers && !ds.front().ours(),
              what + ": a dialog, not the registration question -- its words " + fixed3(words));
        // A pass on it: Hủy Bỏ, nothing registered, and the book the
        // right-click opened shut after it, as after any Hủy Bỏ.
        Pass p;
        make_bag(p.game);
        p.item(2, 3, 4, TrashList::Action::Use);
        p.game.items[0].ask = pic;
        const auto r = p.run();
        check(p.game.cancels == 1 && p.game.confirms == 0 && p.game.wrong_confirms == 0 && r.used == 0 &&
                  r.failed == 1 && p.game.items[0].count == 4 && p.game.seq == "RB" && !p.game.book && !r.stuck,
              "  and a pass on it: Hủy Bỏ, nothing registered, the stack of four as it was, the book shut -- " +
                  p.game.seq + " -- " + p.last());
    }
    check(would_have >= 1, std::to_string(would_have) +
                               " of the four with words at 0.90 or more, which would have been confirmed before");

    // The same on the live frame: the cut of the question at rest put back
    // where the live client drew it, its third line taken out there.
    const std::string dir = live_cuts_dir();
    Frame f = scene(1280, 720, 40);
    ScreenRect where;
    if (dir.empty() || !put_back(f, dir, "question_rest", &where)) {
        skip("no cut of the live question here (testdata\\round7)\n");
        return;
    }
    // The picture's top-left is 8,8 into the cut.
    const ScreenRect line{ where.x + 8 + kLineThree.x, where.y + 8 + kLineThree.y, kLineThree.w, kLineThree.h };
    int r = 0, g = 0, b = 0;
    ground_of(f, ScreenRect{ where.x + 8 + kRegisterPanel.x, where.y + 8 + kRegisterPanel.y, kRegisterPanel.w,
                             kRegisterPanel.h },
              r, g, b);
    const int strokes = bright_in(f, line);
    fill(f, line, r, g, b);
    const auto ds = bag::find_dialogs(f, k.store);
    check(strokes > 20 && ds.size() == 1 && !ds.front().registers && !ds.front().ours(),
          "the live cut, its third line taken out where the client drew it (" + std::to_string(strokes) +
              " strokes' pixels): a dialog, not the registration question -- its words " +
              fixed3(ds.empty() ? -1.0f : ds.front().register_words));
}

} // namespace

// ------------------------------------------------------------- charms (Bùa)

/// A frame of the pretend screen with `pic` drawn where questions come up, and
/// the dialog find_dialogs sees in it.
bag::Dialog question_on(Frame& probe, const Frame& pic) {
    probe = scene(1024, 768, 60);
    blit(probe, pic, Pretend::kQX, Pretend::kQY);
    const auto found = bag::find_dialogs(probe, kit().store);
    return found.empty() ? bag::Dialog{} : found.front();
}

void test_charm_kinds() {
    section("Bùa: what a right-click on Tiêu Dùng is allowed to be");
    check(bag::use_kind_of("spend/fragment/1103013") == UseKind::Fragment &&
              bag::use_kind_of("spend/buff/8871061") == UseKind::Charm &&
              bag::use_kind_of("spend/hp/8820131") == UseKind::None &&
              bag::use_kind_of("spend/mana/8810101") == UseKind::None &&
              bag::use_kind_of("spend/craft/8842001") == UseKind::None && bag::use_kind_of("seal/1") == UseKind::None &&
              bag::use_kind_of("other/1") == UseKind::None && bag::use_kind_of("") == UseKind::None,
          "fragments and charms are the only icons a pass may right-click");
    TrashList::Item item;
    item.kin = { "spend/buff/8871051", "spend/buff/8871061" };
    check(item.use_kind() == UseKind::Charm && item.charm() && !item.fragment(), "a picture drawn like charms: a charm");
    item.kin = { "spend/fragment/1103013" };
    check(item.use_kind() == UseKind::Fragment && item.fragment() && !item.charm(), "like fragments: a fragment");
    item.kin = { "spend/buff/8871051", "spend/fragment/1103013" };
    check(item.use_kind() == UseKind::None && !item.fragment() && !item.charm(),
          "like a charm and a fragment both: neither, so never right-clicked");
    item.kin = { "spend/buff/8871051", "spend/hp/8820131" };
    check(item.use_kind() == UseKind::None, "like a charm and a potion both: neither");
    item.kin.clear();
    check(item.use_kind() == UseKind::None, "like nothing in the catalog: neither");
    item.id = "spend/hp/8820131";
    item.kin = { "spend/buff/8871051" };
    check(item.use_kind() == UseKind::None, "a potion's own id: neither, whatever its kin say");
    item.id = "spend/buff/8871061";
    item.via = "spend/buff/8871051";
    check(item.charm() && item.clock_id() == "spend/buff/8871051",
          "a charm that came along for being drawn like the one chosen has the chosen one's clock");
    item.via.clear();
    check(item.clock_id() == "spend/buff/8871061", "and a chosen charm its own");
    item.id.clear();
    item.file = "configs\\trash\\use\\a.png";
    check(item.clock_id() == "a.png", "a picture cut from the bag, its file's name");

    ItemCatalog catalog;
    if (!catalog.load(ItemCatalog::default_dir()) || catalog.find("spend/buff/8871061") == nullptr ||
        catalog.find("spend/hp/8820131") == nullptr || catalog.find("spend/fragment/1103013") == nullptr) {
        skip("the item catalog is not installed here\n");
        return;
    }
    CleanSettings s;
    s.use_ids = { "spend/buff/8871061", "spend/hp/8820131", "spend/fragment/1103013" };
    s.delete_ids = { "spend/buff/8871061", "spend/fragment/1103013" };
    TrashList list;
    list.build(temp_path("no_trash_pictures"), s, catalog);
    int charm_uses = 0, charm_deletes = 0, fragment_uses = 0, potion_uses = 0;
    bool charm_then_delete = false, fragment_then_delete = false;
    for (const TrashList::Item& it : list.items) {
        const bool use = it.action == TrashList::Action::Use;
        if (it.charm()) {
            if (use) {
                ++charm_uses;
                charm_then_delete = charm_then_delete || it.then_delete;
            } else {
                ++charm_deletes;
            }
        }
        if (it.fragment() && use) {
            ++fragment_uses;
            fragment_then_delete = fragment_then_delete || it.then_delete;
        }
        if (!it.id.empty() && use && bag::use_kind_of(it.id) == UseKind::None) ++potion_uses;
    }
    check(charm_uses >= 1 && charm_deletes == 0 && !charm_then_delete,
          "a charm chosen to use and to delete: Use items only -- it is never deleted, nor 'the rest of the stack'");
    check(fragment_uses >= 1 && fragment_then_delete, "a fragment chosen the same way: used and its rest deleted, as before");
    check(potion_uses == 0, "a potion on the Dùng list is still no item to use");
}

// ------------------------------------------- charms: each colour a charm of its own

namespace {

/// The catalog's charms: the card in five colours, the diamond in three.
const char* const kCharmIds[] = { "spend/buff/8871011", "spend/buff/8871021", "spend/buff/8871031",
                                  "spend/buff/8871041", "spend/buff/8871081", "spend/buff/8871051",
                                  "spend/buff/8871061", "spend/buff/8871071" };
const char* const kGreyCard = "spend/buff/8871011";
const char* const kBlueCard = "spend/buff/8871021";
const char* const kRedCard = "spend/buff/8871031";
const char* const kPurpleCard = "spend/buff/8871081";

/// The last digits of a catalog id, for a short line: "81" for 8871081.
std::string last2(const std::string& id) { return id.size() >= 2 ? id.substr(id.size() - 2) : id; }

/// `icon` with `dr`, `dg` and `db` added to its colour: a hue no icon has.
Frame tinted(const Frame& icon, int dr, int dg, int db) {
    Frame t = icon;
    for (std::size_t i = 0; i < t.pixels.size(); i += 4) {
        t.pixels[i] = static_cast<std::uint8_t>(std::clamp(t.pixels[i] + db, 0, 255));
        t.pixels[i + 1] = static_cast<std::uint8_t>(std::clamp(t.pixels[i + 1] + dg, 0, 255));
        t.pixels[i + 2] = static_cast<std::uint8_t>(std::clamp(t.pixels[i + 2] + dr, 0, 255));
    }
    return t;
}

/// Two icons of one drawing averaged pixel by pixel: a colour between theirs.
Frame between(const Frame& a, const Frame& b) {
    Frame t = a;
    for (std::size_t i = 0; i < t.pixels.size() && i < b.pixels.size(); ++i)
        t.pixels[i] = static_cast<std::uint8_t>((a.pixels[i] + b.pixels[i]) / 2);
    return t;
}

/// An icon in a slot as the client draws it: a pixel right of the middle of
/// the slot's dark, blended over it by its own transparency. A slot's size.
Frame slot_tile_of(const Frame& icon) {
    Frame t = scene(34, 34, 14);
    const int ox = (36 - icon.width) / 2, oy = (34 - icon.height) / 2;
    for (int y = 0; y < icon.height; ++y)
        for (int x = 0; x < icon.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * icon.width + x) * 4;
            const int a = icon.pixels[i + 3];
            if (a == 0) continue;
            put_px(t, ox + x, oy + y, (icon.pixels[i + 2] * a + 14 * (255 - a)) / 255,
                   (icon.pixels[i + 1] * a + 14 * (255 - a)) / 255, (icon.pixels[i] * a + 14 * (255 - a)) / 255);
        }
    return t;
}

/// That tile on a frame of its own: the frame and the slot.
std::pair<Frame, ScreenRect> charm_slot(const Frame& icon) {
    Frame f = scene(80, 60, 60);
    const ScreenRect s{ 10, 10, 34, 34 };
    blit(f, slot_tile_of(icon), s.x, s.y);
    return { f, s };
}

/// The list a pass builds with these ids chosen to use and to delete, and
/// no picture cut from the bag.
TrashList charm_list(const ItemCatalog& catalog, const std::vector<std::string>& use,
                     const std::vector<std::string>& del = {}) {
    CleanSettings s;
    s.use_ids = use;
    s.delete_ids = del;
    TrashList list;
    list.build(temp_path("no_trash_pictures"), s, catalog);
    return list;
}

/// A verdict's colour, for a check's line.
std::string colour_text(const TrashList::Verdict& k) {
    return "colour " + (k.colour_id.empty() ? std::string("none") : last2(k.colour_id)) + " " + fixed3(k.colour) +
           ", next " + (k.colour_next_id.empty() ? std::string("none") : last2(k.colour_next_id)) + " " +
           fixed3(k.colour_next) + ", mean off " + std::to_string(k.colour_mean_off);
}

/// The item a verdict took, or "none".
std::string taken_text(const TrashList& list, const TrashList::Verdict& k) {
    if (k.item < 0)
        return std::string(k.doubt == TrashList::Verdict::Doubt::Colour ? "a colour doubt"
                           : k.doubt == TrashList::Verdict::Doubt::Rival ? "a rival doubt"
                                                                          : "none");
    const TrashList::Item& it = list.items[static_cast<std::size_t>(k.item)];
    return (it.id.empty() ? std::string("the cut") : last2(it.id)) +
           (it.action == TrashList::Action::Use ? " to use" : " to delete");
}

/// The catalog with every charm in it, or false.
bool charm_catalog(ItemCatalog& catalog) {
    if (!catalog.load(ItemCatalog::default_dir())) return false;
    for (const char* id : kCharmIds)
        if (catalog.find(id) == nullptr) return false;
    return true;
}

} // namespace

void test_charm_colours() {
    section("Bùa: each colour of a charm is a charm of its own -- the drawing one, told by its colour");
    ItemCatalog catalog;
    if (!charm_catalog(catalog)) {
        skip("the item catalog's charms are not installed here\n");
        return;
    }
    const float min_score = CleanSettings{}.min_score;
    const auto icon = [&](const char* id) { return catalog.find(id)->icon; };
    {
        // Purple chosen: one item, no other colour along with it, its own
        // clock -- and the other colours of its card stay in the catalog.
        const TrashList list = charm_list(catalog, { kPurpleCard });
        int uses = 0;
        const TrashList::Item* it = nullptr;
        for (const TrashList::Item& i : list.items)
            if (i.action == TrashList::Action::Use && i.charm()) {
                ++uses;
                it = &i;
            }
        bool rivals = true;
        for (const char* id : { kGreyCard, kBlueCard, kRedCard, "spend/buff/8871041" })
            rivals = rivals && std::any_of(list.rivals.begin(), list.rivals.end(),
                                           [&](const TrashList::Item& r) { return r.id == id; });
        check(uses == 1 && it != nullptr && it->via.empty() && it->clock_id() == kPurpleCard &&
                  it->colour_of == kPurpleCard && list.items.size() == 1 && rivals,
              "8871081 chosen to use: that one item, nothing brought along, its own clock and colour; 11, 21, 31 and "
              "41 left in the catalog -- " + std::to_string(list.items.size()) + " item(s)");
    }
    {
        // Each of the eight chosen alone, against a slot of each.
        std::string wrong;
        for (const char* ticked : kCharmIds) {
            const TrashList list = charm_list(catalog, { ticked });
            const std::vector<std::string> kin = catalog.same_drawing(ticked);
            std::string line;
            for (const char* in_bag : kCharmIds) {
                const auto [f, s] = charm_slot(icon(in_bag));
                const TrashList::Verdict k = list.judge(f, s, min_score);
                const bool same_id = std::string(ticked) == in_bag;
                const bool same_drawing = std::find(kin.begin(), kin.end(), in_bag) != kin.end();
                bool ok = k.item < 0;
                if (same_id)
                    ok = k.item >= 0 && list.items[static_cast<std::size_t>(k.item)].id == ticked;
                else if (same_drawing)
                    ok = k.item < 0 && k.doubt == TrashList::Verdict::Doubt::None && k.colour_id == in_bag;
                if (!ok) wrong += " | " + last2(ticked) + " chosen, a slot of " + last2(in_bag) + ": " +
                                  taken_text(list, k) + ", " + colour_text(k);
                if (same_id || same_drawing)
                    line += " " + last2(in_bag) + "->" + (k.colour_id.empty() ? std::string("--") : last2(k.colour_id)) +
                            " " + fixed3(k.colour) + "/" + std::to_string(k.colour_mean_off) + " (next " +
                            fixed3(k.colour_next) + ")";
            }
            std::printf("  [info] %s chosen:%s\n", last2(ticked).c_str(), line.c_str());
        }
        check(wrong.empty(), "each of the 8 charms chosen alone, a slot of each: taken for its own id only; the same "
                             "drawing in another colour left alone as that colour; another drawing left alone" + wrong);
    }
    {
        // Two colours chosen: each slot is its own colour's.
        const TrashList list = charm_list(catalog, { kPurpleCard, kBlueCard });
        const auto taken = [&](const char* in_bag) {
            const auto [f, s] = charm_slot(icon(in_bag));
            const TrashList::Verdict k = list.judge(f, s, min_score);
            return k.item < 0 ? std::string() : list.items[static_cast<std::size_t>(k.item)].id;
        };
        check(taken(kPurpleCard) == kPurpleCard && taken(kBlueCard) == kBlueCard && taken(kGreyCard).empty(),
              "81 and 21 chosen: a slot of 81 is 81's, of 21 21's, and of 11 nobody's");
    }
    {
        // Purple to use, grey to delete: a grey slot is deleted -- that very id
        // was chosen for it -- and a red one, brought along to the delete list
        // by the grey card, is left alone: the purple card's drawing won it.
        const TrashList list = charm_list(catalog, { kPurpleCard }, { kGreyCard });
        const auto verdict = [&](const char* in_bag) {
            const auto [f, s] = charm_slot(icon(in_bag));
            return list.judge(f, s, min_score);
        };
        const TrashList::Verdict grey = verdict(kGreyCard), red = verdict(kRedCard), purple = verdict(kPurpleCard);
        const bool grey_deleted = grey.item >= 0 &&
                                  list.items[static_cast<std::size_t>(grey.item)].action == TrashList::Action::Delete &&
                                  list.items[static_cast<std::size_t>(grey.item)].id == kGreyCard;
        const bool purple_used = purple.item >= 0 &&
                                 list.items[static_cast<std::size_t>(purple.item)].action == TrashList::Action::Use;
        check(grey_deleted && red.item < 0 && red.colour_id == kRedCard && purple_used,
              "81 to use and 11 to delete: a slot of 11 is deleted (" + taken_text(list, grey) + "), of 31 left "
              "alone (" + taken_text(list, red) + ", " + colour_text(red) + "), of 81 used");
    }
    {
        // The same with the purple card's own icon held to a score no match
        // reaches -- a frame where it misses the slot and a delete item does
        // not: what won on the delete list is told by its colour all the same.
        // A slot of 81 or of 21 is left alone, told as that colour; one of 11
        // is still deleted.
        TrashList list = charm_list(catalog, { kPurpleCard }, { kGreyCard });
        for (TrashList::Item& it : list.items)
            if (it.id == kPurpleCard) it.min_score = 1.5f;
        const auto verdict = [&](const char* in_bag) {
            const auto [f, s] = charm_slot(icon(in_bag));
            return list.judge(f, s, min_score);
        };
        const TrashList::Verdict purple = verdict(kPurpleCard), blue = verdict(kBlueCard), grey = verdict(kGreyCard);
        const bool grey_deleted = grey.item >= 0 &&
                                  list.items[static_cast<std::size_t>(grey.item)].action == TrashList::Action::Delete &&
                                  list.items[static_cast<std::size_t>(grey.item)].id == kGreyCard;
        check(purple.item < 0 && purple.doubt == TrashList::Verdict::Doubt::None && purple.colour_id == kPurpleCard &&
                  blue.item < 0 && blue.colour_id == kBlueCard && grey_deleted,
              "81 to use missing its slot, 11 to delete: a slot of 81 left alone (" + taken_text(list, purple) + ", " +
                  colour_text(purple) + "), of 21 left alone (" + taken_text(list, blue) + ", " + colour_text(blue) +
                  "), of 11 deleted (" + taken_text(list, grey) + ")");
    }
    {
        // A hue no icon has: the red card turned toward orange. Never taken
        // for red -- its mean colour is far from red's.
        const TrashList list = charm_list(catalog, { kRedCard });
        const auto [f, s] = charm_slot(tinted(icon(kRedCard), 0, 40, -50));
        const TrashList::Verdict k = list.judge(f, s, min_score);
        check(k.item < 0 && k.doubt == TrashList::Verdict::Doubt::Colour,
              "the red card in an orange no icon has: a colour doubt, never 31 -- " + taken_text(list, k) + ", " +
                  colour_text(k));
    }
    {
        // A charm cut from the bag: its colour decided once, as the list is
        // built, against the catalog's icons.
        const BagLayout l;
        const auto cut_of = [&](const Frame& drawn) {
            const auto [f, s] = charm_slot(drawn);
            TrashList list;
            list.add_picture(bag::cut(f, s), TrashList::Action::Use, l);
            list.items.back().file = "item_cut.png";
            list.add_rivals(catalog);
            return list;
        };
        const TrashList purple = cut_of(icon(kPurpleCard));
        const auto judged = [&](const TrashList& list, const char* in_bag) {
            const auto [f, s] = charm_slot(icon(in_bag));
            return list.judge(f, s, min_score);
        };
        const TrashList::Verdict own = judged(purple, kPurpleCard), blue = judged(purple, kBlueCard);
        check(purple.items.size() == 1 && purple.items[0].charm() && purple.items[0].colour_of == kPurpleCard &&
                  own.item == 0 && blue.item < 0 && blue.colour_id == kBlueCard,
              "a cut of a slot of 81: its colour 81 (" + purple.items[0].colour_of + "); it takes a slot of 81, and a "
              "slot of 21 is left alone as 21 -- " + taken_text(purple, blue));
        const TrashList unsure = cut_of(between(icon(kBlueCard), icon(kPurpleCard)));
        const TrashList::Verdict on_purple = judged(unsure, kPurpleCard), on_blue = judged(unsure, kBlueCard);
        check(unsure.items.size() == 1 && unsure.items[0].charm() && unsure.items[0].colour_of.empty() &&
                  on_purple.item < 0 && on_purple.doubt == TrashList::Verdict::Doubt::Colour && on_blue.item < 0 &&
                  on_blue.doubt == TrashList::Verdict::Doubt::Colour,
              "a cut halfway between 21 and 81: no colour, and every slot it takes is a colour doubt -- on 81 " +
                  taken_text(unsure, on_purple) + ", on 21 " + taken_text(unsure, on_blue));
    }
    {
        // A colour chosen whose own drawing does not match the slot is not
        // taken for it on the colour alone (A11): 21 held to a score no match
        // reaches, a slot of 21 won by 81's drawing is left alone.
        TrashList list = charm_list(catalog, { kPurpleCard, kBlueCard });
        for (TrashList::Item& it : list.items)
            if (it.id == kBlueCard) it.min_score = 1.5f;
        const auto [f, s] = charm_slot(icon(kBlueCard));
        const TrashList::Verdict k = list.judge(f, s, min_score);
        check(k.item < 0 && k.doubt == TrashList::Verdict::Doubt::None && k.colour_id == kBlueCard,
              "a slot of 21 whose own icon does not match it: not taken for 21 on 81's drawing -- " +
                  taken_text(list, k));
    }
}

void test_charm_colour_runs() {
    section("Bùa: slots of another colour, or of a colour not sure, left alone by a pass and by a charm run");
    if (skip_without_kit()) return;
    ItemCatalog catalog;
    if (!charm_catalog(catalog)) {
        skip("the item catalog's charms are not installed here\n");
        return;
    }
    const auto icon = [&](const char* id) { return catalog.find(id)->icon; };
    // A bag with a blue card in slot 1, two purple cards in a purple no icon
    // has in slots 2 and 3, and a purple card in slot 4; the purple chosen.
    const auto fill_bag = [&](Pass& p) {
        make_bag(p.game);
        p.game.book_opens = false;
        p.game.reply = Pretend::Reply::Charm;
        const Frame odd = tinted(icon(kPurpleCard), 0, 40, 0);
        const std::pair<int, Frame> slots[] = { { 1, slot_tile_of(icon(kBlueCard)) }, { 2, slot_tile_of(odd) },
                                                { 3, slot_tile_of(odd) }, { 4, slot_tile_of(icon(kPurpleCard)) } };
        for (const auto& [slot, tile] : slots) {
            Pretend::Item it{ slot, 1, 5 };
            it.icon = tile;
            p.game.items.push_back(it);
        }
        p.list = charm_list(catalog, { kPurpleCard });
    };
    BagCleaner::forget_said_once();
    {
        Pass p;
        fill_bag(p);
        const auto r = p.run();
        const auto colour_lines = std::count_if(p.said.begin(), p.said.end(), [](const std::string& l) {
            return l.find("không chắc màu") != std::string::npos;
        });
        check(r.doubtful == 0 && !p.says("khác hình") && colour_lines == 1 && p.game.right_clicks == 0 &&
                  p.game.drags == 0 && p.says("bùa được dùng riêng"),
              "a pass: the purple card said as a charm with a run of its own, the blue one passed over, the two "
              "of a colour not sure said once and not counted as doubts -- " + std::to_string(colour_lines) +
                  " line(s), " + std::to_string(r.doubtful) + " doubtful, " + p.last());
    }
    {
        Pass p;
        fill_bag(p);
        p.mode = BagCleaner::Mode::CharmsOnly;
        const auto r = p.run();
        const ScreenRect four = p.game.slot_rect(4);
        check(p.game.right_clicks == 1 && !p.game.right_at.empty() && four.contains(p.game.right_at[0].first,
                                                                                    p.game.right_at[0].second) &&
                  r.charms_used == 1 && p.game.items[3].count == 4 && r.charms_other_colour == 1 &&
                  r.charms_colour_doubt == 2 && p.says("màu không giống") && p.game.keys == 0,
              "a charm run: only the purple card is right-clicked, and used; the blue one and the two not sure "
              "counted and left alone -- " + std::to_string(p.game.right_clicks) + " right-click(s), " +
                  std::to_string(r.charms_other_colour) + " other colour, " + std::to_string(r.charms_colour_doubt) +
                  " not sure");
        // The same bag again: what was said of the colour is not said twice.
        Pass q;
        fill_bag(q);
        q.mode = BagCleaner::Mode::CharmsOnly;
        q.run();
        check(!q.says("màu không giống"), "and a run after it does not say the colour doubt again");
    }
}

void test_charm_question() {
    section("Bùa: the question taken for a charm's, and the questions that are not");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    Frame probe;
    bag::Dialog d = question_on(probe, k.other);
    check(d.pair.w > 0 && !d.ours() && !d.registers && bag::charm_question(probe, k.store, d),
          "the game's box drawn like the registration's, with words of its own: a charm's question");
    d = question_on(probe, k.question);
    check(d.pair.w > 0 && d.registers && !bag::charm_question(probe, k.store, d),
          "the fragment registration's own words: not a charm's -- it is told by its words");
    d = question_on(probe, *k.store.get("grabbed/hoisinh_confirm"));
    check(d.pair.w > 0 && !bag::charm_question(probe, k.store, d),
          "the revive question: not a charm's, though 'Bạn có muốn sử dụng…' is how a use question opens too");
    d = question_on(probe, *k.store.get("grabbed/del_item_confirm_1"));
    check(d.pair.w > 0 && d.ours() && !bag::charm_question(probe, k.store, d), "the bin's question: not a charm's");
    // The bin's quantity box, its picture stopping above its buttons: where
    // find_dialogs does not see it as a dialog at all it is none to answer,
    // and where it does it is the bin's.
    d = question_on(probe, *k.store.get("grabbed/del_item_confirm_2"));
    check(d.pair.w == 0 || !bag::charm_question(probe, k.store, d), "the bin's quantity box: never a charm's");
    // A box that is not the game's -- other colours, no frame -- though it has
    // the two buttons: its frame is not the registration's.
    Frame plain = k.other;
    fill(plain, ScreenRect{ 0, 40, plain.width, plain.height - 40 }, 200, 30, 30);
    blit(plain, *k.store.get("grabbed/confirm_button"), k.question.width / 2 - 20, 56);
    d = question_on(probe, plain);
    check(d.pair.w == 0 || !bag::charm_question(probe, k.store, d),
          "the two buttons in a box that is not the game's frame: not a charm's");
    check(bag::can_check_charm(k.store) && !bag::can_check_charm(k.bare),
          "the pictures a charm run needs: the registration question's box is one");
    {
        // The message's picture with no Xác Nhận to be found in it: no charm
        // run, for its message could not be closed.
        TemplateStore blank = k.store;
        Frame ok = *k.store.get("grabbed/dialog_ok");
        fill(ok, ScreenRect{ 140, 50, 50, 25 }, 120, 120, 120);
        blank.put("grabbed/dialog_ok", ok);
        check(!bag::can_check_charm(blank), "nor with a message's picture that has no Xác Nhận in it");
    }
    // And each of the questions it tells a charm's from: with one missing, that
    // question could be taken for a charm's.
    for (const char* missing : { "grabbed/hoisinh_confirm", "grabbed/drop_confirm", "grabbed/sell_confirm",
                                 "grabbed/del_item_confirm_1", "grabbed/confirm_button", "grabbed/dialog_ok",
                                 bag::kCloseButtons[0], bag::kCloseButtons[2] }) {
        TemplateStore without;
        for (const char* name : { "grabbed/inventory", "inven/0/trash_can", "grabbed/confirm_button",
                                  "grabbed/del_item_confirm_1", "grabbed/del_item_confirm_2", "grabbed/dialog_ok",
                                  "grabbed/hoisinh_confirm", "grabbed/drop_confirm", "grabbed/sell_confirm",
                                  bag::kRegisterDialog, bag::kCloseButtons[0], bag::kCloseButtons[1],
                                  bag::kCloseButtons[2] }) {
            if (std::string(name) == missing) continue;
            if (const Frame* f = k.store.get(name)) without.put(name, *f);
        }
        check(!bag::can_check_charm(without), std::string("no charm run without ") + missing);
    }
}

/// What one run of charms only comes to, on a pretend bag with `count` of a
/// charm in slot 3.
struct CharmRun {
    Pass p;
    BagCleaner::Result r;
    std::int64_t took_ms = 0;
    explicit CharmRun(Pretend::Reply reply = Pretend::Reply::Charm, int count = 4) {
        make_bag(p.game);
        p.game.book_opens = false;
        p.game.reply = reply;
        p.charm(3, 5, count);
        p.mode = BagCleaner::Mode::CharmsOnly;
    }
    BagCleaner::Result run() {
        const auto from = p.game.time.t;
        r = p.run();
        took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(p.game.time.t - from).count();
        return r;
    }
};

void test_charm_run() {
    section("Bùa: one right-click, the question confirmed, the stack seen one less");
    if (skip_without_kit()) return;
    {
        CharmRun c;
        c.run();
        check(c.p.game.right_clicks == 1 && c.p.game.confirms == 1 && c.p.game.cancels == 0 &&
                  c.p.game.items[0].count == 3 && c.p.game.seq == "RC" && c.r.charms_used == 1 && c.r.trouble.empty(),
              "a charm of 4: right-clicked once, its question confirmed once, 3 left -- " + c.p.game.seq + ", " +
                  c.p.last());
        check(c.r.charm_tries.size() == 1 && c.r.charm_tries[0].how == CharmClock::How::Used &&
                  c.r.charm_tries[0].id == "charm_3",
              "and said used, by the charm's clock id");
        check(c.p.game.book_clicks == 0 && c.p.game.drags == 0 && c.p.game.keys == 0 && c.p.game.slot_clicks == 0 &&
                  !c.r.stuck,
              "with no book to shut, nothing dragged, no key and no click on a slot -- the bag was open and on "
              "Tiêu Dùng already");
        // The cost: the pretend game's own time -- every look at it 40 ms and
        // every wait what it says. With no message from the game, the second
        // after Xác Nhận it is given to come in is waited out, and looked for
        // once more at the end; with one, it is closed as soon as it is seen.
        std::printf("  [info] a charm used, no message from the game: %lld ms of the pretend game's time\n",
                    static_cast<long long>(c.took_ms));
        check(c.took_ms < 2500, "quickly, its message waited for: " + std::to_string(c.took_ms) + " ms");
        CharmRun m;
        m.p.game.notice_after = true;
        m.p.game.lit = true;
        m.run();
        std::printf("  [info] a charm used, its message closed: %lld ms of the pretend game's time\n",
                    static_cast<long long>(m.took_ms));
        check(m.r.charms_used == 1 && m.p.game.messages_closed == 1 && m.took_ms < c.took_ms,
              "and quicker with the message the game shows: " + std::to_string(m.took_ms) + " ms");
    }
    {
        CharmRun c(Pretend::Reply::Charm, 1);
        c.run();
        check(c.p.game.items[0].gone() && c.r.charms_used == 1 && c.p.game.confirms == 1,
              "the last of a stack: the slot seen empty counts as used");
    }
    {
        CharmRun c;
        c.p.due = [](const std::string&) { return false; };
        c.run();
        check(c.p.game.right_clicks == 0 && c.r.charms_waiting == 1 && c.r.charms_seen == 1 && c.r.charm_tries.empty() &&
                  c.p.says("chưa tới giờ"),
              "a charm not yet due: found, said, never right-clicked -- " + c.p.last());
    }
    {
        // Two charms drawn differently: both used, in the order they lie.
        CharmRun c;
        c.p.charm(5, 8, 3, "charm_5");
        c.run();
        check(c.p.game.right_clicks == 2 && c.p.game.confirms == 2 && c.r.charms_used == 2 &&
                  c.p.game.items[0].count == 3 && c.p.game.items[1].count == 2,
              "two kinds of charm: one of each");
    }
    {
        // The same clock id twice -- one kind in two slots: one a run.
        CharmRun c;
        c.p.charm(5, 8, 3, "charm_3");
        c.run();
        check(c.p.game.right_clicks == 1 && c.r.charms_used == 1, "the same kind in two slots: once a run");
    }
    {
        CharmRun c;
        std::vector<std::string> asked;
        c.p.due = [&](const std::string& id) {
            asked.push_back(id);
            return id == "charm_3";
        };
        c.p.charm(5, 8, 3, "charm_5");
        c.run();
        // The one due tried, the run is done: the slot after it is not looked
        // at, nor said to wait.
        check(c.p.game.right_clicks == 1 && c.p.game.items[0].count == 3 && c.p.game.items[1].count == 3 &&
                  c.r.charms_waiting == 0 && c.r.charms_due == 1,
              "one due and one not: only the one that is due, and done with it -- " +
                  std::to_string(c.r.charms_waiting) + " said to wait");
    }
    {
        // The one not due before it: said to wait, passed over.
        CharmRun c;
        c.p.due = [](const std::string& id) { return id == "charm_3"; };
        c.p.charm(1, 8, 3, "charm_1");
        c.run();
        check(c.p.game.right_clicks == 1 && c.p.game.items[0].count == 3 && c.p.game.items[1].count == 3 &&
                  c.r.charms_waiting == 1,
              "one not due before the one due: passed over and said to wait, the one due used");
    }
}

void test_charm_outcomes() {
    section("Bùa: what is not a use is never taken for one");
    if (skip_without_kit()) return;
    {
        CharmRun c(Pretend::Reply::Revive);
        c.run();
        check(c.p.game.right_clicks == 1 && c.p.game.confirms == 0 && c.p.game.wrong_confirms == 0 &&
                  c.p.game.cancels == 1 && c.r.charms_used == 0 &&
                  c.r.charm_tries.size() == 1 && c.r.charm_tries[0].how == CharmClock::How::NotUsed,
              "the revive question after a right-click: Hủy Bỏ, never Xác Nhận, and not used -- " + c.p.last());
    }
    {
        CharmRun c(Pretend::Reply::BinWords);
        c.run();
        check(c.p.game.confirms == 0 && c.p.game.wrong_confirms == 0 && c.p.game.cancels == 1,
              "the bin's question: Hủy Bỏ");
    }
    {
        CharmRun c(Pretend::Reply::Register);
        c.run();
        check(c.p.game.confirms == 0 && c.p.game.cancels == 1 && c.r.charms_used == 0,
              "the fragment registration's question for a charm: Hủy Bỏ");
    }
    {
        CharmRun c;
        c.p.game.registers = false;
        c.run();
        check(c.p.game.confirms == 1 && c.p.game.items[0].count == 4 && c.r.charms_used == 0 && !c.r.trouble.empty() &&
                  c.r.charm_tries.size() == 1 && c.r.charm_tries[0].how == CharmClock::How::MaybeUsed,
              "Xác Nhận pressed and the stack as it was: perhaps used -- not a charm to be tried again in ten "
              "minutes -- and the run stops -- " + c.r.trouble);
    }
    {
        // The game updates the stack late: after Xác Nhận the first looks show
        // the old count. Watched for a while, then seen one less: used.
        CharmRun c;
        c.p.game.registers = false;
        int after = 0;
        c.p.during = [&](BagCleaner&) {
            if (c.p.game.confirms > 0 && ++after == 7) --c.p.game.items[0].count;
        };
        c.run();
        check(c.p.game.confirms == 1 && c.p.game.items[0].count == 3 && c.r.charms_used == 1 && c.r.trouble.empty() &&
                  c.r.charm_tries.size() == 1 && c.r.charm_tries[0].how == CharmClock::How::Used,
              "a stack that goes down a moment after Xác Nhận: seen, and used -- " + c.r.trouble + " (" +
                  std::to_string(after) + " looks after the click)");
    }
    {
        // The game's message after Xác Nhận -- the charm worked -- where the
        // question's Hủy Bỏ was, as on the live client.
        CharmRun c;
        c.p.game.notice_after = true;
        c.p.game.lit = true;
        c.run();
        check(c.p.game.confirms == 1 && c.p.game.notice_ok_clicks == 1 && c.p.game.messages_closed == 1 &&
                  c.r.charms_used == 1 && c.r.charm_tries.size() == 1 &&
                  c.r.charm_tries[0].how == CharmClock::How::Used && c.r.trouble.empty() && c.p.game.keys == 0 &&
                  c.r.charm_notices == 1 && !c.r.notice_left,
              "the message after Xác Nhận is closed with its own Xác Nhận, and the charm is used -- " + c.p.game.seq +
                  ", " + c.p.last());
    }
    {
        // A message somewhere else after Xác Nhận is not GoBot's to close.
        CharmRun c;
        bool shown = false;
        c.p.during = [&](BagCleaner&) {
            if (c.p.game.confirms > 0 && !shown) {
                shown = true;
                c.p.game.show(*kit().store.get("grabbed/dialog_ok"), Pretend::Up::Notice);
            }
        };
        c.p.game.notice_x = 380;
        c.run();
        check(c.p.game.confirms == 1 && c.r.charms_used == 0 && c.r.charm_tries.size() == 1 &&
                  c.r.charm_tries[0].how == CharmClock::How::MaybeUsed && c.p.game.notice_clicks == 0 &&
                  !c.r.trouble.empty(),
              "a message after Xác Nhận where the question puts none: perhaps used, left for the player to read -- " +
                  c.r.trouble);
    }
    for (int swapped_at = 2; swapped_at <= 3; ++swapped_at) {
        // The question replaced by the revive question between the looks that
        // decided and the click: the look after the slow one catches it.
        CharmRun c;
        int looks = 0;
        c.p.during = [&](BagCleaner&) {
            if (c.p.game.right_clicks > 0 && c.p.game.confirms == 0 && ++looks == swapped_at)
                c.p.game.show(*kit().store.get("grabbed/hoisinh_confirm"), Pretend::Up::Question);
        };
        c.p.game.reply = Pretend::Reply::Charm;
        c.run();
        check(c.p.game.confirms == 0 && c.p.game.wrong_confirms == 0 && c.p.game.cancels == 1 &&
                  c.r.charms_used == 0,
              "the question turns into the revive question at look " + std::to_string(swapped_at) +
                  " after the right-click: never confirmed, Hủy Bỏ -- " + c.p.last());
    }
    {
        // Written down as used before the right-click, with the id.
        CharmRun c;
        std::vector<std::string> claimed;
        int rights_then = -1, confirms_then = -1;
        c.p.claim = [&](const std::string& id) {
            claimed.push_back(id);
            rights_then = c.p.game.right_clicks;
            confirms_then = c.p.game.confirms;
        };
        c.run();
        check(claimed.size() == 1 && claimed[0] == "charm_3" && rights_then == 0 && confirms_then == 0 &&
                  c.r.charms_used == 1,
              "the clock is told before the right-click, not after");
    }
    {
        // No question, the stack down: not vouched for. Perhaps used -- the
        // clock counts it so -- and nothing more is right-clicked.
        CharmRun c(Pretend::Reply::Consumed);
        c.p.charm(5, 8, 3, "charm_5");
        c.run();
        check(c.p.game.right_clicks == 1 && c.r.charms_used == 0 && c.r.use_unsure == 1 && !c.r.trouble.empty() &&
                  c.r.charm_tries.size() == 1 && c.r.charm_tries[0].how == CharmClock::How::MaybeUsed,
              "no question and the stack went down: perhaps used, and the run stops -- " + c.r.trouble);
    }
    {
        CharmRun c(Pretend::Reply::Nothing);
        c.run();
        check(c.p.game.right_clicks == 1 && c.p.game.confirms == 0 && c.r.charms_used == 0 &&
                  c.r.charm_tries.size() == 1 && c.r.charm_tries[0].how == CharmClock::How::NotUsed,
              "nothing came of the right-click: not used, and it goes on to the next");
    }
    {
        CharmRun c(Pretend::Reply::Notice);
        c.run();
        check(c.p.game.right_clicks == 1 && c.p.game.notice_clicks == 0 && c.p.game.clicks == 0 &&
                  c.p.game.confirms == 0 && c.r.charms_used == 0 && !c.r.trouble.empty() &&
                  c.r.charm_tries.size() == 1 && c.r.charm_tries[0].how == CharmClock::How::NotUsed &&
                  c.p.game.keys == 0,
              "a message instead of a question: not clicked, not used, the run stops -- " + c.r.trouble);
    }
    {
        // The same, the stack one less: taken, but not by a question GoBot saw.
        CharmRun c(Pretend::Reply::Notice);
        bool took = false;
        c.p.during = [&](BagCleaner&) {
            if (c.p.game.up == Pretend::Up::Notice && !took) {
                took = true;
                --c.p.game.items[0].count;
            }
        };
        c.run();
        check(c.p.game.notice_clicks == 0 && c.p.game.clicks == 0 && c.r.charms_used == 0 && c.r.use_unsure == 1 &&
                  c.r.charm_tries.size() == 1 && c.r.charm_tries[0].how == CharmClock::How::MaybeUsed &&
                  !c.r.trouble.empty(),
              "and with the stack one less: perhaps used, never clicked, the run stops -- " + c.r.trouble);
    }
    {
        CharmRun c;
        c.p.game.sticky = true;
        c.p.game.reply = Pretend::Reply::Revive;
        c.run();
        check(c.r.stuck && c.p.game.right_clicks == 1, "a question that will not close for Hủy Bỏ: held, not hidden");
    }
}

void test_charm_safety() {
    section("Bùa: nothing right-clicked that is not a charm, or with the bag not as it should be");
    if (skip_without_kit()) return;
    const auto none_clicked = [](CharmRun& c) {
        c.run();
        return c.p.game.right_clicks == 0 && c.r.charms_used == 0;
    };
    {
        CharmRun c;
        c.p.list.items.clear();
        c.p.item(3, 5, 4, TrashList::Action::Use, false, true, { "spend/hp/8820131" });
        check(none_clicked(c), "a picture drawn like a potion on the Dùng list: never right-clicked");
    }
    {
        CharmRun c;
        c.p.list.items.clear();
        c.p.item(3, 5, 4, TrashList::Action::Use, false, true, { "spend/buff/8871011", "spend/fragment/1103013" });
        check(none_clicked(c), "drawn like a charm and a fragment both: never");
    }
    {
        CharmRun c;
        c.p.list.items.clear();
        c.p.item(3, 5, 4, TrashList::Action::Delete, false, true, { "spend/buff/8871011" });
        check(none_clicked(c) && !c.p.game.items[0].gone(), "a charm on the delete list: not used here, and not deleted by this run");
    }
    {
        CharmRun c;
        c.p.settings.use_charms = false;
        check(none_clicked(c) && c.r.trouble.find("use_charms=false") != std::string::npos,
              "use_charms off: nothing right-clicked, and it says why -- " + c.r.trouble);
    }
    {
        CharmRun c;
        blit(c.p.game.base, kit().close_x, 760, 330);
        check(none_clicked(c) && c.p.says("đang có cửa sổ khác mở cạnh túi đồ"),
              "another window's close button beside the bag: nothing right-clicked -- " + c.p.last());
    }
    {
        CharmRun c;
        c.p.counts = nullptr;
        CountReader none;
        c.p.counts = &none;
        check(none_clicked(c) && c.p.says("không có bộ chữ số"), "without the game's digits: nothing right-clicked");
    }
    {
        // Not on Tiêu Dùng: the strip says another tab is chosen, and the run
        // clicks Tiêu Dùng's tab -- which this pretend game does not answer to.
        CharmRun c;
        c.p.game.base = scene(1024, 768, 60);
        make_bag(c.p.game, 0);
        check(none_clicked(c) && !c.r.trouble.empty(), "the bag on another tab that will not change: nothing right-clicked -- " + c.r.trouble);
    }
    {
        // The slot holds one drawing and the picture on the list is another.
        CharmRun c;
        Pretend other;
        make_bag(other);
        // Seed 8, half a turn of the sine from the slot's 5: seeds 5 and 99
        // are drawn nearly alike (99 is 5 and thirty-one turns more).
        other.items.push_back(Pretend::Item{ 3, 8, 4 });
        const Frame drawn = other.render();
        c.p.list.items.clear();
        c.p.list.add_picture(bag::cut(drawn, other.slot_rect(3)), TrashList::Action::Use, c.p.game.l);
        c.p.list.items.back().kin = { "spend/buff/8871011" };
        c.p.list.items.back().file = "other";
        const bool none = none_clicked(c);
        check(none, "a slot that is not drawn like the picture on the list: not right-clicked -- clicks " +
                        std::to_string(c.p.game.right_clicks) + ", seen " + std::to_string(c.r.charms_seen) +
                        ", used " + std::to_string(c.r.charms_used) + ", " + c.p.last() + ", " + c.r.trouble);
    }
    {
        // A pass -- not charms only -- leaves charms alone and says so.
        CharmRun c;
        c.p.mode = BagCleaner::Mode::Pass;
        c.run();
        check(c.p.game.right_clicks == 0 && c.p.says("bùa được dùng riêng") && c.r.use_matched == 0 && c.r.used == 0,
              "a whole pass sees a charm and says it has a run of its own -- " + c.p.last());
    }
    {
        CharmRun c;
        c.run();
        check(c.p.game.right_clicks == 1, "(the same bag, charms only: right-clicked once)");
        CharmRun d;
        d.p.settings.use_charms = true;
        d.r = d.p.run(true);
        check(d.p.game.right_clicks == 0 && d.p.game.items[0].count == 4, "looking only: no right-click");
    }
}

void test_charm_hold() {
    section("Bùa: stopped the moment the hold is gone or the time is up");
    if (skip_without_kit()) return;
    {
        CharmRun c;
        c.p.guard = [] { return false; };
        c.run();
        check(c.p.game.right_clicks == 0 && c.r.trouble.find("không còn giữ phím") != std::string::npos,
              "the hold gone before the first right-click: none -- " + c.r.trouble);
    }
    {
        CharmRun c;
        c.p.charm(5, 8, 3, "charm_5");
        int asked = 0;
        c.p.guard = [&] { return ++asked <= 3; };
        c.run();
        check(c.p.game.right_clicks == 1 && c.r.charms_used == 1 && c.r.trouble.find("không còn giữ phím") != std::string::npos,
              "the hold gone after the first charm: the second is not right-clicked -- " +
                  std::to_string(c.p.game.right_clicks) + " right-clicks, " + c.r.trouble);
    }
    {
        // The hold gone the moment Xác Nhận is pressed: the question is still
        // seen gone, and nothing is said to be stuck in a dialog.
        CharmRun c;
        c.p.guard = [&] { return c.p.game.confirms == 0; };
        c.p.debug_dir = temp_path("charm_hold_confirm");
        c.run();
        check(!c.r.stuck && c.r.charm_tries.size() == 1 &&
                  c.r.trouble.find("không còn giữ phím") != std::string::npos &&
                  files_in(c.p.debug_dir, "*_charm_stuck.png").empty() && c.p.game.notice_ok_clicks == 0 &&
                  c.p.game.keys == 0,
              "the hold gone at Xác Nhận: the question seen closed, nothing held as stuck -- " +
                  std::to_string(c.r.charm_tries.size()) + " tried, " + c.r.trouble);
    }
    {
        // A dialog after Xác Nhận that Hủy Bỏ does not close, the hold gone as
        // it comes: still held to be in the way of the keys.
        CharmRun c;
        bool shown = false;
        int looks = 0;
        c.p.during = [&](BagCleaner&) {
            if (c.p.game.confirms > 0 && !shown && ++looks == 3) {
                shown = true;
                c.p.game.sticky = true;
                c.p.game.show(*kit().store.get("grabbed/hoisinh_confirm"), Pretend::Up::Question);
            }
        };
        c.p.guard = [&] { return !shown; };
        c.run();
        check(shown && c.r.stuck && c.p.game.cancels >= 1 && c.p.game.wrong_confirms == 0 && c.p.game.keys == 0 &&
                  c.r.charm_tries.size() == 1 && c.r.charm_tries[0].how == CharmClock::How::MaybeUsed,
              "a dialog after Xác Nhận that will not close, the hold gone: Hủy Bỏ, perhaps used, and held as stuck "
              "-- " + c.r.trouble);
    }
    {
        CharmRun c;
        c.p.deadline_ms = 1;       // the pretend game's first look is already 40 ms
        c.run();
        check(c.p.game.right_clicks == 0 && c.r.trouble.find("quá thời gian") != std::string::npos,
              "past the deadline: nothing right-clicked -- " + c.r.trouble);
    }
    {
        CharmRun c;
        c.p.deadline_ms = 5000;
        c.run();
        check(c.p.game.right_clicks == 1 && c.r.charms_used == 1, "inside the deadline: as usual");
    }
}

void test_charm_packets() {
    section("Bùa: the packets' bag on a charm's slot can stop its right-click, never cause one");
    if (skip_without_kit()) return;
    BagCleaner::forget_said_once();
    struct Asked {
        std::string colour;
        int slot = -1, count = -1;
    };
    // A charm of 4 alone in slot `slot`, the packets answering `says`.
    const auto run_with = [](int slot, BagCleaner::PacketSays says, std::vector<Asked>& asked) {
        auto c = std::make_unique<CharmRun>();
        c->p.list.items.clear();
        c->p.game.items.clear();
        c->p.charm(slot, 5, 4);
        c->p.packets = [&asked, says](const std::string& colour, int j, int count, std::string& said) {
            asked.push_back(Asked{ colour, j, count });
            said = says == BagCleaner::PacketSays::Nothing ? std::string("no current bag") : std::string("8871021 x4");
            return says;
        };
        c->run();
        return c;
    };
    {
        std::vector<Asked> asked;
        const auto c = run_with(1, BagCleaner::PacketSays::Other, asked);
        check(c->p.game.right_clicks == 0 && c->r.charms_vetoed == 1 && c->r.charm_tries.empty() &&
                  c->p.says("theo gói tin ô này là món khác (8871021 x4)") && asked.size() == 1 &&
                  asked[0].slot == 1 && asked[0].count == 4 && asked[0].colour == "spend/buff/pretend_5",
              "the packets name another item in the slot: not right-clicked, counted, said -- row 1 cell 2 asked as "
              "slot " + (asked.empty() ? std::string("none") : std::to_string(asked[0].slot)));
        std::vector<Asked> again;
        const auto d = run_with(1, BagCleaner::PacketSays::Other, again);
        check(d->p.game.right_clicks == 0 && d->r.charms_vetoed == 1 && !d->p.says("theo gói tin"),
              "and the next run the same: left alone, and not said again");
    }
    for (const BagCleaner::PacketSays says : { BagCleaner::PacketSays::Agrees, BagCleaner::PacketSays::Nothing }) {
        std::vector<Asked> asked;
        const auto c = run_with(6, says, asked);
        check(c->p.game.right_clicks == 1 && c->r.charms_used == 1 && c->r.charms_vetoed == 0 && asked.size() == 1 &&
                  asked[0].slot == 6,
              std::string(says == BagCleaner::PacketSays::Agrees ? "the packets agree" : "the packets say nothing") +
                  ": the screen decides, and the charm is used -- row 2 cell 1 asked as slot " +
                  (asked.empty() ? std::string("none") : std::to_string(asked[0].slot)));
    }
}

/// A charm run whose question's Xác Nhận brings the game's message, as on the
/// live client: its button where the question's Hủy Bỏ was, a button under
/// the pointer drawn lit.
struct NoticeRun : CharmRun {
    explicit NoticeRun(int count = 4) : CharmRun(Pretend::Reply::Charm, count) {
        p.game.notice_after = true;
        p.game.lit = true;
    }
};

/// Whether the debug folder `dir` holds a picture kept as `what`.
bool kept_as(const std::string& dir, const char* what) {
    return !files_in(dir, (std::string("*_") + what + ".png").c_str()).empty();
}

void test_charm_notice() {
    section("Bùa: the game's message after Xác Nhận closed with its own Xác Nhận -- never a key");
    if (skip_without_kit()) return;
    BagCleaner::forget_said_once();
    {
        // 1. Two kinds, each answered with a message: one of each used, both
        //    messages closed, nothing else pressed.
        NoticeRun c;
        c.p.charm(5, 8, 3, "charm_5");
        c.run();
        std::printf("  [info] two charms, each with its message: %lld ms of the pretend game's time\n",
                    static_cast<long long>(c.took_ms));
        check(c.p.game.right_clicks == 2 && c.p.game.confirms == 2 && c.p.game.notice_ok_clicks == 2 &&
                  c.p.game.messages_closed == 2 && c.r.charms_used == 2 && c.r.trouble.empty() &&
                  c.p.game.seq == "RCNRCN" && c.p.game.keys == 0 && c.r.charm_notices == 2 && c.r.charms_due == 2 &&
                  c.p.says("(đã đóng thông báo của game)"),
              "two kinds, each with its message: both used in one run, each message closed before the next "
              "right-click -- " + c.p.game.seq + ", " + std::to_string(c.r.charms_used) + " used, " + c.r.trouble);
        check(c.p.game.right_clicks_with_message == 0 && c.p.game.keys_with_message == 0 &&
                  c.p.game.notice_other_clicks == 0 && c.p.game.wrong_confirms == 0 && !c.r.notice_left,
              "and nothing right-clicked or keyed with a message up, nothing clicked on it but its button");
    }
    {
        // 2. A message, and the stack as it was: closed, perhaps used, stop.
        NoticeRun c;
        c.p.game.registers = false;
        c.p.debug_dir = temp_path("charm_notice_2");
        c.run();
        check(c.p.game.notice_ok_clicks == 1 && c.p.game.messages_closed == 1 && c.r.charms_used == 0 &&
                  c.r.charm_tries.size() == 1 && c.r.charm_tries[0].how == CharmClock::How::MaybeUsed &&
                  !c.r.trouble.empty() && kept_as(c.p.debug_dir, "charm_notice_other") && c.p.game.keys == 0,
              "a message and the stack as it was: closed, perhaps used, the run stops and the message is kept -- " +
                  c.r.trouble);
    }
    {
        // 3. A message that will not close: a second click, a pixel aside and
        //    not sooner than 300 ms, the pointer moved off it in between --
        //    then left for the player.
        NoticeRun c;
        c.p.game.notice_sticks = true;
        c.p.charm(5, 8, 3, "charm_5");
        c.p.debug_dir = temp_path("charm_notice_3");
        c.run();
        const auto& t = c.p.game.notice_click_t;
        const auto& at = c.p.game.notice_click_at;
        const auto& mv = c.p.game.moves_at_notice_click;
        const long long apart =
            t.size() == 2 ? std::chrono::duration_cast<std::chrono::milliseconds>(t[1] - t[0]).count() : -1;
        check(c.p.game.notice_ok_clicks == 2 && apart >= 300 && at.size() == 2 && at[1].first == at[0].first + 1 &&
                  at[1].second == at[0].second && mv.size() == 2 && mv[1] > mv[0],
              "a message that will not close: clicked twice, " + std::to_string(apart) +
                  " ms apart, a pixel aside, the pointer moved off it between -- " +
                  std::to_string(c.p.game.notice_ok_clicks) + " click(s)");
        check(c.r.charms_used == 1 && c.p.game.right_clicks == 1 && c.r.trouble.find("đóng nó trong game") !=
                  std::string::npos && c.p.game.keys == 0 && c.p.game.keys_with_message == 0 && !c.r.notice_left &&
                  kept_as(c.p.debug_dir, "charm_notice_stuck"),
              "used, as the stack says, and nothing more: the player is told to close it, no key pressed -- " +
                  c.r.trouble);
    }
    {
        // 4. The same with hands that cannot move the pointer: the button it
        //    clicked stays lit, so its label is not to be told -- no second
        //    click. Why the pointer is moved off.
        NoticeRun c;
        c.p.game.notice_sticks = true;
        c.p.game.no_move = true;
        c.run();
        check(c.p.game.notice_ok_clicks == 1 && c.r.charms_used == 1 && !c.r.trouble.empty() && c.p.game.keys == 0,
              "with the pointer left on the message's button: one click only, its lit label not clicked again -- " +
                  std::to_string(c.p.game.notice_ok_clicks) + " click(s), " + c.r.trouble);
    }
    for (const int drop : { 13, 16 }) {
        // 5. A taller box, for a longer text: its button lower down.
        NoticeRun c;
        c.p.game.notice_drop = drop;
        c.run();
        check(c.p.game.notice_ok_clicks == 1 && c.p.game.messages_closed == 1 && c.r.charms_used == 1 &&
                  c.r.trouble.empty(),
              "a message " + std::to_string(drop) + " pixels taller: closed, and used -- " + c.r.trouble);
    }
    {
        // 6. The first kind's message, and its stack going down, only after
        //    the second it is given: closed at the next round, then the
        //    second kind used.
        NoticeRun c;
        c.p.game.notice_late_first_ms = 1200;
        c.p.game.notice_takes = true;
        c.p.charm(5, 8, 3, "charm_5");
        c.run();
        check(c.p.game.messages_closed == 2 && c.p.game.notice_ok_clicks == 2 && c.r.charms_used == 2 &&
                  c.r.trouble.empty() && c.p.game.seq == "RCNRCN" && c.p.game.right_clicks_with_message == 0 &&
                  c.p.game.keys == 0,
              "a message come late: closed before the next right-click, and the next kind used -- " + c.p.game.seq +
                  ", " + c.r.trouble);
    }
    {
        // 7. The only kind's message 1.2 s after its Xác Nhận: closed before
        //    the run ends.
        NoticeRun c;
        c.p.game.notice_late_ms = 1200;
        c.run();
        check(c.p.game.messages_closed == 1 && c.r.charms_used == 1 && c.r.trouble.empty() && c.r.charm_notices == 1 &&
                  !c.r.notice_left && c.p.game.keys == 0,
              "the last kind's message come late: closed at the end of the run -- " + c.p.game.seq + ", " +
                  c.r.trouble);
    }
    {
        // 8. The hold gone while the message is waited for: nothing clicked
        //    on it, perhaps used, the run over at once -- and the message
        //    handed to the next run, which closes it and opens nothing.
        NoticeRun c;
        c.p.game.registers = false;
        c.p.charm(5, 8, 3, "charm_5");
        c.p.guard = [&] { return !c.p.game.msg_on; };
        c.run();
        check(c.p.game.notice_ok_clicks == 0 && c.p.game.right_clicks == 1 && c.r.charm_tries.size() == 1 &&
                  c.r.charm_tries[0].how == CharmClock::How::MaybeUsed && !c.r.trouble.empty() && c.r.notice_left &&
                  c.r.notice_place.valid() && !c.r.notice_carried && c.p.game.keys == 0,
              "the hold gone while the message is waited for: not clicked, perhaps used, handed to the next run -- " +
                  c.r.trouble);
        // The next run: the bag shut, nothing due, the message still up.
        c.p.guard = nullptr;
        c.p.due = [](const std::string&) { return false; };
        c.p.own_notice = c.r.notice_place;
        c.p.game.bag_keyed = true;
        c.p.game.bag_shut = true;
        c.run();
        check(c.p.game.notice_ok_clicks == 1 && c.p.game.messages_closed == 1 && c.p.game.keys == 0 &&
                  c.p.game.bag_shut && c.p.game.right_clicks == 1 && !c.r.notice_left && c.r.charm_notices == 1,
              "the next run, nothing due: the message closed, the bag not opened -- " +
                  std::to_string(c.p.game.keys) + " key(s), " + c.r.trouble);
    }
    for (const bool other_tab : { true, false }) {
        // 8b. The same with the bag open for the next run, on another tab or
        //     scrolled down: the message closed, and nothing else clicked --
        //     neither Tiêu Dùng nor the scroll bar.
        NoticeRun c;
        c.p.game.registers = false;
        c.p.charm(5, 8, 3, "charm_5");
        c.p.guard = [&] { return !c.p.game.msg_on; };
        c.run();
        const bool handed = c.r.notice_left;
        c.p.guard = nullptr;
        c.p.due = [](const std::string&) { return false; };
        c.p.own_notice = c.r.notice_place;
        if (other_tab) {
            c.p.game.tabs_work = true;
            c.p.game.cur_tab = 1;
        } else {
            c.p.game.total_rows = 8;
            c.p.game.first_row = 2;
        }
        const int clicks = c.p.game.clicks;
        c.run();
        check(handed && c.p.game.notice_ok_clicks == 1 && c.p.game.messages_closed == 1 &&
                  c.p.game.clicks == clicks + 1 && c.p.game.scroll_clicks == 0 && c.p.game.keys == 0 &&
                  c.p.game.cur_tab == (other_tab ? 1 : kUseTab) && c.p.game.first_row == (other_tab ? 0 : 2) &&
                  !c.r.notice_left && c.r.charm_notices == 1,
              std::string("the next run, nothing due, the bag open ") + (other_tab ? "on another tab" : "scrolled down") +
                  ": the message closed, nothing else clicked -- " + std::to_string(c.p.game.clicks - clicks) +
                  " click(s), " + std::to_string(c.p.game.scroll_clicks) + " scroll(s), " + c.r.trouble);
    }
    {
        // 9. A message up before anything, not GoBot's: nothing pressed --
        //    with the bag open, and with it shut, no bag key either.
        for (const bool shut : { false, true }) {
            NoticeRun c;
            c.p.game.msg_on = true;
            c.p.game.msg_pair = ScreenRect{ 700, 520, 85, 21 };
            c.p.game.bag_keyed = shut;
            c.p.game.bag_shut = shut;
            c.run();
            check(c.p.game.right_clicks == 0 && c.p.game.clicks == 0 && c.p.game.keys == 0 &&
                      c.p.game.notice_ok_clicks == 0 && c.p.says("đang có một thông báo trong game"),
                  std::string("a message up before anything, the bag ") + (shut ? "shut" : "open") +
                      ": nothing right-clicked, clicked or keyed -- " + c.p.last());
        }
    }
    {
        // 10. A dialog of two buttons comes up as the message is closed:
        //     Hủy Bỏ on it, not known whether used.
        NoticeRun c;
        bool shown = false;
        c.p.during = [&](BagCleaner&) {
            if (c.p.game.notice_ok_clicks > 0 && !shown) {
                shown = true;
                c.p.game.show(*kit().store.get("grabbed/hoisinh_confirm"), Pretend::Up::Question);
            }
        };
        c.run();
        check(shown && c.p.game.cancels >= 1 && c.p.game.wrong_confirms == 0 && c.r.charms_used == 0 &&
                  c.r.charm_tries.size() == 1 && c.r.charm_tries[0].how == CharmClock::How::MaybeUsed &&
                  !c.r.stuck && !c.r.trouble.empty() && c.p.game.keys == 0,
              "a dialog as the message is closed: Hủy Bỏ, perhaps used, nothing held -- " + c.r.trouble);
    }
    {
        // 12. A stop while the message is up: no click on it.
        NoticeRun c;
        c.p.during = [&](BagCleaner& me) {
            if (c.p.game.msg_on) me.cancel();
        };
        c.run();
        check(c.p.game.notice_ok_clicks == 0 && c.p.game.right_clicks == 1 && c.p.game.keys == 0,
              "a stop with the message up: nothing clicked on it");
    }
    {
        // 13. Every kind due tried in rows 1 and 2 of a longer tab: done, the
        //     rows below never scrolled to.
        NoticeRun c;
        c.p.game.total_rows = 8;
        c.p.charm(8, 8, 3, "charm_8");
        c.p.charm(10, 3, 3, "charm_10");
        c.run();
        check(c.r.charms_used == 3 && c.p.game.scroll_clicks == 0 && c.r.trouble.empty() && c.r.charms_due == 3,
              "three kinds due, all in the first rows of eight: all used, the scroll bar never clicked -- " +
                  std::to_string(c.r.charms_used) + " used, " + std::to_string(c.p.game.scroll_clicks) +
                  " scroll(s), " + c.r.trouble);
    }
    {
        // 15. The bag shut at the start: opened and shut again, two keys in
        //     all; with a message left up, left open -- one key.
        NoticeRun c;
        c.p.game.bag_keyed = true;
        c.p.game.bag_shut = true;
        c.run();
        check(c.r.charms_used == 1 && c.p.game.messages_closed == 1 && c.p.game.keys == 2 && c.p.game.bag_shut,
              "the bag shut at the start: opened, the charm used and its message closed, shut again -- " +
                  std::to_string(c.p.game.keys) + " key(s)");
        NoticeRun d;
        d.p.game.bag_keyed = true;
        d.p.game.bag_shut = true;
        d.p.game.notice_sticks = true;
        d.run();
        check(d.r.charms_used == 1 && d.p.game.keys == 1 && !d.p.game.bag_shut && d.p.game.keys_with_message == 0 &&
                  d.p.says("vẫn còn một thông báo đang mở"),
              "with the message left up: the bag left open, no key over it -- " + std::to_string(d.p.game.keys) +
                  " key(s)");
    }
    {
        // 16. The deadline: 12 s and 3 s a kind, 20 to 40 s.
        const std::pair<int, int> deadlines[] = { { 0, 20000 }, { 1, 20000 }, { 2, 20000 },
                                                  { 3, 21000 }, { 8, 36000 }, { 10, 40000 } };
        bool all = true;
        std::string got;
        for (const auto& d : deadlines) {
            const int ms = BagCleaner::charm_deadline_ms(d.first);
            all = all && ms == d.second;
            got += (got.empty() ? "" : ", ") + std::to_string(d.first) + ": " + std::to_string(ms);
        }
        check(all, "the deadline of a charm run by kinds due -- " + got);
        // The only kind due on the last page of a tab of twelve rows: found
        // and used inside its deadline.
        NoticeRun c;
        c.p.game.total_rows = 12;
        c.p.game.items[0].slot = 11 * 6 + 2;
        c.p.deadline_ms = BagCleaner::charm_deadline_ms(1);
        c.run();
        std::printf("  [info] the only charm on the last page of twelve rows: %lld ms, %d scroll(s)\n",
                    static_cast<long long>(c.took_ms), c.p.game.scroll_clicks);
        check(c.r.charms_used == 1 && c.r.trouble.empty() && c.took_ms < BagCleaner::charm_deadline_ms(1) &&
                  c.p.game.scroll_clicks == 8,
              "the only kind on the last page of twelve rows: used inside its deadline -- " +
                  std::to_string(c.took_ms) + " ms, " + c.r.trouble);
    }
}

/// A charm question, its Hủy Bỏ under the pointer and lit or not, and the band
/// its message would have: no lone Xác Nhận there. The message itself, taller
/// or not, lit or not.
void test_charm_notice_pictures() {
    section("Bùa: a lone Xác Nhận is a message's, never a question's -- lit or not");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    Pretend g;
    make_bag(g);
    g.lit = true;
    g.show(k.other, Pretend::Up::Question);
    const ScreenRect p = g.shown_pair;
    const ScreenRect band = bag::message_band(p);
    bag::LoneOk how;
    Frame f = g.render();
    const bool plain = bag::lone_ok(f, k.store, band, nullptr, &how);
    g.px = p.x + p.w * 3 / 4;
    g.py = p.cy();
    f = g.render();
    bag::LoneOk lit_how;
    const bool lit = bag::lone_ok(f, k.store, band, nullptr, &lit_how);
    std::printf("  [info] a question's Hủy Bỏ in the band: label %s, edge %s, corner %s; lit: label %s\n",
                fixed3(how.label_score).c_str(), fixed3(how.edge_score).c_str(), fixed3(how.corner_score).c_str(),
                fixed3(lit_how.label_score).c_str());
    // Unlit, the question is a dialog of two buttons, which lone_ok turns down
    // whatever its label scores: that label is what tells it, lit or not.
    check(p.valid() && !plain && !lit && how.label_score < 0.95f && lit_how.label_score < 0.95f,
          "11. a charm question in the band its message would have, its Hủy Bỏ lit or not: no lone Xác Nhận -- "
          "never clicked as a message");
    // The message itself there, its button lit and not.
    Pretend m;
    make_bag(m);
    m.lit = true;
    m.msg_on = true;
    m.msg_pair = p;
    Frame n = m.render();
    ScreenRect at;
    bag::LoneOk seen;
    const bool found = bag::lone_ok(n, k.store, band, &at, &seen);
    std::printf("  [info] the pretend message: label %s, edge %s, corner %s at %d,%d\n",
                fixed3(seen.label_score).c_str(), fixed3(seen.edge_score).c_str(), fixed3(seen.corner_score).c_str(),
                at.x, at.y);
    check(found && at.x == m.msg_label().x && at.y == m.msg_label().y && bag::message_up(n, k.store),
          "the message where the question's Hủy Bỏ was: its lone Xác Nhận found, at its label");
    m.px = m.msg_button().cx();
    m.py = m.msg_button().cy();
    n = m.render();
    bag::LoneOk dim;
    check(!bag::lone_ok(n, k.store, band, nullptr, &dim),
          "and not with its button lit by the pointer over it -- label " + fixed3(dim.label_score) + ", corner " +
              fixed3(dim.corner_score));
    // Unlit again, where it is found in the band.
    m.px = m.py = -1;
    n = m.render();
    check(!bag::lone_ok(n, k.store, ScreenRect{ 0, 0, 300, 300 }), "nor outside the band");
    // A taller box: its button lower, in the band down to 40 pixels lower --
    // the band ends 57 below the pair's top -- and not 48.
    for (const int drop : { 13, 16, 40, 48 }) {
        Pretend t;
        make_bag(t);
        t.msg_on = true;
        t.msg_pair = p;
        t.notice_drop = drop;
        check(bag::lone_ok(t.render(), k.store, band) == (drop <= 40),
              "a message " + std::to_string(drop) + " pixels taller: its button " +
                  (drop <= 40 ? "found" : "not found") + " in the band");
    }
    check(bag::message_band(ScreenRect{ 514, 397, 85, 21 }).contains(561, 401) &&
              !bag::message_band(ScreenRect{ 514, 397, 85, 21 }).contains(518, 401) &&
              bag::message_box(ScreenRect{ 514, 397, 85, 21 }).x == 410 &&
              bag::message_box(ScreenRect{ 514, 397, 85, 21 }).y == 341,
          "the band of a question at 514,397 holds the live message's label at 561,401, not the question's own "
          "Xác Nhận label at 518,401; its box from 410,341");
}

/// Where the cuts of the live client's charm frames are (2026-11-02 23:12 and
/// 2026-11-03 06:44): beside this test, else the copy handed over with them.
/// Empty when neither is here. Small parts, each named <what>_<x>_<y>.png by
/// where it was -- the question box, the message box, the bag's tab strip,
/// grid and scroll bar -- none with a name, a chat line, money or the map in
/// it; never in git or the zip.
std::string charm_cuts_dir() {
    for (const std::string& d : { exe_relative("testdata\\charms"), std::string(kLiveBuild) + "handoff\\charms\\testdata" })
        if (!files_in(d, "charm_*.png").empty()) return d;
    return {};
}

void test_live_charm() {
    section("Bùa: the live client's charm question, its message and its bag, on cuts of its frames");
    if (skip_without_kit()) return;
    const std::string dir = charm_cuts_dir();
    if (dir.empty()) {
        skip("no cuts of the live charm frames here (handoff\\charms\\testdata)\n");
        return;
    }
    const Kit& k = kit();
    // The question's pair as find_dialogs puts it, for the band its message
    // is looked for in; 515,397 on the cut when it is not here.
    ScreenRect pair{ 515, 397, 85, 21 };
    {
        // The question (06:44:46): "Thời gian hiệu lực tối đa là 2 giờ. Bạn có
        // muốn sử dụng không?" over [Xác Nhận][Hủy Bỏ].
        Frame f = scene(1024, 768, 40);
        if (!put_back(f, dir, "charm_question")) {
            skip("no cut of the charm question here\n");
        } else {
            const auto ds = bag::find_dialogs(f, k.store);
            const bag::Dialog d = ds.size() == 1 ? ds.front() : bag::Dialog{};
            // The frame it is told by, as charm_question compares it: the
            // registration question's box from 6 above its buttons -- at 105,56
            // in its picture -- to its bottom, where these buttons have it.
            const int top = 56 - 6;
            Frame strip;
            strip.width = k.question.width;
            strip.height = k.question.height - top;
            strip.pixels.assign(k.question.pixels.begin() + static_cast<std::ptrdiff_t>(top) * k.question.width * 4,
                                k.question.pixels.end());
            const auto frame = TemplateMatcher::find(f, ScreenRect{ d.pair.x - 105 - 2, d.pair.y - 6 - 2,
                                                                    strip.width + 4, strip.height + 4 },
                                                     strip, -1.0f);
            std::printf("  [info] the charm question: pair at %d,%d %dx%d, its frame %s\n", d.pair.x, d.pair.y,
                        d.pair.w, d.pair.h, fixed3(frame.score).c_str());
            check(ds.size() == 1 && !d.ours() && !d.registers && bag::charm_question(f, k.store, d, 0.92f) &&
                      std::abs(d.pair.x - 514) <= 1 && d.pair.y == 397,
                  "one dialog, a charm's question at charm_frame_score 0.92 (its frame " + fixed3(frame.score) +
                      "), its buttons at 514,397 -- " + std::to_string(ds.size()) + " dialog(s), at " +
                      std::to_string(d.pair.x) + "," + std::to_string(d.pair.y));
            if (d.pair.valid()) pair = d.pair;
            // No lone Xác Nhận where its message would be -- its Hủy Bỏ is
            // there -- nor at its own Xác Nhận, which has Hủy Bỏ to its right
            // and not the box's frame.
            bag::LoneOk in_band, own;
            const bool band_ok = bag::lone_ok(f, k.store, bag::message_band(pair), nullptr, &in_band);
            bag::lone_ok(f, k.store, ScreenRect{ pair.x + 4 - 1, pair.y + 4 - 1, 34 + 2, 13 + 2 }, nullptr, &own);
            std::printf("  [info] the charm question: in its message's band the label %s; at its own Xác Nhận the "
                        "label %s, the frame right of it %s, the corner %s\n",
                        fixed3(in_band.label_score).c_str(), fixed3(own.label_score).c_str(),
                        fixed3(own.edge_score).c_str(), fixed3(own.corner_score).c_str());
            // The question is a dialog of two buttons, which lone_ok turns down
            // whatever its label scores: that label in the band is what tells it.
            check(!band_ok && in_band.label_score < 0.95f && own.label_score >= 0.95f && own.edge_score <= 0.30f,
                  "no lone Xác Nhận in its message's band; its own Xác Nhận has no box's frame right of it -- "
                  "label " + fixed3(own.label_score) + ", frame " + fixed3(own.edge_score));
            // Its Hủy Bỏ lit by the pointer over it: still no lone Xác Nhận.
            Frame lit = f;
            Pretend::light(lit, ScreenRect{ pair.x + pair.w / 2, pair.y, pair.w - pair.w / 2, pair.h });
            check(!bag::lone_ok(lit, k.store, bag::message_band(pair)),
                  "nor with its Hủy Bỏ lit: never taken for a message's button");
        }
    }
    {
        // The message after its Xác Nhận (06:44:47): "Bạn đã nhận được hiệu
        // ứng Kinh nghiệm tăng 10." over a lone Xác Nhận.
        Frame f = scene(1024, 768, 40);
        if (!put_back(f, dir, "charm_notice")) {
            skip("no cut of the charm message here\n");
        } else {
            const auto whole = TemplateMatcher::find(f, ScreenRect{ 0, 0, f.width, f.height },
                                                     *k.store.get("grabbed/dialog_ok"), -1.0f);
            std::printf("  [info] the charm message: grabbed/dialog_ok whole at %d,%d, %s\n", whole.at.x, whole.at.y,
                        fixed3(whole.score).c_str());
            check(bag::find_dialogs(f, k.store).empty() && bag::notice_up(f, k.store),
                  "no dialog of two buttons, and a message is up -- dialog_ok whole " + fixed3(whole.score));
            // Its lone Xác Nhận, where the question's Hủy Bỏ was: what a run
            // clicks to close it.
            ScreenRect at;
            bag::LoneOk how;
            const bool found = bag::lone_ok(f, k.store, bag::message_band(pair), &at, &how);
            std::printf("  [info] the charm message's Xác Nhận: label at %d,%d, %s; the frame right of it %s; the "
                        "corner %s (pair at %d,%d)\n",
                        at.x, at.y, fixed3(how.label_score).c_str(), fixed3(how.edge_score).c_str(),
                        fixed3(how.corner_score).c_str(), pair.x, pair.y);
            check(found && how.label_score >= 0.98f && how.edge_score >= 0.95f && how.corner_score >= 0.97f &&
                      at.x == 561 && at.y == 401 && bag::message_up(f, k.store),
                  "its lone Xác Nhận found in the band of the question's pair: label " + fixed3(how.label_score) +
                      ", frame " + fixed3(how.edge_score) + ", corner " + fixed3(how.corner_score) + ", at " +
                      std::to_string(at.x) + "," + std::to_string(at.y));
            Frame lit = f;
            Pretend::light(lit, ScreenRect{ at.x - 4, at.y - 4, 42, 21 });
            check(!bag::lone_ok(lit, k.store, bag::message_band(pair)),
                  "and not with the pointer over it, lit: a lit label is not clicked again");
        }
    }

    // The bag, Tiêu Dùng: 8871081 in row 1, cell 2 -- 63 before the use at
    // 06:44, 64 before the one at 23:12, 62 after the one at 06:44.
    ItemCatalog catalog;
    if (!charm_catalog(catalog)) {
        skip("the item catalog's charms are not installed here\n");
        return;
    }
    const BagLayout l;
    const float min_score = CleanSettings{}.min_score;
    const TrashList purple = charm_list(catalog, { kPurpleCard }), blue = charm_list(catalog, { kBlueCard });
    const struct {
        const char* what;
        int         count;
        const char* when;
    } bags[] = { { "charm_baga", 63, "06:44:46, before its use" },
                 { "charm_bagb", 64, "23:12:25, before its use" },
                 { "charm_bagc", 62, "06:44:47, after it" } };
    for (const auto& b : bags) {
        Frame f = scene(1024, 768, 40);
        if (!put_back(f, dir, b.what)) {
            skip("no %s cut here\n", b.what);
            continue;
        }
        const BagView v = bag::find(f, k.store, l);
        const auto slots = v.slots(l);
        if (!v.found || slots.size() < 2) {
            check(false, std::string(b.when) + ": the bag found -- not found");
            continue;
        }
        const ScreenRect s = slots[1];
        const int count = k.counts.read(f, s);
        const TrashList::Verdict kp = purple.judge(f, s, min_score), kb = blue.judge(f, s, min_score);
        std::printf("  [info] %s: strip at %d,%d, tab %d, first row %d of %d; row 1 cell 2 at %d,%d reads %d; 81 "
                    "chosen: drawing %s, %s; 21 chosen: %s\n",
                    b.when, v.strip.x, v.strip.y, v.selected_tab, v.first_row, v.total_rows, s.x, s.y, count,
                    fixed3(kp.item >= 0 ? kp.score : kp.like_score).c_str(), colour_text(kp).c_str(),
                    taken_text(blue, kb).c_str());
        check(v.selected_tab == kUseTab && count == b.count,
              std::string(b.when) + ": the bag on Tiêu Dùng, row 1 cell 2 reading " + std::to_string(b.count) +
                  " -- read " + std::to_string(count));
        check(kp.item >= 0 && purple.items[static_cast<std::size_t>(kp.item)].id == kPurpleCard &&
                  kp.colour_id == kPurpleCard && kp.colour >= 0.94f && kp.colour_next <= 0.85f &&
                  kp.colour_mean_off >= 0 && kp.colour_mean_off <= 10,
              std::string("  8871081 chosen: the slot is 8871081's, by its colour -- ") + colour_text(kp));
        check(kb.item < 0 && kb.doubt == TrashList::Verdict::Doubt::None && kb.colour_id == kPurpleCard,
              std::string("  8871021 chosen: the slot left alone, a charm of 8871081's colour -- ") +
                  taken_text(blue, kb) + ", " + colour_text(kb));
    }
}

// --------------------------------------- charms: a stack of one, no count printed

namespace {

/// The seven charms of one in the live client's bag of 2026-11-03 19:54, by
/// their slot on Tiêu Dùng's first page -- row 2 cell 6, then row 3 -- each
/// with no number printed over the light rim of its card or diamond, and each
/// left alone by 2026.11.03b ("không đọc được số lượng trên ô đồ"), the owner
/// having chosen all eight colours.
struct Single {
    int         slot;
    const char* id;
};
const Single kLiveSingles[] = { { 11, "spend/buff/8871071" }, { 12, "spend/buff/8871061" },
                                { 13, "spend/buff/8871051" }, { 14, "spend/buff/8871041" },
                                { 15, "spend/buff/8871031" }, { 16, "spend/buff/8871021" },
                                { 17, "spend/buff/8871011" } };
const char* const kBlueDiamond = "spend/buff/8871071";
const char* const kPinkDiamond = "spend/buff/8871061";
const char* const kGreenCard = "spend/buff/8871041";

/// "r2c6" for slot 11 of a page.
std::string cell_text(int slot) { return "r" + std::to_string(slot / 6 + 1) + "c" + std::to_string(slot % 6 + 1); }

std::string fixed1(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(v));
    return buf;
}

/// A charm colour's catalog icon whole, as the list a pass builds has it
/// (TrashList::Item::bare), chosen or a rival; null when it has none.
const Frame* bare_in(const TrashList& list, const std::string& id) {
    for (const TrashList::Item& it : list.items)
        if (it.id == id && it.bare.valid()) return &it.bare;
    for (const TrashList::Item& r : list.rivals)
        if (r.id == id && r.bare.valid()) return &r.bare;
    return nullptr;
}

/// The game's digits, loaded once, drawn as the client prints a count: each
/// digit kDigitStep on, its strokes in one colour and -- `ring` 0 or more --
/// first the pixels round them, side and corner, in that grey.
struct Digits {
    Frame d[10];
    bool  ok = true;
    explicit Digits(const std::string& font_dir) {
        for (int i = 0; i < 10; ++i)
            ok = load_png(font_dir + "\\lower\\" + std::to_string(i) + ".png", d[i]) && d[i].valid() && ok;
    }
    void draw(Frame& f, int x, int y, const std::string& text, int red, int green, int blue, int ring) const {
        for (const char c : text) {
            const Frame& glyph = d[c - '0'];
            for (int pass = ring >= 0 ? 0 : 1; pass < 2; ++pass)
                for (int gy = 0; gy < glyph.height; ++gy)
                    for (int gx = 0; gx < glyph.width; ++gx) {
                        std::uint8_t r = 0, g = 0, b = 0;
                        glyph.get(gx, gy, r, g, b);
                        if ((r + g + b) / 3 <= 128) continue;
                        if (pass == 0) {
                            for (int oy = -1; oy <= 1; ++oy)
                                for (int ox = -1; ox <= 1; ++ox) put_px(f, x + gx + ox, y + gy + oy, ring, ring, ring);
                        } else {
                            put_px(f, x + gx, y + gy, red, green, blue);
                        }
                    }
            x += kDigitStep;
        }
    }
};

/// The counts drawn over a stack of one's corner: every one from 2 to 99, and
/// some of three and four digits, 1s among them.
std::vector<std::string> printed_numbers() {
    std::vector<std::string> out;
    for (int n = 2; n <= 99; ++n) out.push_back(std::to_string(n));
    for (const char* n : { "100", "101", "110", "111", "199", "999", "1000", "1111", "9999" }) out.emplace_back(n);
    return out;
}

/// The live bag of 19:54 put back where it was on a frame of the client's
/// size, and the bag as found there: false when its cut is not here.
bool singles_bag(Frame& f, BagView& v, std::vector<ScreenRect>& slots) {
    const std::string dir = charm_cuts_dir();
    f = scene(1024, 768, 40);
    if (dir.empty() || !put_back(f, dir, "charm_bag_singles")) return false;
    const BagLayout l;
    v = bag::find(f, kit().store, l);
    slots = v.slots(l);
    return true;
}

/// Slot `s` of `f` on a small frame of its own, a few pixels round it: that
/// frame, and the slot on it.
std::pair<Frame, ScreenRect> slot_alone(const Frame& f, const ScreenRect& s) {
    constexpr int kRim = 6;
    return { bag::cut(f, ScreenRect{ s.x - kRim, s.y - kRim, s.w + 2 * kRim, s.h + 2 * kRim }),
             ScreenRect{ kRim, kRim, s.w, s.h } };
}

/// A charm run on a pretend bag of the client's own charm icons, each drawn as
/// the bag draws it (slot_tile_of) and a count of 2 or more printed over it:
/// `in_bag` what lies where, `ticked` the ids chosen -- the list a pass builds
/// for them. The game's message after Xác Nhận comes and is closed, as on the
/// live client; no book opens.
struct CharmBagRun {
    struct In {
        int         slot;
        const char* id;
        int         count;
    };
    Pass p;
    BagCleaner::Result r;
    std::int64_t took_ms = 0;
    CharmBagRun(const ItemCatalog& catalog, const std::vector<In>& in_bag, const std::vector<std::string>& ticked) {
        make_bag(p.game);
        p.game.book_opens = false;
        p.game.reply = Pretend::Reply::Charm;
        p.game.notice_after = true;
        p.game.lit = true;
        for (const In& in : in_bag) {
            Pretend::Item it{ in.slot, 1, in.count };
            it.icon = slot_tile_of(catalog.find(in.id)->icon);
            p.game.items.push_back(it);
        }
        p.list = charm_list(catalog, ticked);
        p.mode = BagCleaner::Mode::CharmsOnly;
    }
    BagCleaner::Result run() {
        const auto from = p.game.time.t;
        r = p.run();
        took_ms = std::chrono::duration_cast<std::chrono::milliseconds>(p.game.time.t - from).count();
        return r;
    }
    /// How each try came out, in order: U used, M taken for used, N not used.
    std::string tries() const {
        std::string out;
        for (const auto& t : r.charm_tries)
            out += t.how == CharmClock::How::Used ? 'U' : t.how == CharmClock::How::MaybeUsed ? 'M' : 'N';
        return out;
    }
};

} // namespace

void test_charm_singles_live() {
    section("Bùa: a stack of one, nothing printed over a card's white rim, on the live client's bag (19:54)");
    if (skip_without_kit()) return;
    Frame f;
    BagView v;
    std::vector<ScreenRect> slots;
    if (!singles_bag(f, v, slots)) {
        skip("no cut of the live bag of 2026-11-03 19:54 here (handoff\\charms\\testdata\\charm_bag_singles_*)\n");
        return;
    }
    ItemCatalog catalog;
    if (!charm_catalog(catalog)) {
        skip("the item catalog's charms are not installed here\n");
        return;
    }
    const Kit& k = kit();
    const BagLayout l;
    const float min_score = CleanSettings{}.min_score;
    check(v.found && v.strip.x == 134 && v.strip.y == 293 && v.selected_tab == kUseTab && v.first_row == 0 &&
              slots.size() == 24,
          "the bag found at 134,293, on Tiêu Dùng, its first row on top -- at " + std::to_string(v.strip.x) + "," +
              std::to_string(v.strip.y) + ", tab " + std::to_string(v.selected_tab) + ", row " +
              std::to_string(v.first_row) + ", " + std::to_string(slots.size()) + " slots");
    if (!v.found || slots.size() < 24) return;
    // All eight chosen, as the owner had them.
    const TrashList list = charm_list(catalog, std::vector<std::string>(std::begin(kCharmIds), std::end(kCharmIds)));
    int icons = 0;
    for (const char* id : kCharmIds) icons += bare_in(list, id) != nullptr ? 1 : 0;
    check(icons == 8, "each of the eight colours has its catalog icon whole on the list -- " + std::to_string(icons));
    if (icons < 8) return;

    {
        // The seven of one: each its own colour's, unreadable to read as in
        // 11.03b, and one by its bare icon -- by no other colour's.
        std::string wrong, info;
        float worst_fit = 0.0f, other_fit = 255.0f;
        for (const Single& one : kLiveSingles) {
            const ScreenRect& s = slots[static_cast<std::size_t>(one.slot)];
            const Frame& icon = *bare_in(list, one.id);
            const TrashList::Verdict kv = list.judge(f, s, min_score);
            const bool judged = kv.item >= 0 && list.items[static_cast<std::size_t>(kv.item)].id == one.id;
            const int read = k.counts.read(f, s);
            bool bare = false;
            const int count = k.counts.read_charm(f, s, &icon, l, &bare);
            int unlike = -1;
            float fit = -1.0f;
            const bool corner = bag::bare_corner(f, s, icon, l, &unlike, &fit);
            float own_fit = -1.0f;
            const bool fits = bag::icon_fits(f, s, icon, l, &own_fit);
            bool other_reads = false;
            for (const char* other : kCharmIds) {
                if (one.id == std::string(other)) continue;
                float of = 255.0f;
                bag::icon_fits(f, s, *bare_in(list, other), l, &of);
                other_fit = std::min(other_fit, of);
                other_reads = other_reads ||
                              k.counts.read_charm(f, s, bare_in(list, other), l) != CountReader::kUnreadable;
            }
            worst_fit = std::max(worst_fit, fit);
            info += " " + cell_text(one.slot) + " " + last2(one.id) + " fit " + fixed1(fit) + ", " +
                    std::to_string(unlike) + " unlike;";
            if (!judged || read != CountReader::kUnreadable || count != 1 || !bare || !corner || unlike != 0 ||
                fit > 1.0f || !fits || own_fit != fit || other_reads)
                wrong += " | " + cell_text(one.slot) + " " + last2(one.id) + ": " + taken_text(list, kv) + ", read " +
                         std::to_string(read) + ", read_charm " + std::to_string(count) + (bare ? " (bare)" : "") +
                         ", fit " + fixed1(fit) + ", " + std::to_string(unlike) + " unlike" +
                         (other_reads ? ", another colour's icon reads" : "");
        }
        std::printf("  [info] the seven of one:%s another colour's icon fits one at %s at best\n", info.c_str(),
                    fixed1(other_fit).c_str());
        check(wrong.empty(),
              "row 2 cell 6 and row 3 cells 1-6: each taken for its own colour; read says kUnreadable, as 11.03b "
              "did; read_charm with that colour's icon 1, the corner bare -- 0 pixels unlike, fit " +
                  fixed1(worst_fit) + " at worst -- and with any other colour's icon kUnreadable" + wrong);
        check(other_fit >= 40.0f, "another colour's icon fits a stack of one at " + fixed1(other_fit) +
                                      " at best, against kBareFitMean " + fixed1(bag::kBareFitMean));
        // No icon to go by: no stack of one there.
        const ScreenRect& s = slots[11];
        const Frame none;
        check(k.counts.read_charm(f, s, nullptr, l) == CountReader::kUnreadable &&
                  k.counts.read_charm(f, s, &none, l) == CountReader::kUnreadable,
              "with no icon, or an empty one, row 2 cell 6 is no stack of one: kUnreadable, as read says");
    }
    {
        // 8871081 with its count printed, here and on the earlier cuts: the
        // number, never its icon bare.
        const Frame& purple = *bare_in(list, kPurpleCard);
        const auto printed = [&](const Frame& g, const ScreenRect& s, int want, const char* when) {
            bool bare = true;
            const int count = k.counts.read_charm(g, s, &purple, l, &bare);
            int unlike = -1;
            float fit = -1.0f;
            const bool corner = bag::bare_corner(g, s, purple, l, &unlike, &fit);
            std::printf("  [info] %s, row 1 cell 2: read_charm %d; the corner %d pixel(s) unlike 8871081's icon "
                        "(fit %s)\n",
                        when, count, unlike, fixed1(fit).c_str());
            check(count == want && !bare && !corner && unlike >= 60,
                  std::string(when) + ": 8871081 x" + std::to_string(want) + " reads " + std::to_string(count) +
                      ", not bare -- " + std::to_string(unlike) + " pixels unlike its icon, fit " + fixed1(fit));
        };
        printed(f, slots[1], 62, "19:54:46");
        const std::string dir = charm_cuts_dir();
        const struct {
            const char* what;
            int         count;
            const char* when;
        } bags[] = { { "charm_baga", 63, "06:44:46" }, { "charm_bagb", 64, "23:12:25" },
                     { "charm_bagc", 62, "06:44:47" } };
        for (const auto& b : bags) {
            Frame g = scene(1024, 768, 40);
            if (!put_back(g, dir, b.what)) {
                skip("no %s cut here\n", b.what);
                continue;
            }
            const BagView w = bag::find(g, k.store, l);
            const auto ws = w.slots(l);
            if (!w.found || ws.size() < 2) {
                check(false, std::string(b.when) + ": the bag found -- not found");
                continue;
            }
            printed(g, ws[1], b.count, b.when);
        }
    }
    {
        // Every count printed in this bag reads the same by read_charm,
        // whatever charm's icon it is given -- and with none.
        std::string wrong, read_here;
        int numbers = 0;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const int n = k.counts.read(f, slots[i]);
            if (n < 2) continue;
            ++numbers;
            read_here += " " + std::to_string(n);
            for (const char* id : kCharmIds)
                if (k.counts.read_charm(f, slots[i], bare_in(list, id), l) != n)
                    wrong += " | " + cell_text(static_cast<int>(i)) + " with " + last2(id) + "'s icon";
            if (k.counts.read_charm(f, slots[i], nullptr, l) != n)
                wrong += " | " + cell_text(static_cast<int>(i)) + " with none";
        }
        check(numbers == 16 && wrong.empty(),
              "the " + std::to_string(numbers) + " counts printed in the bag (" + read_here +
                  ") read the same by read_charm with any charm's icon, or none" + wrong);
    }
    {
        // The silver box of one in row 4 cell 2: white in its corner, and no
        // charm -- no charm's icon makes it one.
        const ScreenRect& s = slots[19];
        bool any = false;
        for (const char* id : kCharmIds)
            any = any || k.counts.read_charm(f, s, bare_in(list, id), l) != CountReader::kUnreadable;
        check(k.counts.read(f, s) == CountReader::kUnreadable && !any,
              "row 4 cell 2, a box of one that is no charm: kUnreadable with every charm's icon");
    }
}

void test_charm_singles_printed() {
    section("Bùa: a count drawn over a stack of one's corner is never taken for one -- white, dim, grey or coloured");
    if (skip_without_kit()) return;
    Frame f;
    BagView v;
    std::vector<ScreenRect> slots;
    if (!singles_bag(f, v, slots) || !v.found || slots.size() < 24) {
        skip("no cut of the live bag of 2026-11-03 19:54 here (handoff\\charms\\testdata\\charm_bag_singles_*)\n");
        return;
    }
    ItemCatalog catalog;
    if (!charm_catalog(catalog)) {
        skip("the item catalog's charms are not installed here\n");
        return;
    }
    const Kit& k = kit();
    const Digits digits(k.font_dir);
    check(digits.ok, "the game's digits, to draw counts with");
    if (!digits.ok) return;
    const BagLayout l;
    const TrashList list = charm_list(catalog, std::vector<std::string>(std::begin(kCharmIds), std::end(kCharmIds)));
    const std::vector<std::string> numbers = printed_numbers();
    {
        // Each number over each of the seven, where the client prints it and a
        // pixel off each way.
        const struct {
            const char* what;
            int         r, g, b, ring;
        } inks[] = { { "white, outline 10 (print_number's)", 250, 250, 250, 10 },
                     { "white, outline 136 (the client's lightest)", 255, 255, 255, 136 },
                     { "dim 187, no outline", 187, 187, 187, -1 },
                     { "grey 150, no outline: read sees no white", 150, 150, 150, -1 } };
        const int shifts[][2] = { { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
        int least_all = 1 << 20;
        for (const auto& ink : inks) {
            int drawn = 0, ones = 0, bares = 0, right = 0, least = 1 << 20;
            std::string least_at;
            for (const Single& one : kLiveSingles) {
                const Frame& icon = *bare_in(list, one.id);
                const auto [alone, s] = slot_alone(f, slots[static_cast<std::size_t>(one.slot)]);
                for (const std::string& number : numbers)
                    for (const auto& d : shifts) {
                        Frame g = alone;
                        digits.draw(g, s.x + 2 + d[0], s.y + 3 + d[1], number, ink.r, ink.g, ink.b, ink.ring);
                        ++drawn;
                        const int n = k.counts.read_charm(g, s, &icon, l);
                        ones += n == 1 ? 1 : 0;
                        right += n == std::stoi(number) ? 1 : 0;
                        int unlike = -1;
                        bares += bag::bare_corner(g, s, icon, l, &unlike) ? 1 : 0;
                        if (unlike >= 0 && unlike < least) {
                            least = unlike;
                            least_at = last2(one.id) + " " + number + " shifted " + std::to_string(d[0]) + "," +
                                       std::to_string(d[1]);
                        }
                    }
            }
            least_all = std::min(least_all, least);
            std::printf("  [info] %s: %d drawn, %d read as drawn, the least unlike %d (%s)\n", ink.what, drawn, right,
                        least, least_at.c_str());
            check(drawn == 7 * 107 * 5 && ones == 0 && bares == 0 && least >= 8,
                  std::string(ink.what) + ": " + std::to_string(drawn) + " counts (2 to 99 and nine of 100 to 9999) over the seven, a "
                  "pixel off each way: read_charm never 1 (" + std::to_string(ones) + "), the corner never bare (" +
                  std::to_string(bares) + "), " + std::to_string(least) + " pixels unlike its icon at the least");
        }
        std::printf("  [info] every count drawn: %d pixels unlike the icon at the least\n", least_all);
    }
    {
        // 8871081's corner has no white: a count there in a colour read does
        // not take for white is still a count, never a stack of one.
        const Frame& purple = *bare_in(list, kPurpleCard);
        const auto [plain, s] = charm_slot(catalog.find(kPurpleCard)->icon);
        bool bare = false;
        const int alone = k.counts.read_charm(plain, s, &purple, l, &bare);
        check(alone == 1 && bare, "8871081 x1 drawn as the bag draws it: read_charm 1, by its bare icon -- " +
                                      std::to_string(alone));
        const struct {
            const char* what;
            int         r, g, b;
        } inks[] = { { "grey 150", 150, 150, 150 }, { "grey 110", 110, 110, 110 }, { "red", 220, 40, 40 },
                     { "yellow", 230, 210, 40 } };
        for (const auto& ink : inks) {
            int ones = 0, least = 1 << 20;
            for (int n = 2; n <= 99; ++n) {
                Frame g = plain;
                digits.draw(g, s.x + 2, s.y + 3, std::to_string(n), ink.r, ink.g, ink.b, -1);
                ones += k.counts.read_charm(g, s, &purple, l) == 1 ? 1 : 0;
                int unlike = -1;
                bag::bare_corner(g, s, purple, l, &unlike);
                if (unlike >= 0) least = std::min(least, unlike);
            }
            check(ones == 0 && least >= 8,
                  std::string("8871081 with a count in ") + ink.what + ", 2 to 99: never read as one -- " +
                      std::to_string(ones) + " were; " + std::to_string(least) + " pixels unlike at the least");
        }
    }
    {
        // The seven a little off what was measured: in 16-bit colour, with
        // noise of 12, the whole slot a pixel right -- still bare; lit all
        // over, as the pointer over it lights it -- refused.
        std::string wrong, info;
        std::uint32_t seed = 1103;
        const auto noise = [&seed] {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<int>((seed >> 16) % 25u) - 12;
        };
        float worst16 = 0.0f, worst_noise = 0.0f;
        for (const Single& one : kLiveSingles) {
            const Frame& icon = *bare_in(list, one.id);
            const auto [alone, s] = slot_alone(f, slots[static_cast<std::size_t>(one.slot)]);
            Frame dull = alone, noisy = alone, right = alone, lit = alone;
            for (int y = s.y; y < s.bottom(); ++y)
                for (int x = s.x; x < s.right(); ++x) {
                    std::uint8_t r = 0, g = 0, b = 0;
                    alone.get(x, y, r, g, b);
                    put_px(dull, x, y, r & 0xF8, g & 0xFC, b & 0xF8);
                    put_px(noisy, x, y, std::clamp(r + noise(), 0, 255), std::clamp(g + noise(), 0, 255),
                           std::clamp(b + noise(), 0, 255));
                    alone.get(x - 1, y, r, g, b);
                    put_px(right, x, y, r, g, b);
                }
            Pretend::light(lit, s);
            float fit16 = -1.0f, fit_noise = -1.0f, fit_right = -1.0f, fit_lit = -1.0f;
            int u16 = -1, u_noise = -1, u_right = -1, u_lit = -1;
            const bool b16 = bag::bare_corner(dull, s, icon, l, &u16, &fit16);
            const bool b_noise = bag::bare_corner(noisy, s, icon, l, &u_noise, &fit_noise);
            const bool b_right = bag::bare_corner(right, s, icon, l, &u_right, &fit_right);
            const bool b_lit = bag::bare_corner(lit, s, icon, l, &u_lit, &fit_lit);
            const int lit_reads = k.counts.read_charm(lit, s, &icon, l);
            worst16 = std::max(worst16, fit16);
            worst_noise = std::max(worst_noise, fit_noise);
            info += " " + last2(one.id) + " " + fixed1(fit16) + "/" + fixed1(fit_noise) + "/" + fixed1(fit_lit);
            if (!b16 || !b_noise || !b_right || b_lit || lit_reads == 1)
                wrong += " | " + last2(one.id) + ": 16-bit " + std::to_string(u16) + ", noise " +
                         std::to_string(u_noise) + ", a pixel right " + std::to_string(u_right) + ", lit " +
                         std::to_string(u_lit) + " (" + std::to_string(lit_reads) + ")";
        }
        std::printf("  [info] fit in 16-bit / with noise / lit:%s\n", info.c_str());
        check(wrong.empty() && worst16 <= bag::kBareFitMean && worst_noise <= bag::kBareFitMean,
              "the seven in 16-bit colour (fit " + fixed1(worst16) + " at worst), with noise of 12 (" +
                  fixed1(worst_noise) + ") and a pixel right still bare; lit all over, not -- refused" + wrong);
    }
}

void test_charm_singles_run() {
    section("Bùa: stacks of one used in a charm run -- the live bag of 19:54 rebuilt -- and what is not a use");
    if (skip_without_kit()) return;
    ItemCatalog catalog;
    if (!charm_catalog(catalog)) {
        skip("the item catalog's charms are not installed here\n");
        return;
    }
    BagCleaner::forget_said_once();
    const std::vector<std::string> all(std::begin(kCharmIds), std::end(kCharmIds));
    {
        // The live bag of 19:54: 8871081 x62 in row 1 cell 2, the seven of one
        // in row 2 cell 6 and row 3; all eight chosen, the GUI's deadline.
        std::vector<CharmBagRun::In> in_bag = { { 1, kPurpleCard, 62 } };
        for (const Single& one : kLiveSingles) in_bag.push_back({ one.slot, one.id, 1 });
        CharmBagRun c(catalog, in_bag, all);
        const int deadline = BagCleaner::charm_deadline_ms(8);
        c.p.deadline_ms = deadline;
        c.run();
        bool gone = true;
        for (std::size_t i = 1; i < c.p.game.items.size(); ++i) gone = gone && c.p.game.items[i].gone();
        std::set<std::string> ids;
        for (const auto& t : c.r.charm_tries) ids.insert(t.id);
        std::printf("  [info] the live bag of 19:54 rebuilt, eight kinds used: %lld ms of the pretend game's time, "
                    "the deadline %d\n",
                    static_cast<long long>(c.took_ms), deadline);
        check(c.p.game.right_clicks == 8 && c.p.game.confirms == 8 && c.r.charms_used == 8 &&
                  c.p.game.items[0].count == 61 && gone && c.tries() == "UUUUUUUU" && ids.size() == 8 &&
                  c.r.charms_unread == 0 && c.r.trouble.empty() && c.p.game.messages_closed == 8 &&
                  c.p.game.keys == 0 && c.p.game.right_clicks_with_message == 0,
              "all eight chosen: one run right-clicks each once -- 81 to 61, the seven of one seen gone -- each "
              "message closed, no key -- " + std::to_string(c.p.game.right_clicks) + " right-clicks, " +
                  std::to_string(c.r.charms_used) + " used, tries " + c.tries() + ", " + c.r.trouble);
        check(c.took_ms < deadline, "inside the deadline of eight kinds: " + std::to_string(c.took_ms) + " of " +
                                        std::to_string(deadline) + " ms");
    }
    {
        // Two kinds, x62 and x1: both in one run, the one seen gone on two looks.
        CharmBagRun c(catalog, { { 1, kPurpleCard, 62 }, { 11, kBlueDiamond, 1 } }, { kPurpleCard, kBlueDiamond });
        c.run();
        check(c.p.game.right_clicks == 2 && c.r.charms_used == 2 && c.p.game.items[0].count == 61 &&
                  c.p.game.items[1].gone() && c.tries() == "UU" && c.r.trouble.empty(),
              "8871081 x62 and 8871071 x1: both used in one run -- tries " + c.tries() + ", " + c.r.trouble);
    }
    {
        // 8871081 x1: no white in its corner, still its bare icon.
        CharmBagRun c(catalog, { { 1, kPurpleCard, 1 } }, { kPurpleCard });
        c.run();
        check(c.p.game.right_clicks == 1 && c.r.charms_used == 1 && c.p.game.items[0].gone() && c.tries() == "U",
              "8871081 x1: used, seen gone -- tries " + c.tries() + ", " + c.r.trouble);
    }
    {
        // 2 to 1: the 1 read by the bare icon after the use -- one less, used.
        CharmBagRun c(catalog, { { 17, kGreyCard, 2 } }, { kGreyCard });
        c.run();
        check(c.p.game.right_clicks == 1 && c.r.charms_used == 1 && c.p.game.items[0].count == 1 &&
                  c.tries() == "U" && c.p.says("; còn 1") && c.r.use_unsure == 0,
              "8871011 x2, a white-rim card: used, one left, seen one less (11.03b could not read the one) -- " +
                  c.p.last());
    }
    {
        // Xác Nhận and the single still there, its icon bare: the stack as it
        // was -- taken for used, the run stopped, never tried again soon.
        CharmBagRun c(catalog, { { 11, kBlueDiamond, 1 } }, { kBlueDiamond });
        c.p.game.registers = false;
        c.run();
        check(c.p.game.right_clicks == 1 && c.p.game.confirms == 1 && c.p.game.items[0].count == 1 &&
                  c.r.charms_used == 0 && c.tries() == "M" && c.r.failed == 1 &&
                  c.p.says("đã bấm Xác Nhận mà số lượng bùa không giảm") && !c.r.trouble.empty(),
              "8871071 x1 still there after Xác Nhận: perhaps used, the run stopped -- " + c.r.trouble);
    }
    {
        // The bag closes up its gaps after the single is used: another single
        // moves into its slot, drawn alike. No count of the colour used reads
        // there -- no answer, taken for used, never the stack as it was.
        const struct {
            const char* used;
            const char* next;
        } pairs[] = { { kGreenCard, kPurpleCard }, { kBlueDiamond, kPinkDiamond } };
        for (const auto& two : pairs) {
            CharmBagRun c(catalog, { { 11, two.used, 1 }, { 12, two.next, 1 } }, { two.used });
            c.p.game.compacts = true;
            const BagLayout l;
            const Frame drawn = c.p.game.render();
            const float like =
                bag::alike(bag::cut(drawn, c.p.game.slot_rect(11)), bag::cut(drawn, c.p.game.slot_rect(12)), l);
            c.run();
            const std::string why = c.r.charm_tries.empty() ? std::string() : c.r.charm_tries[0].why;
            std::printf("  [info] %s x1 used, %s x1 moved into its slot: alike %s\n", last2(two.used).c_str(),
                        last2(two.next).c_str(), fixed3(like).c_str());
            check(like >= 0.80f && c.p.game.right_clicks == 1 && c.p.game.items[0].gone() && c.tries() == "M" &&
                      c.r.use_unsure == 1 && c.r.failed == 0 &&
                      why.find("không đọc lại được số lượng trên ô đồ") != std::string::npos,
                  last2(two.used) + " x1 used and " + last2(two.next) + " x1 moved into its slot (alike " +
                      fixed3(like) + "): no answer, taken for used -- never the stack as it was -- " + why);
        }
    }
    {
        // Something white over the corner that its icon does not have: no
        // count read, and no stack of one -- not right-clicked.
        CharmBagRun c(catalog, { { 11, kBlueDiamond, 1 } }, { kBlueDiamond });
        c.p.game.items[0].smudged = true;
        c.run();
        check(c.p.game.right_clicks == 0 && c.r.charms_unread == 1 && c.r.charms_seen == 1 &&
                  c.r.charm_tries.empty() && c.p.says("không đọc được số lượng trên ô đồ"),
              "8871071 x1 with white over its corner its icon has not: left alone, counted unread -- " + c.p.last());
    }
    {
        // A right-click that brings no question: the single as it was, not used.
        CharmBagRun c(catalog, { { 11, kBlueDiamond, 1 } }, { kBlueDiamond });
        c.p.game.reply = Pretend::Reply::Nothing;
        c.run();
        check(c.p.game.right_clicks == 1 && c.p.game.confirms == 0 && c.r.charms_used == 0 && c.tries() == "N" &&
                  c.r.use_unsure == 0 && c.p.says("không thấy câu hỏi nào"),
              "8871071 x1 and no question: not used, the stack as it was -- " + c.p.last());
    }
    {
        // The game takes the first single only as its message shows, 1.2 s on:
        // seen gone on a later look, and the second used after it.
        CharmBagRun c(catalog, { { 11, kBlueDiamond, 1 }, { 12, kPinkDiamond, 1 } }, { kBlueDiamond, kPinkDiamond });
        c.p.game.notice_late_first_ms = 1200;
        c.p.game.notice_takes = true;
        c.run();
        check(c.r.charms_used == 2 && c.p.game.items[0].gone() && c.p.game.items[1].gone() && c.tries() == "UU" &&
                  c.p.game.messages_closed == 2 && c.r.trouble.empty() && c.p.game.right_clicks_with_message == 0,
              "a single taken late, as its message shows: seen gone, and the next used -- tries " + c.tries() + ", " +
                  c.r.trouble);
    }

    // The packets' bag on a single: another count, or none, stops it.
    struct Asked {
        std::string colour;
        int slot = -1, count = -1;
    };
    const auto with_packets = [&](const std::vector<CharmBagRun::In>& in_bag, const std::vector<std::string>& ticked,
                                  BagCleaner::PacketSays says, const std::string& held, std::vector<Asked>& asked) {
        auto c = std::make_unique<CharmBagRun>(catalog, in_bag, ticked);
        c->p.packets = [&asked, says, held](const std::string& colour, int j, int count, std::string& said) {
            asked.push_back(Asked{ colour, j, count });
            said = held;
            return says;
        };
        c->run();
        return c;
    };
    using Says = BagCleaner::PacketSays;
    {
        std::vector<Asked> asked;
        const auto c =
            with_packets({ { 11, kBlueDiamond, 1 } }, { kBlueDiamond }, Says::CountDiffers, "8871071 x2", asked);
        check(c->p.game.right_clicks == 0 && c->r.charms_vetoed == 1 && c->r.charm_tries.empty() &&
                  c->p.says("theo gói tin ô này có 8871071 x2, mà màn hình thấy 1 cái") && asked.size() == 1 &&
                  asked[0].slot == 11 && asked[0].count == 1 && asked[0].colour == kBlueDiamond,
              "8871071 x1 by its bare icon, the packets' slot 11 holding 2: not right-clicked, counted, said -- " +
                  c->p.last());
        std::vector<Asked> again;
        const auto d =
            with_packets({ { 11, kBlueDiamond, 1 } }, { kBlueDiamond }, Says::CountDiffers, "8871071 x2", again);
        check(d->p.game.right_clicks == 0 && d->r.charms_vetoed == 1 && !d->p.says("theo gói tin"),
              "and the next run the same: left alone, not said again");
    }
    {
        std::vector<Asked> asked;
        const auto c = with_packets({ { 11, kBlueDiamond, 1 } }, { kBlueDiamond }, Says::CountDiffers, "empty", asked);
        check(c->p.game.right_clicks == 0 && c->r.charms_vetoed == 1 &&
                  c->p.says("theo gói tin ô này trống, mà màn hình thấy 1 cái"),
              "the packets' slot empty: not right-clicked, said as empty -- " + c->p.last());
    }
    {
        std::vector<Asked> asked;
        const auto c = with_packets({ { 11, kBlueDiamond, 1 } }, { kBlueDiamond }, Says::Other, "8871061 x1", asked);
        check(c->p.game.right_clicks == 0 && c->r.charms_vetoed == 1 && c->p.says("là món khác (8871061 x1)"),
              "the packets naming another item there: not right-clicked -- " + c->p.last());
    }
    for (const Says says : { Says::Agrees, Says::Nothing }) {
        std::vector<Asked> asked;
        const auto c = with_packets({ { 11, kBlueDiamond, 1 } }, { kBlueDiamond }, says,
                                    says == Says::Agrees ? "8871071 x1" : "no current bag", asked);
        check(c->p.game.right_clicks == 1 && c->r.charms_used == 1 && c->r.charms_vetoed == 0 && asked.size() == 1,
              std::string(says == Says::Agrees ? "the packets agree" : "the packets say nothing") +
                  ": the single used, the screen deciding");
    }
    {
        // A count printed, the packets' slot holding another: not compared, as
        // in 11.03b -- most likely not the slot the screen shows.
        std::vector<Asked> asked;
        const auto c =
            with_packets({ { 1, kPurpleCard, 62 } }, { kPurpleCard }, Says::CountDiffers, "8871081 x61", asked);
        check(c->p.game.right_clicks == 1 && c->r.charms_used == 1 && c->r.charms_vetoed == 0 && asked.size() == 1 &&
                  asked[0].count == 62,
              "8871081 x62 printed, the packets' slot holding 61: used, the packets not compared");
    }
}

// --------------------------------- charms: x2 used to x1, the live bag of 22:05

namespace {

/// The eight charms on Tiêu Dùng's first page of the live client's bag of
/// 2026-11-03 22:05, by their slot, with the count before and after the use
/// that 2026.11.03b stopped at: 8871081 used first, 61 to 60, then 8871071 in
/// row 2 cell 1, 2 to 1 -- the one with no number printed over the card's
/// light rim, which it could not read ("không thấy ô đồ trống ra hay bớt đúng
/// một (không đọc lại được số lượng trên ô đồ)").
struct PairSlot {
    int         slot;
    const char* id;
    int         before, after;
};
const PairSlot kLivePair[] = { { 1, "spend/buff/8871081", 61, 60 }, { 6, "spend/buff/8871071", 2, 1 },
                               { 7, "spend/buff/8871061", 2, 2 },   { 8, "spend/buff/8871051", 2, 2 },
                               { 9, "spend/buff/8871041", 2, 2 },   { 10, "spend/buff/8871031", 2, 2 },
                               { 11, "spend/buff/8871021", 2, 2 },  { 12, "spend/buff/8871011", 2, 2 } };

/// A cut of the live bag of 22:05, `what`, put back where it was on a frame of
/// the client's size, and the bag as found there: false when its cut is not
/// here. `where` gets the cut's box.
bool pair_bag(const char* what, Frame& f, BagView& v, std::vector<ScreenRect>& slots, ScreenRect* where = nullptr) {
    const std::string dir = charm_cuts_dir();
    f = scene(1024, 768, 40);
    if (dir.empty() || !put_back(f, dir, what, where)) return false;
    const BagLayout l;
    v = bag::find(f, kit().store, l);
    slots = v.slots(l);
    return true;
}

/// The pixels of `a` and `b`, two pictures of one size, that differ at all,
/// those in `but` left out; -1 when their sizes differ.
int pixels_off(const Frame& a, const Frame& b, const ScreenRect& but = ScreenRect{}) {
    if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height) return -1;
    int n = 0;
    for (int y = 0; y < a.height; ++y)
        for (int x = 0; x < a.width; ++x) {
            if (but.contains(x, y)) continue;
            std::uint8_t ar = 0, ag = 0, ab = 0, br = 0, bg = 0, bb = 0;
            a.get(x, y, ar, ag, ab);
            b.get(x, y, br, bg, bb);
            n += ar != br || ag != bg || ab != bb ? 1 : 0;
        }
    return n;
}

} // namespace

void test_charm_pair_live() {
    section("Bùa: x2 used to x1 on the live client's bag (22:05), where 11.03b stopped -- one less by its bare icon");
    if (skip_without_kit()) return;
    Frame before, after;
    BagView vb, va;
    std::vector<ScreenRect> sb, sa;
    ScreenRect cut_b, cut_a;
    if (!pair_bag("charm_pair_before", before, vb, sb, &cut_b) ||
        !pair_bag("charm_pair_after", after, va, sa, &cut_a)) {
        skip("no cuts of the live bag of 2026-11-03 22:05 here (handoff\\charms\\testdata\\charm_pair_*)\n");
        return;
    }
    ItemCatalog catalog;
    if (!charm_catalog(catalog)) {
        skip("the item catalog's charms are not installed here\n");
        return;
    }
    const Kit& k = kit();
    const BagLayout l;
    const float min_score = CleanSettings{}.min_score;
    // Each cut the bag's tab strip, grid and scroll bar alone, as the cut of
    // 19:54: above the money line, at about y 562 on these frames, and left
    // of the character's name, at about x 275.
    const auto boxed = [](const ScreenRect& c) {
        return c.x == 11 && c.y == 382 && c.w == 258 && c.h == 176 && c.right() <= 275 && c.bottom() <= 562;
    };
    check(boxed(cut_b) && boxed(cut_a), "both cuts 258x176 from 11,382: above the money line, left of the name");
    const auto at_17_383 = [](const BagView& v, const std::vector<ScreenRect>& slots) {
        return v.found && v.strip.x == 17 && v.strip.y == 383 && v.selected_tab == kUseTab && v.first_row == 0 &&
               slots.size() == 24;
    };
    check(at_17_383(vb, sb) && at_17_383(va, sa),
          "before and after, the bag found at 17,383 -- moved from 134,293 at 19:54 -- on Tiêu Dùng, its first row on "
          "top -- at " + std::to_string(vb.strip.x) + "," + std::to_string(vb.strip.y) + " and " +
              std::to_string(va.strip.x) + "," + std::to_string(va.strip.y));
    if (!at_17_383(vb, sb) || !at_17_383(va, sa)) return;
    // All eight chosen, as the owner had them.
    const TrashList list = charm_list(catalog, std::vector<std::string>(std::begin(kCharmIds), std::end(kCharmIds)));
    int icons = 0;
    for (const char* id : kCharmIds) icons += bare_in(list, id) != nullptr ? 1 : 0;
    if (icons < 8) {
        check(false, "each of the eight colours has its catalog icon whole on the list -- " + std::to_string(icons));
        return;
    }

    {
        // The eight on both looks: each taken for its own colour, its catalog
        // icon fitting the slot outside the corner, the count printed read
        // alike by read_charm and read -- but 8871071 after its use: nothing
        // printed, its corner its icon's own, bare, which read cannot tell
        // from a count it cannot read.
        std::string wrong, info;
        float worst_fit = 0.0f;
        for (const PairSlot& c : kLivePair) {
            const Frame& icon = *bare_in(list, c.id);
            int counts[2] = { 0, 0 };
            for (const bool used : { false, true }) {
                const Frame& f = used ? after : before;
                const ScreenRect& s = (used ? sa : sb)[static_cast<std::size_t>(c.slot)];
                const int want = used ? c.after : c.before;
                const TrashList::Verdict kv = list.judge(f, s, min_score);
                const bool judged = kv.item >= 0 && list.items[static_cast<std::size_t>(kv.item)].id == c.id &&
                                    kv.colour_id == c.id;
                float fit = -1.0f;
                const bool fits = bag::icon_fits(f, s, icon, l, &fit);
                bool bare = false;
                const int count = k.counts.read_charm(f, s, &icon, l, &bare);
                const int read = k.counts.read(f, s);
                const bool corner = bag::bare_corner(f, s, icon, l);
                const bool one = want == 1;
                worst_fit = std::max(worst_fit, fit);
                counts[used ? 1 : 0] = count;
                if (!judged || !fits || count != want || bare != one || corner != one ||
                    read != (one ? CountReader::kUnreadable : want))
                    wrong += " | " + std::string(used ? "after, " : "before, ") + cell_text(c.slot) + " " +
                             last2(c.id) + ": " + taken_text(list, kv) + ", " + colour_text(kv) + ", fit " +
                             fixed1(fit) + ", read_charm " + std::to_string(count) + (bare ? " (bare)" : "") +
                             ", read " + std::to_string(read);
            }
            info += " " + cell_text(c.slot) + " " + last2(c.id) + " " + std::to_string(counts[0]) + "->" +
                    std::to_string(counts[1]) + ";";
        }
        std::printf("  [info] read_charm, before -> after:%s the icons fit at %s at worst\n", info.c_str(),
                    fixed1(worst_fit).c_str());
        check(wrong.empty(),
              "8871081 61 then 60, 8871071 2 then 1 by its bare icon, the six others 2 on both looks: each taken for "
              "its own colour, its icon fitting at " + fixed1(worst_fit) + " at worst; read the same but on the one, "
              "kUnreadable" + wrong);
    }
    {
        // 8871071 after its use: what 11.03b read -- the light rim white in
        // the corner and no number, kUnreadable -- and what 11.03c reads: its
        // own icon bare, 0 pixels unlike; with no other colour's icon a stack
        // of one.
        const ScreenRect& s = sa[6];
        const Frame& icon = *bare_in(list, kBlueDiamond);
        int unlike = -1;
        float fit = -1.0f;
        const bool corner = bag::bare_corner(after, s, icon, l, &unlike, &fit);
        bool other_reads = false;
        float other_fit = 255.0f;
        for (const char* other : kCharmIds) {
            if (std::string(other) == kBlueDiamond) continue;
            float of = 255.0f;
            bag::icon_fits(after, s, *bare_in(list, other), l, &of);
            other_fit = std::min(other_fit, of);
            other_reads = other_reads ||
                          k.counts.read_charm(after, s, bare_in(list, other), l) != CountReader::kUnreadable;
        }
        const int read = k.counts.read(after, s);
        std::printf("  [info] 8871071 after, row 2 cell 1: read %d; its icon fit %s, %d pixel(s) unlike; another "
                    "colour's icon fits at %s at best\n",
                    read, fixed1(fit).c_str(), unlike, fixed1(other_fit).c_str());
        check(read == CountReader::kUnreadable && corner && unlike == 0 && fit <= 1.0f && !other_reads &&
                  other_fit >= 40.0f,
              "8871071 x1 after its use: read kUnreadable, as 11.03b had it; its own icon bare, fit " + fixed1(fit) +
                  ", " + std::to_string(unlike) + " pixels unlike; no other colour's icon (best " +
                  fixed1(other_fit) + ") reads it");
    }
    {
        // What slot_now goes by after a use, on these two looks: the slot
        // alike the one before outside the count corner -- kStillThere, 0.80
        // -- and read one less with its colour's icon: Less, used. Nothing
        // but the two used slots changed between the looks.
        std::string changed, wrong, info;
        for (std::size_t i = 0; i < sb.size(); ++i)
            if (pixels_off(bag::cut(before, sb[i]), bag::cut(after, sa[i])) != 0)
                changed += " " + cell_text(static_cast<int>(i));
        for (const PairSlot& c : kLivePair) {
            if (c.before == c.after) continue;
            const Frame& icon = *bare_in(list, c.id);
            const std::size_t i = static_cast<std::size_t>(c.slot);
            const float like = bag::alike(bag::cut(after, sa[i]), bag::cut(before, sb[i]), l);
            const int was = k.counts.read_charm(before, sb[i], &icon, l);
            const int left = k.counts.read_charm(after, sa[i], &icon, l);
            info += " " + cell_text(c.slot) + " " + last2(c.id) + " alike " + fixed3(like) + ";";
            if (like < 0.80f || was != c.before || left != was - 1)
                wrong += " | " + cell_text(c.slot) + " " + last2(c.id) + ": alike " + fixed3(like) + ", " +
                         std::to_string(was) + " then " + std::to_string(left);
        }
        std::printf("  [info] the used slots after, against before:%s\n", info.c_str());
        check(changed == " r1c2 r2c1" && wrong.empty(),
              "only row 1 cell 2 and row 2 cell 1 changed (" + changed + " ), each alike its look before and read "
              "one less -- what slot_now takes for used" + wrong);
    }
    {
        // The single left is the client's drawing of 8871071 x1 wherever it
        // lies: row 2 cell 6's at 19:54 pixel for pixel. And each of the seven
        // x2 here differs from its colour's x1 then only in the count corner --
        // so those stand for the x1 the six others would turn into
        // (test_charm_pair_run).
        Frame g;
        BagView w;
        std::vector<ScreenRect> ws;
        if (!singles_bag(g, w, ws) || !w.found || ws.size() < 24) {
            skip("no cut of the live bag of 2026-11-03 19:54 here, to set the singles beside\n");
        } else {
            const int same = pixels_off(bag::cut(after, sa[6]), bag::cut(g, ws[11]));
            std::string wrong, info;
            for (const PairSlot& c : kLivePair) {
                if (c.before != 2) continue;
                for (const Single& one : kLiveSingles) {
                    if (std::string(one.id) != c.id) continue;
                    const Frame two = bag::cut(before, sb[static_cast<std::size_t>(c.slot)]);
                    const Frame single = bag::cut(g, ws[static_cast<std::size_t>(one.slot)]);
                    const int all = pixels_off(two, single);
                    const int outside =
                        pixels_off(two, single, ScreenRect{ 0, 0, bag::kCountCornerW, bag::kCountCornerH });
                    info += " " + last2(c.id) + " " + std::to_string(all) + ";";
                    if (all <= 0 || outside != 0)
                        wrong += " | " + last2(c.id) + ": " + std::to_string(all) + " pixels differ, " +
                                 std::to_string(outside) + " outside the corner";
                }
            }
            std::printf("  [info] each x2 of 22:05 against its colour's x1 of 19:54, pixels differing:%s\n",
                        info.c_str());
            check(same == 0, "8871071 x1 at 22:05, row 2 cell 1, is 19:54's in row 2 cell 6 pixel for pixel -- " +
                                 std::to_string(same) + " pixel(s) differ");
            check(wrong.empty(), "the seven x2 differ from their x1 of 19:54 in the count corner only" + wrong);
        }
    }
}

void test_charm_pair_run() {
    section("Bùa: the live bag of 22:05 rebuilt from its own slots -- x2 to x1 is one less, used, and the run goes on");
    if (skip_without_kit()) return;
    Frame before, after;
    BagView vb, va;
    std::vector<ScreenRect> sb, sa;
    if (!pair_bag("charm_pair_before", before, vb, sb) || !pair_bag("charm_pair_after", after, va, sa) || !vb.found ||
        !va.found || sb.size() < 24 || sa.size() < 24) {
        skip("no cuts of the live bag of 2026-11-03 22:05 here (handoff\\charms\\testdata\\charm_pair_*)\n");
        return;
    }
    ItemCatalog catalog;
    if (!charm_catalog(catalog)) {
        skip("the item catalog's charms are not installed here\n");
        return;
    }
    BagCleaner::forget_said_once();
    // Slot `slot` as the live client drew it, before the use or after.
    const auto live = [&](int slot, bool used) {
        return bag::cut(used ? after : before, (used ? sa : sb)[static_cast<std::size_t>(slot)]);
    };
    {
        // As at 22:05: 8871081 x61 in row 1 cell 2 and 8871071 x2 in row 2
        // cell 1, each drawn as the client drew it before its use and after,
        // the packets' bag agreeing, as it did -- through charm_one and
        // slot_now with the colour's icon, as the GUI runs them.
        CharmBagRun c(catalog, { { 1, kPurpleCard, 61 }, { 6, kBlueDiamond, 2 } }, { kPurpleCard, kBlueDiamond });
        c.p.game.items[0].drawn = { { 61, live(1, false) }, { 60, live(1, true) } };
        c.p.game.items[1].drawn = { { 2, live(6, false) }, { 1, live(6, true) } };
        std::vector<std::pair<int, int>> asked;
        c.p.packets = [&asked](const std::string&, int j, int count, std::string& said) {
            asked.emplace_back(j, count);
            said = "the same";
            return BagCleaner::PacketSays::Agrees;
        };
        c.run();
        const std::vector<std::pair<int, int>> slots_asked = { { 1, 61 }, { 6, 2 } };
        check(c.p.game.right_clicks == 2 && c.p.game.confirms == 2 && c.r.charms_used == 2 && c.tries() == "UU" &&
                  c.p.game.items[0].count == 60 && c.p.game.items[1].count == 1 && c.p.says("; còn 60") &&
                  c.p.says("; còn 1") && c.r.use_unsure == 0 && c.r.failed == 0 && c.r.trouble.empty() &&
                  c.p.game.messages_closed == 2 && asked == slots_asked,
              "8871081 x61 and 8871071 x2, the packets' slots 1 and 6 agreeing: both used -- 61 to 60, and 2 to 1 by "
              "its bare icon, where 11.03b stopped -- tries " + c.tries() + ", " + c.r.trouble);
    }
    {
        // All eight chosen, as the owner had them: 8871081 x61 and the seven
        // x2, each turned by its use into its x1 -- 8871071's as at 22:05, the
        // six others' as the client drew them at 19:54, which differ from
        // their x2 only in the count corner (test_charm_pair_live).
        Frame g;
        BagView w;
        std::vector<ScreenRect> ws;
        if (!singles_bag(g, w, ws) || !w.found || ws.size() < 24) {
            skip("no cut of the live bag of 2026-11-03 19:54 here, for the six other singles\n");
            return;
        }
        std::vector<CharmBagRun::In> in_bag;
        for (const PairSlot& one : kLivePair) in_bag.push_back({ one.slot, one.id, one.before });
        CharmBagRun c(catalog, in_bag, std::vector<std::string>(std::begin(kCharmIds), std::end(kCharmIds)));
        for (std::size_t i = 0; i < std::size(kLivePair); ++i) {
            const PairSlot& one = kLivePair[i];
            Frame used = live(one.slot, true);
            if (one.before == one.after)
                for (const Single& s : kLiveSingles)
                    if (std::string(s.id) == one.id) used = bag::cut(g, ws[static_cast<std::size_t>(s.slot)]);
            c.p.game.items[i].drawn = { { one.before, live(one.slot, false) }, { one.before - 1, used } };
        }
        const int deadline = BagCleaner::charm_deadline_ms(8);
        c.p.deadline_ms = deadline;
        c.run();
        bool ones = true;
        for (std::size_t i = 1; i < c.p.game.items.size(); ++i) ones = ones && c.p.game.items[i].count == 1;
        std::set<std::string> ids;
        for (const auto& t : c.r.charm_tries) ids.insert(t.id);
        std::printf("  [info] the live bag of 22:05 rebuilt, eight kinds used: %lld ms of the pretend game's time, "
                    "the deadline %d\n",
                    static_cast<long long>(c.took_ms), deadline);
        check(c.p.game.right_clicks == 8 && c.p.game.confirms == 8 && c.r.charms_used == 8 &&
                  c.tries() == "UUUUUUUU" && ids.size() == 8 && c.p.game.items[0].count == 60 && ones &&
                  c.r.use_unsure == 0 && c.r.failed == 0 && c.r.charms_unread == 0 && c.r.trouble.empty() &&
                  c.p.game.messages_closed == 8 && c.p.game.keys == 0,
              "all eight chosen: one run right-clicks each once -- 81 to 60, the seven x2 each seen one less -- "
              "each message closed, no stop -- " + std::to_string(c.p.game.right_clicks) + " right-clicks, " +
                  std::to_string(c.r.charms_used) + " used, tries " + c.tries() + ", " + c.r.trouble);
        check(c.took_ms < deadline, "inside the deadline of eight kinds: " + std::to_string(c.took_ms) + " of " +
                                        std::to_string(deadline) + " ms");
    }
}

void test_charm_singles_sources() {
    section("Bùa: a stack of one is read so only by the charm run -- the pass, fragments and the shop as before");
#ifdef GOBOT_SOURCE_DIR
    std::string cpp = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/bag.cpp");
    if (cpp.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    cpp.erase(std::remove(cpp.begin(), cpp.end(), '\r'), cpp.end());
    const auto body = [&](const std::string& head) {
        const std::size_t from = cpp.find(head);
        if (from == std::string::npos) return std::string();
        const std::size_t end = cpp.find("\n}", from);
        return end == std::string::npos ? std::string() : cpp.substr(from, end + 2 - from);
    };
    const auto count_in = [](const std::string& text, const std::string& part) {
        int n = 0;
        for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
        return n;
    };
    // CountReader::read as it was: what the pass, the fragments, the shop and
    // the potion count go by.
    check(body("int CountReader::read(const Frame& f, const ScreenRect& slot) const {") ==
              "int CountReader::read(const Frame& f, const ScreenRect& slot) const {\n"
              "    if (!loaded_ || !f.valid()) return kUnreadable;\n"
              "    // White the digits leave over says nothing in a slot's corner: the item\n"
              "    // under the count can be white itself.\n"
              "    int white = 0, stray = 0;\n"
              "    const int n = scan(f, ScreenRect{ slot.x, slot.y, 28, 16 }, 6, false, true, white, stray);\n"
              "    if (white == 0) return 1;                 // nothing printed: a stack of one\n"
              "    if (n <= 0 || n > 9999) return kUnreadable;\n"
              "    return n;\n"
              "}",
          "bag.cpp: CountReader::read is as it was");
    const std::string sweep = body("void BagCleaner::charm_sweep(");
    const std::string slot_now = body("BagCleaner::SlotNow BagCleaner::slot_now(");
    const std::string charm_one = body("BagCleaner::UseResult BagCleaner::charm_one(");
    const std::string use_one = body("BagCleaner::UseResult BagCleaner::use_one(");
    check(count_in(cpp, "read_charm(") == 3 && count_in(sweep, "read_charm(") == 1 &&
              count_in(slot_now, "read_charm(") == 1 && count_in(sweep, "counts_->read(") == 0,
          "read_charm is asked by charm_sweep and slot_now only, and charm_sweep reads no count else");
    check(count_in(cpp, ", &icon)") == 3 && count_in(charm_one, ", &icon)") == 3 &&
              count_in(use_one, "slot_now(v, index, before, count, look, shot, why))") == 2 &&
              count_in(use_one, "slot_now(") == 2,
          "slot_now is handed a charm's icon by charm_one alone; use_one's two looks read as before");
    std::string others;
    for (const char* head : { "BagCleaner::Result BagCleaner::run_pass(", "bool BagCleaner::trash_one(",
                              "void BagCleaner::use_sweep(", "BagCleaner::UseResult BagCleaner::use_one(",
                              "BagCleaner::Probe BagCleaner::probe(", "bool BagCleaner::shop_count(",
                              "void BagCleaner::shop_buy(" }) {
        const std::string b = body(head);
        if (b.empty() || b.find("read_charm(") != std::string::npos || b.find("bare_corner(") != std::string::npos ||
            b.find("bare_of(") != std::string::npos)
            others += std::string(" | ") + head;
    }
    check(others.empty(), "the pass, the fragments, the probe and the shop ask neither read_charm nor bare_corner" +
                              others);
    const std::string stock = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/potion_stock.cpp");
    check(!stock.empty() && stock.find("read_charm(") == std::string::npos &&
              stock.find("bare_corner(") == std::string::npos,
          "potion_stock.cpp neither");
    check(sweep.find("++res.charms_unread;") != std::string::npos &&
              sweep.find("no count read, and the corner is not its bare ") != std::string::npos &&
              count_in(sweep, "bare ? \" (nothing printed over its icon)\" : \"\"") == 2,
          "charm_sweep counts a slot it reads no count in, and logs why; a single's lines say \"nothing printed "
          "over its icon\"");
#else
    skip("no source folder\n");
#endif
}

void test_charm_clock() {
    section("Bùa: when each may be used again, kept in a file");
    const std::string path = temp_path("gobot_charm_clock.txt");
    DeleteFileA(path.c_str());
    const std::int64_t now = 1'800'000'000;
    {
        CharmClock c(path);
        check(!c.load() && c.due("spend/buff/8871011", now) && c.next("spend/buff/8871011") == 0,
              "no file: nothing known, every charm due");
        const std::int64_t at = CharmClock::next_after(CharmClock::How::Used, 60, now);
        check(at == now + 3600 && CharmClock::next_after(CharmClock::How::MaybeUsed, 60, now) == now + 3600 &&
                  CharmClock::next_after(CharmClock::How::NotUsed, 60, now) == now + CharmClock::kRetryMinutes * 60,
              "used or perhaps used: after the hour; not used: after ten minutes");
        check(c.set_next("spend/buff/8871011", at) && !c.due("spend/buff/8871011", now) &&
                  !c.due("spend/buff/8871011", at - 1) && c.due("spend/buff/8871011", at) &&
                  c.due("spend/buff/8871061", now),
              "set: not due until its time, and only that charm's");
    }
    {
        CharmClock again(path);
        check(again.load() && again.next("spend/buff/8871011") == now + 3600 && !again.due("spend/buff/8871011", now + 10),
              "read back by a GoBot started again");
        again.set_next("configs\\trash\\use\\a=b.png", now + 5);
        CharmClock third(path);
        check(third.load() && third.next("configs\\trash\\use\\a=b.png") == now + 5 &&
                  third.next("spend/buff/8871011") == now + 3600,
              "another charm kept beside it, a file's name with an '=' in it as well");
    }
    {
        // Another GoBot wrote a charm's time since this one read the file.
        CharmClock a(path), b(path);
        a.load();
        b.load();
        a.set_next("x1", now + 1);
        b.set_next("x2", now + 2);
        CharmClock all(path);
        all.load();
        check(all.next("x1") == now + 1 && all.next("x2") == now + 2, "two writers: neither's time is lost");
    }
    {
        write_all(path, "good=1800000100\r\n\r\nno equals sign\n=5\nbad=12x\nlong=99999999999999999999\nok2=7\n");
        CharmClock c(path);
        check(c.load() && c.next("good") == 1800000100 && c.next("ok2") == 7 && c.all().size() == 2,
              "a file with garbage in it: the lines that are not a time are passed over");
    }
    {
        // What is known stays known: a file read again adds to it, the later
        // time of either wins, and one that cannot be read takes nothing away.
        write_all(path, "a=1800000100\nb=1800000500\n");
        CharmClock c(path);
        c.set_next("a", 1800000300);
        c.set_next("c", 1800000900);
        write_all(path, "a=1800000100\nb=1800000500\n");        // the file as another GoBot left it
        check(c.load() && c.next("a") == 1800000300 && c.next("b") == 1800000500 && c.next("c") == 1800000900,
              "read again: its own later time kept, the file's others added");
        write_all(path, "a=1800009999\n");
        c.load();
        check(c.next("a") == 1800009999 && c.next("c") == 1800000900, "and a later time in the file wins");
    }
    {
        // A clock that cannot be written: the time is still kept, and a load
        // that fails does not wipe it, so the charm just used is not due again.
        const std::string blocker = temp_path("gobot_charm_blocker");
        write_all(blocker, "a file where a folder would have to be");
        CharmClock c(blocker + "\\sub\\charm_clock.txt");
        check(!c.set_next("spend/buff/8871011", now + 3600) && !c.save(), "a clock that cannot be written says so");
        check(!c.load() && !c.due("spend/buff/8871011", now) && c.next("spend/buff/8871011") == now + 3600,
              "and the time it was given is not lost to a reading that fails");
        DeleteFileA(blocker.c_str());
    }
    {
        // A time further ahead than any charm lasts is not trusted.
        CharmClock c(path);
        c.set_next("far", now + 400LL * 24 * 3600);
        c.set_next("near", now + CleanSettings::kCharmEveryMost * 60LL);
        check(c.due("far", now) && !c.due("near", now), "a time a year ahead is due; a day ahead is not");
    }
    {
        TrashList::Item byfile, bycatalog, byvia;
        byfile.file = "D:\\old folder\\GoBot\\configs\\trash\\use\\item_123_n.png";
        bycatalog.id = "spend/buff/8871011";
        byvia.id = "spend/buff/8871051";
        byvia.via = "spend/buff/8871061";
        check(byfile.clock_id() == "item_123_n.png" && bycatalog.clock_id() == "spend/buff/8871011" &&
                  byvia.clock_id() == "spend/buff/8871061",
              "a picture's clock id is its file name, not the folder a version was unzipped to");
    }
    DeleteFileA(path.c_str());
    DeleteFileA((path + ".new").c_str());
}

void test_charm_settings() {
    section("Bùa: [Trash] use_charms, charm_every_minutes and charm_wait_ms");
    const std::string path = temp_path("gobot_charm_trash.ini");
    CleanSettings s;
    write_all(path, "[Trash]\nenabled=false\n");
    check(s.load(path) && !s.use_charms && s.charm_every_minutes == 60 && s.charm_wait_ms == 1000,
          "a file without them: off, an hour, a second");
    write_all(path, "[Trash]\nuse_charms=true\ncharm_every_minutes=90\ncharm_wait_ms=1500\n");
    check(s.load(path) && s.use_charms && s.charm_every_minutes == 90 && s.charm_wait_ms == 1500, "read back");
    write_all(path, "[Trash]\nenabled=false\n");
    check(s.load(path) && !s.use_charms, "and off again when the line is taken out: never left as an earlier reading had it");
    write_all(path, "[Trash]\ncharm_every_minutes=1\ncharm_wait_ms=50\n");
    check(s.load(path) && s.charm_every_minutes == CleanSettings::kCharmEveryLeast &&
              s.charm_wait_ms == CleanSettings::kCharmWaitLeast,
          "numbers too small are brought up");
    write_all(path, "[Trash]\ncharm_every_minutes=99999\ncharm_wait_ms=99999\n");
    check(s.load(path) && s.charm_every_minutes == CleanSettings::kCharmEveryMost &&
              s.charm_wait_ms == CleanSettings::kCharmWaitMost,
          "and too large ones down");
    write_all(path, "[Trash]\nenabled=false\n");
    check(s.load(path) && s.charm_frame_score > 0.915f && s.charm_frame_score < 0.925f,
          "the box score a charm's question needs: 0.92 unless set");
    write_all(path, "[Trash]\ncharm_frame_score=0.5\n");
    check(s.load(path) && s.charm_frame_score == CleanSettings::kCharmFrameLeast, "and no lower than 0.85");
    write_all(path, "[Trash]\ncharm_frame_score=0.999\n");
    check(s.load(path) && s.charm_frame_score == CleanSettings::kCharmFrameMost, "nor higher than 0.99");
    DeleteFileA(path.c_str());
#ifdef GOBOT_SOURCE_DIR
    CleanSettings shipped;
    const std::string text = read_all(std::string(GOBOT_SOURCE_DIR) + "/configs/trash.ini");
    check(shipped.load(std::string(GOBOT_SOURCE_DIR) + "/configs/trash.ini") && !shipped.use_charms &&
              text.find("\nuse_charms=false\n") != std::string::npos && shipped.charm_every_minutes == 60 &&
              text.find("\ncharm_frame_score=0.92\n") != std::string::npos && shipped.charm_frame_score > 0.915f &&
              shipped.charm_frame_score < 0.925f,
          "the shipped trash.ini ships charms off: use_charms=false, an hour, the box score 0.92");
#endif
}

/// What one shop run comes to, on a pretend bag with `hp_slots` slots of Thịt
/// bò khô and `mp_slots` of Rượu lá tre in Tiêu Dùng, and -- with `opener` --
/// the Người Giữ Sân item in Khác.
struct ShopRun {
    Pass p;
    ItemCatalog catalog;
    BagCleaner::Result r;
    bool ok = false;
    explicit ShopRun(int hp_slots, int mp_slots, bool opener = true) {
        const std::string hp = CleanSettings::kBuyHpId, mp = CleanSettings::kBuyMpId;
        const std::string door = p.settings.buy_opener;
        if (!kit().shop_ok || !catalog.load(ItemCatalog::default_dir()) || catalog.find(hp) == nullptr ||
            catalog.find(mp) == nullptr || catalog.find(door) == nullptr)
            return;
        make_bag(p.game);
        p.game.book_opens = false;
        p.game.tabs_work = true;
        p.game.shop_icon_hp = catalog.find(hp)->tile;
        p.game.shop_icon_mp = catalog.find(mp)->tile;
        int slot = 0;
        for (int i = 0; i < hp_slots; ++i) {
            Pretend::Item it{ slot++, 3, 100 };
            it.icon = p.game.shop_icon_hp;
            p.game.items.push_back(it);
        }
        for (int i = 0; i < mp_slots; ++i) {
            Pretend::Item it{ slot++, 3, 100 };
            it.icon = p.game.shop_icon_mp;
            p.game.items.push_back(it);
        }
        if (opener) {
            Pretend::Item o{ 0, 1, 1 };
            o.tab = kOtherTab;
            o.opener = true;
            o.icon = catalog.find(door)->tile;
            p.game.items.push_back(o);
        }
        p.list.add_catalog(catalog, { hp, mp, door }, TrashList::Action::Delete, p.settings.catalog_score);
        p.list.add_rivals(catalog);
        p.mode = BagCleaner::Mode::ShopBuy;
        p.settings.buy_potions = true;
        ok = true;
    }
    BagCleaner::Result run(bool dry = false) {
        r = p.run(dry);
        return r;
    }
    /// Slots of each potion now in Tiêu Dùng.
    int slots(bool hp) const {
        int n = 0;
        for (const Pretend::Item& it : p.game.items)
            if (!it.gone() && it.tab == kUseTab && it.icon.width > 0 &&
                it.icon.pixels == (hp ? p.game.shop_icon_hp : p.game.shop_icon_mp).pixels)
                ++n;
        return n;
    }
};

void test_shop() {
    section("Mua bình: counted, bought 100 at a time with the number read back, and only on a screen as expected");
    if (skip_without_kit()) return;
    if (!kit().shop_ok) {
        skip("the shop's pictures are not installed here\n");
        return;
    }
    {
        ShopRun probe(0, 0);
        if (!probe.ok) {
            skip("the item catalog is not installed here\n");
            return;
        }
    }
    const auto spoken = [](const ShopRun& s, const char* what) { return s.p.says(what); };

    {
        ShopRun s(4, 4);
        s.run();
        check(s.r.ran && s.r.trouble.empty() && s.r.shop_hp_before == 4 && s.r.shop_mp_before == 4 &&
                  s.r.shop_bought == 0 && s.p.game.opener_clicks == 0 && s.p.game.right_clicks == 0 &&
                  s.p.game.digit_keys == 0 && spoken(s, "đủ bình"),
              "enough of both: counted, the item never right-clicked, nothing typed -- " + s.r.trouble + " / " +
                  s.p.last());
    }
    {
        ShopRun s(1, 4);
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 5 && s.p.game.buys == std::vector<int>(5, 0) &&
                  s.slots(true) == 6 && s.slots(false) == 4 && s.r.shop_hp_before == 1 && s.r.shop_hp_after == 6,
              "HP under its threshold: five bought up to six slots, MP left alone -- " + s.r.trouble + " / " +
                  s.p.last());
        check(s.p.game.digit_keys == 10 && s.p.game.wrong_confirms == 0 && s.p.game.cancels == 0 &&
                  s.p.game.typed == "100",
              "each time 00 typed after the game's 1 in the box, and the box read as 100 before Xác Nhận (" +
                  std::to_string(s.p.game.digit_keys) + " digit keys)");
        check(!s.p.game.shop_on && s.p.game.esc_presses == 1 && s.p.game.thoat_clicks == 0 &&
                  s.p.game.opener_clicks == 1,
              "one bought as a try, the slots counted beside the open shop, then the rest: the shop opened once "
              "and shut once at the end, with one ESC, seen to work");
    }
    {
        // Black frames now and then, as the live client gave right after the
        // quantity box came up: looked at again, never taken for the box gone.
        ShopRun s(1, 4);
        s.p.game.black_every = 6;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 5 && s.p.game.buys == std::vector<int>(5, 0) &&
                  s.slots(true) == 6 && s.p.game.wrong_confirms == 0 && s.p.game.cancels == 0 &&
                  s.p.game.black_looks > 10,
              "a black frame every sixth look: looked at again, and five Thịt bò khô bought as with none (" +
                  std::to_string(s.p.game.black_looks) + " black) -- " + s.r.trouble + " / " + s.p.last());
    }
    {
        // Black from the first look on: nothing pressed, nothing clicked, and
        // said.
        ShopRun s(1, 4);
        s.p.game.black_from = 1;
        s.run();
        check(!s.r.trouble.empty() && s.r.shop_bought == 0 && s.p.game.right_clicks == 0 &&
                  s.p.game.clicks == 0 && s.p.game.drags == 0 && s.p.game.digit_keys == 0 && s.p.game.keys == 0,
              "a game black throughout: no key, no click, no purchase -- " + s.r.trouble);
    }
    {
        // Black from the ESC that closes the shop on, for good: Thoát is not
        // clicked where the shop was, and the run keeps the hold.
        ShopRun s(2, 4);
        s.p.game.esc_closes_shop = false;
        s.p.during = [&s](BagCleaner&) {
            if (s.p.game.esc_presses > 0 && s.p.game.black_from < 0) s.p.game.black_from = s.p.game.looks + 1;
        };
        s.run();
        check(s.p.game.esc_presses == 1 && s.p.game.thoat_clicks == 0 && s.r.stuck && !s.r.trouble.empty(),
              "black from the ESC on: Thoát never clicked blind, the hold kept -- " + s.r.trouble);
    }
    {
        // A black stretch waited out once: a game that stays black does not
        // make every later look wait as long again.
        ShopRun s(1, 4);
        s.p.game.black_from = 1;
        s.run();
        check(s.p.game.black_looks <= 30,
              "a game black for good: " + std::to_string(s.p.game.black_looks) + " black looks in all, the wait once");
    }
    {
        // Black once the quantity box has been seen, and for good: Hủy Bỏ is
        // not pressed blind, nothing is typed, and the run keeps the hold.
        ShopRun s(1, 4);
        s.p.during = [&s](BagCleaner&) {
            if (s.p.game.qty_up && s.p.game.black_from < 0) s.p.game.black_from = s.p.game.looks + 2;
        };
        s.run();
        check(s.r.shop_bought == 0 && s.p.game.digit_keys == 0 && s.p.game.wrong_confirms == 0 &&
                  s.p.game.cancels == 0 && s.p.game.qty_up && s.r.stuck &&
                  s.r.trouble.find("không đọc được số") != std::string::npos,
              "black for good once the box is seen: nothing typed, no Hủy Bỏ clicked blind, the hold kept -- " +
                  s.r.trouble);
    }
    {
        // The potions' row second in the list, as on the live client with two
        // other items above it: the shop is still found, and each potion
        // clicked where its row is.
        ShopRun s(1, 4);
        s.p.game.row_drop = 42;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 5 && s.p.game.buys == std::vector<int>(5, 0) &&
                  s.slots(true) == 6 && s.p.game.wrong_confirms == 0,
              "the potions' row second in the shop's list: found, and five Thịt bò khô bought -- " + s.r.trouble +
                  " / " + s.p.last());
    }
    {
        ShopRun s(0, 2);
        s.run();
        // One of each as a try, the slots counted, then the rest of each.
        std::vector<int> want{ 0, 1 };
        want.insert(want.end(), 5, 0);
        want.insert(want.end(), 3, 1);
        check(s.r.trouble.empty() && s.p.game.buys == want && s.slots(true) == 6 && s.slots(false) == 6,
              "both under: Thịt bò khô first, then Rượu lá tre, each to its limit -- " + s.r.trouble);
    }
    {
        ShopRun s(2, 6);
        s.p.game.key_drops = 2;
        s.run();
        check(s.r.trouble.empty() && s.p.game.buys.size() == 4 && s.p.game.wrong_confirms == 0 &&
                  s.p.game.digit_keys > 8,
              "digits lost on the way: read back, the rest typed, and 100 is what is confirmed (" +
                  std::to_string(s.p.game.digit_keys) + " digit keys) -- " + s.r.trouble);
    }
    {
        ShopRun s(2, 6);
        s.p.game.keys_dead = true;
        s.run();
        check(s.r.shop_bought == 0 && s.p.game.wrong_confirms == 0 && s.p.game.cancels >= 1 &&
                  s.p.game.digit_keys > 0 && s.r.trouble.find("không nhận phím số") != std::string::npos &&
                  !s.p.game.shop_on && s.p.game.esc_presses == 1,
              "a box that never reads 100: Hủy Bỏ, never Xác Nhận, nothing bought, the shop shut -- " + s.r.trouble);
    }
    {
        // As on the live client: the box Mua brings up takes no keys, the one a
        // double-click on the potion brings up does. Mua is never clicked; the
        // potion double-clicked, 00 typed straight in, five bought.
        ShopRun s(1, 4);
        s.p.game.box_needs_click = true;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 5 && s.p.game.buys == std::vector<int>(5, 0) &&
                  s.p.game.mua_clicks == 0 && s.p.game.double_clicks == 5 && s.p.game.box_clicks == 0 &&
                  s.p.game.wrong_confirms == 0 && s.p.game.digit_keys == 10 && s.p.game.scan_keys == 0,
              "the potion double-clicked, never Mua, 00 typed straight in: five bought (" +
                  std::to_string(s.p.game.double_clicks) + " double-clicks) -- " + s.r.trouble + " / " + s.p.last());
    }
    {
        // A box that takes digits only once clicked into, even after the
        // double-click: the second way of typing clicks into it.
        ShopRun s(1, 4);
        s.p.game.box_needs_click = true;
        s.p.game.double_unfocused = true;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 5 && s.p.game.box_clicks >= 5 && s.p.game.scan_keys > 0 &&
                  s.p.game.wrong_confirms == 0 && s.p.game.mua_clicks == 0,
              "a box that takes digits only once clicked into: clicked into on the second way, five bought (" +
                  std::to_string(s.p.game.box_clicks) + " clicks into it) -- " + s.r.trouble + " / " + s.p.last());
    }
    {
        // Digits taken only as scan codes alone: the key codes put nothing in
        // the box, and it is typed again as GoC types.
        ShopRun s(1, 4);
        s.p.game.scan_only = true;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 5 && s.p.game.scan_keys > 0 && s.p.game.wrong_confirms == 0 &&
                  s.p.says("mã quét phím"),
              "digits taken only as scan codes: typed again that way, five bought (" +
                  std::to_string(s.p.game.scan_keys) + " scan keys) -- " + s.r.trouble + " / " + s.p.last());
    }
    {
        // A click into the box does nothing and the digits draw Hủy Bỏ pressed,
        // which find_dialogs does not see as a dialog: still known by where it
        // is, Hủy Bỏ clicked, nothing confirmed, and the run does not end on a
        // box left up.
        ShopRun s(1, 4);
        s.p.game.box_needs_click = true;
        s.p.game.double_unfocused = true;
        s.p.game.click_dead = true;
        bool hidden = false;
        s.p.during = [&](BagCleaner&) {
            if (s.p.game.qty_up && s.p.game.cancel_pressed && bag::find_dialogs(s.p.game.render(), kit().store).empty())
                hidden = true;
        };
        s.run();
        check(hidden && s.r.shop_bought == 0 && s.p.game.wrong_confirms == 0 && s.p.game.cancels >= 1 &&
                  !s.p.game.qty_up && !s.p.game.shop_on && s.p.says("ô vẫn là 1") &&
                  s.r.trouble.find("không nhận phím số") != std::string::npos,
              "Hủy Bỏ drawn pressed, the pair not seen as a dialog: the box still followed by its Xác Nhận, "
              "Hủy Bỏ clicked, nothing bought -- " + s.r.trouble);
        check(s.p.game.digit_keys == 4 && s.p.game.scan_keys == 2 && s.p.game.backspaces == 0,
              "and no more keys once both ways put nothing in the box: two as key codes, two as scan codes (" +
                  std::to_string(s.p.game.digit_keys) + " digit keys)");
    }
    {
        // The same, and Hủy Bỏ does not close the box either: the run ends
        // keeping the hold, the box known to be up by its Xác Nhận.
        ShopRun s(1, 4);
        s.p.game.box_needs_click = true;
        s.p.game.double_unfocused = true;
        s.p.game.click_dead = true;
        s.p.game.qty_sticky = true;
        s.run();
        check(s.r.stuck && s.p.game.qty_up && s.r.shop_bought == 0 && s.p.game.wrong_confirms == 0 &&
                  s.p.game.digit_keys == 4 && !s.r.trouble.empty(),
              "a box that will not close, its Hủy Bỏ drawn pressed: the hold kept -- " + s.r.trouble);
    }
    {
        // A trial whose Hủy Bỏ does not close the box: not said to have
        // worked, the other potion not tried, the hold kept.
        ShopRun s(4, 4);
        s.p.game.qty_sticky = true;
        s.run(true);
        check(s.r.trouble.find("thử mua: đã bấm Hủy Bỏ mà không thấy") != std::string::npos && s.r.stuck &&
                  !s.p.says("thử mua: dừng ở hộp nhập số lượng") && s.p.game.digit_keys == 0 &&
                  s.p.game.buys.empty(),
              "a trial whose Hủy Bỏ leaves the box up: said so, stopped, the hold kept -- " + s.r.trouble);
    }
    {
        // The press speed: the same five bought at each step, read back as 100
        // every time; the quicker the step, the less time the run takes.
        std::vector<long long> took;
        bool same = true;
        for (const int ms : { 60, 80, 150 }) {
            ShopRun s(1, 4);
            s.p.settings.press_ms = ms;
            const auto from = s.p.game.time.t;
            s.run();
            took.push_back(static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(s.p.game.time.t - from).count()));
            same = same && s.r.trouble.empty() && s.r.shop_bought == 5 && s.p.game.wrong_confirms == 0;
        }
        check(same && took.size() == 3 && took[0] < took[1] && took[1] < took[2],
              "press speed 60, 80, 150 ms: five bought each time, in " + std::to_string(took[0]) + ", " +
                  std::to_string(took[1]) + " and " + std::to_string(took[2]) + " ms of pretend time");
    }
    {
        // The shop kept open between rounds lying over the bag's tab strip, as
        // a small game window can have it: shut, the bag looked at again, the
        // rest bought on a shop opened again.
        ShopRun s(1, 4);
        s.p.game.kSX = 290;
        s.p.game.kSY = 296;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 5 && s.p.game.wrong_confirms == 0 &&
                  s.p.game.esc_presses == 2 && s.p.game.opener_clicks == 2 && !s.p.game.shop_on,
              "the shop over the bag's strip after the trial: shut, the bag counted, five bought in all (" +
                  std::to_string(s.p.game.esc_presses) + " ESC) -- " + s.r.trouble + " / " + s.p.last());
    }
    {
        // The shop open already -- opened late after the last run gave up on
        // it: taken as it is, the item not right-clicked again, five bought.
        ShopRun s(1, 4);
        s.p.game.shop_on = true;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 5 && s.p.game.wrong_confirms == 0 &&
                  s.p.says("cửa hàng đã mở sẵn"),
              "the shop open already: taken as it is, five bought (" + std::to_string(s.p.game.opener_clicks) +
                  " right-clicks on the item) -- " + s.r.trouble + " / " + s.p.last());
    }
    {
        ShopRun s(2, 6);
        s.p.game.qty_is_sell = true;
        s.run();
        check(s.r.shop_bought == 0 && s.p.game.digit_keys == 0 && s.p.game.wrong_confirms == 0 &&
                  s.p.game.cancels >= 1 && !s.p.game.shop_on,
              "the sell question where the buy question should be: nothing typed, Hủy Bỏ -- " + s.r.trouble);
    }
    {
        ShopRun s(2, 6);
        s.p.game.full_bag = true;
        s.run();
        check(s.r.shop_full && s.r.shop_bought == 0 && s.p.game.notice_clicks == 1 && s.p.game.digit_keys == 0 &&
                  !s.p.game.shop_on && s.r.trouble.empty(),
              "the bag full: its message dismissed with its one Xác Nhận, nothing typed, the shop shut -- " + s.r.trouble);
    }
    {
        ShopRun s(1, 4, false);
        s.run();
        check(s.p.game.right_clicks == 0 && s.p.game.opener_clicks == 0 &&
                  s.r.trouble.find("không thấy món Người Giữ Sân") != std::string::npos,
              "no Người Giữ Sân item in Khác: nothing right-clicked -- " + s.r.trouble);
    }
    {
        // No scroll bar to read: said, with the bag's picture kept, and nothing clicked on Khác.
        ShopRun s(1, 4);
        s.p.game.thumb = false;
        s.p.debug_dir = temp_path("shop_debug");
        s.run();
        check(s.p.game.opener_clicks == 0 && s.r.trouble.find("thanh cuộn") != std::string::npos &&
                  !files_in(s.p.debug_dir, "*shop_scroll_unknown*.png").empty(),
              "a scroll bar that cannot be read: said, the picture kept, nothing right-clicked -- " + s.r.trouble);
    }
    {
        // The shop comes only after GoBot has stopped waiting for it: waited for
        // once more and shut, with its one ESC, nothing bought.
        ShopRun s(1, 4);
        s.p.game.open_after_looks = 40;
        s.run();
        check(s.p.game.opener_clicks == 1 && s.p.game.digit_keys == 0 && s.p.game.esc_presses == 1 &&
                  !s.p.game.shop_on && s.r.trouble.find("không thấy cửa hàng") != std::string::npos,
              "a shop that comes late: waited for once more and shut -- " + s.r.trouble);
    }
    {
        // What a hold kept for a purchase is let go on.
        const Kit& k = kit();
        Frame shop = scene(1024, 768, 60);
        blit(shop, *k.store.get(bag::kShopRow), 100, 50);
        blit(shop, *k.store.get(bag::kShopButtons), 100 + bag::kShopButtonsDx, 50 + bag::kShopButtonsDy);
        Frame buttons_only = scene(1024, 768, 60);
        blit(buttons_only, *k.store.get(bag::kShopButtons), 400, 400);
        const Frame& pair = *k.store.get("grabbed/confirm_button");
        Frame half = scene(1024, 768, 60);
        blit(half, bag::cut(pair, ScreenRect{ 0, 0, pair.width / 2 - 1, pair.height }), 500, 300);
        check(bag::shop_left_up(shop, k.store) && bag::shop_left_up(buttons_only, k.store) &&
                  bag::shop_left_up(half, k.store) && bag::shop_left_up(scene(1024, 768, 0), k.store) &&
                  bag::shop_left_up(Frame{}, k.store) && !bag::shop_left_up(scene(1024, 768, 60), k.store),
              "shop_left_up: the shop, its buttons alone, a lone Xác Nhận, no frame and a black one are things "
              "still up; a clear screen is not");
    }
    {
        ShopRun s(1, 4);
        s.p.game.opener_opens = false;
        s.run();
        check(s.p.game.opener_clicks == 1 && s.p.game.digit_keys == 0 && s.p.game.esc_presses == 0 &&
                  s.r.trouble.find("không thấy cửa hàng") != std::string::npos,
              "the item right-clicked and no shop come: nothing more pressed, ESC included -- " + s.r.trouble);
    }
    {
        ShopRun s(4, 4);
        s.run(true);
        check(s.r.trouble.empty() && s.p.game.opener_clicks == 1 && s.p.game.digit_keys == 0 &&
                  s.p.game.cancels == 2 && s.p.game.buys.empty() && s.r.shop_bought == 0 && !s.p.game.shop_on &&
                  spoken(s, "thử mua: dừng ở hộp nhập số lượng"),
              "a trial: both items tried as far as the box, Hủy Bỏ each time, nothing typed or bought, the shop shut -- " +
                  s.r.trouble);
    }
    {
        ShopRun s(1, 4);
        s.p.game.shop_merges = true;
        s.run();
        check(s.p.game.buys.size() == 1 && s.p.game.opener_clicks == 1 && s.r.shop_no_gain &&
                  s.r.trouble.find("không tăng") != std::string::npos,
              "bought one as a try and no new slot: not bought again -- " + s.r.trouble + " (" +
                  std::to_string(s.p.game.buys.size()) + " buys)");
    }
    {
        ShopRun s(2, 4);
        s.p.game.esc_closes_shop = false;
        s.run();
        check(s.p.game.buys.size() == 4 && s.p.game.esc_presses >= 1 && s.p.game.thoat_clicks >= 1 &&
                  !s.p.game.shop_on && s.r.trouble.empty(),
              "an ESC that does not close the shop: its Thoát clicked -- " + s.r.trouble);
    }
    {
        ShopRun s(0, 6);
        s.p.guard = [&s] { return s.p.game.buys.size() < 2; };
        s.run();
        check(s.p.game.buys.size() == 2 && !s.p.game.shop_on &&
                  s.r.trouble.find("không còn giữ phím") != std::string::npos,
              "the hold gone after two: nothing more bought, the shop shut -- " + s.r.trouble);
    }
    {
        ShopRun s(0, 6);
        s.p.settings.buy_potions = false;
        s.run();
        check(s.p.game.right_clicks == 0 && s.p.game.digit_keys == 0 &&
                  s.r.trouble.find("buy_potions=false") != std::string::npos,
              "with the switch off nothing is bought or right-clicked -- " + s.r.trouble);
    }
    {
        // The cell clicked for HP sells MP: one bought as a try, the slots counted,
        // and the potion that grew is not the one asked for -- not bought again.
        ShopRun s(1, 4);
        s.p.game.swap_columns = true;
        s.run();
        check(s.p.game.buys.size() == 1 && s.r.shop_no_gain && !s.p.game.shop_on && s.slots(true) == 1,
              "a column swapped: one purchase as a try, seen not to be the potion asked for, and no more -- " + s.r.trouble);
    }
    {
        // The question replaced by another between two keys: no more digits, Hủy Bỏ, never Xác Nhận.
        ShopRun s(2, 6);
        bool swapped = false;
        s.p.during = [&](BagCleaner&) {
            if (s.p.game.qty_up && s.p.game.typed.size() == 2 && !swapped) {
                swapped = true;
                s.p.game.shown = *kit().store.get("grabbed/hoisinh_confirm");
                s.p.game.shown_pair = s.p.game.question_pair(s.p.game.shown);
            }
        };
        s.run();
        check(swapped && s.p.game.digit_keys == 1 && s.p.game.wrong_confirms == 0 && s.p.game.cancels >= 1 &&
                  s.r.shop_bought == 0 && !s.p.game.shop_on,
              "the question turns into another after the first key: nothing more typed, Hủy Bỏ -- " + s.r.trouble);
    }
    {
        // A limit under the threshold's own: a potion with its threshold 0 is left alone.
        ShopRun s(0, 0);
        s.p.settings.buy_hp_below = 0;
        s.p.settings.buy_hp_to = 0;
        s.p.settings.buy_mp_below = 2;
        s.p.settings.buy_mp_to = 3;
        s.run();
        check(s.p.game.buys == std::vector<int>(3, 1) && s.slots(true) == 0 && s.slots(false) == 3,
              "a potion whose threshold is 0 is never bought: only MP, up to three -- " + s.r.trouble);
    }
}

/// What the bag reader makes of saved frames of a live game -- a folder named
/// by GOBOT_TEST_BAG_FRAMES -- for working out why a run could not read the
/// bag: said as [info] lines, never checked.
void say_saved_bags() {
    const char* dir = std::getenv("GOBOT_TEST_BAG_FRAMES");
    if (dir == nullptr || *dir == 0) return;
    const BagLayout l;
    for (const std::string& name : files_in(dir, "*.png")) {
        Frame f;
        if (!load_png(std::string(dir) + "\\" + name, f)) continue;
        const BagView v = bag::find(f, kit().store, l);
        std::printf("  [info] %s: %dx%d found=%d strip=%d,%d tab=%d slot_rows=%d first_row=%d total_rows=%d "
                    "scroll_known=%d bin_empty=%d\n",
                    name.c_str(), f.width, f.height, v.found ? 1 : 0, v.strip.x, v.strip.y, v.selected_tab,
                    v.slot_rows, v.first_row, v.total_rows, v.scroll_known() ? 1 : 0, v.bin_empty ? 1 : 0);
        // The shop and any dialog up: whether it is taken for the buy
        // question, and what its box reads.
        if (kit().shop_ok) {
            const bag::Shop s = bag::find_shop(f, kit().store);
            if (s.found)
                std::printf("  [info]   shop: row at %d,%d (%.3f), buttons at %d,%d (%.3f)\n", s.row.x, s.row.y,
                            s.row_score, s.buttons.x, s.buttons.y, s.buttons_score);
            for (const bag::Dialog& d : bag::find_dialogs(f, kit().store)) {
                const ScreenRect box{ d.pair.x - 88, d.pair.y + 3, 82, std::max(4, d.pair.h - 6) };
                std::printf("  [info]   dialog: pair at %d,%d %dx%d ours=%d registers=%d buy_question=%d "
                            "(box not asked: %d) box reads %d\n",
                            d.pair.x, d.pair.y, d.pair.w, d.pair.h, d.ours() ? 1 : 0, d.registers ? 1 : 0,
                            bag::shop_question(f, kit().store, d, kit().counts) ? 1 : 0,
                            bag::shop_question(f, kit().store, d, kit().counts, CleanSettings::kBuyFrameDefault,
                                               false)
                                ? 1
                                : 0,
                            kit().counts.read_field(f, box));
            }
        }
        if (!v.found) continue;
        // The scroll track's column: its brightness top to bottom, in runs.
        const int tx = v.strip.x + l.scroll_up_dx;
        std::string runs;
        int from = -1, last = -1;
        for (int y = v.strip.y + l.track_top_dy - 4; y <= v.strip.y + l.track_bottom_dy + 4; ++y) {
            std::uint8_t r = 0, g = 0, b = 0;
            f.get(tx, y, r, g, b);
            const int lum = (r * 299 + g * 587 + b * 114) / 1000;
            if (from < 0) { from = y; last = lum; }
            else if (std::abs(lum - last) > 40) {
                runs += " [" + std::to_string(from - v.strip.y) + ".." + std::to_string(y - 1 - v.strip.y) + "]=" + std::to_string(last);
                from = y;
                last = lum;
            }
        }
        if (from >= 0) runs += " [" + std::to_string(from - v.strip.y) + "..]=" + std::to_string(last);
        std::printf("  [info]   track at x=%d (strip+%d), y from strip: %s\n", tx, l.scroll_up_dx, runs.c_str());
        // What each slot of the page on show is judged.
        const auto slots = v.slots(l);
        int empty = 0;
        for (const ScreenRect& s : slots) empty += bag::is_empty(f, s, l) ? 1 : 0;
        std::printf("  [info]   %zu slots on show, %d empty\n", slots.size(), empty);
        // The two potions against every slot on show, at a score low enough to
        // show a near miss.
        static ItemCatalog catalog;
        static bool loaded = catalog.load(ItemCatalog::default_dir());
        if (loaded && catalog.find(CleanSettings::kBuyHpId) && catalog.find(CleanSettings::kBuyMpId)) {
            TrashList list;
            list.add_catalog(catalog, { CleanSettings::kBuyHpId, CleanSettings::kBuyMpId }, TrashList::Action::Delete,
                             0.70f);
            list.add_rivals(catalog);
            for (std::size_t s = 0; s < slots.size(); ++s) {
                if (bag::is_empty(f, slots[s], l)) continue;
                const TrashList::Verdict k = list.judge(f, slots[s], 0.70f);
                if (k.item >= 0)
                    std::printf("  [info]   slot %zu (row %zu col %zu): %s at %.3f\n", s, s / 6 + 1, s % 6 + 1,
                                list.items[static_cast<std::size_t>(k.item)].id.c_str(), k.score);
                else if (k.like >= 0)
                    std::printf("  [info]   slot %zu: looks like %s (%.3f) but %s is as near (%.3f)\n", s,
                                list.items[static_cast<std::size_t>(k.like)].id.c_str(), k.like_score,
                                k.rival.c_str(), k.rival_score);
            }
        }
        // On Khác: the Người Giữ Sân item by the catalog's icon alone, as the
        // shop run builds its list without a picture of the player's.
        if (loaded && v.selected_tab == kOtherTab && catalog.find(CleanSettings{}.buy_opener)) {
            const CleanSettings defaults;
            TrashList list;
            list.add_catalog(catalog, { CleanSettings::kBuyHpId, CleanSettings::kBuyMpId, defaults.buy_opener },
                             TrashList::Action::Delete, defaults.catalog_score);
            list.add_rivals(catalog);
            for (std::size_t s = 0; s < slots.size(); ++s) {
                if (bag::is_empty(f, slots[s], l)) continue;
                const TrashList::Verdict k = list.judge(f, slots[s], defaults.catalog_score);
                if (k.item >= 0)
                    std::printf("  [info]   Khác slot %zu (row %zu col %zu): %s at %.3f\n", s, s / 6 + 1, s % 6 + 1,
                                list.items[static_cast<std::size_t>(k.item)].id.c_str(), k.score);
                else if (k.like >= 0)
                    std::printf("  [info]   Khác slot %zu: looks like %s (%.3f) but %s is as near (%.3f)\n", s,
                                list.items[static_cast<std::size_t>(k.like)].id.c_str(), k.like_score,
                                k.rival.c_str(), k.rival_score);
            }
        }
    }
}

void test_shop_pieces() {
    section("Mua bình: the shop, its question and the pictures it needs");
    if (skip_without_kit()) return;
    say_saved_bags();
    if (!kit().shop_ok) {
        skip("the shop's pictures are not installed here\n");
        return;
    }
    const Kit& k = kit();
    {
        const Frame& row = *k.store.get(bag::kShopRow);
        const Frame& buttons = *k.store.get(bag::kShopButtons);
        Frame f = scene(1024, 768, 60);
        blit(f, row, 100, 50);
        blit(f, buttons, 100 + bag::kShopButtonsDx, 50 + bag::kShopButtonsDy);
        const bag::Shop s = bag::find_shop(f, k.store);
        check(s.found && s.row.x == 100 && s.row.y == 50 && s.buttons.x == 100 + bag::kShopButtonsDx &&
                  s.buttons.y == 50 + bag::kShopButtonsDy && s.mua_x() == 100 + bag::kShopButtonsDx + bag::kShopMuaX,
              "the shop found by its first row and its buttons where the row says");
        Frame row_only = scene(1024, 768, 60);
        blit(row_only, row, 100, 50);
        Frame buttons_only = scene(1024, 768, 60);
        blit(buttons_only, buttons, 300, 400);
        Frame wrong_place = scene(1024, 768, 60);
        blit(wrong_place, row, 100, 50);
        blit(wrong_place, buttons, 100 + bag::kShopButtonsDx + 40, 50 + bag::kShopButtonsDy);
        // The row lower in the list, the buttons where they were: 257 under it,
        // as measured on the live client.
        Frame second = scene(1024, 768, 60);
        blit(second, row, 100, 92);
        blit(second, buttons, 100 + bag::kShopButtonsDx, 50 + bag::kShopButtonsDy);
        const bag::Shop s2 = bag::find_shop(second, k.store);
        check(s2.found && s2.row.y == 92 && s2.buttons.y == 50 + bag::kShopButtonsDy &&
                  s2.hp_x() == 100 + bag::kShopHpX && s2.item_y() == 92 + bag::kShopItemY,
              "the row second in the list is found with the buttons 257 under it, and its items clicked where it is");
        Frame above = scene(1024, 768, 60);
        blit(above, row, 100, 400);
        blit(above, buttons, 100 + bag::kShopButtonsDx, 300);
        check(!bag::find_shop(above, k.store).found, "never with the buttons above the row");
        check(!bag::find_shop(row_only, k.store).found && !bag::find_shop(buttons_only, k.store).found &&
                  !bag::find_shop(wrong_place, k.store).found && !bag::find_shop(scene(1024, 768, 60), k.store).found,
              "never by one alone, nor by buttons where the row does not have them");
        Frame full = scene(1024, 768, 60);
        blit(full, *k.store.get(bag::kShopFull), 300, 200);
        ScreenRect at;
        check(bag::shop_full_up(full, k.store, &at) && at.x == 300 && at.y == 200 &&
                  !bag::shop_full_up(scene(1024, 768, 60), k.store),
              "the full-bag message found where it is");
        // Another message in the same frame -- its line of words rubbed out --
        // is not that message.
        Frame other = *k.store.get(bag::kShopFull);
        fill(other, ScreenRect{ 20, 22, 158, 16 }, 40, 44, 52);
        Frame other_up = scene(1024, 768, 60);
        blit(other_up, other, 300, 200);
        check(!bag::shop_full_up(other_up, k.store), "and not by its frame alone: with its words gone it is not found");
    }
    {
        Pretend g;
        make_bag(g);
        g.tabs_work = true;
        g.shop_on = true;
        g.qty_up = true;
        g.shown = k.buy_question;
        g.shown_pair = g.question_pair(g.shown);
        const auto asks = [&](const Frame& frame, float score = CleanSettings::kBuyFrameDefault) {
            const auto dialogs = bag::find_dialogs(frame, k.store);
            return dialogs.size() == 1 && bag::shop_question(frame, k.store, dialogs.front(), k.counts, score);
        };
        for (const char* typed : { "1", "", "100", "10" }) {
            g.typed = typed;
            check(asks(g.render()), std::string("the buy question taken for the shop's, its box holding '") + typed + "'");
        }
        Frame sell = scene(1024, 768, 60), bin = scene(1024, 768, 60), revive = scene(1024, 768, 60),
              reg = scene(1024, 768, 60), plain = scene(1024, 768, 60), drop = scene(1024, 768, 60);
        blit(sell, *k.store.get(bag::kSellQuestion), Pretend::kQX, Pretend::kQY);
        blit(bin, *k.store.get("grabbed/del_item_confirm_2"), Pretend::kQX, Pretend::kQY);
        blit(revive, *k.store.get("grabbed/hoisinh_confirm"), Pretend::kQX, Pretend::kQY);
        blit(drop, *k.store.get("grabbed/drop_confirm"), Pretend::kQX, Pretend::kQY);
        blit(reg, k.question, Pretend::kQX, Pretend::kQY);
        blit(plain, *k.store.get("grabbed/confirm_button"), Pretend::kQX + 100, Pretend::kQY + 50);
        check(!asks(sell), "the sell question is not the buy question: its words are the sell question's");
        check(!asks(bin) && !asks(revive) && !asks(reg) && !asks(drop) && !asks(plain),
              "nor the bin's, the revive, the drop or the registration question, nor two buttons alone");
        check(!bag::shop_question(g.render(), k.store, bag::find_dialogs(g.render(), k.store).front(), CountReader{}),
              "and no question at all without the digits to read its box with");
    }
    {
        check(bag::can_check_shop(k.store) && !bag::can_check_shop(k.bare), "the pictures a purchase needs");
        for (const char* missing : { bag::kShopRow, bag::kShopButtons, bag::kShopFull, bag::kSellQuestion,
                                     "grabbed/hoisinh_confirm", "grabbed/drop_confirm", "grabbed/confirm_button",
                                     "grabbed/del_item_confirm_2", bag::kCloseButtons[0] }) {
            TemplateStore without;
            for (const char* name : { "grabbed/inventory", "inven/0/trash_can", "grabbed/confirm_button",
                                      "grabbed/del_item_confirm_1", "grabbed/del_item_confirm_2",
                                      "grabbed/dialog_ok", "grabbed/hoisinh_confirm", "grabbed/drop_confirm",
                                      bag::kSellQuestion, bag::kShopRow, bag::kShopButtons, bag::kShopFull,
                                      bag::kCloseButtons[0], bag::kCloseButtons[1], bag::kCloseButtons[2] }) {
                if (std::string(name) == missing) continue;
                if (const Frame* f = k.store.get(name)) without.put(name, *f);
            }
            check(!bag::can_check_shop(without), std::string("no purchase without ") + missing);
        }
    }
    {
        const std::string path = temp_path("gobot_shop_trash.ini");
        CleanSettings s;
        write_all(path, "[Trash]\nenabled=false\n");
        check(s.load(path) && !s.buy_potions && s.buy_every_minutes == 60 && s.buy_hp_below == 3 && s.buy_hp_to == 6 &&
                  s.buy_mp_below == 3 && s.buy_mp_to == 6 && s.buy_opener == "other/8950604",
              "a file without them: off, an hour, 3 to 6 slots of each, the portrait of the item");
        write_all(path, "[Trash]\nbuy_potions=true\nbuy_every_minutes=30\nbuy_hp_below=2\nbuy_hp_to=9\n"
                        "buy_mp_below=0\nbuy_mp_to=4\nbuy_opener=other/1\nbuy_frame_score=0.8\n");
        check(s.load(path) && s.buy_potions && s.buy_every_minutes == 30 && s.buy_hp_below == 2 && s.buy_hp_to == 9 &&
                  s.buy_mp_below == 0 && s.buy_mp_to == 4 && s.buy_opener == "other/1" &&
                  s.buy_frame_score > 0.79f && s.buy_frame_score < 0.81f,
              "read back");
        write_all(path, "[Trash]\nenabled=false\n");
        check(s.load(path) && !s.buy_potions, "and off again when the line is taken out: never left as an earlier reading had it");
        write_all(path, "[Trash]\nbuy_every_minutes=1\nbuy_hp_below=50\nbuy_hp_to=4\nbuy_mp_to=999\nbuy_frame_score=0.1\n");
        check(s.load(path) && s.buy_every_minutes == CleanSettings::kBuyEveryLeast && s.buy_hp_to == 4 &&
                  s.buy_hp_below == 4 && s.buy_mp_to == CleanSettings::kBuySlotsMost &&
                  s.buy_frame_score == CleanSettings::kBuyFrameLeast,
              "numbers out of range brought in, a threshold never over its limit");
        write_all(path, "[Trash]\nbuy_opener=\n");
        check(s.load(path) && s.buy_opener == "other/8950604", "an empty opener is the portrait");
        // "Tốc độ bấm nút": 80 when the file says nothing, each step read back,
        // and anything else brought into 60..150.
        write_all(path, "[Trash]\nenabled=false\n");
        check(s.load(path) && s.press_ms == 80, "a file without press_ms: 80, as GoBot has always pressed");
        bool steps = true;
        for (const int ms : { 60, 80, 100, 150 }) {
            write_all(path, "[Trash]\npress_ms=" + std::to_string(ms) + "\n");
            steps = steps && s.load(path) && s.press_ms == ms;
        }
        write_all(path, "[Trash]\npress_ms=10\n");
        const bool low = s.load(path) && s.press_ms == CleanSettings::kPressLeastMs;
        write_all(path, "[Trash]\npress_ms=999\n");
        const bool high = s.load(path) && s.press_ms == CleanSettings::kPressMostMs;
        check(steps && low && high, "press_ms: 60, 80, 100 and 150 read back; 10 read as 60 and 999 as 150");
        DeleteFileA(path.c_str());
#ifdef GOBOT_SOURCE_DIR
        CleanSettings shipped;
        const std::string text = read_all(std::string(GOBOT_SOURCE_DIR) + "/configs/trash.ini");
        check(shipped.load(std::string(GOBOT_SOURCE_DIR) + "/configs/trash.ini") && !shipped.buy_potions &&
                  text.find("\nbuy_potions=false\n") != std::string::npos,
              "the shipped trash.ini ships the potion shop off: buy_potions=false");
        check(shipped.press_ms == 80 && text.find("\npress_ms=80\n") != std::string::npos,
              "and presses at 80 ms, as ever: press_ms=80");
#endif
    }
}

/// The log from here on, kept in memory at Info for as long as this lives:
/// what a run says of the bag's tabs goes only there.
struct LogLines {
    LogLevel was = Logger::instance().level();
    LogLines() {
        Logger::instance().set_level(LogLevel::Info);
        Logger::instance().set_ring_capacity(4000);
        Logger::instance().clear_ring();
    }
    ~LogLines() {
        Logger::instance().set_level(was);
        Logger::instance().set_ring_capacity(0);
    }
    LogLines(const LogLines&) = delete;
    LogLines& operator=(const LogLines&) = delete;
    std::string line_with(const std::string& part) const {
        for (const LogLine& l : Logger::instance().ring())
            if (l.text.find(part) != std::string::npos) return l.text;
        return {};
    }
    bool has(const std::string& part) const { return !line_with(part).empty(); }
};

/// The five numbers after "tab means " in a line of the log; none when they
/// are not there.
std::vector<int> means_in(const std::string& line) {
    std::vector<int> out;
    const std::size_t at = line.find("tab means ");
    if (at == std::string::npos) return out;
    int m[kBagTabs] = {};
    if (std::sscanf(line.c_str() + at + 10, "%d %d %d %d %d", &m[0], &m[1], &m[2], &m[3], &m[4]) == kBagTabs)
        out.assign(m, m + kBagTabs);
    return out;
}

std::string means_of(const BagView& v) {
    std::string out;
    for (int t = 0; t < kBagTabs; ++t) out += (t ? " " : "") + std::to_string(v.tab_mean[t]);
    return out;
}

/// The bag's strip read as no tab chosen -- -1 -- as on the live client in both
/// shop runs of 2026-11-03 (farm30 report, B4): looked at again, the pointer
/// moved off, before a run stops on it; another tab read for sure still stops
/// it; and nothing is counted, bought or right-clicked on a look that does not
/// read Tiêu Dùng.
void test_tab_unsure() {
    section("Túi đồ: a tab not read is looked at again before a stop, and never counted on (farm30 B4)");
    if (skip_without_kit()) return;
    {
        // The reading itself: every tab's mean grey kept, the chosen one the
        // darkest by kChosenTabDarker at least; with none drawn chosen, -1 and
        // the means kept all the same.
        Pretend g;
        make_bag(g);
        bool all = true, darker = true;
        for (int t = 0; t < kBagTabs; ++t) {
            all = all && g.v.tab_mean[t] >= 0;
            if (t != kUseTab) darker = darker && g.v.tab_mean[t] - g.v.tab_mean[kUseTab] >= 10;
        }
        check(g.v.selected_tab == kUseTab && all && darker,
              "the strip on Tiêu Dùng: read chosen, every tab's mean grey kept, Tiêu Dùng's darker than each by 10 or "
              "more -- " + means_of(g.v));
        g.unread = [](int) { return true; };
        const BagView blank = bag::find(g.look(), kit().store, g.l);
        check(blank.found && blank.selected_tab == -1 && blank.tab_mean[0] >= 0 && blank.tab_mean[kBagTabs - 1] >= 0 &&
                  g.unread_seen == 1,
              "drawn with no tab chosen: found, read -1, the means kept -- " + means_of(blank));
    }
    if (!kit().shop_ok) {
        skip("the shop's pictures are not installed here\n");
        return;
    }
    {
        ShopRun probe(0, 0);
        if (!probe.ok) {
            skip("the item catalog is not installed here\n");
            return;
        }
    }
    const std::string dir = temp_path("tab_unsure_debug");
    make_folder(dir);
    for (const std::string& name : files_in(dir, "*.png")) remove_file(dir + "\\" + name);
    const auto pictures = [&](const char* where) {
        return static_cast<int>(files_in(dir, std::string("*_tab_unsure_") + where + ".png").size());
    };
    const auto nothing_done = [](const ShopRun& s) {
        return s.r.shop_hp_before == -1 && s.r.shop_bought == 0 && s.p.game.right_clicks == 0 &&
               s.p.game.opener_clicks == 0 && s.p.game.digit_keys == 0;
    };

    // ---- the click on Tiêu Dùng (select_tab)
    {
        // The pointer left on the tab it clicked spoils the read, as the game
        // lights a control under it: moved off after the click, the tab is
        // read at once.
        ShopRun s(4, 4);
        s.p.game.cur_tab = kOtherTab;
        s.p.game.hover_unreads = true;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_hp_before == 4 && s.r.shop_mp_before == 4 && !s.r.tab_unsure &&
                  s.p.game.tab_clicks == 1 && s.p.game.unread_seen == 0 && s.p.game.moves >= 1 && s.p.says("đủ bình"),
              "the pointer on a tab spoils its read: moved off after the click, Tiêu Dùng read at once and counted, "
              "4 and 4 -- " + s.r.trouble + " / " + s.p.last());
        ShopRun t(4, 4);
        t.p.game.cur_tab = kOtherTab;
        t.p.game.hover_unreads = true;
        t.p.game.no_move = true;
        t.run();
        check(t.r.trouble.find("không chuyển được sang ngăn Tiêu Dùng") != std::string::npos && t.r.tab_unsure &&
                  nothing_done(t) && t.p.game.tab_clicks == 1 &&
                  t.p.game.unread_seen >= 10 + BagCleaner::kTabRelooks,
              "and with no way to move it, every look after the click reads no tab: looked at " +
                  std::to_string(BagCleaner::kTabRelooks) + " more times, then a stop -- nothing counted, nothing "
                  "right-clicked (" + std::to_string(t.p.game.unread_seen) + " looks with no tab) -- " + t.r.trouble);
    }
    {
        // No tab read for the ten short looks after the click and three more:
        // looked at again, and the shop goes on.
        ShopRun s(4, 4);
        s.p.game.cur_tab = kOtherTab;
        int from = -1;
        s.p.during = [&](BagCleaner&) {
            if (from < 0 && s.p.game.tab_clicks > 0) from = s.p.game.looks + 1;
        };
        s.p.game.unread = [&](int n) { return from >= 0 && n >= from && n < from + 13; };
        LogLines log;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_hp_before == 4 && s.r.shop_mp_before == 4 && !s.r.tab_unsure &&
                  s.p.game.tab_clicks == 1 && s.p.game.unread_seen == 13 &&
                  log.has("(Tiêu Dùng) read at shop_tab after 4 more looks -- the first read -1"),
              "-1 on the 10 looks after the click and 3 more: looked at again, Tiêu Dùng read on the 4th, one click, "
              "counted 4 and 4 -- " + s.r.trouble + " / " + log.line_with("read at shop_tab"));
    }
    {
        // No tab read on any look: one click, eight more looks, a stop -- and
        // its picture kept once a GoBot start, its numbers in the log.
        BagCleaner::forget_said_once();
        LogLines log;
        bool stopped[2] = { false, false };
        int kept[2] = { 0, 0 };
        for (int round = 0; round < 2; ++round) {
            ShopRun s(4, 4);
            s.p.game.unread = [](int) { return true; };
            s.p.debug_dir = dir;
            s.run();
            stopped[round] = s.r.trouble.find("không chuyển được sang ngăn Tiêu Dùng") != std::string::npos &&
                             s.r.tab_unsure && nothing_done(s) && s.p.game.tab_clicks == 1;
            kept[round] = pictures("shop_tab");
        }
        check(stopped[0] && stopped[1],
              "-1 on every look: Tiêu Dùng clicked once, looked at again, then a stop -- nothing counted, bought or "
              "right-clicked");
        check(kept[0] == 1 && kept[1] == 1,
              "one picture kept the first time, none the second: once a GoBot start, so the newest few dozen are not "
              "pushed out (" + std::to_string(kept[0]) + ", " + std::to_string(kept[1]) + ")");
        const std::string line = log.line_with("tab not read as 2 (Tiêu Dùng) at shop_tab -- read -1, strip at 300,300");
        const std::vector<int> m = means_in(line);
        check(m.size() == kBagTabs && *std::min_element(m.begin(), m.end()) >= 0,
              "the log says the tab wanted, the tab read, where the strip is, how alike, and every tab's mean -- " + line);
    }
    {
        // Khác read for sure on every look: Tiêu Dùng clicked once more, as
        // the game may have let the first click go, then a stop. A first
        // click let go: the second takes.
        ShopRun s(4, 4);
        s.p.game.cur_tab = kOtherTab;
        s.p.game.tabs_stuck = true;
        s.run();
        check(s.r.trouble.find("không chuyển được sang ngăn Tiêu Dùng") != std::string::npos && s.r.tab_unsure &&
                  nothing_done(s) && s.p.game.tab_clicks == 2 && s.p.game.unread_seen == 0,
              "Khác read for sure on every look: Tiêu Dùng clicked once more, then a stop -- nothing counted (" +
                  std::to_string(s.p.game.tab_clicks) + " clicks) -- " + s.r.trouble);
        ShopRun t(4, 4);
        t.p.game.cur_tab = kOtherTab;
        t.p.game.tab_clicks_lost = 1;
        t.run();
        check(t.r.trouble.empty() && t.r.shop_hp_before == 4 && t.r.shop_mp_before == 4 && t.p.game.tab_clicks == 2 &&
                  !t.r.tab_unsure,
              "the first click let go by the game: the second takes, and it counts as ever -- " + t.r.trouble);
    }

    // ---- the count (shop_count): the moment its first look reads no tab is
    // found by trying, so that the test does not hang on how many looks come
    // before it.
    int count_look = -1;
    for (int k = 2; k <= 16 && count_look < 0; ++k) {
        ShopRun s(4, 4);
        s.p.game.unread = [k](int n) { return n >= k && n < k + 3; };
        LogLines log;
        s.run();
        if (!log.has("(Tiêu Dùng) read at shop_count after 3 more looks")) continue;
        count_look = k;
        check(s.r.trouble.empty() && s.r.shop_hp_before == 4 && s.r.shop_mp_before == 4 && s.r.shop_bought == 0 &&
                  !s.r.tab_unsure && s.p.game.moves >= 3 && s.p.says("đủ bình"),
              "-1 on three looks of the count: looked at again, the pointer moved off each time, and counted all the "
              "same, 4 and 4, nothing bought (from look " + std::to_string(k) + ") -- " + s.r.trouble);
    }
    check(count_look > 0, "(a moment in the count with three looks that read no tab: found)");
    if (count_look > 0) {
        BagCleaner::forget_said_once();
        ShopRun s(4, 4);
        s.p.game.unread = [count_look](int n) { return n >= count_look; };
        s.p.debug_dir = dir;
        s.run();
        check(s.r.trouble.find("không chắc túi đồ đang ở ngăn Tiêu Dùng") != std::string::npos && s.r.tab_unsure &&
                  nothing_done(s) && s.p.game.moves >= BagCleaner::kTabRelooks && pictures("shop_count") == 1,
              "no tab read for good from there: " + std::to_string(BagCleaner::kTabRelooks) +
                  " more looks, then a stop, its picture kept -- nothing counted, bought or right-clicked -- " +
                  s.r.trouble);
        // Khác read there for sure: stopped at once, nothing looked at again.
        ShopRun o(4, 4);
        o.p.during = [&](BagCleaner&) {
            if (o.p.game.looks + 1 == count_look) o.p.game.cur_tab = kOtherTab;
        };
        o.run();
        check(o.r.trouble.find("không chắc túi đồ đang ở ngăn Tiêu Dùng") != std::string::npos && o.r.tab_unsure &&
                  nothing_done(o) && o.p.game.moves == 0 && o.p.game.unread_seen == 0,
              "Khác read for sure in the count: stopped at once, the pointer not moved, nothing looked at again or "
              "counted -- " + o.r.trouble);
    }

    // ---- the charms: Tiêu Dùng's click, and the sweep.
    {
        BagCleaner::forget_said_once();
        CharmRun c;
        c.p.game.unread = [](int) { return true; };
        c.p.debug_dir = dir;
        c.run();
        check(c.p.game.right_clicks == 0 && c.r.charms_used == 0 && c.r.charm_tries.empty() && c.r.tab_unsure &&
                  c.r.trouble.find("không chuyển được sang ngăn Tiêu Dùng") != std::string::npos &&
                  pictures("charm_tab") == 1,
              "charms, no tab read on any look: Tiêu Dùng clicked, looked at again, then a stop with its picture -- no "
              "charm right-clicked -- " + c.r.trouble);
    }
    int sweep_look = -1;
    for (int k = 2; k <= 12 && sweep_look < 0; ++k) {
        CharmRun c;
        c.p.game.unread = [k](int n) { return n >= k && n < k + 3; };
        LogLines log;
        c.run();
        if (!log.has("(Tiêu Dùng) read at charm_sweep after 3 more looks")) continue;
        sweep_look = k;
        check(c.p.game.right_clicks == 1 && c.r.charms_used == 1 && !c.r.tab_unsure && c.r.trouble.empty(),
              "charms, three looks of the sweep that read no tab: looked at again, and the charm used as ever (from "
              "look " + std::to_string(k) + ") -- " + c.p.last());
    }
    check(sweep_look > 0, "(a moment in the charm sweep with three looks that read no tab: found)");
    if (sweep_look > 0) {
        BagCleaner::forget_said_once();
        CharmRun c;
        c.p.game.unread = [sweep_look](int n) { return n >= sweep_look; };
        c.p.debug_dir = dir;
        c.run();
        check(c.p.game.right_clicks == 0 && c.r.charms_used == 0 && c.r.charm_tries.empty() && c.r.tab_unsure &&
                  c.r.trouble.empty() && c.p.says("không chắc túi đồ đang ở ngăn Tiêu Dùng, nên không dùng bùa nào") &&
                  pictures("charm_sweep") == 1,
              "and no tab read for good from there: the sweep stops, says so, keeps its picture, and is flagged "
              "tab_unsure -- not a bag with no charm in it -- with no charm right-clicked -- " + c.p.last());
    }
    for (const std::string& name : files_in(dir, "*.png")) remove_file(dir + "\\" + name);
    RemoveDirectoryA(dir.c_str());
}

/// What gui_app.cpp makes of it, where no pretend game reaches: the shop waits
/// for nobody at the mouse under its hold, the charm job says the tab was not
/// read and waits ten minutes; and the bag's own order -- the tab read before
/// anything is counted or right-clicked.
/// The band under the tabs' labels (2026.11.04d, F16): on 2026-11-04 the
/// shop's look read no tab -- Tiêu Dùng chosen, the game's red glow round its
/// top lifting its inside to the others' -- where the band under the labels
/// read it 68 against 121 and 122. The glow drawn here over a pretend strip;
/// the kept cuts of the live bag read as they are, when they are here.
void test_tab_band() {
    section("Túi đồ: the chosen tab read by the band under its label, a red glow round a tab or not (F16)");
    if (skip_without_kit()) return;
    const Kit& k = kit();
    const BagLayout l;
    // A tab's top rows lit, as the game's glow lights them: rows 1 to 5.
    const auto glow = [&l](Frame& f, int strip_x, int strip_y, int tab) {
        for (int y = strip_y + 1; y <= strip_y + 5; ++y)
            for (int x = strip_x + tab * l.tab_w; x < strip_x + (tab + 1) * l.tab_w; ++x) {
                std::uint8_t* p = &f.pixels[(static_cast<std::size_t>(y) * f.width + x) * 4];
                p[0] = 230;
                p[1] = 230;
                p[2] = 255;
            }
    };
    const auto inside_reads = [](const BagView& v, int tab) {
        for (int t = 0; t < kBagTabs; ++t)
            if (t != tab && v.tab_mean[t] - v.tab_mean[tab] < 10) return false;
        return true;
    };
    const auto band_by = [](const BagView& v, int tab) {
        int least = 255;
        for (int t = 0; t < kBagTabs; ++t)
            if (t != tab) least = std::min(least, v.tab_band[t] - v.tab_band[tab]);
        return least;
    };
    const auto bands = [](const BagView& v) {
        std::string out;
        for (int t = 0; t < kBagTabs; ++t) out += (t ? " " : "") + std::to_string(v.tab_band[t]);
        return out;
    };
    {
        Frame f = scene(1024, 768, 60);
        blit(f, strip_on(kUseTab), 300, 300);
        const BagView plain = bag::find(f, k.store, l);
        glow(f, 300, 300, kUseTab);
        const BagView lit = bag::find(f, k.store, l);
        check(plain.selected_tab == kUseTab && inside_reads(plain, kUseTab) && band_by(plain, kUseTab) >= 10,
              "the pretend strip on Tiêu Dùng: read so by the inside and by the band -- band " + bands(plain));
        check(lit.found && !inside_reads(lit, kUseTab) && lit.selected_tab == kUseTab && band_by(lit, kUseTab) >= 10,
              "Tiêu Dùng lit by the glow round its top: no longer darkest inside, still read by the band -- inside " +
                  means_of(lit) + ", band " + bands(lit));
        Frame other = scene(1024, 768, 60);
        blit(other, strip_on(kUseTab), 300, 300);
        glow(other, 300, 300, 3);
        Frame none = scene(1024, 768, 60);
        blit(none, strip_on(0), 300, 300);
        glow(none, 300, 300, kUseTab);
        check(bag::find(other, k.store, l).selected_tab == kUseTab && bag::find(none, k.store, l).selected_tab == -1,
              "the glow on Khác beside it: Tiêu Dùng all the same; no tab drawn chosen and Tiêu Dùng lit: none");
        // The band and the inside at odds -- Khác darkest inside, Tiêu Dùng
        // under its label: no tab, never the band's alone.
        Frame odds = scene(1024, 768, 60);
        const Frame s3 = strip_on(kOtherTab);
        blit(odds, s3, 300, 300);
        for (int y = 300 + s3.height - 4; y < 300 + s3.height; ++y)
            for (int x = 300 + kUseTab * l.tab_w; x < 300 + (kUseTab + 1) * l.tab_w; ++x) {
                std::uint8_t* q = &odds.pixels[(static_cast<std::size_t>(y) * odds.width + x) * 4];
                q[0] = q[1] = q[2] = 50;
            }
        const BagView at_odds = bag::find(odds, k.store, l);
        check(at_odds.found && inside_reads(at_odds, kOtherTab) && band_by(at_odds, kUseTab) >= 10 &&
                  at_odds.selected_tab == -1,
              "Khác darkest inside, Tiêu Dùng under its label: at odds, no tab -- inside " + means_of(at_odds) +
                  ", band " + bands(at_odds));
    }
    // The kept cuts of the live bag, Tiêu Dùng chosen: the band reads it by
    // far more than kChosenTabDarker.
    const std::string dir = charm_cuts_dir();
    if (dir.empty()) {
        skip("no cuts of the live bag here (handoff\\charms\\testdata): the band on the live client\n");
        return;
    }
    for (const char* what : { "charm_baga", "charm_bagb", "charm_bagc", "charm_bag_singles" }) {
        Frame f = scene(1024, 768, 40);
        if (!put_back(f, dir, what)) {
            skip("no %s cut here\n", what);
            continue;
        }
        const BagView v = bag::find(f, k.store, l);
        std::printf("  [info] %s: inside %s, band %s\n", what, means_of(v).c_str(), bands(v).c_str());
        check(v.found && v.selected_tab == kUseTab && band_by(v, kUseTab) >= 40,
              std::string(what) + ": Tiêu Dùng read, darker under its label than every other tab by 40 or more -- "
                                  "band " + bands(v));
    }
    // The shop's own look of 2026-11-04 20:40:00 (gobot_gui.log:289, kept as
    // shop_tab): Tiêu Dùng chosen under the game's red glow -- the inside
    // reads no tab, the band reads it.
    Frame f = scene(1024, 768, 40);
    if (!put_back(f, dir, "shop_tab_glow")) {
        skip("no shop_tab_glow cut here\n");
        return;
    }
    const BagView v = bag::find(f, k.store, l);
    std::printf("  [info] shop_tab_glow: inside %s, band %s\n", means_of(v).c_str(), bands(v).c_str());
    check(v.found && v.selected_tab == kUseTab && band_by(v, kUseTab) >= 40 && !inside_reads(v, kUseTab),
          "the shop's look of 20:40:00, Tiêu Dùng under the red glow: read by the band (by 40 or more), never by its "
          "inside -- inside " + means_of(v) + ", band " + bands(v));
}

void test_tab_unsure_sources() {
    section("Túi đồ: the shop's quiet moment and the charms' tab line, in their source (farm30 B4)");
#ifdef GOBOT_SOURCE_DIR
    const std::string gui = read_all(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/gui_app.cpp");
    const std::string cpp = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/bag.cpp");
    if (gui.empty() || cpp.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    const std::size_t npos = std::string::npos;
    const auto has = [](const std::string& text, const std::string& part) { return text.find(part) != npos; };
    const auto count_of = [](const std::string& text, const std::string& part) {
        int n = 0;
        for (std::size_t at = text.find(part); at != npos; at = text.find(part, at + 1)) ++n;
        return n;
    };
    const auto slice = [&](const std::string& text, const std::string& from, const std::string& to) {
        const std::size_t a = text.find(from);
        const std::size_t b = a == npos ? npos : text.find(to, a);
        return a == npos || b == npos ? std::string() : text.substr(a, b - a);
    };
    const auto body = [&](const std::string& text, const std::string& head) {
        const std::size_t a = text.find(head);
        const std::size_t b = a == npos ? npos : text.find("\n}", a);
        return a == npos || b == npos ? std::string() : text.substr(a, b + 2 - a);
    };

    // The shop: the hold, the keys let go, a quiet second, and only then the run.
    const std::string shop = slice(gui, "const auto run_shop = [&](bool forced, bool dry) {", "while (!trash_stop_) {");
    const std::size_t hold = shop.find("take_hold(\"trash cleaning\")");
    const std::size_t let_go = hold == npos ? npos : shop.find("input_.release_all();", hold);
    const std::size_t quiet = shop.find("const DWORD quiet = last_input();");
    const std::size_t asked = shop.find("touched = !trash_stop_ && !lost && last_input() != quiet;");
    const std::size_t ran = shop.find("r = cleaner.run();");
    check(hold != npos && let_go != npos && quiet != npos && asked != npos && ran != npos && hold < let_go &&
              let_go < quiet && quiet < asked && asked < ran &&
              has(shop, "for (int slept = 0; slept < kShopQuietMs && !trash_stop_ && !lost; slept += 50) {") &&
              has(gui, "constexpr int kShopQuietMs = 1000;"),
          "gui_app.cpp: run_shop takes the hold, lets the keys go, and runs only after a second with nobody at the "
          "mouse or keyboard");
    // A button's run looks for that second a few times under the one hold, the
    // hold and HP watched as the run watches them; a timed run once.
    check(has(shop, "const int tries = forced || dry ? kShopQuietTries : 1;") &&
              has(gui, "constexpr int kShopQuietTries = 5;") && has(shop, "lost = !still_ours();") &&
              has(shop, "cleaner.set_guard(still_ours);") &&
              has(shop, "else if (lost && hp_under())"),
          "gui_app.cpp: \"Mua ngay\" and \"Thử mua\" look for the quiet second up to 5 times, the hold and HP "
          "watched; a timed run once");
    check(has(shop, "not_now(\"Chờ: bạn đang dùng chuột hoặc bàn phím, mua bình sau.\", kShopTouchedMs);") &&
              has(gui, "constexpr std::uint64_t kShopTouchedMs = 30'000;") &&
              count_of(shop, "if (!held) end_hold(\"trash cleaning\");") == 2 && count_of(shop, "take_hold(") == 1,
          "and when somebody was, it lets the hold go as it was and asks again in 30 s, saying why");
    check(!has(gui, "line += \" Dừng: \" + r.trouble") &&
              count_of(gui, "line += (line.empty() ? \"\" : \" \") + std::string(\"Dừng: \") + r.trouble + \".\";") == 2,
          "gui_app.cpp: the shop's line no longer begins \"Trash:  Dừng\" with two spaces");

    // The charms: the tab line instead of "no charm there", and ten minutes.
    const std::string job = slice(gui, "const auto run_charms = [&](bool forced) {", "// ---- the potion shop ----");
    const std::size_t tab_branch = job.find("} else if (tab_stop) {");
    const std::size_t none = job.find("Không thấy bùa nào trong ngăn Tiêu Dùng của túi đồ.");
    check(has(job, "const bool tab_stop = r.tab_unsure && r.trouble.empty();") && tab_branch != npos && none != npos &&
              tab_branch < none && has(job, "không chắc túi đồ đang ở ngăn Tiêu Dùng, nên dừng dùng bùa"),
          "gui_app.cpp: a charm sweep stopped on the strip says the tab was not sure, before \"Không thấy bùa nào\" "
          "can be said");
    check(has(job, "if (!forced && r.tab_unsure) charm_check_ms = now_ms() + kCharmTabRetryMs;") &&
              has(gui, "constexpr std::uint64_t kCharmTabRetryMs = 600'000;") &&
              has(job, "r.trouble.empty() && !r.tab_unsure)"),
          "and the charms wait ten minutes after it, the kinds due not written down as not found");

    // bag.cpp: the pointer moved off after the tab's click; nothing counted or
    // right-clicked before the tab is read.
    const std::string pick_tab = body(cpp, "bool BagCleaner::select_tab(");
    const std::size_t clicked = pick_tab.find("click(tx, ty);");
    const std::size_t parked = pick_tab.find("park_pointer();");
    check(clicked != npos && parked != npos && clicked < parked && parked - clicked < 40 &&
              has(pick_tab, "more < kTabRelooks && v.found && v.selected_tab < 0") &&
              has(pick_tab, "if (clicks > 0 && (!v.found || v.selected_tab < 0)) break;"),
          "bag.cpp: select_tab moves the pointer off right after its click, looks again only while no tab is read, "
          "and clicks again only on another tab read for sure");
    const std::string counting = body(cpp, "bool BagCleaner::shop_count(");
    const std::size_t tab_check = counting.find("if (v.selected_tab != tab) {");
    const std::size_t relook = counting.find("v.found && v.selected_tab < 0 && unread < kTabRelooks");
    const std::size_t judged = counting.find("trash_->judge(");
    check(tab_check != npos && relook != npos && judged != npos && tab_check < relook && relook < judged &&
              count_of(counting, "trash_->judge(") == 1,
          "bag.cpp: shop_count looks again on a look with no tab read, and judges a slot only after the tab is read");
    const std::string sweep = body(cpp, "void BagCleaner::charm_sweep(");
    const std::size_t s_check = sweep.find("if (v.selected_tab != tab) {");
    const std::size_t s_relook = sweep.find("v.found && v.selected_tab < 0 && unread < kTabRelooks");
    const std::size_t s_flag = sweep.find("res.tab_unsure = true;");
    const std::size_t s_used = sweep.find("charm_one(");
    check(s_check != npos && s_relook != npos && s_flag != npos && s_used != npos && s_check < s_relook &&
              s_relook < s_flag && s_flag < s_used,
          "bag.cpp: charm_sweep looks again on a look with no tab read, flags the run when it stops on the strip, and "
          "right-clicks a charm only after the tab is read");
#else
    skip("built without the source tree's path\n");
#endif
}

/// The charm job builds the trash list every half minute: its folders are
/// read quietly, and only the start's own pictures say how many they are
/// (farm30 report, T6: 35 lines of "TemplateStore: 0 templates" in 18 min).
void test_trash_list_quiet() {
    section("Dọn rác: the trash list's folders read again without a line each time (farm30 T6)");
    const std::string dir = temp_path("quiet_trash");
    make_folder(dir);
    make_folder(dir + "\\delete");
    bool quiet = false, loaded = false, said = false;
    {
        LogLines log;
        TrashList list;
        loaded = list.load(dir, BagLayout{}) && list.items.empty();
        quiet = !log.has("TemplateStore:");
        TemplateStore loud;
        loud.load(dir + "\\delete");
        said = log.has("TemplateStore: 0 templates from " + dir + "\\delete");
    }
    RemoveDirectoryA((dir + "\\delete").c_str());
    RemoveDirectoryA(dir.c_str());
    check(loaded && quiet, "TrashList::load reads its folders with no line in the log");
    check(said, "and a TemplateStore loaded as ever still says how many pictures it has, and from where");
#ifdef GOBOT_SOURCE_DIR
    const std::string cpp = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/bag.cpp");
    const std::string tm = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/template_matcher.cpp");
    std::size_t quiet_loads = 0;
    for (std::size_t at = cpp.find(".load(sub, true)"); at != std::string::npos; at = cpp.find(".load(sub, true)", at + 1))
        ++quiet_loads;
    check(quiet_loads == 1 && tm.find("if (path_exists(staged) && load(staged))") != std::string::npos &&
              tm.find("if (!images.empty() && load(images))") != std::string::npos,
          "only the trash list reads quietly: the start's line (\"522 templates\") stays");
#endif
}

void test_use(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_pictures();
    test_registers();
    test_refuses();
    test_nothing();
    test_notice();
    test_order();
    test_use_over_delete();
    test_look_only();
    test_safety();
    test_rest_of_one_slot();
    test_gaps_closed();
    test_unproven();
    test_late_question();
    test_other_window();
    test_named_questions();
    test_fragments_only();
    test_save_order();
    test_settings();
    test_live_question();
    test_switch();
    test_whole_sequence();
    test_book_will_not_close();
    test_book_or_window_up();
    test_edited_question();
    test_charm_kinds();
    test_charm_colours();
    test_charm_colour_runs();
    test_charm_question();
    test_charm_run();
    test_charm_outcomes();
    test_charm_safety();
    test_charm_hold();
    test_charm_packets();
    test_charm_notice_pictures();
    test_charm_notice();
    test_live_charm();
    test_charm_singles_live();
    test_charm_pair_live();
    test_charm_singles_printed();
    test_charm_singles_run();
    test_charm_pair_run();
    test_charm_singles_sources();
    test_charm_clock();
    test_charm_settings();
    test_shop_pieces();
    test_shop();
    test_tab_unsure();
    test_tab_band();
    test_tab_unsure_sources();
    test_trash_list_quiet();
}
