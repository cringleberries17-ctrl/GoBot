// gobot_test -- the game at 1024x768, 1280x720 and 1366x768 (2026.11.13c, the
// owner: "nhớ là cần làm cho đối ứng các độ phân giải: 1080 x 768 [1024 x 768],
// 1280 x 720, 1366 x 768"). Every frame here is made up at the size it stands
// for, from the pictures GoBot ships where they are installed: nothing reads
// the real desktop, and nothing is pressed or clicked.
//
//   - The character select: its "Bắt Đầu Game" half found when the rest is cut
//     off by a frame 720 rows tall, never on the Kết Nối page and never on
//     1024x768; the slots clicked by where the picture is (the 1024x768 block
//     centred or top-left: the offsets; pinned to the corner: the middle -- a
//     bet, the corner's other reading kept as a known limitation) or by
//     login.ini's slot_anchor; the keeper's look at a hung game by the same
//     lookup; a 1366x768 window on a screen 768 rows tall, its bottom read
//     black.
//   - The game off the screen or under the taskbar: which rows and columns,
//     why, the cure, the login's note, and Tự chat's reason.
//   - The channel list's title-offset click only on 1024x768; the display
//     scale said in the login's own words.
//   - The in-world screens at the three sizes: the Level plate, the hotbar,
//     the chat bar, the detector's boxes, the player list, a centred message
//     and the check-in board.
//   - The wiring, by the source: the gates, the kept pictures, client_on_screen
//     as it was.

#include "gobot/auto_chat.hpp"
#include "gobot/auto_login.hpp"
#include "gobot/checkin_board.hpp"
#include "gobot/config.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/player_list.hpp"
#include "gobot/potion_stock.hpp"
#include "gobot/stall_watch.hpp"
#include "gobot/template_matcher.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <windows.h>

using namespace gobot;

namespace {

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_section)(const char*) = nullptr;

void check(bool cond, const std::string& what) { g_check(cond, what); }
void section(const char* name) { g_section(name); }

constexpr std::size_t npos = std::string::npos;

/// `text` contains `part`: for the Vietnamese said.
bool has_text(const std::string& text, const std::string& part) { return text.find(part) != npos; }

struct Size {
    int w, h;
    std::string name() const { return std::to_string(w) + "x" + std::to_string(h); }
};
const Size kSizes[] = { { 1024, 768 }, { 1280, 720 }, { 1366, 768 } };

Frame make_frame(int w, int h) {
    Frame f;
    f.width = w;
    f.height = h;
    f.pixels.assign(static_cast<std::size_t>(w) * h * 4, 255);
    return f;
}

void put_px(Frame& f, int x, int y, int r, int g, int b) {
    if (x < 0 || y < 0 || x >= f.width || y >= f.height) return;
    const std::size_t i = (static_cast<std::size_t>(y) * f.width + x) * 4;
    f.pixels[i] = static_cast<std::uint8_t>(b);
    f.pixels[i + 1] = static_cast<std::uint8_t>(g);
    f.pixels[i + 2] = static_cast<std::uint8_t>(r);
    f.pixels[i + 3] = 255;
}

/// A scene nothing is found in: a gradient, as the login tests use.
Frame backdrop(int w, int h) {
    Frame f = make_frame(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) put_px(f, x, y, x * 255 / w, y * 255 / h, 128);
    return f;
}

/// A picture over a frame, its see-through pixels left out and whatever runs
/// past the frame's edge cut off -- as the game draws a page bigger than its
/// client.
void paste(Frame& into, const Frame& what, int px, int py) {
    for (int y = 0; y < what.height; ++y)
        for (int x = 0; x < what.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * what.width + x) * 4;
            if (what.pixels[i + 3] < TemplateMatcher::kOpaque) continue;
            put_px(into, px + x, py + y, what.pixels[i + 2], what.pixels[i + 1], what.pixels[i]);
        }
}

bool load(const char* name, Frame& out) {
    return TemplateStore::load_one(name, exe_relative("configs/templates.ini"), out) && out.valid();
}

/// A file in this run's own temp folder, which main.cpp makes and removes:
/// see temp_file there.
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

/// How many times `part` is in `text`.
int count_of(const std::string& text, const std::string& part) {
    int n = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++n;
    return n;
}

