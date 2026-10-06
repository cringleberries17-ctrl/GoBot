#pragma once

#include "gobot/common.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>

namespace gobot {

/// One end of a TCP conversation.
struct Endpoint {
    std::uint32_t ip   = 0;     ///< host byte order
    std::uint16_t port = 0;

    bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
    std::string to_string() const;
};

/// Direction-agnostic identity for a TCP connection.
struct FlowKey {
    Endpoint a, b;              ///< stored lowest-first so both directions hash alike

    static FlowKey make(Endpoint x, Endpoint y);
    bool operator==(const FlowKey& o) const { return a == o.a && b == o.b; }
    std::string to_string() const;
};

struct FlowKeyHash {
    std::size_t operator()(const FlowKey& k) const noexcept;
};

/// Per-direction byte stream, rebuilt from sequence numbers.
class HalfStream {
public:
    /// Feed one segment. Duplicates and pure retransmits are dropped; segments
    /// arriving ahead of a gap are parked until the hole fills.
    void push(std::uint32_t seq, const std::uint8_t* data, std::size_t len);

    Bytes&       buffer()       { return buffer_; }
    const Bytes& buffer() const { return buffer_; }
    void consume(std::size_t n);

    bool          initialised() const { return initialised_; }
    std::size_t   pending_segments() const { return pending_.size(); }
    std::uint64_t bytes_seen() const { return bytes_seen_; }

    void reset();

private:
    void drain_pending();

    Bytes         buffer_;
    std::uint32_t next_seq_   = 0;
    bool          initialised_ = false;
    std::uint64_t bytes_seen_ = 0;

    /// seq -> segment, waiting on an earlier hole. Capped so a lost segment
    /// can't grow this without bound.
    std::map<std::uint32_t, Bytes> pending_;
    static constexpr std::size_t kMaxPending = 64;
    static constexpr std::size_t kMaxBuffer  = 4u * 1024 * 1024;
};

/// Reassembles TCP payload bytes per connection and per direction.
///
/// Deliberately lightweight: no SACK handling, no window tracking, no timeout
/// eviction beyond an LRU cap. We are an observer of a healthy local
/// connection, not a security appliance.
class TcpReassembler {
public:
    /// Called with a contiguous, in-order run of stream bytes. The callback
    /// returns how many bytes it consumed; the remainder stays buffered for
    /// the next call, which is what lets a frame span segments.
    using StreamCallback = std::function<std::size_t(const FlowKey&,
                                                     Direction,
                                                     const std::uint8_t* data,
                                                     std::size_t size,
                                                     std::uint64_t timestamp_ms)>;

    void set_callback(StreamCallback cb) { callback_ = std::move(cb); }

    /// Endpoints known to be servers, so direction can be labelled exactly.
    ///
    /// Getting direction wrong is quietly destructive: consumers that only
    /// want one side — the recorder wants server->client — discard everything
    /// when the sides are swapped, and it looks like the capture filter is
    /// broken rather than the labelling. Supply these whenever the caller
    /// knows them; a port of 0 matches any port on that address.
    void set_servers(std::vector<Endpoint> servers) { servers_ = std::move(servers); }
    void set_server(Endpoint server) { servers_ = { server }; }

    /// Feed one captured TCP segment. `ack` distinguishes a client's opening
    /// SYN from the server's SYN-ACK reply.
    void on_segment(Endpoint src, Endpoint dst,
                    std::uint32_t seq, bool syn, bool ack, bool fin, bool rst,
                    const std::uint8_t* payload, std::size_t len,
                    std::uint64_t timestamp_ms);

    void close_flow(const FlowKey& key);
    void clear();

    std::size_t flow_count() const { return flows_.size(); }

private:
    struct Flow {
        HalfStream to_server;
        HalfStream to_client;
        Endpoint   client;              ///< first talker
        bool       client_known = false;
        std::uint64_t last_seen_ms = 0;
    };

    void evict_stale(std::uint64_t now);

    /// Decide which of the two endpoints is the client.
    bool identify_client(Flow& flow, Endpoint src, Endpoint dst,
                         bool syn, bool ack, std::size_t len) const;

    std::unordered_map<FlowKey, Flow, FlowKeyHash> flows_;
    StreamCallback        callback_;
    std::vector<Endpoint> servers_;

    static constexpr std::size_t   kMaxFlows = 64;
    static constexpr std::uint64_t kFlowIdleMs = 120'000;
};

} // namespace gobot
