#include "gobot/crash_watch.hpp"
#include "gobot/common.hpp"
#include "gobot/game_install.hpp"
#include "gobot/input_manager.hpp"
#include "gobot/logger.hpp"
#include "gobot/no_real_input.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include <windows.h>

namespace gobot {
namespace {

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

/// What Windows hands back for a command line: a counted string, its text
/// straight after it (UNICODE_STRING).
struct CountedText {
    USHORT   bytes;
    USHORT   room;
    wchar_t* text;
};

/// The command line of the program behind `process`, UTF-8, or "" when
/// Windows will not say.
///
/// Asked through a handle that may only ask after the program -- the right
/// that tells where a program runs from, not one that reads its memory --
/// which Windows 8.1 and later answer for this (ProcessCommandLineInformation).
std::string command_line_of_handle(HANDLE process) {
    using Query = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const Query query = reinterpret_cast<Query>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (query == nullptr || process == nullptr) return {};
    constexpr ULONG kCommandLine = 60;      // ProcessCommandLineInformation
    std::vector<unsigned char> room(2048);
    for (int tries = 0; tries < 3; ++tries) {
        ULONG need = 0;
        const LONG status =
            query(process, kCommandLine, room.data(), static_cast<ULONG>(room.size()), &need);
        if (status >= 0) {
            const auto* line = reinterpret_cast<const CountedText*>(room.data());
            if (line->text == nullptr || line->bytes == 0) return {};
            return wide_to_utf8(std::wstring(line->text, line->bytes / sizeof(wchar_t)));
        }
        // Too little room, and Windows says how much it wants. A command line
        // is at most 32767 characters: anything wanting more is not one.
        if (need <= room.size() || need > 70000) return {};
        room.resize(need);
    }
    return {};
}

/// The process a window belongs to, 0 when it cannot be told.
unsigned long owner_of(void* hwnd) {
    DWORD pid = 0;
    if (hwnd != nullptr) GetWindowThreadProcessId(static_cast<HWND>(hwnd), &pid);
    return pid;
}

/// End every crash reporter called `name` that is reporting on one of
/// `games`, and no other: how many were ended.
///
/// Windows' crash reporter is one program for every crash on the machine, so
/// ending it by its name ended other programs' reports, and the crashed
/// programs behind them. Each is asked again through the very handle that
/// ends it, so that one which has gone since it was looked at, its number
/// taken by another program, is never ended in its place.
int end_reporters(const std::string& name, const std::vector<RunningProgram>& games) {
    if (name.empty()) return 0;
    if (refuse_real_input("end a program")) return 0;

    int ended = 0;
    for (const RunningProgram& p : programs_called(name)) {
        if (!CrashWatch::reports_on(CrashWatch::command_line_of(p.pid), games)) {
            GB_INFO("CrashWatch: left %s (pid %lu) running: it is not reporting on the game",
                    p.name.c_str(), p.pid);
            continue;
        }
        HANDLE proc = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                  p.pid);
        if (proc == nullptr) {
            GB_WARN("CrashWatch: cannot end %s (pid %lu) -- it has ended already, or runs at a "
                    "higher privilege than this program", p.name.c_str(), p.pid);
            continue;
        }
        if (CrashWatch::reports_on(command_line_of_handle(proc), games)) {
            if (TerminateProcess(proc, 0)) ++ended;
        } else {
            GB_WARN("CrashWatch: left pid %lu running: it is no longer reporting on the game",
                    p.pid);
        }
        CloseHandle(proc);
    }
    return ended;
}

/// Post a message to another program's window: the only way this file
/// reaches one.
void post(void* hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (refuse_real_input("post a message to another program's window")) return;
    PostMessageA(static_cast<HWND>(hwnd), message, wparam, lparam);
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

    // Which program is the game: by where it runs from, never by its name
    // alone. Found once a look, so that a copy of the game which came up
    // since the last one is known by its window.
    const GameInstall game = game_install();

    // A box first, because a client that is showing one is still running and
    // would otherwise read as merely "not in the world" for as long as the box
    // stays up -- which is forever, since nobody is there to click it. The
    // game's boxes from the game alone; Windows' crash reporter's while it is
    // reporting on the game, as it speaks for a program that can no longer
    // speak itself.
    const auto box_among = [&](const std::string& who,
                               const std::vector<InputManager::WindowInfo>& windows) {
        for (const auto& w : windows) {
            const std::string text = InputManager::window_text(w.hwnd);
            if (!looks_like_a_box(w.window_class, w.title, text)) continue;

            out.what   = Trouble::ErrorBox;
            out.dialog = w.hwnd;
            out.code   = code_in(w.title + "\n" + text);
            out.detail = who + " is showing a box: " +
                         (text.empty() ? w.title : text);
            if (!out.code.empty()) out.detail += "  [code " + out.code + "]";
            hung_since_ms_ = 0;
            return true;
        }
        return false;
    };
    if (!settings_.game_process.empty() &&
        box_among(settings_.game_process, game_windows(settings_.game_process, game)))
        return out;

    // The game's programs now: what a crash reporter has to be reporting on
    // for its box to be the game's, and whether the game is running at all.
    const std::vector<RunningProgram> games = game_programs(settings_.game_process, game);

    // Windows' crash reporter puts up the same box for every program that
    // crashes, so its name says nothing about whose crash it is: the process
    // its command line names does. A box of somebody else's crash read as the
    // game's once ended a healthy client mid-session.
    if (!settings_.reporter_process.empty() && !games.empty()) {
        std::vector<InputManager::WindowInfo> for_the_game;
        for (InputManager::WindowInfo& w : InputManager::windows_of(settings_.reporter_process)) {
            const unsigned long reporter = owner_of(w.hwnd);
            if (reports_on(command_line_of(reporter), games)) {
                for_the_game.push_back(std::move(w));
            } else if (reporter != told_other_reporter_) {
                told_other_reporter_ = reporter;
                GB_INFO("CrashWatch: %s (pid %lu) is up for another program, not the game -- "
                        "left alone", settings_.reporter_process.c_str(), reporter);
            }
        }
        if (box_among(settings_.reporter_process, for_the_game)) return out;
    }

    if (games.empty()) {
        out.what   = Trouble::Gone;
        out.detail = settings_.game_process + " is not running";
        hung_since_ms_ = 0;
        return out;
    }

    // Still running. Is it answering?
    const auto windows = game_windows(settings_.game_process, game);
    bool answering = windows.empty();     // no window yet is not the same as hung
    for (const auto& w : windows)
        if (!InputManager::window_hung(w.hwnd)) { answering = true; break; }

    if (answering) {
        hung_since_ms_ = 0;
        return out;
    }

    const long long t = static_cast<long long>(now_ms());   // the steady clock (common.hpp)
    if (hung_since_ms_ == 0) hung_since_ms_ = t;
    const long long stuck_for = (t - hung_since_ms_) / 1000;
    if (stuck_for >= settings_.hung_grace_s) {
        out.what   = Trouble::Hung;
        out.detail = settings_.game_process + " has not answered for " +
                     std::to_string(stuck_for) + "s";
    }
    return out;
}

CrashWatch::Move CrashWatch::next_move(const Look& seen, bool seen_running, bool start_pending) {
    if (!seen.bad()) return start_pending ? Move::Follow : Move::Watch;
    // Never up since the keeper began: there is no client to end and no
    // session for the server to see close -- the way GoC treats a missing
    // window -- so it is a start. Anything else is the client falling over.
    if (!seen_running && seen.what == Trouble::Gone) return Move::Start;
    return Move::Restart;
}

// ------------------------------------------------------------- shutting down

bool CrashWatch::shut_down(const Look& seen) {
    // Everything here posts to other programs' windows or ends programs, so
    // under the "no real input" switch none of it happens -- not even the
    // waits, which only make sense after something was asked.
    if (refuse_real_input("end the game client")) return false;

    const GameInstall game = game_install();
    const GameInstall::Drives drives = GameInstall::Drives::here();

    // The game's programs as they are before anything is asked to close. A
    // crash reporter is the game's only while it reports on one of these, and
    // the game being ended below must not make its own reporter a stranger.
    const std::vector<RunningProgram> games = game_programs(settings_.game_process, game);

    // Click the box away first -- while it is still the game's own, or its
    // crash reporter's: the look that found it was a moment ago, and a box
    // that has since become somebody else's is not this to answer.
    //
    // A process with a modal dialog up is sitting in that dialog's message
    // loop and will not act on anything else, including a polite request to
    // close. Dismissing it lets the program take its own exit path, which is
    // better than killing it part way through writing its files.
    if (seen.dialog != nullptr && IsWindow(static_cast<HWND>(seen.dialog))) {
        if (box_for_the_game(seen.dialog, games)) {
            GB_INFO("CrashWatch: dismissing the box");
            post(seen.dialog, WM_COMMAND, IDOK, 0);
            post(seen.dialog, WM_CLOSE, 0, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        } else {
            GB_INFO("CrashWatch: left a box up -- it is neither the game's nor a crash report "
                    "on the game");
        }
    }

    // Ask the windows that are left to close, and give them a moment: the
    // game's own, never another program's of its name, and the launcher's --
    // the one beside the game, never a program of its name from elsewhere.
    for (const auto& w : game_windows(settings_.game_process, game))
        post(w.hwnd, WM_CLOSE, 0, 0);
    for (const auto& w : InputManager::windows_of(settings_.launcher_process))
        if (game.beside_the_game(w.path, drives)) post(w.hwnd, WM_CLOSE, 0, 0);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Then end whatever is still there: the game and its launcher by where
    // they run from, the crash reporter by what it is reporting on. Every
    // other program of those names is left running, and the log says so.
    int ended = end_game(settings_.game_process, game);
    ended += end_beside_the_game(settings_.launcher_process, game);
    ended += end_reporters(settings_.reporter_process, games);
    if (ended > 0) GB_INFO("CrashWatch: ended %d process(es)", ended);

    std::this_thread::sleep_for(std::chrono::seconds(2));
    const bool clean = !game_running(settings_.game_process, game);
    if (!clean)
        GB_WARN("CrashWatch: %s is still running. If GoBot is not elevated it "
                "cannot end a process that is.", settings_.game_process.c_str());
    return clean;
}

bool CrashWatch::box_for_the_game(void* box, const std::vector<RunningProgram>& games) const {
    const unsigned long owner = owner_of(box);
    if (owner == 0) return false;
    for (const RunningProgram& g : games)
        if (g.pid == owner) return true;            // one of the game's own
    // Else a crash reporter's, reporting on the game.
    if (settings_.reporter_process.empty()) return false;
    const std::string path = program_path(owner);
    const std::size_t slash = path.find_last_of("\\/");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    return !name.empty() && to_lower(name) == to_lower(settings_.reporter_process) &&
           reports_on(command_line_of(owner), games);
}

// ------------------------------------------------------- the crash reporter

unsigned long CrashWatch::reported_pid(const std::string& command_line) {
    // Split as Windows splits a command line: at spaces and tabs, with a
    // quoted run -- the program's own path, often -- kept whole.
    std::vector<std::string> words;
    std::string word;
    bool quoted = false;
    bool in_word = false;
    for (const char c : command_line) {
        if (c == '"') {
            quoted = !quoted;
            in_word = true;
            continue;
        }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (in_word) words.push_back(word);
            word.clear();
            in_word = false;
            continue;
        }
        word += c;
        in_word = true;
    }
    if (in_word) words.push_back(word);

    for (std::size_t i = 0; i + 1 < words.size(); ++i) {
        const std::string flag = to_lower(words[i]);
        if (flag != "-p" && flag != "/p") continue;
        // The number whole, and nothing but: never "-pss", nor "-p 18x". A
        // line that cannot be read is no report on anything.
        const std::string& n = words[i + 1];
        if (n.empty() || n.size() > 10 || n.find_first_not_of("0123456789") != std::string::npos)
            return 0;
        const unsigned long long pid = std::stoull(n);
        return pid > 0xFFFFFFFFull ? 0 : static_cast<unsigned long>(pid);
    }
    return 0;
}

bool CrashWatch::reports_on(const std::string& command_line,
                            const std::vector<RunningProgram>& games) {
    const unsigned long pid = reported_pid(command_line);
    if (pid == 0) return false;
    for (const RunningProgram& g : games)
        if (g.pid == pid) return true;
    return false;
}

std::string CrashWatch::command_line_of(unsigned long pid) {
    if (pid == 0) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return {};
    const std::string out = command_line_of_handle(process);
    CloseHandle(process);
    return out;
}

} // namespace gobot
