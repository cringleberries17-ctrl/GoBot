// Template matching on OpenCV.
//
// Compiled only when GOBOT_HAVE_OPENCV is defined; otherwise the hand-rolled
// matcher in template_matcher.cpp is used instead and this file holds only
// the stand-ins at its end. Both satisfy the same two functions, so
// everything above them is unchanged.
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
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace gobot {

/// A OneFrame's frame, and that frame turned into grey and into colour, each
/// made by the first search under it that needs it.
struct TemplateMatcher::OneFrame::State {
    const Frame*        frame = nullptr;
    const std::uint8_t* data = nullptr;     ///< its pixels' place: moved or resized, it is another frame
    int                 width = 0;
    int                 height = 0;
    State*              outer = nullptr;    ///< the one this was made inside, on the same thread
    cv::Mat             grey;
    cv::Mat             colour;
};

namespace {

/// The innermost OneFrame alive on this thread, or null.
thread_local TemplateMatcher::OneFrame::State* t_one_frame = nullptr;

/// A frame's pixels as a BGRA matrix, not copied.
cv::Mat bgra_of(const Frame& f) {
    return cv::Mat(f.height, f.width, CV_8UC4, const_cast<std::uint8_t*>(f.pixels.data()));
}

/// The part of `area` that lies inside the frame; zero-sized when none does.
cv::Rect part_of(const Frame& f, const ScreenRect& area) {
    const int x0 = std::max(0, area.x), y0 = std::max(0, area.y);
    const int x1 = std::min(f.width, area.right()), y1 = std::min(f.height, area.bottom());
    if (x1 <= x0 || y1 <= y0) return cv::Rect(0, 0, 0, 0);
    return cv::Rect(x0, y0, x1 - x0, y1 - y0);
}

bool none(const cv::Rect& r) { return r.width <= 0 || r.height <= 0; }

int conversion(bool colour) { return colour ? cv::COLOR_BGRA2BGR : cv::COLOR_BGRA2GRAY; }

/// Wrap a frame's pixels as a BGR matrix. Copies: the alpha channel has to go,
/// and the region of interest below would otherwise alias the caller's buffer.
cv::Mat image_of(const Frame& f, const ScreenRect& area, bool colour) {
    const cv::Rect part = part_of(f, area);
    if (none(part)) return {};

    cv::Mat out;
    cv::cvtColor(bgra_of(f)(part), out, conversion(colour));
    return out;
}

/// The OneFrame on this thread that holds `f`, or null.
TemplateMatcher::OneFrame::State* one_frame_of(const Frame& f) {
    for (TemplateMatcher::OneFrame::State* s = t_one_frame; s != nullptr; s = s->outer)
        if (s->frame == &f && s->data == f.pixels.data() && s->width == f.width && s->height == f.height)
            return s;
    return nullptr;
}

/// The whole of a OneFrame's frame in grey or colour, converted at the first
/// search that asks.
const cv::Mat& whole_of(TemplateMatcher::OneFrame::State& s, bool colour) {
    cv::Mat& m = colour ? s.colour : s.grey;
    if (m.empty()) cv::cvtColor(bgra_of(*s.frame), m, conversion(colour));
    return m;
}

/// A `rows` x `cols` matrix of `type` cut from `kept`, which grows to the
/// largest size asked of it and keeps it.
///
/// Why a view rather than the matrix itself: OpenCV writes in place into a
/// matrix that already has the size and type asked, a view included, and
/// allocates again for any other. The popups look for four pictures of four
/// sizes in each frame, so a score map kept as it was would still be 3 MB
/// made afresh at every look; cut to size from one that only grows, it is
/// made once.
cv::Mat room(cv::Mat& kept, int rows, int cols, int type) {
    if (kept.empty() || kept.type() != type || kept.rows < rows || kept.cols < cols)
        kept.create(std::max(kept.rows, rows), std::max(kept.cols, cols), type);
    return kept(cv::Rect(0, 0, cols, rows));
}

/// What one search writes into: the part of the frame looked at, converted,
/// its score map and the map of where the scores blew up.
///
/// Made afresh for every search, these were up to 5 MB allocated and freed
/// several times a second by each worker that looks -- most of GoBot's page
/// faults, and on a PC with one core and 3.5 GB, time and memory the game
/// was short of. So they are kept and lent out, one set per search running
/// at that moment.
///
/// Kept in one pool for the process rather than one set per thread: a dozen
/// threads look now and then, and std::async's run on Windows' own pool
/// threads, which never end; a set each would keep tens of MB for good,
/// where searches running at the same moment are one or two.
struct Workspace {
    cv::Mat grey;
    cv::Mat colour;
    cv::Mat scores;
    cv::Mat broken;
};

class Workspaces {
public:
    /// Sets kept between searches. More running at once are made, used and
    /// let go, as every search's were before.
    static constexpr std::size_t kKept = 2;

    Workspaces() { kept_.reserve(kKept); }

    std::unique_ptr<Workspace> take() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!kept_.empty()) {
                std::unique_ptr<Workspace> w = std::move(kept_.back());
                kept_.pop_back();
                return w;
            }
        }
        return std::make_unique<Workspace>();
    }

    /// Never allocates -- there is room reserved for every set kept -- so it
    /// is safe in a destructor.
    void give(std::unique_ptr<Workspace> w) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (w && kept_.size() < kKept) kept_.push_back(std::move(w));
    }

