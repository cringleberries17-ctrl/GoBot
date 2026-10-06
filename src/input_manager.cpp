#include "gobot/input_manager.hpp"
#include "gobot/common.hpp"
#include "gobot/logger.hpp"

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

/// Executable file name owning `hwnd`, or "" if it can't be determined.
std::string process_of(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return {};

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};

    char path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    std::string out;
    if (QueryFullProcessImageNameA(proc, 0, path, &size)) {
        out = path;
        const auto slash = out.find_last_of("\\/");
        if (slash != std::string::npos) out = out.substr(slash + 1);
    }
    CloseHandle(proc);
    return out;
}

BOOL CALLBACK collect_proc(HWND hwnd, LPARAM param) {
    auto* list = reinterpret_cast<std::vector<InputManager::WindowInfo>*>(param);

    if (!IsWindowVisible(hwnd)) return TRUE;

    // An untitled window is still a window.
    //
    // This used to drop them, which cost a day: the game's launcher -- the one
    // with the purple start button on it -- has no title at all. It is a bare
    // #32770 dialog put up by GhostOnlineVTC.exe, and because it was never in
    // this list, nothing could find it, and the login flow sat waiting out a
    // whole minute for a window that was on the screen in front of it. The
    // size and visibility tests below are what actually keep the junk out.
    char title[512]{};
    GetWindowTextA(hwnd, title, sizeof(title) - 1);

    // Tool windows and tiny shells are never the game.
    RECT r{};
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    if ((r.right - r.left) < 200 || (r.bottom - r.top) < 150) return TRUE;

    // Never offer our own window: sending input to ourselves does nothing
    // useful and makes the list confusing.
    const std::string proc = process_of(hwnd);
    static const std::string self = [] {
        char p[MAX_PATH]{};
        GetModuleFileNameA(nullptr, p, MAX_PATH);
        std::string s = p;
        const auto slash = s.find_last_of("\\/");
        return slash == std::string::npos ? s : s.substr(slash + 1);
    }();
    if (!proc.empty() && _stricmp(proc.c_str(), self.c_str()) == 0) return TRUE;

    char cls[256]{};
    GetClassNameA(hwnd, cls, sizeof(cls) - 1);

    InputManager::WindowInfo info;
    info.hwnd         = hwnd;
    info.title        = title;
    info.window_class = cls;
    info.process      = proc;
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
    if (to_lower(process_of(hwnd)) != hunt->want) return TRUE;

    char title[512]{};
    GetWindowTextA(hwnd, title, sizeof(title) - 1);
    char cls[256]{};
    GetClassNameA(hwnd, cls, sizeof(cls) - 1);

    InputManager::WindowInfo info;
    info.hwnd         = hwnd;
    info.title        = title;
    info.window_class = cls;
    info.process      = hunt->want;
    hunt->out->push_back(std::move(info));
    return TRUE;
}

