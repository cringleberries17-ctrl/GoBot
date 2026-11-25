#pragma once

// A start GoBot owes (2026.11.24).
//
// The owner, 2026-11-24: "Kiểm tra các phần khác xem có phần nào không làm
// được thì dừng không, cần thay cơ chế này bằng 1 cơ chế khác" -- and, of the
// drops, "thử lại luôn ... chứ không phải là dừng 90s". Tự đánh quái ticked and
// asked for by the run -- BẮT ĐẦU, the cold start once the keeper is in the
// world, the bin's doubt over (resume_after_bin), the tick put on mid-run --
// and refused by something that passes was not asked again until the next
// BẮT ĐẦU: Npcap or its service up late after the PC's start (the hunting
// spot's spot_refusal), a chat line or the bin's dialog in doubt that is seen
// shut a moment later, the detector not up the first time. The rest of the
// run went on all night without the fighting. The macro a resume asked for
// the same, and the cold start's whole start refused by a doubt with the
// keeper alone running.
//
// Now such a start is owed: GoBot's window asks again every kOwedAskMs while
// the run is on (GuiApp::owed_tick), through the start's own checks, and at
// once when a watcher sees the doubt that refused it over. Never after the
// player's own stop -- DỪNG, F12, closing GoBot, F11, the full screen's Attack
// switch, the tick taken off -- and BẮT ĐẦU starts afresh. Pure and tested
// (jobs_gui_test.cpp).

#include "gobot/react_wait.hpp"

#include <cstdint>

namespace gobot {

/// How often a start owed is asked again: the owner's 30 s for laggy PCs
/// (kReactMs, 2026-11-15) -- each ask goes through the start's checks, and a
/// refusal's note is said again only when its reason changed.
inline constexpr std::uint64_t kOwedAskMs = static_cast<std::uint64_t>(kReactMs);

/// Whether a start owed is asked again now: with the run on (`running`; for
/// the cold start's jobs, its keeper running), what it starts still wanted
/// (`wanted`: Tự đánh quái ticked, a key on) and not running already
/// (`started`), and no sooner than kOwedAskMs after the last ask (`asked_ms`,
/// on now_ms()'s clock; 0: at once -- a watcher saw its doubt over). A last ask
/// later than `now_ms` is no ask due.
constexpr bool owed_due(std::uint64_t now_ms, std::uint64_t asked_ms, bool running, bool wanted, bool started) {
    if (!running || !wanted || started) return false;
    return asked_ms == 0 || (now_ms >= asked_ms && now_ms - asked_ms >= kOwedAskMs);
}

/// Whether a start owed is owed no longer: the run over -- every job ended by
/// itself (DỪNG, F12 and closing GoBot forget it at once) -- what it starts no
/// longer wanted, or started meanwhile, by whatever asked.
constexpr bool owed_dropped(bool running, bool wanted, bool started) {
    return !running || !wanted || started;
}

} // namespace gobot
