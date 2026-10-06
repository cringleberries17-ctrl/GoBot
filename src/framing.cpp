#include "gobot/framing.hpp"
#include "gobot/byte_reader.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace gobot {

bool FramingSpec::plausible() const {
    if (length_size != 1 && length_size != 2 && length_size != 4) return false;
    if (opcode_size != 1 && opcode_size != 2) return false;
    if (header_size == 0 || header_size > 64) return false;
    if (length_offset + length_size > header_size) return false;
    if (opcode_offset + opcode_size > header_size) return false;
    if (min_frame < header_size) return false;
    if (max_frame < min_frame) return false;

    // Overlapping length and opcode fields would mean one value serving two
    // purposes. Possible in principle, never seen in practice, and allowing it
    // floods the detector with false positives.
    const std::uint32_t l0 = length_offset, l1 = length_offset + length_size;
    const std::uint32_t o0 = opcode_offset, o1 = opcode_offset + opcode_size;
    if (l0 < o1 && o0 < l1) return false;

    return true;
}

bool FramingSpec::checksum_ok(const std::uint8_t* p, std::size_t available) const {
    if (!has_checksum()) return true;
    if (checksum_offset + 2 > header_size || header_size > available) return false;

    const std::size_t words = header_size / 2;
    std::uint64_t sum = 0, check = 0;

    for (std::size_t w = 0; w < words; ++w) {
        std::uint64_t v = 0;
        if (!read_uint_at(p, available, w * 2, 2, true, v)) return false;
        if (w * 2 == checksum_offset) check = v;
        else                          sum += v;
    }
    return (sum & 0xFFFF) == check;
}

std::string FramingSpec::describe() const {
    std::ostringstream os;
    os << "header=" << header_size
       << " len@" << length_offset << ":" << length_size
       << (length_le ? "le" : "be")
       << (length_includes_header ? "(total)" : "(body)");
    if (length_adjust) os << (length_adjust > 0 ? "+" : "") << length_adjust;
    os << " op@" << opcode_offset << ":" << opcode_size
       << (opcode_le ? "le" : "be");
    return os.str();
}

std::string FramingSpec::to_ini() const {
    std::ostringstream os;
    os << "[Framing]\n"
       << "header_size=" << header_size << "\n"
       << "length_offset=" << length_offset << "\n"
       << "length_size=" << length_size << "\n"
       << "length_endian=" << (length_le ? "little" : "big") << "\n"
       << "length_includes_header=" << (length_includes_header ? "true" : "false") << "\n"
       << "length_adjust=" << length_adjust << "\n"
       << "opcode_offset=" << opcode_offset << "\n"
       << "opcode_size=" << opcode_size << "\n"
       << "opcode_endian=" << (opcode_le ? "little" : "big") << "\n"
       << "min_frame=" << min_frame << "\n"
       << "max_frame=" << max_frame << "\n";
    if (has_checksum())
        os << "checksum_offset=" << checksum_offset << "\n";
    if (has_trailer())
        os << "trailer_opcode=0x" << std::hex << trailer_opcode << std::dec << "\n"
           << "trailer_bytes=" << trailer_bytes << "\n";
    return os.str();
}

// ------------------------------------------------------------- frame scanner

