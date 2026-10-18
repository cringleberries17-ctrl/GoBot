#include "gobot/auto_login.hpp"
#include "gobot/common.hpp"
#include "gobot/game_install.hpp"
#include "gobot/logger.hpp"
#include "gobot/no_real_input.hpp"
#include "gobot/session_manager.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>

namespace gobot {
namespace {

/// For pointing at when a page wants no class or process filter.
const std::string kNothing;

/// VTCZone's program. Its window is found by its title, but whether it is
/// running at all, and which minimised window is its, is asked of the program.
const char* const kPublisherProcess = "VTCZone.exe";

/// The game's program, when login.ini names none: the one the keeper watches.
/// Running with no window yet still counts as the game being up.
const char* const kGameProcess = "Game.exe";

/// How long a click held back by a window in the way is tried again before
/// the attempt is given up. Long enough for a person to move the window; a
/// window nobody is there to move is the next attempt's, or a person's.
constexpr int kHeldMaxS = 30;

/// How long after bringing a window to the front it is looked at, and how
/// often at most it is brought there while a page of it is awaited: often
/// enough to find the page soon after the player stops using the desktop,
/// and not so often as to fight them for it.
constexpr int kSettleMs = 800;
constexpr int kForwardEveryS = 5;

/// How many looks, how far apart, decide that a game is stuck on a screen the
/// crossing does not know -- which ends it.
constexpr int kJudgeLooks = 3;
constexpr int kJudgeGapMs = 500;

/// Each window in Vietnamese, for the simple screen.
const char* window_vi(AutoLogin::On w) {
    switch (w) {
        case AutoLogin::On::Publisher: return "VTCZone";
        case AutoLogin::On::Client:    return "launcher của game";
        case AutoLogin::On::Game:      return "game";
    }
    return "game";
}

/// Each page's screen in Vietnamese, by what a player sees on it.
const char* screen_vi(AutoLogin::Step s) {
    switch (s) {
        case AutoLogin::Step::GameList:       return "danh sách game của VTCZone";
        case AutoLogin::Step::StartGame:      return "Chơi game";
        case AutoLogin::Step::ClientLauncher: return "START";
        case AutoLogin::Step::Connect:
        case AutoLogin::Step::ConnectAgain:   return "Kết Nối";
        case AutoLogin::Step::Channel:        return "chọn kênh";
        case AutoLogin::Step::Character:      return "chọn nhân vật";
        default:                              return "đăng nhập";
    }
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trimmed(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// How each of the three windows is asked for.
struct Spec {
    const std::string* title;
    const std::string* klass;
    const std::string* exe;
};

Spec spec_of(const AutoLogin::Settings& s, AutoLogin::On w) {
    // The game's launcher is found by class and program rather than by name,
    // because it has no name: it is a bare #32770 dialog put up by
    // GhostOnlineVTC.exe.
    switch (w) {
        case AutoLogin::On::Client: return { &s.client_window, &s.client_class, &s.client_process };
        case AutoLogin::On::Game:   return { &s.game_window, &s.game_class, &s.game_process };
        case AutoLogin::On::Publisher: break;
    }
    return { &s.launcher_window, &kNothing, &kNothing };
}

/// The window `InputManager::find_game_window` would pick for this spec,
/// found without a word in the log.
///
/// That one says so when it finds nothing, which is right when a person asked
/// for a window and wrong for a look taken every second while a page is
/// awaited: a minute of waiting for the game to open would be sixty warnings.
/// The scoring is the same -- a program or a class is strong evidence, a title
/// on its own only counts when it matches exactly -- so the two never
/// disagree about which window is meant. A Security Alert box is never it: it
/// is a #32770 of the game's launcher too, and is looked for on its own.
void* best_window(const std::vector<InputManager::WindowInfo>& all, const Spec& spec) {
    const std::string want_title = lower(*spec.title);
    const std::string want_class = lower(*spec.klass);
    const std::string want_proc  = lower(*spec.exe);
    constexpr int kStrong = 3;

    void* best = nullptr;
    int best_rank = -1;
    for (const auto& w : all) {
        if (AutoLogin::is_security_alert(w.window_class, w.title)) continue;

        int score = 0;
        if (!want_proc.empty()) {
            // A name that could not be read is unknown, not wrong.
            if (w.process.empty())                     score += 0;
            else if (lower(w.process) == want_proc)    score += 4;
            else                                       continue;
        }
        if (!want_class.empty() && lower(w.window_class) == want_class) score += kStrong;
        if (!want_title.empty()) {
            const std::string t = lower(w.title);
            if (t == want_title)                              score += kStrong;
            else if (t.rfind(want_title, 0) == 0)             score += 2;
            else if (t.find(want_title) != std::string::npos) score += 1;
        }
        if (score < kStrong || score <= best_rank) continue;
        best_rank = score;
        best = w.hwnd;
    }
    return best;
}

/// Of `all`, the windows of the game's own program, told by where each one
/// runs from (GameInstall). The game's window is found by its title, and a
/// folder or a document named GhostOnlineVTC has that title too -- one the
/// crossing would bring forward and click on -- as might any other program
/// called Game.exe. A window whose program cannot be read is not the game.
std::vector<InputManager::WindowInfo> only_the_game(std::vector<InputManager::WindowInfo> all) {
    const GameInstall install = GameInstall::here();
    const GameInstall::Drives drives = GameInstall::Drives::here();
    all.erase(std::remove_if(all.begin(), all.end(),
                             [&](const InputManager::WindowInfo& w) {
                                 return !install.is_game(w.path, drives);
                             }),
              all.end());
    return all;
}

/// The program a window belongs to, by its file name -- or its class when the
/// program will not say, as a more privileged one will not. Never its title:
/// a title can be anything, a chat with somebody or a document's name, and
/// this goes into the log and onto the screen.
std::string program_of(HWND hwnd) {
    if (hwnd == nullptr) return "?";
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
        if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH]{};
            DWORD size = MAX_PATH;
            std::string name;
            if (QueryFullProcessImageNameW(proc, 0, path, &size) && size > 0) {
                const std::wstring wide(path, size);
                const std::wstring file = wide.substr(wide.find_last_of(L"\\/") + 1);
                const int n = WideCharToMultiByte(CP_UTF8, 0, file.c_str(),
                                                  static_cast<int>(file.size()), nullptr, 0,
                                                  nullptr, nullptr);
                if (n > 0) {
                    name.resize(static_cast<std::size_t>(n));
                    WideCharToMultiByte(CP_UTF8, 0, file.c_str(), static_cast<int>(file.size()),
                                        name.data(), n, nullptr, nullptr);
                }
            }
            CloseHandle(proc);
            if (!name.empty()) return name;
        }
    }
    char cls[256]{};
    if (GetClassNameA(hwnd, cls, sizeof(cls) - 1) > 0) return cls;
    return "?";
}

/// Whether a window is Windows' taskbar, on the main screen or another one.
bool is_taskbar(HWND hwnd) {
    char cls[64]{};
    GetClassNameA(hwnd, cls, sizeof(cls) - 1);
    return std::strcmp(cls, "Shell_TrayWnd") == 0 || std::strcmp(cls, "Shell_SecondaryTrayWnd") == 0;
}

/// Where two boxes meet, and whether that is nowhere.
AutoLogin::Box meet(const AutoLogin::Box& a, const AutoLogin::Box& b) {
    return AutoLogin::Box{ std::max(a.left, b.left), std::max(a.top, b.top),
                           std::min(a.right, b.right), std::min(a.bottom, b.bottom) };
}
bool nowhere(const AutoLogin::Box& b) { return b.right <= b.left || b.bottom <= b.top; }

/// Every screen, as the rectangle it shows of the desktop.
BOOL CALLBACK add_screen(HMONITOR, HDC, LPRECT r, LPARAM out) {
    reinterpret_cast<std::vector<AutoLogin::Box>*>(out)->push_back(
        AutoLogin::Box{ r->left, r->top, r->right, r->bottom });
    return TRUE;
}

/// What a wait that ran out says, by which page it was and whether the
/// window that page is drawn on ever came up.
AutoLogin::Why why_not_seen(const AutoLogin::Page& page, bool window_up) {
    using Why = AutoLogin::Why;
    if (!window_up) {
        switch (page.on) {
            case AutoLogin::On::Publisher: return Why::LauncherNoWindow;
            case AutoLogin::On::Client:    return Why::ClientStart;
            case AutoLogin::On::Game:      return Why::GameWindow;
        }
    }
    switch (page.step) {
        case AutoLogin::Step::ClientLauncher: return Why::ClientStart;
        case AutoLogin::Step::Connect:
        case AutoLogin::Step::ConnectAgain:   return Why::Connect;
        case AutoLogin::Step::Channel:        return Why::Channel;
        case AutoLogin::Step::Character:      return Why::Character;
        default:                              return Why::GamePage;
    }
}

} // namespace

const char* AutoLogin::step_name(Step s) {
    switch (s) {
        case Step::Idle:           return "idle";
        case Step::StartLauncher:  return "starting the launcher";
        case Step::FindLauncher:   return "finding the launcher";
        case Step::GameList:       return "choosing the game";
        case Step::StartGame:      return "start game";
        case Step::ClientLauncher: return "client launcher";
        case Step::GameWindow:     return "waiting for the game window";
        case Step::Connect:        return "connect";
        case Step::Channel:        return "choosing a channel";
        case Step::ConnectAgain:   return "connect again";
        case Step::Character:      return "choosing the character";
        case Step::EndClient:      return "ending the half-started client";
        case Step::InWorld:        return "in the world";
        case Step::Blocked:        return "blocked";
    }
    return "?";
}

const char* AutoLogin::step_name_vi(Step s) {
    switch (s) {
        case Step::Idle:           return "chưa làm gì";
        case Step::StartLauncher:  return "mở VTCZone";
        case Step::FindLauncher:   return "tìm cửa sổ VTCZone";
        case Step::GameList:       return "chọn Ghost Online trong VTCZone";
        case Step::StartGame:      return "bấm Chơi game";
        case Step::ClientLauncher: return "bấm START";
        case Step::GameWindow:     return "chờ nhân vật vào game";
        case Step::Connect:        return "bấm Kết Nối";
        case Step::Channel:        return "chọn kênh";
        case Step::ConnectAgain:   return "bấm Kết Nối";
        case Step::Character:      return "chọn nhân vật";
        case Step::EndClient:      return "đóng game đang mở dở";
        case Step::InWorld:        return "đã vào game";
        case Step::Blocked:        return "cần người kiểm tra";
    }
    return "?";
}

const char* AutoLogin::on_name(On w) {
    switch (w) {
        case On::Publisher: return "the publisher's launcher";
        case On::Client:    return "the game launcher";
        case On::Game:      return "the game";
    }
    return "?";
}

const char* AutoLogin::pick_name(Pick p) {
    switch (p) {
        case Pick::One:  return "one";
        case Pick::Row:  return "row";
        case Pick::Slot: return "slot";
        case Pick::Step: return "step";
    }
    return "one";
}

AutoLogin::Pick AutoLogin::pick_from(const std::string& word, Pick fallback) {
    if (word == "one")  return Pick::One;
    if (word == "row")  return Pick::Row;
    if (word == "slot") return Pick::Slot;
    if (word == "step") return Pick::Step;
    return fallback;
}

