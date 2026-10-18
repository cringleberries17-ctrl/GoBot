// gobot_test -- closing stray popups: the allow-list, what keeps it shut, and
// the pretend game it is tried on.
//
// The pictures are the ones the build stages: GoC's own and the grabbed set
// (copied beside this test from the main checkout's build). The scenes are
// frame.png from the live client -- a plain dark frame where it is not here --
// with popups and dialogs drawn onto it, and the saved frames of the live
// rounds for what must never be touched. Nothing is pressed anywhere: the
// clicks and the ESC go to a pretend game, and nothing reaches the desktop.

#include "gobot/popup_closer.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/quick_quest.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

template <typename... Args>
std::string fmt(const char* f, Args... args) {
    char buf[512];
    std::snprintf(buf, sizeof buf, f, args...);
    return buf;
}

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const char* const kLiveBuild = "C:\\Users\\player\\source\\GoBot\\build\\";

/// Draw `pic` onto `f` at `x`, `y`, its see-through pixels left out.
void blit(Frame& f, const Frame& pic, int x, int y) {
    for (int py = 0; py < pic.height; ++py)
        for (int px = 0; px < pic.width; ++px) {
            const int fx = x + px, fy = y + py;
            if (fx < 0 || fy < 0 || fx >= f.width || fy >= f.height) continue;
            const std::uint8_t* s = &pic.pixels[(static_cast<std::size_t>(py) * pic.width + px) * 4];
            if (s[3] < TemplateMatcher::kOpaque) continue;
            std::uint8_t* d = &f.pixels[(static_cast<std::size_t>(fy) * f.width + fx) * 4];
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        }
}

Frame part(const Frame& f, int x, int y, int w, int h) {
    Frame out;
    out.width = w;
    out.height = h;
    out.pixels.resize(static_cast<std::size_t>(w) * h * 4);
    for (int py = 0; py < h; ++py)
        std::copy_n(&f.pixels[(static_cast<std::size_t>(y + py) * f.width + x) * 4], static_cast<std::size_t>(w) * 4,
                    &out.pixels[static_cast<std::size_t>(py) * w * 4]);
    return out;
}

/// A dark frame with nothing on it.
Frame plain_frame() {
    Frame f;
    f.width = 1280;
    f.height = 720;
    f.pixels.assign(static_cast<std::size_t>(1280) * 720 * 4, 0);
    for (std::size_t i = 0; i < f.pixels.size(); i += 4) {
        f.pixels[i] = 20;
        f.pixels[i + 1] = 22;
        f.pixels[i + 2] = 30;
        f.pixels[i + 3] = 255;
    }
    return f;
}

/// Everything the tests look with, loaded once.
struct Kit {
    TemplateStore store;
    PopupEyes     eyes;
    Frame base;                 ///< frame.png, or a plain dark frame
    bool  live_base = false;
    bool  ok = false;
    std::string why;
    const Frame* get(const char* name) const { return store.get(name); }
};

void load_kit(Kit& x) {
    if (!x.store.load_best(exe_relative("configs/templates.ini"))) {
        x.why = "no pictures";
        return;
    }
    x.eyes.load(x.store, exe_relative("configs"));
    if (!x.eyes.ready(&x.why)) return;
    for (const char* name : { "grabbed/dialog_ok", "grabbed/invite_fail2", "grabbed/hoisinh_confirm",
                              "grabbed/del_item_confirm_1", "grabbed/del_item_confirm_2",
                              "grabbed/party_invite_buttons", "grabbed/chardeadpanel",
                              "grabbed/client_menu_dialog", "grabbed/login_popup1", "inven/1/monsterf",
                              "inven/1/btn_CloseDialog.spr/0", "grabbed/check_in_close" })
        if (!x.store.get(name)) {
            x.why = name;
            return;
        }
    Frame frame;
    for (const std::string& p : { exe_relative("frame.png"), std::string(kLiveBuild) + "bin\\Release\\frame.png" })
        if (!x.live_base && exists(p) && load_png(p, frame) && frame.width == 1280 && frame.height == 720) {
            x.base = frame;
            x.live_base = true;
        }
    if (!x.live_base) x.base = plain_frame();
    x.ok = true;
}

