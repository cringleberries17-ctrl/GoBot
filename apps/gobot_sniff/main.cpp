// gobot_sniff — protocol discovery workbench.
//
// The bot cannot act until GhostOnline's wire format is known: how messages
// are delimited, which opcode carries monster positions, where HP sits inside
// that packet. None of that is published, and the reference client did not
// contain it either — it shipped raw frames to a remote server that held the
// knowledge. This tool is how you derive it yourself, from your own traffic.
//
// Workflow:
//   1. gobot_sniff --list                       pick your adapter
//   2. gobot_sniff --record log.bin --host <ip> capture a few minutes of play
//   3. gobot_sniff --detect log.bin             find the framing
//        -> paste the printed [Framing] block into configs/protocol.ini
//   4. gobot_sniff --watch                      see which opcodes actually fire
//   5. gobot_sniff --diff 0x1234                find the fields inside one
//        -> write a [Packet.0x1234] block into configs/protocol.ini
//   6. gobot_sniff --decode                     confirm the layout reads right

#include "gobot/byte_reader.hpp"
#include "gobot/config.hpp"
#include "gobot/framing.hpp"
#include "gobot/logger.hpp"
#include "gobot/net_discovery.hpp"
#include "gobot/opcode_registry.hpp"
#include "gobot/packet_capture.hpp"
#include "gobot/protocol_analyzer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace gobot;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

void print_usage() {
    std::printf(R"(gobot_sniff — GhostOnline protocol discovery

USAGE
  gobot_sniff <command> [options]

COMMANDS
  --list                     List capture interfaces and exit.
  --discover                 Show which process owns which TCP connection, and
                             the pcap filter that would capture it. Use this
                             instead of netstat: --discover --process Game.exe
                             prints exactly what to pass to --host/--port.
  --record <file>            Capture the raw server->client stream to a file.
  --detect <file>            Brute-force the framing of a recorded stream.
  --watch                    Live opcode histogram (needs framing configured).
  --dump <opcode>            Hexdump live packets of one opcode.
  --diff <opcode>            Show which byte offsets change between successive
                             packets of one opcode. This is how you locate
                             coordinate, HP and id fields. Add --from to read a
                             recording instead of live traffic.
  --decode                   Decode live traffic with configs/protocol.ini and
                             print the result. Use to verify a layout.
  --analyze                  Capture, then infer what each opcode means. Same
                             engine the GUI's automatic setup uses; prints why
                             each opcode was or was not classified.

OPTIONS
  --process <exe>            For --discover: only this executable.
  --iface <name>             Capture interface (default: auto-detect).
  --host <ip>                Game server IP. Narrows the capture filter.
  --port <n>                 Game server port.
  --filter <bpf>             Raw BPF filter, overrides --host/--port.
  --config <path>            Protocol config (default: configs/protocol.ini).
  --seconds <n>              Stop after n seconds (default: run until Ctrl+C).
  --max <n>                  For --dump/--diff: stop after n packets.
  --from <capture>           For --diff: analyse a file written by --record
                             instead of capturing. Needs no game running and
                             nothing happening in front of you, and the same
                             bytes can be re-read when a hypothesis changes --
                             which is the difference between measuring and
                             remembering. Both framings are tried and the one
                             that tiles is used, since a recording does not say
                             which direction it holds.
                             (Still prompts for Administrator: this program
                             asks for it up front because capture needs it.)
  --versus <capture>         For --diff --from: compare the two recordings
                             rather than describing one. Record one capture
                             standing still and one walking, and the position
                             fields are the ones quiet in the first and busy in
                             the second. Timers and sequence numbers are busy
                             in both and rule themselves out -- which a single
                             capture cannot tell you, and which is how an
                             afternoon gets spent on a field that turns out to
                             be a clock.
  --verbose                  Debug logging.

NOTES
  Npcap must be installed and this process must run as Administrator.
  Capture is read-only: nothing is injected, rewritten or blocked.
)");
}

struct Options {
    std::string command;
    std::string file;
    std::string from;        ///< --from: analyse this recording, not live traffic
    std::string versus;      ///< --versus: compare --from against this one
    std::string iface;
    std::string host;
    std::string filter;
    std::string config = "configs/protocol.ini";
    std::string process;
    std::uint16_t port = 0;
    std::uint16_t opcode = 0;
    int  seconds = 0;
    int  max = 0;
    bool verbose = false;
};

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };

        if (a == "--list")          { o.command = "list"; }
        else if (a == "--discover") { o.command = "discover"; }
        else if (a == "--process")  { o.process = next("--process"); }
        else if (a == "--record")   { o.command = "record"; o.file = next("--record"); }
        else if (a == "--detect")   { o.command = "detect"; o.file = next("--detect"); }
        else if (a == "--watch")    { o.command = "watch"; }
        else if (a == "--decode")   { o.command = "decode"; }
        else if (a == "--analyze")  { o.command = "analyze"; }
        else if (a == "--verify")   { o.command = "verify"; o.file = next("--verify"); }
        else if (a == "--dump")     { o.command = "dump";
                                      o.opcode = static_cast<std::uint16_t>(std::strtoul(next("--dump").c_str(), nullptr, 0)); }
        else if (a == "--diff")     { o.command = "diff";
                                      o.opcode = static_cast<std::uint16_t>(std::strtoul(next("--diff").c_str(), nullptr, 0)); }
        else if (a == "--iface")    { o.iface = next("--iface"); }
        else if (a == "--host")     { o.host = next("--host"); }
        else if (a == "--port")     { o.port = static_cast<std::uint16_t>(std::stoi(next("--port"))); }
        else if (a == "--filter")   { o.filter = next("--filter"); }
        else if (a == "--config")   { o.config = next("--config"); }
        else if (a == "--seconds")  { o.seconds = std::stoi(next("--seconds")); }
        else if (a == "--max")      { o.max = std::stoi(next("--max")); }
        else if (a == "--from")     { o.from = next("--from"); }
        else if (a == "--versus")   { o.versus = next("--versus"); }
        else if (a == "--verbose")  { o.verbose = true; }
        else if (a == "--help" || a == "-h") { print_usage(); std::exit(0); }
        else {
            std::fprintf(stderr, "error: unknown argument '%s'\n\n", a.c_str());
            print_usage();
            return false;
        }
    }
    return !o.command.empty();
}

