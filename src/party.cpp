#include "gobot/party.hpp"
#include "gobot/logger.hpp"
#include "gobot/party_chat.hpp"
#include "gobot/party_invite.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace gobot {
namespace {

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// A list written as one name per line, or separated by commas. Both because
/// the commercial client writes commas and a person editing by hand writes
/// lines, and refusing either would be a papercut nobody needs.
std::vector<std::string> split_names(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == ',' || c == '\n' || c == ';') {
            const std::string name = trim(current);
            if (!name.empty()) out.push_back(name);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    const std::string name = trim(current);
    if (!name.empty()) out.push_back(name);
    return out;
}

std::string join_names(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& n : names) {
        if (!out.empty()) out += ",";
        out += n;
    }
    return out;
}

/// Whether a name is on a list, allowing for a reader that got a letter wrong.
///
/// An exact comparison is right for a name typed by a person and wrong for one
/// read off a screen: a live invitation from "ExampleWin" came back as
/// "Exampley in", because the W is drawn as two strokes and the reader saw two
/// characters. Nine letters out of ten were right -- plenty to recognise a
/// name by, nothing like enough to match it exactly.
///
/// So both sides are folded to bare lowercase letters, an exact hit wins, and
/// otherwise a longest-common-subsequence ratio above the threshold does. That
/// is the same measure, and the same default, the other helper uses for this
/// exact job.
bool contains(const std::vector<std::string>& names, const std::string& who,
              double min_match = 0.7) {
    const std::string want = PartyInvite::fold(who);
    if (want.empty()) return false;

    for (const std::string& n : names) {
        const std::string have = PartyInvite::fold(n);
        if (have.empty()) continue;
        if (have == want) return true;
        if (min_match > 0.0 && PartyChat::alike(have, want) >= min_match) return true;
    }
    return false;
}

} // namespace

const char* Party::status_name(Status s) {
    switch (s) {
        case Status::Alone:   return "alone";
        case Status::InParty: return "in a party";
        case Status::Leader:  return "leading a party";
        case Status::Unknown: break;
    }
    return "unknown";
}

// ------------------------------------------------------------------ settings

bool Party::Settings::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) return false;

    auto_accept        = ini.get_bool("Party", "party_auto_accept", auto_accept);
    auto_invite        = ini.get_bool("Party", "party_auto_party", auto_invite);
    always_leader      = ini.get_bool("Party", "party_always_leader", always_leader);
    use_blacklist      = ini.get_bool("Party", "use_blacklist", use_blacklist);
    by_magic_word      = ini.get_bool("Party", "party_by_magic_word", by_magic_word);
    magic_word         = ini.get("Party", "party_magic_word", magic_word);
    by_list            = ini.get_bool("Party", "party_by_list", by_list);
    use_priority_list  = ini.get_bool("Party", "party_use_priority_list", use_priority_list);

    whitelist   = split_names(ini.get("Party", "party_whitelist", {}));
    blacklist   = split_names(ini.get("Party", "party_blacklist", {}));
    priority    = split_names(ini.get("Party", "party_priority_list", {}));
    accept_only = split_names(ini.get("Party", "party_accept_only", {}));
    accept_all  = ini.get_bool("Party", "party_accept_all", accept_only.empty());

    // Written as numbers by the commercial client, so read as numbers.
    exp_share  = ini.get_int("Party", "party_exp_share_type", 0) == 1
                   ? ExpShare::More : ExpShare::Equal;
    loot_share = ini.get_int("Party", "party_drop_share_type", 0) == 1
                   ? LootShare::Contribution : LootShare::Free;
    return true;
}

