#pragma once

#include "gobot/template_matcher.hpp"

namespace gobot {

/// The "Sự kiện và điểm danh" board: the game's events window, which the client
/// opens on the "Điểm danh" calendar after every login and again as the clock
/// passes 00:00 (the owner, 2026-11-09). 878x600, it covers the middle of a
/// 1024x768 window, and a click there lands on it instead of on the bag, the
/// shop or the quest window under it.
///
/// Told by its title bar, "Sự kiện và điểm danh": the 280x30 picture GoBot ships
/// as grabbed/auto_login_success_indicator, cut from the other helper, which
/// took the board's coming up for its sign that a login had worked. It is the
/// client's own art -- a mean difference of 0.51 of 255 against the title of
/// dialog_EventIntergration.spr, at 298,3 of the board -- and every pixel of it
/// is opaque. templates.ini names it checkin_board_title.
///
/// Shared by everything that must not click into the board or that closes it:
/// the bag jobs (BagCleaner), the quick quest and the popup closer.

/// The title's name in templates.ini, and the file it stands for: a store
/// loaded with the name map has both, a test's own store either.
inline const char* const kCheckinBoardTitle = "checkin_board_title";
inline const char* const kCheckinBoardTitleFile = "grabbed/auto_login_success_indicator";

/// How alike the title has to be: a bar of words and frame 280 wide, which
/// nothing else of the game draws.
constexpr float kCheckinBoardScore = 0.90f;

/// The board from its title's top-left, as the client's art has it: the whole
/// window, its top band -- where its X is, at +555,5 from the title, never
/// clicked here -- and the X itself.
constexpr ScreenRect kCheckinBoardFromTitle{ -298, -3, 878, 600 };
constexpr ScreenRect kCheckinBandFromTitle{ -298, -3, 878, 40 };
constexpr ScreenRect kCheckinXFromTitle{ 555, 5, 18, 18 };

/// How far from where it was a title looked for again may be: a search of
/// 296x46 rather than of the whole frame.
constexpr int kCheckinRelookSlack = 8;

/// How long a job that found the board up -- the bag jobs, the quick quest --
/// waits before it looks again: not trouble, and nothing is pressed meanwhile.
/// The popup closer, when "Tự đóng thông báo" is ticked, has closed it by then.
constexpr int kCheckinBoardWaitMs = 60'000;

/// The title picture in `store`, by its name in templates.ini or else by its
/// file's; null when neither is there, and then the board is never seen.
const Frame* checkin_board_picture(const TemplateStore& store);

/// The board's title on `f`: anywhere, or with `around` -- a title as found a
/// moment ago -- only within kCheckinRelookSlack of it. `title` gets where it
/// is, `score` how alike it is; false with no picture, no frame or no title at
/// kCheckinBoardScore. The first search of a whole frame in a GoBot start says
/// in the log how long it took.
bool checkin_board_title(const Frame& f, const TemplateStore& store, ScreenRect* title = nullptr,
                         float* score = nullptr, const ScreenRect* around = nullptr);
/// The same with the title picture itself, for a job that keeps its own few
/// pictures rather than a store (the quick quest's QuestPictures).
bool checkin_board_title(const Frame& f, const Frame& picture, ScreenRect* title = nullptr, float* score = nullptr,
                         const ScreenRect* around = nullptr);

} // namespace gobot