#ifdef GOBOT_SOURCE_DIR
std::string source(const std::string& rel) {
    std::ifstream in(std::string(GOBOT_SOURCE_DIR) + rel, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// A function's definition in `src`, from `head` to the brace that closes it
/// -- the first at the start of a line -- or "" when there is none.
std::string body(const std::string& src, const std::string& head) {
    const std::size_t from = src.find(head);
    if (from == npos) return {};
    const std::size_t end = src.find("\n}", from);
    return end == npos ? std::string() : src.substr(from, end + 2 - from);
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != npos; }
#endif

const AutoLogin::Page* page_of(const AutoLogin::Settings& s, AutoLogin::Step step) {
    for (const AutoLogin::Page& p : s.pages)
        if (p.step == step) return &p;
    return nullptr;
}

std::string xy(int x, int y) { return std::to_string(x) + "," + std::to_string(y); }

// ------------------------------------------------------------- character select

void test_character_select() {
    section("Game sizes (2026.11.13c): the character select at 1024x768, 1280x720 and 1366x768");
    using A = AutoLogin;
    const A::Settings shipped;
    const A::Page* who = page_of(shipped, A::Step::Character);
    check(who != nullptr && who->row_dx == -518 && who->row_dy == -160 && who->row_step == 202 && who->row_across,
          "the shipped character page: slots from -518,-160 of its picture, 202 apart across");
    if (who == nullptr) return;

    // ---- the rule alone, with no picture ----
    {
        struct Case { int w, h, x, y; A::SlotLayout layout; int sx, sy; const char* what; };
        const Case cases[] = {
            { 1024, 768, 842, 655, A::SlotLayout::Picture, 324, 495, "1024x768, where it was measured" },
            { 1024, 768, 300, 200, A::SlotLayout::Picture, -218, 40, "1024x768 anywhere: as before" },
            { 1366, 768, 1013, 655, A::SlotLayout::Picture, 495, 495, "1366x768, the block centred" },
            { 1366, 768, 1184, 655, A::SlotLayout::Centre, 495, 495, "1366x768, the buttons in the corner" },
            { 1366, 768, 842, 655, A::SlotLayout::Picture, 324, 495, "1366x768, the block top-left" },
            { 1280, 720, 970, 631, A::SlotLayout::Picture, 452, 471, "1280x720, the block centred" },
            { 1280, 720, 1098, 607, A::SlotLayout::Centre, 452, 471, "1280x720, the buttons in the corner" },
            { 1280, 720, 842, 655, A::SlotLayout::Picture, 324, 495, "1280x720, the block top-left" },
            { 1366, 768, 600, 300, A::SlotLayout::Unknown, 82, 140, "1366x768, nowhere known: the offsets" },
        };
        for (const Case& c : cases) {
            const ScreenRect at{ c.x, c.y, 169, 99 };
            bool points = true;
            std::string said;
            for (int k = 0; k < 3; ++k) {
                int x = 0, y = 0;
                A::slot_point(*who, at, c.w, c.h, k, x, y);
                points = points && x == c.sx + 202 * k && y == c.sy;
                said += (k ? " " : "") + xy(x, y);
            }
            check(A::slot_layout(at, c.w, c.h) == c.layout && points,
                  std::string(c.what) + ": layout " + A::slot_layout_name(A::slot_layout(at, c.w, c.h)) +
                      ", slots " + said);
        }
        // Within the slack and no further.
        check(A::slot_layout(ScreenRect{ 1184 + 12, 655 - 12, 169, 99 }, 1366, 768) == A::SlotLayout::Centre &&
                  A::slot_layout(ScreenRect{ 1184 + 13, 655, 169, 99 }, 1366, 768) == A::SlotLayout::Unknown,
              "a place 12 px off a layout's is that layout, 13 px off is not");
        // A player's own offsets keep their meaning in the middle too.
        A::Page moved = *who;
        moved.row_dx = -500;
        moved.row_dy = -150;
        int x = 0, y = 0;
        A::slot_point(moved, ScreenRect{ 1184, 655, 169, 99 }, 1366, 768, 1, x, y);
        check(x == 683 + (842 - 500 - 512) + 202 && y == 384 + (655 - 150 - 384),
              "the middle's slots follow login.ini's row_dx and row_dy as the picture's do -- " + xy(x, y));
        A::slot_point(*who, ScreenRect{ 842, 655, 169, 99 }, 1024, 768, -3, x, y);
        check(x == 324 && y == 495, "a negative slot is the first");

        // The corner's other reading -- a KNOWN LIMITATION, not a proof: the
        // whole 1024x768 block anchored to the bottom-right corner puts the
        // picture in the very same place as the buttons pinned there, the
        // stage beside it, and its slot 1 is then at the offsets (1184-518 on
        // 1366x768, 1098-518,607-160 on 1280x720). The place cannot tell the
        // two apart: auto clicks the middle there, which for that layout is
        // empty stage -- and Bắt Đầu Game would start the character already
        // chosen. login.ini's slot_anchor=picture sets it right.
        const struct { int w, h, x, y, block_x, block_y, mid_x, mid_y; } corners[] = {
            { 1366, 768, 1184, 655, 666, 495, 495, 495 }, { 1280, 720, 1098, 607, 580, 447, 452, 471 } };
        for (const auto& c : corners) {
            const ScreenRect at{ c.x, c.y, 169, 99 };
            const A::SlotClick bet = A::slot_click(*who, at, c.w, c.h, 0);
            A::Page anchored = *who;
            anchored.slot_anchor = A::SlotAnchor::Picture;
            const A::SlotClick set = A::slot_click(anchored, at, c.w, c.h, 0);
            check(bet.layout == A::SlotLayout::Centre && bet.x == c.mid_x && bet.y == c.mid_y &&
                      (bet.x != c.block_x || bet.y != c.block_y) && set.layout == A::SlotLayout::Picture &&
                      set.x == c.block_x && set.y == c.block_y,
                  "known limitation on " + std::to_string(c.w) + "x" + std::to_string(c.h) + ": the picture at " +
                      xy(c.x, c.y) + " is the buttons pinned to the corner or the whole block anchored there; auto "
                      "bets on the first and clicks slot 1 at " + xy(bet.x, bet.y) + ", where the second has it at " +
                      xy(c.block_x, c.block_y) + " -- slot_anchor=picture clicks " + xy(set.x, set.y));
        }
    }

    // ---- login.ini's slot_anchor: the layout set by somebody who has seen it ----
    {
        check(A::slot_anchor_from("auto", A::SlotAnchor::Centre) == A::SlotAnchor::Auto &&
                  A::slot_anchor_from("picture", A::SlotAnchor::Auto) == A::SlotAnchor::Picture &&
                  A::slot_anchor_from("Centre", A::SlotAnchor::Auto) == A::SlotAnchor::Centre &&
                  A::slot_anchor_from("center", A::SlotAnchor::Auto) == A::SlotAnchor::Centre &&
                  A::slot_anchor_from("middle", A::SlotAnchor::Auto) == A::SlotAnchor::Auto &&
                  A::slot_anchor_from("", A::SlotAnchor::Picture) == A::SlotAnchor::Picture &&
                  std::string(A::slot_anchor_name(A::SlotAnchor::Centre)) == "centre" &&
                  who->slot_anchor == A::SlotAnchor::Auto,
              "slot_anchor: auto, picture, centre (or center), any case; a word it does not know is the table's -- "
              "auto as shipped");
        A::Page centre = *who;
        centre.slot_anchor = A::SlotAnchor::Centre;
        A::Page picture = *who;
        picture.slot_anchor = A::SlotAnchor::Picture;
        const ScreenRect top_left{ 842, 655, 169, 99 }, odd{ 600, 300, 169, 99 };
        const A::SlotClick c1 = A::slot_click(centre, top_left, 1366, 768, 0);
        const A::SlotClick c2 = A::slot_click(centre, top_left, 1024, 768, 0);
        const A::SlotClick p1 = A::slot_click(picture, odd, 1366, 768, 1);
        check(c1.layout == A::SlotLayout::Centre && c1.x == 495 && c1.y == 495 && c2.x == 324 && c2.y == 495 &&
                  p1.layout == A::SlotLayout::Picture && p1.x == 600 - 518 + 202 && p1.y == 300 - 160,
              "set, it holds wherever the picture is: centre from the middle (on 1024x768 the same as measured), "
              "picture from the picture -- " + xy(c1.x, c1.y) + ", " + xy(c2.x, c2.y) + ", " + xy(p1.x, p1.y));

        // Read from the page's own section and written back for a slot page
        // only -- the other pages' sections are not given a key they never use.
        const std::string path = temp_path("res_sizes_login.ini");
        A::Settings s;
        for (A::Page& p : s.pages)
            if (p.step == A::Step::Character) p.slot_anchor = A::SlotAnchor::Picture;
        A::Settings back;
        const bool saved = s.save(path) && back.load(path);
        const std::string text = read_all(path);
        const A::Page* got = page_of(back, A::Step::Character);
        bool others_auto = true;
        for (const A::Page& p : back.pages)
            if (p.step != A::Step::Character) others_auto = others_auto && p.slot_anchor == A::SlotAnchor::Auto;
        check(saved && got != nullptr && got->slot_anchor == A::SlotAnchor::Picture && others_auto &&
                  count_of(text, "slot_anchor=") == 1 && has_text(text, "slot_anchor=picture"),
              "slot_anchor=picture in [Page.character] saved and read back, the only slot_anchor line in the file");
        std::remove(path.c_str());
    }

    // ---- the crossing's own sum: the page found, and the picture it was found in ----
    {
        const A::SlotClick none = A::slot_click(*who, ScreenRect{ 842, 655, 169, 99 }, 0, 0, 0);
        check(none.frame_w == 1024 && none.frame_h == 768 && !none.other_size &&
                  none.layout == A::SlotLayout::Picture && none.x == 324 && none.y == 495,
              "no size recorded: taken as 1024x768, slot 1 as measured, nothing kept");
        const A::SlotClick corner = A::slot_click(*who, ScreenRect{ 1184, 655, 169, 99 }, 1366, 768, 0);
        const A::SlotClick third = A::slot_click(*who, ScreenRect{ 1013, 655, 169, 99 }, 1366, 768, 2);
        check(corner.frame_w == 1366 && corner.frame_h == 768 && corner.other_size &&
                  corner.layout == A::SlotLayout::Centre && corner.x == 495 && corner.y == 495 &&
                  third.layout == A::SlotLayout::Picture && third.x == 899 && third.y == 495 && third.other_size,
              "found in a 1366x768 picture: in the corner slot 1 at " + xy(corner.x, corner.y) +
                  ", centred slot 3 at " + xy(third.x, third.y) + ", the screen kept once");
        const A::SlotClick measured = A::slot_click(*who, ScreenRect{ 1184, 655, 169, 99 }, 1024, 768, 0);
        check(measured.layout == A::SlotLayout::Picture && measured.x == 1184 - 518 && !measured.other_size,
              "the same place in a picture recorded as 1024x768 is as before: the offsets");
    }

    // ---- the real pictures ----
    Frame choose, bar, dialog, title;
    if (!load("select_char_indicator", choose) || !load("game_ready_indicator", bar) ||
        !load(A::kChannelPanel, dialog) || !load("channel_first_index", title)) {
        skip("the login's pictures are not installed here\n");
        return;
    }
    check(choose.width == 169 && choose.height == 99 && choose.height > A::kCharStartRows,
          "the character select's picture is 169x99, its Bắt Đầu Game the top " + std::to_string(A::kCharStartRows) +
              " rows");
    TemplateStore store;
    store.put("select_char_indicator", choose);
    store.put("game_ready_indicator", bar);
    store.put(A::kChannelPanel, dialog);
    store.put("channel_first_index", title);

    struct Placed { Size size; int x, y; bool cut; A::SlotLayout layout; int sx, sy; const char* what; };
    const Placed placed[] = {
        { { 1024, 768 }, 842, 655, false, A::SlotLayout::Picture, 324, 495, "1024x768 as measured" },
        { { 1366, 768 }, 1013, 655, false, A::SlotLayout::Picture, 495, 495, "1366x768 centred" },
        { { 1366, 768 }, 1184, 655, false, A::SlotLayout::Centre, 495, 495, "1366x768 in the corner" },
        { { 1366, 768 }, 842, 655, false, A::SlotLayout::Picture, 324, 495, "1366x768 top-left" },
        { { 1280, 720 }, 970, 631, true, A::SlotLayout::Picture, 452, 471, "1280x720 centred, cut by the bottom" },
        { { 1280, 720 }, 842, 655, true, A::SlotLayout::Picture, 324, 495, "1280x720 top-left, cut by the bottom" },
        { { 1280, 720 }, 1098, 607, false, A::SlotLayout::Centre, 452, 471, "1280x720 in the corner" },
    };
    for (const Placed& p : placed) {
        Frame f = backdrop(p.size.w, p.size.h);
        paste(f, choose, p.x, p.y);
        const A::PageHit hit = A::page_found(*who, f, store);
        // As the crossing sums it: the page found, in a picture this size.
        const A::SlotClick s0 = A::slot_click(*who, hit.at, f.width, f.height, 0);
        const A::SlotClick s2 = A::slot_click(*who, hit.at, f.width, f.height, 2);
        const int x0 = s0.x, y0 = s0.y, x2 = s2.x, y2 = s2.y;
        check(hit.found && hit.at.x == p.x && hit.at.y == p.y && hit.at.w == 169 && hit.at.h == 99 &&
                  hit.by_start == p.cut && s0.layout == p.layout && s0.other_size == (p.size.w != 1024) &&
                  x0 == p.sx && y0 == p.sy && x2 == p.sx + 404 && y2 == p.sy,
              std::string(p.what) + ": found at " + xy(hit.at.x, hit.at.y) + (hit.by_start ? " by Bắt Đầu Game" : "") +
                  ", slots 1 and 3 at " + xy(x0, y0) + " and " + xy(x2, y2));
        // The keeper's look at a hung game takes it for a page of the login.
        A::Settings s;
        check(login_page_up(f, s, store), std::string(p.what) + ": the keeper's look sees a page of the login");
    }

    // ---- what is never taken for it ----
    // Plain negatives: the Kết Nối bar along the bottom of a game of another
    // size, whole or cut by the bottom edge, with no character select --
    // never the character select, wherever it is. What keeps it out is the
    // half's own score (kCharStartScore): the 50 rows of "Bắt Đầu Game" do not
    // fit on a bar 42 rows tall, and the Kết Nối guard cannot see a cut bar
    // anyway. The guard itself is shown by the frame with both, below.
    for (const Size& size : { Size{ 1280, 720 }, Size{ 1366, 768 } }) {
        bool none = true, none_cut = true;
        for (const int x : { 20, 400, size.w / 2 - 123, size.w - 260 }) {
            Frame f = backdrop(size.w, size.h);
            paste(f, bar, x, size.h - 42);
            none = none && !A::page_found(*who, f, store).found;
            for (const int under : { 30, 20 }) {
                Frame cut = backdrop(size.w, size.h);
                paste(cut, bar, x, size.h - under);
                none_cut = none_cut && !A::page_found(*who, cut, store).found;
            }
        }
        check(none, "on " + size.name() + " the Kết Nối bar at the bottom, here and there, is not the character "
                    "select");
        check(none_cut, "on " + size.name() + " nor is the bar cut by the bottom edge, 30 or 20 of its rows in "
                        "view, here and there");
    }
    {
        // Both: the character select cut off, the Kết Nối bar up beside it --
        // a frame the half could be taken from, refused (the Kết Nối guard:
        // without it this frame's half would be taken).
        Frame f = backdrop(1280, 720);
        paste(f, choose, 970, 631);
        paste(f, bar, 100, 100);
        const A::PageHit hit = A::page_found(*who, f, store);
        check(!hit.found, "a cut character select with the Kết Nối bar in the same frame is not taken (its Kết "
                          "Thúc is never clicked)");
        // On 1024x768 the half is never tried: a picture cut off there is not
        // the page it was measured on.
        Frame low = backdrop(1024, 768);
        paste(low, choose, 842, 700);
        check(!A::page_found(*who, low, store).found, "on 1024x768 a cut character select is not found by its half");
        // Nor on a page other than the character select's.
        A::Page other = *who;
        other.step = A::Step::Connect;
        Frame cut = backdrop(1280, 720);
        paste(cut, choose, 970, 631);
        check(!A::page_found(other, cut, store).found, "and only the character select is ever found by its half");
        // Whole and in view: found by the whole picture, not the half.
        Frame whole = backdrop(1280, 720);
        paste(whole, choose, 500, 300);
        const A::PageHit seen = A::page_found(*who, whole, store);
        check(seen.found && !seen.by_start && seen.at.x == 500 && seen.at.y == 300,
              "a character select all in view is found whole, as before");
    }

    // ---- a 1366x768 window on a screen 768 rows tall ----
    // The taskbar hidden, nothing over the game, so its frames are trusted --
    // but the client's rows from 737 down are past the screen's edge and read
    // black: the bottom 17 of the picture's 99 rows, centred (1013,655) or
    // top-left (842,655). The half is not looked for there (the picture would
    // not run past the frame's edge). What happens is pinned as it is: the
    // whole picture still scores about 0.90 against the page's 0.70, so it is
    // found whole and its slots are as with nothing black. Were it not, the
    // login's advice says the bottom of the game is hidden (fit_note).
    for (const int x : { 1013, 842 }) {
        Frame f = backdrop(1366, 768);
        paste(f, choose, x, 655);
        for (int y = 737; y < 768; ++y)
            for (int c = 0; c < 1366; ++c) put_px(f, c, y, 0, 0, 0);
        const A::PageHit hit = A::page_found(*who, f, store);
        const A::SlotClick s0 = A::slot_click(*who, hit.at, f.width, f.height, 0);
        check(hit.found && !hit.by_start && hit.at.x == x && hit.at.y == 655 && hit.score >= 0.85f &&
                  s0.layout == A::SlotLayout::Picture && s0.x == x - 518 && s0.y == 495,
              "1366x768 with its bottom 31 rows black past the screen's edge, the picture at " + xy(x, 655) +
                  ": still found whole (score " + std::to_string(hit.score) + "), slot 1 at " + xy(s0.x, s0.y));
    }

    // ---- the keeper's look: the channel list by its panel too ----
    {
        A::Settings s;
        Frame f = backdrop(1280, 720);
        paste(f, dialog, 460, 190);
        check(login_page_up(f, s, store), "the keeper's look sees the channel list by its panel alone, its title "
                                          "not found");
        check(!login_page_up(backdrop(1366, 768), s, store), "and nothing on a scene with no page of the login");
    }
}

// ------------------------------------------------------- the game off the screen

void test_fit() {
    section("Game sizes (2026.11.13c): the game off the screen or under the taskbar");
    using A = AutoLogin;
    using Box = A::Box;
    const std::vector<Box> laptop{ Box{ 0, 0, 1366, 768 } };
    const Box shown_bar{ 0, 728, 1366, 768 };
    const Box hidden_bar{ 0, 766, 1366, 806 };

    // A 1366x768 game in a window on a 1366x768 screen: 31 rows past the
    // bottom, 40 more under the taskbar.
    A::Fit f = A::fit_of(Box{ 0, 31, 1366, 799 }, laptop, { shown_bar });
    check(f.hidden_bottom == 71 && f.off_screen && f.taskbar && f.hidden_right == 0 && f.screen_h == 768 &&
              f.width() == 1366 && f.height() == 768,
          "1366x768 at 0,31 on a 768-px screen with the taskbar shown: the bottom 71 rows, both reasons -- " +
              std::to_string(f.hidden_bottom));
    const std::string cure = A::fit_advice(f);
    check(has_text(cure, "71 hàng dưới cùng") && has_text(cure, "bị thanh tác vụ che và nằm ngoài màn hình") &&
              has_text(cure, "Màn hình cao 768") && has_text(cure, "1280x720") && has_text(cure, "tự ẩn"),
          "and what to do: the screen is 768 tall, so 1280x720 with the taskbar hidden -- \"" + cure + "\"");
    f = A::fit_of(Box{ 0, 31, 1366, 799 }, laptop, { hidden_bar });
    check(f.hidden_bottom == 31 && f.off_screen && !f.taskbar,
          "the same with the taskbar hidden (2 px left of it): 31 rows, off the screen only");
    f = A::fit_of(Box{ 0, 31, 1280, 751 }, laptop, { shown_bar });
    check(f.hidden_bottom == 23 && !f.off_screen && f.taskbar,
          "1280x720 at 0,31 with the taskbar shown: its bottom 23 rows under the taskbar -- " +
              std::to_string(f.hidden_bottom));
    const std::string bar_cure = A::fit_advice(f);
    check(f.taskbar_px == 40 && has_text(bar_cure, "23 hàng dưới cùng") && has_text(bar_cure, "bị thanh tác vụ che,") &&
              has_text(bar_cure, "Taskbar settings") && !has_text(bar_cure, "Màn hình cao") &&
              !has_text(bar_cure, "kéo game"),
          "and the cure is the taskbar hidden -- not the game higher: 720 rows, 31 of title bar and the 40 px "
          "taskbar do not fit in 768 -- \"" + bar_cure + "\"");
    f = A::fit_of(Box{ 100, 300, 1124, 1068 }, { Box{ 0, 0, 1920, 1080 } }, { Box{ 0, 1032, 1920, 1080 } });
    check(f.hidden_bottom == 36 && f.taskbar && f.taskbar_px == 48 &&
              has_text(A::fit_advice(f), "hoặc kéo game lên cao hơn."),
          "on a 1920x1080 screen there is room above the taskbar: the game higher is said too");
    f = A::fit_of(Box{ 0, 31, 1280, 751 }, laptop, { hidden_bar });
    check(f.hidden_bottom == 0 && A::fit_advice(f).empty(), "1280x720 with the taskbar hidden fits: nothing said");
    f = A::fit_of(Box{ 700, 300, 1724, 1068 }, { Box{ 0, 0, 2560, 1440 } }, { Box{ 0, 1392, 2560, 1440 } });
    check(f.hidden_bottom == 0 && f.hidden_right == 0 && f.hidden_top == 0 && f.hidden_left == 0 &&
              A::fit_advice(f).empty(),
          "1024x768 on the owner's 2560x1440 screen: nothing hidden");
    f = A::fit_of(Box{ 0, 0, 1366, 768 }, laptop, {});
    check(f.hidden_bottom == 0 && f.hidden_right == 0 && !f.hides(),
          "1366x768 at 0,0 with no taskbar above it -- the rule alone: nothing hidden (whether a game in Full Mode "
          "is reported is game_fit's z-order walk, not tried live)");
    f = A::fit_of(Box{ 1, 0, 1367, 700 }, laptop, {});
    check(f.hidden_bottom == 0 && f.hidden_right == 1 && A::fit_advice(f).empty(),
          "a game a pixel past the right edge: that column, under what is said -- not every row");
    f = A::fit_of(Box{ 400, 31, 1424, 799 }, laptop, { shown_bar });
    check(f.hidden_right == 58 && f.hidden_bottom == 71 && has_text(A::fit_advice(f), "71 hàng"),
          "dragged right as well: the rows first, then the columns of the rows still seen");
    f = A::fit_of(Box{ 0, 31, 1024, 799 }, { Box{ 0, 0, 1024, 768 } }, { Box{ 0, 0, 60, 768 } });
    check(f.hidden_bottom == 31 && f.off_screen && !f.taskbar && f.hidden_left == 60,
          "a taskbar standing up the side hides no row at the bottom -- its 60 columns at the left, said after the "
          "rows, which are off the screen only");
    check(A::fit_reason(A::Fit{}, "x").empty() && A::fit_advice(A::Fit{}).empty(),
          "no window, no fit: nothing said");
    f = A::fit_of(Box{ 0, 300, 1280, 1020 }, laptop, {});
    check(f.hidden_bottom == 252 && f.off_screen && has_text(A::fit_advice(f), "kéo cửa sổ game vào trong màn hình"),
          "a 1280x720 game dragged down past the edge, short enough for the screen: drag it back in");

    // The top and the left, where the HUD's bars are.
    f = A::fit_of(Box{ 0, 31, 1280, 751 }, laptop, { Box{ 0, 0, 1366, 40 } });
    const std::string top_cure = A::fit_advice(f);
    check(f.hidden_top == 9 && f.hidden_bottom == 0 && f.taskbar && has_text(top_cure, "9 hàng trên cùng") &&
              has_text(top_cure, "máu") && has_text(top_cure, "tự ẩn") && !has_text(top_cure, "kéo game"),
          "the taskbar along the top over a 1280x720 game at 0,31: its top 9 rows, the HP bars, no room to move "
          "it -- \"" + top_cure + "\"");
    f = A::fit_of(Box{ 100, 31, 1124, 799 }, { Box{ 0, 0, 1920, 1080 } }, { Box{ 0, 0, 1920, 48 } });
    check(f.hidden_top == 17 && has_text(A::fit_advice(f), "hoặc kéo game xuống thấp hơn."),
          "and on a 1920x1080 screen, room below it: the game lower is said too");
    f = A::fit_of(Box{ -100, 31, 924, 699 }, laptop, {});
    const std::string left_cure = A::fit_advice(f);
    check(f.hidden_left == 100 && f.hidden_bottom == 0 && f.off_screen && has_text(left_cure, "100 cột bên trái") &&
              has_text(left_cure, "kéo cửa sổ game vào trong màn hình"),
          "a game dragged past the left edge: its left 100 columns -- \"" + left_cure + "\"");

    // Frames the game draws itself (PrintWindow): what is hidden is still read.
    f = A::fit_of(Box{ 0, 31, 1280, 751 }, laptop, { shown_bar });
    const std::string drawn = A::fit_advice(f, false);
    check(has_text(drawn, "23 hàng dưới cùng") && has_text(drawn, "tự vẽ") && !has_text(drawn, "nên GoBot không") &&
              has_text(drawn, "tự ẩn"),
          "the game drawing itself for GoBot: the rows are said, not what stops -- nothing does -- \"" + drawn + "\"");

    // The login's note, for Kết Nối or the character select never found.
    f = A::fit_of(Box{ 0, 31, 1366, 799 }, laptop, { hidden_bar });
    const std::string note = A::fit_note(A::Why::Character, f);
    check(has_text(note, "31 hàng dưới cùng") && has_text(note, "Bắt Đầu Game") &&
              has_text(note, "Game không nằm trọn trên màn hình") && !A::fit_note(A::Why::Connect, f).empty() &&
              A::fit_note(A::Why::World, f).empty() && A::fit_note(A::Why::Character, A::Fit{}).empty() &&
              has_text(A::fit_advice(f), "Tự đăng nhập"),
          "Tự đăng nhập with the bottom of the game past the edge: Kết Nối and the character select's reasons say "
          "so, nothing else's -- \"" + note + "\"");

    // Tự chat's reason (chat_gate): the bar not seen shut, and the bottom of
    // the game hidden -- said, not "not found".
    ChatGateInput in;
    in.window = in.focused = true;
    in.looked = in.hud = true;
    in.hp_pct = 90.0f;
    in.bar = ChatBarState::Unknown;
    const ChatGate plain = chat_gate(in);
    in.unseen = "71 hàng dưới cùng của game bị thanh tác vụ che, nên GoBot không thấy ô chat để Tự chat.";
    const ChatGate hidden = chat_gate(in);
    check(!plain.send && has_text(plain.why, "Chưa thấy ô chat") && !hidden.send && hidden.why == in.unseen &&
              hidden.wait_ms == plain.wait_ms,
          "Tự chat with the bottom of the game hidden: the line waits as before and says why");
    in.bar = ChatBarState::Closed;
    check(chat_gate(in).send, "and with the bar seen shut the reason does not hold a line back");
}

// ----------------------------------------------- the channel list, display scale

void test_channel_and_scale() {
    section("Game sizes (2026.11.13c): the channel list's offsets on 1024x768 only, the display scale in the "
            "login's words");
    using A = AutoLogin;
    using Why = A::Why;
    check(A::advice(Why::Channel) == "Không thấy danh sách kênh (Chọn Kênh)." &&
              A::advice(Why::Channel, "60") == A::advice(Why::Channel) &&
              A::advice(Why::Channel, "explorer.exe") == A::advice(Why::Channel) &&
              A::advice(Why::Channel, "1280x") == A::advice(Why::Channel) &&
              A::advice(Why::Channel, "x720") == A::advice(Why::Channel) &&
              A::advice(Why::Channel, "channel_list") == A::advice(Why::Channel),
          "the channel list never seen: said as before -- a detail with an x in it that is not digits x digits "
          "included");
    const std::string sized = A::advice(Why::Channel, "1280x720");
    check(has_text(sized, "1280x720") && has_text(sized, "không bấm") && has_text(sized, "login_debug"),
          "found by its title on a 1280x720 game with no panel: nothing clicked, and what to send -- \"" + sized +
              "\"");
    const std::string note = A::game_scale_note(125);
    check(has_text(note, "125%") && has_text(note, "Sửa cho game") && has_text(note, "100%"),
          "the game enlarged by Windows, said in the login's own words -- \"" + note + "\"");
#ifdef GOBOT_SOURCE_DIR
    const std::string login = source("/src/auto_login.cpp");
    const std::string choose = body(login, "AutoLogin::Click AutoLogin::choose_channel(");
    const std::size_t only = choose.find("if (f.width != kMeasuredW || f.height != kMeasuredH) {");
    const std::size_t kept = choose.find("keep_frame(\"channel_\" + seen);", only == npos ? 0 : only);
    const std::size_t offsets = choose.find("x = title_x;");
    check(only != npos && kept != npos && offsets != npos && only < kept && kept < offsets &&
              has(choose.substr(only, offsets - only), "return Click::NotTaken;"),
          "auto_login.cpp: the title's offsets clicked only on a 1024x768 game; on another, a picture kept and "
          "nothing clicked");
    const std::string said = body(login, "std::string AutoLogin::advice() const {");
    check(has(said, "game_scale_pct_ > 100") && has(said, "game_scale_note(game_scale_pct_)") &&
              has(said, "why_ == Why::Character") && has(said, "why_ == Why::World"),
          "auto_login.cpp: the login's own reasons for the game's pages name the display scale");
    const std::string grab = body(login, "bool AutoLogin::grab(On which, void* hwnd, Frame& out, bool& trusted) {");
    check(has(grab, "game_scale_pct_ = shown.stretched() && shown.percent > 100 ? shown.percent : 0;"),
          "auto_login.cpp: how Windows shows the game asked as it is pictured");
    check(has(said, "fit_note(why_, login_fit_)") &&
              has(grab, "if (which == On::Game) login_fit_ = capture.from_screen() ? game_fit(hwnd) : Fit{};"),
          "auto_login.cpp: the login's own reasons say the bottom of the game was hidden, as it was last read off "
          "the screen");
    const std::size_t slot_at = login.find("} else if (page.pick == Pick::Slot) {");
    const std::string slot = slot_at == npos ? std::string() : login.substr(slot_at, 4500);
    check(has(slot, "const SlotClick slot = slot_click(page, at, found_w_, found_h_, choice);") &&
              has(slot, "done = press(sx, sy);") && has(slot, "keep_frame(\"character_\" + size);") &&
              has(slot, "if (slot.other_size && !character_kept_) {") && has(slot, "(layout: \" +") &&
              has(slot, "\", slot_anchor=\" +") && has(slot, "set slot_anchor=picture in [Page.%s] of login.ini"),
          "auto_login.cpp: the slots by slot_click from the size await() recorded, the screen kept once a crossing "
          "at another size, the layout and slot_anchor in the log, the corner's bet warned of");
    // The wiring slot_click hangs on: await() records the size of the picture
    // the page was found in before it says the page is up, and run() clears
    // it -- without them every crossing would be summed as on 1024x768.
    const std::string awaits = body(login, "AutoLogin::Seen AutoLogin::await(");
    const std::size_t found_at = awaits.find("const PageHit hit = page_in(index, mine);");
    const std::size_t fw_at = found_at == npos ? npos : awaits.find("found_w_ = mine.width;", found_at);
    const std::size_t fh_at = found_at == npos ? npos : awaits.find("found_h_ = mine.height;", found_at);
    const std::size_t up_at = found_at == npos ? npos : awaits.find("return Seen::Page;", found_at);
    const std::string runs = body(login, "bool AutoLogin::run() {");
    check(found_at != npos && fw_at != npos && fh_at != npos && up_at != npos && fw_at < up_at && fh_at < up_at &&
              has(runs, "found_w_ = found_h_ = 0;") && has(runs, "character_kept_ = false;") &&
              has(runs, "login_fit_ = Fit{};"),
          "auto_login.cpp: await() records found_w_/found_h_ from the picture the page was found in, before "
          "Seen::Page; run() clears them, the kept flag and the fit");
    const std::string stall = body(source("/src/stall_watch.cpp"), "bool login_page_up(");
    check(has(stall, "AutoLogin::page_found(page, f, pages)") && !has(stall, "TemplateMatcher::find"),
          "stall_watch.cpp: the keeper's look at a hung game goes by the crossing's own lookup");
#endif
}

// ----------------------------------------------------------- the sweep

void test_sweep() {
    section("Game sizes (2026.11.13c): the in-world screens at 1024x768, 1280x720 and 1366x768");

    // What follows the frame's size by itself.
    for (const Size& s : kSizes) {
        const DetectorTuning t = DetectorTuning{}.resolved(s.w, s.h);
        check(t.area.x == 0 && t.area.y == 104 && t.area.w == s.w && t.area.h == s.h - 104 - 98 &&
                  t.exclude.x == s.w - 212 && t.exclude.w == 212 && t.exclude.h == 360 &&
                  t.exclude2.x == s.w - 40 && t.exclude2.y == s.h - 200,
              "the monster detector's boxes on " + s.name() + ": the play field " + xy(t.area.w, t.area.h) +
                  ", the notices and the icons at the right edge");
        PlayerList list;
        list.set_settings(PlayerList::Settings{});
        list.place(s.w - 68, 139, s.w, s.h);
        const PlayerList::Settings& g = list.geometry();
        const int row_y = g.first_row_y + g.row_height / 2;
        check(g.roi_x + g.roi_w <= s.w && g.roi_y + g.roi_h <= s.h && list.in_panel(g.click_x, row_y) &&
                  list.in_panel(g.click_x + g.invite_dx, row_y + g.invite_dy) && s.w - g.roi_x == 1024 - 744,
              "the player list on " + s.name() + " hangs off its button at " + xy(s.w - 68, 139) +
                  ", its first row and its Req Party inside it");
    }

    Frame plate, hotbar_pic, ok_box, pair, board;
    TemplateStore chat_store;
    ChatSettings chat;
    const bool have = load("hpmp_indicator2", plate) && load(hotbar::kPicture, hotbar_pic) &&
                      load("grabbed/dialog_ok", ok_box) && load("grabbed/confirm_button", pair) &&
                      load(kCheckinBoardTitleFile, board) && chat_store.load_best(exe_relative("configs/templates.ini")) &&
                      can_look_for_chat(chat_store, chat) && chat_store.get(chat.closed_picture) != nullptr;
    if (!have) {
        skip("the HUD's, the hotbar's, the message's, the board's or the chat bar's pictures are not installed here\n");
        return;
    }
    TemplateStore message;
    message.put(AutoLogin::kMessageNotice, ok_box);
    message.put(AutoLogin::kMessageButtons, pair);
    const struct { Size s; int hx, hy; } bars[] = {
        { { 1024, 768 }, 413, 673 }, { { 1280, 720 }, 469, 625 }, { { 1366, 768 }, 512, 673 } };
    for (const auto& b : bars) {
        const Size& s = b.s;
        // The Level plate where it always sits.
        Frame world = backdrop(s.w, s.h);
        paste(world, plate, AutoLogin::kLandmarkX, AutoLogin::kLandmarkY);
        check(AutoLogin::plate_at(world, &plate), "the HUD's Level plate at 44,4 on " + s.name());
        // The hotbar along the bottom, wherever across.
        paste(world, hotbar_pic, b.hx, b.hy);
        const HotbarSpot spot = find_hotbar(world, hotbar_pic);
        check(spot.found && spot.x == b.hx + hotbar::kSlotDx && spot.y == b.hy + hotbar::kSlotDy,
              "the hotbar on " + s.name() + " found at " + xy(b.hx, b.hy) + ", slot A at " + xy(spot.x, spot.y));
        // The chat bar in the bottom-left corner, shut: over the dark of the
        // world, as the chat tests' pretend game draws it.
        Frame bottom = make_frame(s.w, s.h);
        for (int y = 0; y < s.h; ++y)
            for (int x = 0; x < s.w; ++x) put_px(bottom, x, y, 30, 22, 20);
        paste(bottom, *chat_store.get(chat.open_picture), 0, s.h - 30);
        paste(bottom, *chat_store.get(chat.closed_picture), 85, s.h - 27);
        const ChatBar seen = look_chat_bar(bottom, chat_store, chat);
        check(seen.found && seen.state == ChatBarState::Closed && seen.y >= s.h - 40 && seen.x < 100,
              "the chat bar on " + s.name() + " found shut at the bottom-left, at " + xy(seen.x, seen.y) +
                  " (closed " + std::to_string(seen.closed_score) + ")");
        // A message of the game's in the middle: its lone Xác Nhận.
        Frame msg = backdrop(s.w, s.h);
        const int mx = s.w / 2 - ok_box.width / 2, my = s.h / 2 - ok_box.height / 2;
        paste(msg, ok_box, mx, my);
        // Looked for before the line is put together: its place is said from
        // what the look filled in.
        ScreenRect label;
        const bool lone = AutoLogin::message_in(msg, message, &label);
        check(lone && label.x >= mx && label.y >= my && label.x < mx + ok_box.width && label.y < my + ok_box.height,
              "a message centred on " + s.name() + " at " + xy(mx, my) + ": its Xác Nhận found inside it, at " +
                  xy(label.x, label.y));
        // The check-in board, centred: its title where the board puts it.
        const int bx = (s.w - kCheckinBoardFromTitle.w) / 2, by = (s.h - kCheckinBoardFromTitle.h) / 2;
        Frame events = backdrop(s.w, s.h);
        paste(events, board, bx - kCheckinBoardFromTitle.x, by - kCheckinBoardFromTitle.y);
        ScreenRect at;
        check(checkin_board_title(events, board, &at) && at.x + kCheckinBoardFromTitle.x == bx &&
                  at.y + kCheckinBoardFromTitle.y == by,
              "the check-in board centred on " + s.name() + " at " + xy(bx, by) + ", told by its title");
    }
#ifdef GOBOT_SOURCE_DIR
    const std::string bag = source("/src/bag.cpp");
    check(has(bag, "const ScreenRect centre{ f.width / 2 - 110, f.height / 2 - 60, 220, 120 };"),
          "bag.cpp: a charm's question is kept clear of round the middle of whatever size the game is");
#endif
}

// ----------------------------------------------------------- the wiring

void test_wiring() {
    section("Game sizes (2026.11.13c): the gates, the kept pictures and the docs (their source)");
#ifdef GOBOT_SOURCE_DIR
    const std::string gui = source("/apps/gobot_gui/gui_app.cpp");
    const std::string hpp = source("/apps/gobot_gui/gui_app.hpp");
    const std::string view = source("/apps/gobot_gui/simple_view.cpp");
    const std::string potions = source("/apps/gobot_gui/simple_potions.cpp");
    const std::string chat = source("/src/auto_chat.cpp");
    const std::string vitals = source("/src/vitals_reader.cpp");

    const std::string scaling = body(gui, "void GuiApp::check_game_scaling() {");
    check(has(scaling, "AutoLogin::game_fit(game)") && has(scaling, "game_fit_shared_ = fit;") &&
              !has(scaling, "std::ofstream") && !has(scaling, "save_png"),
          "gui_app.cpp: whether the game is all on the screen asked with the scale, every few seconds -- Windows' "
          "answers only, no file");
    check(has(scaling, "if (input_.window_valid() && (fit.width() <= 0 || fit.height() <= 0)) fit = game_fit_;") &&
              has(scaling, "else if (fit.width() > 0)"),
          "gui_app.cpp: a game minimised keeps the look it had, and a window gone is never said to be \"all on the "
          "screen again (client 0x0)\"");
    const std::string unseen = body(gui, "std::string GuiApp::game_unseen(int rows, const std::string& what, bool "
                                         "from_screen) const {");
    check(has(unseen, "if (!from_screen) return {};") && has(unseen, "if (fit.hidden_bottom <= rows) return {};"),
          "gui_app.cpp: a read is put off for the hidden rows only when its frames come off the screen -- the game "
          "drawing itself (PrintWindow) has those rows");
    check(has(hpp, "static constexpr int kHotbarRowsUnder = 10;") && has(hpp, "static constexpr int kChatRowsUnder = 6;"),
          "gui_app.hpp: the hotbar's rows and the chat's, as far up from the bottom as they reach");
    const std::string checklist = body(view, "void GuiApp::simple_checklist() {");
    const std::size_t covered = checklist.find("label(\"Cửa sổ GoBot đang che game\");");
    const std::size_t off = checklist.find("label(\"Game không nằm trọn trên màn hình\");");
    const std::size_t scaled = checklist.find("game_scaling_.stretched()");
    check(covered != npos && off != npos && scaled != npos && covered < off && off < scaled &&
              has(checklist, "hint(AutoLogin::fit_advice(game_fit_, off_screen).c_str());") &&
              has(checklist, "if (have_game && game_fit_.hides()) {") &&
              has(checklist, "const bool off_screen = seen.reads == 0 || seen.capture_from_screen;") &&
              has(checklist, "mark(off_screen ? Mark::Bad : Mark::Wait);"),
          "simple_view.cpp: \"Game không nằm trọn trên màn hình\" under GoBot's own window's row, only while rows "
          "or columns are hidden, with what to do -- a cross while the reader's frames come off the screen, a dot "
          "and no \"GoBot không ...\" while the game draws itself");
    const std::size_t gate = potions.find("ui.unseen = game_unseen(kHotbarRowsUnder, ");
    const std::size_t look = potions.find("ui.look = std::async(");
    check(gate != npos && look != npos && gate < look && has(potions, "if (!ui.unseen.empty()) return;") &&
              has(potions, "line = ui.unseen;") &&
              has(potions, "\"đếm được số bình trên thanh phím\", vitals_.stats().capture_from_screen);"),
          "simple_potions.cpp: no potion look with the hotbar's rows hidden on the reader's frames read off the "
          "screen, and the slot's line says why");
    check(has(gui, "in.unseen = game_unseen(kChatRowsUnder, \"thấy ô chat để Tự chat\", capture.from_screen());") &&
              has(gui, "[this, &capture] { return game_unseen(kChatRowsUnder, \"thấy ô chat để Tự chat\", "
                       "capture.from_screen()); }"),
          "gui_app.cpp: Tự chat's gate and its sender say the chat's rows are hidden (its frames always off the "
          "screen)");
    check(has(chat, "in.unseen.empty() ? std::string(kNoBar) : in.unseen") &&
              has(chat, "const std::string hidden = unseen_ ? unseen_() : std::string();"),
          "auto_chat.cpp: the reason said in place of \"Chưa thấy ô chat\" at the gate and before the first Enter");
    const std::string party = body(gui, "void GuiApp::party_loop() {");
    check(has(party, "chat_unseen = game_unseen(kChatRowsUnder, ") &&
              has(party, "\"đọc được dòng chat để mời người chat từ khoá\", look.from_screen);") &&
              has(party, "if (rules.auto_invite && have_font && chat_unseen.empty()) {") &&
              has(party, "say_party(chat_unseen);"),
          "gui_app.cpp: the keyword's chat lines not read with their rows hidden on a look read off the screen, said "
          "once on the Tổ đội tab");
    // Bag jobs as they were (the plan's rejected fix): the whole client on a
    // screen, by its corners, the taskbar not asked.
    const std::string on_screen = body(gui, "bool client_on_screen(void* window) {");
    check(has(on_screen, "MonitorFromPoint(p, MONITOR_DEFAULTTONULL)") && !has(on_screen, "game_fit") &&
              !has(on_screen, "part_seen") && !has(on_screen, "taskbar"),
          "gui_app.cpp: client_on_screen as it was -- a taskbar over rows the bag never uses stops no bag job");

    const std::string read = body(vitals, "void VitalsReader::loop() {");
    check(has(read, "if (!grab_ && hp_pct >= 0.0f && anchored && (frame.width != 1024 || frame.height != 768))") &&
              has(read, "keep_size_sample(frame);"),
          "vitals_reader.cpp: a picture at another size kept only off the real window, HP read by the landmark, "
          "never at 1024x768");
    const std::string keep = body(vitals, "void VitalsReader::keep_size_sample(const Frame& f) {");
    const std::size_t async = keep.find("keeping_size_ = std::async(std::launch::async, [shot = f] {");
    const std::size_t save = keep.find("save_png(shot, dir + \"\\\\\" + name)");
    check(async != npos && save != npos && async < save && has(keep, "\"size_\" + std::to_string(shot.width)") &&
              has(keep, "files_in(dir, \"size_*.png\")") && has(keep, "i + kSizeSamplesKept < names.size()") &&
              has(keep, "std::find(sizes_kept_.begin(), sizes_kept_.end(), size)"),
          "vitals_reader.cpp: vitals_debug\\size_<W>x<H>_<when>.png written off the reader's thread, once a "
          "size, the newest 6 kept apart from the vitals_*.png");

    // The notes.
    const std::string login_ini = source("/configs/login.ini");
    const std::string macro_ini = source("/configs/macro.ini");
    const std::size_t who_at = login_ini.find("# The character to play");
    const std::string who_notes = who_at == npos ? std::string() : login_ini.substr(who_at);
    check(has(who_notes, "1280x720 and 1366x768 NOT VERIFIED") &&
              has(who_notes, "login_debug\\login_*_character_<W>x<H>.png") && has(who_notes, "The corner is a guess") &&
              has(who_notes, "slot_anchor=picture   always the offsets from the picture") &&
              has(who_notes, "\nslot_anchor=auto"),
          "configs/login.ini: [Page.character] says how the slots are placed at another size, that it is not "
          "verified and the corner a guess, what is kept, and slot_anchor (auto as shipped)");
    check(!has(macro_ini, "will be out at any other size") && has(macro_ini, "once at 1366x768") &&
              !has(macro_ini, "1366x768 too (2026.11.13c)"),
          "configs/macro.ini: the HUD's landmark said seen at the sizes it was seen at, 1366x768 once -- not "
          "credited to 2026.11.13c");
#endif
}

// ---------------------- the game's window in the corner after a login (2026.11.14)

void test_corner() {
    section("After Tự đăng nhập, the game's window in the top-left corner of its screen (2026.11.14, the owner: "
            "\"Mặc định sẽ kéo lên góc trên bên trái đi\", \"chỉ kéo lên sau khi auto login thôi\")");
    using CP = AutoLogin::CornerPlace;
    // A window as Windows 11 draws it: the client 1 px in from the visible
    // frame's left and 31 px under its top (the title bar).
    const auto window = [](int x, int y, int w, int h) { return ScreenRect{ x, y, w + 2, h + 32 }; };
    const auto client = [](int x, int y, int w, int h) { return ScreenRect{ x + 1, y + 31, w, h }; };
    const auto said = [](const CP& p) {
        return std::string(p.move ? "move to " + std::to_string(p.x) + "," + std::to_string(p.y) : "stay: ") +
               p.why + (p.client_fits ? ", fits" : ", does not fit") + (p.title_hidden ? ", title hidden" : "");
    };
    {
        // 1024x768 on a 2560x1440 screen, the taskbar at the bottom.
        const CP p = AutoLogin::corner_place(window(300, 200, 1024, 768), client(300, 200, 1024, 768),
                                             ScreenRect{ 0, 0, 2560, 1400 }, ScreenRect{ 0, 0, 2560, 1440 }, true,
                                             false);
        check(p.move && p.x == 0 && p.y == 0 && p.client_fits && !p.title_hidden,
              "1024x768 on 2560x1440: the frame to 0,0, title bar shown -- " + said(p));
        const CP again = AutoLogin::corner_place(window(0, 0, 1024, 768), client(0, 0, 1024, 768),
                                                 ScreenRect{ 0, 0, 2560, 1400 }, ScreenRect{ 0, 0, 2560, 1440 }, true,
                                                 false);
        check(!again.move && has_text(again.why, "corner already"), "and once there, not moved again -- " + said(again));
    }
    {
        // 1280x720 on a 1366x768 screen with a 40 px taskbar: 31 + 720 rows do
        // not fit 728, the picture alone does -- the title bar above the edge.
        const CP p = AutoLogin::corner_place(window(40, 10, 1280, 720), client(40, 10, 1280, 720),
                                             ScreenRect{ 0, 0, 1366, 728 }, ScreenRect{ 0, 0, 1366, 768 }, true, false);
        check(p.move && p.x == -1 && p.y == -31 && p.client_fits && p.title_hidden,
              "1280x720 on 1366x768, taskbar shown: the picture to 0,0, the title bar above the screen -- " + said(p));
        // The taskbar hidden: the frame fits whole.
        const CP q = AutoLogin::corner_place(window(40, 10, 1280, 720), client(40, 10, 1280, 720),
                                             ScreenRect{ 0, 0, 1366, 768 }, ScreenRect{ 0, 0, 1366, 768 }, true, false);
        check(q.move && q.x == 0 && q.y == 0 && q.client_fits && !q.title_hidden,
              "1280x720 on 1366x768, taskbar hidden: the frame to 0,0 -- " + said(q));
    }
    {
        // 1366x768 on a 1366x768 screen: with the taskbar hidden the picture
        // fits only in the corner itself; with it shown not at all -- the
        // frame in the corner then, its title bar kept.
        const CP hidden = AutoLogin::corner_place(window(0, 0, 1366, 768), client(0, 0, 1366, 768),
                                                  ScreenRect{ 0, 0, 1366, 768 }, ScreenRect{ 0, 0, 1366, 768 }, true,
                                                  false);
        check(hidden.move && hidden.x == -1 && hidden.y == -31 && hidden.client_fits && hidden.title_hidden,
              "1366x768 on 1366x768, taskbar hidden: the picture to 0,0 -- " + said(hidden));
        // With it shown, it fits neither way: the picture in the corner all
        // the same, the most of it on the screen -- the rows under the taskbar
        // come back when it hides; the frame there would put 31 rows past the
        // screen's bottom, black in every capture (the review).
        const CP shown = AutoLogin::corner_place(window(100, 50, 1366, 768), client(100, 50, 1366, 768),
                                                 ScreenRect{ 0, 0, 1366, 728 }, ScreenRect{ 0, 0, 1366, 768 }, true,
                                                 false);
        check(shown.move && shown.x == -1 && shown.y == -31 && !shown.client_fits && shown.title_hidden,
              "1366x768 on 1366x768, taskbar shown: fits neither way -- the picture to 0,0 all the same -- " +
                  said(shown));
        // A window whose picture already covers the screen: left there.
        const CP covers = AutoLogin::corner_place(window(-1, -31, 1366, 768), client(-1, -31, 1366, 768),
                                                  ScreenRect{ 0, 0, 1366, 728 }, ScreenRect{ 0, 0, 1366, 768 }, true,
                                                  false);
        check(!covers.move && has_text(covers.why, "covers its screen"),
              "a picture covering its screen already, title bar or not, is left there -- " + said(covers));
        // 1024x768 on a 1366x768 laptop with its taskbar: the picture in the
        // corner, 40 rows under the taskbar rather than 71 out of sight.
        const CP laptop = AutoLogin::corner_place(window(150, 0, 1024, 768), client(150, 0, 1024, 768),
                                                  ScreenRect{ 0, 0, 1366, 728 }, ScreenRect{ 0, 0, 1366, 768 }, true,
                                                  false);
        check(laptop.move && laptop.x == -1 && laptop.y == -31 && laptop.title_hidden && !laptop.client_fits,
              "1024x768 on a 1366x768 laptop, taskbar shown: the picture to 0,0 -- " + said(laptop));
    }
    {
        // A second screen left of the first, and a taskbar along the top.
        const CP left = AutoLogin::corner_place(window(-1500, 100, 1024, 768), client(-1500, 100, 1024, 768),
                                                ScreenRect{ -1920, 0, 1920, 1040 }, ScreenRect{ -1920, 0, 1920, 1080 },
                                                true, false);
        check(left.move && left.x == -1920 && left.y == 0 && left.client_fits,
              "on a screen left of the main one: its own corner, -1920,0 -- " + said(left));
        const CP top = AutoLogin::corner_place(window(300, 300, 1024, 768), client(300, 300, 1024, 768),
                                               ScreenRect{ 0, 40, 1920, 1040 }, ScreenRect{ 0, 0, 1920, 1080 }, true,
                                               false);
        check(top.move && top.x == 0 && top.y == 40, "a taskbar along the top: under it, 0,40 -- " + said(top));
    }
    {
        // Never a minimised game, nor Full Mode, nor one of no size.
        const CP mini = AutoLogin::corner_place(window(300, 200, 1024, 768), client(300, 200, 1024, 768),
                                                ScreenRect{ 0, 0, 1920, 1040 }, ScreenRect{ 0, 0, 1920, 1080 }, true,
                                                true);
        const CP full = AutoLogin::corner_place(ScreenRect{ 0, 0, 1366, 768 }, ScreenRect{ 0, 0, 1366, 768 },
                                                ScreenRect{ 0, 0, 1366, 728 }, ScreenRect{ 0, 0, 1366, 768 }, false,
                                                false);
        const CP none = AutoLogin::corner_place(ScreenRect{}, ScreenRect{}, ScreenRect{ 0, 0, 1920, 1040 },
                                                ScreenRect{ 0, 0, 1920, 1080 }, true, false);
        const CP maxi = AutoLogin::corner_place(window(-8, -8, 1920, 1009), client(-8, -8, 1920, 1009),
                                                ScreenRect{ 0, 0, 1920, 1040 }, ScreenRect{ 0, 0, 1920, 1080 }, true,
                                                false, true);
        check(!mini.move && has_text(mini.why, "minimised") && !full.move && has_text(full.why, "Full Mode") &&
                  !none.move && !maxi.move && has_text(maxi.why, "maximised"),
              "a minimised game, a maximised one, a Full Mode one and one of no size are left where they are");
    }
    check(AutoLogin::Settings{}.move_to_corner, "on as shipped");
#ifdef GOBOT_SOURCE_DIR
    std::ifstream in_src(std::string(GOBOT_SOURCE_DIR) + "/src/auto_login.cpp", std::ios::binary);
    const std::string src((std::istreambuf_iterator<char>(in_src)), std::istreambuf_iterator<char>());
    std::ifstream in_ini(std::string(GOBOT_SOURCE_DIR) + "/configs/login.ini", std::ios::binary);
    const std::string ini((std::istreambuf_iterator<char>(in_ini)), std::istreambuf_iterator<char>());
    int calls = 0;
    for (std::size_t at = src.find("move_to_corner();"); at != npos; at = src.find("move_to_corner();", at + 1)) ++calls;
    int acted = 0;
    for (std::size_t at = src.find("acted_ = true;"); at != npos; at = src.find("acted_ = true;", at + 1)) ++acted;
    // In move_to_corner's own body, in this order: the switch, a run that did
    // something, pretend hands, then the real move behind refuse_real_input.
    const std::size_t body = src.find("void AutoLogin::move_to_corner() {");
    const auto after = [&](const char* part, std::size_t from) {
        return from == npos ? npos : src.find(part, from);
    };
    const std::size_t sw = after("if (!settings_.move_to_corner) return;", body);
    const std::size_t act = after("if (!acted_) {", sw);
    const std::size_t pre = after("const bool pretend = hands_.find || hands_.grab || hands_.click || hands_.focus;", act);
    const std::size_t pre2 = after("if (pretend && !hands_.place) return;", pre);
    const std::size_t refuse = after("if (refuse_real_input(\"move the game's window\")) return;", pre2);
    const std::size_t hung = after("SendMessageTimeoutW(h, WM_NULL", refuse);
    const std::size_t set = after("SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS", hung);
    check(calls == 2 && acted == 2 && body != npos && sw != npos && act != npos && pre != npos && pre2 != npos &&
              refuse != npos && hung != npos && set != npos &&
              has_text(src, "move_to_corner = ini.get_bool(\"Login\", \"move_game_to_corner\", true);") &&
              has_text(src, "ini.set_bool(\"Login\", \"move_game_to_corner\", move_to_corner);") &&
              has_text(src, "IsIconic(h) != FALSE, IsZoomed(h) != FALSE);") &&
              has_text(ini, "\nmove_game_to_corner=true"),
          "auto_login.cpp: moved only after the two ways a crossing logs in, and only by a run that clicked or "
          "started something; the switch asked first; pretend hands -- any -- never move a real window; the real "
          "move behind refuse_real_input, never into a game that does not answer, asked not waited on, never "
          "resized; login.ini ships it on");
    std::ifstream in_gui(std::string(GOBOT_SOURCE_DIR) + "/apps/gobot_gui/gui_app.cpp", std::ios::binary);
    const std::string gui((std::istreambuf_iterator<char>(in_gui)), std::istreambuf_iterator<char>());
    check(has_text(gui, "if (flow.moved_to_corner()) clear_after_login_ = true;") &&
              has_text(gui, "if (clear_after_login_.exchange(false) && overlap_fraction() > 0.02f) {"),
          "gui_app.cpp: after a crossing that moved the game, GoBot's own window moved off it by the window's "
          "thread when it covers it");
#endif
}

} // namespace

void test_res_sizes(void (*check_fn)(bool, const std::string&), void (*section_fn)(const char*)) {
    g_check = check_fn;
    g_section = section_fn;
    test_character_select();
    test_fit();
    test_channel_and_scale();
    test_sweep();
    test_wiring();
    test_corner();
}
