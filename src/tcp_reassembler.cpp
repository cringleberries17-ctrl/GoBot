#include "gobot/tcp_reassembler.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <tuple>

namespace gobot {

std::string Endpoint::to_string() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u",
                  (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF,
                  static_cast<unsigned>(port));
    return buf;
}

FlowKey FlowKey::make(Endpoint x, Endpoint y) {
    FlowKey k;
    // Canonical ordering so both directions map to one flow.
    if (std::tie(x.ip, x.port) <= std::tie(y.ip, y.port)) {
        k.a = x; k.b = y;
    } else {
        k.a = y; k.b = x;
    }
    return k;
}

std::string FlowKey::to_string() const {
    return a.to_string() + " <-> " + b.to_string();
}

std::size_t FlowKeyHash::operator()(const FlowKey& k) const noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ULL;
    };
    mix(k.a.ip);
    mix(k.a.port);
    mix(k.b.ip);
    mix(k.b.port);
    return static_cast<std::size_t>(h ^ (h >> 32));
}

// ---------------------------------------------------------------- HalfStream

void HalfStream::push(std::uint32_t seq, const std::uint8_t* data, std::size_t len) {
    if (len == 0) return;

    if (!initialised_) {
        // First payload byte we see defines the origin. We may have joined an
        // established connection mid-flight; that is fine, the frame scanner
        // resyncs.
        next_seq_ = seq;
        initialised_ = true;
    }

    // Signed difference handles sequence-number wraparound correctly.
    const std::int32_t delta = static_cast<std::int32_t>(seq - next_seq_);

    if (delta == 0) {
        buffer_.insert(buffer_.end(), data, data + len);
        next_seq_ += static_cast<std::uint32_t>(len);
        bytes_seen_ += len;
        drain_pending();
    } else if (delta < 0) {
        // Retransmit or overlap. Keep only the part past next_seq_.
        const std::size_t overlap = static_cast<std::size_t>(-delta);
        if (overlap < len) {
            const std::size_t fresh = len - overlap;
            buffer_.insert(buffer_.end(), data + overlap, data + len);
            next_seq_ += static_cast<std::uint32_t>(fresh);
            bytes_seen_ += fresh;
            drain_pending();
        }
        // else: pure duplicate, drop.
    } else {
        // Ahead of a hole. Park it; a retransmit of the missing run should
        // arrive shortly.
        if (pending_.size() < kMaxPending) {
            pending_.emplace(seq, Bytes(data, data + len));
        } else {
            // The hole never filled. Give up on it and jump the stream
            // forward — a permanently stalled half-stream is worse than a
            // resync, which the frame scanner handles.
            GB_WARN("HalfStream: pending overflow, skipping gap of %d bytes", delta);
            auto first = pending_.begin();
            next_seq_ = first->first;
            buffer_.insert(buffer_.end(), first->second.begin(), first->second.end());
            next_seq_ += static_cast<std::uint32_t>(first->second.size());
            pending_.erase(first);
            drain_pending();
        }
    }

    if (buffer_.size() > kMaxBuffer) {
        // Nothing upstream is consuming; most likely the FramingSpec is wrong
        // and every byte is a resync. Drop the oldest half.
        GB_WARN("HalfStream: buffer over %zu bytes, discarding oldest half", kMaxBuffer);
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_.size() / 2));
    }
}

void HalfStream::drain_pending() {
    bool progressed = true;
    while (progressed && !pending_.empty()) {
        progressed = false;
        auto it = pending_.begin();
        const std::int32_t delta = static_cast<std::int32_t>(it->first - next_seq_);

        if (delta <= 0) {
            const std::size_t overlap = static_cast<std::size_t>(-delta);
            if (overlap < it->second.size()) {
                buffer_.insert(buffer_.end(), it->second.begin() + static_cast<std::ptrdiff_t>(overlap), it->second.end());
                const std::size_t fresh = it->second.size() - overlap;
                next_seq_ += static_cast<std::uint32_t>(fresh);
                bytes_seen_ += fresh;
            }
            pending_.erase(it);
            progressed = true;
        }
    }
}

