#pragma once

namespace gobot {

// ---- how long GoBot waits for the game to react ----
//
// The owner, 2026-11-15, for players on slow PCs (1 core, 3.5 GB): "đối với
// những máy có hiện tượng lag, giật, đơ máy thì khi GoBot không thấy hiển thị
// túi đồ hoặc không thấy màn hình phản ứng thì sẽ dừng xử lý ... khi GoBot
// thực hiện thao tác, sẽ có thêm 30 giây chờ phản ứng. Nếu màn hình phản ứng
// luôn thì thực hiện tiếp bước tiếp theo, còn nếu màn hình không phản ứng gì
// thì sẽ dừng xử lý."
//
// So after GoBot does something to the game -- a key, a click, a right-click,
// a double-click, a drag, typing, an ESC -- it looks for what that did (the
// bag shown, a dialog up, a window gone, a tab chosen) for up to kReactMs, by
// the clock, at the look gap that step always had: on the first look that
// shows it the next step goes at once, and only after kReactMs with nothing
// shown is the step given up, the way it was given up before on its shorter
// wait. Every look of such a wait still asks the job's stop (F12, DỪNG) and its
// guards (the hold, the character down).
//
// It is never a reason to press again: a key that toggles -- the bag's, the
// chat's Enter, ESC -- pressed twice on a slow game shuts what the first one
// opened late. Controls a second click cannot undo keep the extra clicks they
// had, spread over the wait.
//
// Waits that are not reactions keep their own times: settles before a look,
// two looks that have to agree, and waits for something that may never come
// (a message a purchase may or may not bring up).
inline constexpr int kReactMs = 30'000;

/// A reaction that came later than this is said in the log, with how long it
/// took: what a slow PC looks like, measured.
inline constexpr int kReactSlowMs = 2'000;

} // namespace gobot
