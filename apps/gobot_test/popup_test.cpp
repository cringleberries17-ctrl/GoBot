// gobot_test -- closing stray popups: the allow-list, what keeps it shut, and
// the pretend game it is tried on.
//
// The pictures are the ones the build stages: GoC's own and the grabbed set
// (copied beside this test from the main checkout's build). The scenes are
// frame.png from the live client as it was saved -- a plain dark frame where
// that is not here -- with popups and dialogs drawn onto it, and the saved
// frames of the live rounds for what must never be touched. Nothing is pressed anywhere: the
// clicks and the ESC go to a pretend game, and nothing reaches the desktop.

#include "gobot/popup_closer.hpp"
#include "gobot/checkin_board.hpp"
#include "gobot/common.hpp"
#include "gobot/config.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/quick_quest.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"
#include "test_support.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
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

/// The build tree of the checkout these tests were built from, where the live
/// tests saved their rounds (live\shots, frozen\...). Never committed, and
/// never written by GoBot as it plays; what GoBot writes is read beside this
/// test, and only as saved (load_live_frame).
#ifdef GOBOT_SOURCE_DIR
const char* const kLiveBuild = GOBOT_SOURCE_DIR "/build/";
#else
const char* const kLiveBuild = "";
#endif

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
    /// The check-in board (checkin_board.hpp): its title and the client's
    /// boxed X, which the board's art wears -- and, when it was handed over
    /// with these tests, the whole board as the client's own art draws it.
    bool  board_ok = false;
    Frame board_title, box_x, board_art;
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
    // frame.png beside this test, only as it was saved: gobot_cli --read
    // rewrites it.
    Frame frame;
    if (load_live_frame(exe_relative("frame.png"), frame) && frame.width == 1280 && frame.height == 720) {
        x.base = frame;
        x.live_base = true;
    }
    if (!x.live_base) x.base = plain_frame();
    // The board: its title and X from the store, the whole of it from the
    // test data -- the client's frame, its "Điểm danh" calendar and its one
    // button, cut from the game's own sprites (no frame of the live game).
    const Frame* title = checkin_board_picture(x.store);
    const Frame* box = x.store.get("inven/1/btn_CloseBox.spr/0");
    if (title != nullptr && box != nullptr) {
        x.board_title = *title;
        x.box_x = *box;
        x.board_ok = true;
        std::vector<std::string> dirs{ exe_relative("testdata\\popups") };
#ifdef GOBOT_SOURCE_DIR
        dirs.push_back(std::string(GOBOT_SOURCE_DIR) + "\\build\\handoff\\popups\\testdata");
#endif
        for (const std::string& d : dirs) {
            Frame art;
            if (load_png(d + "\\checkin_board_878x600.png", art) && art.width == 878 && art.height == 600) {
                x.board_art = art;
                break;
            }
        }
    }
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

/// The monster book as the live client draws it, by the two pictures cut from
/// it (bag::find_book): its first tab at `x`, `y` -- 306,156 on the live
/// client -- and, with `with_x`, its X where the live client has it from the
/// tab. False when the pictures are not here.
bool add_live_book(Pretend& g, bool with_x, int x = 306, int y = 156) {
    const Kit& k = kit();
    const Frame* tab = k.get(bag::kBookTitle);
    const Frame* cross = k.get(bag::kBookClose);
    if (tab == nullptr || cross == nullptr) return false;
    // One picture holding both, the X where it is from the tab.
    Frame book;
    book.width = bag::kBookXDx + cross->width;
    book.height = std::max(tab->height, bag::kBookXDy + cross->height);
    book.pixels.assign(static_cast<std::size_t>(book.width) * book.height * 4, 0);
    blit(book, *tab, 0, 0);
    ScreenRect control;
    if (with_x) {
        blit(book, *cross, bag::kBookXDx, bag::kBookXDy);
        control = ScreenRect{ x + bag::kBookXDx, y + bag::kBookXDy, cross->width, cross->height };
    }
    g.add("book", book, x, y, control, /*esc_closes=*/true);
    return true;
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
    skip("the pictures are not all installed here: %s\n", k.why.c_str());
    return true;
}

/// The check-in board: 878x600 and opaque all over, as the game draws it,
/// its title where the art has it (+298,3 into the board) and -- with
/// `with_x` -- the client's boxed X `x_dx`, `x_dy` from the title, where the
/// art has it unless told otherwise. The rest a flat panel: the title is what
/// GoBot goes by.
Frame board_picture(bool with_x, int x_dx = kCheckinXFromTitle.x, int x_dy = kCheckinXFromTitle.y) {
    const Kit& k = kit();
    Frame b;
    b.width = kCheckinBoardFromTitle.w;
    b.height = kCheckinBoardFromTitle.h;
    b.pixels.assign(static_cast<std::size_t>(b.width) * b.height * 4, 255);
    for (std::size_t i = 0; i < b.pixels.size(); i += 4) {
        b.pixels[i] = 84;
        b.pixels[i + 1] = 78;
        b.pixels[i + 2] = 72;
    }
    const int tx = -kCheckinBoardFromTitle.x, ty = -kCheckinBoardFromTitle.y;
    blit(b, k.board_title, tx, ty);
    if (with_x) blit(b, k.box_x, tx + x_dx, ty + x_dy);
    return b;
}

/// Where the boards here go on the 1280x720 frames: 200,60, the title at
/// 498,63.
constexpr int kBoardX = 200, kBoardY = 60;

