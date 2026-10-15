#include "gobot/field_finder.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <unordered_map>

namespace gobot {
namespace {

/// Milliseconds a measurement stays usable. A packet paired with a position
/// from a second ago would be correlated against movement it did not describe.
constexpr std::uint64_t kTruthStaleMs = 400;

/// How far a measurement has to move before a run is worth correlating.
///
/// Different for the two axes, because the two axes are not alike. A
/// character walks hundreds of pixels sideways and a few dozen upward -- a
/// jump is the whole of its vertical range -- so one threshold cannot serve
/// both without either letting a stationary view vouch for a y field or
/// throwing away every honest one.
constexpr double kMinTravelX = 200.0;
constexpr double kMinTravelY = 60.0;

/// And a bar that sat full all run cannot say which field is health.
constexpr double kMinVitalSwing = 8.0;

/// How much a candidate coordinate has to vary before it counts as one rather
/// than a constant that happens to sit in the right range.
constexpr double kMinSpread = 24.0;

/// Correlation, accumulated one pair at a time.
///
/// Welford's method rather than sums of squares. Packet fields run to hundreds
/// of thousands and positions to tens of thousands, and squaring those across
/// a few hundred samples loses enough precision that the variance comes out
/// slightly negative -- whose square root is a NaN, which then compares false
/// against every threshold and sorts straight to the top of the results. This
/// formulation never subtracts two large nearly-equal numbers.
struct Accum {
    std::size_t n = 0;
    double mean_a = 0, mean_b = 0, m2a = 0, m2b = 0, cab = 0;

    void add(double a, double b) {
        ++n;
        const double da = a - mean_a;
        const double db = b - mean_b;
        mean_a += da / static_cast<double>(n);
        mean_b += db / static_cast<double>(n);
        m2a += da * (a - mean_a);
        m2b += db * (b - mean_b);
        cab += da * (b - mean_b);
    }

    /// Zero when either side never varied. A field that is always the same
    /// number tells you nothing, and neither does a measurement that is -- so
    /// "perfectly correlated with a constant" is not a finding.
    double correlation() const {
        if (n < 4 || m2a <= 1e-9 || m2b <= 1e-9) return 0.0;
        const double c = cab / std::sqrt(m2a * m2b);
        return std::isfinite(c) ? c : 0.0;
    }

    double slope() const {
        if (n < 4 || m2b <= 1e-9) return 0.0;
        const double s = cab / m2b;
        return std::isfinite(s) ? s : 0.0;
    }
};

std::vector<FieldFinder::Reading> all_readings(std::size_t payload) {
    std::vector<FieldFinder::Reading> out;
    const std::size_t len = std::min(payload, FieldFinder::kMaxScan);
    for (int width : { 2, 4 }) {
        for (std::size_t off = 0; off + width <= len; ++off) {
            for (bool is_signed : { false, true }) {
                for (bool big_endian : { false, true }) {
                    FieldFinder::Reading r;
                    r.offset = off;
                    r.width = width;
                    r.is_signed = is_signed;
                    r.big_endian = big_endian;
                    out.push_back(r);
                }
            }
            // And as a float, which is how this game writes positions.
            if (width != 4) continue;
            for (bool big_endian : { false, true }) {
                FieldFinder::Reading r;
                r.offset = off;
                r.width = 4;
                r.is_float = true;
                r.big_endian = big_endian;
                out.push_back(r);
            }
        }
    }
    return out;
}

} // namespace

double FieldFinder::Reading::read(const std::uint8_t* p) const {
    if (width == 2) {
        const std::uint16_t raw = big_endian
            ? static_cast<std::uint16_t>((p[0] << 8) | p[1])
            : static_cast<std::uint16_t>((p[1] << 8) | p[0]);
        return is_signed ? static_cast<double>(static_cast<std::int16_t>(raw))
                         : static_cast<double>(raw);
    }
    const std::uint32_t raw = big_endian
        ? (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
          (static_cast<std::uint32_t>(p[2]) << 8)  |  static_cast<std::uint32_t>(p[3])
        : (static_cast<std::uint32_t>(p[3]) << 24) | (static_cast<std::uint32_t>(p[2]) << 16) |
          (static_cast<std::uint32_t>(p[1]) << 8)  |  static_cast<std::uint32_t>(p[0]);
    if (is_float) {
        float f = 0.0f;
        std::memcpy(&f, &raw, sizeof f);
        // Infinities and NaNs are not measurements of anything; they are what
        // random bytes look like when read as a float, and letting them
        // through poisons every accumulator they touch.
        return std::isfinite(f) ? static_cast<double>(f) : 0.0;
    }
    return is_signed ? static_cast<double>(static_cast<std::int32_t>(raw))
                     : static_cast<double>(raw);
}

const char* FieldFinder::to_string(Quantity q) {
    switch (q) {
        case Quantity::X:  return "x";
        case Quantity::Y:  return "y";
        case Quantity::Hp: return "hp";
        case Quantity::Mp: return "mp";
    }
    return "?";
}

std::string FieldFinder::Reading::describe() const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "+%zu %s%d%s", offset,
                  is_float ? "f" : is_signed ? "i" : "u",
                  width * 8, big_endian ? "be" : "le");
    return buf;
}

