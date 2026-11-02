#include "gobot/file_watch.hpp"
#include "gobot/common.hpp"

#include <windows.h>

namespace gobot {

std::uint64_t file_write_stamp(const std::string& path) {
    // Asked by the wide name, as IniFile opens it: a GoBot folder with a
    // Vietnamese name is not one the ANSI call can find.
    if (path.empty()) return 0;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(utf8_to_wide(path).c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<std::uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
           fad.ftLastWriteTime.dwLowDateTime;
}

bool FileWatch::due(std::uint64_t now_ms, const std::function<std::uint64_t()>& stamp) {
    if (!read_) {
        // Stamped before the caller reads: a write landing between the two
        // leaves a stamp older than what was read, and the next look reads
        // the file once more -- one read too many, never an edit missed.
        stamp_ = stamp();
        checked_ms_ = now_ms;
        read_ = true;
        retry_ = false;
        return true;
    }
    if (now_ms - checked_ms_ < gap_ms_) return false;
    checked_ms_ = now_ms;
    const std::uint64_t now_stamp = stamp();
    if (now_stamp == stamp_ && !retry_) return false;
    stamp_ = now_stamp;
    retry_ = false;
    return true;
}

bool FileWatch::due(const std::string& path) {
    return due(now_ms(), [&path] { return file_write_stamp(path); });
}

} // namespace gobot
