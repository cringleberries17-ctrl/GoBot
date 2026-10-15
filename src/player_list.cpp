#include "gobot/player_list.hpp"
#include "gobot/logger.hpp"
#include "gobot/party_chat.hpp"
#include "gobot/party_invite.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

namespace gobot {
namespace {

// The panel and its columns from the top-left of the button that opens the
// list. The numbers measured on a 1024x768 client are this button, at
// 956,139, plus these; on the 1280x720 client the button is at 1212,139 in
// every one of 1,036 saved frames, and the list hangs off it the same way.
constexpr int kRoiDx = -212, kRoiDy = 21;
constexpr int kLevelDx = -208;
constexpr int kNameDx = -161;
constexpr int kClickDx = -94;
constexpr int kFirstRowDy = 33;

} // namespace

PlayerList::Settings PlayerList::Settings::at_button(int bx, int by) const {
    Settings s = *this;
    s.roi_x = bx + kRoiDx;
    s.roi_y = by + kRoiDy;
    s.level_x = bx + kLevelDx;
    s.name_x = bx + kNameDx;
    s.click_x = bx + kClickDx;
    s.first_row_y = by + kFirstRowDy;
    return s;
}

void PlayerList::place(int bx, int by, int frame_w, int frame_h) {
    placed_ = settings_.fixed ? settings_ : settings_.at_button(bx, by);
    placed_ok_ = true;
    frame_w_ = frame_w;
    frame_h_ = frame_h;
}

bool PlayerList::in_panel(int x, int y) const {
    if (!placed_ok_) return false;
    const ScreenRect panel{ placed_.roi_x, placed_.roi_y, placed_.roi_w, placed_.roi_h };
    return panel.contains(x, y) && x >= 0 && y >= 0 && x < frame_w_ && y < frame_h_;
}

bool PlayerList::click(InputManager& input, int x, int y) const {
    return click_ ? click_(x, y) : input.click_at(x, y);
}

void PlayerList::Settings::load(const IniFile& ini) {
    // Only an explicit "fixed" keeps the numbers below as written: every
    // party.ini from before has them, measured on a 1024x768 client, and
    // their being there says nothing about wanting them.
    std::string place = ini.get("PlayerList", "place", "auto");
    std::transform(place.begin(), place.end(), place.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    fixed = place == "fixed";
    level_x = ini.get_int("PlayerList", "level_x", level_x);
    level_w = ini.get_int("PlayerList", "level_width", level_w);
    open_picture = ini.get("PlayerList", "open_button_picture", open_picture);
    open_score   = ini.get_float("PlayerList", "open_score", open_score);
    roi_x        = ini.get_int("PlayerList", "roi_x", roi_x);
    roi_y        = ini.get_int("PlayerList", "roi_y", roi_y);
    roi_w        = ini.get_int("PlayerList", "roi_width", roi_w);
    roi_h        = ini.get_int("PlayerList", "roi_height", roi_h);
    first_row_y  = ini.get_int("PlayerList", "first_row_y", first_row_y);
    row_height   = std::max(1, ini.get_int("PlayerList", "row_height", row_height));
    rows         = std::max(1, ini.get_int("PlayerList", "rows", rows));
    grid_picture = ini.get("PlayerList", "grid_picture", grid_picture);
    grid_score   = ini.get_float("PlayerList", "grid_score", grid_score);
    name_x       = ini.get_int("PlayerList", "name_x", name_x);
    name_w       = ini.get_int("PlayerList", "name_width", name_w);
    click_x      = ini.get_int("PlayerList", "click_x", click_x);
    invite_dx    = ini.get_int("PlayerList", "invite_button_dx", invite_dx);
    invite_dy    = ini.get_int("PlayerList", "invite_button_dy", invite_dy);
    menu_wait_ms = std::max(100, ini.get_int("PlayerList", "menu_wait_ms", menu_wait_ms));
    min_name_match =
        ini.get_float("PlayerList", "min_name_match",
                      static_cast<float>(min_name_match));
}

void PlayerList::Settings::save(IniFile& ini) const {
    ini.set("PlayerList", "open_button_picture", open_picture);
    ini.set_float("PlayerList", "open_score", open_score);
    ini.set_int("PlayerList", "roi_x", roi_x);
    ini.set_int("PlayerList", "roi_y", roi_y);
    ini.set_int("PlayerList", "roi_width", roi_w);
    ini.set_int("PlayerList", "roi_height", roi_h);
    ini.set_int("PlayerList", "first_row_y", first_row_y);
    ini.set_int("PlayerList", "row_height", row_height);
    ini.set_int("PlayerList", "rows", rows);
    ini.set("PlayerList", "grid_picture", grid_picture);
    ini.set_float("PlayerList", "grid_score", grid_score);
    ini.set_int("PlayerList", "name_x", name_x);
    ini.set_int("PlayerList", "name_width", name_w);
    ini.set_int("PlayerList", "click_x", click_x);
    ini.set_int("PlayerList", "invite_button_dx", invite_dx);
    ini.set_int("PlayerList", "invite_button_dy", invite_dy);
    ini.set_int("PlayerList", "menu_wait_ms", menu_wait_ms);
    ini.set_float("PlayerList", "min_name_match",
                  static_cast<float>(min_name_match));
    ini.set("PlayerList", "place", fixed ? "fixed" : "auto");
    ini.set_int("PlayerList", "level_x", level_x);
    ini.set_int("PlayerList", "level_width", level_w);
}

std::vector<PlayerList::Row> PlayerList::read(const Frame& frame,
                                              const GlyphFont& font) const {
    std::vector<Row> out;
    // Not before the list has been placed from its button: the numbers it
    // starts with belong to one size of client.
    if (!placed_ok_ || !frame.valid() || !font.loaded()) return out;
    const Settings& g = placed_;

    for (int i = 0; i < g.rows; ++i) {
        const int top = g.first_row_y + i * g.row_height;
        if (top < 0 || top + g.row_height > frame.height) break;

        ScreenRect box{ g.name_x, top, g.name_w, g.row_height };
        if (box.x < 0 || box.x + box.w > frame.width) break;

        const std::string text = font.read_line(frame, box);

        // One or two characters is noise off the panel's own border, not a
        // player; the shortest name this game allows is longer than that.
        if (PartyInvite::fold(text).size() < 3) continue;

        Row row;
        row.index = i;
        row.y     = top + g.row_height / 2;
        row.name  = text;
        out.push_back(row);
    }
    return out;
}

bool PlayerList::menu_up(const Frame& frame, int row_y) const {
    if (!frame.valid()) return false;

    // The three buttons are pale blocks on a near-black panel, and they are the
    // only pale thing in that rectangle. Counted rather than matched against a
    // picture because their text changes with the client's language and their
    // position follows the row that was picked.
    const int x0 = std::max(0, placed_.click_x + placed_.invite_dx - 30);
    const int x1 = std::min(frame.width, placed_.click_x + placed_.invite_dx + 30);
    const int y0 = std::max(0, row_y + 6);
    const int y1 = std::min(frame.height, row_y + 72);
    if (x1 <= x0 || y1 <= y0) return false;

    int pale = 0, looked = 0;
    for (int y = y0; y < y1; y += 2) {
        for (int x = x0; x < x1; x += 2) {
            std::uint8_t r, g, b;
            frame.get(x, y, r, g, b);
            ++looked;
            if (r > 110 && g > 110 && b > 110) ++pale;
        }
    }
    // A fifth of that rectangle pale means the buttons are drawn in it; an
    // empty list panel there is almost entirely dark.
    return looked > 0 && pale * 5 > looked;
}

bool PlayerList::open(const Frame& frame, const TemplateStore& store) const {
    if (!frame.valid()) return false;
    const Frame* grid = store.get(settings_.grid_picture);
    if (grid == nullptr || !grid->valid()) return false;
    return TemplateMatcher::find(
               frame, ScreenRect{ 0, 0, frame.width, frame.height }, *grid,
               settings_.grid_score).found;
}

bool PlayerList::ensure_open(InputManager& input, ScreenCapture& capture,
                             const TemplateStore& store, std::string* why) {
    auto fail = [&](const std::string& text) {
        if (why) *why = text;
        return false;
    };

    // Input goes to whatever has the foreground, not to the window the
    // coordinates were worked out against.
    if (!input.window_focused()) {
        input.focus_window();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    auto showing = [&](Frame& into) {
        return capture.grab(input.target_window(), into) && into.valid() &&
               open(into, store);
    };

    Frame frame;
    if (!capture.grab(input.target_window(), frame) || !frame.valid())
        return fail("could not capture the game");

    const Frame* button = store.get(settings_.open_picture);
    if (button == nullptr || !button->valid())
        return fail("no picture called '" + settings_.open_picture + "', which is "
                    "the button that opens the list");

    const auto found = TemplateMatcher::find(
        frame, ScreenRect{ 0, 0, frame.width, frame.height }, *button,
        settings_.open_score);
    if (!found.found)
        return fail("the players-on-map button is not on screen");

    // The list hangs off its button, wherever the client's size puts that.
    place(found.at.x, found.at.y, frame.width, frame.height);

    const int bx = found.at.x + found.at.w / 2;
    const int by = found.at.y + found.at.h / 2;

    // Already up: leave it alone.
    //
    // An earlier version closed it and opened it again to be sure of a clean
    // panel, and that was worse than the problem. The button toggles between
    // the player list and whatever panel was showing before it, so every extra
    // press flips the state -- a run that closed and reopened ended up back on
    // the client's activity notices and reported that nobody was on the map.
    if (open(frame, store)) return true;

    // Two presses at most, checked after each rather than assumed. One should
    // do it; the second covers the case where the first arrives while the
    // panel is busy with something else.
    for (int press = 0; press < 2; ++press) {
        input.click_at(bx, by);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        if (showing(frame)) return true;
    }

    // Nothing worked, and the presses may have left the panel somewhere it
    // was not. Put it back.
    input.click_at(bx, by);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    return fail("the player list did not come up, or nobody else is on this map");
}

int PlayerList::find(const std::vector<Row>& rows, const std::string& who) const {
    const std::string want = PartyInvite::fold(who);
    if (want.empty()) return -1;

    int best = -1;
    double best_score = 0.0;

    for (const Row& row : rows) {
        const std::string have = PartyInvite::fold(row.name);
        if (have.empty()) continue;

        if (have == want) return row.index;

        // The list is read off the screen and so is the name being looked
        // for, so both sides may have a letter wrong. Take the best row above
        // the threshold rather than the first.
        const double score = PartyChat::alike(have, want);
        if (score >= settings_.min_name_match && score > best_score) {
            best_score = score;
            best = row.index;
        }
    }
    return best;
}

bool PlayerList::invite(InputManager& input, ScreenCapture& capture,
                        const TemplateStore& /*store*/, const Row& row,
                        std::string* why) const {
    // Both clicks inside the list as placed from its button, or neither.
    // Outside it is the play field -- where the 1024x768 numbers put them on
    // a 1280x720 client -- or another panel, and a click there is a walk or
    // somebody else's button.
    const int x = placed_.click_x + placed_.invite_dx;
    const int y = row.y + placed_.invite_dy;
    if (!placed_ok_) {
        if (why) *why = "the list has not been found -- the button that opens it was not seen";
        return false;
    }
    if (!in_panel(placed_.click_x, row.y) || !in_panel(x, y)) {
        GB_WARN("PlayerList: not clicking row %d at %d,%d nor Req Party at %d,%d -- outside "
                "the list at %d,%d %dx%d in a %dx%d window",
                row.index + 1, placed_.click_x, row.y, x, y, placed_.roi_x, placed_.roi_y,
                placed_.roi_w, placed_.roi_h, frame_w_, frame_h_);
        if (why) *why = "that row, or its Req Party, is outside the list";
        return false;
    }

    if (!input.window_focused()) {
        input.focus_window();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    click(input, placed_.click_x, row.y);

    // Wait for the menu to be drawn before pressing anything on it.
    //
    // Seven hundred milliseconds was not enough: the first live run clicked
    // where "Req Party" was going to be, the menu came up afterwards, and the
    // run reported "Invited" with the menu still sitting on screen untouched.
    // The same click a moment later worked perfectly.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(settings_.menu_wait_ms));

    // "Req Party" sits between a private message and a trade offer, and it is
    // reached by offset from the row rather than by looking for a lone button
    // -- there are three of them, they look alike, and clicking either of the
    // others is not a quiet failure.
    if (!click(input, x, y)) {
        if (why) *why = "the invite click was refused";
        return false;
    }

    // Check the press landed, by watching the MENU rather than the list.
    //
    // Two wrong versions came before this one. Checking once after 1.1 seconds
    // reported failure three runs running while the press was working -- the
    // client takes its time. Then polling for the whole panel to close looked
    // right and was not: the panel only closes when the invitation actually
    // goes out, and the client refuses one to somebody already in your party,
    // leaving the list up. That is a refusal, not a missed click, and calling
    // it a missed click sent the loop round to try again.
    //
    // The menu closing is the thing that means "the button was pressed". What
    // the server then makes of it is a separate question.
    bool pressed = false;
    for (int i = 0; i < 10 && !pressed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        Frame after;
        if (!capture.grab(input.target_window(), after) || !after.valid()) continue;
        pressed = !menu_up(after, row.y);
    }
    if (!pressed) {
        if (why) *why = "the menu is still up after four seconds, so that press "
                        "did not land";
        return false;
    }

    GB_INFO("PlayerList: invited row %d ('%s') -- row at %d,%d, Req Party at %d,%d",
            row.index + 1, row.name.c_str(), placed_.click_x, row.y, x, y);
    return true;
}

} // namespace gobot