void FieldFinder::set_truth(double x, double y, float hp_pct, float mp_pct,
                            bool position_valid) {
    std::lock_guard<std::mutex> lock(mutex_);
    truth_[0] = x;
    truth_[1] = y;
    truth_[2] = hp_pct;
    truth_[3] = mp_pct;
    truth_valid_[0] = truth_valid_[1] = position_valid;
    truth_valid_[2] = hp_pct >= 0.0f;
    truth_valid_[3] = mp_pct >= 0.0f;
    truth_at_ms_ = now_ms();
}

void FieldFinder::on_packet(const GamePacket& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.packets;

    if (truth_at_ms_ == 0 || now_ms() - truth_at_ms_ > kTruthStaleMs) return;
    if (samples_.size() >= kMaxPackets) { ++stats_.dropped; return; }
    ++stats_.paired;

    Sample s;
    s.opcode = p.opcode;
    s.from_server = p.from_server;
    const std::size_t len = std::min(p.payload.size(), kMaxScan);
    s.payload.assign(p.payload.begin(), p.payload.begin() + len);
    for (int q = 0; q < 4; ++q) {
        s.truth[q] = truth_[q];
        s.valid[q] = truth_valid_[q];
    }
    s.at_ms = now_ms();
    samples_.push_back(std::move(s));
}

std::vector<FieldFinder::Reading>
FieldFinder::id_candidates(const std::vector<const Sample*>& group) const {
    if (group.empty()) return {};

    std::size_t shortest = SIZE_MAX;
    for (const Sample* s : group) shortest = std::min(shortest, s->payload.size());
    if (shortest == SIZE_MAX) return {};

    // Every four-byte window that could name a creature, not a shortlist.
    //
    // Guessing which field is the identity was the thing that kept going
    // wrong: a heuristic picked the most-repeated value, which against real
    // traffic was part of a float holding the movement speed. There are only
    // about ninety windows in a packet this size and grouping by one is
    // cheap, so the honest answer is to try them all and let the correlation
    // say which was right.
    std::vector<Reading> out;
    for (std::size_t off = 0; off + 4 <= shortest; ++off) {
        Reading r;
        r.offset = off;
        r.width = 4;

        std::unordered_map<std::uint32_t, std::size_t> counts;
        for (const Sample* s : group) {
            ++counts[static_cast<std::uint32_t>(r.read(s->payload.data() + off))];
            if (counts.size() > kMaxEntities) break;
        }
        // One value is a constant, hundreds is a counter or a coordinate.
        // Somewhere in between is a name.
        if (counts.size() < kMinEntities || counts.size() > kMaxEntities) continue;
        out.push_back(r);
    }
    return out;
}

