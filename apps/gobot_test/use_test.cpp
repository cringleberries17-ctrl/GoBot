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
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

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
#include <string>
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
        x += g.width + 1;
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
        k.ok = true;
        return true;
    }();
    (void)loaded;
    return k;
}

bool skip_without_kit() {
    if (kit().ok) return false;
    std::printf("  [skip] the dialog pictures or the digits are not installed here: %s\n", kit().why.c_str());
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
    enum class Reply { Register, BinWords, Revive, Other, Notice, Nothing, Consumed };
    struct Item {
        int slot = 0;
        int seed = 1;
        int count = 1;
        Frame ask;              ///< its own registration question, naming it; else the kit's
        bool smudged = false;   ///< something white over its count that is no number
        /// The client's own icon for it, a slot's size, drawn in place of the
        /// made-up drawing of `seed`: a catalog icon as the bag shows it.
        Frame icon;
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
    int drags = 0, bin_confirms = 0, keys = 0, slot_clicks = 0;
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
    Item* at(int x, int y) {
        for (Item& it : items)
            if (!it.gone() && slot_rect(it.slot).contains(x, y)) return &it;
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
        Frame f = base;
        // The scroll bar's thumb the whole length of its track: a tab of four
        // rows at most, at its top -- what the live client shows for one.
        if (thumb)
            fill(f, ScreenRect{ v.strip.x + l.scroll_up_dx, v.strip.y + l.track_top_dy, 1,
                                l.track_bottom_dy - l.track_top_dy + 1 },
                 255, 255, 255);
        for (const Item& it : items) {
            if (it.gone()) continue;
            const ScreenRect s = slot_rect(it.slot);
            if (it.icon.valid())
                blit(f, it.icon, s.x, s.y);
            else
                paint_item(f, s, it.seed);
            if (it.count >= 2) print_number(f, s.x + 3, s.y + 3, std::to_string(it.count), k.font_dir);
            if (it.smudged) fill(f, ScreenRect{ s.x + 4, s.y + 4, 7, 6 }, 250, 250, 250);
        }
        if (up == Up::InBin || up == Up::BinAsking) paint_item(f, ScreenRect{ v.trash.x - 4, v.trash.y - 2, 24, 26 }, 6);
        if (book) {
            blit(f, k.book_tab, kTabX, kTabY);
            const ScreenRect x = book_x_rect();
            if (!book_without_x) blit(f, k.book_x, x.x, x.y);
        }
        if (up == Up::Question || up == Up::Notice) blit(f, shown, kQX, kQY);
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

    Frame look() {
        time.t += std::chrono::milliseconds(40);
        return render();
    }

    BagCleaner::Hands hands() {
        BagCleaner::Hands h;
        h.right_click = [this](int x, int y) {
            ++right_clicks;
            right_at.emplace_back(x, y);
            book_at_right_click.push_back(book);
            events += 'R';
            seq += 'R';
            Item* it = at(x, y);
            if (it == nullptr || up != Up::None) return true;
            const Kit& k = kit();
            about = static_cast<int>(it - items.data());
            switch (reply) {
                case Reply::Register: show(it->ask.valid() ? it->ask : k.question, Up::Question); break;
                case Reply::BinWords: show(*k.store.get("grabbed/del_item_confirm_1"), Up::Question); break;
                case Reply::Revive:   show(*k.store.get("grabbed/hoisinh_confirm"), Up::Question); break;
                case Reply::Other:    show(k.other, Up::Question); break;
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
                const ScreenRect n{ kQX, kQY, shown.width, shown.height };
                if (n.contains(x, y)) ++notice_clicks;
                return true;
            }
            if (up == Up::Question) {
                const ScreenRect p = shown_pair;
                if (!p.contains(x, y)) return true;
                if (x < p.x + p.w / 2) {
                    if (reply == Reply::Register) {
                        ++confirms;
                        if (registers) {
                            --items[static_cast<std::size_t>(about)].count;
                            events += 'U';
                            seq += 'U';
                            left(static_cast<std::size_t>(about));
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
        h.key = [this](std::uint32_t) {
            ++keys;
            return true;
        };
        return h;
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

    Pass() {
        settings.tabs[0] = settings.tabs[1] = settings.tabs[3] = settings.tabs[4] = false;
        settings.tabs[kUseTab] = true;
        // What these tests try is the pass with registering switched on;
        // off, as it ships, is tried on its own (test_switch).
        settings.use_fragments = true;
    }
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
        if (load_png("C:\\Users\\player\\source\\GoBot\\build\\live\\shots\\r3_B_000.png", live)) {
            const BagView w = bag::find(live, k.store, BagLayout{});
            check(w.found && w.selected_tab == kUseTab,
                  "live r3_B_000, the bag on Tiêu Dùng: Tiêu Dùng chosen -- " + std::to_string(w.selected_tab));
        } else {
            std::printf("  [skip] no live\\shots\\r3_B_000.png here\n");
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
        std::printf("  [skip] the item catalog is not installed here\n");
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
        check(p.game.right_clicks == 0 && p.says("không chắc túi đồ đang ở ngăn Tiêu Dùng"),
              "the strip not showing Tiêu Dùng chosen: no right-click -- " + p.last());
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
        const char* file;
        bool other;
        const char* what;
    } frames[] = {
        { "C:\\Users\\player\\source\\GoBot\\build\\live\\shots\\r3_D_015.png", true,
          "live r3_D_015, Kho Cash open beside Tiêu Dùng" },
        { "C:\\Users\\player\\source\\GoBot\\build\\live\\shots\\r3_B_000.png", false,
          "live r3_B_000, the bag alone" },
        { "C:\\Users\\player\\source\\GoBot\\build\\live\\shots\\r1_A_014.png", false,
          "live r1_A_014, the bag alone" },
    };
    for (const auto& fr : frames) {
        Frame live;
        if (!load_png(fr.file, live)) {
            std::printf("  [skip] no %s here\n", fr.file);
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
        check(p.game.right_clicks == 0 && r.used == 0 && p.says("không giống mảnh ghép nào"),
              "not drawn like a fragment: never right-clicked -- " + p.last());
        Pass q;
        make_bag(q.game);
        q.item(1, 4, 6, TrashList::Action::Use, false, true, { "spend/fragment/1103013", "spend/hp/8820131" });
        q.run(true);
        check(q.game.right_clicks == 0 && q.says("nhưng sẽ không dùng: hình này không giống mảnh ghép nào"),
              "drawn like a fragment and a potion both: Thử tìm says it will not be used -- " + q.last());
    }

    // The player's own Tiêu Dùng, on real frames: which slots a cut picture of
    // would be a fragment -- what the bag picture offers "Dùng" for.
    ItemCatalog catalog;
    if (!catalog.load(ItemCatalog::default_dir()) || catalog.find("spend/fragment/1103063") == nullptr) {
        std::printf("  [skip] the item catalog is not installed here\n");
        return;
    }
    const struct {
        const char* file;
        std::vector<int> fragments;     ///< slots holding a fragment
        std::vector<int> others;        ///< potions, scrolls, boxes, books, bowls
        const char* what;
    } frames[] = {
        { "C:\\Users\\player\\source\\GoBot\\build\\live\\shots\\r1_A_014.png", { 6, 7, 10 },
          { 0, 1, 2, 3, 4, 5, 8, 9, 11 }, "live r1_A_014: three fragments, a box, a bundle, potions" },
        { "C:\\Users\\player\\source\\GoBot\\build\\frozen\\bin\\Release\\trash_debug\\20261009_003955_1_no_dialog.png",
          { 11, 13, 14 }, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 15, 16, 17, 18, 19, 20, 21, 22, 23 },
          "a saved pass's frame: fragments among scrolls, chests, books, bowls and potions" },
    };
    for (const auto& fr : frames) {
        Frame live;
        if (!load_png(fr.file, live)) {
            std::printf("  [skip] no %s here\n", fr.file);
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
        std::printf("  [skip] the item catalog is not installed here\n");
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
                                  std::string("C:\\Users\\player\\source\\GoBot\\build\\handoff\\round7\\coder\\testdata") })
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
        std::printf("  [skip] no cuts of the live frames here (testdata\\round7)\n");
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
            std::printf("  [skip] no %s cut here\n", c.what);
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
            std::printf("  [skip] no cuts of the book here\n");
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
        std::printf("  [skip] no cut of the live question here (testdata\\round7)\n");
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
}