Kit& kit() {
    // Loaded where it stays: the eyes keep a pointer to the store.
    static Kit k;
    static const bool loaded = [] {
        load_kit(k);
        return true;
    }();
    (void)loaded;
    return k;
}

/// A pretend game: a frame with things drawn on it, which a click on a
/// thing's own control takes away, and an ESC that does what the game does.
struct Pretend {
    struct Shown {
        std::string what;
        Frame       pic;
        int         x = 0, y = 0;
        ScreenRect  control;        ///< a click here closes it; empty: nothing does
        bool        esc_closes = false;
    };
    Frame base;
    std::vector<Shown> shown;
    std::uint64_t t = 0;
    int clicks = 0, escs = 0, missed = 0;
    std::vector<std::pair<int, int>> clicked;
    std::vector<std::uint64_t> pressed_at;      ///< when each click and ESC came, on the pretend clock
    /// ESC with nothing it closes brings up the ESC menu, as in the game --
    /// drawn `menu_lag_ms` after the ESC, a busy game being slow to.
    bool esc_opens_menu = true;
    std::uint64_t menu_lag_ms = 0;
    std::uint64_t menu_from = 0;

    bool drawn(const Shown& s) const { return s.what != "menu" || t >= menu_from; }
    Frame render() const {
        Frame f = base;
        for (const Shown& s : shown)
            if (drawn(s)) blit(f, s.pic, s.x, s.y);
        return f;
    }
    void add(const std::string& what, const Frame& pic, int x, int y, ScreenRect control = {},
             bool esc_closes = false) {
        shown.push_back(Shown{ what, pic, x, y, control, esc_closes });
    }
    bool has(const std::string& what) const {
        return std::any_of(shown.begin(), shown.end(), [&](const Shown& s) { return s.what == what; });
    }
    bool click(int x, int y) {
        ++clicks;
        clicked.emplace_back(x, y);
        pressed_at.push_back(t);
        for (std::size_t i = shown.size(); i-- > 0;)
            if (drawn(shown[i]) && shown[i].control.valid() && shown[i].control.contains(x, y)) {
                shown.erase(shown.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        ++missed;
        return true;
    }
    bool esc() {
        ++escs;
        pressed_at.push_back(t);
        for (std::size_t i = shown.size(); i-- > 0;)
            if (shown[i].esc_closes) {
                shown.erase(shown.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        if (esc_opens_menu && !has("menu")) {
            add_menu();
            menu_from = t + menu_lag_ms;
        }
        return true;
    }
    void add_menu() {
        // The ESC menu as the live client draws it: its picture, and its X
        // where after_invite.png shows it.
        const Kit& k = kit();
        const Frame* menu = k.get("grabbed/client_menu_dialog");
        const Frame* x = k.get("inven/1/btn_CloseDialog.spr/0");
        Frame drawn = *menu;
        blit(drawn, *x, PopupEyes::kMenuXDx, PopupEyes::kMenuXDy);
        add("menu", drawn, 547, 238,
            ScreenRect{ 547 + PopupEyes::kMenuXDx, 238 + PopupEyes::kMenuXDy, x->width, x->height });
    }
};

/// A closer on the pretend game, on its clock.
struct Rig {
    InputManager no_window;     // never used: the hands stand in for it
    Pretend      game;
    std::unique_ptr<PopupCloser> closer;
    std::vector<std::string> log;
    /// What one capture costs on the pretend clock: a busy machine.
    std::uint64_t grab_ms = 0;

    explicit Rig(bool plain = false) {
        game.base = plain ? plain_frame() : kit().base;
        closer = std::make_unique<PopupCloser>(no_window, kit().eyes, [this](Frame& f) {
            game.t += grab_ms;
            f = game.render();
            return true;
        });
        PopupCloser::Hands h;
        h.click = [this](int x, int y) { return game.click(x, y); };
        h.esc = [this] { return game.esc(); };
        closer->set_hands(h);
        PopupCloser::Clock c;
        c.now_ms = [this] { return game.t; };
        c.sleep = [this](int ms) { game.t += static_cast<std::uint64_t>(ms); };
        closer->set_clock(c);
        closer->on_report([this](const std::string& line) { log.push_back(line); });
    }
    /// The worker's cadence: a turn a second, for `seconds` -- or one every
    /// `step` seconds, for the long waits in which nothing is to happen.
    std::vector<PopupCloser::Turn> run(int seconds, int step = 1) {
        std::vector<PopupCloser::Turn> turns;
        const std::uint64_t until = game.t + static_cast<std::uint64_t>(seconds) * 1000;
        while (game.t < until) {
            turns.push_back(closer->turn());
            game.t += static_cast<std::uint64_t>(step) * 1000;
        }
        return turns;
    }
    bool said(const std::string& part) const {
        return std::any_of(log.begin(), log.end(), [&](const std::string& l) { return l.find(part) != std::string::npos; });
    }
};

/// The check-in notice where a player would see it, and its Xác Nhận.
void add_notice(Pretend& g, int x = 542, int y = 320) {
    const Frame& n = *kit().get("grabbed/login_popup1");
    g.add("notice", n, x, y,
          ScreenRect{ x + PopupEyes::kNoticeOkDx, y + PopupEyes::kNoticeOkDy, 36, 16 });
}

/// The monster book; with `with_x`, its X near the top-right corner.
void add_book(Pretend& g, bool with_x, int x = 300, int y = 120) {
    const Kit& k = kit();
    Frame book = *k.get("inven/1/monsterf");
    ScreenRect control;
    if (with_x) {
        const Frame& cross = *k.get("inven/1/btn_CloseDialog.spr/0");
        const int cx = book.width - cross.width - 8, cy = 8;
        blit(book, cross, cx, cy);
        control = ScreenRect{ x + cx, y + cy, cross.width, cross.height };
    }
    g.add("book", book, x, y, control, /*esc_closes=*/true);
}

bool skip_without_kit() {
    Kit& k = kit();
    if (k.ok) return false;
    std::printf("  [skip] the pictures are not all installed here: %s\n", k.why.c_str());
    return true;
}

void test_notice() {
    section("Popups: the check-in notice, by its own Xác Nhận (PU-A1)");
    if (skip_without_kit()) return;
    if (!kit().live_base) std::printf("  [info] no frame.png here: drawn on a plain frame\n");
    Rig r;
    add_notice(r.game);
    const auto early = r.run(3);
    check(r.game.clicks == 0 && std::all_of(early.begin(), early.end(), [](PopupCloser::Turn t) {
              return t == PopupCloser::Turn::Waiting;
          }),
          "up for 2 s: nothing yet");
    const auto then = r.closer->turn();
    check(then == PopupCloser::Turn::Closed && r.game.clicks == 1 && r.game.missed == 0 && r.game.escs == 0,
          std::string("at 3 s: closed with exactly one click, inside its Xác Nhận -- ") +
              PopupCloser::turn_name(then));
    check(r.said("closed the check-in notice at 542,320 (words "),
          "logged: " + (r.log.empty() ? std::string("nothing") : r.log.back()));
    r.run(10);
    check(r.game.clicks == 1, "and nothing more once it is gone");

    // A stop that comes while the fresh frame is looked at -- F12, DỪNG,
    // Ctrl+C -- presses nothing (R2A-4).
    {
        Rig s;
        add_notice(s.game);
        bool stop = false;
        PopupCloser::Guard g;
        g.take = [&] {
            stop = true;            // the stop arrives once the hold is taken
            return true;
        };
        g.stopped = [&] { return stop; };
        s.closer->set_guard(g);
        s.run(6);
        check(s.game.clicks == 0 && s.game.escs == 0 && s.game.has("notice"),
              "a stop between the hold and the press: nothing pressed");
    }

    // Moved between looks: the 3 s start again where it is now.
    Rig m;
    add_notice(m.game, 542, 320);
    m.run(2);
    m.game.shown.clear();
    add_notice(m.game, 600, 300);
    m.run(2);
    check(m.game.clicks == 0, "moved after 2 s: still nothing 2 s later");
    m.run(2);
    check(m.game.clicks == 1, "and closed once it has stayed put for 3 s");
}

void test_board_not_listed() {
    section("Popups: the check-in board is not on the list yet (PU-R7)");
    if (skip_without_kit()) return;
    // Its title has never been grabbed from the live client, so nothing tells
    // it -- and its close button alone is no reason to click anything.
    Rig r;
    r.game.add("board's X", *kit().get("grabbed/check_in_close"), 900, 150);
    r.run(8);
    check(r.game.clicks == 0 && r.game.escs == 0, "check_in_close on its own: nothing pressed");
}

void test_book() {
    section("Popups: Yêu Quái Bách Khoa, by its X or else ESC (PU-A3, PU-A8)");
    if (skip_without_kit()) return;
    {
        Rig r;
        add_book(r.game, true);
        r.run(5);
        check(r.game.clicks == 1 && r.game.missed == 0 && r.game.escs == 0 && !r.game.has("book"),
              "with its X: one click, on the X");
    }
    {
        Rig r;
        add_book(r.game, false);
        r.run(5);
        check(r.game.escs == 1 && r.game.clicks == 0 && !r.game.has("book") && !r.game.has("menu"),
              "the X cut away, nothing else up: one ESC");
    }
    {
        // The game took the ESC for the menu instead: the menu GoBot's own ESC
        // brought up is shut again by its X, once.
        Rig r;
        add_book(r.game, false);
        r.game.shown.back().esc_closes = false;
        r.run(5);
        check(r.game.escs == 1 && r.game.clicks == 1 && r.game.missed == 0 && !r.game.has("menu"),
              "an ESC that brought up the ESC menu: the menu shut by its X");
        check(r.said("GoBot's ESC brought up the ESC menu"), "and said so");
        r.run(40);
        check(r.game.escs == 2, "the book still up: tried once more after 30 s");
        // Ten minutes from the first try, which was at 3 s.
        r.run(500, 10);
        check(r.game.escs == 2 && r.said("left Yêu Quái Bách Khoa alone"),
              "then left alone for 10 minutes (PU-A8)");
        r.run(120, 2);
        check(r.game.escs > 2, "and tried again once the 10 minutes are over (" +
                                   std::to_string(r.game.escs) + " ESC in all)");
    }
    {
        // The player opened the ESC menu and then the book from it, and the
        // client left the menu open under the book. GoBot's ESC shuts the
        // book and shows the menu again: the player's, never GoBot's to close.
        Rig r;
        r.game.add_menu();
        add_book(r.game, false);
        r.run(5);
        check(r.game.escs == 1 && r.game.clicks == 0 && !r.game.has("book") && r.game.has("menu"),
              "the player's ESC menu under the book: one ESC for the book, and the menu left up (R2A-3)");
        check(r.said("the ESC menu was already open under"), "and said so");
        r.run(30);
        check(r.game.clicks == 0 && r.game.escs == 1, "nor touched afterwards");
    }
    {
        // The notice up beside the book, and its Xác Nhận not taking: the
        // book's ESC would go to the notice first, so there is none.
        Rig r;
        add_book(r.game, false);
        add_notice(r.game, 900, 420);
        r.game.shown.back().control = ScreenRect{};
        r.run(60);
        check(r.game.escs == 0, "the book without its X, and the notice up too: no ESC, for a minute");
        check(r.game.clicks == 2 && r.game.missed == 2,
              "the notice clicked twice -- at 3 s and after 30 s -- and then left alone");
    }
}

void test_menu_not_ours() {
    section("Popups: the player's own ESC menu is left alone");
    if (skip_without_kit()) return;
    Rig r;
    r.game.add_menu();
    r.run(10);
    check(r.game.clicks == 0 && r.game.escs == 0, "the ESC menu up with no ESC of GoBot's: nothing");
    add_notice(r.game, 900, 420);
    r.run(10);
    check(r.game.clicks == 0, "and the notice beside it is not closed either: the menu's X is a window "
                              "nothing here may touch");
    {
        // The menu with its X not to be seen as the X's picture -- lit under
        // the pointer, say: the menu itself keeps everything shut.
        Rig bare;
        bare.game.add("menu", *kit().get("grabbed/client_menu_dialog"), 547, 238);
        add_notice(bare.game, 900, 420);
        bare.run(10);
        check(bare.game.clicks == 0 && bare.game.escs == 0,
              "the ESC menu without its X beside the notice: nothing closed (the menu, by its picture)");
    }
    Frame shot;
    const std::string after = exe_relative("templates\\grabbed\\after_invite.png");
    if (exists(after) && load_png(after, shot)) {
        Rig real;
        real.game.base = shot;
        real.run(6);
        check(real.game.clicks == 0 && real.game.escs == 0,
              "grabbed/after_invite.png, the live ESC menu: nothing (PU-A6)");
    } else {
        std::printf("  [skip] no grabbed/after_invite.png here\n");
    }
}

void test_blockers() {
    section("Popups: anything else up, and nothing is closed (PU-A4, PU-A5)");
    if (skip_without_kit()) return;
    Kit& k = kit();
    struct Other {
        const char* what;
        const char* picture;
        int x, y;
        /// The [Xác Nhận][Hủy Bỏ] pair drawn under it, this far from its
        /// top-left: the quantity dialog's picture stops above its buttons,
        /// which bag.cpp finds there on the live client.
        int pair_dx = -1, pair_dy = -1;
    };
    const Other others[] = {
        { "the bin's question", "grabbed/del_item_confirm_1", 100, 120 },
        { "the bin's quantity dialog", "grabbed/del_item_confirm_2", 100, 120, 100, 50 },
        { "the revive question", "grabbed/hoisinh_confirm", 100, 120 },
        { "a party invitation", "grabbed/party_invite_buttons", 140, 560 },
        { "the party's refusal notice", "grabbed/invite_fail2", 100, 120 },
        { "the death panel", "grabbed/chardeadpanel", 60, 90 },
    };
    // The notice is drawn last, clear of the rest, so that it is plainly up
    // and nothing but the other thing keeps it open.
    for (const Other& o : others) {
        Rig r;
        r.game.add(o.what, *k.get(o.picture), o.x, o.y);
        if (o.pair_dx >= 0)
            r.game.add("pair", *k.get("grabbed/confirm_button"), o.x + o.pair_dx, o.y + o.pair_dy);
        add_notice(r.game, 900, 420);
        const auto turns = r.run(8);
        check(r.game.clicks == 0 && r.game.escs == 0 &&
                  std::count(turns.begin(), turns.end(), PopupCloser::Turn::Blocked) > 0,
              std::string("the notice with ") + o.what + " up: nothing pressed");
    }
    {
        // The quest window, as the quest tests draw it.
        Rig r;
        Frame quest;
        if (load_png(TemplateStore::default_dir() + "\\quest\\1\\Dlg_Quest_Neo.png", quest) ||
            load_png(TemplateStore::client_images() + "\\quest\\1\\Dlg_Quest_Neo.png", quest)) {
            r.game.add("quest", quest, 40, 60);
            add_notice(r.game, 900, 420);
            r.run(8);
            check(r.game.clicks == 0 && r.game.escs == 0, "the notice with the Nhiệm Vụ window open: nothing");
        } else {
            std::printf("  [skip] no Dlg_Quest_Neo picture here\n");
        }
    }
    {
        // The chat line open, as the chat tests draw it -- on a plain frame,
        // so that it is the only chat bar there is.
        Rig r(true);
        ChatSettings chat;
        const Frame* open = k.get(chat.open_picture.c_str());
        if (open != nullptr) {
            r.game.add("chat", *open, 0, 690);
            add_notice(r.game, 900, 420);
            r.run(8);
            check(r.game.clicks == 0 && r.game.escs == 0, "the notice with the chat line open: nothing");
        } else {
            std::printf("  [skip] no chat bar picture here\n");
        }
    }
    {
        // A window nothing knows, with an X: Kho Cash, a shop.
        Rig r;
        r.game.add("x", *k.get("inven/1/btn_CloseDialog.spr/0"), 1197, 341);
        add_notice(r.game);
        r.run(8);
        check(r.game.clicks == 0, "the notice with a lone close button elsewhere: nothing");
        check(r.said("a window with a close button is up at 1197,341"), "and why is said once");
    }
    // Not the notice: other one-button dialogs alone.
    for (const char* name : { "grabbed/dialog_ok", "grabbed/invite_fail2" }) {
        Rig r;
        r.game.add(name, *k.get(name), 542, 320);
        r.run(8);
        check(r.game.clicks == 0 && r.game.escs == 0, std::string(name) + " alone: nothing (PU-A5)");
    }
    {
        // The notice's frame and button with another dialog's words.
        Frame other = *k.get("grabbed/login_popup1");
        const Frame& fail = *k.get("grabbed/invite_fail2");
        blit(other, part(fail, PopupEyes::kWordsX, PopupEyes::kWordsY, PopupEyes::kWordsW, PopupEyes::kWordsH),
             PopupEyes::kWordsX, PopupEyes::kWordsY);
        Rig r;
        r.game.add("lookalike", other, 542, 320);
        r.run(8);
        check(r.game.clicks == 0, "a one-button dialog with another dialog's words: nothing (PU-A5)");
    }
}

void test_gates() {
    section("Popups: the rest of GoBot, and the player, come first (PU-A7)");
    if (skip_without_kit()) return;
    for (const char* why : { "a hold is on (trash cleaning)", "the trash cleaner holds the game",
                             "the chat line may be open", "the keeper is crossing",
                             "the game is not in front", "the HUD is not up" }) {
        Rig r;
        PopupCloser::Guard g;
        g.gates = [why] { return std::string(why); };
        r.closer->set_guard(g);
        add_notice(r.game);
        r.run(10);
        check(r.game.clicks == 0 && r.said(why), std::string(why) + ": nothing, and put off");
    }
    {
        Rig r;
        int takes = 0, dones = 0;
        PopupCloser::Guard g;
        g.take = [&] { ++takes; return false; };     // somebody at the machine
        g.done = [&] { ++dones; };
        r.closer->set_guard(g);
        add_notice(r.game);
        r.run(20, 2);
        check(r.game.clicks == 0 && takes == 1 && dones == 0,
              "the mouse or keyboard used during the quiet moment: nothing, and not asked again for 30 s");
        r.run(16, 2);
        check(takes == 2 && r.game.clicks == 0, "asked again after the 30 s");
    }
    {
        Rig r;
        int dones = 0;
        PopupCloser::Guard g;
        g.take = [] { return true; };
        g.done = [&] { ++dones; };
        r.closer->set_guard(g);
        add_notice(r.game);
        r.run(6);
        check(r.game.clicks == 1 && dones == 1, "allowed: one click, and the hold let go after it");
    }
}

/// A guard that times the hold on the pretend clock: taken when asked for,
/// the quiet wait standing in for the window's, and let go when done.
struct Timed {
    std::uint64_t taken = 0, gone = 0;
    int takes = 0, dones = 0;
    PopupCloser::Guard guard(Pretend& g, std::uint64_t quiet_ms = 400) {
        PopupCloser::Guard out;
        out.take = [this, &g, quiet_ms] {
            ++takes;
            taken = g.t;
            g.t += quiet_ms;
            return true;
        };
        out.done = [this, &g] {
            ++dones;
            gone = g.t;
        };
        return out;
    }
    std::uint64_t held() const { return gone - taken; }
};

/// The same, every hold kept: from the moment it was asked for to the moment
/// it was let go, on the pretend clock.
struct Holds {
    std::vector<std::uint64_t> held;
    std::uint64_t asked = 0;
    bool on = false;
    PopupCloser::Guard guard(Pretend& g, std::uint64_t quiet_ms = 400) {
        PopupCloser::Guard out;
        out.take = [this, &g, quiet_ms] {
            asked = g.t;
            g.t += quiet_ms;
            on = true;
            return true;
        };
        out.done = [this, &g] {
            if (on) held.push_back(g.t - asked);
            on = false;
        };
        return out;
    }
};

void test_hold() {
    section("Popups: the game is held for at most 2 s, and let go after the click (PU-R5, R2A-2)");
    if (skip_without_kit()) return;
    {
        // Looked at again under the hold only where it was: found there, in
        // a few milliseconds, and nowhere else.
        Pretend p;
        p.base = kit().base;
        add_notice(p);
        const Frame f = p.render();
        const ScreenRect at{ 542, 320, 196, 80 }, elsewhere{ 100, 100, 196, 80 };
        const std::size_t there = kit().eyes.popups(f, false, &at).size();
        const std::size_t not_there = kit().eyes.popups(f, false, &elsewhere).size();
        check(there == 1 && not_there == 0,
              fmt("the notice looked for again round its own place: found (%zu); round another: not (%zu)",
                  there, not_there));
    }
    {
        // Every capture costing 150 ms: the notice clicked, and the hold --
        // the potion keys waiting -- over the moment the click is in.
        Rig r;
        r.grab_ms = 150;
        Timed timed;
        r.closer->set_guard(timed.guard(r.game));
        add_notice(r.game);
        r.run(6);
        check(r.game.clicks == 1 && timed.dones == 1 && timed.held() <= PopupCloser::kHoldMs &&
                  r.game.pressed_at.size() == 1 && timed.gone == r.game.pressed_at[0],
              fmt("the notice: one click %llu ms into the hold, and the hold let go then -- held %llu ms",
                  static_cast<unsigned long long>(r.game.pressed_at.empty() ? 0 : r.game.pressed_at[0] - timed.taken),
                  static_cast<unsigned long long>(timed.held())));
        check(r.said("closed the check-in notice"), "and the look that it is gone, after, holds nothing");
    }
    {
        // A click that does not take: the notice still up for the 1.5 s it is
        // given -- looked at with the game let go all the same.
        Rig r;
        r.grab_ms = 150;
        Timed timed;
        r.closer->set_guard(timed.guard(r.game));
        add_notice(r.game);
        r.game.shown.back().control = ScreenRect{};
        r.run(6);
        check(r.game.clicks == 1 && r.game.missed == 1 && timed.dones == 1 && timed.gone == r.game.pressed_at[0] &&
                  timed.held() <= PopupCloser::kHoldMs && r.said("is still up after closing it"),
              fmt("a click that did not take: the hold let go at the click, %llu ms in, not after the 1.5 s "
                  "looking", static_cast<unsigned long long>(timed.held())));
    }
    {
        // The book with no X, and the ESC bringing up the ESC menu: its X
        // clicked under the same hold, and the hold let go by 2 s.
        Rig r;
        r.grab_ms = 150;
        Timed timed;
        r.closer->set_guard(timed.guard(r.game));
        add_book(r.game, false);
        r.game.shown.back().esc_closes = false;
        r.run(6);
        check(r.game.escs == 1 && r.game.clicks == 1 && !r.game.has("menu") && timed.dones == 1 &&
                  timed.held() <= PopupCloser::kHoldMs && timed.gone >= r.game.pressed_at.back(),
              fmt("an ESC that brought up the menu: its X clicked, and the hold let go %llu ms after it was "
                  "asked for", static_cast<unsigned long long>(timed.held())));
    }
    {
        // The menu that ESC brings up drawn only 1.5 s after it -- seen once
        // the hold was let go: the game taken again, its gates and all, for
        // that one click on its X, and let go at once. Never left up to be
        // taken for the player's (S-RS2-3, F-RF-2).
        Rig r;
        r.grab_ms = 150;
        Holds holds;
        r.closer->set_guard(holds.guard(r.game));
        add_book(r.game, false);
        r.game.shown.back().esc_closes = false;
        r.game.menu_lag_ms = 1500;
        r.run(6);
        bool short_holds = !holds.held.empty();
        std::string each;
        for (const std::uint64_t h : holds.held) {
            short_holds = short_holds && h <= PopupCloser::kHoldMs;
            each += fmt(" %llu ms", static_cast<unsigned long long>(h));
        }
        check(r.game.escs == 1 && r.game.clicks == 1 && r.game.missed == 0 && !r.game.has("menu") &&
                  holds.held.size() == 2 && short_holds && r.said("taken again and closed"),
              "the menu drawn after the hold was let go: closed by its X under a hold taken again -- holds" + each);
    }
    {
        // The same with the moment not right to take the game again -- the
        // player at the keyboard, say: nothing pressed, and the menu left.
        Rig r;
        r.grab_ms = 150;
        Holds holds;
        PopupCloser::Guard g = holds.guard(r.game);
        int gate_asks = 0;
        g.gates = [&] { return ++gate_asks > 1 ? std::string("someone is at the keyboard") : std::string(); };
        r.closer->set_guard(g);
        add_book(r.game, false);
        r.game.shown.back().esc_closes = false;
        r.game.menu_lag_ms = 1500;
        r.run(6);
        check(r.game.escs == 1 && r.game.clicks == 0 && holds.held.size() == 1 && r.said("not the moment"),
              "and with the gates saying no then: the menu left as it is, nothing more pressed");
    }
    {
        // A busy machine: the look under the hold takes 1.3 s. Too late to
        // press and still let go in time -- nothing pressed, put off, and the
        // hold let go at once.
        Rig r;
        Timed timed;
        r.closer->set_guard(timed.guard(r.game));
        add_notice(r.game);
        r.run(3);
        r.grab_ms = 1300;
        r.closer->turn();
        check(r.game.clicks == 0 && timed.takes == 1 && timed.dones == 1 &&
                  timed.held() <= PopupCloser::kHoldMs && r.said("the look before the press took"),
              fmt("the look under the hold taking 1.3 s: nothing pressed, held %llu ms",
                  static_cast<unsigned long long>(timed.held())));
        r.grab_ms = 0;
        r.run(40);
        check(r.game.clicks == 1, "and closed on a later try, once the machine is quicker");
    }
}

void test_real_frames() {
    section("Popups: frames from the live rounds -- nothing is taken for a popup (PU-A6)");
    if (skip_without_kit()) return;
    std::vector<std::string> frames;
    for (const char* folder : { "live\\shots\\", "frozen\\live\\shots\\" }) {
        const std::string dir = std::string(kLiveBuild) + folder;
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA((dir + "*.png").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::vector<std::string> these;
        do {
            const std::string name = fd.cFileName;
            if (name.find("_full") == std::string::npos && name.find("zoom") == std::string::npos)
                these.push_back(dir + name);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        std::sort(these.begin(), these.end());
        // Every sixth: the bag open, Kho Cash, the bin's questions, the lot.
        for (std::size_t i = 0; i < these.size(); i += 6) frames.push_back(these[i]);
    }
    if (frames.empty()) {
        std::printf("  [skip] no live round frames here\n");
        return;
    }
    const PopupEyes& eyes = kit().eyes;
    int quiet = 0, looked = 0;
    std::string loud;
    for (const std::string& p : frames) {
        Frame f;
        if (!load_png(p, f)) continue;
        ++looked;
        // As the worker looks every second: the ESC menu is only looked for
        // straight after GoBot's own ESC.
        const std::vector<PopupSeen> up = eyes.popups(f, false);
        const bool none = up.empty();
        if (none) ++quiet;
        else if (loud.empty()) loud = p.substr(p.find_last_of('\\') + 1);
    }
    check(looked > 100 && quiet == looked,
          fmt("%d of %d saved frames: neither the notice nor the book taken to be up", quiet, looked) +
              (loud.empty() ? std::string{} : " -- not " + loud));
    // A few of them on the pretend game, a turn a second, as the worker does.
    int pressed = 0, run = 0;
    for (std::size_t i = 0; i < frames.size() && run < 8; i += frames.size() / 8 + 1) {
        Frame f;
        if (!load_png(frames[i], f)) continue;
        ++run;
        Rig r;
        r.game.base = f;
        r.run(5);
        pressed += r.game.clicks + r.game.escs;
    }
    check(run > 0 && pressed == 0, fmt("%d of them run for 5 s on the pretend game: %d presses", run, pressed));
}

void test_settings() {
    section("Popups: shipped off (PU-A9)");
#ifdef GOBOT_SOURCE_DIR
    IniFile shipped;
    check(shipped.load(std::string(GOBOT_SOURCE_DIR) + "/configs/app.ini") &&
              shipped.has_key("Simple", "close_popups") && !shipped.get_bool("Simple", "close_popups", true),
          "the shipped app.ini has [Simple] close_popups=false");
    const std::string src = read_all(std::string(GOBOT_SOURCE_DIR) + "/src/popup_closer.cpp");
    check(!src.empty() && src.find("close_ic") == std::string::npos &&
              src.find("\"grabbed/dialog_ok\"") == std::string::npos &&
              src.find("type_text") == std::string::npos && src.find("captcha") == std::string::npos,
          "popup_closer.cpp clicks no generic close_ic button and no blank dialog_ok, types nothing, "
          "and has nothing to do with a captcha");
    const std::string gui = read_all(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/simple_popups.cpp");
    check(!gui.empty() && gui.find("if (!ui.enabled || ui.running) return;") != std::string::npos,
          "the worker does not start while \"Tự đóng thông báo\" is unticked");
#else
    std::printf("  [skip] no source folder\n");
#endif
}

} // namespace

void test_popups(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_notice();
    test_board_not_listed();
    test_book();
    test_menu_not_ours();
    test_blockers();
    test_gates();
    test_hold();
    test_real_frames();
    test_settings();
}