CaptureConfig make_capture_config(const Options& o) {
    CaptureConfig cfg;
    cfg.interface_name  = o.iface;
    cfg.bpf_filter      = o.filter;
    cfg.server_ip_start = o.host;
    cfg.server_ip_end   = o.host;
    cfg.port_start      = o.port;
    cfg.port_end        = o.port;

    // When --process names the game, read its actual sockets: they give both
    // the exact capture filter and which end is the server, so direction is
    // labelled rather than inferred.
    //
    // Deriving the filter here also means a hand-written one cannot go stale.
    // A game that reconnects often lands on a different server address, and a
    // filter pinned to the old one silently captures nothing at all.
    if (!o.process.empty()) {
        std::vector<ProcessConnection> conns;
        for (const auto& c : connections_for_process(o.process)) {
            if (!c.established) continue;
            const std::uint32_t r = c.remote.ip;
            if (r == 0 || (r >> 24) == 127 || (r >> 16) == 0xA9FE) continue;
            if (c.remote.port == 80 || c.remote.port == 443) continue;
            conns.push_back(c);
            cfg.known_servers.push_back(c.remote);
        }

        if (!conns.empty()) {
            if (cfg.bpf_filter.empty() && cfg.server_ip_start.empty()) {
                cfg.bpf_filter = build_filter_for(conns);
                std::printf("Capturing %s: %s\n", o.process.c_str(), cfg.bpf_filter.c_str());
            } else {
                std::printf("Using %zu server endpoint(s) from %s\n",
                            conns.size(), o.process.c_str());
            }
        } else {
            std::printf("Warning: %s has no game-like connection open.\n", o.process.c_str());
        }
    }
    return cfg;
}

/// Read a framing block out of protocol.ini.
FramingSpec load_framing(const std::string& path, bool& found,
                         const char* section = "Framing") {
    FramingSpec spec;
    IniFile ini;
    found = false;

    if (!ini.load(path) || !ini.has_section(section)) return spec;

    found = true;
    const std::string S = section;
    spec.header_size   = static_cast<std::uint32_t>(ini.get_int(S, "header_size", 4));
    spec.length_offset = static_cast<std::uint32_t>(ini.get_int(S, "length_offset", 0));
    spec.length_size   = static_cast<std::uint32_t>(ini.get_int(S, "length_size", 2));
    spec.length_le     = ini.get(S, "length_endian", "little") != "big";
    spec.length_includes_header = ini.get_bool(S, "length_includes_header", true);
    spec.length_adjust = ini.get_int(S, "length_adjust", 0);
    spec.opcode_offset = static_cast<std::uint32_t>(ini.get_int(S, "opcode_offset", 2));
    spec.opcode_size   = static_cast<std::uint32_t>(ini.get_int(S, "opcode_size", 2));
    spec.opcode_le     = ini.get(S, "opcode_endian", "little") != "big";
    spec.min_frame     = static_cast<std::uint32_t>(ini.get_int(S, "min_frame", static_cast<int>(spec.header_size)));
    spec.max_frame     = static_cast<std::uint32_t>(ini.get_int(S, "max_frame", 65535));
    spec.checksum_offset = static_cast<std::uint32_t>(ini.get_int(S, "checksum_offset", static_cast<int>(FramingSpec::kNoChecksum)));
    spec.trailer_opcode = static_cast<std::uint16_t>(ini.get_int(S, "trailer_opcode", 0));
    spec.trailer_bytes  = static_cast<std::uint32_t>(ini.get_int(S, "trailer_bytes", 0));
    return spec;
}

/// Also hand the capture the client-side framing.
///
/// The two directions are framed differently in this game -- the opcode sits at
/// offset 2 coming down and offset 6 going up -- so a capture told only about
/// `[Framing]` decodes the server's half and grinds the client's half into
/// resync noise, a byte at a time. Every tool here was doing that, which is why
/// the client half never appeared in --watch or --diff and had to be analysed
/// out of band.
///
/// A missing `[FramingClient]` is not fatal, just half-blind, so say so rather
/// than failing.
void attach_client_framing(PacketCapture& cap, const std::string& config) {
    bool found = false;
    const FramingSpec client = load_framing(config, found, "FramingClient");
    if (found) {
        cap.set_client_framing(client);
    } else {
        std::printf("note: no [FramingClient] in %s -- what the client sends\n"
                    "      cannot be decoded, and counts as resyncs.\n",
                    config.c_str());
    }
}

void wait_for_stop(int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds ? seconds : 86400);
    while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

// ------------------------------------------------------------------ commands

int cmd_list() {
    if (!PacketCapture::npcap_available()) {
        std::fprintf(stderr,
            "Npcap service is not running.\n"
            "  Install from https://npcap.com/#download\n"
            "  Tick 'WinPcap API-compatible Mode' during setup.\n");
        return 1;
    }

    const auto interfaces = PacketCapture::list_interfaces();
    if (interfaces.empty()) {
        std::fprintf(stderr, "No interfaces found. Are you running as Administrator?\n");
        return 1;
    }

    const std::string def = PacketCapture::default_interface();

    std::printf("\nCapture interfaces:\n\n");
    for (const auto& i : interfaces) {
        const bool is_default = (i.name == def);
        std::printf("  %s%-16s %s\n",
                    is_default ? "* " : "  ",
                    i.ipv4.empty() ? "(no IPv4)" : i.ipv4.c_str(),
                    i.description.empty() ? i.name.c_str() : i.description.c_str());
        std::printf("      %s\n", i.name.c_str());
    }
    std::printf("\n  * = default route (used when --iface is omitted)\n\n");
    return 0;
}

/// Show which process owns which socket.
///
/// This replaces the "run netstat -n -b and copy the address across" step. The
/// game already knows its server; asking the OS which sockets the game process
/// owns is both easier and less error-prone than asking a person to read it
/// off a terminal.
int cmd_discover(const Options& o) {
    auto conns = o.process.empty() ? tcp_connections()
                                   : connections_for_process(o.process);

    // Established, routable, non-web: what game traffic looks like.
    std::vector<ProcessConnection> interesting;
    for (const auto& c : conns) {
        if (!c.established) continue;
        const std::uint32_t r = c.remote.ip;
        if (r == 0 || (r >> 24) == 127 || (r >> 16) == 0xA9FE) continue;
        if (c.remote.port == 80 || c.remote.port == 443) continue;
        interesting.push_back(c);
    }

    if (conns.empty()) {
        std::printf("\nNo TCP connections found%s.\n",
                    o.process.empty() ? "" : (" for " + o.process).c_str());
        return 1;
    }

    std::printf("\n%-22s %-7s %-24s %s\n", "Process", "PID", "Remote", "Local");
    std::printf("%s\n", std::string(76, '-').c_str());

    for (const auto& c : interesting) {
        std::printf("%-22s %-7u %-24s %s\n",
                    c.process.empty() ? "?" : c.process.c_str(),
                    c.pid,
                    c.remote.to_string().c_str(),
                    c.local.to_string().c_str());
    }

    if (interesting.empty()) {
        std::printf("(none that look like game traffic -- only loopback, "
                    "link-local or web connections)\n");
        std::printf("\nIs the game logged in to a character? The socket only "
                    "exists once connected.\n");
        return 1;
    }

    if (!o.process.empty()) {
        std::printf("\nFilter for these connections:\n\n  %s\n",
                    build_filter_for(interesting).c_str());
        const auto& first = interesting.front();
        std::printf("\nOr pass them directly:\n\n"
                    "  gobot_sniff --record capture.bin --host %s --port %u\n",
                    ip_to_string(first.remote.ip).c_str(), first.remote.port);

        const std::string iface =
            PacketCapture::find_interface_by_ip(ip_to_string(first.local.ip));
        if (!iface.empty())
            std::printf("\nAdapter carrying it:\n\n  %s\n", iface.c_str());
    } else {
        std::printf("\nNarrow to the game with:  --discover --process <exe name>\n");
    }
    return 0;
}

