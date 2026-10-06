#pragma once

#include "gobot/input_manager.hpp"
#include "gobot/screen_capture.hpp"
#include "gobot/template_matcher.hpp"

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
        /// hence the offsets.
        int click_dx = -1;
        int click_dy = -1;

        /// What proves the click landed. Either something that must appear, or
        /// (when `gone` is set) `find` itself having to disappear.
        std::string confirm;
        bool gone = false;

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

    /// Close whatever is in the way.
    ///
    /// The game has four close buttons across its dialog styles, plus an OK and
    /// a confirm. This tries each, repeatedly, until the screen has none of
    /// them left or the budget runs out. Every other feature calls this when it
    /// finds something unexpected on screen, which is why it takes no argument
    /// about what the something is.
    int close_popups(int budget = 4);

    /// Ask the driver to stop at the next opportunity.
    void cancel() { cancelled_ = true; }
    void resume() { cancelled_ = false; }

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
    bool                 cancelled_ = false;
    std::function<void(const std::string&)> report_;
};

} // namespace gobot
