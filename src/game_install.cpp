#include "gobot/game_install.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"
#include "gobot/no_real_input.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <mutex>

#include <windows.h>
#include <tlhelp32.h>

namespace gobot {
namespace {

/// Where Windows keeps the game installer's record: the 32-bit view first,
/// which is where a 32-bit installer writes it.
const wchar_t* const kRecordKeys[] = {
    L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Ghost Online VTC",
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Ghost Online VTC",
};

/// Where VTCZone puts the game unless told otherwise. The map files are looked
/// for there too, when there is no record.
const char* const kUsualProgram = "C:\\VTCZone\\Games\\Ghost_Online_PC_VTC\\Game.exe";

/// The game's program's own name, for a record that names its icon instead.
const char* const kGameExe = "Game.exe";

/// The game's own window, as measured on the live client.
const wchar_t* const kGameClass = L"GhostSoul";
const wchar_t* const kGameTitle = L"GhostOnlineVTC";

std::string trimmed(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool starts_with_nocase(const std::string& s, const char* prefix) {
    const std::size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

bool same_text_nocase(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return a.empty() && b.empty();
    const std::wstring wa = utf8_to_wide(a), wb = utf8_to_wide(b);
    return CompareStringOrdinal(wa.c_str(), static_cast<int>(wa.size()), wb.c_str(),
                                static_cast<int>(wb.size()), TRUE) == CSTR_EQUAL;
}

std::string file_name_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// The folder a path in the one form is in, "" when it names none.
std::string folder_of(const std::string& path) {
    const std::size_t slash = path.find_last_of('\\');
    return slash == std::string::npos || slash == 0 ? std::string{} : path.substr(0, slash);
}

/// Which file or folder a path in the one form is on disk: its volume's
/// serial number and its number on that volume, or "" when it cannot be
/// opened. Opened only to ask -- with no right to read it or to change it,
/// and every kind of sharing allowed, so that nothing using it notices.
std::string file_id_on_disk(const std::string& path) {
    // Never anything across the network -- a share, a drive mapped to one, a
    // network device: a server that does not answer would hold the look up
    // for as long as Windows waits on it. Nor a path that is not a full one.
    std::wstring wide = utf8_to_wide(path);
    if (wide.size() >= 2 && wide[1] == L':' && (wide.size() == 2 || wide[2] == L'\\')) {
        const wchar_t root[4] = { wide[0], L':', L'\\', L'\0' };
        if (GetDriveTypeW(root) == DRIVE_REMOTE) return {};
        // The long-path form, which takes a path of any length. A drive on
        // its own is its top folder.
        if (wide.size() == 2) wide += L'\\';
        wide = L"\\\\?\\" + wide;
    } else if (starts_with_nocase(path, "\\Device\\HarddiskVolume")) {
        // The kernel's form, left so for want of a drive letter it maps to.
        wide = L"\\\\?\\GLOBALROOT" + wide;
    } else {
        return {};
    }
    HANDLE h = CreateFileW(wide.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char id[80]{};
    FILE_ID_INFO full{};
    BY_HANDLE_FILE_INFORMATION older{};
    if (GetFileInformationByHandleEx(h, FileIdInfo, &full, sizeof(full))) {
        // 128 bits, which ReFS needs; NTFS's numbers fit in the lower 64.
        unsigned long long low = 0, high = 0;
        std::memcpy(&low, full.FileId.Identifier, sizeof(low));
        std::memcpy(&high, full.FileId.Identifier + sizeof(low), sizeof(high));
        std::snprintf(id, sizeof(id), "%016llx:%016llx%016llx",
                      static_cast<unsigned long long>(full.VolumeSerialNumber), high, low);
    } else if (GetFileInformationByHandle(h, &older)) {
        std::snprintf(id, sizeof(id), "%08lx:%08lx%08lx", older.dwVolumeSerialNumber,
                      older.nFileIndexHigh, older.nFileIndexLow);
    }
    CloseHandle(h);
    return id;
}

/// Whether two paths in the one form name one file or folder on disk, as
/// `drives` tells them apart. Not when either cannot be opened.
bool same_on_disk(const std::string& a, const std::string& b, const GameInstall::Drives& drives) {
    if (!drives.file_id) return false;
    const std::string ia = drives.file_id(a);
    if (ia.empty()) return false;
    const std::string ib = drives.file_id(b);
    return !ib.empty() && ia == ib;
}

/// The path a process handle was started from, UTF-8, or "".
std::string path_of_handle(HANDLE process) {
    // Most paths fit the short buffer; a path past MAX_PATH gets the long one.
    for (DWORD room : { static_cast<DWORD>(MAX_PATH * 2), static_cast<DWORD>(32768) }) {
        std::wstring path(room, L'\0');
        DWORD size = room;
        if (QueryFullProcessImageNameW(process, 0, path.data(), &size) && size > 0) {
            path.resize(size);
            return wide_to_utf8(path);
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
    }
    return {};
}

/// One string of the installer's record, "" when there is none.
std::string record_value(const wchar_t* key, const wchar_t* name) {
    HKEY h{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return {};
    DWORD type = 0;
    DWORD size = 0;
    std::wstring value;
    if (RegQueryValueExW(h, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && size > 0 && size < 65536) {
        value.assign(size / sizeof(wchar_t) + 1, L'\0');
        DWORD got = size;
        if (RegQueryValueExW(h, name, nullptr, &type, reinterpret_cast<LPBYTE>(value.data()),
                             &got) != ERROR_SUCCESS)
            value.clear();
    }
    RegCloseKey(h);
    // A stored string need not end in a nul, or may end in several.
    value.resize(wcsnlen(value.c_str(), value.size()));
    if (type == REG_EXPAND_SZ && !value.empty()) {
        const DWORD n = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        std::wstring out(n, L'\0');
        if (n > 0 && ExpandEnvironmentStringsW(value.c_str(), out.data(), n) > 0) {
            out.resize(wcsnlen(out.c_str(), out.size()));
            value = out;
        }
    }
    return wide_to_utf8(value);
}

/// The programs behind the game's own window: now, and any seen earlier in
/// this session, whose windows may have gone since -- a client that crashed
/// is still the game until it has been ended.
std::vector<std::string> behind_the_games_window() {
    std::vector<std::string> now;
    for (HWND w = FindWindowExW(nullptr, nullptr, kGameClass, kGameTitle); w != nullptr;
         w = FindWindowExW(nullptr, w, kGameClass, kGameTitle)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        const std::string path = program_path(pid);
        if (!path.empty()) now.push_back(path);
    }
    static std::mutex mutex;
    static std::vector<std::string> seen;
    std::lock_guard<std::mutex> lock(mutex);
    for (const std::string& path : now)
        if (std::find(seen.begin(), seen.end(), path) == seen.end()) seen.push_back(path);
    return seen;
}

/// Separators, `.` and `..`, and the prefixes: everything `plain` does but
/// the long names.
std::string tidy(std::string p, const GameInstall::Drives& drives) {
    p = trimmed(p);
    if (p.size() >= 2 && p.front() == '"' && p.back() == '"') p = trimmed(p.substr(1, p.size() - 2));
    std::replace(p.begin(), p.end(), '/', '\\');

    // What Windows puts in front of a path past MAX_PATH (\\?\), of a share
    // (\\?\UNC\), of a device (\\.\), and what the kernel writes (\??\).
    if (starts_with_nocase(p, "\\\\?\\UNC\\") || starts_with_nocase(p, "\\??\\UNC\\"))
        p = "\\\\" + p.substr(8);
    else if (starts_with_nocase(p, "\\\\?\\GLOBALROOT\\"))
        p = p.substr(14);
    else if (starts_with_nocase(p, "\\\\?\\") || starts_with_nocase(p, "\\\\.\\") ||
             starts_with_nocase(p, "\\??\\"))
        p = p.substr(4);

    // The kernel's own form names the device, \Device\HarddiskVolume3\...:
    // read as the drive that device is. HarddiskVolume3 is not the start of
    // HarddiskVolume30, so the device has to end where a folder begins.
    if (starts_with_nocase(p, "\\Device\\")) {
        for (const auto& [drive, device] : drives.devices) {
            if (device.empty() || !starts_with_nocase(p, device.c_str())) continue;
            if (p.size() > device.size() && p[device.size()] != '\\') continue;
            p = drive + p.substr(device.size());
            break;
        }
    }

    // One separator at a time, and no `.` or `..` -- never climbing past the
    // drive, or past a share's server and name.
    const bool share = p.size() >= 2 && p[0] == '\\' && p[1] == '\\';
    const bool rooted = !share && !p.empty() && p[0] == '\\';
    std::vector<std::string> parts;
    std::size_t at = 0;
    while (at <= p.size()) {
        const std::size_t next = p.find('\\', at);
        const std::string part = p.substr(at, next == std::string::npos ? std::string::npos
                                                                         : next - at);
        at = next == std::string::npos ? p.size() + 1 : next + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (parts.size() > (share ? 2u : 1u)) parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    std::string out = share ? "\\\\" : rooted ? "\\" : "";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += '\\';
        out += parts[i];
    }
    return out;
}

} // namespace

// ------------------------------------------------------------------- paths

GameInstall::Drives GameInstall::Drives::here() {
    Drives d;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        const wchar_t drive[3] = { static_cast<wchar_t>(L'A' + i), L':', L'\0' };
        wchar_t target[1024]{};
        if (QueryDosDeviceW(drive, target, 1024) == 0) continue;
        // A drive made with subst names a folder rather than a device, and
        // no program's path is given in that form.
        if (std::wcsncmp(target, L"\\Device\\", 8) != 0) continue;
        d.devices.emplace_back(wide_to_utf8(drive), wide_to_utf8(target));
    }
    d.long_name = [](const std::string& path) -> std::string {
        const std::wstring wide = utf8_to_wide(path);
        const DWORD n = GetLongPathNameW(wide.c_str(), nullptr, 0);
        if (n == 0) return {};
        std::wstring out(n, L'\0');
        const DWORD got = GetLongPathNameW(wide.c_str(), out.data(), n);
        if (got == 0 || got >= n) return {};
        out.resize(got);
        return wide_to_utf8(out);
    };
    d.file_id = file_id_on_disk;
    return d;
}

std::string GameInstall::plain(const std::string& path, const Drives& drives) {
    std::string p = tidy(path, drives);
    // Every 8.3 name has a tilde in it, and asking the disk is not free.
    if (drives.long_name && p.find('~') != std::string::npos) {
        const std::string longer = drives.long_name(p);
        if (!longer.empty()) p = tidy(longer, drives);
    }
    return p;
}

bool GameInstall::same_file(const std::string& a, const std::string& b, const Drives& drives) {
    const std::string pa = plain(a, drives);
    const std::string pb = plain(b, drives);
    if (pa.empty() || pb.empty()) return false;
    if (same_text_nocase(pa, pb)) return true;
    // Different folders on the way can still lead to one file: a path through
    // a junction or a subst drive, against where the file really is -- the
    // path Windows gives for a running program. The disk is asked only when
    // the names match: every window on the screen is held up against the game
    // this way, and neither changes a program's own name.
    return same_text_nocase(file_name_of(pa), file_name_of(pb)) && same_on_disk(pa, pb, drives);
}

std::string GameInstall::program_in_icon(const std::string& icon) {
    std::string s = trimmed(icon);
    if (!s.empty() && s.front() == '"') {
        const std::size_t close = s.find('"', 1);
        s = s.substr(1, close == std::string::npos ? std::string::npos : close - 1);
    } else {
        // ",0" after the path says which icon in the file.
        const std::size_t comma = s.rfind(',');
        if (comma != std::string::npos && comma + 1 < s.size() &&
            s.find_first_not_of("-0123456789 ", comma + 1) == std::string::npos)
            s.resize(comma);
    }
    s = trimmed(s);
    if (s.empty()) return {};
    // An icon file rather than the program: the program beside it.
    if (s.size() <= 4 || !same_text_nocase(s.substr(s.size() - 4), ".exe")) {
        const std::size_t slash = s.find_last_of("\\/");
        if (slash == std::string::npos) return {};
        s = s.substr(0, slash + 1) + kGameExe;
    }
    return s;
}

// ------------------------------------------------------------- the install

void GameInstall::add(const std::string& program, const Drives& drives) {
    if (program.empty()) return;
    for (const std::string& known : programs)
        if (same_file(known, program, drives)) return;
    programs.push_back(program);
}

GameInstall GameInstall::found(const std::vector<std::string>& records,
                               const std::vector<std::string>& seen_behind_window,
                               const std::function<bool(const std::string&)>& there,
                               const Drives& drives) {
    GameInstall out;
    // A record's program only while it is there: a folder deleted or moved
    // since is nowhere the game runs from.
    for (const std::string& program : records)
        if (!program.empty() && there && there(program)) out.add(program, drives);
    // The usual place whenever the game is in it, whatever the records say. A
    // record naming a folder since gone used to hide it, and the game that
    // VTCZone put there was then known only once its window had been seen.
    if (there && there(kUsualProgram)) out.add(kUsualProgram, drives);
    for (const std::string& seen : seen_behind_window) out.add(seen, drives);
    return out;
}

GameInstall GameInstall::here() {
    std::vector<std::string> records;
    for (const wchar_t* key : kRecordKeys)
        records.push_back(program_in_icon(record_value(key, L"DisplayIcon")));
    return found(records, behind_the_games_window(),
                 [](const std::string& program) {
                     return path_exists(program) && !is_folder(program);
                 },
                 Drives::here());
}

bool GameInstall::is_game(const std::string& path, const Drives& drives) const {
    if (path.empty()) return false;
    for (const std::string& program : programs)
        if (same_file(path, program, drives)) return true;
    return false;
}

bool GameInstall::beside_the_game(const std::string& path, const Drives& drives) const {
    const std::string folder = folder_of(plain(path, drives));
    if (folder.empty()) return false;
    for (const std::string& program : programs) {
        const std::string theirs = folder_of(plain(program, drives));
        if (theirs.empty()) continue;
        if (same_text_nocase(folder, theirs) || same_on_disk(folder, theirs, drives)) return true;
    }
    return false;
}

std::vector<RunningProgram> GameInstall::games_among(const std::vector<RunningProgram>& running,
                                                     const Drives& drives) const {
    std::vector<RunningProgram> out;
    for (const RunningProgram& p : running)
        if (is_game(p, drives)) out.push_back(p);
    return out;
}

bool GameInstall::names_the_game(const std::string& name) const {
    const std::string asked = trimmed(name);
    if (asked.empty()) return false;
    // The game's own name counts with nowhere known for it too: a Game.exe is
    // then not the game, rather than any Game.exe being taken for it.
    if (same_text_nocase(asked, kGameExe)) return true;
    for (const std::string& program : programs)
        if (same_text_nocase(file_name_of(program), asked)) return true;
    return false;
}

// ------------------------------------------------------- what is running

std::string program_path(unsigned long pid) {
    if (pid == 0) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return {};
    const std::string out = path_of_handle(process);
    CloseHandle(process);
    return out;
}

std::vector<RunningProgram> programs_called(const std::string& name) {
    std::vector<RunningProgram> out;
    const std::string asked = trimmed(name);
    if (asked.empty()) return out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            const std::string exe = wide_to_utf8(entry.szExeFile);
            if (!same_text_nocase(exe, asked)) continue;
            RunningProgram p;
            p.pid = entry.th32ProcessID;
            p.name = exe;
            p.path = program_path(p.pid);
            out.push_back(std::move(p));
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return out;
}

std::vector<RunningProgram> game_programs(const std::string& name, const GameInstall& install) {
    if (install.programs.empty()) return {};
    return install.games_among(programs_called(name), GameInstall::Drives::here());
}

bool game_running(const std::string& name, const GameInstall& install) {
    return !game_programs(name, install).empty();
}

std::vector<InputManager::WindowInfo> game_windows(const std::string& name,
                                                   const GameInstall& install) {
    std::vector<InputManager::WindowInfo> out;
    if (install.programs.empty()) return out;
    const GameInstall::Drives drives = GameInstall::Drives::here();
    for (InputManager::WindowInfo& w : InputManager::windows_of(name))
        if (install.is_game(w.path, drives)) out.push_back(std::move(w));
    return out;
}

// ------------------------------------------------------------------ ending

namespace {

/// End program `pid` if `still` holds of where the very handle that would end
/// it says it runs from -- so that a program which has ended since it was
/// looked at, its number taken by another, is never ended in its place.
/// `what` names it for the log. The caller asks the "no real input" switch.
bool end_if_still(unsigned long pid, const std::function<bool(const std::string&)>& still,
                  const char* what) {
    HANDLE process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        GB_WARN("Cannot end %s (pid %lu) -- it has ended already, or runs at a higher "
                "privilege than this program", what, pid);
        return false;
    }
    bool ended = false;
    if (!still(path_of_handle(process)))
        GB_WARN("Left pid %lu running: it is no longer %s", pid, what);
    else
        ended = TerminateProcess(process, 0) != FALSE;
    CloseHandle(process);
    return ended;
}

} // namespace

bool end_game_program(unsigned long pid, const GameInstall& install) {
    if (refuse_real_input("end a program")) return false;
    const GameInstall::Drives drives = GameInstall::Drives::here();
    return end_if_still(
        pid, [&](const std::string& path) { return install.is_game(path, drives); }, "the game");
}

int end_game(const std::string& name, const GameInstall& install) {
    if (refuse_real_input("end the game")) return 0;
    if (install.programs.empty()) {
        // Nothing is the game then, so nothing is ended -- said once per try,
        // because a client left running needs a reason a person can read.
        GB_WARN("Not ending %s: where the game is installed is not known -- no record of "
                "\"Ghost Online VTC\" names a program that is there, it is not in the usual "
                "folder, and no window of the game's has been seen", name.c_str());
        return 0;
    }
    const GameInstall::Drives drives = GameInstall::Drives::here();
    int ended = 0;
    for (const RunningProgram& p : programs_called(name)) {
        if (!install.is_game(p, drives)) {
            // Said, because a client left running when the keeper meant to
            // end one has a reason a person should be able to read. Not where
            // it runs from: that can be a folder with somebody's name in it.
            GB_INFO("Left %s (pid %lu) running: %s", p.name.c_str(), p.pid,
                    p.path.empty() ? "where it runs from cannot be read, so it is not taken "
                                     "for the game"
                                   : "it runs from somewhere other than where the game is "
                                     "installed");
            continue;
        }
        if (end_game_program(p.pid, install)) ++ended;
    }
    return ended;
}

int end_beside_the_game(const std::string& name, const GameInstall& install) {
    if (trimmed(name).empty()) return 0;
    if (refuse_real_input("end a program")) return 0;
    const GameInstall::Drives drives = GameInstall::Drives::here();
    const auto beside = [&](const std::string& path) {
        return install.beside_the_game(path, drives);
    };
    int ended = 0;
    for (const RunningProgram& p : programs_called(name)) {
        if (!beside(p.path)) {
            // Said, as for the game: a launcher left running is a window the
            // next crossing finds. Not where it runs from, which can be a
            // folder with somebody's name in it.
            GB_INFO("Left %s (pid %lu) running: %s", p.name.c_str(), p.pid,
                    p.path.empty() ? "where it runs from cannot be read, so it is not taken "
                                     "for the game's launcher"
                                   : "it does not run from the game's folder, so it is not the "
                                     "game's launcher");
            continue;
        }
        if (end_if_still(p.pid, beside, "the game's launcher")) ++ended;
    }
    return ended;
}

} // namespace gobot
