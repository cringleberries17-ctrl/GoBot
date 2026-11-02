#include "gobot/input_manager.hpp"
#include "gobot/common.hpp"
#include "gobot/game_install.hpp"
#include "gobot/logger.hpp"
#include "gobot/no_real_input.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

/// Full path of the executable owning `hwnd`, UTF-8, or "" if it can't be
/// determined. Wide underneath: the narrow call turned a folder named in
/// Vietnamese into question marks.
std::string path_of(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return program_path(pid);
}

/// The file name at the end of a path.
std::string file_name(const std::string& path) {
    const auto slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A window's text and class, UTF-8, without asking the window anything.
//
// GetWindowText sends WM_GETTEXT to a window of this process and waits for
// the thread that owns it -- for GoBot's own window, the UI thread. For
// another program's window it reads the text Windows keeps, which
// InternalGetWindowText reads for every window, with no message at all. Wide,
// so a Vietnamese title arrives as UTF-8 rather than in the code page. Never
// GetWindowTextLength here: it sends WM_GETTEXTLENGTH to GoBot's own windows.
std::string text_of(HWND hwnd) {
    wchar_t text[1024]{};
    const int n = InternalGetWindowText(hwnd, text, 1024);
    if (n <= 0) return {};
    return wide_to_utf8(std::wstring(text, static_cast<std::size_t>(std::min(n, 1023))));
}

std::string class_of(HWND hwnd) {
    wchar_t cls[256]{};
    const int n = GetClassNameW(hwnd, cls, 256);
    if (n <= 0) return {};
    return wide_to_utf8(std::wstring(cls, static_cast<std::size_t>(std::min(n, 255))));
}

/// Whether a window's thread takes a message within 100 ms. Attaching to a
/// thread pools the two threads' input, and one that has stopped answering
/// then holds the one that attached -- GoBot's UI thread, F12 and all.
bool answers(HWND hwnd) {
    DWORD_PTR ignored = 0;
    return SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_NORMAL | SMTO_ABORTIFHUNG, 100, &ignored) != 0;
}

BOOL CALLBACK collect_proc(HWND hwnd, LPARAM param) {
    auto* list = reinterpret_cast<std::vector<InputManager::WindowInfo>*>(param);

    if (!IsWindowVisible(hwnd)) return TRUE;

    // Never this process's own windows, and passed over before anything is
    // read off them. The keeper lists windows from its own thread, and reading
    // GoBot's own title sent WM_GETTEXT to the UI thread, which may be waiting
    // in join() for the keeper after F12, DỪNG or unticking: GoBot then showed
    // Not Responding, and the rest of the stop, release_all() included, never
    // ran.
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    // Tool windows and tiny shells are never the game.
    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    if ((r.right - r.left) < 200 || (r.bottom - r.top) < 150) return TRUE;

    // Nor another copy of GoBot: sending input to one does nothing useful and
    // makes the list confusing.
    const std::string path = program_path(pid);
    const std::string proc = file_name(path);
    static const std::string self = [] {
        wchar_t p[MAX_PATH]{};
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        return file_name(wide_to_utf8(p));
    }();
    if (!proc.empty() && _stricmp(proc.c_str(), self.c_str()) == 0) return TRUE;

    // An untitled window is still a window.
    //
    // This used to drop them, which cost a day: the game's launcher -- the one
    // with the purple start button on it -- has no title at all. It is a bare
    // #32770 dialog put up by GhostOnlineVTC.exe, and because it was never in
    // this list, nothing could find it, and the login flow sat waiting out a
    // whole minute for a window that was on the screen in front of it. The
    // size and visibility tests above are what actually keep the junk out.
    InputManager::WindowInfo info;
    info.hwnd         = hwnd;
    info.title        = text_of(hwnd);
    info.window_class = class_of(hwnd);
    info.process      = proc;
    info.path         = path;
    list->push_back(std::move(info));
    return TRUE;
}

/// Extended-key VKs need KEYEVENTF_EXTENDEDKEY or the game sees the numpad
/// twin instead (arrows are the ones that actually bite here).
bool is_extended(std::uint32_t vk) {
    switch (vk) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_INSERT: case VK_DELETE:
        case VK_RCONTROL: case VK_RMENU:
        case VK_NUMLOCK: case VK_DIVIDE:
            return true;
        default:
            return false;
    }
}

/// Every input this class sends goes through here. Whichever way a key, a
/// click or a move got this far, the "no real input" switch is asked once
/// more just before Windows is -- the one place nothing can get round.
UINT inject(INPUT* in, UINT count, const char* what) {
    if (refuse_real_input(what)) return 0;
    return SendInput(count, in, sizeof(INPUT));
}

} // namespace

InputManager::InputManager()
    : rng_(std::random_device{}()) {}

InputManager::~InputManager() {
    release_all();
}

// ------------------------------------------------------------------- window

std::vector<InputManager::WindowInfo> InputManager::enumerate_windows() {
    std::vector<WindowInfo> out;
    EnumWindows(collect_proc, reinterpret_cast<LPARAM>(&out));
    return out;
}

namespace {

struct Hunt {
    std::vector<InputManager::WindowInfo>* out = nullptr;
    std::string want;        ///< lowercase process name
};

BOOL CALLBACK hunt_proc(HWND hwnd, LPARAM param) {
    auto* hunt = reinterpret_cast<Hunt*>(param);
    if (!IsWindowVisible(hwnd)) return TRUE;
    const std::string path = path_of(hwnd);
    if (to_lower(file_name(path)) != hunt->want) return TRUE;

    InputManager::WindowInfo info;
    info.hwnd         = hwnd;
    info.title        = text_of(hwnd);
    info.window_class = class_of(hwnd);
    info.process      = hunt->want;
    info.path         = path;
    hunt->out->push_back(std::move(info));
    return TRUE;
}

BOOL CALLBACK gather_text(HWND child, LPARAM param) {
    auto* out = reinterpret_cast<std::string*>(param);
    const std::string text = text_of(child);
    if (!text.empty()) {
        if (!out->empty()) out->push_back('\n');
        out->append(text);
    }
    return TRUE;
}

} // namespace

