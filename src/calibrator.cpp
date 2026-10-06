#include "gobot/calibrator.hpp"
#include "gobot/byte_reader.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <sstream>
#include <thread>

namespace gobot {
namespace {

/// The schedule. Walking phases are repeated so a one-off bump — a monster
/// knocking the character back, a slope — cannot pass for a correlation.
struct Step {
    CalibPhase phase;
    std::uint32_t ms;
};

const Step kSchedule[] = {
    { CalibPhase::Idle,  1500 },
    { CalibPhase::Right, 2500 },
    { CalibPhase::Left,  2500 },
    { CalibPhase::Right, 2500 },
    { CalibPhase::Left,  2500 },
    { CalibPhase::Idle,  1200 },
    { CalibPhase::Jump,  3000 },
    { CalibPhase::Idle,  1200 },
    { CalibPhase::Cast,  3000 },
    { CalibPhase::Idle,  1000 },
};

double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

bool read_field(const Bytes& b, std::size_t off, FieldType t, double& out) {
    ByteReader r(b.data(), b.size());
    r.seek(off);
    switch (t) {
        case FieldType::I16: out = r.i16(); break;
        case FieldType::U16: out = r.u16(); break;
        case FieldType::I32: out = r.i32(); break;
        case FieldType::F32: out = r.f32(); break;
        default: return false;
    }
    return r.ok();
}

} // namespace

const char* to_string(CalibPhase p) {
    switch (p) {
        case CalibPhase::Idle:   return "standing still";
        case CalibPhase::Right:  return "walking right";
        case CalibPhase::Left:   return "walking left";
        case CalibPhase::Jump:   return "jumping";
        case CalibPhase::Cast:   return "casting";
        case CalibPhase::Done:   return "done";
        case CalibPhase::Failed: return "failed";
    }
    return "?";
}

ProtocolCalibrator::ProtocolCalibrator(InputManager& input) : input_(input) {}

ProtocolCalibrator::~ProtocolCalibrator() { stop(); }

void ProtocolCalibrator::set_phase(CalibPhase p) {
    phase_.store(p);
    GB_INFO("Calibration: %s", to_string(p));
}

bool ProtocolCalibrator::start() {
    if (running_.load()) return false;

    if (!input_.window_valid()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "No game window selected.";
        return false;
    }
    if (!input_.window_focused()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = "Click the game window first -- calibration drives the character, "
                  "and input only reaches a focused window.";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.clear();
        status_ = "Running.";
    }

    std::uint32_t total = 0;
    for (const auto& s : kSchedule) total += s.ms;
    total_ms_.store(total);
    started_ms_.store(now_ms());

    running_.store(true);
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&ProtocolCalibrator::run, this);
    return true;
}

void ProtocolCalibrator::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    input_.release_all();
}

void ProtocolCalibrator::run() {
    for (const auto& step : kSchedule) {
        if (!running_.load()) break;
        set_phase(step.phase);

        const std::uint64_t until = now_ms() + step.ms;
        bool jumped_recently = false;

        while (running_.load() && now_ms() < until) {
            // Losing focus mid-run invalidates everything after it, because
            // the character stops responding while packets keep arriving.
            if (!input_.window_focused()) {
                std::lock_guard<std::mutex> lock(mutex_);
                status_ = "Game window lost focus -- calibration abandoned.";
                input_.release_all();
                set_phase(CalibPhase::Failed);
                running_.store(false);
                return;
            }

            switch (step.phase) {
                case CalibPhase::Right: input_.move_right(); break;
                case CalibPhase::Left:  input_.move_left();  break;

                case CalibPhase::Jump:
                    // Space the jumps out so each arc is separable, rather
                    // than merging into one long airborne stretch.
                    if (!jumped_recently) {
                        input_.jump();
                        jumped_recently = true;
                    }
                    break;

                case CalibPhase::Cast:
                    input_.use_skill(cast_key_);
                    std::this_thread::sleep_for(std::chrono::milliseconds(700));
                    continue;

                case CalibPhase::Idle:
                default:
                    input_.stop_move();
                    break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (step.phase == CalibPhase::Jump) {
                static int tick = 0;
                if (++tick % 9 == 0) jumped_recently = false;
            }
        }

        input_.stop_move();
    }

    input_.release_all();
    set_phase(CalibPhase::Done);
    running_.store(false);

    std::lock_guard<std::mutex> lock(mutex_);
    status_ = "Finished with " + std::to_string(samples_.size()) + " packets.";
}

void ProtocolCalibrator::on_packets(const std::vector<GamePacket>& packets) {
    const CalibPhase p = phase_.load();
    if (p == CalibPhase::Done || p == CalibPhase::Failed) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_.size() >= kMaxSamples) return;

    for (const auto& pkt : packets) {
        CalibSample s;
        s.opcode = pkt.opcode;
        s.payload = pkt.payload;
        s.timestamp_ms = pkt.timestamp_ms ? pkt.timestamp_ms : now_ms();
        s.phase = p;
        s.from_server = pkt.from_server;
        samples_.push_back(std::move(s));
        if (samples_.size() >= kMaxSamples) break;
    }
}