void HalfStream::consume(std::size_t n) {
    if (n == 0) return;
    if (n >= buffer_.size()) {
        buffer_.clear();
    } else {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(n));
    }
}

void HalfStream::reset() {
    buffer_.clear();
    pending_.clear();
    next_seq_ = 0;
    initialised_ = false;
    bytes_seen_ = 0;
}

// ------------------------------------------------------------ TcpReassembler

bool TcpReassembler::identify_client(Flow& flow, Endpoint src, Endpoint dst,
                                     bool syn, bool ack, std::size_t len) const {
    // 1. A known server endpoint settles it outright.
    for (const auto& s : servers_) {
        if (s.ip == 0) continue;
        const bool src_is_server = (src.ip == s.ip && (s.port == 0 || src.port == s.port));
        const bool dst_is_server = (dst.ip == s.ip && (s.port == 0 || dst.port == s.port));
        if (src_is_server) { flow.client = dst; return true; }
        if (dst_is_server) { flow.client = src; return true; }
    }

    // 2. Caught the handshake: a bare SYN is the client opening, SYN-ACK is
    //    the server answering.
    if (syn) {
        flow.client = ack ? dst : src;
        return true;
    }

    // 3. Joined mid-connection, which is the normal case when the bot starts
    //    after the game is already logged in. Ports decide it: the client
    //    binds an ephemeral high port, the server listens on a fixed lower
    //    one. "Whoever we happened to hear first" is a coin flip, and getting
    //    it wrong silently drops the half of the traffic we actually want.
    constexpr std::uint16_t kEphemeralFloor = 32768;
    const bool src_ephemeral = src.port >= kEphemeralFloor;
    const bool dst_ephemeral = dst.port >= kEphemeralFloor;

    if (src_ephemeral != dst_ephemeral) {
        flow.client = src_ephemeral ? src : dst;
        return true;
    }
    if (src.port != dst.port) {
        flow.client = (src.port > dst.port) ? src : dst;
        return true;
    }

    // 4. Nothing to go on. First talker, as before.
    if (len > 0) {
        flow.client = src;
        return true;
    }
    return false;
}

void TcpReassembler::on_segment(Endpoint src, Endpoint dst,
                                std::uint32_t seq, bool syn, bool ack, bool fin, bool rst,
                                const std::uint8_t* payload, std::size_t len,
                                std::uint64_t timestamp_ms) {
    const FlowKey key = FlowKey::make(src, dst);

    if (rst) {
        close_flow(key);
        return;
    }

    auto it = flows_.find(key);
    if (it == flows_.end()) {
        if (flows_.size() >= kMaxFlows) evict_stale(timestamp_ms);
        if (flows_.size() >= kMaxFlows) {
            GB_WARN("TcpReassembler: flow table full, ignoring %s", key.to_string().c_str());
            return;
        }
        it = flows_.emplace(key, Flow{}).first;
    }

    Flow& flow = it->second;
    flow.last_seen_ms = timestamp_ms;

    if (!flow.client_known)
        flow.client_known = identify_client(flow, src, dst, syn, ack, len);

    if (syn) {
        // TCP counts SYN as one sequence number; payload starts at seq+1.
        seq += 1;
    }

    if (len == 0) {
        if (fin) close_flow(key);
        return;
    }

    const bool to_server = flow.client_known && src.ip == flow.client.ip && src.port == flow.client.port;
    const Direction dir  = to_server ? Direction::ToServer : Direction::ToClient;
    HalfStream& half     = to_server ? flow.to_server : flow.to_client;

    half.push(seq, payload, len);

    if (callback_ && !half.buffer().empty()) {
        const std::size_t consumed =
            callback_(key, dir, half.buffer().data(), half.buffer().size(), timestamp_ms);
        half.consume(std::min(consumed, half.buffer().size()));
    }

    if (fin) close_flow(key);
}

void TcpReassembler::close_flow(const FlowKey& key) {
    flows_.erase(key);
}

void TcpReassembler::clear() {
    flows_.clear();
}

void TcpReassembler::evict_stale(std::uint64_t now) {
    for (auto it = flows_.begin(); it != flows_.end();) {
        if (now > it->second.last_seen_ms + kFlowIdleMs) {
            it = flows_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace gobot
