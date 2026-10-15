#pragma once

#include "gobot/config.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

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
        int menu_wait_ms = 1100;

        /// How alike a row has to be to the name being looked for.
        ///
        /// Not an exact match, because these names are read off the screen,
        /// and lower here than elsewhere because this list reads worse than
        /// the invitation dialog does: the same player came back as
        /// "Mag cTo h n", which is seven of ten letters in the right order.
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

    /// For tests: the clicks go here instead of to the game.
    void set_click(std::function<bool(int, int)> fn) { click_ = std::move(fn); }

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
    /// with a reason, if the button that opens it is not on screen.
    bool ensure_open(InputManager& input, ScreenCapture& capture,
                     const TemplateStore& store, std::string* why);

    /// Every row that has a name on it -- none until the list is placed.
    std::vector<Row> read(const Frame& frame, const GlyphFont& font) const;

    /// The row holding this player, or -1. Matched loosely -- see
    /// `min_name_match`.
    int find(const std::vector<Row>& rows, const std::string& who) const;

    /// Pick a row and press "Req Party". The caller has already decided this
    /// player should be invited.
    bool invite(InputManager& input, ScreenCapture& capture,
                const TemplateStore& store, const Row& row,
                std::string* why) const;

private:
    bool click(InputManager& input, int x, int y) const;

    Settings settings_;
    Settings placed_;
    bool     placed_ok_ = false;
    int      frame_w_ = 0, frame_h_ = 0;    ///< the frame the list was placed in
    std::function<bool(int, int)> click_;
};

} // namespace gobot
