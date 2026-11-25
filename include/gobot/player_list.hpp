#pragma once

#include "gobot/config.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/react_wait.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gobot {

/// The list of players on the map, and inviting one of them.
///
/// This is the acting half of auto-party. The deciding half -- noticing that
/// somebody typed the keyword in chat -- has been there a while and was never
/// connected to anything, so a player could ask to join all day and nothing
/// would happen.
///
/// Inviting is three clicks and none of them is obvious:
///
///   1. the little figure under the minimap, which opens the list
///   2. the row with that player's name on it
///   3. "Req Party", which appears over the list only once a row is picked
///
/// The third is the reason this could not be measured for weeks: the button
/// does not exist until a player is selected, and selecting a player needs
/// somebody else on the map. Measured at last against a live list --
/// `162 | ExampleWin | Đạo Nhân` -- where picking the row at 862,182 put three
/// buttons over the panel:
///
///   Tin Nhắn Riêng      y 188..205     a private message
///   Req Party           y 212..229     this one, centred 861,220
///   Thêm Giao Dịch      y 236..253     a trade offer
///
/// which is +0,+38 from the row that was clicked. The two it sits between are
/// a private message and a trade offer, and clicking either of those by
/// mistake is not a quiet failure -- which is why the invite is found by
/// offset from the row rather than by looking for a lone button.
class PlayerList {
public:
    struct Settings {
        /// The button that opens the list.
        std::string open_picture = "player_in_map_icon";
        float       open_score = 0.85f;

        /// The panel, in client coordinates.
        int roi_x = 744, roi_y = 160, roi_w = 276, roi_h = 214;

        /// The rows: the first one's top edge, the pitch, and how many fit.
        int first_row_y = 172, row_height = 20, rows = 10;

        /// How the list is told apart from the panel that shares its place.
        ///
        /// The client draws its activity notices in the same rectangle, so
        /// "is there text in the panel?" answers the wrong question -- the
        /// first live run read five notices as five players with
        /// unpronounceable names. Reading the level column instead does not
        /// work either: the list is yellow on near-black and the bitmap
        /// reader turns "162" into "fC2".
        ///
        /// So it is recognised by the one thing the notices panel has not got:
        /// the empty rows, with the two vertical column separators ruled
        /// through them. That picture carries no text, so it looks the same
        /// whoever happens to be on the map.
        std::string grid_picture = "grabbed/player_list_grid";
        float       grid_score = 0.90f;

        /// The name column, which is the middle of the three.
        int name_x = 795, name_w = 135;

        /// Where along a row to click to select it.
        int click_x = 862;

        /// "Req Party", from the point the row was clicked.
        int invite_dx = 0, invite_dy = 38;

        /// How long to wait after picking a row for the menu to be drawn.
        ///
        /// Seven hundred was not enough: the first live run clicked where
        /// "Req Party" was going to be, the menu appeared afterwards, and the
        /// run reported success with the menu untouched on screen.
        ///
        /// Since 2026-11-15 (the owner, for slow PCs: "sẽ có thêm 30 giây chờ
        /// phản ứng") the menu is looked for instead, a look every quarter of
        /// a second, and Req Party is clicked once two looks in a row show it
        /// -- however soon or late that is, up to kReactMs (or this, if it is
        /// longer) from the row's click; never blind. Not shown by then:
        /// nothing clicked.
        int menu_wait_ms = 1100;

        /// How alike a row has to be to the name being looked for.
        ///
        /// Not an exact match, because these names are read off the screen,
        /// and lower here than elsewhere because this list reads worse than
        /// the invitation dialog does: the same player's ten-letter name came
        /// back with three letters lost or misread, the other seven in order.
        double min_name_match = 0.6;

        /// The level column, the first of the three.
        int level_x = 748, level_w = 45;

        /// party.ini [PlayerList] place = fixed: the panel, the columns,
        /// first_row_y and click_x are used as written, the old way.
        /// Otherwise -- and with no key at all -- they are placed from where
        /// the button that opens the list was found, every time the list is
        /// used. The numbers above were measured on a 1024x768 client, and on
        /// a 1280x720 one they read and click over the play field: the button
        /// keeps to the right edge (956,139 on the one, 1212,139 on the
        /// other) and the list hangs off it.
        bool fixed = false;

        /// These settings with the panel placed from the button whose
        /// top-left is `bx`,`by`. For a button at 956,139 -- where it is on a
        /// 1024x768 client -- that is exactly the numbers above.
        Settings at_button(int bx, int by) const;

        void load(const IniFile& ini);
        void save(IniFile& ini) const;
    };

    struct Row {
        int         index = 0;      ///< 0-based, from the top
        int         y = 0;          ///< where to click, in client coordinates
        std::string name;
    };

    /// Asked between the steps of ensure_open() and invite() and all through
    /// their waits; true there stops it with nothing more clicked, and `why`
    /// says "stopped". F12 or DỪNG in the middle of an invite used to wait out
    /// all of it -- some eight seconds of the row and Req Party clicked -- with
    /// the window that reads F12 frozen behind it. Empty: never stopped.
    using Stop = std::function<bool()>;

    void set_settings(const Settings& s) {
        settings_ = s;
        placed_ = s;
        placed_ok_ = false;
    }
    Settings settings() const { return settings_; }

    /// Place the list from the button that opens it, found with its top-left
    /// at `bx`,`by` in a frame `frame_w` x `frame_h`: what read(), menu_up()
    /// and invite() go by from then on. ensure_open() does this each time it
    /// finds the button; nothing is read or clicked before it has.
    void place(int bx, int by, int frame_w, int frame_h);
    bool placed() const { return placed_ok_; }
    /// The panel, the columns and the click points as placed.
    const Settings& geometry() const { return placed_; }

