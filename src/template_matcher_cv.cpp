// Template matching on OpenCV.
//
// Compiled only when GOBOT_HAVE_OPENCV is defined; otherwise the hand-rolled
// matcher in template_matcher.cpp is used instead and this file is empty.
// Both satisfy the same two functions, so everything above them is unchanged.
//
// Why the change. Both clients this project replaces use OpenCV, and matching
// by hand turned out to be a poor trade twice over. Once for correctness: the
// hand-rolled version searched a pyramid of reduced copies, the levels were
// built by halving, integer halving turns a step of three into one rather than
// two, and the descent multiplied the coordinate by the wrong number. Every
// template whose shorter side was around eighteen pixels was invisible, in
// silence. And once for speed: the features still to be built match dozens of
// pictures against every frame -- forty inventory slots, sixteen exchange
// items -- and eighty milliseconds apiece does not survive that.
//
// It also fixes something the hand-rolled version could not do at all. That
// one reduced every picture to grey before correlating, so an icon in red and
// the same icon in blue were the same picture to it. Several of the sets here
// are exactly that: four crest tiers, sixteen exchange items. matchTemplate
// correlates the channels, so the colour counts.

#ifdef GOBOT_HAVE_OPENCV

#include "gobot/template_matcher.hpp"
#include "gobot/logger.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace gobot {
namespace {

/// Wrap a frame's pixels as a BGR matrix. Copies: the alpha channel has to go,
/// and the region of interest below would otherwise alias the caller's buffer.
cv::Mat image_of(const Frame& f, const ScreenRect& area, bool colour) {
    const int x0 = std::max(0, area.x), y0 = std::max(0, area.y);
    const int x1 = std::min(f.width, area.right()), y1 = std::min(f.height, area.bottom());
    if (x1 <= x0 || y1 <= y0) return {};

    const cv::Mat whole(f.height, f.width, CV_8UC4,
                        const_cast<std::uint8_t*>(f.pixels.data()));
    cv::Mat out;
    cv::cvtColor(whole(cv::Rect(x0, y0, x1 - x0, y1 - y0)), out,
                 colour ? cv::COLOR_BGRA2BGR : cv::COLOR_BGRA2GRAY);
    return out;
}

/// A template's transparency, as a three channel mask, or an empty matrix when
/// the picture is fully opaque.
///
/// Three channels rather than one because matchTemplate wants the mask to have
/// the same shape as what it is matching, and a one channel mask against a
/// three channel template is one of the ways it throws.
cv::Mat mask_of(const Frame& f, bool colour) {
    bool any_transparent = false;
    for (std::size_t i = 3; i < f.pixels.size(); i += 4)
        if (f.pixels[i] < TemplateMatcher::kOpaque) { any_transparent = true; break; }
    if (!any_transparent) return {};

    cv::Mat mask(f.height, f.width, colour ? CV_8UC3 : CV_8UC1);
    for (int y = 0; y < f.height; ++y) {
        for (int x = 0; x < f.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * f.width + x) * 4;
            const auto v = static_cast<std::uint8_t>(
                f.pixels[i + 3] >= TemplateMatcher::kOpaque ? 255 : 0);
            if (colour) mask.at<cv::Vec3b>(y, x) = cv::Vec3b(v, v, v);
            else        mask.at<std::uint8_t>(y, x) = v;
        }
    }
    return mask;
}

/// Work out the real correlation at one position, over the mask only.
///
/// matchTemplate is used to find candidates, not to score them. Its masked
/// normalised correlation has positions where the denominator collapses -- a
/// patch of flat colour under a mostly-transparent template -- and those come
/// back as infinity rather than as an error. Clamping that into the valid
/// range turns the worst possible match into the best possible score.
///
/// That is not hypothetical. Run against the live client, an eighteen pixel
/// close button that is a third transparent matched a flat stretch of a
/// notification banner at exactly 1.000, and the bot clicked that empty patch
/// six times running, reporting success each time.
///
/// So every candidate is scored again here, directly: subtract the means,
/// divide by the deviations, over the opaque pixels and across the channels.
/// It costs one template-sized pass per candidate, and it cannot be fooled by
/// a flat region -- a flat region has no deviation to divide by, and is
/// refused outright rather than scored.
double true_score(const cv::Mat& hay, const cv::Mat& needle, const cv::Mat& mask,
                  int px, int py) {
    const int channels = needle.channels();
    if (px < 0 || py < 0) return -1.0;
    if (px + needle.cols > hay.cols || py + needle.rows > hay.rows) return -1.0;

    double sum_a = 0.0, sum_b = 0.0;
    long long n = 0;
    for (int y = 0; y < needle.rows; ++y) {
        const std::uint8_t* nrow = needle.ptr<std::uint8_t>(y);
        const std::uint8_t* hrow = hay.ptr<std::uint8_t>(py + y);
        const std::uint8_t* mrow = mask.empty() ? nullptr : mask.ptr<std::uint8_t>(y);
        for (int x = 0; x < needle.cols; ++x) {
            if (mrow && mrow[x * channels] == 0) continue;
            for (int c = 0; c < channels; ++c) {
                sum_a += hrow[(px + x) * channels + c];
                sum_b += nrow[x * channels + c];
                ++n;
            }
        }
    }
    if (n < 24) return -1.0;

    const double mean_a = sum_a / static_cast<double>(n);
    const double mean_b = sum_b / static_cast<double>(n);

    double cov = 0.0, var_a = 0.0, var_b = 0.0;
    for (int y = 0; y < needle.rows; ++y) {
        const std::uint8_t* nrow = needle.ptr<std::uint8_t>(y);
        const std::uint8_t* hrow = hay.ptr<std::uint8_t>(py + y);
        const std::uint8_t* mrow = mask.empty() ? nullptr : mask.ptr<std::uint8_t>(y);
        for (int x = 0; x < needle.cols; ++x) {
            if (mrow && mrow[x * channels] == 0) continue;
            for (int c = 0; c < channels; ++c) {
                const double a = hrow[(px + x) * channels + c] - mean_a;
                const double b = nrow[x * channels + c] - mean_b;
                cov += a * b;
                var_a += a * a;
                var_b += b * b;
            }
        }
    }

    // Either side being flat means there is nothing to compare. A flat patch
    // correlates with everything and with nothing, so it is refused rather
    // than scored. Roughly two grey levels of deviation is the floor.
    const double least = static_cast<double>(n) * 4.0;
    if (var_a < least || var_b < least) return -1.0;

    const double s = cov / std::sqrt(var_a * var_b);
    return std::isfinite(s) ? s : -1.0;
}

} // namespace

