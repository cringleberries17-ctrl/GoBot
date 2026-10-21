#include "gobot/party_chat.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <vector>

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

/// Letters and digits only, lowercased.
///
/// Names come off a screen, and what separates one reading of a name from
/// another is punctuation and spacing far more often than letters: a guild tag
/// drawn as [Tag] or (Tag), a stray dot where a glyph was clipped. Comparing
/// what is left after those are dropped compares the part that was actually
/// read.
std::string canonical(const std::string& s) {
    std::string out;
    for (const unsigned char c : s)
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

bool word_at(const std::string& haystack, const std::string& needle, std::size_t at) {
    const bool start_ok = at == 0 ||
                          !std::isalnum(static_cast<unsigned char>(haystack[at - 1]));
    const std::size_t end = at + needle.size();
    const bool end_ok = end >= haystack.size() ||
                        !std::isalnum(static_cast<unsigned char>(haystack[end]));
    return start_ok && end_ok;
}

} // namespace

bool PartyChat::split(const std::string& raw, Line& out) {
    const std::string line = trim(raw);
    if (line.empty()) return false;

    const auto colon = line.find(':');
    if (colon == std::string::npos) return false;

    out.who  = trim(line.substr(0, colon));
    out.said = trim(line.substr(colon + 1));
    // A colon with nothing before it is not somebody speaking, and a name with
    // nothing after it has asked for nothing.
    return !out.who.empty() && !out.said.empty();
}

bool PartyChat::split_read(const std::string& raw, Line& out) {
    std::string line = trim(raw);
    if (line.empty()) return false;

    // Drop a leading channel tag.
    //
    // The client writes "[Kênh] Name: message" and the reader loses the
    // brackets, leaving stray letters in front of the name. Everything up to
    // and including the channel word goes, along with whatever the opening
    // bracket turned into.
    static const char* kTags[] = { "K\xC3\xAAnh", "Kenh", "Khu",
                                   "Th\xC6\xB0\xE1\xBB\x9Dng" };
    for (const char* tag : kTags) {
        const std::size_t at = line.find(tag);
        if (at != std::string::npos && at < 12) {
            line = trim(line.substr(at + std::strlen(tag)));
            break;
        }
    }
    while (!line.empty() &&
           (line.front() == '|' || line.front() == ']' || line.front() == '[' ||
            line.front() == 'l' || line.front() == 'I' || line.front() == ' '))
        line.erase(line.begin());
    line = trim(line);
    if (line.empty()) return false;

    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        out.who  = trim(line.substr(0, colon));
        out.said = trim(line.substr(colon + 1));
        return !out.who.empty() && !out.said.empty();
    }

    // No colon came back at all. Take the first word as the name.
    const auto space = line.find_first_of(" \t");
    if (space == std::string::npos) return false;
    out.who  = trim(line.substr(0, space));
    out.said = trim(line.substr(space + 1));
    return !out.who.empty() && !out.said.empty();
}

bool PartyChat::asks_to_join(const std::string& said, const Settings& s) {
    if (s.keyword.empty() || said.empty()) return false;

    const std::string haystack = lowered(said);
    const std::string needle = lowered(s.keyword);

    for (std::size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + 1)) {
        if (!s.whole_word_only || word_at(haystack, needle, at)) return true;
    }
    return false;
}

namespace {

/// The longest common subsequence of two strings, over two rows rather than a
/// full table: names are short, but this runs on every line of every look.
std::size_t common_length(const std::string& x, const std::string& y) {
    std::vector<std::size_t> previous(y.size() + 1, 0), current(y.size() + 1, 0);
    for (std::size_t i = 1; i <= x.size(); ++i) {
        for (std::size_t j = 1; j <= y.size(); ++j) {
            current[j] = x[i - 1] == y[j - 1]
                       ? previous[j - 1] + 1
                       : std::max(previous[j], current[j - 1]);
        }
        previous.swap(current);
    }
    return previous[y.size()];
}

} // namespace

double PartyChat::alike(const std::string& a, const std::string& b) {
    const std::string x = canonical(a), y = canonical(b);
    if (x.empty() || y.empty()) return x == y ? 1.0 : 0.0;
    if (x == y) return 1.0;
    const double common = static_cast<double>(common_length(x, y));
    return common / static_cast<double>(std::max(x.size(), y.size()));
}

