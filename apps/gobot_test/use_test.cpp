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
#include "gobot/checkin_board.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/menu_text.hpp"
#include "gobot/popup_closer.hpp"
#include "gobot/quiet_second.hpp"
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
#include <thread>
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
    /// The check-in board's title (checkin_board.hpp), in the store under its
    /// file's name: the board drawn as its title, all GoBot looks for.
    bool board_ok = false;
    Frame board_title;
    /// The game's menu an ESC with nothing to close brings up (grabbed/
    /// client_menu_dialog), in the store under its name when it is installed.
    bool menu_ok = false;
    Frame menu;
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
        // The check-in board's title, by its file's name: the copy of
        // templates.ini beside the exe may be older than the alias.
        k.board_ok = TemplateStore::load_one(kCheckinBoardTitleFile, ini, k.board_title) && k.board_title.valid();
        if (k.board_ok) k.store.put(kCheckinBoardTitleFile, k.board_title);
        k.menu_ok = TemplateStore::load_one("grabbed/client_menu_dialog", ini, k.menu) && k.menu.valid();
        if (k.menu_ok) k.store.put("grabbed/client_menu_dialog", k.menu);
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
    /// The bag key's toggle drawn only this many looks after the key (2026.11.14b,
    /// a slow VM); the toggles still to be drawn, by the look they come on --
    /// a test may put one there itself (a shut on its way as the run begins).
    /// `bag_keys`: bag keys pressed; `bag_keys_taken`, how many the game takes
    /// (-1 all): those after it do nothing. `clicks_on_shut_bag`: clicks, of
    /// any kind, while the bag was shut and a tab was where one was clicked.
    int  bag_lag_looks = 0, bag_keys = 0, bag_keys_taken = -1, clicks_on_shut_bag = 0;
    std::vector<int> bag_flips_due;
    /// What an ESC does (2026.11.14b, esc_out): on the quantity question it
    /// is a Hủy Bỏ (`esc_closes_qty`); on the bag alone it shuts it
    /// (`esc_closes_bag`); on the bin's question it gives the item back, or
    /// closes it with the item left in the bin, or does nothing (`esc_on_bin`).
    /// Each counted; `esc_on_nothing`, an ESC with nothing at all up.
    enum class EscBin { GivesBack, Leaves, Ignores };
    bool esc_closes_qty = true, esc_closes_bag = true;
    EscBin esc_on_bin = EscBin::GivesBack;
    int  esc_on_qty = 0, esc_on_bag = 0, esc_on_bin_q = 0, esc_on_nothing = 0;
    /// The bin's question not drawn while the bag is (the 21:31 replay of
    /// 2026-11-14): hidden, it is seen only once the bag is shut. `bin_sticky`:
    /// its Hủy Bỏ does nothing; `bin_box_shows`, the number its box shows
    /// when 0 or more, the stack's count otherwise.
    bool bin_ask_hidden_while_bag = false, bin_sticky = false;
    int  bin_box_shows = -1;
    /// Clicks on the bin while it holds an item or asks about one (the review
    /// of 2026.11.14b: on 2026-11-14 at 21:31 two more brought up nothing).
    int  bin_clicks = 0;
    /// An ESC on the bag drawn shut only this many looks later (0: at once),
    /// a slow game; an ESC meanwhile has nothing left to close. With
    /// `menu_on_esc`, an ESC with nothing to close brings up the game's menu
    /// (`menu_up`, kit().menu at kMX,kMY, its X where the live client has
    /// it): a click on the X shuts it -- but the next `menu_x_misses` --
    /// any other click or key goes to it, an ESC shutting it.
    int  esc_bag_lag_looks = 0;
    std::vector<int> esc_shuts_due;
    bool menu_on_esc = false, menu_up = false;
    int  menu_shows = 0, menu_x_clicks = 0, menu_x_misses = 0, menu_other_clicks = 0, esc_on_menu = 0;
    static constexpr int kMX = 20, kMY = 500;
    static ScreenRect menu_x_rect() {
        const Frame& x = kit().close_x;
        return ScreenRect{ kMX + PopupEyes::kMenuXDx, kMY + PopupEyes::kMenuXDy, x.width, x.height };
    }
    void show_menu() {
        if (!menu_on_esc || menu_up || !kit().menu_ok) return;
        menu_up = true;
        ++menu_shows;
    }
    /// The digit keys there had been at the first click into the quantity
    /// box (-1: none): the box is never clicked into before 00 is typed.
    int  digits_at_box_click = -1;
    bool bag_drawn() const { return !bag_shut; }
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
    /// `use_tab_clicks`: those on Tiêu Dùng, which no shop run makes.
    int  tab_clicks = 0, tab_clicks_lost = 0, use_tab_clicks = 0;
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
    /// The question with its Hủy Bỏ drawn pressed from the start, which
    /// find_dialogs does not see as a dialog.
    bool qty_hidden = false;
    /// What the bag has room for: purchases that go through before the game
    /// says it is full (-1: no end). With none left the message comes in place
    /// of the quantity box -- as GoC has it -- or, with `full_late_ms` 0 or
    /// more, that long after Xác Nhận on the box, that purchase not made.
    int  room = -1;
    int  full_late_ms = -1;
    bool full_up = false, full_due = false;
    std::chrono::steady_clock::time_point full_at{};
    bool full_sticks = false;       ///< a click on its Xác Nhận does not close it
    bool full_lit = false;          ///< its Xác Nhận drawn lit, as with the pointer over it
    /// Clicks on its Xác Nhận, and where; how often it came up; keys and ESC
    /// while it was up; and the digit keys there had been when it first did.
    int  full_clicks = 0, full_shows = 0, keys_with_full = 0, esc_with_full = 0, digits_at_full = -1;
    std::vector<std::pair<int, int>> full_click_at;
    /// A message GoBot does not know -- grabbed/dialog_ok -- where questions
    /// come up: in place of the quantity box (`lone_instead`), for one look and
    /// then the box (`lone_flash`), or once the question is gone after Xác
    /// Nhận, the purchase made (`lone_after_confirm`). A click on its button
    /// closes it; any other click is the message's.
    bool lone_up = false, lone_instead = false, lone_flash = false, lone_after_confirm = false;
    bool lone_sticks = false;       ///< a click on its Xác Nhận does not close it
    int  lone_clicks = 0, lone_looks = 0, esc_with_lone = 0;
    bool lone_focused = false;
    /// Its Xác Nhận drawn lit, as with the pointer over it: lone_ok misses
    /// it, the notice's whole picture does not (the review's KEYS-1).
    bool lone_lit = false;
    bool confirm_ignored = false;   ///< Xác Nhận on the question does nothing
    /// The check-in board, drawn as its title -- all GoBot looks for -- where
    /// nothing else is; ESC closes it. Clicks, double-clicks, right-clicks and
    /// keys that reached the game while it was up.
    bool board_on = false;
    int  acts_with_board = 0;
    static constexpr int kBoardX = 10, kBoardY = 200;
    /// The board drawn whole, as the live one is: an opaque panel in front of
    /// everything over the shop -- its row, its Mua | Thoát -- and where the
    /// quantity box and the messages come up, its title on it; the bag left
    /// clear. It takes every key and click: ESC closes it, nothing else does
    /// anything.
    bool board_whole = false;
    static constexpr ScreenRect kBoardPanel{ 580, 0, 444, 360 };
    static constexpr int kWholeTitleX = 620, kWholeTitleY = 5;
    bool esc_closes_shop = true;
    /// The 2026.11.24 review: the shop opens again on the tab last used
    /// (`shop_keeps_tab`), and its Thoát takes no click (`thoat_dead`).
    bool shop_keeps_tab = false, thoat_dead = false;
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
    int  doubles_under = 0;         ///< double-clicks with the board, a message or the full message up
    int  doubles_from_open = 0;     ///< double-clicks whose last look was taken with the key window open
    void show_full() {
        full_up = true;
        ++full_shows;
        if (digits_at_full < 0) digits_at_full = digit_keys;
    }
    /// The quantity box up, its number the game's 1, its caret after it --
    /// and then the keys of the macro's in `box_keys_next`, taken late by the
    /// game (test_nav_keys).
    void open_box(bool focused) {
        qty_up = true;
        typed = "1";
        caret = -1;
        box_focused = focused;
        cancel_pressed = qty_hidden;
        shown = qty_is_sell ? *kit().store.get(bag::kSellQuestion) : kit().buy_question;
        shown_pair = question_pair(shown);
        for (const std::uint32_t vk : box_keys_next) macro_key_in_box(vk);
        box_keys_next.clear();
    }
    // Home, End, PgUp, PgDn, Insert and Delete (test_nav_keys, 2026.11.11).
    // The shop's list scrolled: its potions' row out of sight -- for good with
    // `row_scrolled`, for the next `row_hidden_looks` looks -- the buttons
    // where they were, and a double-click at the old cells landing on another
    // item (`wrong_item_doubles`, bought as 2). A key of the macro's in the
    // box, taken by the game as it comes up (`box_keys_next`, counted in
    // `box_macro_keys`, never in digit_keys): Home and End move its caret
    // (`caret`, -1 at the end), Delete erases the digit after it.
    bool row_scrolled = false;
    int  row_hidden_looks = 0, wrong_item_doubles = 0, box_macro_keys = 0;
    int  caret = -1;
    std::vector<std::uint32_t> box_keys_next;
    bool row_off() const { return row_scrolled || row_hidden_looks > 0; }
    void macro_key_in_box(std::uint32_t vk) {
        ++box_macro_keys;
        if (vk == VK_HOME) caret = 0;
        else if (vk == VK_END) caret = -1;
        else if (vk == VK_DELETE && caret >= 0 && caret < static_cast<int>(typed.size()))
            typed.erase(static_cast<std::size_t>(caret), 1);
    }
    void open_question(bool focused) {
        // The potions' room only: the pet's food has its own (pet_room).
        if (room == 0 && full_late_ms < 0 && shop_sel < 3) {
            show_full();
            return;
        }
        if (lone_instead || lone_flash) {
            lone_up = true;
            lone_looks = lone_flash ? 1 : 0;
            lone_focused = focused;
            return;
        }
        open_box(focused);
    }
    /// The full-bag message's Xác Nhận, where its picture has it: the label at
    /// 148,58 of its top-left, the button round it.
    static ScreenRect full_ok_rect() { return ScreenRect{ kFX + 144, kFY + 54, 42, 21 }; }
    /// The message GoBot does not know: where it is drawn, and its button.
    static ScreenRect lone_rect() {
        const Frame& ok = *kit().store.get("grabbed/dialog_ok");
        return ScreenRect{ kQX, kQY, ok.width, ok.height };
    }
    static ScreenRect lone_button() {
        const ScreenRect in = notice_label();
        return ScreenRect{ kQX + in.x - 4, kQY + in.y - 4, 42, 21 };
    }
    /// The potion's cell at (x, y): 0 Thịt bò khô, 1 Rượu lá tre, -1 none --
    /// none on another tab of the shop than Dược Phẩm (2026.11.24).
    int cell_at(int x, int y) const {
        if (pet_tabs && shop_tab != bag::kShopTabPotions) return -1;
        const ScreenRect hp{ kSX + bag::kShopHpX - 14, kSY + row_drop + bag::kShopItemY - 8, 28, 16 };
        const ScreenRect mp{ kSX + bag::kShopMpX - 14, kSY + row_drop + bag::kShopItemY - 8, 28, 16 };
        return hp.contains(x, y) ? 0 : mp.contains(x, y) ? 1 : -1;
    }
    int  box_clicks = 0, scan_keys = 0;
    int  key_drops = 0;             ///< every this-many digit keys is lost (0: none)
    std::string typed = "1";
    std::vector<int> buys;          ///< what each confirmed purchase was (0 HP, 1 MP)
    int  digit_keys = 0, backspaces = 0, esc_presses = 0, thoat_clicks = 0, opener_clicks = 0;
    int kSX = 700, kSY = 20;        ///< where the shop's row is: moved over the bag by a test
    /// How far down the list the potions' row is: 42 a row, the buttons where
    /// they are -- the live shop has had two other items above the potions.
    int row_drop = 0;
    // The shop's tabs and the pet's food (test_pet_shop, 2026.11.24). With
    // `pet_tabs` the strip of five tabs is drawn where the live client has it
    // from the buttons (bag::shop_tab_rect), the tab chosen dark and the rest
    // grey, and a click on a tab chooses it -- but the next
    // `shop_tab_clicks_lost`, and none with `shop_tabs_stuck`. On Linh Thú the
    // potions' row is not drawn; `pet_cells` are, each name right-aligned as
    // the game draws them (MenuText's own letters, pale yellow on the dark name
    // field), and a double-click on one's icon brings up the quantity question
    // for it -- bought as 3 when it is the pet's Thịtbò, else as 4. With
    // `pet_room` 0 the game's message that its bag is full -- one GoBot has no
    // picture of, so dialog_ok -- comes in place of the box (-1: no end).
    bool pet_tabs = false, shop_tabs_stuck = false;
    int  shop_tab = 0, shop_tab_clicks = 0, shop_tab_clicks_lost = 0;
    std::vector<int> shop_tab_clicked;      ///< the tab each click on the strip went to
    struct PetCell {
        int column = 1, row = 0;
        std::string name = CleanSettings::kBuyPetName;
    };
    std::vector<PetCell> pet_cells;
    int  pet_room = -1;
    ScreenRect pet_cell_rect(const PetCell& c) const {
        const ScreenRect b = shop_buttons_rect();
        return ScreenRect{ b.x - bag::kShopButtonsDx + c.column * bag::kShopCellPitch,
                           b.y - bag::kShopButtonsDy + 42 * c.row, 150, 40 };
    }
    ScreenRect pet_icon_rect(const PetCell& c) const {
        const ScreenRect r = pet_cell_rect(c);
        return ScreenRect{ r.x + 1, r.y + 1, 34, 36 };
    }
    /// A cell of the list as the game draws it: the icon's dark box, the name
    /// field, the name right-aligned on it in MenuText's first face at 12 px.
    void draw_pet_cell(Frame& f, const PetCell& c) const {
        const ScreenRect r = pet_cell_rect(c);
        fill(f, pet_icon_rect(c), 30, 26, 22);
        fill(f, ScreenRect{ r.x + bag::kShopNameFieldX, r.y + 2, bag::kShopNameFieldEndX - bag::kShopNameFieldX + 1, 17 },
             41, 41, 49);
        const std::vector<std::string> faces = MenuText::faces();
        const Frame ink = faces.empty() ? Frame{} : MenuText::render(c.name, faces.front(), 12, false);
        if (!ink.valid()) return;
        // The ink's last column (the render keeps a pixel round it) on the
        // names' end.
        const int x0 = r.x + bag::kShopNameLastX - (ink.width - 2), y0 = r.y + 3;
        for (int y = 0; y < ink.height; ++y)
            for (int x = 0; x < ink.width; ++x)
                if (ink.pixels[(static_cast<std::size_t>(y) * ink.width + x) * 4 + 1] >= 128)
                    put_px(f, x0 + x, y0 + y, 255, 255, 155);
    }
    bool on_pet_tab() const { return pet_tabs && shop_tab == bag::kShopTabPet; }
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
        if (qty_up) {
            const ScreenRect p = shown_pair;
            const ScreenRect box{ p.x - 88, p.y + 3, 82, 15 };
            if (box.contains(x, y)) {
                ++box_clicks;
                if (digits_at_box_click < 0) digits_at_box_click = digit_keys;
                if (!click_dead) {
                    box_focused = true;
                    cancel_pressed = false;
                }
                return true;
            }
            if (!p.contains(x, y)) return true;
            if (x < p.x + p.w / 2) {
                if (confirm_ignored) return true;
                last_confirm = time.t;
                if (typed == "100" && !qty_is_sell) {
                    if (shop_sel >= 3) {
                        // The pet's food, or another item of Linh Thú
                        // (2026.11.24): bought, the pet's room one less.
                        buys.push_back(shop_sel);
                        if (shop_sel == 3 && pet_room > 0) --pet_room;
                        if (lone_after_confirm) lone_up = true;
                    } else if (room == 0) {
                        // No room: the game says so a while on, and nothing is bought.
                        full_due = true;
                        full_at = time.t + std::chrono::milliseconds(full_late_ms);
                    } else {
                        buys.push_back(shop_sel);
                        if (room > 0) --room;
                        if (lone_after_confirm) lone_up = true;
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
        // A tab of the shop (2026.11.24): chosen, but for a click lost.
        if (pet_tabs) {
            const ScreenRect b = shop_buttons_rect();
            for (int t = 0; t < bag::kShopTabs; ++t) {
                if (!bag::shop_tab_rect(b, t).contains(x, y)) continue;
                ++shop_tab_clicks;
                shop_tab_clicked.push_back(t);
                if (shop_tabs_stuck) return true;
                if (shop_tab_clicks_lost > 0) {
                    --shop_tab_clicks_lost;
                    return true;
                }
                shop_tab = t;
                return true;
            }
        }
        const ScreenRect buttons = shop_buttons_rect();
        if (buttons.contains(x, y)) {
            if (x >= buttons.x + buttons.w / 2) {
                ++thoat_clicks;
                if (thoat_dead) return true;
                shop_on = false;
                shop_sel = -1;
                if (!shop_keeps_tab) shop_tab = bag::kShopTabPotions;
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
    int drags = 0, bin_confirms = 0, bin_cancels = 0, keys = 0, slot_clicks = 0, clicks = 0;
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
        if (tabs_work && !bag_shut) blit(f, strip_on(cur_tab), 300, 300);
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
            // The potions' row on Dược Phẩm only; Linh Thú's cells on it
            // (2026.11.24), the strip of tabs over the list.
            if (!row_off() && !(pet_tabs && shop_tab != bag::kShopTabPotions))
                blit(f, *k.store.get(bag::kShopRow), kSX, kSY + row_drop);
            const ScreenRect b = shop_buttons_rect();
            blit(f, *k.store.get(bag::kShopButtons), b.x, b.y);
            if (pet_tabs) {
                for (int t = 0; t < bag::kShopTabs; ++t) {
                    const ScreenRect r = bag::shop_tab_rect(b, t);
                    if (t == shop_tab) fill(f, r, 14, 14, 14);
                    else fill(f, r, 115, 123, 132);
                    fill(f, ScreenRect{ r.x, r.y, r.w, 1 }, 206, 206, 206);
                }
                if (on_pet_tab())
                    for (const PetCell& c : pet_cells) draw_pet_cell(f, c);
            }
        }
        if (full_up) {
            blit(f, *k.store.get(bag::kShopFull), kFX, kFY);
            if (full_lit) light(f, full_ok_rect());
        }
        if (lone_up) {
            blit(f, *k.store.get("grabbed/dialog_ok"), kQX, kQY);
            if (lone_lit) light(f, lone_button());
        }
        if (board_on && !board_whole) blit(f, k.board_title, kBoardX, kBoardY);
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
        if (up == Up::BinAsking && !(bin_ask_hidden_while_bag && !bag_shut)) {
            blit(f, *k.store.get("grabbed/del_item_confirm_2"), kBX, kBY);
            fill(f, ScreenRect{ kBX, kBY + 49, 196, 30 }, 216, 216, 216);
            const ScreenRect box = box_rect();
            for (int yy = box.y; yy < box.bottom(); ++yy)
                for (int xx = box.x; xx < box.right(); ++xx)
                    put_px(f, xx, yy, 65 + (yy - box.y), 65 + (yy - box.y), 70 + (yy - box.y));
            const Item& it = items[static_cast<std::size_t>(about)];
            print_number(f, box.x + 7, box.y + 6, std::to_string(bin_box_shows >= 0 ? bin_box_shows : it.count),
                         k.font_dir);
            blit(f, *k.store.get("grabbed/confirm_button"), bin_pair().x, bin_pair().y);
        }
        if (board_on && board_whole) {
            fill(f, kBoardPanel, 46, 34, 24);
            blit(f, k.board_title, kWholeTitleX, kWholeTitleY);
        }
        // The game's menu in front of everything, its X on it.
        if (menu_up) {
            blit(f, k.menu, kMX, kMY);
            blit(f, k.close_x, menu_x_rect().x, menu_x_rect().y);
        }
        return f;
    }

    // The hold's key window (test_key_window, 2026.11.10), as the cleaner's
    // KeyWindow opens and shuts it: whether it is open; how often each; what
    // reached the game with it open; whether it was opened with something up
    // or out, or in a purchase's money stretch -- from its double-click until
    // kShopFullLateMs after its Xác Nhận; and the looks taken with it open
    // while the quantity box, a message, the board or a dialog was up. With
    // `window_potions`, a potion key goes down as it opens and at each look
    // while it is open; a drag judged on a look taken with it open, or less
    // than 500 ms after such a potion, is counted. `shut_refusals`: the next
    // shuts that say a key is still down.
    bool keys_open = false;
    int  keys_opens = 0, keys_shuts = 0, acts_with_keys = 0, opens_unclean = 0, opens_in_money = 0;
    int  looks_open_unclean = 0, drags_from_open = 0, drags_soon = 0, shut_refusals = 0;
    int  rights_from_open = 0, rights_soon = 0;     ///< the same for a right-click
    bool window_potions = false, potion_pressed = false;
    std::chrono::steady_clock::time_point potion_at{}, look_at{}, last_double{}, last_confirm{};
    bool look_open = false;
    /// Asked at every shut, before it answers: a test's way to have something
    /// come up -- the board, a message -- as the keys let through are waited
    /// up (the review's K1).
    std::function<void()> on_shut;
    /// The searches made so far (bag::search_counts) as each look was taken,
    /// and each opening of the window: how many looks there were, and how many
    /// purchases (the review's T1). Looks taken with it open in the shop once
    /// something is bought: the keys' time between purchases.
    std::vector<bag::SearchCounts> counts_at_look;
    std::vector<std::pair<int, std::size_t>> opened_on;
    int looks_open_bought = 0;
    bool clean_now() const {
        return up == Up::None && !qty_up && !full_up && !lone_up && !board_on && !msg_on;
    }
    void press_potion() {
        if (!window_potions) return;
        potion_pressed = true;
        potion_at = time.t;
    }
    // The potion break (test_potion_break, 2026.11.11): `vitals_due` HP or MP
    // potions due whose key the window lets through -- what KeyWindow::
    // potion_due says while any is -- one drunk each time the cleaner asks
    // with the window open, its key down then and 60 ms of the game's time
    // gone; `vitals_drunk` those that went, `vitals_unsafe` those that went in
    // a purchase's money stretch or with anything up, `due_asked` how often
    // the cleaner asked.
    int  vitals_due = 0, vitals_drunk = 0, vitals_unsafe = 0, due_asked = 0;
    // The run's own potion presses (test_own_potions, 2026.11.11b): with
    // `own_on`, OwnPotions::due names `own_vk` while `vitals_due` is over 0;
    // each press of that key one drunk (`own_drunk`), the potion's time noted
    // as for one let through; `own_unsafe` those pressed in a money stretch,
    // with the box, a message or the board up, or the bag not clean.
    bool own_on = false;
    std::uint32_t own_vk = 'G';
    int  own_drunk = 0, own_unsafe = 0, own_asked = 0;
    BagCleaner::OwnPotions own_potions() {
        BagCleaner::OwnPotions o;
        o.due = [this](std::string& line) -> std::uint32_t {
            ++own_asked;
            if (vitals_due <= 0) return 0;
            line = "HP on G";
            return own_vk;
        };
        o.pressed = [](std::uint32_t) {};
        return o;
    }
    /// Openings with the shop up and nothing bought yet: the first purchase's.
    int  opens_first_buy = 0;
    bool money_now() const {
        return last_double > last_confirm || (last_confirm != std::chrono::steady_clock::time_point{} &&
                                              time.t - last_confirm < std::chrono::milliseconds(1000));
    }
    // One of the six no letter through the window (test_nav_keys, 2026.11.11):
    // with `nav_on_open` one goes down and up each time it opens, `on_nav`
    // then called (the list scrolled, say); KeyWindow::nav_ago_ms says so
    // unless `nav_hook` is off. Of the cleaner's own inputs after one: those
    // less than kNavSettleMs after it (`acts_soon_after_nav`), the least gap,
    // and the double-clicks, drags and right-clicks by a look taken less than
    // that after it (`decided_unsettled`).
    bool nav_on_open = false, nav_hook = true;
    std::function<void()> on_nav;
    int  nav_through = 0, acts_after_nav = 0, acts_soon_after_nav = 0, decided_unsettled = 0;
    long long nearest_after_nav_ms = -1;
    std::chrono::steady_clock::time_point nav_up_at{};
    static long long ms_of(std::chrono::steady_clock::duration d) {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
    }
    void own_act(bool by_look) {
        if (nav_through == 0) return;
        const long long gap = ms_of(time.t - nav_up_at);
        ++acts_after_nav;
        if (gap < BagCleaner::kNavSettleMs) ++acts_soon_after_nav;
        if (nearest_after_nav_ms < 0 || gap < nearest_after_nav_ms) nearest_after_nav_ms = gap;
        if (by_look && ms_of(look_at - nav_up_at) < BagCleaner::kNavSettleMs) ++decided_unsettled;
    }
    BagCleaner::KeyWindow key_window() {
        BagCleaner::KeyWindow w;
        w.open = [this] {
            ++keys_opens;
            if (!clean_now()) ++opens_unclean;
            const bool money = last_double > last_confirm ||
                               (last_confirm != std::chrono::steady_clock::time_point{} &&
                                time.t - last_confirm < std::chrono::milliseconds(1000));
            if (money) ++opens_in_money;
            if (shop_on && buys.empty()) ++opens_first_buy;
            opened_on.emplace_back(looks, buys.size());
            keys_open = true;
            press_potion();
            if (nav_on_open) {
                ++nav_through;
                nav_up_at = time.t;
                if (on_nav) on_nav();
            }
            return true;
        };
        if (nav_hook)
            w.nav_ago_ms = [this] { return nav_through > 0 ? static_cast<int>(ms_of(time.t - nav_up_at)) : -1; };
        w.shut = [this](int) {
            ++keys_shuts;
            keys_open = false;
            if (on_shut) on_shut();
            if (shut_refusals > 0) {
                --shut_refusals;
                return false;
            }
            return true;
        };
        w.potion_ago_ms = [this] {
            return potion_pressed
                       ? static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(time.t - potion_at).count())
                       : -1;
        };
        w.potion_due = [this] {
            ++due_asked;
            if (keys_open && vitals_due > 0) {
                --vitals_due;
                ++vitals_drunk;
                if (money_now() || !clean_now()) ++vitals_unsafe;
                potion_pressed = true;
                potion_at = time.t;
                time.t += std::chrono::milliseconds(60);
            }
            return vitals_due > 0;
        };
        return w;
    }

    /// Every this-many looks comes back black but for the corners Windows
    /// rounds, as the live client's did now and then (0: none); and from look
    /// `black_from` on every one does (-1: never).
    int black_every = 0;
    int black_from = -1;
    int looks = 0, black_looks = 0;
    Frame look() {
        look_at = time.t;
        look_open = keys_open;
        counts_at_look.push_back(bag::search_counts());
        if (keys_open && shop_on && !buys.empty()) ++looks_open_bought;
        if (keys_open) press_potion();
        time.t += std::chrono::milliseconds(40);
        ++looks;
        // A bag key's toggle drawn late, on the look it comes on.
        for (auto it = bag_flips_due.begin(); it != bag_flips_due.end();) {
            if (looks >= *it) {
                bag_shut = !bag_shut;
                it = bag_flips_due.erase(it);
            } else {
                ++it;
            }
        }
        // And an ESC's shut of it.
        for (auto it = esc_shuts_due.begin(); it != esc_shuts_due.end();) {
            if (looks >= *it) {
                bag_shut = true;
                it = esc_shuts_due.erase(it);
            } else {
                ++it;
            }
        }
        if (shop_due >= 0 && looks >= shop_due) {
            shop_on = true;
            shop_due = -1;
        }
        if (full_due && time.t >= full_at) {
            full_due = false;
            show_full();
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
        if (keys_open && !clean_now()) ++looks_open_unclean;
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
        if (row_hidden_looks > 0) --row_hidden_looks;
        if (!bag_shut && ((unread && unread(looks)) || (hover_unreads && v.strip.contains(px, py)))) {
            blit(f, strip_on(0), v.strip.x, v.strip.y);
            ++unread_seen;
        }
        // A message for one look only, and then the quantity box.
        if (lone_up && lone_flash && lone_looks > 0 && --lone_looks == 0) {
            lone_up = false;
            open_box(lone_focused);
        }
        return f;
    }

    BagCleaner::Hands hands() {
        BagCleaner::Hands h;
        h.right_click = [this](int x, int y) {
            ++right_clicks;
            if (keys_open) ++acts_with_keys;
            own_act(true);
            if (look_open) ++rights_from_open;
            if (potion_pressed && look_at - potion_at < std::chrono::milliseconds(500)) ++rights_soon;
            if (msg_on) ++right_clicks_with_message;
            px = x;
            py = y;
            right_at.emplace_back(x, y);
            book_at_right_click.push_back(book);
            events += 'R';
            seq += 'R';
            if (board_on) ++acts_with_board;
            if (board_on && board_whole) return true;       // the board in front takes it
            // A message in front takes it.
            if (full_up || lone_up) return true;
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
            if (keys_open) ++acts_with_keys;
            own_act(false);
            px = x;
            py = y;
            // The game's menu in front takes every click; its X shuts it.
            if (menu_up) {
                if (menu_x_rect().contains(x, y)) {
                    ++menu_x_clicks;
                    if (menu_x_misses > 0) --menu_x_misses;
                    else menu_up = false;
                } else {
                    ++menu_other_clicks;
                }
                return true;
            }
            if (bag_shut && v.strip.contains(x, y)) ++clicks_on_shut_bag;
            if (board_on) ++acts_with_board;
            if (board_on && board_whole) return true;       // the board in front takes it
            // The shop's messages in front take every click: their Xác Nhận
            // closes them -- the full-bag one's not with `full_sticks`, nor the
            // other's with `lone_sticks`.
            if (full_up) {
                if (full_ok_rect().contains(x, y)) {
                    ++full_clicks;
                    full_click_at.emplace_back(x, y);
                    if (!full_sticks) full_up = false;
                }
                return true;
            }
            if (lone_up) {
                if (lone_button().contains(x, y)) {
                    ++lone_clicks;
                    if (!lone_sticks) lone_up = false;
                }
                return true;
            }
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
            // A tab is clicked only on a bag drawn: shut, the click goes into
            // the game world (2026-11-14, 20:31).
            if (tabs_work && !bag_shut) {
                for (int t = 0; t < kBagTabs; ++t) {
                    int tx = 0, ty = 0;
                    v.tab_point(l, t, tx, ty);
                    if (std::abs(x - tx) <= l.tab_w / 2 && std::abs(y - ty) <= 8) {
                        ++tab_clicks;
                        if (t == kUseTab) ++use_tab_clicks;
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
            if ((up == Up::InBin || up == Up::BinAsking) && near_bin.contains(x, y)) ++bin_clicks;
            if (up == Up::InBin && near_bin.contains(x, y)) {
                up = Up::BinAsking;
                return true;
            }
            if (up == Up::BinAsking) {
                // Hidden under the bag, it takes no click there.
                if (bin_ask_hidden_while_bag && !bag_shut) return true;
                const ScreenRect p = bin_pair();
                if (!p.contains(x, y)) return true;
                if (x < p.x + p.w / 2) {
                    ++bin_confirms;
                    items[static_cast<std::size_t>(about)].count = 0;
                    events += 'X';
                    seq += 'X';
                    left(static_cast<std::size_t>(about));
                } else {
                    ++bin_cancels;
                    if (bin_sticky) return true;
                }
                up = Up::None;
            }
            return true;
        };
        h.drag = [this](int fx, int fy, int tx, int ty) {
            ++drags;
            if (keys_open) ++acts_with_keys;
            own_act(true);
            // The look it was judged on: the last before it.
            if (look_open) ++drags_from_open;
            if (potion_pressed && look_at - potion_at < std::chrono::milliseconds(500)) ++drags_soon;
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
            if (keys_open) ++acts_with_keys;
            if (look_open) ++doubles_from_open;
            own_act(true);
            last_double = time.t;
            if (board_on || lone_up || full_up) ++doubles_under;
            if (board_on) ++acts_with_board;
            if ((board_on && board_whole) || full_up || lone_up) return true;      // what is in front takes it
            // A cell of Linh Thú (2026.11.24): the pet's food (3) or another
            // item (4); with no room left for it, the game's message instead.
            if (shop_on && !qty_up && on_pet_tab())
                for (const PetCell& c : pet_cells) {
                    if (!pet_icon_rect(c).contains(x, y)) continue;
                    const bool food = c.name == CleanSettings::kBuyPetName;
                    shop_sel = food ? 3 : 4;
                    if (food && pet_room == 0) {
                        lone_up = true;
                        return true;
                    }
                    open_question(!double_unfocused);
                    return true;
                }
            const int cell = shop_on && !qty_up ? cell_at(x, y) : -1;
            if (cell < 0) return one(x, y) && one(x, y);
            // The list scrolled: another item lies where the potion was.
            if (row_off()) ++wrong_item_doubles;
            shop_sel = row_off() ? 2 : cell;
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
                move_at.emplace_back(x, y);
                return true;
            };
        return h;
    }
    /// Where each Hands::move put the pointer, in order.
    std::vector<std::pair<int, int>> move_at;
    /// A key into the pretend game, as its key code or as its scan code alone.
    bool key_in(std::uint32_t vk, bool as_scan) {
        ++keys;
        if (keys_open) ++acts_with_keys;
        own_act(false);
        // The run's own potion (2026.11.11b): drunk with nothing in front.
        if (own_on && vk == own_vk) {
            if (money_now() || qty_up || full_up || lone_up || board_on || msg_on || !clean_now()) {
                ++own_unsafe;
            } else if (vitals_due > 0) {
                --vitals_due;
                ++own_drunk;
                potion_pressed = true;
                potion_at = time.t;
            }
            return true;
        }
        // The game's menu in front takes every key; ESC shuts it.
        if (menu_up) {
            if (vk == VK_ESCAPE) {
                ++esc_presses;
                ++esc_on_menu;
                menu_up = false;
            }
            return true;
        }
        if (msg_on) ++keys_with_message;
        if (board_on) ++acts_with_board;
        // The whole board in front takes every key: ESC closes it, and the
        // box, a message and the shop under it get none.
        if (board_on && board_whole) {
            if (vk == VK_ESCAPE) {
                ++esc_presses;
                board_on = false;
            }
            return true;
        }
        // A message in front takes the key; ESC closes it, and nothing else.
        if (full_up || lone_up) {
            if (full_up) ++keys_with_full;
            if (vk == VK_ESCAPE) {
                ++esc_presses;
                if (full_up) ++esc_with_full;
                if (lone_up) ++esc_with_lone;
                full_up = lone_up = false;
            }
            return true;
        }
        // The check-in board in front: an ESC closes it, and not the shop.
        if (board_on && vk == VK_ESCAPE && !qty_up) {
            ++esc_presses;
            board_on = false;
            return true;
        }
        if (bag_keyed && !qty_up && vk != VK_ESCAPE) {
            ++bag_keys;
            if (bag_keys_taken >= 0 && bag_keys > bag_keys_taken) return true;
            if (bag_lag_looks > 0) bag_flips_due.push_back(looks + bag_lag_looks);
            else bag_shut = !bag_shut;
            return true;
        }
        if (qty_up) {
            // ESC on the quantity question: its Hủy Bỏ, nothing bought.
            if (vk == VK_ESCAPE) {
                ++esc_presses;
                if (esc_closes_qty) {
                    ++esc_on_qty;
                    qty_up = false;
                    typed = "1";
                }
                return true;
            }
            if (vk >= '0' && vk <= '9') {
                ++digit_keys;
                if (box_needs_click && !box_focused) {
                    cancel_pressed = true;
                } else if (!keys_dead && !(scan_only && !as_scan) &&
                           !(key_drops > 0 && digit_keys % key_drops == 0) && typed.size() < 6) {
                    if (caret < 0) typed += static_cast<char>(vk);
                    else typed.insert(static_cast<std::size_t>(caret++), 1, static_cast<char>(vk));
                }
            } else if (vk == VK_BACK) {
                ++backspaces;
                if (caret < 0 && !typed.empty()) typed.pop_back();
                else if (caret > 0) typed.erase(static_cast<std::size_t>(--caret), 1);
            }
            return true;
        }
        if (vk == VK_ESCAPE) {
            ++esc_presses;
            // The window in front: the shop, then the bin's question, then
            // the bag; nothing up at all is counted.
            if (shop_on) {
                if (esc_closes_shop) {
                    shop_on = false;
                    shop_sel = -1;
                    if (!shop_keeps_tab) shop_tab = bag::kShopTabPotions;    // it opens on Dược Phẩm again
                }
                return true;
            }
            if (up == Up::BinAsking && esc_on_bin != EscBin::Ignores) {
                ++esc_on_bin_q;
                up = esc_on_bin == EscBin::GivesBack ? Up::None : Up::InBin;
                return true;
            }
            // The bag -- not one an ESC is shutting already, drawn late.
            if (!bag_shut && esc_shuts_due.empty()) {
                if (esc_closes_bag) {
                    ++esc_on_bag;
                    if (esc_bag_lag_looks > 0) esc_shuts_due.push_back(looks + esc_bag_lag_looks);
                    else bag_shut = true;
                }
                return true;
            }
            if (up == Up::None || up == Up::InBin) {
                ++esc_on_nothing;
                show_menu();
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
    std::function<bool()> hold_check;               ///< the hold alone, HP not asked (set_hold_check); unset: not set
    std::function<void(const std::string&)> claim;  ///< called with a charm's id before it is right-clicked
    int deadline_ms = 0;
    ScreenRect own_notice;                          ///< handed to set_own_notice when set
    bool own_notice_out = false;                    ///< ... clicked to no end by the last run (2026.11.24 review)
    int shop_hp = 0, shop_mp = 0;                   ///< the shop's plan (set_shop_plan)
    int shop_pet = 0;                               ///< and the pet's food in it (2026.11.24)
    bool left_held = false;                         ///< handed to set_left_held
    bool key_window = false;                        ///< the pretend game's key window handed over (set_key_window)
    bool own = false;                               ///< the pretend game's own potions handed over (set_own_potions)
    /// The second of the day the check-in board's night hold goes by
    /// (set_day_clock, 2026.11.15d); unset: none (-1), so that no test depends
    /// on the hour it runs at.
    std::function<int()> day_clock;
    /// How long the game is given to react (set_react_ms); 0: kReactMs.
    int react_ms = 0;
    /// The run's give-way for the macro's HP potion (set_yield, 2026.11.21);
    /// unset: not asked.
    std::function<bool()> yield;
    /// The kept hold's sweep (sweep_kept, 2026.11.21) instead of a run, and
    /// whether it left the screen clear.
    bool sweep = false;
    bool swept_clear = false;

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
        if (hold_check) cleaner.set_hold_check(hold_check);
        if (claim) cleaner.set_charm_claim(claim);
        if (packets) cleaner.set_charm_packets(packets);
        if (deadline_ms > 0) cleaner.set_deadline_ms(deadline_ms);
        if (own_notice.valid()) cleaner.set_own_notice(own_notice, own_notice_out);
        cleaner.set_shop_plan(shop_hp, shop_mp, shop_pet);
        cleaner.set_left_held(left_held);
        if (key_window) cleaner.set_key_window(game.key_window());
        if (own) {
            game.own_on = true;
            cleaner.set_own_potions(game.own_potions());
        }
        if (!debug_dir.empty()) cleaner.set_debug_dir(debug_dir);
        cleaner.set_day_clock(day_clock ? day_clock : std::function<int()>([] { return -1; }));
        if (react_ms > 0) cleaner.set_react_ms(react_ms);
        if (yield) cleaner.set_yield(yield);
        cleaner.on_report([this](const std::string& line) { said.push_back(line); });
        if (sweep) {
            result = BagCleaner::Result{};
            swept_clear = cleaner.sweep_kept(result, "a kept hold");
        } else {
            result = cleaner.run();
        }
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
    {
        // The close buttons searched one after another on a PC of two CPUs or
        // fewer, each on a thread of its own otherwise (2026.11.16): the same
        // answer both ways, down to which X is said first.
        bag::force_searches_inline(-1);
        check(bag::searches_inline() == (std::thread::hardware_concurrency() <= 2),
              "the searches inline on two logical CPUs or fewer, on threads above (this PC: " +
                  std::to_string(std::thread::hardware_concurrency()) + ")");
        Frame f = scene(1024, 768, 60);
        const int xs[] = { 100, 420, 760 }, ys[] = { 120, 330, 560 };
        int drawn = 0;
        for (const char* name : bag::kCloseButtons)
            if (drawn < 3) {
                blit(f, *k.store.get(name), xs[drawn], ys[drawn]);
                ++drawn;
            }
        const auto both_ways = [&](const Frame& on, const ScreenRect* also_own, const std::string& what) {
            ScreenRect at_inline{}, at_threads{};
            bag::force_searches_inline(1);
            const bool in = bag::other_window_up(on, k.store, BagView{}, BagLayout{}, &at_inline, also_own);
            bag::force_searches_inline(0);
            const bool th = bag::other_window_up(on, k.store, BagView{}, BagLayout{}, &at_threads, also_own);
            bag::force_searches_inline(-1);
            check(in == th && at_inline.x == at_threads.x && at_inline.y == at_threads.y &&
                      at_inline.w == at_threads.w && at_inline.h == at_threads.h,
                  what + ": inline " + (in ? "another window at " + std::to_string(at_inline.x) + "," +
                                                 std::to_string(at_inline.y)
                                           : std::string("none")) +
                      ", on threads " +
                      (th ? "another window at " + std::to_string(at_threads.x) + "," + std::to_string(at_threads.y)
                          : std::string("none")));
            return std::make_pair(in, at_inline);
        };
        const auto first = both_ways(f, nullptr, "three close buttons, none the bag's");
        check(first.first, "and one of them is taken for another window's");
        if (first.first) both_ways(f, &first.second, "the first of them the caller's own");
        // Without a single close button on it: no other window either way.
        const auto none = both_ways(scene(1024, 768, 60), nullptr, "no close button at all");
        check(!none.first, "and none is taken for another window's");
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
        //    then, since 2026.11.24 (the owner: "Kiểm tra các phần khác xem có
        //    phần nào không làm được thì dừng không, cần thay cơ chế này bằng 1
        //    cơ chế khác"), handed to the next run, which clicks it again: no
        //    longer left for the player, with no charm used while it stays up.
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
                  std::to_string(c.p.game.notice_ok_clicks) + " click(s), never a third in the run");
        const std::string again =
            "thông báo của game chưa đóng sau 2 lần bấm Xác Nhận — GoBot bấm lại ở lần dùng bùa sau (khoảng 1 phút)";
        check(c.r.charms_used == 1 && c.p.game.right_clicks == 1 && c.r.trouble.find(again) != std::string::npos &&
                  c.r.trouble.find("đóng nó trong game") == std::string::npos && c.p.game.keys == 0 &&
                  c.p.game.keys_with_message == 0 && c.r.notice_left && c.r.notice_clicked && !c.r.notice_carried &&
                  c.r.notice_place.valid() && kept_as(c.p.debug_dir, "charm_notice_stuck"),
              "used, as the stack says, the second kind not right-clicked under it, no key pressed -- and the "
              "message handed to the next run, clicked by this one: " + c.r.trouble);
        // The next run, a minute on (GuiApp's kCharmNoticeAgainMs), the message
        // still up: its own Xác Nhận clicked twice again, at the place handed
        // over, kNoticeRetryMs apart -- no key, no right-click, the bag left as
        // it is -- and handed on, clicked again: unlimited.
        const ScreenRect place = c.r.notice_place;
        const std::size_t picture_files = files_in(c.p.debug_dir, "*_charm_notice_stuck.png").size();
        for (int run = 2; run <= 3; ++run) {
            c.p.due = [](const std::string&) { return false; };
            c.p.own_notice = c.r.notice_place;
            const int clicks = c.p.game.notice_ok_clicks, rights = c.p.game.right_clicks;
            const std::size_t from = c.p.game.notice_click_t.size();
            c.run();
            const auto& ts = c.p.game.notice_click_t;
            const long long gap =
                ts.size() == from + 2 ? std::chrono::duration_cast<std::chrono::milliseconds>(ts[from + 1] - ts[from]).count()
                                      : -1;
            check(c.p.game.notice_ok_clicks == clicks + 2 && gap >= 300 && c.p.game.right_clicks == rights &&
                      c.p.game.keys == 0 && c.p.game.keys_with_message == 0 && c.p.game.notice_other_clicks == 0 &&
                      c.p.game.wrong_confirms == 0 && c.r.notice_left && c.r.notice_carried && c.r.notice_clicked &&
                      c.r.notice_place.x == place.x && c.r.notice_place.y == place.y &&
                      c.r.trouble.find(again) != std::string::npos && c.r.charm_tries.empty(),
                  "run " + std::to_string(run) + ", the message still up: clicked twice again (" +
                      std::to_string(gap) + " ms apart) where it was handed over, nothing else pressed, and handed "
                      "on, clicked -- " + c.r.trouble);
        }
        // The 2026.11.24 review: handed over as clicked to no end by the last
        // run (GuiApp's charm_notice_clicked) -- one quick click, watched
        // kNoticeCloseMs x 2, not the 15 + 30 s again every minute.
        for (int run = 4; run <= 5; ++run) {
            c.p.due = [](const std::string&) { return false; };
            c.p.own_notice = c.r.notice_place;
            c.p.own_notice_out = true;
            const int clicks = c.p.game.notice_ok_clicks, rights = c.p.game.right_clicks;
            c.run();
            check(c.p.game.notice_ok_clicks == clicks + 1 && c.p.game.right_clicks == rights && c.p.game.keys == 0 &&
                      c.r.notice_left && c.r.notice_carried && c.r.notice_clicked &&
                      c.r.notice_place.x == place.x && c.r.notice_place.y == place.y && c.r.charm_tries.empty(),
                  "run " + std::to_string(run) + ", handed over clicked to no end: one quick click, handed on -- " +
                      c.r.trouble);
        }
        c.p.own_notice_out = false;
        check(files_in(c.p.debug_dir, "*_charm_notice_stuck.png").size() <= picture_files + 1,
              "its picture kept once for the runs handed it, not once a run -- " +
                  std::to_string(files_in(c.p.debug_dir, "*_charm_notice_stuck.png").size()) + " in all");
        // The game lets it go at last: the next run closes it with one click,
        // and hands nothing on.
        c.p.game.notice_sticks = false;
        c.p.own_notice = c.r.notice_place;
        const int before = c.p.game.notice_ok_clicks;
        c.run();
        check(c.p.game.notice_ok_clicks == before + 1 && !c.p.game.msg_on && c.p.game.messages_closed == 1 &&
                  !c.r.notice_left && !c.r.notice_clicked && c.r.charm_notices == 1 && c.r.trouble.empty() &&
                  c.p.game.keys == 0,
              "then the message gives way: the next run closes it with one click, nothing handed on -- " +
                  c.r.trouble);
    }
    {
        // 4. The same with hands that cannot move the pointer: the button it
        //    clicked stays lit, so its label is not to be told -- no second
        //    click. Why the pointer is moved off. Handed to the next run all
        //    the same (2026.11.24), said with its one click.
        NoticeRun c;
        c.p.game.notice_sticks = true;
        c.p.game.no_move = true;
        c.run();
        check(c.p.game.notice_ok_clicks == 1 && c.r.charms_used == 1 && !c.r.trouble.empty() && c.p.game.keys == 0 &&
                  c.r.notice_left && c.r.notice_clicked &&
                  c.r.trouble.find("chưa đóng sau 1 lần bấm Xác Nhận") != std::string::npos,
              "with the pointer left on the message's button: one click only, its lit label not clicked again, "
              "handed to the next run -- " + std::to_string(c.p.game.notice_ok_clicks) + " click(s), " + c.r.trouble);
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
        // HP low (2026.11.12b, the owner: "máu thấp thì không dùng bùa nữa,
        // cái này cũng cần sửa"): the HP potion due as the run begins and
        // again after each Xác Nhận -- the run presses it itself between two
        // charms, never with the question or the message up, no key window
        // opened, and both kinds are used all the same.
        CharmBagRun c(catalog, { { 1, kPurpleCard, 62 }, { 11, kBlueDiamond, 1 } }, { kPurpleCard, kBlueDiamond });
        c.p.own = true;
        c.p.game.vitals_due = 1;
        int confirms_seen = 0;
        c.p.during = [&](BagCleaner&) {
            if (c.p.game.confirms > confirms_seen) {
                confirms_seen = c.p.game.confirms;
                c.p.game.vitals_due = 1;
            }
        };
        c.run();
        check(c.p.game.right_clicks == 2 && c.r.charms_used == 2 && c.tries() == "UU" && c.r.trouble.empty() &&
                  c.p.game.messages_closed == 2 && c.p.game.right_clicks_with_message == 0,
              "HP low: both kinds used in one run, each message closed -- tries " + c.tries() + ", " + c.r.trouble);
        check(c.p.game.own_drunk >= 2 && c.p.game.own_unsafe == 0 && c.p.game.rights_soon == 0,
              "the HP potion pressed by the run itself between the charms (" + std::to_string(c.p.game.own_drunk) +
                  "), never with the question or the message up, no right-click from a look under 500 ms after it");
    }
    {
        // A right-click that brought nothing up (the review of 2026.11.12b):
        // the potions of the rest of the run are no longer held by it -- the
        // next round's look, clean, drinks before the next kind is used.
        CharmBagRun c(catalog, { { 1, kPurpleCard, 62 }, { 11, kBlueDiamond, 1 } }, { kPurpleCard, kBlueDiamond });
        c.p.game.reply = Pretend::Reply::Nothing;
        c.p.own = true;
        bool switched = false;
        c.p.during = [&](BagCleaner&) {
            if (!switched && c.p.game.right_clicks >= 1) {
                switched = true;
                c.p.game.reply = Pretend::Reply::Charm;
                c.p.game.vitals_due = 1;
            }
        };
        c.run();
        check(c.p.game.right_clicks == 2 && c.tries() == "NU" && c.p.game.own_drunk == 1 &&
                  c.p.game.own_unsafe == 0 && c.p.game.rights_soon == 0,
              "nothing came of the first right-click: the HP potion still pressed before the second kind, which is "
              "used -- tries " + c.tries() + ", " + std::to_string(c.p.game.own_drunk) + " drunk, " + c.r.trouble);
    }
    {
        // Nothing due: asked, nothing pressed, the run as before.
        CharmBagRun c(catalog, { { 1, kPurpleCard, 62 } }, { kPurpleCard });
        c.p.own = true;
        c.run();
        check(c.r.charms_used == 1 && c.p.game.own_drunk == 0 && c.p.game.own_unsafe == 0 &&
                  c.p.game.own_asked >= 1 && c.p.game.keys == 0,
              "no potion due: asked, no key pressed -- " + std::to_string(c.p.game.own_asked) + " asked");
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
                              "BagCleaner::Probe BagCleaner::probe(", "bool BagCleaner::shop_open(",
                              "int BagCleaner::shop_purchase(", "void BagCleaner::shop_buy(" }) {
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

/// What one shop run comes to, on a pretend bag -- the bag open on Tiêu Dùng,
/// and with `opener` the Người Giữ Sân item in Khác -- planned to buy `hp_buy`
/// of Thịt bò khô and `mp_buy` of Rượu lá tre, with room in the bag for `room`
/// purchases (-1: no end). Nothing in the bag is counted (2026.11.09).
struct ShopRun {
    Pass p;
    ItemCatalog catalog;
    BagCleaner::Result r;
    bool ok = false;
    int hp_buy = 0, mp_buy = 0;
    explicit ShopRun(int hp, int mp, bool opener = true, int room = -1) : hp_buy(hp), mp_buy(mp) {
        const std::string hp_id = CleanSettings::kBuyHpId, mp_id = CleanSettings::kBuyMpId;
        const std::string door = p.settings.buy_opener;
        if (!kit().shop_ok || !catalog.load(ItemCatalog::default_dir()) || catalog.find(hp_id) == nullptr ||
            catalog.find(mp_id) == nullptr || catalog.find(door) == nullptr)
            return;
        make_bag(p.game);
        p.game.book_opens = false;
        p.game.tabs_work = true;
        p.game.room = room;
        if (opener) {
            Pretend::Item o{ 0, 1, 1 };
            o.tab = kOtherTab;
            o.opener = true;
            o.icon = catalog.find(door)->tile;
            p.game.items.push_back(o);
        }
        p.list.add_catalog(catalog, { hp_id, mp_id, door }, TrashList::Action::Delete, p.settings.catalog_score);
        p.list.add_rivals(catalog);
        p.mode = BagCleaner::Mode::ShopBuy;
        p.settings.buy_potions = true;
        p.shop_hp = hp;
        p.shop_mp = mp;
        ok = true;
    }
    BagCleaner::Result run(bool dry = false) {
        r = p.run(dry);
        return r;
    }
    /// Purchases of each potion the pretend shop sold.
    int bought(bool hp) const {
        return static_cast<int>(std::count(p.game.buys.begin(), p.game.buys.end(), hp ? 0 : 1));
    }
    /// The pet's food on Linh Thú (2026.11.24): `n` purchases of it in the
    /// plan, the shop's tabs drawn and `cells` on Linh Thú -- the pet's Thịtbò
    /// in the right column's first row when none are given -- and the shop
    /// moved down, so that its strip of tabs is on the frame.
    void pet(int n, std::vector<Pretend::PetCell> cells = {}) {
        p.game.pet_tabs = true;
        p.game.kSY = 400;
        p.game.pet_cells = cells.empty() ? std::vector<Pretend::PetCell>{ Pretend::PetCell{} } : std::move(cells);
        p.settings.buy_pet = true;
        p.shop_pet = n;
    }
    /// Purchases of the pet's food the pretend shop sold, and of another item
    /// of Linh Thú.
    int pets() const { return static_cast<int>(std::count(p.game.buys.begin(), p.game.buys.end(), 3)); }
    int other_pet_items() const { return static_cast<int>(std::count(p.game.buys.begin(), p.game.buys.end(), 4)); }
    /// The pointer moved, after a double-click and after Xác Nhận, onto the
    /// shop's bare panel left of Mua: how many times.
    int moves_off_list() const {
        const ScreenRect b = p.game.shop_buttons_rect();
        return static_cast<int>(std::count(p.game.move_at.begin(), p.game.move_at.end(),
                                           std::make_pair(b.x - 30, b.y + bag::kShopButtonY)));
    }
};

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

void test_shop() {
    section("Mua bình: the slots set, bought 100 at a time with the number read back, nothing counted, and the "
            "game's full-bag message the end of a round");
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

    // ---- what is bought, in which order, and nothing counted
    {
        ShopRun s(5, 0);
        s.run();
        check(s.r.ran && s.r.trouble.empty() && s.r.shop_bought == 5 && s.r.shop_bought_hp == 5 &&
                  s.r.shop_bought_mp == 0 && s.p.game.buys == std::vector<int>(5, 0) && s.p.game.wrong_confirms == 0,
              "five slots of Thịt bò khô set: five bought, Rượu lá tre not touched -- " + s.r.trouble + " / " +
                  s.p.last());
        check(s.p.game.digit_keys == 10 && s.p.game.cancels == 0 && s.p.game.typed == "100",
              "each time 00 typed after the game's 1 in the box, and the box read as 100 before Xác Nhận (" +
                  std::to_string(s.p.game.digit_keys) + " digit keys)");
        check(!s.p.game.shop_on && s.p.game.esc_presses == 1 && s.p.game.thoat_clicks == 0 &&
                  s.p.game.opener_clicks == 1,
              "the shop opened once and shut once at the end, with one ESC, seen to work");
        check(s.p.game.use_tab_clicks == 0 && s.p.game.scroll_clicks == 0 && !spoken(s, "ngăn Tiêu Dùng có"),
              "nothing counted: Tiêu Dùng never clicked, the bag never scrolled, no count said");
        check(s.moves_off_list() == 10,
              "the pointer moved off the list after every double-click and off the question after every Xác Nhận, "
              "never clicked there (" + std::to_string(s.moves_off_list()) + " moves)");
    }
    {
        // Turns, HP first, then the rest of the longer list.
        ShopRun s(3, 1);
        s.run();
        check(s.r.trouble.empty() && s.p.game.buys == std::vector<int>({ 0, 1, 0, 0 }) && s.r.shop_bought_hp == 3 &&
                  s.r.shop_bought_mp == 1,
              "3 + 1: Thịt bò khô, Rượu lá tre, then the two Thịt bò khô left -- " + s.r.trouble);
        ShopRun t(1, 3);
        t.run();
        check(t.r.trouble.empty() && t.p.game.buys == std::vector<int>({ 0, 1, 1, 1 }),
              "1 + 3: HP, MP, then MP twice more -- " + t.r.trouble);
    }
    {
        // A potion set to 0 is never bought; both 0 press nothing at all.
        ShopRun s(0, 2);
        s.run();
        check(s.r.trouble.empty() && s.p.game.buys == std::vector<int>(2, 1) && s.bought(true) == 0,
              "HP set to 0: only the two Rượu lá tre -- " + s.r.trouble);
        ShopRun none(0, 0);
        none.p.game.bag_shut = none.p.game.bag_keyed = true;
        none.run();
        check(!none.r.ran && none.r.trouble.empty() && none.p.game.keys == 0 && none.p.game.clicks == 0 &&
                  none.p.game.right_clicks == 0 && none.p.game.double_clicks == 0 && none.p.game.bag_shut &&
                  spoken(none, "cả hai số ô đều là 0"),
              "both 0: nothing pressed, not even the bag key, and said so");
    }
    {
        // The plan cut to kShopMaxBuys, taking turns.
        const auto order = bag::shop_order(25, 25, BagCleaner::kShopMaxBuys);
        const auto hold = bag::shop_order(7, 7, BagCleaner::kShopBuysPerHold);
        check(order.size() == 40 && order.front() == bag::ShopPotion::Hp && order[1] == bag::ShopPotion::Mp &&
                  std::count(order.begin(), order.end(), bag::ShopPotion::Hp) == 20 && hold.size() == 10 &&
                  std::count(hold.begin(), hold.end(), bag::ShopPotion::Mp) == 5,
              "the order: 25 + 25 cut to 40, 20 of each in turns; a timed part's 10, five of each");
        check(BagCleaner::shop_deadline_ms(1) == 36'000 && BagCleaner::shop_deadline_ms(10) == 90'000 &&
                  BagCleaner::shop_deadline_ms(40) == 270'000 && BagCleaner::shop_deadline_ms(50) == 300'000 &&
                  BagCleaner::shop_deadline_ms(0) == 30'000,
              "the deadline: 30 s and 6 s a purchase, 300 s at most");
        ShopRun s(25, 25);
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 40 && s.bought(true) == 20 && s.bought(false) == 20,
              "a plan of 25 + 25: forty bought in one round, 20 of each, and no more -- " +
                  std::to_string(s.r.shop_bought) + " -- " + s.r.trouble);
        ShopRun late(10, 10);
        late.p.deadline_ms = 30'000;
        late.run();
        check(late.r.shop_bought < 20 && late.r.trouble.find("lâu quá thời gian cho phép") != std::string::npos &&
                  !late.p.game.shop_on,
              "past its deadline a run stops, nothing more bought, the shop shut -- " +
                  std::to_string(late.r.shop_bought) + " bought -- " + late.r.trouble);
    }

    // ---- the game's full-bag message
    {
        // Instead of the box at the first purchase, as GoC has it.
        ShopRun s(3, 3, true, 0);
        s.run();
        const ScreenRect ok = Pretend::full_ok_rect();
        check(s.r.shop_full && s.r.shop_bought == 0 && s.p.game.buys.empty() && s.p.game.full_clicks == 1 &&
                  !s.p.game.full_up && s.r.trouble.empty() && !s.r.full_stuck,
              "full at the first purchase: the round ends there, its message closed with one click -- " + s.r.trouble);
        check(s.p.game.digits_at_full == 0 && s.p.game.digit_keys == 0 && s.p.game.esc_with_full == 0 &&
                  s.p.game.keys_with_full == 0 && s.p.game.double_clicks == 1 && !s.p.game.shop_on &&
                  s.p.game.esc_presses == 1,
              "no digit and no ESC while it was up, MP never tried, the shop shut after with its one ESC");
        // Its label where lone_ok finds it, and the click in its middle.
        Frame f = scene(1024, 768, 60);
        blit(f, *kit().store.get(bag::kShopFull), Pretend::kFX, Pretend::kFY);
        ScreenRect label;
        const bool found = bag::lone_ok(f, kit().store,
                                        ScreenRect{ Pretend::kFX + bag::kShopFullOkBand.x,
                                                    Pretend::kFY + bag::kShopFullOkBand.y, bag::kShopFullOkBand.w,
                                                    bag::kShopFullOkBand.h },
                                        &label);
        check(found && label.x == Pretend::kFX + 148 && label.y == Pretend::kFY + 58 &&
                  s.p.game.full_click_at.size() == 1 && s.p.game.full_click_at[0].first == label.cx() &&
                  s.p.game.full_click_at[0].second == label.cy() && ok.contains(label.cx(), label.cy()),
              "its Xác Nhận clicked in the middle of its label, found at 148,58 of the message (" +
                  std::to_string(label.x - Pretend::kFX) + "," + std::to_string(label.y - Pretend::kFY) + ")");
    }
    for (const int k : { 1, 3 }) {
        ShopRun s(3, 3, true, k);
        s.run();
        check(s.r.shop_full && s.r.shop_bought == k && static_cast<int>(s.p.game.buys.size()) == k &&
                  s.p.game.full_clicks == 1 && s.p.game.digit_keys == 2 * k && s.p.game.esc_with_full == 0 &&
                  !s.p.game.shop_on && s.r.trouble.empty(),
              "room for " + std::to_string(k) + ": " + std::to_string(k) +
                  " bought, the next brings the message instead of its box -- closed, nothing typed after it, no ESC "
                  "while it was up, the shop shut -- " + s.r.trouble);
    }
    for (const int late : { 0, 150, 400, 900 }) {
        // After Xác Nhận on the box, that late: that purchase is not made, and
        // not counted -- the looks after the question has gone go on until a
        // second after the click -- and no double-click goes under it.
        ShopRun s(3, 3, true, 2);
        s.p.game.full_late_ms = late;
        s.run();
        const int counted = 2;
        check(s.r.shop_full && s.r.shop_bought == counted && s.p.game.buys.size() == 2 &&
                  s.p.game.full_clicks == 1 && s.p.game.esc_with_full == 0 && s.p.game.keys_with_full == 0 &&
                  s.p.game.digits_at_full == 6 && s.p.game.digit_keys == 6 && s.p.game.double_clicks == 3 &&
                  !s.p.game.shop_on,
              "the message " + std::to_string(late) + " ms after Xác Nhận on the third: seen, closed, no ESC or key "
              "while it was up, the shop shut; " + std::to_string(s.r.shop_bought) + " counted (" +
                  std::to_string(counted) + " wanted) -- " + s.r.trouble);
    }
    {
        // A-1: its label not to be told -- lit, as with the pointer over it:
        // the picture's own place for Xác Nhận is clicked, and the message
        // counts as closed only once two looks do not show it.
        ShopRun s(1, 1, true, 0);
        s.p.game.full_lit = true;
        s.run();
        const ScreenRect ok = Pretend::full_ok_rect();
        check(s.r.shop_full && s.p.game.full_clicks == 1 && !s.p.game.full_up && s.p.game.full_click_at.size() == 1 &&
                  s.p.game.full_click_at[0].first == Pretend::kFX + bag::kShopFullOkX &&
                  s.p.game.full_click_at[0].second == Pretend::kFY + bag::kShopFullOkY &&
                  ok.contains(s.p.game.full_click_at[0].first, s.p.game.full_click_at[0].second) &&
                  s.r.trouble.empty(),
              "its label lit: the fixed point clicked, and closed -- " + s.r.trouble);
        // A message that will not close: two clicks, and no ESC then; the
        // stuck rule's 30 s since the last click (2026.11.14b, 10 s from the
        // double-click until 2026.11.15d: kStuckMs, react_ms()), ESC on it,
        // the shop and the bag, each on a look that shows it -- before, the
        // shop was left open behind it, holding the keys.
        ShopRun t(1, 1, true, 0);
        t.p.game.full_sticks = true;
        t.run();
        check(t.r.shop_full && t.r.full_stuck && t.p.game.full_clicks == 2 && !t.p.game.full_up &&
                  t.p.game.esc_with_full == 1 && t.p.game.keys_with_full == 1 && t.p.game.esc_presses == 3 &&
                  !t.p.game.shop_on && !t.r.stuck && t.r.esc_swept &&
                  t.r.trouble.find("thông báo túi đầy không tắt được") != std::string::npos &&
                  t.r.trouble.find("kẹt quá 30 giây (mua bình)") != std::string::npos,
              "a message that ignores clicks: exactly two, then after the stuck rule's 30 s ESC on it, the shop and "
              "the bag -- " + t.r.trouble);
        // Gone on one look after the click, back on the next: not closed by
        // that look -- clicked once more, and closed only once two looks do
        // not show it.
        ShopRun u(1, 1, true, 0);
        int looks_after = 0;
        u.p.during = [&](BagCleaner&) {
            if (u.p.game.full_clicks == 1 && !u.p.game.full_up && ++looks_after == 2) u.p.game.full_up = true;
        };
        u.run();
        check(u.r.shop_full && u.p.game.full_clicks == 2 && !u.p.game.full_up && !u.r.full_stuck &&
                  u.p.game.esc_with_full == 0,
              "a message gone on one look and back on the next is clicked again, and closed only once two looks "
              "do not show it (" + std::to_string(u.p.game.full_clicks) + " clicks)");
        // Slow to go after the first click -- gone only at the end of the
        // wait for it, or after -- whenever it goes: seen gone on two looks,
        // closed, never said not closed, and never clicked while gone.
        // Gone by 2400 ms, before the look of step 4 -- the first wait's last
        // looks among those (2.5 s) -- one click is all it gets.
        std::string clicks_by;
        bool all_closed = true, one_click = true;
        for (int gone_after = 1900; gone_after <= 2700; gone_after += 100) {
            ShopRun w(1, 1, true, 0);
            bool timed = false;
            std::chrono::steady_clock::time_point first{};
            w.p.during = [&](BagCleaner&) {
                Pretend& game = w.p.game;
                if (game.full_clicks != 1) return;
                if (!timed) {
                    timed = true;
                    first = game.time.t;
                }
                game.full_up = game.time.t - first < std::chrono::milliseconds(gone_after);
            };
            w.run();
            all_closed = all_closed && w.r.shop_full && !w.p.game.full_up && !w.r.full_stuck && w.r.trouble.empty() &&
                         w.p.game.esc_with_full == 0 && !w.p.game.shop_on && w.p.game.full_clicks <= 2;
            if (gone_after <= 2400) one_click = one_click && w.p.game.full_clicks == 1;
            clicks_by += (clicks_by.empty() ? "" : ", ") + std::to_string(gone_after) + " ms: " +
                         std::to_string(w.p.game.full_clicks) + (w.r.trouble.empty() ? "" : " (" + w.r.trouble + ")");
        }
        check(all_closed && one_click,
              "a message slow to go after the first click is closed by it -- seen gone on two looks, late or not, "
              "never \"không bấm được\" -- or clicked again only on a look that shows it (" + clicks_by + ")");
    }
    {
        // A-8: left up from before, the bag shut: closed before the bag key,
        // and nothing else pressed -- no key at all.
        ShopRun s(3, 3);
        s.p.game.bag_shut = s.p.game.bag_keyed = true;
        s.p.game.show_full();
        s.run();
        check(s.r.shop_full && !s.r.ran && s.p.game.full_clicks == 1 && !s.p.game.full_up && s.p.game.keys == 0 &&
                  s.p.game.right_clicks == 0 && s.p.game.double_clicks == 0 && s.p.game.bag_shut && !s.r.stuck,
              "the message up at the start: closed, the bag key never pressed, nothing right-clicked -- " +
                  s.r.trouble);
        // And one that will not close there: no ESC, and no hold for it alone.
        ShopRun t(3, 3);
        t.p.game.bag_shut = t.p.game.bag_keyed = true;
        t.p.game.full_sticks = true;
        t.p.game.show_full();
        t.run();
        check(t.r.shop_full && t.r.full_stuck && t.p.game.full_clicks == 2 && t.p.game.keys == 0 && !t.r.stuck,
              "one that will not close at the start: two clicks, no key, and the keys not held for a message alone -- " +
                  t.r.trouble);
    }
    {
        // A trial that meets the full bag says so, buys nothing and presses no digit.
        ShopRun s(1, 1, true, 0);
        s.run(true);
        check(s.r.shop_full && s.p.game.buys.empty() && s.p.game.digit_keys == 0 && s.p.game.full_clicks == 1 &&
                  spoken(s, "thử mua: game báo túi đồ đã đầy") && !s.p.game.shop_on,
              "a trial that meets the full bag: said, closed, nothing bought or typed -- " + s.r.trouble);
    }
    {
        // What a hold kept for a purchase is let go on (A-3): the full-bag
        // message, or another, alone is no reason.
        const Kit& k = kit();
        Frame shop = scene(1024, 768, 60);
        blit(shop, *k.store.get(bag::kShopRow), 100, 50);
        blit(shop, *k.store.get(bag::kShopButtons), 100 + bag::kShopButtonsDx, 50 + bag::kShopButtonsDy);
        Frame buttons_only = scene(1024, 768, 60);
        blit(buttons_only, *k.store.get(bag::kShopButtons), 400, 400);
        const Frame& pair = *k.store.get("grabbed/confirm_button");
        Frame half = scene(1024, 768, 60);
        blit(half, bag::cut(pair, ScreenRect{ 0, 0, pair.width / 2 - 1, pair.height }), 500, 300);
        Frame other_half = scene(1024, 768, 60);
        blit(other_half, bag::cut(pair, ScreenRect{ pair.width - pair.width / 2 + 1, 0, pair.width / 2 - 1, pair.height }),
             500, 300);
        check(bag::shop_left_up(shop, k.store) && bag::shop_left_up(buttons_only, k.store) &&
                  bag::shop_left_up(half, k.store) && bag::shop_left_up(other_half, k.store) &&
                  bag::shop_left_up(scene(1024, 768, 0), k.store) && bag::shop_left_up(Frame{}, k.store) &&
                  !bag::shop_left_up(scene(1024, 768, 60), k.store),
              "shop_left_up: the shop, its buttons alone, either button of a question alone, no frame and a black "
              "one are things still up; a clear screen is not");
        Frame full = scene(1024, 768, 60);
        blit(full, *k.store.get(bag::kShopFull), 300, 200);
        Frame lone = scene(1024, 768, 60);
        blit(lone, *k.store.get("grabbed/dialog_ok"), 300, 200);
        check(bag::shop_full_up(full, k.store) && !bag::shop_left_up(full, k.store) &&
                  !bag::shop_left_up(lone, k.store),
              "and the full-bag message alone, or a message GoBot does not know, is not: it takes no key");
    }

    // ---- other messages, and purchases not seen through (A-6, D11)
    {
        BagCleaner::forget_said_once();
        ShopRun s(2, 2);
        s.p.game.lone_instead = true;
        s.p.debug_dir = temp_path("shop_message_debug");
        make_folder(s.p.debug_dir);
        for (const std::string& name : files_in(s.p.debug_dir, "*.png")) remove_file(s.p.debug_dir + "\\" + name);
        s.run();
        check(s.r.shop_message && !s.r.shop_full && s.r.shop_bought == 0 && s.p.game.lone_clicks == 1 &&
                  !s.p.game.lone_up && s.p.game.digit_keys == 0 && s.p.game.double_clicks == 1 && !s.p.game.shop_on &&
                  !files_in(s.p.debug_dir, "*shop_message.png").empty(),
              "another message in place of the box: its picture kept, its Xác Nhận clicked, the round over, never "
              "taken for the full bag -- " + s.r.trouble + " / " + s.p.last());
        for (const std::string& name : files_in(s.p.debug_dir, "*.png")) remove_file(s.p.debug_dir + "\\" + name);
        RemoveDirectoryA(s.p.debug_dir.c_str());
    }
    {
        // The question with its buttons hidden from find_dialogs is not a message.
        ShopRun s(1, 0);
        s.p.game.qty_hidden = true;
        s.run();
        check(!s.r.shop_message && !s.r.shop_full && s.r.shop_bought == 0 && s.p.game.digit_keys == 0 &&
                  s.p.game.wrong_confirms == 0 && s.r.trouble.find("không thấy hộp nhập số lượng") != std::string::npos,
              "the quantity question with Hủy Bỏ drawn pressed, which find_dialogs does not see: no message, nothing "
              "typed or confirmed -- " + s.r.trouble);
        // A message on one look only, the box on the next: not a message.
        ShopRun t(2, 0);
        t.p.game.lone_flash = true;
        t.run();
        check(!t.r.shop_message && t.r.trouble.empty() && t.r.shop_bought == 2 && t.p.game.lone_clicks == 0,
              "a message caught on one look and gone on the next: not a message, both bought -- " + t.r.trouble);
    }
    {
        // Xác Nhận that the box does not take: the question not seen gone,
        // and the purchase counted as bought, the round over. Watched for
        // react_ms() from the Xác Nhận since 2026.11.15d, which is the stuck
        // rule's 30 s from that same click: no Hủy Bỏ then (shop_cancel asks
        // the stuck rule first) -- the ESCs shut the question, the shop and
        // the bag, as S2 of the stuck rule has it.
        ShopRun s(3, 3);
        s.p.game.confirm_ignored = true;
        s.run();
        check(s.r.shop_unsure == 1 && s.r.shop_unsure_hp == 1 && s.r.shop_bought == 0 && s.p.game.buys.empty() &&
                  s.p.game.double_clicks == 1 && s.p.game.cancels == 0 && !s.p.game.qty_up && s.r.esc_swept &&
                  !s.p.game.shop_on && s.r.trouble.find("không chắc lần mua") != std::string::npos &&
                  s.r.trouble.find("kẹt quá 30 giây (mua bình)") != std::string::npos,
              "a question still up after Xác Nhận: unsure, counted, no more bought, shut with ESC after 30 s -- " +
                  s.r.trouble);
        // A message after the question has gone, on both looks (2026.11.11c,
        // the owner: "cần sửa để bot tự tắt thông báo này"): closed by its own
        // Xác Nhận, the purchase counted as bought as an unsure one is, and the
        // round goes on. The pretend game puts one up after every purchase, so
        // the second ends the round -- closed too -- and the shop is shut by
        // an ESC that meets no message.
        ShopRun t(3, 3);
        t.p.game.lone_after_confirm = true;
        t.run();
        check(t.r.shop_notices == 2 && t.r.shop_unsure == 2 && t.r.shop_unsure_hp == 1 && t.r.shop_unsure_mp == 1 &&
                  t.r.shop_bought == 0 && t.p.game.buys.size() == 2 && t.p.game.lone_clicks == 2 &&
                  t.p.game.double_clicks == 2 && !t.r.shop_message && !t.r.shop_full,
              "a message after the question has gone: closed by its own Xác Nhận, counted as bought, the round goes "
              "on to the second -- " + t.r.trouble);
        check(!t.p.game.lone_up && !t.p.game.shop_on && !t.r.stuck && t.p.game.esc_with_lone == 0 &&
                  t.p.game.esc_presses == 1 && t.p.game.thoat_clicks == 0 &&
                  t.r.trouble.find("thông báo lạ 2 lần") != std::string::npos &&
                  t.r.trouble.find("không bấm ESC") == std::string::npos,
              "the second notice in a round ends it, closed too: the shop shut by one ESC, nothing left up or held, "
              "the trouble says two -- " + t.r.trouble);
        // One notice only, then the bag full: closed and counted, and the next
        // double-click meets the full-bag message as usual.
        ShopRun one(3, 3);
        one.p.game.lone_after_confirm = true;
        one.p.game.room = 1;
        one.run();
        check(one.r.shop_notices == 1 && one.r.shop_unsure == 1 && one.r.shop_full && one.p.game.lone_clicks == 1 &&
                  one.p.game.buys.size() == 1 && !one.p.game.lone_up && !one.p.game.shop_on && !one.r.stuck &&
                  one.p.game.esc_with_lone == 0,
              "one notice then the bag full: closed, counted, and the next double-click meets the full-bag message -- " +
                  one.r.trouble);
        // A notice that two clicks do not close: left for the player as before,
        // unsure, counted, no more bought -- no third click, and no ESC into it:
        // the shop is left open behind it, the hold kept for the shop.
        ShopRun stuck(3, 3);
        stuck.p.game.lone_after_confirm = stuck.p.game.lone_sticks = true;
        stuck.run();
        check(stuck.r.shop_unsure == 1 && stuck.r.shop_notices == 0 && stuck.r.shop_bought == 0 &&
                  stuck.p.game.buys.size() == 1 && stuck.p.game.lone_clicks == 2 && stuck.p.game.double_clicks == 1 &&
                  stuck.r.trouble.find("bạn đóng nó giúp") != std::string::npos &&
                  stuck.r.trouble.find("không chắc lần mua") != std::string::npos,
              "a notice that will not close: two clicks, unsure, counted, no more bought -- " + stuck.r.trouble);
        check(stuck.p.game.esc_with_lone == 1 && stuck.p.game.esc_presses == 3 && !stuck.p.game.lone_up &&
                  !stuck.p.game.shop_on && !stuck.r.stuck && stuck.p.game.thoat_clicks == 0 && stuck.r.esc_swept,
              "and never a third click, Thoát not clicked; 10 s from the question on (2026.11.14b), ESC on it, the "
              "shop and the bag, nothing held -- " + stuck.r.trouble);
    }
    {
        // A message that comes up after the last purchase's looks, so that the
        // shop's own ESC finds it (2026.11.11c): closed by its own Xác Nhận, and
        // one ESC then shuts the shop. Put up on the first look after the tail
        // (its last look is at 1100 ms by this clock, the shop's own first look
        // after the purchase at 1140 ms).
        ShopRun s(1, 0);
        bool shown = false;
        s.p.during = [&](BagCleaner&) {
            Pretend& g = s.p.game;
            if (!shown && g.buys.size() == 1 && !g.qty_up &&
                g.time.t - g.last_confirm >= std::chrono::milliseconds(1120))
                shown = g.lone_up = true;
        };
        s.run();
        check(shown && s.p.says("trên cửa hàng") && s.p.game.lone_clicks == 1 && !s.p.game.lone_up &&
                  !s.p.game.shop_on && !s.r.stuck && s.p.game.esc_with_lone == 0 && s.p.game.esc_presses == 1 &&
                  s.p.game.thoat_clicks == 0 && s.r.shop_notices == 1 && s.r.shop_bought == 1 &&
                  s.r.trouble.empty(),
              "a message up before the shop's ESC: closed by its own Xác Nhận, then one ESC shuts the shop -- " +
                  s.r.trouble + " / " + s.p.last());
    }
    {
        // A message in place of the box whose Xác Nhận does not close it: two
        // clicks, and no ESC then; 10 s from the double-click on (2026.11.14b),
        // ESC on it, the shop and the bag -- before, the shop was left open
        // and the keys held.
        ShopRun u(2, 2);
        u.p.game.lone_instead = u.p.game.lone_sticks = true;
        u.run();
        check(u.r.shop_message && u.p.game.lone_clicks == 2 && !u.p.game.lone_up && u.p.game.esc_with_lone == 1 &&
                  u.p.game.esc_presses == 3 && u.p.game.digit_keys == 0 && !u.p.game.shop_on && !u.r.stuck &&
                  u.r.esc_swept && u.r.trouble.find("bạn đóng nó giúp") != std::string::npos,
              "a message in place of the box that will not close: two clicks, nothing typed, then after 10 s ESC on "
              "it, the shop and the bag -- " + u.r.trouble);
    }

    // ---- the check-in board (B-3): nothing clicked into it
    if (!kit().board_ok) {
        skip("the check-in board's title is not installed here\n");
    } else {
        {
            // Up as the run begins, the bag shut: no key at all.
            ShopRun s(3, 3);
            s.p.game.bag_shut = s.p.game.bag_keyed = true;
            s.p.game.board_on = true;
            s.run();
            check(s.r.board_up && !s.r.ran && s.r.trouble.empty() && s.p.game.keys == 0 && s.p.game.clicks == 0 &&
                      s.p.game.right_clicks == 0 && s.p.game.acts_with_board == 0 && s.p.game.bag_shut,
                  "the board up at the start of a shop run: nothing pressed, the bag key included");
        }
        {
            // Come up before the right-click on the item.
            ShopRun s(3, 3);
            bool once = false;
            s.p.during = [&](BagCleaner&) {
                if (!once && s.p.game.tab_clicks > 0 && s.p.game.cur_tab == kOtherTab) once = s.p.game.board_on = true;
            };
            s.run();
            check(s.r.board_up && s.p.game.opener_clicks == 0 && s.p.game.right_clicks == 0 && s.p.game.buys.empty() &&
                      s.p.game.acts_with_board == 0 && s.r.trouble.empty(),
                  "the board come up before the right-click on the item: not right-clicked, nothing pressed after -- " +
                      s.r.trouble);
        }
        {
            // Before the double-click.
            ShopRun s(3, 3);
            bool once = false;
            s.p.during = [&](BagCleaner&) {
                if (!once && s.p.game.shop_on) once = s.p.game.board_on = true;
            };
            s.run();
            check(s.r.board_up && s.p.game.opener_clicks == 1 && s.p.game.double_clicks == 0 && s.p.game.buys.empty() &&
                      !s.p.game.shop_on,
                  "the board come up as the shop opens: no double-click, the shop shut after -- " + s.r.trouble);
        }
        {
            // Before Xác Nhận: never clicked; Hủy Bỏ only on the question seen.
            ShopRun s(3, 3);
            bool once = false;
            s.p.during = [&](BagCleaner&) {
                if (!once && s.p.game.qty_up && s.p.game.typed == "100") once = s.p.game.board_on = true;
            };
            s.run();
            check(s.r.board_up && s.p.game.buys.empty() && s.p.game.wrong_confirms == 0 && s.p.game.cancels == 1 &&
                      !s.p.game.qty_up && s.r.shop_bought == 0 && s.r.shop_unsure == 0,
                  "the board come up as 100 is read back: no Xác Nhận, Hủy Bỏ on the question still seen -- " +
                      s.r.trouble);
        }
        {
            // After a purchase: no more double-clicks, and the shop shut as
            // ever -- its ESC to the board in front, then Thoát.
            ShopRun s(3, 3);
            bool once = false;
            s.p.during = [&](BagCleaner&) {
                if (!once && s.p.game.buys.size() == 1 && !s.p.game.qty_up) once = s.p.game.board_on = true;
            };
            s.run();
            check(s.r.board_up && s.r.shop_bought == 1 && s.p.game.double_clicks == 1 && s.p.game.esc_presses == 1 &&
                      s.p.game.thoat_clicks == 1 && !s.p.game.shop_on && !s.p.game.board_on,
                  "the board come up after a purchase: nothing more bought, the shop's ESC taken by the board and "
                  "Thoát clicked -- " + s.r.trouble);
        }
        // ---- the board drawn whole, as the live one: the shop and the box under it
        {
            // After a purchase, the shop hidden whole: not taken for shut --
            // its one ESC to the board, Thoát on the look after it, and no
            // hold left.
            ShopRun s(3, 3);
            bool once = false;
            s.p.during = [&](BagCleaner&) {
                if (!once && s.p.game.buys.size() == 1 && !s.p.game.qty_up)
                    once = s.p.game.board_on = s.p.game.board_whole = true;
            };
            s.run();
            check(s.r.board_up && s.r.shop_bought == 1 && s.p.game.double_clicks == 1 && s.p.game.esc_presses == 1 &&
                      s.p.game.thoat_clicks == 1 && !s.p.game.shop_on && !s.p.game.board_on && !s.r.stuck &&
                      s.p.game.acts_with_board == 1,
                  "the whole board over the shop after a purchase: the shop not taken for shut -- one ESC, to the "
                  "board, then Thoát -- and nothing else pressed into the board -- " + s.r.trouble);
        }
        for (const int k : { 1, 4 }) {
            // Come up once 100 is typed -- as the box is read back (the first
            // look), or on the look before Xác Nhận (the fourth) -- over the
            // box: hidden, not gone. No Xác Nhận, no Hủy Bỏ, no key into it,
            // the box not taken for gone; and once the box has been up the
            // owner's 10 s (2026.11.14b), ESC on the board, the box, the shop
            // and the bag, each on a look that shows it -- before, the keys
            // stayed held for what was typed.
            ShopRun s(3, 3);
            int looks = 0;
            s.p.during = [&](BagCleaner&) {
                if (!s.p.game.board_on && s.p.game.qty_up && s.p.game.typed == "100" && ++looks == k)
                    s.p.game.board_on = s.p.game.board_whole = true;
            };
            s.run();
            check(s.r.board_up && !s.r.stuck && !s.p.game.qty_up && s.p.game.esc_on_qty == 1 &&
                      s.p.game.buys.empty() && s.p.game.wrong_confirms == 0 && s.p.game.cancels == 0 &&
                      s.p.game.esc_presses == 4 && s.p.game.acts_with_board == 1 && !s.p.game.board_on &&
                      !s.p.game.shop_on && s.r.shop_bought == 0 && s.r.shop_unsure == 0 && s.r.esc_swept &&
                      s.r.trouble.find("đè lên hộp nhập số lượng") != std::string::npos,
                  "the whole board over the quantity box at look " + std::to_string(k) +
                      " after 100: nothing confirmed or cancelled, the box not taken for gone; after 10 s ESC on the "
                      "board, the box, the shop and the bag -- " + s.r.trouble);
        }
        {
            // Come up after the first 0: no more digits, no click; after 10 s
            // the same ESCs.
            ShopRun s(3, 3);
            bool once = false;
            s.p.during = [&](BagCleaner&) {
                if (!once && !s.p.game.board_on && s.p.game.qty_up && s.p.game.typed == "10")
                    once = s.p.game.board_on = s.p.game.board_whole = true;
            };
            s.run();
            check(s.r.board_up && !s.r.stuck && s.p.game.digit_keys == 1 && !s.p.game.qty_up &&
                      s.p.game.esc_on_qty == 1 && s.p.game.cancels == 0 && s.p.game.wrong_confirms == 0 &&
                      s.p.game.esc_presses == 4 && s.p.game.acts_with_board == 1 && s.p.game.buys.empty() &&
                      s.r.trouble.find("đè lên hộp nhập số lượng") != std::string::npos,
                  "the whole board over the box after the first 0: no more digits, no Hủy Bỏ blind; after 10 s ESC on "
                  "the board, the box, the shop and the bag -- " + s.r.trouble);
        }
        {
            // What the GUI's look for a clear game meets then: nothing the bin's
            // or the shop's looks can see -- only the board's title, which is
            // what keeps the shop's hold (look_for_clear).
            Pretend g;
            make_bag(g);
            g.shop_on = g.qty_up = true;
            g.open_box(true);
            g.board_on = g.board_whole = true;
            const Frame f = g.render();
            check(bag::nothing_held(f, kit().store, g.l) && !bag::shop_left_up(f, kit().store) &&
                      checkin_board_title(f, kit().store),
                  "a quantity box and a shop under the whole board: nothing_held and shop_left_up see nothing, the "
                  "board's title is seen");
        }
    }

    // ---- one purchase's guards, as ever
    {
        // Black frames now and then, as the live client gave right after the
        // quantity box came up: looked at again, never taken for the box gone.
        ShopRun s(5, 0);
        s.p.game.black_every = 6;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 5 && s.p.game.buys == std::vector<int>(5, 0) &&
                  s.p.game.wrong_confirms == 0 && s.p.game.cancels == 0 && s.p.game.black_looks > 10,
              "a black frame every sixth look: looked at again, and five Thịt bò khô bought as with none (" +
                  std::to_string(s.p.game.black_looks) + " black) -- " + s.r.trouble + " / " + s.p.last());
    }
    {
        // Black from the first look on: nothing pressed, nothing clicked, and
        // said.
        ShopRun s(5, 0);
        s.p.game.black_from = 1;
        s.run();
        check(!s.r.trouble.empty() && s.r.shop_bought == 0 && s.p.game.right_clicks == 0 &&
                  s.p.game.clicks == 0 && s.p.game.drags == 0 && s.p.game.digit_keys == 0 && s.p.game.keys == 0,
              "a game black throughout: no key, no click, no purchase -- " + s.r.trouble);
        // The black stretch waited out once, for the owner's 30 s by the clock
        // since 2026.11.15d (a look every 200 ms, BagCleaner::kBlackGapMs, and
        // the pretend look's own 40 ms; 20 looks before): some 125 looks, and
        // a few after it.
        check(s.p.game.black_looks >= kReactMs / 300 && s.p.game.black_looks <= kReactMs / 200 + 30,
              "a game black for good: " + std::to_string(s.p.game.black_looks) +
                  " black looks in all, the wait of 30 s once");
    }
    {
        // Black from the ESC that closes the shop on, for good: Thoát is not
        // clicked where the shop was, and the run keeps the hold.
        ShopRun s(2, 0);
        s.p.game.esc_closes_shop = false;
        s.p.during = [&s](BagCleaner&) {
            if (s.p.game.esc_presses > 0 && s.p.game.black_from < 0) s.p.game.black_from = s.p.game.looks + 1;
        };
        s.run();
        check(s.p.game.esc_presses == 1 && s.p.game.thoat_clicks == 0 && s.r.stuck && !s.r.trouble.empty(),
              "black from the ESC on: Thoát never clicked blind, the hold kept -- " + s.r.trouble);
    }
    {
        // Black once the quantity box has been seen, and for good: Hủy Bỏ is
        // not pressed blind, nothing is typed, and the run keeps the hold.
        ShopRun s(5, 0);
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
        ShopRun s(5, 0);
        s.p.game.row_drop = 42;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 5 && s.p.game.buys == std::vector<int>(5, 0) &&
                  s.p.game.wrong_confirms == 0,
              "the potions' row second in the shop's list: found, and five Thịt bò khô bought -- " + s.r.trouble +
                  " / " + s.p.last());
    }
    {
        // Every third digit key lost: the first of each purchase but the
        // first, so the box reads 10 -- read back, the last 0 typed again,
        // and 100 confirmed. (Every second, as before 2026.11.15d, loses the
        // retyped 0 of the second purchase too; that is below.)
        ShopRun s(4, 0);
        s.p.game.key_drops = 3;
        s.run();
        check(s.r.trouble.empty() && s.p.game.buys.size() == 4 && s.p.game.wrong_confirms == 0 &&
                  s.p.game.digit_keys > 8,
              "digits lost on the way: read back, the rest typed, and 100 is what is confirmed (" +
                  std::to_string(s.p.game.digit_keys) + " digit keys) -- " + s.r.trouble);
        // The rest typed and that lost as well: the box shows nothing of
        // those keys for react_ms() (2026.11.15d) -- the game not reacting,
        // to the owner's 30 s -- and by then the stuck rule's 30 s since
        // them are up: no more keys, no Hủy Bỏ, no Xác Nhận, ESC on the box,
        // the shop and the bag. The first purchase bought.
        ShopRun t(4, 0);
        t.p.game.key_drops = 2;
        t.run();
        check(t.p.game.buys.size() == 1 && t.p.game.wrong_confirms == 0 && t.p.game.cancels == 0 &&
                  !t.p.game.qty_up && !t.p.game.shop_on && t.r.esc_swept &&
                  t.r.trouble.find("không thành 100 sau 30 giây") != std::string::npos,
              "the rest typed and lost too: nothing of it for 30 s, no more keys, shut with ESC, one bought -- " +
                  t.r.trouble);
    }
    {
        // Keys that never reach the box, either way of typing: each given its
        // 30 s to show in it (2026.11.15d), and by then the stuck rule's 30 s
        // since the last keys are up -- no Hủy Bỏ (shop_cancel asks the stuck
        // rule first), the ESCs shut the box, the shop and the bag.
        ShopRun s(4, 0);
        s.p.game.keys_dead = true;
        s.run();
        check(s.r.shop_bought == 0 && s.p.game.wrong_confirms == 0 && s.p.game.cancels == 0 &&
                  s.p.game.digit_keys > 0 && s.r.trouble.find("không nhận phím số") != std::string::npos &&
                  !s.p.game.shop_on && !s.p.game.qty_up && s.r.esc_swept &&
                  s.r.trouble.find("kẹt quá 30 giây (mua bình)") != std::string::npos,
              "a box that never reads 100: never Xác Nhận, nothing bought, shut with ESC after 30 s -- " +
                  s.r.trouble);
    }
    {
        // As on the live client: the box Mua brings up takes no keys, the one a
        // double-click on the potion brings up does. Mua is never clicked; the
        // potion double-clicked, 00 typed straight in, five bought.
        ShopRun s(5, 0);
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
        ShopRun s(5, 0);
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
        ShopRun s(5, 0);
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
        // is, nothing confirmed, and the run does not end on a box left up.
        // Each way of typing is given its 30 s to show in the box
        // (2026.11.15d), and by then the stuck rule's 30 s since the last keys
        // are up: no Hủy Bỏ, the ESCs shut it, the shop and the bag.
        ShopRun s(5, 0);
        s.p.game.box_needs_click = true;
        s.p.game.double_unfocused = true;
        s.p.game.click_dead = true;
        bool hidden = false;
        s.p.during = [&](BagCleaner&) {
            if (s.p.game.qty_up && s.p.game.cancel_pressed && bag::find_dialogs(s.p.game.render(), kit().store).empty())
                hidden = true;
        };
        s.run();
        check(hidden && s.r.shop_bought == 0 && s.p.game.wrong_confirms == 0 && s.p.game.cancels == 0 &&
                  s.r.esc_swept && !s.p.game.qty_up && !s.p.game.shop_on && s.p.says("ô vẫn là 1") &&
                  s.r.trouble.find("không nhận phím số") != std::string::npos,
              "Hủy Bỏ drawn pressed, the pair not seen as a dialog: the box still followed by its Xác Nhận, "
              "each way of typing waited 30 s for, then shut with ESC, nothing bought -- " + s.r.trouble);
        check(s.p.game.digit_keys == 4 && s.p.game.scan_keys == 2 && s.p.game.backspaces == 0,
              "and no more keys once both ways put nothing in the box: two as key codes, two as scan codes (" +
                  std::to_string(s.p.game.digit_keys) + " digit keys)");
    }
    {
        // The same, and Hủy Bỏ does not close the box either: once nothing has
        // come of the last keys for the stuck rule's 30 s, ESC on it, the shop
        // and the bag (2026.11.14b, 30 s since 2026.11.15d) -- before, the run
        // ended keeping the hold.
        ShopRun s(5, 0);
        s.p.game.box_needs_click = true;
        s.p.game.double_unfocused = true;
        s.p.game.click_dead = true;
        s.p.game.qty_sticky = true;
        s.run();
        check(!s.r.stuck && !s.p.game.qty_up && s.p.game.esc_on_qty == 1 && s.r.esc_swept && !s.p.game.shop_on &&
                  s.r.shop_bought == 0 && s.p.game.wrong_confirms == 0 && s.p.game.digit_keys == 4 &&
                  s.r.trouble.find("kẹt quá 30 giây (mua bình)") != std::string::npos,
              "a box that will not close, its Hủy Bỏ drawn pressed: shut with ESC after 30 s, nothing bought, "
              "nothing held -- " + s.r.trouble);
    }
    {
        // A trial whose Hủy Bỏ does not close the box: not said to have
        // worked, the other potion not tried -- and shut with ESC after the
        // owner's 10 s (2026.11.14b).
        ShopRun s(1, 1);
        s.p.game.qty_sticky = true;
        s.run(true);
        check(s.r.trouble.find("thử mua: đã bấm Hủy Bỏ mà không thấy") != std::string::npos && !s.r.stuck &&
                  s.p.game.esc_on_qty == 1 && !s.p.game.qty_up && !s.p.says("thử mua: dừng ở hộp nhập số lượng") &&
                  s.p.game.digit_keys == 0 && s.p.game.buys.empty(),
              "a trial whose Hủy Bỏ leaves the box up: said so, stopped, the box shut with ESC -- " +
                  s.r.trouble);
    }
    {
        // The press speed: the same five bought at each step, read back as 100
        // every time; the quicker the step, the less time the run takes.
        std::vector<long long> took;
        bool same = true;
        for (const int ms : { 60, 80, 150 }) {
            ShopRun s(5, 0);
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
        // The shop open already -- opened late after the last run gave up on
        // it: taken as it is, the item not right-clicked again, five bought.
        ShopRun s(5, 0);
        s.p.game.shop_on = true;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 5 && s.p.game.wrong_confirms == 0 &&
                  s.p.game.opener_clicks == 0 && s.p.says("cửa hàng đã mở sẵn"),
              "the shop open already: taken as it is, five bought (" + std::to_string(s.p.game.opener_clicks) +
                  " right-clicks on the item) -- " + s.r.trouble + " / " + s.p.last());
    }
    {
        ShopRun s(2, 2);
        s.p.game.qty_is_sell = true;
        s.run();
        check(s.r.shop_bought == 0 && s.p.game.digit_keys == 0 && s.p.game.wrong_confirms == 0 &&
                  s.p.game.cancels >= 1 && !s.p.game.shop_on,
              "the sell question where the buy question should be: nothing typed, Hủy Bỏ -- " + s.r.trouble);
    }
    {
        ShopRun s(5, 0, false);
        s.run();
        check(s.p.game.right_clicks == 0 && s.p.game.opener_clicks == 0 &&
                  s.r.trouble.find("không thấy món Người Giữ Sân") != std::string::npos,
              "no Người Giữ Sân item in Khác: nothing right-clicked -- " + s.r.trouble);
    }
    {
        // No scroll bar to read: said as that, with the bag's picture kept,
        // and nothing right-clicked on Khác.
        ShopRun s(5, 0);
        s.p.game.thumb = false;
        s.p.debug_dir = temp_path("shop_debug");
        s.run();
        check(s.p.game.opener_clicks == 0 && s.p.game.right_clicks == 0 &&
                  s.r.trouble.find("thanh cuộn") != std::string::npos &&
                  !files_in(s.p.debug_dir, "*shop_scroll_unknown*.png").empty(),
              "a scroll bar that cannot be read: said, the picture kept, nothing right-clicked -- " + s.r.trouble);
    }
    {
        // A slow game (2026.11.15d, the owner: "sẽ có thêm 30 giây chờ phản
        // ứng"): the shop drawn forty looks -- some 7.6 s -- after the
        // right-click is waited for, and the round goes on at once, every
        // purchase made; the log says how long it took.
        LogLines log;
        ShopRun s(5, 0);
        s.p.game.open_after_looks = 40;
        s.run();
        check(s.p.game.opener_clicks == 1 && s.r.trouble.empty() && s.r.shop_bought == 5 &&
                  s.p.game.buys == std::vector<int>(5, 0) && log.has("Trash: the game took ") &&
                  log.has(" ms to show the shop after the right-click"),
              "a shop that comes 7.6 s after the right-click: waited for, five bought -- " + s.r.trouble + " / " +
                  log.line_with("to show the shop"));
    }
    {
        // The shop comes only after GoBot has stopped waiting for it -- the 30
        // s of the right-click's wait are some 158 looks of the pretend game:
        // waited for once more and shut, with its one ESC, nothing bought; the
        // wait given up after its 30 s and not much later.
        LogLines log;
        ShopRun s(5, 0);
        s.p.game.open_after_looks = 165;
        s.run();
        check(s.p.game.opener_clicks == 1 && s.p.game.digit_keys == 0 && s.p.game.esc_presses == 1 &&
                  !s.p.game.shop_on && s.r.trouble.find("không thấy cửa hàng hiện ra sau 30 giây") != std::string::npos &&
                  log.has("Trash: nothing after 30 s -- the shop after the right-click"),
              "a shop that comes later than its 30 s: waited for once more and shut -- " + s.r.trouble);
    }
    {
        ShopRun s(5, 0);
        s.p.game.opener_opens = false;
        s.run();
        check(s.p.game.opener_clicks == 1 && s.p.game.digit_keys == 0 && s.p.game.esc_presses == 0 &&
                  s.r.trouble.find("không thấy cửa hàng") != std::string::npos,
              "the item right-clicked and no shop come: nothing more pressed, ESC included -- " + s.r.trouble);
    }
    {
        ShopRun s(1, 1);
        s.run(true);
        check(s.r.trouble.empty() && s.p.game.opener_clicks == 1 && s.p.game.digit_keys == 0 &&
                  s.p.game.cancels == 2 && s.p.game.buys.empty() && s.r.shop_bought == 0 && !s.p.game.shop_on &&
                  spoken(s, "thử mua: dừng ở hộp nhập số lượng") && s.p.game.use_tab_clicks == 0,
              "a trial: both items tried as far as the box, Hủy Bỏ each time, nothing typed or bought, the shop shut -- " +
                  s.r.trouble);
        // A trial whatever the plan: one of each, both 0 included.
        ShopRun t(0, 0);
        t.run(true);
        check(t.r.trouble.empty() && t.p.game.double_clicks == 2 && t.p.game.cancels == 2 && t.p.game.digit_keys == 0,
              "a trial with both set to 0 still tries one of each as far as the box -- " + t.r.trouble);
    }
    {
        ShopRun s(2, 2);
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
        // The question replaced by another between two keys: no more digits, Hủy Bỏ, never Xác Nhận.
        ShopRun s(2, 2);
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
}

/// The plan's bookkeeping between rounds (bag::shop_owed_after), the first
/// look after a start (bag::start_due_ms) and the clock saved when a plan
/// begins: pure, no game.
void test_shop_plan() {
    section("Mua bình: what is still owed of a plan after each round, and the first look after a start");
    using bag::ShopEnd;
    const bag::ShopOwed plan = bag::shop_new_plan(3, 3);
    check(plan.hp == 3 && plan.mp == 3 && plan.troubles == 0, "a new plan owes the slots set");
    bag::ShopOwed o = bag::shop_owed_after(plan, 2, 1, 0, 0, ShopEnd::Clean);
    check(o.hp == 1 && o.mp == 2 && o.troubles == 0, "bought 2 + 1, clean: 1 + 2 still owed");
    o = bag::shop_owed_after(plan, 1, 0, 1, 0, ShopEnd::Trouble);
    check(o.hp == 1 && o.mp == 3 && o.troubles == 1,
          "one bought and one unsure, then trouble: both counted, 1 + 3 owed, one trouble");
    // 2026.11.24 (the owner: "Kiểm tra các phần khác xem có phần nào không làm
    // được thì dừng không, cần thay cơ chế này bằng 1 cơ chế khác"): troubles
    // in a row never drop the plan -- until then the second dropped it until
    // the next. Counted, the rest owed, tried again every 30 s.
    const bag::ShopOwed two = bag::shop_owed_after(o, 0, 0, 0, 0, ShopEnd::Trouble);
    check(two.hp == 1 && two.mp == 3 && two.troubles == 2,
          "a second trouble in a row: what is owed kept, two troubles counted -- the plan no longer dropped");
    bag::ShopOwed five = two;
    for (int i = 0; i < 3; ++i) five = bag::shop_owed_after(five, 0, 0, 0, 0, ShopEnd::Trouble);
    check(five.hp == 1 && five.mp == 3 && five.troubles == 5,
          "five troubles in a row: still 1 + 3 owed, five counted -- no limit");
    {
        // What may have gone through is still never bought again, trouble
        // after trouble: an unsure purchase of each round counted as bought.
        bag::ShopOwed u = bag::shop_new_plan(3, 3);
        u = bag::shop_owed_after(u, 0, 0, 1, 0, ShopEnd::Trouble);
        u = bag::shop_owed_after(u, 0, 0, 0, 1, ShopEnd::Trouble);
        u = bag::shop_owed_after(u, 1, 0, 1, 1, ShopEnd::Trouble);
        check(u.hp == 0 && u.mp == 1 && u.troubles == 3,
              "unsure purchases in three troubled rounds in a row: each counted as bought, 0 + 1 owed, three "
              "troubles -- never bought twice");
    }
    const bag::ShopOwed reset = bag::shop_owed_after(bag::shop_owed_after(o, 0, 0, 0, 0, ShopEnd::Clean), 0, 0, 0, 0,
                                                     ShopEnd::Trouble);
    check(reset.hp == 1 && reset.mp == 3 && reset.troubles == 1,
          "a clean round between two troubles: they are not in a row");
    check(bag::shop_owed_after(five, 1, 0, 0, 0, ShopEnd::Clean).troubles == 0 &&
              bag::shop_owed_after(five, 1, 0, 0, 0, ShopEnd::Clean).hp == 0 &&
              bag::shop_owed_after(five, 1, 0, 0, 0, ShopEnd::Clean).mp == 3,
          "a clean round after five troubles: the troubles reset, the rest bought on as a part");
    check(bag::shop_owed_after(plan, 1, 1, 0, 0, ShopEnd::Full).hp == 0 &&
              bag::shop_owed_after(plan, 1, 1, 0, 0, ShopEnd::Full).mp == 0 &&
              bag::shop_owed_after(plan, 0, 0, 0, 0, ShopEnd::Message).hp == 0 &&
              bag::shop_owed_after(five, 0, 0, 0, 0, ShopEnd::Full).hp + bag::shop_owed_after(five, 0, 0, 0, 0,
                                                                                              ShopEnd::Full).mp == 0 &&
              bag::shop_owed_after(five, 0, 0, 0, 0, ShopEnd::Full).troubles == 0 &&
              bag::shop_owed_after(five, 0, 0, 0, 0, ShopEnd::Message).troubles == 0 &&
              bag::shop_owed_after(five, 0, 0, 0, 0, ShopEnd::Message).mp == 0,
          "the bag full, or another message: nothing more of this plan, MP included -- after troubles too, which "
          "they reset (the game said no: unchanged on 2026.11.24)");
    const bag::ShopOwed paused = bag::shop_owed_after(o, 0, 0, 0, 0, ShopEnd::Paused);
    check(paused.hp == o.hp && paused.mp == o.mp && paused.troubles == o.troubles,
          "the board up, HP low, somebody at the mouse, a stop: owed and troubles as they were");
    check(bag::shop_owed_after(plan, 9, 9, 0, 0, ShopEnd::Clean).hp == 0 && bag::shop_new_plan(-2, 4).hp == 0,
          "never under 0");
    const bag::ShopOwed fresh = bag::shop_new_plan(5, 0);
    check(fresh.hp == 5 && fresh.mp == 0 && fresh.troubles == 0, "the next plan replaces whatever was owed: 5 + 0");

    // 2026.11.10, the owner's "Không giới hạn": the numbers go to the box's
    // bound, and a plan over what one hold buys is bought in parts -- the
    // holds themselves as they were.
    check(CleanSettings::kBuySlotsPerRunMost == 999 && CleanSettings::kBuySlotsFromOldMost == 20 &&
              BagCleaner::kShopMaxBuys == 40 && BagCleaner::kShopBuysPerHold == 10 &&
              BagCleaner::kShopPartGapMs == 60'000 && BagCleaner::kShopDeadlineMostMs == 300'000,
          "the numbers 0 to 999, an old key's 20; a hold's 40 (Mua ngay) and 10 (timed), a part's minute, 300 s "
          "as they were");
    {
        const bag::ShopOwed big = bag::shop_new_plan(72, 72);
        const auto first = bag::shop_order(big.hp, big.mp, BagCleaner::kShopMaxBuys);
        check(big.hp == 72 && big.mp == 72 && first.size() == 40 &&
                  std::count(first.begin(), first.end(), bag::ShopPotion::Hp) == 20,
              "72 + 72 owed: \"Mua ngay\" buys 40 of it in its one hold, 20 HP and 20 MP in turns");
        const bag::ShopOwed rest = bag::shop_owed_after(big, 20, 20, 0, 0, ShopEnd::Clean);
        const auto part = bag::shop_order(rest.hp, rest.mp, BagCleaner::kShopBuysPerHold);
        check(rest.hp == 52 && rest.mp == 52 && part.size() == 10 &&
                  std::count(part.begin(), part.end(), bag::ShopPotion::Mp) == 5,
              "52 + 52 left, bought 10 a timed part, five of each");
        check(bag::shop_owed_after(rest, 3, 2, 0, 0, ShopEnd::Full).hp == 0 &&
                  bag::shop_owed_after(rest, 3, 2, 0, 0, ShopEnd::Full).mp == 0,
              "and the bag full ends it, however much was still owed");
        const bag::ShopOwed most =
            bag::shop_new_plan(CleanSettings::kBuySlotsPerRunMost, CleanSettings::kBuySlotsPerRunMost);
        check(bag::shop_order(most.hp, most.mp, BagCleaner::kShopMaxBuys).size() == 40 &&
                  bag::shop_order(most.hp, most.mp, BagCleaner::kShopBuysPerHold).size() == 10,
              "999 + 999: still 40 a \"Mua ngay\" hold and 10 a timed one");
        check(BagCleaner::shop_deadline_ms(BagCleaner::kShopMaxBuys) < BagCleaner::kShopDeadlineMostMs &&
                  BagCleaner::shop_deadline_ms(BagCleaner::kShopBuysPerHold) < BagCleaner::kShopDeadlineMostMs,
              "a hold's deadline under 300 s for 40 purchases, and for 10");
    }

    // The first look after a start (2026.11.12, the owner: "khi bật thì bắt
    // đầu đếm thời gian"): a whole interval from the start, but for a clock
    // the last worker left still counting -- a quick MACRO OFF and ON -- kept,
    // 45 s at least. Before, 45 s, or what was left of the plan time saved.
    const std::uint64_t t0 = 9'000'000;
    check(bag::start_due_ms(0, t0, 60, 45'000) == t0 + 3'600'000 &&
              bag::start_due_ms(0, t0, 15, 45'000) == t0 + 900'000,
          "none kept: a whole interval from the start, 60 or 15 minutes -- not 45 s");
    check(bag::start_due_ms(t0 + 1'200'000, t0, 60, 45'000) == t0 + 1'200'000,
          "a clock still counting, 20 minutes left: kept, 20 minutes");
    check(bag::start_due_ms(t0 + 30'000, t0, 60, 45'000) == t0 + 45'000,
          "30 s left of it: 45 s at least, never straight away");
    check(bag::start_due_ms(t0 - 600'000, t0, 60, 45'000) == t0 + 3'600'000 &&
              bag::start_due_ms(t0, t0, 60, 45'000) == t0 + 3'600'000,
          "one that ran out while nothing ran, or runs out at the start: counted afresh, a whole hour");
    check(bag::start_due_ms(t0 + 50 * 60'000ull, t0, 15, 45'000) == t0 + 50 * 60'000ull,
          "a clock of an hour's interval, lowered to 15 minutes since: what was left of it kept, as a MACRO OFF "
          "and ON kept it before");
    check(bag::start_due_ms(0, t0, 0, 45'000) == t0 + 60'000 && bag::start_due_ms(0, t0, -5, 15'000) == t0 + 60'000,
          "an interval under a minute counts one minute");

    // The file the plan time is kept in: shop_clock.txt beside the charms'
    // charm_clock.txt -- only compared here, never opened -- and the shop's
    // one id written and read back as the worker does it, in a file of the
    // test's own. Since 2026.11.12 a start no longer waits by it: a whole
    // interval from the start is never sooner than the plan it holds.
    const std::int64_t now = 1'800'000'000;
    const std::string charms = CharmClock::default_path(), shop = shop_clock_path();
    const std::size_t cut = charms.find_last_of("\\/");
    check(cut != std::string::npos && shop == charms.substr(0, cut + 1) + "shop_clock.txt" &&
              std::string(kShopClockId) == "shop",
          "the plan time kept in shop_clock.txt beside charm_clock.txt, by the id \"shop\": " + shop);
    const std::string path = temp_path("gobot_shop_clock.txt");
    DeleteFileA(path.c_str());
    {
        CharmClock c(path);
        check(!c.load() && c.next(kShopClockId) == 0, "no shop_clock.txt yet: no plan time");
        check(c.set_next(kShopClockId, now + 3600), "a plan begun at `now`: the next at now + 60 min, written");
    }
    {
        // GoBot started again 10 minutes on: the plan time read back is 50
        // minutes ahead, and the first look, a whole hour from that start,
        // comes 10 minutes after it -- never sooner.
        CharmClock again(path);
        const std::uint64_t start = 9'000'000, first = bag::start_due_ms(0, start, 60, 45'000);
        check(again.load() && again.next(kShopClockId) == now + 3600 &&
                  static_cast<std::int64_t>((first - start) / 1000) >= again.next(kShopClockId) - (now + 600),
              "read back as written; a GoBot started again 10 minutes on looks a whole hour on, after the 50 "
              "minutes left of the plan");
    }
    DeleteFileA(path.c_str());
}

/// The live client's own pictures of the Người Giữ Sân item's shop
/// (2026-11-11, 1024x768, Dược Phẩm chosen): beside GoBot in trash_debug, or
/// the copies in build\handoff\shop\testdata.
std::vector<std::pair<std::string, Frame>> live_shop_frames() {
    std::vector<std::pair<std::string, Frame>> out;
    for (const char* name : { "20261111_214922_0_shop_full.png", "20261111_221637_5_shop_full.png" }) {
        std::vector<std::string> paths{ exe_relative("trash_debug\\") + name };
#ifdef GOBOT_SOURCE_DIR
        paths.push_back(std::string(GOBOT_SOURCE_DIR) + "\\build\\handoff\\shop\\testdata\\" + name);
#endif
        for (const std::string& path : paths) {
            Frame f;
            if (load_png(path, f) && f.valid()) {
                out.emplace_back(name, std::move(f));
                break;
            }
        }
    }
    return out;
}

/// Columns `from`..`to` of rows `y0`..`y1` of `f` moved `by` pixels across,
/// what they leave filled with each row's own colour at `ground_x`: a name of
/// the game's own letters laid out again.
void shift_cols(Frame& f, int from, int to, int by, int y0, int y1, int ground_x) {
    const Frame was = f;
    for (int y = y0; y <= y1; ++y) {
        std::uint8_t gr = 0, gg = 0, gb = 0;
        was.get(ground_x, y, gr, gg, gb);
        for (int x = from; x <= to; ++x) put_px(f, x, y, gr, gg, gb);
        for (int x = from; x <= to; ++x) {
            std::uint8_t r = 0, g = 0, b = 0;
            was.get(x, y, r, g, b);
            put_px(f, x + by, y, r, g, b);
        }
    }
}

/// 2026.11.24, the owner: "Khi ra tới bản đồ TổCổLong thì cần mua lại HP cho
/// pet. Cái này ở trong tab Linh Thú của người giữ sân và là item "Thịtbò" =>
/// cần có lựa chọn để mua và mua bao nhiêu slot tương tự như mua HP MP". The
/// settings; the shop's tabs and the pet's food's name on the live client's
/// own pictures; and the round on the pretend game.
void test_pet_shop() {
    section("Mua HP pet (2026.11.24): Thịt bò on the shop's Linh Thú tab -- the settings, the tabs and the name on "
            "the live pictures, the round");
    // ---- the settings ----
    {
        const std::string path = temp_path("gobot_pet_trash.ini");
        CleanSettings s;
        write_all(path, "[Trash]\nenabled=false\n");
        check(s.load(path) && !s.buy_pet && s.buy_pet_slots == 1 && CleanSettings::kBuyPetSlotsDefault == 1 &&
                  std::string(CleanSettings::kBuyPetName) == "Thịtbò",
              "a file without the keys -- every file before 2026.11.24: off, one slot");
        write_all(path, "[Trash]\nbuy_pet=true\nbuy_pet_slots=4\n");
        check(s.load(path) && s.buy_pet && s.buy_pet_slots == 4, "buy_pet=true and 4 slots read back");
        write_all(path, "[Trash]\nbuy_pet_slots=4\n");
        check(s.load(path) && !s.buy_pet && s.buy_pet_slots == 4,
              "off again with its line taken out: never left as an earlier reading had it");
        write_all(path, "[Trash]\nbuy_pet=true\nbuy_pet_slots=5000\n");
        const bool high = s.load(path) && s.buy_pet_slots == CleanSettings::kBuySlotsPerRunMost;
        write_all(path, "[Trash]\nbuy_pet=true\nbuy_pet_slots=-2\n");
        check(high && s.load(path) && s.buy_pet_slots == 0, "slots held to 0..999, as the potions'");
        // Written as the card writes it (IniFile::set_in_place), read back.
        write_all(path, "[Trash]\nenabled=false\n");
        check(IniFile::set_in_place(path, "Trash", "buy_pet", "true") &&
                  IniFile::set_in_place(path, "Trash", "buy_pet_slots", "2") && s.load(path) && s.buy_pet &&
                  s.buy_pet_slots == 2,
              "the card's writes read back: on, 2 slots");
        DeleteFileA(path.c_str());
#ifdef GOBOT_SOURCE_DIR
        CleanSettings shipped;
        const std::string text = read_all(std::string(GOBOT_SOURCE_DIR) + "/configs/trash.ini");
        check(shipped.load(std::string(GOBOT_SOURCE_DIR) + "/configs/trash.ini") && !shipped.buy_pet &&
                  shipped.buy_pet_slots == 1 && text.find("\nbuy_pet=false\n") != std::string::npos &&
                  text.find("\nbuy_pet_slots=1\n") != std::string::npos,
              "the shipped trash.ini ships it off, one slot: buy_pet=false, buy_pet_slots=1");
#endif
    }
    // ---- the round's order ----
    {
        const auto o = bag::shop_round_order(2, 1, 1, 40);
        check(o.size() == 4 && o[0] == bag::ShopPotion::Pet && o[1] == bag::ShopPotion::Pet &&
                  o[2] == bag::ShopPotion::Hp && o[3] == bag::ShopPotion::Mp,
              "the pet's food first, then the potions' turns: one switch of the tab each way");
        check(bag::shop_round_order(3, 0, 0, 2).size() == 2 && bag::shop_round_order(0, 2, 1, 40) == bag::shop_order(2, 1, 40) &&
                  bag::shop_round_order(-1, 0, 0, 40).empty(),
              "cut to the most a round buys; with none of the pet's, the potions' order as ever");
    }
    if (skip_without_kit()) return;
    if (!kit().shop_ok) {
        skip("the shop's pictures are not installed here\n");
        return;
    }
    const Frame& buttons_pic = *kit().store.get(bag::kShopButtons);

    // ---- the live pictures: the tabs from the buttons ----
    const auto frames = live_shop_frames();
    if (frames.empty()) skip("no picture of the live shop here (trash_debug, build\\handoff\\shop\\testdata)\n");
    for (const auto& [name, f] : frames) {
        const TemplateMatcher::Match m = TemplateMatcher::find(f, ScreenRect{ 0, 0, f.width, f.height }, buttons_pic, 0.85f);
        check(m.found && m.at.x == 497 && m.at.y == 541,
              name + ": the shop's Mua | Thoát at 497,541 (" + std::to_string(m.at.x) + "," + std::to_string(m.at.y) + ")");
        if (!m.found) continue;
        const ScreenRect b = m.at;
        const ScreenRect pet = bag::shop_tab_rect(b, bag::kShopTabPet);
        check(pet.x == 498 && pet.right() == 562 && pet.y == 178 && pet.bottom() == 199 && pet.cx() == 530 &&
                  pet.cy() == 188 && pet.cx() - b.x == 33 && pet.cy() - b.y == -353,
              name + ": Linh Thú from x 498 to 561, y 178 to 198 -- clicked at 530,188, 33 right of the buttons and "
                     "353 above them");
        const ScreenRect first = bag::shop_tab_rect(b, bag::kShopTabPotions);
        check(first.x == 306 && first.cx() == 338 && bag::shop_tab_rect(b, 4).right() == 626,
              name + ": Dược Phẩm from x 306, Công Thức Chế Tạo to 625: five tabs of 64");
        std::string drawn;
        for (int t = 0; t < bag::kShopTabs; ++t) drawn += std::to_string(bag::shop_tab_drawn(f, b, t));
        const bag::ShopTabs tabs = bag::shop_tabs(f, b);
        check(drawn == "10000" && tabs.strip && tabs.chosen == bag::kShopTabPotions,
              name + ": the strip read where the buttons put it, Dược Phẩm chosen and the rest not (" + drawn + ")");
        // The tab's own words: where MenuText reads "Linh Thú" over the whole
        // strip, it is in that tab.
        const ScreenRect strip{ first.x, first.y, 5 * bag::kShopTabW, bag::kShopTabH };
        const MenuText::Hit words = MenuText::find(f, strip, bag::kShopTabNames[bag::kShopTabPet]);
        std::printf("  [info] %s: \"Linh Thú\" read over the strip %.2f at %d,%d %dx%d (%s %d px)\n", name.c_str(),
                    static_cast<double>(words.score), words.at.x, words.at.y, words.at.w, words.at.h,
                    words.face.c_str(), words.px);
        check(!words.found || pet.contains(words.at.cx(), words.at.cy()),
              name + ": where MenuText reads the words Linh Thú on the strip, they are in the tab the buttons put "
                     "there");
        // Linh Thú chosen instead: the two tabs' bodies swapped.
        Frame chosen = f;
        for (const int dy : bag::kShopTabBodyRows)
            for (int x = 0; x < bag::kShopTabW; ++x) {
                std::uint8_t r0 = 0, g0 = 0, b0 = 0, r3 = 0, g3 = 0, b3 = 0;
                f.get(first.x + x, first.y + dy, r0, g0, b0);
                f.get(pet.x + x, pet.y + dy, r3, g3, b3);
                put_px(chosen, first.x + x, first.y + dy, r3, g3, b3);
                put_px(chosen, pet.x + x, pet.y + dy, r0, g0, b0);
            }
        const bag::ShopTabs now = bag::shop_tabs(chosen, b);
        check(now.strip && now.chosen == bag::kShopTabPet && bag::shop_tab_drawn(chosen, b, bag::kShopTabPet) == 1,
              name + ": Linh Thú drawn as the chosen one is: read chosen, Dược Phẩm not");
        Frame covered = f;
        fill(covered, ScreenRect{ pet.x + 10, pet.y, 30, pet.h }, 230, 230, 230);
        check(bag::shop_tab_drawn(covered, b, bag::kShopTabPet) == -1 && !bag::shop_tabs(covered, b).strip,
              name + ": something over the tab: not read either way, nothing clicked by the strip");
        check(bag::shop_tab_drawn(f, ScreenRect{ 10, 100, 119, 21 }, 0) == -1,
              name + ": a strip off the frame: not read");

        // ---- the live pictures: the pet's name, whole ----
        const bag::ShopItemSeen piece = bag::shop_item(f, b, CleanSettings::kBuyPetName);
        std::printf("  [info] %s: \"%s\" in Dược Phẩm's list: ok %d, best %.2f at %d,%d %dx%d, %d passed over (%s)\n",
                    name.c_str(), CleanSettings::kBuyPetName, piece.ok ? 1 : 0, static_cast<double>(piece.score),
                    piece.at.x, piece.at.y, piece.at.w, piece.at.h, piece.passed, piece.why);
        check(!piece.ok, name + ": Thịtbò on Dược Phẩm is only the start of Thịtbòkhô: refused, nothing to click (" +
                             piece.why + ")");
        const bag::ShopItemSeen whole = bag::shop_item(f, b, "Thịtbòkhô");
        check(whole.ok && whole.cell.column == 1 && whole.cell.x == 463 + bag::kShopMpX && whole.cell.y > 285 &&
                  whole.cell.y < 320,
              name + ": Thịtbòkhô itself found whole, in the right column -- its icon box at " +
                  std::to_string(whole.cell.x) + "," + std::to_string(whole.cell.y));
        const bag::ShopItemSeen mp = bag::shop_item(f, b, "Rượulátre");
        check(mp.ok && mp.cell.column == 0 && mp.cell.x == 311 + bag::kShopMpX,
              name + ": Rượulátre found whole, in the left column");
        // Thịtbò laid out as the list would lay out its own name: the game's
        // own letters moved right to where the names end, khô gone.
        // (Thịtbò's letters and their outline, 552 to 582 of rows 285 to 301,
        // moved 17 right onto where khô ended, at 598.)
        Frame alone = f;
        shift_cols(alone, 552, 582, 17, 285, 301, 520);
        const bag::ShopItemSeen found = bag::shop_item(alone, b, CleanSettings::kBuyPetName);
        std::printf("  [info] %s: \"%s\" laid out alone: ok %d, %.2f at %d,%d %dx%d, column %d, icon %d,%d\n",
                    name.c_str(), CleanSettings::kBuyPetName, found.ok ? 1 : 0, static_cast<double>(found.score),
                    found.at.x, found.at.y, found.at.w, found.at.h, found.cell.column, found.cell.x, found.cell.y);
        check(found.ok && found.cell.column == 1 && found.cell.x == 463 + bag::kShopMpX && found.cell.y > 285 &&
                  found.cell.y < 320,
              name + ": a cell with Thịtbò alone, as the game's letters draw it: found whole, its icon box clicked");
        // The end of a longer name -- khô put back, left of it: not whole.
        Frame longer = alone;
        for (int y = 285; y <= 301; ++y)
            for (int x = 583; x <= 599; ++x) {
                std::uint8_t r = 0, g = 0, bl = 0;
                f.get(x, y, r, g, bl);
                put_px(longer, x - 32, y, r, g, bl);
            }
        const bag::ShopItemSeen tail = bag::shop_item(longer, b, CleanSettings::kBuyPetName);
        check(!tail.ok, name + ": Thịtbò at the end of a longer name (letters left of it): refused (" + tail.why + ")");
    }

    // ---- the round on the pretend game ----
    {
        ShopRun probe(0, 0);
        if (!probe.ok) {
            skip("the item catalog is not installed here\n");
            return;
        }
    }
    const auto clicked_tabs = [](const ShopRun& s) {
        std::string t;
        for (const int c : s.p.game.shop_tab_clicked) t += std::to_string(c);
        return t;
    };
    {
        ShopRun s(0, 0);
        s.pet(1);
        s.run();
        check(s.r.ran && s.r.trouble.empty() && s.r.shop_bought == 1 && s.r.shop_bought_pet == 1 && s.pets() == 1 &&
                  s.p.game.buys == std::vector<int>{ 3 } && !s.r.pet_ended && !s.r.shop_full && !s.r.shop_message,
              "one slot: Linh Thú chosen, Thịt bò bought once, nothing else -- " + s.r.trouble + " / " + s.p.last());
        check(clicked_tabs(s) == "30" && s.p.game.typed == "100" && s.p.game.digit_keys == 2 &&
                  s.p.game.double_clicks == 1 && s.p.game.mua_clicks == 0 && !s.p.game.shop_on &&
                  s.p.game.esc_presses == 1 && s.p.game.thoat_clicks == 0,
              "one click on Linh Thú, a double-click on its icon, 00 after the game's 1 read back as 100, Xác Nhận; "
              "never Mua; Dược Phẩm clicked again before the close (the shop left as it was found, the 2026.11.24 "
              "review); the shop shut with its one ESC");
    }
    {
        ShopRun s(0, 0);
        s.pet(3);
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought_pet == 3 && s.pets() == 3 && clicked_tabs(s) == "30",
              "three slots: three bought, the tab clicked once (and Dược Phẩm again before the close) -- " +
                  s.r.trouble);
    }
    {
        // The first click on the tab let go by the game: a second, a pixel
        // aside, chooses it.
        ShopRun s(0, 0);
        s.pet(1);
        s.p.game.shop_tab_clicks_lost = 1;
        s.p.react_ms = 1500;
        s.run();
        check(s.r.trouble.empty() && s.pets() == 1 && clicked_tabs(s) == "330",
              "a click on the tab the game let go: clicked once more, then bought -- " + s.r.trouble);
    }
    {
        // A tab that never changes: two clicks, then nothing bought, the
        // picture kept and said -- tried again later by the window's round.
        ShopRun s(0, 0);
        s.pet(1);
        s.p.game.shop_tabs_stuck = true;
        s.p.react_ms = 1500;
        const std::string dir = temp_path("pet_tab_debug");
        CreateDirectoryA(dir.c_str(), nullptr);
        for (const std::string& n : files_in(dir, "*.png")) remove_file(dir + "\\" + n);
        s.p.debug_dir = dir;
        s.run();
        check(s.pets() == 0 && s.p.game.double_clicks == 0 && s.p.game.shop_tab_clicks == 2 &&
                  s.r.trouble.find("ngăn Linh Thú") != std::string::npos && kept_as(dir, "pet_tab") &&
                  !s.p.game.shop_on,
              "Linh Thú never chosen: two clicks, no double-click, pet_tab kept, the shop shut -- " + s.r.trouble);
    }
    {
        // Only a longer name beginning with it: never clicked.
        ShopRun s(0, 0);
        Pretend::PetCell other;
        other.name = "Thịtbònướng";
        s.pet(1, { other });
        const std::string dir = temp_path("pet_item_debug");
        CreateDirectoryA(dir.c_str(), nullptr);
        for (const std::string& n : files_in(dir, "*.png")) remove_file(dir + "\\" + n);
        s.p.debug_dir = dir;
        s.run();
        check(s.pets() == 0 && s.other_pet_items() == 0 && s.p.game.double_clicks == 0 &&
                  s.r.trouble.find(CleanSettings::kBuyPetName) != std::string::npos &&
                  (kept_as(dir, "pet_item_place") || kept_as(dir, "pet_no_item")) && !s.p.game.shop_on,
              "Linh Thú with Thịtbònướng alone: not clicked, the picture kept, the shop shut -- " + s.r.trouble);
    }
    {
        // Both: the pet's own food, the other passed over.
        ShopRun s(0, 0);
        Pretend::PetCell other, food;
        other.name = "Thịtbònướng";
        other.column = 0;
        food.row = 1;
        s.pet(1, { other, food });
        s.run();
        check(s.r.trouble.empty() && s.pets() == 1 && s.other_pet_items() == 0,
              "Thịtbònướng in the left column, Thịtbò below on the right: Thịtbò bought -- " + s.r.trouble);
    }
    {
        // The game's no -- its bag full, a message GoBot has no picture of --
        // in place of the box: closed, the pet's buying over, no trouble.
        ShopRun s(0, 0);
        s.pet(2);
        s.p.game.pet_room = 0;
        s.run();
        check(s.pets() == 0 && s.r.pet_ended && s.r.trouble.empty() && !s.r.shop_full && !s.r.shop_message &&
                  s.p.game.lone_clicks == 1 && !s.p.game.lone_up && s.p.game.double_clicks == 1 && !s.p.game.shop_on,
              "the pet's bag full: its message closed with its Xác Nhận, nothing more tried, no trouble, the shop "
              "shut -- " + s.r.trouble);
        ShopRun t(0, 0);
        t.pet(3);
        t.p.game.pet_room = 1;
        t.run();
        check(t.pets() == 1 && t.r.shop_bought_pet == 1 && t.r.pet_ended && t.r.trouble.empty(),
              "room for one: one bought, the second brings the message -- closed, over -- " + t.r.trouble);
    }
    {
        // A round with the potions too: the pet's first on Linh Thú, then
        // Dược Phẩm clicked again and the potions bought there.
        ShopRun s(1, 1);
        s.pet(1);
        s.run();
        check(s.r.trouble.empty() && s.p.game.buys == std::vector<int>({ 3, 0, 1 }) && clicked_tabs(s) == "30" &&
                  s.r.shop_bought_pet == 1 && s.r.shop_bought_hp == 1 && s.r.shop_bought_mp == 1,
              "the pet's food, then Dược Phẩm again for Thịt bò khô and Rượu lá tre -- " + s.r.trouble);
    }
    // ---- the 2026.11.24 review: a shop that opens again on the tab last used ----
    {
        // Opened on Linh Thú: taken by its Mua | Thoát and its strip, Dược
        // Phẩm clicked, the HP potion bought there.
        ShopRun s(1, 0);
        s.pet(0);
        s.p.game.shop_keeps_tab = true;
        s.p.game.shop_tab = bag::kShopTabPet;
        s.run();
        check(s.r.trouble.empty() && s.p.game.buys == std::vector<int>{ 0 } && clicked_tabs(s) == "0" &&
                  s.r.shop_bought_hp == 1 && !s.p.game.shop_on && !s.r.stuck,
              "the shop come up on Linh Thú: seen open all the same, Dược Phẩm clicked, Thịt bò khô bought, the shop "
              "shut -- " + s.r.trouble + " / " + s.p.last());
    }
    {
        // A pet round whose close fails on Linh Thú -- Dược Phẩm never chosen
        // again, the ESC and Thoát taken by nothing: the hold kept, as any
        // shop left up (its Mua | Thoát seen; the potions' row is not drawn).
        ShopRun s(0, 0);
        s.pet(1);
        s.p.react_ms = 1500;
        s.p.during = [&s](BagCleaner&) {
            if (s.pets() >= 1) {
                s.p.game.shop_tabs_stuck = true;
                s.p.game.esc_closes_shop = false;
                s.p.game.thoat_dead = true;
            }
        };
        s.run();
        check(s.pets() == 1 && s.p.game.shop_on && s.p.game.shop_tab == bag::kShopTabPet && s.r.stuck &&
                  !s.r.trouble.empty(),
              "a shop left up on Linh Thú: the hold kept, said -- " + s.r.trouble);
    }
    {
        // A notice after Xác Nhận on the pet's food: counted as bought and the
        // round goes on, as for the potions -- never "the pet's bag full".
        ShopRun s(0, 0);
        s.pet(3);
        s.p.game.lone_after_confirm = true;
        s.p.during = [&s](BagCleaner&) {
            if (s.pets() >= 1) s.p.game.lone_after_confirm = false;
        };
        s.run();
        std::printf("  [info] pet notice: %d bought, %d unsure, ended %d, notices %d -- %s\n", s.r.shop_bought_pet,
                    s.r.shop_unsure_pet, s.r.pet_ended ? 1 : 0, s.r.shop_notices, s.r.trouble.c_str());
        check(!s.r.pet_ended && s.r.shop_bought_pet + s.r.shop_unsure_pet == 3,
              "a notice after the first of 3 pet purchases: 3 counted, the pet's buying not ended -- " + s.r.trouble);
    }
    {
        // Its own switch: a plan of the pet's food alone with buy_pet off
        // presses nothing of the shop.
        ShopRun s(0, 0);
        s.pet(1);
        s.p.settings.buy_pet = false;
        s.run();
        check(s.pets() == 0 && s.p.game.opener_clicks == 0 && s.r.trouble.find("buy_pet=false") != std::string::npos,
              "buy_pet off: the shop not even opened -- " + s.r.trouble);
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
        check(s.load(path) && !s.buy_potions && s.buy_every_minutes == 60 && s.buy_hp_slots == 3 &&
                  s.buy_mp_slots == 3 && s.buy_opener == "other/8950604" && !s.buy_after_trash,
              "a file without them: off, an hour, 3 slots of each, the portrait of the item, not after each pass");
        // "Mua ngay sau khi dọn rác" (2026.11.13b): read, and off again when
        // the line is taken out.
        write_all(path, "[Trash]\nbuy_after_trash=true\n");
        check(s.load(path) && s.buy_after_trash, "buy_after_trash=true read");
        write_all(path, "[Trash]\nenabled=false\n");
        check(s.load(path) && !s.buy_after_trash, "buy_after_trash off again with its line taken out");
        write_all(path, "[Trash]\nbuy_potions=true\nbuy_every_minutes=30\nbuy_hp_slots=2\nbuy_mp_slots=0\n"
                        "buy_opener=other/1\nbuy_frame_score=0.8\n");
        check(s.load(path) && s.buy_potions && s.buy_every_minutes == 30 && s.buy_hp_slots == 2 &&
                  s.buy_mp_slots == 0 && s.buy_opener == "other/1" && s.buy_frame_score > 0.79f &&
                  s.buy_frame_score < 0.81f,
              "read back: 2 slots of HP, MP left alone");
        write_all(path, "[Trash]\nenabled=false\n");
        check(s.load(path) && !s.buy_potions && s.buy_hp_slots == 3 && s.buy_mp_slots == 3,
              "and off, 3 and 3, again when the lines are taken out: never left as an earlier reading had them");
        // No cap of GoBot's own since 2026.11.10 (the owner: "Không giới
        // hạn"): 50 and 999 read as written, only past the box's bound
        // brought in.
        write_all(path, "[Trash]\nbuy_every_minutes=1\nbuy_hp_slots=5000\nbuy_mp_slots=-4\nbuy_frame_score=0.1\n");
        check(s.load(path) && s.buy_every_minutes == CleanSettings::kBuyEveryLeast &&
                  s.buy_hp_slots == CleanSettings::kBuySlotsPerRunMost && s.buy_hp_slots == 999 &&
                  s.buy_mp_slots == 0 && s.buy_frame_score == CleanSettings::kBuyFrameLeast,
              "numbers out of range brought in: slots 0 to 999");
        write_all(path, "[Trash]\nbuy_hp_slots=50\nbuy_mp_slots=999\n");
        check(s.load(path) && s.buy_hp_slots == 50 && s.buy_mp_slots == 999,
              "50 and 999 set on the card read as they are: no cap of 20 any more");
        write_all(path, "[Trash]\nbuy_hp_slots=21\nbuy_mp_slots=1000\n");
        check(s.load(path) && s.buy_hp_slots == 21 && s.buy_mp_slots == CleanSettings::kBuySlotsPerRunMost,
              "21 reads 21; 1000, one over the box's bound, reads 999");
        // A file from before 2026.11.09 -- the owner's: buy_*_to, the slots it
        // bought up to, stand for the new keys until the card saves them; the
        // new key, once there, wins; buy_*_below is passed over, but for 0,
        // which left that potion alone.
        write_all(path, "[Trash]\nbuy_hp_below=7\nbuy_hp_to=10\nbuy_mp_below=7\nbuy_mp_to=10\n");
        check(s.load(path) && s.buy_hp_slots == 10 && s.buy_mp_slots == 10,
              "a file with buy_hp_to=10 and buy_mp_to=10 and no new keys reads 10 and 10");
        write_all(path, "[Trash]\nbuy_hp_to=10\nbuy_hp_slots=4\nbuy_mp_to=10\nbuy_mp_slots=0\n");
        check(s.load(path) && s.buy_hp_slots == 4 && s.buy_mp_slots == 0,
              "with both, the new key wins: 4, and 0 for MP");
        // The old key keeps 2026.11.09's cap of 20: an upgraded file never
        // buys more on its own; only a number set on the card goes over it.
        write_all(path, "[Trash]\nbuy_hp_to=60\nbuy_mp_below=2\n");
        check(s.load(path) && s.buy_hp_slots == CleanSettings::kBuySlotsFromOldMost && s.buy_hp_slots == 20 &&
                  s.buy_mp_slots == 3,
              "an old buy_hp_to of 60 read as 20; buy_mp_below alone is passed over: 3");
        write_all(path, "[Trash]\nbuy_hp_to=60\nbuy_hp_slots=60\nbuy_mp_to=999\nbuy_mp_slots=21\n");
        check(s.load(path) && s.buy_hp_slots == 60 && s.buy_mp_slots == 21,
              "with the new keys there, 60 and 21 as set on the card -- over the old key's 20");
        write_all(path, "[Trash]\nbuy_hp_to=-3\nbuy_mp_to=20\n");
        check(s.load(path) && s.buy_hp_slots == 0 && s.buy_mp_slots == 20,
              "an old buy_hp_to under 0 reads 0; 20 reads 20");
        write_all(path, "[Trash]\nbuy_hp_below=0\nbuy_hp_to=6\nbuy_mp_below=3\nbuy_mp_to=6\n");
        check(s.load(path) && s.buy_hp_slots == 0 && s.buy_mp_slots == 6,
              "an old buy_hp_below=0 left HP alone: 0, not its buy_hp_to of 6; MP's below of 3 reads its 6");
        write_all(path, "[Trash]\nbuy_hp_below=0\nbuy_hp_to=6\nbuy_hp_slots=2\nbuy_mp_below=-1\nbuy_mp_to=6\n");
        check(s.load(path) && s.buy_hp_slots == 2 && s.buy_mp_slots == 0,
              "the new key still wins over a below of 0: 2; a below under 0 leaves MP alone as well: 0");
        write_all(path, "[Trash]\nbuy_hp_below=\nbuy_hp_to=6\nbuy_mp_below=x\nbuy_mp_to=5\n");
        check(s.load(path) && s.buy_hp_slots == 6 && s.buy_mp_slots == 5,
              "an empty or unreadable below is no opt-out: the old to, 6 and 5");
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
        check(shipped.buy_hp_slots == 3 && shipped.buy_mp_slots == 3 &&
                  text.find("\nbuy_hp_slots=3\n") != std::string::npos &&
                  text.find("\nbuy_mp_slots=3\n") != std::string::npos && text.find("\nbuy_hp_below=") == std::string::npos &&
                  text.find("\nbuy_hp_to=") == std::string::npos && text.find("\nbuy_mp_below=") == std::string::npos &&
                  text.find("\nbuy_mp_to=") == std::string::npos,
              "and buys 3 + 3 each time, with no below/to keys left: buy_hp_slots=3, buy_mp_slots=3");
#endif
    }
    // The full-bag message is told from every other message by its words:
    // the wordless one-button picture, the check-in reward's notice and a
    // live charm message all pass its frame (0.85) and none its words.
    {
        std::vector<std::pair<std::string, Frame>> others;
        others.emplace_back("grabbed/dialog_ok", *k.store.get("grabbed/dialog_ok"));
        Frame notice;
        if (TemplateStore::load_one("grabbed/login_popup1", exe_relative("configs/templates.ini"), notice))
            others.emplace_back("grabbed/login_popup1", notice);
        for (const auto& [name, pic] : others) {
            Frame f = scene(1024, 768, 60);
            blit(f, pic, 413, 343);
            const bag::ShopFullLook how = bag::shop_full_look(f, k.store);
            check(!bag::shop_full_up(f, k.store) && how.words < 0.90f,
                  name + " is not the full-bag message: whole " + fixed3(how.whole) + ", words " + fixed3(how.words));
        }
        // A 197x80 cut of the live charm message of 2026-11-08 22:13:51 at
        // 413,343 (trash_debug 20261108_221351_3): handed over beside the
        // charms' cuts when it is here -- no name, no chat in it.
        Frame live = scene(1024, 768, 40);
        if (!put_back(live, std::string(kLiveBuild) + "handoff\\shop\\testdata", "shop_charm_notice")) {
            skip("no cut of the live charm message here (handoff\\shop\\testdata\\shop_charm_notice_*)\n");
        } else {
            const bag::ShopFullLook how = bag::shop_full_look(live, k.store);
            std::printf("  [info] the live charm message against the full-bag picture: whole %s, words %s\n",
                        fixed3(how.whole).c_str(), fixed3(how.words).c_str());
            check(!bag::shop_full_up(live, k.store) && how.words < 0.90f && bag::lone_ok(live, k.store,
                                                                                     ScreenRect{ 0, 0, 1024, 768 }),
                  "the live charm message: a lone Xác Nhận, never the full-bag message -- words " + fixed3(how.words));
        }
        // The live buy question (2026-11-01 16:34:10): never a lone message.
        Frame q;
        const std::string shot =
            std::string(kLiveBuild) + "bin\\Release\\trash_debug\\20261101_163410_0_shop_question.png";
        if (!load_png(shot, q) || !q.valid()) {
            skip("no live frame of the buy question here (trash_debug\\20261101_163410_0_shop_question.png)\n");
        } else {
            check(!bag::lone_ok(q, k.store, ScreenRect{ 0, 0, q.width, q.height }) && !bag::shop_full_up(q, k.store) &&
                      !bag::find_dialogs(q, k.store).empty(),
                  "the live buy question: a dialog of two buttons, never a lone message, never the full bag");
        }
    }
}

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
        return s.r.shop_bought == 0 && s.p.game.right_clicks == 0 && s.p.game.opener_clicks == 0 &&
               s.p.game.digit_keys == 0;
    };

    // ---- the click on Khác (select_tab): the one tab the shop clicks, since
    // nothing is counted (2026.11.09).
    {
        // The pointer left on the tab it clicked spoils the read, as the game
        // lights a control under it: moved off after the click, the tab is
        // read at once.
        ShopRun s(1, 0);
        s.p.game.hover_unreads = true;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 1 && !s.r.tab_unsure && s.p.game.tab_clicks == 1 &&
                  s.p.game.use_tab_clicks == 0 && s.p.game.unread_seen == 0 && s.p.game.moves >= 1,
              "the pointer on a tab spoils its read: moved off after the click, Khác read at once and one bought -- " +
                  s.r.trouble + " / " + s.p.last());
        ShopRun t(1, 0);
        t.p.game.hover_unreads = true;
        t.p.game.no_move = true;
        t.run();
        check(t.r.trouble.find("không chuyển được sang ngăn Khác") != std::string::npos && t.r.tab_unsure &&
                  nothing_done(t) && t.p.game.tab_clicks == 1 &&
                  t.p.game.unread_seen >= 10 + BagCleaner::kTabRelooks,
              "and with no way to move it, every look after the click reads no tab: looked at " +
                  std::to_string(BagCleaner::kTabRelooks) + " more times, then a stop -- nothing right-clicked (" +
                  std::to_string(t.p.game.unread_seen) + " looks with no tab) -- " + t.r.trouble);
    }
    {
        // No tab read for the ten short looks after the click and three more:
        // looked at again, and the shop goes on.
        ShopRun s(1, 0);
        int from = -1;
        s.p.during = [&](BagCleaner&) {
            if (from < 0 && s.p.game.tab_clicks > 0) from = s.p.game.looks + 1;
        };
        s.p.game.unread = [&](int n) { return from >= 0 && n >= from && n < from + 13; };
        LogLines log;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 1 && !s.r.tab_unsure && s.p.game.tab_clicks == 1 &&
                  s.p.game.unread_seen == 13 &&
                  log.has("(Khác) read at shop_other_tab after 4 more looks -- the first read -1"),
              "-1 on the 10 looks after the click and 3 more: looked at again, Khác read on the 4th, one click, one "
              "bought -- " + s.r.trouble + " / " + log.line_with("read at shop_other_tab"));
    }
    {
        // No tab read on any look: one click, eight more looks, a stop -- and
        // its picture kept once a GoBot start, its numbers in the log.
        BagCleaner::forget_said_once();
        LogLines log;
        bool stopped[2] = { false, false };
        int kept[2] = { 0, 0 };
        for (int round = 0; round < 2; ++round) {
            ShopRun s(1, 0);
            s.p.game.unread = [](int) { return true; };
            s.p.debug_dir = dir;
            s.run();
            stopped[round] = s.r.trouble.find("không chuyển được sang ngăn Khác") != std::string::npos &&
                             s.r.tab_unsure && nothing_done(s) && s.p.game.tab_clicks == 1;
            kept[round] = pictures("shop_other_tab");
        }
        check(stopped[0] && stopped[1],
              "-1 on every look: Khác clicked once, looked at again, then a stop -- nothing bought or right-clicked");
        check(kept[0] == 1 && kept[1] == 1,
              "one picture kept the first time, none the second: once a GoBot start, so the newest few dozen are not "
              "pushed out (" + std::to_string(kept[0]) + ", " + std::to_string(kept[1]) + ")");
        const std::string line = log.line_with("tab not read as 3 (Khác) at shop_other_tab -- read -1, strip at 300,300");
        const std::vector<int> m = means_in(line);
        check(m.size() == kBagTabs && *std::min_element(m.begin(), m.end()) >= 0,
              "the log says the tab wanted, the tab read, where the strip is, how alike, and every tab's mean -- " + line);
    }
    {
        // Tiêu Dùng read for sure on every look: Khác clicked again, as the
        // game may have let the first click go -- kShopTabClicks in all since
        // 2026.11.14b -- then a stop. A first click let go: the second takes.
        ShopRun s(1, 0);
        s.p.game.tabs_stuck = true;
        s.run();
        check(s.r.trouble.find("không chuyển được sang ngăn Khác") != std::string::npos && s.r.tab_unsure &&
                  nothing_done(s) && s.p.game.tab_clicks == BagCleaner::kShopTabClicks && s.p.game.unread_seen == 0,
              "Tiêu Dùng read for sure on every look: Khác clicked " + std::to_string(BagCleaner::kShopTabClicks) +
                  " times in all, then a stop (" + std::to_string(s.p.game.tab_clicks) + " clicks) -- " + s.r.trouble);
        ShopRun t(1, 0);
        t.p.game.tab_clicks_lost = 1;
        t.run();
        check(t.r.trouble.empty() && t.r.shop_bought == 1 && t.p.game.tab_clicks == 2 && !t.r.tab_unsure,
              "the first click let go by the game: the second takes, and it buys as ever -- " + t.r.trouble);
    }

    // ---- the look for the item on Khác (shop_open): the bag on Khác from the
    // start, so no tab is clicked; the moment its first look reads no tab is
    // found by trying, so that the test does not hang on how many looks come
    // before it.
    int page_look = -1;
    for (int k = 2; k <= 16 && page_look < 0; ++k) {
        ShopRun s(1, 0);
        s.p.game.cur_tab = kOtherTab;
        s.p.game.unread = [k](int n) { return n >= k && n < k + 3; };
        LogLines log;
        s.run();
        if (!log.has("(Khác) read at shop_other after 3 more looks")) continue;
        page_look = k;
        check(s.r.trouble.empty() && s.r.shop_bought == 1 && !s.r.tab_unsure && s.p.game.moves >= 3 &&
                  s.p.game.tab_clicks == 0,
              "-1 on three looks for the item: looked at again, the pointer moved off each time, and the item found "
              "all the same, one bought (from look " + std::to_string(k) + ") -- " + s.r.trouble);
    }
    check(page_look > 0, "(a moment in the look for the item with three looks that read no tab: found)");
    if (page_look > 0) {
        BagCleaner::forget_said_once();
        ShopRun s(1, 0);
        s.p.game.cur_tab = kOtherTab;
        s.p.game.unread = [page_look](int n) { return n >= page_look; };
        s.p.debug_dir = dir;
        s.run();
        check(s.r.trouble.find("không chắc túi đồ đang ở ngăn Khác") != std::string::npos && s.r.tab_unsure &&
                  nothing_done(s) && s.p.game.moves >= BagCleaner::kTabRelooks && pictures("shop_other") == 1,
              "no tab read for good from there: " + std::to_string(BagCleaner::kTabRelooks) +
                  " more looks, then a stop, its picture kept -- nothing right-clicked -- " + s.r.trouble);
        // Tiêu Dùng read there for sure: stopped at once, nothing looked at again.
        ShopRun o(1, 0);
        o.p.game.cur_tab = kOtherTab;
        o.p.during = [&](BagCleaner&) {
            if (o.p.game.looks + 1 == page_look) o.p.game.cur_tab = kUseTab;
        };
        o.run();
        check(o.r.trouble.find("không chắc túi đồ đang ở ngăn Khác") != std::string::npos && o.r.tab_unsure &&
                  nothing_done(o) && o.p.game.moves == 0 && o.p.game.unread_seen == 0,
              "Tiêu Dùng read for sure in the look for the item: stopped at once, the pointer not moved, nothing "
              "looked at again or right-clicked -- " + o.r.trouble);
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
    const std::string shop =
        slice(gui, "const auto run_shop = [&](bool forced, bool dry, bool after_pass) {", "while (!trash_stop_) {");
    const std::size_t hold = shop.find("take_hold(\"trash cleaning\")");
    const std::size_t let_go = hold == npos ? npos : shop.find("input_.release_all();", hold);
    const std::size_t quiet = shop.find("const DWORD quiet = last_input();");
    const std::size_t asked = shop.find("touched = !trash_stop_ && !lost && said == QuietSaid::Touched;");
    const std::size_t ran = shop.find("r = cleaner.run();");
    check(hold != npos && let_go != npos && quiet != npos && asked != npos && ran != npos && hold < let_go &&
              let_go < quiet && quiet < asked && asked < ran &&
              has(shop, "for (int slept = 0; slept < kShopQuietMs && !trash_stop_ && !lost; slept += 50) {") &&
              has(gui, "constexpr int kShopQuietMs = 1000;"),
          "gui_app.cpp: run_shop takes the hold, lets the keys go, and runs only after a second with nobody at the "
          "mouse or keyboard");
    // A button's run looks for that second a few times under the one hold, the
    // hold and HP watched as the run watches them; a timed run once -- and one
    // that rides a pass not at all (2026.11.11), the hold and HP looked at once.
    check(has(shop, "const int tries = forced || dry ? kShopQuietTries : after_pass ? 0 : 1;") &&
              has(shop, "if (after_pass) lost = !still_ours();") &&
              has(gui, "constexpr int kShopQuietTries = 5;") && has(shop, "lost = !still_ours();") &&
              has(shop, "cleaner.set_guard(still_ours);") &&
              has(shop, "else if (lost && hp_under())"),
          "gui_app.cpp: \"Mua ngay\" and \"Thử mua\" look for the quiet second up to 5 times, the hold and HP "
          "watched; a timed run once");
    // 2026.11.08: no longer a flat 30 s, which held every key off for a second
    // every 31 s on 2026-11-08 -- 30 s, then 2, 5 and 10 minutes, one spoiled
    // look after another, never past the shop's own interval.
    check(has(shop, "not_now(\"Chờ: bạn đang dùng chuột hoặc bàn phím, mua bình sau.\", touched_ms);") &&
              has(shop, "touched_ms = shop_quiet.next_ms(static_cast<std::uint64_t>(settings.buy_every_minutes) *") &&
              !has(gui, "kShopTouchedMs") &&
              count_of(shop, "if (!held) end_hold(\"trash cleaning\");") == 2 && count_of(shop, "take_hold(") == 1,
          "and when somebody was, it lets the hold go as it was and asks again after the back-off's wait, saying "
          "why");
    {
        QuietBackOff waits;
        std::vector<std::uint64_t> got;
        for (int i = 0; i < 5; ++i) got.push_back(waits.next_ms(30ull * 60'000));
        check(got == std::vector<std::uint64_t>{ 30'000, 120'000, 300'000, 600'000, 600'000 },
              "... the back-off at a 30-minute interval: 30 s, 2 min, 5 min, 10 min, 10 min");
    }
    check(!has(gui, "line += \" Dừng: \" + r.trouble") &&
              count_of(gui, "line += (line.empty() ? \"\" : \" \") + std::string(\"Dừng: \") + r.trouble + \".\";") == 2,
          "gui_app.cpp: the shop's line no longer begins \"Trash:  Dừng\" with two spaces");

    // The charms: the tab line instead of "no charm there", and a minute --
    // ten from 2026.11.08 to 2026.11.24 (the owner: "Kiểm tra các phần khác
    // xem có phần nào không làm được thì dừng không, cần thay cơ chế này bằng
    // 1 cơ chế khác": no long pause in place of a stop).
    const std::string job = slice(gui, "const auto run_charms = [&](bool forced) {", "// ---- the potion shop ----");
    const std::size_t tab_branch = job.find("} else if (tab_stop) {");
    const std::size_t none = job.find("Không thấy bùa nào trong ngăn Tiêu Dùng của túi đồ.");
    check(has(job, "const bool tab_stop = r.tab_unsure && r.trouble.empty();") && tab_branch != npos && none != npos &&
              tab_branch < none && has(job, "không chắc túi đồ đang ở ngăn Tiêu Dùng, nên dừng dùng bùa"),
          "gui_app.cpp: a charm sweep stopped on the strip says the tab was not sure, before \"Không thấy bùa nào\" "
          "can be said");
    check(has(job, "if (!forced && r.tab_unsure) charm_check_ms = now_ms() + kCharmTabRetryMs;") &&
              has(gui, "constexpr std::uint64_t kCharmTabRetryMs = 60'000;") &&
              !has(gui, "kCharmTabRetryMs = 600'000") && has(job, "(forced ? \".\" : \"; thử lại sau 1 phút.\")") &&
              !has(job, "thử lại sau 10 phút") && has(job, "r.trouble.empty() && !r.tab_unsure)") &&
              CharmClock::kRetryMinutes == 10,
          "and the charms try again a minute after it (2026.11.24; ten minutes before), saying so -- the kinds due "
          "not written down as not found; a charm not found still waits its ten minutes (CharmClock::kRetryMinutes)");

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
    // The shop counts nothing (2026.11.09): Khác is the one tab it reads, and
    // its look for the item reads the tab first, as the count did.
    const std::string opening = body(cpp, "bool BagCleaner::shop_open(");
    const std::size_t tab_check = opening.find("if (v.selected_tab != kOtherTab) {");
    const std::size_t relook = opening.find("v.found && v.selected_tab < 0 && unread < kTabRelooks");
    const std::size_t flagged = opening.find("res.tab_unsure = true;");
    const std::size_t judged = opening.find("trash_->judge(");
    check(!has(cpp, "bool BagCleaner::shop_count(") && !has(cpp, "kShopRounds") && tab_check != npos &&
              relook != npos && flagged != npos && judged != npos && tab_check < relook && relook < flagged &&
              flagged < judged && has(opening, "tab_unsure(f, v, kOtherTab, \"shop_other\");"),
          "bag.cpp: no shop_count; shop_open looks again on a look with no tab read, flags a strip read otherwise, "
          "and judges a slot only after Khác is read");
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

/// The "Sự kiện và điểm danh" board (checkin_board.hpp): told by its title,
/// what a search of a whole frame costs, nothing taken for it that is not it,
/// and every bag job's first look -- before any key, the bag's included.
void test_checkin_board() {
    section("Bảng Sự kiện và điểm danh: told by its title, and no bag job presses anything under it");
    if (skip_without_kit()) return;
    if (!kit().board_ok) {
        skip("the check-in board's title (%s) is not installed here\n", kCheckinBoardTitleFile);
        return;
    }
    const Kit& k = kit();
    const Frame& title = k.board_title;
    check(title.width == 280 && title.height == 30, "the title is 280x30 -- " + std::to_string(title.width) + "x" +
                                                        std::to_string(title.height));
    {
        // Where the board centred on a 1024x768 window has it: 298,3 into the
        // board, the board at 73,84.
        Frame f = scene(1024, 768, 60);
        blit(f, title, 73 + 298, 84 + 3);
        ScreenRect at;
        float score = 0.0f;
        const auto from = std::chrono::steady_clock::now();
        constexpr int kTimes = 5;
        bool all = true;
        for (int i = 0; i < kTimes; ++i) all = all && checkin_board_title(f, k.store, &at, &score);
        const auto ms = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - from)
                            .count() / 1000.0 / kTimes;
        std::printf("  [info] a title search on 1024x768: %.1f ms\n", ms);
        check(all && at.x == 371 && at.y == 87 && score >= 0.99f,
              "the title found where it is, at " + std::to_string(at.x) + "," + std::to_string(at.y) + " (" +
                  fixed3(score) + ")");
        check(ms < 200.0, "a search of the whole frame is cheap enough to make before every click: " +
                              std::to_string(static_cast<int>(ms)) + " ms");
        // Looked for again where it was: a 296x46 search, found a few pixels
        // off, and not where it is not.
        const ScreenRect close_by{ 371 + 5, 87 - 4, 280, 30 };
        const ScreenRect away{ 100, 500, 280, 30 };
        ScreenRect again;
        check(checkin_board_title(f, k.store, &again, nullptr, &close_by) && again.x == 371 && again.y == 87 &&
                  !checkin_board_title(f, k.store, nullptr, nullptr, &away),
              "looked for again within 8 pixels of where it was: found there, and nowhere else");
        // Said once a GoBot start: a second search of a whole frame says nothing.
        LogLines log;
        checkin_board_title(f, k.store);
        check(!log.has("Checkin: a title search on"), "the cost said once a GoBot start, not on every look");
    }
    {
        // Nothing else is the board: a blank screen, the title with other
        // words, the close buttons alone, the old attendance dialog's X.
        Frame other = title;
        write_words(other, 3, ScreenRect{ 60, 5, 160, 20 });
        Frame words = scene(1024, 768, 60);
        blit(words, other, 371, 87);
        Frame xs = scene(1024, 768, 60);
        blit(xs, k.close_x, 600, 100);
        blit(xs, *k.store.get(bag::kCloseButtons[1]), 300, 100);
        Frame old_x;
        if (TemplateStore::load_one("grabbed/check_in_close", exe_relative("configs/templates.ini"), old_x))
            blit(xs, old_x, 450, 150);
        float score = 0.0f;
        checkin_board_title(words, k.store, nullptr, &score);
        check(!checkin_board_title(scene(1024, 768, 60), k.store) && !checkin_board_title(words, k.store) &&
                  !checkin_board_title(xs, k.store) && !checkin_board_title(Frame{}, k.store) &&
                  !checkin_board_title(words, TemplateStore{}),
              "not the board: a blank screen, the title with other words, the close buttons alone, no frame, no "
              "picture");
    }
    {
        // By its name in templates.ini, which the shipped file gives it.
        TemplateStore named;
        named.put(kCheckinBoardTitle, title);
        Frame f = scene(1024, 768, 60);
        blit(f, title, 200, 150);
        check(checkin_board_picture(named) != nullptr && checkin_board_title(f, named),
              "a store with the short name alone finds it");
#ifdef GOBOT_SOURCE_DIR
        const std::string names = read_all(std::string(GOBOT_SOURCE_DIR) + "/configs/templates.ini");
        check(names.find("\ncheckin_board_title            = grabbed/auto_login_success_indicator") !=
                  std::string::npos,
              "configs/templates.ini names it: checkin_board_title = grabbed/auto_login_success_indicator");
#endif
    }
    {
        // The owner's own saved frames, read where they are and never copied:
        // none shows the board, and none is taken for it.
        const std::string dir = std::string(kLiveBuild) + "bin\\Release\\trash_debug";
        const auto names = files_in(dir, "*.png");
        if (names.empty()) {
            skip("no saved frames of the live game here (trash_debug)\n");
        } else {
            int looked = 0, taken = 0;
            std::string which;
            for (const std::string& name : names) {
                if (looked >= 40) break;
                Frame f;
                if (!load_png(dir + "\\" + name, f) || !f.valid()) continue;
                ++looked;
                if (checkin_board_title(f, k.store)) {
                    ++taken;
                    which += " " + name;
                }
            }
            check(looked > 0 && taken == 0, "the owner's saved frames: none taken for the board (" +
                                                std::to_string(looked) + " looked at)" + which);
        }
    }
    // Every bag job's first look, the board up: nothing pressed at all.
    {
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 3, TrashList::Action::Delete);
        p.game.bag_shut = p.game.bag_keyed = true;
        p.game.board_on = true;
        const auto r = p.run();
        check(r.board_up && !r.ran && r.trouble.empty() && p.game.keys == 0 && p.game.drags == 0 &&
                  p.game.clicks == 0 && p.game.right_clicks == 0 && !p.game.items[0].gone() && p.game.bag_shut &&
                  p.says("bảng Sự kiện và điểm danh"),
              "the trash pass with the board up: no bag key, nothing dragged or clicked, said -- " + p.last());
        Pass open;
        make_bag(open.game);
        open.item(0, 7, 3, TrashList::Action::Delete);
        open.game.board_on = true;
        const auto ro = open.run();
        check(ro.board_up && open.game.drags == 0 && open.game.clicks == 0 && open.game.keys == 0 &&
                  !open.game.items[0].gone(),
              "nor with the bag open already");
    }
    {
        CharmRun c;
        c.p.game.bag_shut = c.p.game.bag_keyed = true;
        c.p.game.board_on = true;
        c.run();
        check(c.r.board_up && c.p.game.keys == 0 && c.p.game.clicks == 0 && c.p.game.right_clicks == 0 &&
                  c.r.charm_tries.empty() && c.r.trouble.empty(),
              "a charm run with the board up: no key -- ESC least of all -- no right-click, no charm tried");
    }
#ifdef GOBOT_SOURCE_DIR
    {
        // In the source: the board looked for in run_pass before the bag key
        // is ever pressed, and before a charm's own message is closed.
        std::string cpp = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/bag.cpp");
        cpp.erase(std::remove(cpp.begin(), cpp.end(), '\r'), cpp.end());
        const std::size_t from = cpp.find("BagCleaner::Result BagCleaner::run_pass() {");
        const std::size_t board = cpp.find("if (board_up(f, res)) return res;", from);
        const std::size_t charms = cpp.find("if (mode_ == Mode::CharmsOnly && !charms_start(f, v, res)) {", from);
        const std::size_t key = cpp.find("press(settings_.bag_key);", from);
        const std::size_t full = cpp.find("if (bag::shop_full_up(f, store_, &at)) {", from);
        check(from != std::string::npos && board != std::string::npos && charms != std::string::npos &&
                  key != std::string::npos && full != std::string::npos && board < full && full < charms &&
                  charms < key,
              "bag.cpp: run_pass looks for the board, then the shop's full-bag message, before the charms' own "
              "message and before press(settings_.bag_key)");
    }
#endif
}

/// Dọn rác's speed (2026.11.10, the owner's "Thao tác dọn rác chậm"): no pass
/// with no tab to look through; Tiêu Dùng alone when every item lies there;
/// the last look of an item gone judged on; quicker looks where the game
/// answers at once; a line for every item and every pass -- and every check
/// as it was: the box's whole blink, two looks kDoneApartMs apart, back_out's
/// and the close's polls.
void test_pass_speed() {
    section("Dọn rác: no tab, no pass; Tiêu Dùng alone for a list of spend/ icons; quicker looks, the same checks "
            "(2026.11.10)");
    {
        // Which tabs a pass looks through.
        const auto icon = [](const std::string& id) {
            TrashList::Item it;
            it.id = id;
            return it;
        };
        TrashList spend;
        spend.items = { icon("spend/hp/8820121"), icon("spend/mana/8810101") };
        CleanSettings s;                // Trang Bị 1, Trang Bị 2, Tiêu Dùng and Khác ticked, as shipped
        const bag::PassTabs narrowed = bag::pass_tabs(s, spend);
        check(narrowed.spend_only && narrowed.count == 1 && narrowed.tabs[kUseTab] && !narrowed.tabs[0] &&
                  !narrowed.tabs[1] && !narrowed.tabs[3] && std::string(narrowed.none_said()).empty(),
              "every item a spend/ icon: Tiêu Dùng alone of the four tabs shipped ticked");
        TrashList cut = spend;
        cut.items.push_back(TrashList::Item{});
        cut.items.back().file = "configs\\trash\\delete\\item.png";
        TrashList other = spend;
        other.items.push_back(icon("other/8950604"));
        other.items.back().via = "spend/hp/8820121";      // brought along for its drawing: still another group's
        TrashList none;
        const bag::PassTabs with_cut = bag::pass_tabs(s, cut), with_other = bag::pass_tabs(s, other),
                            empty = bag::pass_tabs(s, none);
        check(!with_cut.spend_only && with_cut.count == 4 && !with_other.spend_only && with_other.count == 4 &&
                  !empty.spend_only && empty.count == 4,
              "a picture cut from the bag, an icon of another group (one come along by its drawing too), or no list: "
              "every ticked tab, as before");
        CleanSettings off = s;
        for (bool& t : off.tabs) t = false;
        CleanSettings no_use = s;
        no_use.tabs[kUseTab] = false;
        const bag::PassTabs nothing = bag::pass_tabs(off, cut), no_use_tab = bag::pass_tabs(no_use, spend);
        check(nothing.count == 0 && !nothing.ticked_any && std::string(nothing.none_said()) == bag::kNoTabVi &&
                  no_use_tab.count == 0 && no_use_tab.ticked_any &&
                  std::string(no_use_tab.none_said()) == bag::kNoUseTabVi &&
                  std::string(bag::kNoTabVi) == "Chưa chọn ngăn nào ở 'Tìm trong:', nên không xoá được gì",
              "no tab ticked, or every item one of Tiêu Dùng's with that tab not ticked: nothing to look through, "
              "and said why");
    }
    if (skip_without_kit()) return;
    {
        // No tab ticked: not a look, not a key, not a click.
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        for (bool& t : p.settings.tabs) t = false;
        const auto r = p.run();
        const bool untouched = p.game.looks == 0 && p.game.keys == 0 && p.game.clicks == 0 && p.game.drags == 0;
        check(!r.ran && r.trouble == bag::kNoTabVi && untouched && !p.game.items[0].gone(),
              "no tab ticked: the pass stops before its first look, nothing pressed, not even the bag key -- " +
                  r.trouble);
        // Nor with only spend/ icons listed and Tiêu Dùng not ticked.
        Pass q;
        make_bag(q.game);
        q.item(0, 7, 2, TrashList::Action::Delete, false, true, { "spend/hp/8820121" });
        q.list.items.back().id = "spend/hp/8820121";
        q.settings.tabs[0] = q.settings.tabs[1] = q.settings.tabs[3] = true;
        q.settings.tabs[kUseTab] = false;
        const auto rq = q.run();
        check(!rq.ran && rq.trouble == bag::kNoUseTabVi && q.game.looks == 0 && q.game.keys == 0 &&
                  q.game.clicks == 0,
              "every item a spend/ icon and Tiêu Dùng not ticked: the same -- " + rq.trouble);
        // An item an earlier pass may have left in the bin is given back all
        // the same: that is what a pass with no tab is still for.
        Pass g;
        make_bag(g.game);
        g.item(0, 7, 2, TrashList::Action::Delete);
        for (bool& t : g.settings.tabs) t = false;
        g.left_held = true;
        g.game.up = Pretend::Up::InBin;
        g.game.about = 0;
        const auto rg = g.run();
        check(rg.ran && !rg.stuck && g.game.up == Pretend::Up::None && g.game.bin_confirms == 0 &&
                  g.game.drags == 0 && g.game.items[0].count == 2,
              "with an item maybe left in the bin (set_left_held): the pass gives it back, Hủy Bỏ, and looks through "
              "no tab -- " + g.game.events);
    }
    {
        // Every item on the list a spend/ icon: Tiêu Dùng alone. The same
        // drawing laid on Trang Bị 2 -- which no real bag does -- shows which
        // tabs were looked through.
        struct Way {
            const char* what;
            std::string id, file;
            bool narrowed;
        };
        const Way ways[] = {
            { "every item a spend/ icon", "spend/hp/8820121", "", true },
            { "one picture cut from the bag", "", "configs\\trash\\delete\\item.png", false },
            { "one icon of another group", "other/8950604", "", false },
        };
        for (const Way& w : ways) {
            LogLines log;
            Pass p;
            make_bag(p.game);
            p.game.tabs_work = true;
            p.settings.tabs[1] = p.settings.tabs[3] = true;     // Trang Bị 2, Tiêu Dùng, Khác
            p.item(0, 7, 2, TrashList::Action::Delete, false, true, { "spend/hp/8820121" });
            p.list.items.back().id = w.id;
            p.list.items.back().file = w.file;
            p.item(3, 7, 3, TrashList::Action::Delete, false, false);
            p.game.items.back().tab = 1;
            const auto r = p.run();
            const bool said = log.has("Trash: every item on the list is a spend/ icon -- only Tiêu Dùng looked "
                                      "through, not Trang Bị 2, Khác");
            if (w.narrowed)
                check(r.ran && r.deleted == 1 && p.game.items[0].gone() && p.game.items[1].count == 3 &&
                          p.game.tab_clicks == 0 && said,
                      std::string(w.what) + ": Tiêu Dùng alone -- no tab clicked, Trang Bị 2's slot not looked at, "
                      "and the log says which were passed over -- " + p.game.events);
            else
                check(r.ran && r.deleted == 2 && p.game.items[0].gone() && p.game.items[1].gone() &&
                          p.game.tab_clicks == 3 && p.game.use_tab_clicks == 1 && !said,
                      std::string(w.what) + ": every ticked tab, as before -- " + std::to_string(p.game.tab_clicks) +
                          " tabs clicked, " + p.game.events);
        }
    }
    {
        // Two stacks deleted, on the pretend clock (a look 40 ms): each step
        // timed in a line of its own, and the pass's line; a look every 80 ms
        // at the bin and for its dialog, the item's last look judged on by
        // the next round -- and the box watched a whole blink, 840 ms on six
        // looks that agree, and the item seen gone twice 150 ms apart and more,
        // as before.
        LogLines log;
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.item(1, 7, 3, TrashList::Action::Delete, false, false);
        const auto r = p.run();
        const std::string first = log.line_with("Trash: item at Tiêu Dùng, hàng 1 ô 1");
        const std::string second = log.line_with("Trash: item at Tiêu Dùng, hàng 1 ô 2");
        const std::string steps = " deleted in 1620 ms -- drag 0 ms, in the bin after 120 ms (1 look), its dialog "
                                  "120 ms after the click, the box read on 6 looks in 840 ms, closed 190 ms after Xác "
                                  "Nhận (1 question), then 310 ms till the item was seen gone, last look 40 ms";
        check(r.deleted == 2 && p.game.items[0].gone() && p.game.items[1].gone() &&
                  first.find(steps) != std::string::npos && second.find(steps) != std::string::npos,
              "a line for each item: 1620 ms each where 2026.11.09 took 1870 with its round's look -- " + first);
        check(log.has("Trash: pass steps -- bag open already; Tiêu Dùng: tab 0 ms, top 40 ms, 1 page (scrolling 0 "
                      "ms), 2 items in 3240 ms; 3360 ms in all"),
              "and one for the pass -- " + log.line_with("Trash: pass steps"));
        check(p.game.looks == 27,
              "27 looks in all, where 2026.11.09 took 29: the second item and the end judged on the last look of "
              "the one before -- " + std::to_string(p.game.looks));
    }
    {
        // The bag shut: its key, and a look every 80 ms until it is open; and
        // at the end its key, and a look every 80 ms until it is seen gone
        // (2026.11.14b): the look before the key, and one 120 ms after it.
        LogLines log;
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.game.bag_keyed = true;
        p.game.bag_shut = true;
        const auto r = p.run();
        check(r.deleted == 1 && p.game.bag_shut && p.game.bag_keys == 2 &&
                  log.has("Trash: pass steps -- bag opened in 120 ms (1 look)") && log.has("; bag shut in 160 ms;") &&
                  !log.has("bag still seen"),
              "the bag opened by its key seen on the first look, 120 ms on, and shut again, seen gone -- " +
                  log.line_with("Trash: pass steps"));
    }
#ifdef GOBOT_SOURCE_DIR
    {
        const std::string cpp = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/bag.cpp");
        const std::string im = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/input_manager.cpp");
        const std::string gui = read_all(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/gui_app.cpp");
        const std::string view = read_all(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/simple_view.cpp");
        if (cpp.empty() || im.empty() || gui.empty() || view.empty()) {
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
        const auto body = [&](const std::string& text, const std::string& head) {
            const std::size_t a = text.find(head);
            const std::size_t b = a == npos ? npos : text.find("\n}", a);
            return a == npos || b == npos ? std::string() : text.substr(a, b + 2 - a);
        };
        // What stays as it was: the box's whole blink and its three looks that
        // agree, two looks kDoneApartMs apart, back_out's and the close's polls.
        const std::string box = body(cpp, "int BagCleaner::box_holds(");
        const std::string wait = body(cpp, "bag::Dialog BagCleaner::wait_for_dialog(");
        const std::string back = body(cpp, "bool BagCleaner::back_out() {");
        const std::string ask = body(cpp, "int BagCleaner::answer(");
        check(has(cpp, "constexpr int kBoxWatchMs = 1200;") && has(cpp, "constexpr int kBoxLookMs = 100;") &&
                  has(cpp, "constexpr int kBoxEnoughMs = 750;") && has(cpp, "constexpr int kDoneApartMs = 150;") &&
                  has(box, "if (!wait_ms(kBoxLookMs)) return said(CountReader::kUnreadable);") &&
                  has(box, "if (reads >= 3 && now() - start >= std::chrono::milliseconds(kBoxEnoughMs)) return "
                           "said(value);") &&
                  has(box, "return said(reads >= 2 ? value : CountReader::kUnreadable);") &&
                  has(wait, "} else if (now() - gone_at >= std::chrono::milliseconds(kDoneApartMs)) {"),
              "bag.cpp: the box watched a whole blink, three looks agreeing over 750 ms; the item gone on two looks "
              "150 ms apart -- as before");
        // Since 2026.11.15d (the owner's 30 s for the game to react) those polls
        // are bounded by the clock, at react_ms(): back_out's two -- its Hủy Bỏ
        // each its share, the bin's click the whole -- through a stop as long
        // as ever (kCancelLookMs), and the close's after Xác Nhận, still every
        // 150 ms, asking the stop and the guards on every look.
        check(count_of(back, "pause_ms(150);") == 2 && count_of(back, "for (int i = 0; i < 12") == 0 &&
                  has(back, "while (ms_between(cancelled_at, now()) < (react_stop() ? kCancelLookMs : share)) {") &&
                  has(back, "while (!up && ms_between(bin_at, now()) < (react_stop() ? kCancelLookMs : react_ms_)) {") &&
                  has(ask, "while (!closed && ms_between(confirmed_at, now()) < react_ms_) {") &&
                  has(ask, "if (!react_gap(150)) return kFailed;") && !has(ask, "for (int i = 0; i < 12"),
              "bag.cpp: back_out's polls and the close's after Xác Nhận every 150 ms, by the clock up to react_ms() "
              "(2026.11.15d)");
        // What is quicker: the looks at the bag's key, the bin and its dialog;
        // another's dialog still two looks 150 ms apart; the drag's steps at the
        // pace; one search for dialogs a frame.
        const std::string one = body(cpp, "bool BagCleaner::trash_one(");
        const std::string pass = body(cpp, "BagCleaner::Result BagCleaner::run_pass() {");
        const std::string drag = body(im, "bool InputManager::drag_on_game(");
        // Since 2026.11.15d each of those waits goes on for react_ms() -- the
        // owner's 30 s -- by the clock (kBagOpenWaitMs and kInBinWaitMs gone),
        // still a look every 80 ms, the stop and the guards asked on each.
        check(has(cpp, "constexpr int kQuickLookMs = 80;") && !has(cpp, "constexpr int kBagOpenWaitMs") &&
                  !has(cpp, "constexpr int kInBinWaitMs") &&
                  has(pass, "while (!v.found && ms_between(key_at, now()) < react_ms_) {") &&
                  has(pass, "if (!react_gap(kQuickLookMs)) return res;") &&
                  has(one, "while (!waiting && ms_between(moved_at, now()) < react_ms_) {") &&
                  has(one, "wait_for_dialog(react_ms_, foreign, &shot)") &&
                  has(ask, "wait_for_dialog(900, foreign, nullptr, done)") &&
                  has(wait, "while (ms_between(from, now()) < ms) {") && has(wait, "if (!react_gap(pause)) break;") &&
                  has(wait, "} else if (ms_between(other_at, now()) >= kDoneApartMs) {") &&
                  has(wait, "foreign = other_again;") && !has(wait, "waited += 150"),
              "bag.cpp: a look every 80 ms after the bag's key, at the bin and for its dialogs, each wait react_ms() "
              "by the clock (the second question's 900 ms kept); another's dialog on two looks 150 ms apart");
        check(has(drag, "jitter(paced(18, 12), paced(30, 20))") && !has(drag, "jitter(18, 30)"),
              "input_manager.cpp: the drag's ten steps at press_ms's pace, 12 to 20 ms at the least");
        check(count_of(one, "bag::find_dialogs(g, store_)") == 2 && count_of(ask, "bag::find_dialogs(f, store_)") == 1 &&
                  has(ask, "if (up.empty() &&"),
              "bag.cpp: one search for dialogs a frame -- the last look of an item, and the refusal's test on the "
              "look that saw the dialog closed");
        check(has(pass, "trash_one(f, v, static_cast<int>(s), count, &handed_f, &handed_v, &handed);") &&
                  has(pass, "if (fresh && !clean(v, up)) {") && has(one, "*seen_clean = true;") &&
                  has(one, "if (dialogs.empty() && after != nullptr && after_view != nullptr && seen_clean != nullptr)"),
              "bag.cpp: the next round judges on an item's last look only when it was gone with nothing up");
        // The worker: no hold and no key with nothing to look through -- on the
        // pass's own list, built just before (the charms build one of theirs
        // earlier in the file).
        const std::size_t guard = gui.find("if (pass_tabs.count == 0 && !left_held) {");
        const std::size_t built =
            guard == npos ? npos : gui.rfind("list.build(exe_relative(\"configs/trash\"), settings, catalog);", guard);
        const std::size_t left = guard == npos ? npos : gui.find("cleaner.set_left_held(left_held);", guard);
        const std::size_t hold = guard == npos ? npos : gui.find("take_hold(\"trash cleaning\")", guard);
        check(built != npos && guard != npos && left != npos && hold != npos && guard - built < 1000 && guard < left &&
                  left < hold && has(gui, "const bool left_held = held || trash_left_held_;") &&
                  has(view, "bag::pass_tabs(simple_clean_, simple_trash_)") &&
                  has(view, "coloured(kWait, (std::string(looked.none_said()) + \".\").c_str());"),
              "gui_app.cpp: no tab to look through and nothing left in the bin -- no hold, no bag key; the card says "
              "why");
    }
#endif
}

void test_key_window() {
    section("The hold's key window in a bag job (2026.11.10): open only on a clean look of a pass or a shop round, "
            "shut before every input of the cleaner's own, never in a charm run nor a purchase's money stretch");
    if (skip_without_kit()) return;
    // What every run here must hold to, whatever else it did.
    const auto kept_shut = [](const Pretend& g, const std::string& what) {
        check(g.acts_with_keys == 0 && g.opens_unclean == 0 && g.drags_from_open == 0 && g.drags_soon == 0 &&
                  g.rights_from_open == 0 && g.rights_soon == 0 && !g.keys_open,
              what + ": no click, drag, right-click, double-click or key with the window open (" +
                  std::to_string(g.acts_with_keys) + "), never opened with something up or out (" +
                  std::to_string(g.opens_unclean) + "), nothing dragged or right-clicked from a look taken with it "
                  "open or under 500 ms after a potion (" + std::to_string(g.drags_from_open + g.rights_from_open) +
                  ", " + std::to_string(g.drags_soon + g.rights_soon) + "), and shut at the end");
    };

    // ---- a pass: two stacks, a potion going down each time it opens ----
    {
        Pass plain;
        make_bag(plain.game);
        plain.item(0, 7, 2, TrashList::Action::Delete);
        plain.item(1, 7, 3, TrashList::Action::Delete, false, false);
        plain.run();
        LogLines log;
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.item(1, 7, 3, TrashList::Action::Delete, false, false);
        p.key_window = true;
        p.game.window_potions = true;
        const auto r = p.run();
        check(r.deleted == 2 && r.trouble.empty() && p.game.items[0].gone() && p.game.items[1].gone() &&
                  p.game.events == plain.game.events,
              "both stacks deleted as without the window -- " + p.game.events + " / " + r.trouble);
        check(p.game.keys_opens >= 1 && p.game.keys_shuts >= 1,
              "the window opened on the first clean look, and shut before the drag -- " +
                  std::to_string(p.game.keys_opens) + " opened, " + std::to_string(p.game.keys_shuts) + " shuts");
        kept_shut(p.game, "a pass");
        // The slot found on that look was looked at again, the window shut and
        // the potion's half second over: one look more, and time.
        check(p.game.looks > plain.game.looks && p.game.looks <= plain.game.looks + 3,
              "one look more for the slot judged again with the window shut -- " + std::to_string(p.game.looks) +
                  " looks, " + std::to_string(plain.game.looks) + " without the window");
        check(log.has("Trash: pass steps --"), "the pass's line as ever");
        // The review's T1: the look the window opens on is searched for
        // dialogs once -- its clean test's search, the window's checks going
        // by it -- not three times more.
        int judged = 0;
        std::uint64_t most = 0;
        for (const auto& [n, bought] : p.game.opened_on) {
            const auto at = static_cast<std::size_t>(n);
            if (n < 1 || at >= p.game.counts_at_look.size()) continue;
            ++judged;
            most = std::max(most, p.game.counts_at_look[at].dialogs - p.game.counts_at_look[at - 1].dialogs);
        }
        check(judged >= 1 && most == 1,
              "the look the window opened on: one search for dialogs, the round's own -- at most " +
                  std::to_string(most) + " on " + std::to_string(judged) + " such looks");
    }
    {
        // Two tabs and a fragment to register on Tiêu Dùng: the tab's click,
        // the use sweep's right-click, the book's X -- each with the window shut,
        // the sweep's looks after the potion's half second.
        Pass p;
        make_bag(p.game);
        p.game.tabs_work = true;
        p.settings.tabs[1] = true;
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.item(3, 7, 3, TrashList::Action::Delete, false, false);
        p.game.items.back().tab = 1;
        p.item(5, 9, 1, TrashList::Action::Use);
        p.key_window = true;
        p.game.window_potions = true;
        const auto r = p.run();
        check(r.deleted == 2 && r.used == 1 && r.trouble.empty() && p.game.tab_clicks >= 1,
              "two tabs and a fragment: both stacks deleted, the fragment registered -- " + p.game.seq + " / " +
                  r.trouble);
        kept_shut(p.game, "two tabs and the use sweep");
    }
    {
        // A look only: the window opens on its looks, and nothing is pressed.
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.key_window = true;
        p.game.window_potions = true;
        const auto r = p.run(true);
        check(r.ran && r.matched == 1 && p.game.drags == 0 && p.game.keys_opens >= 1 && p.game.acts_with_keys == 0,
              "Thử tìm: the window open while it looks, nothing dragged -- " + std::to_string(p.game.keys_opens));
    }
    {
        // A key not seen up when shut: that input is not made, the pass stops
        // and says why.
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.key_window = true;
        p.game.shut_refusals = 1;
        const auto r = p.run();
        check(p.game.drags == 0 && p.game.items[0].count == 2 && r.deleted == 0 &&
                  r.trouble.find("chưa nhả kịp") != std::string::npos && p.game.acts_with_keys == 0,
              "a key still down at the shut: nothing dragged, the pass stopped -- " + r.trouble);
    }
    {
        // F12 in the middle: once the cleaner is cancelled the window is never
        // opened again.
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.item(1, 7, 3, TrashList::Action::Delete, false, false);
        p.key_window = true;
        int opens_at_stop = -1;
        p.during = [&](BagCleaner& c) {
            if (opens_at_stop < 0 && p.game.keys_opens > 0) {
                c.cancel();
                opens_at_stop = p.game.keys_opens;
            }
        };
        p.run();
        check(opens_at_stop >= 1 && p.game.keys_opens == opens_at_stop && p.game.acts_with_keys == 0 &&
                  !p.game.keys_open,
              "a stop: no opening after it, and shut -- " + std::to_string(p.game.keys_opens) + " opened");
    }

    // ---- a charm run: never ----
    {
        CharmRun c;
        c.p.key_window = true;
        c.p.game.window_potions = true;
        c.run();
        check(c.r.charms_used == 1 && c.p.game.keys_opens == 0 && c.p.game.acts_with_keys == 0,
              "Dùng bùa: the window never opened, the charm used as ever -- " + std::to_string(c.p.game.keys_opens));
    }

    // ---- a shop round: between purchases only ----
    if (!kit().shop_ok) {
        skip("the shop's pictures are not installed here\n");
        return;
    }
    {
        ShopRun s(2, 1);
        if (!s.ok) {
            skip("the item catalog is not installed here\n");
            return;
        }
        s.p.key_window = true;
        s.p.game.window_potions = true;
        s.run();
        check(s.r.shop_bought == 3 && s.r.trouble.empty() && s.p.game.wrong_confirms == 0 &&
                  s.p.game.digit_keys == 6,
              "2 + 1 bought, 00 typed each time, as without the window -- " + s.r.trouble);
        check(s.p.game.keys_opens >= 3 && s.p.game.opens_in_money == 0 && s.p.game.looks_open_unclean == 0,
              "the window open while Khác is looked through and after each purchase's tail, never from a double-click "
              "until a second after its Xác Nhận, nor with the box or a message up -- " +
                  std::to_string(s.p.game.keys_opens) + " opened, " + std::to_string(s.p.game.opens_in_money) +
                  " in a purchase");
        kept_shut(s.p.game, "a shop round");
        // The keys' time between purchases is not shrunk to nothing: the look
        // each next purchase is judged on is taken with the window open (the
        // owner's "cả khi mua HP và MP cũng cần ra skill").
        check(s.p.game.looks_open_bought >= 2,
              "a look with the window open before each of the next two purchases -- " +
                  std::to_string(s.p.game.looks_open_bought));
        // The review's T1: the tail's last look, the window opened on it, is
        // searched for dialogs and the full-bag message once, by the tail.
        int judged = 0;
        std::uint64_t most_d = 0, most_f = 0;
        for (const auto& [n, bought] : s.p.game.opened_on) {
            const auto at = static_cast<std::size_t>(n);
            if (bought == 0 || n < 1 || at >= s.p.game.counts_at_look.size()) continue;
            ++judged;
            const bag::SearchCounts& a = s.p.game.counts_at_look[at - 1];
            const bag::SearchCounts& b = s.p.game.counts_at_look[at];
            most_d = std::max(most_d, b.dialogs - a.dialogs);
            most_f = std::max(most_f, b.full - a.full);
        }
        check(judged >= 2 && most_d == 1 && most_f == 1,
              "after a purchase, the look the window opened on: one search for dialogs and one for the full message, "
              "the tail's own -- at most " + std::to_string(most_d) + " and " + std::to_string(most_f) + " on " +
                  std::to_string(judged) + " such looks");
    }
    {
        // The review's K1: keys let through while the look before a
        // double-click is judged, and something come up as the last of them is
        // waited up -- the check-in board, or a message: the double-click goes
        // by a look taken after the shut, never by the one judged with the
        // window open.
        for (const int what : { 0, 1 }) {
            if (what == 0 && !kit().board_ok) continue;
            ShopRun s(2, 1);
            s.p.key_window = true;
            s.p.game.window_potions = true;
            bool once = false;
            s.p.game.on_shut = [&] {
                Pretend& g = s.p.game;
                if (once || g.buys.size() != 1 || g.qty_up) return;
                once = true;
                g.time.t += std::chrono::milliseconds(300);     // a key held 300 ms, waited up
                if (what == 0) g.board_on = true;
                else g.lone_up = true;
            };
            s.run();
            const bool ended = what == 0 ? s.r.board_up : s.r.shop_message;
            check(once && ended && s.r.shop_bought == 1 && s.p.game.double_clicks == 1 &&
                      s.p.game.doubles_under == 0 && s.p.game.buys.size() == 1,
                  std::string(what == 0 ? "the board" : "a message") +
                      " come up while the keys were waited up before the second double-click: seen on a look after "
                      "the shut, no double-click under it, the round ended -- " +
                      std::to_string(s.p.game.double_clicks) + " double-clicks, " +
                      std::to_string(s.p.game.doubles_under) + " under it / " + s.r.trouble);
            kept_shut(s.p.game, what == 0 ? "the board before the double-click" : "a message before the double-click");
        }
    }
    {
        // The review's T1: a dialog come up in a purchase's tail -- what the
        // tail found on its last look is what the window goes by, and that
        // look had a dialog: never opened on it, and the next purchase stops.
        ShopRun s(2, 1);
        s.p.key_window = true;
        s.p.game.window_potions = true;
        int after_buy = 0;
        s.p.during = [&](BagCleaner&) {
            Pretend& g = s.p.game;
            if (g.buys.size() == 1 && !g.qty_up && ++after_buy == 2) g.show(kit().other, Pretend::Up::Question);
        };
        s.run();
        int opened_after = 0;
        for (const auto& [n, bought] : s.p.game.opened_on) opened_after += bought > 0 ? 1 : 0;
        check(after_buy >= 2 && s.r.shop_bought == 1 && opened_after == 0 && s.p.game.double_clicks == 1 &&
                  s.r.trouble.find("hộp thoại") != std::string::npos,
              "a dialog up on the tail's looks: the window not opened after the purchase, the next one stopped on it "
              "-- " + std::to_string(opened_after) + " opened after it / " + s.r.trouble);
        kept_shut(s.p.game, "a dialog in the tail");
    }
    {
        // The bag full instead of the box, and a full message come late, in
        // the tail: never a key into either, never an ESC with it up.
        ShopRun now(3, 3, true, 1);
        now.p.key_window = true;
        now.p.game.window_potions = true;
        now.run();
        check(now.r.shop_full && now.r.shop_bought == 1 && now.p.game.esc_with_full == 0 &&
                  now.p.game.keys_with_full == 0 && now.p.game.looks_open_unclean == 0 &&
                  now.p.game.opens_in_money == 0,
              "full at the second purchase: no key with it up -- " + now.r.trouble);
        kept_shut(now.p.game, "the bag full");
        ShopRun late(3, 3, true, 1);
        late.p.game.full_late_ms = 600;
        late.p.key_window = true;
        late.p.game.window_potions = true;
        late.run();
        check(late.r.shop_full && late.p.game.looks_open_unclean == 0 && late.p.game.opens_in_money == 0 &&
                  late.p.game.esc_with_full == 0,
              "the full message 600 ms after Xác Nhận: the window still shut through it -- " + late.r.trouble);
        kept_shut(late.p.game, "the bag full late");
    }
    {
        // A trial: as far as each box, the window open only between them.
        ShopRun t(1, 1);
        t.p.key_window = true;
        t.run(true);
        check(t.p.game.digit_keys == 0 && t.p.game.cancels == 2 && t.p.game.opens_in_money == 0 &&
                  t.p.game.looks_open_unclean == 0,
              "Thử mua: both boxes answered Hủy Bỏ, the window never open with one up -- " + t.r.trouble);
        kept_shut(t.p.game, "Thử mua");
    }
}

/// HP and MP during a pass and a shop round (2026.11.11, the owner: "Mua luôn,
/// bơm xen"): an HP or MP potion due goes in a break the cleaner keeps the key
/// window open for -- between two items, as the shop is seen, before each
/// double-click -- never in a purchase's money stretch nor with anything up; a
/// round goes on with HP under the HP potion's mark and is stopped only under
/// half of it, put off and never trouble; and the full-bag message is closed
/// with HP low. GoBot's window's side -- the gates, the retries -- is read
/// from its source in pass_keys_test.cpp.
void test_own_potions() {
    section("HP and MP with the macro not running (2026.11.11b, the owner: \"GoBot tự bấm bình\"): the run presses the "
            "potion itself between the items and before each double-click -- never in a money stretch nor with "
            "anything up -- and drags and double-clicks only from a look after it");
    if (skip_without_kit()) return;
    const auto gone_count = [](const Pretend& g) {
        return static_cast<int>(
            std::count_if(g.items.begin(), g.items.end(), [](const Pretend::Item& i) { return i.gone(); }));
    };
    // ---- a pass: HP due again as each item goes ----
    {
        Pass p;
        make_bag(p.game);
        for (const int s : { 0, 1, 2 }) p.item(s, 7, 2 + s, TrashList::Action::Delete, false, s == 0);
        p.own = true;
        int gone_seen = 0;
        p.during = [&](BagCleaner&) {
            const int gone = gone_count(p.game);
            if (gone > gone_seen) {
                gone_seen = gone;
                p.game.vitals_due = 1;
            }
        };
        const auto r = p.run();
        check(r.deleted == 3 && r.trouble.empty(), "three stacks deleted with the potion pressed between -- " + r.trouble);
        check(p.game.own_drunk >= 2 && p.game.own_unsafe == 0 && p.game.drags_soon == 0 && p.game.keys_opens == 0 &&
                  p.game.drags_from_open == 0,
              "the run pressed the HP potion itself between the items (" + std::to_string(p.game.own_drunk) +
                  "), never with anything up, no window opened, no drag under 500 ms after it");
    }
    // ---- none due: nothing pressed ----
    {
        Pass p;
        make_bag(p.game);
        for (const int s : { 0, 1 }) p.item(s, 7, 2 + s, TrashList::Action::Delete, false, s == 0);
        p.own = true;
        const auto r = p.run();
        check(r.deleted == 2 && p.game.own_drunk == 0 && p.game.own_unsafe == 0 && p.game.own_asked >= 1,
              "none due: asked, nothing pressed -- " + std::to_string(p.game.own_asked) + " asked");
    }
    if (!kit().shop_ok) {
        skip("the shop's pictures are not installed here\n");
        return;
    }
    // ---- a shop round at low HP: bought all, the potion between purchases ----
    {
        ShopRun s(3, 2);
        if (!s.ok) {
            skip("the item catalog is not installed here\n");
            return;
        }
        s.p.own = true;
        s.p.game.vitals_due = 1;
        std::size_t bought_seen = 0;
        s.p.during = [&](BagCleaner&) {
            if (s.p.game.buys.size() > bought_seen) {
                bought_seen = s.p.game.buys.size();
                s.p.game.vitals_due = 1;
            }
        };
        s.run();
        check(s.r.shop_bought == 5 && s.r.trouble.empty() && s.p.game.wrong_confirms == 0 &&
                  s.p.game.digit_keys == 10,
              "all five bought, 00 typed each time, with the potion pressed between -- " + s.r.trouble);
        check(s.p.game.own_drunk >= 5 && s.p.game.own_unsafe == 0 && s.p.game.keys_opens == 0 &&
                  s.p.game.doubles_under == 0,
              "the run pressed the HP potion itself as the shop was seen and before each double-click (" +
                  std::to_string(s.p.game.own_drunk) + "), never in a money stretch or with the box up");
    }
}

void test_potion_break() {
    section("HP and MP during a bag job (2026.11.11): an HP or MP potion due goes in a break between the job's own "
            "clicks -- never in a purchase's money stretch nor with the box up -- and, since 2026.11.11b, only the "
            "character down stops a round, put off, never trouble");
    if (skip_without_kit()) return;
    const auto kept_shut = [](const Pretend& g, const std::string& what) {
        check(g.acts_with_keys == 0 && g.opens_unclean == 0 && g.opens_in_money == 0 && g.drags_from_open == 0 &&
                  g.drags_soon == 0 && g.rights_from_open == 0 && g.rights_soon == 0 && g.vitals_unsafe == 0 &&
                  !g.keys_open,
              what + ": no input of the cleaner's with the window open, never opened with something up or in a "
                     "purchase, no potion in a money stretch or with the box up (" +
                  std::to_string(g.vitals_unsafe) + "), nothing dragged or right-clicked from an open look or under "
                  "500 ms after a potion, and shut at the end");
    };
    const auto gone_count = [](const Pretend& g) {
        return static_cast<int>(
            std::count_if(g.items.begin(), g.items.end(), [](const Pretend::Item& i) { return i.gone(); }));
    };

    // ---- a pass: HP and MP due again as each item goes ----
    {
        Pass plain;
        make_bag(plain.game);
        for (const int s : { 0, 1, 2 }) plain.item(s, 7, 2 + s, TrashList::Action::Delete, false, s == 0);
        plain.key_window = true;
        plain.run();
        Pass p;
        make_bag(p.game);
        for (const int s : { 0, 1, 2 }) p.item(s, 7, 2 + s, TrashList::Action::Delete, false, s == 0);
        p.key_window = true;
        int gone_seen = 0;
        p.during = [&](BagCleaner&) {
            const int gone = gone_count(p.game);
            if (gone > gone_seen) {
                gone_seen = gone;
                p.game.vitals_due = 2;      // HP and MP both under their marks again
            }
        };
        const auto r = p.run();
        check(r.deleted == 3 && r.trouble.empty() && plain.result.deleted == 3 && plain.game.vitals_drunk == 0,
              "three stacks deleted, as with no potion due -- " + r.trouble);
        check(p.game.vitals_drunk >= 4 && p.game.vitals_due == 0,
              "HP and MP drunk between the items, in a break on the look each deletion handed over -- " +
                  std::to_string(p.game.vitals_drunk) + " drunk");
        check(p.game.looks > plain.game.looks,
              "the slot after a break looked at again with the window shut, half a second after the potion -- " +
                  std::to_string(p.game.looks) + " looks, " + std::to_string(plain.game.looks) + " with none due");
        kept_shut(p.game, "a pass with potions due");
    }

    if (!kit().shop_ok) {
        skip("the shop's pictures are not installed here\n");
        return;
    }
    // GoBot's window's guard for a round whose HP potion is drunk between
    // purchases: since 2026.11.11b at any HP, stopped only with the character
    // down, under bag::kJobDownHpPct (bag::hp_gates) -- noting a stop for HP
    // as its hp_cut does.
    const bag::HpGates gate = bag::hp_gates(true, 80, 80);
    check(gate.drinks && gate.start == bag::kJobDownHpPct && gate.stop == bag::kJobDownHpPct,
          "the round's gates (2026.11.11b): at any HP, stopped only with the character down");

    // ---- HP under the potion's mark, over half of it: the round goes on ----
    {
        ShopRun s(3, 2);
        if (!s.ok) {
            skip("the item catalog is not installed here\n");
            return;
        }
        s.p.key_window = true;
        float hp = 60.0f;
        bool hp_cut = false;
        s.p.guard = [&] {
            if (hp < static_cast<float>(gate.stop)) {
                hp_cut = true;
                return false;
            }
            return true;
        };
        s.p.game.vitals_due = 2;
        std::size_t bought_seen = 0;
        s.p.during = [&](BagCleaner&) {
            if (s.p.game.buys.size() > bought_seen) {
                bought_seen = s.p.game.buys.size();
                s.p.game.vitals_due = 2;
            }
        };
        s.run();
        check(s.r.shop_bought == 5 && s.r.trouble.empty() && !hp_cut && s.p.game.wrong_confirms == 0 &&
                  s.p.game.digit_keys == 10,
              "HP 60% under the potion's 80%, over half of it: all five bought, 00 typed each time -- " + s.r.trouble);
        check(s.p.game.vitals_drunk == 10,
              "HP and MP drunk as the shop was seen and before each next double-click -- " +
                  std::to_string(s.p.game.vitals_drunk) + " drunk of 10");
        kept_shut(s.p.game, "a round with potions due");
    }
    // ---- none due: the window open from the look that sees the shop ----
    {
        ShopRun s(1, 0);
        s.p.key_window = true;
        s.p.game.window_potions = true;
        s.run();
        check(s.r.shop_bought == 1 && s.r.trouble.empty() && s.p.game.opens_first_buy >= 1 &&
                  s.p.game.vitals_drunk == 0 && s.p.game.double_clicks == 1 && s.p.game.doubles_under == 0,
              "no HP or MP due: the window open from the look that first sees the shop all the same, the first "
              "purchase's double-click by a look after the shut -- " +
                  std::to_string(s.p.game.opens_first_buy) + " opened before it");
        kept_shut(s.p.game, "the first purchase");
    }
    // ---- a message come up with the window open since the last tail ----
    {
        // The review's KEYS-1: a message only the notice's whole picture
        // matches -- its button drawn lit, so lone_ok misses it and the look
        // before the second double-click is judged clean -- come up after the
        // first purchase's tail opened the window, HP and MP due. keys_open
        // asks nothing of a window open already: the break asks keys_may_open
        // of that look, takes none, and leaves the window to the purchase's
        // own shut, the double-click going by the look after it. The player
        // closes the message as it shuts, and the list has moved a row by then.
        const Kit& k = kit();
        Frame lit = scene(1024, 768, 60);
        blit(lit, *k.store.get("grabbed/dialog_ok"), Pretend::kQX, Pretend::kQY);
        Pretend::light(lit, Pretend::lone_button());
        check(!bag::lone_ok(lit, k.store, ScreenRect{ 0, 0, lit.width, lit.height }) && bag::notice_up(lit, k.store),
              "a message with its button lit: lone_ok misses it, the notice's picture matches it");
        ShopRun s(2, 1);
        s.p.key_window = true;
        bool shown = false, closed = false;
        s.p.during = [&](BagCleaner&) {
            Pretend& g = s.p.game;
            if (shown || g.buys.size() != 1 || g.qty_up || !g.keys_open) return;
            shown = true;
            g.lone_up = g.lone_lit = true;
            g.vitals_due = 3;
        };
        s.p.game.on_shut = [&] {
            Pretend& g = s.p.game;
            if (!shown || closed) return;
            closed = true;
            g.lone_up = false;
            g.row_drop = 42;
        };
        s.run();
        check(shown && closed && s.r.shop_bought == 3 && s.r.trouble.empty() && !s.r.shop_message &&
                  s.p.game.wrong_confirms == 0 && s.p.game.wrong_item_doubles == 0,
              "a message lone_ok misses, up with the window open from the last tail: all three bought, the second "
              "double-click on the row where it has moved to -- " + s.r.trouble);
        check(s.p.game.vitals_unsafe == 1 && s.p.game.vitals_drunk == 3 && s.p.game.doubles_under == 0 &&
                  s.p.game.doubles_from_open == 0 && s.p.game.opens_unclean == 0,
              "no potion break over it -- one potion asked while that look was judged, the window open since the "
              "tail, and none held open for more (" + std::to_string(s.p.game.vitals_unsafe) + " with it up) -- the "
              "window shut by the purchase, every double-click by a look taken shut after it");
    }
    // ---- HP under half of it at the start: nothing bought, put off ----
    {
        ShopRun s(3, 2);
        s.p.key_window = true;
        bool hp_cut = false;
        s.p.guard = [&] {
            hp_cut = true;          // the character down
            return false;
        };
        s.p.game.vitals_due = 2;
        s.run();
        bag::ShopRoundSeen seen;
        seen.hp_cut = hp_cut;
        seen.trouble = !s.r.trouble.empty() || !s.r.ran;
        check(hp_cut && s.p.game.buys.empty() && s.p.game.opener_clicks == 0 && s.p.game.double_clicks == 0 &&
                  bag::shop_end(seen) == bag::ShopEnd::Paused,
              "the character down at the start: stopped before the item is right-clicked, nothing bought -- put "
              "off, not trouble -- " + s.r.trouble);
        kept_shut(s.p.game, "a round stopped for HP");
    }
    // ---- HP under half after two -- and back by the end ----
    {
        ShopRun s(3, 2);
        s.p.key_window = true;
        float hp = 70.0f;
        bool hp_cut = false;
        s.p.guard = [&] {
            // Down to 0% -- the character down -- as the third purchase comes,
            // back to 70% by the end (revived): what a look after the round
            // would see.
            if (s.p.game.buys.size() == 2 && !hp_cut) hp = 0.0f;
            if (hp < static_cast<float>(gate.stop)) {
                hp_cut = true;
                hp = 70.0f;
                return false;
            }
            return true;
        };
        s.run();
        bag::ShopRoundSeen seen;
        seen.hp_cut = hp_cut;
        seen.trouble = !s.r.trouble.empty() || !s.r.ran;
        bag::ShopRoundSeen looked_after = seen;     // HP looked at after the round, as 2026.11.10 did
        looked_after.hp_cut = hp < static_cast<float>(gate.stop);
        const bag::ShopEnd end = bag::shop_end(seen);
        const bag::ShopOwed owed =
            bag::shop_owed_after(bag::shop_new_plan(3, 2), s.r.shop_bought_hp, s.r.shop_bought_mp, s.r.shop_unsure_hp,
                                 s.r.shop_unsure_mp, end);
        check(hp_cut && s.r.shop_bought == 2 && s.r.trouble.find("nhân vật đã gục") != std::string::npos &&
                  !s.p.game.shop_on && s.p.game.wrong_confirms == 0,
              "the character down before the third purchase: stopped there, two bought, the shop shut -- " +
                  s.r.trouble);
        check(end == bag::ShopEnd::Paused && bag::shop_end(looked_after) == bag::ShopEnd::Trouble &&
                  owed.hp + owed.mp == 3 && owed.troubles == 0,
              "put off by the guard's own word, though HP is back by the end -- a look after the round would have "
              "called it trouble; the rest owed, no trouble counted");
        kept_shut(s.p.game, "a round stopped for HP after two");
    }
    // ---- the full-bag message with HP low ----
    {
        // Full at the second purchase, HP under the round's level from the
        // moment the message is up: closed all the same -- the hold still the
        // round's -- and the shop shut, no ESC or key with it up.
        ShopRun s(3, 3, true, 1);
        s.p.key_window = true;
        s.p.guard = [&s] { return !s.p.game.full_up; };
        s.p.hold_check = [] { return true; };
        s.run();
        check(s.r.shop_full && s.r.shop_bought == 1 && s.p.game.full_clicks == 1 && !s.p.game.full_up &&
                  s.p.game.esc_with_full == 0 && s.p.game.keys_with_full == 0 && !s.p.game.shop_on,
              "HP low with the full-bag message up: the message closed, then the shop -- " + s.r.trouble);
        kept_shut(s.p.game, "the bag full with HP low");
        // Another job's hold by then: not clicked, and still no ESC. The hold
        // lost as the message comes, as the guard says it -- in the GUI the
        // round's guard asks the hold too (still_ours) -- not from the start:
        // since 2026.11.15d the hold is asked on every look of every wait for
        // the game (react_stop), and a hold another job's from the start ends
        // the round at its first wait, before any message (the review of
        // 2026.11.15d).
        ShopRun t(3, 3, true, 1);
        t.p.guard = [&t] { return !t.p.game.full_up; };
        t.p.hold_check = [&t] { return !t.p.game.full_up; };
        t.run();
        check(t.r.shop_full && t.p.game.full_clicks == 0 && t.p.game.full_up && t.p.game.esc_with_full == 0 &&
                  t.p.game.keys_with_full == 0,
              "the hold no longer the round's: the message not clicked, no ESC or key with it up -- " + t.r.trouble);
    }
    // ---- another message in the box's place with HP low ----
    {
        // The review's HP-2: HP under the round's level from the double-click
        // on, and a message GoBot does not know in place of the quantity box:
        // closed with its own Xác Nhận all the same -- the hold still the
        // round's -- and the shop shut, nothing kept held.
        ShopRun s(2, 2);
        s.p.game.lone_instead = true;
        s.p.guard = [&s] { return s.p.game.double_clicks == 0; };
        s.p.hold_check = [] { return true; };
        s.run();
        check(s.r.shop_message && s.p.game.lone_clicks == 1 && !s.p.game.lone_up && !s.p.game.shop_on && !s.r.stuck &&
                  s.p.game.esc_with_lone == 0 && s.p.game.digit_keys == 0 && s.r.trouble.empty(),
              "HP low with a message in the box's place: the message closed, then the shop, nothing kept held -- " +
                  s.r.trouble);
        // Another job's hold by then -- from the double-click on, as the
        // guard's no (see the full-bag message's above): not clicked, no ESC
        // into it, left held.
        ShopRun t(2, 2);
        t.p.game.lone_instead = true;
        t.p.guard = [&t] { return t.p.game.double_clicks == 0; };
        t.p.hold_check = [&t] { return t.p.game.double_clicks == 0; };
        t.run();
        check(t.r.shop_message && t.p.game.lone_clicks == 0 && t.p.game.lone_up && t.p.game.esc_with_lone == 0 &&
                  t.p.game.esc_presses == 0 && t.r.stuck &&
                  t.r.trouble.find("bot không còn giữ phím (có việc khác đã lấy lại), nên dừng mua bình") !=
                      std::string::npos,
              "the hold no longer the round's: the message not clicked, no ESC into it, the shop left held -- " +
                  t.r.trouble);
    }
    // ---- a charm run: never ----
    {
        CharmRun c;
        c.p.key_window = true;
        c.p.game.vitals_due = 2;
        c.run();
        check(c.r.charms_used == 1 && c.p.game.keys_opens == 0 && c.p.game.vitals_drunk == 0 &&
                  c.p.game.acts_with_keys == 0,
              "Dùng bùa with HP and MP due: no break, the window never opened -- " +
                  std::to_string(c.p.game.keys_opens) + " opened");
    }
}

/// Home, End, PgUp, PgDn, Insert and Delete through the key window (2026.11.11,
/// the owner: "Home End PgUp PgDn Ins Del"): what the game does with them on
/// the bag or the shop is not known, so the cleaner's next input of its own,
/// and the look it goes by, wait kNavSettleMs after one came up; a shop's
/// double-click goes only by a look on which the potions' row itself matched
/// -- the list scrolled, the buttons alone found, it is looked at again and
/// then stopped, nothing clicked; and one landing late in the quantity box is
/// caught by the read-back. Which keys go is in pass_keys_test.cpp.
void test_nav_keys() {
    section("Home, End, PgUp, PgDn, Insert, Delete in a bag job (2026.11.11): the cleaner's next input a moment after "
            "one, a double-click only by a look with the potions' row on it, and one in the box answered Hủy Bỏ");
    if (skip_without_kit()) return;
    const auto rows_unmatched = [] {
        int n = 0;
        for (const LogLine& l : Logger::instance().ring())
            n += l.text.find("Trash: shop row not matched before the double-click on ") != std::string::npos ? 1 : 0;
        return n;
    };

    // ---- the settle: a pass, one of the six at every opening ----
    for (const bool hook : { true, false }) {
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.item(1, 7, 3, TrashList::Action::Delete, false, false);
        p.key_window = true;
        p.game.nav_on_open = true;
        p.game.nav_hook = hook;
        const auto r = p.run();
        if (hook) {
            check(r.deleted == 2 && r.trouble.empty() && p.game.nav_through >= 1 && p.game.acts_after_nav >= 1,
                  "a pass, a key of the six through at each opening: both stacks deleted -- " +
                      std::to_string(p.game.nav_through) + " through / " + r.trouble);
            check(p.game.acts_soon_after_nav == 0 && p.game.decided_unsettled == 0 &&
                      p.game.nearest_after_nav_ms >= BagCleaner::kNavSettleMs && p.game.acts_with_keys == 0,
                  "no input of the cleaner's own sooner than " + std::to_string(BagCleaner::kNavSettleMs) +
                      " ms after it, nor dragged by a look taken sooner -- the least gap " +
                      std::to_string(p.game.nearest_after_nav_ms) + " ms");
        } else {
            check(r.deleted == 2 && p.game.acts_soon_after_nav > 0,
                  "and without the window saying so -- a letter's way -- the drag comes sooner (the least gap " +
                      std::to_string(p.game.nearest_after_nav_ms) + " ms): the settle is the six's alone");
        }
    }

    if (!kit().shop_ok) {
        skip("the shop's pictures are not installed here\n");
        return;
    }
    // ---- the settle: a shop round ----
    for (const bool hook : { true, false }) {
        ShopRun s(2, 1);
        if (!s.ok) {
            skip("the item catalog is not installed here\n");
            return;
        }
        s.p.key_window = true;
        s.p.game.nav_on_open = true;
        s.p.game.nav_hook = hook;
        s.run();
        if (hook) {
            check(s.r.shop_bought == 3 && s.r.trouble.empty() && s.p.game.wrong_confirms == 0 &&
                      s.p.game.digit_keys == 6 && s.p.game.nav_through >= 3,
                  "a shop round, a key of the six at each opening: 2 + 1 bought, 00 typed each time -- " +
                      std::to_string(s.p.game.nav_through) + " through / " + s.r.trouble);
            check(s.p.game.acts_soon_after_nav == 0 && s.p.game.decided_unsettled == 0 &&
                      s.p.game.nearest_after_nav_ms >= BagCleaner::kNavSettleMs && s.p.game.opens_in_money == 0 &&
                      s.p.game.acts_with_keys == 0,
                  "the right-click, each double-click and ESC no sooner than " +
                      std::to_string(BagCleaner::kNavSettleMs) + " ms after it, each by a look taken after that -- "
                      "the least gap " + std::to_string(s.p.game.nearest_after_nav_ms) + " ms");
        } else {
            check(s.r.shop_bought == 3 && s.p.game.acts_soon_after_nav > 0,
                  "without the window saying so the double-click comes sooner -- the least gap " +
                      std::to_string(s.p.game.nearest_after_nav_ms) + " ms");
        }
    }

    // ---- the list scrolled before the second double-click ----
    {
        LogLines log;
        ShopRun s(3, 0);
        s.p.key_window = true;
        s.p.game.nav_on_open = true;
        s.p.game.on_nav = [&s] {
            if (s.p.game.buys.size() == 1) s.p.game.row_scrolled = true;      // PgDn went after the first
        };
        s.run();
        check(s.p.game.row_scrolled && s.p.game.buys == std::vector<int>({ 0 }) && s.p.game.double_clicks == 1 &&
                  s.p.game.wrong_item_doubles == 0 && !s.p.game.qty_up && s.p.game.wrong_confirms == 0,
              "the potions' row gone from sight, the shop's buttons still there: no double-click by where the row "
              "was -- " + std::to_string(s.p.game.double_clicks) + " double-clicks, " +
                  std::to_string(s.p.game.wrong_item_doubles) + " on another item");
        check(rows_unmatched() == BagCleaner::kShopRowRelooks + 1 &&
                  s.r.trouble.find("danh sách có thể đã bị cuộn") != std::string::npos && !s.p.game.shop_on,
              "looked at again " + std::to_string(BagCleaner::kShopRowRelooks) + " times, each said with its "
              "scores, then stopped with trouble and the shop shut -- " + std::to_string(rows_unmatched()) +
                  " lines / " + s.r.trouble);
        check(log.has("(row 0.00, buttons "), "the row's score in the line: " +
                                                   log.line_with("Trash: shop row not matched"));
    }
    {
        // Out of sight for two looks only: looked at again, and bought.
        LogLines log;
        ShopRun s(3, 0);
        s.p.key_window = true;
        s.p.game.nav_on_open = true;
        bool once = false;
        s.p.game.on_nav = [&] {
            if (once || s.p.game.buys.size() != 1) return;
            once = true;
            s.p.game.row_hidden_looks = 2;
        };
        s.run();
        check(once && s.r.trouble.empty() && s.p.game.buys == std::vector<int>({ 0, 0, 0 }) &&
                  s.p.game.wrong_item_doubles == 0 && s.p.game.double_clicks == 3 && rows_unmatched() == 2,
              "the row out of sight for two looks: looked at again, the purchase made by the look it is seen on "
              "-- " + std::to_string(rows_unmatched()) + " lines / " + s.r.trouble);
    }
    {
        // Without the window at all -- the player's wheel: the same.
        ShopRun s(2, 0);
        s.p.during = [&](BagCleaner&) {
            if (s.p.game.buys.size() == 1) s.p.game.row_scrolled = true;
        };
        s.run();
        check(s.p.game.buys.size() == 1 && s.p.game.double_clicks == 1 && s.p.game.wrong_item_doubles == 0 &&
                  s.r.trouble.find("danh sách có thể đã bị cuộn") != std::string::npos,
              "no key window, the list scrolled by hand: no double-click by the buttons alone either -- " +
                  s.r.trouble);
    }

    // ---- one of the six landing late in the quantity box ----
    {
        // Home, the caret before the game's 1: "00" makes it 001, which reads
        // as no count -- Hủy Bỏ, nothing bought.
        ShopRun s(2, 0);
        s.p.key_window = true;
        s.p.during = [&](BagCleaner&) {
            if (s.p.game.buys.size() == 1 && s.p.game.box_macro_keys == 0 && s.p.game.box_keys_next.empty())
                s.p.game.box_keys_next = { VK_HOME };
        };
        s.run();
        check(s.p.game.box_macro_keys == 1 && s.p.game.buys == std::vector<int>({ 0 }) && s.p.game.cancels == 1 &&
                  s.p.game.wrong_confirms == 0 && s.p.game.digit_keys == 4 && s.p.game.typed == "001" &&
                  s.r.trouble.find("không thành 100") != std::string::npos && !s.p.game.qty_up,
              "Home in the box as it came up: 00 typed after the game's 1 reads 001, not 100 -- Hủy Bỏ, never Xác "
              "Nhận; the cleaner's own digits 4, the macro's key counted apart -- " + s.r.trouble);
    }
    {
        // Home and Delete: the box emptied -- 100 typed into it and read back,
        // a purchase as it should be.
        ShopRun s(2, 0);
        s.p.key_window = true;
        s.p.during = [&](BagCleaner&) {
            if (s.p.game.buys.size() == 1 && s.p.game.box_macro_keys == 0 && s.p.game.box_keys_next.empty())
                s.p.game.box_keys_next = { VK_HOME, VK_DELETE };
        };
        s.run();
        check(s.p.game.box_macro_keys == 2 && s.p.game.buys == std::vector<int>({ 0, 0 }) &&
                  s.p.game.wrong_confirms == 0 && s.p.game.cancels == 0 && s.p.game.digit_keys == 5 &&
                  s.r.trouble.empty(),
              "Home and Delete in the box: emptied, 100 typed and read back -- 100 bought, never another number -- " +
                  s.r.trouble);
    }
}

/// Bug 1 of 2026-11-14 (the owner: "Khi dọn rác xong thì tắt luôn túi đồ khiến
/// xử lý mua HPMP bị dừng => trước khi thực hiện xử lý mua HPMP cần check xem
/// túi đồ có đang mở không, nếu có thì thực hiện luôn, còn không thì mở túi đồ
/// lên và thao tác mua"): at 20:31 the shop round that rode a pass looked a
/// tenth of a second after the pass's bag key, still saw the bag, and clicked
/// Khác on a bag no longer there; at 21:37 the game took neither of two clicks
/// on Khác. 2026.11.14b: the round looks twice before its key, opens a bag
/// shut and sees it stay open, clicks Khác only where its latest look shows
/// the bag and up to four times; and every job's end sees the bag gone.
void test_shop_bag() {
    section("Mua bình (2026.11.14b): the bag looked at twice before its key, opened only when shut and seen to stay "
            "open; Khác clicked on a fresh look, up to four times; a job's end sees the bag gone");
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
    const auto took_ms = [](const Pretend& g, std::chrono::steady_clock::time_point from) {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(g.time.t - from).count());
    };
    {
        // 20:31: the pass's own shut key drawn a look late -- the round's first
        // look still shows the bag, the next not. Looked at until two looks
        // agree, the bag opened once by its key, Khác clicked once on it, one
        // bought, the bag shut again at the end: two keys, never a click on
        // where the bag was.
        LogLines log;
        ShopRun s(1, 0);
        s.p.game.bag_keyed = true;
        s.p.game.bag_flips_due = { 2 };
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 1 && s.p.game.bag_keys == 2 && s.p.game.tab_clicks == 1 &&
                  s.p.game.clicks_on_shut_bag == 0 && s.p.game.bag_shut && s.p.game.right_clicks == 1,
              "20:31's shape -- the bag shutting as the round looks: looked at again, opened by its key once, Khác "
              "clicked once on it, one bought, shut at the end (" + std::to_string(s.p.game.bag_keys) + " bag keys, " +
                  std::to_string(s.p.game.tab_clicks) + " tab clicks, " +
                  std::to_string(s.p.game.clicks_on_shut_bag) + " on a shut bag) -- " + s.r.trouble);
    }
    {
        // Open on both looks: taken as it is -- no key -- and left open.
        ShopRun s(1, 0);
        s.p.game.bag_keyed = true;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 1 && s.p.game.bag_keys == 0 && !s.p.game.bag_shut,
              "the bag open on both looks: no bag key, one bought, the bag left open as it was found -- " +
                  s.r.trouble);
    }
    {
        // Shut on both: its key once, the opening drawn two looks late --
        // looked for, never a second key -- seen to stay open, one bought, and
        // shut at the end, seen gone.
        LogLines log;
        ShopRun s(1, 0);
        s.p.game.bag_keyed = s.p.game.bag_shut = true;
        s.p.game.bag_lag_looks = 2;
        s.run();
        check(s.r.trouble.empty() && s.r.shop_bought == 1 && s.p.game.bag_keys == 2 && s.p.game.bag_shut &&
                  !log.has("bag still seen"),
              "the bag shut, drawn open two looks after its key: one key to open it, one to shut it at the end, "
              "seen gone -- " + std::to_string(s.p.game.bag_keys) + " keys -- " + s.r.trouble);
    }
    {
        // Opened, and gone again at the look a moment later: no second key, no
        // tab clicked, the round stopped.
        ShopRun s(1, 0);
        s.p.game.bag_keyed = s.p.game.bag_shut = true;
        int seen_open = 0;
        s.p.during = [&](BagCleaner&) {
            Pretend& g = s.p.game;
            if (g.bag_keys == 1 && !g.bag_shut && ++seen_open == 2) g.bag_shut = true;
        };
        s.run();
        check(s.r.trouble.find("túi đồ vừa mở lại đóng ngay") != std::string::npos && s.p.game.bag_keys == 1 &&
                  s.p.game.tab_clicks == 0 && s.p.game.right_clicks == 0 && s.r.shop_bought == 0,
              "opened and gone again a moment later: one key only, nothing clicked, stopped -- " + s.r.trouble);
    }
    {
        // The bag on and off on every look -- a frame drawn as it shuts and
        // opens, over and over: no two looks agree, and no key goes (the
        // review of 2026.11.14b).
        ShopRun s(1, 0);
        s.p.game.bag_keyed = true;
        s.p.during = [&](BagCleaner&) { s.p.game.bag_shut = !s.p.game.bag_shut; };
        s.run();
        check(s.r.trouble.find("túi đồ lúc thấy lúc không") != std::string::npos && s.p.game.bag_keys == 0 &&
                  s.p.game.tab_clicks == 0 && s.p.game.right_clicks == 0 && s.r.shop_bought == 0,
              "the bag seen and not seen by turns: no bag key, nothing clicked, stopped -- " + s.r.trouble);
    }
    {
        // A dialog, or the check-in board, come up after the round's first
        // look and before the bag key (the review of 2026.11.14b): the looks
        // that decide the bag is shut are searched for them too, and the key
        // does not go into them.
        ShopRun s(1, 0);
        s.p.game.bag_keyed = s.p.game.bag_shut = true;
        s.p.during = [&](BagCleaner&) {
            Pretend& g = s.p.game;
            if (g.looks == 1 && g.up == Pretend::Up::None) g.show(kit().other, Pretend::Up::Question);
        };
        s.run();
        check(s.p.game.bag_keys == 0 && s.p.game.right_clicks == 0 && s.p.game.tab_clicks == 0 &&
                  s.r.trouble.find("có một hộp thoại đang mở") != std::string::npos,
              "a dialog come up before the bag key: no key, nothing clicked -- " + s.r.trouble);
        if (kit().board_ok) {
            ShopRun b(1, 0);
            b.p.game.bag_keyed = b.p.game.bag_shut = true;
            b.p.during = [&](BagCleaner&) {
                if (b.p.game.looks == 1) b.p.game.board_on = true;
            };
            b.run();
            check(b.p.game.bag_keys == 0 && b.p.game.acts_with_board == 0 && b.r.board_up,
                  "and the check-in board come up then: no key into it, the round put off for the board -- " +
                      b.r.trouble);
        }
    }
    {
        // Khác not taken at first -- a click or two the game let go: clicked
        // again, up to four times, a pixel further aside each time. (21:37's
        // stop, the game taking no click for 16 s after a failed drag, is the
        // stuck case below: still a stop.)
        ShopRun two(1, 0);
        two.p.game.tab_clicks_lost = 2;
        two.run();
        ShopRun three(1, 0);
        three.p.game.tab_clicks_lost = 3;
        three.run();
        check(two.r.trouble.empty() && two.r.shop_bought == 1 && two.p.game.tab_clicks == 3 &&
                  three.r.trouble.empty() && three.r.shop_bought == 1 && three.p.game.tab_clicks == 4,
              "Khác let go twice: bought on the third click; three times: on the fourth (" +
                  std::to_string(two.p.game.tab_clicks) + ", " + std::to_string(three.p.game.tab_clicks) + ") -- " +
                  two.r.trouble + three.r.trouble);
        ShopRun stuck(1, 0);
        stuck.p.game.tabs_stuck = true;
        stuck.p.game.bag_keyed = true;
        const auto from = stuck.p.game.time.t;
        stuck.run();
        check(stuck.r.tab_unsure && stuck.r.trouble.find("không chuyển được sang ngăn Khác") != std::string::npos &&
                  stuck.p.game.tab_clicks == BagCleaner::kShopTabClicks && stuck.p.game.right_clicks == 0 &&
                  stuck.p.game.bag_keys == 0 && !stuck.p.game.bag_shut &&
                  took_ms(stuck.p.game, from) >= kReactMs && took_ms(stuck.p.game, from) < kReactMs + 5'000 &&
                  stuck.r.trouble.find("sau 30 giây") != std::string::npos,
              "Khác never taken: four clicks spread over the 30 s the game is given (2026.11.15d), then a stop not "
              "much later -- nothing right-clicked, the bag open as it was found, no key (" +
                  std::to_string(took_ms(stuck.p.game, from)) + " ms) -- " + stuck.r.trouble);
    }
    {
        // A pass and a charm run keep their two clicks.
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.game.tabs_work = true;
        p.game.cur_tab = kOtherTab;
        p.game.tabs_stuck = true;
        p.run();
        CharmRun c;
        c.p.game.tabs_work = true;
        c.p.game.cur_tab = kOtherTab;
        c.p.game.tabs_stuck = true;
        c.run();
        check(p.game.tab_clicks == 2 && c.p.game.tab_clicks == 2 && c.p.game.right_clicks == 0,
              "a pass and a charm run: Tiêu Dùng clicked twice, as before (" + std::to_string(p.game.tab_clicks) +
                  ", " + std::to_string(c.p.game.tab_clicks) + ")");
    }
    {
        // A pass's end: the bag it opened shut, its key once, and looked at
        // until it is seen gone -- drawn two looks late here. A key the game
        // ignores: said in the log, and never a second one.
        LogLines log;
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.game.bag_keyed = p.game.bag_shut = true;
        p.game.bag_lag_looks = 2;
        const auto r = p.run();
        const std::string steps = log.line_with("Trash: pass steps");
        check(r.deleted == 1 && p.game.bag_shut && p.game.bag_keys == 2 && !log.has("bag still seen") &&
                  steps.find("; bag shut in ") != std::string::npos,
              "the pass's end: its key once, looked at until the bag is gone -- " + steps);
        LogLines log2;
        Pass q;
        make_bag(q.game);
        q.item(0, 7, 2, TrashList::Action::Delete);
        q.game.bag_keyed = q.game.bag_shut = true;
        q.game.bag_keys_taken = 1;
        q.run();
        // Looked for the owner's 30 s since 2026.11.15d (react_ms(); 1.5 s
        // before): a look every 80 ms, the pretend look's own 40 ms on top.
        check(q.game.bag_keys == 2 && !q.game.bag_shut &&
                  log2.has("bag still seen 30000 ms after its key (250 looks) -- not pressed again") &&
                  log2.has("Trash: nothing after 30 s -- the bag shut after its key"),
              "a shut key the game ignores: looked for 30 s, said, and never a third key -- " +
                  log2.line_with("bag still seen"));
    }
#ifdef GOBOT_SOURCE_DIR
    {
        const std::string cpp = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/bag.cpp");
        const std::size_t npos = std::string::npos;
        const auto body = [&](const std::string& head) {
            const std::size_t a = cpp.find(head);
            const std::size_t b = a == npos ? npos : cpp.find("\n}", a);
            return a == npos || b == npos ? std::string() : cpp.substr(a, b + 2 - a);
        };
        const std::string pass = body("BagCleaner::Result BagCleaner::run_pass() {");
        const std::size_t ready = pass.find("if (mode_ == Mode::ShopBuy && !shop_bag_ready(f, v, res, opened_here, "
                                            "open_ms, open_looks)) return res;");
        const std::size_t bought = pass.find("shop_buy(v, res, stop);");
        const std::size_t close_from = pass.find("const auto close_bag = [&] {");
        const std::size_t close_to = pass.find("};", close_from);
        const std::string close = close_from == npos || close_to == npos ? std::string()
                                                                         : pass.substr(close_from, close_to - close_from);
        const std::size_t key = close.find("press(settings_.bag_key);");
        const std::size_t seen = close.find("if (!bag::find(g, store_, l).found) return;");
        // Looked for react_ms() -- the owner's 30 s -- since 2026.11.15d.
        const std::size_t loop = close.find("while (!gone && ms_between(key_at, now()) < react_ms_) {");
        const std::string open = body("bool BagCleaner::shop_open(");
        const std::string ready_body = body("bool BagCleaner::shop_bag_ready(");
        check(ready != npos && bought != npos && ready < bought && seen != npos && key != npos && loop != npos &&
                  seen < key && key < loop && close.find("press(", loop) == npos &&
                  open.find("select_tab(kOtherTab, v, res, \"shop_other_tab\", kShopTabClicks)") != npos &&
                  open.find("const BagView seen_now = bag::find(f0, store_, l);") != npos &&
                  std::count(ready_body.begin(), ready_body.end(), '\n') > 10 &&
                  ready_body.find("press(settings_.bag_key);") != npos &&
                  ready_body.find("press(", ready_body.find("press(settings_.bag_key);") + 1) == npos &&
                  pass.find("line += \"; bag shut in \"") != npos,
              "bag.cpp: the shop's round readies its bag before shop_buy, one key at most; close_bag presses only "
              "on a bag seen and none in its wait; shop_open clicks Khác on its own look, kShopTabClicks times at "
              "most; the steps line as it was");
    }
#endif
}

/// Bug 2 of 2026-11-14 (the owner: "Trường hợp bị dừng quá 10s thì tắt toàn
/// bộ hiển thị shop, túi đồ trên màn hình bằng phím ESC. Xử lý này thực hiện
/// cho cả khi hộp nhập số lượng không biến mất"; and "khi dialog mua HPMP hiện
/// ra và không click vào đâu nữa thì sẽ có thể ấn được 00 luôn" -- no click
/// into the box before 00): a step of the shop or of a pass stuck kStuckMs
/// ends with ESC until nothing is up. Never in a charm run.
void test_stuck_esc() {
    section("Kẹt quá 30 giây (2026.11.14b, 30 s since 2026.11.15d): ESC until no shop, no dialog, no message and no bag "
            "is up -- the shop's quantity box, the bin's question; never a charm run's");
    if (skip_without_kit()) return;
    const auto took_ms = [](const Pretend& g, std::chrono::steady_clock::time_point from) {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(g.time.t - from).count());
    };
    // The stuck rule is the owner's 30 s for the game to react since
    // 2026.11.15d (react_wait.hpp), counted from GoBot's last input for the step.
    check(BagCleaner::kStuckMs == kReactMs && kReactMs == 30'000 && kReactSlowMs == 2'000 &&
              BagCleaner::kEscMax == 6 && BagCleaner::kEscMenuXDx == PopupEyes::kMenuXDx &&
              BagCleaner::kEscMenuXDy == PopupEyes::kMenuXDy,
          "the stuck rule's 30 s (kReactMs), six ESCs at most, the menu's X where the popup closer has it");
    const bool shop = kit().shop_ok && ShopRun(0, 0).ok;
    if (!shop) {
        skip("the shop's pictures or the item catalog are not installed here\n");
    } else {
        {
            // S1: the box takes neither keys nor a click -- a stray click took
            // its focus -- and Hủy Bỏ does not close it. 00 typed at once, no
            // click into it first; each way of typing given its 30 s to show in
            // the box (2026.11.15d), and then the stuck rule's 30 s since the
            // last keys are up: ESC on the question, the shop and the bag,
            // each on a look that shows it, and nothing held.
            ShopRun s(1, 0);
            s.p.game.keys_dead = s.p.game.click_dead = s.p.game.qty_sticky = true;
            const auto from = s.p.game.time.t;
            s.run();
            const Pretend& g = s.p.game;
            check(g.buys.empty() && g.wrong_confirms == 0 && s.r.shop_bought == 0 && s.r.shop_unsure == 0 &&
                      !g.qty_up && !g.shop_on && g.bag_shut && g.esc_on_qty == 1 && g.esc_on_nothing == 0 &&
                      !s.r.stuck && s.r.esc_swept && s.r.esc_presses == 3 && g.esc_presses == 3 &&
                      s.r.trouble.find("kẹt quá 30 giây (mua bình): GoBot đã bấm ESC 3 lần") != std::string::npos &&
                      took_ms(g, from) >= 2 * BagCleaner::kStuckMs,
                  "a box stuck: each way of typing waited 30 s for, then three ESCs -- the question, the shop, the bag "
                  "-- nothing bought or held (" + std::to_string(g.esc_presses) + " ESCs, " +
                      std::to_string(took_ms(g, from)) + " ms) -- " + s.r.trouble);
            check(g.digit_keys >= 2 && (g.digits_at_box_click < 0 || g.digits_at_box_click >= 2) && g.box_clicks <= 1,
                  "and 00 typed straight in, no click into the box before it (" +
                      std::to_string(g.digits_at_box_click) + " digits at the first box click)");
            // And no Hủy Bỏ once the 30 s since the last keys are up
            // (shop_cancel asks the stuck rule before each click): the ESCs
            // are the stop's clean-up -- and not much later than the two
            // waits of 30 s (2026.11.15d).
            std::printf("  [info] a stuck box: %lld ms, %d Hủy Bỏ click(s)\n", took_ms(g, from), g.cancels);
            check(g.cancels == 0 && took_ms(g, from) < 2 * BagCleaner::kStuckMs + 15'000,
                  "and Hủy Bỏ not clicked past the 30 s -- " + std::to_string(g.cancels) + " click(s), " +
                      std::to_string(took_ms(g, from)) + " ms in all");
        }
        {
            // The character down meanwhile (the review of 2026.11.14b): the hold
            // still the round's, but the guard says no -- HP under 1 %, the
            // death panel up -- and no ESC of the sweep's goes over it.
            ShopRun s(1, 0);
            s.p.game.keys_dead = s.p.game.click_dead = s.p.game.qty_sticky = true;
            s.p.hold_check = [] { return true; };
            s.p.guard = [&s] { return s.p.game.digit_keys == 0; };
            s.run();
            check(!s.r.esc_swept && s.r.esc_presses == 0 && s.p.game.esc_on_bag == 0,
                  "the character down, the hold still the round's: no ESC of the sweep's -- " + s.r.trouble);
        }
        {
            // The bag drawn shut ten looks after its ESC, a slow game (the
            // review of 2026.11.14b): the next ESC waits for two looks that
            // agree, half a second apart, and none lands on an empty screen --
            // no game menu brought up.
            if (!kit().menu_ok) {
                skip("the game's menu picture is not installed here\n");
            } else {
                ShopRun s(1, 0);
                s.p.game.keys_dead = s.p.game.click_dead = s.p.game.qty_sticky = true;
                s.p.game.esc_bag_lag_looks = 10;
                s.p.game.menu_on_esc = true;
                s.run();
                const Pretend& g = s.p.game;
                check(g.esc_on_nothing == 0 && g.menu_shows == 0 && g.esc_on_qty == 1 && g.esc_on_bag == 1 &&
                          g.bag_shut && !g.shop_on && s.r.esc_presses == 3 && !s.r.stuck &&
                          s.r.trouble.find("GoBot đã bấm ESC 3 lần để đóng") != std::string::npos,
                      "the bag shut ten looks after its ESC: no ESC on nothing, no menu, three ESCs and clear (" +
                          std::to_string(g.esc_presses) + " ESCs, " + std::to_string(g.esc_on_nothing) +
                          " on nothing) -- " + s.r.trouble);
                // Shut later still: an ESC does land on nothing and the menu
                // comes up -- shut by its X, found where the popup closer
                // looks, and clicked again when the first click is lost;
                // looked at again; never left up.
                ShopRun m(1, 0);
                m.p.game.keys_dead = m.p.game.click_dead = m.p.game.qty_sticky = true;
                m.p.game.esc_bag_lag_looks = 40;
                m.p.game.menu_on_esc = true;
                m.p.game.menu_x_misses = 1;
                LogLines log;
                m.run();
                const Pretend& h = m.p.game;
                check(h.menu_shows == 1 && !h.menu_up && h.menu_x_clicks == 2 && h.menu_other_clicks == 0 &&
                          h.esc_on_nothing == 1 && h.esc_on_menu == 0 && log.has("brought up the game's menu") &&
                          log.has("the game's menu shut by its X"),
                      "an ESC that brought up the game's menu: its X clicked, again when the click was lost, the "
                      "menu seen gone (" + std::to_string(h.menu_x_clicks) + " X clicks, " +
                          std::to_string(h.esc_on_nothing) + " ESC on nothing) -- " + m.r.trouble);
            }
        }
        {
            // S1b: the box read back short of 100 again and again on a slow
            // game -- its last 0 eaten each time: once the box has shown
            // nothing of the last keys for the stuck rule's 30 s (2026.11.15d)
            // the read-back loop stops (kShopStuck) with no Hủy Bỏ and no Xác
            // Nhận, and the ESCs shut it, the shop and the bag.
            LogLines log;
            ShopRun s(1, 0);
            s.p.during = [&](BagCleaner&) {
                Pretend& g = s.p.game;
                if (!g.qty_up) return;
                if (g.typed == "100") g.typed = "10";
                g.time.t += std::chrono::milliseconds(900);
            };
            s.run();
            const Pretend& g = s.p.game;
            check(log.has("Trash: shop purchase of Thịt bò khô stuck -- the quantity box up") &&
                      log.has("stuck 30 s, nothing bought; everything to be shut with ESC") && g.buys.empty() &&
                      g.wrong_confirms == 0 && g.cancels == 0 && g.esc_on_qty == 1 && !g.qty_up && !g.shop_on &&
                      s.r.esc_swept && !s.r.stuck && s.r.shop_unsure == 0 &&
                      s.r.trouble.find("không thành 100 sau 30 giây") != std::string::npos &&
                      log.has("Trash: nothing after 30 s -- the keys typed in the quantity box"),
                  "the box short of 100, nothing of the last keys for 30 s: no more keys, no Hủy Bỏ, no Xác Nhận -- ESC "
                  "on it, the shop and the bag, nothing bought or held -- " + s.r.trouble + " / " +
                      log.line_with("stuck -- the quantity box"));
        }
        {
            // S2: Xác Nhận does nothing, the question stays: counted as bought
            // (unsure), never bought twice -- then the ESCs.
            ShopRun s(1, 0);
            s.p.game.confirm_ignored = s.p.game.qty_sticky = true;
            s.run();
            check(s.r.shop_unsure == 1 && s.p.game.buys.empty() && s.p.game.esc_on_qty == 1 && !s.p.game.qty_up &&
                      !s.p.game.shop_on && !s.r.stuck && s.r.esc_swept && s.p.game.esc_on_nothing == 0,
                  "the question still up after Xác Nhận: unsure, counted, then shut with ESC -- " + s.r.trouble);
        }
        {
            // S3: slow but going well, the box up well over the stuck rule's
            // 30 s: never cut -- the watch is asked only after a read-back
            // short of 100, and counts from the last keys (2026.11.15d). Slow on
            // every look while the box is up (4500 ms each); and slow only
            // before the first digit, so that 30 s are up before any key goes
            // in (11 s a look): 00 typed all the same, bought.
            for (const bool before_keys : { false, true }) {
                ShopRun s(1, 0);
                std::chrono::steady_clock::time_point up_at{}, gone_at{};
                s.p.during = [&](BagCleaner&) {
                    Pretend& g = s.p.game;
                    if (g.qty_up) {
                        if (up_at == std::chrono::steady_clock::time_point{}) up_at = g.time.t;
                        if (!before_keys) g.time.t += std::chrono::milliseconds(4500);
                        else if (g.digit_keys == 0) g.time.t += std::chrono::milliseconds(11'000);
                    } else if (up_at != std::chrono::steady_clock::time_point{} &&
                               gone_at == std::chrono::steady_clock::time_point{}) {
                        gone_at = g.time.t;
                    }
                };
                s.run();
                const long long slow = static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(gone_at - up_at).count());
                check(s.r.trouble.empty() && s.r.shop_bought == 1 && s.p.game.esc_presses == 1 && !s.r.esc_swept &&
                          slow >= BagCleaner::kStuckMs,
                      std::string(before_keys ? "slow before its first key" : "slow all the way") +
                          ": a purchase that goes well (" + std::to_string(slow) + " ms with the box up) is never "
                          "cut: bought, the one ESC the shop's own -- " + s.r.trouble);
            }
        }
        {
            // A step seen through late is no step stuck (the review of
            // 2026.11.14b): the full-bag message after Xác Nhận, closed by
            // its own Xác Nhận ten seconds and more after the question came --
            // at once (the question taken to be up still) and a moment on
            // (the question seen gone first) -- and the shop shut as ever,
            // its one ESC, the bag left as it was found.
            for (const int late_ms : { 0, 600 }) {
                ShopRun s(1, 0, true, 0);
                s.p.game.full_late_ms = late_ms;
                s.p.during = [&](BagCleaner&) {
                    if (s.p.game.qty_up) s.p.game.time.t += std::chrono::milliseconds(1500);
                };
                s.run();
                const Pretend& g = s.p.game;
                check(s.r.shop_full && g.buys.empty() && !g.full_up && !s.r.esc_swept && s.r.esc_presses == 0 &&
                          g.esc_presses == 1 && !g.shop_on && !g.bag_shut && !s.r.stuck &&
                          s.r.trouble.find("kẹt quá") == std::string::npos,
                      "the full-bag message " + std::to_string(late_ms) + " ms after Xác Nhận, closed over 10 s "
                      "after the question came: no sweep, the shop's own ESC -- " + s.r.trouble);
            }
            // And the first purchase stopped on its row search more than ten
            // seconds after the right-click: the shop was seen, its step over.
            ShopRun s(1, 0);
            int shop_looks = 0;
            s.p.during = [&](BagCleaner&) {
                Pretend& g = s.p.game;
                if (!g.shop_on || ++shop_looks < 2) return;
                g.row_scrolled = true;
                g.time.t += std::chrono::milliseconds(3000);
            };
            s.run();
            check(s.r.trouble.find("không thấy hàng Thịt bò khô") != std::string::npos && !s.r.esc_swept &&
                      s.p.game.esc_presses == 1 && !s.p.game.shop_on && !s.p.game.bag_shut &&
                      s.r.trouble.find("kẹt quá") == std::string::npos,
                  "a row search ended over 10 s after the right-click: no sweep, the shop's own ESC -- " +
                      s.r.trouble);
        }
        {
            // S4: an ESC that closes nothing -- a shop ESC does not shut -- is
            // let go once; the second ends the sweep, and the shop is shut as
            // ever (its one ESC, then Thoát). The bag, found open, stays.
            ShopRun s(1, 0);
            s.p.game.keys_dead = s.p.game.click_dead = s.p.game.qty_sticky = true;
            s.p.game.esc_closes_shop = false;
            s.run();
            check(s.r.esc_presses == 3 && s.p.game.esc_on_qty == 1 && s.p.game.esc_on_bag == 0 &&
                      s.p.game.esc_presses == 4 && s.p.game.thoat_clicks == 1 && !s.p.game.shop_on &&
                      s.p.game.esc_on_nothing == 0 && !s.r.stuck &&
                      s.r.trouble.find("mà chưa đóng hết được") != std::string::npos,
                  "a shop ESC does not shut: the question shut, two ESCs that close nothing end the sweep, then the "
                  "shop's own ESC and Thoát (" + std::to_string(s.p.game.esc_presses) + " ESCs) -- " + s.r.trouble);
        }
        {
            // The hold another job's: no ESC of the sweep's.
            ShopRun s(1, 0);
            s.p.game.keys_dead = s.p.game.click_dead = s.p.game.qty_sticky = true;
            s.p.hold_check = [&s] { return s.p.game.digit_keys == 0; };
            s.run();
            check(!s.r.esc_swept && s.r.esc_presses == 0 && s.p.game.esc_on_bag == 0,
                  "the hold lost meanwhile: no ESC of the sweep's -- " + s.r.trouble);
        }
    }
    {
        // P1: 2026-11-14 21:31 -- the bin's question not to be seen while the
        // bag is up. The bin clicked again only while the step is young; ten
        // seconds on, ESC: the bag shut, the question seen and given its Hủy
        // Bỏ; the item back, not deleted, the bag key never pressed after it.
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.game.bin_ask_hidden_while_bag = true;
        p.game.esc_on_bin = Pretend::EscBin::Ignores;
        const auto from = p.game.time.t;
        const auto r = p.run();
        check(p.game.items[0].count == 2 && p.game.bin_confirms == 0 && p.game.bin_cancels == 1 &&
                  p.game.esc_on_bag == 1 && p.game.esc_on_nothing == 0 && p.game.up == Pretend::Up::None &&
                  p.game.bag_shut && p.game.bag_keys == 0 && r.esc_swept && r.esc_presses == 1 &&
                  took_ms(p.game, from) >= BagCleaner::kStuckMs &&
                  r.trouble.find("kẹt quá 30 giây (dọn rác)") != std::string::npos,
              "21:31's bin: its question waited 30 s for, then one ESC shuts the bag, the question it hid gets its "
              "Hủy Bỏ, the item back, nothing deleted (" + std::to_string(p.game.esc_presses) + " ESCs) -- " +
                  r.trouble);
        // The same with ESC taken by the hidden question as a Hủy Bỏ: the bin
        // seen empty, so nothing is held after it.
        Pass q;
        make_bag(q.game);
        q.item(0, 7, 2, TrashList::Action::Delete);
        q.game.bin_ask_hidden_while_bag = true;
        q.game.esc_on_bin = Pretend::EscBin::GivesBack;
        const auto rq = q.run();
        check(q.game.items[0].count == 2 && q.game.bin_confirms == 0 && q.game.esc_on_bin_q == 1 &&
                  q.game.esc_on_bag == 1 && q.game.bag_shut && !rq.stuck && rq.esc_presses == 2,
              "and with the ESC giving the item back: the bin seen empty, the bag shut, nothing held -- " + rq.trouble);
        // The pass opened the bag itself (the review of 2026.11.14b): the ESC
        // shuts it, and close_bag, finding no bag, never presses its key over
        // it -- the bag key once, to open it.
        Pass o;
        make_bag(o.game);
        o.item(0, 7, 2, TrashList::Action::Delete);
        o.game.bag_keyed = o.game.bag_shut = true;
        o.game.bin_ask_hidden_while_bag = true;
        o.game.esc_on_bin = Pretend::EscBin::Ignores;
        const auto ro = o.run();
        check(o.game.bag_keys == 1 && o.game.bag_shut && o.game.esc_on_bag == 1 && o.game.items[0].count == 2 &&
                  o.game.bin_confirms == 0 && o.game.bin_cancels == 1 && ro.esc_swept,
              "a bag the pass opened, shut by the ESC: no bag key after it (" + std::to_string(o.game.bag_keys) +
                  " in all) -- " + ro.trouble);
        // 21:31 on a slow game: the bin's click given its 30 s (2026.11.15d),
        // and then the bin is not clicked again -- the stuck rule's 30 s since
        // that click are up -- the trouble saying so; then the ESCs.
        Pass s;
        make_bag(s.game);
        s.item(0, 7, 2, TrashList::Action::Delete);
        s.game.bin_ask_hidden_while_bag = true;
        s.game.esc_on_bin = Pretend::EscBin::Ignores;
        s.during = [&s](BagCleaner&) { s.game.time.t += std::chrono::milliseconds(500); };
        const auto rs = s.run();
        std::printf("  [info] 21:31 on a slow game: %d click(s) on the bin\n", s.game.bin_clicks);
        check(s.game.bin_clicks == 1 &&
                  rs.trouble.find("thùng rác đang có đồ chờ xoá quá 30 giây") != std::string::npos &&
                  s.game.items[0].count == 2 && s.game.bin_confirms == 0 && s.game.esc_on_bag == 1 && rs.esc_swept,
              "21:31, slow: the bin clicked " + std::to_string(s.game.bin_clicks) + " time(s), not again once its "
              "30 s were up; then the ESCs, nothing deleted -- " + rs.trouble);
    }
    {
        // An item GoBot left in the bin before this pass, its question hidden
        // under the bag and its Hủy Bỏ taking nothing (the review of
        // 2026.11.14b): the give-back fails, and the pass's start shuts what
        // is up with ESC at once -- older than 10 s by then. Nothing deleted.
        LogLines log;
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.game.up = Pretend::Up::BinAsking;
        p.game.about = 0;
        p.game.bin_ask_hidden_while_bag = p.game.bin_sticky = true;
        const auto r = p.run();
        check(r.esc_swept && log.has("Trash: stuck at pass start -- ESC") && p.game.esc_on_bag == 1 &&
                  p.game.items[0].count == 2 && p.game.bin_confirms == 0 && p.game.esc_on_nothing == 0 &&
                  p.game.up == Pretend::Up::None,
              "a leftover in the bin, its question hidden and sticky: swept at the pass's start, nothing deleted -- " +
                  r.trouble);
        // The same with the bag shut and the bin's question in sight: Hủy Bỏ
        // taking nothing, then the sweep at the start.
        LogLines log2;
        Pass q;
        make_bag(q.game);
        q.item(0, 7, 2, TrashList::Action::Delete);
        q.game.up = Pretend::Up::BinAsking;
        q.game.about = 0;
        q.game.bag_shut = q.game.bin_sticky = true;
        const auto rq = q.run();
        check(rq.esc_swept && log2.has("Trash: stuck at pass start -- ESC") && q.game.esc_on_bin_q == 1 &&
                  q.game.items[0].count == 2 && q.game.bin_confirms == 0 && q.game.esc_on_nothing == 0 &&
                  q.game.up == Pretend::Up::None,
              "the bin's question up with the bag shut, its Hủy Bỏ taking nothing: swept at the pass's start, "
              "nothing deleted -- " + rq.trouble);
    }
    {
        // A pass whose item sticks with the character down (the review of
        // 2026.11.14b): the guard's no -- the hold alone still the pass's --
        // and no ESC goes over the death panel.
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.game.bin_ask_hidden_while_bag = true;
        p.game.esc_on_bin = Pretend::EscBin::Ignores;
        p.hold_check = [] { return true; };
        p.guard = [&p] { return p.game.drags == 0; };
        const auto r = p.run();
        check(p.game.esc_presses == 0 && !r.esc_swept && r.esc_presses == 0 && p.game.items[0].count == 2,
              "an item stuck with the character down: no ESC -- " + r.trouble);
    }
    {
        // P2: the ESC leaves the item in the bin: never deleted by it; the
        // next pass gives it back first, then deletes it as ever.
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.game.bin_ask_hidden_while_bag = true;
        p.game.esc_on_bin = Pretend::EscBin::Leaves;
        const auto r1 = p.run();
        const bool waiting = p.game.up == Pretend::Up::InBin && p.game.items[0].count == 2 && r1.stuck;
        p.game.bin_ask_hidden_while_bag = false;
        p.game.bag_keyed = true;
        const auto r2 = p.run();
        check(waiting && p.game.bin_confirms == 1 && p.game.items[0].gone() && r2.deleted == 1 && !r2.stuck,
              "an ESC that leaves the item in the bin: nothing deleted, the keys held; the next pass gives it back "
              "and then deletes it -- " + r1.trouble + " / " + r2.trouble);
    }
    {
        // P3: the bin's quantity box seen, its number not the stack's, and
        // Hủy Bỏ taking nothing: ten seconds on, Hủy Bỏ once more, then ESC.
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.game.bin_sticky = true;
        p.game.bin_box_shows = 9;
        const auto r = p.run();
        check(p.game.items[0].count == 2 && p.game.bin_confirms == 0 && p.game.esc_on_bin_q == 1 &&
                  p.game.up == Pretend::Up::None && p.game.esc_on_nothing == 0 && r.esc_swept,
              "a bin's question Hủy Bỏ does not close: ESC after 10 s, the item back, nothing deleted -- " + r.trouble);
    }
    {
        // P4: a look-only pass with an item waiting: no ESC, no click.
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.game.up = Pretend::Up::InBin;
        p.game.about = 0;
        p.run(true);
        check(p.game.esc_presses == 0 && p.game.clicks == 0 && p.game.keys == 0,
              "a look-only pass with an item in the bin: nothing pressed, ESC least of all");
    }
    {
        // C1: a charm run, a question that will not close: never Esc, nor any key.
        CharmRun c(Pretend::Reply::Other);
        c.p.game.sticky = true;
        c.run();
        check(c.p.game.esc_presses == 0 && c.p.game.keys == 0 && !c.r.esc_swept,
              "a charm run with a question that will not close: no ESC, no key -- " + c.r.trouble);
    }
#ifdef GOBOT_SOURCE_DIR
    {
        const std::string cpp = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/bag.cpp");
        const std::size_t npos = std::string::npos;
        const auto body = [&](const std::string& head) {
            const std::size_t a = cpp.find(head);
            const std::size_t b = a == npos ? npos : cpp.find("\n}", a);
            return a == npos || b == npos ? std::string() : cpp.substr(a, b + 2 - a);
        };
        const auto count_of = [](const std::string& text, const std::string& part) {
            int n = 0;
            for (std::size_t at = text.find(part); at != npos; at = text.find(part, at + 1)) ++n;
            return n;
        };
        const std::string esc = body("bool BagCleaner::esc_out(Result& res, const char* where) {");
        const std::string close = body("bool BagCleaner::shop_close(const bag::Shop& shop, Result& res) {");
        const std::size_t refuse = esc.find("if (mode_ == Mode::CharmsOnly) return false;");
        const std::size_t pressed = esc.find("press(VK_ESCAPE)");
        check(refuse != npos && pressed != npos && refuse < pressed && count_of(cpp, "VK_ESCAPE") == 2 &&
                  count_of(close, "press(VK_ESCAPE)") == 1 && count_of(esc, "press(VK_ESCAPE)") == 1 &&
                  cpp.find("VK_RETURN") == npos && esc.find("press(settings_.bag_key)") == npos &&
                  esc.find("if (!grab_ || !grab_(g) || !g.valid()) {") < pressed,
              "bag.cpp: ESC only in shop_close and esc_out, which refuses a charm run before anything and presses "
              "only on a look; no Enter anywhere, no bag key in the sweep");
        // The review of 2026.11.14b: the round's end looks again after any
        // sweep, cleared or not; the right-click's step over when the shop is
        // seen; the end of shop_buy sweeps only with something of the step
        // still up; the menu's X found, never clicked blind.
        const std::string pass = body("BagCleaner::Result BagCleaner::run_pass() {");
        const std::string open = body("bool BagCleaner::shop_open(");
        const std::string buy = body("void BagCleaner::shop_buy(");
        const std::size_t found = open.find("if (shop.found) {");
        check(pass.find("const bool swept_before = esc_tried_;\n            esc_out(res, \"shop end\");\n            if "
                        "(!swept_before && esc_tried_) seen = grab_ && grab_(g) && g.valid();") != npos &&
                  found != npos && open.find("step_on_ = false;", found) < open.find("shot = g;", found) &&
                  buy.find("if (shop_up && step_over() && step_left() && esc_out(res, \"shop\")) shop_up = false;") !=
                      npos &&
                  esc.find("if (!menu_x_at(seen, x, y)) {") != npos && esc.find("kEscMenuXDx + menu_x->width / 2") == npos,
              "bag.cpp: the round's end looks again after any sweep; the shop seen ends the right-click's step; "
              "shop_buy's end sweeps only with the step's things up; the menu's X clicked only where it is seen");
    }
#endif
}

/// The owner, 2026-11-15, for slow PCs: "khi GoBot thực hiện thao tác, sẽ có
/// thêm 30 giây chờ phản ứng. Nếu màn hình phản ứng luôn thì thực hiện tiếp
/// bước tiếp theo, còn nếu màn hình không phản ứng gì thì sẽ dừng xử lý."
/// The bag after its key, on the pretend clock: come late, waited for and gone
/// on with; never come, given up after 30 s and not much later, its key once;
/// a stop or the guard's no in the middle of the wait ending it at once. And
/// the check-in board alone left up by the ESC sweep in its night hold.
void test_react_wait() {
    section("30 giây chờ phản ứng (2026.11.15d): the bag's key waited 30 s for, on at once when it shows; the "
            "check-in board left alone by the ESCs from 23:55 to 00:03");
    if (skip_without_kit()) return;
    const auto ms_of = [](std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
    };
    check(kReactMs == 30'000 && kReactSlowMs == 2'000 && BagCleaner::kStuckMs == kReactMs &&
              BagCleaner::kCancelLookMs == 1800 && BagCleaner::kCancelClicksShare == 5 &&
              BagCleaner::kReactPotionMs == 2'000,
          "30 s for the game to react, a reaction past 2 s said in the log; the stuck rule the same 30 s; a Hủy Bỏ "
          "watched its share of them, 1.8 s at least");
    {
        // (a) The bag drawn sixty looks -- some 7 s -- after its key: waited
        // for, the item deleted, the bag's own shut seen as late; one key each
        // way, never a second, and the log says how long the game took.
        LogLines log;
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.game.bag_keyed = p.game.bag_shut = true;
        p.game.bag_lag_looks = 60;
        int react = 0;
        p.during = [&](BagCleaner& c) { react = c.react_ms(); };
        const auto r = p.run();
        check(react == kReactMs && r.deleted == 1 && r.trouble.empty() && p.game.bag_keys == 2 && p.game.bag_shut &&
                  log.has("Trash: the game took ") && log.has(" ms to show the bag after its key") &&
                  !log.has("bag still seen") && !log.has("Trash: nothing after"),
              "the bag 7 s late after its key: waited for, the item deleted, one key to open and one to shut -- " +
                  std::to_string(p.game.bag_keys) + " keys, " + log.line_with("to show the bag after") + " -- " +
                  r.trouble);
    }
    {
        // (b) A key the game never takes: the bag looked for 30 s, then the
        // run given up as it was on its 1.8 s -- the same trouble, said with
        // its 30 s -- not much later, and no second key.
        LogLines log;
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.game.bag_keyed = p.game.bag_shut = true;
        p.game.bag_keys_taken = 0;
        std::chrono::steady_clock::time_point key_at{};
        p.during = [&](BagCleaner&) {
            if (p.game.bag_keys == 1 && key_at == std::chrono::steady_clock::time_point{}) key_at = p.game.time.t;
        };
        const auto r = p.run();
        const long long took = ms_of(key_at, p.game.time.t);
        check(p.game.bag_keys == 1 && r.deleted == 0 && p.game.drags == 0 &&
                  r.trouble.find("không mở được túi đồ") != std::string::npos &&
                  r.trouble.find("sau 30 giây") != std::string::npos && took >= kReactMs - 200 &&
                  took < kReactMs + 2'000 && log.has("Trash: nothing after 30 s -- the bag after its key"),
              "the bag never shown: given up after its 30 s (" + std::to_string(took) + " ms), one key, nothing "
              "dragged -- " + r.trouble);
    }
    {
        // (c) A stop five seconds into that wait ends it there, as F12 and
        // DỪNG always have; and so does the guard's no -- HP under the job's
        // level, the hold lost -- said as the guard is.
        for (const bool by_guard : { false, true }) {
            Pass p;
            make_bag(p.game);
            p.item(0, 7, 2, TrashList::Action::Delete);
            p.game.bag_keyed = p.game.bag_shut = true;
            p.game.bag_keys_taken = 0;
            std::chrono::steady_clock::time_point key_at{};
            const auto five_on = [&] {
                return p.game.bag_keys == 1 && key_at != std::chrono::steady_clock::time_point{} &&
                       ms_of(key_at, p.game.time.t) >= 5'000;
            };
            p.during = [&](BagCleaner& c) {
                if (p.game.bag_keys == 1 && key_at == std::chrono::steady_clock::time_point{}) key_at = p.game.time.t;
                if (!by_guard && five_on()) c.cancel();
            };
            if (by_guard) p.guard = [&] { return !five_on(); };
            const auto r = p.run();
            const long long took = ms_of(key_at, p.game.time.t);
            check(p.game.bag_keys == 1 && r.deleted == 0 && p.game.drags == 0 && took >= 5'000 && took < 6'500 &&
                      (!by_guard || r.trouble.find("máu xuống thấp hoặc bot không còn giữ phím") != std::string::npos),
                  std::string(by_guard ? "the guard's no" : "a stop") + " 5 s into the bag's wait: over at once (" +
                      std::to_string(took) + " ms after the key), no second key -- " + r.trouble);
        }
    }
    {
        // (d) The review of 2026.11.15d: a black stretch is a wait for the
        // game like the others -- the guards asked on every black look. The
        // game black from the first look, HP under the job's level 2 s in:
        // the stretch shortened to the old 4 s (kBlackLooks looks kBlackGapMs
        // apart), not sat through for 30 s with every potion held off, and
        // the run said stopped by the guard.
        LogLines log;
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.game.black_from = 1;
        const auto from = p.game.time.t;
        p.guard = [&] { return ms_of(from, p.game.time.t) < 2'000; };
        const auto r = p.run();
        const long long took = ms_of(from, p.game.time.t);
        check(r.deleted == 0 && p.game.drags == 0 && p.game.black_looks <= 40 &&
                  took < 4'000 + 3'000 &&
                  r.trouble.find("máu xuống thấp hoặc bot không còn giữ phím") != std::string::npos &&
                  log.has("said no while the run waited for the game"),
              "a black screen with the guard's no 2 s in: the stretch over by the old 4 s, not 30 s (" +
                  std::to_string(p.game.black_looks) + " black looks, " + std::to_string(took) + " ms) -- " +
                  r.trouble);
    }
    {
        // (e) The review of 2026.11.15d: a wait after the work -- the bag's
        // shut at a pass's end -- is shortened by the guard's no and not
        // counted as a stop: the pass that deleted its item is no pass
        // "stopped mid-way" (which would put it on again a few seconds later
        // and refuse the shop its ride). The bag drawn shut sixty looks after
        // its key, HP under the level from that key on.
        LogLines log;
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.game.bag_keyed = p.game.bag_shut = true;
        p.game.bag_lag_looks = 60;
        // The last look before the shutting key: close_bag's own.
        std::chrono::steady_clock::time_point shut_at{};
        p.during = [&](BagCleaner&) {
            if (p.game.bag_keys < 2) shut_at = p.game.time.t;
        };
        p.guard = [&] { return p.game.bag_keys < 2; };
        const auto r = p.run();
        const long long took = ms_of(shut_at, p.game.time.t);
        check(r.deleted == 1 && r.trouble.empty() && p.game.bag_keys == 2 && took < 3'000 &&
                  !log.has("said no while the run waited for the game"),
              "the guard's no while the bag shuts at the pass's end: that wait over at once (" + std::to_string(took) +
                  " ms), the pass not said stopped -- " + r.trouble);
    }
    {
        // (f) The review of 2026.11.15d: a charm whose stack still reads the
        // same after Xác Nhận is watched up to react_ms() now, and an HP or
        // MP potion due is given its moment meanwhile -- the charm run's
        // guard says no only with the character down. The potion due from
        // the Xác Nhận on: pressed by the run while the stack is read, never
        // with anything up.
        LogLines log;
        CharmRun c;
        c.p.game.registers = false;
        c.p.own = true;
        bool due_set = false;
        c.p.during = [&](BagCleaner&) {
            if (!due_set && c.p.game.confirms >= 1) {
                due_set = true;
                c.p.game.vitals_due = 1;
            }
        };
        c.run();
        check(c.p.game.confirms == 1 && c.r.charms_used == 0 && c.p.game.items[0].count == 4 &&
                  c.p.game.own_drunk == 1 && c.p.game.own_unsafe == 0 &&
                  log.has("own potion press while the charm's stack is read"),
              "a charm's stack the same after Xác Nhận: the HP potion pressed while it is watched (" +
                  std::to_string(c.p.game.own_drunk) + " drunk, " + std::to_string(c.took_ms) + " ms) -- " +
                  c.r.trouble);
        // And the deadline moved on once for one Xác Nhận: the stack's wait
        // credits only what is past the question's, both from that click.
        check(BagCleaner::react_credit_ms(1'500) == 0 && BagCleaner::react_credit_ms(10'000) == 8'000 &&
                  BagCleaner::react_credit_ms(20'000, 10'000) == 10'000 &&
                  BagCleaner::react_credit_ms(30'000, 1'000) == 28'000 &&
                  BagCleaner::react_credit_ms(8'000, 10'000) == 0,
              "a wait's credit to the deadline: past 2 s, less what an earlier wait from the same input credited "
              "(a question gone at 10 s and the stack at 20 s: 8 s and 10 s, not 8 s and 18 s)");
    }
#ifdef GOBOT_SOURCE_DIR
    {
        const std::string cpp = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/bag.cpp");
        const std::size_t npos = std::string::npos;
        check(cpp.find("react_potion(shot, BagView{}, \"while the charm's stack is read\", out.asked_at);") != npos &&
                  cpp.find("react_took(ms_between(out.asked_at, now()), \"the charm's stack down after Xác Nhận\", "
                           "closed_ms);") != npos &&
                  cpp.find("react_none(\"the charm's stack down after Xác Nhận\", closed_ms);") != npos &&
                  cpp.find("const ReactAfter after_work(*this);") != npos &&
                  cpp.find("const ReactAfter round_end(*this);") != npos &&
                  cpp.find("if (!short_wait && react_stop()) short_wait = true;") != npos,
              "bag.cpp: the charm's stack wait presses potions and credits the deadline once; the bag's shut and the "
              "shop's ESC not counted a stop; the black stretch asks the guards");
    }
#endif
    {
        // A test on the real clock gives the game less (set_react_ms): the
        // same wait, given up after its second.
        Pass p;
        make_bag(p.game);
        p.item(0, 7, 2, TrashList::Action::Delete);
        p.game.bag_keyed = p.game.bag_shut = true;
        p.game.bag_keys_taken = 0;
        p.react_ms = 1'000;
        const auto from = p.game.time.t;
        const auto r = p.run();
        const long long took = ms_of(from, p.game.time.t);
        check(p.game.bag_keys == 1 && r.trouble.find("sau 1 giây") != std::string::npos && took >= 1'000 &&
                  took < 3'000,
              "set_react_ms(1000): the bag given up after its second (" + std::to_string(took) + " ms) -- " +
                  r.trouble);
    }
    // The check-in board alone in its night hold (the owner, 2026-11-15:
    // "Trường hợp qua ngày thì chờ tới 00:03 mới ấn ESC"): the ESC sweep of a
    // stuck quantity box shuts the box, the shop and the bag, and the board
    // that came up after them is left -- the sweep ends clear. At noon the
    // same board gets its ESC.
    if (!kit().board_ok || !kit().shop_ok || !ShopRun(0, 0).ok) {
        skip("the check-in board's, the shop's pictures or the item catalog are not installed here\n");
    } else {
        for (const bool night : { true, false }) {
            LogLines log;
            ShopRun s(1, 0);
            s.p.game.keys_dead = s.p.game.click_dead = s.p.game.qty_sticky = true;
            s.p.day_clock = [night] { return night ? 23 * 3600 + 58 * 60 : 12 * 3600; };
            bool came = false;
            s.p.during = [&](BagCleaner&) {
                if (!came && s.p.game.esc_presses >= 3) came = s.p.game.board_on = true;
            };
            s.run();
            const Pretend& g = s.p.game;
            if (night)
                check(came && g.board_on && g.esc_presses == 3 && s.r.esc_presses == 3 && s.r.esc_swept &&
                          !s.r.stuck && g.acts_with_board == 0 && log.has("in its night hold"),
                      "23:58: the board alone after the box, the shop and the bag are shut is left up, no ESC on it, "
                      "the sweep clear (" + std::to_string(g.esc_presses) + " ESCs) -- " + s.r.trouble);
            else
                check(came && !g.board_on && g.esc_presses == 4 && s.r.esc_presses == 4 && !s.r.stuck &&
                          !log.has("in its night hold"),
                      "noon: the same board gets its ESC (" + std::to_string(g.esc_presses) + " ESCs) -- " +
                          s.r.trouble);
        }
        check(checkin_night_hold(23 * 3600 + 55 * 60) && checkin_night_hold(2 * 60 + 59) &&
                  !checkin_night_hold(3 * 60) && !checkin_night_hold(23 * 3600 + 54 * 60 + 59) &&
                  !checkin_night_hold(-1),
              "the night hold: 23:55:00 to 00:02:59 by the PC's clock, none with no time of day");
    }
}

/// The audit of 2026.11.21 (the owner: "không để các chức năng block nhau"):
/// a timed job gives way for the macro's HP potion on a digit at a safe point
/// (B2, set_yield), and the trash worker sweeps a hold it keeps with ESC
/// itself (B1, sweep_kept). The rules -- bag::drink_yield, bag::kept_sweep_due --
/// and GoBot's window's wiring are tried in pass_keys_test.cpp.
void test_kept_and_yield() {
    section("Bag jobs not blocking the rest (2026.11.21): a pass and a shop round give way between items and "
            "purchases; a kept hold's quantity box under the check-in board swept with ESC");
    if (skip_without_kit()) return;
    const auto gone_count = [](const Pretend& g) {
        return static_cast<int>(
            std::count_if(g.items.begin(), g.items.end(), [](const Pretend::Item& i) { return i.gone(); }));
    };
    {
        // A pass of three stacks, the potion come due as the first goes: it
        // stops before the second, nothing in the bin, no dialog up.
        Pass p;
        make_bag(p.game);
        for (const int s : { 0, 1, 2 }) p.item(s, 7, 2 + s, TrashList::Action::Delete, false, s == 0);
        int asked = 0;
        p.yield = [&] {
            ++asked;
            return gone_count(p.game) >= 1;
        };
        const auto r = p.run();
        const Frame end = p.game.render();
        check(r.yielded && r.deleted == 1 && !r.stuck && asked >= 1 &&
                  r.trouble.find("nhường phím cho macro uống") != std::string::npos &&
                  bag::nothing_held(end, kit().store, p.game.l),
              "a pass: one stack deleted, then given way between two items -- nothing in the bin, no dialog -- (" +
                  std::to_string(r.deleted) + " deleted, asked " + std::to_string(asked) + ") -- " + r.trouble);
        Pass q;
        make_bag(q.game);
        for (const int s : { 0, 1, 2 }) q.item(s, 7, 2 + s, TrashList::Action::Delete, false, s == 0);
        q.yield = [] { return false; };
        const auto all = q.run();
        check(!all.yielded && all.deleted == 3 && all.trouble.empty(),
              "never asked to give way: all three, as ever -- " + all.trouble);
        // The review of 2026.11.21: the give-way due from the run's very
        // start -- a slow bag or page on a laggy PC used kDrinkYieldAfterMs
        // up before the first safe point -- still lets the first item go.
        Pass e;
        make_bag(e.game);
        for (const int s : { 0, 1, 2 }) e.item(s, 7, 2 + s, TrashList::Action::Delete, false, s == 0);
        e.yield = [] { return true; };
        const auto first = e.run();
        check(first.yielded && first.deleted == 1 && !first.stuck,
              "due from the start: the first stack still deleted, then given way (" + std::to_string(first.deleted) +
                  " deleted) -- " + first.trouble);
    }
    {
        // The same for a charm run: its first charm right-clicked and used.
        CharmRun c;
        c.p.yield = [] { return true; };
        c.run();
        check(c.r.charms_used == 1 && c.p.game.right_clicks == 1 && !c.r.stuck,
              "a charm run with the give-way due from the start: its first charm still used (" +
                  std::to_string(c.r.charms_used) + " used) -- " + c.r.trouble);
    }
    if (!kit().shop_ok || !ShopRun(0, 0).ok) {
        skip("the shop's pictures or the item catalog are not installed here\n");
        return;
    }
    {
        // A shop round of three: given way after the first purchase, before
        // the next double-click -- no quantity box up, nothing typed into one.
        ShopRun s(3, 0);
        s.p.yield = [&] { return !s.p.game.buys.empty(); };
        s.run();
        const Pretend& g = s.p.game;
        check(s.r.yielded && s.r.shop_bought == 1 && g.double_clicks == 1 && !g.qty_up && !s.r.stuck &&
                  g.wrong_confirms == 0 && s.r.trouble.find("nhường phím cho macro uống") != std::string::npos,
              "a shop round: one bought, then given way before the next double-click, no box left up (" +
                  std::to_string(s.r.shop_bought) + " bought, " + std::to_string(g.double_clicks) +
                  " double-clicks) -- " + s.r.trouble);
        // The review of 2026.11.21: the give-way due from the start -- the
        // shop opened slower than kDrinkYieldAfterMs -- still buys the first
        // stack before it gives way, so the potions out are bought at last.
        ShopRun e(3, 0);
        e.p.yield = [] { return true; };
        e.run();
        check(e.r.yielded && e.r.shop_bought == 1 && e.p.game.double_clicks == 1 && !e.p.game.qty_up && !e.r.stuck,
              "a shop round with the give-way due from the start: one bought first, then given way (" +
                  std::to_string(e.r.shop_bought) + " bought) -- " + e.r.trouble);
    }
    if (!kit().board_ok) {
        skip("the check-in board's picture is not installed here\n");
        return;
    }
    {
        // The hold kept at 00:00: the whole board over the shop and a quantity
        // box with 100 typed. After 00:03 the worker's sweep: ESC on the board,
        // the box, the shop and the bag, each on a look that shows it -- no
        // digit, no Xác Nhận -- and the screen clear for look_for_clear.
        ShopRun s(1, 0);
        Pretend& g = s.p.game;
        g.shop_on = true;
        g.open_box(true);
        g.typed = "100";
        g.board_on = g.board_whole = true;
        const Frame before = g.render();
        s.p.sweep = true;
        s.p.day_clock = [] { return 3 * 60 + 5; };      // 00:03:05
        s.run();
        const Frame after = g.render();
        check(bag::nothing_held(before, kit().store, g.l) && !bag::shop_left_up(before, kit().store) &&
                  checkin_board_title(before, kit().store),
              "before: only the board's title to be seen, as look_for_clear met it");
        check(s.p.swept_clear && !g.board_on && !g.qty_up && !g.shop_on && g.esc_on_qty == 1 && g.esc_presses >= 3 &&
                  g.buys.empty() && g.wrong_confirms == 0 && g.cancels == 0 && g.digit_keys == 0 &&
                  s.r.esc_swept && bag::nothing_held(after, kit().store, g.l) &&
                  !bag::shop_left_up(after, kit().store) && !checkin_board_title(after, kit().store),
              "after 00:03 the kept hold's sweep: the board, the box, the shop shut by ESC, nothing bought or typed, "
              "the screen clear (" + std::to_string(g.esc_presses) + " ESCs) -- " + s.r.trouble);
    }
}

void test_pet_shop_suite(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_pet_shop();
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
    test_shop_plan();
    test_pet_shop();
    test_checkin_board();
    test_kept_and_yield();
    test_tab_unsure();
    test_tab_band();
    test_tab_unsure_sources();
    test_trash_list_quiet();
    test_pass_speed();
    test_key_window();
    test_potion_break();
    test_own_potions();
    test_nav_keys();
    test_shop_bag();
    test_stuck_esc();
    test_react_wait();
}