std::vector<TemplateMatcher::Match> TemplateMatcher::find_all(
        const Frame& frame, const ScreenRect& area, const Frame& tpl,
        float min_score, std::size_t limit, Tone tone) {
    std::vector<Match> out;
    if (!frame.valid() || !tpl.valid() || limit == 0) return out;

    const ScreenRect look = area.valid() ? area
                                         : ScreenRect{ 0, 0, frame.width, frame.height };
    if (tpl.width > look.w || tpl.height > look.h) return out;

    try {
        const bool colour = tone == Tone::Colour;
        const cv::Mat hay = image_of(frame, look, colour);
        const cv::Mat needle = image_of(tpl, ScreenRect{ 0, 0, tpl.width, tpl.height }, colour);
        if (hay.empty() || needle.empty()) return out;
        if (needle.cols > hay.cols || needle.rows > hay.rows) return out;

        const cv::Mat mask = mask_of(tpl, colour);

        cv::Mat scores;
        if (mask.empty()) cv::matchTemplate(hay, needle, scores, cv::TM_CCOEFF_NORMED);
        else              cv::matchTemplate(hay, needle, scores, cv::TM_CCOEFF_NORMED, mask);

        // Where the masked normalisation blew up. Those are not matches and
        // must not be allowed to look like good ones -- clamping them into
        // range is exactly how an infinity became a perfect score.
        cv::patchNaNs(scores, -1.0f);
        cv::Mat broken;
        cv::compare(scores, 1.0 + 1e-3, broken, cv::CMP_GT);
        scores.setTo(-1.0f, broken);

        // Candidates from matchTemplate, scored again properly. It is allowed
        // to over-report here -- a candidate below the asked-for score may
        // still verify above it, and the other way round -- so the net is cast
        // a little wider and `true_score` has the final say.
        const double net = std::max(0.3, static_cast<double>(min_score) - 0.2);
        for (std::size_t tried = 0; tried < limit + 16; ++tried) {
            double headline = 0.0;
            cv::Point where;
            cv::minMaxLoc(scores, nullptr, &headline, nullptr, &where);
            if (!std::isfinite(headline) || headline < net) break;

            // Taken out of the running whether or not it verifies, so the next
            // pass finds the next thing rather than this one again.
            const int x0 = std::max(0, where.x - tpl.width / 2);
            const int y0 = std::max(0, where.y - tpl.height / 2);
            const int x1 = std::min(scores.cols, where.x + tpl.width / 2 + 1);
            const int y1 = std::min(scores.rows, where.y + tpl.height / 2 + 1);
            scores(cv::Rect(x0, y0, x1 - x0, y1 - y0)).setTo(-1.0f);

            const double real = true_score(hay, needle, mask, where.x, where.y);
            if (real < min_score) continue;

            Match m;
            m.found = true;
            m.score = static_cast<float>(real);
            m.at = ScreenRect{ look.x + where.x, look.y + where.y, tpl.width, tpl.height };
            out.push_back(m);
            if (out.size() >= limit) break;
        }
    } catch (const cv::Exception& e) {
        GB_WARN("TemplateMatcher: OpenCV refused this match (%s)", e.what());
        return {};
    }

    std::sort(out.begin(), out.end(),
              [](const Match& a, const Match& b) { return a.score > b.score; });
    return out;
}

TemplateMatcher::Match TemplateMatcher::find(const Frame& frame, const ScreenRect& area,
                                             const Frame& tpl, float min_score,
                                             Tone tone) {
    const auto all = find_all(frame, area, tpl, min_score, 1, tone);
    return all.empty() ? Match{} : all.front();
}

std::string TemplateMatcher::backend() {
    // Asked of OpenCV itself rather than written down here, so this cannot
    // drift from what is actually linked.
    return "OpenCV " + cv::getVersionString() + ", linked into this executable";
}

} // namespace gobot

#endif // GOBOT_HAVE_OPENCV
