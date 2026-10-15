#include "gobot/common.hpp"
#include "gobot/byte_reader.hpp"
#include "gobot/config.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>

namespace gobot {

std::uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int size = static_cast<int>(utf8.size());
    // Not UTF-8 -- a path typed into an ini by a program that saved it in the
    // machine's code page -- is read the way Windows would have read it.
    UINT page = CP_UTF8;
    int n = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, utf8.data(), size, nullptr, 0);
    if (n <= 0) {
        page = CP_ACP;
        n = MultiByteToWideChar(page, 0, utf8.data(), size, nullptr, 0);
        if (n <= 0) return {};
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(page, 0, utf8.data(), size, out.data(), n);
    return out;
}

std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int size = static_cast<int>(wide.size());
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), size, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), size, out.data(), n, nullptr, nullptr);
    return out;
}

bool path_exists(const std::string& path) {
    return !path.empty() && GetFileAttributesW(utf8_to_wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool is_folder(const std::string& path) {
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(utf8_to_wide(path).c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool make_folder(const std::string& path) {
    if (path.empty()) return false;
    return CreateDirectoryW(utf8_to_wide(path).c_str(), nullptr) != 0 ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

bool remove_file(const std::string& path) {
    return !path.empty() && DeleteFileW(utf8_to_wide(path).c_str()) != 0;
}

bool move_file(const std::string& from, const std::string& to) {
    return !from.empty() && !to.empty() &&
           MoveFileExW(utf8_to_wide(from).c_str(), utf8_to_wide(to).c_str(),
                       MOVEFILE_REPLACE_EXISTING) != 0;
}

std::vector<std::string> files_in(const std::string& folder, const std::string& pattern) {
    std::vector<std::string> out;
    std::string spec = folder;
    if (!spec.empty() && spec.back() != '\\' && spec.back() != '/') spec += '\\';
    spec += pattern;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(utf8_to_wide(spec).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        out.push_back(wide_to_utf8(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

std::FILE* open_file(const std::string& path, const char* mode) {
    if (path.empty() || mode == nullptr) return nullptr;
    return _wfopen(utf8_to_wide(path).c_str(), utf8_to_wide(mode).c_str());
}

// ---------------------------------------------------------------- version

std::string version_line(const std::string& text, int line) {
    std::size_t at = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        at = 3;
    for (int i = 0; i < line; ++i) {
        const std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) return {};
        at = nl + 1;
    }
    std::size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();

    // Printable ASCII only: this goes into a window title and the log, and a
    // file edited by hand -- or not this file at all -- must not put anything
    // else there.
    std::string out;
    for (std::size_t i = at; i < end && out.size() < 40; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0x20 && c < 0x7F) out += static_cast<char>(c);
    }
    const auto first = out.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const auto last = out.find_last_not_of(' ');
    return out.substr(first, last - first + 1);
}

namespace {

const std::string& version_text() {
    static const std::string text = [] {
        std::string t;
        std::FILE* f = _wfopen(utf8_to_wide(exe_relative("VERSION.txt")).c_str(), L"rb");
        if (!f) return t;
        char buf[512];
        const std::size_t n = std::fread(buf, 1, sizeof(buf), f);
        std::fclose(f);
        t.assign(buf, n);
        return t;
    }();
    return text;
}

} // namespace

std::string package_version() {
    static const std::string v = version_line(version_text(), 0);
    return v;
}

std::string package_commit() {
    static const std::string c = version_line(version_text(), 1);
    return c;
}

std::string version_shown() {
    const std::string v = package_version();
    // "A build of your own": one made from the source, never packaged.
    return v.empty() ? std::string("b\xe1\xba\xa3n t\xe1\xbb\xb1 build") : v;
}

bool read_uint_at(const std::uint8_t* data, std::size_t size, std::size_t offset,
                  int width, bool little_endian, std::uint64_t& out) {
    if (width != 1 && width != 2 && width != 4 && width != 8) return false;
    const std::size_t w = static_cast<std::size_t>(width);
    if (offset + w > size || offset + w < offset) return false;

    std::uint64_t v = 0;
    if (little_endian) {
        for (std::size_t i = 0; i < w; ++i)
            v |= static_cast<std::uint64_t>(data[offset + i]) << (8 * i);
    } else {
        for (std::size_t i = 0; i < w; ++i)
            v = (v << 8) | data[offset + i];
    }
    out = v;
    return true;
}

std::string hexdump(const std::uint8_t* data, std::size_t size,
                    std::size_t bytes_per_line, int indent) {
    if (bytes_per_line == 0) bytes_per_line = 16;
    const std::string pad(indent < 0 ? 0 : static_cast<std::size_t>(indent), ' ');

    std::string out;
    out.reserve(size * 4 + (size / bytes_per_line + 1) * (indent + 12));

    // Wide enough for a 64-bit offset; "%04zx" only pads, it never truncates
    // a large value, and an 8-byte buffer would clip past 0x10000.
    char line[24];
    for (std::size_t off = 0; off < size; off += bytes_per_line) {
        out += pad;
        std::snprintf(line, sizeof(line), "%04zx  ", off);
        out += line;

        const std::size_t n = (size - off < bytes_per_line) ? size - off : bytes_per_line;

        for (std::size_t i = 0; i < bytes_per_line; ++i) {
            if (i < n) {
                std::snprintf(line, sizeof(line), "%02x ", data[off + i]);
                out += line;
            } else {
                out += "   ";
            }
            if (i % 8 == 7) out += ' ';
        }

        out += ' ';
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t c = data[off + i];
            out += (c >= 0x20 && c <= 0x7e) ? static_cast<char>(c) : '.';
        }
        out += '\n';
    }
    return out;
}

} // namespace gobot
