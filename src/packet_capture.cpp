#include "gobot/packet_capture.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include <pcap.h>

namespace gobot {
namespace {

constexpr std::size_t kEthernetHeaderLen = 14;
constexpr std::uint16_t kEtherTypeIPv4   = 0x0800;
constexpr std::uint16_t kEtherTypeVlan   = 0x8100;
constexpr std::uint8_t  kProtoTcp        = 6;

std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |
            static_cast<std::uint32_t>(p[3]);
}

std::string ip_to_string(std::uint32_t ip) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return buf;
}

bool valid_ipv4(const std::string& s) {
    in_addr addr{};
    return !s.empty() && inet_pton(AF_INET, s.c_str(), &addr) == 1;
}

} // namespace

PacketCapture::PacketCapture() = default;

PacketCapture::~PacketCapture() {
    stop();
}

// ------------------------------------------------------------- enumeration

bool PacketCapture::npcap_available() {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;

    bool running = false;
    if (SC_HANDLE svc = OpenServiceA(scm, "npcap", SERVICE_QUERY_STATUS)) {
        SERVICE_STATUS status{};
        if (QueryServiceStatus(svc, &status))
            running = status.dwCurrentState == SERVICE_RUNNING;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return running;
}

std::vector<Interface> PacketCapture::list_interfaces() {
    std::vector<Interface> out;

    // wpcap.dll is delay-loaded, so touching any pcap function without Npcap
    // present would fault. The service check reaches the SCM rather than the
    // library, making it safe to ask first.
    if (!npcap_available()) {
        GB_WARN("Npcap is not installed or not running; no capture interfaces");
        return out;
    }

    pcap_if_t* devices = nullptr;
    char errbuf[PCAP_ERRBUF_SIZE]{};

    if (pcap_findalldevs(&devices, errbuf) == -1) {
        GB_ERROR("pcap_findalldevs failed: %s", errbuf);
        GB_ERROR("Npcap installed? Running as Administrator?");
        return out;
    }

    for (pcap_if_t* d = devices; d; d = d->next) {
        Interface iface;
        iface.name        = d->name ? d->name : "";
        iface.description = d->description ? d->description : "";
        iface.loopback    = (d->flags & PCAP_IF_LOOPBACK) != 0;

        for (pcap_addr_t* a = d->addresses; a; a = a->next) {
            if (a->addr && a->addr->sa_family == AF_INET) {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(a->addr);
                char buf[INET_ADDRSTRLEN]{};
                inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
                iface.ipv4 = buf;
                break;
            }
        }
        out.push_back(std::move(iface));
    }

    pcap_freealldevs(devices);
    return out;
}

std::string PacketCapture::find_interface_by_ip(const std::string& ip) {
    for (const auto& i : list_interfaces()) {
        if (!i.ipv4.empty() && i.ipv4 == ip) return i.name;
    }
    return {};
}

std::string PacketCapture::default_interface() {
    // Ask Windows which adapter carries the default route, then match its IP
    // against the pcap device list. Guessing "the first non-loopback adapter"
    // picks a VM bridge or a disconnected Wi-Fi card often enough to matter.
    std::string best_ip;

    ULONG size = 0;
    if (GetAdaptersInfo(nullptr, &size) == ERROR_BUFFER_OVERFLOW && size > 0) {
        std::vector<std::uint8_t> buf(size);
        auto* info = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data());

        if (GetAdaptersInfo(info, &size) == NO_ERROR) {
            for (IP_ADAPTER_INFO* a = info; a; a = a->Next) {
                const std::string gw = a->GatewayList.IpAddress.String;
                const std::string ip = a->IpAddressList.IpAddress.String;
                if (gw.empty() || gw == "0.0.0.0") continue;
                if (ip.empty() || ip == "0.0.0.0") continue;
                best_ip = ip;
                break;
            }
        }
    }

    if (!best_ip.empty()) {
        if (std::string name = find_interface_by_ip(best_ip); !name.empty()) {
            GB_INFO("Default interface: %s (%s)", name.c_str(), best_ip.c_str());
            return name;
        }
    }

    for (const auto& i : list_interfaces()) {
        if (!i.loopback && !i.ipv4.empty()) {
            GB_WARN("No default-route match; falling back to %s (%s)",
                    i.name.c_str(), i.ipv4.c_str());
            return i.name;
        }
    }
    return {};
}

