#include "gobot/dpi.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"

#include <windows.h>

#include <cctype>
#include <cmath>
#include <iterator>
#include <sstream>
#include <string>

namespace gobot {

namespace {

// Asked for by name: none of these is in Windows 7 or 8, and an import of one
// would stop every program here starting there.
using SetProcessCtx = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
using SetProcessWay = HRESULT(WINAPI*)(int);      // shcore's SetProcessDpiAwareness
using SetThreadCtx  = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
using GetThreadCtx  = DPI_AWARENESS_CONTEXT(WINAPI*)();
using GetWindowCtx  = DPI_AWARENESS_CONTEXT(WINAPI*)(HWND);
using AwarenessOf   = DPI_AWARENESS(WINAPI*)(DPI_AWARENESS_CONTEXT);
using DpiOfWindow   = UINT(WINAPI*)(HWND);

constexpr int kProcessPerMonitor = 2;   // PROCESS_PER_MONITOR_DPI_AWARE, shellscalingapi.h

struct DpiApi {
    SetProcessCtx set_process = nullptr;    // 10 1703
    SetThreadCtx  set_thread = nullptr;     // 10 1607, as the rest
    GetThreadCtx  get_thread = nullptr;
    GetWindowCtx  get_window = nullptr;
    AwarenessOf   awareness_of = nullptr;
    DpiOfWindow   dpi_of_window = nullptr;