/// `board` on the pretend game at `x`, `y`. ESC closes it, as the owner says;
/// so would a click on its X, `x_dx`, `x_dy` from the title, which GoBot
/// never makes (clicked_board_x).
void add_board(Pretend& g, const Frame& board, int x = kBoardX, int y = kBoardY, int x_dx = kCheckinXFromTitle.x,
               int x_dy = kCheckinXFromTitle.y) {
    const int tx = x - kCheckinBoardFromTitle.x, ty = y - kCheckinBoardFromTitle.y;
    g.add("board", board, x, y, ScreenRect{ tx + x_dx, ty + x_dy, kCheckinXFromTitle.w, kCheckinXFromTitle.h },
          /*esc_closes=*/true);
}

/// Any click on the board's X, where add_board put it.
bool clicked_board_x(const Pretend& g, int x = kBoardX, int y = kBoardY, int x_dx = kCheckinXFromTitle.x,
                     int x_dy = kCheckinXFromTitle.y) {
    const int tx = x - kCheckinBoardFromTitle.x, ty = y - kCheckinBoardFromTitle.y;
    const ScreenRect cross{ tx + x_dx, ty + x_dy, kCheckinXFromTitle.w, kCheckinXFromTitle.h };
    return std::any_of(g.clicked.begin(), g.clicked.end(),
                       [&](const std::pair<int, int>& p) { return cross.contains(p.first, p.second); });
}

