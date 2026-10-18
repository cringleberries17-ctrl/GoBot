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
//
// gobot_sniff_files is this program built with GOBOT_SNIFF_FILES_ONLY: the
// commands that read recordings, and none that capture. Capturing needs
// administrator, so gobot_sniff asks for it on every start, even for a
// command that only reads a file -- and recordings are read afterwards by
// agents that can never answer that prompt. Built without the capture code,
// the twin asks for nothing, imports nothing from wpcap.dll, and refuses the
// live commands by name.

#include "gobot/byte_reader.hpp"
#include "gobot/config.hpp"
#include "gobot/capture_file.hpp"
#include "gobot/framing.hpp"
#include "gobot/logger.hpp"
#include "gobot/net_discovery.hpp"
#include "gobot/no_real_input.hpp"
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
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace gobot;

// 1 for gobot_sniff_files (CMakeLists.txt): every live command is left out.
#ifndef GOBOT_SNIFF_FILES_ONLY
#define GOBOT_SNIFF_FILES_ONLY 0
#endif

namespace {

/// This program's name, for the commands its hints suggest.
constexpr const char* kProgram = GOBOT_SNIFF_FILES_ONLY ? "gobot_sniff_files" : "gobot_sniff";

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

void print_usage() {
#if GOBOT_SNIFF_FILES_ONLY
    std::printf(R"(gobot_sniff_files — GhostOnline protocol discovery, from recordings

The commands of gobot_sniff that read the files gobot_sniff --record writes.
It captures nothing, so it needs no administrator and asks for none. To
record, or to watch live traffic, use gobot_sniff.

USAGE
  gobot_sniff_files <command> [options]

COMMANDS
  --verify <file>            Tile a recording with the configured framing --
                             [FramingClient] for a .c2s file, [Framing] for
                             any other -- and say how well it fits, including
                             the frames found inside 0x0081.
  --detect <file>            Brute-force the framing of a recorded stream.
  --dump <opcode>            Hexdump every frame of one opcode in --from.
  --diff <opcode>            Show which byte offsets change between successive
                             frames of one opcode in --from: how coordinate,
                             HP and id fields are found. With --versus, compare
                             two recordings.
  --decode                   Decode --from with the layouts in the config.
  --analyze                  Infer what each opcode in --from means. Same
                             engine the GUI's automatic setup uses.

OPTIONS
  --from <file>              The recording --dump, --diff, --decode and
                             --analyze read: one file --record wrote, such as
                             walk.flow0.s2c, walk.c2s or walk.udp.s2c. A name
                             ending .s2c or .c2s says which half it holds;
                             with neither, both framings are tried and the one
                             that tiles is used.
  --versus <file>            For --diff: the recording to compare --from with.
                             Record one standing still and one walking, and
                             the position fields are the ones quiet in the
                             first and busy in the second.
  --inner                    Take the frames found inside 0x0081 instead of
                             those read straight off the stream. The same
                             opcode number need not mean the same thing in
                             both, so the two are never mixed.
  --udp                      Say the file holds UDP. <name>.udp.s2c and
                             <name>.udp.c2s do, and are read as UDP with or
                             without it; with any other file it is an error.
  --config <path>            Protocol config (default: configs/protocol.ini).
  --max <n>                  For --dump: stop after n frames.
  --verbose                  Debug logging.

NOTES
  A .udp file holds the game's datagrams one after another, read as one run
  of frames -- which is what datagrams that hold whole frames read as.
  Refused here, because they capture: --list, --discover, --record, --watch,
  and --dump, --diff, --decode and --analyze without --from.
)");
#else
    std::printf(R"(gobot_sniff — GhostOnline protocol discovery

USAGE
  gobot_sniff <command> [options]

COMMANDS
  --list                     List capture interfaces and exit.
  --discover                 Show which process owns which TCP connection, and
                             the pcap filter that would capture it. Use this
                             instead of netstat: --discover --process Game.exe
                             prints exactly what to pass to --host/--port.
  --record <name>            Record the raw traffic, both directions, TCP and
                             UDP apart: <name>.s2c and <name>.c2s for the TCP
                             stream, <name>.udp.s2c and <name>.udp.c2s for UDP
                             datagrams, <name>.flowN.s2c for each connection.
                             Nothing is framed, filtered or dropped. Refused
                             unless --process, --host, --port or --filter says
                             what to record: with none, it would record every
                             program's traffic on the machine.
  --detect <file>            Brute-force the framing of a recorded stream.
  --verify <file>            Tile a recording with the configured framing --
                             [FramingClient] for a .c2s file, [Framing] for
                             any other -- and say how well it fits, including
                             the frames found inside 0x0081.
  --watch                    Live opcode histogram (needs framing configured).
                             Frames found inside 0x0081, and frames over UDP,
                             are counted in tables of their own.
  --dump <opcode>            Hexdump live packets of one opcode, or with --from
                             the frames of one opcode in a recording.
  --diff <opcode>            Show which byte offsets change between successive
                             packets of one opcode. This is how you locate
                             coordinate, HP and id fields. Add --from to read a
                             recording instead of live traffic.
  --decode                   Decode live traffic with configs/protocol.ini and
                             print the result -- or a recording, with --from.
                             Use to verify a layout.
  --analyze                  Capture, then infer what each opcode means -- or
                             read a recording, with --from. Same engine the
                             GUI's automatic setup uses; prints why each
                             opcode was or was not classified.

OPTIONS
  --process <exe>            For --discover, and to aim a capture: the TCP
                             connections and UDP sockets this executable owns.
                             Asked as Game.exe, only the game's own: another
                             program of that name is left out.
  --iface <name>             Capture interface (default: auto-detect).
  --host <ip>                Game server IP. Narrows the capture filter.
  --port <n>                 Game server port.
  --filter <bpf>             Raw BPF filter, overrides --host/--port/--process.
                             Without it the filter takes TCP and UDP both.
  --config <path>            Protocol config (default: configs/protocol.ini).
  --seconds <n>              Stop after n seconds (default: run until Ctrl+C).
  --max <n>                  For --dump/--diff: stop after n packets.
  --inner                    For --dump/--diff/--decode/--analyze: take the
                             frames found inside 0x0081 instead of those read
                             straight off the stream. The same opcode number
                             need not mean the same thing in both, so the two
                             are never mixed. Works with --from too.
  --udp                      For --dump/--diff/--decode/--analyze, live: take
                             frames that came over UDP instead of TCP. (With
                             --from the file says it: <name>.udp.s2c and
                             <name>.udp.c2s hold UDP, any other file TCP.)
  --from <capture>           For --diff, --dump, --decode and --analyze:
                             analyse a file written by --record instead of
                             capturing. Needs no game running and nothing
                             happening in front of you, and the same bytes can
                             be re-read when a hypothesis changes -- which is
                             the difference between measuring and remembering.
                             A name ending .s2c or .c2s says which direction it
                             holds; otherwise both framings are tried and the
                             one that tiles is used.
                             (Still prompts for Administrator: this program
                             asks for it up front because capture needs it.
                             gobot_sniff_files reads recordings without it.)
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
  GoC throws away opcodes 0x002E and 0x0031 when they come over UDP. Nothing
  here does: a recording keeps every byte, and --watch counts every frame.
  gobot_sniff_files runs the commands that read recordings, without asking
  for administrator.
)");
#endif
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
    bool inner = false;      ///< --inner: frames found inside 0x0081
    bool udp = false;        ///< --udp: frames that came over UDP
};

/// Whether a command takes this frame: by default those read straight off
/// the TCP stream, which is all it ever saw; with --inner and --udp the other
/// kinds instead. One kind at a time, because an opcode number need not mean
/// the same thing inside a wrapper, or over UDP, as on the stream. For a
/// recording, `o` is as_read()'s: the file says whether it is UDP.
bool picked(const GamePacket& p, const Options& o) {
    return p.udp == o.udp && (p.depth > 0) == o.inner;
}

/// The kind of frame a command is taking, for what it prints.
std::string kind_taken(const Options& o) {
    if (o.udp && o.inner) return "frames inside 0x0081 that came over UDP";
    if (o.udp)            return "frames that came over UDP";
    if (o.inner)          return "frames found inside 0x0081";
    return "frames on the TCP stream";
}

#if !GOBOT_SNIFF_FILES_ONLY
/// Where a frame of this kind was found, as where_found() says it.
std::string where_label(bool udp, std::uint8_t depth) {
    GamePacket g;
    g.udp = udp;
    g.depth = depth;
    const std::string w = where_found(g);
    return w.empty() ? "TCP stream" : w;
}
#endif

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
        else if (a == "--inner")    { o.inner = true; }
        else if (a == "--udp")      { o.udp = true; }
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

#if !GOBOT_SNIFF_FILES_ONLY
/// Say so when --process left out programs of the name it was given: asked as
/// Game.exe it takes only the game, by where it runs from. Not where they run
/// from: that can be a folder with somebody's name in it.
void say_passed_over(const SocketOwners& owners, const std::string& process) {
    if (owners.passed_over() == 0) return;
    std::printf("Left out %zu program(s) called %s that do not run from where the game is\n"
                "installed -- another program of that name, or one whose path could not be read.\n",
                owners.passed_over(), process.c_str());
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
        // One for both of the game's socket tables, so that each process is
        // looked up once.
        SocketOwners owners = SocketOwners::here(o.process);
        std::vector<ProcessConnection> conns;
        for (const auto& c : connections_for_process(owners)) {
            if (!c.established) continue;
            const std::uint32_t r = c.remote.ip;
            if (r == 0 || (r >> 24) == 127 || (r >> 16) == 0xA9FE) continue;
            if (c.remote.port == 80 || c.remote.port == 443) continue;
            conns.push_back(c);
            cfg.known_servers.push_back(c.remote);
        }

        if (!conns.empty()) {
            if (cfg.bpf_filter.empty() && cfg.server_ip_start.empty()) {
                // The game's own UDP sockets too, by their local port, and UDP
                // on the servers' ports from any host, as GoC takes it. Whether
                // it talks UDP at all is one of the things a recording is for,
                // and its UDP need not use the port of its TCP connection.
                cfg.bpf_filter = build_filter_for(conns, udp_ports_for_process(owners));
                std::printf("Capturing %s: %s\n", o.process.c_str(), cfg.bpf_filter.c_str());
            } else {
                std::printf("Using %zu server endpoint(s) from %s\n",
                            conns.size(), o.process.c_str());
            }
        } else {
            std::printf("Warning: %s has no game-like connection open.\n", o.process.c_str());
        }
        say_passed_over(owners, o.process);
    }
    return cfg;
}
#endif