std::vector<FieldFinder::Candidate> FieldFinder::analyse(std::size_t min_samples,
                                                         double min_correlation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Candidate> out;

    // Split by opcode and direction: each is its own message with its own
    // layout, and mixing them is meaningless.
    std::map<std::pair<std::uint16_t, bool>, std::vector<const Sample*>> streams;
    for (const Sample& s : samples_) streams[{ s.opcode, s.from_server }].push_back(&s);

    for (const auto& [key, group] : streams) {
        if (group.size() < min_samples + 1) continue;

        std::size_t shortest = SIZE_MAX;
        for (const Sample* s : group) shortest = std::min(shortest, s->payload.size());
        if (shortest < 2) continue;

        const std::vector<Reading> readings = all_readings(shortest);

        // Ungrouped first. The client->server stream is only ever about this
        // character, so there is nothing to separate and grouping it would
        // only split the evidence.
        // Correlate the values themselves, not the differences between them.
        //
        // Differences were the obvious choice -- they cancel the game's unknown
        // coordinate origin -- but they are a poor one. A straight-line fit
        // cancels the origin just as well, through its intercept, while a
        // difference throws away everything except consecutive pairs and
        // doubles the effect of every measurement error. Against the real game
        // that left a dozen usable samples out of hundreds and a correlation
        // of 0.6 on a field that was almost certainly right.
        const auto correlate = [&](const std::vector<const Sample*>& run,
                                   bool grouped, const Reading& id, std::uint32_t id_value) {
            if (run.size() < min_samples) return;

            // A quantity that never varied says nothing about which field
            // measures it: every constant correlates with every constant.
            //
            // This is not a nicety. On flat ground the view does not scroll
            // vertically at all, so the measured y is the same number from
            // start to finish -- and every y result from such a run is noise
            // dressed up as a finding, including several at 0.88 that sent an
            // afternoon chasing the wrong offsets. A quantity that did not
            // move is not evidence and is not reported.
            bool varied[4] = { false, false, false, false };
            for (int q = 0; q < 4; ++q) {
                double lo = 1e18, hi = -1e18;
                for (const Sample* s : run) {
                    if (!s->valid[q]) continue;
                    lo = std::min(lo, s->truth[q]);
                    hi = std::max(hi, s->truth[q]);
                }
                const double span = hi - lo;
                varied[q] = span >= (q == 0 ? kMinTravelX
                                   : q == 1 ? kMinTravelY
                                            : kMinVitalSwing);
            }
            if (!varied[0] && !varied[1] && !varied[2] && !varied[3]) return;

            // Elapsed time, as a decoy.
            //
            // Nearly every packet carries a clock or a sequence number, and a
            // character walking out and back spends the first half of the run
            // on one side and the second half on the other -- so the clock
            // correlates with position about as well as position does. The way
            // to tell them apart is to ask both questions: a coordinate tracks
            // where you are, a counter tracks when you asked. Whichever it
            // follows more closely is what it is.
            const std::uint64_t t0 = run.front()->at_ms;

            for (const Reading& r : readings) {
                Accum acc[4];
                Accum clock;
                for (const Sample* s : run) {
                    const double v = r.read(s->payload.data() + r.offset);
                    clock.add(v, static_cast<double>(s->at_ms - t0));
                    for (int q = 0; q < 4; ++q) {
                        if (!s->valid[q]) continue;
                        acc[q].add(v, s->truth[q]);
                    }
                }
                const double tick = std::abs(clock.correlation());

                for (int q = 0; q < 4; ++q) {
                    if (!varied[q]) continue;
                    if (acc[q].n < min_samples) continue;
                    const double c = acc[q].correlation();
                    if (!std::isfinite(c) || std::abs(c) < min_correlation) continue;
                    if (tick >= std::abs(c)) continue;      // a clock, not a coordinate

                    Candidate cand;
                    cand.opcode = key.first;
                    cand.direction = key.second ? Direction::ToClient : Direction::ToServer;
                    cand.reading = r;
                    cand.quantity = static_cast<Quantity>(q);
                    cand.grouped = grouped;
                    cand.id_reading = id;
                    cand.id_value = id_value;
                    cand.correlation = c;
                    cand.scale = acc[q].slope();
                    cand.samples = acc[q].n;
                    cand.clock_correlation = tick;
                    out.push_back(cand);
                }
            }
        };

        correlate(group, false, Reading{}, 0);

        // Then grouped by whatever looks like it names a creature. This is
        // what makes a server->client position packet readable: one opcode
        // carries the whole level, and only one creature in it is us.
        for (const Reading& id : id_candidates(group)) {
            std::map<std::uint32_t, std::vector<const Sample*>> by_entity;
            for (const Sample* s : group) {
                if (id.offset + 4 > s->payload.size()) continue;
                by_entity[static_cast<std::uint32_t>(id.read(s->payload.data() + id.offset))]
                    .push_back(s);
            }
            for (const auto& [value, run] : by_entity) correlate(run, true, id, value);
        }
    }

    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) {
        if (std::abs(std::abs(a.correlation) - std::abs(b.correlation)) > 0.01)
            return std::abs(a.correlation) > std::abs(b.correlation);
        return a.samples > b.samples;
    });

    // One line per answer.
    //
    // The same bytes read four ways produce four candidates that are all the
    // same discovery, and a byte window one along from the real one usually
    // correlates nearly as well. Keeping the best of each (creature, quantity)
    // turns thirty near-identical rows into the handful that mean something.
    std::vector<Candidate> trimmed;
    for (const Candidate& c : out) {
        bool covered = false;
        for (const Candidate& k : trimmed) {
            if (k.opcode != c.opcode || k.quantity != c.quantity) continue;
            if (k.grouped != c.grouped) continue;
            if (k.grouped && (k.id_value != c.id_value ||
                              k.id_reading.offset != c.id_reading.offset)) continue;
            if (std::abs(static_cast<int>(k.reading.offset) -
                         static_cast<int>(c.reading.offset)) <= 4) { covered = true; break; }
        }
        if (!covered) trimmed.push_back(c);
    }
    return trimmed;
}