float ProtocolCalibrator::progress() const {
    const std::uint64_t total = total_ms_.load();
    if (total == 0) return 0.0f;
    const CalibPhase p = phase_.load();
    if (p == CalibPhase::Done) return 1.0f;
    const std::uint64_t elapsed = now_ms() - started_ms_.load();
    return std::min(1.0f, static_cast<float>(elapsed) / static_cast<float>(total));
}

std::string ProtocolCalibrator::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::size_t ProtocolCalibrator::sample_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.size();
}

// --------------------------------------------------------------- analysis

std::vector<FieldFinding> ProtocolCalibrator::analyse() const {
    std::vector<CalibSample> samples;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        samples = samples_;
    }

    std::vector<FieldFinding> out;
    if (samples.size() < 50) {
        GB_WARN("Calibration: only %zu packets, not enough to conclude anything",
                samples.size());
        return out;
    }

    // Group by opcode, keeping arrival order so deltas mean something.
    std::map<std::uint16_t, std::vector<const CalibSample*>> by_opcode;
    for (const auto& s : samples) by_opcode[s.opcode].push_back(&s);

    for (const auto& [opcode, list] : by_opcode) {
        if (list.size() < 30) continue;

        std::size_t len = list.front()->payload.size();
        for (const auto* s : list) len = std::min(len, s->payload.size());
        if (len < 4) continue;

        for (std::size_t off = 0; off + 2 <= len; ++off) {
            for (FieldType t : { FieldType::F32, FieldType::I32, FieldType::I16 }) {
                const std::size_t w = (t == FieldType::I16) ? 2 : 4;
                if (off + w > len) continue;

                // Deltas between consecutive packets, bucketed by what the
                // character was being made to do at the time.
                std::vector<double> d_right, d_left, d_idle, d_jump, d_cast;
                double prev = 0;
                bool have_prev = false;
                CalibPhase prev_phase = CalibPhase::Idle;

                bool readable = true;
                for (const auto* s : list) {
                    double v = 0;
                    if (!read_field(s->payload, off, t, v)) { readable = false; break; }
                    if (t == FieldType::F32 && !std::isfinite(v)) { readable = false; break; }

                    if (have_prev && s->phase == prev_phase) {
                        const double d = v - prev;
                        switch (s->phase) {
                            case CalibPhase::Right: d_right.push_back(d); break;
                            case CalibPhase::Left:  d_left.push_back(d);  break;
                            case CalibPhase::Idle:  d_idle.push_back(d);  break;
                            case CalibPhase::Jump:  d_jump.push_back(d);  break;
                            case CalibPhase::Cast:  d_cast.push_back(d);  break;
                            default: break;
                        }
                    }
                    prev = v;
                    prev_phase = s->phase;
                    have_prev = true;
                }
                if (!readable) continue;

                // ---- x: moves one way on Right, the other on Left ----
                if (d_right.size() >= 8 && d_left.size() >= 8) {
                    const double mr = mean(d_right);
                    const double ml = mean(d_left);
                    const double mi = mean(d_idle);

                    // Opposite signs, both clearly non-zero, and still while
                    // idle. A counter climbs regardless of which key is held;
                    // a coordinate does not.
                    const bool opposed = (mr > 0 && ml < 0) || (mr < 0 && ml > 0);
                    const double swing = std::fabs(mr - ml);
                    const double drift = std::fabs(mi);

                    if (opposed && swing > 0.5 && drift < swing * 0.2) {
                        FieldFinding f;
                        f.opcode = opcode;
                        f.offset = off;
                        f.width = w;
                        f.type = t;
                        f.role = "x";
                        f.confidence = std::min(0.97, 0.6 + std::min(0.3, swing / 40.0));

                        std::ostringstream e;
                        e << "mean change per packet: walking right " << mr
                          << ", walking left " << ml << ", standing still " << mi
                          << " (" << d_right.size() << "/" << d_left.size()
                          << " samples)";
                        f.evidence = e.str();
                        out.push_back(std::move(f));
                        continue;
                    }
                }

                // ---- y: still while walking, moves while jumping ----
                if (d_jump.size() >= 8 && (d_right.size() + d_idle.size()) >= 8) {
                    std::vector<double> flat = d_idle;
                    flat.insert(flat.end(), d_right.begin(), d_right.end());

                    double jump_var = 0, flat_var = 0;
                    for (double v : d_jump) jump_var += std::fabs(v);
                    for (double v : flat)   flat_var += std::fabs(v);
                    jump_var /= std::max<std::size_t>(1, d_jump.size());
                    flat_var /= std::max<std::size_t>(1, flat.size());

                    if (jump_var > 1.0 && jump_var > flat_var * 4.0) {
                        FieldFinding f;
                        f.opcode = opcode;
                        f.offset = off;
                        f.width = w;
                        f.type = t;
                        f.role = "y";
                        f.confidence = std::min(0.9, 0.55 + std::min(0.3, jump_var / 60.0));

                        std::ostringstream e;
                        e << "mean absolute change: jumping " << jump_var
                          << ", on the ground " << flat_var
                          << " (" << d_jump.size() << " jump samples)";
                        f.evidence = e.str();
                        out.push_back(std::move(f));
                        continue;
                    }
                }

                // ---- mp: drops while casting, steady otherwise ----
                if (d_cast.size() >= 5 && t != FieldType::F32) {
                    const double mc = mean(d_cast);
                    std::vector<double> other = d_idle;
                    other.insert(other.end(), d_right.begin(), d_right.end());
                    other.insert(other.end(), d_left.begin(), d_left.end());
                    const double mo = mean(other);

                    if (mc < -1.0 && std::fabs(mo) < std::fabs(mc) * 0.3) {
                        FieldFinding f;
                        f.opcode = opcode;
                        f.offset = off;
                        f.width = w;
                        f.type = t;
                        f.role = "mp";
                        f.confidence = 0.75;

                        std::ostringstream e;
                        e << "mean change per packet: casting " << mc
                          << ", otherwise " << mo
                          << " (" << d_cast.size() << " casting samples)";
                        f.evidence = e.str();
                        out.push_back(std::move(f));
                    }
                }
            }
        }
    }

    // Prefer the strongest claim per (opcode, role), and drop overlapping
    // offsets so one field is not reported three ways.
    std::sort(out.begin(), out.end(), [](const FieldFinding& a, const FieldFinding& b) {
        return a.confidence > b.confidence;
    });

    std::vector<FieldFinding> kept;
    for (const auto& f : out) {
        bool clash = false;
        for (const auto& k : kept) {
            if (k.opcode != f.opcode) continue;
            const bool overlap = f.offset < k.offset + k.width && k.offset < f.offset + f.width;
            if (overlap || (k.role == f.role && k.opcode == f.opcode)) { clash = true; break; }
        }
        if (!clash) kept.push_back(f);
    }

    GB_INFO("Calibration: %zu field(s) identified from %zu packets",
            kept.size(), samples.size());
    for (const auto& f : kept)
        GB_INFO("  0x%04X +%zu as %s -> %s (%.0f%%)", f.opcode, f.offset,
                f.role.c_str(), f.evidence.c_str(), f.confidence * 100.0);
    return kept;
}

