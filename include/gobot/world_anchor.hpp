#pragma once

#include "gobot/game_state.hpp"
#include "gobot/mob_detector.hpp"

#include <cstdint>
#include <vector>

namespace gobot {

/// Ties the server's coordinates to the screen.
///
/// Two halves of the same picture arrive separately and neither is complete.
/// The server broadcasts where every creature in the level is, in its own
/// units, and says nothing whatsoever about where you are -- your own position
/// is not in its stream, and the client's half, which does carry it, is
/// obfuscated. The screen shows creatures and your character together, but in
/// pixels, relative to a camera that moves, and with no idea which creature is
/// which.
///
/// Put the two together and the missing quantity falls out. Every pairing of a
/// creature on the wire with a creature on the screen implies an offset between
/// world and screen. Wrong pairings imply scattered nonsense; right ones all
/// imply the same offset, because they are all looking through the same camera.
/// So the offset that the most pairings agree on is the camera, and once the
/// camera is known the character's world position is simply wherever it is
/// standing on screen, plus that.
///
/// No correlation, no calibration run, no walking about: three creatures
/// visible at once is enough, and it re-solves itself every frame.
class WorldAnchor {
public:
    /// How the screen maps onto the server's coordinates.
    ///
    /// The scale is solved for rather than assumed. It is tempting to take the
    /// server's units for screen pixels -- the numbers are the right order of
    /// magnitude -- but they are not: two platforms a few hundred pixels apart
    /// on screen are nearly seven hundred units apart on the wire. Assuming
    /// 1:1 makes every pairing disagree with every other and no amount of
    /// evidence ever adds up.
    struct Fix {
        bool   valid = false;
        double scale = 1.0;         ///< world = screen * scale + offset
        double offset_x = 0.0;
        double offset_y = 0.0;
        int    agreed = 0;          ///< creatures the transform explains
        int    considered = 0;      ///< landmarks the answer was judged on
        double chance = 0.0;        ///< how many a random transform would get
    };

    /// Feed one frame of each half, plus where the view is.
    ///
    /// `seen` is what the detector found on screen; its players are ignored,
    /// since the server's creature list has no entry for your own character
    /// and pairing it against one would be pairing it against nothing.
    ///
    /// Nothing is matched here. Matching what is on screen now against what is
    /// on the wire now cannot work on this game: two or three creatures are
    /// visible at a time, and two points exactly determine a scale and an
    /// offset, so every wrong pairing fits perfectly and nothing is ruled out.
    ///
    /// What this does instead is remember. A creature that is standing still
    /// has a screen position that changes only as the camera scrolls, so
    /// adding the camera back gives a number that is the same every time it is
    /// seen -- a fixed landmark, in screen units. Walk the length of a map and
    /// a dozen of them accumulate, each seen at a different moment. Those are
    /// then matched against the creature list all at once, and a dozen
    /// landmarks is enough to rule out everything but the truth.
    void observe(const std::vector<Monster>& world, const std::vector<Nameplate>& seen,
                 double camera_x, double camera_y);

    /// How many landmarks have been collected, and how many are solid enough
    /// to match on.
    int landmarks() const;

    /// Start over: a new map has nothing to do with the old one.
    void reset();

    /// The best current answer, which may be from an earlier frame.
    Fix current() const { return held_; }

    /// The landmarks being matched on, most-seen first. For seeing why a fix
    /// is or is not being believed.
    struct Rival {
        double wander = 0.0;        ///< how far it strayed, in screen pixels
        double x = 0.0, y = 0.0;    ///< screen position plus camera
        int    weight = 0;          ///< times seen
    };
    std::vector<Rival> rivals(std::size_t count) const;

    /// Whether the held answer is recent enough to steer by.
    bool fresh(std::uint64_t now_ms) const;

    /// Turn a screen position into a world position, using the held fix.
    ///
    /// The camera has to be passed in because the fix is expressed against
    /// screen coordinates with the camera added back -- that is the frame in
    /// which the landmarks stand still, and so the frame the transform was
    /// solved in. Handing it a bare screen position would answer as though the
    /// view had never scrolled.
    bool to_world(int screen_x, int screen_y, double camera_x, double camera_y,
                  Vec2& out) const;

