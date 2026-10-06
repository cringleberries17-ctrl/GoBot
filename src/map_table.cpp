#include "gobot/map_table.hpp"
#include "gobot/logger.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include <windows.h>

namespace gobot {
namespace {

/// A cursor over the de-obfuscated file.
///
/// Every read is bounds-checked and sets `bad` rather than throwing, because
/// this is a third party's undocumented format: the shape was worked out by
/// reading it, and a build that lays it out differently should come back empty
/// rather than walk off the end of the buffer.
struct Cursor {
    const std::vector<unsigned char>& data;
    std::size_t at = 0;
    bool        bad = false;

    bool have(std::size_t n) const { return !bad && at + n <= data.size(); }

    std::uint32_t u32() {
        if (!have(4)) { bad = true; return 0; }
        const std::uint32_t v = data[at] | (data[at + 1] << 8) |
                                (data[at + 2] << 16) |
                                (static_cast<std::uint32_t>(data[at + 3]) << 24);
        at += 4;
        return v;
    }

    std::uint16_t u16() {
        if (!have(2)) { bad = true; return 0; }
        const std::uint16_t v = static_cast<std::uint16_t>(data[at] | (data[at + 1] << 8));
        at += 2;
        return v;
    }

    unsigned char u8() {
        if (!have(1)) { bad = true; return 0; }
        return data[at++];
    }

    /// A length-prefixed string: the length plain, the text masked and stored
    /// backwards.
    ///
    /// Only the text is masked. Lengths, counts and the numeric columns are
    /// written as they are -- masking the whole file turns the column count
    /// into nonsense and the parse ends before it starts.
    std::string text() {
        const std::uint16_t len = u16();
        if (!have(len)) { bad = true; return {}; }
        std::string s(len, '\0');
        for (std::uint16_t i = 0; i < len; ++i)
            s[len - 1 - i] = static_cast<char>(data[at + i] ^ MapTable::kMask);
        at += len;
        return s;
    }
};

/// What each column holds, as the table's own header describes it.
enum : std::uint32_t {
    kTypeInt32 = 6,
    kTypeString = 7,
    kTypeByte = 2,
};

} // namespace

std::string MapTable::Entry::stem() const {
    char buf[32];
    std::snprintf(buf, sizeof buf, "t%d_s%d", theme, stage);
    return buf;
}

bool MapTable::load(const std::string& path) {
    entries_.clear();

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 64) { std::fclose(f); return false; }

    std::vector<unsigned char> data(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) return false;

    if (std::memcmp(data.data(), "Ghost", 5) != 0) {
        GB_WARN("MapTable: %s is not a Ghost table", path.c_str());
        return false;
    }

    // "Ghost\0\0\0", a four-byte region tag, and a build timestamp.
    constexpr std::size_t kHeader = 32;
    Cursor c{ data, kHeader };
    const std::uint32_t columns = c.u32();
    if (columns == 0 || columns > 64) return false;

    std::vector<std::uint32_t> types(columns);
    for (std::uint32_t i = 0; i < columns; ++i) types[i] = c.u32();

    const std::uint32_t rows = c.u32();
    if (rows == 0 || rows > 100000) return false;

    // The column names come before the rows, and are read rather than assumed:
    // a later build that adds a column would otherwise be parsed as though the
    // new field were one of the old ones.
    std::vector<std::string> names(columns);
    for (std::uint32_t i = 0; i < columns; ++i) names[i] = c.text();
    if (c.bad) return false;

    int theme_col = -1, stage_col = -1, name_col = -1;
    for (std::uint32_t i = 0; i < columns; ++i) {
        if (names[i] == "nTheme")           theme_col = static_cast<int>(i);
        else if (names[i] == "nStage")      stage_col = static_cast<int>(i);
        else if (names[i] == "szStageName") name_col = static_cast<int>(i);
    }
    if (theme_col < 0 || stage_col < 0 || name_col < 0) {
        GB_WARN("MapTable: %s has no theme/stage/name columns", path.c_str());
        return false;
    }

