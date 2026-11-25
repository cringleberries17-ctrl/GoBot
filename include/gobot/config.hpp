#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gobot {

/// Minimal INI reader/writer: `[Section]`, `key=value`, `#` or `;` comments.
/// Preserves nothing about formatting — configs are rewritten wholesale.
/// `set_in_place` is the exception, and exists for the files that cannot
/// afford that.
class IniFile {
public:
    bool load(const std::string& path);
    bool save(const std::string& path, const std::string& header = {}) const;

    /// Change one key in a file on disk, leaving everything else as it is.
    ///
    /// `save` rewrites a file out of the values this object holds and keeps
    /// nothing else, which is right for a file this program generated and
    /// wrong for one a person has written in. configs/login.ini is the second
    /// kind: it carries the measured geometry of every page of the crossing
    /// and the reasoning behind each number, and a bot that threw all of that
    /// away because somebody picked a different channel would be destroying
    /// the only record of how those numbers were arrived at.
    ///
    /// Adds the key if the section has not got one, and the section too if it
    /// is missing. The file keeps its own line endings, LF or CRLF. False if
    /// the file could not be read or written.
    static bool set_in_place(const std::string& path, const std::string& section,
                             const std::string& key, const std::string& value);
    static bool set_in_place(const std::string& path, const std::string& section,
                             const std::string& key, int value);

    std::string   get(const std::string& section, const std::string& key,
                      const std::string& def = {}) const;
    int           get_int(const std::string& section, const std::string& key, int def = 0) const;
    float         get_float(const std::string& section, const std::string& key, float def = 0.f) const;
    bool          get_bool(const std::string& section, const std::string& key, bool def = false) const;

    void set(const std::string& section, const std::string& key, const std::string& value);
    void set_int(const std::string& section, const std::string& key, int value);
    void set_float(const std::string& section, const std::string& key, float value);
    void set_bool(const std::string& section, const std::string& key, bool value);

    bool has_section(const std::string& section) const;
    bool has_key(const std::string& section, const std::string& key) const;
    std::vector<std::string> sections() const;
    std::vector<std::string> keys(const std::string& section) const;

    void clear() { data_.clear(); order_.clear(); }

private:
    std::map<std::string, std::map<std::string, std::string>> data_;
    std::vector<std::string> order_;    ///< section write order
};

// --------------------------------------------------------------- app config

struct AppConfig {
    // capture
    std::string   interface_name;
    std::string   server_ip_start;
    std::string   server_ip_end;
    std::uint16_t port_start = 0;
    std::uint16_t port_end = 0;
    std::string   bpf_filter;
    std::uint32_t batch_interval_ms = 50;

    // target window
    std::string window_title = "GhostOnline";
    std::string window_class;
    std::string process_name;

    // paths
    std::string protocol_path = "configs/protocol.ini";
    std::string log_path = "gobot.log";
    std::string log_level = "info";

    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

/// Resolve a path relative to the executable, so the tools work regardless of
/// the shell's working directory.
std::string exe_relative(const std::string& relative);

} // namespace gobot
