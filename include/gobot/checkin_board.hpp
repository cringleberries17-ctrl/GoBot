#pragma once

#include "gobot/template_matcher.hpp"

#include <cstdint>

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

/// How alike the title has to be where the board was seen whole a moment ago
/// (PopupEyes::board_covered_at): the game writes its lines of news ("Khôi
/// phục năng lực Quỷ Hồn.", the clan's guards...) at the top of the screen,
/// over the board, and two of them across the title bring it down to 0.70 --
/// 0.80 to 0.89 for less -- on the frame of 2026-11-17 21:36:32. With no
/// board up, the title scores 0.44 at best anywhere on the saved frames. Never
/// for a first sight: that is kCheckinBoardScore's alone.
constexpr float kCheckinBoardCoveredScore = 0.60f;

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

/// When the board may be shut with ESC (the owner, 2026-11-15: "Sau khi chọn
/// nhân vật xong và vào tới màn hình game thì sẽ chờ 30 giây rồi mới ấn ESC
/// để tắt thông báo. Trường hợp qua ngày thì chờ tới 00:03 mới ấn ESC"):
///
/// - not before kCheckinBoardLoginWaitMs from the later of the character's
///   arrival in the world (the keeper's crossing, when there was one) and the
///   board's first sight at its place -- it comes up right at the arrival, so
///   a login by hand is waited for the same way;
/// - never while the PC's clock reads from kCheckinNightFromS (23:55:00) to
///   kCheckinNightToS (00:03:00): the game opens the board again as the day
///   passes, and a PC clock a few minutes behind the game's sees that board
///   before its own 00:00 -- the five minutes before midnight are held for
///   that, and nothing is pressed for the board until 00:03.
constexpr std::uint64_t kCheckinBoardLoginWaitMs = 30'000;
constexpr int kCheckinNightFromS = 23 * 3600 + 55 * 60;
constexpr int kCheckinNightToS = 3 * 60;

/// The PC's clock is in the board's night hold. `second` is the second of the
/// local day, 0 to 86399; a negative one -- a pretend clock that has no time
/// of day -- is never held.
constexpr bool checkin_night_hold(int second) {
    return second >= 0 && (second >= kCheckinNightFromS || second < kCheckinNightToS);
}

/// The board's ESC with the character down (2026.11.21, the audit's B10). The
/// popup closer presses nothing with HP read under bag::kJobDownHpPct, and the
/// reviver knows a death only by its panel: the board opened over a panel
/// already up -- a death in the last seconds before 00:00, the board of the
/// new day on top of it -- hid the panel, the reviver saw the character
/// alive, the closer saw it down, and nothing was pressed until a person did.
/// Now the board alone -- closed with ESC, nothing clicked -- gets its ESC
/// with the character down, but only when the reviver runs and its last turn,
/// at most kCheckinDownLookMs before, saw no death panel nor its question:
/// the ESC then goes to the board on top, never to a panel the reviver can
/// see. Everything else of the board's stays: its night hold to 00:03, its
/// 30 s after the arrival, the other gates. `no_panel_ms`: that turn's time
/// on `now`'s clock, 0 for none.
constexpr std::uint64_t kCheckinDownLookMs = 4000;
constexpr bool checkin_down_esc(bool board, bool reviver_running, std::uint64_t no_panel_ms, std::uint64_t now) {
    return board && reviver_running && no_panel_ms != 0 && now >= no_panel_ms &&
           now - no_panel_ms <= kCheckinDownLookMs;
}

/// The second of the local day by the PC's clock, 0 to 86399.
int local_second_of_day();

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
/// pictures rather than a store (the quick quest's QuestPictures). `min_score`
/// below kCheckinBoardScore only with `around`: a board known to be there,
/// its title partly covered (kCheckinBoardCoveredScore).
bool checkin_board_title(const Frame& f, const Frame& picture, ScreenRect* title = nullptr, float* score = nullptr,
                         const ScreenRect* around = nullptr, float min_score = kCheckinBoardScore);

} // namespace gobot