// ------------------------------------------------------------- saying why

bool AutoLogin::worth_retrying(Why why) {
    switch (why) {
        case Why::LauncherFailed:
        case Why::LauncherNoWindow:
        case Why::GamePage:
        case Why::ClientStart:
        case Why::GameWindow:
        case Why::Connect:
        case Why::Channel:
        case Why::Character:
        case Why::World:
        // A window in the way may well be gone by the next attempt, which
        // brings VTCZone and the game to the front all over again.
        case Why::Covered:
            return true;
        default:
            return false;
    }
}

std::string AutoLogin::hidden_advice(const std::string& by) {
    if (by.empty())
        return "Cửa sổ GoBot cần xem đang bị thu nhỏ hoặc nằm ngoài màn hình, nên GoBot chưa "
               "nhìn thấy nó.";
    // The taskbar cannot be moved out of the way as a window can: what helps
    // is having it hide itself, or the window higher up.
    if (by == "taskbar")
        return "Thanh tác vụ (taskbar) của Windows đang che một phần cửa sổ GoBot cần xem, nên "
               "GoBot chưa nhìn thấy nó — hãy cho thanh tác vụ tự ẩn (chuột phải vào thanh tác "
               "vụ → Taskbar settings → Automatically hide the taskbar), hoặc kéo cửa sổ đó lên "
               "cao hơn.";
    const bool ours = lower(by).rfind("gobot", 0) == 0;
    const std::string who = ours ? std::string("Cửa sổ GoBot")
                          : by == "?" ? std::string("Một cửa sổ khác")
                                      : "Cửa sổ của " + by;
    return who + " đang che cửa sổ GoBot cần xem, nên GoBot chưa nhìn thấy nó — hãy dời cửa sổ "
                 "đó ra.";
}

std::string AutoLogin::advice(Why why, const std::string& detail) {
    // A number of seconds when the flow knows how long it waited, and the
    // usual wait when it does not.
    const auto seconds = [&](const char* usual) { return detail.empty() ? std::string(usual) : detail; };

    switch (why) {
        case Why::None:
            return {};
        case Why::Cancelled:
            return "Đã dừng.";
        case Why::NotSetUp:
            return "Màn hình " + detail + " chưa có hình nhận diện trong configs/login.ini — xem "
                   "Nâng cao.";
        case Why::NoPicture:
            return "Thiếu hình nhận diện màn hình " + detail + " — cài lại GoBot.";
        case Why::NoLauncher:
            return "Chưa ghi đường dẫn tới VTCZone (dòng launcher= trong configs/login.ini).";
        case Why::LauncherMissing:
            return "Không thấy VTCZone tại " + detail + ". Hãy mở VTCZone bằng tay, hoặc sửa dòng "
                   "launcher= trong configs/login.ini.";
        case Why::LauncherFailed:
            return "Không mở được VTCZone (" + detail + ").";
        case Why::LauncherNoWindow:
            return detail == "running"
                       ? "VTCZone đang chạy nhưng không thấy cửa sổ của nó (có thể đang ẩn "
                         "dưới khay hệ thống) — hãy mở cửa sổ VTCZone lên."
                       : "Đã mở VTCZone nhưng 30 giây không thấy cửa sổ của nó.";
        case Why::SignIn:
            return "VTCZone đang hiện màn hình đăng nhập. GoBot không đăng nhập thay bạn: hãy đăng "
                   "nhập VTCZone (chọn ghi nhớ nếu có) rồi bấm BẮT ĐẦU lại.";
        case Why::GamePage:
            return "Không thấy Ghost Online / nút Chơi game trong VTCZone sau " + seconds("60") +
                   " giây.";
        case Why::ClientStart:
            return "Launcher của game không hiện nút START sau " + seconds("90") +
                   " giây — có thể đang cập nhật hoặc có hộp Security Alert.";
        case Why::SecurityAlert:
            return "Launcher của game đang hiện hộp Security Alert. GoBot không tự bấm Yes: hãy tự "
                   "xem và trả lời hộp đó, rồi bấm BẮT ĐẦU lại.";
        case Why::GameWindow:
            return "Đã bấm START nhưng cửa sổ game không mở.";
        case Why::Connect:
            return "Không thấy nút Kết Nối trong game.";
        case Why::Channel:
            return "Không thấy danh sách kênh (Chọn Kênh).";
        case Why::Character:
            return "Không thấy màn hình Chọn Nhân Vật.";
        case Why::World:
            return "Đã chọn nhân vật nhưng 120 giây vẫn chưa vào game.";
        case Why::Covered: {
            // "Chưa bấm", not yet: the same words say what it is waiting on
            // while it waits, and why it stopped once it has. Nothing to click
            // yet, only to look at: hidden_advice.
            if (detail.empty())
                return "Chỗ GoBot cần bấm không nằm trên màn hình (cửa sổ bị thu nhỏ, hoặc bị kéo "
                       "ra ngoài màn hình?), nên GoBot chưa bấm.";
            if (detail == "taskbar")
                return "Thanh tác vụ (taskbar) của Windows đang che chỗ GoBot cần bấm, nên GoBot "
                       "chưa bấm — hãy cho thanh tác vụ tự ẩn, hoặc kéo cửa sổ VTCZone hay game "
                       "lên cao hơn.";
            const bool ours = lower(detail).rfind("gobot", 0) == 0;
            const std::string who = ours ? std::string("Cửa sổ GoBot")
                                  : detail == "?" ? std::string("Một cửa sổ khác")
                                                  : "Cửa sổ của " + detail;
            return who + " đang che chỗ GoBot cần bấm, nên GoBot chưa bấm — hãy để VTCZone và "
                         "game hiện rõ, không bị che.";
        }
        case Why::BadPoint: {
            // Which window and which screen, by the page's key, from the
            // table: the file says where on a page to click, not which
            // window the page is drawn on.
            std::string window = window_vi(On::Game);
            std::string screen = detail;
            for (const Page& p : default_pages())
                if (p.key == detail) {
                    window = window_vi(p.on);
                    screen = screen_vi(p.step);
                }
            return "Chỗ cần bấm ở màn hình " + screen + " nằm ngoài cửa sổ " + window +
                   " — hãy sửa click_dx/click_dy (hoặc row_dx, row_dy, row_step) của [Page." +
                   detail + "] trong configs/login.ini.";
        }
    }
    return {};
}

std::string AutoLogin::gave_up(int attempts, Why why, const std::string& detail) {
    std::string reason = advice(why, detail);
    if (!reason.empty() && reason.back() == '.') reason.pop_back();
    return "Đã thử " + std::to_string(attempts) + " lần không vào lại được: " + reason +
           ". Cần người kiểm tra.";
}

std::string AutoLogin::advice() const {
    if (worth_retrying(why_) && attempts_made_ > 1)
        return gave_up(attempts_made_, why_, why_detail_);
    return advice(why_, why_detail_);
}

// ---------------------------------------------------------------- the table

std::vector<AutoLogin::Page> AutoLogin::default_pages() {
    // Timings from a real crossing: the launcher takes about ten seconds to
    // show its window, the client another sixteen to appear after the start
    // button, and the world about twenty after the character is picked. The
    // waits here are the generous version of those.
    //
    // The pictures and the geometry are the ones that ship in
    // configs/login.ini, where each is explained and how it was measured. They
    // are here as well so that a missing file, or one with a page left out,
    // still crosses: the file says what to change, this says what works.
    auto page = [](Step step, const char* key, const char* picture, On on, int wait_s) {
        Page p;
        p.step    = step;
        p.key     = key;
        p.picture = picture;
        p.on      = on;
        p.wait_s  = wait_s;
        p.score   = 0.70f;
        return p;
    };

    std::vector<Page> out{
        page(Step::GameList,       "game_list",     "app_select_page",       On::Publisher, 60),
        page(Step::StartGame,      "start_game",    "start_game_page",       On::Publisher, 60),
        page(Step::ClientLauncher, "client_start",  "start_game_button",     On::Client,    90),
        page(Step::Connect,        "connect",       "game_ready_indicator",  On::Game,      90),
        page(Step::Channel,        "channel_list",  "channel_first_index",   On::Game,      60),
        // "Kết Nối" a second time. Picking a channel closes the list and puts
        // the same connect button back up; without this press the character
        // screen never comes, which showed as the flow waiting out its whole
        // minute on 'character' and then starting the crossing over. The
        // second attempt then worked -- because restarting is what supplied
        // the missing press.
        page(Step::ConnectAgain,   "connect_again", "game_ready_indicator",  On::Game,      60),
        page(Step::Character,      "character",     "select_char_indicator", On::Game,      60),
    };

    // The account's games and the channels are lists. The characters stand
    // side by side on a stage and are picked where they stand, then started
    // with a button in the corner -- two clicks, which is what Slot is.
    for (Page& p : out) {
        if (p.step == Step::GameList) p.pick = Pick::Row;
        if (p.step == Step::Channel) {
            // Row 1 is 140 below the "Chọn Kênh" banner and 54 in from its
            // left edge, on the "Kênh N" label; the eight rows are 31 apart.
            p.pick = Pick::Row;
            p.click_dx = 54;
            p.click_dy = 140;
            p.row_step = 31;
            p.row_across = false;
            p.row_count = 8;
        }
        if (p.step == Step::Character) {
            // Slot 1 is well left of and above "Bắt Đầu Game", which the
            // picture marks; the slots are 202 apart across the stage.
            p.pick = Pick::Slot;
            p.click_dx = 81;
            p.click_dy = 25;
            p.row_dx = -518;
            p.row_dy = -160;
            p.row_step = 202;
            p.row_across = true;
            p.row_count = 3;
            p.step_pause_ms = 1200;
        }
    }
    return out;
}

// ------------------------------------------------------------------ settings

