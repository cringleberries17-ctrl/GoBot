#pragma once

#include "gobot/config.hpp"

#include <string>
#include <vector>

namespace gobot {

/// The party settings, configs/party.ini, and how a name read off the screen
/// is matched against their lists.
///
/// The settings are the commercial client's, key for key. Its own party
/// configuration file lists exactly these fifteen, which is as good a
/// definition of "what a party feature needs" as either of them wrote down.
class Party {
public:
    /// How the party divides what it earns, with the other helper's own
    /// names: experience is shared equally or weighted, loot is free for all
    /// or by contribution.
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

        /// Whether a name may be invited, by the rules above: the blacklist
        /// matched loosely, the invite and priority lists by allowed_name.
        bool may_invite(const std::string& who) const;

        /// Whether an invitation from this name may be taken. Not the same
        /// question: inviting goes by the invite list, accepting by
        /// `accept_only`, and the blacklist wins over both. `accept_only` is
        /// matched by allowed_name, the blacklist loosely.
        bool may_accept(const std::string& who) const;
    };

    /// Whether `read`, a name read off the screen, is `listed`, a name on one
    /// of the lists that let somebody in: accept_only, the invite list and
    /// the priority list. Both are folded (PartyInvite::fold: case, accents
    /// and spaces off) and kept to letters and digits, and then:
    ///  - the same, always;
    ///  - a listed name of kAllowExactUpTo letters or fewer, only that:
    ///    "Anh" is not "Anh1" or "xAnh";
    ///  - a longer one also with one letter off -- wrong, missing or extra,
    ///    never two: the reader gets a letter wrong now and then ("ExampleWin"
    ///    came back as "Exampley in").
    /// Not the blacklist's 70%: a false hit there turns somebody away, here it
    /// lets a stranger in. The price: a name one letter off a listed one of
    /// five letters or more -- Friend2 for Friend -- is let in too; the
    /// reader's one wrong letter looks just the same.
    static bool allowed_name(const std::string& listed, const std::string& read);
    static constexpr std::size_t kAllowExactUpTo = 4;
};

} // namespace gobot