// The [Framing] and [FramingClient] blocks are read by gobot::load_framing
// (framing.hpp), the one reader shared with gobot_cli and the tests.

#if !GOBOT_SNIFF_FILES_ONLY
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
#endif

// ------------------------------------------------------------------ commands

#if !GOBOT_SNIFF_FILES_ONLY
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
    // With --process, one SocketOwners for both socket tables, so that each
    // process is looked up once; asked as Game.exe, it takes the game alone.
    std::optional<SocketOwners> owners;
    std::vector<std::uint16_t> udp_ports;
    if (!o.process.empty()) owners.emplace(SocketOwners::here(o.process));
    auto conns = owners ? connections_for_process(*owners) : tcp_connections();
    if (owners) {
        udp_ports = udp_ports_for_process(*owners);
        say_passed_over(*owners, o.process);
    }

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
        if (!udp_ports.empty()) {
            std::printf("\nUDP sockets %s holds, by local port:", o.process.c_str());
            for (std::uint16_t p : udp_ports) std::printf(" %u", static_cast<unsigned>(p));
            std::printf("\n");
        }
        std::printf("\nFilter for these connections, TCP and UDP:\n\n  %s\n",
                    build_filter_for(interesting, udp_ports).c_str());
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
    // What to capture, decided before any file is opened: a refusal must not
    // empty the files of an earlier recording of the same name.
    const CaptureConfig cfg = make_capture_config(o);

    // Nothing named, nothing recorded. With no process, address, port or
    // filter the capture takes every TCP and UDP flow on the machine, and the
    // files would hold other programs' traffic -- a browser's, a chat's --
    // beside the game's, or instead of it.
    if (PacketCapture::captures_everything(cfg)) {
        std::fprintf(stderr,
            "error: nothing says what to record, so this would record every TCP and UDP\n"
            "       flow on the machine -- other programs' traffic with the game's.\n");
        // A --host that is an address narrows the capture, and a --process
        // that finds the game's connections makes the filter unless --host
        // was given -- so whichever was given is where it went wrong.
        if (!o.host.empty())
            std::fprintf(stderr, "       --host %s is not an IPv4 address.\n", o.host.c_str());
        else if (!o.process.empty())
            std::fprintf(stderr,
                "       %s has no game-like connection open, so --process named nothing.\n",
                o.process.c_str());
        std::fprintf(stderr,
            "Name what to record:\n"
            "  --process Game.exe --port 15101   the game's own connections and UDP sockets,\n"
            "                                    and port 15101 when it has none open\n"
            "  --host <server-ip>, --port <n>    one server, one port, or both\n"
            "  --filter \"<bpf>\"                  a capture filter of your own\n");
        return 2;
    }

    // Both directions, to separate files. They are separate protocols as far
    // as framing is concerned — detection has to run on one direction of one
    // connection — and the client->server side is where your own actions live,
    // which makes it the most informative half for working out what means what.
    const std::string s2c_path = o.file + ".s2c";
    const std::string c2s_path = o.file + ".c2s";

    // UDP in files of its own. A TCP file is one byte stream, cut wherever
    // the segments fell; a UDP file is the game's datagrams one after another,
    // each of which GoC reads as whole frames from its first byte. Mixed into
    // one file, neither would tile.
    const std::string udp_s2c_path = o.file + ".udp.s2c";
    const std::string udp_c2s_path = o.file + ".udp.c2s";

    std::ofstream out(s2c_path, std::ios::binary | std::ios::trunc);
    std::ofstream out_c2s(c2s_path, std::ios::binary | std::ios::trunc);
    std::ofstream out_udp(udp_s2c_path, std::ios::binary | std::ios::trunc);
    std::ofstream out_udp_c2s(udp_c2s_path, std::ios::binary | std::ios::trunc);
    if (!out || !out_c2s || !out_udp || !out_udp_c2s) {
        std::fprintf(stderr, "error: cannot write %s / %s / %s / %s\n",
                     s2c_path.c_str(), c2s_path.c_str(),
                     udp_s2c_path.c_str(), udp_c2s_path.c_str());
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

    // Every datagram whole, in the order it arrived. Nothing is dropped:
    // GoC throws away 0x002E and 0x0031 when they come over UDP, and what
    // those are is for this recording to show.
    std::atomic<std::uint64_t> udp_bytes{0};
    std::atomic<std::uint64_t> udp_bytes_c2s{0};
    std::atomic<std::uint64_t> udp_datagrams{0};
    cap.set_raw_udp_tap([&](const FlowKey&, Direction dir,
                            const std::uint8_t* data, std::size_t size) {
        std::lock_guard<std::mutex> lock(write_mutex);
        std::ofstream& to = (dir == Direction::ToClient) ? out_udp : out_udp_c2s;
        to.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        (dir == Direction::ToClient ? udp_bytes : udp_bytes_c2s).fetch_add(size);
        udp_datagrams.fetch_add(1);
    });

    if (!cap.start(cfg)) return 1;

    std::printf("Recording to %s.*: the TCP stream and UDP datagrams, both directions.\n",
                o.file.c_str());
    if (o.seconds > 0)
        std::printf("Play the game normally. It stops by itself after %d s, or on Ctrl+C.\n\n",
                    o.seconds);
    else
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
            std::printf("\r  S->C %llu B | C->S %llu B | UDP %llu B in %llu | %llu packets | "
                        "%llu segs | dropped %llu   ",
                        static_cast<unsigned long long>(bytes.load()),
                        static_cast<unsigned long long>(bytes_c2s.load()),
                        static_cast<unsigned long long>(udp_bytes.load() + udp_bytes_c2s.load()),
                        static_cast<unsigned long long>(udp_datagrams.load()),
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
    out_udp.close();
    out_udp_c2s.close();

    std::printf("\n\nserver -> client : %llu bytes  ->  %s\n"
                "client -> server : %llu bytes  ->  %s\n",
                static_cast<unsigned long long>(bytes.load()), s2c_path.c_str(),
                static_cast<unsigned long long>(bytes_c2s.load()), c2s_path.c_str());
    std::printf("UDP, server -> client : %llu bytes  ->  %s\n"
                "UDP, client -> server : %llu bytes  ->  %s\n"
                "                        %llu datagram%s in all%s\n",
                static_cast<unsigned long long>(udp_bytes.load()), udp_s2c_path.c_str(),
                static_cast<unsigned long long>(udp_bytes_c2s.load()), udp_c2s_path.c_str(),
                static_cast<unsigned long long>(udp_datagrams.load()),
                udp_datagrams.load() == 1 ? "" : "s",
                udp_datagrams.load() == 0 ? " -- the game sent no UDP that the filter let in" : "");
    if (cap.stats().udp_fragments > 0)
        std::printf("!! %llu UDP packet(s) were pieces of a datagram that IP split up. Nothing\n"
                    "   here puts the pieces back together, so they are NOT in the .udp files.\n",
                    static_cast<unsigned long long>(cap.stats().udp_fragments));
    if (cap.stats().udp_by_port > 0)
        std::printf("!! %llu datagram(s) had neither a server of the game's nor this machine at\n"
                    "   just one end, so only their port numbers said which .udp file they went\n"
                    "   to: they may be in the wrong one, as the other direction.\n",
                    static_cast<unsigned long long>(cap.stats().udp_by_port));

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
                    "  - --host matches the game server (--process Game.exe finds it)\n");
    } else if (bytes_c2s.load() < 256) {
        std::printf("\nBarely anything went client -> server. That half carries your own\n"
                    "actions, and it is the clearest evidence there is for working out\n"
                    "which packet holds your position. If the character stood still for\n"
                    "this capture, record again while walking around.\n");
    } else {
        std::printf("\nNext: gobot_sniff --detect %s\n",
                    busiest.empty() ? s2c_path.c_str() : busiest.c_str());
        std::printf("Or, with the framing known, see what it holds, inside 0x0081 too:\n"
                    "      gobot_sniff --verify %s\n",
                    busiest.empty() ? s2c_path.c_str() : busiest.c_str());
    }
    if (udp_bytes.load() > 0)
        std::printf("And the UDP the server sent: gobot_sniff --verify %s\n", udp_s2c_path.c_str());
    std::printf("gobot_sniff_files runs --verify, --detect and the --from commands on these\n"
                "files without asking for administrator.\n");
    return 0;
}
#endif

