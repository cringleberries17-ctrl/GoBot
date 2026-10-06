#pragma once

#include "gobot/mob_detector.hpp"
#include "gobot/screen_capture.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

namespace gobot {

/// Where the view is looking, worked out from how the scenery moves.
///
/// Everything the detector reports is in screen pixels, and screen pixels mean
/// nothing the moment the camera scrolls: the same spot in the level is at a
/// different place on screen a second later. To save a position and come back
/// to it, the position has to be fixed to the level rather than to the window.
///
/// This gets that by watching the background slide. Between two frames the
/// scenery shifts by exactly the distance the camera moved — a 2D game scrolls
/// by whole pixels, so matching one frame against the next gives that distance
/// exactly, with nothing left over to accumulate into drift. Adding those up
/// gives the camera's position, and the camera's position plus a thing's place
/// on screen gives where that thing is in the level.
///
/// The origin is wherever tracking started, which makes positions comparable
/// within a session rather than absolute. `travelled()` says how far the view
/// has moved since, so the UI can show whether a saved position is still
/// meaningful.
class CameraTracker {
public:
    /// Where to look for scenery. The HUD does not scroll, and matching it
    /// against itself would report that the camera never moves.
    void set_play_area(const ScreenRect& r);
    ScreenRect play_area() const;

    /// Feed one captured frame.
    void update(const Frame& f);

    /// Forget where we are. Call on a map change — the new level's scenery has
    /// nothing to do with the old one's and the first match would be garbage.
    void reset();

    struct Fix {
        bool   tracking = false;    ///< a match was found on the last frame
        double x = 0.0, y = 0.0;    ///< camera position, origin where we started
        int    dx = 0, dy = 0;      ///< how far it moved on the last frame
        float  confidence = 0.0f;   ///< 0..1; below ~0.25 the match means nothing
        std::uint64_t updates = 0;
        std::uint64_t lost = 0;     ///< frames where nothing matched
    };
    Fix fix() const;

    /// Total distance the view has moved since the origin was set.
    double travelled() const;

    /// Turn a screen position into a level position.
    void to_world(int screen_x, int screen_y, double& wx, double& wy) const;

    /// And back, for drawing a saved position on the live view.
    void to_screen(double wx, double wy, int& screen_x, int& screen_y) const;

    /// Match one frame against another and report the shift. Exposed for
    /// tests, which can shift a picture by a known amount and check it comes
    /// back.
    /// `predict_dx`/`predict_dy` say where the view was heading, and the
    /// search is centred there. Pass zero when there is nothing to go on.
    static bool estimate_shift(const std::vector<std::uint8_t>& prev, int w, int h,
                               const Frame& cur, const ScreenRect& area,
                               int predict_dx, int predict_dy,
                               int& dx, int& dy, float& confidence);

    /// Furthest the view is assumed to move between two frames. Beyond this a
    /// match is not looked for: that is a teleport or a map change, not a walk.
    static constexpr int kMaxShift = 56;

    /// How far either side of the predicted shift the match is looked for.
    ///
    /// Wide enough to work from a standing start, since the first frame after
    /// a loss or a map change has no prediction to go on and the window is
    /// then simply centred on zero. Centring it on the prediction is what buys
    /// the reach: at full speed the window sits around fifty-odd pixels of
    /// scroll and still extends forty either side, so the tracker follows a
    /// brisk walk that a blind search of the same cost would lose -- and
    /// losing it silently corrupts every position measured against it.
    static constexpr int kSearchRadius = 40;

    /// Below this the match is not worth acting on. Scenery is detailed enough
    /// that a correct alignment scores far better than a wrong one; anything
    /// marginal is a loading screen, a full-screen effect, or a jump too big
    /// to have been a walk.
    static constexpr float kMinConfidence = 0.25f;

private:
    mutable std::mutex mutex_;
    ScreenRect         area_;

    std::vector<std::uint8_t> prev_;     ///< previous frame's play area, grey
    int                       prev_w_ = 0, prev_h_ = 0;

    Fix    fix_;
    double travelled_ = 0.0;

    /// Where the view was heading, so the next search can start from there.
    int    predicted_dx_ = 0;
    int    predicted_dy_ = 0;
};

} // namespace gobot
