#pragma once

#include "gobot/common.hpp"
#include "gobot/opcode_registry.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace gobot {

/// Observations for one opcode, gathered while capturing.
struct OpcodeObservations {
    std::vector<Bytes>         payloads;
    std::uint64_t              to_server = 0;
    std::uint64_t              to_client = 0;
    std::vector<std::uint64_t> timestamps_ms;   ///< parallel to payloads

    std::uint64_t count() const { return to_server + to_client; }
};

/// One proposed field inside a packet.
struct FieldGuess {
    std::size_t offset = 0;
    std::size_t width = 0;
    FieldType   type = FieldType::I32;
    std::string role;           ///< "id", "x", "y", "hp", "max_hp", "kind"
    double      confidence = 0.0;
};

/// A proposed meaning for an opcode, with a layout ready to apply.
struct SemanticGuess {
    std::uint16_t          opcode = 0;
    Semantic               semantic = Semantic::Unknown;
    double                 confidence = 0.0;   ///< 0..1
    std::string            reason;             ///< why, in plain words
    std::vector<FieldSpec> fields;             ///< proposed layout
    std::string            suggested_name;
};

/// Infer what opcodes mean from how their bytes behave.
///
/// Reading a protocol by hand means staring at hex until patterns appear. Most
/// of that is mechanical and can be done by the machine:
///
///   * Direction is the strongest signal available. Client->server packets
///     describe only *you*, so a client-sent packet carrying two smoothly
///     changing numbers is your position, with very little ambiguity.
///   * Coordinates move in small steps between consecutive packets, and stay
///     inside a bounded range. Random bytes do neither.
///   * Floats are self-identifying: interpreting arbitrary bytes as f32
///     overwhelmingly yields denormals or astronomical values, so a field that
///     reads as sane floats almost certainly is one.
///   * Entity ids take many distinct values across a capture but repeat within
///     it, which is a different fingerprint from both counters and coordinates.
///
/// None of these are proof. Every guess carries a confidence and a stated
/// reason so it can be judged rather than trusted, and the caller confirms
/// before anything is applied.
std::vector<SemanticGuess> analyze_protocol(
    const std::map<std::uint16_t, OpcodeObservations>& data);

/// Human-readable summary of what one opcode's bytes look like.
std::string describe_opcode(std::uint16_t opcode, const OpcodeObservations& obs);

} // namespace gobot