FrameScanResult extract_frames(const FramingSpec& spec,
                               const std::uint8_t* buf,
                               std::size_t size,
                               Direction dir,
                               std::uint64_t timestamp_ms,
                               bool locked) {
    FrameScanResult result;
    result.locked = locked;
    if (!spec.plausible() || buf == nullptr) return result;

    // The opcode a header at `p` carries, or 0 if it cannot be read. Needed
    // before the size is known, because one opcode's frames are longer than
    // their length field admits.
    auto opcode_at = [&](std::size_t p) -> std::uint64_t {
        std::uint64_t op = 0;
        if (!read_uint_at(buf + p, size - p, spec.opcode_offset,
                          static_cast<int>(spec.opcode_size), spec.opcode_le, op))
            return 0;
        return op;
    };

    // Size a frame header at `p` would declare, or 0 if it declares nonsense.
    auto declared_at = [&](std::size_t p) -> std::uint32_t {
        if (p + spec.header_size > size) return 0;
        if (!spec.checksum_ok(buf + p, size - p)) return 0;
        std::uint64_t rl = 0;
        if (!read_uint_at(buf + p, size - p, spec.length_offset,
                          static_cast<int>(spec.length_size), spec.length_le, rl))
            return 0;
        const std::uint32_t t = spec.total_size_for(rl, opcode_at(p));
        return (t >= spec.min_frame && t <= spec.max_frame) ? t : 0;
    };

    std::size_t pos = 0;
    // Set after any byte-skip, cleared once a frame has been confirmed by the
    // header that follows it. A stream the caller has not yet locked starts
    // here: its first bytes are as untrustworthy as post-garbage ones.
    bool resyncing = !locked;

    while (pos + spec.header_size <= size) {
        std::uint64_t raw_len = 0;
        if (!read_uint_at(buf + pos, size - pos, spec.length_offset,
                          static_cast<int>(spec.length_size), spec.length_le, raw_len)) {
            break;      // header straddles the end of what we have
        }

        // A failing checksum means this is not a frame header, whatever the
        // length field claims. Checking it before trusting the length is what
        // stops the scanner swallowing hundreds of real frames inside one
        // bogus oversized "frame".
        const bool header_valid = spec.checksum_ok(buf + pos, size - pos);

        const std::uint32_t total =
            header_valid ? spec.total_size_for(raw_len, opcode_at(pos)) : 0;

        if (!header_valid || total < spec.min_frame || total > spec.max_frame) {
            // Lost alignment. Slide one byte and try again. Doing this
            // silently would hide a wrong FramingSpec, so it's counted.
            ++pos;
            ++result.resyncs;
            resyncing = true;
            continue;
        }

        if (pos + total > size) {
            // While locked, this just means the frame hasn't fully arrived —
            // wait. While resyncing it means the opposite: we landed on bytes
            // that happen to parse as an enormous length, and trusting it
            // would stall the scanner until that many bytes arrive, which may
            // be never. Keep sliding instead.
            if (!resyncing) break;
            ++pos;
            ++result.resyncs;
            continue;
        }

        if (resyncing) {
            // Re-locking after garbage: the first offset that merely *parses*
            // is not good enough. Two random bytes look like a valid length
            // often enough that accepting one swallows the real frames behind
            // it. Require the following header to be plausible too — a chain
            // of two is far harder to hit by chance than one.
            const std::size_t next = pos + total;
            const bool chain_ok = (next == size)                          // clean end
                               || (next + spec.header_size > size)        // can't tell yet
                               || (declared_at(next) != 0);               // next parses
            if (!chain_ok) {
                ++pos;
                ++result.resyncs;
                continue;
            }
            resyncing = false;
        }

        std::uint64_t opcode = 0;
        if (!read_uint_at(buf + pos, size - pos, spec.opcode_offset,
                          static_cast<int>(spec.opcode_size), spec.opcode_le, opcode)) {
            ++pos;
            ++result.resyncs;
            continue;
        }

        // The trailer is framing, not content: it is excluded from the payload
        // so that a field at body offset N is at offset N whether or not its
        // opcode happens to carry one.
        std::size_t body_end = pos + total;
        if (spec.trailer_bytes != 0 && opcode == spec.trailer_opcode) {
            const std::size_t trailer = spec.trailer_bytes;
            body_end = (body_end >= pos + spec.header_size + trailer)
                         ? body_end - trailer
                         : pos + spec.header_size;
        }

        GamePacket pkt;
        pkt.opcode       = static_cast<std::uint16_t>(opcode);
        pkt.from_server  = (dir == Direction::ToClient);
        pkt.timestamp_ms = timestamp_ms;
        pkt.payload.assign(buf + pos + spec.header_size, buf + body_end);

        result.packets.push_back(std::move(pkt));
        pos += total;
        result.locked = true;      // a frame parsed cleanly; alignment confirmed
    }

    result.consumed = pos;
    return result;
}

// ------------------------------------------------------------ auto-detection

