#pragma once

#include "gobot/common.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/opcode_registry.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gobot {

/// What the character is being made to do, so field changes can be attributed.
enum class CalibPhase {
    Idle,       ///< baseline: nothing should move
    Right,      ///< walking right: x increases
    Left,       ///< walking left: x decreases
    Jump,       ///< y departs from its resting value and returns
    Cast,       ///< a skill is used: mp drops
    Done,
    Failed,
};

const char* to_string(CalibPhase p);

/// One captured packet, tagged with what was happening at the time.
struct CalibSample {
    std::uint16_t opcode = 0;
    Bytes         payload;
    std::uint64_t timestamp_ms = 0;
    CalibPhase    phase = CalibPhase::Idle;
    bool          from_server = true;
};

/// A field the calibration identified, and why.
struct FieldFinding {
    std::uint16_t opcode = 0;
    std::size_t   offset = 0;
    std::size_t   width = 4;
    FieldType     type = FieldType::I32;
    std::string   role;          ///< "x", "y", "mp", "hp"
    double        confidence = 0.0;
    std::string   evidence;      ///< the numbers behind the claim
};

/// Identifies packet fields by driving the character and watching what moves.
///
/// Every earlier attempt to map this protocol statistically failed the same
/// way: the capture happened to catch a character standing still, and nothing
/// in it distinguished a coordinate from a counter. Correlation against known
/// input removes the guesswork — if a field climbs every time we hold Right
/// and falls every time we hold Left, it is the x coordinate, and no amount of
/// coincidence explains it.
///
/// The character must be somewhere it can walk a few steps in both directions
/// without falling or being interrupted.
class ProtocolCalibrator {
public:
    ProtocolCalibrator(InputManager& input);
    ~ProtocolCalibrator();

    ProtocolCalibrator(const ProtocolCalibrator&) = delete;
    ProtocolCalibrator& operator=(const ProtocolCalibrator&) = delete;

    /// Key used for the Cast phase. Should be a skill that costs mana.
    void set_cast_key(std::uint32_t vk) { cast_key_ = vk; }

    /// Begin. Returns false if the game window is not focused, since driving
    /// input into the wrong window is both useless and rude.
    bool start();
    void stop();
    bool running() const { return running_.load(); }

    /// Feed packets in from the capture callback for the whole run.
    void on_packets(const std::vector<GamePacket>& packets);

    CalibPhase phase() const { return phase_.load(); }
    float      progress() const;
    std::string status() const;

    std::size_t sample_count() const;

    /// Correlate and report. Call once the run reaches Done.
    std::vector<FieldFinding> analyse() const;

    /// Turn findings into packet layouts ready to register.
    static std::vector<PacketSpec> to_specs(const std::vector<FieldFinding>& findings);

private:
    void run();
    void set_phase(CalibPhase p);

    InputManager&     input_;
    std::uint32_t     cast_key_ = '1';

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<CalibPhase> phase_{CalibPhase::Idle};
    std::atomic<std::uint64_t> started_ms_{0};
    std::atomic<std::uint64_t> total_ms_{0};

    mutable std::mutex       mutex_;
    std::vector<CalibSample> samples_;
    std::string              status_;

    /// Cap the recording: a long run on a busy map would otherwise accumulate
    /// more than the analysis needs.
    static constexpr std::size_t kMaxSamples = 40000;
};

} // namespace gobot
