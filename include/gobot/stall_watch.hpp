#pragma once

#include "gobot/auto_login.hpp"
#include "gobot/config.hpp"
#include "gobot/crash_watch.hpp"
#include "gobot/glyph_font.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/reviver.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace gobot {

/// Notices a game that runs and answers but has nobody in the world.
///
/// CrashWatch sees a client that has exited, stopped answering or put up a
/// Windows error box. A disconnect the game draws inside its own window -- or
/// a client left at the channel list or the character select -- is none of
/// those: the process is fine, and the run would sit there all night. GoC has
/// no rule for this either; it knows where its client is from packets. This
/// is the screen's way: the HUD gone for stall_min minutes of looks that can
/// be trusted, and the keeper ends the client and logs back in, as for a
/// crash, counted against max_restarts.
///
/// What it never restarts: a dead character (the reviver or the player's own
/// stop rule deals with that), a screen that cannot be seen (the game behind
/// another window, minimised), the minutes when GoBot itself holds the game
/// (a trash pass, a chat line, a quest round, a crossing), and the first
/// minute after getting into the world. A screen that keeps changing and is
/// not a login page is taken for a map loading, and is given longer.
///
/// No captcha is looked for, waited on or treated specially, by the player's
/// decision.

/// One look at the game, every few seconds.
struct StallLook {
    /// Nothing can be told from it: the window minimised, the grab failed, or
    /// the frame came off the screen while another window was in front of the
    /// game or over it -- whatever covers the game is in it.
    bool blind = false;
    bool hud = false;           ///< the character's HUD is up: in the world
    bool death = false;         ///< the death panel or the revive question is up
    bool login_page = false;    ///< one of the crossing's own pages: Kết Nối, the channels, the characters
    bool changed = false;       ///< the screen moved since the last look (see StallEyes)
    bool held = false;          ///< GoBot holds the game: a hold, a trash pass, a chat line in doubt
    bool crossing = false;      ///< the keeper is logging in
};

/// configs/app.ini [Crash]: stall_enabled, stall_min, stall_loading_max_min.
struct StallSettings {
    bool enabled = false;
    int  stall_min = 5;
    int  loading_max_min = 15;

    static constexpr int kMinMinutes = 1;
    static constexpr int kMaxMinutes = 30;
    static constexpr int kMaxLoadingMinutes = 90;

    /// Missing or out of range is the default, or brought into range. The
    /// loading allowance is never shorter than stall_min.
    void load(const IniFile& ini);
};

/// The rule, on looks and a clock in milliseconds. Pure logic.
class StallWatch {
public:
    /// After the keeper gets in -- or the game window is found again -- the
    /// HUD is not there at once. Nothing counts for this long.
    static constexpr std::uint64_t kGraceMs = 60'000;
    /// How many looks the "keeps changing" rule goes back over, and on how
    /// many of them the screen has to have moved.
    static constexpr int kChangeWindow = 12;
    static constexpr int kChangeAtLeast = 6;
    /// A gap between two looks longer than this -- the keeper busy elsewhere,
    /// the machine asleep -- counts as this much, not as all of it.
    static constexpr std::uint64_t kMaxStepMs = 15'000;

    enum class State {
        Off,            ///< not turned on
        Watching,       ///< in the world, dead, or nothing to count
        Paused,         ///< counting, held where it was: blind, GoBot busy, or just in
        Counting,       ///< no HUD on looks that can be trusted
        Stalled,        ///< long enough: end the client and log back in
    };
    /// What the screen looked like while it counted.
    enum class Screen {
        Unknown,        ///< still, and none of the crossing's pages
        Loading,        ///< kept changing, and none of the crossing's pages
        LoginPage,      ///< one of the crossing's pages
    };
    static const char* state_name(State s);
    /// "page", "loading", "unknown" -- for the log.
    static const char* screen_name(Screen s);

    struct Verdict {
        State         state = State::Off;
        Screen        screen = Screen::Unknown;
        std::uint64_t counted_ms = 0;   ///< no HUD, on looks that count
        std::uint64_t limit_ms = 0;     ///< what it is counting towards
        int minutes() const { return static_cast<int>(counted_ms / 60'000); }
    };

    explicit StallWatch(StallSettings s = {}) : settings_(s) {}
    void set_settings(const StallSettings& s) { settings_ = s; }
    const StallSettings& settings() const { return settings_; }

    /// One look, taken at `now_ms`.
    Verdict look(const StallLook& l, std::uint64_t now_ms);

    /// In the world again, or a fresh start: nothing counted, and nothing
    /// counts for kGraceMs from `now_ms`.
    void restart(std::uint64_t now_ms);

    /// What the last look came to.
    const Verdict& last() const { return last_; }

private:
    StallSettings settings_;
    std::uint64_t counted_ms_ = 0;
    std::uint64_t last_ms_ = 0;          ///< when the last look was taken; 0 for none
    bool          last_counted_ = false; ///< and whether it counted
    std::uint64_t grace_until_ms_ = 0;
    std::deque<bool> changes_;           ///< the screen moved, on the last kChangeWindow looks that counted
    std::deque<bool> pages_;             ///< a login page, on the same looks
    Verdict       last_;
};

/// Whether the screen moved between looks: a 64 x 36 grey thumbnail of each,
/// and the mean difference between them, in grey levels.
class StallEyes {
public:
    static constexpr int kThumbW = 64;
    static constexpr int kThumbH = 36;
    /// More than this, in grey levels on average, is a screen that moved.
    static constexpr double kChangedAbove = 3.0;

