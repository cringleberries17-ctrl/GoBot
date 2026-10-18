#pragma once

#include "gobot/game_install.hpp"
#include "gobot/tcp_reassembler.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
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

/// Whose sockets `--process <name>` takes, decided once per process.
///
/// By the program's file name -- unless that is the game's own name
/// (GameInstall::names_the_game: Game.exe, or the name the game is installed
/// under). Game.exe is what a great many programs are called, and another
/// one running beside the game would put its traffic into a recording of the
/// game's, so a process of that name counts only when it is the game by
/// where it runs from (GameInstall::is_game). One whose path cannot be read
/// is not the game.
class SocketOwners {
public:
    /// What is known of one process: its file name, and the full path it
    /// runs from, "" when that cannot be read.
    struct Owner {
        std::string name;
        std::string path;
    };
    using LookUp = std::function<Owner(std::uint32_t pid)>;

    /// The processes `look_up` describes, and the game where `install` and
    /// `drives` say it is: pretend ones for a test. `here` is this machine's.
    SocketOwners(std::string exe_name, LookUp look_up, GameInstall install,
                 GameInstall::Drives drives);

    /// This machine's processes, and the game where GameInstall::here()
    /// finds it.
    static SocketOwners here(const std::string& exe_name);

    /// Whether the sockets of process `pid` are taken.
    bool takes(std::uint32_t pid);

    /// How many processes of the name asked for were left out for not being
    /// the game by where they run from: another program called Game.exe, or
    /// a game whose path could not be read. Worth saying when it is not 0.
    std::size_t passed_over() const { return passed_over_; }

private:
    std::string         exe_name_;
    LookUp              look_up_;
    GameInstall         install_;
    GameInstall::Drives drives_;
    bool                by_path_ = false;     ///< exe_name_ names the game
    std::unordered_map<std::uint32_t, bool> answers_;
    std::size_t         passed_over_ = 0;
};

/// Every TCP connection on the machine, with its owning process.
///
/// This is what makes capture setup automatic. The game already knows which
/// server it is talking to; asking Windows which sockets the game process owns
/// is strictly better than asking the user to run netstat and copy an address
/// across by hand.
std::vector<ProcessConnection> tcp_connections();

std::vector<ProcessConnection> connections_for_pid(std::uint32_t pid);

/// The connections of the processes `--process <exe_name>` takes
/// (SocketOwners). Given the SocketOwners, it can be the same one
/// udp_ports_for_process uses, so that each process is looked up once.
std::vector<ProcessConnection> connections_for_process(const std::string& exe_name);
std::vector<ProcessConnection> connections_for_process(SocketOwners& owners);

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

/// The local ports of the UDP sockets of the processes `--process
/// <exe_name>` takes (SocketOwners), lowest first. Read from Windows' socket
/// table, like tcp_connections(); a UDP socket has no remote end there, so
/// its own port is all there is.
std::vector<std::uint16_t> udp_ports_for_process(const std::string& exe_name);
std::vector<std::uint16_t> udp_ports_for_process(SocketOwners& owners);

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