namespace {

/// Tile `sample` with `spec` and score the fit.
///
/// A correct spec consumes the sample end to end with zero resyncs. Wrong ones
/// either resync constantly or produce absurd frame sizes, both of which the
/// scoring punishes. Distinct-opcode count breaks ties: a spec that reads the
/// opcode out of a payload byte tends to produce near-random opcodes, while
/// the real field yields a compact recurring set.
FramingScore score_spec(const FramingSpec& spec,
                        const std::uint8_t* sample,
                        std::size_t size) {
    FramingScore fs;
    fs.spec = spec;

    std::size_t pos = 0;
    std::size_t frames = 0;
    std::size_t resyncs = 0;
    std::set<std::uint16_t> opcodes;
    std::size_t explained = 0;

    std::map<std::uint32_t, std::size_t> size_counts;
    std::map<std::uint16_t, std::map<std::uint32_t, std::size_t>> size_by_opcode;

    // Header-checksum evidence: how often each u16 slot equals the sum of the
    // rest. Probing a bounded prefix keeps this off the hot path of a search
    // that evaluates tens of thousands of candidates.
    static constexpr std::size_t kMaxHeaderWords = 8;
    static constexpr std::size_t kChecksumProbeFrames = 400;
    std::size_t checksum_hits[kMaxHeaderWords] = {};
    std::size_t checksum_tested = 0;

    while (pos + spec.header_size <= size) {
        std::uint64_t raw_len = 0;
        if (!read_uint_at(sample + pos, size - pos, spec.length_offset,
                          static_cast<int>(spec.length_size), spec.length_le, raw_len))
            break;

        // Same rule the real scanner applies: a header that fails its checksum
        // is not a header, whatever its length field says. Scoring has to
        // model this or it ranks specs on behaviour they will never exhibit.
        const bool header_valid = spec.checksum_ok(sample + pos, size - pos);
        const std::uint32_t total = header_valid ? spec.total_size(raw_len) : 0;

        if (!header_valid || total < spec.min_frame || total > spec.max_frame || pos + total > size) {
            ++resyncs;
            ++pos;

            // Give up only when resyncing has cost more than the spec has
            // managed to explain.
            //
            // This was previously "more than 64 resyncs and more than 4 per
            // frame", which aborted the *correct* spec: a real capture hits a
            // rough patch early — a frame whose body the length field does not
            // describe, or a gap where packets were missed — and 140 bytes of
            // resync after 5 good frames tripped it. Scoring then saw fewer
            // than 8 frames and returned zero, so the right answer never
            // competed at all.
            if (resyncs > 2048 && resyncs > explained) break;
            continue;
        }

        std::uint64_t op = 0;
        read_uint_at(sample + pos, size - pos, spec.opcode_offset,
                     static_cast<int>(spec.opcode_size), spec.opcode_le, op);
        const auto opcode = static_cast<std::uint16_t>(op);
        opcodes.insert(opcode);

        ++size_counts[total];
        ++size_by_opcode[opcode][total];

        // Header checksum test.
        //
        // Many protocols put a check word in the header equal to the sum of
        // the others. Where one exists it is ground truth: a wrong framing
        // satisfies it with probability 1/65536 per frame, so agreement across
        // hundreds of frames is proof in a way no statistic can match. Test
        // every u16 slot in the header as the candidate check word.
        if (spec.header_size >= 8 && spec.header_size <= 16 && frames < kChecksumProbeFrames) {
            const std::size_t words = spec.header_size / 2;
            for (std::size_t target = 0; target < words && target < kMaxHeaderWords; ++target) {
                std::uint64_t sum = 0, check = 0;
                bool readable = true;
                for (std::size_t w = 0; w < words; ++w) {
                    std::uint64_t v = 0;
                    if (!read_uint_at(sample + pos, size - pos, w * 2, 2, true, v)) {
                        readable = false;
                        break;
                    }
                    if (w == target) check = v;
                    else             sum += v;
                }
                if (readable && (sum & 0xFFFF) == check) ++checksum_hits[target];
            }
            ++checksum_tested;
        }

        explained += total;
        ++frames;
        pos += total;
    }

    fs.frames_parsed    = frames;
    fs.bytes_explained  = explained;
    fs.distinct_opcodes = opcodes.size();
    fs.coverage = size ? static_cast<double>(explained) / static_cast<double>(size) : 0.0;

    if (frames < 8) {
        fs.score = 0.0;
        return fs;
    }

    // Coverage dominates; a clean tiling is the whole signal.
    double s = fs.coverage * 1000.0;

    // Resyncs count against, but as a proportion of the stream.
    //
    // This was previously a flat 5 points per resynced *byte*, which was
    // catastrophic: a real capture routinely needs to resync past ~10% of the
    // stream (frames whose body the length field does not describe, chunks
    // lost to a capture gap), and on a 50 KB sample that came to -23,000 —
    // enough to bury the correct spec under any garbage one that happened to
    // tile cleanly. Coverage already reflects unexplained bytes; this just
    // adds a proportional nudge.
    const double resync_fraction = size ? static_cast<double>(resyncs) / static_cast<double>(size) : 0.0;
    s -= 600.0 * std::min(resync_fraction, 1.0);

    // Prefer a compact opcode alphabet, but not a degenerate one-opcode fit
    // (which is what you get when the "opcode" lands on a constant byte).
    if (opcodes.size() >= 2) {
        const double ratio = static_cast<double>(opcodes.size()) / static_cast<double>(frames);
        if (ratio < 0.5) s += 100.0 * (1.0 - ratio);
    } else {
        s -= 200.0;
    }

    // Absolute alphabet size matters too, not just the ratio. A real capture
    // exercises a few dozen message types; a wrong framing reading "opcodes"
    // out of payload bytes produces hundreds. The ratio test alone misses this
    // when the bogus spec also inflates the frame count, which is exactly what
    // a one-byte length field does.
    if (opcodes.size() > 60)
        s -= 4.0 * static_cast<double>(opcodes.size() - 60);

    // A one-byte length field is the degenerate fit. Almost any byte is a
    // "valid" length, so such a spec tiles essentially any stream end to end
    // and scores perfect coverage on pure noise. Real protocols need two bytes
    // or more the moment a message can exceed 255 bytes, which every game
    // protocol's map and inventory packets do. Without this penalty the
    // garbage fits crowd out the real answer.
    if (spec.length_size == 1) s -= 300.0;

    // Frame-size regularity. A real protocol emits a small set of recurring
    // message shapes; a bogus tiling produces a scatter of arbitrary sizes.
    // This replaces an earlier "more frames is better" term, which had it
    // exactly backwards — it rewarded the degenerate fits for chopping the
    // stream into more, smaller pieces.
    const double size_ratio = static_cast<double>(size_counts.size()) /
                              static_cast<double>(frames);
    s += 250.0 * (1.0 - std::min(size_ratio * 4.0, 1.0));

    // Opcode/size agreement. In a real protocol a given message type has a
    // characteristic size, so opcode predicts length. Under a wrong framing the
    // two are unrelated.
    //
    // Measured naively this is degenerate: a spec that reads "opcodes" out of
    // random payload bytes gives almost every frame a unique opcode, each
    // trivially agreeing with its own single size, and scores a perfect 1.0.
    // So weight it by how compact the opcode alphabet is — agreement only
    // means anything when opcodes actually repeat.
    std::size_t dominant = 0;
    for (const auto& [op, sizes] : size_by_opcode) {
        std::size_t best_for_op = 0;
        for (const auto& [sz, n] : sizes) best_for_op = std::max(best_for_op, n);
        dominant += best_for_op;
    }
    const double agreement = frames ? static_cast<double>(dominant) / static_cast<double>(frames) : 0.0;
    const double opcode_ratio = frames ? static_cast<double>(opcodes.size()) / static_cast<double>(frames) : 1.0;
    const double agreement_weight = 1.0 - std::min(opcode_ratio * 2.0, 1.0);
    s += 250.0 * agreement * agreement_weight;

    const double avg_frame = static_cast<double>(explained) / static_cast<double>(frames);
    s += 40.0 * std::log10(static_cast<double>(frames));
    if (avg_frame > 512.0) s -= 300.0;      // implausible mean message size

    // Field alignment. A read that straddles a constant byte and the real
    // opcode produces identical statistics to the correct read — same frame
    // count, same distinct values, same everything — so nothing measured from
    // the tiling can separate them. Protocol fields sit on their natural
    // alignment as a matter of course, which breaks the tie honestly.
    //
    // Only meaningful for multi-byte fields: every offset is trivially aligned
    // for a one-byte field, so awarding this unconditionally handed a free
    // bonus to exactly the degenerate specs it was meant to rule out.
    if (spec.opcode_size > 1 && spec.opcode_offset % spec.opcode_size == 0) s += 40.0;
    if (spec.length_size > 1 && spec.length_offset % spec.length_size == 0) s += 40.0;

    // Prefer the simpler of two equivalent descriptions: a length that is
    // plainly the frame size beats one needing an offset to mean the same.
    if (spec.length_adjust == 0) s += 25.0;

    // A header checksum that holds is as close to proof as this gets, so it
    // outweighs every statistical term combined. Require near-unanimity: a
    // partial match is coincidence, a consistent one is the protocol telling
    // us the header layout directly.
    if (checksum_tested >= 20) {
        for (std::size_t w = 0; w < kMaxHeaderWords; ++w) {
            const double agree = static_cast<double>(checksum_hits[w]) /
                                 static_cast<double>(checksum_tested);
            if (agree > 0.95) {
                s += 2000.0;
                // Record it on the spec so the scanner can enforce it from
                // here on: this is the difference between a framing that
                // merely ranks well and one that cannot lose alignment.
                fs.spec.checksum_offset = static_cast<std::uint32_t>(w * 2);
                break;
            }
        }
    }

    // Endianness tie-break. Both readings of a two-byte opcode tile the same
    // stream and yield the same number of distinct values, so tiling alone
    // cannot separate them. Opcodes are small integers in every protocol worth
    // the name — reading the right way round gives 0x0038, the wrong way gives
    // 0x3800 — so a compact value range is real evidence for the reading.
    //
    // Restricted to two-byte opcodes for the same reason as the alignment
    // bonus: a one-byte opcode cannot exceed 255 and would collect this for
    // free.
    if (spec.opcode_size > 1 && !opcodes.empty() && *opcodes.rbegin() < 4096) s += 60.0;

    // Parsimony. Several specs tile an identical stream identically — a
    // header declared larger than it is, with the opcode read from one byte of
    // the real two, consumes the same bytes and scores the same coverage. What
    // separates them is how much of the header the spec actually accounts for.
    // Mild, because real headers genuinely do carry fields we don't model:
    // GhostOnline's own 8-byte header spends 4 bytes on neither length nor
    // opcode, and a harsh penalty here would reject the correct answer.
    const std::uint32_t covered = spec.length_size + spec.opcode_size;
    const std::uint32_t slack = spec.header_size > covered ? spec.header_size - covered : 0;
    s -= static_cast<double>(slack) * 10.0;

    fs.score = s;
    return fs;
}

} // namespace

