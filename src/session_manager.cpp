#include "gobot/session_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/no_real_input.hpp"
#include "gobot/vitals_reader.hpp"

#include <cstdlib>
#include <cstring>

#include <windows.h>
#include <tlhelp32.h>

namespace gobot {

bool SessionManager::hud_present(const Frame& f) {
    if (!f.valid()) return false;
    BarSpec hp, mp;
    return VitalsReader::find_bars(f, hp, mp) == 2 &&
           hp.valid() && mp.valid() &&
           // Found as a pair and stacked, which is what `find_bars` promises
           // only when it found two. When it finds one it falls back to the
           // topmost band of each colour, and those can be anything -- a red
           // banner on a loading screen above a blue one is two bars by that
           // reckoning. Insisting they line up and sit close together rejects
           // the fallback's guesses without rejecting a real HUD.
           std::abs(hp.x - mp.x) <= 4 &&
           mp.y > hp.y && mp.y - hp.y < 40;
}

bool SessionManager::process_running(const std::string& exe_name) {
    if (exe_name.empty()) return false;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;

    if (Process32FirstW(snap, &entry)) {
        do {
            char name[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1, name, sizeof name,
                                nullptr, nullptr);
            if (_stricmp(name, exe_name.c_str()) == 0) { found = true; break; }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

bool SessionManager::start_program(const std::string& path) {
    if (path.empty()) return false;
    // Asked before anything about the program, so that a test can aim this at
    // one that is not there and still see it refused.
    if (refuse_real_input("start a program")) return false;

    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wide_len <= 0) return false;
    std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), wide_len);

    // Start it in its own directory. Game launchers routinely load data by
    // relative path and fail in confusing ways when started from elsewhere.
    std::wstring dir = wide;
    const std::size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    else dir.clear();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::wstring command = L"\"" + wide + L"\"";
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr,
                                   dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    if (!ok) {
        GB_WARN("Could not start '%s' (error %lu)", path.c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    GB_INFO("Started %s", path.c_str());
    return true;
}

} // namespace gobot
