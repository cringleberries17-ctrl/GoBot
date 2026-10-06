#pragma once

#include "gobot/config.hpp"
#include "gobot/dialog_driver.hpp"
#include "gobot/routine.hpp"
#include "gobot/screen_capture.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace gobot {

/// Fishing and mining, which are the same loop with different pictures.
///
/// Both are: hold a tool, press a key, wait, repeat -- and deal with the tool
/// running out. Fishing wears through rods and bait; mining breaks pickaxes.
/// Both periodically hand what they gathered to an NPC. The only real
/// differences are which picture means "the tool is gone" and which routine
/// replaces it, so both are this class with a different configuration rather
/// than two near-identical loops that drift apart.
///
/// What is deliberately separated here is deciding from doing. `decide` takes a
/// frame and returns what ought to happen next, touching nothing; the loop then
/// does it. That split is the only reason any of this can be tested: a bot that
/// decides and acts in one step can only be tested by running it against a
/// game, standing at a fishing spot, for an hour.
class Gatherer {
public:
    /// What the loop should do next.
    enum class Action {
        Nothing,        ///< wait -- vitals too low, or a dialog in the way
        Work,           ///< press the key that casts or mines
        Replace,        ///< the tool is gone or broken: run the buying routine
        HandIn,         ///< enough cycles done: run the exchange routine
        Stop,           ///< finished, or stuck
    };
    static const char* action_name(Action a);

    struct Settings {
        /// What this is called in the config and in the log.
        std::string name;

        /// The tool in the quick slot. When this picture is *absent* the tool
        /// is gone and wants replacing.
        std::string tool_picture;

        /// A picture meaning the tool is broken but still in the slot --
        /// mining's `broken_pickaxe`. Fishing has no equivalent and leaves
        /// this empty.
        std::string broken_picture;

        /// Routines, by the names in configs/routines.ini.
        std::string buy_routine;
        std::string hand_in_routine;

        /// The key that does the work, and how long one go takes.
        std::uint32_t work_key = 0;
        int work_interval_ms = 3000;

        /// Work cycles between trips to the NPC. 0 means never.
        int hand_in_every = 0;

        /// Stop after this many cycles. 0 means keep going.
        int stop_after = 0;

        /// Hold off while health or mana is under this, rather than working
        /// through it. The healing feature does the drinking; this only
        /// declines to start another cycle in the meantime.
        int pause_below_hp_pct = 0;
        int pause_below_mp_pct = 0;

        /// How many failed replacements before giving up. A loop that keeps
        /// trying to buy a rod it cannot afford would otherwise spend the night
        /// walking into a shop.
        int give_up_after = 3;

        void load(const IniFile& ini, const std::string& section);
        void save(IniFile& ini, const std::string& section) const;

        /// What is wrong with it, or empty if nothing is.
        std::string trouble() const;
    };

    /// What the gatherer knows about the moment it is deciding in.
    struct Look {
        bool tool_present = false;
        bool tool_broken = false;
        float hp_pct = -1.0f;       ///< -1 when there is no reading
        float mp_pct = -1.0f;
    };

    /// Read a frame into a Look. Separate from deciding so a test can hand in
    /// a situation directly rather than having to draw it.
    Look see(const Frame& frame, const TemplateStore& store) const;

    /// What to do about it. No side effects beyond counting.
    Action decide(const Look& look) const;

    void set_settings(const Settings& s);
    Settings settings() const;

    /// Tell it a cycle happened, or that a replacement failed. The loop calls
    /// these after acting on what `decide` said.
    void worked();
    void handed_in();
    void replaced(bool ok);
    void reset();

    struct Stats {
        int cycles = 0;             ///< work keys pressed
        int hand_ins = 0;
        int replacements = 0;
        int failed_replacements = 0;
        int since_hand_in = 0;
    };
    Stats stats() const;

    /// Why it stopped, when `decide` returns Stop.
    std::string trouble() const;

private:
    mutable std::mutex mutex_;
    Settings           settings_;
    Stats              stats_;
    mutable std::string trouble_;
};

} // namespace gobot
