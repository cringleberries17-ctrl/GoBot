#pragma once

#include "gobot/tcp_reassembler.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gobot {

/// A TCP connection owned by a local process.
struct ProcessConnection {
    Endpoint      local;
    Endpoint      remote;
    std::uint32_t pid = 0;
    std::string   process;      ///< executable file name
    bool          established = false;

    std::string to_string() const;
};

/// Every TCP connection on the machine, with its owning process.
///
/// This is what makes capture setup automatic. The game already knows which
/// server it is talking to; asking Windows which sockets the game process owns
/// is strictly better than asking the user to run netstat and copy an address
/// across by hand.
std::vector<ProcessConnection> tcp_connections();

std::vector<ProcessConnection> connections_for_pid(std::uint32_t pid);
std::vector<ProcessConnection> connections_for_process(const std::string& exe_name);

/// Connections that plausibly carry game traffic.
///
/// Drops loopback, link-local and unestablished sockets, plus the browser-ish
/// ports (80/443) a launcher or overlay keeps open — those are the usual
/// decoys next to the real game socket.
std::vector<ProcessConnection> game_connection_candidates(std::uint32_t pid);

/// PID owning a window.
std::uint32_t pid_for_window(void* hwnd);

/// Local IPv4 addresses of this machine, for matching a connection to an
/// adapter.
std::vector<std::string> local_ipv4_addresses();

/// The local ports of the UDP sockets a process owns, by executable name,
/// lowest first. Read from Windows' socket table, like tcp_connections(); a
/// UDP socket has no remote end there, so its own port is all there is.
std::vector<std::uint16_t> udp_ports_for_process(const std::string& exe_name);

/// Build a pcap filter matching the given connections, TCP and UDP both, as
/// GoC's "(tcp or udp)" does. UDP is matched from any host on the servers'
/// ports too, as GoC's own "(tcp or udp) and port 15101" matches it, and on
/// `udp_ports` -- local ports of the game's own UDP sockets
/// (udp_ports_for_process) -- whatever server they talk to. Empty input
/// yields "(tcp or udp)".
std::string build_filter_for(const std::vector<ProcessConnection>& conns,
                             const std::vector<std::uint16_t>& udp_ports = {});

std::string ip_to_string(std::uint32_t ip_host_order);

} // namespace gobot