int cmd_record(const Options& o) {
    // Both directions, to separate files. They are separate protocols as far
    // as framing is concerned — detection has to run on one direction of one
    // connection — and the client->server side is where your own actions live,
    // which makes it the most informative half for working out what means what.
    const std::string s2c_path = o.file + ".s2c";
    const std::string c2s_path = o.file + ".c2s";

    std::ofstream out(s2c_path, std::ios::binary | std::ios::trunc);
    std::ofstream out_c2s(c2s_path, std::ios::binary | std::ios::trunc);
    if (!out || !out_c2s) {
        std::fprintf(stderr, "error: cannot write %s / %s\n",
                     s2c_path.c_str(), c2s_path.c_str());
        return 1;
    }

    PacketCapture cap;

    // Recording must not depend on the framing being known — discovering it is
    // the whole point. Raw-only mode skips framing and delivers every
    // reassembled byte to the tap exactly once.
    cap.set_raw_only(true);

    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::uint64_t> bytes_c2s{0};
    std::mutex write_mutex;

    // Keep connections apart. A game commonly holds more than one socket open,
    // and writing them all to one file interleaves two unrelated byte streams:
    // the result cannot tile under any framing, so detection either fails or
    // latches onto something meaningless. The per-flow buffers are what get
    // analysed; the combined file is kept only for reference.
    std::map<std::uint64_t, Bytes>       per_flow;
    std::map<std::uint64_t, std::string> flow_names;

    cap.set_raw_tap([&](const FlowKey& key, Direction dir,
                        const std::uint8_t* data, std::size_t size) {
        std::lock_guard<std::mutex> lock(write_mutex);
        if (dir == Direction::ToClient) {
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            bytes.fetch_add(size);

            const std::uint64_t id = FlowKeyHash{}(key);
            auto& buf = per_flow[id];
            if (flow_names.find(id) == flow_names.end()) flow_names[id] = key.to_string();
            buf.insert(buf.end(), data, data + size);
        } else {
            out_c2s.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
            bytes_c2s.fetch_add(size);
        }
    });

    if (!cap.start(make_capture_config(o))) return 1;

    std::printf("Recording server->client stream to %s\n", o.file.c_str());
    std::printf("Play the game normally. Ctrl+C to stop.\n\n");

    std::thread reporter([&] {
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            const auto s = cap.stats();
            // Drops are shown live, not only at the end. A recording made on a
            // lossy adapter is not merely short: a missing segment leaves a
            // hole that desynchronises everything after it, so later frames
            // read as garbage and the framing appears wrong when it is not.
            // Seeing it happen beats discovering it during analysis.
            std::printf("\r  S->C %llu B | C->S %llu B | %llu packets | "
                        "%llu segs | dropped %llu   ",
                        static_cast<unsigned long long>(bytes.load()),
                        static_cast<unsigned long long>(bytes_c2s.load()),
                        static_cast<unsigned long long>(s.packets_seen),
                        static_cast<unsigned long long>(s.tcp_segments),
                        static_cast<unsigned long long>(s.pcap_dropped));
            std::fflush(stdout);
        }
    });

    wait_for_stop(o.seconds);
    g_stop.store(true);
    reporter.join();
    cap.stop();
    out.close();
    out_c2s.close();

    std::printf("\n\nserver -> client : %llu bytes  ->  %s\n"
                "client -> server : %llu bytes  ->  %s\n",
                static_cast<unsigned long long>(bytes.load()), s2c_path.c_str(),
                static_cast<unsigned long long>(bytes_c2s.load()), c2s_path.c_str());

    // Whether this recording is worth analysing at all.
    //
    // A capture that lost packets and one whose framing is wrong look the same
    // downstream -- both tile badly -- and the fixes are opposite: a bigger
    // buffer or a quieter link in the first case, a different FramingSpec in
    // the second. The driver knows which happened, so say so here rather than
    // leaving it to be guessed at from resync counts later.
    {
        const auto s = cap.stats();
        std::printf("\ncapture: %llu packets, %llu TCP segments, %llu dropped by the driver\n",
                    static_cast<unsigned long long>(s.packets_seen),
                    static_cast<unsigned long long>(s.tcp_segments),
                    static_cast<unsigned long long>(s.pcap_dropped));
        if (s.pcap_dropped > 0) {
            const double pct = 100.0 * static_cast<double>(s.pcap_dropped) /
                               static_cast<double>(s.pcap_dropped + s.packets_seen);
            std::printf("!! %.2f%% of packets were dropped before they reached this program.\n"
                        "   The reassembled stream has holes, so frames after each hole are\n"
                        "   garbage and no FramingSpec will tile them. Re-record on a quieter\n"
                        "   link, or raise the capture buffer, before trusting this file.\n", pct);
        } else {
            std::printf("   No drops. Any tiling trouble in this file is the framing, not loss.\n");
        }
    }

    // Write each connection separately and point at the busiest, which is the
    // one worth running detection on.
    std::string busiest;
    std::size_t busiest_size = 0;
    int index = 0;

    if (per_flow.size() > 1)
        std::printf("\n%zu separate connections were captured. Detection must run on one\n"
                    "of them alone -- interleaved streams do not tile.\n\n", per_flow.size());

    for (const auto& [id, buf] : per_flow) {
        const std::string path = o.file + ".flow" + std::to_string(index++) + ".s2c";
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(buf.data(), 1, buf.size(), f);
            std::fclose(f);
        }
        std::printf("  %-8zu bytes  %-40s  %s\n",
                    buf.size(), flow_names[id].c_str(), path.c_str());
        if (buf.size() > busiest_size) {
            busiest_size = buf.size();
            busiest = path;
        }
    }

    if (bytes.load() < 1024) {
        std::printf("\nThat is very little data. Check that:\n"
                    "  - the game was actually connected and moving\n"
                    "  - --iface is the adapter carrying game traffic\n"
                    "  - --host matches the game server (omit it to capture all TCP)\n");
    } else if (bytes_c2s.load() < 256) {
        std::printf("\nBarely anything went client -> server. That half carries your own\n"
                    "actions, and it is the clearest evidence there is for working out\n"
                    "which packet holds your position. If the character stood still for\n"
                    "this capture, record again while walking around.\n");
    } else {
        std::printf("\nNext: gobot_sniff --detect %s\n",
                    busiest.empty() ? s2c_path.c_str() : busiest.c_str());
    }
    return 0;
}