/// Whether --udp, given with a recording, fits it. A recording says for
/// itself whether it holds UDP (capture_file::udp_from_name); --udp with one
/// that holds the TCP stream is a mistake worth stopping on, not ignoring.
bool udp_fits(const Options& o, const std::string& path) {
    if (!o.udp || capture_file::udp_from_name(path)) return true;
    std::fprintf(stderr, "error: --udp, but %s holds the TCP stream. A recording's UDP is in\n"
                         "       <name>.udp.s2c and <name>.udp.c2s.\n", path.c_str());
    return false;
}

/// `o` as it applies to a recording: UDP when the file holds UDP, whatever
/// --udp said, so that picked() and kind_taken() read it as they read a
/// live capture.
Options as_read(const Options& o, const std::string& path) {
    Options r = o;
    r.udp = capture_file::udp_from_name(path);
    return r;
}

int cmd_detect(const Options& o) {
    if (!udp_fits(o, o.file)) return 1;

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
    std::printf("\nNext: add the [Framing] block above, then run %s\n",
                GOBOT_SNIFF_FILES_ONLY ? "--verify on a recording" : "--watch");
    return 0;
}

#if !GOBOT_SNIFF_FILES_ONLY
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

    // Frames found inside 0x0081, and frames that came over UDP, counted per
    // opcode in tables of their own: by (over UDP, depth, opcode). The same
    // opcode number need not mean the same thing in each, so none of them is
    // added to the counts above, which stay what they always were.
    struct Other { std::uint64_t count = 0; std::size_t last = 0; };
    using OtherKey = std::tuple<bool, std::uint8_t, std::uint16_t>;
    std::map<OtherKey, Other> others;

    cap.set_packet_callback([&](const std::vector<GamePacket>& packets) {
        std::lock_guard<std::mutex> lock(counts_mutex);
        for (const auto& p : packets) {
            if (!p.plain()) {
                Other& r = others[OtherKey{ p.udp, p.depth, p.opcode }];
                ++r.count;
                r.last = p.payload.size();
                continue;
            }
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
            std::vector<std::pair<OtherKey, Other>> other_rows;
            {
                std::lock_guard<std::mutex> lock(counts_mutex);
                sorted.assign(counts.begin(), counts.end());
                size_copy = sizes;
                other_rows.assign(others.begin(), others.end());
            }
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            std::sort(other_rows.begin(), other_rows.end(),
                      [](const auto& a, const auto& b) { return a.second.count > b.second.count; });

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

            // What was inside 0x0081 and what came over UDP, per opcode. None
            // of it reaches the bot yet (plan item P0-3); the layouts in
            // protocol.ini were written for the frames above, so the "mapped
            // as" column is not repeated here.
            std::uint64_t inner_total = 0, udp_total = 0;
            for (const auto& [key, r] : other_rows) {
                if (std::get<1>(key) > 0) inner_total += r.count;
                else                      udp_total   += r.count;
            }
            if (inner_total > 0 || s.unwrap.unopened || s.unwrap.unreadable || s.unwrap.too_deep) {
                std::printf("\n--- inside 0x0081: %llu frames (counted only; the bot does not use them) ---\n",
                            static_cast<unsigned long long>(inner_total));
                std::printf("%-10s %-10s %-8s %s\n", "opcode", "count", "last", "found");
                std::size_t shown = 0;
                for (const auto& [key, r] : other_rows) {
                    if (std::get<1>(key) == 0) continue;
                    if (++shown > 25) break;
                    std::printf("0x%04X     %-10llu %-8zuB %s\n",
                                std::get<2>(key), static_cast<unsigned long long>(r.count), r.last,
                                where_label(std::get<0>(key), std::get<1>(key)).c_str());
                }
                std::printf("  left shut, marker 0x0020: %llu | unreadable insides: %llu | "
                            "past depth %u: %llu\n",
                            static_cast<unsigned long long>(s.unwrap.unopened),
                            static_cast<unsigned long long>(s.unwrap.unreadable),
                            static_cast<unsigned>(kMaxWrapDepth),
                            static_cast<unsigned long long>(s.unwrap.too_deep));
                std::printf("  own length not the room they fill: %llu | header check word off: %llu\n",
                            static_cast<unsigned long long>(s.unwrap.length_differs),
                            static_cast<unsigned long long>(s.unwrap.checksum_bad));
            }
            if (s.udp_datagrams > 0) {
                std::printf("\n--- over UDP: %llu datagrams, %llu frames, %llu bytes unread, "
                            "%llu fragments left, %llu directions told by port alone ---\n",
                            static_cast<unsigned long long>(s.udp_datagrams),
                            static_cast<unsigned long long>(s.udp_frames),
                            static_cast<unsigned long long>(s.udp_unread_bytes),
                            static_cast<unsigned long long>(s.udp_fragments),
                            static_cast<unsigned long long>(s.udp_by_port));
                if (udp_total > 0) {
                    std::printf("%-10s %-10s %-8s\n", "opcode", "count", "last");
                    std::size_t shown = 0;
                    for (const auto& [key, r] : other_rows) {
                        if (std::get<1>(key) != 0) continue;
                        if (++shown > 25) break;
                        std::printf("0x%04X     %-10llu %-8zuB\n",
                                    std::get<2>(key), static_cast<unsigned long long>(r.count), r.last);
                    }
                }
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
#endif

/// One frame as --dump shows it, live or from a recording: which it is,
/// where it was found, and its bytes.
void print_dumped(const GamePacket& p, int n) {
    const std::string where = where_found(p);
    std::printf("\n--- #%d  opcode 0x%04X  %zu bytes%s%s ---\n", n, p.opcode,
                p.payload.size(), where.empty() ? "" : ", ", where.c_str());
    std::printf("%s", hexdump(p.payload, 16, 2).c_str());
}

#if !GOBOT_SNIFF_FILES_ONLY
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
    std::atomic<std::uint64_t> passed_by{0};      // the other kinds, for the closing line

    cap.set_packet_callback([&](const std::vector<GamePacket>& packets) {
        for (const auto& p : packets) {
            if (p.opcode != o.opcode) continue;
            if (!picked(p, o)) { passed_by.fetch_add(1); continue; }
            const int n = seen.fetch_add(1) + 1;
            print_dumped(p, n);
            if (o.max > 0 && n >= o.max) g_stop.store(true);
        }
    });

    if (!cap.start(make_capture_config(o))) return 1;

    std::printf("Dumping opcode 0x%04X, %s. Ctrl+C to stop.\n", o.opcode, kind_taken(o).c_str());
    wait_for_stop(o.seconds);
    cap.stop();

    std::printf("\nCaptured %d packets.\n", seen.load());
    if (passed_by.load() > 0)
        std::printf("%llu more of opcode 0x%04X went by that were not %s.\n"
                    "--inner and --udp take the others.\n",
                    static_cast<unsigned long long>(passed_by.load()), o.opcode,
                    kind_taken(o).c_str());
    return 0;
}
#endif

/// Read every frame of a recorded stream, and say how it was read.
///
/// A recording's name usually says which direction it holds, and the two are
/// framed differently; when it says neither, both specs are tried and the one
/// that tiles is used. That choice is reported: a file analysed under the
/// wrong spec yields plausible nonsense rather than an obvious failure. The
/// frames of a .udp recording are UDP ones. `ok` is false when the file could
/// not be read, which has been said; end_reading() finishes what is said.
capture_file::Reading read_recording(const std::string& path, const Options& o,
                                     std::vector<GamePacket>& out) {
    capture_file::Reading r;
    if (!udp_fits(o, path)) return r;
    bool have_server = false, have_client = false;
    const FramingSpec server = load_framing(o.config, have_server);
    const FramingSpec client = load_framing(o.config, have_client, "FramingClient");
    if (!have_server) {
        std::fprintf(stderr, "error: no [Framing] section in %s\n", o.config.c_str());
        return r;
    }

    r = capture_file::read_all(path, server, have_client ? client : FramingSpec{}, out);
    if (!r.ok) {
        std::fprintf(stderr, "error: %s\n", r.error.c_str());
        return r;
    }

    const char* half = (r.half == capture_file::Half::Client)
                           ? "[FramingClient] (client -> server)"
                           : "[Framing] (server -> client)";
    if (r.decided_by == capture_file::HalfSource::Name)
        std::printf("  named %s, so reading it as %s\n",
                    (r.half == capture_file::Half::Client) ? ".c2s" : ".s2c", half);
    else
        std::printf("  no .s2c/.c2s in the name; read as %s\n", half);
    if (capture_file::udp_from_name(path))
        std::printf("  UDP: the game's datagrams, one after another\n");
    std::printf("  %-36s %5zu frames, %6zu resync bytes\n",
                half, r.frames, r.resyncs);

    // Frames inside 0x0081 are taken with --inner and never mixed with the
    // others: say what was there either way.
    if (r.inner > 0)
        std::printf("  and %zu frame(s) inside 0x0081 -- %s\n", r.inner,
                    o.inner ? "the ones taken here (--inner)" : "--inner takes those instead");
    return r;
}

/// The end of what read_recording() says: a warning when the reading looks
/// wrong.
void end_reading(const capture_file::Reading& r) {
    std::printf("\n");
    if (r.resyncs > r.frames) {
        std::printf("  !! more resync bytes than frames. Either this is the other\n"
                    "     direction, the framing is wrong, or the capture lost\n"
                    "     packets. Treat anything below with suspicion.\n\n");
    }
}

/// Read a recorded stream and pull out every payload of one opcode, of the
/// kind asked for: straight off the stream, or with --inner from inside
/// 0x0081.
bool samples_from_file(const std::string& path, const Options& o, std::vector<Bytes>& out) {
    std::vector<GamePacket> frames;
    const capture_file::Reading r = read_recording(path, o, frames);
    if (!r.ok) return false;

    const Options taken = as_read(o, path);
    std::size_t other_kind = 0;
    for (const GamePacket& p : frames) {
        if (p.opcode != o.opcode) continue;
        if (picked(p, taken)) out.push_back(p.payload);
        else                  ++other_kind;
    }
    if (other_kind > 0)
        std::printf("  %zu of opcode 0x%04X left out, being %s\n", other_kind, o.opcode,
                    o.inner ? "straight off the stream" : "inside 0x0081");
    end_reading(r);
    return true;
}

/// --dump --from: every frame of one opcode in a recording, shown as --dump
/// shows them live.
int dump_file(const Options& o) {
    std::printf("Reading %s\n", o.from.c_str());
    std::vector<GamePacket> frames;
    const capture_file::Reading r = read_recording(o.from, o, frames);
    if (!r.ok) return 1;
    end_reading(r);

    const Options taken = as_read(o, o.from);
    std::printf("Dumping opcode 0x%04X, %s.\n", o.opcode, kind_taken(taken).c_str());
    int found = 0, shown = 0;
    std::size_t passed_by = 0;
    for (const GamePacket& p : frames) {
        if (p.opcode != o.opcode) continue;
        if (!picked(p, taken)) { ++passed_by; continue; }
        ++found;
        if (o.max <= 0 || shown < o.max) print_dumped(p, ++shown);
    }

    std::printf("\n%d frame(s) of opcode 0x%04X in the recording", found, o.opcode);
    if (shown < found) std::printf(", the first %d shown (--max)", shown);
    std::printf(".\n");
    if (passed_by > 0) {
        Options other = taken;
        other.inner = !taken.inner;
        std::printf("%zu more of opcode 0x%04X were %s; %s takes those.\n", passed_by,
                    o.opcode, kind_taken(other).c_str(),
                    o.inner ? "leaving out --inner" : "--inner");
    }
    return 0;
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
    if (!samples_from_file(o.from, o, a)) return 1;
    std::printf("B: %s\n", o.versus.c_str());
    if (!samples_from_file(o.versus, o, b)) return 1;

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

/// Byte-offset volatility analysis.
///
/// The core discovery technique: capture many instances of one opcode and see
/// which offsets change. An entity id is constant across updates for the same
/// entity; a coordinate changes smoothly as you walk; HP changes only when you
/// take damage. The change *pattern* identifies the field far more reliably
/// than staring at hex.
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
        if (!samples_from_file(o.from, o, samples)) return 1;
        std::printf("%zu packets of opcode 0x%04X in the recording%s.\n",
                    samples.size(), o.opcode, o.inner ? ", inside 0x0081" : "");
    } else {
#if GOBOT_SNIFF_FILES_ONLY
        // Never reached: main() refuses --diff without --from here, as it
        // captures.
        return 2;
#else
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
        std::uint64_t passed_by = 0;       // the other kinds of this opcode

        cap.set_packet_callback([&](const std::vector<GamePacket>& packets) {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& p : packets) {
                if (p.opcode != o.opcode) continue;
                if (!picked(p, o)) { ++passed_by; continue; }
                samples.push_back(p.payload);
                if (static_cast<int>(samples.size()) >= want) g_stop.store(true);
            }
        });

        if (!cap.start(make_capture_config(o))) return 1;

        std::printf("Collecting up to %d packets of opcode 0x%04X, %s.\n", want, o.opcode,
                    kind_taken(o).c_str());
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

        if (passed_by > 0)
            std::printf("\n%llu more of opcode 0x%04X went by that were not %s;\n"
                        "--inner and --udp take the others.\n",
                        static_cast<unsigned long long>(passed_by), o.opcode,
                        kind_taken(o).c_str());
#endif
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
    if (o.from.empty())
        std::printf("Then: gobot_sniff --decode\n");
    else
        std::printf("Then: %s --decode%s --from %s\n", kProgram, o.inner ? " --inner" : "",
                    o.from.c_str());
    return 0;
}

/// One frame into what the inference reads: per opcode, which way it went,
/// and its first 256 bodies.
void observe(std::map<std::uint16_t, OpcodeObservations>& obs, const GamePacket& p) {
    auto& e = obs[p.opcode];
    if (p.from_server) ++e.to_client; else ++e.to_server;
    if (e.payloads.size() < 256) {
        e.payloads.push_back(p.payload);
        e.timestamps_ms.push_back(p.timestamp_ms);
    }
}

int report_analysis(const Options& o, const std::map<std::uint16_t, OpcodeObservations>& obs,
                    std::uint64_t passed_by);

#if !GOBOT_SNIFF_FILES_ONLY
/// Capture, then run the same inference the GUI's automatic setup runs.
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
    std::uint64_t passed_by = 0;     // frames of the other kinds, left out

    // One kind of frame at a time (--inner, --udp): the inference works per
    // opcode, and a layout it suggests goes into protocol.ini, whose layouts
    // the bot applies to frames off the TCP stream only.
    cap.set_packet_callback([&](const std::vector<GamePacket>& packets) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& p : packets) {
            if (!picked(p, o)) { ++passed_by; continue; }
            observe(obs, p);
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
    return report_analysis(o, obs, passed_by);
}
#endif

/// --analyze --from: the same inference, on a recording.
int analyze_file(const Options& o) {
    std::printf("Reading %s\n", o.from.c_str());
    std::vector<GamePacket> frames;
    const capture_file::Reading r = read_recording(o.from, o, frames);
    if (!r.ok) return 1;
    end_reading(r);

    const Options taken = as_read(o, o.from);
    std::map<std::uint16_t, OpcodeObservations> obs;
    std::uint64_t passed_by = 0;
    for (const GamePacket& p : frames) {
        if (!picked(p, taken)) { ++passed_by; continue; }
        observe(obs, p);
    }
    return report_analysis(taken, obs, passed_by);
}

/// What --analyze prints, live or from a recording: the per-opcode evidence
/// alongside the verdicts, because "found nothing" is the failure that needs
/// explaining most and a bare empty list explains nothing at all.
int report_analysis(const Options& o, const std::map<std::uint16_t, OpcodeObservations>& obs,
                    std::uint64_t passed_by) {
    // Live, the line before is the progress count, left without its newline.
    std::printf("%sRead: %s.", o.from.empty() ? "\n\n" : "", kind_taken(o).c_str());
    if (passed_by > 0 && o.from.empty())
        std::printf(" %llu frames of the other kinds were left out; --inner and --udp\n"
                    "take them instead.", static_cast<unsigned long long>(passed_by));
    else if (passed_by > 0)
        std::printf(" %llu frames of the other kind were left out; %s takes them\n"
                    "instead.", static_cast<unsigned long long>(passed_by),
                    o.inner ? "leaving out --inner" : "--inner");
    std::printf("\n");

    std::printf("\n%-9s %-8s %-8s %-8s %-7s %s\n",
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

        // A recording holds one direction, so what the two counts say about
        // the client is said only of a live capture, which sees both.
        if (c2s == 0 && s2c == 0 && !o.from.empty()) {
            std::printf("No frames of this kind were read. Either the recording holds none, or\n"
                        "the framing in %s does not describe it. Check it directly with:\n\n"
                        "  %s --verify %s\n",
                        o.config.c_str(), kProgram, o.from.c_str());
        } else if (c2s == 0 && s2c == 0) {
            std::printf("No packets were decoded at all. Either nothing was captured --\n"
                        "check the adapter and filter -- or the framing in %s does not\n"
                        "describe this stream. Check it directly with:\n\n"
                        "  gobot_sniff --record cap.bin --process <game exe>\n"
                        "  gobot_sniff --verify cap.bin.flow0.s2c\n",
                        o.config.c_str());
        } else if (o.from.empty() && c2s * 20 < s2c) {
            std::printf("The client barely sent anything, which is what an idle character\n"
                        "looks like -- a moving one sends position updates continuously.\n"
                        "Meaning is inferred from how values change, so a recording of a\n"
                        "character standing still contains nothing to recognise.\n\n"
                        "Run it again and keep moving the whole time: walk back and forth,\n"
                        "fight something, take damage, spend mana.\n");
        } else {
            std::printf("There was traffic%s, but no field behaved like a\n"
                        "coordinate or a health bar. The values may be packed in a form this\n"
                        "does not look for -- fixed-point, bit-packed, or delta-encoded.\n\n"
                        "Read the busiest opcode by hand:  %s --diff 0x%04X%s%s%s%s\n",
                        o.from.empty() ? " in both directions" : "", kProgram,
                        rows.empty() ? 0 : rows.front().first, o.inner ? " --inner" : "",
                        o.udp && o.from.empty() ? " --udp" : "",
                        o.from.empty() ? "" : " --from ", o.from.c_str());
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

    if (o.inner || o.udp) {
        // The bot applies protocol.ini's [Packet.*] layouts to frames off the
        // TCP stream only. One inferred from these would never decode the
        // frames it came from, and would take over the stream frames that
        // share its opcode number -- which need not mean the same thing:
        // 0x0014 inside 0x0081 is not 0x0014 outside it (protocol-findings §4d).
        std::printf("These describe %s. The bot does not decode those yet\n"
                    "(P0-3 in docs/goc-parity-plan.md), and it applies [Packet.*] layouts\n"
                    "to frames off the TCP stream only: do not add these to %s\n"
                    "as they stand, or they will be applied to the stream frames that share\n"
                    "the opcode number.\n",
                    kind_taken(o).c_str(), o.config.c_str());
    } else {
        std::printf("Add the ones that look right to %s, then --decode to check them.\n",
                    o.config.c_str());
    }
    return 0;
}

/// Apply the framing from protocol.ini to a recorded file and report exactly
/// what happens. Answers "is this spec right?" directly, instead of leaving it
/// to be inferred from where the search ranked it.
int cmd_verify(const Options& o) {
    if (!udp_fits(o, o.file)) return 1;
    bool have = false, have_client = false;
    const FramingSpec server = load_framing(o.config, have);
    const FramingSpec client = load_framing(o.config, have_client, "FramingClient");
    if (!have) {
        std::fprintf(stderr, "error: no [Framing] section in %s\n", o.config.c_str());
        return 1;
    }

    // The client's half is framed differently -- its opcode at offset 6, not
    // 2 -- so a .c2s recording, TCP or UDP, is tiled with [FramingClient], as
    // --from reads it. Under [Framing] it would be nothing but resyncs.
    capture_file::Half half = capture_file::Half::Server;
    const bool client_half =
        capture_file::half_from_name(o.file, half) && half == capture_file::Half::Client;
    if (client_half && !have_client)
        std::printf("note: no [FramingClient] in %s, so this client half is read with\n"
                    "      [Framing], which frames it wrong.\n", o.config.c_str());
    const bool as_client = client_half && have_client;
    const FramingSpec& spec = as_client ? client : server;

    std::ifstream in(o.file, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "error: cannot read %s\n", o.file.c_str());
        return 1;
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());

    std::printf("\nSpec : %s %s\n", as_client ? "[FramingClient]" : "[Framing]",
                spec.describe().c_str());
    std::printf("File : %s (%zu bytes)%s\n\n", o.file.c_str(), data.size(),
                capture_file::udp_from_name(o.file) ? ", UDP datagrams one after another" : "");
    if (data.empty()) {
        // Not a framing that fails: nothing was recorded in this file at all,
        // which for a .udp file is what a game that sends no UDP leaves.
        std::printf("Nothing to tile: the file is empty.%s\n",
                    capture_file::udp_from_name(o.file)
                        ? " The recording caught no UDP in this direction." : "");
        return 1;
    }

    // The same reading the tests check a real capture with.
    const FrameScanResult scan = capture_file::tile(
        data, spec, as_client ? Direction::ToServer : Direction::ToClient);

    // Frames off the stream, as this has always listed them; those found
    // inside 0x0081 are listed apart, by depth and opcode, below.
    std::size_t body = 0;
    std::map<std::uint16_t, std::size_t> ops;
    struct Inner { std::size_t count = 0, min_size = SIZE_MAX, max_size = 0; };
    std::map<std::pair<std::uint8_t, std::uint16_t>, Inner> inner;
    for (const auto& p : scan.packets) {
        if (p.depth > 0) {
            Inner& row = inner[{ p.depth, p.opcode }];
            ++row.count;
            row.min_size = std::min(row.min_size, p.payload.size());
            row.max_size = std::max(row.max_size, p.payload.size());
            continue;
        }
        body += p.payload.size();
        ++ops[p.opcode];
    }

    std::printf("frames        : %zu\n", scan.outer_frames());
    std::printf("consumed      : %zu / %zu  (%.1f%%)\n", scan.consumed, data.size(),
                data.size() ? 100.0 * scan.consumed / data.size() : 0.0);
    std::printf("resync bytes  : %zu  (%.1f%%)\n", scan.resyncs,
                data.size() ? 100.0 * scan.resyncs / data.size() : 0.0);
    std::printf("distinct ops  : %zu\n", ops.size());
    std::printf("inside 0x0081 : %zu frames%s\n\n", scan.unwrap.inner,
                spec.unwraps() ? "" : "  (this spec does not look inside wrappers)");

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
            if (p.depth == 0 && p.opcode == sorted[i].first) { sz = p.payload.size(); break; }
        }
        std::printf("0x%04X    %-8zu %zu\n", sorted[i].first, sorted[i].second, sz);
    }

    const UnwrapCounts& u = scan.unwrap;
    if (!inner.empty() || u.unopened || u.unreadable || u.too_deep) {
        std::printf("\nInside 0x0081 (each wrapper also counted above, as itself):\n");
        std::printf("%-9s %-6s %-8s %s\n", "opcode", "depth", "count", "body size");
        for (const auto& [key, row] : inner) {
            if (row.min_size == row.max_size)
                std::printf("0x%04X    %-6u %-8zu %zu\n", key.second,
                            static_cast<unsigned>(key.first), row.count, row.max_size);
            else
                std::printf("0x%04X    %-6u %-8zu %zu-%zu\n", key.second,
                            static_cast<unsigned>(key.first), row.count, row.min_size, row.max_size);
        }
        std::printf("left shut, marker 0x0020    : %zu\n", u.unopened);
        std::printf("unreadable insides          : %zu\n", u.unreadable);
        std::printf("not opened, past depth %u    : %zu\n",
                    static_cast<unsigned>(kMaxWrapDepth), u.too_deep);
        std::printf("own length not their room   : %zu\n", u.length_differs);
        std::printf("header check word off       : %zu of %zu\n", u.checksum_bad, u.inner);
    }

    const double pct = data.size() ? 100.0 * scan.resyncs / data.size() : 100.0;
    std::printf("\n%s\n", pct < 25.0
        ? "This spec describes the stream. Occasional resync is normal -- packets\n"
          "get missed, and not every body is described by its length field."
        : "Too much of the stream fails to parse for this spec to be right.");
    (void)body;
    return 0;
}

/// The [Packet.*] layouts in the config, for --decode: false, and said, when
/// there are none.
bool load_layouts(const Options& o, OpcodeRegistry& registry) {
    if (!registry.load(o.config) || registry.size() == 0) {
        std::fprintf(stderr,
            "error: no [Packet.*] layouts in %s\n"
            "Use --diff to work out a layout first.\n", o.config.c_str());
        return false;
    }
    return true;
}

/// What came of one frame through the layouts.
enum class Decoded { Read, Failed, NoLayout };

/// One frame through the layouts, printed as --decode prints it, live or
/// from a recording: labelled with where it was found, when that is not the
/// TCP stream.
Decoded print_decoded(const OpcodeRegistry& registry, const GamePacket& p) {
    const std::string where = where_found(p);
    const std::string tag = where.empty() ? "" : " [" + where + "]";
    if (auto d = registry.decode(p)) {
        std::printf("%s%s\n", d->describe().c_str(), tag.c_str());
        return Decoded::Read;
    }
    if (!registry.known(p.opcode)) return Decoded::NoLayout;
    std::printf("0x%04X FAILED to decode (%zu byte body) — layout too long?%s\n",
                p.opcode, p.payload.size(), tag.c_str());
    return Decoded::Failed;
}

/// Said before decoding frames found inside 0x0081 or over UDP.
void say_not_for_the_bot(const Options& o) {
    if (o.inner || o.udp)
        std::printf("Note: the bot applies these layouts only to frames off the TCP stream,\n"
                    "where an opcode number need not mean what it means here. A layout that\n"
                    "reads %s right is not, by that, right for the bot --\n"
                    "which does not decode those yet (P0-3 in docs/goc-parity-plan.md).\n\n",
                    kind_taken(o).c_str());
}

#if !GOBOT_SNIFF_FILES_ONLY
int cmd_decode(const Options& o) {
    bool have_framing = false;
    const FramingSpec spec = load_framing(o.config, have_framing);
    if (!have_framing) {
        std::fprintf(stderr, "error: no [Framing] section in %s\n", o.config.c_str());
        return 1;
    }

    OpcodeRegistry registry;
    if (!load_layouts(o, registry)) return 1;

    PacketCapture cap;
    cap.set_framing(spec);
    attach_client_framing(cap, o.config);

    std::atomic<std::uint64_t> decoded{0}, skipped{0}, passed_by{0};

    // The layouts were written for frames off the TCP stream, and the same
    // opcode number need not mean the same thing inside 0x0081 or over UDP:
    // those are decoded only when asked for, and labelled.
    cap.set_packet_callback([&](const std::vector<GamePacket>& packets) {
        for (const auto& p : packets) {
            if (!picked(p, o)) { passed_by.fetch_add(1); continue; }
            const Decoded d = print_decoded(registry, p);
            if (d == Decoded::Read)   decoded.fetch_add(1);
            if (d == Decoded::Failed) skipped.fetch_add(1);
        }
    });

    if (!cap.start(make_capture_config(o))) return 1;

    std::printf("Decoding %s with %zu known layouts. Ctrl+C to stop.\n\n",
                kind_taken(o).c_str(), registry.size());
    say_not_for_the_bot(o);
    wait_for_stop(o.seconds);
    cap.stop();

    std::printf("\nDecoded %llu, failed %llu\n",
                static_cast<unsigned long long>(decoded.load()),
                static_cast<unsigned long long>(skipped.load()));
    if (passed_by.load() > 0)
        std::printf("%llu frames of the other kinds went by undecoded; --inner and --udp\n"
                    "take them instead.\n", static_cast<unsigned long long>(passed_by.load()));
    return 0;
}
#endif

/// --decode --from: a recording through the layouts.
int decode_file(const Options& o) {
    OpcodeRegistry registry;
    if (!load_layouts(o, registry)) return 1;

    std::printf("Reading %s\n", o.from.c_str());
    std::vector<GamePacket> frames;
    const capture_file::Reading r = read_recording(o.from, o, frames);
    if (!r.ok) return 1;
    end_reading(r);

    const Options taken = as_read(o, o.from);
    std::printf("Decoding %s with %zu known layouts.\n\n", kind_taken(taken).c_str(),
                registry.size());
    say_not_for_the_bot(taken);

    std::uint64_t decoded = 0, failed = 0, passed_by = 0;
    for (const GamePacket& p : frames) {
        if (!picked(p, taken)) { ++passed_by; continue; }
        const Decoded d = print_decoded(registry, p);
        if (d == Decoded::Read)   ++decoded;
        if (d == Decoded::Failed) ++failed;
    }

    std::printf("\nDecoded %llu, failed %llu\n", static_cast<unsigned long long>(decoded),
                static_cast<unsigned long long>(failed));
    if (passed_by > 0)
        std::printf("%llu frames of the other kind were not decoded; %s takes them instead.\n",
                    static_cast<unsigned long long>(passed_by),
                    o.inner ? "leaving out --inner" : "--inner");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
#if GOBOT_SNIFF_FILES_ONLY
    // Before anything else, and never turned off. This program reads files
    // and prints; should anything in it ever reach for a key, the pointer,
    // the focus, or start or end a program, that is refused instead.
    forbid_real_input();
#endif
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

    // What reads files, in both programs.
    if (o.command == "detect") return cmd_detect(o);
    if (o.command == "verify") return cmd_verify(o);
    if (!o.from.empty()) {
        if (o.command == "dump")    return dump_file(o);
        if (o.command == "diff")    return cmd_diff(o);
        if (o.command == "decode")  return decode_file(o);
        if (o.command == "analyze") return analyze_file(o);
    }

#if GOBOT_SNIFF_FILES_ONLY
    // Everything else captures, and is refused by name rather than tried.
    const bool has_file_form = o.command == "dump" || o.command == "diff" ||
                               o.command == "decode" || o.command == "analyze";
    std::fprintf(stderr,
        "error: --%s%s works on the live network, and gobot_sniff_files reads\n"
        "       recordings only. Use gobot_sniff for it: it asks for administrator, as\n"
        "       capturing needs. Here: --verify <file>, --detect <file>, or --dump,\n"
        "       --diff, --decode or --analyze with --from <recording>.\n",
        o.command.c_str(), has_file_form ? " without --from" : "");
    return 2;
#else
    if (o.command == "list")     return cmd_list();
    if (o.command == "discover") return cmd_discover(o);
    if (o.command == "record")   return cmd_record(o);
    if (o.command == "watch")    return cmd_watch(o);
    if (o.command == "dump")     return cmd_dump(o);
    if (o.command == "diff")     return cmd_diff(o);
    if (o.command == "decode")   return cmd_decode(o);
    if (o.command == "analyze")  return cmd_analyze(o);

    print_usage();
    return 2;
#endif
}
