#pragma once

#include "gobot/screen_capture.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gobot {

class MinimapTracker;
class CameraTracker;
class GlyphFont;
class IniFile;

/// A rectangle in the game window's client area.
struct ScreenRect {
    int x = 0, y = 0, w = 0, h = 0;

    bool valid() const { return w > 0 && h > 0; }
    int  cx() const { return x + w / 2; }
    int  cy() const { return y + h / 2; }
    int  right() const { return x + w; }
    int  bottom() const { return y + h; }

    bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
    bool overlaps(const ScreenRect& o) const {
        return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
    }
};

/// Something on screen wearing a name plate.
///
/// Monsters and players are drawn the same way: a dark translucent box with
/// the name in it, and a health bar directly underneath. The one difference is
/// the mana bar — players have a blue one below their red one, monsters have
/// nothing. That single test is what separates "attack this" from "leave this
/// alone", so the detector is built around finding health bars and then asking
/// what is underneath them.
///
/// The plate sits at the creature's feet rather than over its head, which is
/// convenient: the plate's position is the creature's ground position, and it
/// is the same offset for everything on screen.
struct Nameplate {
    ScreenRect bar;         ///< the health bar
    ScreenRect plate;       ///< the dark name box above it
    bool       is_player = false;
    /// Your own character: the plate made where the name you typed was read
    /// under its feet. Never a plate found by its bars: a plate with bars may
    /// be yours or another player's, a party member's included, and only the
    /// name says which.
    bool       is_self = false;
    float      hp = 1.0f;   ///< 0..1, approximate — the empty part is hard to bound

    /// What the plate says, when a glyph set was available to read it with.
    ///
    /// Accents may not survive and punctuation is not in the set at all, so
    /// this is not the name as the game would spell it -- but it is the same
    /// string every time for the same creature, which is what a whitelist
    /// needs.
    std::string name;
    float       name_confidence = 0.0f;

    /// Which monster it is, for a plate made from the game's packets
    /// (PacketEyes): its slot on the map, and the generation of the slot's
    /// monster, which a respawn changes (PacketWorld::Mob::gen). -1 and 0 for
    /// the screen's plates, which only where they stand tells apart.
    std::int32_t  slot = -1;
    std::uint32_t gen = 0;
    /// A packet plate's blows: when you last hurt it (0 for never); whether
    /// someone else has hurt it since, in the last few seconds; and whether
    /// your id has been proven the hitter's at all -- before that, neither
    /// of the two is to be gone by. Nothing for the screen's plates.
    std::uint64_t hurt_ms = 0;
    bool          hurt_by_other = false;
    bool          actor_checked = false;

    /// Ground position. Taken from the plate, not the bar: the bar is anchored
    /// on its left and shrinks rightward as the creature is hurt, so its centre
    /// drifts while the plate's does not.
    int x() const { return plate.valid() ? plate.cx() : bar.cx(); }
    int y() const { return bar.y; }
};

/// What counts as a health bar, and where to look for one.
struct DetectorTuning {
    /// The health bar's red. Generous, because the bar is drawn over whatever
    /// is behind it and the edges blend.
    int red_min = 140;          ///< red channel at least this
    int other_max = 80;         ///< green and blue at most this

    /// A mana bar underneath means a player.
    int blue_min = 120;
    int blue_red_max = 90;

    /// How many rows of writing have to sit above a health bar before it
    /// counts as a name plate.
    ///
    /// The box behind the writing is not looked for. It is translucent, so it
    /// takes the colour of whatever is behind it -- dark grey over rock,
    /// yellow-green over grass -- and no fixed description of it survives the
    /// character walking across a field. The writing stays white wherever it
    /// is, and scenery does not have any.
    int min_text_rows = 2;

    int min_bar = 10;           ///< shorter than this is artwork, not a bar
    int max_bar = 160;

    /// A health bar is a block, not a line. A single row of reddish pixels is
    /// a highlight on a sprite — a hatband, a sword hilt — and those come with
    /// dark outlines above them that pass for a name box.
    int min_bar_height = 2;