int cmd_detect(const Options& o) {
    std::ifstream in(o.file, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "error: cannot read %s\n", o.file.c_str());
        return 1;
    }

    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (data.size() < 64) {
        std::fprintf(stderr, "error: %s holds only %zu bytes; record more traffic\n",
                     o.file.c_str(), data.size());
        return 1;
    }

    std::printf("\nAnalysing %zu bytes from %s\n", data.size(), o.file.c_str());
    std::printf("Trying every plausible header layout...\n\n");

    // Cap the sample: the search is O(candidates x bytes) and a few hundred KB
    // is already far more evidence than the answer needs.
    const std::size_t sample = std::min<std::size_t>(data.size(), 256 * 1024);
    const auto results = detect_framing(data.data(), sample, 10);

    if (results.empty()) {
        std::printf("No candidate framing tiled the stream.\n\n"
                    "That usually means one of:\n"
                    "  - the payload is encrypted or compressed\n"
                    "  - the capture mixed several connections (narrow it with --host/--port)\n"
                    "  - the recording starts mid-frame and is too short to resync\n\n"
                    "First 256 bytes for manual inspection:\n\n%s\n",
                    hexdump(data.data(), std::min<std::size_t>(data.size(), 256), 16, 2).c_str());
        return 1;
    }

    std::printf("%-4s %-9s %-9s %-8s %-7s  %s\n",
                "#", "coverage", "frames", "opcodes", "score", "layout");
    std::printf("%s\n", std::string(78, '-').c_str());

    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::printf("%-4zu %7.1f%%  %-9zu %-8zu %-7.0f  %s\n",
                    i + 1, r.coverage * 100.0, r.frames_parsed,
                    r.distinct_opcodes, r.score, r.spec.describe().c_str());
    }

    const auto& best = results.front();
    std::printf("\nBest candidate explains %.1f%% of the stream as %zu frames.\n",
                best.coverage * 100.0, best.frames_parsed);

    if (best.coverage < 0.95) {
        std::printf("\nWARNING: coverage below 95%%. Treat this as a guess, not an answer.\n"
                    "Record a longer, single-connection sample and re-run.\n");
    }

    std::printf("\nPaste into %s:\n\n%s\n", o.config.c_str(), best.spec.to_ini().c_str());

    // header_size is only ever a lower bound: tiling proves where frames start
    // and end, not how much of the front is header. Extra header fields —
    // sequence numbers, checksums — are indistinguishable from leading payload
    // bytes, and simply show up as a constant offset on every field below.
    std::printf("header_size is the smallest that fits the length and opcode fields.\n"
                "If the protocol carries more header than that, those bytes appear at\n"
                "the start of every payload -- harmless, but field offsets will all be\n"
                "shifted by however many bytes it is.\n");

    // Show what the winning spec actually produces, so a bogus fit is visible
    // rather than being taken on faith.
    auto scan = extract_frames(best.spec, data.data(), std::min<std::size_t>(data.size(), 8192),
                               Direction::ToClient, 0);
    std::printf("First frames under this layout:\n\n");
    for (std::size_t i = 0; i < std::min<std::size_t>(scan.packets.size(), 12); ++i) {
        const auto& p = scan.packets[i];
        std::printf("  opcode 0x%04X  %zu byte body\n", p.opcode, p.payload.size());
        if (!p.payload.empty()) {
            std::printf("%s", hexdump(p.payload.data(),
                                      std::min<std::size_t>(p.payload.size(), 32), 16, 6).c_str());
        }
    }
    std::printf("\nNext: add the [Framing] block above, then run --watch\n");
    return 0;
}

int cmd_watch(const Options& o) {
    bool have_framing = false;
    const FramingSpec spec = load_framing(o.config, have_framing);

    if (!have_framing) {
        std::fprintf(stderr,
            "error: no [Framing] section in %s\n"
            "Run --record then --detect first.\n", o.config.c_str());
        return 1;
    }

    OpcodeRegistry registry;
    registry.load(o.config);

    PacketCapture cap;
    cap.set_framing(spec);
    attach_client_framing(cap, o.config);

    std::mutex counts_mutex;
    std::map<std::uint16_t, std::uint64_t> counts;
    std::map<std::uint16_t, std::size_t>   sizes;

    cap.set_packet_callback([&](const std::vector<GamePacket>& packets) {
        std::lock_guard<std::mutex> lock(counts_mutex);
        for (const auto& p : packets) {
            ++counts[p.opcode];
            sizes[p.opcode] = p.payload.size();
        }
    });

    if (!cap.start(make_capture_config(o))) return 1;

    std::printf("Watching with framing: %s\n", spec.describe().c_str());
    std::printf("Move, attack, and take damage in game to make opcodes appear.\n");
    std::printf("Ctrl+C to stop.\n\n");

    std::thread reporter([&] {
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(3));

            std::vector<std::pair<std::uint16_t, std::uint64_t>> sorted;
            std::map<std::uint16_t, std::size_t> size_copy;
            {
                std::lock_guard<std::mutex> lock(counts_mutex);
                sorted.assign(counts.begin(), counts.end());
                size_copy = sizes;
            }
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });

            const auto s = cap.stats();
            std::printf("\n=== %zu distinct opcodes | %llu frames | %llu resync bytes ===\n",
                        sorted.size(),
                        static_cast<unsigned long long>(s.frames_decoded),
                        static_cast<unsigned long long>(s.resync_bytes));

            if (s.resync_bytes > s.frames_decoded * 4 && s.frames_decoded > 20) {
                std::printf("!! High resync count — the framing is probably wrong.\n");
            }

            std::printf("%-10s %-10s %-8s %s\n", "opcode", "count", "last", "mapped as");
            for (std::size_t i = 0; i < std::min<std::size_t>(sorted.size(), 25); ++i) {
                const auto [op, n] = sorted[i];
                const PacketSpec* known = registry.find(op);
                std::printf("0x%04X     %-10llu %-8zuB %s\n",
                            op, static_cast<unsigned long long>(n), size_copy[op],
                            known ? known->name.c_str() : "-");
            }
        }
    });

    wait_for_stop(o.seconds);
    g_stop.store(true);
    reporter.join();
    cap.stop();

    std::printf("\nNext: pick a frequent opcode and run --diff 0x<opcode>\n");
    return 0;
}