    /// How far apart a landmark and a creature may be and still count as the
    /// same thing, in SCREEN pixels.
    ///
    /// Screen and not world, which matters more than it sounds. Judging the
    /// error in the server's units lets a candidate transform widen its own
    /// tolerance by shrinking its scale, and the search duly discovers that a
    /// scale of 0.4 explains every landmark at once -- the degenerate answer
    /// where everything is close to everything. Measured on the screen, where
    /// the landmarks actually live, the tolerance is the same whatever the
    /// transform claims, and that answer scores no better than it deserves.
    ///
    /// Separate axes, because the axes are measured differently. A name plate
    /// is centred over its creature, so x lines up closely; the plate floats
    /// above it by however tall the sprite is, which differs between species,
    /// so y is only good to within a body.
    static constexpr double kAgreeToleranceX = 36.0;
    static constexpr double kAgreeToleranceY = 110.0;

    /// The range of scales worth considering. Anything outside it is two
    /// creatures that happened to be close together on screen implying an
    /// absurd zoom, not a real correspondence.
    static constexpr double kMinScale = 0.6;
    static constexpr double kMaxScale = 4.0;

    /// Landmarks a fit must explain before its scale is taken as settled.
    ///
    /// The scale is how many of the server's units fit in a screen pixel. That
    /// is a property of the game's rendering, not of the map or the moment --
    /// it does not change while the client is running. Re-deriving it on every
    /// solve means re-deriving it from whichever landmarks happen to be in
    /// hand, and it wanders by ten per cent between frames, which moves the
    /// character by a few hundred units. Once a fit explains enough to be
    /// trusted, the scale is kept and only the offset is solved for -- one
    /// unknown instead of two, from the same evidence.
    static constexpr int kScaleSettledAt = 10;

    /// How close a fresh fit has to be to the settled scale before it is taken
    /// as the same answer measured again, rather than a different answer.
    static constexpr double kScaleAgrees = 0.15;

    /// Landmarks that a transform has to explain before it is believed.
    ///
    /// Two is meaningless -- two points define the transform, so any pairing
    /// of two landmarks with two creatures fits exactly. Four landmarks all
    /// landing on four different creatures, to within a body's width, is a
    /// coincidence worth about one in a million.
    static constexpr int kMinExplained = 4;

    /// And by this much more than chance would have managed on its own.
    static constexpr double kBeatChanceBy = 1.6;

    /// How many times a creature has to be seen in the same spot before it
    /// counts as a landmark rather than something passing through.
    static constexpr int kMinSightings = 6;

    /// How far a landmark may wander and still be thought stationary, in
    /// screen pixels. A monster that paces its platform is not a landmark.
    static constexpr double kMaxWander = 26.0;

    /// How close two sightings must be to be the same landmark. Generous, so
    /// that a plate measured a few pixels differently from one frame to the
    /// next is one landmark rather than three.
    static constexpr double kSameLandmark = 30.0;

    /// How many landmarks are matched on, best-attested first.
    static constexpr std::size_t kMaxMarksUsed = 12;

    /// Landmarks are matched afresh every so many frames, not every frame:
    /// the search is over every pair of landmarks against every pair of
    /// creatures, which is worth doing well rather than often.
    static constexpr int kSolveEvery = 12;

    /// How long a fix stays usable without being renewed. The camera moves,
    /// but not instantly, and losing sight of the creatures for a moment is
    /// not a reason to forget where the level is.
    static constexpr std::uint64_t kHoldMs = 2500;

private:
    /// A creature that was seen standing in one place, remembered by where
    /// that place is in screen units with the camera added back.
    struct Landmark {
        double x = 0.0, y = 0.0;    ///< screen position plus camera
        int    seen = 0;
        double spread = 0.0;        ///< how much it has wandered since
    };

    void solve();

    std::vector<Landmark> landmarks_;
    std::vector<Monster>  world_;       ///< the creature list, as last seen
    Fix                   held_;
    std::uint64_t         held_at_ms_ = 0;
    int                   since_solve_ = 0;

    double                settled_scale_ = 0.0;   ///< 0 until it is known
    int                   settled_from_ = 0;      ///< landmarks that settled it
};

} // namespace gobot
