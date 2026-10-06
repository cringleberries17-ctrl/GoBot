#include "gobot/camera_tracker.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cmath>

namespace gobot {
namespace {

inline int luma(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return (r * 77 + g * 151 + b * 28) >> 8;
}

/// How many points are compared per candidate shift. A coarse grid is enough:
/// the answer is a whole-pixel translation of a large, detailed picture, and
/// a few hundred well-spread samples pin that down as well as thousands.
constexpr int kGridX = 28;
constexpr int kGridY = 18;

} // namespace

void CameraTracker::set_play_area(const ScreenRect& r) {
    std::lock_guard<std::mutex> lock(mutex_);
    area_ = r;
    prev_.clear();
    prev_w_ = prev_h_ = 0;
    fix_ = Fix{};
    predicted_dx_ = predicted_dy_ = 0;
}

ScreenRect CameraTracker::play_area() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return area_;
}

void CameraTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    prev_.clear();
    prev_w_ = prev_h_ = 0;
    fix_ = Fix{};
    travelled_ = 0.0;
    predicted_dx_ = predicted_dy_ = 0;
}

CameraTracker::Fix CameraTracker::fix() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fix_;
}

double CameraTracker::travelled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return travelled_;
}

void CameraTracker::to_world(int sx, int sy, double& wx, double& wy) const {
    std::lock_guard<std::mutex> lock(mutex_);
    wx = fix_.x + sx;
    wy = fix_.y + sy;
}

void CameraTracker::to_screen(double wx, double wy, int& sx, int& sy) const {
    std::lock_guard<std::mutex> lock(mutex_);
    sx = static_cast<int>(std::lround(wx - fix_.x));
    sy = static_cast<int>(std::lround(wy - fix_.y));
}

bool CameraTracker::estimate_shift(const std::vector<std::uint8_t>& prev, int w, int h,
                                   const Frame& cur, const ScreenRect& area,
                                   int predict_dx, int predict_dy,
                                   int& out_dx, int& out_dy, float& confidence) {
    if (w <= 0 || h <= 0 || prev.size() != static_cast<std::size_t>(w) * h) return false;
    if (!cur.valid()) return false;

    // Sample points well inside the area, so that every candidate shift has
    // somewhere to read from in both pictures.
    struct Sample { int x, y, v; };
    std::vector<Sample> samples;
    samples.reserve(kGridX * kGridY);

    // Margin enough for the furthest the search can reach, which is the
    // predicted shift plus the window around it.
    const int reach = kMaxShift + kSearchRadius;
    const int x0 = reach, x1 = w - reach;
    const int y0 = reach, y1 = h - reach;
    if (x1 <= x0 || y1 <= y0) return false;

    double mean = 0.0;
    for (int gy = 0; gy < kGridY; ++gy) {
        for (int gx = 0; gx < kGridX; ++gx) {
            const int x = x0 + (x1 - x0) * gx / (kGridX - 1);
            const int y = y0 + (y1 - y0) * gy / (kGridY - 1);
            const int v = prev[static_cast<std::size_t>(y) * w + x];
            samples.push_back(Sample{ x, y, v });
            mean += v;
        }
    }
    mean /= static_cast<double>(samples.size());

    // A featureless view -- a loading screen, a cave wall -- matches every
    // shift equally well, and the best of those is meaningless.
    double spread = 0.0;
    for (const Sample& s : samples) spread += std::abs(s.v - mean);
    spread /= static_cast<double>(samples.size());
    if (spread < 6.0) return false;

    double best = 1e18, second = 1e18;
    int best_dx = 0, best_dy = 0;
    double total = 0.0;
    int considered = 0;

    // Search around where the view was already heading, not around nothing.
    //
    // A blind search has to cover every shift it might ever need, and the cost
    // is the square of that reach -- so the reach gets set to whatever is
    // affordable, and a character walking briskly outruns it. The view does
    // not change direction between one frame and the next, though: it carries
    // on at about the speed it was going. Centring a modest window on that
    // speed follows a fast scroll that a far wider blind search would have
    // missed, for less work.
    //
    // The window still reaches back past zero, so a view that stops dead -- a
    // character hitting a wall, which happens constantly -- is caught in the
    // same pass rather than counting as a loss.
    // The window covers the prediction and standing still, not just the
    // prediction. A character that turns round goes from scrolling one way to
    // scrolling the other between two frames, so a window that merely follows
    // the old direction loses the view at every reversal -- which on a bot
    // that paces back and forth is most of the time.
    const int centre_dx = std::clamp(predict_dx, -kMaxShift, kMaxShift);
    const int centre_dy = std::clamp(predict_dy, -kMaxShift, kMaxShift);
    const int lo_dx = std::min(-kSearchRadius, centre_dx - kSearchRadius);
    const int hi_dx = std::max( kSearchRadius, centre_dx + kSearchRadius);
    const int lo_dy = std::min(-kSearchRadius, centre_dy - kSearchRadius);
    const int hi_dy = std::max( kSearchRadius, centre_dy + kSearchRadius);

    for (int dy = lo_dy; dy <= hi_dy; ++dy) {
        for (int dx = lo_dx; dx <= hi_dx; ++dx) {
            double sad = 0.0;
            for (const Sample& s : samples) {
                std::uint8_t r, g, b;
                cur.get(area.x + s.x - dx, area.y + s.y - dy, r, g, b);
                sad += std::abs(luma(r, g, b) - s.v);
            }
            sad /= static_cast<double>(samples.size());

            total += sad;
            ++considered;

            if (sad < best) {
                // Keep the runner-up from a different place, not from the
                // pixel next door: every shift near the right one scores
                // nearly as well, and comparing against those would call every
                // correct answer ambiguous.
                if (std::abs(dx - best_dx) > 3 || std::abs(dy - best_dy) > 3) second = best;
                best = sad;
                best_dx = dx;
                best_dy = dy;
            } else if (sad < second &&
                       (std::abs(dx - best_dx) > 3 || std::abs(dy - best_dy) > 3)) {
                second = sad;
            }
        }
    }
    if (considered == 0) return false;

    const double average = total / considered;
    if (average < 1e-6) return false;

    // How much better the winner is than a typical alignment. A real match on
    // detailed scenery is several times better; noise is not.
    const double margin = 1.0 - best / average;
    const double distinct = second > 0.0 ? 1.0 - best / second : 0.0;

    out_dx = best_dx;
    out_dy = best_dy;
    confidence = static_cast<float>(std::clamp(std::min(margin, distinct) * 2.0, 0.0, 1.0));
    return true;
}