BOOL CALLBACK gather_text(HWND child, LPARAM param) {
    auto* out = reinterpret_cast<std::string*>(param);
    char text[1024]{};
    if (GetWindowTextA(child, text, sizeof(text) - 1) > 0) {
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

    std::string out;
    char title[1024]{};
    if (GetWindowTextA(h, title, sizeof(title) - 1) > 0) out = title;

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

bool InputManager::find_game_window(const std::string& title_substr,
                                    const std::string& window_class,
                                    const std::string& process_name) {
    const std::string want_title = to_lower(title_substr);
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

    const auto windows = enumerate_windows();
    for (const auto& w : windows) {
        int score = 0;

        if (!want_proc.empty()) {
            // A name that could not be read is unknown, not wrong. It comes
            // from OpenProcess, which fails for a game running at a higher
            // integrity level or one an anticheat has put a handle policy on.
            if (w.process.empty())                    score += 0;
            else if (to_lower(w.process) == want_proc) score += 4;
            else                                       continue;  // positively not it
        }

        if (!want_class.empty()) {
            if (to_lower(w.window_class) == want_class) score += kStrong;
            // A class that differs is not disqualifying on its own: it is the
            // identifier most likely to change between builds of the client.
        }

        if (!want_title.empty()) {
            const std::string t = to_lower(w.title);
            if (t == want_title)                              score += kStrong;
            else if (t.rfind(want_title, 0) == 0)             score += 2;
            else if (t.find(want_title) != std::string::npos) score += 1;
        }

        // Nothing asked for at all: anything visible will do, and the caller
        // is choosing from a list.
        if (want_proc.empty() && want_class.empty() && want_title.empty())
            score = 1;

        if (score < kStrong) continue;

        if (score > best_rank) {
            best_rank = score;
            best = &w;
        }
    }

    if (!best) {
        GB_WARN("Game window not found (title~'%s' class='%s' process='%s')",
                title_substr.c_str(), window_class.c_str(), process_name.c_str());
        return false;
    }

    hwnd_.store(best->hwnd);
    GB_INFO("Game window: '%s' [%s] (hwnd %p)",
            best->title.c_str(), best->process.c_str(), best->hwnd);

    if (best_rank <= 1 && !want_title.empty()) {
        // A bare substring hit is exactly how an editor or browser tab named
        // after the game gets picked. Worth saying out loud.
        GB_WARN("Matched on a loose title substring -- confirm this is the game "
                "and not an editor or browser window with a similar title.");
    }
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

bool InputManager::focus_window() {
    void* h = hwnd_.load();
    if (!h || !IsWindow(static_cast<HWND>(h))) return false;

    HWND target = static_cast<HWND>(h);
    if (GetForegroundWindow() == target) return true;

    // Windows refuses SetForegroundWindow from a process that doesn't own the
    // foreground. Attaching to the foreground thread's input queue is the
    // documented way around it.
    const DWORD fg_thread  = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD our_thread = GetCurrentThreadId();

    bool attached = false;
    if (fg_thread != our_thread) attached = AttachThreadInput(our_thread, fg_thread, TRUE) != 0;

    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    SetForegroundWindow(target);
    BringWindowToTop(target);

    if (attached) AttachThreadInput(our_thread, fg_thread, FALSE);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return GetForegroundWindow() == target;
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

bool InputManager::send_key(std::uint32_t vk, bool down) {
    if (vk == 0) return false;

    // Input goes to the focused window, whatever it is. Sending while the game
    // is not foreground types into the user's other applications, so this is a
    // hard gate rather than a warning.
    if (!window_focused()) return false;

    INPUT in{};
    in.type       = INPUT_KEYBOARD;
    in.ki.wVk     = static_cast<WORD>(vk);
    in.ki.wScan   = scan_code(vk);
    in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP);
    if (is_extended(vk)) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;

    const UINT sent = SendInput(1, &in, sizeof(INPUT));
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

bool InputManager::injection_works() {
    // F7 for the probe: unbound in this game, harmless elsewhere, and not a
    // key anyone is likely to be holding down while this runs.
    constexpr int kProbe = VK_F7;

    GetAsyncKeyState(kProbe);        // clear the "pressed since last call" bit

    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = static_cast<WORD>(kProbe);
    in.ki.wScan = scan_code(kProbe);
    if (SendInput(1, &in, sizeof(INPUT)) != 1) return false;

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    const bool seen = (GetAsyncKeyState(kProbe) & 0x8000) != 0;

    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));

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

    return send_key(vk, false);
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
        SendInput(1, &in, sizeof(INPUT));
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
    key_up(key_down_);
    return ok;
}

bool InputManager::face_left() {
    if (block_movement_.load()) return false;
    // A tap short enough to turn the character without displacing it.
    if (!key_down(key_left_)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(18));
    return key_up(key_left_);
}

bool InputManager::face_right() {
    if (block_movement_.load()) return false;
    if (!key_down(key_right_)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(18));
    return key_up(key_right_);
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
    if (!window_focused()) return false;
    if (text.empty()) return false;

    key_press(chat_key);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    // Unicode injection: typing via VK codes would depend on the active
    // keyboard layout, which we don't control.
    for (char ch : text) {
        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wScan   = static_cast<WORD>(static_cast<unsigned char>(ch));
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

        SendInput(2, in, sizeof(INPUT));
        std::this_thread::sleep_for(std::chrono::milliseconds(jitter(20, 45)));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    key_press(chat_key);
    return true;
}

// ------------------------------------------------------------------ pointer

bool InputManager::move_pointer(int x, int y) {
    HWND hwnd = static_cast<HWND>(hwnd_.load());
    if (!hwnd || !IsWindow(hwnd)) return false;

    POINT at{ x, y };
    if (!ClientToScreen(hwnd, &at)) return false;

    // SendInput works in a 0..65535 grid across the whole virtual desktop, not
    // in pixels, so the conversion has to account for monitors left of or above
    // the primary one -- where the origin is negative and a naive scaling puts
    // every click on the wrong screen.
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dx = static_cast<LONG>((at.x - vx) * 65535LL / vw);
    in.mi.dy = static_cast<LONG>((at.y - vy) * 65535LL / vh);
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

bool InputManager::click_at(int x, int y, bool right_button) {
    if (!move_pointer(x, y)) return false;
    // A pause between arriving and pressing. Several of these buttons light up
    // on hover and ignore a click that lands in the same instant as the move.
    std::this_thread::sleep_for(std::chrono::milliseconds(jitter(40, 90)));

    INPUT in[2]{};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = right_button ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    in[1] = in[0];
    in[1].mi.dwFlags = right_button ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;

    if (SendInput(1, &in[0], sizeof(INPUT)) != 1) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(jitter(30, 70)));
    return SendInput(1, &in[1], sizeof(INPUT)) == 1;
}

bool InputManager::type_text(const std::string& utf8) {
    if (utf8.empty()) return false;

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
        if (SendInput(2, in, sizeof(INPUT)) != 2) return false;
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
    if (text.empty()) return 0;

    std::string s = to_lower(text);
    s.erase(0, s.find_first_not_of(" \t"));
    if (s.empty()) return 0;

    // Numeric forms: "90", "0x5A".
    if (std::isdigit(static_cast<unsigned char>(s[0]))) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(s.c_str(), &end, 0);
        if (end && *end == '\0' && v <= 0xFF) return static_cast<std::uint32_t>(v);
    }

    if (s.size() == 1) {
        const char c = s[0];
        if (c >= 'a' && c <= 'z') return static_cast<std::uint32_t>('A' + (c - 'a'));
        if (c >= '0' && c <= '9') return static_cast<std::uint32_t>(c);
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
