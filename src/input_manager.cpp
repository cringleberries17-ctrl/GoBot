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
#include <utility>
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

// ---- the drop's keys: Ctrl + ↓ (InputManager::drop_ctrl_down) ----

// Fixed, whatever the keyboard layout: left Ctrl, as GoBot's key 17 always
// went, and ↓ as the arrows already reach the game -- scan 0x50 with the
// extended flag is E0 50, DIK_DOWN. ↓'s two are used in drop_events alone.
constexpr WORD kCtrlVk   = 0x11;
constexpr WORD kCtrlScan = 0x1D;
constexpr WORD kDownVk   = 0x28;
constexpr WORD kDownScan = 0x50;
/// The high jump's second key (InputManager::chord_ctrl): ↑, E0 48, DIK_UP.
constexpr WORD kUpVk     = 0x26;
constexpr WORD kUpScan   = 0x48;
/// inject()'s `chord`: only the drop's [Ctrl↓, ↓↓] is sent with it.
constexpr bool kChord = true;

/// The process's drops: how many InputManagers have one running or owe its
/// ups, and the thread running one -- a high jump's Ctrl + ↑ too, the same
/// chord. While any does, inject() refuses every key-down and mouse-down from
/// any other thread. Process-wide, as the keyboard is.
std::atomic<int>   g_chord_busy{ 0 };
std::atomic<DWORD> g_chord_owner{ 0 };
std::atomic<bool>  g_said_bare_down{ false };
std::atomic<bool>  g_said_chord_refusal{ false };
std::atomic<InputManager::PretendSender*> g_pretend{ nullptr };

/// InputManager::last_input's, process-wide as the keyboard is: the kind in
/// the top byte of `key` and the code below it, so a reader never has one
/// event's kind with another's code. Its time may be the next event's: those
/// are milliseconds apart, and the line is the log's alone.
std::atomic<std::uint32_t> g_last_input_key{ 0 };
std::atomic<std::uint64_t> g_last_input_ms{ 0 };
std::atomic<const char*>   g_last_input_what{ "" };
/// Set on a thread while type_key sends a typed line's keys (Typing): what
/// goes down is noted for last_input as text, never as which key.
thread_local bool t_typing = false;

/// t_typing for as long as one lives.
class Typing {
public:
    Typing() { t_typing = true; }
    ~Typing() { t_typing = false; }
    Typing(const Typing&) = delete;
    Typing& operator=(const Typing&) = delete;
};

/// The test's pretend Windows, only while the "no real input" switch is on.
InputManager::PretendSender* pretend() {
    InputManager::PretendSender* const p = g_pretend.load();
    return p != nullptr && real_input_forbidden() ? p : nullptr;
}

/// One step's events into `out`; how many. `second`, the chord's key after
/// Ctrl: ↓, the drop's; ↑ in its place for the high jump -- the same steps,
/// Down and DownUp putting ↑ down and up.
UINT drop_events(InputManager::DropStep s, INPUT (&out)[2],
                 InputManager::ChordKey second = InputManager::ChordKey::Down) {
    const auto key = [](WORD vk, WORD scan, DWORD flags) {
        INPUT in{};
        in.type       = INPUT_KEYBOARD;
        in.ki.wVk     = vk;
        in.ki.wScan   = scan;
        in.ki.dwFlags = flags;
        return in;
    };
    const INPUT ctrl_down = key(kCtrlVk, kCtrlScan, 0);
    const INPUT ctrl_up   = key(kCtrlVk, kCtrlScan, KEYEVENTF_KEYUP);
    const INPUT down_down = key(kDownVk, kDownScan, KEYEVENTF_EXTENDEDKEY);
    const INPUT down_up   = key(kDownVk, kDownScan, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP);
    const bool  high      = second == InputManager::ChordKey::Up;
    const INPUT two_down  = high ? key(kUpVk, kUpScan, KEYEVENTF_EXTENDEDKEY) : down_down;
    const INPUT two_up    = high ? key(kUpVk, kUpScan, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP) : down_up;
    switch (s) {
        case InputManager::DropStep::Lead:   out[0] = ctrl_down; return 1;
        case InputManager::DropStep::Down:   out[0] = ctrl_down; out[1] = two_down; return 2;
        case InputManager::DropStep::DownUp: out[0] = two_up; return 1;
        case InputManager::DropStep::CtrlUp: out[0] = ctrl_up; return 1;
        case InputManager::DropStep::BothUp: out[0] = two_up; out[1] = ctrl_up; return 2;
    }
    return 0;
}

