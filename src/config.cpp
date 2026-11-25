#include "gobot/config.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <sstream>

#include <windows.h>

namespace gobot {

namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool parse_bool(const std::string& v, bool def) {
    const std::string s = lower(trim(v));
    if (s == "true" || s == "1" || s == "yes" || s == "on")  return true;
    if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    return def;
}

} // namespace

// ------------------------------------------------------------------- IniFile

bool IniFile::set_in_place(const std::string& path, const std::string& section,
                           const std::string& key, const std::string& value) {
    // Read and written as bytes, so the file keeps its own line endings. A
    // text-mode stream writes every line back with CRLF, and a file that had
    // been all LF came back all CRLF from one key being changed -- every line
    // the same and the file different, which is not leaving it as it was.
    std::vector<std::string> lines;
    std::size_t crlf = 0, lf = 0;
    {
        std::ifstream in(utf8_to_wide(path).c_str(), std::ios::binary);
        if (!in) return false;
        std::string line;
        while (std::getline(in, line)) {
            const bool ended = !in.eof();          // a last line may have no ending
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
                if (ended) ++crlf;
            } else if (ended) {
                ++lf;
            }
            lines.push_back(line);
        }
    }
    // Whichever the file mostly uses; CRLF, as this always wrote, when it has
    // no line ending to go by.
    const char* const eol = lf > crlf ? "\n" : "\r\n";
    const auto write = [&] {
        std::ofstream out(utf8_to_wide(path).c_str(), std::ios::binary | std::ios::trunc);
        if (!out) return false;
        for (const std::string& l : lines) out << l << eol;
        out.flush();
        return static_cast<bool>(out);
    };

    const std::string want_section = lower(trim(section));
    const std::string want_key     = lower(trim(key));

    bool in_section = false;
    bool found_section = false;
    std::size_t last_in_section = 0;     // last line belonging to that section

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string line = trim(lines[i]);

        if (!line.empty() && line.front() == '[') {
            const auto close = line.find(']');
            if (close != std::string::npos) {
                in_section = lower(trim(line.substr(1, close - 1))) == want_section;
                if (in_section) { found_section = true; last_in_section = i; }
            }
            continue;
        }
        if (!in_section) continue;
        last_in_section = i;

        // Comments stay comments. A commented-out key is somebody's note about
        // what the value used to be, not the value.
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        if (lower(trim(line.substr(0, eq))) != want_key) continue;

        lines[i] = trim(key) + "=" + value;
        return write();
    }

    // Not there. Add it: at the end of its section, or in a new one.
    const std::string added = trim(key) + "=" + value;
    if (found_section) {
        // Step back over trailing blank lines so the key joins the section
        // rather than floating in the gap before the next one.
        std::size_t at = last_in_section;
        while (at > 0 && trim(lines[at]).empty()) --at;
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at) + 1, added);
    } else {
        if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back({});
        lines.push_back("[" + trim(section) + "]");
        lines.push_back(added);
    }
    return write();
}

bool IniFile::set_in_place(const std::string& path, const std::string& section,
                           const std::string& key, int value) {
    return set_in_place(path, section, key, std::to_string(value));
}

bool IniFile::load(const std::string& path) {
    std::ifstream in(utf8_to_wide(path).c_str());
    if (!in) return false;

    clear();
    std::string line, section;

    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line.front() == '[') {
            const auto close = line.find(']');
            if (close == std::string::npos) continue;
            section = trim(line.substr(1, close - 1));
            if (!data_.count(section)) order_.push_back(section);
            data_[section];                       // materialise empty section
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (key.empty()) continue;

        if (!data_.count(section)) order_.push_back(section);
        data_[section][key] = val;
    }
    return true;
}

bool IniFile::save(const std::string& path, const std::string& header) const {
    std::ofstream out(utf8_to_wide(path).c_str(), std::ios::trunc);
    if (!out) return false;

    if (!header.empty()) {
        std::istringstream hs(header);
        std::string l;
        while (std::getline(hs, l)) out << "# " << l << "\n";
        out << "\n";
    }

    // Written sections first (in insertion order), then anything added later.
    std::vector<std::string> written;
    auto emit = [&](const std::string& sec) {
        auto it = data_.find(sec);
        if (it == data_.end()) return;
        out << "[" << sec << "]\n";
        for (const auto& [k, v] : it->second) out << k << "=" << v << "\n";
        out << "\n";
        written.push_back(sec);
    };

    for (const auto& sec : order_) emit(sec);
    for (const auto& [sec, _] : data_) {
        if (std::find(written.begin(), written.end(), sec) == written.end()) emit(sec);
    }
    return true;
}

std::string IniFile::get(const std::string& section, const std::string& key,
                         const std::string& def) const {
    auto s = data_.find(section);
    if (s == data_.end()) return def;
    auto k = s->second.find(key);
    return k == s->second.end() ? def : k->second;
}