std::vector<ChecksumLayout> detect_checksum(const std::uint8_t* sample, std::size_t size) {
    std::vector<ChecksumLayout> out;
    if (!sample || size < 1024) return out;

    // Header sizes worth considering, and within each, every u16 slot as the
    // candidate check word.
    for (std::uint32_t header = 8; header <= 16; header += 2) {
        const std::size_t words = header / 2;
        if (size < header) break;

        for (std::size_t slot = 0; slot < words; ++slot) {
            std::size_t hits = 0;
            const std::size_t positions = size - header;

            for (std::size_t p = 0; p < positions; ++p) {
                std::uint64_t sum = 0, check = 0;
                std::size_t nonzero = 0;

                for (std::size_t w = 0; w < words; ++w) {
                    const std::uint64_t v = static_cast<std::uint64_t>(sample[p + w * 2]) |
                                            (static_cast<std::uint64_t>(sample[p + w * 2 + 1]) << 8);
                    if (w == slot) { check = v; continue; }
                    sum += v;
                    if (v != 0) ++nonzero;
                }

                // Skip the degenerate case. Game payloads are largely zeros —
                // this stream is 64% — and 0 + 0 + 0 == 0 holds everywhere in
                // a zero run, which drowns the real signal in hits that mean
                // nothing. A genuine header has a non-zero check word and
                // several non-zero fields behind it.
                if (check == 0 || nonzero < 2) continue;

                if ((sum & 0xFFFF) == check) ++hits;
            }

            const double expected = static_cast<double>(positions) / 65536.0;

            // Demand a wide margin over chance. Real frame headers produce
            // hundreds of hits where chance produces a fraction of one, so
            // this rejects coincidence without excluding anything genuine.
            if (hits > 20 && static_cast<double>(hits) > expected * 20.0) {
                ChecksumLayout c;
                c.header_size = header;
                c.offset      = static_cast<std::uint32_t>(slot * 2);
                c.hits        = hits;
                c.expected    = expected;
                out.push_back(c);
            }
        }
    }

    std::sort(out.begin(), out.end(),
              [](const ChecksumLayout& a, const ChecksumLayout& b) { return a.hits > b.hits; });

    for (const auto& c : out) {
        GB_INFO("Header checksum: header=%u check@%u -- %zu hits vs %.2f expected by chance",
                c.header_size, c.offset, c.hits, c.expected);
    }
    return out;
}