std::vector<InputManager::WindowInfo>
InputManager::windows_of(const std::string& process_name) {
    std::vector<WindowInfo> out;
    if (process_name.empty()) return out;
    Hunt hunt{ &out, to_lower(process_name) };
    EnumWindows(hunt_proc, reinterpret_cast<LPARAM>(&hunt));
    return out;
}

std::string InputManager::window_text(void* hwnd) {
    if (hwnd == nullptr) return {};
    HWND h = static_cast<HWND>(hwnd);

    std::string out = text_of(h);

    EnumChildWindows(h, gather_text, reinterpret_cast<LPARAM>(&out));
    return out;
}

bool InputManager::window_hung(void* hwnd) {
    return hwnd != nullptr && IsHungAppWindow(static_cast<HWND>(hwnd)) != FALSE;
}

InputManager::WindowSpec InputManager::parse_spec(const std::string& text) {
    WindowSpec spec;

    if (text.find("class=") == std::string::npos &&
        text.find("proc=")  == std::string::npos &&
        text.find("title=") == std::string::npos) {
        spec.title = text;
        return spec;
    }

    std::size_t at = 0;
    for (;;) {
        const std::size_t comma = text.find(',', at);
        std::string term = text.substr(
            at, comma == std::string::npos ? std::string::npos : comma - at);

        // A spec is typed by hand often enough to be worth forgiving spaces.
        while (!term.empty() && term.front() == ' ') term.erase(term.begin());
        while (!term.empty() && term.back() == ' ')  term.pop_back();

        if (term.rfind("class=", 0) == 0)      spec.window_class = term.substr(6);
        else if (term.rfind("proc=", 0) == 0)  spec.process      = term.substr(5);
        else if (term.rfind("title=", 0) == 0) spec.title        = term.substr(6);
        else if (!term.empty())                spec.title        = term;

        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return spec;
}

bool InputManager::find_game_window(const std::string& title,
                                    const std::string& window_class,
                                    const std::string& process_name) {
    const std::string want_title = to_lower(title);
    const std::string want_class = to_lower(window_class);
    const std::string want_proc  = to_lower(process_name);

    const WindowInfo* best = nullptr;
    int best_rank = -1;

    // Scored, not filtered.
    //
    // This used to require every identifier given to match, and skip the
    // window otherwise. That is right when all three are known to be correct
    // and wrong the moment one of them is not: another build of the client
    // with a different window class, or a process whose name cannot be read,
    // made the one window being looked for the one window discarded -- and the
    // bot then reported that the game was not running while it was on screen.
    //
    // So each identifier contributes evidence instead. A process name and a
    // window class are strong, because nothing else on the machine shares
    // them; a title is weak, because a browser tab named after the game
    // matches it just as well. Accepting needs at least one strong signal, or
    // a title that matches exactly -- which is what keeps combat keys out of
    // somebody's editor.
    static constexpr int kStrong = 3;

    // A program asked for by the game's name is taken only when it is the
    // game, by where it runs from (GameInstall). Game.exe is what plenty of
    // other programs are called too, and the one chosen here is the one the
    // macro types into. One whose path cannot be read is not the game either:
    // GoBot runs as administrator, which reads the game's.
    const GameInstall install = want_proc.empty() ? GameInstall{} : GameInstall::here();
    const bool the_games_name = install.names_the_game(want_proc);
    const GameInstall::Drives drives = the_games_name ? GameInstall::Drives::here()
                                                      : GameInstall::Drives{};

    const auto windows = enumerate_windows();
    for (const auto& w : windows) {
        int score = 0;

        if (!want_proc.empty()) {
            if (the_games_name) {
                if (!install.is_game(w.path, drives)) continue;   // another program of its name
                score += 4;
            }
            // Any other program: a name that could not be read is unknown,
            // not wrong. It comes from OpenProcess, which fails for a program
            // running at a higher integrity level or one an anticheat has put
            // a handle policy on.
            else if (w.process.empty())                score += 0;
            else if (to_lower(w.process) == want_proc) score += 4;
            else                                       continue;  // positively not it
        }

        if (!want_class.empty()) {
            if (to_lower(w.window_class) == want_class) score += kStrong;
            // A class that differs is not disqualifying on its own: it is the
            // identifier most likely to change between builds of the client.
        }

        if (!want_title.empty()) {
            // Only the whole title is strong. Its start or a piece of it
            // stays under kStrong on its own, and only ranks the windows a
            // program or a class has already let in.
            const std::string t = to_lower(w.title);
            if (t == want_title)                              score += kStrong;
            else if (t.rfind(want_title, 0) == 0)             score += 2;
            else if (t.find(want_title) != std::string::npos) score += 1;
        }

        // Nothing asked for at all finds nothing: no window gets a score.
        if (score < kStrong) continue;

        if (score > best_rank) {
            best_rank = score;
            best = &w;
        }
    }

    if (!best) {
        GB_WARN("Game window not found (title='%s' class='%s' process='%s')",
                title.c_str(), window_class.c_str(), process_name.c_str());
        return false;
    }

    hwnd_.store(best->hwnd);
    GB_INFO("Game window: '%s' [%s] (hwnd %p)",
            best->title.c_str(), best->process.c_str(), best->hwnd);
    return true;
}

void InputManager::set_target_window(void* hwnd) { hwnd_.store(hwnd); }

bool InputManager::window_valid() const {
    void* h = hwnd_.load();
    return h != nullptr && IsWindow(static_cast<HWND>(h));
}

bool InputManager::window_focused() const {
    void* h = hwnd_.load();
    if (!h) return false;
    return GetForegroundWindow() == static_cast<HWND>(h);
}

bool InputManager::focus_window(long long wait_ms, bool* accepted) {
    if (accepted) *accepted = false;
    void* h = hwnd_.load();
    if (!h) return false;       // no window to bring: nothing to refuse either
    // Asked before the window is looked at, so that a test aiming at a window
    // that has gone sees the refusal rather than the missing window.
    if (refuse_real_input("bring a window to the front")) return false;
    if (!IsWindow(static_cast<HWND>(h))) return false;

    HWND target = static_cast<HWND>(h);
    const HWND fg = GetForegroundWindow();
    if (fg == target) {
        if (accepted) *accepted = true;
        return true;
    }

    // Windows refuses SetForegroundWindow from a process that doesn't own the
    // foreground. Attaching to the foreground thread's input queue is the
    // documented way around it, for another program's window. When the window
    // in front is GoBot's own, this process owns the foreground and may hand
    // it on as it is, and attaching would do harm: it ties a worker's
    // SetForegroundWindow to GoBot's UI thread taking the activation messages,
    // and that thread may be waiting in join() for this very worker -- which
    // is what happens when DỪNG is pressed while the party, trash, revive or
    // login worker brings the game forward. Nor to a program that has stopped
    // answering (answers): Windows then likely refuses, and that is a refusal.
    DWORD fg_process = 0;
    const DWORD fg_thread  = fg != nullptr ? GetWindowThreadProcessId(fg, &fg_process) : 0;
    const DWORD our_thread = GetCurrentThreadId();

    bool attached = false;
    if (may_attach_to_foreground(static_cast<std::uint32_t>(fg_thread),
                                 static_cast<std::uint32_t>(fg_process),
                                 static_cast<std::uint32_t>(our_thread),
                                 static_cast<std::uint32_t>(GetCurrentProcessId())) &&
        answers(fg))
        attached = AttachThreadInput(our_thread, fg_thread, TRUE) != 0;

    // Neither of these waits for the game's thread. ShowWindow and
    // BringWindowToTop do, and a game that has stopped answering -- hung, or
    // crashed with Windows' report box up -- held the caller until it
    // answered: GoBot's UI thread, which brings the game back during a run.
    if (IsIconic(target)) ShowWindowAsync(target, SW_RESTORE);
    const bool asked = SetForegroundWindow(target) != FALSE;
    SetWindowPos(target, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_ASYNCWINDOWPOS);

    if (attached) AttachThreadInput(our_thread, fg_thread, FALSE);

    // So the game's own thread does all of it, at its next look at its
    // queue: a game minimised, or busy for a moment, comes forward later than
    // one fixed wait. Looked at every 10 ms -- never waited on -- for
    // wait_ms at most, and kFocusMinMs at least, as ever, before the keys
    // come. A request Windows refused outright is not waited out.
    const auto start = std::chrono::steady_clock::now();
    const auto waited = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();
    };
    const long long most = asked ? std::max(wait_ms, kFocusMinMs) : kFocusMinMs;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const long long ms = waited();
        const bool in_front = GetForegroundWindow() == target;
        if (ms >= kFocusMinMs && in_front) {
            if (accepted) *accepted = true;
            return true;
        }
        if (ms >= most) {
            if (accepted) *accepted = in_front || asked;
            return in_front;
        }
    }
}

