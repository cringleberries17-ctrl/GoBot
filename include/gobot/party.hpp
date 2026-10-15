#pragma once

#include "gobot/clr_bridge.hpp"
#include "gobot/config.hpp"
#include "gobot/screen_capture.hpp"

#include <map>
#include <string>
#include <vector>

namespace gobot {

/// Parties, read through the other helper's own detector.
///
/// This is the one feature of that helper which is wholly reachable from
/// outside. Its fishing and mining are methods on a WPF window and cannot be
/// called at all; its party code is three public static classes --
/// PartyStatusDetector, PartyStatusRules and PartyConfigureDetector -- taking
/// a window handle and plain numbers. So rather than work out from screenshots
/// what a party panel looks like, GoBot asks the code that already knows.
///
/// What that buys is not just the detection. Their configure detector computes
/// where the experience and loot dropdowns are for a given client width, and
/// where each option sits inside them. Those are the numbers this project has
/// otherwise had to measure by hand, get wrong, and find out about by watching
/// a click land on an empty banner.
///
/// The settings are the commercial client's, key for key. Its own party
/// configuration file lists exactly these fifteen, which is as good a
/// definition of "what a party feature needs" as either of them wrote down.
class Party {
public:
    /// What the character is, as the helper's detector sees it.
    enum class Status { Unknown, Alone, InParty, Leader };
    static const char* status_name(Status s);

    /// How the party divides what it earns. The helper's two enums, with its
    /// own names: experience is shared equally or weighted, loot is free for
    /// all or by contribution.
    enum class ExpShare { Equal, More };
    enum class LootShare { Free, Contribution };

    struct Settings {
        /// Both off until a file says otherwise. Each one acts on the game --
        /// an invitation taken, one sent -- and a party.ini that is missing,
        /// or held by another program as it is read, says nothing about what
        /// the player chose: on, they had the tab show both ticked and took
        /// invitations from anyone.
        bool auto_accept = false;       ///< accept invitations
        bool auto_invite = false;       ///< invite others
        bool always_leader = false;     ///< only invite while leading

        bool use_blacklist = false;
        bool by_magic_word = true;
        std::string magic_word = "pt";  ///< chat this to be invited
        bool by_list = false;           ///< only invite names on the list
        bool use_priority_list = false; ///< hold slots for named players

        std::vector<std::string> whitelist;
        std::vector<std::string> blacklist;
        std::vector<std::string> priority;
        std::vector<std::string> accept_only;

        /// Accept an invitation from anyone not on the blacklist. Off, only
        /// from the names in `accept_only`.
        ///
        /// The commercial client's "Chấp nhận tất cả". Its file has no key
        /// for it, so this one is GoBot's own, and a file without it reads as
        /// "everyone" exactly when the list is empty -- which is what that file
        /// meant.
        bool accept_all = true;

        ExpShare  exp_share  = ExpShare::Equal;
        LootShare loot_share = LootShare::Free;

        bool load(const std::string& path);
        bool save(const std::string& path) const;

        /// Whether a name may be invited, by the rules above.
        bool may_invite(const std::string& who) const;

        /// Whether an invitation from this name may be taken. Not the same
        /// question: inviting goes by the invite list, accepting by
        /// `accept_only`, and the blacklist wins over both.
        bool may_accept(const std::string& who) const;
    };

    /// What the helper's detector made of the screen, plus where to click.
    struct State {
        bool   read = false;            ///< false when the bridge could not answer
        Status status = Status::Unknown;
        bool   in_party = false;
        bool   is_leader = false;

        /// What their own rules say is allowed, rather than what we think.
        bool can_accept = false;
        bool can_invite = false;
        bool can_transfer_key = false;

        int client_width = 0;

        /// Click points their configure detector worked out, in client
        /// coordinates. Empty when it could not be asked.
        ///
        /// Keyed by what they are -- "exp_dropdown", "exp_equal",
        /// "loot_contribution" -- because the set depends on which options
        /// their enums happen to carry, and hard-coding four of them here
        /// would silently miss a fifth if they ever add one.
        struct Point { int x = 0, y = 0; };
        std::map<std::string, Point> points;

        /// A point by name, or {0,0} and false.
        bool point(const std::string& name, int& x, int& y) const {
            const auto it = points.find(name);
            if (it == points.end()) return false;
            x = it->second.x;
            y = it->second.y;
            return true;
        }

        std::string trouble;
    };

    void set_settings(const Settings& s) { settings_ = s; }
    Settings settings() const { return settings_; }

    /// Ask the helper what the party looks like right now.
    ///
    /// `hwnd` is the game's window. Needs the bridge, which needs .NET and the
    /// helper installed; without them this comes back with `read == false` and
    /// a reason, and nothing else in GoBot is affected.
    State look(ClrBridge& bridge, void* hwnd) const;

    /// Parse the bridge's reply. Separate from calling it so the parsing can
    /// be tested without .NET, a game, or the helper being installed.
    static State parse(const std::string& reply);

private:
    Settings settings_;
};

} // namespace gobot