/// Ctrl, by its key code's low byte: Ctrl, left Ctrl or right Ctrl. Never a
/// jump's key (InputManager::jump_twice): alone it is the pick-up, with ↑ the
/// high jump, and that one is chord_ctrl's.
bool is_ctrl_key(std::uint32_t vk) {
    const std::uint32_t low = vk & 0xFFu;
    return low == static_cast<std::uint32_t>(kCtrlVk) || low == static_cast<std::uint32_t>(VK_LCONTROL) ||
           low == static_cast<std::uint32_t>(VK_RCONTROL);
}

/// One INPUT as the guards read it.
InputManager::SentEvent sent_event(const INPUT& in) {
    InputManager::SentEvent e;
    if (in.type == INPUT_MOUSE) {
        e.mouse = true;
        e.flags = in.mi.dwFlags;
    } else if (in.type == INPUT_KEYBOARD) {
        e.vk    = in.ki.wVk;
        e.scan  = in.ki.wScan;
        e.flags = in.ki.dwFlags;
    }
    return e;
}

bool key_down_event(const InputManager::SentEvent& e) {
    return !e.mouse && (e.flags & KEYEVENTF_KEYUP) == 0;
}

/// A key-down that is ↓ to Windows: by its key code's low byte, or sent as a
/// scan code by scan 0x50 -- E0 50 is ↓, and 50 alone is ↓ too with NumLock
/// off. Unicode input carries a character there, never a key.
bool down_goes_down(const InputManager::SentEvent& e) {
    return key_down_event(e) && ((e.vk & 0xFFu) == static_cast<unsigned>(kDownVk) ||
                                 ((e.flags & KEYEVENTF_SCANCODE) != 0 && e.scan == kDownScan));
}

/// A key-down that is Ctrl to Windows: sent as a scan code, by the scan
/// code alone; otherwise by its key code.
bool ctrl_goes_down(const InputManager::SentEvent& e) {
    if (!key_down_event(e)) return false;
    if ((e.flags & KEYEVENTF_SCANCODE) != 0) return e.scan == kCtrlScan;
    const unsigned vk = e.vk & 0xFFu;
    return vk == static_cast<unsigned>(kCtrlVk) || vk == static_cast<unsigned>(VK_LCONTROL) ||
           vk == static_cast<unsigned>(VK_RCONTROL);
}

/// InputManager::bare_down_in over any array of events: `at(i)` is the i-th.
template <class At>
bool bare_down_among(std::size_t n, bool chord, At at) {
    bool ctrl_before = false;
    for (std::size_t i = 0; i < n; ++i) {
        const InputManager::SentEvent e = at(i);
        if (down_goes_down(e) && !(chord && ctrl_before)) return true;
        if (ctrl_goes_down(e)) ctrl_before = true;
    }
    return false;
}

/// Whether anything in the array goes down: a key or a mouse button.
template <class At>
bool any_down_among(std::size_t n, At at) {
    constexpr DWORD kButtonDowns =
        MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_RIGHTDOWN | MOUSEEVENTF_MIDDLEDOWN | MOUSEEVENTF_XDOWN;
    for (std::size_t i = 0; i < n; ++i) {
        const InputManager::SentEvent e = at(i);
        if (e.mouse ? (e.flags & kButtonDowns) != 0 : key_down_event(e)) return true;
    }
    return false;
}

/// The last key-down or button-down of the first `went` events of an array
/// that went out, for InputManager::last_input: a typed key (`text`) or a
/// Unicode character as Text, never its code.
void note_went_down(const INPUT* in, UINT went, const char* what, bool text) {
    using Kind = InputManager::LastInput::Kind;
    for (UINT i = went; i-- > 0;) {
        const InputManager::SentEvent e = sent_event(in[i]);
        Kind kind = Kind::None;
        std::uint32_t code = 0;
        if (e.mouse) {
            if ((e.flags & MOUSEEVENTF_LEFTDOWN) != 0)        code = 1;
            else if ((e.flags & MOUSEEVENTF_RIGHTDOWN) != 0)  code = 2;
            else if ((e.flags & MOUSEEVENTF_MIDDLEDOWN) != 0) code = 3;
            else if ((e.flags & MOUSEEVENTF_XDOWN) != 0)      code = 4;
            if (code != 0) kind = Kind::Mouse;
        } else if (key_down_event(e)) {
            const bool typed = text || (e.flags & KEYEVENTF_UNICODE) != 0;
            kind = typed ? Kind::Text : Kind::Key;
            code = typed ? 0u : (e.vk & 0xFFFFu);
        }
        if (kind == Kind::None) continue;
        g_last_input_what.store(what != nullptr ? what : "");
        g_last_input_key.store((static_cast<std::uint32_t>(kind) << 24) | code);
        g_last_input_ms.store(now_ms());
        return;
    }
}

