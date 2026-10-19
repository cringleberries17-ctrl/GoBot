#include "gobot/monster_list.hpp"
#include "gobot/party_chat.hpp"
#include "gobot/party_invite.hpp"

#include <algorithm>
#include <cctype>

namespace gobot {
namespace {

/// The share of letters two names must have in common to be one name: one
/// letter in five may be wrong or missing. 80% rather than the 70% the party
/// lists use, because a wrong match costs more here -- a wrong "Đánh trước"
/// drags the bot across the map after something else, where a missed one
/// only costs a kill.
constexpr double kMinAlike = 0.80;

/// A listed name this short has to match exactly. One letter in four wrong is
/// already 75%, under the bar, but one letter MORE is 80% -- and "HoLy" on the
/// list would then take in every "HoLyX".
constexpr std::size_t kExactUpTo = 4;

/// A name as it is compared: folded to bare lowercase letters and digits.
/// Accents, đ, spaces and punctuation all go, which is what makes a name typed
/// with its accents the same as one read by a glyph set that has none.
std::string letters(const std::string& name) {
    std::string out;
    for (const char c : PartyInvite::fold(name))
        if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(c);
    return out;
}

bool letters_match(const std::string& read, const std::string& listed) {
    if (read.empty() || listed.empty()) return false;
    if (read == listed) return true;
    if (listed.size() <= kExactUpTo) return false;
    // A hair under the bar, so that four letters in five is not lost to the
    // last bit of a double.
    return PartyChat::alike(read, listed) >= kMinAlike - 1e-9;
}

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

} // namespace

bool monster_name_matches(const std::string& read, const std::string& listed) {
    return letters_match(letters(read), letters(listed));
}

bool monster_on_list(const std::vector<std::string>& list, const std::string& read) {
    const std::string have = letters(read);
    if (have.empty()) return false;
    for (const std::string& n : list)
        if (letters_match(have, letters(n))) return true;
    return false;
}

std::vector<std::string> split_monster_names(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    const auto take = [&] {
        const std::string name = trim(current);
        current.clear();
        if (!name.empty() && out.size() < kMaxMonsterNames) out.push_back(name);
    };
    for (const char c : text) {
        if (c == ',' || c == ';' || c == '\n') take();
        else current.push_back(c);
    }
    take();
    return out;
}

std::string join_monster_names(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& n : names) {
        const std::string name = trim(n);
        if (name.empty()) continue;
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out;
}

// ------------------------------------------------------------ sightings

void MonsterSightings::saw(const std::vector<Nameplate>& plates, std::uint64_t now_ms) {
    for (const Nameplate& n : plates)
        if (!n.is_player && !n.name.empty()) saw(n.name, now_ms);
}

void MonsterSightings::saw(const std::string& name, std::uint64_t now_ms) {
    // Older than the window is never shown again.
    for (Spelling& s : spellings_)
        while (!s.at.empty() && s.at.front() + kWindowMs < now_ms) s.at.pop_front();
    spellings_.erase(std::remove_if(spellings_.begin(), spellings_.end(),
                                    [](const Spelling& s) { return s.at.empty(); }),
                     spellings_.end());

    if (name.empty()) return;
    const std::string key = letters(name);
    if (key.empty()) return;

    auto same = std::find_if(spellings_.begin(), spellings_.end(),
                             [&](const Spelling& s) { return s.name == name; });
    if (same == spellings_.end()) {
        if (spellings_.size() >= kMaxSpellings) {
            // The one read longest ago makes room.
            const auto oldest = std::min_element(
                spellings_.begin(), spellings_.end(),
                [](const Spelling& a, const Spelling& b) { return a.at.back() < b.at.back(); });
            spellings_.erase(oldest);
        }
        spellings_.push_back(Spelling{ name, key, ++order_, {} });
        same = spellings_.end() - 1;
    }
    same->at.push_back(now_ms);
    if (same->at.size() > kMaxReadsEach) same->at.pop_front();
}

std::vector<std::string> MonsterSightings::names(std::uint64_t now_ms) const {
    // How often each spelling was read within the window.
    struct Count {
        const Spelling* s;
        std::size_t     n;
    };
    std::vector<Count> counts;
    for (const Spelling& s : spellings_) {
        const std::size_t n = static_cast<std::size_t>(
            std::count_if(s.at.begin(), s.at.end(),
                          [&](std::uint64_t at) { return at + kWindowMs >= now_ms && at <= now_ms; }));
        if (n > 0) counts.push_back(Count{ &s, n });
    }
    // Most read first; a tie keeps the order they were first read in.
    std::sort(counts.begin(), counts.end(), [](const Count& a, const Count& b) {
        return a.n != b.n ? a.n > b.n : a.s->first < b.s->first;
    });

    // Grouped the way the lists would match them -- either way round, since
    // both are only reads -- each group shown by its most read spelling.
    struct Group {
        std::vector<const Spelling*> members;
        std::size_t                  total = 0;
        std::uint64_t                first = 0;
    };
    std::vector<Group> groups;
    for (const Count& c : counts) {
        Group* home = nullptr;
        for (Group& g : groups) {
            for (const Spelling* m : g.members) {
                if (letters_match(c.s->key, m->key) || letters_match(m->key, c.s->key)) {
                    home = &g;
                    break;
                }
            }
            if (home) break;
        }
        if (!home) {
            groups.push_back(Group{ {}, 0, c.s->first });
            home = &groups.back();
        }
        home->members.push_back(c.s);
        home->total += c.n;
        home->first = std::min(home->first, c.s->first);
    }
    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        return a.total != b.total ? a.total > b.total : a.first < b.first;
    });

    std::vector<std::string> out;
    for (const Group& g : groups) {
        if (out.size() >= kMaxShown) break;
        out.push_back(g.members.front()->name);
    }
    return out;
}

} // namespace gobot