int cmd_dump(const Options& o) {
    bool have_framing = false;
    const FramingSpec spec = load_framing(o.config, have_framing);
    if (!have_framing) {
        std::fprintf(stderr, "error: no [Framing] section in %s\n", o.config.c_str());
        return 1;
    }

    PacketCapture cap;
    cap.set_framing(spec);
    attach_client_framing(cap, o.config);
    cap.set_opcode_filter({ o.opcode });

    std::atomic<int> seen{0};

    cap.set_packet_callback([&](const std::vector<GamePacket>& packets) {
        for (const auto& p : packets) {
            if (p.opcode != o.opcode) continue;
            const int n = seen.fetch_add(1) + 1;
            std::printf("\n--- #%d  opcode 0x%04X  %zu bytes ---\n", n, p.opcode, p.payload.size());
            std::printf("%s", hexdump(p.payload, 16, 2).c_str());
            if (o.max > 0 && n >= o.max) g_stop.store(true);
        }
    });

    if (!cap.start(make_capture_config(o))) return 1;

    std::printf("Dumping opcode 0x%04X. Ctrl+C to stop.\n", o.opcode);
    wait_for_stop(o.seconds);
    cap.stop();

    std::printf("\nCaptured %d packets.\n", seen.load());
    return 0;
}

/// Byte-offset volatility analysis.
///
/// The core discovery technique: capture many instances of one opcode and see
/// which offsets change. An entity id is constant across updates for the same
/// entity; a coordinate changes smoothly as you walk; HP changes only when you
/// take damage. The change *pattern* identifies the field far more reliably
/// than staring at hex.
/// Read a recorded stream and pull out every payload of one opcode.
///
/// A recording does not say which direction it is, and the two are framed
/// differently, so both specs are tried and the one that tiles is used. That
/// choice is reported: a file analysed under the wrong spec yields plausible
/// nonsense rather than an obvious failure.
bool samples_from_file(const std::string& path, const std::string& config,
                       std::uint16_t opcode, std::vector<Bytes>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "error: cannot read %s\n", path.c_str());
        return false;
    }
    const std::vector<std::uint8_t> buf((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
    if (buf.empty()) {
        std::fprintf(stderr, "error: %s is empty\n", path.c_str());
        return false;
    }

    bool have_server = false, have_client = false;
    const FramingSpec server = load_framing(config, have_server);
    const FramingSpec client = load_framing(config, have_client, "FramingClient");
    if (!have_server) {
        std::fprintf(stderr, "error: no [Framing] section in %s\n", config.c_str());
        return false;
    }

    struct Try { const char* name; const FramingSpec* spec; Direction dir; bool ok; };
    const Try tries[] = {
        { "[Framing] (server -> client)", &server, Direction::ToClient, true },
        { "[FramingClient] (client -> server)", &client, Direction::ToServer, have_client },
    };

    // The name says it, when --record wrote it: <name>.s2c and <name>.c2s, and
    // .flow0.s2c for a single connection. Trust that over guessing.
    //
    // Guessing is not reliable here. Both specs tile a 47-frame client capture
    // with zero resyncs, because this scanner stops at the first header it
    // cannot use rather than sliding to the end of the file -- so "no resyncs"
    // can mean "read two frames and gave up" as easily as "read all of it".
    // A tie then picks whichever was tried first, which is how a client
    // recording comes to be read as a server one.
    const auto ends_with = [&](const char* suffix) {
        const std::string s = suffix;
        return path.size() >= s.size() &&
               path.compare(path.size() - s.size(), s.size(), s) == 0;
    };

    const Try* best = nullptr;
    if (ends_with(".c2s") && have_client) best = &tries[1];
    else if (ends_with(".s2c"))           best = &tries[0];

    if (best) {
        std::printf("  named %s, so reading it as %s\n",
                    ends_with(".c2s") ? ".c2s" : ".s2c", best->name);
    }

    FrameScanResult best_scan;
    if (best) {
        best_scan = extract_frames(*best->spec, buf.data(), buf.size(),
                                   best->dir, 0, false);
        std::printf("  %-36s %5zu frames, %6zu resync bytes\n",
                    best->name, best_scan.packets.size(), best_scan.resyncs);
    } else {
        for (const Try& t : tries) {
            if (!t.ok) continue;
            FrameScanResult scan = extract_frames(*t.spec, buf.data(), buf.size(),
                                                  t.dir, 0, false);
            std::printf("  %-36s %5zu frames, %6zu resync bytes\n",
                        t.name, scan.packets.size(), scan.resyncs);
            // Frames first, resyncs to break a tie. A spec that reads more of
            // the file is reading more of the file.
            if (!best || scan.packets.size() > best_scan.packets.size() ||
                (scan.packets.size() == best_scan.packets.size() &&
                 scan.resyncs < best_scan.resyncs)) {
                best = &t;
                best_scan = std::move(scan);
            }
        }
        if (best)
            std::printf("  -> no .s2c/.c2s in the name; read as %s\n", best->name);
    }
    if (!best) return false;
    std::printf("\n");
    if (best_scan.resyncs > best_scan.packets.size()) {
        std::printf("  !! more resync bytes than frames. Either this is the other\n"
                    "     direction, the framing is wrong, or the capture lost\n"
                    "     packets. Treat anything below with suspicion.\n\n");
    }

    for (const GamePacket& p : best_scan.packets)
        if (p.opcode == opcode) out.push_back(p.payload);
    return true;
}

/// Compare one opcode across two recordings taken under different conditions.
///
/// A single capture says which bytes move. Two say which bytes move *because
/// of the thing you changed*, and that is a different and much sharper
/// question. Record one capture standing still and one walking, and the
/// position fields are the ones that are quiet in the first and busy in the
/// second -- while timers and sequence numbers, which look identical to a
/// single-capture scan, are busy in both and rule themselves out.
int diff_two(const Options& o) {
    std::vector<Bytes> a, b;
    std::printf("A: %s\n", o.from.c_str());
    if (!samples_from_file(o.from, o.config, o.opcode, a)) return 1;
    std::printf("B: %s\n", o.versus.c_str());
    if (!samples_from_file(o.versus, o.config, o.opcode, b)) return 1;

    if (a.size() < 2 || b.size() < 2) {
        std::fprintf(stderr, "Need at least 2 packets of 0x%04X in each: "
                             "got %zu and %zu.\n", o.opcode, a.size(), b.size());
        return 1;
    }

    std::size_t common = a.front().size();
    for (const auto& s : a) common = std::min(common, s.size());
    for (const auto& s : b) common = std::min(common, s.size());

    std::printf("\n0x%04X: %zu packets in A, %zu in B, %zu common bytes\n\n",
                o.opcode, a.size(), b.size(), common);

    const auto distinct_at = [](const std::vector<Bytes>& v, std::size_t off) {
        std::vector<std::uint8_t> vals;
        vals.reserve(v.size());
        for (const auto& s : v) vals.push_back(s[off]);
        std::sort(vals.begin(), vals.end());
        return static_cast<std::size_t>(
            std::distance(vals.begin(), std::unique(vals.begin(), vals.end())));
    };

    std::printf("%-8s %-10s %-10s %s\n", "offset", "A distinct", "B distinct", "reading");
    std::printf("%s\n", std::string(66, '-').c_str());

    std::vector<std::size_t> woke, slept;
    for (std::size_t off = 0; off < common; ++off) {
        const std::size_t da = distinct_at(a, off);
        const std::size_t db = distinct_at(b, off);
        if (da == 1 && db == 1) continue;             // dead in both; say nothing

        const char* note = "varies in both  <- timer, sequence or counter";
        if (da <= 2 && db >= 5)      { note = "QUIET in A, ACTIVE in B"; woke.push_back(off); }
        else if (da >= 5 && db <= 2) { note = "ACTIVE in A, quiet in B"; slept.push_back(off); }
        else if (da == 1)            { note = "constant in A only"; }
        else if (db == 1)            { note = "constant in B only"; }

        std::printf("  %-6zu %-10zu %-10zu %s\n", off, da, db, note);
    }

    const auto runs_of = [&](const std::vector<std::size_t>& offs, const char* what) {
        if (offs.empty()) return;
        std::printf("\n%s:\n", what);
        std::size_t i = 0;
        while (i < offs.size()) {
            std::size_t j = i;
            while (j + 1 < offs.size() && offs[j + 1] == offs[j] + 1) ++j;
            const std::size_t len = j - i + 1;
            const char* guess = (len == 1) ? "u8"
                              : (len == 2) ? "u16 / i16"
                              : (len == 4) ? "u32 / i32 / f32"
                                           : "struct or string";
            std::printf("  offset %-4zu length %-3zu  -> %s\n", offs[i], len, guess);
            i = j + 1;
        }
    };
    runs_of(woke, "Became active in B -- the fields that respond to what changed");
    runs_of(slept, "Went quiet in B");

    if (woke.empty() && slept.empty()) {
        std::printf("\nNothing changed behaviour between the two captures. Either the\n"
                    "thing you varied is not reported in this opcode, or both captures\n"
                    "caught the same conditions.\n");
    }
    return 0;
}

int cmd_diff(const Options& o) {
    if (!o.versus.empty()) {
        if (o.from.empty()) {
            std::fprintf(stderr, "--versus needs --from as well: two recordings "
                                 "to compare.\n");
            return 1;
        }
        return diff_two(o);
    }

    std::vector<Bytes> samples;

    if (!o.from.empty()) {
        // Re-analysing a recording. No game, no Administrator, no waiting for
        // the right thing to happen in front of you -- and the same bytes can
        // be examined again after a hypothesis changes, which live capture
        // cannot offer.
        std::printf("Reading %s\n", o.from.c_str());
        if (!samples_from_file(o.from, o.config, o.opcode, samples)) return 1;
        std::printf("%zu packets of opcode 0x%04X in the recording.\n",
                    samples.size(), o.opcode);
    } else {
        bool have_framing = false;
        const FramingSpec spec = load_framing(o.config, have_framing);
        if (!have_framing) {
            std::fprintf(stderr, "error: no [Framing] section in %s\n", o.config.c_str());
            return 1;
        }

        PacketCapture cap;
        cap.set_framing(spec);
        attach_client_framing(cap, o.config);
        cap.set_opcode_filter({ o.opcode });

        std::mutex mutex;
        const int want = o.max > 0 ? o.max : 200;

        cap.set_packet_callback([&](const std::vector<GamePacket>& packets) {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& p : packets) {
                if (p.opcode != o.opcode) continue;
                samples.push_back(p.payload);
                if (static_cast<int>(samples.size()) >= want) g_stop.store(true);
            }
        });

        if (!cap.start(make_capture_config(o))) return 1;

        std::printf("Collecting up to %d packets of opcode 0x%04X.\n", want, o.opcode);
        std::printf("Do one thing at a time in game — walk left and right, then\n"
                    "stand still and take damage. Ctrl+C when done.\n\n");

        std::thread reporter([&] {
            while (!g_stop.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                std::lock_guard<std::mutex> lock(mutex);
                std::printf("\r  %zu / %d collected  ", samples.size(), want);
                std::fflush(stdout);
            }
        });

        wait_for_stop(o.seconds);
        g_stop.store(true);
        reporter.join();
        cap.stop();
    }

    if (samples.size() < 2) {
        std::fprintf(stderr, "\nNeed at least 2 packets, got %zu.\n", samples.size());
        return 1;
    }

    // Only analyse the length that every sample shares; variable-length tails
    // (names, item lists) would otherwise dominate the volatility picture.
    std::size_t common = samples.front().size();
    for (const auto& s : samples) common = std::min(common, s.size());

    std::printf("\n\n%zu samples, %zu common bytes\n\n", samples.size(), common);

    std::vector<std::size_t> distinct(common, 0);
    std::vector<std::uint8_t> first(common, 0);

    for (std::size_t off = 0; off < common; ++off) {
        std::vector<std::uint8_t> values;
        values.reserve(samples.size());
        for (const auto& s : samples) values.push_back(s[off]);

        first[off] = values.front();
        std::sort(values.begin(), values.end());
        distinct[off] = static_cast<std::size_t>(std::distance(values.begin(),
                                                 std::unique(values.begin(), values.end())));
    }

    std::printf("%-8s %-8s %-10s %s\n", "offset", "first", "distinct", "reading");
    std::printf("%s\n", std::string(60, '-').c_str());

    for (std::size_t off = 0; off < common; ++off) {
        const char* note;
        if (distinct[off] == 1)                     note = "constant  <- opcode sub-type / flag / padding";
        else if (distinct[off] <= 4)                note = "few values <- enum or boolean";
        else if (distinct[off] > samples.size() / 2) note = "high churn <- coordinate / timestamp / low byte";
        else                                         note = "varies";

        std::printf("  %-6zu 0x%02X     %-10zu %s\n", off, first[off], distinct[off], note);
    }

    // Adjacent volatile bytes are almost always one multi-byte field. Grouping
    // them turns a wall of per-byte noise into field candidates.
    std::printf("\nField candidates (runs of varying bytes):\n\n");
    std::size_t run_start = 0;
    bool in_run = false;

    for (std::size_t off = 0; off <= common; ++off) {
        const bool varies = (off < common) && distinct[off] > 1;

        if (varies && !in_run) {
            run_start = off;
            in_run = true;
        } else if (!varies && in_run) {
            const std::size_t len = off - run_start;
            const char* guess = (len == 1) ? "u8"
                              : (len == 2) ? "u16 / i16"
                              : (len == 4) ? "u32 / i32 / f32"
                              : (len == 8) ? "u64 / f64"
                                           : "string or struct";
            std::printf("  offset %-4zu length %-3zu  -> %s\n", run_start, len, guess);

            // Show the values a 4-byte window would read as, both as integer
            // and float — a float coordinate is instantly recognisable.
            if (len == 4 && samples.size() >= 3) {
                std::printf("        as i32: ");
                for (std::size_t i = 0; i < 3; ++i) {
                    ByteReader r(samples[i].data(), samples[i].size());
                    r.seek(run_start);
                    std::printf("%d ", r.i32());
                }
                std::printf("\n        as f32: ");
                for (std::size_t i = 0; i < 3; ++i) {
                    ByteReader r(samples[i].data(), samples[i].size());
                    r.seek(run_start);
                    std::printf("%.2f ", r.f32());
                }
                std::printf("\n");
            } else if (len == 2 && samples.size() >= 3) {
                std::printf("        as i16: ");
                for (std::size_t i = 0; i < 3; ++i) {
                    ByteReader r(samples[i].data(), samples[i].size());
                    r.seek(run_start);
                    std::printf("%d ", r.i16());
                }
                std::printf("\n");
            }
            in_run = false;
        }
    }

    std::printf("\nWrite a layout into %s, for example:\n\n", o.config.c_str());
    std::printf("  [Packet.0x%04X]\n", o.opcode);
    std::printf("  name=MonsterUpdate\n");
    std::printf("  semantic=monster_update\n");
    std::printf("  fields=id:u32,x:f32,y:f32,hp:i32\n\n");
    std::printf("Then: gobot_sniff --decode\n");
    return 0;
}

