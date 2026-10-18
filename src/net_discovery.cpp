#include "gobot/net_discovery.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <unordered_map>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <psapi.h>

namespace gobot {
namespace {

std::string process_name_for_pid(std::uint32_t pid) {
    if (pid == 0) return {};

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};

    char path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    std::string out;
    if (QueryFullProcessImageNameA(proc, 0, path, &size)) {
        out = path;
        const auto slash = out.find_last_of("\\/");
        if (slash != std::string::npos) out = out.substr(slash + 1);
    }
    CloseHandle(proc);
    return out;
}

bool is_loopback(std::uint32_t ip)   { return (ip >> 24) == 127; }
bool is_linklocal(std::uint32_t ip)  { return (ip >> 16) == 0xA9FE; }   // 169.254/16
bool is_unspecified(std::uint32_t ip) { return ip == 0; }

} // namespace

std::string ip_to_string(std::uint32_t ip) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return buf;
}

std::string ProcessConnection::to_string() const {
    return local.to_string() + " -> " + remote.to_string() +
           "  [" + (process.empty() ? "?" : process) + " pid " + std::to_string(pid) + "]";
}

std::vector<ProcessConnection> tcp_connections() {
    std::vector<ProcessConnection> out;

    ULONG size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_ALL, 0) != ERROR_INSUFFICIENT_BUFFER) {
        GB_WARN("GetExtendedTcpTable: could not size the table");
        return out;
    }

    std::vector<std::uint8_t> buf(size);
    auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());

    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) {
        GB_WARN("GetExtendedTcpTable failed");
        return out;
    }

    // Cache per PID: a process with many sockets would otherwise be opened
    // once per row.
    std::unordered_map<std::uint32_t, std::string> names;

    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_TCPROW_OWNER_PID& row = table->table[i];

        ProcessConnection c;
        c.local.ip    = ntohl(row.dwLocalAddr);
        c.local.port  = static_cast<std::uint16_t>(ntohs(static_cast<u_short>(row.dwLocalPort)));
        c.remote.ip   = ntohl(row.dwRemoteAddr);
        c.remote.port = static_cast<std::uint16_t>(ntohs(static_cast<u_short>(row.dwRemotePort)));
        c.pid         = row.dwOwningPid;
        c.established = (row.dwState == MIB_TCP_STATE_ESTAB);

        auto it = names.find(c.pid);
        if (it == names.end()) it = names.emplace(c.pid, process_name_for_pid(c.pid)).first;
        c.process = it->second;

        out.push_back(std::move(c));
    }
    return out;
}

std::vector<ProcessConnection> connections_for_pid(std::uint32_t pid) {
    std::vector<ProcessConnection> out;
    for (auto& c : tcp_connections()) {
        if (c.pid == pid) out.push_back(std::move(c));
    }
    return out;
}

std::vector<ProcessConnection> connections_for_process(const std::string& exe_name) {
    std::vector<ProcessConnection> out;
    for (auto& c : tcp_connections()) {
        if (_stricmp(c.process.c_str(), exe_name.c_str()) == 0) out.push_back(std::move(c));
    }
    return out;
}

std::vector<ProcessConnection> game_connection_candidates(std::uint32_t pid) {
    std::vector<ProcessConnection> out;

    for (auto& c : connections_for_pid(pid)) {
        if (!c.established) continue;
        if (is_unspecified(c.remote.ip) || is_loopback(c.remote.ip) ||
            is_linklocal(c.remote.ip)) continue;

        // Launchers and in-game overlays keep HTTP sockets open next to the
        // real game socket. They are never the game protocol, and including
        // them pollutes the framing sample with TLS noise that cannot tile.
        if (c.remote.port == 80 || c.remote.port == 443) continue;

        out.push_back(std::move(c));
    }

    // Most game clients hold one long-lived connection; when several survive
    // the filter we keep them all and let per-flow byte counts decide.
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.remote.port < b.remote.port;
    });
    return out;
}

std::uint32_t pid_for_window(void* hwnd) {
    if (!hwnd) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(static_cast<HWND>(hwnd), &pid);
    return pid;
}

