#include "gobot/checkin_board.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>

namespace gobot {

namespace {

/// Whether a search of a whole frame has been timed this GoBot start.
std::atomic<bool> g_timed{ false };

ScreenRect clamp_to(const Frame& f, ScreenRect r) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
    const int x1 = std::min(f.width, r.right()), y1 = std::min(f.height, r.bottom());
    return ScreenRect{ x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0) };
}

} // namespace

const Frame* checkin_board_picture(const TemplateStore& store) {
    if (const Frame* named = store.get(kCheckinBoardTitle)) return named;
    return store.get(kCheckinBoardTitleFile);
}

bool checkin_board_title(const Frame& f, const TemplateStore& store, ScreenRect* title, float* score,
                         const ScreenRect* around) {
    const Frame* pic = checkin_board_picture(store);
    return pic != nullptr && checkin_board_title(f, *pic, title, score, around);
}

bool checkin_board_title(const Frame& f, const Frame& picture, ScreenRect* title, float* score,
                         const ScreenRect* around) {
    const Frame* pic = &picture;
    if (!pic->valid() || !f.valid()) return false;
    // The whole frame, or round where it was: the board is never dragged, and a
    // title a few pixels off is the same board drawn a little aside.
    constexpr int kSlack = kCheckinRelookSlack;
    const ScreenRect area =
        around == nullptr ? ScreenRect{ 0, 0, f.width, f.height }
                          : clamp_to(f, ScreenRect{ around->x - kSlack, around->y - kSlack, pic->width + 2 * kSlack,
                                                    pic->height + 2 * kSlack });
    if (area.w < pic->width || area.h < pic->height) return false;
    const bool time_it = around == nullptr && !g_timed.exchange(true);
    const auto from = std::chrono::steady_clock::now();
    const auto m = TemplateMatcher::find(f, area, *pic, kCheckinBoardScore);
    if (time_it) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from);
        GB_INFO("Checkin: a title search on %dx%d took %lld ms", f.width, f.height,
                static_cast<long long>(ms.count()));
    }
    if (!m.found) return false;
    if (title != nullptr) *title = m.at;
    if (score != nullptr) *score = m.score;
    return true;
}

} // namespace gobot