bool PartyChat::echoes(const std::string& read, const std::string& said) {
    const std::string ours = canonical(said);
    if (ours.size() < 3) return false;      // "pt" is everybody's
    const std::string text = canonical(read);

    // The longest run of letters and digits the two have in common, in a row.
    // Our own line read back has the whole of it -- after the channel and the
    // name, or in pieces where the box wrapped it or the reader dropped a
    // letter -- and somebody's "pt" has two letters of it at most. A subsequence
    // would not do: any long line holds the letters of a short one in order.
    std::size_t run = 0;
    std::vector<std::size_t> previous(ours.size() + 1, 0), current(ours.size() + 1, 0);
    for (std::size_t i = 1; i <= text.size(); ++i) {
        for (std::size_t j = 1; j <= ours.size(); ++j) {
            current[j] = text[i - 1] == ours[j - 1] ? previous[j - 1] + 1 : 0;
            run = std::max(run, current[j]);
        }
        previous.swap(current);
    }
    // Eight in a row, or the whole of our line, or the whole of a line read
    // that is the end of ours wrapped onto a line of its own -- four at least.
    const std::size_t need =
        std::min({ std::size_t{ 8 }, ours.size(), std::max<std::size_t>(text.size(), 4) });
    return run >= need;
}

namespace {

/// A name with what split_read takes off the front of a line read taken off
/// it too -- the reader's leftovers of "[Kênh]" are l's, I's and bars, and so
/// is the first letter of a name that starts with one -- as letters and
/// digits, lowercased.
std::string speaker_key(std::string s) {
    while (!s.empty() && (s.front() == 'l' || s.front() == 'I' || s.front() == 'L' || s.front() == '|' ||
                          s.front() == ' ' || s.front() == '[' || s.front() == ']'))
        s.erase(s.begin());
    return canonical(s);
}

/// Whether speaker `who` is `mine`, both as speaker_key makes them: the same,
/// or for a name of five or more, within one of it in length and alike
/// kOwnNameAlike or more -- the reader drops or doubles a letter now and then.
/// A name of four or fewer only exactly: "TuanAnh" holds "Anh" and is not it.
bool same_speaker(const std::string& who, const std::string& mine) {
    if (mine.size() < 2) return false;
    if (who == mine) return true;
    const std::size_t longer = std::max(who.size(), mine.size()), shorter = std::min(who.size(), mine.size());
    return mine.size() >= 5 && longer - shorter <= 1 && PartyChat::alike(who, mine) >= PartyChat::kOwnNameAlike;
}

} // namespace

bool PartyChat::own_line(const std::string& read, const std::string& own_name,
                         const std::vector<std::pair<std::string, std::uint64_t>>& typed, std::uint64_t now_ms,
                         const Settings& s) {
    Line line;
    const bool split = split_read(read, line);

    // a. Said by the player's own character.
    if (split && same_speaker(speaker_key(line.who), speaker_key(own_name))) return true;

    // b. Nothing but the keyword, which GoBot typed itself a moment ago.
    const std::string keyword = canonical(s.keyword);
    if (split && !keyword.empty() && canonical(line.said) == keyword)
        for (const auto& [text, when] : typed)
            if (now_ms >= when && now_ms - when <= kOwnKeywordMs && canonical(text) == keyword) return true;

    // c. A line of ours read back, as before: an advert, its wrapped rows.
    for (const auto& [text, when] : typed)
        if (now_ms >= when && now_ms - when <= kOwnLineMs && echoes(read, text)) return true;
    return false;
}