bool InputManager::may_attach_to_foreground(std::uint32_t fg_thread, std::uint32_t fg_process,
                                            std::uint32_t our_thread, std::uint32_t our_process) {
    return fg_thread != 0 && fg_thread != our_thread && fg_process != our_process;
}

bool InputManager::window_size(int& width, int& height) const {
    void* h = hwnd_.load();
    if (!h) return false;
    RECT r{};
    if (!GetClientRect(static_cast<HWND>(h), &r)) return false;
    width  = r.right - r.left;
    height = r.bottom - r.top;
    return true;
}

// -------------------------------------------------------------- key sending

std::uint16_t InputManager::scan_code(std::uint32_t vk) {
    return static_cast<std::uint16_t>(MapVirtualKeyA(vk, MAPVK_VK_TO_VSC));
}

bool InputManager::send_key(std::uint32_t vk, bool down, bool owed) {
    // No key, or no window to send it to: nothing to do, and nothing to refuse.
    if (vk == 0 || hwnd_.load() == nullptr) return false;

    // ↓ on a gate changes the map. Never down, whichever setting asked for it
    // -- a key box set to it before it refused it, the packet bot's drop --
    // and said once. Its key-up is let through: it lets go of nothing GoBot
    // holds.
    if (down && never_pressed(vk)) {
        if (!said_never_.exchange(true))
            GB_WARN("Not pressing Down (vk 0x28): standing on a gate it takes the character to another map "
                    "-- a key set to it presses nothing");
        return false;
    }

    // Before the focus gate: refused for being a key at all, not for the game
    // happening to be behind something.
    if (refuse_real_input(down ? "press a key" : "let go of a key")) return false;

    // A key goes down only into the game. Input goes to the focused window,
    // whatever it is, and a key sent while the game is not foreground types
    // into the user's other applications, so that is a hard gate rather than
    // a warning. Key-ups were refused the same way, which left a key down in
    // Windows whenever focus moved in the middle of a press -- the player
    // clicking another window during a potion's 40 ms -- and a key down in
    // Windows is down for whatever is in front: the default jump key, Ctrl,
    // turned the player's typing into shortcuts. So a key-up that is owed goes
    // out whatever is in front, and one that is not still goes only to the
    // game.
    const bool focused = window_focused();
    if (!key_event_allowed(down, focused, owed || (!down && !focused && held(vk)))) return false;

    INPUT in{};
    in.type       = INPUT_KEYBOARD;
    in.ki.wVk     = static_cast<WORD>(vk);
    in.ki.wScan   = scan_code(vk);
    in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP);
    if (is_extended(vk)) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;

    const UINT sent = inject(&in, 1, down ? "press a key" : "let go of a key");
    if (sent != 1) {
        GB_WARN("SendInput failed for vk=0x%02X (err %lu)", vk, GetLastError());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(held_mutex_);
        if (down) held_keys_[vk] = true;
        else      held_keys_.erase(vk);
    }
    return true;
}

