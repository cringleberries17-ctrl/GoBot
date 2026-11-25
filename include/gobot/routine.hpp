#pragma once

#include <string>
#include <vector>

namespace gobot {

class TemplateStore;

/// One step of a routine in configs/routines.ini, as the file writes it. The
/// runner that clicked through these, DialogDriver, is gone (2026.11.16): the
/// steps are read now for what they say -- the reviver's first picture comes
/// from the "revive" routine (auto_chat.cpp) -- and keep the name they had.
struct DialogDriver {
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

        /// false for a step nothing on screen can prove -- one whose
        /// confirmation would be up before the click as well as after, as the
        /// shop's Mua is for the item picked in its list. It names no
        /// `confirm` and no `gone`.
        bool check = true;

        /// How well the pictures have to match, how long to wait for `find`,
        /// and how long to wait for the confirmation.
        float score = 0.80f;
        int   wait_ms = 6000;
        int   confirm_ms = 4000;

        /// How many times to click before giving up. Bounded, always.
        int attempts = 3;

        /// A step that may simply not apply: a popup that is sometimes there,
        /// a confirmation the game only asks for sometimes.
        bool optional = false;
    };
};

/// A named sequence of dialog steps, kept in a config file rather than in code.
///
/// Written as data it is a table somebody can correct in a text editor while
/// the game is open in front of them, which is the only way this gets
/// maintained once the pictures stop matching -- and they will, because the
/// publisher patches the client.
struct Routine {
    std::string name;
    std::vector<DialogDriver::Step> steps;

    bool valid() const { return !name.empty() && !steps.empty(); }

    /// Every routine in a file. Routines live in `[Routine.<name>]` sections,
    /// each step numbered within it:
    ///
    ///     [Routine.buy_fishing_rod]
    ///     1.find    = rod_in_shop
    ///     1.check   = false
    ///     2.find    = ngs_buy_button
    ///     2.confirm = confirm_button
    ///     3.find    = confirm_button
    ///     3.gone    = true
    ///
    /// Step 1 says check = false: the shop's Mua is up whether or not the rod
    /// was picked, so nothing on screen can show that click landed.
    ///
    /// Numbers run from one and stop at the first gap, so commenting a step out
    /// truncates the routine rather than silently shifting the rest up.
    static std::vector<Routine> load(const std::string& path);

    /// Write a set out, so a default file can be produced for editing.
    static bool save(const std::vector<Routine>& routines, const std::string& path,
                     const std::string& header = {});

    /// The one called `name`, or nullptr.
    static const Routine* find(const std::vector<Routine>& routines,
                               const std::string& name);

    /// Every picture any of these routines mentions.
    static std::vector<std::string> pictures_used(const std::vector<Routine>& routines);

    /// Which of `routines` mention a picture the store does not have.
    static std::vector<std::string> missing_pictures(
        const std::vector<Routine>& routines, const TemplateStore& store);
};

} // namespace gobot