/// Every input this class sends goes through here. Whichever way a key, a
/// click or a move got this far, the "no real input" switch is asked once
/// more just before Windows is -- the one place nothing can get round.
///
/// Before that, in this order: ↓ never goes down but inside a drop's Ctrl
/// (`chord`, passed by drop_ctrl_down alone), whoever built the array; and
/// while a drop runs or owes its ups, no other thread puts anything down --
/// a skill key or a click between Ctrl and its up would be Ctrl + that. A
/// test's pretend sender, when there is one, takes the place of Windows.
/// What went down is noted for last_input: a typed line's keys as text.
UINT inject(INPUT* in, UINT count, const char* what, bool chord = false) {
    if (in == nullptr || count == 0) return 0;
    const auto at = [in](std::size_t i) { return sent_event(in[i]); };
    if (bare_down_among(count, chord, at)) {
        if (!g_said_bare_down.exchange(true))
            GB_WARN("Not sending Down (vk 0x28) outside Ctrl + Down: standing on a gate it takes the character "
                    "to another map (%s)", what != nullptr ? what : "?");
        return 0;
    }
    if (g_chord_busy.load() > 0 && g_chord_owner.load() != GetCurrentThreadId() && any_down_among(count, at)) {
        if (!g_said_chord_refusal.exchange(true))
            GB_INFO("Input: refused to %s while Ctrl + Down or Ctrl + Up is pressed, or its keys are owed",
                    what != nullptr ? what : "?");
        return 0;
    }
    if (InputManager::PretendSender* const p = pretend()) {
        std::vector<InputManager::SentEvent> events;
        events.reserve(count);
        for (UINT i = 0; i < count; ++i) events.push_back(at(i));
        const UINT went = p->send ? std::min(count, static_cast<UINT>(p->send(events, chord))) : count;
        note_went_down(in, went, what, t_typing);
        return went;
    }
    if (refuse_real_input(what)) return 0;
    const UINT went = SendInput(count, in, sizeof(INPUT));
    note_went_down(in, went, what, t_typing);
    return went;
}