bool InputManager::key_down(std::uint32_t vk) { return send_key(vk, true); }
bool InputManager::key_up(std::uint32_t vk)   { return send_key(vk, false); }

// Down only into the game in front; up there too, or wherever focus is when owed.
bool InputManager::key_event_allowed(bool down, bool focused, bool owed) {
    return focused || (!down && owed);
}

bool InputManager::injection_works() {
    // F7 for the probe: unbound in this game, harmless elsewhere, and not a
    // key anyone is likely to be holding down while this runs.
    constexpr int kProbe = VK_F7;

    // The probe is a real key press, into whatever window is in front.
    if (refuse_real_input("press F7 to see whether keys get through")) return false;

    GetAsyncKeyState(kProbe);        // clear the "pressed since last call" bit

    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = static_cast<WORD>(kProbe);
    in.ki.wScan = scan_code(kProbe);
    if (inject(&in, 1, "press F7 to see whether keys get through") != 1) return false;

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const bool seen = (GetAsyncKeyState(kProbe) & 0x8000) != 0;

    in.ki.dwFlags = KEYEVENTF_KEYUP;
    inject(&in, 1, "let go of F7");

    return seen;
}

int InputManager::jitter(int min_ms, int max_ms) const {
    std::lock_guard<std::mutex> lock(rng_mutex_);
    if (max_ms <= min_ms) return min_ms;
    return min_ms + static_cast<int>(rng_() % static_cast<unsigned>(max_ms - min_ms + 1));
}

bool InputManager::key_press(std::uint32_t vk, int hold_ms) {
    if (!send_key(vk, true)) return false;

    const int hold = hold_ms >= 0 ? hold_ms : jitter(25, 60);
    std::this_thread::sleep_for(std::chrono::milliseconds(hold));

    // Owed: its down went out above, so it comes up wherever focus has gone.
    return send_key(vk, false, true);
}

bool InputManager::held(std::uint32_t vk) const {
    std::lock_guard<std::mutex> lock(held_mutex_);
    auto it = held_keys_.find(vk);
    return it != held_keys_.end() && it->second;
}

bool InputManager::is_held(std::uint32_t vk) const { return held(vk); }

void InputManager::release_all() {
    std::vector<std::uint32_t> keys;
    {
        std::lock_guard<std::mutex> lock(held_mutex_);
        keys.reserve(held_keys_.size());
        for (const auto& [vk, down] : held_keys_) {
            if (down) keys.push_back(vk);
        }
        held_keys_.clear();
    }

    // Release unconditionally, ignoring the focus gate: if focus was lost
    // while a direction was held, the game still has the key down and the
    // character keeps running. Getting the key up matters more than the gate.
    for (std::uint32_t vk : keys) {
        INPUT in{};
        in.type       = INPUT_KEYBOARD;
        in.ki.wVk     = static_cast<WORD>(vk);
        in.ki.wScan   = scan_code(vk);
        in.ki.dwFlags = KEYEVENTF_KEYUP;
        if (is_extended(vk)) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        inject(&in, 1, "let go of a key");
    }

    if (!keys.empty()) GB_DEBUG("Released %zu held key(s)", keys.size());
}

// ----------------------------------------------------------------- movement

bool InputManager::move_left() {
    if (block_movement_.load()) return false;
    if (held(key_right_)) key_up(key_right_);
    if (held(key_left_)) return true;        // already moving
    return key_down(key_left_);
}

bool InputManager::move_right() {
    if (block_movement_.load()) return false;
    if (held(key_left_)) key_up(key_left_);
    if (held(key_right_)) return true;
    return key_down(key_right_);
}

bool InputManager::stop_move() {
    bool any = false;
    if (held(key_left_))  any |= key_up(key_left_);
    if (held(key_right_)) any |= key_up(key_right_);
    return any;
}

bool InputManager::jump() {
    if (block_movement_.load()) return false;
    return key_press(key_jump_);
}

bool InputManager::double_jump() {
    if (block_movement_.load()) return false;
    if (!key_press(key_jump_)) return false;
    // The second press must land while airborne but after the engine has
    // registered the first; too fast and it is swallowed.
    std::this_thread::sleep_for(std::chrono::milliseconds(jitter(180, 240)));
    return key_press(key_jump_);
}

