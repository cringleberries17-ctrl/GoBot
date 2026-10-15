#include "gobot/screen_layout.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cctype>

namespace gobot {
namespace screen_layout {
namespace {

// Measured on the player's 1280x720 frames and on the 1024x768 ones.
constexpr int kHudBottom   = 104;   ///< the top bars and the pet slots end here
constexpr int kBottomBand  = 98;    ///< the hotbar and the chat under the play field
constexpr int kNoticesW    = 212;
constexpr int kNoticesH    = 360;
constexpr int kIconsW      = 40;
constexpr int kIconsH      = 200;
/// The closed bar's top-left as its picture is found, masked: 84 from the
/// left, 22 rows up from the bottom. (Matched without its mask, which takes
/// in its 16 transparent corner pixels, it reads a column to the right.)
constexpr int kChatBarX    = 84;
constexpr int kChatBarUp   = 22;

} // namespace

ScreenRect play_field(int width, int height) {
    return ScreenRect{ 0, kHudBottom, std::max(0, width),
                       std::max(0, height - kHudBottom - kBottomBand) };
}

ScreenRect notices(int width, int /*height*/) {
    return ScreenRect{ width - kNoticesW, 0, kNoticesW, kNoticesH };
}

ScreenRect right_icons(int width, int height) {
    return ScreenRect{ width - kIconsW, height - kIconsH, kIconsW, kIconsH };
}

int chat_bar_x() { return kChatBarX; }
int chat_bar_y(int height) { return height - kChatBarUp; }

} // namespace screen_layout

// ------------------------------------------------------------------ chat lines

namespace {

// Where the lines are from the closed bar's top-left. The text of the newer
// line sits 18 rows above the bar and the older one 34, 16 apart; a box of
// 16 rows starting two above each takes the whole of its letters. The text
// starts 78 left of the bar, at x 6 on both sizes. The tab strip runs from
// 57 to 42 rows above the bar, and no box may reach into it.
constexpr int kLineUp     = 20;     ///< the newest line's box, from the bar's top
constexpr int kLinePitch  = 16;
constexpr int kLineH      = 16;
constexpr int kTextLeft   = 78;     ///< from the bar's left edge to the text's
constexpr int kLineW      = 390;
constexpr int kTabsBottom = 41;     ///< a box may start no higher than this above the bar

/// How far round the last place found the bar is looked for first.
constexpr int kNearPx = 12;
/// How often at most the whole bottom-left quarter is searched again.
constexpr std::uint64_t kSearchEveryMs = 2000;

} // namespace

void ChatLines::Settings::load(const IniFile& ini) {
    // Only an explicit "fixed" keeps the old numbers. They are in every
    // party.ini from before -- measured on a 1024x768 client -- so their
    // being there says nothing about wanting them.
    std::string place = ini.get("Chat", "place", "auto");
    std::transform(place.begin(), place.end(), place.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    fixed = place == "fixed";
    x = ini.get_int("Chat", "chat_x", x);
    y = ini.get_int("Chat", "chat_y", y);
    width = ini.get_int("Chat", "chat_width", width);
    line_height = std::max(6, ini.get_int("Chat", "line_height", line_height));
    // Fixed reads three lines unless told otherwise, as it always did; placed
    // from the bar, the two a collapsed panel shows.
    lines = std::max(1, ini.get_int("Chat", "lines", fixed ? 3 : lines));
    bar_picture = ini.get("Chat", "bar_picture", bar_picture);
}

const char* ChatLines::bar_name(Bar b) {
    switch (b) {
        case Bar::Found:      return "found";
        case Bar::Remembered: return "not found -- where it was last";
        case Bar::Assumed:    return "never found -- where it is on every client seen";
        case Bar::Fixed:      return "not looked for -- party.ini [Chat] place = fixed";
    }
    return "?";
}

std::vector<ScreenRect> ChatLines::above_bar(int x, int y, int lines, int frame_height) {
    std::vector<ScreenRect> out;
    for (int k = 0; k < lines; ++k) {
        const int top = y - kLineUp - kLinePitch * k;
        if (top < y - kTabsBottom) break;          // the tab strip: never read
        if (top < 0 || top + kLineH > frame_height) continue;
        out.push_back(ScreenRect{ std::max(0, x - kTextLeft), top, kLineW, kLineH });
    }
    // Top line first, as the panel shows them.
    std::reverse(out.begin(), out.end());
    return out;
}

bool ChatLines::find_bar(const Frame& frame, const TemplateStore& store) {
    const Frame* picture = store.get(settings_.bar_picture);
    if (picture == nullptr || !picture->valid() || !frame.valid()) return false;

    const auto take = [&](const ScreenRect& area) {
        const auto hit = TemplateMatcher::find(frame, area, *picture, settings_.bar_score);
        if (!hit.found) return false;
        bar_x_ = hit.at.x;
        bar_y_ = hit.at.y;
        found_once_ = true;
        return true;
    };

    // Where it was last, first: it does not move while the client keeps its
    // size, and that is a small search.
    if (found_once_) {
        const ScreenRect near{ bar_x_ - kNearPx, bar_y_ - kNearPx,
                               picture->width + 2 * kNearPx, picture->height + 2 * kNearPx };
        if (take(near)) return true;
    }
    // Then the whole bottom-left quarter, now and then.
    const std::uint64_t now = now_ms();
    if (searched_ms_ != 0 && now - searched_ms_ < kSearchEveryMs) return false;
    searched_ms_ = now;
    return take(ScreenRect{ 0, frame.height / 2, frame.width / 2, frame.height - frame.height / 2 });
}

std::vector<ScreenRect> ChatLines::boxes(const Frame& frame, const TemplateStore& store) {
    std::vector<ScreenRect> out;
    if (!frame.valid()) return out;

    if (settings_.fixed) {
        bar_ = Bar::Fixed;
        for (int i = 0; i < settings_.lines; ++i) {
            const ScreenRect box{ settings_.x, settings_.y + i * settings_.line_height,
                                  settings_.width, settings_.line_height + 3 };
            if (box.y < 0 || box.y + box.h > frame.height) break;
            out.push_back(box);
        }
        return out;
    }

    if (find_bar(frame, store)) {
        bar_ = Bar::Found;
    } else if (found_once_) {
        bar_ = Bar::Remembered;
    } else {
        bar_ = Bar::Assumed;
        bar_x_ = screen_layout::chat_bar_x();
        bar_y_ = screen_layout::chat_bar_y(frame.height);
        if (!said_assumed_) {
            said_assumed_ = true;
            GB_INFO("Chat: the chat bar (%s) is not on screen, so the lines are read where "
                    "it is on every client seen, its top at %d,%d",
                    settings_.bar_picture.c_str(), bar_x_, bar_y_);
        }
    }
    return above_bar(bar_x_, bar_y_, settings_.lines, frame.height);
}

} // namespace gobot