bool skip_without_board() {
    if (skip_without_kit()) return true;
    if (kit().board_ok) return false;
    skip("the check-in board's title or the boxed X is not installed here\n");
    return true;
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

void test_notice() {
    section("Popups: the check-in notice, by its own Xác Nhận (PU-A1)");
    if (skip_without_kit()) return;
    if (!kit().live_base) std::printf("  [info] no frame.png here, as saved: drawn on a plain frame\n");
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

void test_board() {
    section("Popups: the check-in board, by ESC once it has stayed 3 s, never by its X (PU-B1)");
    if (skip_without_board()) return;
    const Kit& k = kit();
    {
        Rig r;
        add_board(r.game, board_picture(true));
        const auto early = r.run(3);
        check(r.game.clicks == 0 && r.game.escs == 0 &&
                  std::all_of(early.begin(), early.end(),
                              [](PopupCloser::Turn t) { return t == PopupCloser::Turn::Waiting; }),
              "up for 2 s: nothing yet");
        check(r.closer->up(PopupKind::CheckinBoard) &&
                  r.said("the check-in board (Sự kiện và điểm danh) is up at 200,60 (title 1.00 at 498,63; its X at "
                         "+564,+14 from the title)"),
              "seen, and said with its X where the art has it -- " + (r.log.empty() ? std::string() : r.log.front()));
        const auto then = r.closer->turn();
        check(then == PopupCloser::Turn::Closed && r.game.escs == 1 && r.game.clicks == 0 && !r.game.has("board"),
              std::string("at 3 s: exactly one ESC, no click -- ") + PopupCloser::turn_name(then));
        check(r.said("closed the check-in board at 200,60 (title 1.00) with ESC") &&
                  r.closer->last_about(PopupKind::CheckinBoard) &&
                  r.closer->last_vi() == "Đã tắt bảng Sự kiện và điểm danh (bấm ESC).",
              "said: " + (r.log.empty() ? std::string("nothing") : r.log.back()) + " / " + r.closer->last_vi());
        r.run(10);
        check(r.game.escs == 1 && r.game.clicks == 0, "and nothing more once it is gone");
    }
    {
        // The whole board as the client's art draws it -- its calendar, its
        // button, its own X: told by its title, and nothing in it keeps it
        // shut.
        if (!k.board_art.valid()) {
            skip("the client's art of the whole board was not handed over with these tests\n");
        } else {
            Rig r;
            add_board(r.game, k.board_art);
            const auto seen = k.eyes.popups(r.game.render(), false);
            const auto up = std::find_if(seen.begin(), seen.end(),
                                         [](const PopupSeen& p) { return p.kind == PopupKind::CheckinBoard; });
            std::vector<std::string> other;
            if (up != seen.end()) other = k.eyes.blockers(r.game.render(), seen);
            check(seen.size() == 1 && up != seen.end() && up->at.x == kBoardX && up->at.y == kBoardY && up->by_esc &&
                      other.empty(),
                  fmt("the art's board seen at %d,%d (title %.2f), its X %s, and nothing else up%s",
                      up == seen.end() ? -1 : up->at.x, up == seen.end() ? -1 : up->at.y,
                      up == seen.end() ? 0.0 : static_cast<double>(up->score),
                      up != seen.end() && up->control ? fmt("seen at +%d,+%d from the title", up->x - (kBoardX + 298),
                                                            up->y - (kBoardY + 3)).c_str()
                                                      : "not seen",
                      other.empty() ? "" : (": " + other.front()).c_str()));
            r.run(5);
            check(r.game.escs == 1 && r.game.clicks == 0 && !r.game.has("board"),
                  "closed with one ESC, its X never clicked");
        }
    }
    {
        // Moved between looks: the 3 s start again where it is now.
        Rig m;
        add_board(m.game, board_picture(true));
        m.run(2);
        m.game.shown.clear();
        add_board(m.game, board_picture(true), 150, 80);
        m.run(2);
        check(m.game.escs == 0, "moved after 2 s: still nothing 2 s later");
        m.run(2);
        check(m.game.escs == 1 && m.game.clicks == 0, "and one ESC once it has stayed put for 3 s");
    }
    {
        // Its X 20 px off the art's place, still in the board's top band: the
        // board's own, so not a window that keeps it shut (B-1). And no X at
        // all: ESC all the same.
        Rig r;
        add_board(r.game, board_picture(true, 535, 5), kBoardX, kBoardY, 535, 5);
        r.run(5);
        check(r.game.escs == 1 && r.game.clicks == 0 && !clicked_board_x(r.game, kBoardX, kBoardY, 535, 5),
              "its X 20 px aside but in its top band: one ESC, and the X never clicked");
        Rig bare;
        add_board(bare.game, board_picture(false));
        bare.run(5);
        check(bare.game.escs == 1 && bare.game.clicks == 0 && bare.said("no X seen"),
              "no X seen at all: one ESC");
        // An X below the band is some other window's: nothing.
        Rig low;
        add_board(low.game, board_picture(false));
        low.game.add("x", k.box_x, kBoardX + 853, kBoardY + 60);
        low.run(8);
        check(low.game.escs == 0 && low.game.clicks == 0 && low.said("a window with a close button is up"),
              "a close button under the board's top band: nothing pressed, and why is said");
    }
}

void test_board_order() {
    section("Popups: the check-in board's notice first, then the board (PU-B2)");
    if (skip_without_board()) return;
    {
        Rig r;
        add_board(r.game, board_picture(true));
        add_notice(r.game);         // over the board, as the game draws a dialog
        r.run(3);
        check(r.game.clicks == 0 && r.game.escs == 0, "both up for 2 s: nothing yet");
        r.closer->turn();
        check(r.game.clicks == 1 && r.game.missed == 0 && r.game.escs == 0 && !r.game.has("notice"),
              "at 3 s: the notice's Xác Nhận, and no ESC while it is up");
        r.closer->turn();
        check(r.game.escs == 1 && r.game.clicks == 1 && !r.game.has("board") && !clicked_board_x(r.game) &&
                  r.game.pressed_at.size() == 2 && r.game.pressed_at[0] < r.game.pressed_at[1],
              "then the board, by one ESC: the click first, the ESC after, the X never clicked");
    }
    {
        // The book without its X beside the board: an ESC would go to either,
        // so neither is pressed -- the book with its X is closed by it.
        Rig r;
        add_board(r.game, board_picture(true));
        add_book(r.game, false, 30, 120);
        r.run(8);
        check(r.game.escs == 0 && r.game.clicks == 0, "the board and the book without its X: no ESC for either");
    }
    {
        // Not allowed -- a closer told so: the board keeps everything shut,
        // its notice included (B-2); allowed, both go.
        Rig r;
        r.closer->set_allowed(kAllPopups & ~popup_bit(PopupKind::CheckinBoard));
        add_board(r.game, board_picture(true));
        add_notice(r.game);
        const auto turns = r.run(8);
        check(r.game.clicks == 0 && r.game.escs == 0 &&
                  std::count(turns.begin(), turns.end(), PopupCloser::Turn::Blocked) > 0 &&
                  r.said("the check-in board is up, and it is not to be closed now"),
              "the board not allowed: nothing at all, the notice neither, and why is said");
        r.closer->set_allowed(kAllPopups);
        r.run(8);
        check(r.game.clicks == 1 && r.game.escs == 1 && !r.game.has("notice") && !r.game.has("board"),
              "allowed again: the notice, then the board");
    }
}

void test_board_blocked() {
    section("Popups: the check-in board with anything else up: nothing pressed (PU-B3)");
    if (skip_without_board()) return;
    Kit& k = kit();
    struct Other {
        const char* what;
        const char* picture;
        int x, y;
        int pair_dx = -1, pair_dy = -1;
    };
    const Other others[] = {
        { "the bin's question", "grabbed/del_item_confirm_1", 300, 300 },
        { "the bin's quantity dialog", "grabbed/del_item_confirm_2", 300, 300, 100, 50 },
        { "the revive question", "grabbed/hoisinh_confirm", 300, 300 },
        { "a party invitation", "grabbed/party_invite_buttons", 140, 560 },
        { "the party's refusal notice", "grabbed/invite_fail2", 300, 300 },
        { "the death panel", "grabbed/chardeadpanel", 60, 90 },
        { "the bag", "grabbed/inventory", 14, 393 },
        { "a one-button dialog", "grabbed/dialog_ok", 542, 320 },
    };
    // The other thing drawn over the board, as the game draws a dialog.
    for (const Other& o : others) {
        Rig r;
        add_board(r.game, board_picture(true));
        r.game.add(o.what, *k.get(o.picture), o.x, o.y);
        if (o.pair_dx >= 0)
            r.game.add("pair", *k.get("grabbed/confirm_button"), o.x + o.pair_dx, o.y + o.pair_dy);
        const auto turns = r.run(8);
        check(r.game.clicks == 0 && r.game.escs == 0 &&
                  std::count(turns.begin(), turns.end(), PopupCloser::Turn::Blocked) > 0,
              std::string("the board with ") + o.what + " up: nothing pressed");
    }
    {
        Rig r;
        Frame quest;
        if (load_png(TemplateStore::default_dir() + "\\quest\\1\\Dlg_Quest_Neo.png", quest) ||
            load_png(TemplateStore::client_images() + "\\quest\\1\\Dlg_Quest_Neo.png", quest)) {
            add_board(r.game, board_picture(true));
            r.game.add("quest", quest, 40, 60);
            r.run(8);
            check(r.game.clicks == 0 && r.game.escs == 0, "the board with the Nhiệm Vụ window open: nothing");
        } else {
            skip("no Dlg_Quest_Neo picture here\n");
        }
    }
    {
        Rig r(true);
        ChatSettings chat;
        const Frame* open = k.get(chat.open_picture.c_str());
        if (open != nullptr) {
            add_board(r.game, board_picture(true));
            r.game.add("chat", *open, 0, 690);
            r.run(8);
            check(r.game.clicks == 0 && r.game.escs == 0, "the board with the chat line open: nothing");
        } else {
            skip("no chat bar picture here\n");
        }
    }
    {
        // The player's ESC menu over the board: the menu's, not GoBot's, so
        // nothing -- the board's ESC would shut the menu.
        Rig r;
        add_board(r.game, board_picture(true));
        r.game.add_menu();
        r.run(8);
        check(r.game.clicks == 0 && r.game.escs == 0, "the board with the player's ESC menu over it: nothing");
    }
    {
        // A window nothing knows, its X outside the board's top band.
        Rig r;
        add_board(r.game, board_picture(true));
        r.game.add("x", *k.get("inven/1/btn_CloseDialog.spr/0"), 1197, 341);
        r.run(8);
        check(r.game.clicks == 0 && r.game.escs == 0 && r.said("left the check-in board alone: a window with a close "
                                                                 "button is up at 1197,341"),
              "the board with a lone close button elsewhere: nothing, and why is said");
    }
}

void test_board_menu() {
    section("Popups: the check-in board's ESC and the ESC menu; two tries, then left alone (PU-B4)");
    if (skip_without_board()) return;
    {
        // The game takes the ESC for its menu: the menu GoBot's ESC brought
        // up is shut by its X, once; the board is tried once more after 30 s
        // and then left alone for 10 minutes. Its X is never clicked.
        Rig r;
        add_board(r.game, board_picture(true));
        r.game.shown.back().esc_closes = false;
        r.run(5);
        check(r.game.escs == 1 && r.game.clicks == 1 && r.game.missed == 0 && !r.game.has("menu") &&
                  !clicked_board_x(r.game) && r.said("GoBot's ESC brought up the ESC menu") &&
                  r.said("the check-in board is still up after closing it with ESC (try 1 of 2); trying again in 30 s"),
              "an ESC that brought up the menu: the menu shut by its X, the board's X never clicked");
        check(r.closer->last_vi() == "Đã bấm ESC mà bảng Sự kiện và điểm danh vẫn còn.",
              "and the tab says so -- " + r.closer->last_vi());
        r.run(40);
        check(r.game.escs == 2 && r.said("(try 2 of 2); leaving it alone"), "tried once more after 30 s");
        r.run(500, 10);
        check(r.game.escs == 2 && r.game.clicks == 2 && !clicked_board_x(r.game) &&
                  r.said("left the check-in board alone: tried 2 times in 10 minutes") &&
                  r.closer->last_about(PopupKind::CheckinBoard) &&
                  r.closer->last_vi() ==
                      "Bảng Sự kiện và điểm danh vẫn còn sau 2 lần bấm ESC: GoBot để yên, bạn tắt giúp.",
              "then left alone for 10 minutes, and the tab says so -- " + r.closer->last_vi());
    }
    {
        // The player opened the ESC menu, and the board came up over it. The
        // board's ESC shuts the board and shows the menu again: the
        // player's, never GoBot's to close (still_partly_in).
        Rig r;
        r.game.add_menu();
        add_board(r.game, board_picture(true));
        r.run(5);
        check(r.game.escs == 1 && r.game.clicks == 0 && !r.game.has("board") && r.game.has("menu") &&
                  r.said("the ESC menu was already open under the check-in board; left as it is"),
              "the player's ESC menu under the board: one ESC for the board, and the menu left up");
        r.run(30);
        check(r.game.clicks == 0 && r.game.escs == 1, "nor touched afterwards");
    }
}

void test_board_gates() {
    section("Popups: the check-in board waits for the rest of GoBot and the player (PU-B5)");
    if (skip_without_board()) return;
    for (const char* why : { "a hold is on (trash cleaning)", "the trash cleaner holds the game",
                             "the chat line may be open", "the keeper is crossing", "the game is not in front",
                             "the HUD is not up", "HP is low" }) {
        Rig r;
        PopupCloser::Guard g;
        g.gates = [why] { return std::string(why); };
        r.closer->set_guard(g);
        add_board(r.game, board_picture(true));
        r.run(10);
        check(r.game.clicks == 0 && r.game.escs == 0 && r.said(std::string("not closing the check-in board now: ") + why),
              std::string(why) + ": nothing, and put off");
    }
    {
        // A stop between the hold and the press: nothing pressed.
        Rig s;
        add_board(s.game, board_picture(true));
        bool stop = false;
        PopupCloser::Guard g;
        g.take = [&] {
            stop = true;
            return true;
        };
        g.stopped = [&] { return stop; };
        s.closer->set_guard(g);
        s.run(6);
        check(s.game.clicks == 0 && s.game.escs == 0 && s.game.has("board"),
              "a stop between the hold and the press: nothing pressed");
    }
}

void test_board_hold() {
    section("Popups: the check-in board's hold -- at most 2 s, ESC by 1.2 s, a small look under it (PU-B6)");
    if (skip_without_board()) return;
    const Kit& k = kit();
    {
        // Looked for again under the hold only as its title, round where it
        // was: found 8 px off, not 9 -- a search of 296x46, not of the screen.
        Pretend p;
        p.base = k.base;
        add_board(p, board_picture(true));
        const Frame f = p.render();
        const unsigned board = popup_bit(PopupKind::CheckinBoard);
        const ScreenRect at{ kBoardX, kBoardY, 878, 600 };
        const ScreenRect near8{ kBoardX + 8, kBoardY - 8, 878, 600 }, far9{ kBoardX + 9, kBoardY, 878, 600 },
            far9y{ kBoardX, kBoardY + 9, 878, 600 };
        const auto from = std::chrono::steady_clock::now();
        const std::size_t there = k.eyes.popups(f, false, &at, board).size();
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - from)
                            .count();
        std::printf("  [info] the board looked for again where it was: %lld us\n", static_cast<long long>(us));
        check(there == 1 && k.eyes.popups(f, false, &near8, board).size() == 1 &&
                  k.eyes.popups(f, false, &far9, board).empty() && k.eyes.popups(f, false, &far9y, board).empty(),
              "looked for again round its place: found there and 8 px off, not 9 px off either way -- an area of "
              "296x46, under 300x50");
        // The notice's place is not the board's: round it, the board is not
        // looked for at all.
        add_notice(p);
        const ScreenRect notice{ 542, 320, 196, 80 };
        const auto round_notice = k.eyes.popups(p.render(), false, &notice);
        check(round_notice.size() == 1 && round_notice[0].kind == PopupKind::CheckinNotice,
              "round the notice's place: the notice alone");
    }
    {
        // Every capture costing 150 ms: one ESC by 1.2 s into the hold, and
        // the hold let go by 2 s.
        Rig r;
        r.grab_ms = 150;
        Timed timed;
        r.closer->set_guard(timed.guard(r.game));
        add_board(r.game, board_picture(true));
        r.run(6);
        const std::uint64_t esc_at = r.game.pressed_at.empty() ? 0 : r.game.pressed_at[0] - timed.taken;
        check(r.game.escs == 1 && r.game.clicks == 0 && timed.dones == 1 && timed.held() <= PopupCloser::kHoldMs &&
                  esc_at <= PopupCloser::kEscPressByMs,
              fmt("the board: its ESC %llu ms into the hold, the hold let go after %llu ms",
                  static_cast<unsigned long long>(esc_at), static_cast<unsigned long long>(timed.held())));
    }
    {
        // A slow capture under the hold: said in the log with how long it
        // took, and the ESC still by 1.2 s.
        Rig r;
        Timed timed;
        r.closer->set_guard(timed.guard(r.game));
        add_board(r.game, board_picture(true));
        r.run(3);
        r.grab_ms = 650;
        r.closer->turn();
        check(r.said("the re-look under the hold took 650 ms") && r.game.escs == 1 &&
                  timed.held() <= PopupCloser::kHoldMs,
              fmt("a look under the hold of 650 ms: said, one ESC, and the hold let go after %llu ms",
                  static_cast<unsigned long long>(timed.held())));
    }
}

void test_board_lookalikes() {
    section("Popups: nothing else is the check-in board (PU-B7, was PU-R7)");
    if (skip_without_board()) return;
    const Kit& k = kit();
    {
        // The X of the old attendance dialog, and the boxed X, each alone.
        Rig r;
        r.game.add("old X", *k.get("grabbed/check_in_close"), 900, 150);
        r.run(8);
        check(r.game.clicks == 0 && r.game.escs == 0, "check_in_close on its own: nothing pressed");
        Rig x;
        x.game.add("boxed X", k.box_x, 900, 150);
        x.run(8);
        check(x.game.clicks == 0 && x.game.escs == 0, "btn_CloseBox on its own: nothing pressed");
    }
    {
        // The title bar with other words in it: the party's refusal's words
        // over its own.
        Frame other = k.board_title;
        const Frame& fail = *k.get("grabbed/invite_fail2");
        blit(other, part(fail, PopupEyes::kWordsX, PopupEyes::kWordsY, 158, 24), 61, 3);
        Frame body = board_picture(true);
        blit(body, other, -kCheckinBoardFromTitle.x, -kCheckinBoardFromTitle.y);
        Rig r;
        add_board(r.game, body);
        r.run(8);
        check(r.game.clicks == 0 && r.game.escs == 0 && !r.closer->up(PopupKind::CheckinBoard),
              "the board's title bar with other words: not the board, nothing pressed");
    }
    {
        // The owner's own saved frames of the live game, read where they are
        // and never copied (B-7): none shows the board, and none is taken for
        // it.
        int looked = 0, taken = 0;
        std::string which;
        for (const char* folder : { "trash_debug", "quest_debug", "chat_debug", "potion_debug", "vitals_debug" }) {
            const std::string dir = std::string(kLiveBuild) + "bin\\Release\\" + folder;
            std::vector<std::string> names = files_in(dir, "*.png");
            std::sort(names.begin(), names.end());
            for (std::size_t i = 0; i < names.size() && i < 12; ++i) {
                Frame f;
                if (!load_png(dir + "\\" + names[i], f) || !f.valid() || f.width < 640) continue;
                ++looked;
                const auto up = k.eyes.popups(f, false);
                if (std::any_of(up.begin(), up.end(),
                                [](const PopupSeen& p) { return p.kind == PopupKind::CheckinBoard; })) {
                    ++taken;
                    which += std::string(" ") + folder + "\\" + names[i];
                }
            }
        }
        if (looked == 0) skip("no saved frames of the live game here\n");
        else check(taken == 0, fmt("the owner's saved frames: none taken for the board (%d looked at)", looked) + which);
    }
}

void test_board_shots() {
    section("Popups: pictures of the check-in board for the owner (PU-B8)");
    // A day of the worker on a pretend clock: steady milliseconds, the PC's
    // date and its second of the day.
    {
        CheckinShots s;
        const std::uint64_t t0 = 1'000'000;
        s.start(t0, 20261109, 0);
        const auto at = [&](std::uint64_t ms, int day = 20261109, int second = 43'200, bool up = false,
                            bool closed = false, std::uint64_t crossed = 0) {
            const std::vector<std::string> due = s.due(t0 + ms, day, second, up, closed, crossed);
            std::string out;
            for (const std::string& d : due) out += (out.empty() ? "" : " ") + d;
            return out;
        };
        const std::string none = at(14'000);
        const std::string start = at(15'000);
        const std::string again = at(16'000);
        check(none.empty() && start == "start15" && again.empty(),
              "start15: 15 s after BẮT ĐẦU, once -- [" + none + "] [" + start + "] [" + again + "]");
        const std::string first = at(20'000, 20261109, 43'200, true);
        const std::string still = at(21'000, 20261109, 43'201, true);
        const std::string after = at(24'000, 20261109, 43'204, false, true);
        check(first == "first" && still.empty() && after == "after",
              "first: the board's first sight in the day, during its wait; after: once it was closed -- [" + first +
                  "] [" + still + "] [" + after + "]");
        const std::uint64_t crossed = t0 + 100'000;
        const std::string early = at(110'000, 20261109, 43'300, false, false, crossed);
        const std::string in15 = at(115'000, 20261109, 43'305, false, false, crossed);
        const std::string once = at(130'000, 20261109, 43'320, false, false, crossed);
        check(early.empty() && in15 == "in15" && once.empty(),
              "in15: 15 s after the keeper got in, once a crossing -- [" + early + "] [" + in15 + "] [" + once + "]");
        const std::string late = at(200'000, 20261109, 86'399);
        const std::string turned = at(201'000, 20261110, 10);
        const std::string midnight = at(221'000, 20261110, 30);
        const std::string next = at(222'000, 20261110, 31);
        const std::string board_next_day = at(223'000, 20261110, 32, true);
        check(late.empty() && turned.empty() && midnight == "midnight" && next.empty() && board_next_day == "first",
              "midnight: at 00:00:30, once; the board's first sight counted again on the new day -- [" + midnight +
                  "] [" + board_next_day + "]");
    }
    {
        // An arrival whose 15 s were over before the worker started is not
        // this worker's to picture.
        CheckinShots s;
        s.start(500'000, 20261109, 400'000);
        const std::vector<std::string> due = s.due(520'000, 20261109, 1000, false, false, 400'000);
        check(due.size() == 1 && due[0] == "start15", "an arrival long before BẮT ĐẦU: only start15");
    }
    {
        // Kept in a folder of this run's own, the newest 20 of each kind.
        char tmp[MAX_PATH]{};
        const DWORD n = GetTempPathA(MAX_PATH, tmp);
        const std::string dir = (n > 0 && n <= MAX_PATH ? std::string(tmp) : std::string()) + "gobot_test_" +
                                std::to_string(GetCurrentProcessId()) + "\\popup_debug";
        Frame f;
        f.width = 8;
        f.height = 6;
        f.pixels.assign(8 * 6 * 4, 200);
        std::string kept;
        for (int i = 0; i < CheckinShots::kKeep + 3; ++i) kept = keep_popup_frame(dir, f, "in15", false);
        const std::string probe = keep_popup_frame(dir, f, {}, true);
        const auto checkins = files_in(dir, "checkin_*.png");
        const auto probes = files_in(dir, "probe_*.png");
        check(kept.find("\\popup_debug\\checkin_") != std::string::npos &&
                  kept.size() > 9 && kept.compare(kept.size() - 9, 9, "_in15.png") == 0 &&
                  static_cast<int>(checkins.size()) == CheckinShots::kKeep && probes.size() == 1 &&
                  probe.find("\\probe_") != std::string::npos && keep_popup_frame(dir, Frame{}, "x", false).empty(),
              fmt("popup_debug: %zu checkin_ pictures kept of %d, the probe's apart (%zu) -- ", checkins.size(),
                  CheckinShots::kKeep + 3, probes.size()) + kept.substr(kept.find_last_of('\\') + 1));
        for (const std::string& name : files_in(dir, "*.png")) remove_file(dir + "\\" + name);
        RemoveDirectoryA(dir.c_str());
    }
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

void test_live_book() {
    section("Popups: Yêu Quái Bách Khoa as the live client draws it, by its tab and its X (PU-R7)");
    if (skip_without_kit()) return;
    {
        Rig r;
        if (!add_live_book(r.game, true)) {
            skip("no pictures of the live book here\n");
            return;
        }
        const auto seen = kit().eyes.popups(r.game.render(), false);
        check(seen.size() == 1 && seen[0].kind == PopupKind::MonsterBook && seen[0].control && seen[0].x == 816 &&
                  seen[0].y == 169 && seen[0].at.x == 303 && seen[0].at.y == 153,
              fmt("the live book seen, its X at %d,%d", seen.empty() ? -1 : seen[0].x, seen.empty() ? -1 : seen[0].y));
        r.run(5);
        check(r.game.clicks == 1 && r.game.missed == 0 && r.game.escs == 0 && !r.game.has("book") &&
                  r.said("closed Yêu Quái Bách Khoa at 303,153"),
              "closed with one click, on its X");
    }
    {
        // Its X not to be seen -- the mouse button held down on it draws it
        // dark -- and nothing else up: one ESC, as for GoC's picture of it.
        Rig r;
        add_live_book(r.game, false);
        r.run(5);
        check(r.game.escs == 1 && r.game.clicks == 0 && !r.game.has("book"), "its X not seen, nothing else up: one ESC");
    }
    {
        // Beside the bag -- where a fragment's right-click leaves it -- or
        // under the registration question: nothing pressed.
        Rig r;
        add_live_book(r.game, true);
        r.game.add("bag", *kit().get("grabbed/inventory"), 14, 393);
        r.run(8);
        check(r.game.clicks == 0 && r.game.escs == 0 && r.game.has("book"), "the bag open beside it: nothing");
        Rig q;
        add_live_book(q.game, true);
        Frame ask;
        if (TemplateStore::load_one(bag::kRegisterDialog, exe_relative("configs/templates.ini"), ask)) {
            q.game.add("question", ask, 538, 317);
            q.run(8);
            check(q.game.clicks == 0 && q.game.escs == 0 && q.game.has("book"),
                  "the registration question over it: nothing -- never Xác Nhận or Hủy Bỏ, and not the book either");
        }
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
        skip("no grabbed/after_invite.png here\n");
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
            skip("no Dlg_Quest_Neo picture here\n");
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
            skip("no chat bar picture here\n");
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
        skip("no live round frames here\n");
        return;
    }
    const PopupEyes& eyes = kit().eyes;
    int quiet = 0, looked = 0;
    // The recordings of 2026-10-19 with the monster book open, a fragment
    // right-clicked: the book is up on many of their frames, and the bag
    // beside it, or its question over it, on every one of those.
    int book_frames = 0, books = 0, books_blocked = 0;
    std::string loud;
    for (const std::string& p : frames) {
        Frame f;
        if (!load_png(p, f)) continue;
        const std::string name = p.substr(p.find_last_of('\\') + 1);
        // As the worker looks every second: the ESC menu is only looked for
        // straight after GoBot's own ESC.
        const std::vector<PopupSeen> up = eyes.popups(f, false);
        if (name.rfind("fragq_A_", 0) == 0 || name.rfind("book_A_", 0) == 0 || name.rfind("frag_A_", 0) == 0) {
            ++book_frames;
            const bool book = std::any_of(up.begin(), up.end(),
                                          [](const PopupSeen& s) { return s.kind == PopupKind::MonsterBook; });
            const bool notice = std::any_of(up.begin(), up.end(),
                                            [](const PopupSeen& s) { return s.kind != PopupKind::MonsterBook; });
            if (notice && loud.empty()) loud = name;
            if (book) {
                ++books;
                if (!eyes.blockers(f, up).empty()) ++books_blocked;
            }
            continue;
        }
        ++looked;
        const bool none = up.empty();
        if (none) ++quiet;
        else if (loud.empty()) loud = name;
    }
    check(looked > 100 && quiet == looked && loud.empty(),
          fmt("%d of %d saved frames: neither the notice, the check-in board nor the book taken to be up", quiet,
              looked) +
              (loud.empty() ? std::string{} : " -- not " + loud));
    if (book_frames > 0)
        check(books > 0 && books_blocked == books,
              fmt("the book recordings: the live book seen on %d of %d frames looked at, and on every one the bag "
                  "or its question is up beside it, so nothing would be closed (%d)",
                  books, book_frames, books_blocked));
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
    // The frames come from PrintWindow, which shows the game even when it is
    // covered: whether the game is in front and on top at the point is asked
    // as the button goes down, by click_on_game, not a moment before.
    check(src.find("input_.click_on_game(x, y, false, guard_.stopped)") != std::string::npos &&
              src.find("click_at(") == std::string::npos &&
              src.find("WindowFromPoint") == std::string::npos && src.find("GA_ROOT") == std::string::npos,
          "and its one click goes through click_on_game, the stop asked again before the press, with no "
          "check of its own before it");
    const std::string gui = read_all(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/simple_popups.cpp");
    check(!gui.empty() && gui.find("if (!ui.enabled || ui.running) return;") != std::string::npos,
          "the worker does not start while \"Tự đóng thông báo\" is unticked");
#else
    skip("no source folder\n");
#endif
}

#ifdef GOBOT_SOURCE_DIR
/// A source file as it is, line ends as "\n".
std::string source(const char* relative) {
    std::string s = read_all(std::string(GOBOT_SOURCE_DIR) + relative);
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

/// From `head` up to the next line that starts a function at the left edge.
std::string body_of(const std::string& s, const std::string& head) {
    const std::size_t from = s.find(head);
    if (from == std::string::npos) return {};
    const std::size_t end = s.find("\n}\n", from);
    return s.substr(from, end == std::string::npos ? std::string::npos : end - from);
}

std::size_t count_of(const std::string& s, const std::string& part) {
    std::size_t n = 0;
    for (std::size_t at = s.find(part); at != std::string::npos; at = s.find(part, at + 1)) ++n;
    return n;
}
#endif

void test_board_sources() {
    section("Popups: the check-in board in the worker, the quest and the bag jobs (PU-B9)");
#ifdef GOBOT_SOURCE_DIR
    constexpr std::size_t npos = std::string::npos;
    const std::string popups = source("/apps/gobot_gui/simple_popups.cpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    const std::string keeper = source("/apps/gobot_gui/simple_keeper.cpp");
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string quest = source("/apps/gobot_gui/simple_quest.cpp");
    if (popups.empty() || view.empty() || gui.empty() || quest.empty()) {
        skip("the source tree is not here to read\n");
        return;
    }
    // Folded into "Tự đóng thông báo" (the owner, 2026-11-09): no tick of its
    // own, so with it off nothing closes the board; and the worker runs only
    // for a run -- with the bot stopped, a login by "Vào game ngay" included,
    // nothing is pressed.
    const std::string starts = body_of(view, "void GuiApp::simple_start_jobs() {");
    check(popups.find("if (!ui.enabled || ui.running) return;") != npos && count_of(starts, "start_popups();") == 1 &&
              count_of(view, "start_popups()") == 1 && count_of(popups, " start_popups();") == 1 &&
              popups.find("if (ui.enabled && simple_running() && !simple_cold_start_) start_popups();") != npos &&
              keeper.find("start_popups") == npos && gui.find("start_popups(") == npos &&
              popups.find("close_checkin") == npos && view.find("close_checkin") == npos,
          "the board closed only by the \"Tự đóng thông báo\" worker, started only for a run when ticked -- no "
          "close_checkin, nothing from the keeper's one-shot login");
    // A look a second, as before; the pictures for the owner kept between
    // turns, never under a hold; the probe's from its own thread.
    const std::string loop = body_of(popups, "void GuiApp::popup_loop() {");
    const std::size_t turn = loop.find("const PopupCloser::Turn turn = closer.turn();");
    const std::size_t due = loop.find("shots.due(now_ms()");
    const std::size_t keep = loop.find("keep_popup_frame(debug_dir, last, shot, false)");
    const std::size_t take = loop.find("guard.take = [&]() {"), set = loop.find("closer.set_guard(guard);");
    const std::size_t second = loop.find("for (int i = 0; i < 10 && !ui.stop; ++i) "
                                         "std::this_thread::sleep_for(std::chrono::milliseconds(100));");
    check(second != npos && turn != npos && due != npos && keep != npos && take != npos && set != npos &&
              second < turn && turn < due && due < keep && set < turn && count_of(loop, "keep_popup_frame(") == 1,
          "popup_loop: a turn a second; the board's pictures kept after the turn, outside the hold's code");
    const std::string row = body_of(popups, "void GuiApp::simple_popups_row() {");
    const std::size_t async = row.find("ui.probe = std::async(std::launch::async");
    const std::size_t probe = row.find("keep_popup_frame(exe_relative(kPopupDebug), f, {}, true)");
    const std::size_t drawn = row.find("ImGui::EndDisabled();");
    check(async != npos && probe != npos && drawn != npos && async < probe && probe < drawn &&
              count_of(row, "keep_popup_frame(") == 1,
          "Thử xem keeps its picture from its own thread, not the window's");
    check(popups.find("bảng Sự kiện và điểm danh, lúc vào game và lúc qua 00:00: bấm ESC sau khi bảng đứng yên 3 "
                      "giây") != npos &&
              popups.find("Chỉ đóng: thông báo nhận quà điểm danh, bảng Sự kiện và điểm danh (bấm ESC") != npos &&
              popups.find("(sẽ bấm ESC khi bật Tự đóng thông báo)") != npos,
          "the tick's tooltip, its line and Thử xem name the board");
    check(gui.find("    crossed_in_ms_ = now_ms();      // the check-in board comes up now: pictured 15 s on\n"
                   "    return Crossed::In;") != npos,
          "the keeper's arrival kept for the in15 picture");
    // With the tick off the bag jobs and the quest wait for the board on
    // their own, a minute at a time, and say what would close it.
    check(gui.find("\" Bật \\\"Tự đóng thông báo\\\" (thẻ Đánh quái) để GoBot tự tắt bảng điểm danh.\"") != npos &&
              gui.find("charm_check_ms = now_ms() + BagCleaner::kBoardWaitMs;") != npos &&
              gui.find("next = now_ms() + BagCleaner::kBoardWaitMs;") != npos &&
              gui.find("else if (r.board_up) shop_due_ms = now_ms() + BagCleaner::kBoardWaitMs;") != npos,
          "the bag jobs: charms, the pass and the shop put off a minute, with the hint to tick Tự đóng thông báo");
    const std::string round = body_of(quest, "void GuiApp::quest_loop() {");
    const std::size_t looked = round.find("checkin_board_title(seen, pictures.board_title, &title, &score)");
    const std::size_t held = round.find("if (!take_hold(kQuestHold)) {");
    check(looked != npos && held != npos && looked < held &&
              round.find("\"; bật Tự đóng thông báo để GoBot tự tắt bảng điểm danh\"") != npos &&
              round.find("if (r.board) r.why = board_why(board_closes());") != npos,
          "the quest looks for the board before it takes the hold, and says what would close it");
    // As shipped: off, and package.ps1 still refuses a package with it on.
    const std::string ps = source("/package.ps1");
    check(ps.find("@('app.ini', 'Simple', 'close_popups'),") != npos,
          "package.ps1's $mustBeOff still lists [Simple] close_popups");
#else
    skip("no source folder\n");
#endif
}

} // namespace

/// For main.cpp's test of the no-real-input switch, which counts what is
/// refused: a closer with no hands of its own, aimed at `window`, on a
/// check-in notice -- its turns at 0, 1, 2 and 3 s on the pretend clock, the
/// last the one that clicks. What that turn came to, and all the closer said.
/// False, with nothing done, when the pictures are not here.
bool popup_turn_with_no_hands(void* window, std::string& turn, std::string& said) {
    Kit& k = kit();
    if (!k.ok) return false;
    InputManager aimed;
    aimed.set_target_window(window);
    Pretend game;
    game.base = k.base;
    add_notice(game);
    PopupCloser closer(aimed, k.eyes, [&game](Frame& f) {
        f = game.render();
        return true;
    });
    PopupCloser::Clock c;
    c.now_ms = [&game] { return game.t; };
    c.sleep = [&game](int ms) { game.t += static_cast<std::uint64_t>(ms); };
    closer.set_clock(c);
    closer.on_report([&said](const std::string& line) { said += line + "\n"; });
    PopupCloser::Turn last = PopupCloser::Turn::Blind;
    for (int i = 0; i < 4; ++i) {
        last = closer.turn();
        game.t += 1000;
    }
    turn = PopupCloser::turn_name(last);
    return true;
}

void test_popups(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_notice();
    test_board();
    test_board_order();
    test_board_blocked();
    test_board_menu();
    test_board_gates();
    test_board_hold();
    test_board_lookalikes();
    test_board_shots();
    test_book();
    test_live_book();
    test_menu_not_ours();
    test_blockers();
    test_gates();
    test_hold();
    test_real_frames();
    test_settings();
    test_board_sources();
}