bool Party::Settings::save(const std::string& path) const {
    IniFile ini;
    ini.set_bool("Party", "party_auto_accept", auto_accept);
    ini.set_bool("Party", "party_auto_party", auto_invite);
    ini.set_bool("Party", "party_always_leader", always_leader);
    ini.set_bool("Party", "use_blacklist", use_blacklist);
    ini.set_bool("Party", "party_by_magic_word", by_magic_word);
    ini.set("Party", "party_magic_word", magic_word);
    ini.set_bool("Party", "party_by_list", by_list);
    ini.set_bool("Party", "party_use_priority_list", use_priority_list);
    ini.set("Party", "party_whitelist", join_names(whitelist));
    ini.set("Party", "party_blacklist", join_names(blacklist));
    ini.set("Party", "party_priority_list", join_names(priority));
    ini.set("Party", "party_accept_only", join_names(accept_only));
    ini.set_bool("Party", "party_accept_all", accept_all);
    ini.set_int("Party", "party_exp_share_type", exp_share == ExpShare::More ? 1 : 0);
    ini.set_int("Party", "party_drop_share_type",
                loot_share == LootShare::Contribution ? 1 : 0);

    return ini.save(path,
        "GoBot party settings\n"
        "\n"
        "The same keys the commercial client uses, so a configuration can be\n"
        "moved between them without translation. Lists take either commas or\n"
        "one name per line.\n");
}

bool Party::Settings::may_invite(const std::string& who) const {
    if (who.empty()) return false;

    // The blacklist wins over everything, which is the whole point of having
    // one: a name on it is never invited however else it qualifies.
    if (use_blacklist && contains(blacklist, who)) return false;

    // And when invitations are restricted to a list, not being on it is the
    // end of the matter.
    if (by_list && !contains(whitelist, who) && !contains(priority, who))
        return false;

    return true;
}

bool Party::Settings::may_accept(const std::string& who) const {
    // A name that could not be read passes only when no name would have been
    // checked anyway.
    if (who.empty()) return accept_all && !(use_blacklist && !blacklist.empty());

    if (use_blacklist && contains(blacklist, who)) return false;

    // Accepting used to go through may_invite, which never looks at this
    // list -- so "only accept from these" accepted everybody.
    if (!accept_all) return contains(accept_only, who);

    return true;
}

// -------------------------------------------------------------------- state

Party::State Party::parse(const std::string& reply) {
    State out;
    if (reply.empty()) {
        out.trouble = "the bridge said nothing";
        return out;
    }

    std::map<std::string, std::string> fields;
    std::istringstream lines(reply);
    std::string line;
    while (std::getline(lines, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        fields[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }

    if (const auto it = fields.find("error"); it != fields.end()) {
        out.trouble = it->second;
        return out;
    }
    if (fields.find("status") == fields.end()) {
        out.trouble = "the bridge did not report a status";
        return out;
    }

    const auto flag = [&](const char* key) {
        const auto it = fields.find(key);
        return it != fields.end() && it->second == "1";
    };

    const std::string status = lowered(fields["status"]);
    if      (status == "alone")                       out.status = Status::Alone;
    else if (status.find("leader") != std::string::npos ||
             status.find("key") != std::string::npos)  out.status = Status::Leader;
    else if (!status.empty())                          out.status = Status::InParty;

    out.in_party        = flag("is_player_have_party");
    out.is_leader       = flag("is_player_key_party");
    out.can_accept      = flag("can_auto_accept");
    out.can_invite      = flag("can_auto_invite");
    out.can_transfer_key= flag("can_transfer_party_key");

    // Leading implies being in one, whatever the status word happened to say.
    if (out.is_leader) out.status = Status::Leader;
    else if (out.in_party && out.status == Status::Alone) out.status = Status::InParty;

    if (const auto it = fields.find("client_width"); it != fields.end())
        out.client_width = std::atoi(it->second.c_str());

    // The click points, as zero-sized rectangles at a position.
    for (const auto& [key, value] : fields) {
        const auto comma = value.find(',');
        if (comma == std::string::npos) continue;
        if (key.rfind("exp_", 0) != 0 && key.rfind("loot_", 0) != 0) continue;
        out.points[key] = State::Point{
            std::atoi(value.substr(0, comma).c_str()),
            std::atoi(value.substr(comma + 1).c_str()) };
    }

    out.read = true;
    return out;
}

Party::State Party::look(ClrBridge& bridge, void* hwnd) const {
    State out;
    if (!hwnd) {
        out.trouble = "no game window";
        return out;
    }

    std::string trouble;
    std::string reply;
    const std::string handle =
        std::to_string(reinterpret_cast<std::uintptr_t>(hwnd));

    if (!bridge.call("PartyState", handle, reply, &trouble)) {
        out.trouble = trouble.empty() ? "the bridge could not be called" : trouble;
        return out;
    }
    return parse(reply);
}

} // namespace gobot