/// Whether Windows reads a key down now -- the pretend sender's word in a
/// test, and nothing down when it has none.
bool reads_down(InputManager::PretendSender* fake, int vk) {
    if (fake != nullptr) return fake->key_held && fake->key_held(vk);
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/// go_on, asked: a callback that throws, or none, says no.
bool asks_yes(const std::function<bool()>& go_on) noexcept {
    try {
        return go_on && go_on();
    } catch (...) {
        return false;
    }
}

/// The calling thread at TIME_CRITICAL while one lives, put back as it was
/// after: ↓ is held by a sleep, and a thread a busy PC puts off holds it
/// longer.
class Urgent {
public:
    Urgent() : was_(GetThreadPriority(GetCurrentThread())) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
    ~Urgent() {
        if (was_ != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(GetCurrentThread(), was_);
    }
    Urgent(const Urgent&) = delete;
    Urgent& operator=(const Urgent&) = delete;

private:
    int was_;
};

/// Runs `f` as it goes out of scope, however that happens.
template <class F>
class OnExit {
public:
    explicit OnExit(F f) : f_(std::move(f)) {}
    ~OnExit() { f_(); }
    OnExit(const OnExit&) = delete;
    OnExit& operator=(const OnExit&) = delete;

private:
    F f_;
};

} // namespace

InputManager::InputManager()
    : rng_(std::random_device{}()) {}

InputManager::~InputManager() {
    release_all();
    // An up that would not go out is nobody's to send any more: no longer
    // counted as owed, or every other key-down in the process would wait on
    // it for good.
    std::lock_guard<std::mutex> lock(chord_mutex_);
    if (chord_counted_) {
        chord_counted_ = false;
        if (g_chord_busy.fetch_sub(1) == 1) g_said_chord_refusal = false;
    }
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
    // A drop's Ctrl and ↓ first, in their own order -- ↓ before Ctrl, in one
    // array -- and the drop cut short: it puts nothing down after this, as
    // its look before ↓ and its ↓ are under the same lock. Never in
    // held_keys_, which would let them go in any order. A high jump's ↑ and
    // Ctrl the same way, ↑ first.
    if (chord_in_flight_.load() || down_owed_.load() || up_owed_.load() || ctrl_owed_.load()) {
        std::lock_guard<std::mutex> lock(chord_mutex_);
        if (chord_in_flight_.load()) chord_cancel_ = true;
        send_owed();
        chord_busy_sync();
    }

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

// The packet bot's old drop, ↓ held under the jump key: always refused, as
// ↓ is never pressed (send_key). It stays so; the drop is drop_ctrl_down.
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
    // A typed line's letter: noted for last_input as text, never which key.
    const Typing typing;
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

// ---------------------------------------------------------- the drop: Ctrl + ↓

std::vector<InputManager::KeyEvent> InputManager::drop_batch(DropStep s) { return chord_batch(ChordKey::Down, s); }

std::vector<InputManager::KeyEvent> InputManager::chord_batch(ChordKey key, DropStep s) {
    INPUT in[2]{};
    const UINT n = drop_events(s, in, key);
    std::vector<KeyEvent> out;
    for (UINT i = 0; i < n; ++i) {
        KeyEvent e;
        e.vk    = in[i].ki.wVk;
        e.scan  = in[i].ki.wScan;
        e.flags = in[i].ki.dwFlags;
        out.push_back(e);
    }
    return out;
}

bool InputManager::bare_down_in(const SentEvent* events, std::size_t n, bool chord) {
    if (events == nullptr) return false;
    return bare_down_among(n, chord, [events](std::size_t i) { return events[i]; });
}

void InputManager::set_pretend_sender(PretendSender* p) { g_pretend.store(p); }

unsigned InputManager::pretend_inject(const std::vector<SentEvent>& events) {
    if (pretend() == nullptr || events.empty()) return 0;
    std::vector<INPUT> in(events.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
        const SentEvent& e = events[i];
        if (e.mouse) {
            in[i].type       = INPUT_MOUSE;
            in[i].mi.dwFlags = e.flags;
        } else {
            in[i].type       = INPUT_KEYBOARD;
            in[i].ki.wVk     = e.vk;
            in[i].ki.wScan   = e.scan;
            in[i].ki.dwFlags = e.flags;
        }
    }
    // Never a chord: a test's array meets the guards as any key or click does.
    return inject(in.data(), static_cast<UINT>(in.size()), "send a test's input");
}

InputManager::LastInput InputManager::last_input() {
    LastInput out;
    out.at_ms = g_last_input_ms.load();
    const std::uint32_t key = g_last_input_key.load();
    out.what = g_last_input_what.load();
    if (out.at_ms == 0 || key == 0) return LastInput{};
    out.kind = static_cast<LastInput::Kind>(key >> 24);
    out.vk = key & 0xFFFFFFu;
    return out;
}

std::string InputManager::last_input_text(const LastInput& in, std::uint64_t at_ms) {
    if (in.kind == LastInput::Kind::None || in.at_ms == 0) return "none since GoBot started";
    std::string out;
    switch (in.kind) {
        case LastInput::Kind::Key:
            out = "key " + key_name(in.vk);
            break;
        case LastInput::Kind::Text:
            out = "typed text";
            break;
        case LastInput::Kind::Mouse:
            out = in.vk == 1 ? "the left mouse button" : in.vk == 2 ? "the right mouse button"
                                                       : in.vk == 3 ? "the middle mouse button"
                                                                    : "a mouse button";
            break;
        case LastInput::Kind::None:
            break;
    }
    const bool before = in.at_ms <= at_ms;
    const std::uint64_t gap = before ? at_ms - in.at_ms : in.at_ms - at_ms;
    char when[48];
    std::snprintf(when, sizeof when, " %.1f s %s", static_cast<double>(gap) / 1000.0, before ? "before" : "after");
    out += when;
    if (in.what != nullptr && *in.what != '\0') out += std::string(" (") + in.what + ")";
    return out;
}

bool InputManager::keys_owed() const noexcept {
    return (down_owed_.load() || up_owed_.load() || ctrl_owed_.load()) && !chord_in_flight_.load();
}

void InputManager::chord_busy_sync() noexcept {
    const bool owed = down_owed_.load() || up_owed_.load() || ctrl_owed_.load();
    if (!owed) owed_tries_ = 0;
    const bool busy = owed || chord_in_flight_.load();
    if (busy == chord_counted_) return;
    chord_counted_ = busy;
    if (busy) g_chord_busy.fetch_add(1);
    else if (g_chord_busy.fetch_sub(1) == 1) g_said_chord_refusal = false;
}

bool InputManager::send_owed() {
    INPUT b[2]{};
    if (down_owed_.load()) {
        // Ctrl↑ never alone while ↓ is owed: in one array, ↓'s first.
        const UINT n = drop_events(DropStep::BothUp, b);
        const UINT went = inject(b, n, "let go of Down and Ctrl");
        if (went >= 1) down_owed_ = false;
        if (went >= 2) ctrl_owed_ = false;
    } else if (up_owed_.load()) {
        // A high jump's the same way: ↑'s up first, Ctrl's after it.
        const UINT n = drop_events(DropStep::BothUp, b, ChordKey::Up);
        const UINT went = inject(b, n, "let go of Up and Ctrl");
        if (went >= 1) up_owed_ = false;
        if (went >= 2) ctrl_owed_ = false;
    } else if (ctrl_owed_.load()) {
        const UINT n = drop_events(DropStep::CtrlUp, b);
        if (inject(b, n, "let go of Ctrl") == n) ctrl_owed_ = false;
    }
    return !down_owed_.load() && !up_owed_.load() && !ctrl_owed_.load();
}

bool InputManager::settle_owed() noexcept {
    if (!down_owed_.load() && !up_owed_.load() && !ctrl_owed_.load()) return true;
    try {
        std::lock_guard<std::mutex> lock(chord_mutex_);
        if (chord_in_flight_.load()) return false;     // the drop sends its own, in its order
        const bool clear = send_owed();
        if (!clear) ++owed_tries_;
        chord_busy_sync();
        return clear;
    } catch (...) {
        return false;
    }
}

void InputManager::end_chord(bool send_ups) noexcept {
    DWORD me = GetCurrentThreadId();
    try {
        std::lock_guard<std::mutex> lock(chord_mutex_);
        if (!chord_in_flight_.load()) return;
        if (send_ups) {
            try {
                send_owed();
            } catch (...) {
                // Left owed, for settle_owed and release_all.
            }
        }
        chord_in_flight_ = false;
        chord_cancel_ = false;
        g_chord_owner.compare_exchange_strong(me, 0);
        chord_busy_sync();
    } catch (...) {
        // The lock itself failed: no drop runs from here, whatever is owed.
        chord_in_flight_ = false;
        chord_cancel_ = false;
        g_chord_owner.compare_exchange_strong(me, 0);
    }
}

InputManager::DropPress InputManager::drop_ctrl_down(const std::function<bool()>& go_on) noexcept {
    DropPress out;
    try {
        drop_chord(ChordKey::Down, go_on, out);
    } catch (...) {
        // The guard has sent what was owed. `out` says how far it got: Ctrl
        // down (Aborted), or ↓ too (Cut: the game may have taken it).
        if (out.down_ms != 0) out.held_ms = static_cast<std::uint32_t>(now_ms() - out.down_ms);
    }
    return out;
}

InputManager::DropPress InputManager::chord_ctrl(ChordKey key, const std::function<bool()>& go_on) noexcept {
    // ↓'s is drop_ctrl_down itself, guard and all: the drop goes one way.
    if (key == ChordKey::Down) return drop_ctrl_down(go_on);
    DropPress out;
    try {
        drop_chord(key, go_on, out);
    } catch (...) {
        // As drop_ctrl_down's: the guard has sent what was owed.
        if (out.down_ms != 0) out.held_ms = static_cast<std::uint32_t>(now_ms() - out.down_ms);
    }
    return out;
}

InputManager::DropPress InputManager::high_jump(const std::function<bool()>& go_on) noexcept {
    return chord_ctrl(ChordKey::Up, go_on);
}

// Each send under chord_mutex_, so that release_all on another thread -- F12,
// DỪNG, HUNT OFF, the game losing focus -- comes wholly before the look and ↓,
// or wholly after ↓: never between them. No sleep is taken under it, and
// nothing between Ctrl's down and its up logs or takes another lock. The high
// jump's ↑ goes through the same steps in ↓'s place (`high`), held its own
// time, its [Ctrl↓, ↑↓] sent as any array is.
void InputManager::drop_chord(ChordKey key, const std::function<bool()>& go_on, DropPress& out) {
    PretendSender* const fake = pretend();
    const auto in_front = [this, fake] {
        if (fake != nullptr) return !fake->focused || fake->focused();
        return window_focused();
    };
    const auto ms_since = [](std::chrono::steady_clock::time_point from) {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from).count());
    };
    const bool high = key == ChordKey::Up;
    // The up owed for the key after Ctrl: ↓'s, or the high jump's ↑'s.
    std::atomic<bool>& second_owed = high ? up_owed_ : down_owed_;

    // ---- before anything goes down: as a key goes ----
    if (hwnd_.load() == nullptr) return;
    if (fake == nullptr && (high ? refuse_real_input("press Ctrl + Up to jump high")
                                 : refuse_real_input("press Ctrl + Down to drop through a platform")))
        return;
    if (block_movement_.load()) return;
    if (!in_front()) return;
    if (!asks_yes(go_on)) return;
    const int lead = jitter(kDropLeadMinMs, kDropLeadMaxMs);
    const int hold = high ? jitter(kJumpHoldMinMs, kJumpHoldMaxMs) : jitter(kDropHoldMinMs, kDropHoldMaxMs);
    const int tail = jitter(kDropTailMinMs, kDropTailMaxMs);

    {
        Urgent urgent;
        // The steps send their own ups; whatever cuts them short -- a throw --
        // leaves the guard to.
        bool done = false;
        const OnExit guard([this, &done] { end_chord(!done); });

        // ---- Ctrl alone ----
        auto ctrl_from = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(chord_mutex_);
            // None while a drop runs or its ups are owed, here or on another
            // manager: one owner at a time.
            if (g_chord_busy.load() > 0) return;
            DWORD nobody = 0;
            if (!g_chord_owner.compare_exchange_strong(nobody, GetCurrentThreadId())) return;
            chord_cancel_ = false;
            chord_in_flight_ = true;
            ctrl_owed_ = true;
            chord_busy_sync();
            INPUT b[2]{};
            const UINT n = drop_events(DropStep::Lead, b, key);
            if (inject(b, n, "press Ctrl") != n) {
                ctrl_owed_ = false;
                done = true;
                return;
            }
            ctrl_from = std::chrono::steady_clock::now();
            out.sent = DropSent::Aborted;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(lead));

        // ---- the last look, then ↓ inside Ctrl ----
        // go_on asked outside the lock: it may take locks of its own, or let
        // go of every key. A release_all after it is seen under the lock.
        const bool go = asks_yes(go_on);
        {
            std::lock_guard<std::mutex> lock(chord_mutex_);
            INPUT b[2]{};
            if (!go || chord_cancel_.load() || !in_front()) {
                if (ctrl_owed_.load()) {
                    const UINT n = drop_events(DropStep::CtrlUp, b, key);
                    if (inject(b, n, "let go of Ctrl") == n) ctrl_owed_ = false;
                }
                done = true;
                return;
            }
            second_owed = true;
            const UINT n = drop_events(DropStep::Down, b, key);
            // Only the drop's ↓ is let past inject()'s guard as its chord; ↑
            // needs no letting past, and goes as any array goes.
            const UINT sent = high ? inject(b, n, "press Ctrl + Up") : inject(b, n, "press Ctrl + Down", kChord);
            if (sent != n) {
                // ↓ did not go out, Ctrl may have: both let go of.
                const UINT m = drop_events(DropStep::BothUp, b, key);
                const UINT up = inject(b, m, high ? "let go of Up and Ctrl" : "let go of Down and Ctrl");
                if (up >= 1) second_owed = false;
                if (up >= 2) ctrl_owed_ = false;
                done = true;
                return;
            }
            out.down_ms = now_ms();
            out.lead_ms = static_cast<std::uint32_t>(ms_since(ctrl_from));
            out.sent = DropSent::Cut;
        }

        // ---- the hold: cut short by a no or a release_all ----
        const auto held_from = std::chrono::steady_clock::now();
        for (int ms = ms_since(held_from); ms < hold; ms = ms_since(held_from)) {
            if (chord_cancel_.load() || !asks_yes(go_on)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(10, hold - ms)));
        }

        // ---- ↓ up: wherever focus is ----
        bool stuck = false;
        {
            std::lock_guard<std::mutex> lock(chord_mutex_);
            if (second_owed.load()) {
                INPUT b[2]{};
                UINT n = drop_events(DropStep::DownUp, b, key);
                if (inject(b, n, high ? "let go of Up" : "let go of Down") == n) {
                    second_owed = false;
                } else {
                    n = drop_events(DropStep::BothUp, b, key);
                    const UINT up = inject(b, n, high ? "let go of Up and Ctrl" : "let go of Down and Ctrl");
                    if (up >= 1) second_owed = false;
                    if (up >= 2) ctrl_owed_ = false;
                    stuck = up == 0;
                }
            }
            out.held_ms = static_cast<std::uint32_t>(now_ms() - out.down_ms);
            out.sent = out.held_ms < kDropCutMs ? DropSent::Cut : DropSent::Sent;
        }
        const auto second_up = std::chrono::steady_clock::now();

        // ---- Ctrl up: wherever focus is, never before ↓'s ----
        if (!stuck && !chord_cancel_.load() && asks_yes(go_on))
            std::this_thread::sleep_for(std::chrono::milliseconds(tail));
        {
            std::lock_guard<std::mutex> lock(chord_mutex_);
            if (!stuck && ctrl_owed_.load() && !second_owed.load()) {
                INPUT b[2]{};
                const UINT n = drop_events(DropStep::CtrlUp, b, key);
                if (inject(b, n, "let go of Ctrl") == n || inject(b, n, "let go of Ctrl") == n)
                    ctrl_owed_ = false;
                if (!ctrl_owed_.load()) out.tail_ms = static_cast<std::uint32_t>(ms_since(second_up));
            }
            done = true;
        }
    }

    // A ↓ Windows still reads down -- the game's own up lost somewhere -- is
    // owed, and sent again. The high jump's ↑ the same.
    if (out.down_ms != 0 && (fake == nullptr || fake->key_held)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (reads_down(fake, high ? VK_UP : VK_DOWN)) {
            {
                std::lock_guard<std::mutex> lock(chord_mutex_);
                second_owed = true;
                chord_busy_sync();
            }
            settle_owed();
        }
    }
}

