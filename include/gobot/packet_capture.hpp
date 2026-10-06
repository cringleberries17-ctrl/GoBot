#pragma once

#include "gobot/common.hpp"
#include "gobot/framing.hpp"
#include "gobot/tcp_reassembler.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct pcap;            // forward-declared so pcap.h stays out of public headers
typedef struct pcap pcap_t;

namespace gobot {

/// A capture-capable network adapter.
struct Interface {
    std::string name;           ///< \Device\NPF_{GUID} — what pcap_open_live wants
    std::string description;    ///< human-readable
    std::string ipv4;           ///< first IPv4 bound to it, empty if none
    bool        loopback = false;
};

struct CaptureConfig {
    std::string   interface_name;       ///< empty = auto-pick by local IP / default route
    std::string   bpf_filter;           ///< empty = built from the fields below
    std::string   server_ip_start;      ///< inclusive; empty = any
    std::string   server_ip_end;        ///< inclusive; empty = any
    std::uint16_t port_start = 0;       ///< 0 = any
    std::uint16_t port_end   = 0;
    int           snaplen    = 65535;
    int           read_timeout_ms = 100;
    bool          promiscuous = false;  ///< off: we only want this host's traffic

    /// Endpoints known to be servers. Supplying these makes direction
    /// labelling exact instead of inferred — worth doing whenever the caller
    /// knows them, which it does when the connection came from the game's own
    /// socket table.
    std::vector<Endpoint> known_servers;
};

/// Sniffs the game's TCP stream off the wire and hands up decoded frames.
///
/// Read-only by design. Nothing here injects, rewrites or blocks traffic — the
/// bot observes the connection and acts through synthetic keyboard input, the
/// same split the reference client used.
class PacketCapture {
public:
    using PacketCallback    = std::function<void(const std::vector<GamePacket>&)>;
    using DisconnectCallback = std::function<void()>;

    PacketCapture();
    ~PacketCapture();

    PacketCapture(const PacketCapture&) = delete;
    PacketCapture& operator=(const PacketCapture&) = delete;

    /// Enumerate adapters. Needs Npcap installed; returns empty if the driver
    /// is absent or the process lacks Administrator rights.
    static std::vector<Interface> list_interfaces();

    /// Pick the adapter that owns `ip`, or "" if none does.
    static std::string find_interface_by_ip(const std::string& ip);

    /// Adapter carrying the default route — the usual right answer.
    static std::string default_interface();

    /// True if the Npcap service is present and running.
    static bool npcap_available();

    void set_framing(const FramingSpec& spec);
    FramingSpec framing() const;

    /// Framing for what the client sends, when it differs from what the server
    /// sends — which in this game it does. Both directions use an eight-byte
    /// header, but the opcode sits at offset 2 coming down and offset 6 going
    /// up, so one spec decodes half the conversation and throws the other half
    /// away as noise. Unset, both directions use set_framing().
    void set_client_framing(const FramingSpec& spec);

    /// Only these opcodes are forwarded. Empty = forward everything.
    void set_opcode_filter(const std::vector<std::uint16_t>& opcodes);

    void set_packet_callback(PacketCallback cb)       { on_packets_ = std::move(cb); }
    void set_disconnect_callback(DisconnectCallback cb) { on_disconnect_ = std::move(cb); }

    /// Frames are accumulated and delivered in batches this often, which keeps
    /// the callback off the capture thread's critical path.
    void set_batch_interval_ms(std::uint32_t ms) { batch_interval_ms_ = ms; }

    bool start(const CaptureConfig& cfg);
    void stop();
    bool running() const { return running_.load(); }

    /// Hot-swap adapters without tearing down state (e.g. VPN came up).
    bool switch_interface(const std::string& interface_name);

    struct Stats {
        std::uint64_t packets_seen     = 0;
        std::uint64_t tcp_segments     = 0;
        std::uint64_t frames_decoded   = 0;
        std::uint64_t frames_forwarded = 0;
        std::uint64_t resync_bytes     = 0;
        std::uint64_t pcap_dropped     = 0;
        std::uint64_t last_packet_ms   = 0;
    };
    Stats stats() const;

    /// Raw stream tap, for gobot_sniff's --record. Null to detach.
    using RawStreamTap = std::function<void(const FlowKey&, Direction,
                                            const std::uint8_t*, std::size_t)>;
    void set_raw_tap(RawStreamTap tap);

    /// Raw-only mode: skip framing entirely, hand every reassembled byte to
    /// the raw tap exactly once, and consume it.
    ///
    /// This exists because recording must not depend on the framing being
    /// known — discovering the framing is the whole point of recording. In
    /// normal mode the tap is not called: the stream buffer retains partial
    /// frames between calls, so a tap there would re-see bytes it had already
    /// been given and duplicate them into the recording.
    void set_raw_only(bool raw_only) { raw_only_.store(raw_only); }
    bool raw_only() const { return raw_only_.load(); }

private:
    void capture_loop();
    void watchdog_loop();
    void on_raw_packet(const std::uint8_t* data, std::size_t caplen,
                       std::uint64_t timestamp_ms);
    std::size_t on_stream(const FlowKey& key, Direction dir,
                          const std::uint8_t* data, std::size_t size,
                          std::uint64_t timestamp_ms);
    void flush_batch();
    std::string build_filter(const CaptureConfig& cfg) const;
    bool open_device(const std::string& name, const std::string& filter);

    pcap_t*         handle_ = nullptr;
    CaptureConfig   config_;
    TcpReassembler  reassembler_;

    /// Per (flow, direction) frame-alignment state (see extract_frames) and
    /// how many leading bytes of the current buffer the raw tap has already
    /// been given, so it is never handed the same bytes twice.
    std::mutex                                     lock_state_mutex_;
    std::unordered_map<std::uint64_t, bool>        stream_locked_;
    std::unordered_map<std::uint64_t, std::size_t> stream_tapped_;

    mutable std::mutex mutex_;
    FramingSpec        framing_;
    FramingSpec        client_framing_;
    bool               has_client_framing_ = false;
    std::vector<bool>  opcode_allowed_;     ///< indexed by opcode; empty = allow all
    bool               filter_active_ = false;

    std::vector<GamePacket> batch_;
    std::mutex              batch_mutex_;
    std::uint32_t           batch_interval_ms_ = 50;
    std::uint64_t           last_flush_ms_ = 0;

    PacketCallback     on_packets_;
    DisconnectCallback on_disconnect_;
    RawStreamTap       raw_tap_;
    std::mutex         tap_mutex_;
    std::atomic<bool>  raw_only_{false};

    std::thread       capture_thread_;
    std::thread       watchdog_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    mutable std::mutex stats_mutex_;
    Stats              stats_;
};

} // namespace gobot