// ------------------------------------------------------------ configuration

void PacketCapture::set_framing(const FramingSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    framing_ = spec;
}

FramingSpec PacketCapture::framing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return framing_;
}

void PacketCapture::set_client_framing(const FramingSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    client_framing_ = spec;
    has_client_framing_ = spec.plausible();
}

void PacketCapture::set_opcode_filter(const std::vector<std::uint16_t>& opcodes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (opcodes.empty()) {
        opcode_allowed_.clear();
        filter_active_ = false;
        GB_INFO("Opcode filter cleared (forwarding all)");
        return;
    }
    opcode_allowed_.assign(65536, false);
    for (std::uint16_t op : opcodes) opcode_allowed_[op] = true;
    filter_active_ = true;
    GB_INFO("Opcode filter: %zu opcodes allowed", opcodes.size());
}

void PacketCapture::set_raw_tap(RawStreamTap tap) {
    std::lock_guard<std::mutex> lock(tap_mutex_);
    raw_tap_ = std::move(tap);
}

std::string PacketCapture::build_filter(const CaptureConfig& cfg) const {
    if (!cfg.bpf_filter.empty()) return cfg.bpf_filter;

    std::ostringstream f;
    f << "tcp";

    const bool have_start = valid_ipv4(cfg.server_ip_start);
    const bool have_end   = valid_ipv4(cfg.server_ip_end);

    if (have_start && have_end && cfg.server_ip_start != cfg.server_ip_end) {
        // BPF has no range primitive; the usual trick is a /N net when the
        // range is aligned. Rather than guess, filter loosely on the pair and
        // let the reassembler sort it out.
        f << " and (net " << cfg.server_ip_start << " or net " << cfg.server_ip_end << ")";
    } else if (have_start) {
        f << " and host " << cfg.server_ip_start;
    }

    if (cfg.port_start != 0) {
        if (cfg.port_end != 0 && cfg.port_end != cfg.port_start) {
            f << " and portrange " << cfg.port_start << "-" << cfg.port_end;
        } else {
            f << " and port " << cfg.port_start;
        }
    }
    return f.str();
}

// -------------------------------------------------------------- lifecycle

bool PacketCapture::open_device(const std::string& name, const std::string& filter) {
    char errbuf[PCAP_ERRBUF_SIZE]{};

    pcap_t* h = pcap_open_live(name.c_str(),
                               config_.snaplen,
                               config_.promiscuous ? 1 : 0,
                               config_.read_timeout_ms,
                               errbuf);
    if (!h) {
        GB_ERROR("pcap_open_live(%s) failed: %s", name.c_str(), errbuf);
        return false;
    }

    if (pcap_datalink(h) != DLT_EN10MB) {
        GB_ERROR("Interface %s is not Ethernet (datalink %d); unsupported",
                 name.c_str(), pcap_datalink(h));
        pcap_close(h);
        return false;
    }

    if (!filter.empty()) {
        bpf_program prog{};
        if (pcap_compile(h, &prog, filter.c_str(), 1, PCAP_NETMASK_UNKNOWN) == -1) {
            GB_ERROR("Failed to compile BPF '%s': %s", filter.c_str(), pcap_geterr(h));
            pcap_close(h);
            return false;
        }
        if (pcap_setfilter(h, &prog) == -1) {
            GB_ERROR("Failed to set BPF filter: %s", pcap_geterr(h));
            pcap_freecode(&prog);
            pcap_close(h);
            return false;
        }
        pcap_freecode(&prog);
        GB_INFO("BPF filter: %s", filter.c_str());
    }

    handle_ = h;
    return true;
}