bool InputManager::jump_down() {
    if (block_movement_.load()) return false;
    if (!key_down(key_down_)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const bool ok = key_press(key_jump_);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    send_key(key_down_, false, true);
    return ok;
}

bool InputManager::face_left() {
    if (block_movement_.load()) return false;
    // A tap short enough to turn the character without displacing it.
    if (!key_down(key_left_)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(18));
    return send_key(key_left_, false, true);
}

bool InputManager::face_right() {
    if (block_movement_.load()) return false;
    if (!key_down(key_right_)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(18));
    return send_key(key_right_, false, true);
}

// ------------------------------------------------------------------ actions

bool InputManager::use_skill(std::uint32_t vk) {
    if (block_attack_.load()) return false;
    return key_press(vk);
}

bool InputManager::normal_attack() {
    if (block_attack_.load()) return false;
    return key_press(key_attack_);
}

bool InputManager::loot() { return key_press(key_loot_); }

bool InputManager::send_chat(const std::string& text, std::uint32_t chat_key) {
    // It pressed Enter and typed whatever it was given, one byte at a time as
    // Unicode, without looking: a Vietnamese letter came out as two wrong
    // characters, the game ignores Unicode input in at least one of its boxes,
    // and letters typed when the line had not opened were skill keys. A line
    // needs a look at the screen before each key, which ChatSender does.
    (void)chat_key;
    GB_WARN("Chat: send_chat typed nothing (%zu bytes asked for): a line is only typed once "
            "the screen shows the chat line open, which ChatSender does", text.size());
    return false;
}

// ------------------------------------------------------------------- typing

InputManager::KeyEvent InputManager::typed_key_event(std::uint32_t vk, bool down, KeyInject how) {
    KeyEvent e;
    e.scan  = scan_code(vk);
    e.flags = down ? 0 : KEYEVENTF_KEYUP;
    if (is_extended(vk)) e.flags |= KEYEVENTF_EXTENDEDKEY;
    if (how == KeyInject::Scancode) {
        // As GoC sends keys: the scan code alone, which the game reads the
        // way a keyboard's own key is read.
        e.flags |= KEYEVENTF_SCANCODE;
    } else {
        e.vk = static_cast<std::uint16_t>(vk);
    }
    return e;
}

namespace {

INPUT key_event(std::uint32_t vk, bool down, InputManager::KeyInject how) {
    const InputManager::KeyEvent e = InputManager::typed_key_event(vk, down, how);
    INPUT in{};
    in.type       = INPUT_KEYBOARD;
    in.ki.wVk     = e.vk;
    in.ki.wScan   = e.scan;
    in.ki.dwFlags = e.flags;
    return in;
}

} // namespace

bool InputManager::type_key(std::uint32_t vk, bool shift, int hold_ms, KeyInject how) {
    if (vk == 0 || hwnd_.load() == nullptr) return false;
    // As send_key: never ↓.
    if (never_pressed(vk)) {
        if (!said_never_.exchange(true))
            GB_WARN("Not pressing Down (vk 0x28): standing on a gate it takes the character to another map "
                    "-- a key set to it presses nothing");
        return false;
    }
    if (refuse_real_input("type a key")) return false;
    if (!window_focused()) return false;

    INPUT down[2]{};
    UINT n = 0;
    if (shift) down[n++] = key_event(VK_SHIFT, true, how);
    down[n++] = key_event(vk, true, how);
    const UINT went = inject(down, n, "type a key");
    if (went == n) std::this_thread::sleep_for(std::chrono::milliseconds(std::max(0, hold_ms)));

    // Up whatever happened, and not gated on focus: a key that went down and
    // never came up is held in the game from then on.
    INPUT up[2]{};
    UINT m = 0;
    up[m++] = key_event(vk, false, how);
    if (shift) up[m++] = key_event(VK_SHIFT, false, how);
    inject(up, m, "let go of a typed key");

    if (went != n) {
        GB_WARN("SendInput failed for vk=0x%02X (err %lu)", vk, GetLastError());
        return false;
    }
    return true;
}

bool InputManager::type_unit(wchar_t unit) {
    if (hwnd_.load() == nullptr) return false;
    if (refuse_real_input("type a character")) return false;
    if (!window_focused()) return false;
    INPUT in[2]{};
    in[0].type       = INPUT_KEYBOARD;
    in[0].ki.wScan   = static_cast<WORD>(unit);
    in[0].ki.dwFlags = KEYEVENTF_UNICODE;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    return inject(in, 2, "type a character") == 2;
}

bool InputManager::post_char(wchar_t unit, bool ime) {
    if (hwnd_.load() == nullptr) return false;
    // A message into another program's window: the one OS call here that is
    // not SendInput, so it asks the switch itself.
    if (refuse_real_input("post a character to another program's window")) return false;
    if (!window_focused()) return false;
    HWND h = static_cast<HWND>(hwnd_.load());
    return PostMessageW(h, ime ? WM_IME_CHAR : WM_CHAR, static_cast<WPARAM>(unit), 1) != FALSE;
}

bool InputManager::caps_lock_on() {
    return (GetKeyState(VK_CAPITAL) & 1) != 0;
}

// ------------------------------------------------------------------ pointer

namespace {

/// The pace of this thread's clicks and drags on the game: see Pace. 0 is as
/// ever.
thread_local int t_press_ms = 0;

/// `ms`, the pause as it is at 80, at this thread's pace -- never under
/// `floor_ms`, under which the game is known to miss a press.
int paced(int ms, int floor_ms) {
    return t_press_ms > 0 ? std::max(floor_ms, ms * t_press_ms / 80) : ms;
}

// SendInput works in a 0..65535 grid across the whole virtual desktop, not
// in pixels, so the conversion has to account for monitors left of or above
// the primary one -- where the origin is negative and a naive scaling puts
// every click on the wrong screen.
void to_absolute(POINT at, LONG& dx, LONG& dy) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    dx = static_cast<LONG>((at.x - vx) * 65535LL / vw);
    dy = static_cast<LONG>((at.y - vy) * 65535LL / vh);
}

} // namespace

InputManager::Pace::Pace(int press_ms) : was_(t_press_ms) { t_press_ms = press_ms; }
InputManager::Pace::~Pace() { t_press_ms = was_; }