std::vector<FieldFinder::Summary> FieldFinder::summarise() const {
    // One lock for the whole thing: id_candidates() does not take it, and
    // reaching for it again inside the loop would deadlock on the way in.
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Summary> out;
    std::map<std::pair<std::uint16_t, bool>, std::vector<const Sample*>> streams;
    for (const Sample& s : samples_) streams[{ s.opcode, s.from_server }].push_back(&s);

    for (const auto& [key, group] : streams) {
        Summary s;
        s.opcode = key.first;
        s.direction = key.second ? Direction::ToClient : Direction::ToServer;
        s.count = group.size();
        s.payload_bytes = group.empty() ? 0 : group.front()->payload.size();

        const std::vector<Reading> ids = id_candidates(group);
        if (!ids.empty()) {
            std::unordered_map<std::uint32_t, int> seen;
            for (const Sample* p : group)
                if (ids[0].offset + 4 <= p->payload.size())
                    ++seen[static_cast<std::uint32_t>(ids[0].read(p->payload.data() + ids[0].offset))];
            s.entities = seen.size();
            s.id_field = ids[0].describe();
        }
        out.push_back(s);
    }

    std::sort(out.begin(), out.end(), [](const Summary& a, const Summary& b) {
        return a.count > b.count;
    });
    return out;
}

std::vector<FieldFinder::Pair> FieldFinder::position_pairs(std::size_t min_samples,
                                                           double min_correlation) const {
    const std::vector<Candidate> all = analyse(min_samples, min_correlation);

    std::vector<Pair> out;
    for (const Candidate& x : all) {
        if (x.quantity != Quantity::X || !x.grouped) continue;
        for (const Candidate& y : all) {
            if (y.quantity != Quantity::Y || !y.grouped) continue;
            if (y.opcode != x.opcode) continue;
            if (y.id_reading.offset != x.id_reading.offset) continue;
            if (y.id_value != x.id_value) continue;
            // The two halves of a position sit next to each other, not at
            // opposite ends of the packet.
            const int gap = std::abs(static_cast<int>(y.reading.offset) -
                                     static_cast<int>(x.reading.offset));
            if (gap == 0 || gap > 8) continue;

            Pair p;
            p.x = x;
            p.y = y;
            p.score = std::min(std::abs(x.correlation), std::abs(y.correlation));
            out.push_back(p);
        }
    }

    std::sort(out.begin(), out.end(), [](const Pair& a, const Pair& b) {
        if (std::abs(a.score - b.score) > 0.01) return a.score > b.score;
        return a.x.samples > b.x.samples;
    });
    return out;
}

// -------------------------------------------------------------- storage

