#pragma once

#include "gobot/input_manager.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace gobot {

/// Drives the game's dialogs: find a thing, click it, check it worked.
///
/// Every feature still to be built is this shape underneath. Fishing opens a
/// shop and buys a rod. Mining opens an NPC menu and picks an option. Party
/// accepts an invitation and sets two toggles. Crest boxes find an icon in the
/// bag and click it until there are none left. Written out separately each
/// time, that is twenty-odd near-identical retry loops, which is twenty-odd
/// chances to get the retry subtly wrong in a way that only shows up at three
/// in the morning on somebody's overnight run.
///
/// The part worth being careful about is not the clicking. It is knowing
/// whether the click landed. A click that misses leaves the screen exactly as
/// it was, so a driver that clicks and moves on will walk the whole sequence
/// into nothing and report success. Every step here therefore names something
/// that must become true afterwards -- another picture appearing, or this one
/// going away -- and a step that cannot show it worked is a step that failed.
/// A picture that was up already did not become true: a step whose
/// confirmation is on screen before its first click is refused, with nothing
/// clicked. The one way round is a step marked `check = false` in
/// routines.ini -- picking an item in a shop's list, where Mua is up either
/// way -- which is clicked once and reported as not checked, never as
/// confirmed.
class DialogDriver {
public:
    /// Where a step's frames come from. Supplied rather than captured directly
    /// so a test can script a sequence of screens and watch what the driver
    /// does with them, which is the only way to test the retry logic without a
    /// game running.
    using Grab = std::function<bool(Frame&)>;

    DialogDriver(InputManager& input, const TemplateStore& store, Grab grab);

    /// One thing to find and click.
    struct Step {
        /// What marks the page or the button. A name in the template store.
        std::string find;

        /// Where to click, from the picture's top-left. -1 for both means its
        /// middle, which is right for a button and wrong for a page marker --
        /// hence the offsets. Kept inside the picture: an offset past its
        /// right or bottom edge clicks at that edge (click_point), so a typo
        /// in routines.ini cannot send a click across the screen.
        int click_dx = -1;
        int click_dy = -1;

        /// What proves the click landed. Either something that must appear, or
        /// (when `gone` is set) `find` itself having to disappear. Something
        /// that must appear means something the click brings up: it is looked
        /// for before the click as well, and one already up before the first
        /// click gets the step refused (up before a retry, it is the earlier
        /// click having landed late).
        std::string confirm;
        bool gone = false;

        /// false for a step nothing on screen can prove -- one whose
        /// confirmation would be up before the click as well as after, as the
        /// shop's Mua is for the item picked in its list. It names no
        /// `confirm` and no `gone`, is clicked once, and is said to be not
        /// checked rather than reported as confirmed.
        bool check = true;

        /// How well the pictures have to match, how long to wait for `find`,
        /// and how long to wait for the confirmation.
        float score = 0.80f;
        int   wait_ms = 6000;
        int   confirm_ms = 4000;

        /// How many times to click before giving up. Bounded, always: a step
        /// that retries forever is a bot that spends the night clicking at a
        /// server that has stopped answering.
        int attempts = 3;

        /// A step that may simply not apply. A popup that is sometimes there,
        /// a confirmation the game only asks for sometimes. Not finding `find`
        /// is then success rather than failure.
        bool optional = false;
    };

    struct Result {
        bool        ok = false;
        ScreenRect  at;             ///< where `find` was, when it was found
        int         attempts_used = 0;
        std::string trouble;        ///< why not, in words worth reading
    };

    /// Wait for a picture to appear. Does not click.
    bool wait_for(const std::string& name, float score, int wait_ms, ScreenRect& at);

    /// Wait for a picture to go away.
    bool wait_gone(const std::string& name, float score, int wait_ms);

    Result run(const Step& step);

    /// Run a sequence, stopping at the first failure. Returns how many steps
    /// completed, so the caller can say where it got to rather than just that
    /// it did not finish.
    std::size_t run_all(const std::vector<Step>& steps, std::string* trouble = nullptr);

    /// Where a step clicks on a picture found at `at`: the step's offsets from
    /// its top-left, kept inside the picture, or its middle where they are
    /// negative.
    static void click_point(const ScreenRect& at, const Step& step, int& x, int& y);

    /// Ask the driver to stop at the next opportunity. From any thread: the
    /// command line's Ctrl+C watcher calls it while a step waits.
    void cancel() { cancelled_.store(true); }
    void resume() { cancelled_.store(false); }

    /// Called as each step is attempted, for a running commentary.
    void on_step_report(std::function<void(const std::string&)> fn) {
        report_ = std::move(fn);
    }

    /// How long to leave between looks while waiting. Short enough to feel
    /// prompt, long enough that waiting six seconds is not sixty captures.
    static constexpr int kLookEveryMs = 250;

private:
    bool look(Frame& frame);
    bool find_now(const Frame& frame, const std::string& name, float score,
                  ScreenRect& at) const;

    void say(const std::string& line) const { if (report_) report_(line); }

    InputManager&        input_;
    const TemplateStore& store_;
    Grab                 grab_;
    std::atomic<bool>    cancelled_{false};
    std::function<void(const std::string&)> report_;
};

} // namespace gobot
