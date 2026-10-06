#pragma once

#include "gobot/common.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace gobot {

/// Finds which bytes of which packet carry something we already know.
///
/// The framing of this protocol is solved — frames tile the stream with no
/// resyncs — but nothing is known about what is inside them. The usual way to
/// find out is to stare at hexdumps and guess, which is how the first attempt
/// at this went and it did not work.
///
/// This asks the question the other way round. The screen already says where
/// the character is, how much health it has and how much mana, measured
/// continuously by machinery that is already running. So every byte offset in
/// every packet is tried as a number, and the one whose *changes* track the
/// changes in something we measured is the field we were looking for.
/// Correlating movement rather than absolute values is what lets this work
/// without knowing the game's coordinate origin or its units.
///
/// The hard part is that one opcode carries every creature in the level, so
/// two packets in a row describe different things and the difference between
/// them is not movement. That is what defeated the first version of this.
/// The fix is to find the identity field first — the byte run that repeats a
/// handful of distinct values across the capture, one per creature — group the
/// packets by it, and only then look at how each group changes. The character
/// is whichever group moves the way the screen said it did.
class FieldFinder {
public:
    enum class Quantity { X, Y, Hp, Mp };
    static const char* to_string(Quantity q);

    /// How a run of bytes is being read.
    struct Reading {
        std::size_t offset = 0;
        int         width = 2;          ///< 2 or 4 bytes
        bool        is_signed = false;
        bool        big_endian = false;

        /// Read the four bytes as a float rather than an integer.
        ///
        /// Worth its own flag rather than being left out. This game writes
        /// positions as float32 -- a character at 2648.1 rather than 2648 --
        /// and a search that only tries integers reads that as 1,159,632,794
        /// and finds nothing, which is exactly what happened for a long time.
        /// A float's bytes do not vary smoothly with its value, so no integer
        /// reading of them correlates with anything.
        bool        is_float = false;

        std::string describe() const;
        double read(const std::uint8_t* p) const;
    };

    struct Candidate {
        std::uint16_t opcode = 0;
        Direction     direction = Direction::ToClient;
        Reading       reading;
        Quantity      quantity = Quantity::X;

        /// The field the packets were grouped by, and which group this was.
        /// Absent for a stream that needed no grouping.
        bool          grouped = false;
        Reading       id_reading;
        std::uint32_t id_value = 0;

        /// Pearson correlation between this field's changes and the measured
        /// quantity's changes. 1 is perfect, -1 is perfect and inverted.
        double correlation = 0.0;

        /// Game units per measured unit, from the slope of the fit. For a
        /// position this is the ratio between the server's coordinates and
        /// screen pixels.
        double scale = 0.0;

        /// How well the same field tracks the clock instead. A coordinate
        /// follows where you are; a sequence number follows when you asked.
        /// A field that follows the clock more closely than the measurement
        /// is a counter, however good its correlation looks -- and a run that
        /// walks out and comes back puts the first half of the time on one
        /// side and the second half on the other, so a counter correlates
        /// with position nearly as well as position does.
        double clock_correlation = 0.0;

        std::size_t samples = 0;
    };

    /// What we independently know, right now. Call often; packets are paired
    /// with the most recent reading.
    void set_truth(double x, double y, float hp_pct, float mp_pct, bool position_valid);

    /// Feed one decoded frame. Stored, not analysed — the analysis needs to
    /// see the whole capture before it can tell entities apart.
    void on_packet(const GamePacket& p);

    /// Work through everything collected. Not cheap; call once at the end.
    std::vector<Candidate> analyse(std::size_t min_samples = 12,
                                   double min_correlation = 0.6) const;

    /// Candidates that come in pairs: an x and a y from the same creature in
    /// the same packet, which is what a position actually looks like.
    ///
    /// This is the thing that separates a finding from a coincidence. Testing
    /// every byte window against every identity field means hundreds of
    /// thousands of hypotheses, and at those odds a correlation of 0.97 over a
    /// dozen samples turns up by chance repeatedly. Two of them agreeing, on
    /// the same creature, does not.
    struct Pair {
        Candidate x, y;
        double    score = 0.0;      ///< the weaker of the two correlations
    };
    std::vector<Pair> position_pairs(std::size_t min_samples = 12,
                                     double min_correlation = 0.8) const;

    /// Keep everything collected, so the analysis can be redone without
    /// capturing again. Walking the character around is the expensive part.
    bool save(const std::string& path) const;
    bool load(const std::string& path);