// ------------------------------------------------------ the jumps: ↑ and ↑↑

InputManager::JumpPress InputManager::jump_twice(std::uint32_t vk, int gap_ms,
                                                 const std::function<bool()>& go_on) noexcept {
    JumpPress out;
    try {
        jump_keys(vk, std::max(0, gap_ms), go_on, out);
    } catch (...) {
        // Each ↑'s guard sent its up; `out` says how far it got.
    }
    return out;
}

InputManager::JumpPress InputManager::jump_once(std::uint32_t vk, const std::function<bool()>& go_on) noexcept {
    JumpPress out;
    try {
        jump_keys(vk, 0, go_on, out);
    } catch (...) {
        // As jump_twice's.
    }
    return out;
}

void InputManager::jump_keys(std::uint32_t vk, int gap_ms, const std::function<bool()>& go_on, JumpPress& out) {
    PretendSender* const fake = pretend();

    // ---- before anything goes down: as a key goes ----
    if (vk == 0 || hwnd_.load() == nullptr) return;
    // Never ↓, which standing on a gate takes the character to another map;
    // never Ctrl -- alone the pick-up, and with ↑ the high jump, chord_ctrl's.
    if (never_pressed(vk) || is_ctrl_key(vk)) return;
    if (fake == nullptr && refuse_real_input("press Up to jump")) return;
    if (block_movement_.load()) return;
    // Nothing of a chord running or owed, here or anywhere: its keys first.
    if (chord_in_flight_.load() || down_owed_.load() || up_owed_.load() || ctrl_owed_.load() ||
        g_chord_busy.load() > 0)
        return;
    if (!asks_yes(go_on)) return;
    const int hold1 = jitter(kJumpHoldMinMs, kJumpHoldMaxMs);
    const int hold2 = jitter(kJumpHoldMinMs, kJumpHoldMaxMs);
    const int second_at = gap_ms > 0 ? gap_ms + jitter(-kJumpSecondJitterMs, kJumpSecondJitterMs) : 0;

    // Held and timed by sleeps, which a busy PC puts off: at TIME_CRITICAL.
    Urgent urgent;
    bool whole = false;
    if (!jump_key(vk, hold1, go_on, out.first_down_ms, out.held1, whole)) return;
    out.sent = JumpSent::First;
    if (gap_ms <= 0 || !whole) return;

    // ---- the wait for the second: go_on asked through it ----
    // The last of it by yielding: a sleep ends on the timer's tick, 15.6 ms
    // apart, and the second is timed tighter than that.
    using Clock = std::chrono::steady_clock;
    constexpr auto kSpin = std::chrono::milliseconds(20);
    const Clock::time_point due{ std::chrono::milliseconds(static_cast<std::int64_t>(out.first_down_ms) + second_at) };
    for (;;) {
        if (!asks_yes(go_on)) return;
        const Clock::duration left = due - Clock::now();
        if (left <= kSpin) break;
        std::this_thread::sleep_for(std::min<Clock::duration>(left - kSpin, std::chrono::milliseconds(10)));
    }
    while (Clock::now() < due) std::this_thread::yield();

    // ---- the second ↑: in time, with go_on's yes, or not at all ----
    if (now_ms() > out.first_down_ms + static_cast<std::uint64_t>(gap_ms + kJumpSecondSendLateMs)) return;
    if (!asks_yes(go_on)) return;
    if (jump_key(vk, hold2, go_on, out.second_down_ms, out.held2, whole)) out.sent = JumpSent::Both;
}