std::vector<PacketSpec> ProtocolCalibrator::to_specs(
    const std::vector<FieldFinding>& findings) {

    std::map<std::uint16_t, std::vector<const FieldFinding*>> by_opcode;
    for (const auto& f : findings) by_opcode[f.opcode].push_back(&f);

    std::vector<PacketSpec> out;
    for (const auto& [opcode, fields] : by_opcode) {
        PacketSpec spec;
        spec.opcode = opcode;

        bool has_xy = false, has_mp = false;
        for (const auto* f : fields) {
            if (f->role == "x" || f->role == "y") has_xy = true;
            if (f->role == "mp" || f->role == "hp") has_mp = true;
        }

        // Driving our own character means anything that responded is about
        // us, so these are self packets rather than entity ones.
        if (has_xy)      { spec.semantic = Semantic::SelfMove;  spec.name = "PlayerMove"; }
        else if (has_mp) { spec.semantic = Semantic::SelfHpMp;  spec.name = "Vitals"; }
        else continue;

        std::vector<const FieldFinding*> sorted = fields;
        std::sort(sorted.begin(), sorted.end(),
                  [](const FieldFinding* a, const FieldFinding* b) {
                      return a->offset < b->offset;
                  });

        for (const auto* f : sorted) {
            FieldSpec fs;
            fs.name = f->role;
            fs.type = f->type;
            fs.has_fixed_offset = true;
            fs.fixed_offset = f->offset;
            spec.fields.push_back(std::move(fs));
        }
        out.push_back(std::move(spec));
    }
    return out;
}

} // namespace gobot
