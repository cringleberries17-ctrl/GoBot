#include "gobot/crash_watch.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/session_manager.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

#include <windows.h>
#include <tlhelp32.h>

namespace gobot {
namespace {

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

/// Read a run of digits starting at `at`, or "" if there are none there.
std::string digits_at(const std::string& text, std::size_t at) {
    std::size_t end = at;
    while (end < text.size() && is_digit(text[end])) ++end;
    return text.substr(at, end - at);
}

/// Skip spaces, colons and the odd '#' between a word and its number.
std::size_t past_filler(const std::string& text, std::size_t at) {
    while (at < text.size() &&
           (text[at] == ' ' || text[at] == ':' || text[at] == '#' ||
            text[at] == '=' || text[at] == '\t'))
        ++at;
    return at;
}

/// End every process of this name. Returns how many were ended.
///
/// By name rather than by handle because the thing being ended has usually
/// already lost its window, which is the only handle anybody was holding.
int end_all(const std::string& exe_name) {
    if (exe_name.empty()) return 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    const std::string want = to_lower(exe_name);
    int ended = 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            char name[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1, name, MAX_PATH,
                                nullptr, nullptr);
            if (to_lower(name) != want) continue;

            HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
            if (proc == nullptr) {
                GB_WARN("CrashWatch: cannot end %s (pid %lu) -- it is running at a "
                        "higher privilege than this program", name,
                        static_cast<unsigned long>(entry.th32ProcessID));
                continue;
            }
            if (TerminateProcess(proc, 0)) ++ended;
            CloseHandle(proc);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return ended;
}

} // namespace

const char* CrashWatch::trouble_name(Trouble t) {
    switch (t) {
        case Trouble::None:     return "fine";
        case Trouble::Gone:     return "the client has exited";
        case Trouble::Hung:     return "the client has stopped answering";
        case Trouble::ErrorBox: return "the client is showing an error";
    }
    return "?";
}

// ------------------------------------------------------------------ settings

void CrashWatch::Settings::load(const IniFile& ini) {
    game_process     = ini.get("Crash", "game_process", game_process);
    launcher_process = ini.get("Crash", "launcher_process", launcher_process);
    reporter_process = ini.get("Crash", "reporter_process", reporter_process);
    hung_grace_s = std::max(5, ini.get_int("Crash", "hung_grace_s", hung_grace_s));
    max_restarts = std::max(0, ini.get_int("Crash", "max_restarts", max_restarts));
    cooldown_s   = std::max(0, ini.get_int("Crash", "cooldown_s", cooldown_s));
}

void CrashWatch::Settings::save(IniFile& ini) const {
    ini.set("Crash", "game_process", game_process);
    ini.set("Crash", "launcher_process", launcher_process);
    ini.set("Crash", "reporter_process", reporter_process);
    ini.set_int("Crash", "hung_grace_s", hung_grace_s);
    ini.set_int("Crash", "max_restarts", max_restarts);
    ini.set_int("Crash", "cooldown_s", cooldown_s);
}

CrashWatch::CrashWatch(Settings s) : settings_(std::move(s)) {}

// -------------------------------------------------------------- reading a box

std::string CrashWatch::code_in(const std::string& text) {
    const std::string lower = to_lower(text);

    // A hex code, which is what Windows itself produces.
    const std::size_t hex = lower.find("0x");
    if (hex != std::string::npos) {
        std::size_t end = hex + 2;
        while (end < lower.size() && std::isxdigit(static_cast<unsigned char>(lower[end])))
            ++end;
        if (end > hex + 2) return text.substr(hex, end - hex);
    }

    // A number after the word for "error". Vietnamese first, because this
    // client is Vietnamese and says "Lỗi"; the accents are left out of the
    // match because the text arrives in whatever code page the dialog used
    // and "l?i" should still be recognised.
    static const char* kWords[] = { "error", "err", "code", "m\xC3\xA3 l\xE1\xBB\x97i",
                                    "l\xE1\xBB\x97i", "ma loi", "loi" };
    for (const char* word : kWords) {
        std::size_t at = lower.find(word);
        while (at != std::string::npos) {
            const std::size_t number = past_filler(lower, at + std::strlen(word));
            const std::string got = digits_at(lower, number);
            if (!got.empty()) return got;
            at = lower.find(word, at + 1);
        }
    }

    // A bracketed number, which is how this client writes one. Its disconnect
    // box says, in full:
    //
    //   Connect Error
    //   Kết nối với máy chủ đã bị ngắt.[122]
    //
    // -- square brackets, hard against the full stop, and the word "Error" on
    // a different line from the number so that looking after the word finds
    // nothing. Every invented test case passed before this dialog was seen;
    // the dialog did not.
    static const char kOpen[]  = "([{";
    static const char kClose[] = ")]}";
    for (int kind = 0; kind < 3; ++kind) {
        std::size_t open = lower.find(kOpen[kind]);
        while (open != std::string::npos) {
            const std::string got = digits_at(lower, open + 1);
            if (!got.empty() && open + 1 + got.size() < lower.size() &&
                lower[open + 1 + got.size()] == kClose[kind])
                return got;
            open = lower.find(kOpen[kind], open + 1);
        }
    }
    return {};
}

bool CrashWatch::looks_like_a_box(const std::string& window_class,
                                  const std::string& title,
                                  const std::string& text) {
    // A standard dialog. Every message box Windows draws is one of these, and
    // so is almost every one a game draws with the API rather than with its
    // own renderer.
    if (window_class == "#32770") return true;

    // Windows' own crash reporter does not use that class, and the thing it
    // says is the most useful sentence in the whole business.
    const std::string all = to_lower(title + "\n" + text);
    return all.find("stopped working") != std::string::npos ||
           all.find("has stopped") != std::string::npos ||
           all.find("ng\xC3\xABng ho\xE1\xBA\xA1t \xC4\x91\xE1\xBB\x99ng") != std::string::npos;
}

// --------------------------------------------------------------- the one look

CrashWatch::Look CrashWatch::look() {
    Look out;

    // A box first, because a client that is showing one is still running and
    // would otherwise read as merely "not in the world" for as long as the box
    // stays up -- which is forever, since nobody is there to click it.
    for (const std::string& who : { settings_.game_process, settings_.reporter_process }) {
        if (who.empty()) continue;
        for (const auto& w : InputManager::windows_of(who)) {
            const std::string text = InputManager::window_text(w.hwnd);
            if (!looks_like_a_box(w.window_class, w.title, text)) continue;

            out.what   = Trouble::ErrorBox;
            out.dialog = w.hwnd;
            out.code   = code_in(w.title + "\n" + text);
            out.detail = who + " is showing a box: " +
                         (text.empty() ? w.title : text);
            if (!out.code.empty()) out.detail += "  [code " + out.code + "]";
            hung_since_ms_ = 0;
            return out;
        }
    }

    if (!SessionManager::process_running(settings_.game_process)) {
        out.what   = Trouble::Gone;
        out.detail = settings_.game_process + " is not running";
        hung_since_ms_ = 0;
        return out;
    }

    // Still running. Is it answering?
    const auto windows = InputManager::windows_of(settings_.game_process);
    bool answering = windows.empty();     // no window yet is not the same as hung
    for (const auto& w : windows)
        if (!InputManager::window_hung(w.hwnd)) { answering = true; break; }

    if (answering) {
        hung_since_ms_ = 0;
        return out;
    }

    const long long t = now_ms();
    if (hung_since_ms_ == 0) hung_since_ms_ = t;
    const long long stuck_for = (t - hung_since_ms_) / 1000;
    if (stuck_for >= settings_.hung_grace_s) {
        out.what   = Trouble::Hung;
        out.detail = settings_.game_process + " has not answered for " +
                     std::to_string(stuck_for) + "s";
    }
    return out;
}

// ------------------------------------------------------------- shutting down

bool CrashWatch::shut_down(const Look& seen) {
    // Click the box away first.
    //
    // A process with a modal dialog up is sitting in that dialog's message
    // loop and will not act on anything else, including a polite request to
    // close. Dismissing it lets the program take its own exit path, which is
    // better than killing it part way through writing its files.
    if (seen.dialog != nullptr && IsWindow(static_cast<HWND>(seen.dialog))) {
        GB_INFO("CrashWatch: dismissing the box");
        PostMessageA(static_cast<HWND>(seen.dialog), WM_COMMAND, IDOK, 0);
        PostMessageA(static_cast<HWND>(seen.dialog), WM_CLOSE, 0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }

    // Ask the windows that are left to close, and give them a moment.
    for (const std::string& who : { settings_.game_process, settings_.launcher_process })
        for (const auto& w : InputManager::windows_of(who))
            PostMessageA(static_cast<HWND>(w.hwnd), WM_CLOSE, 0, 0);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Then end whatever is still there.
    int ended = 0;
    for (const std::string& who : { settings_.game_process, settings_.launcher_process,
                                    settings_.reporter_process }) {
        if (who.empty()) continue;
        if (SessionManager::process_running(who)) ended += end_all(who);
    }
    if (ended > 0) GB_INFO("CrashWatch: ended %d process(es)", ended);

    std::this_thread::sleep_for(std::chrono::seconds(2));
    const bool clean = !SessionManager::process_running(settings_.game_process);
    if (!clean)
        GB_WARN("CrashWatch: %s is still running. If GoBot is not elevated it "
                "cannot end a process that is.", settings_.game_process.c_str());
    return clean;
}

} // namespace gobot