    /// How many bright, colourless pixels the box has to contain before it
    /// counts as holding a name. Dark artwork above a red sprite passes every
    /// other test; it has no writing on it.
    int min_name_px = 14;

    /// Where to look. Everything outside is ignored, which is how the game's
    /// own panels — quest log, skill bar, shop buttons, all full of red — are
    /// kept out of the results.
    ///
    /// Left empty, as it is unless written by hand, it is worked out from the
    /// frame's own size on every look -- below the top bars, above the hotbar
    /// and chat (screen_layout::play_field) -- so the same tuning serves a
    /// 1024x768 client and a 1280x720 one. A box measured for one size cut
    /// the right fifth off the other and took in its hotbar.
    ScreenRect area;

    /// Two rectangles to ignore inside the area, for panels that sit in the
    /// play field: the minimap and its notices down the top right, and the
    /// icons down the right edge, whose red reads as health bars. Each worked
    /// out from the frame's size while left empty, as `area` is.
    ScreenRect exclude;
    ScreenRect exclude2;

    /// Whether the box and both rectangles follow the frame's size: none of
    /// them has been written down by hand.
    bool automatic() const { return !area.valid() && !exclude.valid() && !exclude2.valid(); }

    /// This tuning as it is used on a frame `width` x `height`: each of the
    /// three rectangles left empty filled in for that size.
    DetectorTuning resolved(int width, int height) const;

    /// The measured colours, with the box and both rectangles following the
    /// frame's size.
    static DetectorTuning defaults();

    /// [Detect] in configs/hunt.ini, where a tuning edited by hand on the full
    /// screen is kept. A rectangle written as "auto", or not at all, follows
    /// the frame's size. `load` leaves anything missing as it was.
    void load(const IniFile& ini);
    /// Each key changed in place, so the rest of the file stays as it is.
    bool save(const std::string& path) const;
};

/// Finds monsters on screen.
class MobDetector {
public:
    MobDetector();
    ~MobDetector();

    MobDetector(const MobDetector&) = delete;
    MobDetector& operator=(const MobDetector&) = delete;

    /// The window to look at. A different one from the window it had -- the
    /// game found again, one picked from a list, or none -- forgets all that
    /// was seen in the old one at once (forget()).
    void set_target_window(void* hwnd);

    /// Forget every plate, where your character was last, the last frame and
    /// the counts stats() gives; a look already under way is thrown away
    /// rather than published. set_target_window() does this when the window
    /// changes, and so does a grab that fails: serving the last look of a
    /// closed or restarted game kept the hunter walking and attacking towards
    /// monsters where they stood in the old window, on the login pages too.
    void forget();

    /// Frames come from the window, through ScreenCapture, unless this is
    /// set; a test sets its own. Set it before start(), never while running.
    void set_grab(std::function<bool(void* window, Frame& out)> grab) { grab_ = std::move(grab); }

    void set_tuning(const DetectorTuning& t);
    DetectorTuning tuning() const;

    /// Optional: trackers fed from the same frames, so identifying the map and
    /// following the view cost no extra screen captures. Must outlive the
    /// detector.
    void set_minimap(MinimapTracker* m) { minimap_.store(m); }
    void set_camera(CameraTracker* c) { camera_.store(c); }

    void start();
    /// Returns at once, give or take a look under way: the wait between two
    /// looks is cut short (wake_). It is GoBot's own thread that stops it, at
    /// every F11, DỪNG and F12.
    void stop();
    bool running() const { return running_.load(); }

    /// The wait between two looks, as it always was: kLookGapMs until
    /// set_period_ms says otherwise.
    static constexpr int kLookGapMs = 140;
    int period_ms() const { return period_ms_.load(); }