    /// Whether a click at `x`,`y` is inside the placed panel and inside the
    /// frame it was placed in: the only place a row or "Req Party" is ever
    /// clicked. Outside it is the play field, or another panel.
    bool in_panel(int x, int y) const;

    /// For tests: the clicks go here instead of to the game -- the button
    /// that opens the list, the row and Req Party.
    void set_click(std::function<bool(int, int)> fn) { click_ = std::move(fn); }
    /// For tests: frames from here instead of the game's capture, and a
    /// pretend clock -- `now` in ms, and `sleep` that moves it -- instead of
    /// the steady one the waits go by.
    void set_grab(std::function<bool(Frame&)> fn) { grab_ = std::move(fn); }
    void set_clock(std::function<std::uint64_t()> now, std::function<void(int)> sleep) {
        now_ = std::move(now);
        sleep_ = std::move(sleep);
    }
    /// How long, from a click, the list is given to show what it did -- the
    /// list up after its button, the row's menu drawn, the menu gone after Req
    /// Party -- before the step is given up as it always was: kReactMs (the
    /// owner, 2026-11-15: 30 s for slow PCs). Every look of it asks the stop.
    /// Settable for a test.
    void set_react_ms(int ms) { react_ms_ = ms; }
    int  react_ms() const { return react_ms_; }

    /// Whether the three-button menu is drawn over the list, below this row.
    ///
    /// The menu closing is what says the button was pressed. Whether the
    /// invitation is then accepted by the server is a separate question: the
    /// client refuses one to somebody already in your party and leaves the
    /// list up, which is a refusal rather than a missed click.
    bool menu_up(const Frame& frame, int row_y) const;

    /// Whether the list is showing, by its ruled empty rows.
    bool open(const Frame& frame, const TemplateStore& store) const;

    /// Open it if it is not already, and place it from its button. False,
    /// with a reason, if the button that opens it is not on screen -- or when
    /// `stop` said so.
    bool ensure_open(InputManager& input, ScreenCapture& capture,
                     const TemplateStore& store, std::string* why,
                     const Stop& stop = {});
    /// Whether the last ensure_open() that said true pressed the button to
    /// get there, rather than finding the list up already -- the player's,
    /// perhaps, and left alone.
    bool opened_it() const { return opened_it_; }

    /// Shut the list (2026.11.17b, C): the owner's "cứ mở khung pt" -- every
    /// invite left it up over the right of the screen, and a message of the
    /// game's could be under it. By the same button that opens it, found by
    /// its picture wherever the client's size puts it (956,139 on a 1024x768
    /// client, 1212,139 on a 1280x720 one; no capture at 1366x768 yet, and
    /// the picture is searched for over the whole frame, as ensure_open
    /// does): the button flips between the list and the panel that was there
    /// before. Never ESC -- the client gives ESC to whatever window it likes.
    ///
    /// Not up on a fresh look, nor on a second kCloseSettleMs later: nothing
    /// pressed, true. A good invite shuts the list by itself a moment after
    /// Req Party, and a press then would open it again. Up on both: one press
    /// on its button, then looked at every quarter of a second, up to
    /// close_wait_ms(), until a look shows it gone -- true -- or not -- false,
    /// with why; never a second press, which would flip it back up. The
    /// game's mouse is the caller's all the while.
    bool close(InputManager& input, ScreenCapture& capture, const TemplateStore& store,
               std::string* why, const Stop& stop = {});
    /// The longest close() holds the game's mouse waiting for the list to go
    /// after its press: about as long as the party worker's other short
    /// holds (InviteReply::kCloseHoldMs), so the other jobs are not kept
    /// waiting by it. A list not seen gone by then is said once and left.
    static constexpr int kCloseWaitMs = 3000;
    static constexpr int kCloseSettleMs = 300;
    void set_close_wait_ms(int ms) { close_wait_ms_ = ms; }
    int  close_wait_ms() const { return close_wait_ms_; }

    /// Every row that has a name on it -- none until the list is placed.
    std::vector<Row> read(const Frame& frame, const GlyphFont& font) const;

    /// The row holding this player, or -1. Matched loosely -- see
    /// `min_name_match`.
    int find(const std::vector<Row>& rows, const std::string& who) const;

    /// Pick a row and press "Req Party". The caller has already decided this
    /// player should be invited. `stop` as for ensure_open.
    bool invite(InputManager& input, ScreenCapture& capture,
                const TemplateStore& store, const Row& row,
                std::string* why, const Stop& stop = {}) const;

private:
    /// A click on the game, with the caller's `stop` asked again after the
    /// pointer's pause, just before the button goes down.
    bool click(InputManager& input, int x, int y, const Stop& stop) const;
    /// Sleep `ms` in short slices, asking `stop` between them: false once it
    /// says stop. On the test's clock when set_clock gave one.
    bool nap(int ms, const Stop& stop) const;
    /// A fresh frame of the game (or of the test's grab).
    bool grab(InputManager& input, ScreenCapture& capture, Frame& into) const;
    /// Milliseconds on the steady clock, or the test's.
    std::uint64_t clock_ms() const;

    Settings settings_;
    Settings placed_;
    bool     placed_ok_ = false;
    int      frame_w_ = 0, frame_h_ = 0;    ///< the frame the list was placed in
    std::function<bool(int, int)> click_;
    std::function<bool(Frame&)> grab_;
    std::function<std::uint64_t()> now_;
    std::function<void(int)> sleep_;
    int      react_ms_ = kReactMs;
    int      close_wait_ms_ = kCloseWaitMs;
    bool     opened_it_ = false;
};

} // namespace gobot