bool InputManager::move_pointer(int x, int y) {
    HWND hwnd = static_cast<HWND>(hwnd_.load());
    if (!hwnd) return false;    // no window to aim at: nothing to refuse either
    // Before the window is looked at: a test aims this at a window that has
    // gone, so that nothing could move were the switch ever broken.
    if (refuse_real_input("move the pointer")) return false;
    if (!IsWindow(hwnd)) return false;

    POINT at{ x, y };
    if (!ClientToScreen(hwnd, &at)) return false;

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    to_absolute(at, in.mi.dx, in.mi.dy);
    return inject(&in, 1, "move the pointer") == 1;
}

bool InputManager::click_at(int x, int y, bool right_button) {
    HWND hwnd = static_cast<HWND>(hwnd_.load());
    if (hwnd == nullptr) return false;
    if (refuse_real_input("click")) return false;
    if (!move_pointer(x, y)) return false;
    // A pause between arriving and pressing. Several of these buttons light up
    // on hover and ignore a click that lands in the same instant as the move.
    std::this_thread::sleep_for(std::chrono::milliseconds(jitter(40, 90)));

    // The press and the release carry the point, so a hand on the mouse in
    // that pause does not take them somewhere else.
    POINT at{ x, y };
    if (!ClientToScreen(hwnd, &at)) return false;

    INPUT in[2]{};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK |
                       (right_button ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN);
    to_absolute(at, in[0].mi.dx, in[0].mi.dy);
    in[1] = in[0];
    in[1].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK |
                       (right_button ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);

    if (inject(&in[0], 1, "press a mouse button") != 1) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(jitter(30, 70)));
    return inject(&in[1], 1, "let go of a mouse button") == 1;
}

const char* InputManager::game_click_name(GameClick c) {
    switch (c) {
        case GameClick::Clicked:    return "clicked";
        case GameClick::NoWindow:   return "no game window";
        case GameClick::Refused:    return "no real input in this process";
        case GameClick::OffClient:  return "the point is outside the game's window";
        case GameClick::OffScreen:  return "the point is on no screen";
        case GameClick::NotInFront: return "the game is not in front";
        case GameClick::Covered:    return "another window is over the game there";
        case GameClick::Moved:      return "the game's window moved";
        case GameClick::Stopped:    return "stopped";
        case GameClick::Failed:     return "Windows did not take the click";
    }
    return "?";
}

bool InputManager::would_reach_game(void* game, int x, int y, GameClick* why, bool need_front) {
    const auto no = [why](GameClick c) {
        if (why) *why = c;
        return false;
    };
    const HWND g = static_cast<HWND>(game);
    if (g == nullptr || !IsWindow(g)) return no(GameClick::NoWindow);
    if (IsIconic(g)) return no(GameClick::NotInFront);
    RECT client{};
    if (!GetClientRect(g, &client)) return no(GameClick::NoWindow);
    if (x < 0 || y < 0 || x >= client.right || y >= client.bottom) return no(GameClick::OffClient);
    POINT at{ x, y };
    if (!ClientToScreen(g, &at)) return no(GameClick::NoWindow);
    if (MonitorFromPoint(at, MONITOR_DEFAULTTONULL) == nullptr) return no(GameClick::OffScreen);
    if (need_front && GetForegroundWindow() != g) return no(GameClick::NotInFront);
    // The game under the point, by its top-level window: a child of the game
    // is the game, and anything else there is what the click would land on.
    const HWND there = WindowFromPoint(at);
    if (there == nullptr || GetAncestor(there, GA_ROOT) != GetAncestor(g, GA_ROOT))
        return no(GameClick::Covered);
    return true;
}

// A click on the game, only where it lands on the game.
//
// The frames come from PrintWindow, which shows the game even when it is
// covered, so a point worked out from one says nothing about what is on top
// of it now. Everything is asked again after the pause on arrival, just
// before the button goes down. The press and the release carry the point, so
// a hand on the mouse cannot move them. And the release always goes out once
// the press has, because a button left down is a drag.
bool InputManager::click_on_game(int x, int y, bool right_button,
                                 const std::function<bool()>& stop, GameClick* why,
                                 bool need_front, int presses) {
    const auto came_to = [why](GameClick c) {
        if (why) *why = c;
        return c == GameClick::Clicked;
    };
    const HWND game = static_cast<HWND>(hwnd_.load());
    // No game aimed at: nothing to do, and nothing to refuse.
    if (game == nullptr) return came_to(GameClick::NoWindow);
    if (refuse_real_input("click on the game")) return came_to(GameClick::Refused);
    if (stop && stop()) return came_to(GameClick::Stopped);

    GameClick not_there = GameClick::Failed;
    if (!would_reach_game(game, x, y, &not_there, need_front)) return came_to(not_there);
    POINT at{ x, y };
    if (!ClientToScreen(game, &at)) return came_to(GameClick::NoWindow);

    INPUT move{};
    move.type = INPUT_MOUSE;
    move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    to_absolute(at, move.mi.dx, move.mi.dy);
    if (inject(&move, 1, "move the pointer") != 1) return came_to(GameClick::Failed);
    // The pause click_at has too: buttons that light up on hover ignore a
    // press in the same instant as the move.
    std::this_thread::sleep_for(std::chrono::milliseconds(jitter(paced(40, 35), paced(90, 45))));

    if (stop && stop()) return came_to(GameClick::Stopped);
    if (!would_reach_game(game, x, y, &not_there, need_front)) return came_to(not_there);
    POINT now{ x, y };
    if (!ClientToScreen(game, &now)) return came_to(GameClick::NoWindow);
    if (now.x != at.x || now.y != at.y) return came_to(GameClick::Moved);

    INPUT down = move;
    down.mi.dwFlags |= right_button ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    INPUT up = move;
    up.mi.dwFlags |= right_button ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    for (int press = 0; press < std::max(1, presses); ++press) {
        if (press > 0) {
            // The next press of a double-click: soon after the release, well
            // inside the double-click time, and only where it still lands on
            // the game.
            std::this_thread::sleep_for(std::chrono::milliseconds(jitter(paced(50, 40), paced(90, 60))));
            GameClick cut = GameClick::Clicked;
            POINT again{ x, y };
            if (stop && stop()) cut = GameClick::Stopped;
            else if (!would_reach_game(game, x, y, &not_there, need_front)) cut = not_there;
            else if (!ClientToScreen(game, &again)) cut = GameClick::NoWindow;
            else if (again.x != at.x || again.y != at.y) cut = GameClick::Moved;
            if (cut != GameClick::Clicked) {
                // The first click went out: said, as it is not "no click".
                GB_INFO("Input: a double-click at %d,%d cut after %d of %d clicks -- %s", x, y, press, presses,
                        game_click_name(cut));
                return came_to(cut);
            }
        }
        if (inject(&down, 1, "press a mouse button") != 1) return came_to(GameClick::Failed);
        std::this_thread::sleep_for(std::chrono::milliseconds(jitter(paced(30, 25), paced(70, 40))));
        if (inject(&up, 1, "let go of a mouse button") != 1) {
            GB_WARN("SendInput failed to let go of the mouse button at %d,%d (err %lu)", x, y,
                    GetLastError());
            return came_to(GameClick::Failed);
        }
    }
    return came_to(GameClick::Clicked);
}