    /// The wait between looks from now on, held to kMinPeriodMs..kMaxPeriodMs.
    /// GoBot's window asks for a longer one while the game's packets steer the
    /// hunter, and kLookGapMs otherwise (GuiApp::detector_pace_tick), every
    /// pass of its loop. A shorter one than the loop waits on takes effect at
    /// once -- the next look is taken now, not when the longer wait would have
    /// ended -- so the screen is looked at afresh the moment it steers again;
    /// a longer one from the next wait. Any thread.
    void set_period_ms(int ms);
    static constexpr int kMinPeriodMs = 50;
    static constexpr int kMaxPeriodMs = 5000;

    /// Whether the game's packets steer the hunt just now -- set with the
    /// wait, at every pass of GoBot's window (GuiApp::detector_pace_tick).
    /// The hunter then takes where the character is from them, not from its
    /// plate, so a plate unfound past the grace is said, not warned of: twice
    /// in the farm of 2026-11-03, the name right both times. Any thread.
    void set_packets_steer(bool on) { packets_steer_.store(on); }

    /// Monsters, nearest first (nearest_first) from your own character: from
    /// its plate in this frame, or, while that is covered for a moment, from
    /// where player() still remembers it. In the order they were found when
    /// there is neither.
    std::vector<Nameplate> mobs() const;

    /// Put `list` nearest first from `px`, `py`, a height counting three times
    /// its distance: something level with you is reachable, something the
    /// same distance away but two platforms up is not. Two the same distance
    /// away keep the order they came in. The order mobs() serves; exposed for
    /// tests, and for whatever else hands the hunter monsters.
    static void nearest_first(std::vector<Nameplate>& list, int px, int py);

    /// Every plate found, players included. For the tuning view.
    std::vector<Nameplate> plates() const;

    /// Your own character, found by its name (set_character_name) -- never
    /// another player, whatever bars they wear. With no name there is none.
    ///
    /// Remembered briefly when it cannot be seen. The character's own name
    /// plate is routinely covered for a frame or two by its pet, its own
    /// casting effects, or the grass it is standing in, and a bot that stops
    /// dead every time that happens spends most of a fight standing still. A
    /// character does not teleport, so the last place it was is a better
    /// answer than no answer for a moment.
    ///
    /// `fresh` says whether this was actually seen just now.
    bool player(Nameplate& out, bool* fresh = nullptr) const;

    /// How long a remembered position stays usable.
    static constexpr std::uint64_t kPlayerMemoryMs = 1500;

    /// How long your plate may go unfound before "Cannot find" is said: as
    /// many looks as kPlayerMemoryMs makes at kLookGapMs, at the wait in use
    /// -- 1500 at 140 ms, 6000 at the resting 600 ms; never under
    /// kPlayerMemoryMs.
    static std::uint64_t sightings_grace_ms(int period_ms);

    /// What the log says of the searches for your character: "found" once it
    /// is found, and "cannot find" only once it has gone unfound for longer
    /// than `grace_ms` -- not at every look the plate drops out for, which it
    /// does several times a minute: 213 lines each way in five minutes of
    /// the hunt of 2026-11-01. One search's answer at `now` goes in; +1 is a
    /// "found" to say, -1 a "cannot find", 0 nothing.
    struct PlayerSightings {
        bool          said_found = false;
        std::uint64_t missing_since_ms = 0;     ///< 0 while found
        int note(bool got, std::uint64_t now, std::uint64_t grace_ms);
    };

    bool latest_frame(Frame& out) const;

    struct Stats {
        std::uint64_t frames = 0;
        std::uint64_t failures = 0;
        std::size_t   last_mobs = 0;
        bool          capture_from_screen = false;
        bool          player_seen = false;
    };
    Stats stats() const;

    /// Read the names, instead of guessing whether there is one.
    ///
    /// Without this the detector decides a health bar has a name over it from
    /// pixel statistics -- is the patch above it bright in places and dark in
    /// others? -- and that guess is behind most of what goes wrong: plates
    /// missed against bright grass, a temple roof against the sky read as nine
    /// monsters, the character's own plate lost under its casting glow. The
    /// game draws its text with a bitmap font, so with the font in hand the
    /// question has an exact answer, and the answer comes with the name
    /// attached.
    ///
    /// Must outlive the detector. Optional: everything still works without
    /// one, the old way.
    void set_font(const GlyphFont* f) { font_.store(f); }