bool AutoLogin::Settings::load(const std::string& path) {
    IniFile ini;
    if (!ini.load(path)) return false;

    // A score that is not a match score is not used. A comma for the decimal
    // point is the likely way to get one -- "0,80" reads as 0 -- and at 0 a
    // picture is found in every frame: a page would be clicked wherever the
    // best of nothing was, and the sign-in check would see the form in every
    // frame. Said once in the log, since this is read every frame on the tab.
    std::string bad_score;
    const auto score = [&](const std::string& section, const char* key, float usual) {
        const float v = ini.get_float(section, key, usual);
        if (v >= kMinScore && v <= 1.0f) return v;      // false for NaN too
        if (bad_score.empty()) bad_score = "[" + section + "] " + key + "=" + ini.get(section, key);
        return usual;
    };

    launcher_path   = ini.get("Login", "launcher", launcher_path);
    launcher_window = ini.get("Login", "launcher_window", launcher_window);
    client_window   = ini.get("Login", "client_window", client_window);
    client_class    = ini.get("Login", "client_class", client_class);
    client_process  = ini.get("Login", "client_process", client_process);
    game_window     = ini.get("Login", "game_window", game_window);
    game_class      = ini.get("Login", "game_class", game_class);
    game_process    = ini.get("Login", "game_process", game_process);
    signin_picture  = ini.get("Login", "signin_picture", signin_picture);
    signin_score    = score("Login", "signin_score", signin_score);

    // What an older GoBot kept for typing an account into VTCZone. Its values
    // are not looked at: GoBot no longer signs in to anything, and a file that
    // still holds them -- an encrypted password included -- is the player's,
    // so it is left exactly as it is rather than tidied. Only that they are
    // there is said, once, so somebody reading the log knows they do nothing.
    stale_keys = ini.has_key("Login", "username") || ini.has_key("Login", "password") ||
                 ini.has_section("Page.login_page");
    if (stale_keys) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
            GB_INFO("login.ini: the username=, password= and [Page.login_page] an older "
                    "GoBot kept are no longer used, and are ignored");
    }

    channel_index   = ini.get_int("Login", "channel_index", channel_index);
    character_index = ini.get_int("Login", "character_index", character_index);
    game_slot     = ini.get_int("Login", "game_slot", game_slot);
    attempts      = std::clamp(ini.get_int("Login", "attempts", attempts), 1, 5);

    heal_on_arrival = ini.get_bool("Login", "heal_on_arrival", heal_on_arrival);
    heal_hp_key = static_cast<std::uint32_t>(ini.get_int("Login", "heal_hp_key", 0));
    heal_mp_key = static_cast<std::uint32_t>(ini.get_int("Login", "heal_mp_key", 0));

    pages = default_pages();
    for (Page& page : pages) {
        const std::string section = "Page." + page.key;
        page.picture  = ini.get(section, "picture", page.picture);
        page.click_dx = ini.get_int(section, "click_dx", page.click_dx);
        page.click_dy = ini.get_int(section, "click_dy", page.click_dy);
        page.wait_s   = std::clamp(ini.get_int(section, "wait_s", page.wait_s), 1, kMaxWaitS);
        page.score    = score(section, "score", page.score);
        page.pick     = pick_from(ini.get(section, "pick", pick_name(page.pick)),
                                  page.pick);
        page.row_step = ini.get_int(section, "row_step", page.row_step);
        page.row_across = ini.get_bool(section, "row_across", page.row_across);
        page.row_dx   = ini.get_int(section, "row_dx", page.row_dx);
        page.row_dy   = ini.get_int(section, "row_dy", page.row_dy);
        page.row_count  = ini.get_int(section, "row_count", page.row_count);
        page.step_dx  = ini.get_int(section, "step_dx", page.step_dx);
        page.step_dy  = ini.get_int(section, "step_dy", page.step_dy);
        // Ten seconds at most: a pause between two clicks of one page that is
        // longer than that is a typo, and would hold the crossing for it.
        page.step_pause_ms = std::clamp(
            ini.get_int(section, "step_pause_ms", page.step_pause_ms), 100, 10000);
    }

    if (!bad_score.empty()) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
            GB_WARN("login.ini: %s is not a match score (%.1f to 1), so the usual one is used",
                    bad_score.c_str(), static_cast<double>(kMinScore));
    }
    return true;
}

bool AutoLogin::Settings::save(const std::string& path) const {
    IniFile ini;
    ini.set("Login", "launcher", launcher_path);
    ini.set("Login", "launcher_window", launcher_window);
    ini.set("Login", "client_window", client_window);
    ini.set("Login", "client_class", client_class);
    ini.set("Login", "client_process", client_process);
    ini.set("Login", "game_window", game_window);
    ini.set("Login", "game_class", game_class);
    ini.set("Login", "game_process", game_process);
    ini.set("Login", "signin_picture", signin_picture);
    ini.set_float("Login", "signin_score", signin_score);
    ini.set_int("Login", "channel_index", channel_index);
    ini.set_int("Login", "character_index", character_index);
    ini.set_int("Login", "game_slot", game_slot);
    ini.set_int("Login", "attempts", attempts);
    ini.set_bool("Login", "heal_on_arrival", heal_on_arrival);
    ini.set_int("Login", "heal_hp_key", static_cast<int>(heal_hp_key));
    ini.set_int("Login", "heal_mp_key", static_cast<int>(heal_mp_key));

    for (const Page& page : pages) {
        const std::string section = "Page." + page.key;
        ini.set(section, "picture", page.picture);
        ini.set_int(section, "click_dx", page.click_dx);
        ini.set_int(section, "click_dy", page.click_dy);
        ini.set_int(section, "wait_s", page.wait_s);
        ini.set_float(section, "score", page.score);
        ini.set(section, "pick", pick_name(page.pick));
        ini.set_int(section, "row_step", page.row_step);
        ini.set_bool(section, "row_across", page.row_across);
        ini.set_int(section, "row_dx", page.row_dx);
        ini.set_int(section, "row_dy", page.row_dy);
        ini.set_int(section, "row_count", page.row_count);
        ini.set_int(section, "step_dx", page.step_dx);
        ini.set_int(section, "step_dy", page.step_dy);
        ini.set_int(section, "step_pause_ms", page.step_pause_ms);
    }

    return ini.save(path,
        "GoBot auto-login\n"
        "\n"
        "The crossing from VTCZone to a character in the world. The sequence\n"
        "is fixed; what each page looks like is not, because those pages belong\n"
        "to the launchers and the client and differ between installs.\n"
        "\n"
        "VTCZone has to be signed in already -- sign in to it once, with its own\n"
        "remember option, and it stays that way. GoBot only presses Choi game.\n"
        "When VTCZone shows its sign-in form instead (signin_picture), the\n"
        "crossing stops and says so; nothing on that form is touched.\n"
        "\n"
        "Each [Page.*] names the picture that identifies it. Fill one in again\n"
        "by getting the launcher onto that page and running:\n"
        "\n"
        "  gobot_cli --window <launcher title> --tplsrc <picture set> --survey\n"
        "\n"
        "whereupon it prints what it found and where. Put the name here.\n"
        "\n"
        "click_dx/click_dy say where to click relative to the picture's top\n"
        "left corner; -1 for both means the middle of it. For the two pages\n"
        "that are a list -- the account's games and the channel list --\n"
        "row_step is how far apart the entries are, and the index is taken\n"
        "from game_slot and channel_index above.\n");
}

std::vector<const AutoLogin::Page*> AutoLogin::Settings::unidentified() const {
    std::vector<const Page*> out;
    for (const Page& page : pages)
        if (!page.identified()) out.push_back(&page);
    return out;
}

std::size_t AutoLogin::Settings::recognised(const TemplateStore& store) const {
    std::size_t n = 0;
    for (const Page& page : pages)
        if (page.identified() && store.get(page.picture) != nullptr) ++n;
    return n;
}