/// Capture, then run the same inference the GUI's automatic setup runs.
///
/// Prints the per-opcode evidence alongside the verdicts, because "found
/// nothing" is the failure that needs explaining most and a bare empty list
/// explains nothing at all.
int cmd_analyze(const Options& o) {
    bool have_framing = false;
    const FramingSpec spec = load_framing(o.config, have_framing);
    if (!have_framing) {
        std::fprintf(stderr, "error: no [Framing] section in %s\n", o.config.c_str());
        return 1;
    }

    PacketCapture cap;
    cap.set_framing(spec);
    attach_client_framing(cap, o.config);

    std::mutex mutex;
    std::map<std::uint16_t, OpcodeObservations> obs;

    cap.set_packet_callback([&](const std::vector<GamePacket>& packets) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& p : packets) {
            auto& e = obs[p.opcode];
            if (p.from_server) ++e.to_client; else ++e.to_server;
            if (e.payloads.size() < 256) {
                e.payloads.push_back(p.payload);
                e.timestamps_ms.push_back(p.timestamp_ms);
            }
        }
    });

    if (!cap.start(make_capture_config(o))) return 1;

    const int seconds = o.seconds > 0 ? o.seconds : 40;
    std::printf("Capturing for %ds. Play normally: walk about, fight something,\n"
                "take damage and spend mana. The inference reads how the numbers\n"
                "move, so it needs to see them move.\n\n", seconds);

    std::thread reporter([&] {
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::lock_guard<std::mutex> lock(mutex);
            std::size_t total = 0;
            for (const auto& [op, e] : obs) total += e.payloads.size();
            std::printf("\r  %zu opcodes, %zu packets   ", obs.size(), total);
            std::fflush(stdout);
        }
    });

    wait_for_stop(seconds);
    g_stop.store(true);
    reporter.join();
    cap.stop();

    std::lock_guard<std::mutex> lock(mutex);

    std::printf("\n\n%-9s %-8s %-8s %-8s %-7s %s\n",
                "opcode", "packets", "C->S", "S->C", "body", "note");
    std::printf("%s\n", std::string(72, '-').c_str());

    std::vector<std::pair<std::uint16_t, const OpcodeObservations*>> rows;
    for (const auto& [op, e] : obs) rows.emplace_back(op, &e);
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second->count() > b.second->count(); });

    for (const auto& [op, e] : rows) {
        std::size_t len = e->payloads.empty() ? 0 : e->payloads.front().size();
        for (const auto& p : e->payloads) len = std::min(len, p.size());

        const char* note = "";
        if (e->payloads.size() < 8) note = "too few samples to judge";
        else if (len < 4)           note = "body too small to hold fields";

        std::printf("0x%04X    %-8zu %-8llu %-8llu %-7zu %s\n",
                    op, e->payloads.size(),
                    static_cast<unsigned long long>(e->to_server),
                    static_cast<unsigned long long>(e->to_client),
                    len, note);
    }

    const auto guesses = analyze_protocol(obs);

    std::printf("\n%s\n", std::string(72, '=').c_str());
    std::uint64_t c2s = 0, s2c = 0;
    for (const auto& [op, e] : obs) {
        c2s += e.to_server;
        s2c += e.to_client;
    }

    if (guesses.empty()) {
        std::printf("\nNothing identified.\n\n");
        std::printf("  client -> server : %llu packets\n"
                    "  server -> client : %llu packets\n\n",
                    static_cast<unsigned long long>(c2s),
                    static_cast<unsigned long long>(s2c));

        if (c2s == 0 && s2c == 0) {
            std::printf("No packets were decoded at all. Either nothing was captured --\n"
                        "check the adapter and filter -- or the framing in %s does not\n"
                        "describe this stream. Check it directly with:\n\n"
                        "  gobot_sniff --record cap.bin --process <game exe>\n"
                        "  gobot_sniff --verify cap.bin.flow0.s2c\n",
                        o.config.c_str());
        } else if (c2s * 20 < s2c) {
            std::printf("The client barely sent anything, which is what an idle character\n"
                        "looks like -- a moving one sends position updates continuously.\n"
                        "Meaning is inferred from how values change, so a recording of a\n"
                        "character standing still contains nothing to recognise.\n\n"
                        "Run it again and keep moving the whole time: walk back and forth,\n"
                        "fight something, take damage, spend mana.\n");
        } else {
            std::printf("There was traffic in both directions, but no field behaved like a\n"
                        "coordinate or a health bar. The values may be packed in a form this\n"
                        "does not look for -- fixed-point, bit-packed, or delta-encoded.\n\n"
                        "Read the busiest opcode by hand:  gobot_sniff --diff 0x%04X\n",
                        rows.empty() ? 0 : rows.front().first);
        }
        return 1;
    }

    std::printf("\n%zu suggestion%s:\n\n", guesses.size(), guesses.size() == 1 ? "" : "s");
    for (const auto& g : guesses) {
        std::printf("  0x%04X  %-16s %3d%%  %s\n", g.opcode, to_string(g.semantic),
                    static_cast<int>(g.confidence * 100),
                    OpcodeRegistry::field_list_to_string(g.fields).c_str());
        std::printf("          %s\n\n", g.reason.c_str());
    }

    std::printf("Add the ones that look right to %s, then --decode to check them.\n",
                o.config.c_str());
    return 0;
}

