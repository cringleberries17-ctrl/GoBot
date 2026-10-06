#include "gobot/world_anchor.hpp"
#include "gobot/common.hpp"

#include <algorithm>
#include <cmath>

namespace gobot {
namespace {

/// A map is big and a long session sees a lot of it; cap the table so a
/// confused run cannot grow it without bound.
constexpr std::size_t kMaxLandmarks = 256;

} // namespace

void WorldAnchor::reset() {
    landmarks_.clear();
    world_.clear();
    held_ = Fix{};
    held_at_ms_ = 0;
    since_solve_ = 0;
    // The settled scale survives: it belongs to the game, not to the map that
    // has just been left.
}

int WorldAnchor::landmarks() const {
    int solid = 0;
    for (const Landmark& l : landmarks_)
        if (l.seen >= kMinSightings && l.spread <= kMaxWander) ++solid;
    return solid;
}

void WorldAnchor::observe(const std::vector<Monster>& world,
                          const std::vector<Nameplate>& seen,
                          double camera_x, double camera_y) {
    // Remember where each creature on screen was standing, in screen units
    // with the camera added back. For anything that is not moving, that number
    // is the same every time it is seen, however much the view has scrolled in
    // between -- so sightings minutes apart are sightings of the same thing
    // and can be pooled.
    for (const Nameplate& n : seen) {
        if (n.is_player) continue;
        const double px = n.x() + camera_x;
        const double py = n.y() + camera_y;

        Landmark* hit = nullptr;
        for (Landmark& l : landmarks_) {
            if (std::abs(l.x - px) > kSameLandmark) continue;
            if (std::abs(l.y - py) > kSameLandmark) continue;
            hit = &l;
            break;
        }
        if (!hit) {
            if (landmarks_.size() < kMaxLandmarks)
                landmarks_.push_back(Landmark{ px, py, 1, 0.0 });
            continue;
        }

        // How far it has strayed from where it was first seen, which is what
        // separates a landmark from a monster on patrol.
        hit->spread = std::max(hit->spread,
                               std::max(std::abs(px - hit->x), std::abs(py - hit->y)));
        const double w = hit->seen;
        hit->x = (hit->x * w + px) / (w + 1);
        hit->y = (hit->y * w + py) / (w + 1);
        ++hit->seen;
    }

    if (++since_solve_ < kSolveEvery) return;
    since_solve_ = 0;

    // Keep the world list on hand for the solve, which runs over everything
    // collected rather than over this frame.
    world_ = world;
    solve();
}

void WorldAnchor::solve() {
    // The best-attested landmarks only, and not many of them.
    //
    // Two things fill the table with rubbish. A monster pacing its platform
    // lays down a trail: each step far enough from the last becomes its own
    // landmark, and each looks perfectly stationary because it was only ever
    // seen once or twice. And the detector's own false positives -- a lantern,
    // a flower -- do the same.
    //
    // Keeping them is worse than wasteful. The number of landmarks sets the
    // bar a transform has to clear, since the chance of a stray match scales
    // with how many strays there are; thirty landmarks of which six are real
    // makes the real answer look like noise. A creature that is genuinely
    // standing somewhere the character keeps walking past is seen over and
    // over, so sorting by that and taking the top handful keeps the evidence
    // and discards the trail.
    std::vector<const Landmark*> marks;
    for (const Landmark& l : landmarks_)
        if (l.seen >= kMinSightings && l.spread <= kMaxWander) marks.push_back(&l);

    std::sort(marks.begin(), marks.end(),
              [](const Landmark* a, const Landmark* b) { return a->seen > b->seen; });
    if (marks.size() > kMaxMarksUsed) marks.resize(kMaxMarksUsed);
    if (marks.size() < static_cast<std::size_t>(kMinExplained)) return;
    if (world_.size() < static_cast<std::size_t>(kMinExplained)) return;

    // How thickly the creatures are spread, which is what a match has to beat
    // to mean anything.
    //
    // Counting matches alone cannot choose a scale, and the failure is not
    // subtle: it picks whichever end of the range is allowed. Judge the error
    // in world units and a small scale wins, because it packs the landmarks
    // together until each is near something. Judge it on the screen and a
    // large scale wins, because it spreads them out until each covers
    // something. Both are the same mistake -- counting hits without asking how
    // many a shuffled deck would have produced.
    //
    // So the score is hits minus the hits expected by chance at that scale.
    // A transform now has to explain the landmarks better than a random one
    // would, and the degenerate answers score nothing because that is exactly
    // what they are.
    double world_lo = 1e30, world_hi = -1e30;
    for (const Monster& m : world_) {
        world_lo = std::min(world_lo, static_cast<double>(m.pos.x));
        world_hi = std::max(world_hi, static_cast<double>(m.pos.x));
    }
    const double world_span = std::max(1.0, world_hi - world_lo);

    // The ranking is "hits beyond chance" and the gate is applied afterwards,
    // not here. Ranking and gating are different jobs: a baseline strict
    // enough to be a sensible gate rejects every hypothesis during the search,
    // leaving nothing to gate.
    int    best_explained = 0;
    double best_score = -1e18;
    double best_chance = 0.0;
    double best_scale = 1.0, best_x = 0.0, best_y = 0.0;
    std::vector<std::uint8_t> taken;

    for (std::size_t a = 0; a < marks.size(); ++a) {
        for (std::size_t b = a + 1; b < marks.size(); ++b) {
            const double dmark = marks[a]->x - marks[b]->x;
            if (std::abs(dmark) < 60.0) continue;      // too close to divide by

            for (const Monster& p : world_) {
                for (const Monster& q : world_) {
                    if (&p == &q) continue;
                    const double scale = (p.pos.x - q.pos.x) / dmark;
                    if (scale < kMinScale || scale > kMaxScale) continue;

                    const double ox = p.pos.x - scale * marks[a]->x;
                    const double oy = p.pos.y - scale * marks[a]->y;

                    // One landmark, one creature.
                    //
                    // Letting two landmarks both claim the same creature is
                    // what lets a tiny scale win: squeeze the landmarks close
                    // enough together and they all pile onto whichever few
                    // creatures they land near, scoring as though each had
                    // been recognised. A matching cannot be padded that way --
                    // fourteen landmarks explained means fourteen different
                    // creatures, which a collapsed transform cannot produce.
                    taken.assign(world_.size(), 0);
                    int explained = 0;
                    for (const Landmark* l : marks) {
                        const double wx = scale * l->x + ox;
                        const double wy = scale * l->y + oy;
                        for (std::size_t k = 0; k < world_.size(); ++k) {
                            if (taken[k]) continue;
                            const Monster& m = world_[k];
                            if (std::abs(m.pos.x - wx) > kAgreeToleranceX * scale) continue;
                            if (std::abs(m.pos.y - wy) > kAgreeToleranceY * scale) continue;
                            taken[k] = 1;
                            ++explained;
                            break;
                        }
                    }
                    // What a transform of this scale would have explained had
                    // the landmarks been scattered at random.
                    const double window = 2.0 * kAgreeToleranceX * scale;
                    const double by_chance =
                        static_cast<double>(marks.size()) *
                        std::min(1.0, window * static_cast<double>(world_.size()) / world_span);
                    const double score = explained - by_chance;

                    if (score <= best_score) continue;
                    best_score = score;
                    best_chance = by_chance;
                    best_explained = explained;
                    best_scale = scale;
                    best_x = ox;
                    best_y = oy;
                }
            }
        }
    }
    if (best_explained < kMinExplained) {
        held_.valid = false;
        held_.agreed = best_explained;
        held_.considered = static_cast<int>(marks.size());
        held_.chance = best_chance;
        return;
    }

    // Refit on everything that agreed, rather than keeping the two points the
    // search happened to start from.
    //
    // Two points determine the transform exactly, which means they also carry
    // all of their own error into it: a landmark measured a few pixels off
    // tilts the whole scale, and the answer jumps about between frames as the
    // search lands on different starting pairs. Fitting a line through every
    // agreeing pair averages that away, and the result stops wobbling.
    for (int pass = 0; pass < 2; ++pass) {
        double n = 0.0, sl = 0.0, sw = 0.0, sll = 0.0, slw = 0.0;
        double sy_mark = 0.0, sy_world = 0.0;
        taken.assign(world_.size(), 0);
        for (const Landmark* l : marks) {
            const double wx = best_scale * l->x + best_x;
            const double wy = best_scale * l->y + best_y;
            const Monster* match = nullptr;
            for (std::size_t k = 0; k < world_.size(); ++k) {
                if (taken[k]) continue;
                const Monster& m = world_[k];
                if (std::abs(m.pos.x - wx) > kAgreeToleranceX * best_scale) continue;
                if (std::abs(m.pos.y - wy) > kAgreeToleranceY * best_scale) continue;
                taken[k] = 1;
                match = &m;
                break;
            }
            if (!match) continue;
            n += 1.0;
            sl += l->x;
            sw += match->pos.x;
            sll += l->x * l->x;
            slw += l->x * match->pos.x;
            sy_mark += l->y;
            sy_world += match->pos.y;
        }
        if (n < 2.0) break;
        const double denom = n * sll - sl * sl;
        if (std::abs(denom) < 1e-6) break;

        const double fitted = (n * slw - sl * sw) / denom;

        // Snap to the settled scale when this fit broadly agrees with it.
        //
        // Not by filtering the search -- constraining what may be looked for
        // turned a working fit into nothing at all, because one early and
        // merely adequate answer got to decide what every later one was
        // allowed to be. The search stays free; what the settled value does is
        // steady the last step, where a handful of landmarks would otherwise
        // re-derive a game constant from scratch and get it ten per cent
        // different each time.
        const bool agrees = settled_scale_ > 0.0 &&
                            std::abs(fitted - settled_scale_) <= settled_scale_ * kScaleAgrees;
        best_scale = agrees ? settled_scale_ : fitted;
        best_scale = std::max(kMinScale, std::min(kMaxScale, best_scale));
        best_x = (sw - best_scale * sl) / n;
        // One scale for both axes: the view is not stretched, and y has far
        // less spread to fit a slope through.
        best_y = (sy_world - best_scale * sy_mark) / n;
    }

    // Count again against the refitted transform: the refit is what will be
    // used, so it is the refit that has to be shown to work.
    int agreed = 0;
    taken.assign(world_.size(), 0);
    for (const Landmark* l : marks) {
        const double wx = best_scale * l->x + best_x;
        const double wy = best_scale * l->y + best_y;
        for (std::size_t k = 0; k < world_.size(); ++k) {
            if (taken[k]) continue;
            const Monster& m = world_[k];
            if (std::abs(m.pos.x - wx) > kAgreeToleranceX * best_scale) continue;
            if (std::abs(m.pos.y - wy) > kAgreeToleranceY * best_scale) continue;
            taken[k] = 1;
            ++agreed;
            break;
        }
    }

    Fix f;
    f.considered = static_cast<int>(marks.size());
    f.agreed = agreed;
    f.chance = best_chance;
    f.scale = best_scale;
    f.offset_x = best_x;
    f.offset_y = best_y;
    // Believed only if it beat chance by a clear margin as well as clearing
    // the floor. Explaining nine landmarks sounds convincing until you work
    // out that eight would have fallen somewhere useful anyway.
    f.valid = agreed >= kMinExplained &&
              static_cast<double>(agreed) >= best_chance * kBeatChanceBy;

    // A fit this good is the best evidence there will be about the scale, and
    // the scale does not change afterwards. Keep it, and let later solves ask
    // only where the map is.
    if (agreed >= kScaleSettledAt && agreed > settled_from_) {
        settled_scale_ = best_scale;
        settled_from_ = agreed;
    }

    // Kept either way, so a caller can watch the evidence gather rather than
    // stare at a blank until it crosses the line. Only a fix that passed is
    // stamped with a time, which is what `fresh` and `to_world` go by.
    held_ = f;
    if (f.valid) held_at_ms_ = now_ms();
}

std::vector<WorldAnchor::Rival> WorldAnchor::rivals(std::size_t count) const {
    std::vector<Rival> out;
    for (const Landmark& l : landmarks_) {
        if (l.seen < kMinSightings || l.spread > kMaxWander) continue;
        out.push_back(Rival{ l.spread, l.x, l.y, l.seen });
    }
    std::sort(out.begin(), out.end(),
              [](const Rival& a, const Rival& b) { return a.weight > b.weight; });
    if (out.size() > count) out.resize(count);
    return out;
}

bool WorldAnchor::fresh(std::uint64_t at_ms) const {
    return held_.valid && held_at_ms_ != 0 && at_ms - held_at_ms_ <= kHoldMs;
}

bool WorldAnchor::to_world(int screen_x, int screen_y, double camera_x, double camera_y,
                           Vec2& out) const {
    if (!held_.valid) return false;
    out.x = static_cast<float>(held_.scale * (screen_x + camera_x) + held_.offset_x);
    out.y = static_cast<float>(held_.scale * (screen_y + camera_y) + held_.offset_y);
    return true;
}

} // namespace gobot
