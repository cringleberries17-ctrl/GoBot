#include "gobot/protocol_analyzer.hpp"
#include "gobot/byte_reader.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <set>
#include <sstream>

namespace gobot {
namespace {

/// A numeric field candidate at one offset, read one particular way.
struct NumericField {
    std::size_t offset = 0;
    std::size_t width = 0;
    FieldType   type = FieldType::I32;

    std::vector<double> values;

    double min_v = 0, max_v = 0, mean = 0;
    std::size_t distinct = 0;

    /// Fraction of consecutive pairs whose change is small relative to the
    /// field's overall range. High for coordinates, low for ids and noise.
    double smoothness = 0.0;

    /// Fraction of consecutive pairs that are identical. High for ids within
    /// a burst, and for constants.
    double stickiness = 0.0;

    /// How often the value reverses direction. Separates a position, which
    /// turns around, from a sequence number, which only ever climbs — both
    /// otherwise look equally "smooth".
    std::size_t direction_changes = 0;

    bool plausible_float = false;
    bool looks_constant = false;
};

double fraction(std::size_t n, std::size_t total) {
    return total ? static_cast<double>(n) / static_cast<double>(total) : 0.0;
}

/// Is this a value a game would plausibly store in a float?
bool sane_float(float f) {
    if (!std::isfinite(f)) return false;
    const float a = std::fabs(f);
    if (a == 0.0f) return true;
    return a > 1e-4f && a < 1e7f;
}

/// Measure one field. `indices`, when non-null, restricts the measurement to
/// a subset of the samples — used to look at a single entity's own track
/// inside a packet stream that interleaves many of them.
NumericField measure(const std::vector<Bytes>& samples, std::size_t offset,
                     std::size_t width, FieldType type,
                     const std::vector<std::size_t>* indices = nullptr) {
    NumericField f;
    f.offset = offset;
    f.width = width;
    f.type = type;

    const std::size_t n = indices ? indices->size() : samples.size();
    f.values.reserve(n);
    std::size_t sane = 0;

    for (std::size_t k = 0; k < n; ++k) {
        const Bytes& s = samples[indices ? (*indices)[k] : k];
        if (offset + width > s.size()) continue;
        ByteReader r(s.data(), s.size());
        r.seek(offset);

        double v = 0;
        switch (type) {
            case FieldType::I16: v = r.i16(); break;
            case FieldType::U16: v = r.u16(); break;
            case FieldType::I32: v = r.i32(); break;
            case FieldType::U32: v = static_cast<double>(r.u32()); break;
            case FieldType::F32: {
                const float g = r.f32();
                if (sane_float(g)) ++sane;
                v = static_cast<double>(g);
                break;
            }
            default: break;
        }
        if (!r.ok()) continue;
        f.values.push_back(v);
    }

    if (f.values.empty()) return f;

    if (type == FieldType::F32) f.plausible_float = fraction(sane, f.values.size()) > 0.92;

    f.min_v = *std::min_element(f.values.begin(), f.values.end());
    f.max_v = *std::max_element(f.values.begin(), f.values.end());
    f.mean  = std::accumulate(f.values.begin(), f.values.end(), 0.0) /
              static_cast<double>(f.values.size());

    std::set<double> uniq(f.values.begin(), f.values.end());
    f.distinct = uniq.size();
    f.looks_constant = (f.distinct == 1);

    const double range = f.max_v - f.min_v;
    if (range > 0 && f.values.size() > 2) {
        std::size_t small = 0, same = 0;

        // "Small" relative to the field's own span. The factor has to be
        // generous: a monster pacing a 60-unit patrol in 5-unit steps is every
        // bit as much a coordinate as one crossing a 3000-unit map, and a
        // tight threshold rejects the first while accepting the second.
        // Uniformly random values clear this only about a quarter of the time,
        // which is what keeps noise out.
        const double threshold = range * 0.25;

        double last_delta = 0.0;
        for (std::size_t i = 1; i < f.values.size(); ++i) {
            const double delta = f.values[i] - f.values[i - 1];
            const double d = std::fabs(delta);
            if (d <= threshold) ++small;
            if (d == 0.0) ++same;

            if (delta != 0.0) {
                if (last_delta != 0.0 && ((delta > 0) != (last_delta > 0)))
                    ++f.direction_changes;
                last_delta = delta;
            }
        }
        f.smoothness = fraction(small, f.values.size() - 1);
        f.stickiness = fraction(same, f.values.size() - 1);
    }
    return f;
}

/// Does this field behave like a map coordinate?
bool coordinate_like(const NumericField& f) {
    if (f.values.size() < 8) return false;
    if (f.looks_constant) return false;

    // Needs to actually move, but not teleport every packet.
    if (f.smoothness < 0.55) return false;
    if (f.stickiness > 0.95) return false;
    if (f.distinct < 4) return false;

    // A sequence number is perfectly smooth and never turns around. Requiring
    // at least one reversal keeps counters and timestamps out.
    if (f.direction_changes < 1) return false;

    if (f.type == FieldType::F32) return f.plausible_float;

    // Integer coordinates live in map-sized ranges. Anything spanning
    // hundreds of millions is an id or a timestamp, not a position.
    const double span = f.max_v - f.min_v;
    if (span < 2 || span > 200000) return false;
    if (std::fabs(f.max_v) > 1e7) return false;
    return true;
}

/// Could this be the *other* axis of a coordinate pair?
///
/// Held to a weaker standard than the primary axis on purpose. On flat ground
/// a character's vertical position barely changes, and a side-scroller's y is
/// often pinned to the floor for long stretches — demanding that both axes
/// show movement and reversal would reject the commonest case there is. It
/// still has to read plausibly for its type, so this does not open the door
/// to arbitrary bytes.
bool coordinate_partner(const NumericField& f, const NumericField& primary) {
    if (f.type != primary.type || f.width != primary.width) return false;
    if (f.values.size() < 8) return false;
    if (f.type == FieldType::F32 && !f.plausible_float) return false;

    if (f.looks_constant) return true;              // pinned to the floor
    if (f.smoothness >= 0.55) return true;          // moving like a coordinate

    if (f.type != FieldType::F32) {
        const double span = f.max_v - f.min_v;
        if (span > 0 && span <= 200000 && f.stickiness > 0.5) return true;  // mostly still
    }
    return false;
}

/// Does this field behave like an entity id?
bool id_like(const NumericField& f) {
    if (f.values.size() < 8) return false;
    if (f.looks_constant) return false;
    if (f.distinct < 3) return false;

    // Repeats often (the same entity updates repeatedly) but jumps when it
    // changes — the opposite profile to a coordinate.
    if (f.smoothness > 0.5) return false;
    if (f.type == FieldType::F32) return false;
    return f.stickiness > 0.15 || f.distinct < f.values.size() / 2;
}

/// A value pair that behaves like current/maximum.
bool hp_pair_like(const NumericField& cur, const NumericField& max) {
    if (cur.values.size() < 6 || max.values.size() != cur.values.size()) return false;
    if (max.distinct > 3) return false;          // a maximum rarely changes
    if (max.looks_constant && max.max_v <= 0) return false;
    if (cur.distinct < 3) return false;

    std::size_t ok = 0;
    for (std::size_t i = 0; i < cur.values.size(); ++i) {
        if (cur.values[i] >= 0 && max.values[i] > 0 && cur.values[i] <= max.values[i]) ++ok;
    }
    return fraction(ok, cur.values.size()) > 0.9;
}

std::size_t common_length(const std::vector<Bytes>& samples) {
    if (samples.empty()) return 0;
    std::size_t n = samples.front().size();
    for (const auto& s : samples) n = std::min(n, s.size());
    return n;
}

const char* type_token(FieldType t) {
    switch (t) {
        case FieldType::I16: return "i16";
        case FieldType::U16: return "u16";
        case FieldType::I32: return "i32";
        case FieldType::U32: return "u32";
        case FieldType::F32: return "f32";
        default: return "i32";
    }
}

FieldSpec make_field(const std::string& name, const NumericField& f) {
    FieldSpec s;
    s.name = name;
    s.type = f.type;
    s.has_fixed_offset = true;
    s.fixed_offset = f.offset;
    return s;
}

/// Build every numeric reading worth considering inside one payload shape.
std::vector<NumericField> candidates(const std::vector<Bytes>& samples, std::size_t len) {
    std::vector<NumericField> out;
    for (std::size_t off = 0; off + 2 <= len; ++off) {
        if (off + 4 <= len) {
            out.push_back(measure(samples, off, 4, FieldType::I32));
            out.push_back(measure(samples, off, 4, FieldType::F32));
        }
        out.push_back(measure(samples, off, 2, FieldType::I16));
    }
    return out;
}

/// Partition sample indices by the value of a field.
///
/// Entity updates interleave: consecutive packets describe different monsters,
/// so a coordinate read straight down the stream jumps around and looks like
/// noise. Split by entity id first and each track is smooth again.
std::vector<std::vector<std::size_t>> group_by(const std::vector<Bytes>& samples,
                                               const NumericField& key) {
    std::map<double, std::vector<std::size_t>> by_value;
    for (std::size_t i = 0; i < key.values.size() && i < samples.size(); ++i)
        by_value[key.values[i]].push_back(i);

    std::vector<std::vector<std::size_t>> out;
    for (auto& [v, idx] : by_value) {
        if (idx.size() >= 5) out.push_back(std::move(idx));
    }
    return out;
}

/// Mean smoothness of a field measured inside each group separately.
double grouped_smoothness(const std::vector<Bytes>& samples,
                          const std::vector<std::vector<std::size_t>>& groups,
                          std::size_t offset, std::size_t width, FieldType type,
                          bool& plausible_float) {
    if (groups.empty()) return 0.0;

    double total = 0.0;
    std::size_t counted = 0;
    std::size_t float_ok = 0;

    std::size_t turned = 0;
    std::size_t constant_tracks = 0;

    for (const auto& g : groups) {
        const NumericField f = measure(samples, offset, width, type, &g);
        if (f.values.size() < 4) continue;
        if (f.plausible_float) ++float_ok;

        if (f.looks_constant) {
            // A still entity, or an axis pinned to the floor. Counts as
            // perfectly smooth rather than being skipped — skipping it made
            // a stationary y look like no evidence at all.
            ++constant_tracks;
            total += 1.0;
            ++counted;
            continue;
        }
        total += f.smoothness;
        if (f.direction_changes >= 1) ++turned;
        ++counted;
    }
    if (counted == 0) return 0.0;
    plausible_float = (float_ok * 2 >= counted);

    // Counters climb forever and never turn around. Only judge this on the
    // tracks that actually moved — constant ones say nothing either way.
    const std::size_t moving = counted - constant_tracks;
    if (moving > 0 && turned * 2 < moving) return 0.0;

    return total / static_cast<double>(counted);
}

/// Find an adjacent pair of coordinate-like fields of the same type — the
/// (x, y) signature.
bool find_xy(const std::vector<NumericField>& cands,
             const NumericField*& x, const NumericField*& y) {
    x = y = nullptr;
    double best = 0;

    for (const auto& a : cands) {
        if (!coordinate_like(a)) continue;
        for (const auto& b : cands) {
            if (b.offset != a.offset + a.width) continue;   // must be adjacent
            if (!coordinate_partner(b, a)) continue;

            // Prefer the pair that moves most convincingly, and prefer floats,
            // which are far less likely to be a coincidence.
            double score = a.smoothness + b.smoothness;
            if (a.type == FieldType::F32) score += 0.6;

            // Protocol fields sit on their natural alignment far more often
            // than not, and a misaligned read that straddles two real fields
            // can look deceptively smooth — the halves of a float pair being
            // the classic case.
            if (a.offset % a.width == 0) score += 0.5;

            if (score > best) {
                best = score;
                x = &a;
                y = &b;
            }
        }
    }
    return x != nullptr;
}

} // namespace

std::string describe_opcode(std::uint16_t opcode, const OpcodeObservations& obs) {
    std::ostringstream os;
    os << "0x" << std::hex << opcode << std::dec
       << "  n=" << obs.count()
       << "  C->S=" << obs.to_server
       << "  S->C=" << obs.to_client;

    const std::size_t len = common_length(obs.payloads);
    os << "  body=" << len << "B";
    return os.str();
}

std::vector<SemanticGuess> analyze_protocol(
    const std::map<std::uint16_t, OpcodeObservations>& data) {

    std::vector<SemanticGuess> out;

    for (const auto& [opcode, obs] : data) {
        if (obs.payloads.size() < 8) continue;      // too little to say anything

        const std::size_t len = common_length(obs.payloads);
        if (len < 4) continue;

        const auto cands = candidates(obs.payloads, len);

        const NumericField* x = nullptr;
        const NumericField* y = nullptr;
        const bool has_xy = find_xy(cands, x, y);

        const bool client_sent = obs.to_server > 0;
        const bool mostly_client = obs.to_server > obs.to_client;

        // ---- self position --------------------------------------------
        // A packet the client sends can only be about the client. Two
        // adjacent smoothly-moving numbers in one is your position.
        if (has_xy && client_sent) {
            SemanticGuess g;
            g.opcode = opcode;
            g.semantic = Semantic::SelfMove;
            g.suggested_name = "PlayerMove";
            g.confidence = mostly_client ? 0.88 : 0.72;
            if (x->type == FieldType::F32) g.confidence += 0.07;

            std::ostringstream r;
            r << "Sent by the client (" << obs.to_server << " times), so it describes you. "
              << "Offsets " << x->offset << " and " << y->offset << " hold two adjacent "
              << type_token(x->type) << " values that move in small steps "
              << "(" << static_cast<int>(x->smoothness * 100) << "% / "
              << static_cast<int>(y->smoothness * 100) << "% smooth), "
              << "ranging " << static_cast<long long>(x->min_v) << ".."
              << static_cast<long long>(x->max_v) << " and "
              << static_cast<long long>(y->min_v) << ".."
              << static_cast<long long>(y->max_v) << ".";
            g.reason = r.str();

            g.fields.push_back(make_field("x", *x));
            g.fields.push_back(make_field("y", *y));
            out.push_back(std::move(g));
            continue;
        }

        // ---- monster / player position --------------------------------
        // Server-only, with an id ahead of the coordinates.
        //
        // The ungrouped (x, y) test above rarely fires here, and shouldn't:
        // these packets interleave entities, so a coordinate read straight
        // down the stream jumps between monsters and looks like noise. Find
        // the id first, split the stream into per-entity tracks, and each
        // track moves smoothly again.
        if (!client_sent) {
            const NumericField* best_id = nullptr;
            const NumericField* best_x = nullptr;
            const NumericField* best_y = nullptr;
            double best_score = 0.0;
            bool   best_float = false;

            for (const auto& id : cands) {
                if (id.width < 4 || id.type == FieldType::F32) continue;
                if (!id_like(id)) continue;

                const auto groups = group_by(obs.payloads, id);
                if (groups.size() < 2) continue;     // no real partitioning

                // Enough of the stream must fall into sizeable groups, or the
                // "id" is really a counter and the split is meaningless.
                std::size_t covered = 0;
                for (const auto& g2 : groups) covered += g2.size();
                if (fraction(covered, obs.payloads.size()) < 0.5) continue;

                for (std::size_t off = id.offset + id.width; off + 8 <= len; ++off) {
                    for (FieldType t : { FieldType::F32, FieldType::I32 }) {
                        bool fx = false, fy = false;
                        const double sx = grouped_smoothness(obs.payloads, groups, off, 4, t, fx);
                        const double sy = grouped_smoothness(obs.payloads, groups, off + 4, 4, t, fy);
                        if (sx < 0.6 || sy < 0.6) continue;
                        if (t == FieldType::F32 && !(fx && fy)) continue;

                        double score = sx + sy + (t == FieldType::F32 ? 0.6 : 0.0);

                        // Fields of a struct share an alignment. We cannot
                        // assume it is 0 mod 4 — the framer's header_size is a
                        // lower bound, so every payload offset may be shifted
                        // by however many header bytes it could not account
                        // for — but the id and the coordinates should agree
                        // with each other. Requiring that consistency stops
                        // the result drifting between runs onto whichever
                        // misaligned read happened to look smoothest.
                        if (off % 4 == id.offset % 4) score += 0.5;

                        if (score > best_score) {
                            best_score = score;
                            best_id = &id;
                            best_float = (t == FieldType::F32);
                            // Re-measure over the whole stream purely to carry
                            // offset/type into the layout.
                            static NumericField hx, hy;
                            hx = measure(obs.payloads, off, 4, t);
                            hy = measure(obs.payloads, off + 4, 4, t);
                            best_x = &hx;
                            best_y = &hy;
                        }
                    }
                }
            }

            if (best_id && best_x && best_y) {
                const auto groups = group_by(obs.payloads, *best_id);

                SemanticGuess g;
                g.opcode = opcode;
                g.semantic = Semantic::MonsterUpdate;
                g.suggested_name = "EntityUpdate";
                g.confidence = 0.72 + (best_float ? 0.08 : 0.0);

                std::ostringstream r;
                r << "Server-only. Offset " << best_id->offset << " holds "
                  << best_id->distinct << " distinct values that repeat, which reads as "
                  << "an entity id. Grouping the packets by it gives " << groups.size()
                  << " separate tracks, and within each one the values at offsets "
                  << best_x->offset << " and " << best_y->offset << " move in small steps -- "
                  << "so those are per-entity coordinates. "
                  << "Whether these are monsters or other players is not something the "
                  << "bytes can say; check in game before applying.";
                g.reason = r.str();

                g.fields.push_back(make_field("id", *best_id));
                g.fields.push_back(make_field("x", *best_x));
                g.fields.push_back(make_field("y", *best_y));
                out.push_back(std::move(g));
                continue;
            }

            // The ungrouped coordinate fallback deliberately comes *after* the
            // vitals check below. A draining HP bar and a walking character
            // both look like "smooth numbers", but "never exceeds the constant
            // beside it" is a far more specific signature than "moves in small
            // steps" — so the more specific test has to get first refusal.
        }

        // ---- hp / mp ---------------------------------------------------
        // Server-sent, carrying one or more current/maximum pairs.
        //
        // Vitals almost always travel together: HP and MP in one packet, as
        // two adjacent current/max pairs. Finding only the first would leave
        // MP unmapped, and the bot gates MP potions on it — so collect every
        // pair, not just the best one.
        if (!client_sent && len <= 64) {
            std::vector<std::pair<const NumericField*, const NumericField*>> pairs;

            for (std::size_t w : { std::size_t{4}, std::size_t{2} }) {
                for (const auto& a : cands) {
                    if (a.width != w || a.type == FieldType::F32) continue;
                    for (const auto& b : cands) {
                        if (b.width != w || b.type == FieldType::F32) continue;
                        if (b.offset != a.offset + w) continue;
                        if (!hp_pair_like(a, b)) continue;

                        // Don't let a pair overlap one already taken.
                        bool clash = false;
                        for (const auto& [pc, pm] : pairs) {
                            const std::size_t lo = pc->offset, hi = pm->offset + pm->width;
                            if (a.offset < hi && lo < b.offset + b.width) { clash = true; break; }
                        }
                        if (!clash) pairs.emplace_back(&a, &b);
                    }
                }
                if (!pairs.empty()) break;      // prefer a consistent width
            }

            std::sort(pairs.begin(), pairs.end(), [](const auto& l, const auto& r) {
                return l.first->offset < r.first->offset;
            });

            if (!pairs.empty()) {
                SemanticGuess g;
                g.opcode = opcode;
                g.semantic = Semantic::SelfHpMp;
                g.suggested_name = pairs.size() >= 2 ? "VitalsUpdate" : "HpUpdate";
                g.confidence = pairs.size() >= 2 ? 0.72 : 0.6;

                std::ostringstream r;
                r << "Server-sent packet carrying " << pairs.size()
                  << " current/maximum pair" << (pairs.size() == 1 ? "" : "s") << ". ";

                static const char* const kCur[] = { "hp", "mp" };
                static const char* const kMax[] = { "max_hp", "max_mp" };

                const std::size_t take = std::min<std::size_t>(pairs.size(), 2);
                for (std::size_t i = 0; i < take; ++i) {
                    const auto* cur = pairs[i].first;
                    const auto* max = pairs[i].second;

                    r << "At offset " << cur->offset << " a value varying over "
                      << static_cast<long long>(cur->min_v) << ".."
                      << static_cast<long long>(cur->max_v)
                      << " never exceeds the near-constant "
                      << static_cast<long long>(max->mean) << " at offset "
                      << max->offset << ". ";

                    g.fields.push_back(make_field(kCur[i], *cur));
                    g.fields.push_back(make_field(kMax[i], *max));
                }

                if (take >= 2) {
                    r << "Labelled HP first and MP second, which is the usual order -- "
                         "but that is convention, not evidence. Apply it, then watch the "
                         "Dashboard bars while you take damage and spend mana: if they are "
                         "swapped, exchange the hp and mp names in the layout.";
                } else {
                    r << "Only one pair here, so this could be HP or MP -- the bytes "
                         "cannot say which. Apply it and check the Dashboard against your "
                         "bars; if it tracks mana, rename hp/max_hp to mp/max_mp.";
                }
                g.reason = r.str();
                out.push_back(std::move(g));
                continue;
            }
        }

        // ---- single-entity position broadcast --------------------------
        // Last resort: server-sent coordinates with no id to group by, and
        // no vitals interpretation. Often your own position echoed back.
        if (!client_sent && has_xy) {
            SemanticGuess g;
            g.opcode = opcode;
            g.semantic = Semantic::MonsterUpdate;
            g.suggested_name = "EntityUpdate";
            g.confidence = 0.5;

            std::ostringstream r;
            r << "Server-only, with two adjacent " << type_token(x->type)
              << " values at offsets " << x->offset << "/" << y->offset
              << " that move smoothly. No repeating id field was found, so this may "
              << "be a single entity -- possibly your own position broadcast back.";
            g.reason = r.str();

            g.fields.push_back(make_field("x", *x));
            g.fields.push_back(make_field("y", *y));
            out.push_back(std::move(g));
        }
    }

    std::sort(out.begin(), out.end(),
              [](const SemanticGuess& a, const SemanticGuess& b) {
                  return a.confidence > b.confidence;
              });

    GB_INFO("Protocol analysis: %zu suggestions from %zu opcodes", out.size(), data.size());
    return out;
}

} // namespace gobot