std::vector<std::string> PartyChat::without_own(const std::vector<std::string>& lines, const std::string& own_name,
                                                const std::vector<std::pair<std::string, std::uint64_t>>& typed,
                                                std::uint64_t now_ms) {
    std::vector<std::string> out;
    // Nothing read -- the chat out of sight, or empty: nothing is learned or
    // forgotten, and the time does not count towards forgetting.
    if (lines.empty()) return out;
    if (last_look_ms_ != 0 && now_ms > last_look_ms_ && now_ms - last_look_ms_ <= kLookGapMs)
        reading_ms_ += now_ms - last_look_ms_;
    last_look_ms_ = now_ms;
    for (auto it = own_speakers_.begin(); it != own_speakers_.end();)
        it = reading_ms_ - it->second > kOwnGoneMs ? own_speakers_.erase(it) : std::next(it);

    // The keyword as GoBot typed it last, in the last kOwnLineMs.
    const std::string keyword = canonical(settings_.keyword);
    bool have_typed = false;
    std::uint64_t last_typed = 0;
    if (!keyword.empty())
        for (const auto& [text, when] : typed)
            if (now_ms >= when && now_ms - when <= kOwnLineMs && canonical(text) == keyword &&
                (!have_typed || when > last_typed)) {
                have_typed = true;
                last_typed = when;
            }
    const bool first_minute = have_typed && now_ms - last_typed <= kOwnKeywordMs;
    const std::string mine = speaker_key(own_name);

    for (const std::string& raw : lines) {
        Line line;
        const bool bare = !keyword.empty() && split_read(raw, line) && canonical(line.said) == keyword;
        const std::string who = bare ? speaker_key(line.who) : std::string();
        // Its first minute: the line of the keyword GoBot typed is on the
        // chat, read as the reader reads it. With a name set, a speaker not at
        // all like it is somebody else's.
        if (bare && first_minute && who.size() >= 2 && (mine.size() < 2 || alike(who, mine) >= kLearnAlike)) {
            own_speakers_[who] = reading_ms_;
            learned_ = true;
            learned_from_ = last_typed;
        }
        if (own_line(raw, own_name, typed, now_ms, settings_)) continue;
        if (bare) {
            // Said by a speaker taken for the player's: for as long as such a
            // line is on the chat, however long ago GoBot typed it -- a quiet
            // channel keeps a line for half an hour.
            const auto known = std::find_if(own_speakers_.begin(), own_speakers_.end(),
                                            [&](const auto& s) { return same_speaker(who, s.first); });
            if (known != own_speakers_.end()) {
                known->second = reading_ms_;
                continue;
            }
            // Nothing learned from the keyword GoBot typed last: nothing to
            // tell the player's line from another's by.
            if (have_typed && !(learned_ && learned_from_ == last_typed)) continue;
        }
        out.push_back(raw);
    }
    return out;
}

std::vector<std::string> PartyChat::own_speakers() const {
    std::vector<std::string> out;
    for (const auto& [who, seen] : own_speakers_) out.push_back(who);
    return out;
}

std::vector<std::string> PartyChat::saw(const std::vector<std::string>& raw_lines,
                                        std::uint64_t now_ms) {
    std::vector<std::string> ready;

    const int votes_wanted = std::max(1, settings_.votes);
    // A vote only counts towards one that is recent. Without this, a name seen
    // once an hour ago and once now would be two votes out of three, and a
    // single stray misread would eventually invite somebody who never asked.
    const std::uint64_t vote_window =
        static_cast<std::uint64_t>(std::max(1, settings_.vote_gap_ms)) *
        static_cast<std::uint64_t>(votes_wanted) * 8;

    for (const std::string& raw : raw_lines) {
        Line line;
        // `split_read`, not `split`: these lines have been through the screen
        // reader, which loses the colon after the name as often as not. The
        // strict version discarded every line of a live run -- the chat said
        // "Exampley in pt", the keyword was read correctly, and nothing was
        // ever counted because there was no colon in it.
        if (!split_read(raw, line)) continue;
        if (!asks_to_join(line.said, settings_)) continue;

        // Count it against a name already being counted when the two are alike
        // enough, so a letter dropped on one read does not start a second
        // tally that never reaches three.
        std::string key = line.who;
        for (const auto& [known, tally] : counting_) {
            if (now_ms - tally.last_ms > vote_window) continue;
            if (alike(known, line.who) >= settings_.min_name_match) {
                key = known;
                break;
            }
        }

        Tally& tally = counting_[key];
        if (now_ms - tally.last_ms > vote_window) tally.votes = 0;
        ++tally.votes;
        tally.last_ms = now_ms;

        if (tally.votes < votes_wanted) continue;

        // Believed. Now: has this player been invited recently?
        const auto cooldown = static_cast<std::uint64_t>(
            std::max(0, settings_.cooldown_s)) * 1000;
        bool cooling_down = false;
        for (const auto& [who, when] : invited_at_)
            if (alike(who, key) >= settings_.min_name_match &&
                now_ms - when < cooldown) {
                cooling_down = true;
                break;
            }

        if (cooling_down) continue;

        invited_at_[key] = now_ms;
        tally.votes = 0;                // start again for the next request
        ready.push_back(key);
    }

    return ready;
}

void PartyChat::reset() {
    counting_.clear();
    invited_at_.clear();
    own_speakers_.clear();
    reading_ms_ = 0;
    last_look_ms_ = 0;
    learned_ = false;
    learned_from_ = 0;
}

std::map<std::string, int> PartyChat::cooling(std::uint64_t now_ms) const {
    std::map<std::string, int> out;
    const auto cooldown = static_cast<std::uint64_t>(
        std::max(0, settings_.cooldown_s)) * 1000;
    for (const auto& [who, when] : invited_at_) {
        if (now_ms - when >= cooldown) continue;
        out[who] = static_cast<int>((cooldown - (now_ms - when)) / 1000);
    }
    return out;
}

} // namespace gobot
