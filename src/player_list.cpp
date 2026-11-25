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

/// What `why` says when the stop predicate ended it.
constexpr const char* kStopped = "stopped";

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

bool PlayerList::click(InputManager& input, int x, int y, const Stop& stop) const {
    // Only with the game in front and the game itself under the point, both
    // asked again as the button goes down: the frames come from PrintWindow,
    // which shows the game even when it is covered. So is the stop: one that
    // comes in the pointer's pause clicks nothing.
    return click_ ? click_(x, y) : input.click_on_game(x, y, false, stop);
}

bool PlayerList::grab(InputManager& input, ScreenCapture& capture, Frame& into) const {
    if (grab_) return grab_(into) && into.valid();
    return capture.grab(input.target_window(), into) && into.valid();
}

std::uint64_t PlayerList::clock_ms() const {
    if (now_) return now_();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

bool PlayerList::nap(int ms, const Stop& stop) const {
    // A test's clock: its sleep moves it, in the same slices.
    if (sleep_) {
        for (int slept = 0;; slept += 50) {
            if (stop && stop()) return false;
            if (slept >= ms) return true;
            sleep_(std::min(50, ms - slept));
        }
    }
    // Against the clock rather than by adding up the slices: Windows rounds
    // each sleep up to its 15.6 ms tick, and 50 ms slices made a wait of a
    // second a quarter longer.
    using clock = std::chrono::steady_clock;
    const auto until = clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        if (stop && stop()) return false;
        const auto left = until - clock::now();
        if (left <= clock::duration::zero()) return true;
        std::this_thread::sleep_for(std::min<clock::duration>(left, std::chrono::milliseconds(50)));
    }
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
                             const TemplateStore& store, std::string* why,
                             const Stop& stop) {
    auto fail = [&](const std::string& text) {
        if (why) *why = text;
        return false;
    };
    const auto stopped = [&] { return stop && stop(); };
    opened_it_ = false;
    if (stopped()) return fail(kStopped);

    // Input goes to whatever has the foreground, not to the window the
    // coordinates were worked out against.
    if (!input.window_focused()) {
        input.focus_window();
        if (!nap(250, stop)) return fail(kStopped);
    }

    auto showing = [&](Frame& into) {
        return grab(input, capture, into) && open(into, store);
    };

    Frame frame;
    if (!grab(input, capture, frame))
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

    // ONE press, since 2026-11-15, and the list looked for every quarter of a
    // second until it shows -- up to react_ms() from the press (the owner, for
    // slow PCs: "sẽ có thêm 30 giây chờ phản ứng. Nếu màn hình phản ứng luôn
    // thì thực hiện tiếp bước tiếp theo, còn nếu màn hình không phản ứng gì thì
    // sẽ dừng xử lý"). Until then two presses 800 ms apart and a third "to put
    // it back": the button toggles, so on a slow game the second shut the list
    // the first opened late, and the third flipped it again. Nothing more is
    // pressed for a list that does not show: it may be up and not known, or
    // the press lost, and the next asker presses again.
    if (stopped()) return fail(kStopped);
    if (!click(input, bx, by, stop))
        return fail(stopped() ? kStopped
                              : "the click on the players-on-map button was refused -- the game is not in front, "
                                "or something covers it");
    const std::uint64_t pressed_at = clock_ms();
    const std::uint64_t show_by = static_cast<std::uint64_t>(std::max(800, react_ms_));
    for (;;) {
        if (!nap(250, stop)) return fail(kStopped);
        if (showing(frame)) {
            const std::uint64_t took = clock_ms() - pressed_at;
            if (took > static_cast<std::uint64_t>(kReactSlowMs))
                GB_INFO("PlayerList: the game took %llu ms to show the list after the press on its button",
                        static_cast<unsigned long long>(took));
            opened_it_ = true;
            return true;
        }
        if (clock_ms() - pressed_at >= show_by) break;
    }
    GB_INFO("PlayerList: nothing after %d s to show the list after the press on its button",
            static_cast<int>(show_by / 1000));
    return fail("the player list did not come up within " + std::to_string(show_by / 1000) +
                " s of the press on its button, or nobody else is on this map");
}