bool InputManager::jump_key(std::uint32_t vk, int hold_ms, const std::function<bool()>& go_on,
                            std::uint64_t& down_ms, std::uint32_t& held_ms, bool& whole) {
    PretendSender* const fake = pretend();
    whole = false;
    // Down only into the game in front.
    const bool front = fake != nullptr ? (!fake->focused || fake->focused()) : window_focused();
    if (!front) return false;

    // As send_key builds a key: ↑ extended, E0 48 -- never the numpad's 8.
    INPUT in{};
    in.type       = INPUT_KEYBOARD;
    in.ki.wVk     = static_cast<WORD>(vk);
    in.ki.wScan   = scan_code(vk);
    in.ki.dwFlags = is_extended(vk) ? KEYEVENTF_EXTENDEDKEY : 0;
    // Held before it goes: a release_all the moment it is down lets go of it.
    {
        std::lock_guard<std::mutex> lock(held_mutex_);
        held_keys_[vk] = true;
    }
    if (inject(&in, 1, "press Up to jump") != 1) {
        std::lock_guard<std::mutex> lock(held_mutex_);
        held_keys_.erase(vk);
        return false;
    }
    down_ms = now_ms();

    // ---- its up, whatever happens: owed, so wherever focus is ----
    bool upped = false;
    const auto let_go = [&] {
        INPUT up = in;
        up.ki.dwFlags |= KEYEVENTF_KEYUP;
        const bool went = inject(&up, 1, "let go of Up") == 1;
        upped = true;
        held_ms = static_cast<std::uint32_t>(now_ms() - down_ms);
        // Not taken: still held, and release_all sends it again.
        if (went) {
            std::lock_guard<std::mutex> lock(held_mutex_);
            held_keys_.erase(vk);
        }
    };
    const OnExit guard([&] {
        if (upped) return;
        try {
            let_go();
        } catch (...) {
            // Left held, for release_all.
        }
    });

    // ---- the hold: cut short by a no, or a release_all that let go of it ----
    const auto from = std::chrono::steady_clock::now();
    const auto ms_since = [&from] {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from).count());
    };
    bool cut = false;
    for (int ms = ms_since(); ms < hold_ms; ms = ms_since()) {
        if (!held(vk) || !asks_yes(go_on)) {
            cut = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(10, hold_ms - ms)));
    }
    let_go();
    whole = !cut;
    return true;
}

int InputManager::release_stray_drop_keys() noexcept {
    try {
        PretendSender* const fake = pretend();
        if (fake == nullptr && refuse_real_input("let go of a Down or Ctrl left down")) return 0;
        std::lock_guard<std::mutex> lock(chord_mutex_);
        if (chord_in_flight_.load()) return 0;
        int went = 0;
        INPUT b[2]{};
        if (reads_down(fake, VK_DOWN)) {
            const UINT n = drop_events(DropStep::DownUp, b);
            if (inject(b, n, "let go of Down") == n) ++went;
        }
        // The high jump's ↑, before Ctrl as its owed ups go (2026.11.05b).
        if (reads_down(fake, VK_UP)) {
            const UINT n = drop_events(DropStep::DownUp, b, ChordKey::Up);
            if (inject(b, n, "let go of Up") == n) ++went;
        }
        if (reads_down(fake, VK_CONTROL)) {
            const UINT n = drop_events(DropStep::CtrlUp, b);
            if (inject(b, n, "let go of Ctrl") == n) ++went;
        }
        return went;
    } catch (...) {
        return 0;
    }
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
