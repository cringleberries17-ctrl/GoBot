#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gobot {

/// Who has asked to be invited, by saying the word in chat.
///
/// The mechanic is simple to describe and full of traps: a player types "pt",
/// the bot notices, and invites whoever said it. The traps are that the chat
/// is read off the screen, that reading it is never quite reliable, that the
/// same line stays on screen for a while after it is read, and that inviting
/// somebody twice in ten seconds is rude at best.
///
/// The shape here is the other helper's, taken from the configuration record
/// its own code is constructed with -- six lines of chat, a keyword, three
/// reads a hundred and fifty milliseconds apart, a name match that need only
/// be seven tenths right, and thirty seconds before the same player is invited
/// again. Those numbers are theirs; the two that matter are explained where
/// they are used.
///
/// What is not theirs is the reading. Theirs can be told to use Tesseract or a
/// bitmap font and defaults to the bitmap font; ours matches the game's own
/// shipped glyphs and is exact, so the fuzzy name match below is a safety net
/// rather than the mechanism.
class PartyChat {
public:
    struct Settings {
        /// What a player types to ask for an invitation.
        std::string keyword = "pt";

        /// How many times a line must be seen before it is believed, and how
        /// far apart those looks are.
        ///
        /// Reading text off a screen is not reliable the way reading a file
        /// is: a line can be half-drawn, scrolled mid-capture, or crossed by
        /// something floating over it. Their answer is to look three times and
        /// take what agrees, and it is a good one -- a misread rarely repeats
        /// the same way twice, while a real line reads the same every time.
        int votes = 3;
        int vote_gap_ms = 150;

        /// How alike two names must be to count as the same player, from 0 to
        /// 1. A safety net for a reader that drops a letter.
        double min_name_match = 0.7;

        /// How long before the same player may be invited again. Without this
        /// a player who says "pt" once gets invited on every pass for as long
        /// as their line stays on screen.
        int cooldown_s = 30;

        /// Only whole words count. Without this, a player called "Spot" saying
        /// anything at all would be read as asking to join.
        bool whole_word_only = true;
    };

    /// One line as it was read off the screen.
    struct Line {
        std::string who;        ///< the name before the delimiter
        std::string said;       ///< the rest
    };

    /// Split a raw chat line into a name and what they said.
    ///
    /// The game writes "Name: message", but the colon is the only reliable
    /// marker and names themselves may contain almost anything. So the split
    /// is at the first colon, and a line with none is not a player speaking.
    static bool split(const std::string& raw, Line& out);

    /// The same, for a line that has been through the screen reader.
    ///
    /// `split` wants a colon, which is right for the text the client draws and
    /// wrong for the text that comes back from reading it. A live line read off
    /// this client came out as
    ///
    ///     l l KênhlConCáMâo Tỉm anh trai nuõi Ocen luõn
    ///
    /// where "[Kênh]" lost its brackets, the colon after the name vanished
    /// altogether, and the message picked up its own misreadings. So this
    /// drops a leading channel tag, splits on the colon when there is one, and
    /// otherwise takes the first word as the name.
    ///
    /// The name that falls out is approximate. That is survivable because
    /// nothing is done with it directly: it is matched against the list of
    /// players on the map, loosely, and it is that list which decides who gets
    /// invited.
    static bool split_read(const std::string& raw, Line& out);

    /// Whether a message asks for an invitation.
    ///
    /// Matched as a whole word by default: "pt" must be the word, not part of
    /// one. The commercial client's own help says as much -- "chat 'pt' to be
    /// invited" -- and a bot that invites everyone who mentions "spotted" is
    /// not doing what was asked.
    static bool asks_to_join(const std::string& said, const Settings& s);

    /// How alike two names are, from 0 to 1, by their longest common
    /// subsequence. The helper uses the same measure.
    static double alike(const std::string& a, const std::string& b);

    /// Whether a line read off the chat is `said` coming back: the bot's own
    /// words, said by Tự chat, rather than somebody asking for an invitation.
    /// The line read has the channel and the speaker's name in front, and one
    /// too long for the chat box goes on in a line of its own; so it is
    /// enough that the two have eight letters and digits in a row in common,
    /// or the whole of a shorter line.
    static bool echoes(const std::string& read, const std::string& said);

    void set_settings(const Settings& s) { settings_ = s; }
    Settings settings() const { return settings_; }

    /// Offer one screenful of chat. Call it once per look.
    ///
    /// Returns the names that have now been seen asking often enough to be
    /// believed, and that are not inside their cooldown. A name is returned
    /// once and then not again until the cooldown has passed.
    std::vector<std::string> saw(const std::vector<std::string>& raw_lines,
                                 std::uint64_t now_ms);

    /// Forget everything: who has been counted, and who is on cooldown.
    void reset();

    /// Who is waiting on their cooldown, and for how much longer.
    std::map<std::string, int> cooling(std::uint64_t now_ms) const;

private:
    Settings settings_;

    /// Votes so far for each name, and when it was last seen asking, so that
    /// a name seen once an hour ago does not combine with one seen now.
    struct Tally { int votes = 0; std::uint64_t last_ms = 0; };
    std::map<std::string, Tally>         counting_;
    std::map<std::string, std::uint64_t> invited_at_;
};

} // namespace gobot