namespace {
constexpr std::uint32_t kFileMagic = 0x46464E44;    // "FFND"
// 2 adds the arrival time of each sample, without which a clock cannot be
// told from a coordinate. An older file is refused rather than read with the
// timestamps left at zero, which would make every field look like a clock.
constexpr std::uint32_t kFileVersion = 2;
}

std::vector<FieldFinder::Check>
FieldFinder::check_layout(std::uint16_t opcode, bool from_server,
                          std::size_t id_off, std::size_t x_off, std::size_t y_off,
                          int width, bool is_float, bool big_endian) const {
    std::lock_guard<std::mutex> lock(mutex_);

    Reading id_r; id_r.offset = id_off; id_r.width = 4;
    Reading x_r;  x_r.offset = x_off;  x_r.width = width;
    x_r.is_float = is_float; x_r.big_endian = big_endian;
    Reading y_r;  y_r.offset = y_off;  y_r.width = width;
    y_r.is_float = is_float; y_r.big_endian = big_endian;

    const std::size_t need = std::max({ id_off + 4, x_off + static_cast<std::size_t>(width),
                                        y_off + static_cast<std::size_t>(width) });

    struct Bucket { Accum x, y; double xlo = 1e18, xhi = -1e18, ylo = 1e18, yhi = -1e18; };
    std::map<std::uint32_t, Bucket> by_id;

    for (const Sample& s : samples_) {
        if (s.opcode != opcode || s.from_server != from_server) continue;
        if (s.payload.size() < need) continue;
        if (!s.valid[0] && !s.valid[1]) continue;

        const auto id = static_cast<std::uint32_t>(id_r.read(s.payload.data() + id_off));
        Bucket& b = by_id[id];
        const double xv = x_r.read(s.payload.data() + x_off);
        const double yv = y_r.read(s.payload.data() + y_off);
        if (s.valid[0]) {
            b.x.add(xv, s.truth[0]);
            b.xlo = std::min(b.xlo, xv);
            b.xhi = std::max(b.xhi, xv);
        }
        if (s.valid[1]) {
            b.y.add(yv, s.truth[1]);
            b.ylo = std::min(b.ylo, yv);
            b.yhi = std::max(b.yhi, yv);
        }
    }

    std::vector<Check> out;
    for (const auto& [id, b] : by_id) {
        Check c;
        c.id = id;
        c.samples = std::max(b.x.n, b.y.n);
        c.x_corr = b.x.correlation();
        c.x_scale = b.x.slope();
        c.y_corr = b.y.correlation();
        c.y_scale = b.y.slope();
        if (b.xhi >= b.xlo) { c.x_min = b.xlo; c.x_max = b.xhi; }
        if (b.yhi >= b.ylo) { c.y_min = b.ylo; c.y_max = b.yhi; }
        out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const Check& a, const Check& b) {
        return std::abs(a.x_corr) > std::abs(b.x_corr);
    });
    return out;
}

std::vector<FieldFinder::PositionField>
FieldFinder::position_fields(double world_w, double world_h,
                             std::size_t min_samples) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PositionField> out;

    // Group by opcode and direction; each is its own message with its own
    // layout and mixing them means nothing.
    std::map<std::pair<std::uint16_t, bool>, std::vector<const Sample*>> groups;
    for (const Sample& s : samples_) groups[{ s.opcode, s.from_server }].push_back(&s);

    for (const auto& [key, group] : groups) {
        if (group.size() < min_samples) continue;
        std::size_t shortest = SIZE_MAX;
        for (const Sample* s : group) shortest = std::min(shortest, s->payload.size());
        if (shortest < 8) continue;

        // A coordinate is a number in the right range that is not always the
        // same number. Both halves matter: a field that is always 1024.0 is in
        // range and is not a position, and a field that wanders over the whole
        // float line is varied and is not one either.
        const auto looks_like = [&](std::size_t off, double limit,
                                    double& lo, double& hi, std::size_t& inside) {
            lo = 1e30; hi = -1e30; inside = 0;
            Reading r; r.offset = off; r.width = 4; r.is_float = true;
            for (const Sample* s : group) {
                if (off + 4 > s->payload.size()) continue;
                const double v = r.read(s->payload.data() + off);
                if (v < 1.0 || v > limit) continue;
                ++inside;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
            return inside * 100 >= group.size() * 90 && hi - lo > kMinSpread;
        };

        for (std::size_t off = 0; off + 8 <= shortest; ++off) {
            double xlo, xhi, ylo, yhi;
            std::size_t xin = 0, yin = 0;
            if (!looks_like(off, world_w, xlo, xhi, xin)) continue;
            if (!looks_like(off + 4, world_h, ylo, yhi, yin)) continue;

            PositionField f;
            f.opcode = key.first;
            f.direction = key.second ? Direction::ToClient : Direction::ToServer;
            f.x_offset = off;
            f.y_offset = off + 4;
            f.samples = group.size();
            f.x_min = xlo; f.x_max = xhi;
            f.y_min = ylo; f.y_max = yhi;
            f.in_range_pct = 100.0 * static_cast<double>(std::min(xin, yin)) /
                             static_cast<double>(group.size());
            out.push_back(f);
        }
    }

    std::sort(out.begin(), out.end(), [](const PositionField& a, const PositionField& b) {
        return a.samples > b.samples;
    });
    return out;
}