    bool threads() const { return set_thread != nullptr && get_thread != nullptr; }
};

const DpiApi& api() {
    static const DpiApi found = [] {
        DpiApi d;
        if (const HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            d.set_process   = reinterpret_cast<SetProcessCtx>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
            d.set_thread    = reinterpret_cast<SetThreadCtx>(GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
            d.get_thread    = reinterpret_cast<GetThreadCtx>(GetProcAddress(user32, "GetThreadDpiAwarenessContext"));
            d.get_window    = reinterpret_cast<GetWindowCtx>(GetProcAddress(user32, "GetWindowDpiAwarenessContext"));
            d.awareness_of  = reinterpret_cast<AwarenessOf>(GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext"));
            d.dpi_of_window = reinterpret_cast<DpiOfWindow>(GetProcAddress(user32, "GetDpiForWindow"));
        }
        return d;
    }();
    return found;
}

/// The calling thread to `mode`: real pixels are per-monitor v2, or on 10 1607,
/// which has no v2, the first per-monitor way -- the same pixels to everything
/// GoBot asks of a window. What was there before, or null when refused.
DPI_AWARENESS_CONTEXT set_mode(const DpiApi& d, DpiScope::Mode mode) {
    if (mode == DpiScope::Mode::Scaled) return d.set_thread(DPI_AWARENESS_CONTEXT_UNAWARE);
    if (const DPI_AWARENESS_CONTEXT was = d.set_thread(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return was;
    return d.set_thread(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
}

/// shcore, for what user32 has no way of its own to do before 10 1703: from
/// System32 only, as every DLL here is. Null before 8.1, which has none.
HMODULE shcore() {
    HMODULE m = GetModuleHandleW(L"shcore.dll");
    if (m == nullptr) m = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return m;
}

/// shcore's way of making a process per-monitor, for 10 1607.
SetProcessWay shcore_set_process() {
    const HMODULE m = shcore();
    return m != nullptr ? reinterpret_cast<SetProcessWay>(GetProcAddress(m, "SetProcessDpiAwareness")) : nullptr;
}

/// A screen's display scale as its DPI (96 at 100%), or 0 when Windows will not
/// say. Asked in real pixels: a thread Windows scales is told 96 for every one.
UINT screen_dpi(HMONITOR screen) {
    using DpiOfScreen = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static const DpiOfScreen get = [] {
        const HMODULE m = shcore();
        return m != nullptr ? reinterpret_cast<DpiOfScreen>(GetProcAddress(m, "GetDpiForMonitor")) : nullptr;
    }();
    UINT x = 0, y = 0;
    const DpiScope pixels(DpiScope::Mode::RealPixels);
    return get != nullptr && screen != nullptr && SUCCEEDED(get(screen, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) ? x : 0;
}

/// What the start made of the process, once.
struct Start {
    bool aware = false;
    int  view_percent = 100;
};

const Start& start() {
    static const Start found = [] {
        Start s;
        const DpiApi& d = api();
        if (d.threads()) {
            if (d.set_process != nullptr) {
                d.set_process(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            } else if (const SetProcessWay set_way = shcore_set_process()) {
                set_way(kProcessPerMonitor);
            }
            // Refused when the process has a way already: from a manifest, an
            // earlier call, or a high-DPI override on this program's
            // Compatibility tab, which Windows applies before main(). Real
            // pixels only if that way is per monitor. Asked of this thread,
            // which nothing has changed yet: the process's way, which every
            // thread GoBot starts takes.
            s.aware = thread_sees_real_pixels();
        }
        // How the main screen looks to those threads: its real width over the
        // width they are told.
        if (const HDC screen = GetDC(nullptr)) {
            const int told = GetDeviceCaps(screen, HORZRES);
            const int real = GetDeviceCaps(screen, DESKTOPHORZRES);
            ReleaseDC(nullptr, screen);
            if (told > 0 && real > 0) s.view_percent = static_cast<int>(std::lround(100.0 * real / told));
        }
        return s;
    }();
    return found;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool is_dpi_mode(const std::string& word) {
    const std::string w = upper(word);
    return w == "HIGHDPIAWARE" || w == "DPIUNAWARE" || w == "GDIDPISCALING";
}

bool has_dpi_word(const std::string& layers) {
    std::istringstream words(layers);
    std::string word;
    while (words >> word)
        if (is_dpi_mode(word)) return true;
    return false;
}

/// The program's value under `key`, or empty when there is none.
std::string read_layers(HKEY root, const std::wstring& key, const std::wstring& exe) {
    wchar_t buf[4096]{};
    DWORD bytes = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(root, key.c_str(), exe.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr,
                     buf, &bytes) != ERROR_SUCCESS)
        return {};
    return wide_to_utf8(buf);
}

/// Writes it, or with nothing left, takes the value away. ERROR_SUCCESS or why not.
LSTATUS write_layers(HKEY root, const std::wstring& key, const std::wstring& exe, const std::string& layers) {
    if (layers.empty()) {
        const LSTATUS rc = RegDeleteKeyValueW(root, key.c_str(), exe.c_str());
        return rc == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : rc;
    }
    const std::wstring value = utf8_to_wide(layers);
    HKEY opened = nullptr;
    LSTATUS rc = RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &opened, nullptr);
    if (rc != ERROR_SUCCESS) return rc;
    rc = RegSetValueExW(opened, exe.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(opened);
    return rc;
}

/// When `exe`'s override was set: as GoBot set it (kDpiStampKey), or else
/// when this user's compatibility settings were last written -- the override
/// among them, as the Compatibility tab sets it by hand. That key's own time
/// moves with every program's setting, so it is only the fallback. False when
/// neither is known.
bool override_set_at(const std::wstring& wexe, FILETIME& at) {
    ULONGLONG stamp = 0;
    DWORD bytes = sizeof(stamp);
    if (RegGetValueW(HKEY_CURRENT_USER, utf8_to_wide(kDpiStampKey).c_str(), wexe.c_str(), RRF_RT_REG_QWORD, nullptr,
                     &stamp, &bytes) == ERROR_SUCCESS &&
        stamp != 0) {
        at.dwLowDateTime = static_cast<DWORD>(stamp & 0xFFFFFFFFu);
        at.dwHighDateTime = static_cast<DWORD>(stamp >> 32);
        return true;
    }
    HKEY layers = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, utf8_to_wide(kCompatLayersKey).c_str(), 0, KEY_QUERY_VALUE, &layers) !=
        ERROR_SUCCESS)
        return false;
    const LSTATUS rc = RegQueryInfoKeyW(layers, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                                        nullptr, nullptr, nullptr, &at);
    RegCloseKey(layers);
    return rc == ERROR_SUCCESS;
}

/// Whether the window's program, `exe`, was started after its override was set.
bool started_since_override(void* window, const std::string& exe) {
    DWORD pid = 0;
    GetWindowThreadProcessId(static_cast<HWND>(window), &pid);
    if (pid == 0) return false;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return false;
    FILETIME created{}, exited{}, kernel{}, user{};
    const BOOL timed = GetProcessTimes(process, &created, &exited, &kernel, &user);
    CloseHandle(process);
    FILETIME set{};
    return timed && override_set_at(utf8_to_wide(exe), set) && CompareFileTime(&created, &set) > 0;
}

} // namespace

bool become_dpi_aware() { return start().aware; }

int threads_view_percent() { return start().view_percent; }

bool real_pixels_possible() { return api().threads(); }

DpiScope::DpiScope(Mode mode) {
    const DpiApi& d = api();
    // Null when refused, and then there is nothing to put back.
    if (d.threads()) previous_ = set_mode(d, mode);
}

DpiScope::DpiScope(void* window) {
    const DpiApi& d = api();
    if (window == nullptr || !d.threads() || d.get_window == nullptr) return;
    if (const DPI_AWARENESS_CONTEXT its = d.get_window(static_cast<HWND>(window))) previous_ = d.set_thread(its);
}

DpiScope::~DpiScope() {
    if (previous_ != nullptr) api().set_thread(static_cast<DPI_AWARENESS_CONTEXT>(previous_));
}

void set_thread_dpi(DpiScope::Mode mode) {
    const DpiApi& d = api();
    if (d.threads()) set_mode(d, mode);
}

bool thread_sees_real_pixels() {
    const DpiApi& d = api();
    if (d.get_thread == nullptr || d.awareness_of == nullptr) return false;
    return d.awareness_of(d.get_thread()) == DPI_AWARENESS_PER_MONITOR_AWARE;
}

WindowScaling window_scaling(void* window) {
    WindowScaling s;
    const HWND hwnd = static_cast<HWND>(window);
    const DpiApi& d = api();
    if (hwnd == nullptr || !IsWindow(hwnd) || !d.threads() || d.get_window == nullptr) return s;

    // The same client area asked for twice: in real pixels, and in the window's
    // own way of seeing them. Windows answers each caller in its own pixels.
    // Either switch refused, the two would be asked in the same pixels and
    // always agree: a stretched game would pass for one shown as it is.
    RECT real{}, own{};
    {
        const DpiScope pixels(DpiScope::Mode::RealPixels);
        if (!thread_sees_real_pixels() || !GetClientRect(hwnd, &real)) return s;
    }
    const DPI_AWARENESS_CONTEXT its = d.get_window(hwnd);
    if (its == nullptr) return s;
    const DPI_AWARENESS_CONTEXT before = d.set_thread(its);
    if (before == nullptr) return s;
    const BOOL got = GetClientRect(hwnd, &own);
    d.set_thread(before);
    if (!got) return s;

    s.real_w = real.right - real.left;
    s.real_h = real.bottom - real.top;
    s.own_w = own.right - own.left;
    s.own_h = own.bottom - own.top;
    if (s.real_w <= 0 || s.real_h <= 0 || s.own_w <= 0 || s.own_h <= 0) return s;
    s.known = true;
    s.percent = static_cast<int>(std::lround(100.0 * s.real_w / s.own_w));
    const DPI_AWARENESS way = d.awareness_of != nullptr ? d.awareness_of(its) : DPI_AWARENESS_INVALID;
    s.handles_dpi = way == DPI_AWARENESS_SYSTEM_AWARE || way == DPI_AWARENESS_PER_MONITOR_AWARE;
    const HMONITOR here = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO screen{};
    screen.cbSize = sizeof(screen);
    if (GetMonitorInfoW(here, &screen)) s.on_main_screen = (screen.dwFlags & MONITORINFOF_PRIMARY) != 0;
    if (!s.on_main_screen) {
        const UINT here_dpi = screen_dpi(here);
        const UINT main_dpi = screen_dpi(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
        s.screen_like_main = here_dpi == 0 || main_dpi == 0 || here_dpi == main_dpi;
    }
    return s;
}

int drawn_at_percent(void* window) {
    const DpiApi& d = api();
    if (window == nullptr || d.dpi_of_window == nullptr) return 0;
    const UINT dpi = d.dpi_of_window(static_cast<HWND>(window));
    return dpi == 0 ? 0 : static_cast<int>((dpi * 100 + 48) / 96);
}

ScaleCure cure_for(const WindowScaling& s, bool overridden, bool started_since) {
    if (!s.stretched()) return ScaleCure::None;
    if (!s.handles_dpi) {
        if (!overridden) {
            if (s.on_main_screen) return ScaleCure::SetOverride;
            // The override has it draw at the main screen's scale: a cure on
            // another screen only where that one has the same scale. Elsewhere
            // it would be shown stretched still, after a start again for nothing.
            return s.screen_like_main ? ScaleCure::SetOverrideOrMove : ScaleCure::MoveToMainScreen;
        }
        // Set, and the program started since, yet it still does not handle
        // DPI: Windows did not take it -- a path it does not start the program
        // by, say, or a setting for all users that says otherwise.
        return started_since ? ScaleCure::OverrideIgnored : ScaleCure::RestartGame;
    }
    // One that handles DPI draws for the main screen's scale as it was when it
    // started, and Windows scales it to any other.
    return s.on_main_screen ? ScaleCure::RestartOrSignIn : ScaleCure::MoveToMainScreen;
}

ScaleCure cure_for(void* hwnd, const WindowScaling& s) {
    if (!s.stretched() || s.handles_dpi) return cure_for(s, false);
    // Whether it is set is asked of the registry, not remembered: it outlives
    // a run of GoBot, and so does the game.
    const std::string exe = window_program(hwnd);
    const bool overridden = has_high_dpi_override(exe);
    return cure_for(s, overridden, overridden && started_since_override(hwnd, exe));
}

const char* cure_advice(ScaleCure cure) {
    switch (cure) {
        case ScaleCure::SetOverride:
            return "its program does not handle display scale itself: give it the high-DPI override "
                   "and start it again, or set Windows' display scale to 100%";
        case ScaleCure::SetOverrideOrMove:
            return "its program does not handle display scale itself, and this screen is scaled: move "
                   "it to a screen at 100%, or give it the high-DPI override and start it again";
        case ScaleCure::RestartGame:
            return "its program has the high-DPI override, but was started before it was set: close "
                   "it and start it again";
        case ScaleCure::OverrideIgnored:
            return "its program has the high-DPI override and was started since, but Windows does not "
                   "take it: set it by hand on the program the launcher starts (Properties > "
                   "Compatibility > Change high DPI settings > Override high DPI scaling behavior: "
                   "Application), or set Windows' display scale to 100%";
        case ScaleCure::MoveToMainScreen:
            return "this screen's display scale is not the main screen's, and no setting of its program "
                   "cures that: move it to the main screen, or give both screens the same scale";
        case ScaleCure::RestartOrSignIn:
            return "it draws at the scale the main screen had before: start it again, and if that "
                   "does not do it, sign out of Windows and in again";
        case ScaleCure::None:
            break;
    }
    return "nothing to do";
}

std::string window_program(void* window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(static_cast<HWND>(window), &pid);
    if (pid == 0) return {};
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return {};
    wchar_t path[1024]{};
    DWORD n = static_cast<DWORD>(std::size(path));
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &n);
    CloseHandle(process);
    return ok ? wide_to_utf8(std::wstring(path, n)) : std::string();
}

std::string own_exe_path() {
    wchar_t path[1024]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return n == 0 || n >= std::size(path) ? std::string() : wide_to_utf8(std::wstring(path, n));
}

std::string with_high_dpi_override(const std::string& layers) {
    std::istringstream words(layers);
    std::string word, kept;
    while (words >> word) {
        if (word == "~" || is_dpi_mode(word)) continue;
        kept += ' ';
        kept += word;
    }
    return "~" + kept + " HIGHDPIAWARE";
}

std::string without_dpi_override(const std::string& layers) {
    std::istringstream words(layers);
    std::string word, kept;
    bool tilde = false;
    while (words >> word) {
        if (word == "~") {
            tilde = true;
            continue;
        }
        if (is_dpi_mode(word)) continue;
        if (!kept.empty()) kept += ' ';
        kept += word;
    }
    if (kept.empty()) return {};
    return tilde ? "~ " + kept : kept;
}

bool has_high_dpi_override(const std::string& exe, const char* key) {
    if (exe.empty()) return false;
    std::istringstream words(read_layers(HKEY_CURRENT_USER, utf8_to_wide(key), utf8_to_wide(exe)));
    std::string word;
    while (words >> word)
        if (upper(word) == "HIGHDPIAWARE") return true;
    return false;
}

bool set_high_dpi_override(const std::string& exe, std::string* why, const char* key, const char* stamp_key) {
    const auto fail = [why](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (exe.empty()) return fail("no program to set it for");

    const std::wstring wkey = utf8_to_wide(key);
    const std::wstring wexe = utf8_to_wide(exe);
    const std::string value = with_high_dpi_override(read_layers(HKEY_CURRENT_USER, wkey, wexe));
    const LSTATUS rc = write_layers(HKEY_CURRENT_USER, wkey, wexe, value);
    if (rc != ERROR_SUCCESS)
        return fail("Windows would not save the setting (error " + std::to_string(rc) + ")");

    // When, for telling a game started since from one started before (the
    // Layers key's own time moves with every program's setting). Nothing is
    // lost without it but that telling, so a refusal is not a failure.
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG stamp = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    HKEY stamps = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, utf8_to_wide(stamp_key).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &stamps, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(stamps, wexe.c_str(), 0, REG_QWORD, reinterpret_cast<const BYTE*>(&stamp), sizeof(stamp));
        RegCloseKey(stamps);
    }

    GB_INFO("Display scale: %s is set to draw at its own size (%s); it takes effect when it is started again",
            exe.c_str(), value.c_str());
    return true;
}

bool has_any_dpi_override(const std::string& exe, const char* key) {
    if (exe.empty()) return false;
    const std::wstring wkey = utf8_to_wide(key);
    const std::wstring wexe = utf8_to_wide(exe);
    return has_dpi_word(read_layers(HKEY_CURRENT_USER, wkey, wexe)) ||
           has_dpi_word(read_layers(HKEY_LOCAL_MACHINE, wkey, wexe));
}

bool clear_dpi_override(const std::string& exe, std::string* why, const char* key) {
    const auto fail = [why](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    if (exe.empty()) return fail("no program to clear it for");

    const std::wstring wkey = utf8_to_wide(key);
    const std::wstring wexe = utf8_to_wide(exe);
    // All users' as well: the tab's "Change settings for all users" puts it
    // there, and it applies just the same.
    const struct { HKEY root; const char* name; } kHives[] = {
        { HKEY_CURRENT_USER, "this user's" },
        { HKEY_LOCAL_MACHINE, "all users'" },
    };
    for (const auto& hive : kHives) {
        const std::string was = read_layers(hive.root, wkey, wexe);
        if (!has_dpi_word(was)) continue;
        const std::string now = without_dpi_override(was);
        const LSTATUS rc = write_layers(hive.root, wkey, wexe, now);
        if (rc != ERROR_SUCCESS)
            return fail(std::string("Windows would not change ") + hive.name + " setting (error " +
                        std::to_string(rc) + ")");
        GB_INFO("Display scale: took the high-DPI override off %s, %s setting (was '%s', now '%s'); it takes "
                "effect when it is started again",
                exe.c_str(), hive.name, was.c_str(), now.c_str());
    }
    return true;
}

} // namespace gobot