bool PacketCapture::start(const CaptureConfig& cfg) {
    if (running_.load()) {
        GB_WARN("PacketCapture already running");
        return false;
    }

    if (!npcap_available()) {
        GB_ERROR("Npcap service is not running. Install Npcap and start the service.");
        return false;
    }

    config_ = cfg;

    std::string iface = cfg.interface_name;
    if (iface.empty()) {
        if (!cfg.server_ip_start.empty()) iface = find_interface_by_ip(cfg.server_ip_start);
        if (iface.empty()) iface = default_interface();
    }
    if (iface.empty()) {
        GB_ERROR("No capture interface found. Run with --list to see options.");
        return false;
    }

    if (!open_device(iface, build_filter(cfg))) return false;

    // Tell the reassembler which side is the server whenever we know. Without
    // it, direction on an already-established connection is inferred, and a
    // wrong guess silently drops the half of the traffic callers want.
    std::vector<Endpoint> servers = cfg.known_servers;

    if (servers.empty() && valid_ipv4(cfg.server_ip_start) &&
        cfg.server_ip_start == cfg.server_ip_end) {
        in_addr a{};
        inet_pton(AF_INET, cfg.server_ip_start.c_str(), &a);
        Endpoint server;
        server.ip   = ntohl(a.S_un.S_addr);
        server.port = cfg.port_start;
        servers.push_back(server);
    }

    if (!servers.empty()) {
        for (const auto& s : servers)
            GB_INFO("Known server endpoint: %s", s.to_string().c_str());
    }
    reassembler_.set_servers(std::move(servers));

    reassembler_.set_callback([this](const FlowKey& key, Direction dir,
                                     const std::uint8_t* data, std::size_t size,
                                     std::uint64_t ts) {
        return on_stream(key, dir, data, size, ts);
    });

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_ = Stats{};
    }
    {
        // Offsets from a previous session refer to buffers that no longer
        // exist; carrying them over would skip real bytes.
        std::lock_guard<std::mutex> lock(lock_state_mutex_);
        stream_locked_.clear();
        stream_tapped_.clear();
    }

    stopping_.store(false);
    running_.store(true);
    last_flush_ms_ = now_ms();

    capture_thread_  = std::thread(&PacketCapture::capture_loop, this);
    watchdog_thread_ = std::thread(&PacketCapture::watchdog_loop, this);

    GB_INFO("Capture started on %s", iface.c_str());
    return true;
}

void PacketCapture::stop() {
    if (!running_.exchange(false)) return;

    stopping_.store(true);

    // breakloop makes pcap_dispatch return promptly; the handle itself is
    // closed on the capture thread to avoid a use-after-free race.
    if (handle_) pcap_breakloop(handle_);

    if (capture_thread_.joinable())  capture_thread_.join();
    if (watchdog_thread_.joinable()) watchdog_thread_.join();

    if (handle_) {
        pcap_close(handle_);
        handle_ = nullptr;
    }

    reassembler_.clear();
    {
        std::lock_guard<std::mutex> lock(batch_mutex_);
        batch_.clear();
    }
    GB_INFO("Capture stopped");
}

bool PacketCapture::switch_interface(const std::string& interface_name) {
    if (!running_.load()) {
        config_.interface_name = interface_name;
        return true;
    }
    CaptureConfig cfg = config_;
    cfg.interface_name = interface_name;
    stop();
    return start(cfg);
}

// ----------------------------------------------------------------- capture

void PacketCapture::capture_loop() {
    while (running_.load() && !stopping_.load()) {
        pcap_pkthdr* header = nullptr;
        const std::uint8_t* data = nullptr;

        const int rc = pcap_next_ex(handle_, &header, &data);

        if (rc == 1 && header && data) {
            on_raw_packet(data, header->caplen, now_ms());
        } else if (rc == 0) {
            // Read timeout; normal.
        } else if (rc == -2) {
            break;      // breakloop
        } else {
            GB_ERROR("pcap_next_ex failed: %s", pcap_geterr(handle_));
            break;
        }

        // Deliver on the interval even when traffic is bursty.
        if (now_ms() - last_flush_ms_ >= batch_interval_ms_) flush_batch();
    }
    flush_batch();
}