bool AutoLogin::Settings::launcher_found() const {
    // Wide, because the path is UTF-8 and an install folder with a Vietnamese
    // name in it is not something the ANSI call can find.
    if (launcher_path.empty()) return false;
    const int n = MultiByteToWideChar(CP_UTF8, 0, launcher_path.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, launcher_path.c_str(), -1, wide.data(), n);
    const DWORD attr = GetFileAttributesW(wide.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string AutoLogin::running_launcher(bool look, const std::string& program) {
    static std::mutex mutex;
    // What each program asked after was last seen running from, so that
    // asking after one -- a test's, which nothing runs -- never hands back
    // another's.
    static std::map<std::string, std::string> seen;
    const std::string key = lower(program);
    // Reading a running program's path, to adopt it: on 2026-10-15 a test's
    // empty launcher path became the running VTCZone's this way, and the
    // crossing it was in then drove the real desktop.
    if (look && refuse_real_input("read a running launcher's path, to adopt it")) return {};
    if (look) {
        const std::wstring want = utf8_to_wide(program);
        std::string now;
        HANDLE snap = want.empty() ? INVALID_HANDLE_VALUE
                                   : CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (Process32FirstW(snap, &entry)) {
                do {
                    if (_wcsicmp(entry.szExeFile, want.c_str()) != 0) continue;
                    HANDLE proc =
                        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                    if (proc == nullptr) continue;
                    wchar_t path[MAX_PATH * 2]{};
                    DWORD size = static_cast<DWORD>(std::size(path));
                    if (QueryFullProcessImageNameW(proc, 0, path, &size) && size > 0)
                        now = wide_to_utf8(std::wstring(path, size));
                    CloseHandle(proc);
                } while (now.empty() && Process32NextW(snap, &entry));
            }
            CloseHandle(snap);
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (!now.empty()) seen[key] = now;
    }
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = seen.find(key);
    return it == seen.end() ? std::string{} : it->second;
}

bool AutoLogin::Settings::adopt_running_launcher(bool look) {
    // A path that is there, or none at all, is kept whatever is running (see
    // adopt_launcher). Asked first, so that the switch refuses only a path
    // that would really be replaced.
    if (launcher_path.empty() || launcher_found()) return false;
    if (refuse_real_input("adopt a running VTCZone's path")) return false;
    return adopt_launcher(running_launcher(look));
}

bool AutoLogin::Settings::adopt_launcher(const std::string& there) {
    // Only when the file's path names nothing: a path that is there is the
    // player's choice, even with another VTCZone running -- and no path at
    // all says there is no launcher to start, which is not this to undo.
    if (launcher_path.empty() || launcher_found()) return false;
    if (there.empty() || there == launcher_path || !path_exists(there)) return false;
    static std::mutex said_mutex;
    static std::string said;
    {
        std::lock_guard<std::mutex> lock(said_mutex);
        if (said != there) {
            said = there;
            GB_INFO("AutoLogin: VTCZone is not at %s, which configs/login.ini names -- using the "
                    "running one's own path, %s, for this session",
                    launcher_path.c_str(), there.c_str());
        }
    }
    launcher_path = there;
    return true;
}

bool AutoLogin::Settings::file_missing(const std::string& path) {
    // Asked by the wide name, the way IniFile opens it, so the two cannot
    // disagree about which file is meant. Only "not found" counts: a file
    // that is there and cannot be looked at is still somebody's file.
    if (path.empty()) return false;
    if (path_exists(path)) return false;
    const DWORD err = GetLastError();
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

// ------------------------------------------------------------ what is seen

bool AutoLogin::load_pictures(TemplateStore& store, const std::string& name_map_ini,
                              const std::string& from) {
    // Asked for by name: a folder of PNGs or the other helper's assembly.
    if (!from.empty()) {
        const bool is_dll = from.size() > 4 &&
            _stricmp(from.c_str() + from.size() - 4, ".dll") == 0;
        if (is_dll && store.load_from_assembly(from, name_map_ini)) return true;
        if (!is_dll && store.load(from)) {
            store.apply_names(name_map_ini);
            return true;
        }
        GB_WARN("AutoLogin: no pictures from %s; using the best set on this machine",
                from.c_str());
    }

    // Otherwise the same set everything else in GoBot uses. The launcher's
    // pages were cut out of the other helper's assembly into
    // templates\grabbed -- the very same pixels -- so a machine without that
    // helper, which is every machine a package goes to, crosses as well.
    return store.load_best(name_map_ini);
}

TemplateStore AutoLogin::pictures_for(const Settings& settings, const TemplateStore& all) {
    TemplateStore out;
    for (const std::string& name : picture_names(settings))
        if (const Frame* picture = all.get(name)) out.put(name, *picture);
    return out;
}

std::vector<std::string> AutoLogin::picture_names(const Settings& settings) {
    std::vector<std::string> out;
    for (const Page& page : settings.pages)
        if (!page.picture.empty()) out.push_back(page.picture);
    if (!settings.signin_picture.empty()) out.push_back(settings.signin_picture);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

AutoLogin::Publisher AutoLogin::look_at_publisher(const Frame& vtczone,
                                                  const TemplateStore& store,
                                                  const Settings& settings) {
    if (!vtczone.valid()) return Publisher::Unknown;
    const ScreenRect all{ 0, 0, vtczone.width, vtczone.height };

    // Its own pages first. A launcher that shows the game, or "Chơi game", is
    // signed in, whatever else is drawn beside them -- so a blue button that
    // happens to look like the sign-in one never stops a crossing that can go.
    for (const Page& page : settings.pages) {
        if (page.on != On::Publisher) continue;
        const Frame* picture = store.get(page.picture);
        if (picture && TemplateMatcher::find(vtczone, all, *picture, page.score).found)
            return Publisher::Ready;
    }
    const Frame* signin = settings.signin_picture.empty() ? nullptr
                                                          : store.get(settings.signin_picture);
    if (signin && TemplateMatcher::find(vtczone, all, *signin, settings.signin_score).found)
        return Publisher::SignIn;
    return Publisher::Unknown;
}

bool AutoLogin::signin_settled(long long up_ms, long long started_ms) {
    if (up_ms < kSignInHoldMs) return false;
    // A VTCZone started a moment ago may be signing itself in behind the form.
    return started_ms < 0 || started_ms >= kSignInGraceMs;
}

bool AutoLogin::is_security_alert(const std::string& window_class, const std::string& title) {
    return lower(trimmed(window_class)) == "#32770" && lower(trimmed(title)) == "security alert";
}

void* AutoLogin::window_of(const Settings& settings, On which) {
    auto all = InputManager::enumerate_windows();
    if (which == On::Game) all = only_the_game(std::move(all));
    return best_window(all, spec_of(settings, which));
}

void* AutoLogin::minimised_window(const Settings& settings, const std::string& process) {
    // Of that program's windows at any size, the minimised ones, picked by the
    // same rule as a window that is up: a title on its own has to be exact.
    std::vector<InputManager::WindowInfo> down;
    for (InputManager::WindowInfo& w : InputManager::windows_of(process))
        if (IsIconic(static_cast<HWND>(w.hwnd))) down.push_back(std::move(w));
    return best_window(down, spec_of(settings, On::Publisher));
}

AutoLogin::Landing AutoLogin::landing(void* window, int x, int y, std::string* in_the_way) {
    if (in_the_way) in_the_way->clear();
    const HWND target = static_cast<HWND>(window);
    // Asked first: a minimised window has no client area to speak of, and
    // every point would read as off it -- which is not an offset gone wrong.
    // The flow brings a minimised window back; until then the click waits.
    // So does one with no size yet, still being laid out.
    if (target == nullptr || !IsWindow(target) || IsIconic(target)) return Landing::Minimised;
    RECT client{};
    if (!GetClientRect(target, &client) || client.right <= 0 || client.bottom <= 0)
        return Landing::Minimised;

    // On the window itself, not its frame or beside it. The points come from
    // pictures of its client area, so one outside that is an offset gone
    // wrong, and a click there is aimed at nothing of the window's.
    if (x < 0 || y < 0 || x >= client.right || y >= client.bottom) return Landing::OffClient;

    POINT at{ x, y };
    if (!ClientToScreen(target, &at)) return Landing::OffScreen;
    // Dragged part off the screen: a click there would go nowhere at all.
    if (MonitorFromPoint(at, MONITOR_DEFAULTTONULL) == nullptr) return Landing::OffScreen;

    // What a click there would actually reach. A child of the window -- a
    // button on the game's launcher, the view inside VTCZone's frame -- is
    // still the window; anything else, even another window of the same
    // program, is not.
    const HWND there = WindowFromPoint(at);
    const HWND top = there != nullptr ? GetAncestor(there, GA_ROOT) : nullptr;
    if (top != nullptr && top == GetAncestor(target, GA_ROOT)) return Landing::Lands;
    // The taskbar by what the player sees, as uncovered() calls it, rather
    // than as the explorer.exe behind it: its advice is its own.
    if (in_the_way)
        *in_the_way = top != nullptr && is_taskbar(top) ? std::string("taskbar") : program_of(top);
    return Landing::Covered;
}

bool AutoLogin::lies_over(const Box& drawn, const Box& client, const std::vector<Box>& screens,
                          bool taskbar) {
    for (const Box& screen : screens) {
        const Box seen = meet(drawn, screen);
        if (nowhere(seen)) continue;
        // All there is of a taskbar set to hide itself, on this screen: the
        // strip along the edge that brings it back. Over nothing anybody
        // reads -- the frame of a game reaching that edge loses two rows.
        if (taskbar && std::min(seen.right - seen.left, seen.bottom - seen.top) <= kHiddenBarPx)
            continue;
        if (!nowhere(meet(seen, client))) return true;
    }
    return false;
}

void* AutoLogin::over(void* window) {
    const HWND target = static_cast<HWND>(window);
    RECT client{};
    if (target == nullptr || !GetClientRect(target, &client)) return nullptr;
    POINT tl{ 0, 0 }, br{ client.right, client.bottom };
    if (!ClientToScreen(target, &tl) || !ClientToScreen(target, &br)) return nullptr;
    const Box area{ tl.x, tl.y, br.x, br.y };

    // Only what is on a screen. Past every screen's edge nothing is seen,
    // the game's frame included: a window kept on top there, or the rest of
    // a taskbar hidden past the bottom, lay over the part of a game taller
    // than the screen, and the crossing waited for it to move. With no
    // screen to be told of, the window itself stands in for one.
    std::vector<Box> screens;
    EnumDisplayMonitors(nullptr, nullptr, add_screen, reinterpret_cast<LPARAM>(&screens));
    if (screens.empty()) screens.push_back(area);

    // Every window above it, from the one just above to the top. The
    // window's own popups -- owned by it -- are its; anything else is not,
    // even another window of the same program.
    const HWND root = GetAncestor(target, GA_ROOT);
    const HWND owner = GetAncestor(root, GA_ROOTOWNER);
    int guard = 0;
    for (HWND w = GetWindow(root, GW_HWNDPREV); w != nullptr && guard < 4096;
         w = GetWindow(w, GW_HWNDPREV), ++guard) {
        if (!IsWindowVisible(w) || IsIconic(w)) continue;
        if (GetAncestor(w, GA_ROOTOWNER) == owner) continue;
        // Click-through overlays are passed over, as WindowFromPoint passes
        // them: a game's or a graphics driver's, drawn over every window.
        if (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TRANSPARENT) continue;
        // Nor what is not on this desktop: a window on another virtual
        // desktop, or a suspended app's, is "visible" and cloaked.
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked)
            continue;
        // Where it is drawn, not its rectangle: a window's invisible resizing
        // border reaches a few pixels past what is seen of it, and one put
        // beside the game is not over it.
        RECT r{};
        if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)) &&
            !GetWindowRect(w, &r))
            continue;
        if (lies_over(Box{ r.left, r.top, r.right, r.bottom }, area, screens, is_taskbar(w)))
            return w;
    }
    return nullptr;
}

bool AutoLogin::uncovered(void* window, std::string* in_the_way) {
    if (in_the_way) in_the_way->clear();
    RECT client{};
    if (window == nullptr || !GetClientRect(static_cast<HWND>(window), &client)) return false;
    // Anything over any part of it, not only over a few points: a window over
    // the strip where the connect page's button is -- far below every point
    // looked at -- left that page unseen in frames taken for the game's, and
    // a game waiting on Kết Nối was ended as stuck.
    if (IsIconic(static_cast<HWND>(window))) return false;
    if (void* const w = over(window)) {
        // Windows' taskbar is always on top, and a game window taller than
        // the screen less the taskbar has its bottom under it: called what
        // the player sees, not the program behind it.
        const HWND hw = static_cast<HWND>(w);
        if (in_the_way) *in_the_way = is_taskbar(hw) ? std::string("taskbar") : program_of(hw);
        return false;
    }
    const int w = static_cast<int>(client.right), h = static_cast<int>(client.bottom);
    const int at[][2] = { { w / 2, h / 2 },     { w / 4, h / 4 },     { 3 * w / 4, h / 4 },
                          { w / 4, 3 * h / 4 }, { 3 * w / 4, 3 * h / 4 } };
    // A window over a point, or the window minimised, and the picture is not
    // the window's. A point off every screen is neither: it reads black, and
    // no page or HUD is found in black -- counting it against the window
    // left a game dragged part off the screen never looked at, its wait run
    // out as "not found". Some of it has to be on a screen, though.
    bool seen = false;
    for (const auto& p : at) {
        std::string who;
        switch (landing(window, p[0], p[1], &who)) {
            case Landing::Lands:
                seen = true;
                break;
            case Landing::OffScreen:
                break;
            case Landing::Covered:
                if (in_the_way) *in_the_way = who;
                return false;
            default:
                return false;
        }
    }
    return seen;
}

bool AutoLogin::plate_at(const Frame& game, const Frame* landmark) {
    if (!game.valid() || landmark == nullptr || !landmark->valid()) return false;
    // Only where it sits, give or take a few pixels: the same plate found
    // anywhere else on the screen says nothing about the HUD. The area holds
    // every top-left corner from x-4..x+4 and y-4..y+4, the picture beside it.
    const int x0 = std::max(0, kLandmarkX - kLandmarkSlack);
    const int y0 = std::max(0, kLandmarkY - kLandmarkSlack);
    const ScreenRect look{ x0, y0, landmark->width + kLandmarkX + kLandmarkSlack - x0,
                           landmark->height + kLandmarkY + kLandmarkSlack - y0 };
    return TemplateMatcher::find(game, look, *landmark, kLandmarkScore).found;
}

bool AutoLogin::in_world(const Frame& game, const Frame* landmark) {
    if (!game.valid()) return false;
    if (SessionManager::hud_present(game)) return true;
    return plate_at(game, landmark);
}

std::size_t AutoLogin::furthest_up(const std::vector<Page>& pages, std::size_t first,
                                   const std::function<bool(std::size_t)>& up,
                                   const std::string& halt_at) {
    std::size_t halt = kNone;
    for (std::size_t i = 0; i < pages.size() && !halt_at.empty(); ++i)
        if (pages[i].key == halt_at) { halt = i; break; }

    std::size_t best = kNone;
    for (std::size_t i = first; i < pages.size(); ++i) {
        bool ambiguous = false;
        for (std::size_t j = 0; j < i && !ambiguous; ++j)
            ambiguous = pages[j].picture == pages[i].picture;
        if (ambiguous) continue;
        if (up(i)) best = i;            // the last one up, not the first
    }
    if (best != kNone && halt != kNone && best > halt) best = halt;
    return best;
}

std::size_t AutoLogin::first_allowed_page(const std::vector<Page>& pages, bool client_up,
                                          bool game_up) {
    // The windows come in the order the crossing meets them, so "a page of
    // the game's launcher or later" is a page whose window is at least that.
    const int from = game_up ? static_cast<int>(On::Game)
                   : client_up ? static_cast<int>(On::Client)
                               : static_cast<int>(On::Publisher);
    for (std::size_t i = 0; i < pages.size(); ++i)
        if (static_cast<int>(pages[i].on) >= from) return i;
    return pages.size();
}

