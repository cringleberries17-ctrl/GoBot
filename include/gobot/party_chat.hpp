#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
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
    /// or the whole of a shorter line. A line of ours under three letters and
    /// digits never echoes: alone, "pt" is everybody's -- own_line tells it.
    static bool echoes(const std::string& read, const std::string& said);

    /// Whether a line read off the chat is the player's own: never somebody
    /// asking to be invited, whatever it says, however short. On 2026-10-20 a
    /// Tự chat line that was the keyword alone came back off the chat as the
    /// player asking, and only a hold that happened to be on kept the party
    /// worker from inviting the player's own character. True when:
    ///  a. its speaker is `own_name`, the character's name set on Đánh quái:
    ///     the same letters and digits -- with a leading l, I, L or | taken
    ///     off both, as split_read takes the reader's leftovers of "[Kênh]"
    ///     off the line -- or, for a name of five or more, within one of it in
    ///     length and alike() kOwnNameAlike or more: the reader drops or
    ///     doubles a letter now and then. A name of four or fewer only
    ///     exactly; one that merely holds it ("TuanAnh" for "Anh") is not it.
    ///     The price: with a name of five or more, another character named as
    ///     ours give or take a letter counts as ours too, and is never invited
    ///     by its keyword.
    ///  b. it says nothing but the keyword, and GoBot typed the keyword itself
    ///     in the last kOwnKeywordMs -- whoever the speaker reads as, which
    ///     may be nothing. Somebody else's bare keyword in that minute is
    ///     passed over too; they say it again.
    ///  c. it echoes() a line GoBot typed in the last kOwnLineMs.
    /// `typed` is what GoBot typed, and when, on `now_ms`'s clock.
    /// One line on its own: rule b ends after a minute, while the line can
    /// stay on the chat for minutes. The party worker goes by without_own,
    /// which carries rule b on for as long as the line is there.
    static bool own_line(const std::string& read, const std::string& own_name,
                         const std::vector<std::pair<std::string, std::uint64_t>>& typed,
                         std::uint64_t now_ms, const Settings& s);
    static constexpr double        kOwnNameAlike = 0.8;
    static constexpr std::uint64_t kOwnKeywordMs = 60'000;
    static constexpr std::uint64_t kOwnLineMs = 30 * 60'000;

    /// One look's lines without the player's own, for the votes: those
    /// own_line finds, and the keyword GoBot typed still on the chat after
    /// rule b's minute. On 2026-10-20 the player's Tự chat line that was the
    /// keyword alone was still the top line of the chat more than three
    /// minutes after it was typed, and was taken for the player asking 193 s
    /// after; with no name set on Đánh quái -- as shipped -- or a short one
    /// read a letter wrong, nothing but rule b tells that line, and rule b
    /// lets it go after a minute.
    ///
    /// So the speakers of the lines that are only the keyword in the first
    /// minute after GoBot typed it -- its own line, read as the reader reads
    /// it, letters wrong and all -- are remembered as the player's: any
    /// speaker with no name set, and with one set, a speaker that reads
    /// alike() kLearnAlike or more to it. A line that is only the keyword is
    /// then the player's own too when
    ///  - its speaker is one remembered -- the same letters and digits, or
    ///    for five or more within one of it in length and alike()
    ///    kOwnNameAlike or more, as for the name -- however long ago GoBot
    ///    typed it: a quiet channel keeps a line for half an hour. Each is
    ///    forgotten once kOwnGoneMs of looks that read the chat have gone by
    ///    without it saying the keyword (a look that reads nothing, the chat
    ///    out of sight, counts for nothing);
    ///  - or GoBot typed the keyword in the last kOwnLineMs and nothing was
    ///    remembered in the first minute of the last time it did -- the chat
    ///    not read then, the party worker not running yet: with nothing to
    ///    tell the player's line from another's, another's is passed over,
    ///    the cheaper mistake.
    /// The price: somebody else's bare keyword in that first minute is
    /// remembered too, and passed over while it stays on the chat and
    /// kOwnGoneMs after. The same player saying more than the keyword
    /// ("cho pt") is asking, as ever. `typed` and `now_ms` as for own_line;
    /// the keyword is this one's settings'.
    std::vector<std::string> without_own(const std::vector<std::string>& lines, const std::string& own_name,
                                         const std::vector<std::pair<std::string, std::uint64_t>>& typed,
                                         std::uint64_t now_ms);
    static constexpr double        kLearnAlike = 0.5;
    static constexpr std::uint64_t kOwnGoneMs = 60'000;
    /// Looks further apart than this leave the time between them out of
    /// kOwnGoneMs: the worker was busy, or saw nothing.
    static constexpr std::uint64_t kLookGapMs = 5'000;
    /// The speakers remembered as the player's, for tests and the log.
    std::vector<std::string> own_speakers() const;

    void set_settings(const Settings& s) { settings_ = s; }
    Settings settings() const { return settings_; }

    /// Offer one screenful of chat. Call it once per look.
    ///
    /// Returns the names that have now been seen asking often enough to be
    /// believed, and that are not inside their cooldown. A name is returned
    /// once and then not again until the cooldown has passed.
    std::vector<std::string> saw(const std::vector<std::string>& raw_lines,
                                 std::uint64_t now_ms);

    /// Forget everything: who has been counted, who is on cooldown, and the
    /// speakers without_own remembered.
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

    /// without_own's memory: each speaker taken for the player's, by its
    /// letters and digits, with the reading clock when it was last seen
    /// saying the keyword; the reading clock, which runs only while looks
    /// read the chat; and the keyword GoBot typed whose first minute gave a
    /// speaker, by when it was typed.
    std::map<std::string, std::uint64_t> own_speakers_;
    std::uint64_t reading_ms_ = 0;
    std::uint64_t last_look_ms_ = 0;
    bool          learned_ = false;
    std::uint64_t learned_from_ = 0;
};

} // namespace gobot