/// Apply the framing from protocol.ini to a recorded file and report exactly
/// what happens. Answers "is this spec right?" directly, instead of leaving it
/// to be inferred from where the search ranked it.
int cmd_verify(const Options& o) {
    bool have = false;
    const FramingSpec spec = load_framing(o.config, have);
    if (!have) {
        std::fprintf(stderr, "error: no [Framing] section in %s\n", o.config.c_str());
        return 1;
    }

    std::ifstream in(o.file, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "error: cannot read %s\n", o.file.c_str());
        return 1;
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());

    std::printf("\nSpec : %s\n", spec.describe().c_str());
    std::printf("File : %s (%zu bytes)\n\n", o.file.c_str(), data.size());

    auto scan = extract_frames(spec, data.data(), data.size(), Direction::ToClient, 0, false);

    std::size_t body = 0;
    std::map<std::uint16_t, std::size_t> ops;
    for (const auto& p : scan.packets) {
        body += p.payload.size();
        ++ops[p.opcode];
    }

    std::printf("frames        : %zu\n", scan.packets.size());
    std::printf("consumed      : %zu / %zu  (%.1f%%)\n", scan.consumed, data.size(),
                data.size() ? 100.0 * scan.consumed / data.size() : 0.0);
    std::printf("resync bytes  : %zu  (%.1f%%)\n", scan.resyncs,
                data.size() ? 100.0 * scan.resyncs / data.size() : 0.0);
    std::printf("distinct ops  : %zu\n\n", ops.size());

    if (scan.packets.empty()) {
        std::printf("Nothing parsed. This spec does not describe this stream.\n");
        return 1;
    }

    std::vector<std::pair<std::uint16_t, std::size_t>> sorted(ops.begin(), ops.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::printf("%-9s %-8s %s\n", "opcode", "count", "body size");
    for (std::size_t i = 0; i < std::min<std::size_t>(sorted.size(), 15); ++i) {
        std::size_t sz = 0;
        for (const auto& p : scan.packets) {
            if (p.opcode == sorted[i].first) { sz = p.payload.size(); break; }
        }
        std::printf("0x%04X    %-8zu %zu\n", sorted[i].first, sorted[i].second, sz);
    }

    const double pct = data.size() ? 100.0 * scan.resyncs / data.size() : 100.0;
    std::printf("\n%s\n", pct < 25.0
        ? "This spec describes the stream. Occasional resync is normal -- packets\n"
          "get missed, and not every body is described by its length field."
        : "Too much of the stream fails to parse for this spec to be right.");
    (void)body;
    return 0;
}

