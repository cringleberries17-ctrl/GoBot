#include "gobot/session_manager.hpp"
#include "gobot/common.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/vitals_reader.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <windows.h>
#include <tlhelp32.h>

namespace gobot {

const char* SessionManager::state_name(State s) {
    switch (s) {
        case State::NoWindow:   return "not running";
        case State::NotPlaying: return "not in the world";
        case State::Playing:    return "playing";
        case State::Unknown:    break;
    }
    return "unknown";
}

const char* SessionManager::event_name(Event e) {
    switch (e) {
        case Event::StateChanged:     return "state changed";
        case Event::SessionLost:      return "session lost";
        case Event::SessionRegained:  return "session regained";
        case Event::NeedsAPerson:     return "needs a person";
        case Event::StoppingOnTimer:  return "stopping on timer";
        case Event::ShuttingDown:     return "shutting down";
    }
    return "?";
}

void SessionManager::Settings::load(const IniFile& ini) {
    poll_ms            = ini.get_int ("Session", "poll_ms",            poll_ms);
    lost_grace_s       = ini.get_int ("Session", "lost_grace_s",       lost_grace_s);
    stop_after_minutes = ini.get_int ("Session", "stop_after_minutes", stop_after_minutes);
    shutdown_when_done = ini.get_bool("Session", "shutdown_when_done", shutdown_when_done);
    shutdown_delay_s   = ini.get_int ("Session", "shutdown_delay_s",   shutdown_delay_s);

    poll_ms          = std::clamp(poll_ms, 250, 60000);
    lost_grace_s     = std::clamp(lost_grace_s, 3, 3600);
    shutdown_delay_s = std::clamp(shutdown_delay_s, 10, 3600);
}

void SessionManager::Settings::save(IniFile& ini) const {
    ini.set_int ("Session", "poll_ms",            poll_ms);
    ini.set_int ("Session", "lost_grace_s",       lost_grace_s);
    ini.set_int ("Session", "stop_after_minutes", stop_after_minutes);
    ini.set_bool("Session", "shutdown_when_done", shutdown_when_done);
    ini.set_int ("Session", "shutdown_delay_s",   shutdown_delay_s);
}

SessionManager::SessionManager() = default;
SessionManager::~SessionManager() { stop(); }

void SessionManager::set_settings(const Settings& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = s;
}

SessionManager::Settings SessionManager::settings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

void SessionManager::set_window_hints(const std::string& title,
                                      const std::string& window_class,
                                      const std::string& process_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    title_ = title;
    class_ = window_class;
    process_ = process_name;
}

void SessionManager::on_event(std::function<void(Event, const std::string&)> fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(fn);
}

void SessionManager::fire(Event e, const std::string& detail) {
    std::function<void(Event, const std::string&)> fn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fn = callback_;
    }
    if (fn) fn(e, detail);
}

void SessionManager::start() {
    if (running_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    finished_.store(false);
    state_.store(State::Unknown);
    thread_ = std::thread(&SessionManager::loop, this);
    GB_INFO("Session watcher started");
}

void SessionManager::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    state_.store(State::Unknown);
    GB_INFO("Session watcher stopped");
}

SessionManager::Stats SessionManager::stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

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

void SessionManager::loop() {
    ScreenCapture capture;
    InputManager  finder;
    Frame         frame;

    const auto started = std::chrono::steady_clock::now();
    std::uint64_t not_playing_since = 0;
    bool announced_lost = false;

    while (running_.load()) {
        Settings s;
        std::string title, klass, process;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            s = settings_;
            title = title_; klass = class_; process = process_;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(s.poll_ms));
        if (!running_.load()) break;

        // Find the window afresh every time. Holding a handle across a client
        // restart means watching a window that no longer exists and concluding
        // the game is gone when it is sitting right there with a new one.
        State now = State::NoWindow;
        if (finder.find_game_window(title, klass, process) && finder.window_valid()) {
            if (!capture.grab(finder.target_window(), frame) || !frame.valid()) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.captures_failed;
                continue;               // say nothing rather than guess
            }
            now = hud_present(frame) ? State::Playing : State::NotPlaying;
        }

        const State was = state_.exchange(now);
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.looks;
            if (now == State::Playing) stats_.seconds_playing += s.poll_ms / 1000;
            stats_.seconds_watching =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - started).count());
        }

        if (now != was) {
            if (was != State::Unknown)
                GB_INFO("Session: %s -> %s", state_name(was), state_name(now));
            fire(Event::StateChanged, state_name(now));
        }

        // ---- has the session dropped? ----
        if (now == State::Playing) {
            not_playing_since = 0;
            if (announced_lost) {
                announced_lost = false;
                fire(Event::SessionRegained, "the HUD is back");
                GB_INFO("Session: back in the world");
            }
        } else {
            const std::uint64_t t = now_ms();
            if (not_playing_since == 0) not_playing_since = t;

            const std::uint64_t gone_for = (t - not_playing_since) / 1000;
            if (!announced_lost &&
                gone_for >= static_cast<std::uint64_t>(s.lost_grace_s)) {
                announced_lost = true;
                {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    ++stats_.sessions_lost;
                }
                const std::string why = now == State::NoWindow
                    ? "the client is not running"
                    : "the client is running but the character is not in the world";
                GB_WARN("Session lost after %llus: %s",
                        static_cast<unsigned long long>(gone_for), why.c_str());
                fire(Event::SessionLost, why);
                fire(Event::NeedsAPerson, why);
            }
        }

        // ---- has the run gone on long enough? ----
        if (s.stop_after_minutes > 0 && !finished_.load()) {
            const auto mins = std::chrono::duration_cast<std::chrono::minutes>(
                std::chrono::steady_clock::now() - started).count();
            if (mins >= s.stop_after_minutes) {
                finished_.store(true);
                GB_INFO("Session: %d minutes up, stopping", s.stop_after_minutes);
                fire(Event::StoppingOnTimer, std::to_string(s.stop_after_minutes) +
                                             " minutes up");
                if (s.shutdown_when_done) {
                    const std::string why = "GoBot finished its run";
                    fire(Event::ShuttingDown, why);
                    request_shutdown(s.shutdown_delay_s, why);
                }
            }
        }
    }
}

// ------------------------------------------------------------------ processes

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

bool SessionManager::request_shutdown(int delay_s, const std::string& why) {
    delay_s = std::clamp(delay_s, 10, 3600);

    // Loud, and in the log, because somebody will come back to a machine that
    // is off and want to know what turned it off.
    GB_WARN("SHUTTING DOWN in %d seconds: %s.  Run `shutdown /a` to call it off.",
            delay_s, why.c_str());

    char command[512];
    std::snprintf(command, sizeof command,
                  "shutdown.exe /s /t %d /c \"GoBot: %.200s\"", delay_s, why.c_str());

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        GB_WARN("Could not ask Windows to shut down (error %lu)", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace gobot