void PacketCapture::on_raw_packet(const std::uint8_t* data, std::size_t caplen,
                                  std::uint64_t timestamp_ms) {
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.packets_seen;
        stats_.last_packet_ms = timestamp_ms;
    }

    if (caplen < kEthernetHeaderLen) return;

    std::size_t offset = kEthernetHeaderLen;
    std::uint16_t ethertype = be16(data + 12);

    // Hop over up to two VLAN tags; Npcap on some NICs delivers them.
    for (int i = 0; i < 2 && ethertype == kEtherTypeVlan; ++i) {
        if (caplen < offset + 4) return;
        ethertype = be16(data + offset + 2);
        offset += 4;
    }

    if (ethertype != kEtherTypeIPv4) return;
    if (caplen < offset + 20) return;

    const std::uint8_t* ip = data + offset;
    const std::uint8_t  ihl = static_cast<std::uint8_t>((ip[0] & 0x0F) * 4);
    if (ihl < 20 || caplen < offset + ihl) return;
    if (ip[9] != kProtoTcp) return;

    const std::uint16_t total_len = be16(ip + 2);
    if (total_len < ihl) return;

    // Fragmented IP would need its own reassembly stage. Game traffic inside
    // an MTU never fragments, so a middle/last fragment here means something
    // unrelated slipped through the filter.
    const std::uint16_t frag = be16(ip + 6);
    if ((frag & 0x1FFF) != 0) return;

    const std::uint32_t src_ip = be32(ip + 12);
    const std::uint32_t dst_ip = be32(ip + 16);

    const std::size_t tcp_off = offset + ihl;
    if (caplen < tcp_off + 20) return;

    const std::uint8_t* tcp = data + tcp_off;
    const std::uint16_t src_port = be16(tcp);
    const std::uint16_t dst_port = be16(tcp + 2);
    const std::uint32_t seq      = be32(tcp + 4);
    const std::uint8_t  doff     = static_cast<std::uint8_t>(((tcp[12] >> 4) & 0x0F) * 4);
    const std::uint8_t  flags    = tcp[13];

    if (doff < 20 || caplen < tcp_off + doff) return;

    const bool fin = (flags & 0x01) != 0;
    const bool syn = (flags & 0x02) != 0;
    const bool rst = (flags & 0x04) != 0;
    const bool ack = (flags & 0x10) != 0;

    // Payload length comes from the IP header, not caplen: a short snaplen
    // truncates the capture but not the declared length.
    const std::size_t ip_payload = static_cast<std::size_t>(total_len) - ihl;
    if (ip_payload < doff) return;

    std::size_t payload_len = ip_payload - doff;
    const std::size_t available = caplen - (tcp_off + doff);
    if (payload_len > available) payload_len = available;    // truncated capture

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.tcp_segments;
    }

    Endpoint src{ src_ip, src_port };
    Endpoint dst{ dst_ip, dst_port };

    reassembler_.on_segment(src, dst, seq, syn, ack, fin, rst,
                            data + tcp_off + doff, payload_len, timestamp_ms);
}