std::vector<std::string> local_ipv4_addresses() {
    std::vector<std::string> out;

    ULONG size = 0;
    if (GetAdaptersInfo(nullptr, &size) != ERROR_BUFFER_OVERFLOW || size == 0) return out;

    std::vector<std::uint8_t> buf(size);
    auto* info = reinterpret_cast<IP_ADAPTER_INFO*>(buf.data());
    if (GetAdaptersInfo(info, &size) != NO_ERROR) return out;

    for (IP_ADAPTER_INFO* a = info; a; a = a->Next) {
        for (IP_ADDR_STRING* s = &a->IpAddressList; s; s = s->Next) {
            const std::string ip = s->IpAddress.String;
            if (!ip.empty() && ip != "0.0.0.0") out.push_back(ip);
        }
    }
    return out;
}

std::vector<std::uint16_t> udp_ports_for_process(const std::string& exe_name) {
    std::vector<std::uint16_t> out;

    ULONG size = 0;
    if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET,
                            UDP_TABLE_OWNER_PID, 0) != ERROR_INSUFFICIENT_BUFFER) {
        return out;     // no UDP sockets at all, or no table to read
    }

    std::vector<std::uint8_t> buf(size);
    auto* table = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buf.data());
    if (GetExtendedUdpTable(table, &size, FALSE, AF_INET,
                            UDP_TABLE_OWNER_PID, 0) != NO_ERROR) {
        GB_WARN("GetExtendedUdpTable failed");
        return out;
    }

    std::unordered_map<std::uint32_t, std::string> names;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_UDPROW_OWNER_PID& row = table->table[i];
        auto it = names.find(row.dwOwningPid);
        if (it == names.end())
            it = names.emplace(row.dwOwningPid, process_name_for_pid(row.dwOwningPid)).first;
        if (_stricmp(it->second.c_str(), exe_name.c_str()) != 0) continue;
        out.push_back(static_cast<std::uint16_t>(ntohs(static_cast<u_short>(row.dwLocalPort))));
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string build_filter_for(const std::vector<ProcessConnection>& conns,
                             const std::vector<std::uint16_t>& udp_ports) {
    // UDP as well as TCP, as GoC captures it: nothing GoBot has recorded can
    // say whether the game uses UDP, and a filter that leaves it out cannot
    // find out.
    if (conns.empty() && udp_ports.empty()) return "(tcp or udp)";

    // Match each connection by its remote endpoint. Pinning the port as well
    // as the host keeps a busy server's other services out of the sample.
    std::ostringstream servers;
    if (!conns.empty()) {
        servers << "(tcp or udp) and (";
        for (std::size_t i = 0; i < conns.size(); ++i) {
            if (i) servers << " or ";
            servers << "(host " << ip_to_string(conns[i].remote.ip)
                    << " and port " << conns[i].remote.port << ")";
        }
        servers << ")";
    }

    // UDP on two kinds of port, from any host:
    //
    //   - the game's own UDP sockets, by their local port: a UDP server need
    //     not listen on the port its TCP connection uses, and these are the
    //     game's own traffic whichever server it reaches;
    //   - the servers' ports, as GoC's own "(tcp or udp) and port 15101"
    //     takes them, with no host: a datagram from another of the game's
    //     hosts, to a socket the game opened after this filter was built --
    //     on a map load, say -- is still let in.
    std::vector<std::uint16_t> ports = udp_ports;
    for (const auto& c : conns) ports.push_back(c.remote.port);
    std::sort(ports.begin(), ports.end());
    ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
    ports.erase(std::remove(ports.begin(), ports.end(), std::uint16_t{0}), ports.end());

    std::ostringstream udp;
    if (!ports.empty()) {
        udp << "udp and (";
        for (std::size_t i = 0; i < ports.size(); ++i) {
            if (i) udp << " or ";
            udp << "port " << ports[i];
        }
        udp << ")";
    }

    if (ports.empty()) return servers.str();
    if (conns.empty()) return udp.str();
    return "(" + servers.str() + ") or (" + udp.str() + ")";
}

} // namespace gobot