std::size_t AutoLogin::resume_from(const std::vector<Page>& pages, bool client_up, bool game_up,
                                   const std::function<bool(std::size_t)>& up,
                                   const std::string& halt_at) {
    const std::size_t first = first_allowed_page(pages, client_up, game_up);
    const std::size_t best = furthest_up(pages, first, up, halt_at);
    // furthest_up holds a page beyond `halt_at` to it, and that can be one
    // that may not be touched now; the first that may is the answer then.
    return best != kNone && best >= first ? best : first;
}

// ------------------------------------------------------------------ the flow

AutoLogin::AutoLogin() = default;

void AutoLogin::set_settings(const Settings& s) { settings_ = s; }

void AutoLogin::say(Step s, const std::string& detail) {
    step_.store(s);
    GB_INFO("AutoLogin [%s] %s", step_name(s), detail.c_str());
    if (report_) report_(s, detail);
}

void AutoLogin::fail(Why why, const std::string& english, const std::string& detail) {
    why_ = why;
    trouble_ = english;
    why_detail_ = detail;
}

bool AutoLogin::stop(Why why, const std::string& english, const std::string& detail) {
    fail(why, english, detail);
    say(Step::Blocked, trouble_);
    return false;
}

bool AutoLogin::stop_now() {
    return cancelled_.load();
}