    entries_.reserve(rows);
    for (std::uint32_t r = 0; r < rows && !c.bad; ++r) {
        Entry e;
        for (std::uint32_t i = 0; i < columns && !c.bad; ++i) {
            switch (types[i]) {
                case kTypeInt32: {
                    const std::uint32_t v = c.u32();
                    if (static_cast<int>(i) == theme_col) e.theme = static_cast<int>(v);
                    else if (static_cast<int>(i) == stage_col) e.stage = static_cast<int>(v);
                    break;
                }
                case kTypeString: {
                    std::string s = c.text();
                    if (static_cast<int>(i) == name_col) e.name = std::move(s);
                    break;
                }
                case kTypeByte:
                    c.u8();
                    break;
                default:
                    // An unknown width cannot be skipped, and guessing would
                    // desynchronise every row after this one.
                    GB_WARN("MapTable: column '%s' has unknown type %u",
                            names[i].c_str(), types[i]);
                    c.bad = true;
                    break;
            }
        }
        if (!c.bad && !e.name.empty()) entries_.push_back(std::move(e));
    }

    GB_INFO("MapTable: %zu places from %s", entries_.size(), path.c_str());
    return !entries_.empty();
}

const MapTable::Entry* MapTable::find(const std::string& name) const {
    for (const Entry& e : entries_)
        if (e.name == name) return &e;
    return nullptr;
}

const MapTable::Entry* MapTable::best_match(const std::string& read) const {
    if (read.empty()) return nullptr;

    const Entry* best = nullptr;
    std::size_t longest = 0;
    for (const Entry& e : entries_) {
        if (e.name.size() < 6) continue;        // too short to be distinctive
        if (e.name.size() <= longest) continue;
        if (read.find(e.name) == std::string::npos) continue;
        longest = e.name.size();
        best = &e;
    }
    return best;
}

std::vector<MapTable::Entry> MapTable::closest(const std::string& read,
                                               std::size_t count) const {
    // How much of a name the reading opens with, measured at whichever place
    // in the reading fits it best. The title bar scrolls, so the name is
    // rarely at the start.
    const auto shared = [&](const std::string& name) {
        std::size_t best = 0;
        for (std::size_t start = 0; start < read.size(); ++start) {
            std::size_t n = 0;
            while (n < name.size() && start + n < read.size() && read[start + n] == name[n]) ++n;
            best = std::max(best, n);
        }
        return best;
    };

    std::vector<std::pair<std::size_t, const Entry*>> ranked;
    ranked.reserve(entries_.size());
    for (const Entry& e : entries_) ranked.emplace_back(shared(e.name), &e);
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<Entry> out;
    for (const auto& [n, e] : ranked) {
        if (out.size() >= count || n == 0) break;
        out.push_back(*e);
    }
    return out;
}

std::string MapTable::default_path() {
    // The game's own install, found the same way the collision files are.
    const char* keys[] = {
        "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Ghost Online VTC",
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Ghost Online VTC",
    };
    for (const char* key : keys) {
        HKEY h{};
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;

        char buf[MAX_PATH]{};
        DWORD size = sizeof buf;
        DWORD type = 0;
        const LSTATUS ok = RegQueryValueExA(h, "DisplayIcon", nullptr, &type,
                                            reinterpret_cast<LPBYTE>(buf), &size);
        RegCloseKey(h);
        if (ok != ERROR_SUCCESS || type != REG_SZ) continue;

        // DisplayIcon points at Game.exe; the table sits beside it.
        std::string dir = buf;
        const std::size_t slash = dir.find_last_of("\\/");
        if (slash == std::string::npos) continue;
        dir.resize(slash);

        const std::string path = dir + "\\zVietnam\\Chart\\tb_Map.tbl";
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    return {};
}

} // namespace gobot