int cmd_decode(const Options& o) {
    bool have_framing = false;
    const FramingSpec spec = load_framing(o.config, have_framing);
    if (!have_framing) {
        std::fprintf(stderr, "error: no [Framing] section in %s\n", o.config.c_str());
        return 1;
    }

    OpcodeRegistry registry;
    if (!registry.load(o.config) || registry.size() == 0) {
        std::fprintf(stderr,
            "error: no [Packet.*] layouts in %s\n"
            "Use --diff to work out a layout first.\n", o.config.c_str());
        return 1;
    }

    PacketCapture cap;
    cap.set_framing(spec);
    attach_client_framing(cap, o.config);

    std::atomic<std::uint64_t> decoded{0}, skipped{0};

    cap.set_packet_callback([&](const std::vector<GamePacket>& packets) {
        for (const auto& p : packets) {
            if (auto d = registry.decode(p)) {
                decoded.fetch_add(1);
                std::printf("%s\n", d->describe().c_str());
            } else if (registry.known(p.opcode)) {
                skipped.fetch_add(1);
                std::printf("0x%04X FAILED to decode (%zu byte body) — layout too long?\n",
                            p.opcode, p.payload.size());
            }
        }
    });

    if (!cap.start(make_capture_config(o))) return 1;

    std::printf("Decoding with %zu known layouts. Ctrl+C to stop.\n\n", registry.size());
    wait_for_stop(o.seconds);
    cap.stop();

    std::printf("\nDecoded %llu, failed %llu\n",
                static_cast<unsigned long long>(decoded.load()),
                static_cast<unsigned long long>(skipped.load()));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    Options o;
    if (!parse_args(argc, argv, o)) {
        print_usage();
        return 2;
    }

    Logger::instance().set_level(o.verbose ? LogLevel::Debug : LogLevel::Info);

    // Resolve relative paths against the exe so the tool works from any
    // working directory. An absolute path is left alone.
    const bool absolute = o.config.size() > 1 &&
                          (o.config[1] == ':' || o.config[0] == '\\' || o.config[0] == '/');
    if (!absolute) o.config = exe_relative(o.config);

    if (o.command == "list")     return cmd_list();
    if (o.command == "discover") return cmd_discover(o);
    if (o.command == "record") return cmd_record(o);
    if (o.command == "detect") return cmd_detect(o);
    if (o.command == "watch")  return cmd_watch(o);
    if (o.command == "dump")   return cmd_dump(o);
    if (o.command == "diff")   return cmd_diff(o);
    if (o.command == "decode")  return cmd_decode(o);
    if (o.command == "analyze") return cmd_analyze(o);
    if (o.command == "verify")  return cmd_verify(o);

    print_usage();
    return 2;
}
