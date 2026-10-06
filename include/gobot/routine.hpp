#pragma once

#include "gobot/dialog_driver.hpp"

#include <string>
#include <vector>

namespace gobot {

/// A named sequence of dialog steps, kept in a config file rather than in code.
///
/// Nearly every feature left to build is a sequence of clicks through the
/// game's own dialogs. Buying a fishing rod is: find the rod in the shop list,
/// click it, click buy, confirm. Mining is the same shape at a different NPC.
/// Opening a crest box, exchanging tokens, setting a party's loot rule --
/// all of them.
///
/// Written as C++ that would be twenty near-identical functions, each with its
/// own slightly different retry loop, and each needing a rebuild when the game
/// moves a button by four pixels. Written as data it is a table somebody can
/// correct in a text editor while the game is open in front of them, which is
/// the only way this gets maintained once the pictures stop matching -- and
/// they will, because the publisher patches the client.
///
/// The auto-login flow already works this way and that is the precedent. What
/// is not data is the part that is genuinely decision-making: knowing a pickaxe
/// has broken, knowing the bag is full, knowing how many fish have been caught.
/// Those live in code and call routines to do the clicking.
struct Routine {
    std::string name;
    std::vector<DialogDriver::Step> steps;

    bool valid() const { return !name.empty() && !steps.empty(); }

    /// Every routine in a file. Routines live in `[Routine.<name>]` sections,
    /// each step numbered within it:
    ///
    ///     [Routine.buy_fishing_rod]
    ///     1.find    = rod_in_shop
    ///     1.confirm = ngs_buy_button
    ///     2.find    = ngs_buy_button
    ///     2.confirm = confirm_button
    ///     3.find    = confirm_button
    ///     3.gone    = true
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

    /// Every picture any of these routines mentions. For checking a set of
    /// routines against a template store before running any of them -- a
    /// routine that refers to a picture nobody has is a routine that fails
    /// halfway, having already clicked three things.
    static std::vector<std::string> pictures_used(const std::vector<Routine>& routines);

    /// Which of `routines` mention a picture the store does not have.
    static std::vector<std::string> missing_pictures(
        const std::vector<Routine>& routines, const TemplateStore& store);
};

} // namespace gobot