    /// What the capture contained, for when the answer is "nothing".
    struct Summary {
        std::uint16_t opcode = 0;
        Direction     direction = Direction::ToClient;
        std::size_t   count = 0;
        std::size_t   payload_bytes = 0;
        std::size_t   entities = 0;     ///< distinct values of the best id field
        std::string   id_field;         ///< where that id was found
    };
    std::vector<Summary> summarise() const;

    /// A few payloads of one opcode, as they arrived, with the position that
    /// was measured alongside each. Statistics say which byte; looking at the
    /// bytes says whether the answer is sensible.
    struct Example {
        std::vector<std::uint8_t> payload;
        double x = 0.0, y = 0.0;
        bool   position = false;
    };
    std::vector<Example> examples(std::uint16_t opcode, bool from_server,
                                  std::size_t count) const;

    /// Test one stated layout, instead of searching for any that fits.
    ///
    /// Searching every offset against every reading tests hundreds of
    /// thousands of hypotheses, and at those odds a perfect correlation over
    /// twenty samples turns up by chance several times a run. Naming the
    /// layout first -- this opcode, grouped by the field here, with x there
    /// and y there -- tests one hypothesis, and the number that comes back
    /// means what it says.
    ///
    /// Reports each distinct value of the grouping field separately, because
    /// one of those groups is your character and the rest are not.
    struct Check {
        std::uint32_t id = 0;
        std::size_t   samples = 0;
        double        x_corr = 0.0, x_scale = 0.0, x_min = 0.0, x_max = 0.0;
        double        y_corr = 0.0, y_scale = 0.0, y_min = 0.0, y_max = 0.0;
    };
    std::vector<Check> check_layout(std::uint16_t opcode, bool from_server,
                                    std::size_t id_off, std::size_t x_off,
                                    std::size_t y_off, int width,
                                    bool is_float = false,
                                    bool big_endian = false) const;

    /// Every place in the protocol that looks like a coordinate.
    ///
    /// Correlation needs the thing it is looking for to move with something
    /// you can measure, which rules out every creature that is not you and
    /// every packet that is not about you. But a coordinate is recognisable on
    /// its own: once one position field is known, the shape of the rest is
    /// known too -- the same kind of number, in the same range, in a world of
    /// a particular size. This looks for that shape instead.
    ///
    /// Pairs are reported, because a lone plausible number is weak evidence
    /// and an x with a y beside it is strong.
    struct PositionField {
        std::uint16_t opcode = 0;
        Direction     direction = Direction::ToClient;
        std::size_t   x_offset = 0, y_offset = 0;
        bool          is_float = true;
        int           width = 4;
        std::size_t   samples = 0;
        double        x_min = 0.0, x_max = 0.0;
        double        y_min = 0.0, y_max = 0.0;
        double        in_range_pct = 0.0;
    };

    /// `world_w` and `world_h` bound the map, as learnt from a packet that is
    /// already understood.
    std::vector<PositionField> position_fields(double world_w, double world_h,
                                               std::size_t min_samples = 8) const;

    struct Stats {
        std::uint64_t packets = 0;
        std::uint64_t paired = 0;       ///< packets that had a fresh truth
        std::uint64_t dropped = 0;      ///< over the storage cap
    };
    Stats stats() const;

    /// Longest payload scanned. A position is never a kilobyte into a packet.
    static constexpr std::size_t kMaxScan = 192;

    /// Most packets kept. Ninety seconds of play is a few thousand.
    static constexpr std::size_t kMaxPackets = 40000;

    /// A change bigger than this is not one creature moving.
    static constexpr double kMaxStep = 600.0;

    /// A field that identifies a creature repeats a few distinct values across
    /// the capture: more than one, far fewer than one per packet.
    static constexpr std::size_t kMinEntities = 2;
    static constexpr std::size_t kMaxEntities = 64;

private:
    struct Sample {
        std::uint16_t opcode = 0;
        bool          from_server = true;
        std::vector<std::uint8_t> payload;
        double        truth[4] = { 0, 0, 0, 0 };
        bool          valid[4] = { false, false, false, false };
        std::uint64_t at_ms = 0;    ///< when it arrived, for telling clocks apart
    };

    /// Byte runs that look like they name a creature, best first.
    std::vector<Reading> id_candidates(const std::vector<const Sample*>& group) const;

    mutable std::mutex  mutex_;
    std::vector<Sample> samples_;

    double truth_[4] = { 0, 0, 0, 0 };
    bool   truth_valid_[4] = { false, false, false, false };
    std::uint64_t truth_at_ms_ = 0;

    Stats stats_;
};

} // namespace gobot