bool InputManager::drag(int from_x, int from_y, int to_x, int to_y) {
    if (hwnd_.load() == nullptr) return false;
    if (refuse_real_input("drag")) return false;
    if (!move_pointer(from_x, from_y)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(jitter(60, 110)));

    INPUT down{};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    if (inject(&down, 1, "press a mouse button") != 1) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(jitter(90, 140)));

    // Always let go, whatever happens on the way: a button left held down is
    // an item stuck to the pointer, and the next click drops it somewhere.
    INPUT up = down;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;

    constexpr int kSteps = 10;
    bool moved = true;
    for (int i = 1; i <= kSteps && moved; ++i) {
        moved = move_pointer(from_x + (to_x - from_x) * i / kSteps,
                             from_y + (to_y - from_y) * i / kSteps);
        std::this_thread::sleep_for(std::chrono::milliseconds(jitter(18, 30)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(jitter(80, 130)));
    const bool released = inject(&up, 1, "let go of a mouse button") == 1;
    return moved && released;
}

// A drag on the game, only where it lands on the game -- for the reasons
// click_on_game gives. The press is what picks an item up, so it is the one
// asked about again just before it goes down; and it carries its point, so a
// hand on the mouse in the pause cannot pick up the item next to it.
bool InputManager::drag_on_game(int from_x, int from_y, int to_x, int to_y,
                                const std::function<bool()>& stop, GameClick* why) {
    const auto came_to = [why](GameClick c) {
        if (why) *why = c;
        return c == GameClick::Clicked;
    };
    const HWND game = static_cast<HWND>(hwnd_.load());
    // No game aimed at: nothing to do, and nothing to refuse.
    if (game == nullptr) return came_to(GameClick::NoWindow);
    if (refuse_real_input("drag on the game")) return came_to(GameClick::Refused);
    if (stop && stop()) return came_to(GameClick::Stopped);

    GameClick not_there = GameClick::Failed;
    if (!would_reach_game(game, from_x, from_y, &not_there) ||
        !would_reach_game(game, to_x, to_y, &not_there))
        return came_to(not_there);
    POINT from{ from_x, from_y };
    if (!ClientToScreen(game, &from)) return came_to(GameClick::NoWindow);

    INPUT move{};
    move.type = INPUT_MOUSE;
    move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    to_absolute(from, move.mi.dx, move.mi.dy);
    if (inject(&move, 1, "move the pointer") != 1) return came_to(GameClick::Failed);
    std::this_thread::sleep_for(std::chrono::milliseconds(jitter(paced(60, 45), paced(110, 80))));

    if (stop && stop()) return came_to(GameClick::Stopped);
    if (!would_reach_game(game, from_x, from_y, &not_there)) return came_to(not_there);
    POINT now{ from_x, from_y };
    if (!ClientToScreen(game, &now)) return came_to(GameClick::NoWindow);
    if (now.x != from.x || now.y != from.y) return came_to(GameClick::Moved);

    INPUT down = move;
    down.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
    if (inject(&down, 1, "press a mouse button") != 1) return came_to(GameClick::Failed);
    std::this_thread::sleep_for(std::chrono::milliseconds(jitter(paced(90, 70), paced(140, 100))));

    // From here it is finished whatever happens: a button left held down is
    // an item stuck to the pointer, and the next click drops it somewhere.
    constexpr int kSteps = 10;
    bool moved = true;
    for (int i = 1; i <= kSteps && moved; ++i) {
        moved = move_pointer(from_x + (to_x - from_x) * i / kSteps,
                             from_y + (to_y - from_y) * i / kSteps);
        std::this_thread::sleep_for(std::chrono::milliseconds(jitter(18, 30)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(jitter(paced(80, 60), paced(130, 95))));
    // Let go at the end point; where the pointer is, should the window have
    // gone meanwhile.
    INPUT up{};
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    POINT to{ to_x, to_y };
    if (ClientToScreen(game, &to)) {
        up.mi.dwFlags |= MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        to_absolute(to, up.mi.dx, up.mi.dy);
    }
    if (inject(&up, 1, "let go of a mouse button") != 1) {
        GB_WARN("SendInput failed to let go of the mouse button at %d,%d (err %lu)", to_x, to_y,
                GetLastError());
        return came_to(GameClick::Failed);
    }
    return came_to(moved ? GameClick::Clicked : GameClick::Failed);
}

bool InputManager::type_text(const std::string& utf8) {
    if (utf8.empty()) return false;
    // Not gated on focus at all, so this is the one thing standing between a
    // test and whatever window is in front.
    if (refuse_real_input("type text")) return false;

    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    if (wide_len <= 0) return false;
    std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        wide.data(), wide_len);

    for (const wchar_t ch : wide) {
        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wScan   = static_cast<WORD>(ch);
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        if (inject(in, 2, "type text") != 2) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(jitter(25, 55)));
    }
    return true;
}

// ------------------------------------------------------------------- timing

void InputManager::set_delay_range(int min_ms, int max_ms) {
    std::lock_guard<std::mutex> lock(rng_mutex_);
    if (max_ms < min_ms) std::swap(min_ms, max_ms);
    delay_min_ = std::max(0, min_ms);
    delay_max_ = std::max(delay_min_, max_ms);
}

int InputManager::random_delay() const {
    std::lock_guard<std::mutex> lock(rng_mutex_);
    if (delay_max_ <= delay_min_) return delay_min_;
    return delay_min_ + static_cast<int>(rng_() % static_cast<unsigned>(delay_max_ - delay_min_ + 1));
}

void InputManager::sleep_jitter(int base_ms) const {
    std::this_thread::sleep_for(std::chrono::milliseconds(base_ms + random_delay()));
}

// ---------------------------------------------------------------- key names

std::uint32_t InputManager::parse_key(const std::string& text) {
    std::string s = to_lower(text);
    const std::size_t first = s.find_first_not_of(" \t");
    if (first == std::string::npos) return 0;
    s = s.substr(first, s.find_last_not_of(" \t") - first + 1);

    // One character is that key, before anything is read as a key code. Read
    // as codes, "1" to "9" were the mouse buttons, Backspace and Tab, and "0"
    // was no key at all, so a potion typed as 1 was never drunk.
    if (s.size() == 1) {
        const char c = s[0];
        if (c >= 'a' && c <= 'z') return static_cast<std::uint32_t>('A' + (c - 'a'));
        if (c >= '0' && c <= '9') return static_cast<std::uint32_t>(c);
        return 0;
    }

    // A key code: 0x and hex digits, or two or more decimal digits. Decimal is
    // read as written, never as octal: "010" is ten.
    const auto only = [&s](std::size_t from, bool hex) {
        if (from >= s.size()) return false;
        for (std::size_t i = from; i < s.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (hex ? !std::isxdigit(c) : !std::isdigit(c)) return false;
        }
        return true;
    };
    if (s.size() > 2 && s[0] == '0' && s[1] == 'x' && only(2, true)) {
        const unsigned long v = std::strtoul(s.c_str() + 2, nullptr, 16);
        return v <= 0xFF ? static_cast<std::uint32_t>(v) : 0;
    }
    if (only(0, false)) {
        const unsigned long v = std::strtoul(s.c_str(), nullptr, 10);
        return v <= 0xFF ? static_cast<std::uint32_t>(v) : 0;
    }

    if (s.size() >= 2 && s[0] == 'f' && std::isdigit(static_cast<unsigned char>(s[1]))) {
        const int n = std::atoi(s.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + (n - 1);
    }

    static const struct { const char* n; std::uint32_t vk; } kNamed[] = {
        { "space", VK_SPACE }, { "enter", VK_RETURN }, { "return", VK_RETURN },
        { "tab", VK_TAB }, { "esc", VK_ESCAPE }, { "escape", VK_ESCAPE },
        { "ctrl", VK_CONTROL }, { "control", VK_CONTROL },
        { "shift", VK_SHIFT }, { "alt", VK_MENU },
        { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT },
        { "home", VK_HOME }, { "end", VK_END },
        { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT },
        { "insert", VK_INSERT }, { "delete", VK_DELETE },
        { "backspace", VK_BACK },
        { "numpad0", VK_NUMPAD0 }, { "numpad1", VK_NUMPAD1 }, { "numpad2", VK_NUMPAD2 },
        { "numpad3", VK_NUMPAD3 }, { "numpad4", VK_NUMPAD4 }, { "numpad5", VK_NUMPAD5 },
        { "numpad6", VK_NUMPAD6 }, { "numpad7", VK_NUMPAD7 }, { "numpad8", VK_NUMPAD8 },
        { "numpad9", VK_NUMPAD9 },
    };
    for (const auto& e : kNamed) {
        if (s == e.n) return e.vk;
    }
    return 0;
}

std::string InputManager::key_name(std::uint32_t vk) {
    if (vk == 0) return "";
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Numpad" + std::to_string(vk - VK_NUMPAD0);

    switch (vk) {
        case VK_SPACE:   return "Space";
        case VK_RETURN:  return "Enter";
        case VK_TAB:     return "Tab";
        case VK_ESCAPE:  return "Esc";
        case VK_CONTROL: return "Ctrl";
        case VK_SHIFT:   return "Shift";
        case VK_MENU:    return "Alt";
        case VK_UP:      return "Up";
        case VK_DOWN:    return "Down";
        case VK_LEFT:    return "Left";
        case VK_RIGHT:   return "Right";
        default: break;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

/// Bridge for config.cpp, which needs key labels without including this header.
std::string InputManagerKeyName(std::uint32_t vk) { return InputManager::key_name(vk); }

} // namespace gobot