std::size_t PacketCapture::on_stream(const FlowKey& key, Direction dir,
                                     const std::uint8_t* data, std::size_t size,
                                     std::uint64_t timestamp_ms) {
    if (raw_only_.load()) {
        // Hand the whole run over and consume it. Returning anything less
        // would leave those bytes in the buffer to be delivered again on the
        // next segment, duplicating them in the recording.
        {
            std::lock_guard<std::mutex> lock(tap_mutex_);
            if (raw_tap_) raw_tap_(key, dir, data, size);
        }
        return size;
    }

    FramingSpec spec;
    bool              filtering = false;
    std::vector<bool> allowed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The two directions are framed differently in this game, so the spec
        // depends on which way the bytes are going.
        spec      = (dir == Direction::ToServer && has_client_framing_)
                        ? client_framing_ : framing_;
        filtering = filter_active_;
        if (filtering) allowed = opcode_allowed_;
    }

    if (!spec.plausible()) {
        // No usable framing configured. Drain rather than accumulate: the
        // bytes cannot be parsed and letting the buffer grow to its cap just
        // wastes memory and delays the eventual discard.
        return size;
    }

    // Per-stream alignment state. Attaching to a game that is already
    // connected drops us mid-frame, so a stream starts unlocked and the
    // scanner treats its opening bytes as garbage to slide past rather than a
    // frame to wait for.
    const std::uint64_t stream_id =
        (static_cast<std::uint64_t>(FlowKeyHash{}(key)) << 1) |
        (dir == Direction::ToClient ? 1u : 0u);

    bool        locked = false;
    std::size_t tapped = 0;
    {
        std::lock_guard<std::mutex> lock(lock_state_mutex_);
        if (auto it = stream_locked_.find(stream_id); it != stream_locked_.end())
            locked = it->second;
        if (auto it = stream_tapped_.find(stream_id); it != stream_tapped_.end())
            tapped = it->second;
    }

    // Feed the raw tap the bytes it has not seen yet.
    //
    // We are handed the whole stream buffer every call, and whatever the framer
    // leaves unconsumed comes back at the front of the next one. Tapping the
    // full buffer would therefore re-deliver those leftovers and duplicate them
    // in the recording; tapping nothing (which is what this did) silently
    // records zero bytes. So track how much of the current buffer has already
    // gone out and only pass the tail.
    if (tapped > size) tapped = 0;      // buffer was reset or discarded behind us
    {
        std::lock_guard<std::mutex> lock(tap_mutex_);
        if (raw_tap_ && size > tapped) raw_tap_(key, dir, data + tapped, size - tapped);
    }

    FrameScanResult scan = extract_frames(spec, data, size, dir, timestamp_ms, locked);

    {
        std::lock_guard<std::mutex> lock(lock_state_mutex_);
        stream_locked_[stream_id] = scan.locked;
        // Everything still buffered has already been tapped; it will be
        // presented again at the front of the next call.
        stream_tapped_[stream_id] = size - std::min(scan.consumed, size);
    }

    if (!scan.packets.empty() || scan.resyncs) {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.frames_decoded += scan.packets.size();
        stats_.resync_bytes   += scan.resyncs;
    }

    if (!scan.packets.empty()) {
        std::lock_guard<std::mutex> lock(batch_mutex_);
        for (auto& pkt : scan.packets) {
            if (filtering && pkt.opcode < allowed.size() && !allowed[pkt.opcode]) continue;
            batch_.push_back(std::move(pkt));
        }
    }

    return scan.consumed;
}

void PacketCapture::flush_batch() {
    std::vector<GamePacket> out;
    {
        std::lock_guard<std::mutex> lock(batch_mutex_);
        if (batch_.empty()) {
            last_flush_ms_ = now_ms();
            return;
        }
        out.swap(batch_);
    }

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.frames_forwarded += out.size();
    }

    last_flush_ms_ = now_ms();
    if (on_packets_) on_packets_(out);
}

void PacketCapture::watchdog_loop() {
    // A live game connection is never silent for long — there is always a
    // heartbeat or a position update. Prolonged silence means the game closed
    // or the adapter changed, and the bot should stop pressing keys.
    constexpr std::uint64_t kSilenceMs = 15'000;
    bool reported = false;

    while (running_.load() && !stopping_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        std::uint64_t last = 0;
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            last = stats_.last_packet_ms;
            if (handle_) {
                pcap_stat ps{};
                if (pcap_stats(handle_, &ps) == 0) stats_.pcap_dropped = ps.ps_drop;
            }
        }

        if (last == 0) continue;      // nothing seen yet

        if (now_ms() - last > kSilenceMs) {
            if (!reported) {
                GB_WARN("No game traffic for %llus -- assuming disconnect",
                        static_cast<unsigned long long>(kSilenceMs / 1000));
                reported = true;
                if (on_disconnect_) on_disconnect_();
            }
        } else {
            reported = false;
        }
    }
}

PacketCapture::Stats PacketCapture::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

} // namespace gobot
