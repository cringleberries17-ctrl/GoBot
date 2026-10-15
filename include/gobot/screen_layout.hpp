#pragma once

#include "gobot/config.hpp"
#include "gobot/mob_detector.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <string>
#include <vector>

namespace gobot {

/// Where the client's own panels are, for a client area of a given size.
///
/// The client plays at 1280x720 as well as at 1024x768 -- the player's runs
/// at 1280x720, every one of 1,036 saved frames of it -- and its panels are
/// not at fixed places on both. Each follows the edge it sits against: the
/// bars and pet slots at the top left stay put, the minimap with its notices
/// column and the icons down the right follow the right edge, and the hotbar
/// and the chat follow the bottom. So these are worked out from the size of
/// the frame, measured on frames of both sizes, rather than written down for
/// one of them.
namespace screen_layout {

/// Where monsters are looked for: below the top HUD -- its bars and pet slots
/// end at y 104 on both sizes -- and above the hotbar and chat, the bottom 98
/// rows. The whole width: the panels down the right are cut out of it by the
/// two below, not by narrowing it.
ScreenRect play_field(int width, int height);

/// The minimap and the "Thông báo hoạt động" notices column under it, down
/// the top right: 212 wide, 360 tall, against the right edge.
ScreenRect notices(int width, int height);

/// The icon column on the right edge -- calendar, gift, portrait -- whose red
/// reads as health bars: 40 wide, the bottom 200 rows.
ScreenRect right_icons(int width, int height);

/// The closed chat bar's top-left on a client of `height` rows, where it is
/// when it cannot be found: (84, height - 22) on every saved frame of both
/// sizes, as its picture is found with its mask.
int chat_bar_x();
int chat_bar_y(int height);

} // namespace screen_layout

/// The last lines of the chat panel, placed from the chat bar under them.
///
/// The keyword reader used to read at y 710, 723 and 736 -- measured on a
/// 1024x768 client -- and on the player's 720-row client the first of those
/// boxes ran off the bottom of the frame, so nothing was ever read. The panel
/// sits on the bottom edge and its lines sit on the chat bar, so they are
/// placed from the bar instead: the closed bar, the commercial client's
/// inven/0/dialog_chat_res3.spr/1 (310x16, the picture GoC finds the chat
/// line by too), is found in the bottom-left quarter, and the two lines a
/// collapsed panel shows are written 34 and 18 rows above its top, 16 apart.
/// Above those is the panel's tab strip, which is never read.
///
/// When the bar is not found -- the chat line is open, or a dialog is over
/// it -- the last place it was found is used; when it never has been, where
/// it is on every saved frame, and that is said once in the log.
class ChatLines {
public:
    struct Settings {
        std::string bar_picture = "inven/0/dialog_chat_res3.spr/1";
        float       bar_score = 0.95f;
        int         lines = 2;          ///< how many lines to read, newest last

        /// party.ini [Chat] place = fixed: the numbers below are used as
        /// written, the old way. Anything else, or no key at all, places the
        /// lines from the bar -- the player's own party.ini still holds the
        /// 1024x768 numbers, and having them must not switch this off.
        bool fixed = false;
        int  x = 6, y = 710, width = 390, line_height = 13;

        void load(const IniFile& ini);
    };

    /// Where the bar was taken to be, for the last boxes.
    enum class Bar { Found, Remembered, Assumed, Fixed };
    static const char* bar_name(Bar b);

    void set_settings(const Settings& s) { settings_ = s; }
    const Settings& settings() const { return settings_; }

    /// The boxes to read in `frame`, top line first. Looks for the bar near
    /// where it was last, and over the whole bottom-left quarter at most
    /// every couple of seconds, so a reader asking several times a second
    /// does not pay for a full search each time.
    std::vector<ScreenRect> boxes(const Frame& frame, const TemplateStore& store);

    /// The bar the last boxes were placed from.
    Bar bar() const { return bar_; }
    int bar_x() const { return bar_x_; }
    int bar_y() const { return bar_y_; }

    /// The line boxes for a bar whose top-left is `x`,`y`, top line first: at
    /// most `lines`, none of them reaching up into the tab strip, and none
    /// off a frame `frame_height` rows tall.
    static std::vector<ScreenRect> above_bar(int x, int y, int lines, int frame_height);

private:
    bool find_bar(const Frame& frame, const TemplateStore& store);

    Settings      settings_;
    Bar           bar_ = Bar::Assumed;
    int           bar_x_ = -1, bar_y_ = -1;     ///< the last place found
    bool          found_once_ = false;
    bool          said_assumed_ = false;
    std::uint64_t searched_ms_ = 0;             ///< the last search of the whole quarter
};

} // namespace gobot