    /// The thumbnail of `f`: each cell the mean grey of its part of the frame.
    static std::vector<std::uint8_t> thumbnail(const Frame& f);
    /// The mean absolute difference of two thumbnails, or -1 when either is
    /// missing.
    static double difference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b);

    /// Whether `f` moved since the last frame given here. The first frame,
    /// and any after forget(), has nothing to be compared with: false.
    bool changed(const Frame& f);
    /// Blind looks have no frame worth comparing: the next one starts over.
    void forget() { previous_.clear(); }

private:
    std::vector<std::uint8_t> previous_;
};

/// Whether a frame can be trusted at all: grabbed, and not read off the
/// screen unless the game was `clear` then -- in front, with nothing over it,
/// as the crossing trusts a frame (AutoLogin::uncovered). A frame from the
/// screen holds whatever is over the game; one the game drew itself
/// (PrintWindow) is good wherever the game is.
bool stall_frame_trusted(bool grabbed, bool minimised, bool from_screen, bool clear);

/// One of the crossing's own pages that the game draws -- Kết Nối, the channel
/// list, the character select -- is on `f`, by the pictures `login` names for
/// them, found in `pages`.
bool login_page_up(const Frame& f, const AutoLogin::Settings& login, const TemplateStore& pages);

/// The watchdog's look at the game, for whichever keeper runs it. Its own
/// capture and its own pictures; it presses nothing and holds no hands.
class StallLooker {
public:
    StallLooker();
    ~StallLooker();
    StallLooker(const StallLooker&) = delete;
    StallLooker& operator=(const StallLooker&) = delete;

    /// The HUD's landmark, the death panel's pictures, the in-game Xác Nhận
    /// and the game's glyphs, by their short names in `templates_ini`, from
    /// wherever every other picture comes from -- the staged ones first. A few
    /// small pictures, not the whole set: a keeper runs for hours.
    void load(const std::string& templates_ini);

    /// One capture of the game's `window`, and whether it can be trusted, by
    /// stall_frame_trusted. False when there is no frame at all.
    bool grab(void* window, Frame& f, bool& trusted);
    /// One look at a frame, trusted or not as `trusted` says -- blind when
    /// not. `held` and `crossing` are copied into the look as they are.
    StallLook look_at(const Frame& f, bool trusted, bool held, bool crossing,
                      const AutoLogin::Settings& login, const TemplateStore& pages);

    /// A blind look, or a crossing: the next look has nothing to compare with.
    void forget() { eyes_.forget(); }

    /// The in-game dialog with a Xác Nhận on `f`, for the log only: "none",
    /// or what kind it is and its words as the glyphs read them. Nothing is
    /// clicked: GoC clicks it before ending the client, and the client is
    /// ended here anyway -- nor does this hold anything that could click.
    std::string ok_popup(const Frame& f) const;

private:
    ScreenCapture capture_;
    StallEyes     eyes_;
    Frame         landmark_;
    bool          have_landmark_ = false;
    TemplateStore pictures_;
    GlyphFont     font_;
    std::unique_ptr<InputManager> no_hands_;
    std::unique_ptr<Reviver>      reviver_;
};

/// Keep `f` in `dir` as stall_<when>.png, the newest `keep` of them: what the
/// screen showed when the watchdog ended a client.
bool keep_stall_frame(const Frame& f, const std::string& dir, std::size_t keep = 20);

/// The watchdog as a keeper runs it -- the window's and the command line's
/// alike, so the two cannot drift apart: a look every kLookMs while the
/// client is fine, counted by StallWatch and logged once a minute, and when
/// it has stalled the look the client is ended by, the in-game dialog read
/// and the frame kept on the way.
class StallKeeper {
public:
    /// How often it looks: often enough to count minutes by, rarely enough
    /// that a capture and a few matches cost nothing.
    static constexpr std::uint64_t kLookMs = 5000;

    /// [Crash]'s stall_* keys. The pictures are loaded the first time it is
    /// switched on.
    void set_settings(const StallSettings& s, const std::string& templates_ini);
    const StallSettings& settings() const { return watch_.settings(); }

    /// One call for each of the keeper's looks while the client is fine: a
    /// capture of `window` every kLookMs, judged. True when nobody has been in
    /// the world for too long: `out` is then the look to end the client by --
    /// Trouble::Stalled, its minutes and what was on the screen -- and the
    /// frame is kept in `debug_dir` when one is given.
    bool check(void* window, bool held, const AutoLogin::Settings& login, const TemplateStore& pages,
               std::uint64_t now_ms, CrashWatch::Look& out, const std::string& debug_dir = {});
    /// The judging half of check(), on a frame already taken: for the tests,
    /// which have no game to capture.
    bool judge(const Frame& frame, bool trusted, bool held, const AutoLogin::Settings& login,
               const TemplateStore& pages, std::uint64_t now_ms, CrashWatch::Look& out,
               const std::string& debug_dir = {});

    /// In the world again -- after a crossing, or the game found again: the
    /// count starts afresh, after StallWatch::kGraceMs.
    void restart(std::uint64_t now_ms);

    const StallWatch::Verdict& last() const { return watch_.last(); }

private:
    StallWatch    watch_;
    StallLooker   looker_;
    bool          loaded_ = false;
    std::uint64_t next_ms_ = 0;
    int           logged_minute_ = -1;
};

} // namespace gobot