private:
    std::mutex                              mutex_;
    std::vector<std::unique_ptr<Workspace>> kept_;
};

Workspaces& workspaces() {
    // Never destroyed: a worker still searching as the process ends must not
    // hand its set back to a pool that is already gone.
    static Workspaces* const pool = new Workspaces();
    return *pool;
}

/// A set borrowed for one search, given back however the search ends.
class Lease {
public:
    Lease() : w_(workspaces().take()) {}
    ~Lease() { workspaces().give(std::move(w_)); }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Workspace* operator->() const { return w_.get(); }

private:
    std::unique_ptr<Workspace> w_;
};

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

/// A template made ready to match: its pixels without the alpha, and its mask.
struct Prepared {
    cv::Mat needle;
    cv::Mat mask;     ///< empty when the picture is fully opaque
};

/// Each template made ready once rather than at every search.
///
/// The same few pictures are looked for again and again -- the popups' four
/// every turn, the close buttons at every look of the bag -- and each search
/// converted the picture and built its mask pixel by pixel, again.
///
/// A picture is known by where its pixels are and its size, and taken only
/// if its pixels are still the ones it was made from: a Frame let go and its
/// place given to another picture, or one written over in place, is made
/// again rather than matched with the mask of what was there before. What is
/// handed out is never written into afterwards -- a picture made again gets
/// new matrices -- so a search on another thread still using the old ones is
/// not disturbed.
class PreparedTemplates {
public:
    /// All the pictures kept, in bytes, the copies they are checked against
    /// included. The pictures looked for are small -- buttons, icons, titles
    /// -- and a few hundred of them fit; past this the one longest unused is
    /// let go.
    static constexpr std::size_t kBudget = 4u << 20;
    /// A picture larger than this is made at every search, as before.
    static constexpr std::size_t kLargest = 1u << 20;

    Prepared get(const Frame& tpl, bool colour) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++clock_;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            Entry& e = entries_[i];
            if (e.data != tpl.pixels.data() || e.width != tpl.width || e.height != tpl.height ||
                e.colour != colour)
                continue;
            if (e.pixels == tpl.pixels) {
                e.used = clock_;
                return e.made;
            }
            bytes_ -= e.bytes;
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }

        Entry e;
        e.made.needle = image_of(tpl, ScreenRect{ 0, 0, tpl.width, tpl.height }, colour);
        e.made.mask = mask_of(tpl, colour);
        e.bytes = tpl.pixels.size() + e.made.needle.total() * e.made.needle.elemSize() +
                  e.made.mask.total() * e.made.mask.elemSize();
        if (e.bytes > kLargest) return e.made;

        e.data = tpl.pixels.data();
        e.width = tpl.width;
        e.height = tpl.height;
        e.colour = colour;
        e.pixels = tpl.pixels;
        e.used = clock_;
        while (!entries_.empty() && bytes_ + e.bytes > kBudget) {
            std::size_t oldest = 0;
            for (std::size_t i = 1; i < entries_.size(); ++i)
                if (entries_[i].used < entries_[oldest].used) oldest = i;
            bytes_ -= entries_[oldest].bytes;
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(oldest));
        }
        bytes_ += e.bytes;
        entries_.push_back(std::move(e));
        return entries_.back().made;
    }

private:
    struct Entry {
        const std::uint8_t*       data = nullptr;
        int                       width = 0;
        int                       height = 0;
        bool                      colour = false;
        std::vector<std::uint8_t> pixels;     ///< what it was made from
        Prepared                  made;
        std::size_t               bytes = 0;
        std::uint64_t             used = 0;
    };

    std::mutex         mutex_;
    std::vector<Entry> entries_;
    std::size_t        bytes_ = 0;
    std::uint64_t      clock_ = 0;
};