void CameraTracker::update(const Frame& f) {
    if (!f.valid()) return;

    std::lock_guard<std::mutex> lock(mutex_);

    ScreenRect a = area_;
    if (!a.valid()) {
        // The middle of the window: above is the HUD, below is the chat and the
        // skill bar, and none of those scroll.
        a = ScreenRect{ f.width / 8, f.height / 6,
                        f.width - f.width / 4, f.height / 2 };
        area_ = a;
    }
    if (a.right() > f.width || a.bottom() > f.height) return;

    if (!prev_.empty() && prev_w_ == a.w && prev_h_ == a.h) {
        int dx = 0, dy = 0;
        float conf = 0.0f;
        if (estimate_shift(prev_, prev_w_, prev_h_, f, a,
                           predicted_dx_, predicted_dy_, dx, dy, conf) &&
            conf >= kMinConfidence) {
            fix_.tracking = true;
            fix_.dx = dx;
            fix_.dy = dy;
            fix_.x += dx;
            fix_.y += dy;
            fix_.confidence = conf;
            travelled_ += std::abs(dx) + std::abs(dy);
            ++fix_.updates;

            // Where to look next time. Averaged with the last guess rather
            // than replaced by this one, so a single odd frame does not aim
            // the next search at nothing.
            predicted_dx_ = (predicted_dx_ + dx * 3) / 4;
            predicted_dy_ = (predicted_dy_ + dy * 3) / 4;
        } else {
            // Nothing matched. The position is still the best known one, but
            // it is now stale by however far the view moved unseen, so say so
            // rather than quietly carrying on.
            fix_.tracking = false;
            fix_.confidence = conf;
            ++fix_.lost;
            // Stop predicting from a guess that has just been shown wrong:
            // aiming the next search where this one failed would keep it
            // failing. Start the next one from standing still.
            predicted_dx_ = predicted_dy_ = 0;
        }
    }

    prev_w_ = a.w;
    prev_h_ = a.h;
    prev_.resize(static_cast<std::size_t>(a.w) * a.h);
    for (int y = 0; y < a.h; ++y) {
        for (int x = 0; x < a.w; ++x) {
            std::uint8_t r, g, b;
            f.get(a.x + x, a.y + y, r, g, b);
            prev_[static_cast<std::size_t>(y) * a.w + x] =
                static_cast<std::uint8_t>(luma(r, g, b));
        }
    }
}

} // namespace gobot
