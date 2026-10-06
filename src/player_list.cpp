#include "gobot/player_list.hpp"
#include "gobot/logger.hpp"
#include "gobot/party_chat.hpp"
#include "gobot/party_invite.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace gobot {

void PlayerList::Settings::load(const IniFile& ini) {
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
}

std::vector<PlayerList::Row> PlayerList::read(const Frame& frame,
                                              const GlyphFont& font) const {
    std::vector<Row> out;
    if (!frame.valid() || !font.loaded()) return out;

    for (int i = 0; i < settings_.rows; ++i) {
        const int top = settings_.first_row_y + i * settings_.row_height;
        if (top < 0 || top + settings_.row_height > frame.height) break;

        ScreenRect box{ settings_.name_x, top, settings_.name_w,
                        settings_.row_height };
        if (box.x < 0 || box.x + box.w > frame.width) break;

        const std::string text = font.read_line(frame, box);

        // One or two characters is noise off the panel's own border, not a
        // player; the shortest name this game allows is longer than that.
        if (PartyInvite::fold(text).size() < 3) continue;

        Row row;
        row.index = i;
        row.y     = top + settings_.row_height / 2;
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
    const int x0 = std::max(0, settings_.click_x + settings_.invite_dx - 30);
    const int x1 = std::min(frame.width, settings_.click_x + settings_.invite_dx + 30);
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
                        const TemplateStore& store, const Row& row,
                        std::string* why) const {
    if (!input.window_focused()) {
        input.focus_window();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    input.click_at(settings_.click_x, row.y);

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
    const int x = settings_.click_x + settings_.invite_dx;
    const int y = row.y + settings_.invite_dy;
    if (!input.click_at(x, y)) {
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
            row.index + 1, row.name.c_str(), settings_.click_x, row.y, x, y);
    return true;
}

} // namespace gobot