std::vector<FramingScore> detect_framing(const std::uint8_t* sample,
                                         std::size_t size,
                                         std::size_t max_results) {
    std::vector<FramingScore> out;
    if (!sample || size < 64) {
        GB_WARN("detect_framing: need at least 64 bytes, got %zu", size);
        return out;
    }

    // Search space: the layouts real games actually use. Exhaustive over
    // plausible header geometry, not over every bit pattern.
    static constexpr std::uint32_t kHeaderSizes[]   = { 2, 3, 4, 5, 6, 8, 10, 12 };
    static constexpr std::uint32_t kLengthSizes[]   = { 2, 4, 1 };
    static constexpr std::uint32_t kOpcodeSizes[]   = { 2, 1 };
    static constexpr std::int32_t  kAdjusts[]       = { 0, -2, 2, -4, 4 };

    // Find a header check word first, if the protocol has one. Everything
    // below is then verified rather than merely plausible: a candidate frame
    // boundary either satisfies the checksum or is rejected, so the scanner
    // cannot latch onto a stray length value and swallow the real frames
    // behind it.
    const auto checksums = detect_checksum(sample, size);

    std::vector<FramingScore> all;

    for (std::uint32_t header : kHeaderSizes) {
        for (std::uint32_t lsize : kLengthSizes) {
            for (std::uint32_t loff = 0; loff + lsize <= header; ++loff) {
                for (std::uint32_t osize : kOpcodeSizes) {
                    for (std::uint32_t ooff = 0; ooff + osize <= header; ++ooff) {
                        for (bool lle : { true, false }) {
                            for (bool ole : { true, false }) {
                                for (bool inc : { true, false }) {
                                    for (std::int32_t adj : kAdjusts) {
                                        FramingSpec spec;
                                        spec.header_size   = header;
                                        spec.length_offset = loff;
                                        spec.length_size   = lsize;
                                        spec.length_le     = lle;
                                        spec.length_includes_header = inc;
                                        spec.length_adjust = adj;
                                        spec.opcode_offset = ooff;
                                        spec.opcode_size   = osize;
                                        spec.opcode_le     = ole;
                                        spec.min_frame     = header;
                                        // Real play sends far bigger messages
                                        // than an idle client does — map data,
                                        // inventory, player lists. Capping too
                                        // low makes the correct spec fail to
                                        // tile them and hands the win to a
                                        // wrong one that chops them up.
                                        spec.max_frame     = 65535;

                                        if (!spec.plausible()) continue;

                                        // A one-byte opcode field has no
                                        // endianness; skip the duplicate.
                                        if (osize == 1 && !ole) continue;
                                        if (lsize == 1 && !lle) continue;

                                        // Attach a checksum whose header size
                                        // matches this candidate, and keep the
                                        // fields clear of the check word.
                                        for (const auto& c : checksums) {
                                            if (c.header_size != header) continue;
                                            const bool len_clash = loff < c.offset + 2 && c.offset < loff + lsize;
                                            const bool op_clash  = ooff < c.offset + 2 && c.offset < ooff + osize;
                                            if (len_clash || op_clash) continue;
                                            spec.checksum_offset = c.offset;
                                            break;
                                        }

                                        FramingScore fs = score_spec(spec, sample, size);
                                        if (fs.score > 0.0) all.push_back(std::move(fs));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    std::sort(all.begin(), all.end());

    // Collapse near-duplicates: many adjust/endianness pairs describe the same
    // tiling, and showing ten of them helps nobody.
    std::set<std::pair<std::size_t, std::size_t>> seen;
    for (auto& fs : all) {
        const auto sig = std::make_pair(fs.frames_parsed, fs.bytes_explained);
        if (!seen.insert(sig).second) continue;
        out.push_back(fs);
        if (out.size() >= max_results) break;
    }

    GB_INFO("detect_framing: %zu candidates over %zu bytes, %zu distinct tilings",
            all.size(), size, out.size());
    return out;
}

} // namespace gobot