PreparedTemplates& prepared_templates() {
    // Never destroyed, for the same reason as the workspaces.
    static PreparedTemplates* const kept = new PreparedTemplates();
    return *kept;
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

/// The search proper, on the part of the frame looked at and the template,
/// both converted already.
///
/// One function for find_all and find_all_fresh, so that the two can differ
/// only in where they write: `scores` and `broken` come either empty, to be
/// made here, or as views of exactly the size this search needs, which
/// OpenCV writes in place.
void search(const cv::Mat& hay, const cv::Mat& needle, const cv::Mat& mask,
            cv::Mat& scores, cv::Mat& broken, const ScreenRect& look, const Frame& tpl,
            float min_score, std::size_t limit, std::vector<TemplateMatcher::Match>& out) {
    if (mask.empty()) cv::matchTemplate(hay, needle, scores, cv::TM_CCOEFF_NORMED);
    else              cv::matchTemplate(hay, needle, scores, cv::TM_CCOEFF_NORMED, mask);

    // Where the masked normalisation blew up. Those are not matches and
    // must not be allowed to look like good ones -- clamping them into
    // range is exactly how an infinity became a perfect score.
    cv::patchNaNs(scores, -1.0f);
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

        TemplateMatcher::Match m;
        m.found = true;
        m.score = static_cast<float>(real);
        m.at = ScreenRect{ look.x + where.x, look.y + where.y, tpl.width, tpl.height };
        out.push_back(m);
        if (out.size() >= limit) break;
    }
}

void strongest_first(std::vector<TemplateMatcher::Match>& out) {
    std::sort(out.begin(), out.end(), [](const TemplateMatcher::Match& a, const TemplateMatcher::Match& b) {
        return a.score > b.score;
    });
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
        const cv::Rect part = part_of(frame, look);
        if (none(part)) return out;
        const Prepared ready = prepared_templates().get(tpl, colour);
        if (ready.needle.empty()) return out;
        if (ready.needle.cols > part.width || ready.needle.rows > part.height) return out;

        Lease kept;
        cv::Mat hay;
        if (OneFrame::State* one = one_frame_of(frame)) {
            hay = whole_of(*one, colour)(part);
        } else {
            hay = room(colour ? kept->colour : kept->grey, part.height, part.width,
                       colour ? CV_8UC3 : CV_8UC1);
            cv::cvtColor(bgra_of(frame)(part), hay, conversion(colour));
        }
        cv::Mat scores = room(kept->scores, hay.rows - ready.needle.rows + 1,
                              hay.cols - ready.needle.cols + 1, CV_32FC1);
        cv::Mat broken = room(kept->broken, scores.rows, scores.cols, CV_8UC1);
        search(hay, ready.needle, ready.mask, scores, broken, look, tpl, min_score, limit, out);
    } catch (const cv::Exception& e) {
        GB_WARN("TemplateMatcher: OpenCV refused this match (%s)", e.what());
        return {};
    }

    strongest_first(out);
    return out;
}

std::vector<TemplateMatcher::Match> TemplateMatcher::find_all_fresh(
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
        cv::Mat broken;
        search(hay, needle, mask, scores, broken, look, tpl, min_score, limit, out);
    } catch (const cv::Exception& e) {
        GB_WARN("TemplateMatcher: OpenCV refused this match (%s)", e.what());
        return {};
    }

    strongest_first(out);
    return out;
}

TemplateMatcher::OneFrame::OneFrame(const Frame& frame) : state_(std::make_unique<State>()) {
    state_->frame = &frame;
    state_->data = frame.pixels.data();
    state_->width = frame.width;
    state_->height = frame.height;
    state_->outer = t_one_frame;
    t_one_frame = state_.get();
}

TemplateMatcher::OneFrame::~OneFrame() {
    t_one_frame = state_->outer;
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

#else // !GOBOT_HAVE_OPENCV

// The stand-ins. The hand-rolled matcher keeps no buffers between searches and
// converts nothing ahead, so a fresh search is its only search and OneFrame
// has nothing to hold.

#include "gobot/template_matcher.hpp"

namespace gobot {

struct TemplateMatcher::OneFrame::State {};

TemplateMatcher::OneFrame::OneFrame(const Frame&) {}
TemplateMatcher::OneFrame::~OneFrame() = default;

std::vector<TemplateMatcher::Match> TemplateMatcher::find_all_fresh(
        const Frame& frame, const ScreenRect& area, const Frame& tpl,
        float min_score, std::size_t limit, Tone tone) {
    return find_all(frame, area, tpl, min_score, limit, tone);
}

} // namespace gobot

#endif // GOBOT_HAVE_OPENCV