bool AutoLogin::nap(int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (stop_now()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return !stop_now();
}

// ------------------------------------------------- the desktop, or a pretend one

void* AutoLogin::find(On which) {
    return hands_.find ? hands_.find(which) : window_of(settings_, which);
}

void* AutoLogin::minimised_game() {
    if (hands_.find) return nullptr;
    const std::string program =
        settings_.game_process.empty() ? std::string(kGameProcess) : settings_.game_process;
    // Restored and brought forward once found, so only the game's own.
    std::vector<InputManager::WindowInfo> down;
    for (InputManager::WindowInfo& w : only_the_game(InputManager::windows_of(program)))
        if (IsIconic(static_cast<HWND>(w.hwnd))) down.push_back(std::move(w));
    return best_window(down, spec_of(settings_, On::Game));
}

bool AutoLogin::restore_game(Step during) {
    void* down = minimised_game();
    if (down == nullptr) return false;
    say(during, "the game is minimised -- restoring it");
    input_.set_target_window(down);
    aimed_at_ = On::Game;
    bring_forward();                    // restores a minimised window
    brought_forward_ = std::chrono::steady_clock::now();
    return nap(kSettleMs);
}

bool AutoLogin::bring_forward() {
    if (hands_.focus) return hands_.focus(input_.target_window());
    return input_.focus_window();
}

bool AutoLogin::running(const std::string& program) {
    if (program.empty()) return false;
    return hands_.running ? hands_.running(program) : SessionManager::process_running(program);
}

bool AutoLogin::game_running_now(const std::string& program) {
    if (program.empty()) return false;
    if (hands_.running) return hands_.running(program);
    // By where it runs from: another program called Game.exe running is no
    // reason to leave VTCZone alone, nor one to carry on from the game.
    return game_running(program, GameInstall::here());
}

void AutoLogin::put_vtczone_away() {
    // Once an attempt, and only once the game is up. VTCZone left over the
    // game hides it -- the game is read off the screen -- and GoC puts it
    // away at the same moment ("CheckGameClientOpen: Minimizing VTCZone").
    if (vtczone_put_away_) return;
    void* vtczone = find(On::Publisher);
    if (vtczone == nullptr) return;
    vtczone_put_away_ = true;
    GB_INFO("AutoLogin: the game's window is up -- minimising VTCZone, as GoC does");
    if (hands_.minimise) hands_.minimise(vtczone);
    else if (!refuse_real_input("minimise VTCZone"))
        ShowWindowAsync(static_cast<HWND>(vtczone), SW_SHOWMINNOACTIVE);
}

bool AutoLogin::grab(On which, void* hwnd, Frame& out, bool& trusted) {
    trusted = false;
    out.width = out.height = 0;
    if (hwnd == nullptr) return false;
    if (hands_.grab) {
        if (!hands_.grab(which, hwnd, out) || !out.valid()) return false;
        trusted = true;
        return true;
    }
    ScreenCapture& capture = capture_[static_cast<int>(which)];
    if (!capture.grab(hwnd, out) || !out.valid()) return false;
    // A window that draws itself is seen as it is, whatever lies over it.
    // One read off the screen -- the game, always, on this client -- is
    // whatever is in front of it there, so it is the window only while the
    // window is in front with nothing over it. VTCZone over a half-started
    // game hid the game's page that way, and the crossing went back to
    // VTCZone's "Chơi game" instead.
    const HWND front = GetForegroundWindow();
    std::string over;
    trusted = !capture.from_screen() || (front == static_cast<HWND>(hwnd) && uncovered(hwnd, &over));
    // What to name when it is not: the window in front, or the one over it.
    behind_by_[static_cast<int>(which)] = trusted                         ? std::string{}
                                          : front != static_cast<HWND>(hwnd) ? program_of(front)
                                                                            : over;
    return true;
}

AutoLogin::Ahead AutoLogin::ahead_now() {
    Ahead a;
    a.game = find(On::Game);
    a.client = find(On::Client);
    const std::string game_program =
        settings_.game_process.empty() ? std::string(kGameProcess) : settings_.game_process;
    a.game_up = a.game != nullptr || game_running_now(game_program);
    a.client_up = a.client != nullptr || running(settings_.client_process);
    return a;
}

bool AutoLogin::game_page_in(const Frame& frame) const {
    if (!frame.valid() || store_ == nullptr) return false;
    for (const Page& page : settings_.pages) {
        if (page.on != On::Game) continue;
        const Frame* picture = store_->get(page.picture);
        if (picture && TemplateMatcher::find(frame, ScreenRect{ 0, 0, frame.width, frame.height },
                                             *picture, page.score).found)
            return true;
    }
    return false;
}

const Frame* AutoLogin::landmark() {
    // The landmark is loaded the first time it is wanted, from the same
    // places as every other picture: one small file, not the whole set.
    if (!landmark_loaded_) {
        landmark_loaded_ = true;
        std::string from;
        if (TemplateStore::load_one("hpmp_indicator2", exe_relative("configs/templates.ini"),
                                    landmark_, &from))
            GB_INFO("AutoLogin: the world is told by the HUD's bars and its landmark, from %s",
                    from.c_str());
        else
            GB_WARN("AutoLogin: no picture of the HUD's landmark (hpmp_indicator2) anywhere -- "
                    "the world is told by the HP and MP bars alone");
    }
    return landmark_.valid() ? &landmark_ : nullptr;
}

bool AutoLogin::world_in(const Frame& frame) { return in_world(frame, landmark()); }

bool AutoLogin::world_on(void* hwnd, const Frame& frame, bool trusted) {
    if (trusted) return world_in(frame);
    // Something over another part of the game -- an always-on-top window
    // over its middle, say -- and the picture cannot be trusted as a whole.
    // Its top-left corner is still the game's if nothing is over that, and
    // the landmark sits there: before, a character that had reached the
    // world behind such a window was never seen to, and every attempt ran
    // out on it.
    const Frame* plate = landmark();
    return plate != nullptr && plate_clear(hwnd) && plate_at(frame, plate);
}

bool AutoLogin::plate_clear(void* hwnd) {
    if (hands_.grab) return false;       // a pretend game's pictures are always trusted
    const Frame* plate = landmark();
    const int w = plate != nullptr ? plate->width : 36, h = plate != nullptr ? plate->height : 36;
    // The corners and the middle of everywhere the landmark is looked for.
    const int x0 = std::max(0, kLandmarkX - kLandmarkSlack);
    const int y0 = std::max(0, kLandmarkY - kLandmarkSlack);
    const int x1 = kLandmarkX + kLandmarkSlack + w - 1, y1 = kLandmarkY + kLandmarkSlack + h - 1;
    const int at[][2] = { { x0, y0 }, { x1, y0 }, { x0, y1 }, { x1, y1 },
                          { (x0 + x1) / 2, (y0 + y1) / 2 } };
    for (const auto& p : at)
        if (landing(hwnd, p[0], p[1]) != Landing::Lands) return false;
    return true;
}

void AutoLogin::note_hidden(Step during, On which, bool hidden) {
    if (!hidden) {
        if (hidden_) {
            hidden_ = false;
            holding_ = false;
            held_by_.clear();
            // Said, so that whoever shows the steps -- the keeper's card --
            // stops naming a window that has moved away.
            say(during, std::string(on_name(which)) + " can be seen again");
        }
        return;
    }
    const std::string& by = behind_by_[static_cast<int>(which)];
    if (hidden_ && holding_ && held_by_ == by) return;        // said already
    hidden_ = true;
    holding_ = true;
    held_by_ = by;
    held_where_ = by.empty() ? Landing::Minimised : Landing::Covered;
    say(during, std::string(on_name(which)) +
                    (by.empty() ? " cannot be seen -- minimised, or off the screen"
                                : " is behind a window of " + by) +
                    ", so what is on it cannot be told -- waiting for it to be clear");
}

AutoLogin::FirstLook AutoLogin::first_look(const Ahead& ahead) {
    if (ahead.game != nullptr) {
        input_.set_target_window(ahead.game);
        aimed_at_ = On::Game;
        // In front, a moment to draw, then looked at -- a few times if
        // something is still over it, since a picture read off the screen is
        // only the game's while it is in front and clear. Two pictures it can
        // trust, half a second apart, before it is taken for a game that is
        // not in the world: a HUD half drawn, or hidden for a moment, would
        // otherwise send `gobot_cli --login` off to look for Kết Nối.
        int looked = 0;
        for (int i = 0; i < 3 && looked < 2; ++i) {
            if (looked == 0) {
                bring_forward();
                brought_forward_ = std::chrono::steady_clock::now();
                if (!nap(kSettleMs)) return FirstLook::Stopped;
            } else if (!nap(500)) {
                return FirstLook::Stopped;
            }
            Frame frame;
            bool trusted = false;
            if (grab(On::Game, ahead.game, frame, trusted)) {
                if (world_on(ahead.game, frame, trusted)) return FirstLook::InWorld;
                if (trusted) ++looked;
            }
        }
        put_vtczone_away();
        return FirstLook::Game;
    }
    if (ahead.client != nullptr) {
        input_.set_target_window(ahead.client);
        aimed_at_ = On::Client;
        bring_forward();
        brought_forward_ = std::chrono::steady_clock::now();
        if (!nap(kSettleMs)) return FirstLook::Stopped;
        return FirstLook::Client;
    }
    return FirstLook::Nothing;
}

AutoLogin::GameNow AutoLogin::judge_game() {
    const Ahead ahead = ahead_now();
    if (!ahead.game_up) return GameNow::Gone;
    // Up all this time without a window: a game that never got as far as
    // showing one, which GoC ends the same way. One minimised to the taskbar
    // has a window all right, and nothing can be said of what is on it.
    if (ahead.game == nullptr)
        return minimised_game() != nullptr ? GameNow::Unknown : GameNow::Stuck;

    input_.set_target_window(ahead.game);
    aimed_at_ = On::Game;
    bring_forward();
    brought_forward_ = std::chrono::steady_clock::now();
    if (!nap(kSettleMs)) return GameNow::Unknown;
    // Seen to be stuck, not guessed. What follows can end the client, so it
    // takes three looks half a second apart, and is stuck only when at least
    // two of them could be trusted to be the game's and none of them showed
    // the world or a page: one picture caught with the HUD half hidden is
    // not a game nobody can place. A picture that cannot be trusted says
    // nothing about the game either way -- but for the landmark in its own
    // corner, when nothing is over that.
    int trusted_looks = 0;
    for (int i = 0; i < kJudgeLooks; ++i) {
        if (i > 0 && !nap(kJudgeGapMs)) return GameNow::Unknown;
        Frame frame;
        bool trusted = false;
        if (!grab(On::Game, ahead.game, frame, trusted)) continue;
        if (world_on(ahead.game, frame, trusted)) return GameNow::InWorld;
        if (!trusted) continue;
        ++trusted_looks;
        if (game_page_in(frame)) return GameNow::OnPage;
    }
    return trusted_looks >= 2 ? GameNow::Stuck : GameNow::Unknown;
}

bool AutoLogin::security_alert_up() const {
    for (const auto& w : InputManager::windows_of(settings_.client_process))
        if (is_security_alert(w.window_class, w.title)) return true;
    return false;
}

bool AutoLogin::stop_for_alert() {
    // Not pressed, ever. GoC answers Yes to it; whether to trust a site's
    // certificate is the player's question, not this program's.
    if (!security_alert_up()) return false;
    stop(Why::SecurityAlert,
         "the game's launcher is showing a Security Alert box. It is not answered "
         "here -- a person has to look at it");
    return true;
}

AutoLogin::Click AutoLogin::click(int x, int y) {
    // A stop asked for since the page was found: nothing at all -- not even
    // the window brought forward, which would pull it in front of whatever
    // the player has just turned to.
    if (stop_now()) return Click::Stopped;

    // In front first. A click goes to whatever window is under the pointer,
    // and the page having been found says nothing about that: a window draws
    // its picture of itself just the same with something lying over it. A
    // player clicking back into GoBot, or a chat popping up over VTCZone,
    // would otherwise get the click.
    const bool in_front = bring_forward();

    // Looked for again at the very moment of clicking, not only once a poll:
    // the box can come up while a page is being found, and a click that
    // lands on it is an answer to it.
    if (stop_for_alert()) return Click::Stopped;

    std::string other;
    Landing where = landing(input_.target_window(), x, y, &other);
    // Off the window's client area, wherever the window is: no window in the
    // way is to blame and none moving will help -- the offset is wrong.
    if (where == Landing::OffClient) return Click::BadPoint;
    if (in_front && where == Landing::Lands) {
        // Asked once more at the last moment: bringing the window forward
        // and the looks above take a while, and a stop pressed during them
        // means no click.
        if (stop_now()) return Click::Stopped;
        holding_ = false;
        hidden_ = false;
        held_by_.clear();
        held_where_ = Landing::Lands;
        if (hands_.click) hands_.click(x, y);
        else input_.click_at(x, y);
        return Click::Done;
    }
    // Not in front, though asked: whatever is in front is what is in the way.
    if (!in_front && (where == Landing::Lands || where == Landing::Covered)) {
        where = Landing::Covered;
        other = program_of(GetForegroundWindow());
    }
    holding_ = true;
    held_by_ = other;
    held_where_ = where;
    return Click::Held;
}

void AutoLogin::look_around(Frame (&frames)[3], bool (&behind)[3]) {
    const auto all = hands_.find ? std::vector<InputManager::WindowInfo>{}
                                 : InputManager::enumerate_windows();
    // The game's window as window_of finds it: the game's own program only.
    const auto game_only = hands_.find ? std::vector<InputManager::WindowInfo>{}
                                       : only_the_game(all);
    for (int k = 0; k < 3; ++k) {
        const On w = static_cast<On>(k);
        frames[k].width = frames[k].height = 0;     // not up unless grabbed below
        behind[k] = false;
        void* hwnd = (w == aimed_at_ && input_.window_valid()) ? input_.target_window()
                   : hands_.find                                ? hands_.find(w)
                   : best_window(w == On::Game ? game_only : all, spec_of(settings_, w));
        bool trusted = false;
        if (grab(w, hwnd, frames[k], trusted) && !trusted) {
            // Read off the screen with something in front of it: whatever
            // that is, it is not this window, and no page is looked for in it.
            frames[k].width = frames[k].height = 0;
            behind[k] = true;
        }
    }
}

bool AutoLogin::up_anywhere(std::size_t index, const Frame (&frames)[3]) const {
    // In any of the three pictures look_around kept, not only its own
    // window's: a picture read off the screen while its window is in front
    // can still take in a corner of another window lying over it.
    const Page& page = settings_.pages[index];
    const Frame* picture = store_ ? store_->get(page.picture) : nullptr;
    if (!picture) return false;
    for (const Frame& f : frames) {
        if (!f.valid()) continue;
        if (TemplateMatcher::find(f, ScreenRect{ 0, 0, f.width, f.height }, *picture,
                                  page.score).found)
            return true;
    }
    return false;
}

AutoLogin::Seen AutoLogin::await(std::size_t index, ScreenRect& found, std::size_t& later) {
    const Page& page = settings_.pages[index];
    const Frame* picture = store_->get(page.picture);     // run() checked it is there

    // The window this page is drawn on may not be up yet: the game's launcher
    // comes some seconds after "Chơi game", the game a quarter of a minute
    // after START. Waiting for the window gets a minute of its own. Held to
    // the most a page is waited for, so a wait set by hand cannot overflow.
    bool attached = aimed_at_ == page.on && input_.window_valid();
    const int budget_s = std::clamp(page.wait_s, 1, kMaxWaitS) + (attached ? 0 : 60);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(budget_s);
    bool window_seen = attached;
    bool restored = false;
    bool said_behind = false;
    Frame frames[3];
    bool behind[3] = { false, false, false };

    // When VTCZone was first seen on its sign-in form, this time round.
    std::chrono::steady_clock::time_point signin_since{};
    bool signin_up = false;

    for (;;) {
        if (stop_now()) return Seen::Stopped;

        // Looked for on every page, not only the launcher's own: GoC meets
        // it while waiting for that launcher to close after START, too.
        if (stop_for_alert()) return Seen::Stopped;

        // Nothing of VTCZone's while the game or its launcher is open, and
        // nothing of the launcher's while the game is: not waited for, and so
        // never clicked. The game coming up part way through -- started by
        // hand, or by the attempt before -- moves the wait on to its pages.
        const Ahead ahead_win = ahead_now();
        const std::size_t first =
            first_allowed_page(settings_.pages, ahead_win.client_up, ahead_win.game_up);
        if (index < first) {
            later = first;
            return Seen::Barred;
        }
        if (ahead_win.game != nullptr) put_vtczone_away();

        // Attach to the window this page is drawn on, once it is there.
        //
        // Per page, not at a fixed step. The old rule switched windows on
        // reaching the client's launcher and the connect button, which is
        // right only when the flow walks the whole crossing: resuming into the
        // middle left it looking for a page of the game on the publisher's
        // launcher, where it waited out the whole minute and started over.
        if (!attached || !input_.window_valid()) {
            attached = false;
            void* hwnd = find(page.on);
            // The game minimised before it was ever attached to: brought
            // back once, as an attached window that is minimised is below.
            if (hwnd == nullptr && page.on == On::Game && !restored) {
                restored = restore_game(page.step);
                if (stop_now()) return Seen::Stopped;
                if (restored) hwnd = find(page.on);
            }
            if (hwnd != nullptr) {
                input_.set_target_window(hwnd);
                aimed_at_ = page.on;
                attached = window_seen = true;
                say(page.step, std::string("switching to ") + on_name(page.on));
                bring_forward();
                brought_forward_ = std::chrono::steady_clock::now();
                if (!nap(kSettleMs)) return Seen::Stopped;
            }
        }

        // Minimised after it was found: nothing of it can be seen, so its page
        // would never be found. It is brought back once, as a minimised
        // VTCZone is when an attempt starts; one that will not come back is
        // left to the wait running out.
        if (attached && !restored && IsIconic(static_cast<HWND>(input_.target_window()))) {
            restored = true;
            say(page.step, std::string(on_name(page.on)) + " is minimised -- restoring it");
            bring_forward();
            if (!nap(kSettleMs)) return Seen::Stopped;
        }

        look_around(frames, behind);

        // Its window is read off the screen and something is in front of it,
        // so what is there is not its picture. Brought to the front and
        // looked at again -- every few seconds at most, as GoC defers a stage
        // until the game has the focus, rather than fighting a person using
        // the desktop for it.
        const int own = static_cast<int>(page.on);
        if (attached && behind[own] &&
            std::chrono::steady_clock::now() - brought_forward_ >=
                std::chrono::seconds(kForwardEveryS)) {
            if (!said_behind)
                say(page.step, std::string(on_name(page.on)) +
                                   " is behind another window -- bringing it to the front to "
                                   "look at it");
            said_behind = true;
            bring_forward();
            brought_forward_ = std::chrono::steady_clock::now();
            if (!nap(kSettleMs)) return Seen::Stopped;
            look_around(frames, behind);
        }
        // Still not to be trusted: what is over it is named while this waits
        // -- on the screen, as a held click's is -- not only once the wait has
        // run out with nothing found.
        note_hidden(page.step, page.on, attached && behind[own]);

        // Further on already? The furthest page that is up wins, as it does
        // when an attempt starts -- so a launcher that is already showing
        // "Chơi game", or a client already on its channel list, costs nothing,
        // and a client whose START is up is not given a second one by pressing
        // "Chơi game" again.
        const std::size_t ahead = furthest_up(
            settings_.pages, std::max(index + 1, first),
            [&](std::size_t j) { return up_anywhere(j, frames); }, halt_at_);
        if (ahead != kNone && ahead > index) {
            later = ahead;
            return Seen::Later;
        }

        if (attached) {
            // Only a picture that is the window's: look_around left out one
            // read off the screen with something in front of it, so a blue
            // button on some other window is not VTCZone signed out either.
            const Frame& mine = frames[own];
            if (mine.valid()) {
                const auto hit = TemplateMatcher::find(
                    mine, ScreenRect{ 0, 0, mine.width, mine.height }, *picture, page.score);
                if (hit.found) {
                    found = hit.at;
                    return Seen::Page;
                }
                // Signed out. Waiting will not change that and another attempt
                // will not either; nothing on the form is clicked or typed in.
                //
                // Only once it has stayed up a few seconds -- and, for a
                // VTCZone this flow has just started, not in its first half
                // minute. One that remembers its account may put the form up
                // while it signs itself in, and stopping on that would stop a
                // crossing that was about to go through.
                const bool form = page.on == On::Publisher &&
                                  look_at_publisher(mine, *store_, settings_) == Publisher::SignIn;
                const auto now = std::chrono::steady_clock::now();
                if (form && !signin_up) signin_since = now;
                signin_up = form;
                const auto ms = [](std::chrono::steady_clock::duration d) {
                    return static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
                };
                if (form && signin_settled(ms(now - signin_since),
                                           started_vtczone_ ? ms(now - vtczone_started_) : -1)) {
                    stop(Why::SignIn,
                         "VTCZone is showing its sign-in form. GoBot does not sign in to it: "
                         "sign in to VTCZone once, with its remember option, and start again");
                    return Seen::Stopped;
                }
            }
        }

        if (std::chrono::steady_clock::now() >= until) {
            if (!window_seen) {
                const Spec s = spec_of(settings_, page.on);
                std::string which = s.title->empty() ? std::string{} : "'" + *s.title + "'";
                if (!s.klass->empty())
                    which += (which.empty() ? "" : " ") + ("class " + *s.klass);
                if (!s.exe->empty())
                    which += (which.empty() ? "" : " ") + ("from " + *s.exe);
                fail(why_not_seen(page, false), which + " never appeared",
                     std::to_string(budget_s));
            } else if (attached && behind[own]) {
                // Never seen, because never looked at: say what was in the
                // way, not that the page did not come.
                const std::string& by = behind_by_[own];
                fail(Why::Covered,
                     "waited " + std::to_string(budget_s) + "s for '" + page.key + "', and " +
                         on_name(page.on) + " could not be looked at: " +
                         (by.empty() ? std::string("it is minimised, or off the screen")
                                     : "a window of " + by + " was over it"),
                     by);
            } else {
                fail(why_not_seen(page, true),
                     "waited " + std::to_string(budget_s) + "s for '" + page.key +
                         "' and it never appeared",
                     std::to_string(budget_s));
            }
            note_hidden(page.step, page.on, false);
            return Seen::TimedOut;
        }
        if (!nap(900)) return Seen::Stopped;
    }
}

bool AutoLogin::run() {
    // A cancel is not cleared here: one that arrives before the crossing gets
    // going -- the keeper being stopped in the moment it starts one -- still
    // stops it. A flow is made for one crossing.
    trouble_.clear();
    why_ = Why::None;
    why_detail_.clear();
    attempts_made_ = 0;
    aimed_at_ = On::Publisher;
    holding_ = false;
    hidden_ = false;
    held_by_.clear();
    held_where_ = Landing::Lands;
    started_vtczone_ = false;
    vtczone_put_away_ = false;

    if (cross()) return true;

    // Interrupted rather than defeated: say which.
    if (cancelled_.load()) fail(Why::Cancelled, "cancelled");
    return false;
}

bool AutoLogin::cross() {
    // Refuse before touching anything, rather than halfway through. A flow
    // that starts the launcher and then discovers it cannot read page three
    // has left a window open and a person to close it.
    if (stop_now()) return false;
    const auto missing = settings_.unidentified();
    if (!missing.empty()) {
        std::string list;
        for (const Page* page : missing) {
            if (!list.empty()) list += ", ";
            list += page->key;
        }
        return stop(Why::NotSetUp,
                    "these pages have no picture named for them yet: " + list +
                        ".  Get the launcher onto each and run --survey to find out "
                        "what marks it, then write the name into configs/login.ini.",
                    list);
    }
    for (const Page& page : settings_.pages) {
        if (store_ == nullptr || store_->get(page.picture) == nullptr)
            return stop(Why::NoPicture,
                        "there is no picture called '" + page.picture + "' -- the pictures "
                        "beside GoBot are missing or out of date",
                        page.picture);
    }
    if (settings_.launcher_path.empty())
        return stop(Why::NoLauncher, "no launcher is configured");
    // VTCZone installed somewhere login.ini does not say: where the running
    // one is, remembered, in case it has to be started again later. Only once
    // nothing above has refused -- a refusal touches no window -- and not for
    // a pretend game, whose programs are not the machine's.
    if (!hands_.running) settings_.adopt_running_launcher();
    if (!settings_.signin_picture.empty() && store_->get(settings_.signin_picture) == nullptr)
        GB_WARN("AutoLogin: there is no picture called '%s', so VTCZone's sign-in form "
                "cannot be told from a page that is slow to come",
                settings_.signin_picture.c_str());

    // Where a person asked the crossing to stop, by index, or kNone.
    std::size_t halt = kNone;
    for (std::size_t i = 0; i < settings_.pages.size() && !halt_at_.empty(); ++i)
        if (settings_.pages[i].key == halt_at_) halt = i;
    // A page further on than that one is the crossing already past it, with
    // nothing to do: it is not walked back to, and not crossed beyond.
    const auto past_halt = [&](std::size_t i) {
        if (halt == kNone || i <= halt) return false;
        say(settings_.pages[i].step, "already past '" + halt_at_ + "' -- the client is at '" +
                                         settings_.pages[i].key + "', so nothing is done");
        return true;
    };

    for (int attempt = 1; attempt <= settings_.attempts; ++attempt) {
        if (stop_now()) return false;
        attempts_made_ = attempt;
        holding_ = false;
        hidden_ = false;
        held_by_.clear();
        vtczone_put_away_ = false;
        GB_INFO("AutoLogin: attempt %d of %d", attempt, settings_.attempts);

        // ---- the game and its launcher, before VTCZone is touched ----
        //
        // A game part way in is carried on from where it is. VTCZone brought
        // up in front of it would hide it -- the game is read off the screen
        // -- while VTCZone's own picture of itself still showed "Chơi game"
        // to press, which starts a second launcher for the account. GoC never
        // goes back to VTCZone while a client is open either. And a game
        // already in the world needs nothing at all: that also keeps
        // `gobot_cli --login` from crossing into a game that is playing.
        Ahead ahead = ahead_now();
        // A game minimised to the taskbar is still a game that is up: it is
        // brought back and looked at, not taken for one with no window.
        if (ahead.game == nullptr && ahead.game_up) {
            const bool restored = restore_game(Step::GameWindow);
            if (stop_now()) return false;
            if (restored) ahead = ahead_now();
        }
        const FirstLook first_seen = first_look(ahead);
        if (first_seen == FirstLook::Stopped) return false;
        if (first_seen == FirstLook::InWorld) {
            say(Step::InWorld, "already in the world -- nothing to cross, and nothing clicked");
            return true;
        }

        if (!ahead.game_up && !ahead.client_up) {
            // ---- the publisher's launcher ----
            say(Step::StartLauncher, settings_.launcher_path);
            const bool was_running = running(kPublisherProcess);
            if (!was_running) {
                // Seen running earlier, somewhere else than the file says:
                // started from there.
                if (!hands_.running) settings_.adopt_running_launcher(false);
                // A path that is wrong now is wrong on the next attempt as well.
                if (!settings_.launcher_found())
                    return stop(Why::LauncherMissing,
                                settings_.launcher_path + " does not exist, and VTCZone is not "
                                "running",
                                settings_.launcher_path);
                const bool started = hands_.start ? hands_.start(settings_.launcher_path)
                                                  : SessionManager::start_program(settings_.launcher_path);
                if (!started) {
                    fail(Why::LauncherFailed, "could not start " + settings_.launcher_path,
                         settings_.launcher_path);
                    GB_WARN("AutoLogin: attempt %d -- %s", attempt, trouble_.c_str());
                    if (attempt < settings_.attempts && !nap(5000)) return false;
                    continue;
                }
                // Counted from here, for the sign-in form's grace: a VTCZone
                // that has just started may show it while it signs itself in.
                started_vtczone_ = true;
                vtczone_started_ = std::chrono::steady_clock::now();
            }

            say(Step::FindLauncher, settings_.launcher_window);
            void* vtczone = nullptr;
            bool restored = false;
            for (int i = 0; i < 30 && vtczone == nullptr; ++i) {
                vtczone = find(On::Publisher);
                // Minimised to the taskbar. GoC brings VTCZone back before it
                // clicks, and so does this: before, a minimised VTCZone was a
                // window that was never found, and every attempt failed
                // waiting for it while it sat on the taskbar. Once an attempt
                // -- one that will not come back is a person's to look at.
                if (vtczone == nullptr && !restored) {
                    void* down = hands_.find ? nullptr
                                             : minimised_window(settings_, kPublisherProcess);
                    if (down != nullptr) {
                        restored = true;
                        say(Step::FindLauncher, "VTCZone is minimised -- restoring it");
                        input_.set_target_window(down);
                        aimed_at_ = On::Publisher;
                        bring_forward();                // restores a minimised window
                        if (!nap(1000)) return false;
                        continue;                       // and looks again
                    }
                }
                if (vtczone == nullptr && !nap(1000)) return false;
            }
            if (vtczone == nullptr) {
                // Running with no window is VTCZone put away in the tray, which
                // a person has to bring back; started and no window is a slow
                // start.
                fail(Why::LauncherNoWindow,
                     was_running ? "VTCZone is running and has no window on screen -- is it "
                                   "in the tray?"
                                 : "the launcher's window never appeared",
                     was_running ? "running" : "started");
                GB_WARN("AutoLogin: attempt %d -- %s", attempt, trouble_.c_str());
                if (attempt < settings_.attempts && !nap(5000)) return false;
                continue;
            }
            input_.set_target_window(vtczone);
            aimed_at_ = On::Publisher;
            bring_forward();
            if (!nap(1200)) return false;
        }

        // ---- the pages, in order, starting from wherever it already is ----
        //
        // A launcher that is already open is usually part way through: on the
        // game list, or waiting on the start button. Walking from the first
        // page would then mean waiting a minute for a page that will never
        // come, and failing. So before starting, look at all three windows and
        // begin at the last page that is visible -- the crossing passes
        // through three of them and the later pages are not drawn on the
        // first, and a client already sitting on its own START button must
        // not get "Chơi game" pressed again, which puts a second launcher on
        // top of the first. Only among the pages that may be touched now:
        // with the game up, VTCZone's "Chơi game" is never one of them, even
        // when VTCZone's picture of itself shows it.
        std::size_t from = 0;
        {
            Frame frames[3];
            bool behind[3];
            look_around(frames, behind);
            GB_INFO("AutoLogin: %s %s, %s %s, %s %s",
                    on_name(On::Publisher), frames[0].valid() ? "up" : "not up",
                    on_name(On::Client), frames[1].valid() ? "up" : "not up",
                    on_name(On::Game), frames[2].valid() ? "up" : "not up");
            const Ahead now_up = ahead_now();
            from = resume_from(settings_.pages, now_up.client_up, now_up.game_up,
                               [&](std::size_t j) { return up_anywhere(j, frames); },
                               halt_at_);
            if (past_halt(from)) return true;
            if (from > 0 && from < settings_.pages.size())
                say(settings_.pages[from].step,
                    "already as far as '" + settings_.pages[from].key +
                    "', so the " + std::to_string(from) + " page(s) before it are skipped");
        }

        bool crossed = true;
        // The attempt ended on a page of the game, or on the way from the
        // last one into the world: the game may be up and stuck.
        bool failed_on_game = false;
        std::size_t index = from;
        // A click held back: on which page, since when, and what was last said
        // to be in the way -- said when it changes, not on every look.
        std::size_t held_page = kNone;
        std::chrono::steady_clock::time_point held_since{};
        std::string held_said;
        while (index < settings_.pages.size()) {
            const Page& page = settings_.pages[index];
            if (stop_now()) return false;
            // Gone on to another page: whatever held the last one back is past.
            if (held_page != index) {
                held_page = kNone;
                holding_ = false;
                hidden_ = false;
                held_by_.clear();
            }

            say(page.step, "looking for '" + page.picture + "'");
            ScreenRect at;
            std::size_t later = kNone;
            const Seen seen = await(index, at, later);
            if (seen == Seen::Stopped) return false;
            if (seen == Seen::TimedOut) {
                crossed = false;
                failed_on_game = page.on == On::Game;
                break;
            }
            if (seen == Seen::Later || seen == Seen::Barred) {
                if (later >= settings_.pages.size()) break;
                if (past_halt(later)) return true;
                say(settings_.pages[later].step,
                    seen == Seen::Later
                        ? "'" + settings_.pages[later].key + "' is up already, so the flow "
                          "goes on from there"
                        : std::string("the game or its launcher is open, so nothing of ") +
                              on_name(page.on) + " is pressed -- going on from '" +
                              settings_.pages[later].key + "'");
                index = later;
                continue;
            }

            // Asked to stop here. The page is up and found; leave it that way.
            if (!halt_at_.empty() && page.key == halt_at_) {
                say(page.step, "stopping here as asked -- '" + page.key +
                               "' is up at " + std::to_string(at.x) + "," +
                               std::to_string(at.y) + " " + std::to_string(at.w) +
                               "x" + std::to_string(at.h));
                return true;
            }

            // Where to click: the middle of the picture unless told otherwise,
            // plus one row per index for the pages that are a list.
            //
            // The offsets are measured from the picture, not from the window,
            // because the picture is the only thing whose position is known --
            // these screens are drawn centred in a client area whose size the
            // player chooses.
            int cx = at.x + (page.click_dx >= 0 ? page.click_dx : at.w / 2);
            int cy = at.y + (page.click_dy >= 0 ? page.click_dy : at.h / 2);

            // Every click of the page goes through click(), which clicks only
            // once the page's window is in front and the point is on it. One
            // held back leaves the page unfinished: it is looked for again and
            // clicked from its first click, which for a character slot is the
            // same character picked a second time.
            Click done = Click::Done;
            int held_x = 0, held_y = 0;
            const auto press = [&](int x, int y) {
                const Click c = click(x, y);
                if (c == Click::Held || c == Click::BadPoint) { held_x = x; held_y = y; }
                return c;
            };

            if (page.pick != Pick::One) {
                int choice = 0;
                switch (page.step) {
                    case Step::Channel:   choice = settings_.channel_index; break;
                    case Step::Character: choice = settings_.character_index; break;
                    default:              choice = settings_.game_slot; break;
                }
                choice = std::max(0, choice);

                // An index past the end is a typo, and clicking where the
                // ninth of eight entries would be lands on whatever is below
                // the list. Say so and use the last one.
                if (page.row_count > 0 && choice >= page.row_count) {
                    GB_WARN("AutoLogin: '%s' has %d entries and number %d was asked "
                            "for; using the last one", page.key.c_str(),
                            page.row_count, choice + 1);
                    choice = page.row_count - 1;
                }
                say(page.step, "entry " + std::to_string(choice + 1) + " of " +
                               (page.row_count > 0 ? std::to_string(page.row_count)
                                                   : std::string("?")));

                if (page.pick == Pick::Row) {
                    (page.row_across ? cx : cy) += page.row_step * choice;
                } else if (page.pick == Pick::Slot) {
                    // Two clicks: the character where it stands, then the
                    // button that starts the game. The pause between them is
                    // not politeness -- confirming in the same instant as the
                    // choice is how you start the previous one.
                    int sx = at.x + page.row_dx;
                    int sy = at.y + page.row_dy;
                    (page.row_across ? sx : sy) += page.row_step * choice;
                    done = press(sx, sy);
                    if (done == Click::Done) {
                        say(page.step, "slot " + std::to_string(choice + 1) + " at " +
                                       std::to_string(sx) + "," + std::to_string(sy));
                        if (!nap(page.step_pause_ms)) return false;
                    }
                } else {
                    // A carousel. Every entry is shown in the same place, so
                    // the index cannot move the click -- it says how many
                    // times to press the arrow first. (One held back part way
                    // is wound again from wherever it stands; nothing ships
                    // using a carousel.)
                    const int ax = at.x + page.step_dx;
                    const int ay = at.y + page.step_dy;
                    for (int n = 0; n < choice && done == Click::Done; ++n) {
                        if (stop_now()) return false;
                        done = press(ax, ay);
                        if (done != Click::Done) break;
                        say(page.step, "next (" + std::to_string(n + 1) + " of " +
                                       std::to_string(choice) + ") at " +
                                       std::to_string(ax) + "," + std::to_string(ay));
                        if (!nap(page.step_pause_ms)) return false;
                    }
                }
            }

            if (done == Click::Done) done = press(cx, cy);
            if (done == Click::Stopped) return false;
            if (done == Click::BadPoint) {
                // Not a window in the way, and not something the next attempt
                // gets past: the page's offsets put the point outside the
                // window. Said at once, naming what to put right, rather than
                // waited on for half a minute three times over.
                int w = 0, h = 0;
                input_.window_size(w, h);
                return stop(Why::BadPoint,
                            "the point " + std::to_string(held_x) + "," + std::to_string(held_y) +
                                " to click on '" + page.key + "' is outside " +
                                on_name(page.on) + "'s client area (" + std::to_string(w) + "x" +
                                std::to_string(h) + "): an offset in [Page." + page.key +
                                "] of configs/login.ini is wrong",
                            page.key);
            }
            if (done == Click::Held) {
                const auto now = std::chrono::steady_clock::now();
                const bool fresh = held_page != index;
                if (fresh) {
                    held_page = index;
                    held_since = now;
                }
                const std::string what =
                    held_where_ == Landing::Minimised
                        ? std::string(on_name(page.on)) + " is minimised"
                    : held_by_.empty()
                        ? std::string("that point is off the screen -- is ") + on_name(page.on) +
                              " dragged part off it?"
                        : "a window of " + held_by_ + " is in front of " + on_name(page.on) +
                              " there";
                if (fresh || held_by_ != held_said) {
                    say(page.step, "not clicking at " + std::to_string(held_x) + "," +
                                   std::to_string(held_y) + ": " + what + " -- looking again");
                    held_said = held_by_;
                }
                if (now - held_since >= std::chrono::seconds(kHeldMaxS)) {
                    fail(Why::Covered,
                         "not clicked for " + std::to_string(kHeldMaxS) + "s, because " + what,
                         held_by_);
                    holding_ = false;
                    hidden_ = false;
                    held_by_.clear();
                    crossed = false;
                    failed_on_game = page.on == On::Game;
                    break;
                }
                if (!nap(1000)) return false;
                continue;                   // the same page, looked for again
            }

            say(page.step, "clicked at " + std::to_string(cx) + "," +
                           std::to_string(cy));
            if (!nap(1500)) return false;
            ++index;
        }

        // ---- and then the world ----
        //
        // Waited for by looking for the HUD rather than for a picture of the
        // success screen, because the HUD is what "in the world" actually
        // means: its two bars, or its "Level" plate where it always sits --
        // see in_world -- the same test that decides the first look and
        // whether a game is stuck. The game is read off the
        // screen, so a picture of it counts only while it is in front and
        // clear -- or, for the landmark, while its corner is (see world_on);
        // it is brought there every few seconds at most, so that a HUD left
        // covered -- GoBot's own window over it, say -- does not run out the
        // wait. One that stays covered is named, on the screen while this
        // waits and as the reason when the wait runs out.
        if (crossed) {
            say(Step::GameWindow, settings_.game_window);
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
            bool arrived = false;
            bool unseen = false;        // the last look could not see the HUD's corner
            while (!arrived && std::chrono::steady_clock::now() < until) {
                if (stop_now()) return false;
                void* game = find(On::Game);
                if (game != nullptr) {
                    input_.set_target_window(game);
                    aimed_at_ = On::Game;
                    const auto now = std::chrono::steady_clock::now();
                    if (GetForegroundWindow() != static_cast<HWND>(game) &&
                        now - brought_forward_ >= std::chrono::seconds(kForwardEveryS)) {
                        bring_forward();
                        brought_forward_ = now;
                        if (!nap(kSettleMs)) return false;
                    }
                    Frame frame;
                    bool trusted = false;
                    if (grab(On::Game, game, frame, trusted)) {
                        arrived = world_on(game, frame, trusted);
                        unseen = !arrived && !trusted && !plate_clear(game);
                        note_hidden(Step::GameWindow, On::Game, unseen);
                    }
                }
                if (!arrived && !nap(1500)) return false;
            }
            note_hidden(Step::GameWindow, On::Game, false);
            if (!arrived) {
                const std::string& by = behind_by_[static_cast<int>(On::Game)];
                if (unseen)
                    fail(Why::Covered,
                         "the character was never seen in the world: " +
                             (by.empty() ? std::string("the game is minimised, or off the screen")
                                         : "a window of " + by + " was over the game") +
                             ", so its HUD could not be looked at",
                         by);
                else
                    fail(Why::World, "the character never reached the world");
                crossed = false;
                failed_on_game = true;
            }
        }

        if (!crossed) {
            GB_WARN("AutoLogin: attempt %d got as far as [%s] -- %s",
                    attempt, step_name(step_.load()), trouble_.c_str());
            // The game came up and then went nowhere. In the world after all
            // -- its HUD was covered, say -- is done. On a screen the
            // crossing does not know, it is ended, as GoC's KILL_GAME_PROCESS
            // ends one, and the next attempt starts from VTCZone: carrying on
            // from a screen nobody can read is not possible, and VTCZone is
            // not touched while it is open. Not after the last attempt, nor
            // with nobody to end it: then it is left for a person to see.
            bool ended = false;
            if (failed_on_game) {
                const GameNow game = judge_game();
                if (game == GameNow::InWorld) {
                    say(Step::InWorld, "the character is in the world after all -- its HUD is up");
                    return true;
                }
                if (game == GameNow::Stuck && end_client_ && attempt < settings_.attempts) {
                    say(Step::EndClient,
                        "the game is up and none of the crossing's pages is on it -- ending "
                        "it, as GoC's KILL_GAME_PROCESS ends one");
                    end_client_();
                    say(Step::EndClient, "waiting " + std::to_string(end_cooldown_s_) +
                                             "s before the next attempt");
                    if (!nap(std::max(0, end_cooldown_s_) * 1000)) return false;
                    ended = true;
                }
            }
            if (!ended && attempt < settings_.attempts && !nap(5000)) return false;
            continue;
        }

        if (settings_.heal_on_arrival) {
            bring_forward();
            // False on a cancel: the keys are left alone, and the character
            // is in the world all the same.
            if (nap(400)) {
                if (settings_.heal_hp_key) input_.key_press(settings_.heal_hp_key);
                if (settings_.heal_mp_key) input_.key_press(settings_.heal_mp_key);
            }
        }

        say(Step::InWorld, "logged in");
        return true;
    }

    say(Step::Blocked, trouble_.empty() ? "out of attempts" : trouble_);
    return false;
}

} // namespace gobot