int IniFile::get_int(const std::string& section, const std::string& key, int def) const {
    const std::string v = get(section, key);
    if (v.empty()) return def;
    try {
        return std::stoi(v, nullptr, 0);   // base 0 accepts 0x-prefixed values
    } catch (...) {
        return def;
    }
}

float IniFile::get_float(const std::string& section, const std::string& key, float def) const {
    const std::string v = get(section, key);
    if (v.empty()) return def;
    try {
        return std::stof(v);
    } catch (...) {
        return def;
    }
}

bool IniFile::get_bool(const std::string& section, const std::string& key, bool def) const {
    const std::string v = get(section, key);
    return v.empty() ? def : parse_bool(v, def);
}

void IniFile::set(const std::string& section, const std::string& key, const std::string& value) {
    if (!data_.count(section)) order_.push_back(section);
    data_[section][key] = value;
}

void IniFile::set_int(const std::string& s, const std::string& k, int v) {
    set(s, k, std::to_string(v));
}

void IniFile::set_float(const std::string& s, const std::string& k, float v) {
    std::ostringstream os;
    os << v;
    set(s, k, os.str());
}

void IniFile::set_bool(const std::string& s, const std::string& k, bool v) {
    set(s, k, v ? "true" : "false");
}

bool IniFile::has_section(const std::string& s) const { return data_.count(s) != 0; }

bool IniFile::has_key(const std::string& s, const std::string& k) const {
    auto it = data_.find(s);
    return it != data_.end() && it->second.count(k) != 0;
}

std::vector<std::string> IniFile::sections() const {
    std::vector<std::string> out;
    out.reserve(data_.size());
    for (const auto& [s, _] : data_) out.push_back(s);
    return out;
}

std::vector<std::string> IniFile::keys(const std::string& section) const {
    std::vector<std::string> out;
    auto it = data_.find(section);
    if (it == data_.end()) return out;
    out.reserve(it->second.size());
    for (const auto& [k, _] : it->second) out.push_back(k);
    return out;
}

// ----------------------------------------------------------------- AppConfig

bool AppConfig::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) {
        GB_WARN("AppConfig: cannot read %s -- using defaults", path.c_str());
        return false;
    }

    interface_name    = ini.get("Capture", "interface", interface_name);
    server_ip_start   = ini.get("Capture", "server_ip_start", server_ip_start);
    server_ip_end     = ini.get("Capture", "server_ip_end", server_ip_end);
    port_start        = static_cast<std::uint16_t>(ini.get_int("Capture", "port_start", port_start));
    port_end          = static_cast<std::uint16_t>(ini.get_int("Capture", "port_end", port_end));
    bpf_filter        = ini.get("Capture", "bpf_filter", bpf_filter);
    batch_interval_ms = static_cast<std::uint32_t>(ini.get_int("Capture", "batch_interval_ms", static_cast<int>(batch_interval_ms)));

    window_title  = ini.get("Window", "title", window_title);
    window_class  = ini.get("Window", "class", window_class);
    process_name  = ini.get("Window", "process", process_name);

    protocol_path = ini.get("Paths", "protocol", protocol_path);
    log_path      = ini.get("Paths", "log", log_path);
    log_level     = ini.get("Paths", "log_level", log_level);

    return true;
}

bool AppConfig::save(const std::string& path) const {
    IniFile ini;
    ini.set    ("Capture", "interface", interface_name);
    ini.set    ("Capture", "server_ip_start", server_ip_start);
    ini.set    ("Capture", "server_ip_end", server_ip_end);
    ini.set_int("Capture", "port_start", port_start);
    ini.set_int("Capture", "port_end", port_end);
    ini.set    ("Capture", "bpf_filter", bpf_filter);
    ini.set_int ("Capture", "batch_interval_ms", static_cast<int>(batch_interval_ms));

    ini.set("Window", "title", window_title);
    ini.set("Window", "class", window_class);
    ini.set("Window", "process", process_name);

    ini.set("Paths", "protocol", protocol_path);
    ini.set("Paths", "log", log_path);
    ini.set("Paths", "log_level", log_level);

    return ini.save(path, "GoBot application configuration");
}

std::string exe_relative(const std::string& relative) {
    // In UTF-8, as every path in GoBot is, and asked for in full: a folder
    // named with Vietnamese letters is another path read back in the
    // machine's code page, and a long one does not fit MAX_PATH.
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return relative;
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        if (buf.size() >= 32768) return relative;
        buf.resize(buf.size() * 2);
    }
    std::string dir = wide_to_utf8(buf);
    const auto slash = dir.find_last_of("\\/");
    if (slash == std::string::npos) return relative;
    dir.resize(slash + 1);
    return dir + relative;
}

} // namespace gobot