bool FieldFinder::save(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::FILE* f = open_file(path, "wb");
    if (!f) return false;

    const auto u32 = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
    u32(kFileMagic);
    u32(kFileVersion);
    u32(static_cast<std::uint32_t>(samples_.size()));

    for (const Sample& s : samples_) {
        u32(s.opcode);
        u32(s.from_server ? 1u : 0u);
        u32(static_cast<std::uint32_t>(s.payload.size()));
        std::fwrite(s.payload.data(), 1, s.payload.size(), f);
        std::fwrite(s.truth, sizeof(double), 4, f);
        for (int q = 0; q < 4; ++q) u32(s.valid[q] ? 1u : 0u);
        std::fwrite(&s.at_ms, sizeof(std::uint64_t), 1, f);
    }

    std::fclose(f);
    GB_INFO("FieldFinder: wrote %zu samples to %s", samples_.size(), path.c_str());
    return true;
}

bool FieldFinder::load(const std::string& path) {
    std::FILE* f = open_file(path, "rb");
    if (!f) return false;

    const auto u32 = [&](std::uint32_t& v) { return std::fread(&v, 4, 1, f) == 1; };
    std::uint32_t magic = 0, version = 0, count = 0;
    if (!u32(magic) || !u32(version) || !u32(count) ||
        magic != kFileMagic || version != kFileVersion) {
        std::fclose(f);
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
    samples_.reserve(count);

    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t opcode = 0, server = 0, len = 0;
        if (!u32(opcode) || !u32(server) || !u32(len) || len > kMaxScan) break;

        Sample s;
        s.opcode = static_cast<std::uint16_t>(opcode);
        s.from_server = server != 0;
        s.payload.resize(len);
        if (len && std::fread(s.payload.data(), 1, len, f) != len) break;
        if (std::fread(s.truth, sizeof(double), 4, f) != 4) break;
        bool ok = true;
        for (int q = 0; q < 4; ++q) {
            std::uint32_t v = 0;
            if (!u32(v)) { ok = false; break; }
            s.valid[q] = v != 0;
        }
        if (!ok) break;
        if (std::fread(&s.at_ms, sizeof(std::uint64_t), 1, f) != 1) break;
        samples_.push_back(std::move(s));
    }

    std::fclose(f);
    stats_.packets = samples_.size();
    stats_.paired = samples_.size();
    GB_INFO("FieldFinder: read %zu samples from %s", samples_.size(), path.c_str());
    return !samples_.empty();
}

std::vector<FieldFinder::Example> FieldFinder::examples(std::uint16_t opcode,
                                                        bool from_server,
                                                        std::size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Example> out;
    for (const Sample& s : samples_) {
        if (s.opcode != opcode || s.from_server != from_server) continue;
        if (out.size() >= count) break;
        Example e;
        e.payload = s.payload;
        e.x = s.truth[0];
        e.y = s.truth[1];
        e.position = s.valid[0];
        out.push_back(std::move(e));
    }
    return out;
}

FieldFinder::Stats FieldFinder::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

} // namespace gobot