    /// Your own character's name, so the detector can go and look for it.
    ///
    /// Everything else on screen is found by its health bar, and a plate with
    /// bars may be yours or another player's: the bars say a player stands
    /// there, not which one. Give the detector the name and it reads the
    /// screen for it. Needs a glyph set to read with.
    void set_character_name(const std::string& name);
    std::string character_name() const;

    /// Where the time went, for tuning. Reading is exact but not free, and the
    /// whole art of using it is reading few enough places.
    struct Cost {
        int bars = 0;           ///< runs of red that looked like a health bar
        int looked = 0;         ///< of those, patches that looked like writing
        int read = 0;           ///< of those, patches actually read
    };

    /// Run the detector over one frame. Exposed for tests and for the GUI,
    /// which re-runs it as the user drags the thresholds around.
    static std::vector<Nameplate> detect(const Frame& f, const DetectorTuning& t,
                                         const GlyphFont* font = nullptr,
                                         Cost* cost = nullptr);

    /// Your own character among the plates: the one found by its name
    /// (is_self), the nearest the middle should there ever be two. A plate
    /// found by its bars is never taken for you, however near the middle: it
    /// may be yours or another player's, and taking the nearest player for
    /// you had the bot walk and hit relative to another player whenever one
    /// stood nearer the middle. Exposed for tests; the instance methods use
    /// it internally.
    static bool pick_player(const std::vector<Nameplate>& plates, int frame_width,
                            Nameplate& out);

    /// Your own character on one frame, by its name as the game writes it
    /// under its feet, looked for inside the tuning's box: the plate the
    /// running detector makes on every frame, and what tools that look at
    /// one frame use.
    ///
    /// Only the whole name on its plate counts (GlyphFont::find_whole): a
    /// player whose name contains yours, or is a letter away from it, is not
    /// you. Should two places say it -- your plate and a chat bubble saying
    /// your name, say -- the one nearest `last`, where you were last seen,
    /// is you, and that neighbourhood is searched first. Without `last`, the
    /// best read. `sure` is how well the name was read, 0..1.
    static bool find_self(const Frame& f, const DetectorTuning& t, const GlyphFont& font,
                          const std::string& name, Nameplate& out, float* sure = nullptr,
                          const ScreenRect* last = nullptr);

private:
    void loop();

    void find_character(const Frame& f, const DetectorTuning& t, const GlyphFont& font,
                        const std::string& want, std::vector<Nameplate>& out);

    std::string     character_name_;
    PlayerSightings sightings_;         ///< what the log has said of the searches

    std::atomic<void*>            hwnd_{nullptr};
    std::atomic<const GlyphFont*> font_{nullptr};
    std::atomic<bool>             running_{false};
    std::thread                   thread_;
    /// The wait between looks (set_period_ms), and what cuts it short: stop(),
    /// and a shorter period asked for (wake_now_, under wake_mutex_).
    std::atomic<int>              period_ms_{kLookGapMs};
    std::atomic<bool>             packets_steer_{false};    ///< see set_packets_steer
    std::mutex                    wake_mutex_;
    std::condition_variable       wake_;
    bool                          wake_now_ = false;
    std::atomic<MinimapTracker*> minimap_{nullptr};
    std::atomic<CameraTracker*>  camera_{nullptr};
    std::function<bool(void*, Frame&)> grab_;   ///< see set_grab; empty for the real window
    /// Counted up by forget(). A look begun under an older count was of a
    /// window no longer followed, and is not published.
    std::atomic<std::uint64_t>   generation_{0};

    mutable std::mutex     mutex_;
    Nameplate              last_player_;
    std::uint64_t          last_player_ms_ = 0;
    DetectorTuning         tuning_;
    std::vector<Nameplate> plates_;
    Frame                  frame_;

    mutable std::mutex stats_mutex_;
    Stats              stats_;
};

} // namespace gobot