bool PlayerList::close(InputManager& input, ScreenCapture& capture, const TemplateStore& store,
                       std::string* why, const Stop& stop) {
    auto fail = [&](const std::string& text) {
        if (why) *why = text;
        return false;
    };
    const auto stopped = [&] { return stop && stop(); };
    if (stopped()) return fail(kStopped);

    // Up on two looks kCloseSettleMs apart, or it is left: a good invite
    // shuts the list by itself a moment after Req Party, and the button
    // flips -- a press on a list already going would bring it back up.
    Frame frame;
    if (!grab(input, capture, frame)) return fail("could not capture the game");
    if (!open(frame, store)) return true;
    if (!nap(kCloseSettleMs, stop)) return fail(kStopped);
    if (!grab(input, capture, frame)) return fail("could not capture the game");
    if (!open(frame, store)) return true;

    // Its own button, found by its picture over the whole frame: where it is
    // at whatever size the client is, as ensure_open finds it.
    const Frame* button = store.get(settings_.open_picture);
    if (button == nullptr || !button->valid())
        return fail("no picture called '" + settings_.open_picture + "', which is the button that opens and "
                    "shuts the list");
    const auto found = TemplateMatcher::find(frame, ScreenRect{ 0, 0, frame.width, frame.height }, *button,
                                             settings_.open_score);
    if (!found.found) return fail("the players-on-map button is not on screen");

    if (!input.window_focused()) {
        input.focus_window();
        if (!nap(250, stop)) return fail(kStopped);
    }
    if (stopped()) return fail(kStopped);
    const int bx = found.at.x + found.at.w / 2;
    const int by = found.at.y + found.at.h / 2;
    if (!click(input, bx, by, stop))
        return fail(stopped() ? kStopped
                              : "the click on the players-on-map button was refused -- the game is not in front, "
                                "or something covers it");

    // ONE press, then looked at -- never a second, which would flip it back.
    const std::uint64_t pressed_at = clock_ms();
    const std::uint64_t gone_by = static_cast<std::uint64_t>(std::max(250, close_wait_ms_));
    for (;;) {
        if (!nap(250, stop)) return fail(kStopped);
        Frame now;
        if (grab(input, capture, now) && !open(now, store)) {
            opened_it_ = false;
            return true;
        }
        if (clock_ms() - pressed_at >= gone_by) break;
    }
    return fail("the list still showed " + std::to_string(gone_by / 1000) + " s after the press on its button");
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
                        std::string* why, const Stop& stop) const {
    const auto stopped = [&] { return stop && stop(); };

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
    if (stopped()) {
        if (why) *why = kStopped;
        return false;
    }

    if (!input.window_focused()) {
        input.focus_window();
        if (!nap(250, stop)) {
            if (why) *why = kStopped;
            return false;
        }
    }

    // A row click that did not go -- the game not in front, or something over
    // the list -- is the end of it: Req Party is only ever pressed on a row
    // that was picked.
    if (!click(input, placed_.click_x, row.y, stop)) {
        if (why) {
            if (stopped()) *why = kStopped;
            else *why = "the click on the row was refused -- the game is not in front, or "
                        "something covers the list";
        }
        return false;
    }

    // Wait for the menu to be drawn before pressing anything on it.
    //
    // Seven hundred milliseconds was not enough: the first live run clicked
    // where "Req Party" was going to be, the menu came up afterwards, and the
    // run reported "Invited" with the menu still sitting on screen untouched.
    // The same click a moment later worked perfectly.
    //
    // So, since 2026-11-15 (the owner's 30 s for slow PCs), not a blind
    // menu_wait_ms: the menu looked for every quarter of a second, and Req
    // Party pressed once two looks in a row show it -- one look could catch
    // it half drawn -- up to react_ms() from the row's click, menu_wait_ms if
    // that is longer. Not shown by then: nothing pressed, the row left picked.
    // A stop presses nothing more either, and the menu goes with the next
    // click on the game.
    {
        const std::uint64_t picked_at = clock_ms();
        const std::uint64_t menu_by = static_cast<std::uint64_t>(std::max(settings_.menu_wait_ms, react_ms_));
        int seen = 0;
        while (seen < 2) {
            if (!nap(250, stop)) {
                if (why) *why = kStopped;
                return false;
            }
            Frame now;
            if (grab(input, capture, now) && menu_up(now, row.y)) ++seen;
            else seen = 0;
            if (seen < 2 && clock_ms() - picked_at >= menu_by) {
                GB_INFO("PlayerList: nothing after %d s to show row %d's menu -- Req Party not clicked",
                        static_cast<int>(menu_by / 1000), row.index + 1);
                if (why)
                    *why = "the row's menu did not come up within " + std::to_string(menu_by / 1000) +
                           " s of the click on it, so Req Party was not clicked";
                return false;
            }
        }
        const std::uint64_t took = clock_ms() - picked_at;
        if (took > static_cast<std::uint64_t>(kReactSlowMs))
            GB_INFO("PlayerList: the game took %llu ms to show row %d's menu",
                    static_cast<unsigned long long>(took), row.index + 1);
    }
    if (stopped()) {
        if (why) *why = kStopped;
        return false;
    }

    // "Req Party" sits between a private message and a trade offer, and it is
    // reached by offset from the row rather than by looking for a lone button
    // -- there are three of them, they look alike, and clicking either of the
    // others is not a quiet failure.
    if (!click(input, x, y, stop)) {
        if (why) *why = stopped() ? kStopped : "the invite click was refused";
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
    //
    // Looked at every 400 ms, as before, up to react_ms() from the press
    // (2026-11-15, the owner's 30 s for slow PCs; four seconds before), and
    // never pressed again: a second press on a menu that went late lands on
    // whatever is under it.
    bool pressed = false;
    const std::uint64_t req_at = clock_ms();
    const std::uint64_t land_by = static_cast<std::uint64_t>(std::max(4000, react_ms_));
    while (!pressed && clock_ms() - req_at < land_by) {
        if (!nap(400, stop)) break;
        Frame after;
        if (!grab(input, capture, after)) continue;
        pressed = !menu_up(after, row.y);
    }
    if (!pressed) {
        if (why) {
            if (stopped())
                *why = "stopped before the press could be seen to land";
            else
                *why = "the menu is still up after " + std::to_string(land_by / 1000) +
                       " seconds, so that press did not land";
        }
        if (!stopped())
            GB_INFO("PlayerList: nothing after %d s to show the menu gone after Req Party",
                    static_cast<int>(land_by / 1000));
        return false;
    }
    {
        const std::uint64_t took = clock_ms() - req_at;
        if (took > static_cast<std::uint64_t>(kReactSlowMs))
            GB_INFO("PlayerList: the game took %llu ms to show the menu gone after Req Party",
                    static_cast<unsigned long long>(took));
    }

    GB_INFO("PlayerList: invited row %d ('%s') -- row at %d,%d, Req Party at %d,%d",
            row.index + 1, row.name.c_str(), placed_.click_x, row.y, x, y);
    return true;
}

} // namespace gobot
